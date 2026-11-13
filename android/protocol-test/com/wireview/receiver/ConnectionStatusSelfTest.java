package com.wireview.receiver;

public final class ConnectionStatusSelfTest {
    private static void require(boolean condition, String message) {
        if (!condition) { throw new AssertionError(message); }
    }

    public static void main(String[] args) {
        ConnectionStatus status = new ConnectionStatus();
        status.updateUsb(true, false, false, false);
        require(status.waitingText().contains("等待电脑识别"),
                "USB attach must wait for identification, not a stream request");
        require(!status.waitingText().contains("文件传输"),
                "missing USB configuration must not invent a data-mode problem");
        status.updateUsb(true, true, false, false);
        require(status.waitingText().contains("如电脑未识别")
                        && status.waitingText().contains("文件传输"),
                "explicitly unconfigured USB should provide conditional data-mode guidance");
        status.updateUsb(true, true, false, true);
        require(!status.waitingText().contains("文件传输"),
                "ADB, MTP, PTP or accessory availability must suppress data-mode advice");
        status.updateUsb(true, true, true, false);
        require(!status.waitingText().contains("文件传输"),
                "configured USB must not be called charge-only");
        status.hostContacted();
        status.updateUsb(true, true, false, false);
        require(!status.waitingText().contains("文件传输"),
                "actual communication outranks a later incomplete USB broadcast");
        status.updateUsb(false, true, false, false);
        require(status.waitingText().startsWith("USB 未连接"), "detach must clear waiting state");
        status.updateUsb(true, true, false, false);
        require(status.waitingText().contains("文件传输"),
                "physical reattach must discard communication evidence from the old cable session");
        require(ConnectionStatus.helloText("WireView device profile", false).contains("正在识别"),
                "profile HELLO must identify, not prepare a stream");
        require(ConnectionStatus.helloText("WireView Windows phase-0", false).contains("正在准备投屏"),
                "stream HELLO must prepare streaming");
        require(ConnectionStatus.helloText("WireView Windows phase-0", true).contains("等待画面恢复"),
                "stream reconnect must preserve recovery wording");
        require(ConnectionStatus.helloText("WireView device preview", false).contains("预览"),
                "preview HELLO must not be called stream startup");
        require(!ConnectionStatus.helloText("old or unknown host", false).contains("发起投屏"),
                "unknown HELLO must describe only established communication");
        System.out.println("All WireView connection status tests passed");
    }
}
