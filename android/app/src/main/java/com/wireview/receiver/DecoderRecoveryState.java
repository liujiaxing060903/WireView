package com.wireview.receiver;

/** Reference-chain and bounded-retry state, confined to the decoder worker. */
final class DecoderRecoveryState {
    private boolean awaitingKeyframe;
    private int codecRetries;

    void newStreamOrSurface() {
        codecRetries = 0;
    }

    boolean requireKeyframe() {
        if (awaitingKeyframe) {
            return false;
        }
        awaitingKeyframe = true;
        return true;
    }

    void codecCreated() {
        // A fresh codec has no reference pictures, even if TCP never stopped.
        awaitingKeyframe = false;
        requireKeyframe();
    }

    boolean accepts(boolean keyframe) {
        return !awaitingKeyframe || keyframe;
    }

    boolean startsReferenceChain(boolean keyframe) {
        return awaitingKeyframe && keyframe;
    }

    boolean queued(boolean keyframe) {
        boolean recovered = startsReferenceChain(keyframe);
        if (recovered) {
            awaitingKeyframe = false;
        }
        return recovered;
    }

    boolean retryCodec() {
        // Do not loop indefinitely on a decoder/Surface/driver failure.
        return codecRetries++ == 0;
    }
}
