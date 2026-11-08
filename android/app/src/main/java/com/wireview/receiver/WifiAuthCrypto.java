package com.wireview.receiver;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;

import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Platform crypto for the R3 WiFi authentication handshake. No Android
 * dependencies so the JVM self-test can compile and run it; mirrored by CNG
 * in wifi_crypto.cpp and locked together by the RFC vectors both sides test.
 */
final class WifiAuthCrypto {
    private WifiAuthCrypto() {
    }

    static byte[] hmacSha256(byte[] key, byte[] data) throws GeneralSecurityException {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(data);
    }

    static byte[] pbkdf2Sha256(byte[] password, byte[] salt, int iterations, int length)
            throws GeneralSecurityException {
        PBEKeySpec spec = new PBEKeySpec(
                new String(password, StandardCharsets.UTF_8).toCharArray(),
                salt,
                iterations,
                length * 8);
        try {
            SecretKeyFactory factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256");
            return factory.generateSecret(spec).getEncoded();
        } finally {
            spec.clearPassword();
        }
    }

    /** salt = "WV1-salt" || pcUuid || androidId || noncePc || nonceTablet. */
    static byte[] derivePairingKey(
            char[] pin,
            byte[] pcUuid,
            byte[] androidId,
            byte[] noncePc,
            byte[] nonceTablet) throws GeneralSecurityException {
        byte[] salt = concat(
                Protocol.AUTH_KDF_SALT.getBytes(StandardCharsets.US_ASCII),
                pcUuid,
                androidId,
                noncePc,
                nonceTablet);
        PBEKeySpec spec = new PBEKeySpec(
                pin, salt, Protocol.AUTH_PBKDF2_ITERATIONS, Protocol.AUTH_KEY_SIZE * 8);
        try {
            SecretKeyFactory factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256");
            return factory.generateSecret(spec).getEncoded();
        } finally {
            spec.clearPassword();
        }
    }

    /** MAC input = domain || pcUuid || androidId || noncePc || nonceTablet. */
    static byte[] computeMac(
            byte[] key,
            String domain,
            byte[] pcUuid,
            byte[] androidId,
            byte[] noncePc,
            byte[] nonceTablet) throws GeneralSecurityException {
        return hmacSha256(key, concat(
                domain.getBytes(StandardCharsets.US_ASCII),
                pcUuid,
                androidId,
                noncePc,
                nonceTablet));
    }

    /**
     * v2 AUTH_RESULT integrity tag:
     * HMAC(key, "WV1-result" || outcome || message || credentialView ||
     * noncePc || nonceTablet). credentialView is the on-wire (masked)
     * credential for pairing_complete and empty otherwise.
     */
    static byte[] computeResultTag(
            byte[] key,
            byte outcome,
            String message,
            byte[] credentialView,
            byte[] noncePc,
            byte[] nonceTablet) throws GeneralSecurityException {
        byte[] view = credentialView == null ? new byte[0] : credentialView;
        return hmacSha256(key, concat(
                Protocol.AUTH_DOMAIN_RESULT.getBytes(StandardCharsets.US_ASCII),
                new byte[] {outcome},
                message.getBytes(StandardCharsets.UTF_8),
                view,
                noncePc,
                nonceTablet));
    }

    /** v2 pairing credential mask: HMAC(kdfKey, "WV1-wrap" || noncePc || nonceTablet). */
    static byte[] wrapMask(byte[] kdfKey, byte[] noncePc, byte[] nonceTablet)
            throws GeneralSecurityException {
        return hmacSha256(kdfKey, concat(
                Protocol.AUTH_WRAP_DOMAIN.getBytes(StandardCharsets.US_ASCII),
                noncePc,
                nonceTablet));
    }

    /** XOR of two equal-length arrays (credential masking / unmasking). */
    static byte[] xorMask(byte[] value, byte[] mask) {
        byte[] result = new byte[value.length];
        for (int i = 0; i < value.length; i++) {
            result[i] = (byte) (value[i] ^ mask[i]);
        }
        return result;
    }

    /** Package-private so {@link WifiPairing} can delegate (A7 N7). */
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
}
