package com.wireview.receiver;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.util.Log;
import android.view.Surface;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.FutureTask;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;

final class VideoDecoder implements AutoCloseable {
    interface Listener {
        void onDecoderStatus(String status);

        void onStreamConfigured(int width, int height, int frameRate);

        void onFirstFrameRendered(long presentationTimeUs, long startupMillis);

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
    // A 60 FPS frame budget is 16.67 ms. Two short dequeue attempts made
    // some Qualcomm decoders discard an inter frame just before an output
    // buffer became reusable, which corrupts the reference chain until GOP.
    private static final long INPUT_RETRY_TIMEOUT_US = 40_000L;
    // 待解码帧队列上限。到达上限丢弃最旧帧并请求关键帧：低延迟接收端
    // （Moonlight/scrcpy）的标准做法，防止"延迟棘轮"——两端同为 60fps 时
    // FIFO 积压永远不会自行排空，必须靠丢帧腾出余量。
    private static final int MAX_PENDING_FRAMES = 8;
    // 最旧待渲染帧的到达时刻距今超过该值即整管冲刷并请求关键帧。
    private static final long PIPELINE_FLUSH_THRESHOLD_US = 600_000L;
    private static final long OUTPUT_DRAIN_TIMEOUT_US = 0L;
    private static final long OUTPUT_POLL_INTERVAL_MILLIS = 2L;
    // 自愈重建退避（A8 主张6）：连续重建失败达到阈值后，下次重建需距上次
    // 至少 2^n·100ms（n 为连续失败计数，指数封顶防溢出）；退避期内到达的
    // 帧直接丢弃，只保留挂起标记，到期后由下一帧触发重建。
    private static final int REBUILD_BACKOFF_THRESHOLD = 3;
    private static final long REBUILD_BACKOFF_BASE_MILLIS = 100L;
    private static final int REBUILD_BACKOFF_MAX_EXPONENT = 10;
    // surfaceDestroyed 同步排空等待上限（A8 N4）。
    private static final long SURFACE_TEARDOWN_TIMEOUT_SECONDS = 2L;

    private final CodecProbe.Result selectedDecoder;
    private final boolean forceLowLatency;
    private final Listener listener;
    // 声明为具体类型：queueFrame 需要在提交前预检队列深度（A8 主张1）。
    private final ThreadPoolExecutor worker;
    private final ScheduledExecutorService outputPoller;

    private Surface surface;
    private StreamConfig streamConfig;
    private MediaCodec codec;
    private boolean firstFrameRendered;
    private long firstInputQueuedNanos;
    private long codecGeneration;
    private boolean outputPollScheduled;
    private long receivedFrames;
    private long queuedFrames;
    private long droppedFrames;
    private boolean awaitingRecoveryKeyframe;
    private int decoderErrors;
    private long lastHealthReportNanos;
    // 以下三个自愈退避状态仅在 worker 线程读写。
    private int consecutiveRebuildFailures;
    private long lastRebuildAttemptNanos;
    private boolean selfHealRebuildPending;
    private final AtomicLong backlogDrops = new AtomicLong();
    private final AtomicLong lastBacklogKeyframeRequestNanos = new AtomicLong();
    private final AtomicLong lastPipelineFlushNanos = new AtomicLong();
    private final Map<Long, FrameTiming> pendingFrameTimings = new HashMap<>();

    VideoDecoder(
            CodecProbe.Result selectedDecoder,
            boolean forceLowLatency,
            Listener listener) {
        this.selectedDecoder = selectedDecoder;
        this.forceLowLatency = forceLowLatency;
        this.listener = listener;
        worker = new ThreadPoolExecutor(
                1, 1, 0L, TimeUnit.MILLISECONDS,
                new LinkedBlockingQueue<>(MAX_PENDING_FRAMES),
                runnable -> {
                    Thread thread = new Thread(runnable, "WireViewVideoDecoder");
                    thread.setDaemon(true);
                    return thread;
                },
                (runnable, executor) -> {
                    // 队列已满：丢最旧的待解帧，保住最新帧。被丢的可能是
                    // P 帧，参考链可能断裂，因此节流地请求关键帧。
                    backlogDrops.incrementAndGet();
                    executor.getQueue().poll();
                    if (!executor.getQueue().offer(runnable)) {
                        backlogDrops.incrementAndGet();
                        return;
                    }
                    requestBacklogKeyframe();
                });
        outputPoller = Executors.newSingleThreadScheduledExecutor(runnable -> {
            Thread thread = new Thread(runnable, "WireViewOutputPoller");
            thread.setDaemon(true);
            return thread;
        });
    }

