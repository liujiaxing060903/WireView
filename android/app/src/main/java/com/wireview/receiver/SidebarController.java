package com.wireview.receiver;

import android.app.Activity;
import android.content.Context;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.os.Handler;
import android.os.Looper;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.animation.AccelerateInterpolator;
import android.view.animation.DecelerateInterpolator;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.ArrayList;

/**
 * 平板贴边侧边栏：一个挂在 Activity 窗口上的子窗口（TYPE_APPLICATION_PANEL），
 * 盖在 zOrderOnTop 的视频 Surface 之上，但不吞噬视频区域的触摸。
 *
 * 贴边半隐藏：收起时窗口左移 4dp，只露出 8dp 宽的把手；点击把手展开面板，
 * 再次点击（或点面板头部）收回。面板提供返回（ESC）、键盘、横竖屏、应用
 * 切换四个动作，经协议 type13 发往 Windows 主机；应用切换页由 type14 回包
 * 驱动。所有视图纯代码构建，配色与接收端暗色风格一致。
 */
final class SidebarController {

    interface Callbacks {
        void onEscape();

        void onTouchKeyboard();

        void onRotateOrientation();

        void onRequestWindowList();

        void onMoveWindow(int token, int index, int targetArea);
    }

    static final class WindowEntry {
        final long handle;
        final int area;
        final String title;

        WindowEntry(long handle, int area, String title) {
            this.handle = handle;
            this.area = area;
            this.title = title;
        }
    }

    private static final int COLOR_PANEL = 0xF215171C;
    private static final int COLOR_PANEL_STROKE = 0x2EFFFFFF;
    private static final int COLOR_BUTTON_FILL = 0x17FFFFFF;
    private static final int COLOR_TEXT = 0xF2FFFFFF;
    private static final int COLOR_TEXT_DIM = 0x99FFFFFF;
    private static final int COLOR_ACCENT = 0xFF7AA2FF;

    private final Activity activity;
    private final Callbacks callbacks;
    private final WindowManager windowManager;
    private final Handler handler = new Handler(Looper.getMainLooper());

    private WindowManager.LayoutParams windowParams;
    private LinearLayout rootLayout;
    private LinearLayout panel;
    private FrameLayout panelPages;
    private LinearLayout actionPage;
    private LinearLayout listPage;
    private LinearLayout listRows;
    private TextView listStatus;
    private View handle;
    private boolean expanded;
    private boolean attached;

    private final ArrayList<WindowEntry> windows = new ArrayList<>();
    private int listToken;

    SidebarController(Activity activity, Callbacks callbacks) {
        this.activity = activity;
        this.callbacks = callbacks;
        this.windowManager = (WindowManager) activity.getSystemService(Context.WINDOW_SERVICE);
    }

    // --- 生命周期 ------------------------------------------------------------

    void show() {
        if (attached) {
            rootLayout.setVisibility(View.VISIBLE);
            return;
        }
        if (windowParams == null) {
            buildViews();
        }
        windowParams.token = activity.getWindow().getDecorView().getWindowToken();
        if (windowParams.token == null) {
            // 装饰视图尚未挂到窗口管理器；等下一帧再试。
            handler.postDelayed(this::show, 100L);
            return;
        }
        setExpanded(false, false);
        windowManager.addView(rootLayout, windowParams);
        attached = true;
    }

    void hide() {
        if (!attached) {
            return;
        }
        setExpanded(false, false);
        rootLayout.setVisibility(View.GONE);
        // 子窗口随 Activity 窗口隐藏；这里仅整窗隐藏，避免反复 add/remove。
    }

    void detach() {
        if (attached) {
            windowManager.removeView(rootLayout);
            attached = false;
        }
        handler.removeCallbacksAndMessages(null);
    }

    // --- 视图构建 ------------------------------------------------------------

