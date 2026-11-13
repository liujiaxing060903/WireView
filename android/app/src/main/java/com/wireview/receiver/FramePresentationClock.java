package com.wireview.receiver;

/** Converts stream PTS into the Android monotonic time used by SurfaceView. */
final class FramePresentationClock {
    private long originPtsUs;
    private long originNanos;
    private long frameIntervalNanos = 1_000_000_000L / 120L;
    private boolean anchored;

    void reset() {
        anchored = false;
    }

    void reset(int frameRateNumerator, int frameRateDenominator) {
        frameIntervalNanos = 1_000_000_000L * frameRateDenominator / frameRateNumerator;
        reset();
    }

    long renderTimeNanos(long presentationTimeUs, long nowNanos, boolean startsReferenceChain) {
        // Recovery skipped dependent pictures. The new reference chain starts
        // a fresh live timeline, not a delayed playback of the skipped interval.
        if (startsReferenceChain) { reset(); }
        return renderTimeNanos(presentationTimeUs, nowNanos);
    }

    long renderTimeNanos(long presentationTimeUs, long nowNanos) {
        if (!anchored) {
            originPtsUs = presentationTimeUs;
            originNanos = nowNanos;
            anchored = true;
        }
        long renderNanos = originNanos + (presentationTimeUs - originPtsUs) * 1_000L;
        long latestRenderNanos = nowNanos + frameIntervalNanos;
        if (renderNanos < nowNanos || renderNanos > latestRenderNanos) {
            // Interaction takes priority over replaying stale pictures. Retain
            // the PTS cadence while it fits a one-refresh display budget, and
            // re-anchor a burst/stall within that budget. SurfaceView may then
            // coalesce stale pictures at a VSYNC instead of delaying the cursor.
            renderNanos = Math.max(nowNanos, Math.min(renderNanos, latestRenderNanos));
            originPtsUs = presentationTimeUs;
            originNanos = renderNanos;
        }
        return renderNanos;
    }
}
