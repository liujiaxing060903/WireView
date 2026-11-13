package android.util;

public final class Log {
    public static int e(String tag, String message, Throwable error) {
        System.err.println(tag + ": " + message);
        error.printStackTrace();
        return 0;
    }
}
