# WireView 编码器崩溃 — 根因与修复

对象：`windows/host/src/desktop_encoder.cpp`（源码取自 GitHub 仓库 `liujiaxing060903/WireView`，版本 1.1.20）
现象：`WireView host error: IMFTransform::ProcessOutput failed after output-format renegotiation: 0x8000FFFF`
本机：Windows 11 25H2 26200.9711 · Intel Iris Xe (DEV_46A6) 驱动 32.0.101.6733 · HUAWEI SCMR-W09

---

## 一、根因（已定位到具体代码）

原版 `take_output()`（第 1717–1795 行）是这样处理「编码器要求换输出类型」的：

```cpp
for (unsigned attempt = 0U; attempt < 2U; ++attempt) {
    ...
    const HRESULT result = transform_->ProcessOutput(0, 1, &output_data, &status);
    if (result == MF_E_TRANSFORM_STREAM_CHANGE) {
        ...
        renegotiate_output_type();   // GetOutputAvailableType -> SetOutputType
        continue;                    // <── 立刻再调一次 ProcessOutput
    }
    if (FAILED(result) && attempt > 0U) {
        throw std::runtime_error(
            "IMFTransform::ProcessOutput failed after output-format renegotiation: " +
            hresult_text(result));   // <── 就是这条 0x8000FFFF
    }
```

这里连着犯了三个错，任何一台 Intel QSV 机器都会踩：

1. **换完类型后立刻又调了一次 `ProcessOutput`**。
   `take_output()` 只在收到 `METransformHaveOutput` 事件时被调用，一次事件对应一次 `ProcessOutput`。第一个 `ProcessOutput` 用「流变更」把这次事件消费掉了，编码器此刻手里已经没有待输出的帧；再调一次属于协议外操作，Intel 的 MFT 直接回 `E_UNEXPECTED`（0x8000FFFF）。正确的行为是 `MF_E_TRANSFORM_NEED_MORE_INPUT`。

2. **换类型时没有把流状态考虑进去**。
   这是个异步 MFT（`MF_TRANSFORM_ASYNC=TRUE`，日志里 `asynchronous=yes`），流变更时它内部还挂着上一帧的编码状态。
   （实测补充：这里**不能**用 `MFT_MESSAGE_COMMAND_FLUSH` 复位——我试过，Intel 的 MFT 被 flush 之后就不再抛 `METransformNeedInput`/`METransformHaveOutput` 事件，整个会话静默卡死。正确做法是只换类型、标记需要 IDR，然后把控制权交回事件循环。）

3. **把可恢复错误当致命错误**。
   `if (FAILED(result) && attempt > 0U) throw` —— 一次 `E_UNEXPECTED` 直接让整个会话终止（控制中心里就是「Windows Host 启动/运行失败（退出代码 1）」）。

附带问题：`renegotiate_output_type()` 只取 `GetOutputAvailableType(0, 0)`，换出来的类型**摘要字段和原来一模一样**（就是因为没看 level / 交织模式等真正变化的属性）；而且换类型后没有重新要求关键帧，Android 端解码链会花屏。

所以日志里那三行的完整因果是：

```
requested output-format change        → 编码器首帧提出换类型（Intel QSV 的既成行为）
renegotiated output type ...          → 框架老老实实换了类型
ProcessOutput ... 0x8000FFFF          → 紧接着又拉了一次，MFT 状态不对，E_UNEXPECTED
```

---

## 二、改了什么

改动涉及三个文件（完整 diff 见 `desktop_encoder.fix.patch`）：

- `windows/host/src/desktop_encoder.cpp`（第一部分 5 处 + 第五部分兜底）
- `windows/host/src/windows_video.hpp`（新增 `prefer_software_encoder` 参数）
- `windows/host/src/main.cpp`（`--encode-benchmark N [--software-encoder]`）

### 1. `take_output()` 改为「一次事件一次调用」，返回 `std::optional<Output>`

- `MF_E_TRANSFORM_STREAM_CHANGE` → 换类型 + 标记需要关键帧 → 返回 `std::nullopt`，让事件循环继续等下一个事件，**不再强行二次 Poll**（也不能 flush，见上一节）。
- `MF_E_TRANSFORM_NEED_MORE_INPUT` → 返回 `std::nullopt`（正常情况）。
- `E_UNEXPECTED` → 记一次，要求下一帧 IDR，返回 `std::nullopt`；连续超过 120 次才判定为真故障并报错。
- 重复流变更超过 4 次才抛「repeated its output-format change」，防止死循环。

