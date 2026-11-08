package com.wireview.receiver;

import java.io.EOFException;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.CRC32;

final class Protocol {
    static final byte[] MAGIC = "WVR1".getBytes(StandardCharsets.US_ASCII);
    static final int VERSION_MAJOR = 1;
    static final int VERSION_MINOR = 0;
    static final int HEADER_SIZE = 40;
    static final int MAX_PAYLOAD_SIZE = 16 * 1024 * 1024;

    static final int TYPE_HELLO = 1;
    static final int TYPE_HELLO_ACK = 2;
    static final int TYPE_START_STREAM = 3;
    static final int TYPE_STOP_STREAM = 4;
    static final int TYPE_VIDEO_CHUNK = 5;
    static final int TYPE_KEYFRAME_REQUEST = 6;
    static final int TYPE_INPUT_BATCH = 7;
    static final int TYPE_TELEMETRY = 8;
    static final int TYPE_PING = 9;
    static final int TYPE_PONG = 10;
    static final int TYPE_ERROR = 11;
    // Sent by the Windows control center before a desktop stream. The Android
    // receiver uses it to show the selected output aspect ratio as a frame.
    static final int TYPE_DEVICE_PREVIEW = 12;
    // Tablet edge sidebar -> PC control commands (ESC / touch keyboard /
    // orientation rotation / window list request / move window). Older hosts
    // silently ignore unknown packet types, so this stays backward compatible.
    static final int TYPE_CONTROL_COMMAND = 13;
    // PC -> tablet reply: window list for the application switcher.
    static final int TYPE_WINDOW_LIST = 14;
    // R3 WiFi link authentication (both ends ship from this repository, so
    // the retired type 15 for ime_control is not reused for a new meaning;
    // the WiFi handshake starts fresh numbering at 15).
    static final int TYPE_AUTH_CHALLENGE = 15;
    static final int TYPE_AUTH_RESPONSE = 16;
    static final int TYPE_AUTH_RESULT = 17;

    private Protocol() {
    }

    static final class Header {
        final int type;
        final int flags;
        final long sequence;
        final long timestampMicros;

        Header(int type, int flags, long sequence, long timestampMicros) {
            this.type = type;
            this.flags = flags;
            this.sequence = sequence;
            this.timestampMicros = timestampMicros;
        }
    }

    // ---- R3 WiFi authentication payload layout (mirror of auth_messages.hpp).
    // Pure byte layout only; key derivation (PBKDF2-HMAC-SHA256) and the MAC
    // (HMAC-SHA256) live in WifiPairing. The C++ mirror is the authority.

    static final String AUTH_DOMAIN_AUTHENTICATE = "WV1-auth";
    static final String AUTH_DOMAIN_PAIR = "WV1-pair";
    static final String AUTH_KDF_SALT = "WV1-salt";
    static final String AUTH_DOMAIN_RESULT = "WV1-result";
    static final String AUTH_WRAP_DOMAIN = "WV1-wrap";
    static final int AUTH_PBKDF2_ITERATIONS = 600_000;
    static final int AUTH_NONCE_SIZE = 16;
    static final int AUTH_UUID_SIZE = 16;
    static final int AUTH_KEY_SIZE = 32;
    static final int AUTH_MAC_SIZE = 32;
    /** Packet header flag the PC sets on AUTH_CHALLENGE to advertise the
     *  v2 result integrity layer (tagged AUTH_RESULT, masked credential). */
    static final int FLAG_AUTH_V2 = 0x08;

    static final byte AUTH_MODE_AUTHENTICATE = 0;
    static final byte AUTH_MODE_PAIRING = 1;
    static final byte AUTH_STATUS_OK = 0;
    static final byte AUTH_STATUS_NOT_PAIRED = 1;
    static final byte AUTH_RESULT_ACCEPTED = 0;
    static final byte AUTH_RESULT_REJECTED = 1;
    static final byte AUTH_RESULT_PAIRING_COMPLETE = 2;

    static final class AuthChallenge {
        final byte mode;
        final byte[] noncePc;
        final byte[] pcUuid;
        final String hostName;

        AuthChallenge(byte mode, byte[] noncePc, byte[] pcUuid, String hostName) {
            this.mode = mode;
            this.noncePc = noncePc;
            this.pcUuid = pcUuid;
            this.hostName = hostName;
        }
    }

    static final class AuthResponse {
        final byte status;
        final byte[] nonceTablet;
        final String androidId;
        final String deviceName;
        final byte[] mac;

