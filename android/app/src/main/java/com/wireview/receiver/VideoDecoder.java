package com.wireview.receiver;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileWriter;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

final class VideoDecoder implements AutoCloseable {
    interface Listener {
        void onDecoderStatus(String status);

        void onStreamConfigured(int width, int height, int frameRate);

        void onFirstFrameRendered(long presentationTimeUs, long startupMillis);

        void onFrameSurfaceTarget(long presentationTimeUs, long wantedNanos);

        void onDecoderHealth(
                long receivedFrames,
                long queuedFrames,
                long droppedFrames,
                int decoderErrors,
                boolean decoderActive,
                boolean firstFrameRendered);

        void onFrameTiming(
                long sequence,
                long hostTimestampMicros,
                long arrivalMicros,
                long surfaceReleaseMicros);

        void onKeyframeRequired(long droppedFrames, String reason);
    }

    private static final String TAG = "WireViewDecoder";
    private static final int STREAM_CONFIG_SIZE = 16;
    private static final int CODEC_AVC = 1;
    private static final int FORMAT_ANNEX_B = 1;
    private static final long INPUT_WAIT_BUDGET_US = 24_000L;
    private static final long OUTPUT_WAIT_US = 10_000L;
    private static final long MI8_REFRESH_NANOS = 1_000_000_000L / 60L;

    private final CodecProbe.Result selectedDecoder;
    private final boolean forceLowLatency;
    private final FramePresentationClock mi8PresentationClock;
    private final FramePresentationClock highRefreshPresentationClock = new FramePresentationClock();
    private final Listener listener;
    private final File traceFile;
    private final OneRefreshDemoClock demoClock;
    private BufferedWriter pipelineTrace;
    private int traceRows;
    private final ExecutorService worker;
    private DecoderOutputPump outputPump;

    private Surface surface;
    private StreamConfig streamConfig;
    private long streamConfigArrivalNanos;
    private long streamWorkerNanos;
    private MediaCodec codec;
    private volatile boolean firstFrameRendered;
    private volatile long firstInputQueuedNanos;
    private long codecGeneration;
    private volatile long receivedFrames;
    private volatile long queuedFrames;
    private volatile long droppedFrames;
    private boolean awaitingRecoveryKeyframe;
    private volatile int decoderErrors;
    private long lastHealthReportNanos;
    private final Map<Long, FrameTiming> pendingFrameTimings = new ConcurrentHashMap<>();

    VideoDecoder(
            CodecProbe.Result selectedDecoder,
            boolean forceLowLatency,
            Listener listener,
            File filesDirectory, long gridNs, long periodNs, long leadNs, int warmupFrames) {
        this.traceFile = new File(filesDirectory, "pipeline-android.csv");
        this.demoClock = new OneRefreshDemoClock(gridNs, periodNs, leadNs, warmupFrames);
        this.selectedDecoder = selectedDecoder;
        this.forceLowLatency = forceLowLatency;
        this.mi8PresentationClock = Build.VERSION.SDK_INT == Build.VERSION_CODES.Q
                && "dipper".equals(Build.DEVICE)
                && "OMX.qcom.video.decoder.avc".equals(selectedDecoder.name)
                ? new FramePresentationClock() : null;
        this.listener = listener;
        worker = Executors.newSingleThreadExecutor(runnable -> {
            Thread thread = new Thread(runnable, "WireViewVideoDecoder");
            thread.setDaemon(true);
            return thread;
        });
    }

    void setSurface(Surface newSurface) {
        worker.execute(() -> {
            releaseCodec();
            surface = newSurface;
            if (surface != null && surface.isValid() && streamConfig != null) {
                configureCodec();
            }
        });
    }

    void startStream(byte[] payload) {
        final long receivedNanos = System.nanoTime();
        final StreamConfig config;
        try {
            config = StreamConfig.parse(payload);
        } catch (IllegalArgumentException error) {
            listener.onDecoderStatus("视频配置无效：" + error.getMessage());
            return;
        }

        worker.execute(() -> {
            streamWorkerNanos = System.nanoTime();
            streamConfigArrivalNanos = receivedNanos;
            releaseCodec();
            openPipelineTrace();
            streamConfig = config;
            if (surface != null && surface.isValid()) {
                configureCodec();
            } else {
                listener.onDecoderStatus("已收到视频配置，等待 Surface");
            }
        });
    }