    void setSurface(Surface newSurface) {
        // surfaceDestroyed 契约要求返回后不得再使用旧 Surface。移除 Surface
        // 时（唯一调用方是主线程的 surfaceDestroyed 回调）提交任务并同步
        // 等待 worker 排空，保证返回后不再有积压帧任务向已销毁的 Surface
        // 提交（A8 N4）。worker 自身从不调用 setSurface，主线程等待不会
        // 自锁；worker 里的帧任务最坏阻塞约 80ms，2s 上限绰绰有余。
        if (newSurface == null) {
            FutureTask<Void> teardown = new FutureTask<Void>(() -> {
                releaseCodec();
                surface = null;
            }, null);
            worker.execute(teardown);
            if (worker.isShutdown()) {
                // close() 已执行：releaseCodec 已由 close 排入，codec 必然
                // 已释放，等待只会白等 2s（shutdown 后任务可能被静默拒绝）。
                return;
            }
            try {
                teardown.get(SURFACE_TEARDOWN_TIMEOUT_SECONDS, TimeUnit.SECONDS);
            } catch (InterruptedException error) {
                Thread.currentThread().interrupt();
                Log.w(TAG, "Interrupted while waiting for surface teardown");
            } catch (ExecutionException | TimeoutException error) {
                Log.w(TAG, "Surface teardown did not complete in time", error);
            }
            return;
        }
        worker.execute(() -> {
            resetRebuildBackoff();
            releaseCodec();
            surface = newSurface;
            if (surface.isValid() && streamConfig != null) {
                configureCodec();
            }
        });
    }

    void startStream(byte[] payload) {
        final StreamConfig config;
        try {
            config = StreamConfig.parse(payload);
        } catch (IllegalArgumentException error) {
            listener.onDecoderStatus("视频配置无效：" + error.getMessage());
            return;
        }

        worker.execute(() -> {
            // 会话级（重新）配置：自愈退避状态一并复位。
            resetRebuildBackoff();
            // 通道重连（PC 端断线恢复）会重发同一份 start_stream：解码器
            // 仍在位且配置未变时只清状态、等待恢复关键帧，省掉一整套
            // configure+start（参数阶梯探测）。编码器换代（安全桌面重建等）
            // 的重发同为同配置——此时旧参考链必须作废，但 MediaCodec 层
            // 无需重建，flush 后等关键帧即可。
            if (codec != null && streamConfig != null && config.equals(streamConfig)) {
                try {
                    codec.flush();
                } catch (IllegalStateException ignored) {
                    // Fall through to the full rebuild path.
                    releaseCodec();
                    streamConfig = config;
                    if (surface != null && surface.isValid()) {
                        configureCodec();
                    } else {
                        listener.onDecoderStatus("已收到视频配置，等待 Surface");
                    }
                    return;
                }
                pendingFrameTimings.clear();
                awaitingRecoveryKeyframe = true;
                listener.onDecoderStatus("通道已重连，等待恢复关键帧");
                Log.i(TAG, "start_stream repeated with the same config; decoder kept alive");
                return;
            }
            releaseCodec();
            streamConfig = config;
            if (surface != null && surface.isValid()) {
                configureCodec();
            } else {
                listener.onDecoderStatus("已收到视频配置，等待 Surface");
            }
        });
    }

