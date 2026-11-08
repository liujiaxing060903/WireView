package com.wireview.receiver;

import android.content.Context;
import android.content.SharedPreferences;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyProperties;
import android.util.Base64;

import java.io.IOException;
import java.security.GeneralSecurityException;
import java.security.KeyStore;
import java.security.SecureRandom;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.spec.GCMParameterSpec;

/**
 * R3 WiFi authentication: tablet-side credential storage. The crypto lives
 * in {@link WifiAuthCrypto}; the wire layout lives in {@link Protocol}.
 *
 * <p>Threat model: one-time PIN pairing plus per-connection challenge/
 * response defeats passive sniffing and random LAN scanners. A six digit PIN
 * is ~2^20 entropy against an offline attacker; PBKDF2 raises the cost but
 * does not change the class. Personal LAN tool, not a hostile-network
 * protocol.</p>
 *
 * <p>At-rest protection (audit A6 NF-4): credentials are wrapped with an
 * AES-256-GCM key that lives in the Android Keystore and is not exportable,
 * so a copied prefs file is useless offline. Rows are stored as
 * {@code "v2:" + Base64(iv || ciphertext+tag)}; legacy plaintext rows are
 * migrated to the wrapped format on first read. The JVM protocol self-tests
 * never compile this file (it needs Android framework classes), so the
 * Keystore path cannot break them.</p>
 */
final class WifiPairing {
    private static final String PREFS_FILE = "wireview_wifi_pairing";
    private static final String PREF_CREDENTIAL_PREFIX = "credential_";
    private static final String PREF_NAME_PREFIX = "pc_name_";
    private static final String PREF_ANDROID_ID = "cached_android_id";

    private WifiPairing() {
    }

    /** Persistent {pc uuid -> credential} map in a private SharedPreferences file. */
    static final class TabletCredentials {
        /** Encrypted-row marker: "v2:" + Base64(iv || AES-GCM ciphertext+tag). */
        private static final String ENCRYPTED_PREFIX = "v2:";
        /** Android Keystore alias of the non-exportable AES-GCM wrap key. */
        private static final String KEYSTORE_ALIAS = "wireview-pairing-wrap";
        private static final int GCM_IV_SIZE_BYTES = 12;
        private static final int GCM_TAG_SIZE_BITS = 128;

        private final SharedPreferences prefs;

        TabletCredentials(Context context) {
            prefs = context.getSharedPreferences(PREFS_FILE, Context.MODE_PRIVATE);
        }

        /**
         * Returns the long-term credential for this PC, or null when not
         * paired. A legacy plaintext row is decoded once and immediately
         * re-written in the encrypted format (the same prefs key is
         * overwritten, so no plaintext copy survives the migration).
         */
        byte[] get(byte[] pcUuid) {
            final String key = PREF_CREDENTIAL_PREFIX + hex(pcUuid);
            String stored = prefs.getString(key, null);
            if (stored == null) {
                return null;
            }
            if (stored.startsWith(ENCRYPTED_PREFIX)) {
                byte[] credential = decryptCredential(stored);
                return credential != null && credential.length == Protocol.AUTH_KEY_SIZE
                        ? credential
                        : null;
            }
            // Legacy plaintext row (pre-encryption install): usable as-is,
            // but wrap it now so the plaintext stops living in the file.
            byte[] credential = Base64.decode(stored, Base64.NO_WRAP);
            if (credential.length != Protocol.AUTH_KEY_SIZE) {
                return null;
            }
            try {
                prefs.edit().putString(key, encryptCredential(credential)).apply();
            } catch (GeneralSecurityException | IOException | RuntimeException error) {
                // The credential still authenticates this session; the next
                // read retries the migration.
                PersistentDiagnosticLog.record(
                        "WifiPairing",
                        "credential migration to encrypted storage failed: " + error);
            }
            return credential;
        }

        String pcName(byte[] pcUuid) {
            return prefs.getString(PREF_NAME_PREFIX + hex(pcUuid), null);
        }

        void put(byte[] pcUuid, byte[] credential, String pcName) {
            final String wrapped;
            try {
                wrapped = encryptCredential(credential);
            } catch (GeneralSecurityException | IOException error) {
                PersistentDiagnosticLog.record(
                        "WifiPairing",
                        "encrypting the pairing credential failed: " + error);
                // Never fall back to plaintext: failing this pairing round
                // is safer than storing the credential in the clear.
                throw new IllegalStateException(
                        "pairing credential could not be encrypted", error);
            }
            prefs.edit()
                    .putString(PREF_CREDENTIAL_PREFIX + hex(pcUuid), wrapped)
                    .putString(PREF_NAME_PREFIX + hex(pcUuid), pcName)
                    .apply();
        }

