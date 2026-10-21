package com.wireview.receiver;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity implements ConnectionClient.Listener, VideoDecoder.Listener {
    private static final String TAG = "WireViewUsb";
    private static final String DISPLAY_VERSION = "1.1.20";
    private static final long PEN_MOVE_INTERVAL_MILLIS = 33L;
    private static final String ACTION_USB_PERMISSION =
            "com.wireview.receiver.action.USB_PERMISSION";
    private static final String ACTION_WIREVIEW_STOPPED =
            "com.wireview.receiver.action.WIREVIEW_STOPPED";
    private static final String ACTION_HOST_EXITED =
            "com.wireview.receiver.action.HOST_EXITED";
    private static final String ACTION_VDD_CREATED =
            "com.wireview.receiver.action.VDD_CREATED";
    private static final String ACTION_DEVICE_IDENTIFIED =
            "com.wireview.receiver.action.DEVICE_IDENTIFIED";

    private TextView statusView;
    private TextView deviceInfoView;
    private TextView versionView;
    private LinearLayout connectionPanel;
    private FrameLayout videoContainer;
    private SurfaceView displaySurface;
    private AspectPreviewView previewView;
    private ConnectionClient connection;
    private boolean activityStarted;
    private boolean usbPermissionRequested;
    private boolean usbReceiverRegistered;
    private UsbAccessory activeAccessory;
    private int lastAccessoryCount = -1;
    private boolean ignoreEnumeratedAccessoryUntilAttach;
    private boolean ignoredDetachedAccessoryLogged;
    private final Runnable pollUsbAccessory = new Runnable() {
        @Override
        public void run() {
            if (!activityStarted) {
                return;
            }
            // An attach Intent is an edge-triggered event. Reusing Activity.getIntent()
            // here would continuously resurrect the same accessory after a detach.
            adoptOrRequestAccessory(null);
            inputHandler.postDelayed(this, 500L);
        }
    };
    private final BroadcastReceiver usbPermissionReceiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (ACTION_WIREVIEW_STOPPED.equals(intent.getAction())) {
                PersistentDiagnosticLog.record("MainActivity", "received WireView stopped notice");
                runOnUiThread(MainActivity.this::showExplicitStopNotice);
                return;
            }
            if (ACTION_HOST_EXITED.equals(intent.getAction())) {
                PersistentDiagnosticLog.record("MainActivity", "received Windows host exited notice");
                runOnUiThread(MainActivity.this::showHostExitedNotice);
                return;
            }
            if (ACTION_VDD_CREATED.equals(intent.getAction())) {
                PersistentDiagnosticLog.record("MainActivity", "received VDD created notice");
                int width = intent.getIntExtra("stream_width", 0);
                int height = intent.getIntExtra("stream_height", 0);
                int refresh = intent.getIntExtra("vdd_refresh", 0);
                runOnUiThread(() -> showVddCreatedNotice(width, height, refresh));
                return;
            }
            if (ACTION_DEVICE_IDENTIFIED.equals(intent.getAction())) {
                PersistentDiagnosticLog.record("MainActivity", "received device identified notice");
                runOnUiThread(MainActivity.this::showDeviceIdentifiedNotice);
                return;
            }
            if (UsbManager.ACTION_USB_ACCESSORY_DETACHED.equals(intent.getAction())) {
                UsbAccessory accessory = accessoryExtra(intent);
                Log.i(TAG, "Android USB accessory detached: " + accessory);
                PersistentDiagnosticLog.record(
                        "MainActivity",
                        "USB accessory detached: " + accessory);
                usbPermissionRequested = false;
                activeAccessory = null;
                hasSeenHostConnection = false;
                awaitingHostReconnect = false;
                explicitStopNoticeActive = false;
                idleNoticeActive = false;
                inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                setStreamingPresentation(false);
                statusView.setText("等待电脑端\n请检查 USB 线或确认电脑端 WireView 正在运行。");
                ignoreEnumeratedAccessoryUntilAttach = true;
                ignoredDetachedAccessoryLogged = false;
                setIntent(new Intent(MainActivity.this, MainActivity.class));
                connection.close();
                connection = new ConnectionClient(MainActivity.this);
                if (activityStarted) {
                    connection.start();
                }
                return;
            }
            if (!ACTION_USB_PERMISSION.equals(intent.getAction())) {
                return;
            }
            usbPermissionRequested = false;
            UsbAccessory accessory = accessoryExtra(intent);
            boolean granted = intent.getBooleanExtra(
                    UsbManager.EXTRA_PERMISSION_GRANTED,
                    false);
            Log.i(TAG, "USB permission result: granted=" + granted + " accessory=" + accessory);
            PersistentDiagnosticLog.record(
                    "MainActivity",
                    "USB permission result: granted=" + granted + " accessory=" + accessory);
            if (ignoreEnumeratedAccessoryUntilAttach) {
                PersistentDiagnosticLog.record(
                        "MainActivity",
                        "ignoring permission result received after accessory detach");
                return;
            }
            if (accessory != null
                    && granted) {
                switchToAccessory(accessory);
            } else {
                onStatusChanged("未获得 WireView USB 配件权限，请重新连接 USB 后授权");
            }
        }
    };
    private VideoDecoder videoDecoder;
    private CodecProbe.Result selectedDecoder;
    private int requestedStreamFrameRate = 60;
    // Once the app has reached the host, keep the user-facing state stable while
    // the connection loop transparently recovers a short ADB/Host interruption.
    private boolean hasSeenHostConnection;
    private boolean awaitingHostReconnect;
    private boolean explicitStopNoticeActive;
    // An explicit idle notice comes from a deliberate controller action. Heartbeat
    // updates must not replace it with the less useful generic "connected" label.
    private boolean idleNoticeActive;
    private String lastLoggedConnectionStatus;
    // Keep the configured video geometry so a later immersive-mode/inset layout
    // change can resize the video viewport again instead of leaving stale black bars.
    private int configuredVideoWidth;
    private int configuredVideoHeight;
    private final Handler inputHandler = new Handler(Looper.getMainLooper());
    private final Runnable showWaitingAfterReconnectGrace = () -> {
        if (!awaitingHostReconnect) {
            return;
        }
        if (idleNoticeActive) {
            return;
        }
        PersistentDiagnosticLog.record(
                "Status",
                "reconnect grace expired; showing disconnected computer status");
        hasSeenHostConnection = false;
        setStreamingPresentation(false);
        statusView.setText("等待电脑端\n请检查 USB 线或确认电脑端 WireView 正在运行。");
    };

    private void showExplicitStopNotice() {
        videoDecoder.stopStream();
        setStreamingPresentation(false);
        awaitingHostReconnect = false;
        explicitStopNoticeActive = true;
        idleNoticeActive = true;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        statusView.setText("WireView 已停止；设备仍连接，可再次开始镜像。");
        refreshDeviceInfo();
    }

    private void showHostExitedNotice() {
        videoDecoder.stopStream();
        setStreamingPresentation(false);
        hasSeenHostConnection = false;
        awaitingHostReconnect = false;
        explicitStopNoticeActive = false;
        // 控制中心退出后，保持“等待电脑端”而不是让下一次连接心跳把它覆盖为“已连接”。
        idleNoticeActive = true;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        statusView.setText("等待电脑端\n请检查 USB 线或确认电脑端 WireView 正在运行。");
        refreshDeviceInfo();
    }

    private void showDeviceIdentifiedNotice() {
        awaitingHostReconnect = false;
        explicitStopNoticeActive = false;
        idleNoticeActive = false;
        hasSeenHostConnection = true;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        setStreamingPresentation(false);
        statusView.setText("已连接");
        refreshDeviceInfo();
    }

    private void showVddCreatedNotice(int width, int height, int refresh) {
        setStreamingPresentation(false);
        awaitingHostReconnect = false;
        explicitStopNoticeActive = false;
        idleNoticeActive = true;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        String resolution = width > 0 && height > 0 ? width + " × " + height : "原生自动";
        String refreshText = refresh > 0 ? refresh + " Hz" : "未设置";
        statusView.setText("已创建副屏\n投屏分辨率：" + resolution + "\n副屏刷新率：" + refreshText);
        refreshDeviceInfo();
    }
    private byte[] pendingPenMove;
    private boolean penMoveDispatchScheduled;
    private long lastPenMoveSentAtMillis;
    private final Runnable dispatchPendingPenMove = () -> {
        penMoveDispatchScheduled = false;
        if (pendingPenMove == null || connection == null) {
            return;
        }
        byte[] input = pendingPenMove;
        pendingPenMove = null;
        connection.sendInputBatch(input);
        lastPenMoveSentAtMillis = SystemClock.uptimeMillis();
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        PersistentDiagnosticLog.initialize(getFilesDir());
        PersistentDiagnosticLog.record(
                "MainActivity",
                "onCreate action=" + getIntent().getAction()
                        + " savedState=" + (savedInstanceState != null));
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        videoContainer = new FrameLayout(this);
        videoContainer.addOnLayoutChangeListener((view, left, top, right, bottom,
                oldLeft, oldTop, oldRight, oldBottom) -> {
            if (configuredVideoWidth > 0 && configuredVideoHeight > 0) {
                applyVideoViewport(configuredVideoWidth, configuredVideoHeight);
            }
        });
        root.addView(
                videoContainer,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));

        connectionPanel = new LinearLayout(this);
        connectionPanel.setOrientation(LinearLayout.VERTICAL);
        connectionPanel.setGravity(Gravity.CENTER);
        connectionPanel.setPadding(dp(32), dp(24), dp(32), dp(24));
        GradientDrawable panelBackground = new GradientDrawable();
        panelBackground.setColor(Color.rgb(28, 30, 34));
        panelBackground.setCornerRadius(dp(18));
        connectionPanel.setBackground(panelBackground);
        root.addView(
                connectionPanel,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT,
                        Gravity.CENTER));
        ((FrameLayout.LayoutParams) connectionPanel.getLayoutParams()).setMargins(
                dp(24), 0, dp(24), 0);

        statusView = new TextView(this);
        statusView.setTextColor(Color.WHITE);
        statusView.setTextSize(26.0f);
        statusView.setGravity(Gravity.CENTER);
        statusView.setText("等待电脑端\n请连接 USB 并在电脑打开 WireView 控制中心。");
        connectionPanel.addView(
                statusView,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        deviceInfoView = new TextView(this);
        deviceInfoView.setTextColor(Color.rgb(190, 196, 205));
        deviceInfoView.setTextSize(17.0f);
        deviceInfoView.setGravity(Gravity.CENTER);
        deviceInfoView.setPadding(0, dp(8), 0, 0);
        deviceInfoView.setText(deviceDisplayLabel());
        connectionPanel.addView(
                deviceInfoView,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        versionView = new TextView(this);
        versionView.setTextColor(Color.rgb(124, 130, 140));
        versionView.setTextSize(12.0f);
        versionView.setGravity(Gravity.END);
        versionView.setPadding(0, dp(12), 0, 0);
        versionView.setText("版本 " + DISPLAY_VERSION);
        connectionPanel.addView(
                versionView,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        displaySurface = new SurfaceView(this);
        displaySurface.setBackgroundColor(Color.BLACK);
        // Keep decoded video above the app window. This remains the most stable
        // visible path on the validated Huawei and OPPO devices.
        displaySurface.setZOrderOnTop(true);
        displaySurface.setOnTouchListener((view, event) -> {
            dispatchCapturedInput(event, view);
            return true;
        });
        displaySurface.setOnGenericMotionListener((view, event) -> {
            return dispatchCapturedInput(event, view);
        });
        videoContainer.addView(
                displaySurface,
                new FrameLayout.LayoutParams(1, 1, Gravity.CENTER));

        previewView = new AspectPreviewView(this);
        previewView.setVisibility(View.GONE);
        videoContainer.addView(
                previewView,
                new FrameLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.MATCH_PARENT));

        setContentView(root);
        Log.i(TAG, "Activity created: action=" + getIntent().getAction());
        connection = new ConnectionClient(this);
        registerUsbPermissionReceiver();
        adoptOrRequestAccessory(getIntent());

        String decoderOverride = getIntent().getStringExtra("decoder");
        boolean forceLowLatency = getIntent().getBooleanExtra("force_low_latency", false);
        selectedDecoder = CodecProbe.findBestAvcDecoder(decoderOverride);
        refreshDeviceInfo();
        videoDecoder = new VideoDecoder(selectedDecoder, forceLowLatency, this);
        displaySurface.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                videoDecoder.setSurface(holder.getSurface());
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                // MediaCodec renders directly into the same Surface.
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                videoDecoder.setSurface(null);
            }
        });
    }

    @Override
    protected void onStart() {
        super.onStart();
        PersistentDiagnosticLog.record("MainActivity", "onStart");
        activityStarted = true;
        connection.start();
        inputHandler.removeCallbacks(pollUsbAccessory);
        inputHandler.post(pollUsbAccessory);
    }

    @Override
    protected void onStop() {
        PersistentDiagnosticLog.record("MainActivity", "onStop");
        activityStarted = false;
        inputHandler.removeCallbacks(pollUsbAccessory);
        discardPendingPenMove();
        connection.close();
        super.onStop();
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        Log.i(TAG, "Activity received new intent: action=" + intent.getAction());
        PersistentDiagnosticLog.record(
                "MainActivity",
                "onNewIntent action=" + intent.getAction());
        adoptOrRequestAccessory(intent);
    }

    @Override
    protected void onDestroy() {
        PersistentDiagnosticLog.record("MainActivity", "onDestroy");
        if (usbReceiverRegistered) {
            unregisterReceiver(usbPermissionReceiver);
            usbReceiverRegistered = false;
        }
        videoDecoder.close();
        super.onDestroy();
    }

    private void registerUsbPermissionReceiver() {
        IntentFilter filter = new IntentFilter(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_ACCESSORY_DETACHED);
        filter.addAction(ACTION_WIREVIEW_STOPPED);
        filter.addAction(ACTION_HOST_EXITED);
        filter.addAction(ACTION_VDD_CREATED);
        filter.addAction(ACTION_DEVICE_IDENTIFIED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            // The permission result is filled and delivered by the system USB service.
            registerReceiver(usbPermissionReceiver, filter, Context.RECEIVER_EXPORTED);
        } else {
            registerReceiver(usbPermissionReceiver, filter);
        }
        usbReceiverRegistered = true;
        PersistentDiagnosticLog.record("MainActivity", "USB permission receiver registered");
    }

    private void adoptOrRequestAccessory(Intent intent) {
        UsbAccessory accessory = attachedAccessory(intent);
        boolean fromAttachIntent = accessory != null;
        if (fromAttachIntent) {
            ignoreEnumeratedAccessoryUntilAttach = false;
            ignoredDetachedAccessoryLogged = false;
        }
        UsbManager manager = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (accessory == null && manager != null) {
            UsbAccessory[] accessories = manager.getAccessoryList();
            int accessoryCount = accessories == null ? 0 : accessories.length;
            if (accessoryCount != lastAccessoryCount) {
                lastAccessoryCount = accessoryCount;
                Log.i(TAG, "Accessory enumeration: count=" + accessoryCount);
                PersistentDiagnosticLog.record(
                        "MainActivity",
                        "accessory enumeration count=" + accessoryCount);
            }
            if (accessories != null && accessories.length == 1) {
                accessory = accessories[0];
            }
        }
        if (accessory == null || manager == null) {
            return;
        }
        if (!fromAttachIntent && ignoreEnumeratedAccessoryUntilAttach) {
            if (!ignoredDetachedAccessoryLogged) {
                ignoredDetachedAccessoryLogged = true;
                PersistentDiagnosticLog.record(
                        "MainActivity",
                        "ignoring stale enumerated accessory after detach; waiting for fresh attach");
            }
            return;
        }
        if (accessory.equals(activeAccessory)) {
            return;
        }
        Log.i(
                TAG,
                "Accessory discovered: " + accessory
                        + " permission=" + manager.hasPermission(accessory));
        PersistentDiagnosticLog.record(
                "MainActivity",
                "accessory discovered: " + accessory
                        + " permission=" + manager.hasPermission(accessory));
        if (manager.hasPermission(accessory)) {
            switchToAccessory(accessory);
            return;
        }
        requestAccessoryPermission(manager, accessory);
    }

    private void requestAccessoryPermission(UsbManager manager, UsbAccessory accessory) {
        if (usbPermissionRequested) {
            return;
        }
        Intent permissionIntent = new Intent(ACTION_USB_PERMISSION)
                .setPackage(getPackageName());
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // UsbManager supplies the selected accessory and grant result as fill-in extras.
            flags |= PendingIntent.FLAG_MUTABLE;
        }
        PendingIntent pendingIntent = PendingIntent.getBroadcast(this, 0, permissionIntent, flags);
        usbPermissionRequested = true;
        Log.i(TAG, "Requesting Android USB accessory permission for " + accessory);
        PersistentDiagnosticLog.record(
                "MainActivity",
                "requesting USB accessory permission: " + accessory);
        onStatusChanged("等待 WireView USB 配件授权…");
        manager.requestPermission(accessory, pendingIntent);
    }

    private void switchToAccessory(UsbAccessory accessory) {
        Log.i(TAG, "Switching ConnectionClient to Android USB accessory transport");
        PersistentDiagnosticLog.record(
                "MainActivity",
                "switching ConnectionClient to USB accessory transport: " + accessory
                        + " activityStarted=" + activityStarted);
        activeAccessory = accessory;
        discardPendingPenMove();
        connection.close();
        connection = new ConnectionClient(
                this,
                new UsbAccessoryTransportConnector(getApplicationContext(), accessory));
        if (activityStarted) {
            connection.start();
        }
    }

    private static UsbAccessory attachedAccessory(Intent intent) {
        if (intent == null
                || !UsbManager.ACTION_USB_ACCESSORY_ATTACHED.equals(intent.getAction())) {
            return null;
        }
        return accessoryExtra(intent);
    }

    private static UsbAccessory accessoryExtra(Intent intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY, UsbAccessory.class);
        }
        @SuppressWarnings("deprecation")
        UsbAccessory accessory = intent.getParcelableExtra(UsbManager.EXTRA_ACCESSORY);
        return accessory;
    }

    @Override
    public void onStatusChanged(String status) {
        if (!status.equals(lastLoggedConnectionStatus)) {
            lastLoggedConnectionStatus = status;
            PersistentDiagnosticLog.record("Status", status);
        }
        runOnUiThread(() -> {
            if (status.startsWith("已连接") || status.startsWith("握手成功")) {
                awaitingHostReconnect = false;
                inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                hasSeenHostConnection = true;
                if (idleNoticeActive) {
                    return;
                }
            } else if (status.startsWith("等待电脑端") && hasSeenHostConnection) {
                if (idleNoticeActive) {
                    return;
                }
                videoDecoder.stopStream();
                setStreamingPresentation(false);
                awaitingHostReconnect = true;
                inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                inputHandler.postDelayed(showWaitingAfterReconnectGrace, 3_000L);
                statusView.setText("已连接");
                refreshDeviceInfo();
                return;
            }
            statusView.setText(hasSeenHostConnection
                    ? "已连接"
                    : "等待电脑端\n请连接 USB 并在电脑打开 WireView 控制中心。");
            refreshDeviceInfo();
        });
    }

    @Override
    public void onDecoderStatus(String status) {
        if (status.startsWith("视频流已停止")) {
            runOnUiThread(() -> setStreamingPresentation(false));
            return;
        }
        if (status.startsWith("解码失败")) {
            runOnUiThread(() -> setStreamingPresentation(false));
        }
        onStatusChanged(status);
    }

    @Override
    public void onStreamConfigured(int width, int height, int frameRate) {
        runOnUiThread(() -> {
            // A real stream start is the only normal event that replaces an idle notice.
            idleNoticeActive = false;
            explicitStopNoticeActive = false;
            requestedStreamFrameRate = frameRate;
            applyPreferredDisplayRefreshRate();
            refreshDeviceInfo();
            previewView.setVisibility(View.GONE);
            configuredVideoWidth = width;
            configuredVideoHeight = height;
            applyVideoViewport(width, height);
        });
    }

    @Override
    public void onFirstFrameRendered(long presentationTimeUs, long startupMillis) {
        runOnUiThread(() -> {
            setStreamingPresentation(true);
            refreshDeviceInfo();
            sendDeviceCapabilities();
            inputHandler.postDelayed(this::sendDeviceCapabilities, 1_000L);
        });
        ByteBuffer telemetry = ByteBuffer.allocate(16).order(ByteOrder.LITTLE_ENDIAN);
        telemetry.put((byte) 1); // Telemetry schema version.
        telemetry.put((byte) 1); // Event 1: first decoded frame released to Surface.
        telemetry.putShort((short) 0);
        telemetry.putLong(presentationTimeUs);
        telemetry.putInt((int) Math.max(0L, Math.min(startupMillis, 0xffffffffL)));
        connection.sendTelemetry(telemetry.array());
    }

    @Override
    public void onDecoderHealth(
            long receivedFrames,
            long queuedFrames,
            long droppedFrames,
            int decoderErrors,
            boolean decoderActive,
            boolean firstFrameRendered) {
        int flags = 0;
        if (decoderActive) {
            flags |= 1;
        }
        if (firstFrameRendered) {
            flags |= 1 << 1;
        }
        ByteBuffer telemetry = ByteBuffer.allocate(32).order(ByteOrder.LITTLE_ENDIAN);
        telemetry.put((byte) 1); // Telemetry schema version.
        telemetry.put((byte) 4); // Event 4: decoder input and error health counters.
        telemetry.putShort((short) flags);
        telemetry.putLong(receivedFrames);
        telemetry.putLong(queuedFrames);
        telemetry.putLong(droppedFrames);
        telemetry.putInt(decoderErrors);
        connection.sendTelemetry(telemetry.array());
    }

    @Override
    public void onKeyframeRequired(long droppedFrames, String reason) {
        PersistentDiagnosticLog.record(
                "VideoDecoder",
                "requesting recovery keyframe after dropped=" + droppedFrames + ": " + reason);
        connection.requestKeyframe();
    }

    private void sendDeviceCapabilities() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M || selectedDecoder == null) {
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode activeMode = display.getMode();
        int displayWidth = activeMode.getPhysicalWidth();
        int displayHeight = activeMode.getPhysicalHeight();
        int rotation = display.getRotation();
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            int unrotatedWidth = displayWidth;
            displayWidth = displayHeight;
            displayHeight = unrotatedWidth;
        }
        float maximumRefreshRate = activeMode.getRefreshRate();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == activeMode.getPhysicalWidth()
                    && mode.getPhysicalHeight() == activeMode.getPhysicalHeight()) {
                maximumRefreshRate = Math.max(maximumRefreshRate, mode.getRefreshRate());
            }
        }

        byte[] decoderName = selectedDecoder.name.getBytes(StandardCharsets.UTF_8);
        int flags = 0;
        if (selectedDecoder.hardwareAccelerated) {
            flags |= 1;
        }
        if (selectedDecoder.softwareOnly) {
            flags |= 1 << 1;
        }
        if (selectedDecoder.lowLatency) {
            flags |= 1 << 2;
        }
        if (selectedDecoder.supports1080p60) {
            flags |= 1 << 3;
        }

        ByteBuffer telemetry = ByteBuffer.allocate(16 + decoderName.length)
                .order(ByteOrder.LITTLE_ENDIAN);
        telemetry.put((byte) 1); // Telemetry schema version.
        telemetry.put((byte) 3); // Event 3: decoder and active display capabilities.
        telemetry.put((byte) flags);
        telemetry.put((byte) 0);
        telemetry.putShort(clampUnsignedShort(displayWidth));
        telemetry.putShort(clampUnsignedShort(displayHeight));
        telemetry.putShort(clampUnsignedShort(Math.round(activeMode.getRefreshRate() * 100.0f)));
        telemetry.putShort(clampUnsignedShort(Math.round(maximumRefreshRate * 100.0f)));
        telemetry.putShort(clampUnsignedShort(decoderName.length));
        telemetry.putShort((short) 0);
        telemetry.put(decoderName);
        connection.sendTelemetry(telemetry.array());
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void refreshDeviceInfo() {
        if (deviceInfoView != null) {
            deviceInfoView.setText(deviceDisplayLabel());
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        // This Activity handles orientation itself, so Android does not recreate it.
        // Refresh once now and once after the display mode/rotation settles.
        refreshDeviceInfo();
        inputHandler.postDelayed(this::refreshDeviceInfo, 200L);
    }

    private String deviceDisplayLabel() {
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode activeMode = display.getMode();
        int width = activeMode.getPhysicalWidth();
        int height = activeMode.getPhysicalHeight();
        int rotation = display.getRotation();
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            int unrotatedWidth = width;
            width = height;
            height = unrotatedWidth;
        }
        return width + " × " + height + " · " + Math.round(activeMode.getRefreshRate()) + " Hz";
    }

    @Override
    public byte[] createHelloAcknowledgementPayload() {
        String model = Build.MODEL == null ? "Android" : Build.MODEL;
        String manufacturer = Build.MANUFACTURER == null ? "Android" : Build.MANUFACTURER;
        byte[] modelName = model.getBytes(StandardCharsets.UTF_8);
        byte[] manufacturerName = manufacturer.getBytes(StandardCharsets.UTF_8);
        boolean isTablet = getResources().getConfiguration().smallestScreenWidthDp >= 600;
        Display display = getWindowManager().getDefaultDisplay();
        Display.Mode activeMode = display.getMode();
        int displayWidth = activeMode.getPhysicalWidth();
        int displayHeight = activeMode.getPhysicalHeight();
        int rotation = display.getRotation();
        if (rotation == Surface.ROTATION_90 || rotation == Surface.ROTATION_270) {
            int unrotatedWidth = displayWidth;
            displayWidth = displayHeight;
            displayHeight = unrotatedWidth;
        }
        float maximumRefreshRate = activeMode.getRefreshRate();
        for (Display.Mode mode : display.getSupportedModes()) {
            if (mode.getPhysicalWidth() == activeMode.getPhysicalWidth()
                    && mode.getPhysicalHeight() == activeMode.getPhysicalHeight()) {
                maximumRefreshRate = Math.max(maximumRefreshRate, mode.getRefreshRate());
            }
        }
        ByteBuffer payload = ByteBuffer.allocate(18 + modelName.length + manufacturerName.length)
                .order(ByteOrder.LITTLE_ENDIAN);
        payload.put((byte) 'W').put((byte) 'V').put((byte) 'P').put((byte) 'C');
        payload.put((byte) 2); // Device profile schema version.
        payload.put((byte) (isTablet ? 1 : 0));
        payload.putShort(clampUnsignedShort(displayWidth));
        payload.putShort(clampUnsignedShort(displayHeight));
        payload.putShort(clampUnsignedShort(Math.round(activeMode.getRefreshRate() * 100.0f)));
        payload.putShort(clampUnsignedShort(Math.round(maximumRefreshRate * 100.0f)));
        payload.putShort(clampUnsignedShort(modelName.length));
        payload.putShort(clampUnsignedShort(manufacturerName.length));
        payload.put(modelName);
        payload.put(manufacturerName);
        return payload.array();
    }

    private void applyVideoViewport(int contentWidth, int contentHeight) {
        if (contentWidth <= 0 || contentHeight <= 0 || videoContainer == null ||
                displaySurface == null) {
            return;
        }
        videoContainer.post(() -> {
            int viewportWidth = videoContainer.getWidth();
            int viewportHeight = videoContainer.getHeight();
            if (viewportWidth <= 0 || viewportHeight <= 0) {
                return;
            }
            float scale = Math.min(
                    (float) viewportWidth / contentWidth,
                    (float) viewportHeight / contentHeight);
            int width = Math.max(1, Math.round(contentWidth * scale));
            int height = Math.max(1, Math.round(contentHeight * scale));
            FrameLayout.LayoutParams parameters = new FrameLayout.LayoutParams(
                    width, height, Gravity.CENTER);
            displaySurface.setLayoutParams(parameters);
            // Make the decoded Surface have the same aspect ratio as the content;
            // the surrounding FrameLayout stays black and intentionally absorbs bars.
            displaySurface.getHolder().setFixedSize(contentWidth, contentHeight);
        });
    }

    private void resetVideoViewport() {
        if (displaySurface == null) {
            return;
        }
        configuredVideoWidth = 0;
        configuredVideoHeight = 0;
        displaySurface.setLayoutParams(new FrameLayout.LayoutParams(1, 1, Gravity.CENTER));
    }

    private void showDevicePreview(byte[] payload) {
        if (payload.length != 10 || payload[0] != 1) {
            onStatusChanged("设备预览配置无效");
            return;
        }
        ByteBuffer input = ByteBuffer.wrap(payload).order(ByteOrder.LITTLE_ENDIAN);
        input.position(4);
        int width = Short.toUnsignedInt(input.getShort());
        int height = Short.toUnsignedInt(input.getShort());
        int frameRate = Short.toUnsignedInt(input.getShort());
        runOnUiThread(() -> {
            statusView.setVisibility(View.GONE);
            deviceInfoView.setVisibility(View.GONE);
            versionView.setVisibility(View.GONE);
            resetVideoViewport();
            previewView.showLayout(width, height, frameRate);
            getWindow().getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        });
    }

    private static short clampUnsignedShort(int value) {
        return (short) Math.max(0, Math.min(value, 0xffff));
    }

    private void setStreamingPresentation(boolean streaming) {
        statusView.setVisibility(streaming ? View.GONE : View.VISIBLE);
        deviceInfoView.setVisibility(streaming ? View.GONE : View.VISIBLE);
        versionView.setVisibility(streaming ? View.GONE : View.VISIBLE);
        getWindow().getDecorView().setSystemUiVisibility(streaming
                ? View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                : View.SYSTEM_UI_FLAG_VISIBLE);
        if (streaming && configuredVideoWidth > 0 && configuredVideoHeight > 0) {
            // The immersive flags may change the usable tablet viewport after this
            // callback returns. The layout listener handles that final change; this
            // post covers devices whose inset update is already complete.
            videoContainer.post(() ->
                    applyVideoViewport(configuredVideoWidth, configuredVideoHeight));
        }
        if (!streaming) {
            requestedStreamFrameRate = 60;
            applyPreferredDisplayRefreshRate();
            if (previewView != null) {
                previewView.setVisibility(View.GONE);
            }
            resetVideoViewport();
            // Keep the 1×1 SurfaceView alive while idle/previewing. Some Android
            // compositors do not create a valid decoder target for an invisible
            // video view, which would leave the decoder waiting forever.
        }
    }

    private void applyPreferredDisplayRefreshRate() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.M) {
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        int preferredModeId = 0;
        if (requestedStreamFrameRate >= 120) {
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getRefreshRate() >= 119.0f) {
                    preferredModeId = mode.getModeId();
                    break;
                }
            }
        }
        WindowManager.LayoutParams attributes = getWindow().getAttributes();
        if (attributes.preferredDisplayModeId != preferredModeId) {
            attributes.preferredDisplayModeId = preferredModeId;
            getWindow().setAttributes(attributes);
        }
    }

    @Override
    public void onFrameTiming(
            long sequence,
            long hostTimestampMicros,
            long arrivalMicros,
            long surfaceReleaseMicros) {
        ByteBuffer telemetry = ByteBuffer.allocate(36).order(ByteOrder.LITTLE_ENDIAN);
        telemetry.put((byte) 1); // Telemetry schema version.
        telemetry.put((byte) 2); // Event 2: sampled packet arrival and Surface release.
        telemetry.putShort((short) 0);
        telemetry.putLong(sequence);
        telemetry.putLong(hostTimestampMicros);
        telemetry.putLong(arrivalMicros);
        telemetry.putLong(surfaceReleaseMicros);
        connection.sendTelemetry(telemetry.array());
    }

    private boolean dispatchCapturedInput(MotionEvent event, View viewport) {
        byte[] input = InputBatch.fromMotionEvent(event, viewport);
        if (input == null) {
            return false;
        }
        if (isCoalesciblePenMove(event)) {
            enqueuePenMove(input);
        } else {
            flushPendingPenMove();
            connection.sendInputBatch(input);
        }
        return true;
    }

    private static boolean isCoalesciblePenMove(MotionEvent event) {
        if (event.getActionMasked() != MotionEvent.ACTION_MOVE) {
            return false;
        }
        for (int index = 0; index < event.getPointerCount(); ++index) {
            int tool = event.getToolType(index);
            if (tool == MotionEvent.TOOL_TYPE_STYLUS || tool == MotionEvent.TOOL_TYPE_ERASER) {
                return true;
            }
        }
        return false;
    }

    private void enqueuePenMove(byte[] input) {
        pendingPenMove = input;
        if (penMoveDispatchScheduled) {
            return;
        }
        long now = SystemClock.uptimeMillis();
        long dueAt = lastPenMoveSentAtMillis == 0L
                ? now
                : Math.max(now, lastPenMoveSentAtMillis + PEN_MOVE_INTERVAL_MILLIS);
        penMoveDispatchScheduled = true;
        inputHandler.postAtTime(dispatchPendingPenMove, dueAt);
    }

    private void flushPendingPenMove() {
        inputHandler.removeCallbacks(dispatchPendingPenMove);
        penMoveDispatchScheduled = false;
        if (pendingPenMove == null) {
            return;
        }
        byte[] input = pendingPenMove;
        pendingPenMove = null;
        connection.sendInputBatch(input);
        lastPenMoveSentAtMillis = SystemClock.uptimeMillis();
    }

    private void discardPendingPenMove() {
        inputHandler.removeCallbacks(dispatchPendingPenMove);
        pendingPenMove = null;
        penMoveDispatchScheduled = false;
    }

    @Override
    public void onPacket(Protocol.Packet packet) {
        if (packet.header.type == Protocol.TYPE_HELLO) {
            String hostName = new String(packet.payload, StandardCharsets.UTF_8);
            onStatusChanged("收到 Windows 握手：" + hostName);
        } else if (packet.header.type == Protocol.TYPE_PING) {
            connection.sendPong(packet.header, System.nanoTime() / 1_000L);
        } else if (packet.header.type == Protocol.TYPE_START_STREAM) {
            videoDecoder.startStream(packet.payload);
        } else if (packet.header.type == Protocol.TYPE_VIDEO_CHUNK) {
            videoDecoder.queueFrame(packet);
        } else if (packet.header.type == Protocol.TYPE_STOP_STREAM) {
            runOnUiThread(this::showExplicitStopNotice);
            videoDecoder.stopStream();
        } else if (packet.header.type == Protocol.TYPE_DEVICE_PREVIEW) {
            showDevicePreview(packet.payload);
        }
    }
}