    void queueFrame(Protocol.Packet packet) {
        long arrivalMicros = System.nanoTime() / 1_000L;
        if (BuildConfig.CURSOR_DIAGNOSTICS && mi8PresentationClock == null
                && !isHighRefresh(streamConfig)) {
            // release(true) keeps the video PTS as the requested Surface timestamp.
            // The collector still checks actual presentation before counting a frame.
            final long pts = packet.header.timestampMicros;
            listener.onFrameSurfaceTarget(pts, pts * 1_000L);
        }
        worker.execute(() -> queueFrameOnWorker(packet, arrivalMicros));
    }

    void stopStream() {
        worker.execute(() -> {
            releaseCodec();
            closePipelineTrace();
            streamConfig = null;
            listener.onDecoderStatus("视频流已停止");
        });
    }

    private void configureCodec() {
        if (streamConfig == null || surface == null || !surface.isValid()) {
            return;
        }

        resetDecoderHealth();
        final long beginNanos = System.nanoTime();
        try {
            MediaFormat format = MediaFormat.createVideoFormat(
                    MediaFormat.MIMETYPE_VIDEO_AVC,
                    streamConfig.width,
                    streamConfig.height);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, streamConfig.frameRateNumerator);
            format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024);
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
                    && (selectedDecoder.lowLatency || forceLowLatency)) {
                format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
            }

            final long createBeginNanos = System.nanoTime();
            codec = MediaCodec.createByCodecName(selectedDecoder.name);
            final long createdNanos = System.nanoTime();
            applyVendorLowLatencyParameters(codec, format);
            final long configureBeginNanos = System.nanoTime();
            codec.configure(format, surface, null, 0);
            final long configuredNanos = System.nanoTime();
            highRefreshPresentationClock.reset(
                    streamConfig.frameRateNumerator, streamConfig.frameRateDenominator);
            if (mi8PresentationClock != null) {
                mi8PresentationClock.reset(streamConfig.frameRateNumerator, streamConfig.frameRateDenominator);
            }
            codec.start();
            final long startedNanos = System.nanoTime();
            firstFrameRendered = false;
            firstInputQueuedNanos = 0L;
            listener.onStreamConfigured(
                    streamConfig.width,
                    streamConfig.height,
                    streamConfig.frameRateNumerator);
            reportDecoderHealth(true);
            listener.onDecoderStatus(
                    "解码器已启动：" + streamConfig.width + "×" + streamConfig.height
                            + "@" + streamConfig.frameRateNumerator);
            Log.i(TAG, "Configured " + selectedDecoder.name + " with " + streamConfig
                    + ", standardLowLatency=" + selectedDecoder.lowLatency
                    + ", forcedLowLatency=" + forceLowLatency + ", outputMode=dedicated");
            PersistentDiagnosticLog.record(TAG, "startupUs"
                    + " configReceived=" + streamConfigArrivalNanos / 1_000L
                    + " worker=" + streamWorkerNanos / 1_000L
                    + " begin=" + beginNanos / 1_000L
                    + " createBegin=" + createBeginNanos / 1_000L
                    + " created=" + createdNanos / 1_000L
                    + " configureBegin=" + configureBeginNanos / 1_000L
                    + " configured=" + configuredNanos / 1_000L
                    + " started=" + startedNanos / 1_000L);
            final long generation = codecGeneration;
            outputPump = new DecoderOutputPump(() -> drainOutput(OUTPUT_WAIT_US), error ->
                    worker.execute(() -> {
                        if (generation == codecGeneration) {
                            Log.e(TAG, "Decoder failed while polling output", error);
                            ++decoderErrors;
                            releaseCodec();
                            reportDecoderHealth(true);
                            listener.onDecoderStatus("解码失败：" + error.getMessage());
                        }
                    }));
        } catch (IOException | IllegalArgumentException | IllegalStateException error) {
            Log.e(TAG, "Cannot configure decoder", error);
            ++decoderErrors;
            releaseCodec();
            reportDecoderHealth(true);
            listener.onDecoderStatus("解码器启动失败：" + error.getMessage());
        }
    }

    private void queueFrameOnWorker(Protocol.Packet packet, long arrivalMicros) {
        final long workerMicros = System.nanoTime() / 1_000L;
        if (codec == null) {
            return;
        }

        ++receivedFrames;

        try {
            final boolean keyframe = (packet.header.flags & 1) != 0;
            if (awaitingRecoveryKeyframe && !keyframe) {
                ++droppedFrames;
                reportDecoderHealth(false);
                return;
            }
            int inputIndex = codec.dequeueInputBuffer(INPUT_WAIT_BUDGET_US);
            if (inputIndex < 0) {
                requestRecoveryKeyframe(packet, "没有可用解码输入缓冲区");
                return;
            }

            ByteBuffer input = codec.getInputBuffer(inputIndex);
            if (input == null || input.remaining() < packet.payload.length) {
                codec.queueInputBuffer(inputIndex, 0, 0, packet.header.timestampMicros, 0);
                requestRecoveryKeyframe(packet, "视频帧超过解码输入缓冲区");
                return;
            }

            input.clear();
            input.put(packet.payload);
            if (firstInputQueuedNanos == 0L) {
                firstInputQueuedNanos = System.nanoTime();
            }
            FrameTiming timing = new FrameTiming(packet.header.sequence, packet.header.timestampMicros,
                    arrivalMicros, workerMicros, keyframe);
            pendingFrameTimings.put(packet.header.timestampMicros, timing);
            timing.queueStartMicros = System.nanoTime() / 1_000L;
            codec.queueInputBuffer(
                    inputIndex,
                    0,
                    packet.payload.length,
                    packet.header.timestampMicros,
                    0);
            final long queueReturnMicros = System.nanoTime() / 1_000L;
            if (BuildConfig.CURSOR_DIAGNOSTICS) {
                synchronized (timing) {
                    timing.queueReturnMicros = queueReturnMicros;
                    if (timing.releaseMicros != 0L) {
                        recordPipelineFrame(timing);
                    }
                }
            }
            if (awaitingRecoveryKeyframe && keyframe) {
                awaitingRecoveryKeyframe = false;
                listener.onDecoderStatus("已收到恢复关键帧，解码继续");
                Log.i(TAG, "Recovery keyframe queued: " + packet.header.sequence);
            }
            ++queuedFrames;
            reportDecoderHealth(false);
        } catch (IllegalStateException error) {
            Log.e(TAG, "Decoder failed while queuing a frame", error);
            ++decoderErrors;
            releaseCodec();
            reportDecoderHealth(true);
            listener.onDecoderStatus("解码失败：" + error.getMessage());
        }
    }

    private void requestRecoveryKeyframe(Protocol.Packet packet, String reason) {
        ++droppedFrames;
        if (!awaitingRecoveryKeyframe) {
            awaitingRecoveryKeyframe = true;
            Log.w(TAG, "Requesting recovery keyframe after frame " + packet.header.sequence
                    + ": " + reason);
            listener.onDecoderStatus("解码缓冲不足，正在请求 Windows 发送恢复关键帧");
            listener.onKeyframeRequired(droppedFrames, reason);
        }
        reportDecoderHealth(true);
    }

    private static boolean isHighRefresh(StreamConfig config) {
        return config != null
                && config.frameRateNumerator >= 120L * config.frameRateDenominator;
    }

    private void drainOutput(long firstTimeoutMicros) {
        final StreamConfig config = streamConfig;
        final boolean paceHighRefresh = isHighRefresh(config);
        final boolean paceMi8 = mi8PresentationClock != null && config != null
                && config.width == 2248 && config.height == 1080
                && config.frameRateNumerator == 60L * config.frameRateDenominator;
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long timeoutMicros = firstTimeoutMicros;
        while (codec != null) {
            int outputIndex = codec.dequeueOutputBuffer(info, timeoutMicros);
            timeoutMicros = 0L;
            if (outputIndex >= 0) {
                final long readyNs = System.nanoTime();
                final long outputMicros = readyNs / 1_000L;
                // On the Mi 8, using Android time lets the display catch up instead of
                // queuing buffers with Windows timestamps. One refresh of headroom keeps
                // most of the native 60 Hz stream in separate display slots.
                // High-rate streams also use Android time with one refresh of headroom;
                // the existing clock bounds each target within two refreshes of readiness.
                final long targetNs = paceMi8 ? mi8PresentationClock.renderTimeNanos(
                        info.presentationTimeUs, readyNs + MI8_REFRESH_NANOS)
                        : paceHighRefresh ? highRefreshPresentationClock.renderTimeNanos(
                                info.presentationTimeUs, readyNs + 1_000_000_000L
                                        * config.frameRateDenominator / config.frameRateNumerator) : 0L;
                if (targetNs == 0L) {
                    codec.releaseOutputBuffer(outputIndex, true);
                } else {
                    codec.releaseOutputBuffer(outputIndex, targetNs);
                }
                final long releaseMicros = System.nanoTime() / 1_000L;
                if (BuildConfig.CURSOR_DIAGNOSTICS && (mi8PresentationClock != null || paceHighRefresh)) {
                    listener.onFrameSurfaceTarget(info.presentationTimeUs,
                            targetNs != 0L ? targetNs : info.presentationTimeUs * 1_000L);
                }
                FrameTiming timing = pendingFrameTimings.remove(info.presentationTimeUs);
                if (timing != null && BuildConfig.CURSOR_DIAGNOSTICS) {
                    synchronized (timing) {
                        timing.outputMicros = outputMicros;
                        timing.readyNs = readyNs;
                        timing.targetNs = targetNs;
                        timing.releaseMicros = releaseMicros;
                        timing.pendingAfterOutput = pendingFrameTimings.size();
                        if (timing.queueReturnMicros != 0L) {
                            recordPipelineFrame(timing);
                        }
                    }
                }
                if (timing != null && timing.sequence % 60L == 0L) {
                    listener.onFrameTiming(
                            timing.sequence,
                            timing.hostTimestampMicros,
                            timing.arrivalMicros,
                            releaseMicros);
                }
                if (!firstFrameRendered) {
                    firstFrameRendered = true;
                    long startupMillis = firstInputQueuedNanos == 0L
                            ? -1L
                            : TimeUnit.NANOSECONDS.toMillis(
                                    System.nanoTime() - firstInputQueuedNanos);
                    Log.i(TAG, "First decoded frame released to Surface, pts="
                            + info.presentationTimeUs + ", startupMs=" + startupMillis);
                    listener.onDecoderStatus("首帧已解码并提交到屏幕（" + startupMillis + "ms）");
                    listener.onFirstFrameRendered(info.presentationTimeUs, startupMillis);
                    reportDecoderHealth(true);
                }
                continue;
            }
            if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "Output format: " + codec.getOutputFormat());
                continue;
            }
            break;
        }
    }

    private void resetDecoderHealth() {
        receivedFrames = 0L;
        queuedFrames = 0L;
        droppedFrames = 0L;
        awaitingRecoveryKeyframe = false;
        decoderErrors = 0;
        lastHealthReportNanos = 0L;
    }

    private synchronized void reportDecoderHealth(boolean force) {
        long now = System.nanoTime();
        if (!force && lastHealthReportNanos != 0L
                && now - lastHealthReportNanos < TimeUnit.SECONDS.toNanos(1L)) {
            return;
        }
        lastHealthReportNanos = now;
        listener.onDecoderHealth(
                receivedFrames,
                queuedFrames,
                droppedFrames,
                decoderErrors,
                codec != null,
                firstFrameRendered);
    }

    private void releaseCodec() {
        ++codecGeneration;
        if (outputPump != null) {
            outputPump.close();
            outputPump = null;
        }
        if (codec == null) {
            pendingFrameTimings.clear();
            return;
        }
        try {
            codec.stop();
        } catch (IllegalStateException ignored) {
            // The codec may already be in an error state.
        }
        codec.release();
        codec = null;
        pendingFrameTimings.clear();
    }

    private void applyVendorLowLatencyParameters(MediaCodec activeCodec, MediaFormat format) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) {
            return;
        }

        List<String> parameters = activeCodec.getSupportedVendorParameters();
        final String pictureOrder = "vendor.qti-ext-dec-picture-order.enable";
        if (BuildConfig.CURSOR_DIAGNOSTICS
                && streamConfig.frameRateNumerator >= 120L * streamConfig.frameRateDenominator
                && activeCodec.getName().equals("c2.qti.avc.decoder")
                && parameters.contains(pictureOrder)) {
            // The WireView AVC encoders produce the stream without B-frames.
            format.setInteger(pictureOrder, 1);
            Log.i(TAG, "Requested Qualcomm decode-order output");
        }
        for (String parameter : parameters) {
            String normalized = parameter.toLowerCase(Locale.ROOT);
            if (!normalized.contains("latency")) {
                continue;
            }

            MediaCodec.ParameterDescriptor descriptor =
                    activeCodec.getParameterDescriptor(parameter);
            int type = descriptor == null ? MediaFormat.TYPE_NULL : descriptor.getType();
            Log.i(TAG, "Vendor latency parameter: " + parameter + ", type=" + type);
            if (forceLowLatency && type == MediaFormat.TYPE_INTEGER
                    && (normalized.contains("low-latency")
                    || normalized.contains("low_latency")
                    || normalized.contains("lowlatency"))) {
                format.setInteger(parameter, 1);
                Log.i(TAG, "Requested vendor low-latency parameter: " + parameter);
            }
        }
    }

    private void openPipelineTrace() {
        closePipelineTrace();
        if (!BuildConfig.CURSOR_DIAGNOSTICS) {
            return;
        }
        try {
            pipelineTrace = new BufferedWriter(new FileWriter(traceFile));
            pipelineTrace.write("pts_us,sequence,keyframe,arrival_us,worker_us,queue_begin_us,"
                    + "queue_end_us,output_us,release_us,pending_after_output,ready_ns,target_ns\n");
            traceRows = 0;
        } catch (IOException error) {
            Log.w(TAG, "Cannot open decoder pipeline trace", error);
            closePipelineTrace();
        }
    }

    private synchronized void recordPipelineFrame(FrameTiming timing) {
        if (pipelineTrace == null) {
            return;
        }
        try {
            pipelineTrace.write(timing.hostTimestampMicros + "," + timing.sequence + ","
                    + (timing.keyframe ? 1 : 0) + "," + timing.arrivalMicros + ","
                    + timing.workerMicros + "," + timing.queueStartMicros + "," + timing.queueReturnMicros
                    + "," + timing.outputMicros + "," + timing.releaseMicros + ","
                    + timing.pendingAfterOutput + "," + timing.readyNs + "," + timing.targetNs + "\n");
            if (++traceRows % 32 == 0) {
                pipelineTrace.flush();
            }
        } catch (IOException error) {
            Log.w(TAG, "Cannot write decoder pipeline trace", error);
            closePipelineTrace();
        }
    }

    private void closePipelineTrace() {
        if (pipelineTrace == null) {
            return;
        }
        try {
            pipelineTrace.close();
        } catch (IOException error) {
            Log.w(TAG, "Cannot close decoder pipeline trace", error);
        }
        pipelineTrace = null;
    }

    @Override
    public void close() {
        worker.execute(() -> {
            try {
                releaseCodec();
                closePipelineTrace();
            } finally {
                worker.shutdown();
            }
        });
        try {
            worker.awaitTermination(2, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        }
    }

    private static final class StreamConfig {
        final int width;
        final int height;
        final int frameRateNumerator;
        final int frameRateDenominator;

        StreamConfig(int width, int height, int frameRateNumerator, int frameRateDenominator) {
            this.width = width;
            this.height = height;
            this.frameRateNumerator = frameRateNumerator;
            this.frameRateDenominator = frameRateDenominator;
        }

        static StreamConfig parse(byte[] payload) {
            if (payload.length != STREAM_CONFIG_SIZE) {
                throw new IllegalArgumentException("长度应为 " + STREAM_CONFIG_SIZE);
            }

            ByteBuffer input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
            int codec = Byte.toUnsignedInt(input.get());
            int bitstreamFormat = Byte.toUnsignedInt(input.get());
            int reserved = Short.toUnsignedInt(input.getShort());
            int width = Short.toUnsignedInt(input.getShort());
            int height = Short.toUnsignedInt(input.getShort());
            int fpsNumerator = Short.toUnsignedInt(input.getShort());
            int fpsDenominator = Short.toUnsignedInt(input.getShort());
            int flags = input.getInt();

            if (codec != CODEC_AVC || bitstreamFormat != FORMAT_ANNEX_B) {
                throw new IllegalArgumentException("仅支持 H.264 Annex-B");
            }
            if (reserved != 0 || flags != 0) {
                throw new IllegalArgumentException("保留字段必须为零");
            }
            if (width == 0 || height == 0 || fpsNumerator == 0 || fpsDenominator == 0) {
                throw new IllegalArgumentException("尺寸或帧率为零");
            }
            return new StreamConfig(width, height, fpsNumerator, fpsDenominator);
        }

        @Override
        public String toString() {
            return width + "x" + height + "@" + frameRateNumerator + "/" + frameRateDenominator;
        }
    }

    private static final class FrameTiming {
        final long sequence;
        final long hostTimestampMicros;
        final long arrivalMicros;
        final long workerMicros;
        final boolean keyframe;
        long queueStartMicros;
        long queueReturnMicros;
        long outputMicros;
        long releaseMicros;
        int pendingAfterOutput;
        long readyNs, targetNs;

        FrameTiming(long sequence, long hostTimestampMicros, long arrivalMicros,
                long workerMicros, boolean keyframe) {
            this.sequence = sequence;
            this.hostTimestampMicros = hostTimestampMicros;
            this.arrivalMicros = arrivalMicros;
            this.workerMicros = workerMicros;
            this.keyframe = keyframe;
        }
    }
}
