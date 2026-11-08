package com.wireview.receiver;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;

/**
 * WiFi transport: the tablet listens and the Windows host connects. This is
 * the role-reversed sibling of {@link TcpTransportConnector}: in WiFi mode
 * the PC is the TCP client, so {@link #open()} blocks in accept until the
 * host dials in. Closing the connector aborts a pending accept.
 */
final class WifiServerConnector implements TransportConnector {
    static final int DEFAULT_PORT = 39873;

    private final int port;
    private volatile ServerSocket serverSocket;

    WifiServerConnector() {
        this(DEFAULT_PORT);
    }

    WifiServerConnector(int port) {
        this.port = port;
    }

    @Override
    public TransportConnection open() throws IOException {
        ServerSocket server = new ServerSocket();
        serverSocket = server;
        try {
            try {
                server.bind(new InetSocketAddress(InetAddress.getByName("0.0.0.0"), port), 1);
            } catch (IOException bindFailure) {
                // A silent bind failure (e.g. the port is occupied) used to
                // retry invisibly once per round with an empty diagnostic
                // log; leave a trace naming the port (A7 claim 4).
                PersistentDiagnosticLog.record(
                        "WifiServerConnector",
                        "bind failed on port " + port + ": " + bindFailure.getMessage());
                throw bindFailure;
            }
            // Polling accept: a 2 s bound lets the connection loop re-probe
            // USB (hybrid selection) and notice a flipped switch instead of
            // parking here forever when the PC never dials in.
            server.setSoTimeout(2_000);
            final Socket socket;
            try {
                socket = server.accept();
            } catch (SocketTimeoutException idlePoll) {
                throw idlePoll; // the normal "no dial-in this round" path
            } catch (IOException acceptFailure) {
                PersistentDiagnosticLog.record(
                        "WifiServerConnector",
                        "accept failed on port " + port + ": " + acceptFailure.getMessage());
                throw acceptFailure;
            }
            try {
                socket.setTcpNoDelay(true);
                socket.setKeepAlive(true);
            } catch (IOException error) {
                try {
                    socket.close();
                } catch (IOException ignored) {
                    // Report the original socket option failure.
                }
                throw error;
            }
            return new WifiConnection(socket);
        } finally {
            // One accepted connection per session (concurrency = 1): the
            // listener's job is done until the next connection loop round.
            closeListener();
        }
    }

    /** Aborts a pending accept (switch off / transport switch / shutdown). */
    void shutdown() {
        closeListener();
    }

    private void closeListener() {
        ServerSocket server = serverSocket;
        serverSocket = null;
        if (server != null && !server.isClosed()) {
            try {
                server.close();
            } catch (IOException ignored) {
                // Best effort during switch-off.
            }
        }
    }

    private static final class WifiConnection implements TransportConnection {
        private final Socket socket;

        WifiConnection(Socket socket) {
            this.socket = socket;
        }

        @Override
        public InputStream inputStream() throws IOException {
            return socket.getInputStream();
        }

        @Override
        public OutputStream outputStream() throws IOException {
            return socket.getOutputStream();
        }

        @Override
        public void setReadTimeout(int timeoutMillis) throws IOException {
            socket.setSoTimeout(timeoutMillis);
        }

        @Override
        public String displayName() {
            return "WiFi";
        }

        @Override
        public void close() throws IOException {
            socket.close();
        }
    }
}
