package com.wireview.receiver;

public final class VersionNumberSelfTest {
    public static void main(String[] args) {
        if (!VersionNumber.isNewer("v2.0.10", "2.0.9")) throw new AssertionError("numeric patch ordering");
        if (!VersionNumber.isNewer("3.0.0", "2.99.99")) throw new AssertionError("major ordering");
        if (VersionNumber.isNewer("2.0.0", "2.0.0")) throw new AssertionError("equal version");
        if (VersionNumber.isNewer("1.9.9", "2.0.0")) throw new AssertionError("older version");
        try {
            VersionNumber.isNewer("2.0.1-beta", "2.0.0");
            throw new AssertionError("prerelease accepted as stable");
        } catch (IllegalArgumentException expected) { }
        System.out.println("VersionNumberSelfTest passed");
    }
}
