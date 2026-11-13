package com.wireview.receiver;

import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.Semaphore;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class DecoderOutputPumpSelfTest {
    private static void check(boolean condition, String message) {
        if (!condition) { throw new AssertionError(message); }
    }

    private static void await(CountDownLatch latch) {
        try {
            check(latch.await(1, TimeUnit.SECONDS), "test handshake timed out");
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    public static void main(String[] args) throws Exception {
        LinkedBlockingQueue<Integer> readyOutput = new LinkedBlockingQueue<>();
        Semaphore sharedInputSlot = new Semaphore(0);
        AtomicReference<IllegalStateException> failure = new AtomicReference<>();
        try (DecoderOutputPump pump = new DecoderOutputPump(() -> {
            try {
                if (readyOutput.poll(10, TimeUnit.MILLISECONDS) != null) {
                    sharedInputSlot.release();
                }
            } catch (InterruptedException error) {
                throw new IllegalStateException(error);
            }
        }, failure::set)) {
            // The input thread is blocked waiting for a shared codec slot;
            // output still has to run and release that slot independently.
            readyOutput.add(1);
            check(sharedInputSlot.tryAcquire(1, TimeUnit.SECONDS),
                    "output must progress while the input caller is blocked");
            check(failure.get() == null, "output failed while reclaiming a shared slot");
        }

        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch releaseOutput = new CountDownLatch(1);
        CountDownLatch closing = new CountDownLatch(1);
        CountDownLatch closed = new CountDownLatch(1);
        AtomicInteger calls = new AtomicInteger();
        DecoderOutputPump pump = new DecoderOutputPump(() -> {
            calls.incrementAndGet();
            entered.countDown();
            await(releaseOutput);
        }, failure::set);
        await(entered);
        Thread closer = new Thread(() -> {
            closing.countDown();
            pump.close();
            closed.countDown();
        });
        closer.start();
        await(closing);
        try {
            check(!closed.await(30, TimeUnit.MILLISECONDS),
                    "codec cleanup must wait for an in-flight output call");
        } finally {
            releaseOutput.countDown();
        }
        await(closed);
        closer.join();
        check(calls.get() == 1, "the old codec must not be polled after stop");

        CountDownLatch reported = new CountDownLatch(1);
        IllegalStateException expected = new IllegalStateException("codec output failure");
        AtomicInteger reports = new AtomicInteger();
        try (DecoderOutputPump failed = new DecoderOutputPump(() -> { throw expected; }, error -> {
            failure.set(error);
            reports.incrementAndGet();
            reported.countDown();
        })) {
            await(reported);
        }
        check(failure.get() == expected && reports.get() == 1,
                "an output failure must be delivered once, not swallowed or retried forever");
        System.out.println("Decoder output pump self-test passed");
    }
}
