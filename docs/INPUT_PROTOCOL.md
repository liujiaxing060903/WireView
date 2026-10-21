# WireView 输入协议

Android 到 Windows 的触控、压感笔和鼠标事件使用 `INPUT_BATCH`（包类型 7）。网络包头中的时间戳为 Android 单调时钟微秒值；载荷内部使用小端序。

## 批头（8 字节）

| 偏移 | 大小 | 字段 | 当前值或含义 |
|---:|---:|---|---|
| 0 | 1 | schemaVersion | `1` |
| 1 | 1 | recordSize | `20` |
| 2 | 2 | recordCount | `1..64` |
| 4 | 2 | viewportWidth | Android 视频 Surface 宽度 |
| 6 | 2 | viewportHeight | Android 视频 Surface 高度 |

## 事件记录（20 字节）

| 偏移 | 大小 | 字段 | 含义 |
|---:|---:|---|---|
| 0 | 1 | action | 1=down，2=move，3=up，4=cancel，5=hover |
| 1 | 1 | tool | 1=finger，2=stylus，3=eraser，4=mouse |
| 2 | 2 | pointerId | Android 指针 ID |
| 4 | 2 | normalizedX | `0..65535`，相对 Surface 宽度 |
| 6 | 2 | normalizedY | `0..65535`，相对 Surface 高度 |
| 8 | 2 | pressure | `0..65535` |
| 10 | 2 | tiltX | 有符号，单位 0.01 度，范围 -90°..90° |
| 12 | 2 | tiltY | 有符号，单位 0.01 度，范围 -90°..90° |
| 14 | 2 | buttons | Android `buttonState` 低 16 位 |
| 16 | 4 | timeOffsetMicros | 相对包头时间戳；当前实时事件为 0 |

Windows Host 会严格校验版本、记录大小、数量、视口和载荷总长度。手指通过合成触控注入，笔/橡皮通过合成笔注入并保留压力、倾角和侧键，鼠标使用绝对坐标输入。

归一化坐标先映射到当前目标显示器的 Windows 全局屏幕矩形。鼠标再按整个虚拟桌面转换为 `MOUSEEVENTF_VIRTUALDESK` 的 0–65535 绝对轴；触控与笔的 `ptPixelLocation` 按 `InjectSyntheticPointerInput` 要求，减去 `SM_XVIRTUALSCREEN/SM_YVIRTUALSCREEN`，转换为相对虚拟桌面左上角的像素坐标。不能直接把位于主屏左侧/上方显示器的负数全局坐标传给 Synthetic Pointer API。

Windows 端已将同一批手指事件聚合为完整活动触点集，并在参考平板上真机验证至四指；单支压感笔、橡皮、压力与倾角也已验证。
