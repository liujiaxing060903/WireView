# WireView 开发与调试资料

> 本文由原 README 迁移，保留构建命令、调试方法及历史实验记录。部分阶段性描述已过时；当前项目介绍见 [README](../README.md)，最新开发状态见 [2026-10-21 交接记录](TASK_HANDOFF_2026-10-21.md)。以下命令均在仓库根目录运行。

WireView 是一个通过 USB 将 Android 设备作为 Windows 低延迟扩展显示器使用的项目。

> ## 历史产品快照（2026-09-02）
>
> 当前日常可用路径是：**Windows 控制中心 + 已安装的 Parsec VDD + ADB reverse TCP + Android Receiver**。它支持安全复制屏幕、启用触控/手写笔的复制，以及创建或复用 Parsec 虚拟副屏后的扩展显示。
>
> 可发布文件位于：
>
> - Windows：`release/WireView-1.1.0-Windows-x64.zip`（完整解压后运行 `WireView 控制中心.exe`）
> - Android：`release/WireView-1.1.0-debug.apk`（侧载测试 APK，调试签名）
>
> 控制中心当前使用 **WebView2 混合架构**：`wireview_control.exe` 保留 C++ 的 ADB、Parsec VDD、会话、托盘和状态逻辑，以本地 HTML/CSS/JS 替换可见 UI 层；视觉和交互以根目录 [`style.md`](../style.md) 为准。`wireview_control_legacy.exe` 是可独立构建的原生 Win32/GDI 回退版本，适用于未安装或不兼容 WebView2 Runtime 的旧 Windows 电脑。
>
> 60 FPS 是已验证的可靠档。`120 FPS（实验）`仅表示主机编码/发送目标，尚未证明 Android 设备最终面板能稳定显示真实 120 Hz；请勿把 Host FPS 或解码遥测当作屏幕实际帧率。
>
> Windows 包附带 Parsec VDD 安装材料及兼容性回退说明；Android SDK Platform Tools 因 SDK 许可不随公开包分发。请从官方渠道下载后，将完整 `platform-tools` 文件夹放至发布包根目录。

## 面向下一次 UI 迁移的边界

- 不修改 Android、协议、H.264 编码、输入注入、ADB 重连或 Parsec VDD 的底层业务逻辑；复用 C++ 现有事件处理与状态快照。
- WebView2 使用 Evergreen Runtime；运行时缺失时控制中心会提示安装 Runtime 或启动同目录的 legacy 回退版本。Fixed Version Runtime 体积很大，不作为默认便携包方案。
- 必须保留当前原生控制中心作为旧 Windows 电脑的回退版本。Windows 10 1709 等旧系统需要单独验证 WebView2 兼容性。
- 当前原生界面已使用 `windows/host/assets/ui/` 内的 PNG 资源；WebView2 页面可复用或替换这些资源，但不要在未验证发布复制规则前遗漏它们。

下文保留了阶段 0 的技术验证记录，其中 IddCx 是已冻结的历史实验，不是当前产品副屏路径。当前产品实际链路为：

    Parsec VDD 显示器 -> DXGI 桌面采集 -> GPU H.264 编码 -> ADB reverse TCP -> MediaCodec -> Android Surface

## 当前代码

- protocol：与具体传输无关的二进制协议、CRC 校验和增量流解析器。
- windows/host：Windows Host、会话程序和原生控制中心；当前日常产品路径。
- windows/idd-spike：微软官方 IddCx 示例的冻结隔离验证工程，不是产品路径。
- android：可构建的 Android 11+ 接收端；当前通过可替换的传输接口使用 ADB 反向端口，后续接入 USB accessory。
- docs：架构决策与阶段 0 验收条件。
- scripts：本机构建环境检查。

## 构建当前原型

先进入 Visual Studio 2022 x64 开发者命令行，然后运行：

    cmake --preset windows-debug
    cmake --build --preset windows-debug
    ctest --preset windows-debug

运行协议演示：

    .\build\windows-nmake\windows\host\wireview_host.exe

运行真实 TCP 回环握手测试：

    .\build\windows-nmake\windows\host\wireview_host.exe --loopback

普通 PowerShell 中可用统一脚本构建 Windows Host 并运行全部测试：

    .\scripts\build-windows.ps1

