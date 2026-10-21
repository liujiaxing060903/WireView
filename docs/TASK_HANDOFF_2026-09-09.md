# WireView 客户问题与迭代交接（2026-09-09）

> 这是近期真实用户支持、定位和版本迭代的续接记录。它补充根目录 `CODEX_CONTEXT.md`；遇到内容冲突时，以本文件中标有版本号的近期记录和当前代码为准。

## 1. 产品事实与支持边界

- WireView 是 **Windows → Android** 的本机有线副屏软件：Windows 使用 DXGI 捕获和 H.264 编码，经 `adb reverse tcp:39871 tcp:39871` 在 USB 本地传输；Android 使用 `MediaCodec` 硬解码显示。
- Android 最低版本为 Android 11（`minSdk 30`）。控制中心需要 **WebView2 Evergreen Runtime**。
- 发布包不包含 ADB。用户必须把官方完整 `platform-tools` 文件夹放到 Windows 包根目录，确保 `platform-tools\adb.exe` 存在；不能多嵌套一层 `platform-tools\platform-tools`。
- 复制主屏不依赖 Parsec VDD；扩展屏幕需 Parsec Virtual Display Driver。`ParsecVDisplay.exe` 只负责创建/删除/管理虚拟屏，创建完成后不需要常驻。
- WireView 不使用 USB-C DP Alt Mode、也不接收 Android 的 DP 视频信号。只要 USB 线能稳定提供数据、`adb devices` 能识别并授权，USB 2.0 端口也可以使用；DP1.4“视频输出”与 WireView 无关。
- 同一时间只支持一台 Android 设备会话；多台授权设备时控制中心应提示只保留一台。
- Android 型号库：`data/device-models/android-cn-mobile.tsv`，约 2591 条；未知型号优先显示系统上报厂商/型号。

## 2. 当前可交付版本与构建

截至本交接，最新发布包为 **1.1.19**：

- Windows：`release/WireView-1.1.19-Windows-x64.zip`
  - SHA-256：`4833023B3630D62A60F095C9999AEBC7B488FC91BF1F1FC9DFD410AD61DC0C5B`
- Android：`release/WireView-1.1.19-debug.apk`
  - SHA-256：`6C3998A28C3670794C8FDF39EFFC34E8A41FD23772C37011C02878026C263DA1`

Windows 与 Android 均显示版本 `1.1.19`。Windows 标题行显示“WireView 有线副屏 — USB 平板副屏 版本 1.1.19”；Android 空闲页灰色提示框右下角显示版本。

常用构建：

```powershell
pwsh.exe -NoProfile -File scripts\build-windows.ps1 -BuildDirectory build\windows-release-1.1.1 -Configuration Release
pwsh.exe -NoProfile -File scripts\build-android.ps1
pwsh.exe -NoProfile -File scripts\package-wireview-release.ps1 -Version 1.1.19 -BuildDirectory build\windows-release-1.1.1
```

近期 Windows 构建的 13 项 CTest 已通过；Android Debug APK 构建已通过。构建前若控制中心在运行，须从托盘彻底退出，否则 exe 文件会被锁定。

## 3. 近期版本迭代

| 版本 | 改动 | 状态 |
|---|---|---|
| 1.1.10 前后 | 处理 UAC（安全桌面）出现时 Android 退出/黑屏问题。 | 后续继续修正。 |
| 1.1.13 | 增加 Host 编码器、GPU、Media Foundation 输出格式重新协商等完整诊断。 | 已用于准确定位 Intel QSV 故障。 |
| 1.1.15 | 安全桌面结束后重建捕获/编码管线、重发 `start_stream`，并修复旧 NeedInput 事件导致的恢复异常。 | 用户对“右键 SumatraPDF → 以管理员身份运行 → 点是/否”回归已确认正常。 |
| 1.1.16 | 修复托盘退出后的历史 VDD session 残留误阻塞创建：失主且无当前显示映射的记录自动忽略/清理，不根据陈旧 index 删除真实 Parsec 屏。 | 已实现；仍必须避免删除无法可靠归属的用户副屏。 |
| 1.1.17 | “诊断与日志”不再重复塞入很长的 Host 原始诊断，改为给出本次 `control-*.log` 与 `host-*.log` 的完整路径；停止后也保留最近一次 Host 日志路径。 | 已实现。 |
| 1.1.18 | Windows/Android 增加可见版本号。 | 已实现。 |
| 1.1.19 | VDD 创建时，如果只有一块基线屏且 VDD 未作为独立附加显示出现，会调用 `DisplaySwitch.exe /extend` 尝试修复错误的“仅在 1 上显示/克隆”拓扑；错误文案改为“未成为独立、附加的 Windows 显示器”。 | 已实现；无法替代损坏的 Parsec/Windows/GPU 驱动。 |