### 2. 主循环适配「本次没有帧」

`METransformHaveOutput` 分支拿到 `nullopt` 时：释放最老的一个待回收 NV12 槽位（否则 GPU 输出池 3 个槽位耗尽会抛 `GPU NV12 output surface pool is exhausted`），然后 `continue`。

### 3. `renegotiate_output_type()` 认真挑类型

遍历 `GetOutputAvailableType(0, i)`（遇到 `MF_E_NO_MORE_TYPES` 停），优先选**尺寸匹配当前输出**的 H.264 类型，再退回第一个 H.264 类型；然后补 `MF_MT_AVG_BITRATE`，原样 `SetOutputType` 传回，并刷新 `GetOutputStreamInfo`。

### 4. 流复位后强制 IDR

新增 `reset_stream_after_renegotiation()`：只设 `needs_keyframe_=true`（不 flush）；`submit()` 里在 `needs_keyframe_` 为真时设 `CODECAPI_AVEncVideoForceKeyFrame`，保证换了类型之后第一帧是可解码的 IDR。实测日志里对应 `forceRecoveryKeyframe=enabled`。

### 5. 新增成员与常量

`needs_keyframe_`、`stream_changes_`、`unexpected_output_failures_`；`kMaxStreamChanges=4`、`kMaxUnexpectedOutputFailures=120`；补 `#include <optional>`。

---

## 三、怎么编译（已在本机跑通）

本机最终用的是 **Visual Studio 2026 生成工具（18.11）+ CMake 4.1（VS 自带）**，走 **Visual Studio 生成器**而不是仓库预设的 NMake：

```powershell
$cmake = "D:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$repo  = "D:\deepseek工作文件夹\wv_src\WireView-1.1.20"

& $cmake -S $repo -B "$repo\build\vs2026" -G "Visual Studio 18 2026" -A x64 -DBUILD_TESTING=OFF
& $cmake --build "$repo\build\vs2026" --config Release --target wireview_host
```

产物：`build\vs2026\windows\host\Release\wireview_host.exe`

**为什么不用仓库自带的 `scripts/build-windows.ps1`（NMake 预设）**：在本机会稳定失败在

```
LINK : fatal error LNK1201: 写入程序数据库“…\CMakeScratch\TryCompile-xxx\cmTC_xxx.pdb”时出错
```

——它发生在 CMake 的编译器探测里（`cmake -E vs_link_exe` 这层包装），不是代码问题：我手动用同样的 cl/link 命令在同样深的目录里编译链接都正常。换成走 MSBuild 的 Visual Studio 生成器就完全绕开了这一层。

已有的一键脚本 [编译并部署-WireView.ps1](编译并部署-WireView.ps1) 已经按上面这条路径改写，运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\deepseek工作文件夹\编译并部署-WireView.ps1"
```

其它注意点：

- 顶层 `windows/host/CMakeLists.txt` 在缺少 `external/webview2/sdk` 时会 `FATAL_ERROR`；本仓库里 SDK 齐全（`WebView2.h` + `WebView2LoaderStatic.lib` 都在），无需额外下载。
- `/W4 /WX`（警告即错误）在 MSVC 19.51 下也通过了，没有出现新的警告。
- 发布包用静态 MSVC 运行库（`MultiThreaded`），编出来的 exe 不依赖 VC++ Redist。

---

## 四、验收（10 秒，不用连平板）

```powershell
cd "D:\deepseek工作文件夹\wv_src\WireView-1.1.20\build\vs2026\windows\host\Release"
.\wireview_host.exe --encode-benchmark 2
.\wireview_host.exe --encode-benchmark 2 --software-encoder
```

**修复前**（本机实测，1920×1080 主屏也一样挂）：

```
Requested output-format change → renegotiated → WireView host error: ... 0x8000FFFF
```

**修复后（本机实测，2026-11-08）**：

```
Encoder renegotiated output type: H.264 | size=1920x1080 | frameRate=60/1 | bitRate=24000000 | profile=100
  forceRecoveryKeyframe=enabled