    private int dp(float value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private GradientDrawable rounded(int color, float radiusDp, int strokeColor, float strokeDp) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color);
        drawable.setCornerRadius(dp(radiusDp));
        if (strokeColor != 0) {
            drawable.setStroke(Math.max(1, dp(strokeDp)), strokeColor);
        }
        return drawable;
    }

    private RippleDrawable ripple(GradientDrawable content) {
        return new RippleDrawable(
                android.content.res.ColorStateList.valueOf(0x40FFFFFF), content, null);
    }

    private TextView text(String value, float sizeSp, int color, boolean bold) {
        TextView view = new TextView(activity);
        view.setText(value);
        view.setTextColor(color);
        view.setTextSize(TypedValue.COMPLEX_UNIT_SP, sizeSp);
        view.setGravity(Gravity.CENTER_VERTICAL);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT_BOLD);
        }
        view.setIncludeFontPadding(false);
        return view;
    }

    private void buildViews() {
        windowParams = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        windowParams.gravity = Gravity.START | Gravity.CENTER_VERTICAL;
        windowParams.format = PixelFormat.TRANSLUCENT;

        rootLayout = new LinearLayout(activity);
        rootLayout.setOrientation(LinearLayout.HORIZONTAL);
        rootLayout.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);

        panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable panelBackground = rounded(COLOR_PANEL, 24f, COLOR_PANEL_STROKE, 1f);
        panel.setBackground(panelBackground);
        panel.setOutlineProvider(new android.view.ViewOutlineProvider() {
            @Override
            public void getOutline(View view, android.graphics.Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), dp(24f));
            }
        });
        panel.setClipToOutline(true);
        panel.setElevation(dp(18f));
        panel.setPadding(dp(12f), dp(10f), dp(12f), dp(12f));
        panel.setVisibility(View.GONE);
        panel.setAlpha(0f);

        LinearLayout panelShell = new LinearLayout(activity);
        panelShell.setOrientation(LinearLayout.VERTICAL);
        panelShell.addView(panel, new LinearLayout.LayoutParams(
                dp(224f), WindowManager.LayoutParams.WRAP_CONTENT));

        panelPages = new FrameLayout(activity);
        panel.addView(panelPages, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        buildActionPage();
        buildListPage();

        handle = buildHandle();
        handle.setOnClickListener(view -> {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            setExpanded(!expanded, true);
        });

        rootLayout.addView(panelShell, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        rootLayout.addView(handle, new LinearLayout.LayoutParams(dp(12f), dp(160f)));
    }

    private View buildHandle() {
        FrameLayout grip = new FrameLayout(activity);
        // 把手：竖向胶囊，带细描边；贴边时只露出约 8dp。
        GradientDrawable pill = rounded(0xCC252932, 7f, 0x4DFFFFFF, 1f);
        grip.setBackground(pill);
        grip.setOutlineProvider(new android.view.ViewOutlineProvider() {
            @Override
            public void getOutline(View view, android.graphics.Outline outline) {
                outline.setRoundRect(0, 0, view.getWidth(), view.getHeight(), dp(7f));
            }
        });
        grip.setClipToOutline(true);
        grip.setElevation(dp(10f));

        LinearLayout dots = new LinearLayout(activity);
        dots.setOrientation(LinearLayout.VERTICAL);
        dots.setGravity(Gravity.CENTER);
        for (int index = 0; index < 3; ++index) {
            View dot = new View(activity);
            GradientDrawable dotDrawable = new GradientDrawable();
            dotDrawable.setColor(0x8CFFFFFF);
            dotDrawable.setShape(GradientDrawable.OVAL);
            dot.setBackground(dotDrawable);
            LinearLayout.LayoutParams dotParams = new LinearLayout.LayoutParams(dp(2.5f), dp(2.5f));
            dotParams.topMargin = index == 0 ? 0 : dp(5f);
            dots.addView(dot, dotParams);
        }
        grip.addView(dots, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.WRAP_CONTENT,
                FrameLayout.LayoutParams.WRAP_CONTENT,
                Gravity.CENTER));
        return grip;
    }

    private void buildActionPage() {
        actionPage = new LinearLayout(activity);
        actionPage.setOrientation(LinearLayout.VERTICAL);

        LinearLayout header = new LinearLayout(activity);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        TextView title = text("WireView", 14f, COLOR_TEXT, true);
        TextView hint = text("轻触把手收起", 11f, COLOR_TEXT_DIM, false);
        header.addView(title, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        header.addView(hint, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT,
                LinearLayout.LayoutParams.WRAP_CONTENT));
        actionPage.addView(header, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        ((LinearLayout.LayoutParams) header.getLayoutParams()).bottomMargin = dp(8f);

        actionPage.addView(actionRow("↩", "返回（ESC）", "给电脑发送 Esc 键",
                () -> callbacks.onEscape()));
        actionPage.addView(actionRow("⌨", "键盘", "在副屏上唤出输入法",
                () -> callbacks.onTouchKeyboard()));
        actionPage.addView(actionRow("⟳", "横竖屏", "切换副屏方向（短暂重连）",
                () -> callbacks.onRotateOrientation()));
        actionPage.addView(actionRow("▦", "应用切换", "把窗口移到另一块屏幕",
                () -> {
                    callbacks.onRequestWindowList();
                    showListPage();
                }));

        panelPages.addView(actionPage, new FrameLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
    }

    private View actionRow(String glyph, String label, String description, Runnable action) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackground(ripple(rounded(COLOR_BUTTON_FILL, 14f, 0, 0)));
        row.setClickable(true);
        row.setPadding(dp(12f), 0, dp(12f), 0);
        row.setMinimumHeight(dp(52f));

        TextView icon = text(glyph, 18f, COLOR_ACCENT, false);
        icon.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams iconParams = new LinearLayout.LayoutParams(dp(26f), dp(26f));

        LinearLayout textColumn = new LinearLayout(activity);
        textColumn.setOrientation(LinearLayout.VERTICAL);
        TextView name = text(label, 15f, COLOR_TEXT, false);
        TextView note = text(description, 10.5f, COLOR_TEXT_DIM, false);
        textColumn.addView(name);
        textColumn.addView(note);
        ((LinearLayout.LayoutParams) note.getLayoutParams()).topMargin = dp(2f);

        row.addView(icon, iconParams);
        row.addView(textColumn, new LinearLayout.LayoutParams(
                0, WindowManager.LayoutParams.WRAP_CONTENT, 1f));
        ((LinearLayout.LayoutParams) textColumn.getLayoutParams()).leftMargin = dp(10f);

        row.setOnClickListener(view -> {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            action.run();
        });
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT);
        rowParams.topMargin = dp(6f);
        row.setLayoutParams(rowParams);
        return row;
    }

    private void buildListPage() {
        listPage = new LinearLayout(activity);
        listPage.setOrientation(LinearLayout.VERTICAL);
        listPage.setVisibility(View.GONE);

        LinearLayout header = new LinearLayout(activity);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);

        TextView back = text("‹", 20f, COLOR_TEXT, false);
        back.setGravity(Gravity.CENTER);
        back.setPadding(dp(6f), 0, dp(6f), 0);
        back.setOnClickListener(view -> {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            showActionPage();
        });

        TextView title = text("应用切换", 14f, COLOR_TEXT, true);
        TextView refresh = text("⟳", 16f, COLOR_TEXT_DIM, false);
        refresh.setPadding(dp(8f), 0, dp(4f), 0);
        refresh.setOnClickListener(view -> {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            callbacks.onRequestWindowList();
        });

        header.addView(back, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        header.addView(title, new LinearLayout.LayoutParams(0,
                WindowManager.LayoutParams.WRAP_CONTENT, 1f));
        header.addView(refresh, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        listPage.addView(header, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        ((LinearLayout.LayoutParams) header.getLayoutParams()).bottomMargin = dp(6f);

        listStatus = text("正在获取窗口列表…", 11f, COLOR_TEXT_DIM, false);
        listPage.addView(listStatus, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));

        listRows = new LinearLayout(activity);
        listRows.setOrientation(LinearLayout.VERTICAL);
        ScrollView scroller = new ScrollView(activity);
        scroller.setVerticalScrollBarEnabled(false);
        scroller.addView(listRows, new ViewGroup.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
        LinearLayout.LayoutParams scrollerParams = new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT);
        scrollerParams.topMargin = dp(4f);
        listPage.addView(scroller, scrollerParams);

        panelPages.addView(listPage, new FrameLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
    }

    // --- 页面与展开动画 --------------------------------------------------------

    private void showActionPage() {
        listPage.setVisibility(View.GONE);
        actionPage.setVisibility(View.VISIBLE);
    }

    private void showListPage() {
        actionPage.setVisibility(View.GONE);
        listPage.setVisibility(View.VISIBLE);
        listRows.removeAllViews();
        listStatus.setText("正在获取窗口列表…");
    }

    private void setExpanded(boolean target, boolean animate) {
        expanded = target;
        if (windowParams == null || rootLayout == null) {
            return;
        }
        windowParams.x = target ? 0 : -dp(4f);
        if (attached) {
            windowManager.updateViewLayout(rootLayout, windowParams);
        }
        if (target) {
            panel.setVisibility(View.VISIBLE);
            if (animate) {
                panel.animate().translationX(0f).alpha(1f).setDuration(240L)
                        .setInterpolator(new DecelerateInterpolator(1.5f))
                        .start();
            } else {
                panel.setTranslationX(0f);
                panel.setAlpha(1f);
            }
        } else {
            Runnable hide = () -> {
                if (!expanded) {
                    panel.setVisibility(View.GONE);
                }
            };
            if (animate) {
                panel.animate().translationX(-dp(28f)).alpha(0f).setDuration(200L)
                        .setInterpolator(new AccelerateInterpolator(1.2f))
                        .withEndAction(hide)
                        .start();
            } else {
                panel.setTranslationX(-dp(28f));
                panel.setAlpha(0f);
                hide.run();
            }
        }
    }

    // --- 窗口列表 ------------------------------------------------------------

    void handleWindowList(byte[] payload) {
        handler.post(() -> applyWindowList(payload));
    }

    private void applyWindowList(byte[] payload) {
        windows.clear();
        if (payload == null || payload.length < 6) {
            listStatus.setText("窗口列表无效");
            return;
        }
        ByteBuffer input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        listToken = input.getInt();
        int count = input.getShort() & 0xffff;
        try {
            for (int index = 0; index < count; ++index) {
                long handle = input.getLong();
                int area = input.get() & 0xff;
                int titleBytes = input.getShort() & 0xffff;
                byte[] titleBytesBuffer = new byte[titleBytes];
                input.get(titleBytesBuffer);
                windows.add(new WindowEntry(handle, area, new String(titleBytesBuffer,
                        java.nio.charset.StandardCharsets.UTF_8)));
            }
        } catch (java.nio.BufferUnderflowException error) {
            listStatus.setText("窗口列表损坏");
            return;
        }
        if (windows.isEmpty()) {
            listStatus.setText("没有可切换的窗口");
            return;
        }
        listStatus.setText("点按窗口即移到另一块屏幕");
        listRows.removeAllViews();
        for (int index = 0; index < windows.size(); ++index) {
            WindowEntry entry = windows.get(index);
            listRows.addView(windowRow(entry, index));
        }
    }

    private View windowRow(WindowEntry entry, int index) {
        LinearLayout row = new LinearLayout(activity);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setBackground(ripple(rounded(COLOR_BUTTON_FILL, 12f, 0, 0)));
        row.setClickable(true);
        row.setPadding(dp(10f), dp(8f), dp(10f), dp(8f));

        TextView title = text(entry.title, 13f, COLOR_TEXT, false);
        title.setMaxLines(1);
        title.setEllipsize(android.text.TextUtils.TruncateAt.END);
        row.addView(title, new LinearLayout.LayoutParams(0,
                WindowManager.LayoutParams.WRAP_CONTENT, 1f));

        TextView badge = text(areaLabel(entry.area), 10f, 0xFFFFFFFF, false);
        badge.setGravity(Gravity.CENTER);
        badge.setBackground(rounded(areaColor(entry.area), 9f, 0, 0));
        badge.setPadding(dp(7f), dp(2.5f), dp(7f), dp(2.5f));
        row.addView(badge, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT));

        // 点按即把窗口移到另一块屏：副屏↔主屏互切，其他屏一律移到主屏。
        final int targetArea = entry.area == 1 ? 0 : 1;
        row.setOnClickListener(view -> {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            callbacks.onMoveWindow(listToken, index, targetArea);
            setExpanded(false, true);
        });
        LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.WRAP_CONTENT);
        rowParams.topMargin = dp(4f);
        row.setLayoutParams(rowParams);
        return row;
    }

    private static String areaLabel(int area) {
        switch (area) {
            case 0:
                return "主屏";
            case 1:
                return "副屏";
            default:
                return "其他";
        }
    }

    private static int areaColor(int area) {
        switch (area) {
            case 0:
                return 0xCC3D6BCC;
            case 1:
                return 0xCC3E8A54;
            default:
                return 0xCC6B7280;
        }
    }
}