    void queueFrame(Protocol.Packet packet) {
        // 到达侧预检（A8 主张1 推荐修法）：待解帧积压已满时直接丢弃本帧
        // （低延迟接收端的标准丢帧位置），使有界队列的拒绝策略退化为兜底
        // ——控制任务（stop/setSurface/startStream）从此不与帧任务争抢
        // 队头，不会被拒绝策略的无差别 poll() 丢弃。丢弃计数沿用拒绝路径
        // 的 backlogDrops 语义，遥测中与 droppedFrames 合并上报。
        if (worker.getQueue().size() >= MAX_PENDING_FRAMES) {
            backlogDrops.incrementAndGet();
            requestBacklogKeyframe();
            return;
        }
        long arrivalMicros = System.nanoTime() / 1_000L;
        worker.execute(() -> queueFrameOnWorker(packet, arrivalMicros));
    }

    void stopStream() {
        worker.execute(() -> {
            releaseCodec();
            streamConfig = null;
            resetRebuildBackoff();
            listener.onDecoderStatus("视频流已停止");
        });
    }

    private void configureCodec() {
        if (streamConfig == null || surface == null || !surface.isValid()) {
            return;
        }

        resetDecoderHealth();
        // Moonlight 式低延迟参数阶梯：每档独立 configure+start，并用金丝雀
        // dequeue 探测"静默错误态"（部分老 OMX 在 start 后首个 native 调用
        // 才抛异常）。失败即释放并尝试下一档。本流无 B 帧，picture-order
        // （按解码序输出，DPB 不囤帧重排）对高通老解码器收益最大。
        final String[] attemptKeys = {
                null,
                "low-latency",
                "vendor.qti-ext-dec-picture-order.enable",
                "vendor.qti-ext-dec-low-latency.enable",
                "vdec-lowlatency",
        };
        for (final String lowLatencyKey : attemptKeys) {
            try {
                MediaFormat format = MediaFormat.createVideoFormat(
                        MediaFormat.MIMETYPE_VIDEO_AVC,
                        streamConfig.width,
                        streamConfig.height);
                format.setInteger(MediaFormat.KEY_FRAME_RATE, streamConfig.frameRateNumerator);
                format.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024);
                if (lowLatencyKey != null) {
                    format.setInteger(lowLatencyKey, 1);
                }
                if (selectedDecoder.lowLatency || forceLowLatency) {
                    format.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
                }

                codec = MediaCodec.createByCodecName(selectedDecoder.name);
                applyVendorLowLatencyParameters(codec, format);
                codec.configure(format, surface, null, 0);
                codec.start();
                // 金丝雀：错误态的组件会在首个 native 调用抛 IllegalStateException。
                final MediaCodec.BufferInfo canary = new MediaCodec.BufferInfo();
                codec.dequeueOutputBuffer(canary, 0L);
                firstFrameRendered = false;
                firstInputQueuedNanos = 0L;
                listener.onStreamConfigured(
                        streamConfig.width,
                        streamConfig.height,
                        streamConfig.frameRateNumerator);
                reportDecoderHealth(true);
                listener.onDecoderStatus(
                        "解码器已启动：" + streamConfig.width + "×" + streamConfig.height
                                + "@" + streamConfig.frameRateNumerator
                                + (lowLatencyKey == null ? "" : "（" + lowLatencyKey + "）"));
                Log.i(TAG, "Configured " + selectedDecoder.name + " with " + streamConfig
                        + ", lowLatencyKey=" + lowLatencyKey
                        + ", standardLowLatency=" + selectedDecoder.lowLatency
                        + ", forcedLowLatency=" + forceLowLatency);
                return;
            } catch (IOException | IllegalArgumentException | IllegalStateException error) {
                Log.w(TAG, "Decoder config attempt failed (key=" + lowLatencyKey + ")", error);
                releaseCodec();
            }
        }
        ++decoderErrors;
        reportDecoderHealth(true);
        listener.onDecoderStatus("解码器启动失败：所有低延迟参数组合均被拒绝");
    }

