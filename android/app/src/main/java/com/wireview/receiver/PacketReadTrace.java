package com.wireview.receiver;

import android.os.Build;
import android.util.Log;
import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.io.FileDescriptor;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import java.io.InputStream;

final class PacketReadTrace extends InputStream {
    static File directory;
    private final InputStream input;
    private final FileDescriptor descriptor;
    private boolean quickAck;
    private int quickAckCalls;
    private final long[] reads = new long[150_000 * 5];
    private final long[] packets = new long[45_000 * 8];
    private int readCount;
    private int packetCount;
    private long packetBegin;
    private long firstVideoPts;
    private boolean recording;
    private boolean overflow;

    PacketReadTrace(InputStream input, FileDescriptor descriptor) {
        this.input = input;
        this.descriptor = descriptor;
    }

    void beginPayload(int flags) throws IOException {
        quickAck = descriptor != null && (flags & 0x80) != 0;
        if (quickAck) { acknowledgeNow(); }
    }

    private void acknowledgeNow() throws IOException {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        try {
            // Linux TCP_QUICKACK, confirmed in the installed Android NDK's linux/tcp.h.
            Os.setsockoptInt(descriptor, OsConstants.IPPROTO_TCP, 12, 1);
            ++quickAckCalls;
        } catch (ErrnoException error) {
            throw new IOException("TCP_QUICKACK failed", error);
        }
    }

    void beginPacket() {
        recording = true;
        quickAckCalls = 0;
        packetBegin = System.nanoTime() / 1_000L;
    }

    @Override public int read() throws IOException {
        return input.read();
    }

    @Override public int read(byte[] data, int offset, int size) throws IOException {
        long begin = System.nanoTime() / 1_000L;
        int count = input.read(data, offset, size);
        long end = System.nanoTime() / 1_000L;
        if (recording && count > 0) {
            if (readCount * 5 + 5 <= reads.length) {
                int p = readCount++ * 5;
                reads[p] = packetCount;
                reads[p + 1] = begin;
                reads[p + 2] = end;
                reads[p + 3] = size;
                reads[p + 4] = count;
            } else {
                overflow = true;
            }
        }
        if (quickAck && count > 0 && count < size) { acknowledgeNow(); }
        return count;
    }

    void endPacket(Protocol.Packet packet, long parsed, long dispatched) {
        if (packet.header.type == Protocol.TYPE_VIDEO_CHUNK && firstVideoPts == 0L) {
            firstVideoPts = packet.header.timestampMicros;
        }
        if (packetCount * 8 + 8 <= packets.length) {
            int p = packetCount++ * 8;
            packets[p] = packetCount - 1;
            packets[p + 1] = packet.header.type;
            packets[p + 2] = packet.header.sequence;
            packets[p + 3] = packet.header.timestampMicros;
            packets[p + 4] = packetBegin;
            packets[p + 5] = parsed;
            packets[p + 6] = dispatched;
            packets[p + 7] = quickAckCalls;
        } else {
            overflow = true;
        }
    }

    void save() {
        if (firstVideoPts == 0L || directory == null) { return; }
        try {
            write("receive-packets.csv", "packet,type,sequence,pts_us,begin_us,parsed_us,dispatched_us,quick_ack_calls",
                  packets, packetCount, 8);
            write("receive-reads.csv", "packet,begin_us,end_us,requested,received", reads, readCount, 5);
            try (BufferedWriter done = new BufferedWriter(new FileWriter(new File(directory, "receive-trace.done")))) {
                done.write(firstVideoPts + "," + packetCount + "," + readCount + "," + overflow + "\n");
            }
        } catch (IOException error) {
            Log.e("WireViewReadTrace", "Unable to save receive trace", error);
        }
    }

    private void write(String name, String header, long[] data, int count, int columns) throws IOException {
        try (BufferedWriter output = new BufferedWriter(new FileWriter(new File(directory, name)))) {
            output.write(header);
            output.newLine();
            for (int row = 0; row < count; ++row) {
                for (int column = 0; column < columns; ++column) {
                    if (column != 0) { output.write(','); }
                    output.write(Long.toString(data[row * columns + column]));
                }
                output.newLine();
            }
        }
    }
}

