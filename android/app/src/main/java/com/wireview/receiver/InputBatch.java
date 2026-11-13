package com.wireview.receiver;

import android.view.MotionEvent;
import android.view.View;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class InputBatch {
    private static final int SCHEMA_VERSION = 1;
    private static final int RECORD_SIZE = 20;
    private static final int HEADER_SIZE = 8;
    private static final int MAX_RECORDS = 64;

    private static final int ACTION_DOWN = 1;
    private static final int ACTION_MOVE = 2;
    private static final int ACTION_UP = 3;
    private static final int ACTION_CANCEL = 4;
    private static final int ACTION_HOVER = 5;

    private static final int TOOL_FINGER = 1;
    private static final int TOOL_STYLUS = 2;
    private static final int TOOL_ERASER = 3;
    private static final int TOOL_MOUSE = 4;

    private InputBatch() {
    }

    static byte[] fromMotionEvent(MotionEvent event, View viewport) {
        int width = viewport.getWidth();
        int height = viewport.getHeight();
        if (width <= 0 || height <= 0 || width > 0xffff || height > 0xffff) {
            return null;
        }

        int maskedAction = event.getActionMasked();
        int wireAction = mapAction(maskedAction);
        if (wireAction == 0) {
            return null;
        }

        boolean includeAllPointers = maskedAction == MotionEvent.ACTION_MOVE
                || maskedAction == MotionEvent.ACTION_CANCEL
                || maskedAction == MotionEvent.ACTION_HOVER_MOVE;
        int currentCount = includeAllPointers ? event.getPointerCount() : 1;
        if (currentCount <= 0 || currentCount > MAX_RECORDS) {
            return null;
        }

        int historySize = maskedAction == MotionEvent.ACTION_MOVE ? event.getHistorySize() : 0;
        int penCount = 0;
        if (historySize > 0) {
            for (int index = 0; index < currentCount; index++) {
                if (isPenTool(mapTool(event.getToolType(index)))) {
                    penCount++;
                }
            }
        }
        // Preserve recent pen samples within the existing packet size limit.
        int historyCount = penCount == 0 ? 0 : Math.min(historySize,
                (MAX_RECORDS - currentCount) / penCount);
        int recordCount = currentCount + historyCount * penCount;
        ByteBuffer output = ByteBuffer
                .allocate(HEADER_SIZE + recordCount * RECORD_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN);
        output.put((byte) SCHEMA_VERSION);
        output.put((byte) RECORD_SIZE);
        output.putShort((short) recordCount);
        output.putShort((short) width);
        output.putShort((short) height);

        MotionEvent.PointerCoords point = new MotionEvent.PointerCoords();
        int buttons = event.getButtonState();
        for (int history = historySize - historyCount; history < historySize; history++) {
            for (int index = 0; index < currentCount; index++) {
                int tool = mapTool(event.getToolType(index));
                if (isPenTool(tool)) {
                    event.getHistoricalPointerCoords(index, history, point);
                    writeRecord(output, wireAction, tool, event.getPointerId(index),
                            point, width, height, buttons);
                }
            }
        }
        for (int record = 0; record < currentCount; record++) {
            int pointerIndex = includeAllPointers ? record : event.getActionIndex();
            int tool = mapTool(event.getToolType(pointerIndex));
            event.getPointerCoords(pointerIndex, point);
            writeRecord(output, wireAction, tool, event.getPointerId(pointerIndex),
                    point, width, height, buttons);
        }
        return output.array();
    }

    private static boolean isPenTool(int tool) {
        return tool == TOOL_STYLUS || tool == TOOL_ERASER;
    }

    private static void writeRecord(ByteBuffer output, int action, int tool, int pointerId,
            MotionEvent.PointerCoords point, int width, int height, int buttons) {
        float tilt = point.getAxisValue(MotionEvent.AXIS_TILT);
        int tiltX = clampTiltHundredths(Math.toDegrees(tilt * Math.sin(point.orientation)));
        int tiltY = clampTiltHundredths(Math.toDegrees(-tilt * Math.cos(point.orientation)));

        output.put((byte) action);
        output.put((byte) tool);
        output.putShort((short) pointerId);
        output.putShort((short) normalize(point.x, width));
        output.putShort((short) normalize(point.y, height));
        output.putShort((short) normalizeUnit(point.pressure));
        output.putShort((short) tiltX);
        output.putShort((short) tiltY);
        output.putShort((short) buttons);
        output.putInt(0);
    }

    private static int mapAction(int action) {
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                return ACTION_DOWN;
            case MotionEvent.ACTION_MOVE:
                return ACTION_MOVE;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
                return ACTION_UP;
            case MotionEvent.ACTION_CANCEL:
                return ACTION_CANCEL;
            case MotionEvent.ACTION_HOVER_MOVE:
                return ACTION_HOVER;
            default:
                return 0;
        }
    }

    private static int mapTool(int toolType) {
        switch (toolType) {
            case MotionEvent.TOOL_TYPE_STYLUS:
                return TOOL_STYLUS;
            case MotionEvent.TOOL_TYPE_ERASER:
                return TOOL_ERASER;
            case MotionEvent.TOOL_TYPE_MOUSE:
                return TOOL_MOUSE;
            case MotionEvent.TOOL_TYPE_FINGER:
            case MotionEvent.TOOL_TYPE_UNKNOWN:
            default:
                return TOOL_FINGER;
        }
    }

    private static int normalize(float coordinate, int extent) {
        if (extent <= 1) {
            return 0;
        }
        float clamped = Math.max(0.0f, Math.min(coordinate, extent - 1.0f));
        return Math.round(clamped * 65535.0f / (extent - 1.0f));
    }

    private static int normalizeUnit(float value) {
        float clamped = Math.max(0.0f, Math.min(value, 1.0f));
        return Math.round(clamped * 65535.0f);
    }

    private static int clampTiltHundredths(double degrees) {
        return (int) Math.round(Math.max(-90.0, Math.min(degrees, 90.0)) * 100.0);
    }
}
