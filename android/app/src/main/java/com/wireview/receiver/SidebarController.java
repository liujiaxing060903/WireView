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
import android.os.SystemClock;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.HapticFeedbackConstants;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.animation.AccelerateInterpolator;
import android.view.animation.DecelerateInterpolator;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputConnectionWrapper;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;

/**
 * 平板贴边侧边栏：一个挂在 Activity 窗口上的子窗口（TYPE_APPLICATION_PANEL），
 * 盖在 zOrderOnTop 的视频 Surface 之上，但不吞噬视频区域的触摸。
 *
 * 贴边半隐藏：收起时窗口左移 4dp，只露出 8dp 宽的把手；点击把手展开面板，
 * 再次点击（或点面板头部）收回。面板提供返回（ESC）、键盘、横竖屏、应用
 * 切换四个动作，经协议 type13 发往 Windows 主机；应用切换页由 type14 回包
 * 驱动；输入法由 type15（PC 端 UIA 焦点探测）自动呼出，也可用键盘按钮手动
 * 呼出——输入条拉起的是平板自己的系统输入法（如微信输入法），提交的文本
 * 经 type13 ime_text/ime_key 直达电脑。所有视图纯代码构建。
 */
final class SidebarController {

    interface Callbacks {
        void onEscape();

        void onImeText(byte[] payload);

        void onImeKey(byte[] payload);

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

    private WindowManager.LayoutParams imeParams;
    private LinearLayout imeBar;
    private EditText imeInput;
    private View keyboardRow;
    private boolean imeShowing;
    // show() 的延迟重试只对"当前想要可见"有效；hide() 置否取消重试。
    private boolean wantVisible;
    // hideIme 会触发 IME 的 finishComposingText，短时间内不再发送组词串。
    private long imeSuppressUntilMillis;

    // 把手拖动：垂直方向自由挪位，横向拖过屏幕中线即换边贴靠。
    private boolean edgeOnRight;
    private boolean draggingHandle;
    private float dragStartRawX;
    private float dragStartRawY;
    private int dragStartWindowY;

    private final ArrayList<WindowEntry> windows = new ArrayList<>();
    private int listToken;

    SidebarController(Activity activity, Callbacks callbacks) {
        this.activity = activity;
        this.callbacks = callbacks;
        this.windowManager = (WindowManager) activity.getSystemService(Context.WINDOW_SERVICE);
    }

    // --- 生命周期 ------------------------------------------------------------

    void show() {
        wantVisible = true;
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
            handler.postDelayed(() -> {
                if (wantVisible) {
                    show();
                }
            }, 100L);
            return;
        }
        setExpanded(false, false);
        windowManager.addView(rootLayout, windowParams);
        attached = true;
    }

    void hide() {
        wantVisible = false;
        if (!attached) {
            return;
        }
        setExpanded(false, false);
        hideIme();
        rootLayout.setVisibility(View.GONE);
        // 子窗口随 Activity 窗口隐藏；这里仅整窗隐藏，避免反复 add/remove。
    }

