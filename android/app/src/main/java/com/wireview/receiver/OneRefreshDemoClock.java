package com.wireview.receiver;

import java.util.Arrays;

/** A test-only clock: map sequential input frames to the measured display grid. */
final class OneRefreshDemoClock {
    static final long MAX_WAIT_NS = 16_666_666L;
    private final long origin;
    private final long period;
    private final int warmupFrames;
    private final long[] samples = new long[60];
    private long firstPtsUs, lastReadyNs, baseTarget;
    private int count, remainingWarmup;

    OneRefreshDemoClock(long gridNs, long periodNs, long leadNs, int warmupFrames) {
        origin = gridNs - Math.max(500_000L, Math.min(7_500_000L, leadNs));
        period = periodNs >= 8_300_000L && periodNs <= 8_400_000L ? periodNs : 0L;
        this.warmupFrames = Math.max(0, warmupFrames);
    }

    long target(long ptsUs, long readyNs) {
        if (origin <= 0L || period == 0L || ptsUs <= 0L) {
            return 0L; // The ordinary immediate path also serves as the demo baseline.
        }
        if (lastReadyNs == 0L || readyNs - lastReadyNs > 80_000_000L || ptsUs < firstPtsUs) {
            firstPtsUs = ptsUs;
            count = 0;
            remainingWarmup = warmupFrames;
        }
        lastReadyNs = readyNs;
        if (remainingWarmup > 0) {
            --remainingWarmup;
            return 0L;
        }
        long index = ((ptsUs - firstPtsUs) * 120L + 500_000L) / 1_000_000L;
        if (count < samples.length) {
            samples[count++] = readyNs - index * period;
            if (count == samples.length) {
                Arrays.sort(samples);
                long wanted = samples[count / 2] + 5_300_000L;
                baseTarget = origin + Math.floorDiv(wanted - origin + period / 2, period) * period;
            }
            return 0L;
        }
        long desired = baseTarget + index * period;
        return Math.max(readyNs, Math.min(readyNs + Math.min(2L * period, MAX_WAIT_NS), desired));
    }
}
