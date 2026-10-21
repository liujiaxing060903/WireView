package com.wireview.receiver;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;

/** Current development transport reached through {@code adb reverse}. */
final class TcpTransportConnector implements TransportConnector {
    private final String host;
    private final int port;
    private final int connectTimeoutMillis;

    TcpTransportConnector(String host, int port) {
        this(host, port, 1_000);
    }

    TcpTransportConnector(String host, int port, int connectTimeoutMillis) {
        this.host = host;
        this.port = port;
        this.connectTimeoutMillis = connectTimeoutMillis;
    }

    @Override
    public TransportConnection open() throws IOException {
        Socket socket = new Socket();
        try {
            socket.connect(new InetSocketAddress(host, port), connectTimeoutMillis);
            socket.setTcpNoDelay(true);
            socket.setKeepAlive(true);
            return new TcpConnection(socket);
        } catch (IOException error) {
            try {
                socket.close();
            } catch (IOException ignored) {
                // Preserve the connection failure that explains why open failed.
            }
            throw error;
        }
    }

    private static final class TcpConnection implements TransportConnection {
        private final Socket socket;

        TcpConnection(Socket socket) {
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
        public String displayName() {
            return "ADB/TCP";
        }

        @Override
        public void close() throws IOException {
            socket.close();
        }
    }
}