P2 产品 USB 原型提供独立探针。默认命令只读枚举当前 Android WinUSB 引导接口，不会切换设备或修改驱动：

    .\build\windows-nmake\windows\host\wireview_usb_probe.exe --list-bootstrap

`--switch-unique-bootstrap` 会让唯一候选 Android 设备立即离开当前 USB 模式并以 AOA 身份重新枚举，ADB/MTP 会暂时断开；只能在用户在场并明确准备做 AOA 真机验证时运行。重新枚举后可用 `--inspect-accessory` 检查 WinUSB bulk 端点，用 `--handshake-accessory` 发送 WVR1 握手探针。

Windows Host 的协议会话已经通过 `ReliableByteStream` 与具体传输解耦；当前 ADB/TCP 使用 socket 适配器。产品 WinUSB bulk 适配器已实现独立 overlapped 读写，并可通过显式 `--transport winusb` 接入桌面会话。该入口只等待已经枚举为 AOA 的唯一 accessory，不会自行切换 Android USB 模式或修改驱动；默认仍是 ADB/TCP。

    .\build\windows-nmake\windows\host\wireview_host.exe --stream-desktop 30 --display '\\.\DISPLAYn' --input-log-only --transport winusb

当前 HUAWEI DBR-W10 与 OPPO PKB110 都能切换为 AOA+ADB `PID_2D01`、完成系统授权并由 Android 成功 `openAccessory()`，但 Android 读取线程均收不到 Windows 已写入 bulk OUT `0x01` 的 HELLO；正式 Host 的 overlapped WinUSB 后端也得到相同结果。两台设备都使用现有 `Motorola Composite AUTO Interface` 整设备 WinUSB 绑定，因此该驱动绑定/`PID_2D01` 组合路径列为兼容性阻塞并暂时延期，默认 ADB/TCP 不受影响。统一构建脚本当前运行 13 项 CTest，其中包含短写、逐字节分片、读取超时、断流、损坏包、WinUSB 端点布局、accessory 选择状态机、transport 参数和启动脚本合同回归。

Android 端会把 AOA attach、权限、`openAccessory`、握手首包、ACK、异常与关闭过程写入应用私有的有界诊断日志。日志采用两个 128 KiB 文件轮换，不记录视频帧或触控内容；AOA 导致 ADB 暂时消失后，物理重插恢复 ADB 即可一键提取：

    .\scripts\get-wireview-android-diagnostics.ps1

提取结果写入 `build/android-diagnostics/`。普通 ADB/TCP 的周期性等待状态不会写入该文件，避免淹没 accessory 故障证据。

若 `--switch-unique-bootstrap` 报告接口忙，先退出 Android Studio 以及会维护自带 ADB 的手机互联软件。参考机上的 O+Connect 会通过 Session 0 ADB 独占接口，GlideX 也自带 ADB；诊断测试中临时停止的相关服务均已恢复为原来的“自动/运行中”状态。

探测桌面输出与硬件 H.264 编码能力，以及测试当前物理桌面的采集/编码性能：

    .\build\windows-nmake\windows\host\wireview_host.exe --probe-video
    .\build\windows-nmake\windows\host\wireview_host.exe --capture-benchmark 8
    .\build\windows-nmake\windows\host\wireview_host.exe --encode-benchmark 8

等待 Android 通过 ADB 反向端口连接：

    adb reverse tcp:39871 tcp:39871
    .\build\windows-nmake\windows\host\wireview_host.exe --listen

运行 Android 侧的纯 Java 协议与传输双向流测试：

    .\scripts\test-android-protocol.ps1

构建 Android Debug APK：

    .\scripts\build-android.ps1

将最新版 Debug APK 安装到唯一已授权的 Android 平板并自动打开应用：

    .\scripts\install-wireview-android.ps1

若 APK 已构建、只想重新安装，可附加 `-SkipBuild`。

构建 IddCx 技术验证 DLL 和软件设备启动器：

    .\scripts\build-idd-spike.ps1

生成并静态验证 IddCx 测试驱动包（不会安装）：

    .\scripts\package-idd-spike.ps1

驱动包输出到 `build\idd-package\x64`。测试签名、证书信任和驱动安装会修改 Windows 系统状态，必须在管理员 PowerShell 中显式运行：

    .\scripts\install-idd-spike.ps1

