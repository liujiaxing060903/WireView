# WireView 交接记录（2026-10-21）

> 本记录续接 `docs/TASK_HANDOFF_2026-09-09.md` 与根目录 `CODEX_CONTEXT.md`。近期代码和本文件的发布状态优先。

## 当前结论

- 源码版本已升至 **1.1.20**：Android `versionCode=20`、`versionName=1.1.20`，Windows Web 控制中心和 Android 空闲页均显示 `1.1.20`。
- 本次修复简易模式“扩展屏幕”错误复用完整模式目标显示器的问题，并处理已失效 `DISPLAYn` 的启动路径。
- Windows Release 已在 `build/windows-release-1.1.20/` 构建完成，并于 2026-09-11 通过 **13/13 CTest**。
- 2026-10-22 已重新构建并生成 1.1.20 Windows ZIP、Android APK 和 SHA-256 校验文件。APK 清单与签名已检查，签名证书与 1.1.19 一致；上次缓存拒绝访问在允许写入用户 Gradle 缓存后未再复现，无需先终止现有 Java 进程。
- 已于 2026-10-21 创建并上传公开 GitHub 仓库：<https://github.com/liujiaxing060903/WireView>。默认分支 `main`，本地 `main` 已跟踪 `origin/main`；首次源码提交为 `7b58a93`。

## 1.1.20：简易模式扩展副屏修复

### 用户复现

1. 创建 WireView 扩展副屏并停止会话。
2. 在完整模式的“目标显示器”选择 `DISPLAY1`（主屏）。
3. 切回简易模式并点击“扩展屏幕”。
4. 旧行为错误地捕获主屏，表现为复制；之后切回旧 `DISPLAY6` 可能重复报 `requested capture display is unavailable`。

### 已实现行为

`windows/host/src/control_main.cpp`：

- `begin_simple_extension()` 不再使用完整模式下拉框的当前选择；它先刷新显示器列表并强制选择控制中心当前持有的 `owned_display_device`，只要该副屏仍是非主屏、附加到 Windows，便从该副屏启动。
- 如果 WireView 自己持有的副屏已不再附加到 Windows，程序只向持有该精确 driver index 的 broker 请求移除，并在 broker 完成后创建新的 WireView VDD；不删除或重用其他 Parsec/Sunshine 副屏。
- `start_session()` 在启动普通视频会话前重新枚举附加显示器。持久化/旧下拉框里的失效 `DISPLAYn` 会被拦截、刷新列表并显示可操作提示，不再把无效目标交给 `wireview_host`。

### 代码位置

- 状态字段：`control_main.cpp` 的 `ControlState::recreate_vdd_for_simple_extension*`。
- 安全重建分支：`poll_vdd_broker()`。
- 简易扩展的目标锁定与恢复：`begin_simple_extension()`。
- 通用目标附加性预检：`start_session()`。

## 发布构建与验证（2026-10-22 已完成）

1. 确认构建过程可写入 `C:\Users\liuji\.gradle`。此前 `.zip.lck` 的拒绝访问可能来自沙箱限制，不能仅凭此错误推断 Android Studio 占用；必要时通过正常的权限申请机制重试构建。
2. 构建 Android：

   ```powershell
   pwsh.exe -NoProfile -File scripts\build-android.ps1
   ```

3. 确认 `android/app/build/outputs/apk/debug/output-metadata.json` 的 `versionCode=20`、`versionName=1.1.20`。
4. 打包 Windows 与复制 APK：

   ```powershell
   pwsh.exe -NoProfile -File scripts\package-wireview-release.ps1 `
     -Version 1.1.20 `
     -BuildDirectory build\windows-release-1.1.20
   ```

5. 已用 `Get-FileHash -Algorithm SHA256` 生成校验值，结果保存到 `release/WireView-1.1.20-SHA256SUMS.txt` 与 [发布说明](releases/v1.1.20.md)。

6. 验证结果：Windows 13/13 CTest、Android 三组 Java 自测、包内新旧控制中心状态自测和 Host loopback 全部通过。ZIP 包内程序哈希与构建目录一致，无 ADB/Parsec 安装器或 WebView2 运行缓存；打包脚本版本不符及覆盖保护均已验证。

7. GitHub Release：<https://github.com/liujiaxing060903/WireView/releases/tag/v1.1.20>。Windows 为 x64 便携 ZIP，Android 为延续旧证书的 Debug 签名侧载 APK。

## GitHub 仓库与后续同步

- 本地 Git 已初始化到 `main`，提交署名为 `liujiaxing060903`，邮箱为 GitHub 隐私邮箱 `242791441+liujiaxing060903@users.noreply.github.com`。
- 不提交：`build/`、`release/`、Android/Gradle 缓存、IDE 目录、运行日志和 WebView2 配置文件；这些已在 `.gitignore` 中排除。
- 用户已完成 GitHub CLI 授权，并明确要求创建、上传公开仓库 `liujiaxing060903/WireView`。仓库已创建，首次源码推送已成功；官方 GitHub CLI 位于 `build/github-tools/cli/bin/gh.exe`，下载已按官方 SHA-256 校验。
- 本机系统代理为 `127.0.0.1:7897`。Git 直连 `github.com:443` 超时，本次使用命令级 `-c http.proxy=http://127.0.0.1:7897` 后上传成功；没有修改全局网络设置。后续同步若遇到同样错误，应先核对当前系统代理，而非假定账号授权失效。
- 本轮重新运行 Windows Release 目录的 CTest，结果为 13/13 通过。首次提交约 285 个文件、70 MB；已排除额外的 Android 构建报告、`.gradle-user` 和 `.idea`，保留 WebView2 SDK 构建依赖及其许可。
- 两端发布包已于 2026-10-22 构建并验证，但本轮未追加扩展副屏的真实设备操作回归。
- 用户已明确授权发布 1.1.20 到公开 GitHub Releases；ZIP、APK 和校验文件作为 Release 附件上传，不加入源码 Git 历史。

## 持续约束

- 产品日常链路仍为 ADB reverse + TCP；WinUSB/AOA 为实验且受设备驱动阻塞，不能设为默认。
- 60 FPS 是可靠表述；120 FPS 是主机发送实验档，不能宣传为 Android 面板真实 120 Hz。
- 不删除不能可靠证明归属于当前 WireView broker 的 Parsec/Sunshine 副屏。
- Android 最低版本 Android 11（`minSdk 30`）；控制中心依赖 WebView2 Evergreen Runtime。
- 发布包不包含 ADB；用户放置官方完整 `platform-tools` 文件夹。
