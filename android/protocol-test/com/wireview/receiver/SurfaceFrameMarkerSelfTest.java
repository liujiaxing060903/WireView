package com.wireview.receiver;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class SurfaceFrameMarkerSelfTest {
    public static void main(String[] args) {
        long pts = 1_047_431_042_956L;
        long wanted = 1_047_431_042_956_789L;
        byte[] bytes = SurfaceFrameMarker.encode(pts, wanted);
        ByteBuffer decoded = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
        if (bytes.length != 20 || decoded.get() != 1 || decoded.get() != 8 || decoded.getShort() != 0
                || decoded.getLong() != pts || decoded.getLong() != wanted) {
            throw new AssertionError("preserve PTS and exact opaque nanosecond timestamp; do not reinterpret either clock");
        }
        System.out.println("Surface frame marker self-test passed");
    }
}
