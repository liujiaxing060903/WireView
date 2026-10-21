package com.wireview.receiver;

import java.io.InputStream;
import java.io.OutputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

public final class TransportSelfTest {
    private TransportSelfTest() {
    }

    public static void main(String[] args) throws Exception {
        ExecutorService serverExecutor = Executors.newSingleThreadExecutor();
        try (ServerSocket server = new ServerSocket(0)) {
            server.setSoTimeout(3_000);
            Future<?> serverResult = serverExecutor.submit(() -> runServer(server));

            TransportConnector connector =
                    new TcpTransportConnector("127.0.0.1", server.getLocalPort(), 1_000);
            try (TransportConnection connection = connector.open()) {
                require("ADB/TCP".equals(connection.displayName()), "transport name mismatch");
                byte[] request = "windows-to-android".getBytes(StandardCharsets.UTF_8);
                OutputStream output = connection.outputStream();
                output.write(request);
                output.flush();

                byte[] response = readExactly(connection.inputStream(), 18);
                require(
                        Arrays.equals(
                                response,
                                "android-to-windows".getBytes(StandardCharsets.UTF_8)),
                        "transport response mismatch");
            }
            waitForServer(serverResult);
        } finally {
            serverExecutor.shutdownNow();
            serverExecutor.awaitTermination(3, TimeUnit.SECONDS);
        }

        System.out.println("All WireView Java transport tests passed");
    }

    private static void runServer(ServerSocket server) {
        try (Socket socket = server.accept()) {
            socket.setSoTimeout(3_000);
            byte[] request = readExactly(socket.getInputStream(), 18);
            require(
                    Arrays.equals(
                            request,
                            "windows-to-android".getBytes(StandardCharsets.UTF_8)),
                    "transport request mismatch");
            socket.getOutputStream().write("android-to-windows".getBytes(StandardCharsets.UTF_8));
            socket.getOutputStream().flush();
        } catch (Exception error) {
            throw new RuntimeException(error);
        }
    }

    private static byte[] readExactly(InputStream input, int size) throws Exception {
        byte[] bytes = new byte[size];
        int offset = 0;
        while (offset < size) {
            int count = input.read(bytes, offset, size - offset);
            if (count < 0) {
                throw new AssertionError("transport closed early");
            }
            offset += count;
        }
        return bytes;
    }

    private static void waitForServer(Future<?> result) throws Exception {
        try {
            result.get(3, TimeUnit.SECONDS);
        } catch (ExecutionException error) {
            Throwable cause = error.getCause();
            if (cause instanceof Exception exception) {
                throw exception;
            }
            throw error;
        }
    }

    private static void require(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }
}
