package com.wireview.receiver;

import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;

final class ConnectionClient implements AutoCloseable {
    private static final String TAG = "WireViewConnection";
    interface Listener {
        void onStatusChanged(String status);

        void onPacket(Protocol.Packet packet);

        byte[] createHelloAcknowledgementPayload();
    }

    static final int DEVELOPMENT_PORT = 39871;

    private final Listener listener;
    private final TransportConnector connector;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AtomicLong outboundSequence = new AtomicLong(2L);
    private final Object writeLock = new Object();
    private ExecutorService executor;
    private ExecutorService writer;
    private volatile TransportConnection connection;
    private String lastStatus;

    ConnectionClient(Listener listener) {
        this(listener, new TcpTransportConnector("127.0.0.1", DEVELOPMENT_PORT));
    }

    ConnectionClient(Listener listener, TransportConnector connector) {
        this.listener = listener;
        this.connector = connector;
    }

    void start() {
        if (!running.compareAndSet(false, true)) {
            return;
        }
        if (isUsbAccessory()) {
            PersistentDiagnosticLog.record("ConnectionClient", "starting USB connection loop");
        }
        executor = Executors.newSingleThreadExecutor(runnable -> {
            Thread thread = new Thread(runnable, "WireViewConnection");
            thread.setDaemon(true);
            return thread;
        });
        writer = Executors.newSingleThreadExecutor(runnable -> {
            Thread thread = new Thread(runnable, "WireViewInputSender");
            thread.setDaemon(true);
            return thread;
        });
        executor.execute(this::connectionLoop);
    }

    private void connectionLoop() {
        publishStatus(waitingStatus());
        while (running.get()) {
            boolean handshakeCompleted = false;
            if (isUsbAccessory()) {
                PersistentDiagnosticLog.record("ConnectionClient", "opening USB transport");
            }
            try (TransportConnection activeConnection = connector.open()) {
                connection = activeConnection;
                InputStream input = activeConnection.inputStream();
                OutputStream output = activeConnection.outputStream();
                publishStatus("已连接（" + activeConnection.displayName() + "），等待 WireView 握手");
                if (isUsbAccessory()) {
                    PersistentDiagnosticLog.record(
                            "ConnectionClient",
                            "transport opened; blocking for WVR1 HELLO");
                }

                Protocol.Packet hello = Protocol.readPacket(input);
                if (isUsbAccessory()) {
                    PersistentDiagnosticLog.record(
                            "ConnectionClient",
                            "first WVR1 packet type=" + hello.header.type
                                    + " sequence=" + hello.header.sequence);
                }
                if (hello.header.type != Protocol.TYPE_HELLO) {
                    throw new Protocol.ProtocolException("first packet is not HELLO");
                }
                listener.onPacket(hello);

                byte[] deviceProfile = listener.createHelloAcknowledgementPayload();
                outboundSequence.set(2L);
                Protocol.Header ackHeader = new Protocol.Header(
                        Protocol.TYPE_HELLO_ACK,
                        0,
                        hello.header.sequence,
                        System.nanoTime() / 1_000L);
                synchronized (writeLock) {
                    output.write(Protocol.encode(ackHeader, deviceProfile));
                    output.flush();
                }
                if (isUsbAccessory()) {
                    PersistentDiagnosticLog.record(
                            "ConnectionClient",
                            "HELLO_ACK written and flushed");
                }
                publishStatus("握手成功：" + android.os.Build.MODEL);
                handshakeCompleted = true;

                while (running.get()) {
                    listener.onPacket(Protocol.readPacket(input));
                }
            } catch (IOException | RuntimeException error) {
                if (running.get()
                        && (handshakeCompleted
                                || isUsbAccessory())) {
                    Log.w(TAG, "Connection loop will retry", error);
                }
                if (running.get() && isUsbAccessory()) {
                    PersistentDiagnosticLog.record(
                            "ConnectionClient",
                            "USB connection loop failed; retrying",
                            error);
                }
                if (running.get()) {
                    // Connection refused is the normal idle state while the development
                    // host is not running. Keep the UI stable instead of flashing once per
                    // retry; detailed failures remain available through logcat if needed.
                    publishStatus(waitingStatus());
                    sleepBeforeRetry();
                }
            } finally {
                connection = null;
                if (isUsbAccessory()) {
                    PersistentDiagnosticLog.record("ConnectionClient", "USB connection attempt ended");
                }
            }
        }
    }

    void sendInputBatch(byte[] payload) {
        sendPacket(Protocol.TYPE_INPUT_BATCH, payload);
    }

    void sendTelemetry(byte[] payload) {
        sendPacket(Protocol.TYPE_TELEMETRY, payload);
    }

    void requestKeyframe() {
        sendPacket(Protocol.TYPE_KEYFRAME_REQUEST, new byte[0]);
    }

    void sendPong(Protocol.Header request, long receivedMicros) {
        ByteBuffer payload = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        payload.putLong(request.timestampMicros);
        payload.putLong(receivedMicros);
        sendPacket(Protocol.TYPE_PONG, payload.array());
    }

    private void sendPacket(int type, byte[] payload) {
        TransportConnection activeConnection = connection;
        ExecutorService activeWriter = writer;
        if (!running.get() || activeConnection == null || activeWriter == null || payload == null) {
            return;
        }
        long sequence = outboundSequence.getAndIncrement();
        long timestampMicros = System.nanoTime() / 1_000L;
        byte[] packet = Protocol.encode(
                new Protocol.Header(
                        type,
                        0,
                        sequence,
                        timestampMicros),
                payload);
        try {
            activeWriter.execute(() -> {
                if (!running.get() || connection != activeConnection) {
                    return;
                }
                try {
                    synchronized (writeLock) {
                        OutputStream output = activeConnection.outputStream();
                        output.write(packet);
                        output.flush();
                    }
                } catch (IOException error) {
                    try {
                        activeConnection.close();
                    } catch (IOException ignored) {
                        // The connection loop will publish the stable waiting state.
                    }
                }
            });
        } catch (RejectedExecutionException ignored) {
            // Activity shutdown raced with a final pointer event.
        }
    }

    private String waitingStatus() {
        return "等待电脑端（请保持 USB 连接，并在电脑打开 WireView 控制中心）";
    }

    private boolean isUsbAccessory() {
        return connector instanceof UsbAccessoryTransportConnector;
    }

    private void publishStatus(String status) {
        if (!status.equals(lastStatus)) {
            lastStatus = status;
            listener.onStatusChanged(status);
        }
    }

    private void sleepBeforeRetry() {
        try {
            Thread.sleep(1_000L);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    @Override
    public void close() {
        if (isUsbAccessory()) {
            PersistentDiagnosticLog.record("ConnectionClient", "close requested");
        }
        running.set(false);
        TransportConnection activeConnection = connection;
        if (activeConnection != null) {
            try {
                activeConnection.close();
            } catch (IOException ignored) {
                // Closing is best effort during Activity shutdown.
            }
        }
        if (executor != null) {
            executor.shutdownNow();
            try {
                executor.awaitTermination(2, TimeUnit.SECONDS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            executor = null;
        }
        if (writer != null) {
            writer.shutdownNow();
            try {
                writer.awaitTermination(2, TimeUnit.SECONDS);
            } catch (InterruptedException ignored) {
                Thread.currentThread().interrupt();
            }
            writer = null;
        }
    }
}
