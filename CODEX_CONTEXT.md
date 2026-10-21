# WireView 项目交接上下文

> 本文件记录仓库与当前开发环境的项目事实，不记录聊天过程。除明确写为“待确认”的事项外，内容以当前代码、脚本、已完成构建和真机验证结果为准。

## 2026-10-21 最新交接入口

- 最新交接请先阅读 `docs/TASK_HANDOFF_2026-10-21.md`，并结合 `docs/TASK_HANDOFF_2026-09-09.md`；近期状态优先于下方旧快照。
- 源码版本为 `1.1.20`，包含简易模式扩展副屏目标选择及失效显示器预检修复；Windows Release 的 13 项 CTest 已通过，Android 新版 APK 和 1.1.20 发布包仍待完成。
- 公开源码仓库：<https://github.com/liujiaxing060903/WireView>。默认分支 `main`，本地跟踪 `origin/main`；构建、发布包、备份及运行缓存均未纳入 Git。

## 2026-09-02 当前交接快照（下一次 Task 必读）

> 本节优先级高于下文所有较早记录。Windows 控制中心的 **WebView2 混合界面第一阶段已完成**；不要误以为 ADB、Parsec VDD、会话或 Android 逻辑被迁移到页面层。后续工作是补齐真实设备回归并继续精修 UI。

### 1.0 当前产品与可交付物

> 迁移实施更新（2026-08-31）：WebView2 第一阶段已落地并通过 Windows 构建/13 项 CTest。`wireview_control.exe` 现在是 C++ 后端 + 本地 WebView2 页面；`wireview_control_legacy.exe` 保留原生 Win32/GDI 回退入口。C++ SDK 固定在 `external/webview2/sdk`（Microsoft.Web.WebView2 1.0.4191.47），使用静态 Loader，发布仍采用 Evergreen Runtime、不打包 Fixed Runtime。新页面位于 `windows/host/assets/web/`，构建后复制到控制中心同级 `web/`；现有 PNG 同时复制到 `web/ui/`。桥接仅接受固定页面命令，状态由 C++ 快照推送给页面；未迁移 ADB、VDD、会话、托盘或 Android 逻辑。已自动验证未连接、简易/完整页面和状态同步；尚未在有已授权 Android 设备的环境验证真实运行中/停止后、托盘恢复以及 Runtime 缺失提示，且未执行 VDD Add/Remove。

- 产品定位不变：WireView 通过 USB/ADB 将 Android 手机或平板用作 Windows 的复制屏幕或扩展副屏。当前产品副屏依赖用户已安装的 Parsec VDD；WireView 负责桌面捕获、H.264 编码、ADB reverse TCP 传输、Android 显示，以及可选的触控/手写笔输入回传。
- 当前可发布 Windows 包：`release/WireView-1.1.0-Windows-x64.zip`；完整解压后使用根目录的 `WireView 控制中心.exe`。APK 为 `release/WireView-1.1.0-debug.apk`（调试签名侧载包）。发布包不分发 Android SDK Platform Tools；控制中心会查找系统安装的官方 Platform Tools，或发布包根目录的 `platform-tools\\adb.exe`。Windows/Android 产物当前均未配置正式代码签名。
- 当前已交付控制中心是 **C++ Win32 后端 + WebView2 本地页面**，主后端文件为 `windows/host/src/control_main.cpp`，页面位于 `windows/host/assets/web/`。它已有简易/完整模式、托盘、自动 ADB 设备识别、VDD 管理、复制/扩展与输入启动入口、Windows 显示设置入口、可调整大小的诊断日志窗口及运行状态同步等业务入口；`wireview_control_legacy.exe` 继续保留原生 Win32/GDI 回退版本。
- 当前原生 UI 已添加 PNG 主视觉资源：`windows/host/assets/ui/`，包括复制屏幕、扩展屏幕、仅显示、触控与手写笔图形；`windows/host/CMakeLists.txt` 会在构建后复制到 `wireview_control.exe` 同级 `assets/ui/`。这些是现有原生界面的资源，不是 Web 前端资源。

### 已验证与未完成边界

- 已验证可靠档为 60 FPS 端到端使用；ADB USB 拔插后可以自动重连，Android 在会话停止/Host 退出后会回到空闲或断开状态。Android 端可识别当前分辨率与刷新率，并在未连接时随旋转更新。
- 120 FPS 仍仅表示 Windows Host 编码/发送目标。OPPO PKB110（Android 16、2374×1080、120 Hz）的摄影实测约为 60–70 FPS；不要宣传为已实现设备端真实 120 Hz。后续专项验收仍固定使用“本地 120 格基准视频 + 1/10 秒曝光照片”。
- Parsec VDD broker 的创建/移除机制已实现；某些低版本 Windows（已实测 Windows 10 1709）可能在创建后未能及时把 VDD 附加到 Windows 桌面。用户可以在 Parsec 中手动创建屏幕，再回到控制中心刷新、选择目标并启动。这是 Windows/驱动兼容性限制，不应破坏或删除用户已有 Parsec/Sunshine 显示器。
- 产品 USB（AOA/WinUSB）路径仍受设备驱动兼容性阻塞，日常产品通道是 ADB/TCP；不要将实验 WinUSB 入口改成默认路径。

### 下一阶段：WebView2 控制中心迁移（已获用户同意）

- **迁移范围：仅 Windows 控制中心的展示与交互层。** 必须保留 C++ 后端：ADB 查找/启动、设备探测、Parsec VDD、会话进程、状态文件、托盘、Windows 设置启动、Android 通信与所有既有业务逻辑。Android 端、协议、视频/输入链路不在本次迁移范围。
- 推荐架构：`wireview_control.exe` 继续作为 C++ 宿主与后端；嵌入 WebView2，以本地 HTML/CSS/JS 实现页面；通过受限的宿主—页面桥接调用现有 C++ 事件处理，并将状态快照推送/拉取给前端。不要把 ADB/Parsec/会话逻辑复制到 JavaScript。
- 视觉与产品规范唯一来源是根目录 `style.md`，以及用户提供过的“简易模式 / 完整模式”参考图。简易模式重点是设备卡、复制/扩展大卡片、输入分段选择器与底部状态；完整模式重点是概览卡片、可展开显示配置、副屏/VDD、会话摘要、单一“启动 WireView”CTA、高级工具和进入诊断详情页。具体规则见 `style.md`。
- 设备卡当前优先显示 Android 型号，例如 `PKB110`；以后用户会提供“型号 → 商品名称/手机或平板形态”的映射表。缩略图目前只需区分手机/平板与 USB 已连接/未连接，不需要为每个机型制作真实渲染。
- WebView2 取舍：比 GDI 更容易严格实现参考图中的 CSS 布局、图标、hover/active/disabled 状态与动效，但会引入 WebView2 Runtime 依赖、更慢的首次 UI 初始化和更多内存/进程。推荐优先采用 Evergreen Runtime 并在缺失时给出清晰安装提示；不要将大于 250 MB 的 Fixed Version Runtime 直接塞进首个便携压缩包，除非用户明确接受。Windows 10 1709 等旧系统需要单独实机兼容性验证。
- 回退策略：现有原生控制中心是老电脑回退版本。迁移前应复制/保留可构建的原生入口或明确的 legacy 构建目标；不要覆盖后让老电脑失去可用版本。先建立 WebView2 宿主和最小页面，再逐项迁移 UI，期间保持原生后端和发布包可用。

