package com.wireview.receiver;

final class VersionNumber {
    static boolean isNewer(String latest, String current) {
        long[] a = parts(latest), b = parts(current);
        for (int i = 0; i < 3; i++) {
            if (a[i] != b[i]) return a[i] > b[i];
        }
        return false;
    }

    private static long[] parts(String value) {
        if (!value.matches("[vV]?\\d+\\.\\d+\\.\\d+")) throw new IllegalArgumentException("Invalid version");
        String[] pieces = value.replaceFirst("^[vV]", "").split("\\.");
        return new long[] {Long.parseLong(pieces[0]), Long.parseLong(pieces[1]), Long.parseLong(pieces[2])};
    }
}
