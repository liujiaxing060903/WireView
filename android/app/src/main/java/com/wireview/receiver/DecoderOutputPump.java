package com.wireview.receiver;

/** Owns output dequeue/release independently from codec input and lifecycle. */
final class DecoderOutputPump implements AutoCloseable {
    interface FailureHandler {
        void onFailure(IllegalStateException error);
    }

    private volatile boolean running = true;
    private final Thread thread;

    DecoderOutputPump(Runnable drainOutput, FailureHandler failureHandler) {
        thread = new Thread(() -> {
            try {
                while (running) { drainOutput.run(); }
            } catch (IllegalStateException error) {
                failureHandler.onFailure(error);
            } finally {
                running = false;
            }
        }, "WireViewCodecOutput");
        thread.setDaemon(true);
        thread.start();
    }

    @Override
    public void close() {
        running = false;
        try {
            thread.join();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new IllegalStateException("Interrupted while stopping codec output", error);
        }
    }
}