## 4. 已解决或明显改善的问题

### 4.1 30 分钟后断开

- 早期 Host 有桌面流时长限制，约 30 分钟后会话停止。
- 已移除该定时退出；日志曾显示的 `2147483644 seconds` 即代表近似无限时长，不是 30 分钟定时器。

### 4.2 诊断不足

- 现在 Host 诊断会记录：捕获 DISPLAY、捕获 GPU 名称与 vendor/device ID、显存、编码器名称、低延迟/码率/GOP/B 帧控制项、初始输出格式、`MF_E_TRANSFORM_STREAM_CHANGE` 重新协商过程、最终 HRESULT、首帧/Android decoder 健康信息。
- 底部状态栏只放摘要，完整内容在“诊断与日志”，且可直接把日志文件发给维护者。

### 4.3 UAC 导致平板端退出、黑屏或长期卡顿

复现：投屏过程中，用“以管理员身份运行 SumatraPDF”等方式打开 UAC；安全桌面使 DXGI 桌面捕获暂时不可用。

- 早期：Android 直接断开；中间版本：UAC 期间画面不动，但点“是/否”后黑屏或长期卡顿。
- 现状：1.1.15 的管线重建与 Android 流重启方案已在该回归中被用户确认解决。
- 如以后复现，应优先收集同一会话的 `control-*.log` 和 `host-*.log`，不要仅凭底部摘要判断。

### 4.4 托盘退出后 VDD 残留阻止新建

旧现象：控制中心提示“检测到未处理的 WireView VDD 残留记录，已拒绝创建新副屏”，但 Parsec 当前副屏为 0，且无法安全映射旧 `DISPLAYn`。

- 1.1.16 后：无 owner/broker 存活、无对应显示器的陈旧记录不再阻止创建；同时不会用陈旧 driver index 冒险删除新的用户显示器。
- 原则：如果当前 session 的归属或显示映射无法证明，宁可不删除，不能批量清理 Parsec/Sunshine 的屏。

## 5. Windows / Parsec VDD 类问题

### 5.1 自动创建超时，或创建后显示为 `DISPLAY1`

典型错误：

```text
Parsec VDD created a display but Windows did not attach it before the timeout
```

或新版：

```text
Parsec VDD已接受创建请求，但WireView在30秒内没有找到唯一新增的已附加显示器
```

含义：Parsec 接收了 Add 请求，但 Windows 在等待时间内没有把它作为**独立且附加**的桌面显示器公开。复制模式若正常，Android/ADB 链路通常正常，问题在 Windows 显示拓扑/Parsec 驱动。

曾见案例：第一次 Add 后 Parsec 似乎创建了 `\\.\DISPLAY1`，实际上是 Windows 将新路径设成“仅在 1 上显示”或复制拓扑，因此 WireView 没有新增可捕获 DISPLAY，随后按安全规则移除。

排查顺序：

1. 打开 **设置 → 系统 → 屏幕**，上方是否出现显示器 2。
2. 点击显示器 2，在“多显示器”中选择 **扩展这些显示器**；不能只是看是否有数字 2。
3. 如果选择扩展后自动退回“仅在 1 上显示”，这是 Windows/Parsec/GPU 驱动无法启用虚拟输出，不是 Android、ADB 或 WireView 传输故障。
4. 在设备管理器检查 `Parsec Virtual Display Adapter` 是否有黄色感叹号/代码 31。
5. 用 `ParsecVDisplay.exe` 删除全部虚拟显示器（仅删除虚拟屏，不是卸载驱动），重启后再创建一块。
6. 重装 Parsec VDD 0.45；若依旧不能扩展，卸载后重启，安装 0.41 驱动，再用 0.45 portable 包的 `ParsecVDisplay.exe` 管理副屏。
7. 可用 HDMI 实体显示器作分界测试：清理 VDD/重启后接 HDMI 并设为扩展。HDMI 正常而 VDD 无法扩展，说明是 VDD 驱动路径；HDMI 也会退回仅主屏，则是 Windows/GPU/显示配置问题。

### 5.2 RTX 5070 用户的明确案例

原始诊断曾显示：Parsec Add 后只有物理 `DISPLAY1`（NVIDIA GeForce RTX 5070），`active paths=1`；WireView 已尝试隐藏调用 `/extend`，仍未出现独立的虚拟输出。

- 该用户手动 Parsec 创建能看到显示器 2，但 Windows 下拉框是“仅在 1 上显示”；改成“扩展这些显示器”会立即跳回。
- 结论：这台机器是 Parsec VDD / Windows 显示驱动启用失败；应按 5.1 的驱动修复流程处理。

### 5.3 `requested capture display is unavailable: \\.\DISPLAY32`

