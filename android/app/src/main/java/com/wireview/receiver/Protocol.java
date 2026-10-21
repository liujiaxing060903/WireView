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
