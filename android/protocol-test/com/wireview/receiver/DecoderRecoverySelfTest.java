package com.wireview.receiver;

public final class DecoderRecoverySelfTest {
    private static void check(boolean condition, String message) {
        if (!condition) { throw new AssertionError(message); }
    }

    public static void main(String[] args) {
        DecoderRecoveryState state = new DecoderRecoveryState();
        state.newStreamOrSurface();
        state.codecCreated();
        check(!state.accepts(false), "new codec must not consume old P frames");
        check(state.accepts(true), "new codec must accept IDR");
        check(!state.requireKeyframe(), "do not flood keyframe requests");
        check(state.startsReferenceChain(true) && !state.startsReferenceChain(false),
                "pre-submission timing must identify only a recovery keyframe");
        check(!state.accepts(false),
                "registering output correlation must not commit a keyframe before input submission succeeds");
        check(state.queued(true) && state.accepts(false), "resume only after IDR queued");
        check(!state.startsReferenceChain(true), "ordinary periodic IDR must keep the current presentation clock");
        check(state.requireKeyframe(), "dropped frame invalidates reference chain");
        check(!state.accepts(false), "failed IDR queue must keep recovery pending");
        check(state.retryCodec(), "allow one codec restart");
        state.codecCreated();
        check(!state.retryCodec(), "codec creation must not reset retry budget");
        state.newStreamOrSurface();
        state.codecCreated();
        check(!state.accepts(false), "surface replacement needs a new IDR");
        check(state.retryCodec(), "new surface gives a fresh bounded retry budget");
        System.out.println("Decoder recovery self-test passed");
    }
}
