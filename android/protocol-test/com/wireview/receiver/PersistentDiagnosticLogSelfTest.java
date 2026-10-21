package com.wireview.receiver;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

public final class PersistentDiagnosticLogSelfTest {
    private PersistentDiagnosticLogSelfTest() {
    }

    public static void main(String[] arguments) throws Exception {
        Path directory = Files.createTempDirectory("wireview-diagnostic-log-");
        try {
            PersistentDiagnosticLog.initialize(directory.toFile());
            PersistentDiagnosticLog.record("test", "first\nline");
            String initial = Files.readString(
                    directory.resolve(PersistentDiagnosticLog.CURRENT_FILE_NAME),
                    StandardCharsets.UTF_8);
            require(initial.contains("DiagnosticLog | initialized"), "initialization was not logged");
            require(initial.contains("test | first line"), "message was not sanitized and logged");

            String payload = "x".repeat(4_096);
            for (int index = 0; index < 40; ++index) {
                PersistentDiagnosticLog.record("rotation", index + "-" + payload);
            }
            Path current = directory.resolve(PersistentDiagnosticLog.CURRENT_FILE_NAME);
            Path previous = directory.resolve(PersistentDiagnosticLog.PREVIOUS_FILE_NAME);
            require(Files.exists(current), "current diagnostic log is missing");
            require(Files.exists(previous), "diagnostic log did not rotate");
            require(
                    Files.size(current) <= PersistentDiagnosticLog.MAX_FILE_BYTES + 4_096L,
                    "current diagnostic log exceeded its bound");
            require(
                    Files.size(previous) <= PersistentDiagnosticLog.MAX_FILE_BYTES + 4_096L,
                    "previous diagnostic log exceeded its bound");
            System.out.println("All WireView persistent diagnostic log tests passed");
        } finally {
            try (var paths = Files.walk(directory)) {
                paths.sorted((left, right) -> right.compareTo(left))
                        .forEach(path -> {
                            try {
                                Files.deleteIfExists(path);
                            } catch (Exception ignored) {
                                // Best-effort cleanup for the self-test directory.
                            }
                        });
            }
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new IllegalStateException(message);
        }
    }
}