含义：控制中心保存/选择了 `DISPLAY32`，但 Host 启动时该显示器已不在 Windows 的附加输出列表，或它没有被扩展。不是 Android 模型丢失本身。

处理：停止会话 → 完整模式点击刷新 → 确认下拉列表中实际存在目标副屏 → 必要时到 Windows 显示设置启用“扩展这些显示器” → 再启动。不要把会变动的 `DISPLAYn` 编号硬编码。

## 6. Android / ADB 类问题

### 6.1 控制中心有时显示设备型号，有时只显示“Android 设备”

- 型号、刷新率来自 ADB shell 查询；插拔后在 adb 仍显示 `device` 但型号变成通用名、刷新率变 `-- Hz`，一般表示控制中心在该轮刷新中没有成功完成设备属性读取，而不是 MTP 文件传输状态。
- 若点击启动后 Android `ConnectionClient` 收到 EOF（`WireView connection closed in the middle of a packet`），它是 Windows Host 先退出后 TCP 被关闭的**结果**，需查看 Host 原始错误，而不能直接认定 Android decoder 崩溃。

### 6.2 ADB 已连接但控制中心仍连不上

- Windows 文件资源管理器显示 Android 设备，只证明 MTP；不能证明 ADB 可用。
- 先从 **Windows 包根目录**运行：

```powershell
Get-Item .\platform-tools\adb.exe
.\platform-tools\adb.exe devices
```

- 正确输出应有序列号与 `device`。`unauthorized` 则在平板弹窗点“允许 USB 调试”；空列表则换数据线/接口、重新插拔、检查 ADB 驱动。
- 遇到 PowerShell `CommandNotFoundException`：不是平板问题，是当前目录下缺少 `platform-tools\adb.exe` 或目录放错层级。官方完整 `platform-tools` 应直接位于 WireView 发布目录根目录。

### 6.3 REDMI K Pad 2 双 USB-C 口

- 短边 USB 2.0 口可用于 WireView；WireView 不需要 DP 视频输出能力。
- 真实日志已证明 REDMI K Pad 2（`26048RP6AC`）可完成 ADB、握手并上报 `3008×1880 @120`，所以短口并非其连接失败原因。
- USB 2.0 会比高速 USB 限制带宽余量，但 60 FPS 应优先可用；高分辨率/120 FPS 需要另看编码、传输和设备解码能力。

## 7. 编码器、HDR 与性能兼容性（当前最重要的未完成项）

### 7.1 Intel Quick Sync 输出格式重新协商后失败

一位 Lenovo TB-J706F 用户的 Host 诊断：

```text
Encoding \\.\DISPLAY1 at 2560x1600@60 with Intel® Quick Sync Video H.264 Encoder MFT
Capture adapter: Intel(R) Iris(R) Xe Graphics
IMFTransform::ProcessOutput requested output-format change
Encoder renegotiated output type: H.264 | size=2560x1600 | frameRate=60/1
WireView host error: IMFTransform::ProcessOutput failed after output-format renegotiation: 0x8000FFFF
```

结论：握手、ADB reverse、Android decoder 都已启动；故障发生在 **PC 端 Intel Quick Sync H.264 MFT** 的输出格式重新协商之后。它不是平板编码格式不同、不是 ADB、不是 Parsec。

注意：这不代表所有 Intel 核显都会失败。与特定 Intel GPU/驱动/Windows Media Foundation/目标分辨率组合有关。旧说法中的 “Intel Iris Xe” 仅适用于上述日志所对应机器；遇到 Ultra 系列、Arc 系列或双显卡笔记本，必须看实际 Host 日志的 `Capture adapter` 与 `Encoding ... with` 行，不能根据 CPU 名称猜。

建议用户先尝试低档：`1920×1200 @60 FPS`，再 `2560×1600 @60 FPS`；并升级 OEM/Intel 显卡驱动。真正的产品修复方向是：

1. Intel MFT 在初始化/重新协商失败时，不直接终止会话；
2. 在同一适配器上尝试兼容配置或软件 H.264 回退；
3. 双显卡机器可评估 NVIDIA NVENC 回退，但“Windows 图形设置设为高性能”并不必然改变 DXGI 捕获显示所在 GPU，必须在代码与实机日志中验证。

### 7.2 用户自己的 AMD 780M 参考机

该机真实日志：

```text
Encoding \\.\DISPLAY32 at 2560x1600@120 with AMDh264Encoder
Capture adapter: AMD Radeon 780M Graphics | vendor=0x1002 | device=0x1900
```

说明它使用 AMD 的 H.264 硬件编码器 `AMDh264Encoder`，该路径当前正常。它与 Intel Quick Sync 故障的差异是编码器实现、驱动和 Media Foundation 行为不同，不是“AMD 一定好、Intel 一定坏”。