        void remove(byte[] pcUuid) {
            prefs.edit()
                    .remove(PREF_CREDENTIAL_PREFIX + hex(pcUuid))
                    .remove(PREF_NAME_PREFIX + hex(pcUuid))
                    .apply();
        }

        boolean has(byte[] pcUuid) {
            return prefs.contains(PREF_CREDENTIAL_PREFIX + hex(pcUuid));
        }

        private static String encryptCredential(byte[] credential)
                throws GeneralSecurityException, IOException {
            Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, wrappingKey());
            byte[] ciphertext = cipher.doFinal(credential); // tag appended
            byte[] iv = cipher.getIV();
            return ENCRYPTED_PREFIX + Base64.encodeToString(
                    concat(iv, ciphertext), Base64.NO_WRAP);
        }

        private static byte[] decryptCredential(String stored) {
            try {
                byte[] blob = Base64.decode(
                        stored.substring(ENCRYPTED_PREFIX.length()), Base64.NO_WRAP);
                if (blob.length <= GCM_IV_SIZE_BYTES) {
                    return null;
                }
                Cipher cipher = Cipher.getInstance("AES/GCM/NoPadding");
                cipher.init(
                        Cipher.DECRYPT_MODE,
                        wrappingKey(),
                        new GCMParameterSpec(
                                GCM_TAG_SIZE_BITS, blob, 0, GCM_IV_SIZE_BYTES));
                byte[] plaintext = cipher.doFinal(
                        blob, GCM_IV_SIZE_BYTES, blob.length - GCM_IV_SIZE_BYTES);
                return plaintext != null && plaintext.length == Protocol.AUTH_KEY_SIZE
                        ? plaintext
                        : null;
            } catch (GeneralSecurityException | IOException | RuntimeException error) {
                // Corrupt row or an invalidated Keystore key (factory reset
                // restore, lockscreen credential change): treat as
                // unpaired; re-pairing rewrites the row.
                PersistentDiagnosticLog.record(
                        "WifiPairing",
                        "stored credential could not be decrypted: " + error);
                return null;
            }
        }

        /**
         * The non-exportable AES-256-GCM wrap key in the Android Keystore,
         * generated on first use. The key material never leaves the
         * Keystore, so the wrapped rows in the prefs file cannot be opened
         * anywhere but on this device.
         */
        private static SecretKey wrappingKey()
                throws GeneralSecurityException, IOException {
            KeyStore keyStore = KeyStore.getInstance("AndroidKeyStore");
            keyStore.load(null);
            KeyStore.Entry entry = keyStore.getEntry(KEYSTORE_ALIAS, null);
            if (entry instanceof KeyStore.SecretKeyEntry) {
                return ((KeyStore.SecretKeyEntry) entry).getSecretKey();
            }
            KeyGenerator generator = KeyGenerator.getInstance(
                    KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore");
            generator.init(new KeyGenParameterSpec.Builder(
                            KEYSTORE_ALIAS,
                            KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
                    .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                    .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                    .setKeySize(256)
                    .build());
            return generator.generateKey();
        }
    }

    /** Stable per-install tablet identity; ANDROID_ID changes with signing, so cache it. */
    static String androidId(Context context) {
        SharedPreferences prefs =
                context.getSharedPreferences(PREFS_FILE, Context.MODE_PRIVATE);
        String cached = prefs.getString(PREF_ANDROID_ID, null);
        if (cached != null) {
            return cached;
        }
        String id = android.provider.Settings.Secure.getString(
                context.getContentResolver(), android.provider.Settings.Secure.ANDROID_ID);
        if (id == null || id.length() < 8) {
            // ANDROID_ID is documented null/constant on some builds; fall back
            // to a random install identity so pairing still works. It does
            // not need to be secret, only stable per tablet.
            byte[] random = new byte[16];
            new SecureRandom().nextBytes(random);
            id = hex(random);
        }
        prefs.edit().putString(PREF_ANDROID_ID, id).apply();
        return id;
    }

    /** Delegates to the single shared implementation (audit A7 N7). */
    static byte[] concat(byte[]... parts) {
        return WifiAuthCrypto.concat(parts);
    }

    static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) {
            text.append(String.format("%02x", item));
        }
        return text.toString();
    }
}