    void detach() {
        if (attached) {
            windowManager.removeView(rootLayout);
            attached = false;
        }
        imeShowing = false;
        if (imeBar != null && imeBar.getParent() != null) {
            windowManager.removeView(imeBar);
        }
        if (keyboardRow != null) {
            keyboardRow.setAlpha(1f);
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
        handle.setOnTouchListener(this::onHandleTouch);

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
        keyboardRow = actionRow("⌨", "键盘", "呼出平板输入法打字到电脑",
                this::toggleIme);
        actionPage.addView(keyboardRow);
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

    // --- 把手拖动 -------------------------------------------------------------

    private boolean onHandleTouch(View view, MotionEvent event) {
        final int touchSlop = ViewConfiguration.get(activity).getScaledTouchSlop();
        switch (event.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                dragStartRawX = event.getRawX();
                dragStartRawY = event.getRawY();
                dragStartWindowY = windowParams.y;
                draggingHandle = false;
                return true;
            case MotionEvent.ACTION_MOVE: {
                final float dx = event.getRawX() - dragStartRawX;
                final float dy = event.getRawY() - dragStartRawY;
                if (!draggingHandle
                        && (Math.abs(dy) > touchSlop || Math.abs(dx) > touchSlop)) {
                    draggingHandle = true;
                    if (expanded) {
                        setExpanded(false, true);
                    }
                }
                if (!draggingHandle) {
                    return true;
                }
                final int screenHalf =
                        activity.getResources().getDisplayMetrics().heightPixels / 2;
                final int maxOffset = screenHalf - dp(180f);
                windowParams.y = Math.max(-maxOffset, Math.min(maxOffset,
                        dragStartWindowY + Math.round(dy)));
                final boolean wantRight = event.getRawX()
                        > activity.getResources().getDisplayMetrics().widthPixels / 2f;
                if (wantRight != edgeOnRight) {
                    dockEdge(wantRight);
                } else {
                    windowManager.updateViewLayout(rootLayout, windowParams);
                }
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (!draggingHandle) {
                    view.performClick();
                }
                draggingHandle = false;
                return true;
            default:
                return false;
        }
    }

    private void dockEdge(boolean right) {
        edgeOnRight = right;
        windowParams.gravity = (right ? Gravity.END : Gravity.START) | Gravity.CENTER_VERTICAL;
        windowParams.x = expanded ? 0 : -dp(4f);
        windowManager.updateViewLayout(rootLayout, windowParams);
    }

    // --- 输入法输入条 ----------------------------------------------------------
    // 独立的聚焦子窗口：拉起平板系统输入法（微信输入法等），提交的文本经
    // type13 ime_text 转发，退格/回车等经 ime_key。PC 端 UIA 焦点探测会经
    // type15 自动呼出/收起，键盘按钮则是手动开关。

    private void buildImeWindow() {
        imeParams = new WindowManager.LayoutParams(
                dp(224f),
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_APPLICATION_PANEL,
                WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        imeParams.gravity = Gravity.BOTTOM | Gravity.START;
        imeParams.x = dp(8f);
        imeParams.y = dp(360f);

        imeBar = new LinearLayout(activity);
        imeBar.setOrientation(LinearLayout.HORIZONTAL);
        imeBar.setGravity(Gravity.CENTER_VERTICAL);
        imeBar.setBackground(rounded(COLOR_PANEL, 16f, COLOR_PANEL_STROKE, 1f));
        imeBar.setElevation(dp(14f));
        imeBar.setPadding(dp(8f), dp(6f), dp(8f), dp(6f));

        imeInput = new EditText(activity) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                InputConnection base = super.onCreateInputConnection(outAttrs);
                if (base == null) {
                    return null;
                }
                outAttrs.imeOptions |=
                        EditorInfo.IME_FLAG_NO_EXTRACT_UI | EditorInfo.IME_FLAG_NO_FULLSCREEN;
                // 微信等中文输入法不走 commitText，而是 setComposingText
                // （组词，本地显示拼音）+ finishComposingText（上屏）——必须
                // 追踪组词状态，上屏时把组词串发往电脑；直接 commitText 的
                // 输入法走另一条分支。穿透模式：上屏内容不留在本地框里。
                final String[] composing = {""};
                return new InputConnectionWrapper(base, true) {
                    @Override
                    public boolean setComposingText(CharSequence text,
                            int newCursorPosition) {
                        composing[0] = text == null ? "" : text.toString();
                        return super.setComposingText(text, newCursorPosition);
                    }

                    @Override
                    public boolean finishComposingText() {
                        if (!composing[0].isEmpty()) {
                            sendImeText(composing[0]);
                            composing[0] = "";
                            // 不能在 InputConnection 回调内直接 setText：
                            // 会触发 restartInput 重绑 wrapper，组词可能被
                            // 拆到两个 wrapper 上。延迟到下一轮主循环。
                            handler.post(() -> imeInput.setText(""));
                        }
                        return super.finishComposingText();
                    }

                    @Override
                    public boolean commitText(CharSequence text, int newCursorPosition) {
                        if (text != null && text.length() == 1 && text.charAt(0) == '\n') {
                            sendImeKey((byte) 13);  // VK_RETURN
                            composing[0] = "";
                            handler.post(() -> imeInput.setText(""));
                        } else if (text != null && text.length() > 0) {
                            sendImeText(text.toString());
                            composing[0] = "";
                            handler.post(() -> imeInput.setText(""));
                        }
                        return true;
                    }

                    @Override
                    public boolean deleteSurroundingText(int beforeLength, int afterLength) {
                        if (!composing[0].isEmpty()) {
                            // 组词中的删改只作用于本地拼音，电脑还没收到字。
                            return super.deleteSurroundingText(beforeLength, afterLength);
                        }
                        for (int index = 0; index < beforeLength; ++index) {
                            sendImeKey((byte) 8);   // VK_BACK
                        }
                        for (int index = 0; index < afterLength; ++index) {
                            sendImeKey((byte) 46);  // VK_DELETE
                        }
                        return super.deleteSurroundingText(beforeLength, afterLength);
                    }

                    @Override
                    public boolean sendKeyEvent(KeyEvent event) {
                        // 部分输入法用按键事件而非 deleteSurroundingText 删字，
                        // 不拦截会导致平板删了字、电脑没删。
                        if (event != null && event.getAction() == KeyEvent.ACTION_DOWN) {
                            Byte vk = imeKeyForAndroidKeycode(event.getKeyCode());
                            if (vk != null && composing[0].isEmpty()) {
                                sendImeKey(vk);
                            }
                        }
                        return super.sendKeyEvent(event);
                    }
                };
            }

            @Override
            public boolean onKeyDown(int keyCode, KeyEvent event) {
                if (keyCode == KeyEvent.KEYCODE_BACK) {
                    hideIme();
                    return true;
                }
                return super.onKeyDown(keyCode, event);
            }
        };
        imeInput.setTextColor(COLOR_TEXT);
        imeInput.setHintTextColor(COLOR_TEXT_DIM);
        imeInput.setHint("拼音在这里组词，选字后直达电脑");
        imeInput.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        imeInput.setBackground(rounded(COLOR_BUTTON_FILL, 12f, 0, 0));
        imeInput.setPadding(dp(10f), dp(8f), dp(10f), dp(8f));
        imeInput.setMaxLines(1);
        imeInput.setInputType(InputType.TYPE_CLASS_TEXT);
        imeInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        imeInput.setOnEditorActionListener((view, actionId, event) -> {
            sendImeKey((byte) 13);  // 软键盘回车
            return true;
        });
        LinearLayout.LayoutParams inputParams = new LinearLayout.LayoutParams(
                0, WindowManager.LayoutParams.WRAP_CONTENT, 1f);
        imeBar.addView(imeInput, inputParams);

        TextView closeButton = text("✕", 14f, COLOR_TEXT_DIM, false);
        closeButton.setPadding(dp(8f), 0, dp(4f), 0);
        closeButton.setOnClickListener(view -> hideIme());
        imeBar.addView(closeButton, new LinearLayout.LayoutParams(
                WindowManager.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.WRAP_CONTENT));
    }

    private void sendImeText(String value) {
        if (SystemClock.uptimeMillis() < imeSuppressUntilMillis) {
            return;  // 收起输入条引发的 finishComposingText，不算正文。
        }
        byte[] utf8 = value.getBytes(StandardCharsets.UTF_8);
        if (utf8.length == 0 || utf8.length > 0xffff) {
            return;
        }
        ByteBuffer payload = ByteBuffer.allocate(3 + utf8.length).order(ByteOrder.LITTLE_ENDIAN);
        payload.put((byte) 6);
        payload.putShort((short) utf8.length);
        payload.put(utf8);
        PersistentDiagnosticLog.record("Sidebar", "IME text -> PC: " + value);
        callbacks.onImeText(payload.array());
    }

    private void sendImeKey(byte virtualKey) {
        callbacks.onImeKey(new byte[]{7, virtualKey});
    }

    private static Byte imeKeyForAndroidKeycode(int keycode) {
        switch (keycode) {
            case KeyEvent.KEYCODE_DEL:
                return (byte) 8;      // VK_BACK
            case KeyEvent.KEYCODE_FORWARD_DEL:
                return (byte) 46;     // VK_DELETE
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER:
                return (byte) 13;     // VK_RETURN
            case KeyEvent.KEYCODE_TAB:
                return (byte) 9;      // VK_TAB
            case KeyEvent.KEYCODE_DPAD_LEFT:
                return (byte) 37;     // VK_LEFT
            case KeyEvent.KEYCODE_DPAD_UP:
                return (byte) 38;     // VK_UP
            case KeyEvent.KEYCODE_DPAD_RIGHT:
                return (byte) 39;     // VK_RIGHT
            case KeyEvent.KEYCODE_DPAD_DOWN:
                return (byte) 40;     // VK_DOWN
            default:
                return null;
        }
    }

    void handleImeControl(byte[] payload) {
        handler.post(() -> {
            if (payload != null && payload.length >= 1 && payload[0] == 1) {
                showIme();
            } else {
                hideIme();
            }
        });
    }

    private void toggleIme() {
        if (imeShowing) {
            hideIme();
        } else {
            showIme();
        }
    }

    private void showIme() {
        if (!attached || rootLayout.getVisibility() != View.VISIBLE) {
            return;
        }
        if (imeParams == null) {
            buildImeWindow();
        }
        if (imeBar.getParent() == null) {
            imeParams.token = activity.getWindow().getDecorView().getWindowToken();
            if (imeParams.token == null) {
                return;
            }
            try {
                windowManager.addView(imeBar, imeParams);
            } catch (WindowManager.BadTokenException error) {
                PersistentDiagnosticLog.record("Sidebar", "IME bar attach failed", error);
                return;
            }
        }
        imeBar.setVisibility(View.VISIBLE);
        imeInput.requestFocus();
        // 子窗口刚 addView 时 WMS 焦点切换未完成，showSoftInput 常被静默
        // 忽略；延迟重试一次显著提高呼出成功率。
        InputMethodManager manager = (InputMethodManager)
                activity.getSystemService(Context.INPUT_METHOD_SERVICE);
        if (manager != null) {
            manager.showSoftInput(imeInput, InputMethodManager.SHOW_IMPLICIT);
            handler.postDelayed(() -> {
                if (imeShowing) {
                    manager.showSoftInput(imeInput, InputMethodManager.SHOW_IMPLICIT);
                }
            }, 120L);
        }
        if (!imeShowing) {
            imeShowing = true;
            keyboardRow.setAlpha(1f);
        }
    }

    private void hideIme() {
        boolean wasShowing = imeShowing;
        imeShowing = false;
        if (imeBar != null && imeBar.getParent() != null) {
            // 收起会让 IME 对组词做 finishComposingText，半截拼音不该上屏。
            imeSuppressUntilMillis = SystemClock.uptimeMillis() + 500L;
            InputMethodManager manager = (InputMethodManager)
                    activity.getSystemService(Context.INPUT_METHOD_SERVICE);
            if (manager != null && imeInput.getWindowToken() != null) {
                manager.hideSoftInputFromWindow(imeInput.getWindowToken(), 0);
            }
            imeInput.clearFocus();
            imeInput.setText("");
            windowManager.removeView(imeBar);
        }
        if (wasShowing && keyboardRow != null) {
            keyboardRow.setAlpha(0.75f);
        }
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
        // host 端会对"窗口已在目标屏"的请求自校正为换边，这里再自动刷新
        // 一次列表，保证徽标反映移动后的真实位置。
        final int targetArea = entry.area == 1 ? 0 : 1;
        row.setOnClickListener(view -> {
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            callbacks.onMoveWindow(listToken, index, targetArea);
            listStatus.setText("正在移动窗口…");
            handler.postDelayed(() -> {
                if (expanded) {
                    callbacks.onRequestWindowList();
                }
            }, 600L);
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
