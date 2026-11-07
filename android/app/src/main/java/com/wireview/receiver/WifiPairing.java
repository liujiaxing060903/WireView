package com.wireview.receiver;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.Base64;

import java.security.SecureRandom;

/**
 * R3 WiFi authentication: tablet-side credential storage. The crypto lives
 * in {@link WifiAuthCrypto}; the wire layout lives in {@link Protocol}.
 *
 * <p>Threat model: one-time PIN pairing plus per-connection challenge/
 * response defeats passive sniffing and random LAN scanners. A six digit PIN
 * is ~2^20 entropy against an offline attacker; PBKDF2 raises the cost but
 * does not change the class. Personal LAN tool, not a hostile-network
 * protocol.</p>
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
        private final SharedPreferences prefs;

        TabletCredentials(Context context) {
            prefs = context.getSharedPreferences(PREFS_FILE, Context.MODE_PRIVATE);
        }

        /** Returns the long-term credential for this PC, or null when not paired. */
        byte[] get(byte[] pcUuid) {
            String stored = prefs.getString(PREF_CREDENTIAL_PREFIX + hex(pcUuid), null);
            if (stored == null) {
                return null;
            }
            byte[] credential = Base64.decode(stored, Base64.NO_WRAP);
            if (credential.length != Protocol.AUTH_KEY_SIZE) {
                return null;
            }
            return credential;
        }

        String pcName(byte[] pcUuid) {
            return prefs.getString(PREF_NAME_PREFIX + hex(pcUuid), null);
        }

        void put(byte[] pcUuid, byte[] credential, String pcName) {
            prefs.edit()
                    .putString(PREF_CREDENTIAL_PREFIX + hex(pcUuid),
                            Base64.encodeToString(credential, Base64.NO_WRAP))
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

    static byte[] concat(byte[]... parts) {
        int total = 0;
        for (byte[] part : parts) {
            total += part.length;
        }
        byte[] result = new byte[total];
        int offset = 0;
        for (byte[] part : parts) {
            System.arraycopy(part, 0, result, offset, part.length);
            offset += part.length;
        }
        return result;
    }

    static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) {
            text.append(String.format("%02x", item));
        }
        return text.toString();
    }
}
