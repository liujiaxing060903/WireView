package com.wireview.receiver;

/** Connection-page text, independent of Android UI and transport lifetimes. */
final class ConnectionStatus {
    private boolean connected = true;
    private boolean dataUnavailable;
    private boolean contacted;

    void updateUsb(boolean connected, boolean configurationKnown, boolean configured,
            boolean dataFunctionAvailable) {
        if (!connected || !this.connected) { contacted = false; }
        this.connected = connected;
        dataUnavailable = configurationKnown && !configured && !dataFunctionAvailable;
    }

    void hostContacted() {
        contacted = true;
    }

    String waitingText() {
        if (!connected) { return "USB 未连接\n请用 USB 数据线连接电脑。"; }
        if (dataUnavailable && !contacted) {
            return "USB 已接入，等待电脑识别\n如电脑未识别，请将 USB 用途切换为“文件传输”，并允许 USB 调试。";
        }
        return "USB 已接入，等待电脑识别\n请在电脑打开 WireView 控制中心，并允许手机上的 USB 调试。";
    }

    static String helloText(String hostName, boolean recovering) {
        if ("WireView device profile".equals(hostName)) {
            return "正在识别设备\n电脑正在读取分辨率和刷新率。";
        }
        if ("WireView device preview".equals(hostName)) {
            return "正在准备预览\n电脑正在读取设备资料，请稍候。";
        }
        if ("WireView Windows phase-0".equals(hostName)) {
            return recovering ? "已连接电脑\n等待画面恢复。"
                    : "正在准备投屏\n电脑已发起投屏，正在准备画面。";
        }
        return "已连接电脑\n正在确认设备状态，请稍候。";
    }
}
