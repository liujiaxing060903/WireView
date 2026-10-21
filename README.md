# WireView 有线副屏

让 Android 手机和平板，成为 Windows 的第二块屏幕。

WireView 通过 USB 连接 Windows 与 Android，将电脑画面实时传输到手机或平板。你可以复制主屏进行演示，也可以把平板作为独立的扩展屏幕，放置文档、聊天窗口、参考资料或绘画工具。

项目包含 Windows 控制中心和 Android 接收端，支持触控与手写笔输入回传，视频通过 USB 在本机与设备之间传输。

## 主要功能

- **复制屏幕**：在 Android 设备上显示电脑主屏内容，适合演示与分享。
- **扩展屏幕**：通过 Parsec Virtual Display Driver 创建独立桌面，把窗口拖到平板上使用。
- **触控与手写笔**：支持多点触控、笔压、倾角和橡皮擦事件回传，实际可用能力取决于设备与应用。
- **仅显示模式**：可独立选择是否启用输入，默认仅显示电脑画面。
- **显示配置**：支持设备分辨率识别、输出分辨率设置、等比缩放与自定义显示模式。
- **连接恢复与诊断**：支持 USB 断开后自动重连，并提供会话状态和日志。

## 适合哪些场景

- 用平板扩展笔记本工作区，一边写代码，一边查看文档。
- 将聊天、音乐或参考资料放在副屏，释放主屏空间。
- 复制主屏进行近距离演示与内容分享。
- 在支持的 Android 设备上，通过触控或手写笔操作 Windows 应用。

## 使用条件

- 一台 Windows 10 / Windows 11 x64 电脑。
- 一台 Android 11 或更新版本的手机或平板。
- 一根支持数据传输的 USB 线，并在 Android 设备上开启、授权 USB 调试。
- Windows 端安装 WebView2 Evergreen Runtime，并准备官方完整的 Android SDK Platform Tools。
- 使用扩展屏幕时，需要另外安装 Parsec Virtual Display Driver；复制主屏无需该驱动。

USB 连接用于数据传输，不要求设备支持 USB-C DP 视频输出。一次会话支持连接一台 Android 设备。

## 基本使用流程

1. 在 Android 设备安装并打开 WireView，在电脑上准备好 Windows 程序和相关依赖。
2. 将完整的 `platform-tools` 文件夹放到 Windows 程序根目录，确保存在 `platform-tools/adb.exe`。
3. 用 USB 数据线连接设备，在 Android 弹窗中允许此电脑进行 USB 调试。
4. 打开 `WireView 控制中心.exe`，选择“仅显示”或“触控与手写笔”。
5. 点击“复制屏幕”或“扩展屏幕”开始使用；结束时点击“停止 WireView”。

Windows 便携包和 Android APK 可从 [GitHub Releases](https://github.com/liujiaxing060903/WireView/releases/latest) 下载。详细步骤见 [Windows 使用说明](docs/RELEASE_WINDOWS.md)，版本改动见 [1.1.20 发布说明](docs/releases/v1.1.20.md)；自行构建请参阅 [开发与调试资料](docs/DEVELOPMENT.md)。

## 当前状态与兼容性

- 当前源码版本为 **1.1.20**，项目仍在持续开发与兼容性验证中。
- **60 FPS 为已验证的可靠档位**；120 FPS 是实验性主机编码、发送目标，不保证 Android 屏幕最终以真实 120 Hz 呈现。
- 当前日常连接方式为 USB 上的 ADB/TCP；AOA/WinUSB 仍为实验通道。
- 部分 Intel 编码器、HDR/10-bit 桌面与虚拟显示驱动组合仍存在兼容性问题，建议先使用 SDR、60 FPS 验证。
- 当前不支持音频传输、Wi-Fi/远程连接与多设备同时会话。

## 项目组成

- `windows/host`：Windows 控制中心、桌面捕获、H.264 编码、会话与输入处理。
- `android`：Android 接收端、MediaCodec 解码与触控/手写笔采集。
- `protocol`：Windows 与 Android 共用的通信协议定义及测试。
- `scripts`：构建、安装和验证脚本。
- `docs`：使用说明、架构设计与开发记录。

Windows 使用 DXGI 捕获桌面、Media Foundation 编码 H.264，Android 使用 MediaCodec 解码显示；扩展桌面依赖外部 Parsec VDD。

## 反馈问题

欢迎通过 [GitHub Issues](https://github.com/liujiaxing060903/WireView/issues) 提交问题或建议。反馈连接、黑屏或卡顿问题时，请附上 Windows 版本、显卡型号、Android 设备型号、所用分辨率与帧率，以及能够复现问题的操作步骤。需要进一步定位时，可从控制中心的“诊断与日志”收集对应会话日志。