注意：在启用 Secure Boot 的生产启动策略下，自签名测试包通常可以进入 Driver Store，但不能实际加载。WireView 的安装脚本不会自动关闭 Secure Boot、暂停 BitLocker 或修改 BCD；这些安全策略只能在专用测试环境中由用户明确决定。

安装完成后，普通 PowerShell 中运行以下命令可临时创建虚拟显示器；启动器保持运行期间显示器才存在，按 `x` 移除：

    .\scripts\start-idd-spike.ps1

可逆卸载命令如下；附加 `-RemoveTestCertificate` 时也会删除 WireView 创建的测试证书：

    .\scripts\uninstall-idd-spike.ps1 -RemoveTestCertificate

Android Studio 打开 `android` 目录后点击 Run，会自动构建、安装并启动 Android 应用。连接一台已授权的手机或平板后，在项目根目录运行首帧 USB 视频测试：

    .\scripts\test-usb-video.ps1

该脚本会为当前设备建立 `adb reverse`、启动临时 Windows Host，并发送约 10 秒的合成 H.264 测试流。更换设备、重启设备或 ADB 连接重建后需要重新运行脚本。脚本结束时临时 Host 会退出，Android 应用回到“等待 Windows Host”是正常现象。现阶段 Android Studio Run 负责安装应用，但不能代替电脑端 Host；后续会把 Host 做成长驻的 Windows 程序。

发送当前 Windows 真实桌面并自动检查首帧、解码错误与输入丢帧：

    .\scripts\test-usb-desktop.ps1 -DurationSeconds 15

日常镜像可使用一键常驻入口。它会等待唯一已授权的 USB Android 设备、恢复 ADB 通道、启动平板应用；拔插或应用连接中断后会继续等待并自动续传。默认是不会控制 Windows 的安全镜像模式，关闭窗口或按 `Ctrl+C` 即可停止：

    .\scripts\start-wireview.ps1

启动脚本也支持显式实验通道：

    .\scripts\start-wireview.ps1 -Transport WinUsb -DisplayDevice '\\.\DISPLAYn'

默认 `-Transport Adb` 的行为不变。`WinUsb` 模式不查找或启动 ADB，不主动切换平板 USB 模式，只等待已经枚举为 AOA 的唯一 accessory；断线后会重新确认同一显示器并重新等待 accessory。华为与 OPPO 的 `PID_2D01` 路径当前都没有 WVR1 ACK，因此此选项保留为实验入口，暂不作为日常通道。

该入口会将完整 Host 日志写入临时文件，同时通过原子状态快照提供当前连接、目标显示器、实际视频模式、输出 FPS/码率、首帧、Surface P50/P95，以及平板实际解码器和刷新模式，适合长时间使用。

Windows Host 的最小控制窗口也已提供。它可以从窗口内启动安全镜像、启用输入的镜像，或停止其自身启动的会话：

    .\scripts\open-wireview-control.ps1

