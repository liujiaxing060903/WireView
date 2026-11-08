package com.wireview.receiver;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * One established, reliable, bidirectional byte stream.
 *
 * <p>The WVR1 protocol owns packet framing. Transport implementations only
 * provide ordered bytes and a close operation, whether the underlying link is
 * the current ADB/TCP development tunnel or a future USB accessory channel.</p>
 */
interface TransportConnection extends AutoCloseable {
    InputStream inputStream() throws IOException;

    OutputStream outputStream() throws IOException;

    String displayName();

    /**
     * Bounds how long a blocking read may wait, in milliseconds; zero means
     * forever. Default is a no-op for transports without a socket timeout
     * (the USB accessory streams), which rely on descriptor close to unblock.
     */
    default void setReadTimeout(int timeoutMillis) throws IOException {
    }

    @Override
    void close() throws IOException;
}