Encode result: submitted=120 | encoded=120 | desktopUpdates=112 | desktopUpdateRate=55.39 fps | keyframes=2 | outputRate=59.35 fps | bitrate=11.87 Mbps
Latency: first=0.00 ms | average=14.40 ms | maximum=29.38 ms
Bitstream framing: AnnexB=120/120
```

硬件路径 120/120 帧、约 59.4 fps、平均延迟 14 ms。软件兜底路径 121/121 帧、约 59.6 fps、平均延迟约 243 ms（微软软件编码器内部缓冲约 15 帧，属于正常）。

---

## 五、软件编码兜底（已实现）

`windows/host/src/desktop_encoder.cpp` 现在会在硬件编码器不可用时自动回退到微软软件 H.264 编码器（本机 `mfh264enc.dll` 10.0.26100.9549，实测 1080p60 正常）。触发条件有两种：

1. `find_hardware_encoder()` 抛错（该显卡没有硬件 H.264 MFT）→ 直接改用软件编码器；
2. 硬件 MFT 构造失败（异步属性、D3D11 管理器、类型协商、事件接口任一步抛错）→ 打印原因后重建软件编码器。

实现要点：

| 位置 | 改动 |
|---|---|
| `find_software_encoder()`（新增） | `MFTEnum2` 枚举 H.264/NV12 软件编码器；**不传 `MFT_ENUM_FLAG_HARDWARE`** 即只返回软件 MFT（SDK 里没有 `MFT_ENUM_FLAG_SOFTWARE` 这个标志，我第一版写错，编译期就报未声明） |
| `HardwareAvcEncoder` 构造函数 | 新增 `bool hardware` 参数；软件模式下不要求 `MF_TRANSFORM_ASYNC`、不设置 D3D11 管理器；`ICodecAPI` 取不到时跳过所有编码参数设置（打印 `Encoder controls` 但不 `none` 那一行只在完全取不到时出现） |
| 输出类型顺序 | **必须先设输出类型、再设输入类型**：软件 MFT 在没设输出类型时对 `SetInputType` 返回 `MF_E_TRANSFORM_TYPE_NOT_SET`（0xC00D6D60），反过来则 `SetOutputType` 返回 `MF_E_ATTRIBUTENOTFOUND`（0xC00D36E6）。软件模式先试 High profile 的完整输出类型，失败再试 Main，最后才退到 `select_h264_output_type()` 枚举出来的类型 |
| `GpuNv12Converter::read_nv12()`（新增） | 用已有的 NV12 staging 纹理 `CopyResource` + `Map`，把 Y/UV 平面拷成紧凑的系统内存帧；**拷完立刻 `release_slot`**，不等编码输出——软件编码器内部会缓冲十几帧，3 个 GPU 槽位否则立刻耗尽（实测报 `GPU NV12 output surface pool is exhausted`） |
| `submit_bytes()`（新增） | 软件编码器不走 DXGI surface buffer，改为 `MFCreateMemoryBuffer` + `memcpy` 提交 |
| `next_event()` | 软件 MFT 是同步的、没有事件队列：用 `input_pending_` 模拟 `METransformNeedInput` / `METransformHaveOutput` / `METransformDrainComplete`，并在派发输入前用 `GetInputStatus` 检查 `MFT_INPUT_STATUS_ACCEPT_DATA`，避免 `ProcessInput` 撞上 `MF_E_NOTACCEPTING`；主循环代码不用改 |
| 排空（drain） | 同步 MFT 没有 `METransformDrainComplete` 事件：`drain()` 时置 `input_pending_` 主动拉一次，每取到一帧再置回去，直到 `ProcessOutput` 返回 `NEED_MORE_INPUT` 才报排空完成 |
| 帧丢弃记账 | `take_output()` 用 `dropped_input_` 区分「流变更丢掉一帧」和「暂时没有输出」；主循环只在真丢帧时回收最老的 GPU 槽位，避免误回收已经释放的槽位 |
| 返回码 | `encoded > 0 && (lowLatency || software)` 视为成功（软件编码器没有低延迟标志） |

命令行新增一个强制开关，方便 A/B 对比（只对 `--encode-benchmark` 生效，日常会话是自动回退）：

```powershell
.\wireview_host.exe --encode-benchmark 2                      # 优先生成硬件编码
.\wireview_host.exe --encode-benchmark 2 --software-encoder   # 强制软件编码
```

软件编码时日志里会出现：

```
Encoding \\.\DISPLAY1 at 1920x1080@60 with H264 Encoder MFT
Encoder controls: ...
Encoder path: asynchronous=no | d3d11Aware=no | lowLatencySet=no | software=yes
Encode result: submitted=120 | encoded=120 | ... | bitrate=...
```

开销提示：软件编码每帧要做一次 2560×1600 NV12 回读（约 6.5 MB/帧），CPU 占用明显高于硬件编码。日常用 1080p60 足够流畅；2560×1600 建议只在硬件路径确实不可用时才用。


---

## 六、附：本次诊断的关键实验

| 实验 | 结果 |
|---|---|
| `wireview_host.exe --encode-benchmark 2`（主屏 1080p，无平板/无副屏） | ❌ 同样 0x8000FFFF → 与设备、Parsec VDD、2560×1600 无关 |
| ffmpeg `h264_mf -hw_encoding 0`（微软软件 MFT） | ✅ 1080p60 正常 |
| ffmpeg `h264_qsv`（Intel VPL，绕开 MSMF） | ✅ 1080p60 / 24 Mbps 正常 |
| ffmpeg `h264_mf -hw_encoding 1`（同一颗 QSV，走 MSMF） | ❌ 连初始类型协商都过不去 |

即：硬件编码器本身没问题，问题在 MSMF 这条路径的调用时序上。

---

## 八、新增：重连自动投屏（2026-11-08）

需求：同一个平板再次插 USB 时，自动恢复上次的投屏方式；并提供一个开关。

实现（windows/host/src/control_main.cpp）：

| 位置 | 作用 |
|---|---|
| g_state.auto_resume_* | 开关状态、上次模式（copy/extend）、上次设备（device_model，握手时拿到的型号）、本次连接是否已触发 |
| load_auto_resume_settings() / save_auto_resume_settings() | 读写 %LOCALAPPDATA%\WireView\auto-resume.ini（enabled / mode / device） |
| start_session() | 真实会话（非预览、非设备识别探针）启动时记录当前设备与模式 |
| poll_device_detection() | 设备识别探针结束、没有会话在跑、且型号与记录一致时，按上次模式自动开始 |
| publish_web_state() / handle_web_message() | 把开关状态同步给页面；处理 uto-resume-on / uto-resume-off |

行为约定：

- 默认**关闭**；开关在控制中心「设置 → 重连自动投屏」
- 只对**同一台设备**（按型号字符串匹配）生效
- 每次插线只触发一次；手动停止后不会立刻重启，重新插线才会
- 输入模式沿用当前选择（仅显示 / 触控与手写笔）
- 扩展屏幕仍走原来的 Parsec VDD 创建/复用路径

---

## 九、Android 接收端改动（待编译 APK）

这三处都改在 ndroid/app/src/main/java/com/wireview/receiver/，**必须重新编译 APK 才生效**。
改动前已备份到 ackup-android-<时间戳>，完整 diff 见 ndroid-改动.patch。

| 文件 | 改动 |
|---|---|
| MainActivity.java | 投屏时隐藏整个 connectionPanel（原来只隐藏文字，圆角背景在带黑边的画面里会露出两块深灰色块）；新增左下角「⚙ 设置」入口；新增画面方向偏好（自动 / 锁定横屏 / 锁定竖屏）；解码器偏好从 SharedPreferences 读取，intent 参数优先 |
| CodecProbe.java | 新增 listAvcDecoders()，按评分排序列出设备上所有 AVC 解码器，供设置面板选择 |
| VideoDecoder.java | 配置解码器时额外下发 KEY_OPERATING_RATE（= 流帧率）与 KEY_PRIORITY = 0（实时优先级）；这两个是框架调度提示，部分解码器会据此收窄内部缓冲，麒麟 990 实测忽略 |

设置面板只在**非投屏状态**显示（投屏时会隐藏，避免遮挡画面）：

- 「选择解码器…」：自动（推荐）/ 各硬件与软件解码器，含是否声明低延迟、是否支持 1080p60 → 重新打开 App 生效
- 「画面方向…」：跟随手机 / 锁定横屏 / 锁定竖屏 → 立即生效

编译方式见前面第三节的 Windows 部分之外，Android 需要 JDK 17+ 与 Android SDK Platform 36。
