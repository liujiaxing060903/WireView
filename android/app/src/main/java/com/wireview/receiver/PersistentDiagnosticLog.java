package com.wireview.receiver;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.time.Instant;

/** Small app-private diagnostic trace that survives an AOA re-enumeration. */
final class PersistentDiagnosticLog {
    static final String CURRENT_FILE_NAME = "usb-diagnostic.log";
    static final String PREVIOUS_FILE_NAME = "usb-diagnostic.previous.log";
    static final long MAX_FILE_BYTES = 128L * 1024L;
    private static final int MAX_THROWABLE_CHARACTERS = 8 * 1024;

    private static File currentFile;
    private static File previousFile;

    private PersistentDiagnosticLog() {
    }

    static synchronized void initialize(File filesDirectory) {
        if (filesDirectory == null) {
            return;
        }
        if (!filesDirectory.exists() && !filesDirectory.mkdirs()) {
            return;
        }
        currentFile = new File(filesDirectory, CURRENT_FILE_NAME);
        previousFile = new File(filesDirectory, PREVIOUS_FILE_NAME);
        record("DiagnosticLog", "initialized");
    }

    static synchronized void record(String component, String message) {
        record(component, message, null);
    }

    static synchronized void record(String component, String message, Throwable error) {
        if (currentFile == null) {
            return;
        }
        StringBuilder entry = new StringBuilder()
                .append(Instant.now())
                .append(" | ")
                .append(Thread.currentThread().getName())
                .append(" | ")
                .append(singleLine(component))
                .append(" | ")
                .append(singleLine(message))
                .append('\n');
        if (error != null) {
            StringWriter stack = new StringWriter();
            error.printStackTrace(new PrintWriter(stack));
            String stackText = stack.toString();
            if (stackText.length() > MAX_THROWABLE_CHARACTERS) {
                stackText = stackText.substring(0, MAX_THROWABLE_CHARACTERS)
                        + "\n... stack trace truncated\n";
            }
            entry.append(stackText);
        }
        byte[] bytes = entry.toString().getBytes(StandardCharsets.UTF_8);
        rotateIfNeeded(bytes.length);
        try (FileOutputStream output = new FileOutputStream(currentFile, true)) {
            output.write(bytes);
            output.flush();
        } catch (IOException ignored) {
            // Diagnostics must never disrupt display or input behavior.
        }
    }

    private static void rotateIfNeeded(int incomingBytes) {
        if (!currentFile.exists()
                || currentFile.length() + incomingBytes <= MAX_FILE_BYTES) {
            return;
        }
        if (previousFile.exists()) {
            previousFile.delete();
        }
        if (currentFile.renameTo(previousFile)) {
            currentFile = new File(currentFile.getParentFile(), CURRENT_FILE_NAME);
            return;
        }
        try (FileOutputStream ignored = new FileOutputStream(currentFile, false)) {
            // If rotation is unavailable, truncate rather than grow without bound.
        } catch (IOException ignored) {
            // Diagnostics must never disrupt display or input behavior.
        }
    }

    private static String singleLine(String value) {
        if (value == null) {
            return "";
        }
        return value.replace('\r', ' ').replace('\n', ' ');
    }
}
