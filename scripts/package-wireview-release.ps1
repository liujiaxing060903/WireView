[CmdletBinding()]
param(
    [string]$Version = '1.1.0',
    [string]$BuildDirectory = 'build/windows-release-1.1.0',
    [string]$DependencyDirectory = 'D:\S+Zn\Coding\WireView附件\安装需要'
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

foreach ($required in @(
    (Join-Path $runtimeDirectory 'wireview_control.exe'),
    (Join-Path $runtimeDirectory 'wireview_host.exe'),
    (Join-Path $runtimeDirectory 'wireview_session.exe'),
    (Join-Path $runtimeDirectory 'wireview_vdd_broker.exe'),
    (Join-Path $runtimeDirectory 'web/index.html'),
    (Join-Path $runtimeDirectory 'assets/ui/copy-screen.png'),
    (Join-Path $runtimeDirectory 'data/device-models/android-cn-mobile.tsv'),
    (Join-Path $DependencyDirectory 'ParsecVDisplay-v0.45-setup.exe'),
    (Join-Path $DependencyDirectory 'ParsecVDisplay-v0.45-portable.zip'),
    (Join-Path $DependencyDirectory 'parsec-vdd-0.41.0.0.exe'),
    $apkSource
)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "发布前检查失败，缺少：$required"
    }
}

if (Test-Path -LiteralPath $packageDirectory) {
    Remove-Item -LiteralPath $packageDirectory -Recurse -Force
}
if (Test-Path -LiteralPath $archivePath) {
    Remove-Item -LiteralPath $archivePath -Force
}

New-Item -ItemType Directory -Path $packageDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_control.exe') -Destination (Join-Path $packageDirectory 'WireView 控制中心.exe')
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_host.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_session.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'wireview_vdd_broker.exe') -Destination $packageDirectory
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'web') -Destination (Join-Path $packageDirectory 'web') -Recurse
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'assets') -Destination (Join-Path $packageDirectory 'assets') -Recurse
Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'data') -Destination (Join-Path $packageDirectory 'data') -Recurse
Copy-Item -LiteralPath (Join-Path $repoRoot 'docs/RELEASE_WINDOWS.md') -Destination (Join-Path $packageDirectory 'README.md')
Copy-Item -LiteralPath $apkSource -Destination $apkDestination -Force

$dependencyTarget = Join-Path $packageDirectory '安装依赖/Parsec VDD'
New-Item -ItemType Directory -Path $dependencyTarget -Force | Out-Null
foreach ($dependency in @(
    'ParsecVDisplay-v0.45-setup.exe',
    'ParsecVDisplay-v0.45-portable.zip',
    'parsec-vdd-0.41.0.0.exe'
)) {
    Copy-Item -LiteralPath (Join-Path $DependencyDirectory $dependency) -Destination $dependencyTarget
}

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
