package com.wireview.receiver;

import android.view.MotionEvent;
import android.view.View;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

final class InputBatch {
    private static final int SCHEMA_VERSION = 1;
    private static final int RECORD_SIZE = 20;
    private static final int HEADER_SIZE = 8;

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
        int recordCount = includeAllPointers ? event.getPointerCount() : 1;
        if (recordCount <= 0 || recordCount > 64) {
            return null;
        }

        ByteBuffer output = ByteBuffer
                .allocate(HEADER_SIZE + recordCount * RECORD_SIZE)
                .order(ByteOrder.LITTLE_ENDIAN);
        output.put((byte) SCHEMA_VERSION);
        output.put((byte) RECORD_SIZE);
        output.putShort((short) recordCount);
        output.putShort((short) width);
        output.putShort((short) height);

        for (int record = 0; record < recordCount; record++) {
            int pointerIndex = includeAllPointers ? record : event.getActionIndex();
            int tool = mapTool(event.getToolType(pointerIndex));
            int normalizedX = normalize(event.getX(pointerIndex), width);
            int normalizedY = normalize(event.getY(pointerIndex), height);
            int pressure = normalizeUnit(event.getPressure(pointerIndex));

            float tiltRadians = event.getAxisValue(MotionEvent.AXIS_TILT, pointerIndex);
            float orientation = event.getOrientation(pointerIndex);
            int tiltX = clampTiltHundredths(
                    Math.toDegrees(tiltRadians * Math.sin(orientation)));
            int tiltY = clampTiltHundredths(
                    Math.toDegrees(-tiltRadians * Math.cos(orientation)));

            output.put((byte) wireAction);
            output.put((byte) tool);
            output.putShort((short) event.getPointerId(pointerIndex));
            output.putShort((short) normalizedX);
            output.putShort((short) normalizedY);
            output.putShort((short) pressure);
            output.putShort((short) tiltX);
            output.putShort((short) tiltY);
            output.putShort((short) (event.getButtonState() & 0xffff));
            output.putInt(0);
        }
        return output.array();
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
