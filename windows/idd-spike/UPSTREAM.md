# IddCx 技术验证工程

本目录来自 Microsoft Windows Driver Samples 仓库的 video/IndirectDisplay 示例，许可证见 LICENSE.microsoft.txt。

- 上游仓库：https://github.com/microsoft/Windows-driver-samples
- 上游路径：video/IndirectDisplay
- 引入日期：2026-08-22
- 用途：验证 IddCx 构建、测试签名、软件设备枚举和 DirectX swapchain 获取。

本目录不是最终 WireView 驱动。它刻意与 production 代码隔离；阶段 0 已将安装标识、单显示器枚举和诊断名称改为 WireView 专用值，但帧处理主体仍来自微软样例。只有当验证通过后，才会把所需结构迁移到 `windows/driver`，并实现正式 EDID、设备生命周期和编码帧处理逻辑。

为避免依赖系统级 WDK，本地构建脚本会还原微软签名的 `Microsoft.Windows.WDK.x64` `10.0.26100.6584` 包。该版本是微软当前为 Visual Studio 2022 推荐的受支持 WDK。

## 阶段 0 构建

在仓库根目录运行：

```powershell
.\scripts\build-idd-spike.ps1
```

`scripts/build-idd-spike.ps1` 构建 x64 Debug 驱动 DLL 和软件设备启动器；`scripts/package-idd-spike.ps1` 处理并验证 INF、生成 Catalog。测试签名与安装被拆为独立的管理员步骤，以便清楚控制证书信任和系统驱动状态。