这是当前开发版的控制窗口；启动前会枚举所有已附加到 Windows 桌面的显示器，并允许明确选择目标 `DISPLAYn`。默认顺序为上次保存的目标、现有 Parsec 副屏、其他副屏、主屏，不会仅因打开控制中心而创建新的 Parsec VDD；显示器变化后可点击“刷新”。窗口还会显示 Parsec VDD 版本与当前副屏数量。已有任意 Parsec 副屏时会直接复用，并禁用“创建 WireView 副屏”，防止误创建第三屏；只有当前没有 Parsec 副屏时，该显式操作才能新增 VDD。创建后由独立 broker 持有精确 driver index，视频 Host/USB 重连继续复用同一目标。“移除 WireView 副屏”及控制中心彻底退出会通知 broker 只清理它本次创建的屏幕，不会删除用户原有的 Parsec/Sunshine 显示器。控制中心还会扫描本机 WireView 的残留会话记录；仅当恰好一条记录、owner/broker 都已退出且记录的 `DISPLAYn` 仍是 Parsec 副屏时，才显示“候选残留”的二次确认入口。Parsec 会复用 driver index，所以程序会明确要求用户确认 index 未被其他管理程序重新使用，不会宣称能自动证明归属。“输出分辨率”可选原生自动、常用预设，或直接输入任意偶数尺寸（例如 `1600x1000`）。“视频帧率”可选 60 或“120（实验）”；120 表示 Windows Host 的编码/发送目标，**不代表设备最终屏幕帧率**。选择 120 时控制中心会检查目标 Windows 显示器的当前刷新率与电脑供电状态，并在选项、自动识别、兼容性和启动状态中提示仍需在设备端实测。点击“设备端预览所选画面范围”会先经 ADB 与设备握手、取得当前方向的设备尺寸/刷新率，并只在设备上显示居中的白框：白框内是所选分辨率的等比显示范围，框外是黑边；预览不会创建 VDD、捕获桌面或启动编码，正式开始镜像会自动结束预览。正式视频也采用同一 `fit-center` 规则，绝不拉伸。控制中心另有 5 个持久化的自定义模式槽（`宽x高@Hz`）；保存后其分辨率可直接用于 WireView 选择。槽配置当前不会擅自写入 Parsec VDD 注册表或重新连接用户现有副屏。“传输通道”默认 ADB；WinUSB 保留为“实验，当前阻塞”入口，界面会明确说明华为/OPPO 的 `PID_2D01` 驱动绑定均无法完成 WVR1 握手，启动诊断会话前还会再次警告；目标、分辨率、视频帧率和传输选择会保存到当前 Windows 用户的本地 WireView 配置。窗口会随当前组合显示建议，会话运行时选项锁定，停止后恢复可编辑。最小化或关闭窗口会进入系统托盘。右键托盘图标可恢复窗口、启动/停止镜像，或彻底退出控制中心；彻底退出会结束它启动的会话。窗口和托盘提示会显示实际传输通道、等待 USB、正在镜像和连接恢复等本机状态。

会话运行时，控制中心会直接显示上述摘要；状态区保持固定高度、自动换行并提供纵向滚动，实时刷新时保留用户当前的滚动位置，不会随遥测字段增加而继续拉长窗口。状态文件仍兼容旧版单行文本。状态中的“主机发送 FPS”和码率按已成功发送的编码帧每秒更新，**不是设备最终屏幕帧率**；Surface 延迟随 Android 抽样遥测更新。首帧后 Android 还会回传实际 AVC decoder、硬件/软件实现、标准低延迟标记、当前刷新率与最高刷新率；控制中心据此检查 60/120 FPS 主机发送条件是否与屏幕模式相容，不能据此证明面板实际帧率。Android 同时每秒回传解码输入收到/排队、输入丢帧和解码错误累计值，控制中心显示“健康：启动中/正常/警告/异常”；首个丢帧或错误会立即上报。标准低延迟标记仅作说明，兼容性仍以首帧和真机运行结果为准。

如需用平板触控和压感笔控制当前选择的 Windows 目标显示器，显式启用输入；请先确保该显示器前台没有危险操作：

    .\scripts\start-wireview.ps1 -EnableInput

默认持续 8 小时。可按需设置时长或 16:10 的缩放输出：

    .\scripts\start-wireview.ps1 -DurationMinutes 120
    .\scripts\start-wireview.ps1 -StreamWidth 1920 -StreamHeight 1200

`120` 为实验性主机发送上限：它会让 Android 应用请求 120 Hz 屏幕模式，并把主机编码器、H.264 流标记一并切到 120 FPS；设备最终是否以 120 Hz 显示，必须单独实测：

    .\scripts\start-wireview.ps1 -StreamWidth 1920 -StreamHeight 1200 -StreamFrameRate 120

原生 2560×1600@120 已在参考主机完成 30 分钟链路长测：216000/216000 主机发送帧、持续 120.00 FPS、48.01 Mbps，Android 解码输入丢帧 0、错误 0、会话无重连，3600 个平板到 Surface 提交样本为 P50 17.37 ms、P95 32.17 ms。这证明编码、传输与解码提交链路可运行在该档位，**不构成设备真实 120 Hz 屏幕呈现的保证**。日常推荐 60 FPS；`-StreamFrameRate` 只接受 `60`（默认）或实验性 `120`。