        AuthResponse(
                byte status,
                byte[] nonceTablet,
                String androidId,
                String deviceName,
                byte[] mac) {
            this.status = status;
            this.nonceTablet = nonceTablet;
            this.androidId = androidId;
            this.deviceName = deviceName;
            this.mac = mac;
        }
    }

    static final class AuthResult {
        final byte result;
        final String message;
        final byte[] credential;
        /** v2 integrity tag over the outcome; null in the legacy format. */
        final byte[] tag;

        AuthResult(byte result, String message, byte[] credential) {
            this(result, message, credential, null);
        }

        AuthResult(byte result, String message, byte[] credential, byte[] tag) {
            this.result = result;
            this.message = message;
            this.credential = credential;
            this.tag = tag;
        }
    }

    private static void putString(ByteBuffer output, String value, int maxLength)
            throws ProtocolException {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        if (bytes.length > maxLength) {
            throw new ProtocolException("auth string exceeds the wire limit");
        }
        output.putShort((short) bytes.length);
        output.put(bytes);
    }

    static byte[] encodeAuthChallenge(AuthChallenge challenge) throws ProtocolException {
        ByteBuffer output = ByteBuffer.allocate(
                4 + AUTH_NONCE_SIZE + AUTH_UUID_SIZE
                        + challenge.hostName.getBytes(StandardCharsets.UTF_8).length)
                .order(ByteOrder.LITTLE_ENDIAN);
        output.put((byte) 1); // schema
        output.put(challenge.mode);
        putString(output, challenge.hostName, 128);
        output.put(challenge.noncePc);
        output.put(challenge.pcUuid);
        return output.array();
    }

    static AuthChallenge parseAuthChallenge(byte[] payload) throws ProtocolException {
        ByteBuffer input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        if (remainingShort(input) < 4) {
            throw new ProtocolException("auth challenge payload is too short");
        }
        byte schema = input.get();
        if (schema != 1) {
            throw new ProtocolException("unknown auth challenge schema " + schema);
        }
        byte mode = input.get();
        if (mode != AUTH_MODE_AUTHENTICATE && mode != AUTH_MODE_PAIRING) {
            throw new ProtocolException("unknown auth mode " + mode);
        }
        String hostName = getString(input, 128);
        byte[] noncePc = new byte[AUTH_NONCE_SIZE];
        byte[] pcUuid = new byte[AUTH_UUID_SIZE];
        if (input.remaining() < noncePc.length + pcUuid.length) {
            throw new ProtocolException("auth challenge is truncated");
        }
        input.get(noncePc);
        input.get(pcUuid);
        if (input.hasRemaining()) {
            throw new ProtocolException("auth challenge has trailing bytes");
        }
        return new AuthChallenge(mode, noncePc, pcUuid, hostName);
    }

    static byte[] encodeAuthResponse(AuthResponse response) throws ProtocolException {
        return encodeAuthResponse(response, false);
    }

    /** v2 = true emits schema 2 with one trailing capability byte (value 1);
     *  the tablet only answers schema 2 when the challenge carried the
     *  FLAG_AUTH_V2 header flag, so legacy PCs never see schema 2. */
    static byte[] encodeAuthResponse(AuthResponse response, boolean v2) throws ProtocolException {
        // 2 (schema+status) + 2 x u16 length prefixes + nonce + MAC + strings.
        ByteBuffer output = ByteBuffer.allocate(
                2 + 4 + AUTH_NONCE_SIZE + AUTH_MAC_SIZE + (v2 ? 1 : 0)
                        + response.androidId.getBytes(StandardCharsets.US_ASCII).length
                        + response.deviceName.getBytes(StandardCharsets.UTF_8).length)
                .order(ByteOrder.LITTLE_ENDIAN);
        output.put(v2 ? (byte) 2 : (byte) 1); // schema
        output.put(response.status);
        putString(output, response.androidId, 64);
        putString(output, response.deviceName, 128);
        output.put(response.nonceTablet);
        output.put(response.mac);
        if (v2) {
            output.put((byte) 1); // capability byte
        }
        return output.array();
    }