### 7.3 REDMI K Pad 2 用户的两个 Windows 端故障

该用户的 `control-20260909-42932.log` 已证明 USB/ADB/握手均正常；问题不是短边 USB 2.0 口。

1. **复制主屏**：

```text
WireView host error: cursor compositor requires BGRA desktop frames
```

这很可能是 Windows HDR/10-bit 桌面输出，DXGI 给出 `R10G10B10A2` 等非 BGRA 帧，但当前光标合成代码只支持 BGRA/BGRA sRGB。

临时建议：关闭 Windows HDR，设为 8-bit 色彩输出后重试。代码层的正式修复是扩展 `desktop_encoder.cpp` 光标合成/色彩转换以兼容 10-bit 帧，或实现安全的捕获回退路径；未完成。

2. **扩展副屏**：

```text
Intel(R) Graphics | device 0x7D67
Intel® Quick Sync Video H.264 Encoder MFT
3008x1880 @120
... output-format renegotiation ...
0x8000FFFF
```

这也是 Intel Quick Sync 重新协商失败，高分辨率 120 FPS 使兼容性压力更高。先建议 1920×1200@60；代码回退策略未完成。

### 7.4 扩展模式周期性卡顿 / 大卡顿

曾有一台 Ryzen 9 8940HX + RTX 5060 笔记本，复制流畅、扩展时卡顿；`1920×1200@60` 比 `2560×1600@120` 好，后者表现为隔几秒出现数秒鼠标轨迹不连续、窗口拖动残影。平板是同一台，不能简单归因于 Android 性能。

已做过若干节奏/参数改动，现有日志尚不能证明单一根因；GPU 性能也不是充分解释。后续要继续时，应收集运行中的 Host 原始指标（编码耗时、帧发送节奏、DXGI timeout/重建、decoder telemetry）并做同机分辨率/刷新率矩阵，而不是只根据 CPU/显卡规格推断。

## 8. 其它已见用户问题

### 8.1 重启电脑后不断弹 “无法找到 AdbWinApi.dll” 与命令框

通常是发布目录里 `adb.exe` 被单独复制、遗漏同目录 DLL，或 PATH 指向了不完整 ADB。处理为删除不完整的 ADB 副本，下载官方 Platform Tools，把**完整文件夹**放在发布包根目录。WireView 不分发 ADB。

### 8.2 `tcp:39871 tcp:39871` 是什么

这是 `adb reverse tcp:39871 tcp:39871` 的成功回显：让 Android 自己的 `127.0.0.1:39871` 转到 Windows Host 的本机 `127.0.0.1:39871`。它不是公网端口、不是云服务器，也不代表已经完成 WireView 握手。

### 8.3 Android logcat 的正确采集时机

复现前清空：

```powershell
.\platform-tools\adb.exe logcat -c
```

随后插线/连接 → 点复制或扩展 → 等待异常发生 → 立即导出：

```powershell
.\platform-tools\adb.exe logcat -d -v time | Select-String "WireView|ConnectionClient|VideoDecoder|MediaCodec|AndroidRuntime|FATAL EXCEPTION"
```

最好在断开后立刻导出，避免无关日志覆盖。即使此时控制中心变成“Android 设备”也可导出，但信息可能较少。

## 9. 下次开发优先级

1. **Intel 编码器兼容性**：为 QSV `ProcessOutput` 重新协商失败增加自动回退，不应让 Host 直接退出；先研究现有 `desktop_encoder.cpp` 的 MFT 枚举/选择和可用 NVIDIA/软件编码器路径，所有回退要带明确日志。
2. **HDR/10-bit 捕获兼容性**：解决 `cursor compositor requires BGRA desktop frames`，至少要让用户看到明确、可操作的“关闭 HDR/切到 8-bit”提示，理想是支持 R10G10B10A2 捕获/转换。
3. **扩展模式卡顿**：仅在可获得同机日志/真机时推进；先增加捕获、编码、发送、重建的时间线诊断，再优化，避免没有证据地改帧率策略。
4. 保持 VDD 的非破坏性原则。WireView 能检测/尝试 `/extend`，但不可通过删除未知 Parsec 显示器“强行修复”。

## 10. 修改纪律

- 用户明确要求“改”“实现”“打包”才修改代码或产物；其余用户支持咨询只分析与给操作步骤。
- 改 UI 前先准确理解描述，不自行更改其他视觉效果。
- 任何发布前改动至少运行 Windows 构建与 13 项 CTest；Android 改动另构建 APK。
- `ParsecVDisplay.exe` 与 Parsec 驱动不随 WireView 分发，且不要把 ADB 打进发布包。
- 不要将 Host 的 `output_fps` 直接宣传为 Android 面板真实 120 Hz；当前可靠表述仍为 60 FPS，120 FPS 为实验性主机发送档。