### 接手顺序与验证

1. 先阅读本节、`README.md`、`style.md`、`windows/host/CMakeLists.txt` 和 `windows/host/src/control_main.cpp`，确认现有控件 ID、状态模型与事件处理入口；不要一开始重写业务逻辑。
2. 迁移前先构建现有版本并运行控制中心状态回归。`wireview_control.exe` 若在运行会锁住链接文件，必须先从托盘“退出控制中心”。
3. WebView2 首先验证：运行时缺失提示、页面正常加载、C++→页面状态同步、页面→C++ 启动/停止/设置/模式切换各一个最小闭环；随后才迁移全部视觉与交互细节。
4. 每一次 UI 改动均需同时检查简易模式、完整模式、诊断页、运行中禁用状态、未连接状态和托盘关闭/恢复；不得只靠静态截图判断。

## 2026-08-26 当前交接快照（下一次 Task 优先阅读）

> 本节优先级高于下文较早的长测记录；尤其不能把 Host/解码遥测的 120 FPS 误写成 Android 设备最终面板的 120 Hz。

### 当前可交付基线

- 产品形态：Windows 通过已安装的 Parsec VDD 提供虚拟副屏；WireView 用 DXGI 捕获该 `DISPLAYn`、H.264 编码、经默认 ADB reverse TCP 发送到 Android `SurfaceView`，并可选择回传触控/笔/鼠标输入。
- 已验证可靠档：**60 FPS 端到端使用**。真实 USB 拔插重连、输入、原生及自定义等比黑边输出、设备分辨率识别、画面范围预览与光标修复均已有真机验证。
- 120 的正确表述：控制中心的 `120（实验）`、脚本 `-StreamFrameRate 120` 均是 **Windows Host 编码/发送目标**，不是“设备真实 120 Hz”的承诺。代码界面、自动识别结果、兼容性状态、启动状态与 README 已统一提示“设备最终屏幕帧率需实测”。
- 已知实测：OPPO PKB110（Android 16、2374×1080、120 Hz）播放本地 120 格基准视频，在 1/10 秒曝光照片中约为 12 格；同设备 WireView 即使 Host `output_fps` 约 120，且此前临时加入的 `MediaCodec.releaseOutputBuffer` 遥测也有 110+，原生 `2374×1080` 与等比 `1920×874` 仍只有约 6–7 格，约 60–70 FPS。结论是瓶颈位于该机最终 `SurfaceFlinger` / 显示合成路径，**不是** Host 编码、USB 发送或解码输出释放。不要以控制中心“输出 FPS”、解码输入计数或刷新率匹配文案替代摄影实测。
- 已试且已回退：在 `Surface` 生命周期中调用 `Surface.setFrameRate` 没有可见改善；改用 `TextureView` 曾在 OPPO Android 16 因背景 drawable 崩溃，修正后反而只有约 50–60 FPS。因此当前代码恢复为已稳定的 `SurfaceView.setZOrderOnTop(true)`，并移除了那次实验加入的“设备提交 Surface FPS”等专用指标。

### 当前 UI / VDD 事实

- 控制中心分辨率、VDD 刷新率与视频帧率是三个独立选择：为避免用户误配，选择“投屏分辨率”会同步建议相同的 VDD 物理模式；正式视频采用 `fit-center` 等比显示，宁可黑边也不拉伸。
- 点击“识别已连接设备”会通过 ADB 取得设备分辨率/最高刷新率：有匹配分辨率时自动选择它、VDD 240 Hz 和相应的 60 / 实验性 120 Host 目标；无匹配项时提示用户在 5 个自定义模式槽中添加。用户已成功添加手机 `2374×1080`。
- “设备端预览所选画面范围”发生在创建 VDD/镜像前，只显示白色范围框，不创建副屏、不捕获桌面。
- Parsec VDD 当前是外部依赖，不随 WireView 分发。用户计划的发布指引是：先在 Parsec VDD 手动创建屏幕，再在控制中心选该屏幕并开始镜像；这条可用路径不要求先完成 WireView broker 的真机 Add/Remove 回归。
- 已修复残留记录误阻塞创建：当当前 Parsec 副屏为 0、历史 `vdd-session` 的 `DISPLAYn` 映射无法确认时，状态会说明不可安全清理，但**不会**禁用“创建 WireView 副屏”。“移除”仍只适用于 WireView 本次 broker 创建并持有的副屏。参考机曾出现 `DISPLAY6 / index 0` 残留记录；它是历史状态，不可假设每台机器均相同。

### 下一次接手的优先项

1. 每次先构建并跑 13 项 Windows CTest；若 `wireview_control.exe` 正在运行，先请用户从托盘“彻底退出”，否则链接会被锁定。
2. 若继续 120 专项，只做可验证的显示层定位；验收固定采用“本地 120 格视频基准 + 1/10 秒曝光照片”，不得仅看 Host 遥测。发布前维持“实验性、需实测”的文案。
3. 发布前主要剩余工作是控制中心界面美化、实际发布材料/安装器/许可确认，以及产品 USB（AOA/WinUSB）兼容性阻塞的后续决策；日常 ADB 路径可继续使用。
4. WireView broker 创建/移除 VDD 的真机回归需要独立、没有用户 Parsec 副屏的环境；未经用户明确同意，不要删除或改动现有 Parsec/Sunshine 显示器。

