# WireView 架构

## 目标数据路径

    Windows DWM
        -> WireView IddCx 驱动
        -> D3D11 色彩转换与硬件编码
        -> 压缩帧共享队列
        -> WireView Host USB 传输
        -> Android Receiver
        -> MediaCodec 硬件解码
        -> SurfaceView

当前 Secure Boot 参考机上的开发替代路径为：

    Windows 物理桌面
        -> DXGI Desktop Duplication
        -> D3D11 Video Processor（BGRA -> NV12）
        -> Media Foundation AMD H.264 硬件编码器
        -> ADB reverse TCP/USB
        -> Android MediaCodec
        -> SurfaceView

这条路径复用最终产品除“虚拟扩展显示器输入”之外的全部数据面，且不要求降低 Windows 启动安全策略。

输入事件沿相反方向返回，并拥有高于视频数据的调度优先级。

## 组件边界

### protocol

无平台依赖的线上协议。负责消息类型、帧边界、版本、序号、时间戳和完整性校验，不负责套接字或 USB API。

### Windows IDD

只负责虚拟显示器生命周期和实时画面数据面：

- 通过 IddCx 枚举显示器并接收 DirectX 表面。
- 在 GPU 上完成色彩转换。
- 使用低延迟硬件编码器生成压缩帧。
- 绝不在帧处理线程进行 UI、设备发现或阻塞式重试。

### Windows Host

负责 USB 设备发现、握手、能力协商、传输、输入注入、状态管理和恢复。开发期使用 ADB 通道，产品期切换到 AOA/WinUSB。

### Android Receiver

负责 USB 接收、协议解析、MediaCodec 硬件解码、Surface 呈现、触摸与压感笔采集。

产品 USB 的设备切换、WinUSB/`UsbAccessory` 边界与迁移顺序见
[`PRODUCT_USB_TRANSPORT.md`](PRODUCT_USB_TRANSPORT.md)。

## 延迟原则

- 视频队列以最新画面优先，编码前可丢弃过期帧。
- 已编码的参考帧不能任意丢弃；发生拥塞时请求关键帧恢复。
- H.264 禁用会引入重排的 B 帧，使用短 GOP 和低延迟模式。
- 解码结果直接输出到 Surface，不经过 Bitmap 或 CPU 像素拷贝。
- 光标、输入和遥测使用独立消息，不等待大视频帧。
- 所有阶段使用同一单调时钟域内的微秒时间戳；跨设备延迟通过握手估算时钟偏移。

## 首版明确不支持

- HDR、4K、多虚拟显示器。
- 音频、Wi-Fi 与互联网远程访问。
- DRM 受保护内容。
- AV1 和软件编码作为常规路径。
