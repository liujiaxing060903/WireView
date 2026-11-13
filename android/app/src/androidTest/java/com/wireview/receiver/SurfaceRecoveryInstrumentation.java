package com.wireview.receiver;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.view.SurfaceView;
import android.view.SurfaceHolder;
import android.view.View;

import java.lang.reflect.Field;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Device-only integration test. Requires a running log-only desktop Host.
 * Uses framework Instrumentation, with no downloaded JUnit/AndroidX dependency.
 * Reflection stays in the test APK; the shipping app has no test commands.
 */
public final class SurfaceRecoveryInstrumentation extends Instrumentation {
    private static Object field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    @Override public void onCreate(Bundle arguments) { super.onCreate(arguments); start(); }

    private boolean firstFrame(Object decoder) throws Exception {
        ExecutorService worker = (ExecutorService) field(decoder, "worker");
        return worker.submit(() -> (Boolean) field(decoder, "firstFrameRendered"))
                .get(3, TimeUnit.SECONDS);
    }

    private void waitForFrame(Object decoder) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(12);
        while (System.nanoTime() < deadline) {
            if (firstFrame(decoder)) { return; }
            Thread.sleep(50);
        }
        throw new AssertionError("No frame rendered after codec/Surface creation");
    }

    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            Intent intent = new Intent(Intent.ACTION_MAIN)
                    .setClassName("com.wireview.receiver", "com.wireview.receiver.MainActivity")
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            Activity activity = startActivitySync(intent);
            Object decoder = field(activity, "videoDecoder");
            SurfaceView surface = (SurfaceView) field(activity, "displaySurface");
            waitForFrame(decoder);
            AtomicInteger created = new AtomicInteger();
            AtomicInteger destroyed = new AtomicInteger();
            runOnMainSync(() -> surface.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder holder) { created.incrementAndGet(); }
                @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) { }
                @Override public void surfaceDestroyed(SurfaceHolder holder) { destroyed.incrementAndGet(); }
            }));
            ExecutorService worker = (ExecutorService) field(decoder, "worker");
            long generation = worker.submit(() -> (Long) field(decoder, "codecGeneration")).get(3, TimeUnit.SECONDS);
            runOnMainSync(() -> surface.setVisibility(View.GONE));
            long destroyedDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (destroyed.get() == 0 && System.nanoTime() < destroyedDeadline) { Thread.sleep(25); }
            if (destroyed.get() == 0) { throw new AssertionError("No actual surfaceDestroyed callback"); }
            runOnMainSync(() -> surface.setVisibility(View.VISIBLE));
            // Do not wait for a globally idle UI queue while continuous video
            // drives layout/telemetry. Await the actual Surface/codec events.
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
            boolean rebuilt = false;
            while (System.nanoTime() < deadline) {
                rebuilt = worker.submit(() -> (Long) field(decoder, "codecGeneration") > generation &&
                        field(decoder, "codec") != null).get(3, TimeUnit.SECONDS);
                if (rebuilt) { break; }
                Thread.sleep(50);
            }
            if (!rebuilt) {
                String snapshot = worker.submit(() -> "generation=" + field(decoder, "codecGeneration") +
                        ", old=" + generation + ", errors=" + field(decoder, "decoderErrors") +
                        ", codec=" + field(decoder, "codec") + ", surface=" + field(decoder, "surface") +
                        ", configured=" + (field(decoder, "streamConfig") != null)).get(3, TimeUnit.SECONDS);
                throw new AssertionError("Surface did not recreate codec: created=" + created + ", destroyed=" + destroyed +
                        ", holderValid=" + surface.getHolder().getSurface().isValid() + ", " + snapshot);
            }
            waitForFrame(decoder);
            // Keep the target process alive until Host's explicit STOP, so a
            // successful test does not itself look like an Android crash/EOF.
            long endDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(25);
            while (System.nanoTime() < endDeadline &&
                    worker.submit(() -> field(decoder, "streamConfig") != null).get(3, TimeUnit.SECONDS)) {
                Thread.sleep(100);
            }
            result.putString("stream", "Surface recreated; second decoded frame rendered; check Host for >=2 keyframe requests and one handshake");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("error", error.toString());
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