### 固定验证命令

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -BuildDirectory build\windows-nmake
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-android.ps1
```

Windows 构建脚本会运行 13 项 CTest。Android 构建需要 Gradle 缓存访问；Debug APK 输出为 `android/app/build/outputs/apk/debug/app-debug.apk`，可用 `scripts/install-wireview-android.ps1` 安装。

## 项目目标

WireView 的最终目标是让一台 Android 手机或平板通过 **USB 有线链路**成为 Windows 的低延迟扩展显示器，体验目标接近 SuperDisplay：

- Windows 主屏保持为显示器 1；Android 设备承载 Windows 的独立显示器 2。
- 用户能在 Windows 显示设置中使用“扩展这些显示器”，将窗口拖到平板上，而不是只镜像主屏。
- 平板端硬件解码 H.264 并直接渲染到 `Surface`；提供稳定 60 FPS 与实验性 120 FPS 主机发送档，后者不保证设备最终屏幕以 120 Hz 呈现。
- 平板触控、多点触控、压感笔、橡皮擦、倾角和鼠标事件回传 Windows，映射到目标副屏。
- USB 拔插/连接中断后可恢复；默认安全镜像模式不注入 Windows 输入，显式开启后才注入。

当前产品路径使用外部已签名的 **Parsec VDD** 创建 Windows 虚拟显示器；WireView 负责采集、编码、传输、Android 显示和输入回传。

## 当前已完成功能

### Windows Host 与视频链路

| 模块 | 状态 | 已实现内容 |
|---|---|---|
| DXGI 桌面采集 | 已完成并真机验证 | `IDXGIOutputDuplication` 采集任意指定 `\\.\DISPLAYn`，不再只限制主屏。 |
| GPU 色彩转换 | 已完成 | D3D11 Video Processor 将 BGRA 转 NV12，缩放到用户指定的偶数输出尺寸。 |
| H.264 硬件编码 | 已完成并真机验证 | Media Foundation 硬件 AVC 编码；AMD 参考机使用 `AMDh264Encoder`，低延迟、低延迟 VBR、无 B 帧、GOP 60、单参考帧等控制已接入。 |
| USB 传输 | 已完成并真机验证 | 开发阶段用 `adb reverse tcp:39871 tcp:39871` 上的 loopback TCP；协议使用二进制分包与 CRC。 |
| 多显示器选择 | 已完成 | `wireview_host.exe --list-displays` 枚举附加 DXGI 输出；`--display \\.\DISPLAYn` 精确选择。输入映射随目标显示器虚拟桌面矩形变化。 |
| Parsec VDD 集成 | 已完成，产品路径 | 可探测、创建、保活、移除一个 WireView 自己创建的 VDD；也可复用已有 Parsec VDD 显示器。第三方驱动不随项目安装或分发。 |
| 端口重绑恢复 | 已完成 | 监听端口 `39871` 在断线后通过 `SO_REUSEADDR` 支持快速重新绑定。 |
| Windows 光标叠加 | P0 已完成并真机验证 | 非主屏使用 Windows 系统光标回退；普通箭头、手形、左右/上下缩放与 14×25 I 型光标均已校准。窗口移动/缩放期间与 VDD 原生光标安全切换，桌面及资源管理器框选仍保留 WireView 光标。 |

### Android Receiver

| 模块 | 状态 | 已实现内容 |
|---|---|---|
| APK 与连接 | ADB 路径已完成；产品 USB 抽象已开始 | Android 11+（`minSdk=30`），包名 `com.wireview.receiver`。`ConnectionClient` 已改为依赖可替换的 `TransportConnector` / `TransportConnection`，当前默认后端仍是 ADB reverse TCP。Android Studio 打开 `android/` 后可直接 Run 安装。 |
| H.264 解码与显示 | 已完成并真机验证 | 选择可用 AVC `MediaCodec` 解码器，输出到 `SurfaceView`；首帧、实际格式、启动耗时有遥测。参考平板使用 `c2.qti.avc.decoder`，2560×1600@60 成功。 |
| 60/120 FPS 主机发送档 | 已完成；最终 120 Hz 呈现待验证 | Host `start_stream` 传帧率；Android 端在 120 模式请求受支持的 `Display.Mode`，并回传实际/最高刷新率。原生 2560×1600@120 的 30 分钟长测证明主机发送与解码提交持续 120.00 FPS、零解码输入丢帧，但 OPPO 的摄影实测仅约 60～70 fps；不得将该链路数据表述为面板实际 120 Hz。 |
| 全屏显示 | 已完成 | 首帧后隐藏状态文字、沉浸式全屏；断流/失败时恢复状态页。 |
| 输入采集 | 已完成并真机验证 | `MotionEvent` 采集手指、多指（参考验证到 4 指）、手写笔、橡皮擦、压力、倾角、鼠标；笔移动在约 33 ms 内合并，降低回传压力。 |
| 遥测 | 已完成 | 首帧 Surface 提交、抽样到达至 Surface 时间、输入/握手状态、实际 AVC decoder、实现类型、低延迟标记、平板当前/最高刷新率，以及解码输入收到/排队/丢帧和错误计数通过协议 telemetry 回传。健康计数每秒更新，首个异常立即上报。 |

### Windows 输入注入

| 模块 | 状态 | 已实现内容 |
|---|---|---|
| 手指多点触控 | 已完成并真机验证 | Windows Synthetic Pointer touch 注入，多点与拖拽可用。 |
| 笔输入 | 已完成并真机验证 | Synthetic Pointer pen 注入，压感、倾角、橡皮擦已接入；OneNote 手写笔迹和粗细变化已验证。 |
| 鼠标 | 已完成 | 鼠标坐标映射到虚拟桌面并用 `MOUSEEVENTF_VIRTUALDESK` 注入。 |
| 副屏坐标映射 | 已完成并真机验证 | `DesktopCaptureArea` 包含 left/top/width/height。鼠标使用虚拟桌面绝对轴；Synthetic Touch/Pen 的 `ptPixelLocation` 会从全局屏幕坐标转换为相对虚拟桌面左上角的坐标，因此 VDD 位于主屏左侧或上方时仍能正确响应。 |

### 测试、启动与控制

| 模块 | 状态 | 已实现内容 |
|---|---|---|
| Windows 构建与测试 | 已完成 | `scripts/build-windows.ps1` 构建并运行 13 项 CTest：协议、IDD frame relay、relay pipe、Host loopback、VDD broker 状态发布、控制中心新旧状态格式兼容、重连目标消失时禁止回退主屏、USB probe 常量自测、可靠字节流分片/短写/超时/断线/损坏包测试、WinUSB bulk 端点选择、AOA accessory 选择/等待状态机、非法 transport 参数拒绝、启动脚本 transport 合同。最近一次独立目录完整测试全部通过。 |
| Android 协议测试与构建 | 已完成 | `scripts/test-android-protocol.ps1` 同时运行协议和真实 loopback 双向传输自测；`scripts/build-android.ps1`、`scripts/install-wireview-android.ps1` 负责 APK。 |
| USB 视频/桌面测试 | 已完成并真机验证 | `test-usb-video.ps1` 合成 H.264；`test-usb-desktop.ps1` 真实桌面、首帧、解码丢帧、输入统计。 |
| 稳定性与拔插测试 | 已完成固定副屏软/物理重连及 120 主机链路长测 | `test-usb-reconnect.ps1 -DisplayDevice ...` 已在 `DISPLAY6` 真机验证真实拔出/插回：检测到一次中断，ADB reverse 自动恢复，两次握手均采集同一副屏，拓扑保持 `DISPLAY1 + DISPLAY6`，没有创建 VDD。原生 2560×1600@120 的 30 分钟长测为 216000/216000 Host 发送帧、120.00 FPS、零解码输入丢帧、零错误、零重连；这只证明主机至解码提交链路稳定，不构成设备面板真实 120 Hz 的验证。 |
| 日常启动入口 | 已完成，近期已修订 | 发布版控制中心启动同目录的原生 `wireview_session.exe`：等待唯一已授权设备、建立 ADB reverse、启动 Android 应用、按断线重试，全程不依赖 PowerShell。它自动查找系统已安装的官方 Platform Tools 或发布包根目录的 `platform-tools\adb.exe`；SDK 许可限制 ADB 随包分发。`start-wireview.ps1` 继续保留给开发与命令行使用。 |
| Windows 控制窗口 | P1 基础已完成 | `wireview_control` 可枚举/保存目标，显示 Parsec VDD 状态并安全管理 WireView broker。已有 Parsec 副屏时直接复用并禁止重复创建。运行时通过结构化快照显示连接阶段、目标屏、实际模式、输出 FPS/码率/帧数、重连数、首帧、Surface P50/P95、Android decoder 与实际刷新模式，并给出 60/120 档匹配和解码健康提示；状态区固定高度、自动换行、可纵向滚动且刷新时保留滚动位置，托盘仅保留第一行摘要。真实 Add/Remove 仍待在没有用户副屏的可控环境回归。 |

## 当前项目结构

```text
WireView/
├─ android/                 Android Receiver Gradle 工程
├─ protocol/                与平台无关的 C++ 二进制协议库
├─ windows/host/            Windows Host、编码、传输、输入与控制程序
├─ windows/idd-spike/       冻结的 IddCx 技术验证工程，不是产品路径
├─ windows/common/          IddCx relay 共用声明
├─ scripts/                 PowerShell 构建、安装、测试、启动入口
├─ docs/                    架构、协议与实验说明
├─ build/                   本机生成目录，不应作为源码修改目标
├─ 项目背景.txt             原始项目背景
└─ CODEX_CONTEXT.md         本交接文件
```

### 根目录与构建文件

- `CMakeLists.txt`：顶层 CMake，使用 C++20；Windows 平台加入 `windows/host`。
- `CMakePresets.json`：Windows Debug 构建预设。
- `README.md`：开发与运行手册。部分文字仍保留早期 IddCx 阶段说明，产品路径以 `docs/PARSEC_VDD_ARCHITECTURE.md` 和本文件为准。
- `项目背景.txt`：需求背景；仅作项目背景资料。

### `protocol/`

- `include/wireview/protocol.hpp` / `src/protocol.cpp`：WireView v1 包格式。魔数 `WVR1`、40 字节头、版本、序号、微秒时间戳、CRC32、最大 16 MiB payload、增量 `PacketStreamParser`。
- `include/wireview/input.hpp` / `src/input.cpp`：输入批协议。每条固定 20 字节，包含 action/tool/pointer id/归一化坐标/压力/倾角/buttons/time offset；每批最多 64 条。
- `tests/protocol_tests.cpp`：协议编码、解码、错误路径与流式解析测试。

### `windows/host/`

- `src/main.cpp`：Host CLI。关键命令：`--list-displays`、`--probe-parsec-vdd`、`--stream-desktop`、`--display`、`--parsec-vdd`、`--probe-video`、`--capture-benchmark`、`--encode-benchmark`、`--loopback`。
- `src/desktop_encoder.cpp`：核心采集/光标合成/D3D11 BGRA→NV12/Media Foundation H.264 编码。`DesktopFrameSource` 负责 DXGI Duplication；`CursorCompositor` 用 GPU overlay 合成光标；此文件是当前光标问题的主修改点。
- `src/development_transport.cpp` / `.hpp`：共用 WVR1 Host 会话、握手、视频分块、输入/遥测收发和会话统计；ADB/TCP 监听 `127.0.0.1:39871`，显式 WinUSB 入口复用同一会话，并可通过 `--status-file` 实时发布控制中心快照。
- `src/reliable_byte_stream.cpp` / `.hpp`：Windows 传输无关的可靠双向字节流边界，以及 WVR1 完整写入和增量读包辅助函数；当前 TCP 由 `SocketByteStream` 适配，后续 WinUSB 后端复用同一协议会话。
- `src/winusb_byte_stream.cpp` / `.hpp`：AOA 主接口的 WinUSB bulk 适配器；动态选择唯一 bulk IN/OUT，读写使用独立 overlapped 操作，超时轮询保留未完成读取，析构时取消挂起 I/O。已接入显式 Host CLI、启动脚本和控制中心。
- `src/usb_accessory_discovery.cpp` / `.hpp`：只读枚举 Android WinUSB 接口，区分普通 bootstrap 与 AOA PID，并执行“零个等待、一个打开、多个拒绝”的 accessory 选择状态机；不会发送 AOA 切换请求或修改驱动。
- `src/windows_video.cpp` / `.hpp`：视频输出枚举、显示器目标查找、`DesktopCaptureArea`、硬件编码能力探测等接口。
- `src/input_injector.cpp` / `.hpp`：Windows Synthetic Pointer touch/pen 和鼠标输入注入；按目标显示器矩形变换坐标。
- `src/parsec_vdd.cpp` / `.hpp`：对外部已安装 Parsec VDD 的公开控制接口封装。使用 SetupAPI 找设备接口，`Add/Update/Remove/Version` IOCTL；创建 VDD 时每 100 ms 保活，仅停止自己创建的 driver index。
- `src/annex_b_stream.*`：H.264 Annex B access unit 处理。
- `src/idd_frame_relay.*`、`idd_frame_relay_pipe.*`、`idd_frame_relay_codec.hpp`：冻结 IddCx relay 实验的 Host 侧支持与测试；不作为当前产品数据面。
- `src/control_main.cpp`：Win32 控制窗口与通知区托盘；显示 Parsec 状态，管理现有目标选择，并启动/停止显式 VDD broker。
- `src/vdd_broker_main.cpp`：仅在用户明确创建时运行的 VDD 生命周期守护进程；持有精确 driver index，监听控制中心进程与停止事件，退出前只移除本次创建的显示器。
- `src/usb_accessory_probe.cpp`：P2 Android WinUSB 引导接口枚举、AOA 常量自测、“唯一候选才允许”的显式 AOA 切换、accessory 端点检查和 WVR1 握手探针。已在 HUAWEI DBR-W10 验证到 bulk 写入。
- `tests/idd_frame_relay_tests.cpp`、`tests/idd_frame_relay_pipe_tests.cpp`：relay 测试；`tests/reliable_byte_stream_tests.cpp`：短写、逐字节分片、断流与损坏包测试。
- `CMakeLists.txt`：`wireview_host` 链接 D3D11、DXGI、D3DCompiler、Media Foundation、SetupAPI、User32、Shell 等 Windows 库；MSVC 使用 `/W4 /WX /permissive-`。

### `android/`

- `app/build.gradle.kts`：Android Application；`compileSdk/targetSdk=36`、`minSdk=30`、Java 17。
- `app/src/main/java/com/wireview/receiver/MainActivity.java`：UI、全屏状态、Surface 生命周期、帧率模式、输入事件捕获与 telemetry。
- `ConnectionClient.java`：连接、协议包收发、会话状态机。需接手时先阅读。
- `TransportConnector.java`、`TransportConnection.java`：与 WVR1 解耦的可靠双向字节流边界。
- `TcpTransportConnector.java`：当前 ADB reverse/TCP 开发后端；产品 USB 后端尚未接入。
- `VideoDecoder.java`：MediaCodec 配置、CS​​D/访问单元、Surface 输出、首帧/采样耗时。
- `CodecProbe.java`：选择最合适的 H.264 解码器并报告能力。
- `InputBatch.java`：Android `MotionEvent` 转 WireView 输入批。
- `Protocol.java`：Java 侧 v1 协议实现。
- `protocol-test/.../ProtocolSelfTest.java`：纯 Java 协议测试。
- `protocol-test/.../TransportSelfTest.java`：真实 loopback socket 双向流测试。
- `AndroidManifest.xml`：`MainActivity`、横竖屏适配、网络权限。

### `scripts/`

- `build-windows.ps1`：首选 Windows 构建/CTest 入口。
- `build-android.ps1`、`install-wireview-android.ps1`：Android 构建/安装。
- `start-wireview.ps1`：日常 USB 会话。参数包括 `-EnableInput`、`-DisplayDevice`、`-UseParsecVdd`、分辨率、帧率与时长。
- `test-usb-video.ps1`：合成 H.264 USB 首帧验证。
- `test-usb-desktop.ps1`：真实桌面流、输入、遥测和延迟验证。
- `test-usb-reconnect.ps1`：物理拔插或 `-SimulateAppReconnect` 软中断重连测试；支持固定 `-DisplayDevice`、输出尺寸与帧率，并验证测试前后显示拓扑不变。
- `open-wireview-control.ps1`：启动 Win32 控制窗口。
- `*-idd-spike.ps1`：冻结的 IddCx 实验脚本，勿作为日常副屏路径。

### `docs/`

- `PARSEC_VDD_ARCHITECTURE.md`：当前产品副屏架构与迁移清单。
- `PRODUCT_USB_TRANSPORT.md`：AOA/WinUSB 产品通道边界、风险、迁移顺序与验收条件。
- `INPUT_PROTOCOL.md`：输入协议与注入说明。
- `ARCHITECTURE.md`：总体架构，部分 IddCx 内容是历史/技术验证说明。
- `EXTENDED_DISPLAY_PATH.md`、`PHASE_0.md`：早期自研 IddCx 与阶段记录；需要辨别其“实验”属性。

## 技术方案

### 语言、框架与 API

- Windows：C++20、CMake、MSVC v143、Win32。
- 图形/采集：DXGI 1.2 Desktop Duplication、D3D11、D3D11 Video Processor、D3DCompiler。
- 编码：Windows Media Foundation H.264/AVC 硬件 MFT；参考 GPU 为 AMD Radeon 780M。
- Windows 输入：Synthetic Pointer API、`SendInput` 风格的虚拟桌面鼠标映射。
- VDD：外部 Parsec Virtual Display Driver；SetupAPI 定位其控制接口，公开 IOCTL 控制显示器生命周期。
- Android：Java 17、Android SDK 30+、`MediaCodec`、`SurfaceView`、`MotionEvent`。
- 开发期 USB：ADB reverse + loopback TCP；未来生产传输待替换为 AOA/WinUSB（未实现）。
- 线上协议：自研 C++/Java 对称的 WVR1 二进制协议、CRC32、TCP 流解析。

### 当前核心架构

```text
Windows 显示器 1（真实屏幕）       Windows 显示器 N（Parsec VDD，副屏）
                                               │
                                               ▼
                                  DXGI Desktop Duplication
                                               │ BGRA + 光标 GPU 合成
                                               ▼
                              D3D11 Video Processor：BGRA → NV12
                                               ▼
                           Media Foundation 硬件 H.264 编码器
                                               │ Annex B / WVR1 video_chunk
                                               ▼
                    ADB reverse TCP 127.0.0.1:39871（USB）
                                               ▼
                          Android ConnectionClient / Protocol
                                               ▼
                         MediaCodec 硬件解码 → SurfaceView