    static AuthResponse parseAuthResponse(byte[] payload) throws ProtocolException {
        ByteBuffer input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        if (remainingShort(input) < 4) {
            throw new ProtocolException("auth response payload is too short");
        }
        byte schema = input.get();
        if (schema != 1 && schema != 2) {
            throw new ProtocolException("unknown auth response schema " + schema);
        }
        byte status = input.get();
        if (status != AUTH_STATUS_OK && status != AUTH_STATUS_NOT_PAIRED) {
            throw new ProtocolException("unknown auth status " + status);
        }
        String androidId = getString(input, 64);
        String deviceName = getString(input, 128);
        byte[] nonceTablet = new byte[AUTH_NONCE_SIZE];
        byte[] mac = new byte[AUTH_MAC_SIZE];
        if (input.remaining() < nonceTablet.length + mac.length) {
            throw new ProtocolException("auth response is truncated");
        }
        input.get(nonceTablet);
        input.get(mac);
        if (schema == 2) {
            // The v2 tail is exactly one capability byte (value 1) for now.
            if (input.remaining() != 1 || input.get() != 1) {
                throw new ProtocolException("auth response v2 tail is invalid");
            }
        } else if (input.hasRemaining()) {
            throw new ProtocolException("auth response has trailing bytes");
        }
        return new AuthResponse(status, nonceTablet, androidId, deviceName, mac);
    }

    static byte[] encodeAuthResult(AuthResult result) throws ProtocolException {
        boolean carriesCredential = result.result == AUTH_RESULT_PAIRING_COMPLETE;
        ByteBuffer output = ByteBuffer.allocate(
                4 + result.message.getBytes(StandardCharsets.UTF_8).length
                        + (carriesCredential ? AUTH_KEY_SIZE : 0)
                        + (result.tag != null ? AUTH_MAC_SIZE : 0))
                .order(ByteOrder.LITTLE_ENDIAN);
        output.put((byte) 1); // schema
        output.put(result.result);
        putString(output, result.message, 256);
        if (carriesCredential) {
            output.put(result.credential);
        }
        if (result.tag != null) {
            output.put(result.tag);
        }
        return output.array();
    }

    static AuthResult parseAuthResult(byte[] payload) throws ProtocolException {
        ByteBuffer input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        if (remainingShort(input) < 4) {
            throw new ProtocolException("auth result payload is too short");
        }
        byte schema = input.get();
        if (schema != 1) {
            throw new ProtocolException("unknown auth result schema " + schema);
        }
        byte result = input.get();
        if (result != AUTH_RESULT_ACCEPTED && result != AUTH_RESULT_REJECTED
                && result != AUTH_RESULT_PAIRING_COMPLETE) {
            throw new ProtocolException("unknown auth result " + result);
        }
        String message = getString(input, 256);
        byte[] credential = null;
        if (result == AUTH_RESULT_PAIRING_COMPLETE) {
            if (input.remaining() < AUTH_KEY_SIZE) {
                throw new ProtocolException("auth pairing result is missing the credential");
            }
            credential = new byte[AUTH_KEY_SIZE];
            input.get(credential);
        }
        byte[] tag = null;
        if (input.remaining() == AUTH_MAC_SIZE) {
            tag = new byte[AUTH_MAC_SIZE];
            input.get(tag);
        }
        if (input.hasRemaining()) {
            throw new ProtocolException("auth result has trailing bytes");
        }
        return new AuthResult(result, message, credential, tag);
    }

    private static int remainingShort(ByteBuffer input) {
        return input.remaining();
    }

    private static String getString(ByteBuffer input, int maxLength) throws ProtocolException {
        if (input.remaining() < 2) {
            throw new ProtocolException("auth string length is truncated");
        }
        int length = Short.toUnsignedInt(input.getShort());
        if (length > maxLength) {
            throw new ProtocolException("auth string exceeds the wire limit");
        }
        if (input.remaining() < length) {
            throw new ProtocolException("auth string is truncated");
        }
        byte[] bytes = new byte[length];
        input.get(bytes);
        return new String(bytes, StandardCharsets.UTF_8);
    }

    static final class Packet {
        final Header header;
        final byte[] payload;

        Packet(Header header, byte[] payload) {
            this.header = header;
            this.payload = payload;
        }
    }

    static final class ProtocolException extends IOException {
        ProtocolException(String message) {
            super(message);
        }
    }

