# WireView Windows 发布说明

## 运行环境

- Windows 10 或 Windows 11（x64）。
- 主控制中心使用 Microsoft Edge WebView2 Evergreen Runtime。若启动时提示缺失，请从 <https://developer.microsoft.com/microsoft-edge/webview2/> 安装 Evergreen Runtime。
- WireView 的日常传输使用 ADB。请从 <https://developer.android.google.cn/tools/releases/platform-tools?hl=zh-cn> 下载官方 Android SDK Platform Tools，解压其中完整的 `platform-tools` 文件夹到本发布包根目录。不要只复制 `adb.exe`。

## 第一次使用

1. 安装 `WireView-1.1.0-debug.apk` 到 Android 设备并打开 WireView。
2. 在 Android 的“设置 → 关于手机”中连续点击“版本号”或“MIUI 版本”七次，开启开发者选项。
3. 进入“设置 → 系统 / 更多设置 → 开发者选项”，开启“USB 调试”。
4. 用可传输数据的 USB 线连接电脑；USB 用途选择“文件传输”而不是“仅充电”，并在 Android 弹窗中允许此电脑进行 USB 调试。
5. 如需“扩展屏幕”或创建 WireView 副屏，先进入 `安装依赖\\Parsec VDD`，以管理员身份运行 `ParsecVDisplay-v0.45-setup.exe`；完成后确认设备管理器的 `Parsec Virtual Display Adapter` 没有黄色感叹号。
6. 运行 `WireView 控制中心.exe`。设备连接后可选复制屏幕或扩展屏幕。

## Parsec VDD 兼容性回退

若 Parsec VDD 显示 `UNKNOW_PROBLEM`，或设备管理器报告代码 31：

1. 卸载失效的 Parsec VDD 程序和驱动，并重启 Windows。
2. 以管理员身份运行 `parsec-vdd-0.41.0.0.exe`。
3. 解压 `ParsecVDisplay-v0.45-portable.zip`，只运行其中的 `ParsecVDisplay.exe`。
4. 不要运行压缩包内的 `parsec-vdd-0.45.0.0.exe`，否则会覆盖已安装的 0.41 驱动。

这是在部分电脑上验证过的兼容性回退方案；默认优先使用 0.45 安装器。

## 说明

- Parsec VDD 是第三方驱动，WireView 不会自动安装或删除用户已有的其他 Parsec/Sunshine 显示器。
- Android SDK Platform Tools 不随公开包分发；其官方 SDK 许可限制再分发。控制中心会自动识别放在本包根目录的 `platform-tools\\adb.exe`。
- Windows 与 Android 安装包当前均未使用代码签名证书。Windows 可能显示 SmartScreen 警告；Android APK 为调试签名包，适合直接侧载测试，不可作为已具备正式发布签名的应用商店包。
