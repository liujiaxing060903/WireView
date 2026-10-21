# 阶段 0：技术验证

## 最小纵向切片

在一台参考 Windows 电脑与一台 Android 11+ 真机上实现：

1. Windows 创建一个 1920x1080@60Hz SDR 扩展显示器。
2. IddCx 收到桌面 DirectX 表面并记录帧时间戳。
3. 显卡把表面编码为低延迟 H.264。
4. 压缩帧先通过 ADB USB 通道发送。
5. Android 使用 MediaCodec 解码并直接呈现到 SurfaceView。
6. 两端输出可关联的逐帧延迟记录。

## 验收门槛

- 连续显示 30 分钟，无崩溃或持续增加的队列。
- 1920x1080 模式平均不低于 59 FPS。
- 任意时刻待发送视频不超过两帧。
- 端到端画面延迟 P50 不高于 35ms，P95 不高于 50ms。
- USB 拔插后能够重建会话，不要求重启 Windows。
- 能分别看到获取、转换、编码、传输、解码和呈现耗时。

## Go/No-Go 风险验证

### Windows 显示与编码

- 官方 IddCx 示例能否使用本机 WDK 编译、测试签名并加载。
- Intel、NVIDIA 或 AMD 当前参考显卡是否提供稳定的低延迟 H.264 MFT。
- BGRA 到 NV12 的 GPU 转换和编码是否能稳定控制在一帧预算内。

### Android 解码

- 参考设备是否提供硬件 AVC 解码器。
- KEY_LOW_LATENCY 是否实际生效。
- 60Hz 呈现是否出现额外两帧以上缓冲。

### USB

- ADB USB 通道先证明数据路径。
- AOA 在至少三台不同品牌设备上验证权限、持续吞吐、抖动和重连。
- 不支持 AOA 的设备将来使用 USB 网络共享作为备用通道。

## 任务顺序

1. [x] 固化协议包头、CRC、序号和单调时间戳，并通过增量分片测试。
2. [x] 通过项目本地 WDK NuGet 包建立驱动编译链；Android SDK 已确认可用。
3. [x] 编译并链接微软官方 IddCx 示例 DLL 和软件设备启动器。
4. [x] 建立 Android 接收应用、SurfaceView 与 ADB 反向端口握手骨架。
5. [ ] 生成可安装的测试签名驱动包，并在 Windows 中创建测试显示器（包已签名并暂存；当前参考机的 Secure Boot 生产策略阻止自签名驱动进入 DriverEntry）。
6. [x] 接入 Android 真机，枚举硬件 AVC 解码器并验证 MediaCodec Surface 解码。
7. [x] 使用合成 H.264 码流打通 ADB USB，并确认首帧提交到 Surface。
8. [ ] 将 IddCx 表面接入硬件编码器，完成纵向切片。
9. [x] 在保持 Secure Boot 的参考机上，以物理桌面镜像替代 IddCx 输入，先打通 DXGI GPU 采集、NV12 转换、AMD H.264 硬编码、ADB USB 与 Android Surface 的完整数据面。

## 当前验证结果（2026-08-23）

