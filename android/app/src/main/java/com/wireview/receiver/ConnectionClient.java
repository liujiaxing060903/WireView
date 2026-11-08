package com.wireview.receiver;

import android.content.Context;
import android.os.Build;
import android.os.SystemClock;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
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

        // R3 WiFi: called on the connection thread during a pairing attempt;
        // the implementation must block until the user submits the PIN shown
        // on the PC (or cancels, returning null).
        String requestPairingPin(String pcName);

        // R3 WiFi: pairing completed (or re-pairing refreshed) with this PC.
        void onWifiPairingStateChanged(boolean paired, String pcName);
    }

    static final int DEVELOPMENT_PORT = 39871;

    private final Listener listener;
    private final Context appContext;
    private final TransportConnector defaultConnector;
    // Quick USB probe used while the WiFi switch is on: if the PC-side host
    // is listening again, USB takes over immediately instead of waiting for
    // the WiFi accept loop.
    private final TransportConnector usbProbeConnector;
    private final AtomicBoolean running = new AtomicBoolean(false);
    private final AtomicLong outboundSequence = new AtomicLong(2L);
    private final Object writeLock = new Object();
    private final WifiPairing.TabletCredentials credentials;
    private ExecutorService executor;
    private ExecutorService writer;
    private volatile TransportConnection connection;
    private volatile WifiServerConnector wifiConnector;
    private volatile boolean wifiRequested;
    // Pairing PIN delivered over USB (adb broadcast): the out-of-band channel
    // is the cable itself, so pairing needs no manual PIN typing. One-shot,
    // 60 second validity, consumed by the next mode=pairing challenge.
    private volatile String approvedPairingToken;
    private volatile long approvedPairingTokenAtMillis;
    private String lastStatus;

    ConnectionClient(Listener listener, Context appContext) {
        this(listener, appContext, new TcpTransportConnector("127.0.0.1", DEVELOPMENT_PORT), false);
    }

    ConnectionClient(
            Listener listener,
            Context appContext,
            TransportConnector defaultConnector,
            boolean wifiEnabled) {
        this.listener = listener;
        this.appContext = appContext;
        this.defaultConnector = defaultConnector;
        this.usbProbeConnector =
            defaultConnector instanceof TcpTransportConnector
                ? new TcpTransportConnector("127.0.0.1", DEVELOPMENT_PORT, 300)
                : defaultConnector;
        this.wifiRequested = wifiEnabled;
        this.credentials = new WifiPairing.TabletCredentials(appContext);
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

    /** Enables or disables the WiFi listener at runtime (tablet-side switch). */
    void setWifiEnabled(boolean enabled) {
        if (wifiRequested == enabled) {
            return;
        }
        wifiRequested = enabled;
        PersistentDiagnosticLog.record("ConnectionClient", "wifi transport " + (enabled ? "enabled" : "disabled"));
        if (!enabled) {
            // Close any pending accept so the loop observes the switch
            // immediately instead of waiting for a connection.
            WifiServerConnector connector = wifiConnector;
            wifiConnector = null;
            if (connector != null) {
                connector.shutdown();
            }
        }
    }

    boolean isWifiEnabled() {
        return wifiRequested;
    }

    /** Registers a PIN delivered out-of-band over USB for USB-free-of-PIN pairing. */
    void setApprovedPairingToken(String token) {
        approvedPairingToken = token;
        approvedPairingTokenAtMillis = SystemClock.uptimeMillis();
        PersistentDiagnosticLog.record("ConnectionClient", "pairing token received over USB");
    }

    private String consumeApprovedPairingToken() {
        String token = approvedPairingToken;
        if (token == null) {
            return null;
        }
        approvedPairingToken = null;
        final long age = SystemClock.uptimeMillis() - approvedPairingTokenAtMillis;
        if (age > 60_000L) {
            PersistentDiagnosticLog.record("ConnectionClient", "pairing token expired");
            return null;
        }
        return token;
    }

    private TransportConnector nextConnector() {
        if (wifiRequested) {
            if (wifiConnector == null) {
                wifiConnector = new WifiServerConnector();
            }
            return wifiConnector;
        }
        return defaultConnector;
    }

    /** The transport chosen for one connection round. */
    private static final class RoundConnection {
        final TransportConnection connection;
        final boolean isWifi;

        RoundConnection(TransportConnection connection, boolean isWifi) {
            this.connection = connection;
            this.isWifi = isWifi;
        }
    }

    /**
     * Picks this round's transport. With the WiFi switch on, USB still gets
     * a quick probe first: a listening host takes the connection instantly
     * (USB recovery), and only when it is gone does the loop park in the
     * WiFi accept waiting for a dial-in.
     *
     * <p>The probe result must NOT be opened inside try-with-resources:
     * returning it from there closes the socket before the caller uses it
     * (audit P0 - the closed connection turned every USB recovery round
     * into a deterministic failure loop). The opened connection is handed
     * to the caller unconsumed; the caller owns closing it.</p>
     */
    private RoundConnection nextConnection(TransportConnector loopConnector)
            throws IOException {
        if (loopConnector instanceof WifiServerConnector && wifiRequested
                && defaultConnector instanceof TcpTransportConnector) {
            try {
                TransportConnection probe = usbProbeConnector.open();
                // A live probe connection IS the session (the PC-side host
                // accepted it); USB recovery takes over this round.
                return new RoundConnection(probe, false);
            } catch (IOException usbDown) {
                // USB host gone: fall through to the WiFi accept.
            }
        }
        return new RoundConnection(loopConnector.open(),
                loopConnector instanceof WifiServerConnector);
    }

    private void connectionLoop() {
        publishStatus(waitingStatus());
        while (running.get()) {
            boolean handshakeCompleted = false;
            boolean wifiTransport = wifiRequested;
            if (isUsbAccessory()) {
                PersistentDiagnosticLog.record("ConnectionClient", "opening USB transport");
            }
            final TransportConnector loopConnector = nextConnector();
            final RoundConnection round;
            try {
                round = nextConnection(loopConnector);
            } catch (IOException openFailure) {
                if (running.get()) {
                    publishStatus(waitingStatus());
                    sleepBeforeRetry();
                }
                continue;
            }
            try (TransportConnection activeConnection = round.connection) {
                connection = activeConnection;
                InputStream input = activeConnection.inputStream();
                OutputStream output = activeConnection.outputStream();
                wifiTransport = round.isWifi;
                if (wifiTransport) {
                    // The WiFi handshake is adversarial: bound the AUTH and
                    // HELLO exchange so a half-open or hostile dial cannot
                    // pin this thread forever. Flow reads revert to no
                    // timeout once the handshake is complete.
                    try {
                        activeConnection.setReadTimeout(5_000);
                    } catch (IOException ignored) {
                        // Option unsupported: the exchange still bounds itself
                        // by the PC closing on its own timeouts.
                    }
                }
                publishStatus("已连接（" + activeConnection.displayName() + "），等待 WireView 握手");
                if (isUsbAccessory()) {
                    PersistentDiagnosticLog.record(
                            "ConnectionClient",
                            "transport opened; blocking for WVR1 HELLO");
                }

                if (wifiTransport) {
                    performWifiHandshake(input, output);
                    // Handshake done: release the read timeout for the flow.
                    try {
                        activeConnection.setReadTimeout(0);
                    } catch (IOException ignored) {
                        // A dead socket will fail on the next read anyway.
                    }
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
                                || isUsbAccessory()
                                || wifiTransport)) {
                    Log.w(TAG, "Connection loop will retry", error);
                    PersistentDiagnosticLog.record(
                            "ConnectionClient",
                            "connection attempt failed: " + error);
                }
                if (running.get()) {
                    // Connection refused is the normal idle state while the
                    // development host is not running. Keep the UI stable
                    // instead of flashing once per retry; detailed failures
                    // remain available through the diagnostic log.
                    publishStatus(waitingStatus());
                    sleepBeforeRetry();
                }
            } finally {
                connection = null;
                if (isUsbAccessory()) {
                    PersistentDiagnosticLog.record(
                            "ConnectionClient", "USB connection attempt ended");
                }
            }
        }
    }

    // ---- R3 WiFi authentication (protocol types 15..17). -------------------

    private void performWifiHandshake(InputStream input, OutputStream output) throws IOException {
        Protocol.Packet packet = Protocol.readPacket(input);
        if (packet.header.type != Protocol.TYPE_AUTH_CHALLENGE) {
            throw new Protocol.ProtocolException("first packet is not AUTH_CHALLENGE");
        }
        final Protocol.AuthChallenge challenge = Protocol.parseAuthChallenge(packet.payload);
        // v2 result integrity: only a PC that set the auth_v2 flag learns we
        // speak schema 2, and only schema 2 makes the tagged (and, when
        // pairing, credential-masking) AUTH_RESULT acceptable.
        final boolean pcIsV2 = (packet.header.flags & Protocol.FLAG_AUTH_V2) != 0;
        final String androidId = WifiPairing.androidId(appContext);
        final byte[] androidIdBytes = androidId.getBytes(StandardCharsets.US_ASCII);
        final byte[] nonceTablet = new byte[Protocol.AUTH_NONCE_SIZE];
        new SecureRandom().nextBytes(nonceTablet);
        final byte[] mac = new byte[Protocol.AUTH_MAC_SIZE];
        byte status = Protocol.AUTH_STATUS_NOT_PAIRED;
        byte[] kdfKey = null;

        final byte[] stored = credentials.get(challenge.pcUuid);
        try {
            if (challenge.mode == Protocol.AUTH_MODE_PAIRING) {
                // Pairing always runs the PIN flow (also on re-pairing after
                // a whitelist delete), but a token delivered over USB
                // replaces the manual entry: the cable is the out-of-band
                // channel.
                status = Protocol.AUTH_STATUS_OK;
                String pin = consumeApprovedPairingToken();
                if (pin == null) {
                    pin = listener.requestPairingPin(challenge.hostName);
                }
                if (pin == null) {
                    publishStatus("已取消配对");
                    throw new IOException("pairing cancelled on the tablet");
                }
                kdfKey = WifiAuthCrypto.derivePairingKey(
                        pin.toCharArray(), challenge.pcUuid, androidIdBytes,
                        challenge.noncePc, nonceTablet);
                final byte[] computed = WifiAuthCrypto.computeMac(
                        kdfKey, Protocol.AUTH_DOMAIN_PAIR, challenge.pcUuid,
                        androidIdBytes, challenge.noncePc, nonceTablet);
                System.arraycopy(computed, 0, mac, 0, mac.length);
            } else if (stored != null) {
                status = Protocol.AUTH_STATUS_OK;
                final byte[] computed = WifiAuthCrypto.computeMac(
                        stored, Protocol.AUTH_DOMAIN_AUTHENTICATE, challenge.pcUuid,
                        androidIdBytes, challenge.noncePc, nonceTablet);
                System.arraycopy(computed, 0, mac, 0, mac.length);
            }
        } catch (java.security.GeneralSecurityException error) {
            PersistentDiagnosticLog.record(
                    "ConnectionClient", "auth crypto failed: " + error);
            throw new IOException("authentication crypto failed", error);
        }

        final Protocol.AuthResponse response = new Protocol.AuthResponse(
                status, nonceTablet, androidId, Build.MODEL == null ? "Android" : Build.MODEL, mac);
        final long timestampMicros = System.nanoTime() / 1_000L;
        synchronized (writeLock) {
            output.write(Protocol.encode(
                    new Protocol.Header(Protocol.TYPE_AUTH_RESPONSE, 0, 1L, timestampMicros),
                    Protocol.encodeAuthResponse(response, pcIsV2)));
            output.flush();
        }
        PersistentDiagnosticLog.record(
                "ConnectionClient",
                "auth response sent: mode=" + challenge.mode + " status=" + status
                        + " v2=" + pcIsV2);

        Protocol.Packet resultPacket = Protocol.readPacket(input);
        if (resultPacket.header.type != Protocol.TYPE_AUTH_RESULT) {
            throw new Protocol.ProtocolException("PC did not return AUTH_RESULT");
        }
        final Protocol.AuthResult result = Protocol.parseAuthResult(resultPacket.payload);
        // The key material the PC would have to prove possession of. It is
        // null exactly when this tablet could not produce a proof either
        // (no credential stored and no PIN derived).
        final byte[] proofKey = kdfKey != null ? kdfKey : stored;
        if (result.result == Protocol.AUTH_RESULT_PAIRING_COMPLETE) {
            if (proofKey == null || !pcIsV2 || result.tag == null) {
                // A legacy PC sends the credential in clear and cannot prove
                // anything; a v2 result must always carry the tag.
                publishStatus("配对失败：电脑端版本过旧（缺少完整性保护）");
                throw new IOException(
                        "pairing result lacks the v2 integrity proof (upgrade the PC)");
            }
            verifyResultTag(proofKey, result, result.credential,
                    challenge.noncePc, nonceTablet);
            final byte[] credential = WifiAuthCrypto.xorMask(
                    result.credential,
                    WifiAuthCrypto.wrapMask(proofKey, challenge.noncePc, nonceTablet));
            credentials.put(challenge.pcUuid, credential, result.message);
            PersistentDiagnosticLog.record(
                    "ConnectionClient", "pairing complete with " + result.message);
            publishStatus("配对成功：" + result.message);
            listener.onWifiPairingStateChanged(true, result.message);
        } else if (result.result == Protocol.AUTH_RESULT_REJECTED) {
            publishStatus("电脑端拒绝了此次连接：" + result.message);
            throw new IOException("PC rejected this device: " + result.message);
        } else {
            // AUTH_RESULT_ACCEPTED: the PC is only credible when this tablet
            // itself holds matching key material (status OK). Without a
            // stored credential the challenge/response above proved nothing
            // and an accept would be an unauthenticated takeover.
            if (status != Protocol.AUTH_STATUS_OK) {
                PersistentDiagnosticLog.record(
                        "ConnectionClient",
                        "rejected AUTH_RESULT_ACCEPTED without a stored credential");
                throw new IOException("PC accepted without any shared credential");
            }
            if (pcIsV2) {
                if (result.tag == null) {
                    throw new IOException("auth result lacks the v2 integrity tag");
                }
                verifyResultTag(proofKey, result, null, challenge.noncePc, nonceTablet);
            }
            // Legacy PC + status OK: keep historical compatibility; the
            // tablet's own response MAC already bound the session keys.
        }
    }

    /** Constant-time check of the v2 AUTH_RESULT tag. */
    private void verifyResultTag(
            byte[] proofKey,
            Protocol.AuthResult result,
            byte[] credentialView,
            byte[] noncePc,
            byte[] nonceTablet)
            throws IOException {
        try {
            final byte[] expected = WifiAuthCrypto.computeResultTag(
                    proofKey, result.result, result.message, credentialView,
                    noncePc, nonceTablet);
            if (!java.security.MessageDigest.isEqual(expected, result.tag)) {
                throw new IOException("AUTH_RESULT integrity tag mismatch");
            }
        } catch (java.security.GeneralSecurityException error) {
            throw new IOException("result tag verification failed", error);
        }
    }

    void sendInputBatch(byte[] payload) {
        sendPacket(Protocol.TYPE_INPUT_BATCH, payload);
    }

    void sendTelemetry(byte[] payload) {
        sendPacket(Protocol.TYPE_TELEMETRY, payload);
    }

    void sendControlCommand(byte[] payload) {
        sendPacket(Protocol.TYPE_CONTROL_COMMAND, payload);
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
            if (type == Protocol.TYPE_CONTROL_COMMAND) {
                // 控制命令丢失必须留痕：侧边栏按钮"无响应"排查全靠它。
                PersistentDiagnosticLog.record("ConnectionClient",
                        "control command dropped: type=" + type
                                + " running=" + running.get()
                                + " connection=" + (activeConnection != null)
                                + " writer=" + (activeWriter != null));
            }
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
        return wifiRequested
                ? "等待电脑端（WiFi 模式：确保电脑端已开启 WiFi 投屏）"
                : "等待电脑端（请保持 USB 连接，并在电脑打开 WireView 控制中心）";
    }

    private boolean isUsbAccessory() {
        return defaultConnector instanceof UsbAccessoryTransportConnector;
    }

    private void publishStatus(String status) {
        if (!status.equals(lastStatus)) {
            lastStatus = status;
            listener.onStatusChanged(status);
        }
    }

    private void sleepBeforeRetry() {
        try {
            Thread.sleep(wifiRequested ? 300L : 1_000L);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    @Override
    public void close() {
        if (isUsbAccessory()) {
            PersistentDiagnosticLog.record("ConnectionClient", "close requested");
        }
        // Close-wake ordering (red-team P1-9): stop the loop first, then
        // close every blocking source so accept/read wake with an error,
        // and only then join the executors.
        running.set(false);
        TransportConnection activeConnection = connection;
        if (activeConnection != null) {
            try {
                activeConnection.close();
            } catch (IOException ignored) {
                // Closing is best effort during Activity shutdown.
            }
        }
        WifiServerConnector connector = wifiConnector;
        if (connector != null) {
            connector.shutdown();
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
