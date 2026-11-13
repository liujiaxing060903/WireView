package android.system;

import java.io.FileDescriptor;

public final class Os {
    public static void setsockoptInt(FileDescriptor fd, int level, int option, int value)
            throws ErrnoException {
        throw new UnsupportedOperationException("Android socket options require a real device");
    }
}
