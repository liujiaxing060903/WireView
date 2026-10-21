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

        System.out.println("All WireView Java protocol tests passed");
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }
}

