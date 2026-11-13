package com.wireview.receiver;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.content.res.ColorStateList;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.Point;
import android.graphics.drawable.GradientDrawable;
import android.hardware.display.DisplayManager;
import android.hardware.usb.UsbAccessory;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.net.Uri;
import android.util.Log;
import android.view.MotionEvent;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.Gravity;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowManager;
import android.widget.ImageButton;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.FrameLayout;
import android.widget.PopupWindow;
import android.widget.TextView;
import android.widget.ProgressBar;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

public final class MainActivity extends Activity implements ConnectionClient.Listener, VideoDecoder.Listener {
    private static final String TAG = "WireViewUsb";
    private static final String DISPLAY_VERSION = BuildConfig.VERSION_NAME;
    private static final String ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE";
    private static final int[] ROTATION_ORDER = {
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
            ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE,
            ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT,
            ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE,
    };
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
    private ProgressBar connectionProgress;
    private boolean usbCableConnected = true;
    private ConnectionStatus connectionStatus = new ConnectionStatus();
    private TextView deviceInfoView;
    private TextView versionView;
    private Button updateButton;
    private boolean updateChecking;
    private final Runnable checkUpdates = new Runnable() {
        @Override public void run() {
            checkForUpdates();
            inputHandler.postDelayed(this, 6 * 60 * 60 * 1000L);
        }
    };
    private LinearLayout connectionPanel;
    private FrameLayout videoContainer;
    private SurfaceView displaySurface;
    private AspectPreviewView previewView;
    private PopupWindow rotationControl;
    private boolean streamingPresentation;
    private int rotationControlStartRotation;
    private int rotationControlMargin;
    private int rotationStep = -1;
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
            if (ACTION_USB_STATE.equals(intent.getAction())) {
                readUsbState(intent);
                if (!usbCableConnected) {
                    hasConfiguredStream = false;
                    awaitingHostReconnect = false;
                    idleNoticeActive = false;
                    explicitStopNoticeActive = false;
                    inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                    videoDecoder.stopStream();
                    setStreamingPresentation(false);
                    showWaitingForPc();
                } else if (!hasConfiguredStream && !idleNoticeActive) {
                    showWaitingForPc();
                }
                return;
            }
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
                if (!isWireViewAccessory(accessory)) {
                    return;
                }
                Log.i(TAG, "Android USB accessory detached: " + accessory);
                PersistentDiagnosticLog.record(
                        "MainActivity",
                        "USB accessory detached: " + accessory);
                usbPermissionRequested = false;
                activeAccessory = null;
                hasConfiguredStream = false;
                awaitingHostReconnect = false;
                explicitStopNoticeActive = false;
                idleNoticeActive = false;
                inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                setStreamingPresentation(false);
                setConnectionStatus("USB 已断开\n请重新连接电脑。");
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
            UsbAccessory accessory = accessoryExtra(intent);
            if (accessory != null && !isWireViewAccessory(accessory)) {
                return;
            }
            usbPermissionRequested = false;
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
    private boolean hasConfiguredStream;
    private boolean awaitingHostReconnect;
    private boolean explicitStopNoticeActive;
    // An explicit idle notice comes from a deliberate controller action. Heartbeat
    // updates must not replace it with the less useful generic "connected" label.
    private boolean idleNoticeActive;
    // Data only: the old Activity and its views must not survive rotation.
    private static final class RotationState {
        final ConnectionStatus connectionStatus;
        final String idleNotice;
        final boolean explicitStop;
        final int rotationStep;

