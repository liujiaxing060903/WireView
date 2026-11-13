package com.wireview.receiver;

public final class FramePresentationClockSelfTest {
    private static void check(boolean condition, String message) {
        if (!condition) { throw new AssertionError(message); }
    }

    public static void main(String[] args) {
        FramePresentationClock clock = new FramePresentationClock();
        clock.reset();
        // Actual baseline: Windows PTS ~936373 seconds; Android uptime ~26432s.
        long hostPts = 936_373_467_221L;
        long androidNow = 26_432_603_761_738L;
        long first = clock.renderTimeNanos(hostPts, androidNow);
        check(first == androidNow,
                "the first decoded frame must be available now, without a two-frame cursor delay");
        long second = clock.renderTimeNanos(hostPts + 8_333L, androidNow);
        long third = clock.renderTimeNanos(hostPts + 16_666L, androidNow);
        check(second - first == 8_333_000L,
                "ordinary 120 FPS scheduling must retain its presentation interval");
        check(third >= second && third - androidNow <= 8_333_333L,
                "a decoded burst must favor fresh input over buffering another display refresh");
        long afterStall = clock.renderTimeNanos(hostPts + 25_000L, androidNow + 1_000_000_000L);
        check(afterStall == androidNow + 1_000_000_000L,
                "a late frame must resume now instead of adding another presentation buffer");
        clock.reset();
        long restarted = clock.renderTimeNanos(100L, androidNow + 2_000_000_000L);
        check(restarted == androidNow + 2_000_000_000L,
                "a new codec/session must discard its old timing anchor and added delay");
        clock.reset();
        clock.renderTimeNanos(hostPts, androidNow);
        clock.renderTimeNanos(hostPts + 8_333L, androidNow + 30_000_000L);
        long regular = clock.renderTimeNanos(hostPts + 16_666L, androidNow + 31_000_000L, false);
        check(regular == androidNow + 38_333_000L,
                "ordinary periodic keyframes must retain the current display cadence");
        long recovery = clock.renderTimeNanos(hostPts + 100_000L, androidNow + 60_000_000L, true);
        check(recovery == androidNow + 60_000_000L,
                "a recovery keyframe must not retain the skipped frames' presentation delay");
        long resumed = clock.renderTimeNanos(hostPts + 108_333L, androidNow + 60_000_000L, false);
        check(resumed - recovery == 8_333_000L,
                "frames after recovery must resume a separate 120 FPS cadence");

        // Interaction latency takes priority over displaying every stale frame.
        // A short decoder stall must not add a permanent presentation buffer.
        // Buffers from a burst may share a refresh; steady decoding must still
        // retain the stream's full 120 FPS cadence once the burst has passed.
        clock.reset();
        clock.renderTimeNanos(hostPts, androidNow);
        long previous = clock.renderTimeNanos(hostPts + 8_333L, androidNow + 100_000_000L);
        long steadyFirst = 0L;
        for (int frame = 2; frame <= 240; ++frame) {
            long ptsOffsetUs = frame * 1_000_000L / 120L;
            long ready = frame <= 12
                    ? androidNow + 100_000_000L + (frame - 1L) * 500_000L
                    : androidNow + ptsOffsetUs * 1_000L + 10_000_000L;
            long rendered = clock.renderTimeNanos(hostPts + ptsOffsetUs, ready);
            check(rendered >= previous,
                    "fresh frames must not be scheduled before previously submitted frames");
            check(rendered >= ready && rendered - ready <= 8_333_333L,
                    "a previous decoder stall must not leave more than one refresh of deliberate display waiting");
            if (frame == 180) { steadyFirst = rendered; }
            if (frame == 240) {
                check(Math.abs(rendered - steadyFirst - 500_000_000L) <= 1_000L,
                        "stable decoding must still produce 60 distinct frame slots in half a second");
            }
            previous = rendered;
        }
        System.out.println("Frame presentation clock self-test passed");
    }
}
