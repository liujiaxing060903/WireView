package com.wireview.receiver;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/** A non-interactive, full-screen preview of the eventual fit-center video area. */
final class AspectPreviewView extends View {
    private final Paint framePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint textPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private int contentWidth;
    private int contentHeight;
    private int frameRate;

    AspectPreviewView(Context context) {
        super(context);
        setBackgroundColor(Color.BLACK);
        framePaint.setColor(Color.WHITE);
        framePaint.setStyle(Paint.Style.STROKE);
        framePaint.setStrokeWidth(3.0f);
        textPaint.setColor(Color.WHITE);
        textPaint.setTextAlign(Paint.Align.CENTER);
        textPaint.setTextSize(18.0f * getResources().getDisplayMetrics().scaledDensity);
    }

    void showLayout(int width, int height, int requestedFrameRate) {
        contentWidth = Math.max(0, width);
        contentHeight = Math.max(0, height);
        frameRate = Math.max(0, requestedFrameRate);
        setVisibility(VISIBLE);
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        int viewportWidth = getWidth();
        int viewportHeight = getHeight();
        if (viewportWidth <= 0 || viewportHeight <= 0) {
            return;
        }
        if (contentWidth <= 0 || contentHeight <= 0) {
            canvas.drawRect(2.0f, 2.0f, viewportWidth - 2.0f, viewportHeight - 2.0f, framePaint);
            canvas.drawText("原生（自动）· 全屏显示", viewportWidth / 2.0f,
                    viewportHeight / 2.0f, textPaint);
            return;
        }

        float scale = Math.min(
                (float) viewportWidth / contentWidth,
                (float) viewportHeight / contentHeight);
        float width = contentWidth * scale;
        float height = contentHeight * scale;
        float left = (viewportWidth - width) / 2.0f;
        float top = (viewportHeight - height) / 2.0f;
        RectF frame = new RectF(left + 2.0f, top + 2.0f, left + width - 2.0f, top + height - 2.0f);
        canvas.drawRect(frame, framePaint);
        String label = contentWidth + "×" + contentHeight +
                (frameRate > 0 ? " @ " + frameRate + " FPS" : "");
        canvas.drawText(label, viewportWidth / 2.0f, Math.max(32.0f, top - 18.0f), textPaint);
        canvas.drawText("白框为画面范围；其余区域将保持黑边，不会拉伸", viewportWidth / 2.0f,
                Math.min(viewportHeight - 24.0f, top + height + 32.0f), textPaint);
    }
}
