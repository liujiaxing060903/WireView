package com.wireview.receiver;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Opaque requested buffer timestamp, joined to actual SurfaceFlinger presents. */
final class SurfaceFrameMarker {
    static byte[] encode(long framePtsUs, long surfaceWantedNanos) {
        return ByteBuffer.allocate(20).order(ByteOrder.LITTLE_ENDIAN)
                .put((byte) 1).put((byte) 8).putShort((short) 0)
                .putLong(framePtsUs).putLong(surfaceWantedNanos).array();
    }
}