- C++ 协议与 TCP 回环测试通过。
- Java 协议自测与真实 loopback 双向传输自测通过。
- Android Debug APK 可离线构建。
- IddCx/UMDF 源码已使用微软签名的 WDK 26100.6584 NuGet 包编译并链接。
- IddCx 测试 INF 已改为 WireView 专用设备 ID、独立 UMDF 进程组和 DIRID 13 隔离布局；`InfVerif` 与 `Inf2Cat` 均为零错误、零警告，并生成未签名 Catalog。
- IddCx 技术验证驱动现在只枚举一台有线间接显示器，首选 1920x1080@60，并使用固定容器 ID 保留 Windows 显示布局。
- WireView 0.1.0.1 Catalog 已由本机开发证书签名并作为 `oem134.inf` 暂存；设备能够匹配 INF、WUDFRd、IndirectKmd 与独立 UMDF 组。
- 参考机保持 Secure Boot 开启且未修改 BCD。SetupAPI 的内核信任验证返回 `0x800b0109`，设备启动随后返回代码 31 / `CM_PROB_FAILED_ADD`（`0xC0000001`），驱动入口诊断未执行；因此当前自签名包只能暂存，不能在该安全策略下加载。
- 下一次真机 IddCx 加载验证需要二选一：使用隔离的驱动测试环境并显式启用测试签名，或取得微软签名的预生产/发布驱动；不得在用户不知情时降低主力电脑启动安全。
- OPPO PKB110（Android 16、MT6991）真机握手通过；选中的 `c2.mtk.avc.decoder.lowlatency` 支持低延迟特性与 1080p60。
- HUAWEI DBR-W10（Android 12、SM8250）真机握手通过；`OMX.qcom.video.decoder.avc` 虽为硬解，但测试首帧约 2484ms；`c2.qti.avc.decoder` 不强制低延迟参数时首帧约 17–25ms，因此默认选择硬件 Codec2 路径。
- HUAWEI DBR-W10 上强制 `KEY_LOW_LATENCY=1` 的对照测试首帧约 237ms，反而慢于默认配置；应用保留诊断开关，但不对该设备强制设置。
- Android/Chromium 官方 320x240@25 H.264 测试流通过 ADB 发送 250 帧，MediaCodec 报告首帧已提交到 Surface。
- Android 应用的空闲连接提示已去抖；Windows Host 未运行时保持稳定等待，不再按重试周期闪烁“未就绪”。
- AMD Radeon 780M 的 Desktop Duplication 在 2560x1600 桌面上连续 8 秒取得约 116.93 FPS，937 次采集、前置丢帧 0；GPU 表面格式为 BGRA，未回读 CPU 像素。
- `AMDh264Encoder` 支持 D3D11、异步 MFT、低延迟、LowDelayVBR、码率、GOP、零 B 帧与单参考帧控制。2560x1600@60 的本机编码 481/481 帧成功，平均提交到输出约 8.69ms，首帧约 20.76ms，输出全部为 Annex-B。
- HUAWEI DBR-W10 的真实桌面 USB 流验证持续 15 秒：901/901 帧编码，约 24.20Mbps；`c2.qti.avc.decoder` 首帧 15ms，解码输入丢帧 0，输出格式确认 2560x1600@60。
- 真实桌面测试脚本会等待 Android Activity 与 SurfaceView 就绪后再启动 Host，避免重启应用时旧连接被销毁造成的 `WSAECONNABORTED 10053` 假故障。
- 输入回传协议已固定为版本化的 20 字节记录，包含指针动作、工具类型、归一化坐标、压力、二维倾角、按键和事件时间；C++ 往返/截断/未知记录测试通过。
- Android 已在视频 Surface 上采集手指、鼠标、压感笔和橡皮事件，并通过独立写线程回传；Windows Host 在视频发送线程之外并行接收，使用系统合成触控/笔接口。更新 APK 后的 5 秒双向回归仍为 2560x1600@60、301/301 帧、Android 视频丢帧 0；未触摸时输入事件数为 0。
- 45 秒真实桌面 USB 安全回归通过：2701/2701 帧编码、60.00 FPS、约 24.05Mbps；平板通过 USB 主动回报首帧呈现 22ms。期间触摸/拖动产生 962 条回传记录，零协议拒绝，日志模式确认未注入任何 Windows 输入；Android 仅记录 1 次非持续性的输入缓冲不足。
- 首帧验收已从不稳定的 ADB logcat 抓取改为应用主动发送的版本化遥测包；Windows 本机回环自动测试改用专用端口 39872，避免连接中的平板抢占 39871 开发端口。
- 30 分钟原生 2560x1600@60 USB 稳定性回归通过：108001/108001 帧编码、持续 60.00 FPS、24.00Mbps；Host 平均编码延迟 7.12ms、最大 26.94ms，USB 会话无中断。Android 仅在结束前记录 2 次孤立输入缓冲不足，未发生崩溃、持续掉帧或队列增长，满足阶段 0 的连续显示稳定性门槛。
- USB 拔插恢复验证通过：拔线导致首个会话以 `send 10053` 中断后，Windows Host 保持监听；重新插入、恢复 ADB 反向端口后收到第二次 Android 握手并自动继续传输。恢复后的 55 秒为 3301/3301 帧、60.01 FPS、平均编码延迟 6.96ms，Android 解码零丢帧。
- 单指触控与单支压感笔的真实 Windows 注入已在空白记事本与 OneNote 中验证，笔迹与压力粗细均正常；多点触控已通过 Windows 侧活动触点聚合实现并在真机上验证至四指。
- Parsec `DISPLAY6` 位于主屏左侧（全局 `x=-2560..-1`）时，Synthetic Touch/Pen 曾因误用负数全局坐标而出现 API 返回成功但界面无响应；现已按 Windows 规定减去虚拟桌面原点再填写 `ptPixelLocation`。控制中心 `-EnableInput + DISPLAY6` 路径下手指与手写笔均由用户真机确认恢复。
- 延迟遥测现已完成双向时钟校准：Host 在每次会话开始时发送 8 个 ping，随后每 5 秒刷新一次；平板按秒回报“视频包到达至提交 Surface”的时间，Host 为每个帧样本选择附近时间段内往返最短的校准值，再输出本地指标和完整端到端 P50/P95。
- 自适应输出回收后的历史 60 秒原生 2560x1600@60 回归：编码平均 7.17ms，平板到 Surface P50 15.99ms、P95 25.01ms；当时以编码器的名义媒体 PTS 统计完整端到端 P50 29.36ms、P95 43.78ms。该统计已由后续的真实采集时间戳方案替代，不能再作为阶段 0 的绝对延迟验收结论。
- 已排除的解码调优：`KEY_PRIORITY=0`（实时优先级）在该平板恶化为端到端 P50 53.39ms、P95 96.46ms，故不启用；1920x1200@60 对照也未改善 P95（81.60ms，且出现 2 次孤立输入缓冲不足），默认保留原生 2560x1600。该设备的 `onFrameRendered` 回调使用媒体 PTS 时间轴而非 Android 单调时钟，不能用于跨设备延迟计算，未纳入统计。
- 解码输出回收优化：原实现主要在下一帧输入到达时轮询输出，现为有待输出帧时每 2ms 在同一解码线程补充检查、无待处理帧即停止；调度线程只投递任务，不并发调用 `MediaCodec`。会话代号隔离旧流的延迟任务。零解码输入丢帧已反复验证；绝对端到端值以真实采集时间戳方案为准。
- 已排除的输出回收变体：在输入线程中阻塞等待 4ms/8ms 会增加队列竞争；固定 2/4/6ms 与覆盖至 14ms 的高频补充轮询均不如自适应轮询。`KEY_OPERATING_RATE=120` 也没有带来可重复收益，均不保留。
- 多点触控真机验证：Android 协议已有 `pointerId`；Windows Host 现将每批手指事件聚合为完整活动触点集，并维护主触点、抬起与取消清理。20 秒真实测试收到 1221 个输入包、2420 条手指记录，全部注入成功（平均 95µs、最大 3093µs），用户确认连续拖拽与四指同时触控均可识别。高动态触控会使镜像源的桌面更新率下降，属于独立的动态帧率优化项，不影响多点输入正确性。
- 日常启动入口：`scripts/start-wireview.ps1` 默认安全镜像、持续 8 小时，可自动等待唯一授权 USB 设备、恢复 ADB 反向端口并唤起平板应用；连接中断后继续等待并自动续传。显式 `-EnableInput` 才允许平板控制 Windows，`Ctrl+C` 可随时停止。Host 作为当前控制台进程组的前台子进程运行并将完整输出写入临时日志，避免 `Ctrl+C` 后残留编码进程。
- Android 安装入口：新增 `scripts/install-wireview-android.ps1`，可在一台已授权设备上构建、覆盖安装并自动打开 Debug APK；`-SkipBuild` 用于已构建 APK 的快速重装。
- 新版组合 30 分钟安全稳定性回归通过：108001/108001 帧编码、持续 60.00 FPS、24.00Mbps，Host 平均编码 6.51ms、最大 15.77ms；Android 解码输入丢帧 0、会话无中断。1800 个平板到 Surface 样本为 P50 17.59ms、P95 27.56ms。原先仅在开场校准时测得的完整端到端 P50/P95 为 46.79/57.78ms，高于短测；现已改为会话内持续校准与真实采集时间戳。
- Windows 最小控制窗口：`wireview_control.exe` 与 `scripts/open-wireview-control.ps1` 提供安全镜像、启用输入、停止会话和 VDD 管理；窗口以 Job Object 托管入口脚本及其 Host 子进程。控制中心会枚举已附加显示器，无历史选择时优先复用现有副屏，并把精确 `DISPLAYn` 传给启动脚本；打开窗口本身不会创建 VDD。Host 和入口脚本共用原子结构化快照，窗口实时显示连接阶段、目标屏、实际模式、输出 FPS/码率/帧数、重连、首帧、Surface P50/P95、Android decoder、当前/最高刷新率，以及解码输入/丢帧/错误健康状态；控制中心会区分 120 FPS 数据链路与面板实际 120 Hz，托盘只显示第一行摘要。
- 固定 `DISPLAY6` 的真实物理 USB 拔插回归已通过：Host 检测到一次 `10053` 中断，重新插入后 ADB reverse 自动恢复并完成第二次握手；两次会话都采集 DISPLAY6，测试前后保持 DISPLAY1 + DISPLAY6，未进入 VDD 创建路径。重连后的完整 8 秒会话为 60.05 FPS、解码输入丢帧 0、错误 0。
- 持续校准 30 秒真机只读回归通过：1801/1801 帧、60.00 FPS、平均 Host 编码 6.98ms、Android 解码输入丢帧 0；30 个平板到 Surface 样本 P50 15.75ms、P95 20.47ms。获得 14 个会话内校准样本，最佳往返 1.07ms；当时以名义 PTS 算出的完整端到端 P50/P95 为 30.57/47.66ms，仅保留为历史对照，不再作为验收依据。
- 连续校准后的 30 分钟只读回归再次确认画面稳定：108001/108001 帧、60.00 FPS、平均 Host 编码 7.80ms、Android 解码输入丢帧 0、无会话中断；平板到 Surface P50 17.90ms、P95 37.01ms，获得 367 个校准样本。它暴露出旧方案将编码器名义 PTS 当作真实采集时间会在长时段产生错误的端到端 P95（约 60 秒），故该绝对值已废弃。
- 当前视频包使用编码器的匀速 PTS，以保证 MediaCodec 解码时间线稳定；因此跨设备的绝对 `End-to-end frame latency` 仍受时间戳语义影响，仅作诊断参考。阶段 0 的延迟验收暂以 Host 编码耗时、平板到 Surface 的本地耗时、解码输入丢帧和长期稳定性为准。
- 已排除的解码线程优先级尝试：将解码和输出回收线程提升为 Android `THREAD_PRIORITY_DISPLAY` 后，60 秒真实端到端为 P50 46.97ms、P95 64.40ms，无改善，已撤回且未保留在 APK 中。
- 彩色桌面真机视觉验证确认，先前全黑画面来自 Windows 桌面背景本身，并非解码或呈现失败。当前应用在首帧后隐藏状态文字与系统栏，视频 Surface 全屏显示；平板可见真实 Windows 画面。
- Windows Desktop Duplication 已补齐硬件鼠标指针：Host 将指针形状 GPU 合成到桌面帧中，避免硬件光标在普通移动时缺失。普通箭头约 21 px、左右/上下调整大小约 33 px，手形与热点位置均已真机确认；I 型资源 alpha 不可用时采用独立 fallback，最终为精确 14×25、中央竖线 2 px。仅窗口移动/缩放期间与 VDD 原生光标交接，桌面蓝框与资源管理器多文件框选仍显示鼠标。P0 光标体验已由用户完整确认。
- 120 FPS 性能档已贯通主机帧率、AMD H.264 编码、协议流配置、Android MediaCodec 和 Android 屏幕刷新请求。HUAWEI DBR-W10 曾在系统默认刷新率策略下保持 60 Hz，能力提示正确识别；调整设置后完成原生 2560x1600@120 的 30 分钟安全长测：216000/216000 帧、持续 120.00 FPS、48.01Mbps，Host 编码平均 6.97ms、最大 26.55ms；面板实际 120 Hz，Android 解码输入丢帧 0、错误 0、会话零重连，3600 个平板到 Surface 样本 P50 17.37ms、P95 32.17ms。解码器为 `c2.qti.avc.decoder`（硬件）。1920x1200@120 仍可作为降低主机与 USB 负载的高刷档。
- Windows 控制中心现提供启动前的分辨率和帧率选择：分辨率可用“原生自动”、预设或输入任意正偶数 `宽x高`，帧率可选 60/120；会话运行期间锁定选择，停止后恢复可编辑。命令行入口保留等价的 `-StreamWidth`、`-StreamHeight` 与 `-StreamFrameRate` 参数。
- 控制中心会在用户启动会话时将所选目标显示器、分辨率与帧率保存到当前 Windows 用户的 `%LOCALAPPDATA%\WireView\control-settings.ini`，下次打开时自动恢复；若原显示器不存在则回退到当前现有副屏。配置仅影响 WireView 启动参数，不修改 Windows 显示设置。
- 新增启动前设备预览：Windows 在 ADB 握手时取得 Android 当前方向的物理尺寸、当前/最高刷新率，并发送一个只显示白色边框的预览布局。它不创建 VDD、不启动捕获/编码；白框表示所选内容的等比区域，框外固定为黑边。正式 `SurfaceView` 同样按 `fit-center` 居中布局，避免手机与平板的不同长宽比被拉伸。控制中心另提供 5 个 `宽x高@Hz` 自定义模式槽，保存至 `%LOCALAPPDATA%\WireView\custom-modes.ini` 并加入输出分辨率选择；当前不会自动写 Parsec VDD 注册表或重连用户既有副屏。
- 控制中心会按当前组合显示不具强制性的运行建议，明确区分原生 60 的稳定档、1920x1200@120 的参考满帧档，以及原生 120 的性能边界，避免日常选择时误把“可运行”当成“必然满帧”。120 FPS 还会读取所选 Windows 显示器的当前刷新率与交流电源状态；目标低于 120 Hz、电脑使用电池或两者同时发生时显示明确警告，但不擅自修改用户的显示或电源设置。
- P2 产品 USB 已启动：Android `ConnectionClient` 已从直接依赖 `Socket` 改为可替换的可靠双向字节流，现有 ADB/TCP 后端行为不变；纯 Java 双向 loopback 自测和 Debug APK 构建通过。AOA/WinUSB 的端点、权限、驱动绑定、重连与迁移顺序见 `docs/PRODUCT_USB_TRANSPORT.md`。
- HUAWEI DBR-W10 首次 AOA 真机验证确认 Windows 协议版本 2、`VID_18D1&PID_2D01` WinUSB 绑定和 `0x81/0x01` bulk 端点均正常；Android 系统识别 WireView 并由用户确认配件权限，但应用尚未返回 WVR1 ACK。该华为兼容性项延期，不影响现有 ADB/TCP 路径和 `DISPLAY6`。
- Windows Host 的 WVR1 会话已迁移到 `ReliableByteStream`，TCP 由适配器接入；短写、逐字节分片、断流和损坏包测试均通过。Windows 完整构建为 9/9 CTest；迁移后的固定 `DISPLAY6` 8 秒 ADB 真机回归为 481/481 帧、60.04 FPS、零解码丢帧和错误。下一步是接入 WinUSB bulk I/O 后端。
- WinUSB bulk 后端底层已完成：`WinUsbByteStream` 动态验证唯一 bulk IN/OUT，读写使用独立 overlapped 操作，等待超时不会丢弃挂起读取，关闭时取消 I/O。适配器已编入 Host但尚无运行入口；Windows 当前 10/10 CTest、Android 协议/传输测试与 Debug APK 构建通过，默认 ADB/TCP 行为未改变。
- Host 已加入显式 `--transport winusb` 桌面会话入口和只读 accessory 等待状态机，不自动发送 AOA 切换请求或修改驱动；零个设备等待、一个打开、多个拒绝，握手读取有 10 秒超时。Windows 当前 12/12 CTest；共用会话抽取后的固定 `DISPLAY6` 8 秒 ADB 对照回归为 481/481 帧、60.04 FPS、24.04 Mbps、平均编码 7.38 ms，零解码丢帧/错误，测试后仍为 `DISPLAY1 + DISPLAY6`。
- 启动脚本和控制中心已加入 transport 选择：默认 ADB；WinUSB 模式不要求 ADB、不启动 Activity、不自动切换 USB 模式。断线后脚本重新验证同一 `DISPLAYn` 并重新等待 accessory；控制中心保存/锁定选择、显示实际通道并在实验入口启动前警告，旧配置无字段时回退 ADB。正式目录构建 13/13 CTest；默认 ADB 的固定 `DISPLAY6` 8 秒回归为 481/481 帧、60.05 FPS、24.05 Mbps、平均编码 7.37 ms，零解码丢帧/错误，拓扑不变。新版控制中心已重新启动，等待界面目视确认。
- Android AOA 持久诊断已完成：attach、权限、`openAccessory`、WVR1 首包/ACK、异常和关闭写入应用私有双文件轮换日志，每份上限 128 KiB；普通 ADB 等待状态不落盘。物理重插恢复 ADB 后可由 `scripts/get-wireview-android-diagnostics.ps1` 提取。协议、传输、持久日志三组 Java 自测和 Debug APK 构建通过；固定 `DISPLAY6` 的 8 秒 ADB 回归为 481/481 帧、60.05 FPS、24.04 Mbps、平均编码 7.27 ms，零解码丢帧/错误。
- 华为 AOA 持久日志复测确认权限为 true、`openAccessory()` 成功，但 Android 一直阻塞等待首包，收不到 Windows 已完整写入 OUT `0x01` 的 HELLO；无 ACK 已收敛为该机 `PID_2D01` AOA+ADB 数据面兼容问题。复测同时修复 detach 后轮询重复消费旧 attach Intent、重新认领失效 accessory 的恢复缺陷。修复版固定 `DISPLAY6` 的 8 秒 ADB 回归为 481/481 帧、60.04 FPS、24.08 Mbps、平均编码 7.26 ms，零解码丢帧/错误。
- OPPO PKB110 的默认 ADB/TCP 基线通过：固定 `DISPLAY6`、481/481 帧、60.04 FPS、24.08 Mbps、首帧 15 ms、Surface P50/P95 20.52/21.72 ms，零解码丢帧/错误；脚本已兼容 Android 16/OPlus 的 `topResumedActivity`。但 OPPO 在同一 `PID_2D01` 与 `Motorola Composite AUTO Interface` 下也表现为授权和 `openAccessory()` 成功、收不到 bulk OUT；正式 Host overlapped 后端同样无返回。AOA 产品通道据此作为当前驱动绑定兼容性阻塞延期，不修改现有 USB 驱动，默认继续 ADB/TCP。OPPO detach 日志确认旧 Intent 恢复修复生效，测试中临时停止/禁用的 O+Connect 与 GlideX 服务已全部恢复为 Running/Automatic。控制中心的 WinUSB 下拉项、组合说明和启动确认框已同步标出该双品牌阻塞，关键文本纳入 13/13 CTest 自测。
- 真扩展屏路径已开始：IddCx 无 EDID 虚拟显示器改为申报参考平板的 2560x1600@60（首选）、1920x1200@120 与 1920x1200@60 模式；下一阶段是驱动到 Host 的 GPU 帧中继，详见 `docs/EXTENDED_DISPLAY_PATH.md`。当前 IddCx 输出仍是技术验证 DLL，而不是可发布驱动；在正式签名可用前，物理桌面镜像路径用于继续开发 USB、延迟遥测和触控/压感笔回传。
- IddCx 到 Host 的帧中继 v1 控制协议已落地：共享头定义三槽共享 D3D11 纹理、流描述、帧就绪/释放与停止消息；Host 侧独立序列化与截断/类型错误测试通过，随后由驱动工程直接包含同一头文件，避免两端协议漂移。
- 三槽 GPU 共享纹理已在 Host 侧通过真实 D3D11 验证：以 NT 共享句柄和键控互斥锁创建，使用同一显卡 LUID 在第二个 D3D11 设备打开并核对流属性；这为“驱动不等待、Host 消费后归还槽位”的扩展屏帧路径提供了可复用基础。
- IddCx 到 Host 的跨身份资源交接已由复制进程句柄改为受限命名纹理：Host 为三槽创建随机 `Global\\WireViewIdd-*` 名称和仅限当前用户、LocalSystem、LocalService 的 ACL；驱动只接受该前缀、核对尺寸格式后按名称打开并回传确认。Host 单元/命名管道回归和 IddCx 驱动包编译均已通过；尚未启用逐帧复制和持续消费，不能据此宣称扩展屏已可用。
- 逐帧 GPU 中继闭环已接通：驱动交换链线程仅在可立即取得 key 0 的槽位执行 GPU 复制，再向后台三项队列投递 `frame_ready`；后台线程等待 Host 的 `frame_released`。Host 在持有命名纹理的会话中取得 key 1、核对递增序号并交还 key 0。真实 D3D11 回归连续发布六帧，全部被 Host 正确归还；驱动包的 `InfVerif` 与 `Inf2Cat` 仍为零错误、零警告。Host 暂未将这些纹理接入 H.264 编码器，扩展屏尚不可用。
