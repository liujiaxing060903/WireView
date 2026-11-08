package com.wireview.receiver;

import java.io.ByteArrayInputStream;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;

public final class ProtocolSelfTest {
    private ProtocolSelfTest() {
    }

    public static void main(String[] args) throws Exception {
        byte[] payload = "wireview-java".getBytes(StandardCharsets.UTF_8);
        Protocol.Header header = new Protocol.Header(
                Protocol.TYPE_HELLO,
                4,
                42,
                123456789L);
        byte[] encoded = Protocol.encode(header, payload);
        Protocol.Packet decoded = Protocol.decode(encoded);

        require(decoded.header.type == Protocol.TYPE_HELLO, "type mismatch");
        require(decoded.header.flags == 4, "flags mismatch");
        require(decoded.header.sequence == 42, "sequence mismatch");
        require(decoded.header.timestampMicros == 123456789L, "timestamp mismatch");
        require(Arrays.equals(decoded.payload, payload), "payload mismatch");

        Protocol.Packet streamed = Protocol.readPacket(new ByteArrayInputStream(encoded));
        require(Arrays.equals(streamed.payload, payload), "stream payload mismatch");

        byte[] corrupted = encoded.clone();
        corrupted[corrupted.length - 1] ^= 1;
        boolean rejected = false;
        try {
            Protocol.decode(corrupted);
        } catch (Protocol.ProtocolException expected) {
            rejected = true;
        }
        require(rejected, "corrupted packet was accepted");

        authMessagesTest();
        cryptoVectorTest();

        System.out.println("All WireView Java protocol tests passed");
    }

    private static void authMessagesTest() throws Exception {
        // Round trip the three R3 WiFi authentication payloads exactly as the
        // C++ side encodes them (little endian, u16 length prefixes).
        Protocol.AuthChallenge challenge = new Protocol.AuthChallenge(
                Protocol.AUTH_MODE_PAIRING,
                filled(Protocol.AUTH_NONCE_SIZE, (byte) 0xa5),
                filled(Protocol.AUTH_UUID_SIZE, (byte) 0x11),
                "DESKTOP-ABC123");
        Protocol.AuthChallenge parsedChallenge = Protocol.parseAuthChallenge(
                Protocol.encodeAuthChallenge(challenge));
        require(parsedChallenge.mode == challenge.mode, "challenge mode mismatch");
        require(Arrays.equals(parsedChallenge.noncePc, challenge.noncePc),
                "challenge nonce mismatch");
        require(Arrays.equals(parsedChallenge.pcUuid, challenge.pcUuid),
                "challenge pc uuid mismatch");
        require(parsedChallenge.hostName.equals(challenge.hostName),
                "challenge host name mismatch");

        boolean rejected = false;
        try {
            Protocol.parseAuthChallenge(new byte[0]);
        } catch (Protocol.ProtocolException expected) {
            rejected = true;
        }
        require(rejected, "empty challenge was accepted");

        Protocol.AuthResponse response = new Protocol.AuthResponse(
                Protocol.AUTH_STATUS_OK,
                filled(Protocol.AUTH_NONCE_SIZE, (byte) 0x5a),
                "0123456789abcdef",
                "Xiaomi Pad 4",
                filled(Protocol.AUTH_MAC_SIZE, (byte) 0x77));
        Protocol.AuthResponse parsedResponse = Protocol.parseAuthResponse(
                Protocol.encodeAuthResponse(response));
        require(parsedResponse.status == response.status, "response status mismatch");
        require(parsedResponse.androidId.equals(response.androidId),
                "response android id mismatch");
        require(parsedResponse.deviceName.equals(response.deviceName),
                "response device name mismatch");
        require(Arrays.equals(parsedResponse.nonceTablet, response.nonceTablet),
                "response nonce mismatch");
        require(Arrays.equals(parsedResponse.mac, response.mac), "response mac mismatch");

        // v2 response (schema 2 + one capability byte) and its corruption
        // cases; the C++ parser accepts exactly the same shapes.
        Protocol.AuthResponse parsedV2 = Protocol.parseAuthResponse(
                Protocol.encodeAuthResponse(response, true));
        require(parsedV2.status == response.status, "v2 response status mismatch");
        require(Arrays.equals(parsedV2.mac, response.mac), "v2 response mac mismatch");
        byte[] v2Bytes = Protocol.encodeAuthResponse(response, true);
        require(v2Bytes[0] == 2 && v2Bytes[v2Bytes.length - 1] == 1,
                "v2 response must carry schema 2 and caps 1");
        boolean v2Rejected = false;
        try {
            byte[] badCaps = v2Bytes.clone();
            badCaps[badCaps.length - 1] = 2;
            Protocol.parseAuthResponse(badCaps);
        } catch (Protocol.ProtocolException expected) {
            v2Rejected = true;
        }
        require(v2Rejected, "unknown v2 caps was accepted");
        v2Rejected = false;
        try {
            Protocol.parseAuthResponse(Arrays.copyOf(v2Bytes, v2Bytes.length + 1));
        } catch (Protocol.ProtocolException expected) {
            v2Rejected = true;
        }
        require(v2Rejected, "v2 trailing byte was accepted");

        Protocol.AuthResult pairing = new Protocol.AuthResult(
                Protocol.AUTH_RESULT_PAIRING_COMPLETE, "DESKTOP-ABC123",
                filled(Protocol.AUTH_KEY_SIZE, (byte) 0x33));
        Protocol.AuthResult parsedPairing = Protocol.parseAuthResult(
                Protocol.encodeAuthResult(pairing));
        require(parsedPairing.result == pairing.result, "pairing result mismatch");
        require(Arrays.equals(parsedPairing.credential, pairing.credential),
                "pairing credential mismatch");
        require(parsedPairing.tag == null, "legacy pairing result must not carry a tag");

        // v2 result round trip: legacy fields plus exactly one 32 byte tag.
        Protocol.AuthResult tagged = new Protocol.AuthResult(
                Protocol.AUTH_RESULT_PAIRING_COMPLETE, "DESKTOP-ABC123",
                filled(Protocol.AUTH_KEY_SIZE, (byte) 0x44),
                filled(Protocol.AUTH_MAC_SIZE, (byte) 0x99));
        Protocol.AuthResult parsedTagged = Protocol.parseAuthResult(
                Protocol.encodeAuthResult(tagged));
        require(parsedTagged.result == tagged.result, "tagged result kind mismatch");
        require(Arrays.equals(parsedTagged.credential, tagged.credential),
                "tagged credential mismatch");
        require(Arrays.equals(parsedTagged.tag, tagged.tag), "result tag mismatch");
        byte[] taggedBytes = Protocol.encodeAuthResult(tagged);
        boolean tagRejected = false;
        try {
            Protocol.parseAuthResult(Arrays.copyOf(taggedBytes, taggedBytes.length - 1));
        } catch (Protocol.ProtocolException expected) {
            tagRejected = true;
        }
        require(tagRejected, "truncated tag was accepted");
        tagRejected = false;
        try {
            Protocol.parseAuthResult(Arrays.copyOf(taggedBytes, taggedBytes.length + 1));
        } catch (Protocol.ProtocolException expected) {
            tagRejected = true;
        }
        require(tagRejected, "byte after tag was accepted");

        byte[] pairingBytes = Protocol.encodeAuthResult(pairing);
        byte[] trailing = Arrays.copyOf(pairingBytes, pairingBytes.length + 1);
        trailing[trailing.length - 1] = 0;
        rejected = false;
        try {
            Protocol.parseAuthResult(trailing);
        } catch (Protocol.ProtocolException expected) {
            rejected = true;
        }
        require(rejected, "trailing byte after pairing result was accepted");
    }

