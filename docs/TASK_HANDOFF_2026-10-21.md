# WireView 交接记录（2026-10-21）

> 本记录续接 `docs/TASK_HANDOFF_2026-09-09.md` 与根目录 `CODEX_CONTEXT.md`。近期代码和本文件的发布状态优先。

## 当前结论

- 源码版本已升至 **1.1.20**：Android `versionCode=20`、`versionName=1.1.20`，Windows Web 控制中心和 Android 空闲页均显示 `1.1.20`。
- 本次修复简易模式“扩展屏幕”错误复用完整模式目标显示器的问题，并处理已失效 `DISPLAYn` 的启动路径。
- Windows Release 已在 `build/windows-release-1.1.20/` 构建完成，并于 2026-09-11 通过 **13/13 CTest**。
- 尚未生成 1.1.20 发布 ZIP/APK：上次 Android 构建被 Android Studio 占用的 Gradle Wrapper 锁阻塞，现有 `android/app/build/outputs/apk/debug/app-debug.apk` 仍是旧的 1.1.19 产物，不能作为 1.1.20 发布。
- 本项目已于 2026-10-21 初始化本地 Git 的 `main` 分支。用户已确认 GitHub 账号为 `liujiaxing060903`，目标为公开仓库 `liujiaxing060903/WireView`；上传取决于本机 GitHub CLI 登录授权完成。

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

## 发布待办

1. 完全退出 Android Studio 及 Gradle 任务；此前它持有 `C:\Users\liuji\.gradle\wrapper\dists\gradle-9.4.1-bin\...\gradle-9.4.1-bin.zip.lck`。
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

5. 用 `Get-FileHash -Algorithm SHA256` 对 `release/WireView-1.1.20-Windows-x64.zip` 与 `release/WireView-1.1.20-debug.apk` 生成并记录校验值。

## GitHub 上传待办

- 本地 Git 已初始化到 `main`，提交署名为 `liujiaxing060903`，邮箱为 GitHub 隐私邮箱 `242791441+liujiaxing060903@users.noreply.github.com`。
- 不提交：`build/`、`release/`、Android/Gradle 缓存、IDE 目录、运行日志和 WebView2 配置文件；这些已在 `.gitignore` 中排除。
- 用户已授权创建并上传公开仓库 `liujiaxing060903/WireView`，无需再次询问可见性。官方 GitHub CLI 位于 `build/github-tools/cli/bin/gh.exe`，下载已按官方 SHA-256 校验；目前需用户在浏览器完成设备登录授权。
- 本轮重新运行 Windows Release 目录的 CTest，结果为 13/13 通过。首次提交约 285 个文件、70 MB；已排除额外的 Android 构建报告、`.gradle-user` 和 `.idea`，保留 WebView2 SDK 构建依赖及其许可。
- 公共源码快照不代表 1.1.20 发布包已完成，也不代表本次扩展副屏修复已完成真机回归。
- 由于发布 ZIP/APK 一般体积较大且不属于源码，建议通过 GitHub Releases 上传，而不是加入 Git 历史；需用户明确授权后再创建 Release。

## 持续约束

- 产品日常链路仍为 ADB reverse + TCP；WinUSB/AOA 为实验且受设备驱动阻塞，不能设为默认。
- 60 FPS 是可靠表述；120 FPS 是主机发送实验档，不能宣传为 Android 面板真实 120 Hz。
- 不删除不能可靠证明归属于当前 WireView broker 的 Parsec/Sunshine 副屏。
- Android 最低版本 Android 11（`minSdk 30`）；控制中心依赖 WebView2 Evergreen Runtime。
- 发布包不包含 ADB；用户放置官方完整 `platform-tools` 文件夹。