    private void queueFrameOnWorker(Protocol.Packet packet, long arrivalMicros) {
        if (codec == null) {
            if (selfHealRebuildPending) {
                // 上次失败后的重建被退避推迟：由新到的帧检查退避是否到期，
                // 到期即重建（不到期则本帧按退避语义丢弃）。
                attemptSelfHealingRebuild();
            }
            return;
        }
        maybeFlushPipeline(arrivalMicros);

        ++receivedFrames;

        try {
            final boolean keyframe = (packet.header.flags & 1) != 0;
            if (awaitingRecoveryKeyframe && !keyframe) {
                ++droppedFrames;
                reportDecoderHealth(false);
                return;
            }
            // Reclaim rendered output before requesting more input. Some hardware
            // decoders expose only a small shared buffer pool, so waiting for input
            // first can cause avoidable starvation during a sustained 60 Hz stream.
            drainOutput(0L);
            int inputIndex = codec.dequeueInputBuffer(0L);
            if (inputIndex < 0) {
                drainOutput(INPUT_RETRY_TIMEOUT_US);
                inputIndex = codec.dequeueInputBuffer(INPUT_RETRY_TIMEOUT_US);
            }
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
            pendingFrameTimings.put(
                    packet.header.timestampMicros,
                    new FrameTiming(packet.header.sequence, packet.header.timestampMicros, arrivalMicros));
            codec.queueInputBuffer(
                    inputIndex,
                    0,
                    packet.payload.length,
                    packet.header.timestampMicros,
                    0);
            if (awaitingRecoveryKeyframe && keyframe) {
                awaitingRecoveryKeyframe = false;
                listener.onDecoderStatus("已收到恢复关键帧，解码继续");
                Log.i(TAG, "Recovery keyframe queued: " + packet.header.sequence);
            }
            ++queuedFrames;
            if (consecutiveRebuildFailures > 0) {
                // 重建后的 codec 成功排入一帧：证明自愈有效，退避计数清零。
                consecutiveRebuildFailures = 0;
            }
            reportDecoderHealth(false);
            // Drain any output that became ready while this input was submitted.
            drainOutput(OUTPUT_DRAIN_TIMEOUT_US);
            scheduleOutputPoll();
        } catch (IllegalStateException error) {
            Log.e(TAG, "Decoder failed while queuing a frame", error);
            ++decoderErrors;
            releaseCodec();
            // 先报失败再重建：若重建成功，configureCodec 的"解码器已启动"
            // 会覆盖这条状态，避免终态停留在误导性的"解码失败"。
            listener.onDecoderStatus("解码失败：" + error.getMessage());
            attemptSelfHealingRebuild();
            reportDecoderHealth(true);
        }
    }

    /**
     * 解码器异常后的自愈重建（A8 主张6+N2）：surface 与流配置仍有效时重建
     * 解码器；连续失败达到 {@link #REBUILD_BACKOFF_THRESHOLD} 后按
     * 2^n·100ms 指数退避，退避期内到达的帧直接丢弃、只保留挂起标记，由
     * 后续帧在到期后重试——避免坏状态的 codec 在 60fps 帧驱动下每帧触发
     * 一次完整的 5 档参数阶梯重建。configureCodec 成功后与 startStream
     * 重连路径对齐：置 awaitingRecoveryKeyframe 并请求关键帧，避免孤 P 帧
     * 喂给全新 codec 花屏到下一个 GOP。全档失败（codec 仍为 null）维持
     * 既有的"等下一次 startStream"终态，退避不改变它。
     */
    private void attemptSelfHealingRebuild() {
        if (surface == null || !surface.isValid() || streamConfig == null) {
            // 没有可用渲染目标或流配置：不自愈，交给 setSurface/startStream。
            selfHealRebuildPending = false;
            return;
        }
        long now = System.nanoTime();
        if (consecutiveRebuildFailures >= REBUILD_BACKOFF_THRESHOLD) {
            long backoffNanos = TimeUnit.MILLISECONDS.toNanos(
                    REBUILD_BACKOFF_BASE_MILLIS
                            * (1L << Math.min(
                                    consecutiveRebuildFailures,
                                    REBUILD_BACKOFF_MAX_EXPONENT)));
            if (now - lastRebuildAttemptNanos < backoffNanos) {
                // 退避期内：跳过重建仅丢帧。
                selfHealRebuildPending = true;
                ++droppedFrames;
                reportDecoderHealth(true);
                return;
            }
        }
        selfHealRebuildPending = false;
        ++consecutiveRebuildFailures;
        lastRebuildAttemptNanos = now;
        configureCodec();
        if (codec != null) {
            // configureCodec 内部的 resetDecoderHealth 会清掉该标志，必须在
            // 成功返回后重新置位（对齐 startStream 重连路径的做法）。
            awaitingRecoveryKeyframe = true;
            listener.onKeyframeRequired(
                    droppedFrames + backlogDrops.get(),
                    "解码器自愈重建，等待关键帧");
        }
    }