    private static void cryptoVectorTest() throws Exception {
        // Lock the Java crypto primitives to the same published vectors the
        // C++ CNG tests use, so both ends of the WiFi handshake agree.
        byte[] mac = WifiAuthCrypto.hmacSha256(
                "Jefe".getBytes(StandardCharsets.UTF_8),
                "what do ya want for nothing?".getBytes(StandardCharsets.UTF_8));
        require(hex(mac).equals(
                "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"),
                "HMAC-SHA256 must match the RFC 4231 Jefe vector");

        byte[] derived = WifiAuthCrypto.pbkdf2Sha256(
                "password".getBytes(StandardCharsets.UTF_8),
                "salt".getBytes(StandardCharsets.UTF_8),
                1,
                32);
        require(hex(derived).equals(
                "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"),
                "PBKDF2 iteration 1 must match the published vector");
        derived = WifiAuthCrypto.pbkdf2Sha256(
                "password".getBytes(StandardCharsets.UTF_8),
                "salt".getBytes(StandardCharsets.UTF_8),
                4096,
                32);
        require(hex(derived).equals(
                "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"),
                "PBKDF2 iteration 4096 must match the published vector");

        // v2 result integrity vectors (independently computed with .NET
        // HMACSHA256; the C++ wifi_crypto tests lock onto the same bytes):
        // key = 0x01..0x20, noncePc = 16x0x11, nonceTablet = 16x0x22,
        // credential view = 32x0xAA, message "MI PAD 4".
        byte[] key = new byte[32];
        for (int i = 0; i < key.length; i++) {
            key[i] = (byte) (i + 1);
        }
        byte[] noncePc = filled(16, (byte) 0x11);
        byte[] nonceTablet = filled(16, (byte) 0x22);
        byte[] credView = filled(32, (byte) 0xAA);
        byte[] pairTag = WifiAuthCrypto.computeResultTag(
                key, Protocol.AUTH_RESULT_PAIRING_COMPLETE, "MI PAD 4", credView,
                noncePc, nonceTablet);
        require(hex(pairTag).equals(
                "815fce97464dd863b42a7c2340333ff9db4ebf38eac2b21dbc69935bf821b337"),
                "pairing result tag must match the published vector");
        byte[] acceptedTag = WifiAuthCrypto.computeResultTag(
                key, Protocol.AUTH_RESULT_ACCEPTED, "MI PAD 4", null, noncePc, nonceTablet);
        require(hex(acceptedTag).equals(
                "50d6386ed9be251c7c492e96ca14772ec13a7e39acee9159b4de28f7c0d14798"),
                "accepted result tag must match the published vector");
        byte[] mask = WifiAuthCrypto.wrapMask(key, noncePc, nonceTablet);
        require(hex(mask).equals(
                "82be86bae43ef7a234893d65fe275abe540413e77d419535f01425632e3aef2e"),
                "wrap mask must match the published vector");
        byte[] roundTrip = WifiAuthCrypto.xorMask(
                WifiAuthCrypto.xorMask(credView, mask), mask);
        require(Arrays.equals(roundTrip, credView), "xor mask must be an involution");
    }

    private static byte[] filled(int size, byte value) {
        byte[] bytes = new byte[size];
        Arrays.fill(bytes, value);
        return bytes;
    }

    private static String hex(byte[] bytes) {
        StringBuilder text = new StringBuilder(bytes.length * 2);
        for (byte item : bytes) {
            text.append(String.format("%02x", item));
        }
        return text.toString();
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }
}

