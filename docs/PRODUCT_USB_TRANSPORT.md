# 产品 USB 传输设计

## 目标与边界

WireView 的产品通道采用 Android Open Accessory（AOA）+ Windows WinUSB。Windows 电脑是 USB Host，负责把 Android 设备切换到 accessory mode；Android Receiver 作为 USB device，通过一个 bulk IN 和一个 bulk OUT 端点与 Host 双向通信。

WVR1 包格式、握手、视频、输入和遥测语义保持不变。传输层只提供有序、可靠、可关闭的双向字节流；WVR1 继续负责分包、长度、CRC 和版本检查。ADB reverse/TCP 保留为开发与诊断后端，不能成为产品运行依赖。

官方依据：

- [Android USB accessory 概览](https://developer.android.com/develop/connectivity/usb/accessory)
- [AOA 协议](https://source.android.com/docs/core/interaction/accessories/protocol)
- [Android `UsbAccessory` API](https://developer.android.com/reference/android/hardware/usb/UsbAccessory)
- [Microsoft WinUSB 开发说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/introduction-to-winusb-for-developers)
- [WinUSB 读写流程](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/using-winusb-api-to-communicate-with-a-usb-device)

## 产品数据流

```text
Windows USB 枚举
    -> 识别普通 Android USB 设备
    -> AOA vendor control requests（查询版本、发送标识、启动 accessory）
    -> 等待设备以 AOA VID/PID 重新枚举
    -> SetupAPI 定位 WinUSB 接口
    -> WinUsb_Initialize
    -> bulk OUT: WVR1 视频/控制包
    <- bulk IN : WVR1 输入/遥测包

Android USB_ACCESSORY_ATTACHED
    -> 用户授权 / 已保存的关联
    -> UsbManager.openAccessory
    -> ParcelFileDescriptor
    -> FileInputStream / FileOutputStream
    -> 现有 WVR1 ConnectionClient、MediaCodec 和输入回传
```

AOA v1 已覆盖 WireView 所需的通用双向 bulk 通信；AOA 2.0 的音频和 HID 扩展不属于当前需求。产品正常运行应使用 accessory-only PID，不依赖 accessory+ADB 模式。

## 两端职责

### Windows Host

- 枚举候选 USB 设备，并要求用户明确选择，不能对所有 Android 设备盲目切换模式。
- 在普通 USB 身份上执行 AOA 控制请求，记录设备身份线索，等待重新枚举。
- 只打开匹配 WireView accessory 标识的 AOA 设备。
- 查询 bulk IN/OUT pipe，不硬编码端点号；收发使用 overlapped I/O，并允许读写并行。
- 断线后取消挂起 I/O、关闭 WinUSB/文件句柄、回到枚举状态；保持同一显示器目标，不创建或切换 VDD。
- 向控制中心报告“等待设备 / 正在切换 AOA / 等待授权 / 已连接 / 正在恢复 / 驱动不可用”等明确状态。

### Android Receiver

- 声明 USB accessory feature、attach intent 和 accessory filter。
- 由 `UsbManager` 接收 attachment；未授权时走系统权限流程，授权后调用 `openAccessory`。
- 把 accessory 的文件描述符封装成现有 `TransportConnection`，不复制或重写 WVR1 解析器。
- Activity/Surface 暂时重建时避免把一次物理 USB attach 误判成永久断线；物理拔出时及时关闭连接并清理解码会话。
- ADB/TCP connector 继续保留为显式开发模式，便于安装、日志与对照测试。

## Windows 驱动与安装边界

WinUSB 适合由单个用户态应用独占的 bulk 设备，但设备仍必须正确绑定到 `Winusb.sys`。WinUSB API 只能在相应 USB function 已使用 WinUSB 驱动时发送控制或 bulk transfer，不能越过 MTP/USB composite 等现有 function driver 任意操作设备。

参考 HUAWEI DBR-W10 当前把 `MI_02` ADB function 绑定到系统 `Winusb.sys`，并注册 Android 通用接口 GUID `{F72FE0D4-CBCB-407D-8814-9ED673D0DD6B}`。它可以作为第一台真机的 AOA 控制请求入口，但只在设备已开启 USB 调试并枚举 ADB function 时存在。这个事实只证明“开发原型可启动 AOA”，不能证明“产品无需开发者模式”。

AOA 重新枚举后的 USB 描述符由 Android 平台提供，WireView 也不能像自有固件设备那样增加 Microsoft OS descriptors。因此当前不能承诺“免驱安装”；第一台真机原型必须分别确认切换前引导 function 与切换后 AOA VID/PID 的接口/驱动绑定，再决定签名 INF 或受控 filter driver 的最小匹配范围。若无法在不启用 USB 调试的前提下安全发送最初的 AOA 控制请求，必须把它列为产品方案决策门，并对比 USB 网络传输等替代方案，不能把开发者模式当成未声明的用户前置条件。

若需要 INF，不得用过宽规则接管所有 Android/Google accessory 设备。安装器、驱动签名、卸载恢复和与 ADB/MTP 共存必须作为独立发布验收项。参考：[WinUSB 安装](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/winusb-installation)、[WinUSB 自动安装条件](https://learn.microsoft.com/en-us/windows-hardware/drivers/usbcon/automatic-installation-of-winusb)。

## 实现顺序

1. **已完成：传输边界第一层。** Android `ConnectionClient` 改为依赖 `TransportConnector` / `TransportConnection`；当前 `TcpTransportConnector` 行为不变。纯 Java 自测通过真实 loopback 验证双向流，Debug APK 完整构建通过。
2. **华为兼容性待续：Android AOA 接入。** manifest/filter、attach intent、主动枚举/权限请求和 `UsbAccessoryTransportConnector` 已加入且 APK 构建通过。华为系统能识别并授权，但尚未形成可返回 WVR1 ACK 的应用会话。
3. **Windows AOA 探针已完成首轮真机验证。** 已实现枚举、协议查询、显式唯一候选切换、端点检查和 WVR1 握手探针；HUAWEI DBR-W10 已验证到 WinUSB bulk 写入。Android ACK 问题先移入多品牌兼容阶段。
4. **已完成：Windows 传输抽象。** `development_transport.cpp` 的 WVR1 会话已从 `SOCKET` 参数迁移到 `ReliableByteStream`，TCP 由 `SocketByteStream` 适配。短写、逐字节分片、断流和损坏包测试已加入；Windows 9/9 CTest 和固定 `DISPLAY6` 的 8 秒 ADB 真机回归均通过。
5. **已完成：WinUSB bulk 适配器。** `WinUsbByteStream` 动态要求唯一 bulk IN/OUT，读写使用独立 overlapped 操作，`wait_readable` 超时后保留挂起读取，关闭时取消 I/O。无设备端点测试结合可靠流的短传输/断流测试通过。
6. **已完成代码、待兼容真机：显式 WinUSB 会话入口。** `--stream-desktop ... --transport winusb` 只读等待已经枚举为 AOA 的唯一 accessory：零个继续等待、恰好一个打开、多个立即拒绝；不会发送 AOA 切换请求或修改驱动。TCP 与 WinUSB 共用握手/桌面/输入会话，握手读取有 10 秒分段超时。
7. **已完成代码、真机受驱动绑定阻塞：启动入口、控制中心与恢复。** `start-wireview.ps1 -Transport WinUsb` 不依赖 ADB；Host 断线退出后，脚本重新验证同一 `DISPLAYn`、重新等待唯一 accessory 并累计重连。控制中心默认 ADB，可选择并保存 WinUSB，运行中锁定选择，状态摘要显示通道；WinUSB 下拉项标为“实验，当前阻塞”，组合说明与启动确认框明确指出华为/OPPO 的 `PID_2D01` 驱动绑定均无法完成 WVR1 握手，日常使用 ADB。旧配置/状态没有 transport 时回退 ADB。提示文本已纳入控制中心自测，独立验证构建 13/13 CTest 和默认 ADB 的固定 `DISPLAY6` 真机回归通过；华为与 OPPO 尚不能进入 WinUSB 拔插验收。
8. **已完成：跨重枚举 Android 诊断。** 应用私有目录保存当前/上一份 AOA 诊断日志，各限 128 KiB；记录 attach、权限、`openAccessory`、首包、ACK、异常和关闭，不记录视频/触控内容，也不落盘普通 ADB 周期状态。恢复 ADB 后运行 `scripts/get-wireview-android-diagnostics.ps1` 即可提取；日志轮换自测、Debug APK 和固定 `DISPLAY6` 的 ADB 真机回归均通过。
9. **兼容性阻塞、暂时延期：真机与驱动绑定。** 华为和 OPPO 均在 `PID_2D01` 下完成系统授权与 `openAccessory()`，但 Android 收不到探针或正式 Host 已发送的 bulk OUT。两机都由 `oem97.inf` 以 `Motorola Composite AUTO Interface` 匹配整个 `USB\\VID_18D1&PID_2D01`，尚未验证仅绑定 `MI_00` 或 accessory-only `PID_2D00`。当前不修改现有 USB 驱动，默认继续 ADB/TCP；将来在隔离环境完成最小绑定对照后，再继续 WinUSB 60/120 FPS、输入、拔插和长测。

## 验收条件

- 产品路径在未安装/未启动 ADB 的电脑上可以发现、授权、连接和显示。
- 同一物理 USB 上视频与输入并行，WVR1 协议测试无需修改。
- 拔插后自动恢复到同一 `DISPLAYn`，不新增、删除或回退显示器。
- 2560×1600@60 先达到现有 ADB 基线；随后验证 2560×1600@120。
- 30 分钟无持续队列增长、解码丢帧或错误；控制中心能区分权限、驱动、枚举和传输故障。
- 至少三台不同品牌 Android 设备完成 attach、权限、吞吐、重连和卸载后恢复验证。

## 首次真机 AOA 结果（2026-08-23）

参考设备为 HUAWEI DBR-W10（Android 12）：

- 普通 USB 身份为 `VID_12D1&PID_107D&MI_02`，ADB function 使用 `Winusb.sys` 和 Android 通用接口 GUID；停止 adb server 后可由用户态探针独占打开。
- 设备报告 AOA 协议版本 2，接受 WireView 标识与 start-accessory 请求，并成功重新枚举为 AOA+ADB `VID_18D1&PID_2D01`。
- Windows 使用现有 `oem97.inf`（`Motorola Composite AUTO Interface`）绑定 `Winusb.sys`，无需为这台参考机临时更换驱动。
- AOA 主接口为 interface 0，bulk IN `0x81`、bulk OUT `0x01`，最大包均为 512 字节；WinUSB 查询和写入 WVR1 `HELLO` 均成功。
- Android 系统日志确认进入 accessory mode，识别 `manufacturer=WireView`、`model=WireView Receiver`，并把 `USB_ACCESSORY_ATTACHED` 指向 `com.wireview.receiver/.MainActivity`。用户实际看到并确认了“允许 WireView 访问该 USB 配件”。
- 尽管系统识别、用户授权和 Windows bulk 写入均成功，Android 在延长等待和多次重试中仍未返回 WVR1 `HELLO_ACK`。新增持久日志后复测确认：权限已是 true，`openAccessory()` 返回有效文件描述符，应用随后持续阻塞在读取首个 WVR1 包；Windows 同期对 OUT `0x01` 的完整 HELLO 写入已成功。因此失败点位于该华为 `PID_2D01` AOA+ADB 数据面，而不是权限、打开文件或 ACK 序列化。`PID_2D01` 下的 ADB 辅助接口仍无法被 adb 工具在线访问，必须物理重插后提取日志。
- 物理拔插可恢复普通 USB/ADB 身份；测试前后始终保持 `DISPLAY1 + DISPLAY6`，未创建、删除或切换 VDD。

首台设备阶段的结论曾是华为/EMUI 应用会话兼容问题；下述 OPPO 对照已推翻“仅华为”的判断。Windows Host 的传输无关会话、overlapped WinUSB bulk 适配器和显式 CLI 会话入口已经落地，但当前整设备 WinUSB 绑定/`PID_2D01` 组合路径作为共同阻塞延期；默认仍为 ADB/TCP，现有功能不受影响。

## 第二台真机 AOA 结果（2026-08-24）

OPPO PKB110（Android 16）默认 ADB/TCP 基线正常：固定 `DISPLAY6` 的 2560x1600@60 测试为 481/481 帧、60.04 FPS、24.08 Mbps、首帧 15 ms、Surface P50/P95 20.52/21.72 ms，零解码丢帧/错误。测试脚本因此补充兼容 Android 16/OPlus 的 `topResumedActivity` 状态格式。

AOA 协议版本为 2，设备同样重枚举为 `VID_18D1&PID_2D01`，现有 `Motorola Composite AUTO Interface` 暴露 interface 0、bulk IN `0x81` 与 OUT `0x01`。持久日志确认授权、attach 和 `openAccessory()` 成功，但 Android 阻塞等待首包；探针两次握手和正式 Host 的 overlapped WinUSB 会话都没有收到任何 WVR1 返回字节。物理拔插后的日志也确认 detach 修复生效：枚举归零，没有重新认领旧 accessory。

两品牌同现象后，问题不再按“华为应用兼容”处理，而升级为当前 Windows 整设备 WinUSB 绑定/`PID_2D01` 组合路径的兼容性阻塞。暂不修改或卸载 `oem97.inf`；未来只在可恢复的隔离环境比较 `MI_00` 最小绑定和 accessory-only `PID_2D00`。O+Connect 会维护 Session 0 ADB 并抢占 bootstrap，GlideX 也自带 ADB；测试中临时停止/禁用的服务已全部恢复为 Running/Automatic。

为定位此前 AOA 重枚举后无法在线使用 ADB 读取日志的问题，Android 已增加应用私有持久诊断文件。下一次华为复测后先物理重插恢复普通 ADB，再运行 `scripts/get-wireview-android-diagnostics.ps1`，即可区分 Activity/权限未到达、`openAccessory` 失败、读不到首包、ACK 写入失败或会话异常退出。

本次复测还发现并修复了 detach 后的独立恢复缺陷：Activity 的旧 attach Intent 曾被 500ms 轮询反复消费，平台残留的 accessory 对象会触发无效权限请求和 `openAccessory()` 重试。现在 attach Intent 只消费一次；收到 detach 后忽略旧枚举和迟到权限结果，直到新的 attach 事件。另需注意，参考 Windows 主机的 O+Connect 服务自带 Session 0 ADB，会独占 bootstrap 接口；真机探针测试需临时停止该服务，完成后恢复。