    private void resetRebuildBackoff() {
        consecutiveRebuildFailures = 0;
        lastRebuildAttemptNanos = 0L;
        selfHealRebuildPending = false;
    }

    private void requestBacklogKeyframe() {
        long now = System.nanoTime();
        long last = lastBacklogKeyframeRequestNanos.get();
        if (now - last < 500_000_000L) {
            return;
        }
        if (!lastBacklogKeyframeRequestNanos.compareAndSet(last, now)) {
            return;
        }
        Log.w(TAG, "Decoder queue overflow, dropped oldest frames: " + backlogDrops.get());
        listener.onKeyframeRequired(backlogDrops.get(), "解码队列积压，已丢弃旧帧");
    }

    private void maybeFlushPipeline(long arrivalMicros) {
        if (pendingFrameTimings.isEmpty()) {
            return;
        }
        long oldestArrival = Long.MAX_VALUE;
        for (FrameTiming timing : pendingFrameTimings.values()) {
            oldestArrival = Math.min(oldestArrival, timing.arrivalMicros);
        }
        boolean latencyTooHigh = arrivalMicros - oldestArrival > PIPELINE_FLUSH_THRESHOLD_US;
        boolean backlogTooDeep = pendingFrameTimings.size() > 120;
        if (!latencyTooHigh && !backlogTooDeep) {
            return;
        }
        long now = System.nanoTime();
        long last = lastPipelineFlushNanos.get();
        if (now - last < 1_000_000_000L) {
            return;
        }
        if (!lastPipelineFlushNanos.compareAndSet(last, now)) {
            return;
        }
        try {
            codec.flush();
        } catch (IllegalStateException error) {
            return;
        }
        Log.w(TAG, "Pipeline flushed by latency watchdog, pending="
                + pendingFrameTimings.size() + ", latencyUs=" + (arrivalMicros - oldestArrival));
        pendingFrameTimings.clear();
        awaitingRecoveryKeyframe = true;
        listener.onKeyframeRequired(backlogDrops.get(), "解码延迟过高，已冲刷解码管线");
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

    private void scheduleOutputPoll() {
        if (codec == null || outputPollScheduled) {
            return;
        }
        final long generation = codecGeneration;
        outputPollScheduled = true;
        outputPoller.schedule(() -> {
            try {
                worker.execute(() -> {
                    if (generation != codecGeneration) {
                        return;
                    }
                    outputPollScheduled = false;
                    try {
                        drainOutput(0L);
                    } catch (IllegalStateException error) {
                        Log.e(TAG, "Decoder failed while polling output", error);
                        ++decoderErrors;
                        releaseCodec();
                        listener.onDecoderStatus("解码失败：" + error.getMessage());
                        attemptSelfHealingRebuild();
                        reportDecoderHealth(true);
                        return;
                    }
                    if (!pendingFrameTimings.isEmpty()) {
                        scheduleOutputPoll();
                    }
                });
            } catch (RejectedExecutionException ignored) {
                // Activity shutdown raced with a final output poll.
            }
        }, OUTPUT_POLL_INTERVAL_MILLIS, TimeUnit.MILLISECONDS);
    }

    private void drainOutput(long firstTimeoutMicros) {
        // 排空到最新（scrcpy/moonlight 同款）：解码出多帧时只渲染最后一帧，
        // 中间帧不渲染直接归还解码器。积压帧的显示只会贡献延迟，不会带来
        // 信息量；归还动作同时回收输出缓冲，避免解码器反压。
        MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
        long timeoutMicros = firstTimeoutMicros;
        final java.util.ArrayDeque<long[]> pendingOutputs = new java.util.ArrayDeque<>();
        while (codec != null) {
            int outputIndex = codec.dequeueOutputBuffer(info, timeoutMicros);
            timeoutMicros = 0L;
            if (outputIndex >= 0) {
                pendingOutputs.addLast(new long[] {outputIndex, info.presentationTimeUs});
                continue;
            }
            if (outputIndex == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                Log.i(TAG, "Output format: " + codec.getOutputFormat());
                continue;
            }
            break;
        }
        while (!pendingOutputs.isEmpty() && codec != null) {
            final long[] pending = pendingOutputs.pollFirst();
            final boolean render = pendingOutputs.isEmpty();
            codec.releaseOutputBuffer((int) pending[0], render);
            final FrameTiming timing = pendingFrameTimings.remove(pending[1]);
            if (render && timing != null && timing.sequence % 60L == 0L) {
                listener.onFrameTiming(
                        timing.sequence,
                        timing.hostTimestampMicros,
                        timing.arrivalMicros,
                        System.nanoTime() / 1_000L);
            }
            if (render && !firstFrameRendered) {
                firstFrameRendered = true;
                long startupMillis = firstInputQueuedNanos == 0L
                        ? -1L
                        : TimeUnit.NANOSECONDS.toMillis(
                                System.nanoTime() - firstInputQueuedNanos);
                Log.i(TAG, "First decoded frame released to Surface, pts="
                        + pending[1] + ", startupMs=" + startupMillis);
                listener.onDecoderStatus("首帧已解码并提交到屏幕（" + startupMillis + "ms）");
                listener.onFirstFrameRendered(pending[1], startupMillis);
                reportDecoderHealth(true);
            }
        }
    }

    private void resetDecoderHealth() {
        receivedFrames = 0L;
        queuedFrames = 0L;
        droppedFrames = 0L;
        awaitingRecoveryKeyframe = false;
        decoderErrors = 0;
        lastHealthReportNanos = 0L;
        backlogDrops.set(0L);
        lastBacklogKeyframeRequestNanos.set(0L);
        lastPipelineFlushNanos.set(0L);
    }

    private void reportDecoderHealth(boolean force) {
        long now = System.nanoTime();
        if (!force && lastHealthReportNanos != 0L
                && now - lastHealthReportNanos < TimeUnit.SECONDS.toNanos(1L)) {
            return;
        }
        lastHealthReportNanos = now;
        // queuedFrames is a cumulative counter (monotonic); the PC-side
        // adaptive bitrate reads the queued field as CURRENT DEPTH, so
        // report the in-flight size instead (audit P1: the mismatch made
        // every healthy second look congested and pinned the bitrate to
        // the floor).
        listener.onDecoderHealth(
                receivedFrames,
                pendingFrameTimings.size(),
                droppedFrames + backlogDrops.get(),
                decoderErrors,
                codec != null,
                firstFrameRendered);
    }

    private void releaseCodec() {
        ++codecGeneration;
        outputPollScheduled = false;
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

    @Override
    public void close() {
        outputPoller.shutdownNow();
        worker.execute(this::releaseCodec);
        worker.shutdown();
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

        boolean equals(StreamConfig other) {
            return other != null
                    && width == other.width
                    && height == other.height
                    && frameRateNumerator == other.frameRateNumerator
                    && frameRateDenominator == other.frameRateDenominator;
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

        FrameTiming(long sequence, long hostTimestampMicros, long arrivalMicros) {
            this.sequence = sequence;
            this.hostTimestampMicros = hostTimestampMicros;
            this.arrivalMicros = arrivalMicros;
        }
    }
}
