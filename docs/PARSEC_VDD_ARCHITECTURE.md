# WireView 外部虚拟显示器架构

## 决策

WireView 不再负责枚举 Windows 虚拟显示器，也不再把自研 IddCx 驱动作为产品路径。Windows 的显示器 2 由外部、已签名的 Parsec Virtual Display Driver（VDD）创建；WireView 只管理其会话、采集该显示器、编码并通过 USB 发送到 Android。

```text
Windows 显示器 1（真实屏幕）
Windows 显示器 2（Parsec VDD）
    -> DXGI Desktop Duplication
    -> WireView GPU H.264 编码
    -> USB / ADB
    -> Android MediaCodec Surface

Android 触摸 / 鼠标 / 压感笔
    -> USB / ADB
    -> 映射到显示器 2 的 Windows 桌面坐标
```

当 Windows 显示设置选择“扩展这些显示器”后，显示器 2 是独立桌面区域；把窗口拖入该区域时，平板只显示该窗口及该区域内容，而不是显示器 1 的镜像。

## 外部依赖边界

- VDD 的安装、签名、更新与卸载由可信第三方发行包负责，WireView 不安装自签名驱动，不关闭 Secure Boot，也不修改启动配置。
- WireView 后续会使用 VDD 的公开控制接口创建、保活和移除一块显示器。该接口的显示器需要周期性保活；WireView 退出或无法恢复会话时应安全移除它。
- VDD 的二进制许可与独立控制库的开源许可分开处理。产品发布前必须确认可分发方式，开发阶段不把第三方驱动二进制纳入仓库。

## 已完成的迁移基础

Host 不再只能采集主显示器：

- `wireview_host.exe --list-displays` 列出当前所有已附加的 DXGI 显示器，包含稳定 Windows 设备名，例如 `\\.\DISPLAY2`。
- `--stream-desktop ... --display \\.\DISPLAY2` 精确采集所选显示器；未指定时继续采集主显示器，保持已有镜像用法兼容。
- `scripts/start-wireview.ps1 -DisplayDevice '\\.\DISPLAY2'` 与 `scripts/test-usb-desktop.ps1 -DisplayDevice '\\.\DISPLAY2'` 会把选择传到 Host。
- `scripts/start-wireview.ps1 -UseParsecVdd` 会创建并保活一块仅属于当前 WireView 会话的 VDD 显示器，自动识别它的 `DISPLAYn` 并在会话结束时移除。不能同时传入 `-DisplayDevice`。
- Android 输入现在按被采集显示器的虚拟桌面矩形映射；VDD 位于主屏左、右、上或下方时，触摸、压感笔和鼠标仍会落到副屏，而不是主屏。

当前参考机已安装 Parsec VDD：用户已有的副屏为 `\\.\DISPLAY6`，与主屏 `\\.\DISPLAY1` 同时附加。该编号不是稳定配置，不能硬编码；控制中心每次从 Windows 当前显示器状态枚举。

## 后续顺序

1. [x] 接入 VDD 会话管理：状态检查、创建、周期保活和移除。
2. [x] 在 VDD 出现后自动选择新 `DISPLAYn`，由用户在首次运行时确认 Windows 的“扩展这些显示器”。
3. [x] 将 Android 输入坐标按目标显示器的虚拟桌面矩形映射；继续复用现有多点、笔压和鼠标注入。
4. [x] 将现有目标显示器的枚举、刷新、选择、默认副屏复用和模式选择加入控制中心。
5. [x] 将 Parsec VDD 版本/数量、显式创建/移除操作和 WireView 会话归属加入控制中心。独立 broker 持有精确 driver index，并在控制中心退出或收到停止事件时清理。已有 Parsec 副屏时会直接复用并禁止重复创建；状态发布已有无驱动自测，实际 Add/Remove 尚未在参考机执行。
6. [x] 为 broker 自身被强制终止的极端情况增加残留会话恢复 UI。仅唯一记录、owner/broker 都已退出且记录的 `DISPLAYn` 仍为 Parsec 副屏时，才提供人工二次确认；确认框会明示 driver index 可能已被其他程序复用，不将候选记录宣称为自动可证明归属。其余情况均禁止清理。
7. 在没有外部 Parsec 副屏的可控环境执行 broker Add→Remove 真机回归。
8. [x] 完成固定副屏的软中断恢复：`DISPLAY6` 上两次 Android 握手都采集同一目标，目标消失时 Host 硬失败，不回退主屏或创建 VDD。
9. 在 2560x1600@60 和可用的 120Hz 模式上做物理 USB 拔插、端到端延迟与长时稳定性回归。

## 冻结的实验路径

`windows/idd-spike` 与 `docs/EXTENDED_DISPLAY_PATH.md` 保留为自研 IddCx 技术实验记录，不再进入日常启动、安装或发布流程。它们不会影响 WireView 的镜像功能或新的 VDD 副屏路径。