首次验证平板触控回传时，先使用只记录模式；在平板画面上轻点、拖动几次，脚本结束时应显示 `Android input` 的非零 `records`，且不会向 Windows 注入任何点击：

    .\scripts\test-usb-desktop.ps1 -DurationSeconds 30 -InputLogOnly

确认回传记录正常后，才移除 `-InputLogOnly` 进行受控的真实注入验证。真实注入会映射到当前 Windows 主屏，测试前应先切换到没有危险按钮的空白窗口。

脚本以 Android 经同一 USB 会话发回的 `Android first-frame telemetry` 作为首帧验收依据；ADB 日志仅保留为诊断信息，不再决定测试通过与否。测试结束会给出 `Android arrival-to-Surface latency` 的 P50/P95（视频包到达平板后至解码并提交 Surface 的本地耗时）。当前 `End-to-end frame latency` 仍受编码器时间戳语义影响，只作诊断参考，不作为绝对延迟验收结论。

如需在高动态桌面（例如 OneNote 实时墨迹）优先保持 60Hz，可使用 GPU 缩放后的 1920×1200 测试模式；它保持参考平板的 16:10 比例，且触控坐标仍映射到完整 Windows 主屏：

    .\scripts\test-usb-desktop.ps1 -DurationSeconds 20 -StreamWidth 1920 -StreamHeight 1200

阶段 0 的连续稳定性回归可安全地运行 30 分钟；`-InputLogOnly` 会保留触控统计但不控制 Windows：

    .\scripts\test-usb-desktop.ps1 -DurationSeconds 1800 -InputLogOnly

USB 拔插恢复测试会先提示拔线，再提示重新插入并自动恢复 ADB 反向端口；期间不会向 Windows 注入输入：

    .\scripts\test-usb-reconnect.ps1

参考环境已使用现有 `DISPLAY6` 完成真实物理拔插回归：Host 检测到一次传输中断，ADB reverse 恢复后完成第二次握手，两段会话均继续采集同一 `DISPLAY6`，显示拓扑保持 `DISPLAY1 + DISPLAY6`，没有进入 VDD 创建路径。重连后的完整 8 秒会话为 60.05 FPS、解码输入丢帧 0、错误 0。

可先用软中断模式验证安全的应用/ADB reverse 恢复，无需拔线。显式传入目标后，每次重连都必须继续采集同一 `DISPLAYn`；目标消失时立即失败，不回退主屏、不创建 VDD：

    .\scripts\test-usb-reconnect.ps1 -DurationSeconds 45 -DisplayDevice '\\.\DISPLAY6' -SimulateAppReconnect

不传 `-DisplayDevice` 时仍保留主屏开发测试兼容性。Android 端默认会在输入帧后的 2/4/6ms 非阻塞检查解码输出，以降低常见帧的 Surface 提交等待。WireView 的产品副屏路径改为使用外部、已签名的 Parsec VDD 创建 Windows 显示器 2，再由 WireView 采集该显示器；设计、依赖边界和迁移进度见 [PARSEC_VDD_ARCHITECTURE.md](PARSEC_VDD_ARCHITECTURE.md)。自研 IddCx 技术验证仍保留在 [EXTENDED_DISPLAY_PATH.md](EXTENDED_DISPLAY_PATH.md)，但不再作为日常路径。

触控与压感笔回传的二进制格式见 [INPUT_PROTOCOL.md](INPUT_PROTOCOL.md)。当前 Debug APK 已接入单指/单笔事件采集与 Windows 合成指针入口；首次真实点击验证应在确认主屏没有危险操作按钮后进行。

阶段 0 的当前进度与下一项操作见 [PHASE_0.md](PHASE_0.md)。
产品 AOA/WinUSB 通道的边界、驱动风险与迁移顺序见 [PRODUCT_USB_TRANSPORT.md](PRODUCT_USB_TRANSPORT.md)。

## 平台基线

- Windows 11 x64，首版优先支持 23H2 及以上。
- Visual Studio 2022、MSVC v143、Windows SDK 与 WDK。
- Android 11 及以上作为低延迟首要目标，后续再扩大兼容范围；解码器以真机首帧测试结果选择，不只依赖系统是否声明标准低延迟标记。
- 首个兼容模式为 SDR 1920x1080@60Hz、H.264；参考平板已额外通过 2560x1600@60 原生分辨率验证。