        RotationState(ConnectionStatus connectionStatus, String idleNotice,
                boolean explicitStop, int rotationStep) {
            this.connectionStatus = connectionStatus;
            this.idleNotice = idleNotice;
            this.explicitStop = explicitStop;
            this.rotationStep = rotationStep;
        }
    }
    private String lastLoggedConnectionStatus;
    // Keep the configured video geometry so a later immersive-mode/inset layout
    // change can resize the video viewport again instead of leaving stale black bars.
    private int configuredVideoWidth;
    private int configuredVideoHeight;
    private final Handler inputHandler = new Handler(Looper.getMainLooper());
    private boolean displayReportPending;
    private final Runnable showFirstFrameTimeout = () ->
            setConnectionStatus("已连接电脑，但画面尚未显示\n请在电脑端检查投屏状态。");
    private final Runnable reportDisplayChange = () -> {
        displayReportPending = false;
        positionRotationControl();
        refreshDeviceInfo();
        sendDeviceCapabilities();
    };
    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int displayId) {}
        @Override public void onDisplayRemoved(int displayId) {}
        @Override public void onDisplayChanged(int displayId) {
            if (displayId == getWindowManager().getDefaultDisplay().getDisplayId()) {
                scheduleDisplayReport();
            }
        }
    };
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
        hasConfiguredStream = false;
        awaitingHostReconnect = false;
        setStreamingPresentation(false);
        setConnectionStatus("与电脑的连接已断开\n请检查 USB 线或电脑上的 WireView。");
    };

    private void showExplicitStopNotice() {
        videoDecoder.stopStream();
        setStreamingPresentation(false);
        hasConfiguredStream = false;
        awaitingHostReconnect = false;
        explicitStopNoticeActive = true;
        idleNoticeActive = true;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        setConnectionStatus("投屏已停止\n可在电脑端重新开始投屏。");
        refreshDeviceInfo();
    }

    private void showHostExitedNotice() {
        videoDecoder.stopStream();
        setStreamingPresentation(false);
        hasConfiguredStream = false;
        awaitingHostReconnect = false;
        explicitStopNoticeActive = false;
        idleNoticeActive = true;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        setConnectionStatus("电脑控制中心已退出\n请重新打开 WireView 控制中心。");
        refreshDeviceInfo();
    }

    private void showDeviceIdentifiedNotice() {
        connectionStatus.hostContacted();
        awaitingHostReconnect = false;
        explicitStopNoticeActive = false;
        idleNoticeActive = true;
        hasConfiguredStream = false;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        setStreamingPresentation(false);
        setConnectionStatus("电脑已识别这台设备\n请在电脑端选择复制或扩展。");
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
        setConnectionStatus("已创建副屏\n投屏分辨率：" + resolution + "\n副屏刷新率：" + refreshText);
        refreshDeviceInfo();
    }
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Object retained = getLastNonConfigurationInstance();
        RotationState rotationState = retained instanceof RotationState
                ? (RotationState) retained : null;
        if (rotationState != null) {
            connectionStatus = rotationState.connectionStatus;
            rotationStep = rotationState.rotationStep;
            idleNoticeActive = rotationState.idleNotice != null;
            explicitStopNoticeActive = idleNoticeActive && rotationState.explicitStop;
        }
        PersistentDiagnosticLog.initialize(getFilesDir());
        PacketReadTrace.directory = getFilesDir();
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
        if (idleNoticeActive) {
            setConnectionStatus(rotationState.idleNotice);
            PersistentDiagnosticLog.record("MainActivity", "restored idle notice after rotation");
        } else {
            showWaitingForPc();
        }
        connectionPanel.addView(
                statusView,
                new LinearLayout.LayoutParams(
                        ViewGroup.LayoutParams.MATCH_PARENT,
                        ViewGroup.LayoutParams.WRAP_CONTENT));

        connectionProgress = new ProgressBar(this);
        connectionProgress.setIndeterminateTintList(ColorStateList.valueOf(Color.rgb(101, 162, 255)));
        connectionProgress.setVisibility(View.GONE);
        LinearLayout.LayoutParams progressLayout = new LinearLayout.LayoutParams(dp(28), dp(28));
        progressLayout.gravity = Gravity.CENTER_HORIZONTAL;
        progressLayout.topMargin = dp(12);
        connectionPanel.addView(connectionProgress, progressLayout);

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
        updateButton = new Button(this);
        updateButton.setText("下载新版");
        updateButton.setVisibility(View.GONE);
        updateButton.setAllCaps(false);
        updateButton.setTextSize(14);
        updateButton.setOnClickListener(view -> showUpdateDownloads());
        connectionPanel.addView(updateButton);
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
        videoDecoder = new VideoDecoder(selectedDecoder, forceLowLatency, this, getFilesDir(),
                getIntent().getLongExtra("demo_grid_ns", 0L),
                getIntent().getLongExtra("demo_period_ns", 0L),
                getIntent().getLongExtra("demo_lead_ns", 2_500_000L),
                getIntent().getIntExtra("demo_warmup_frames", 0));
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
        inputHandler.post(checkUpdates);
        PersistentDiagnosticLog.record("MainActivity", "onStart");
        activityStarted = true;
        getSystemService(DisplayManager.class).registerDisplayListener(displayListener, inputHandler);
        getWindow().getDecorView().post(this::showRotationControl);
        refreshDeviceInfo();
        connection.start();
        inputHandler.removeCallbacks(pollUsbAccessory);
        inputHandler.post(pollUsbAccessory);
    }

    @Override
    protected void onStop() {
        inputHandler.removeCallbacks(checkUpdates);
        PersistentDiagnosticLog.record("MainActivity", "onStop");
        activityStarted = false;
        getSystemService(DisplayManager.class).unregisterDisplayListener(displayListener);
        inputHandler.removeCallbacks(reportDisplayChange);
        displayReportPending = false;
        inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
        if (rotationControl != null) {
            rotationControl.dismiss();
            rotationControl = null;
        }
        inputHandler.removeCallbacks(pollUsbAccessory);
        connection.close();
        videoDecoder.stopStream();
        hasConfiguredStream = false;
        awaitingHostReconnect = false;
        if (!isChangingConfigurations()) {
            idleNoticeActive = false;
        }
        setStreamingPresentation(false);
        if (!idleNoticeActive) {
            showWaitingForPc();
        }
        super.onStop();
    }

    @Override
    public Object onRetainNonConfigurationInstance() {
        return new RotationState(connectionStatus,
                idleNoticeActive ? statusView.getText().toString() : null,
                explicitStopNoticeActive, rotationStep);
    }

    private void showUpdateDownloads() {
        new AlertDialog.Builder(this).setTitle("下载 WireView")
                .setItems(new String[] {"GitHub 发布页", "网盘下载（密码：df73）"}, (dialog, which) -> {
                    String url = which == 0 ? UpdateChecker.DOWNLOAD_URL : "https://wwbkc.lanzoue.com/b01euxnp0d";
                    try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); }
                    catch (android.content.ActivityNotFoundException error) { versionView.setText("未找到浏览器，请手动打开下载链接"); }
                }).setNegativeButton("取消", null).show();
    }

    private void checkForUpdates() {
        if (updateChecking) return;
        updateChecking = true;
        updateButton.setEnabled(false);
        new Thread(() -> {
            String latest = null;
            try { latest = UpdateChecker.latestVersion(); }
            catch (Exception error) { Log.w(TAG, "Update check failed", error); }
            final String version = latest;
            runOnUiThread(() -> {
                updateChecking = false;
                if (isFinishing() || isDestroyed()) return;
                updateButton.setEnabled(true);
                if (version == null) return;
                boolean available = VersionNumber.isNewer(version, DISPLAY_VERSION);
                updateButton.setText("发现 " + version + " · 下载新版");
                updateButton.setVisibility(available ? View.VISIBLE : View.GONE);
                Log.i(TAG, "Update check complete: current=" + DISPLAY_VERSION + " latest=" + version);
            });
        }, "WireViewUpdate").start();
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
        filter.addAction(ACTION_USB_STATE);
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
        Intent usbState = registerReceiver(null, new IntentFilter(ACTION_USB_STATE));
        if (usbState != null) {
            readUsbState(usbState);
            if (!usbCableConnected) {
                idleNoticeActive = false;
                explicitStopNoticeActive = false;
            }
            if (!idleNoticeActive) {
                showWaitingForPc();
            }
        }
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
            if (accessories != null && accessories.length == 1 && isWireViewAccessory(accessories[0])) {
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
        UsbAccessory accessory = accessoryExtra(intent);
        return isWireViewAccessory(accessory) ? accessory : null;
    }

    private static boolean isWireViewAccessory(UsbAccessory accessory) {
        // Match the Windows AOA identity and the manifest's accessory_filter.xml.
        return accessory != null
                && "WireView".equals(accessory.getManufacturer())
                && "WireView Receiver".equals(accessory.getModel());
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
            if (!activityStarted) { return; }
            if (status.startsWith("等待电脑端")) {
                if (idleNoticeActive || usbPermissionRequested) {
                    return;
                }
                if (hasConfiguredStream) {
                    videoDecoder.stopStream();
                    setStreamingPresentation(false);
                    awaitingHostReconnect = true;
                    inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                    inputHandler.postDelayed(showWaitingAfterReconnectGrace, 3_000L);
                    setConnectionStatus("连接中断\n正在重新连接电脑…");
                } else {
                    showWaitingForPc();
                }
            } else if (!status.startsWith("正在连接电脑")
                    || (!idleNoticeActive && !awaitingHostReconnect)) {
                setConnectionStatus(status.startsWith("正在连接电脑")
                        ? "正在与电脑通信\n正在确认连接状态，请稍候。" : status);
            }
            refreshDeviceInfo();
        });
    }

    @Override
    public void onDecoderStatus(String status) {
        if (status.startsWith("视频流已停止")) {
            runOnUiThread(() -> setStreamingPresentation(false));
            return;
        }
        if (status.startsWith("解码失败") || status.startsWith("解码器启动失败")
                || status.startsWith("视频配置无效")) {
            runOnUiThread(() -> setStreamingPresentation(false));
        }
        if (!status.startsWith("解码器已启动")) {
            onStatusChanged(status);
        }
    }

    @Override
    public void onStreamConfigured(int width, int height, int frameRate) {
        runOnUiThread(() -> {
            // A real stream start is the only normal event that replaces an idle notice.
            idleNoticeActive = false;
            explicitStopNoticeActive = false;
            hasConfiguredStream = true;
            setConnectionStatus("已连接电脑\n正在等待首帧…");
            inputHandler.removeCallbacks(showFirstFrameTimeout);
            inputHandler.postDelayed(showFirstFrameTimeout, 8_000L);
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
    public void onFrameSurfaceTarget(long presentationTimeUs, long wantedNanos) {
        connection.sendTelemetry(SurfaceFrameMarker.encode(presentationTimeUs, wantedNanos));
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
        telemetry.putShort(clampUnsignedShort(Math.round(display.getRefreshRate() * 100.0f)));
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

    private void showRotationControl() {
        if (!activityStarted || streamingPresentation || rotationControl != null) {
            return;
        }
        ImageButton button = new ImageButton(this);
        button.setImageResource(R.drawable.ic_rotate);
        button.setImageTintList(ColorStateList.valueOf(Color.WHITE));
        button.setScaleType(ImageView.ScaleType.CENTER);
        button.setPadding(0, 0, 0, 0);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            button.setForceDarkAllowed(false);
        }
        button.setContentDescription("旋转屏幕");
        GradientDrawable background = new GradientDrawable();
        background.setColor(0xCC242832);
        background.setShape(GradientDrawable.OVAL);
        button.setBackground(background);
        button.setOnClickListener(view -> rotateDisplay());
        // This small window stays above the hardware video SurfaceView.
        rotationControl = new PopupWindow(button, dp(48), dp(48), false);
        rotationControl.setInputMethodMode(PopupWindow.INPUT_METHOD_NOT_NEEDED);
        rotationControl.setAttachedInDecor(false);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            rotationControl.setIsLaidOutInScreen(true);
        }
        rotationControlStartRotation = getWindowManager().getDefaultDisplay().getRotation();
        rotationControlMargin = dp(12) + rotationControlSafeInset();
        positionRotationControl();
    }

    private int rotationControlSafeInset() {
        WindowInsets insets = videoContainer.getRootWindowInsets();
        if (insets == null) {
            return 0;
        }
        int safeInset = Math.max(Math.max(insets.getStableInsetLeft(), insets.getStableInsetRight()),
                Math.max(insets.getStableInsetTop(), insets.getStableInsetBottom()));
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            DisplayCutout cutout = insets.getDisplayCutout();
            if (cutout != null) {
                safeInset = Math.max(safeInset, Math.max(
                        Math.max(cutout.getSafeInsetLeft(), cutout.getSafeInsetRight()),
                        Math.max(cutout.getSafeInsetTop(), cutout.getSafeInsetBottom())));
            }
        }
        return safeInset;
    }

    private void positionRotationControl() {
        if (rotationControl == null) {
            return;
        }
        Display display = getWindowManager().getDefaultDisplay();
        Point size = new Point();
        display.getRealSize(size);
        int turns = (display.getRotation() - rotationControlStartRotation + 4) % 4;
        int margin = rotationControlMargin;
        // Keep the same physical corner while Android rotates its screen coordinates.
        int x = turns == 0 || turns == 3 ? size.x - margin - rotationControl.getWidth() : margin;
        int y = turns >= 2 ? size.y - margin - rotationControl.getHeight() : margin;
        rotationControl.getContentView().setRotation(-90f * turns);
        if (rotationControl.isShowing()) {
            rotationControl.update(x, y, -1, -1);
        } else {
            rotationControl.showAtLocation(videoContainer, Gravity.TOP | Gravity.LEFT, x, y);
        }
    }

    private void rotateDisplay() {
        if (rotationStep < 0) {
            int rotation = getWindowManager().getDefaultDisplay().getRotation();
            boolean landscape = getResources().getConfiguration().orientation
                    == Configuration.ORIENTATION_LANDSCAPE;
            boolean naturalLandscape = landscape == (rotation % 2 == 0);
            rotationStep = rotation + (naturalLandscape ? 1 : 0);
        }
        rotationStep = (rotationStep + 1) % ROTATION_ORDER.length;
        setRequestedOrientation(ROTATION_ORDER[rotationStep]);
    }

    private void scheduleDisplayReport() {
        if (activityStarted && !displayReportPending) {
            displayReportPending = inputHandler.postDelayed(reportDisplayChange, 100L);
        }
    }

    @Override
    public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        positionRotationControl();
        scheduleDisplayReport();
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
        return width + " × " + height + " · " + Math.round(display.getRefreshRate()) + " Hz";
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
        payload.putShort(clampUnsignedShort(Math.round(display.getRefreshRate() * 100.0f)));
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
            connectionPanel.setVisibility(View.GONE);
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

    private void showWaitingForPc() {
        setConnectionStatus(connectionStatus.waitingText());
    }

    private void readUsbState(Intent intent) {
        usbCableConnected = intent.getBooleanExtra("connected", false);
        boolean dataAvailable = intent.getBooleanExtra("adb", false)
                || intent.getBooleanExtra("mtp", false) || intent.getBooleanExtra("ptp", false)
                || intent.getBooleanExtra("accessory", false);
        connectionStatus.updateUsb(usbCableConnected, intent.hasExtra("configured"),
                intent.getBooleanExtra("configured", false), dataAvailable);
    }

    private void setConnectionStatus(String text) {
        statusView.setText(text);
        if (connectionProgress != null) {
            boolean connecting = text.contains("正在连接") || text.contains("正在重新连接")
                    || text.contains("正在等待首帧") || text.contains("等待画面恢复")
                    || text.contains("正在识别") || text.contains("正在准备")
                    || text.contains("正在与电脑通信");
            connectionProgress.setVisibility(connecting ? View.VISIBLE : View.GONE);
        }
    }

    private void setStreamingPresentation(boolean streaming) {
        streamingPresentation = streaming;
        if (streaming && rotationControl != null) {
            rotationControl.dismiss();
            rotationControl = null;
        } else if (!streaming) {
            showRotationControl();
        }
        inputHandler.removeCallbacks(showFirstFrameTimeout);
        connectionPanel.setVisibility(streaming ? View.GONE : View.VISIBLE);
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
            Display.Mode active = display.getMode();
            float bestRefresh = 0.0f;
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth() == active.getPhysicalWidth()
                        && mode.getPhysicalHeight() == active.getPhysicalHeight()
                        && mode.getRefreshRate() >= 119.0f && mode.getRefreshRate() > bestRefresh) {
                    bestRefresh = mode.getRefreshRate();
                    preferredModeId = mode.getModeId();
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
        connection.sendInputBatch(input);
        return true;
    }

    @Override
    public void onPacket(Protocol.Packet packet) {
        if (packet.header.type == Protocol.TYPE_HELLO) {
            String hostName = new String(packet.payload, StandardCharsets.UTF_8);
            PersistentDiagnosticLog.record("Status", "收到 Windows 握手：" + hostName);
            runOnUiThread(() -> {
                connectionStatus.hostContacted();
                boolean recovering = hasConfiguredStream || awaitingHostReconnect;
                // A real HELLO replaces an old idle notice; opening a socket alone does not.
                idleNoticeActive = false;
                explicitStopNoticeActive = false;
                awaitingHostReconnect = false;
                inputHandler.removeCallbacks(showWaitingAfterReconnectGrace);
                setConnectionStatus(ConnectionStatus.helloText(hostName, recovering));
            });
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