    static byte[] encode(Header header, byte[] payload) {
        if (payload.length > MAX_PAYLOAD_SIZE) {
            throw new IllegalArgumentException("WireView packet payload is too large");
        }

        byte[] packet = new byte[HEADER_SIZE + payload.length];
        ByteBuffer output = ByteBuffer.wrap(packet).order(ByteOrder.LITTLE_ENDIAN);
        output.put(MAGIC);
        output.put((byte) VERSION_MAJOR);
        output.put((byte) VERSION_MINOR);
        output.put((byte) header.type);
        output.put((byte) header.flags);
        output.putShort((short) HEADER_SIZE);
        output.putShort((short) 0);
        output.putInt(payload.length);
        output.putLong(header.sequence);
        output.putLong(header.timestampMicros);
        output.putInt(crc32(payload, 0, payload.length));
        output.putInt(crc32(packet, 0, 36));
        output.put(payload);
        return packet;
    }

    static Packet decode(byte[] packet) throws ProtocolException {
        if (packet.length < HEADER_SIZE) {
            throw new ProtocolException("truncated header");
        }

        HeaderFields fields = validateHeader(Arrays.copyOf(packet, HEADER_SIZE));
        if (packet.length != HEADER_SIZE + fields.payloadSize) {
            throw new ProtocolException("packet size mismatch");
        }
        if (crc32(packet, HEADER_SIZE, fields.payloadSize) != fields.payloadCrc) {
            throw new ProtocolException("payload checksum mismatch");
        }

        return new Packet(
                new Header(fields.type, fields.flags, fields.sequence, fields.timestampMicros),
                Arrays.copyOfRange(packet, HEADER_SIZE, packet.length));
    }

    static Packet readPacket(InputStream input) throws IOException {
        byte[] header = readExactly(input, HEADER_SIZE);
        HeaderFields fields = validateHeader(header);
        byte[] payload = readExactly(input, fields.payloadSize);
        byte[] packet = new byte[HEADER_SIZE + payload.length];
        System.arraycopy(header, 0, packet, 0, HEADER_SIZE);
        System.arraycopy(payload, 0, packet, HEADER_SIZE, payload.length);
        return decode(packet);
    }

    private static HeaderFields validateHeader(byte[] header) throws ProtocolException {
        if (!Arrays.equals(Arrays.copyOf(header, MAGIC.length), MAGIC)) {
            throw new ProtocolException("invalid magic");
        }

        ByteBuffer input = ByteBuffer.wrap(header).order(ByteOrder.LITTLE_ENDIAN);
        input.position(4);
        int major = Byte.toUnsignedInt(input.get());
        input.get();
        int type = Byte.toUnsignedInt(input.get());
        int flags = Byte.toUnsignedInt(input.get());
        int headerSize = Short.toUnsignedInt(input.getShort());
        int reserved = Short.toUnsignedInt(input.getShort());
        long payloadSizeLong = Integer.toUnsignedLong(input.getInt());
        long sequence = input.getLong();
        long timestampMicros = input.getLong();
        int payloadCrc = input.getInt();
        int headerCrc = input.getInt();

        if (major != VERSION_MAJOR) {
            throw new ProtocolException("unsupported protocol version");
        }
        if (headerSize != HEADER_SIZE) {
            throw new ProtocolException("invalid header size");
        }
        if (reserved != 0) {
            throw new ProtocolException("reserved field is not zero");
        }
        if (payloadSizeLong > MAX_PAYLOAD_SIZE) {
            throw new ProtocolException("payload is too large");
        }
        if (crc32(header, 0, 36) != headerCrc) {
            throw new ProtocolException("header checksum mismatch");
        }

        return new HeaderFields(
                type,
                flags,
                sequence,
                timestampMicros,
                (int) payloadSizeLong,
                payloadCrc);
    }

    private static byte[] readExactly(InputStream input, int size) throws IOException {
        byte[] result = new byte[size];
        int offset = 0;
        while (offset < size) {
            int read = input.read(result, offset, size - offset);
            if (read < 0) {
                throw new EOFException("WireView connection closed in the middle of a packet");
            }
            offset += read;
        }
        return result;
    }

    private static int crc32(byte[] bytes, int offset, int length) {
        CRC32 crc = new CRC32();
        crc.update(bytes, offset, length);
        return (int) crc.getValue();
    }

    private static final class HeaderFields {
        final int type;
        final int flags;
        final long sequence;
        final long timestampMicros;
        final int payloadSize;
        final int payloadCrc;

        HeaderFields(
                int type,
                int flags,
                long sequence,
                long timestampMicros,
                int payloadSize,
                int payloadCrc) {
            this.type = type;
            this.flags = flags;
            this.sequence = sequence;
            this.timestampMicros = timestampMicros;
            this.payloadSize = payloadSize;
            this.payloadCrc = payloadCrc;
        }
    }
}
