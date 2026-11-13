package android.os;

import java.io.Closeable;
import java.io.FileDescriptor;
import java.net.Socket;

/** JVM-only descriptor boundary. Socket I/O still uses the real java.net.Socket. */
public final class ParcelFileDescriptor implements Closeable {
    private final FileDescriptor descriptor = new FileDescriptor();
    public static ParcelFileDescriptor fromSocket(Socket socket) {
        if (socket == null || !socket.isConnected()) { throw new IllegalArgumentException("socket"); }
        return new ParcelFileDescriptor();
    }
    public FileDescriptor getFileDescriptor() { return descriptor; }
    @Override public void close() { }
}
