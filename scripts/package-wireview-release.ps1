[CmdletBinding()]
param(
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version = '1.1.20',
    [string]$BuildDirectory = 'build/windows-release-1.1.20'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$runtimeDirectory = Join-Path $repoRoot "$BuildDirectory/windows/host"
$releaseDirectory = Join-Path $repoRoot 'release'
$packageName = "WireView-$Version-Windows-x64"
$packageDirectory = Join-Path $releaseDirectory $packageName
$archivePath = Join-Path $releaseDirectory "$packageName.zip"
$apkSource = Join-Path $repoRoot 'android/app/build/outputs/apk/debug/app-debug.apk'
$apkDestination = Join-Path $releaseDirectory "WireView-$Version-debug.apk"
$apkMetadataPath = Join-Path $repoRoot 'android/app/build/outputs/apk/debug/output-metadata.json'
$webViewLicenseDirectory = Join-Path $repoRoot 'external/webview2/sdk'

foreach ($required in @(
    (Join-Path $runtimeDirectory 'wireview_control.exe'),
    (Join-Path $runtimeDirectory 'wireview_control_legacy.exe'),
    (Join-Path $runtimeDirectory 'wireview_host.exe'),
    (Join-Path $runtimeDirectory 'wireview_session.exe'),
    (Join-Path $runtimeDirectory 'wireview_vdd_broker.exe'),
    (Join-Path $runtimeDirectory 'web/index.html'),
    (Join-Path $runtimeDirectory 'assets/ui/copy-screen.png'),
    (Join-Path $runtimeDirectory 'data/device-models/android-cn-mobile.tsv'),
    (Join-Path $webViewLicenseDirectory 'LICENSE.txt'),
    (Join-Path $webViewLicenseDirectory 'NOTICE.txt'),
    $apkMetadataPath,
    $apkSource
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "发布前检查失败，缺少：$required"
    }
}

$apkMetadata = Get-Content -Raw -LiteralPath $apkMetadataPath | ConvertFrom-Json
if (@($apkMetadata.elements).Count -ne 1 -or
    $apkMetadata.elements[0].versionName -ne $Version) {
    throw "Android APK 版本与发布版本 $Version 不一致，请先重新构建 Android。"
}
$webIndex = Get-Content -Raw -LiteralPath (Join-Path $runtimeDirectory 'web/index.html')
if ($webIndex -notmatch [regex]::Escape("版本 $Version")) {
    throw "Windows 控制中心版本与发布版本 $Version 不一致，请先重新构建 Windows。"
}
foreach ($destination in @($packageDirectory, $archivePath, $apkDestination)) {
    if (Test-Path -LiteralPath $destination) {
        throw "发布产物已存在，拒绝覆盖：$destination"
    }
}

New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_control.exe') -Destination (Join-Path $packageDirectory 'WireView 控制中心.exe')
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_control_legacy.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_host.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_session.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_vdd_broker.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'web') -Destination (Join-Path $packageDirectory 'web') -Recurse
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'assets') -Destination (Join-Path $packageDirectory 'assets') -Recurse
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'data') -Destination (Join-Path $packageDirectory 'data') -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs/RELEASE_WINDOWS.md') -Destination (Join-Path $packageDirectory 'README.md')
Copy-Item -LiteralPath $apkSource -Destination $apkDestination -Force

$dependencyTarget = Join-Path $packageDirectory '安装依赖'
New-Item -ItemType Directory -Path $dependencyTarget -Force | Out-Null
$licenseTarget = Join-Path $packageDirectory 'licenses/WebView2'
New-Item -ItemType Directory -Path $licenseTarget -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $webViewLicenseDirectory 'LICENSE.txt') -Destination $licenseTarget
Copy-Item -LiteralPath (Join-Path $webViewLicenseDirectory 'NOTICE.txt') -Destination $licenseTarget

@'
扩展屏幕需要预先安装 Parsec Virtual Display Driver；复制主屏不需要。
WireView 发布包不分发 ParsecVDisplay.exe 或 Parsec VDD 驱动安装器。

请到 Parsec VDD 管理工具项目查看驱动安装与版本兼容性说明：
https://github.com/nomi-san/parsec-vdd
下载页面：
https://github.com/nomi-san/parsec-vdd/releases

安装后，确认设备管理器中的 Parsec Virtual Display Adapter 工作正常，
再打开 WireView 控制中心使用“扩展屏幕”。请保留其他程序已创建的虚拟显示器。
'@ | Set-Content -LiteralPath (Join-Path $dependencyTarget 'Parsec VDD 下载说明.txt') -Encoding utf8

@'
Android SDK Platform Tools 请从官方页面下载：
https://developer.android.google.cn/tools/releases/platform-tools?hl=zh-cn

下载后，将压缩包中完整的 platform-tools 文件夹放到发布包根目录，
与 assets、data、web、安装依赖 四个文件夹处于同一级，使路径成为：platform-tools\adb.exe

本公开发布包不附带 Android SDK Platform Tools；请阅读根目录 README 的许可说明。
'@ | Set-Content -LiteralPath (Join-Path $packageDirectory '安装依赖/Android Platform Tools 下载说明.txt') -Encoding utf8

Compress-Archive -LiteralPath $packageDirectory -DestinationPath $archivePath -CompressionLevel Optimal

Write-Output "WindowsPackage=$archivePath"
Write-Output "WindowsPackageDirectory=$packageDirectory"
Write-Output "AndroidApk=$apkDestination"