Android MotionEvent / 笔 / 鼠标
       │ input_batch（归一化坐标、压力、倾角等）
       ▼
ADB reverse TCP ← Host development_transport
       ▼
WindowsInputInjector（按 VDD DesktopCaptureArea 映射）
       ▼
Windows Synthetic Pointer / virtual-desktop mouse
```

### 典型调用流程

1. 用户启动 Android Receiver，或控制中心的原生 `wireview_session.exe`（命令行场景仍可用 `start-wireview.ps1`）通过 ADB 打开它。
2. 原生会话入口要求唯一已授权 Android 设备，执行 `adb reverse tcp:39871 tcp:39871`。
3. Host 在 loopback 端口监听，完成 `hello` / `hello_ack`，发送 `start_stream`（尺寸、帧率、编码配置）。
4. Host 按 `--display` 查找目标 DXGI 输出；无参数时采集主屏。`--parsec-vdd` 会先创建/保活一块新 VDD 并自动找到其新 `DISPLAYn`。
5. 每个目标帧在 GPU 上合成光标、转换 NV12、提交低延迟硬件 H.264 MFT，封装为 `video_chunk` 发送。
6. Android 解析流、配置 MediaCodec、直接将解码输出 release 到 `Surface`，并回传首帧与采样 telemetry。
7. Android 将触摸/笔/鼠标批回传；Host 依据当前 capture area 变换坐标并可选择真实注入或仅记录统计。

## 重要设计决策

### 采用 Parsec VDD 而非自研显示驱动

- **当前选择**：使用已经安装、已签名的 Parsec VDD 创建真正的 Windows 虚拟显示器，再由 WireView 捕获该显示器。
- **原因**：用户要求保留 Secure Boot，不关闭 Secure Boot、不改 BCD、不降低启动安全性；自签名 IddCx 驱动可被安装进 Driver Store，但在这类生产启动策略下不能可靠加载。
- **边界**：WireView 不安装/签名/分发 Parsec VDD 驱动，不修改驱动安全策略。WireView 仅调用其公开控制接口。已有 Sunshine/Parsec VDD 使用场景时，优先用 `-DisplayDevice` 复用明确选择的现有显示器，避免误删第三方会话。

### 保留但冻结 IddCx 实验

- `windows/idd-spike` 基于微软示例，验证过构建、签名、安装流程和 relay 代码。
- 该路径不进入日常启动或产品发布；仅保留作隔离研究。不能把 `start-idd-spike.ps1` 当作当前产品副屏解决方案。

### 开发期 ADB reverse，产品期再替换传输

- ADB reverse 让 USB 通信、协议、视频、输入与 Android 解码可以先完整验证，且 Android Studio Run 即可安装。
- 产品版 AOA/WinUSB 尚未实现，不能把当前 ADB 依赖视为最终分发形式。

### 安全默认值

- `start-wireview.ps1` 默认安全镜像，只有 `-EnableInput` 才允许平板控制 Windows。
- 输入真实注入测试需在空白/安全窗口进行；`-InputLogOnly` 用于只验证回传。

### VDD 生命周期与显示器选择

- `-UseParsecVdd` 是“创建并管理一块新 VDD”的开发能力；异常终止可能留下未移除显示器。
- 日常已有 VDD 时，推荐 `-DisplayDevice '\\.\DISPLAYn'` 复用目标显示器，不应同时传 `-UseParsecVdd`。
- Windows 显示器编号会因添加/删除 VDD 变动且可能不连续。参考环境当前副屏曾为 `\\.\DISPLAY6`，不能硬编码；每次以 `wireview_host.exe --list-displays` 输出为准。

## 已解决的问题

| 问题 | 解决方式 | 当前状态 |
|---|---|---|
| Android 只显示“首帧/H.264”文字、黑屏 | `SurfaceView.setZOrderOnTop(true)`，并在首帧后隐藏状态文字、进入沉浸式全屏。 | 已验证。 |
| 普通镜像/副屏 H.264 解码启动不稳定 | Host 发送正确 Annex B/CSD 与 stream 配置；Android 选择实际可用 decoder，并将首帧 telemetry 作为验收。 | 已验证。 |
| Android 最终屏幕呈现未达 120 Hz | 保留稳定 60 与实验性 120 主机发送档；控制中心明确标注其含义，并保留设备提交 Surface 指标。 | OPPO 摄影实测约 60～70 fps，作为发布前兼容性项。 |
| VDD 位于主屏旁边时输入落到主屏 | `DesktopCaptureArea` 引入目标显示器 left/top/size，touch/pen/mouse 全部按其虚拟桌面矩形映射。 | 已验证。 |
| `DISPLAY6` 在主屏左侧时触控/笔无响应 | `InjectSyntheticPointerInput` 要求 `ptPixelLocation` 相对虚拟桌面左上角；Host 现在从目标显示器全局坐标减去 `SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN`。API 返回成功但位置为负的假成功已消除。 | 手指与手写笔均已真机确认恢复。 |
| 虚拟副屏普通移动无光标 | 非主显示器启用 `GetCursorInfo` 系统光标回退，GPU 合成到编码纹理。 | 普通箭头、手形、缩放与 I 型均已验证。 |
| 光标先前出现黑方块 | 可复用 256×256 光标纹理上传新形状前清透明区域；避免旧形状残留。 | 已解决。 |
| 手形与缩放光标位置偏移 | 按系统 cursor hotspot 与平板缩放进行校准。用户已确认手形、左右、上下缩放位置正确。 | 已验证。 |
| I 型光标形状与电脑不一致 | 对透明 alpha 的系统 I 型资源使用精确 fallback；最终画面为 14×25，中央竖线 2 px，并按参考光标逐像素构形。 | 用户已真机确认。 |
| 按下拖动瞬间闪断或框选时无光标 | 仅在 `GUI_INMOVESIZE` 明确表示窗口移动/缩放，且收到按下后的 DXGI 更新时，才隐藏 WireView overlay；普通框选与文件拖放保持 overlay。 | 用户已真机确认。 |
| VDD 断开后端口 `10048` | 监听 socket 在 bind 前设置 `SO_REUSEADDR`。 | 已解决。 |
| 启动脚本 Ctrl+C 留下 Host | 不再使用隐藏后台 `Start-Process` 等待；改为当前控制台前台子进程并重定向日志。 | 用户已确认 Ctrl+C 正常结束。 |

## 阶段任务状态

### P0：完成副屏光标体验（已完成）

- 普通箭头最终显示约 21 px；左右/上下缩放光标约 33 px；手形及热点位置均已在参考平板确认。
- I 型文本光标采用独立 fallback，最终输出为精确 14×25 像素、中央竖线 2 像素宽，形状已与参考电脑光标对齐。
- 窗口移动/缩放期间只在 VDD 原生 cursor 路径确认后隐藏 WireView overlay，消除了按下瞬间闪断与双光标。
- 桌面蓝色框选、资源管理器多文件框选和文件拖放不会被误判为窗口移动，平板端继续显示光标。
- 最新 Windows 独立验证构建与 13 项 CTest 通过，以上交互均由用户真机确认。

### P1：VDD 产品化与控制中心（已完成当前参考环境可安全验证的部分）

- 现有显示器列表、选择 `DISPLAYn`、默认复用副屏、分辨率/帧率已加入 `wireview_control`；参考机已验证默认选择现有 `DISPLAY6`，不会自动创建 VDD。
- Parsec VDD 状态与“显式创建/移除 WireView 副屏”已加入控制中心；普通复用路径与创建路径完全分离，打开/刷新控制中心不会创建屏幕。
- 显式创建由 `wireview_vdd_broker` 管理：broker 在创建前绑定控制中心进程与专用停止事件，记录 driver index/`DISPLAYn`，USB/Host 重连继续复用同一目标；控制中心退出或显式移除时按精确 index 清理。
- broker 状态文件自测、UI 非破坏性检查与残留会话扫描已完成。只有唯一记录、owner/broker 都已退出、且记录的 `DISPLAYn` 仍是 Parsec 副屏时，才提供“候选残留”二次确认入口。Parsec 会复用 driver index，程序无法自动证明它仍属于原会话；确认框会要求用户排除 index 已被其他程序复用，不会根据“Parsec”名称批量删除。
- 当前仍未通过 WireView broker 实际执行 Add/Remove。参考机已有用户需保留的 `DISPLAY6`，所以真机生命周期回归延后到没有外部 Parsec 副屏的可控环境。
- VDD 会话与 USB 断线恢复的固定目标基础已完成：重连 Host 传递同一 `DISPLAYn`，每轮重新验证目标，消失则硬失败。`DISPLAY6` 软中断和真实物理 USB 拔插真机回归均已通过；物理回归中两次握手都复用 DISPLAY6，拓扑不变且未创建 VDD。
- 将 README 与旧 Phase 文档中的早期 IddCx 表述整理为明确的“历史实验”，避免新开发者误走旧路径。

### P1：稳定性、性能与 UX

- VDD 独立桌面的固定目标、物理 USB 拔插和原生 2560×1600@120 的 30 分钟**主机/解码提交链路**稳定性回归已完成；输入功能此前已有单独真机验证。该长测不等同于设备面板真实 120 Hz。
- 控制中心遥测摘要已完成：显示状态、实际模式、输出 FPS/码率/帧数、重连、首帧、Surface P50/P95，以及每秒更新的 Android 解码输入/丢帧/错误健康计数。
- Android decoder 与刷新模式兼容性提示已完成：首帧后回传硬件/软件实现、标准低延迟标记、当前/最高刷新率；控制中心区分数据链路 120 FPS 与面板实际 120 Hz。
- **120 Hz 最终呈现验证（2026-08-26，进行中）**：OPPO PKB110（2374×1080、Android 16）本地 120 格视频可通过 1/10 秒曝光拍出约 12 格，证明屏幕与测量方法正常；WireView 在原生 2374×1080 和等比降档 1920×874 下均只能拍出约 6～7 格。实验期间主机发送稳定约 120 FPS，设备提交 Surface 也稳定 110+ FPS，故瓶颈不在 Windows 编码、USB 发送或 MediaCodec 输出释放，而在 OPPO 最终 SurfaceFlinger/显示合成路径。已尝试在 Surface 创建/尺寸变化后重设 120 Hz `Surface.setFrameRate`，无明显提升；尝试改用 TextureView 后修复了其背景色启动崩溃，但真机仅约 50～60 FPS。用户决定回退到测量前已验证稳定的 `SurfaceView.setZOrderOnTop(true)` 与原控制中心状态摘要；以上两项专用指标也随回退移除。该记录保留用于避免再次将发送或解码帧率误判为面板真实帧率。
- 优化光标：等待、禁止、文本编辑插入点闪烁等系统 cursor 的覆盖范围待确认。

### P2：产品传输与发布

- **当前正在执行。** ADB 仅为开发通道；实现 AOA/WinUSB 产品级 USB 传输、设备发现、权限与安装体验。
- Android 已完成第一层传输解耦：`ConnectionClient` 不再直接持有 `Socket`，默认 `TcpTransportConnector` 保持现有行为；accessory manifest/filter、attach intent 和 `UsbAccessoryTransportConnector` 已加入。协议与真实 loopback 双向传输自测、Debug APK 构建均通过。
- Windows 已加入 `wireview_usb_probe`：默认只读枚举 Android 通用 WinUSB 引导接口并自测 AOA 常量；显式命令可切换唯一候选、检查 accessory 端点并发送 WVR1 握手。HUAWEI DBR-W10 已验证切换、枚举、端点查询和 bulk 写入。
- 参考平板的 ADB function 当前确实绑定 `Winusb.sys`，可作为原型引导入口，但它依赖 USB 调试。无开发者模式进入 AOA、AOA 重枚举后的驱动绑定与最小签名 INF/filter 是 P2 明确决策门，不能作为隐藏产品前置条件。
- HUAWEI DBR-W10 与 OPPO PKB110 的 AOA 真机验证均完成到 Windows bulk 数据面：协议版本 2，重枚举为 `VID_18D1&PID_2D01`，interface 0 的 bulk IN/OUT 为 `0x81/0x01`、512 字节。两机持久日志都确认 Android 收到 attach、权限为 true、`openAccessory()` 成功并阻塞读取，但收不到 Host 的 WVR1 HELLO；正式 Host 的 overlapped `WinUsbByteStream` 对照也无任何返回字节。共同点是现有 `oem97.inf` 把 `PID_2D01` 整设备绑定为 `Motorola Composite AUTO Interface`。该驱动绑定/`PID_2D01` 组合路径作为兼容性阻塞延期，不影响默认 ADB/TCP 与传输抽象代码。
- Windows Host 可靠字节流抽象已完成：WVR1 会话不再直接依赖 `SOCKET`，TCP 通过 `SocketByteStream` 接入；短写、逐字节分片、断流和损坏包测试通过。完整 Windows 构建为 9/9 CTest，通过后的固定 `DISPLAY6` 8 秒 ADB 真机回归为 481/481 帧、60.04 FPS、24.05 Mbps、Surface P50/P95 16.22/17.51 ms，零解码丢帧和错误。
- WinUSB `ReliableByteStream` 后端已完成底层实现并编入 Host：动态验证唯一 bulk IN/OUT，使用相互独立的 overlapped 读写，支持保留式 `wait_readable` 轮询和关闭时取消挂起 I/O。无设备端点测试、Windows 10/10 CTest、Android 协议/传输测试和 Debug APK 构建通过；默认运行入口仍只使用 ADB/TCP。
- Host 已增加显式 `--transport winusb` 桌面会话入口：只读等待已进入 AOA 的唯一 accessory，不自动切换 Android USB 模式或修改驱动；零个等待、多个拒绝，WVR1 握手每段最多等待 10 秒。默认或 `--transport adb` 仍走原路径。Windows 12/12 CTest、Android 构建通过；共用会话抽取后的固定 `DISPLAY6` 8 秒 ADB 回归为 481/481 帧、60.04 FPS、24.04 Mbps、平均编码 7.38 ms，零解码丢帧/错误，拓扑仍为 `DISPLAY1 + DISPLAY6`。
- 启动脚本和控制中心已贯通 transport 选择：默认 `Adb`，WinUSB 不要求 ADB、不启动 Activity、不自动切换 USB 模式；Host 断线退出后，脚本重新验证同一 `DISPLAYn`、重新等待唯一 accessory 并累计重连。控制中心保存选择、运行时锁定、状态摘要显示实际通道；120 FPS 组合会读取所选 Windows 显示器的实际刷新率和系统交流电源状态，副屏低于 120 Hz、电脑使用电池或两者同时发生时显示警告但不自动改系统设置。WinUSB 标为“实验，当前阻塞”，组合说明与启动确认框明确指出华为/OPPO 的 `PID_2D01` 驱动绑定均无法完成 WVR1 握手，日常继续使用 ADB。旧配置/状态缺少字段时回退 ADB。加入提示文本自测后的独立目录构建 13/13 CTest 通过；默认 ADB 的固定 `DISPLAY6` 8 秒回归为 481/481 帧、60.05 FPS、24.05 Mbps、平均编码 7.37 ms，零解码丢帧/错误，拓扑不变。
- Android 已加入 AOA 专用持久诊断链路：应用私有目录中的当前/上一份日志各限 128 KiB，覆盖 attach、权限、`openAccessory`、首包、`HELLO_ACK`、异常和关闭，普通 ADB 重试状态不落盘。物理重插恢复 ADB 后可用 `scripts/get-wireview-android-diagnostics.ps1` 提取到 `build/android-diagnostics/`。协议、传输和持久日志三组 Java 自测、Debug APK 构建与固定 `DISPLAY6` 的 8 秒 ADB 回归均通过；回归为 481/481 帧、60.05 FPS、24.04 Mbps、平均编码 7.27 ms，零解码丢帧/错误。
- AOA 诊断复测同时修复了 Android detach 生命周期：轮询不再重复消费 Activity 保存的旧 `USB_ACCESSORY_ATTACHED` Intent；detach 后忽略平台短暂残留的 accessory 枚举和迟到权限结果，直到新的 attach 事件，避免每秒重试失效的 `openAccessory()` 并刷掉证据。修复版 Debug APK 构建通过，固定 `DISPLAY6` 的 8 秒 ADB 回归为 481/481 帧、60.04 FPS、24.08 Mbps、平均编码 7.26 ms，零解码丢帧/错误。参考机的 O+Connect 会以 Session 0 ADB 抢占 bootstrap；测试中仅临时停止，结束后已恢复自动运行。
- OPPO PKB110 的默认 ADB/TCP 基线通过：固定 `DISPLAY6`、2560x1600@60、481/481 帧、60.04 FPS、24.08 Mbps、首帧 15 ms、Surface P50/P95 20.52/21.72 ms、零解码丢帧/错误，使用 `c2.mtk.avc.decoder.lowlatency`。`test-usb-desktop.ps1` 已兼容 Android 16/OPlus 的 `topResumedActivity` dumpsys 格式，同时保留旧版 `mResumedActivity`。
- OPPO 的物理 detach 真机日志确认旧 Intent 修复生效：关闭 accessory 后枚举降为 0，没有重新认领或重复 `openAccessory()`。O+Connect 与 GlideX 的五项服务在测试期间仅临时停止；曾临时禁用的 O+Connect 两项服务已恢复为 Automatic，最终五项均为 Running/Automatic。
- 产品级签名、安装器、第三方 VDD 分发许可与升级策略待确认。
- 尚不支持 HDR、4K、多虚拟显示器、音频、Wi‑Fi/远程访问、DRM 内容和 AV1 常规路径。

## 下一步开发计划

按优先级执行：

1. **已完成——控制中心显示器选择**：枚举当前附加显示器，允许用户明确选择目标 `DISPLAYn`，默认复用已有副屏，不自动创建新 VDD；选择随分辨率/帧率持久化。
2. **已完成代码、待可控真机执行——安全创建与会话归属**：只有当没有现有 Parsec 副屏且用户点击“创建 WireView 副屏”时才调用 VDD；独立 broker 记录驱动索引、显示器映射和控制中心归属，正常退出/控制中心异常退出时只清理由本次 broker 创建的显示器。参考机尚未通过 WireView broker 执行 Add/Remove 回归。
3. **已完成——残留会话恢复 UI**：启动和刷新时扫描 `vdd-session-*.ini`；多记录、存活进程或 `DISPLAYn` 不再匹配都禁止清理。唯一候选项只提供人工二次确认，并明示 driver index 可能已被复用的风险。
4. **已完成——VDD 与断线恢复联动**：Host 软中断和真实 USB 拔插后都保持同一显式 `DISPLAY6`，目标消失不回退；两次握手、两次采集与拓扑不变已真机通过。
5. **已完成——控制中心遥测摘要**：Host、原生会话入口和命令行 PowerShell 入口共用原子结构化快照，控制中心直接展示连接、视频、输出和 Surface 延迟摘要；新旧状态格式均有自测。
6. **已完成——Android 解码与刷新率能力提示**：首帧后回传实际 decoder、实现类型、低延迟标记、平板当前/最高刷新率；控制中心据此给出 60/120 **主机发送条件**匹配提示。该提示不能证明实际面板帧率；120 仍须按当前交接快照中的摄影方法验收。
7. **已完成——实时解码丢帧与会话健康提示**：Android 每秒回传收到/排队/输入丢帧/解码错误累计值，首个异常立即上报；控制中心显示启动中、正常、警告或异常。60 FPS 短测为 430/430，120 主机目标短测为 498/498，均零丢帧、零错误；这些是解码输入健康数据，非面板帧率。
8. **已完成——VDD 物理拔插与原生 120 主机链路长时回归**：真实 USB 拔出/插回已通过，两次握手均固定 DISPLAY6、拓扑不变；原生 2560×1600@120 的 30 分钟长测为 216000/216000 Host 发送帧、120.00 FPS、48.01 Mbps，Host 编码平均/最大 6.97/26.55 ms，Surface P50/P95 17.37/32.17 ms，零解码输入丢帧、零错误、零重连。它不证明设备最终面板 120 Hz。
9. **兼容性阻塞、暂时延期——产品 USB 通道与发布能力**：Android 传输抽象与 `UsbAccessory` 打开层、Windows AOA 切换/端点检查、Host 可靠字节流、WinUSB overlapped bulk 后端、显式会话入口、控制中心选择、重连框架和持久诊断均已完成。华为与 OPPO 两台设备都在 `PID_2D01` + `Motorola Composite AUTO Interface` 下表现为 `openAccessory()` 成功但收不到 Host bulk OUT；正式 Host 对照同样失败。当前不修改/卸载 USB 驱动，保留 WinUSB 实验入口，日常继续使用已验证稳定的 ADB/TCP。将来恢复此项时先在隔离环境验证最小 MI_00 WinUSB 绑定或 accessory-only `PID_2D00`，再做 60 FPS/输入/拔插。
10. **新增兼容性问题、待专项验证——Android 最终 120 Hz 呈现**：不要再把 Host `output_fps` 或 MediaCodec `releaseOutputBuffer` 速率当作面板实际帧率。后续需在 OPPO 的 SurfaceFlinger/显示模式策略上继续定位，或为设备提供可验证的显示层实现；验收继续使用本地 120 格基准视频 + 1/10 秒曝光。发布前应将“真实 120 Hz 最终呈现”标为未完成，不宣传为通用能力。

## 给下一次 Codex Task 的说明

### 接手前先做的检查

1. 阅读本文件，再阅读 `docs/PARSEC_VDD_ARCHITECTURE.md`、`windows/host/src/desktop_encoder.cpp`、`scripts/start-wireview.ps1`。
2. 不要把 `windows/idd-spike` 当作要继续实现的产品功能；它是冻结实验。
3. 先执行 `scripts/build-windows.ps1`，确保 13 项 CTest 都通过。MSVC 为 `/WX`，警告会导致构建失败。如果 `wireview_control.exe` 正在运行，链接会因文件锁失败；先从托盘彻底退出控制中心。
4. 不要猜测 VDD 的 Windows 编号。先运行：

   ```powershell
   .\build\windows-nmake\windows\host\wireview_host.exe --list-displays
   ```

   然后确认目标是已附加的 Parsec 副屏。参考环境曾使用 `\\.\DISPLAY6`，该值不是稳定配置。
5. 日常复用已有副屏的启动形式是：

   ```powershell
   .\scripts\start-wireview.ps1 -DisplayDevice '\\.\DISPLAYn' -EnableInput
   ```

   不要同时加 `-UseParsecVdd`。后者会新增一块 VDD，仅适合明确需要由 WireView 创建/管理的测试会话。

### 修改时的注意事项

- **光标代码只在 `desktop_encoder.cpp` 内迭代**。先分清 DXGI 光标、Windows 系统回退光标和 VDD 已绘制到桌面帧的原生光标；三者混合容易造成双光标、闪断或坐标偏移。
- 对光标纹理每次换形状必须清空完整 reusable 256×256 纹理；否则会重新出现黑方块。
- 光标坐标以目标 DXGI 输出的 local 坐标为准；输入先映射到 `DesktopCaptureArea` 的全局屏幕矩形。Synthetic Touch/Pen 还必须减去虚拟桌面原点，转换为 `InjectSyntheticPointerInput` 规定的相对虚拟桌面坐标；鼠标则继续使用 `MOUSEEVENTF_VIRTUALDESK` 绝对轴。三种坐标空间不要混用。
- 不要为了临时测试关闭 Secure Boot、改 BCD、暂停 BitLocker，或把测试签名驱动作为产品方案。
- 不要删除已有 Parsec VDD 来“清理”会话，除非能够可靠证明它由当前 WireView 会话创建；用户可能同时使用 Sunshine。
- `start-wireview.ps1` 当前使用前台子进程来保证 Ctrl+C 不残留 Host。不要随意恢复为隐藏 `Start-Process`，除非同时实现可靠的进程组/退出清理。
- 开发日志位于 `%TEMP%\WireView\mirror-*.out.log` 与 `.err.log`。当前 I 型诊断行形如：`System cursor captured: size=... | hotspot=... | monochrome=... | opaquePixels=...`。
- 输入注入具备实际控制 Windows 的能力。默认保持安全模式，执行/演示时只在用户明确要求下使用 `-EnableInput`。
- 用户更偏好中文沟通；任何对用户的回复应使用中文。
