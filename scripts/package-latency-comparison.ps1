[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$base = Join-Path $workspace 'build/latency-comparison'
$adbSource = Join-Path $env:LOCALAPPDATA 'Android/Sdk/platform-tools'

foreach ($variant in @('current', 'v1.1.20')) {
    $package = Join-Path $base $variant
    $destination = Join-Path $package 'windows'
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $runtime = if ($variant -eq 'current') {
        Join-Path $workspace 'build/windows-120fps-pacing/windows/host'
    } else {
        Join-Path $base 'v1.1.20-build/windows/host'
    }
    foreach ($name in @('wireview_control.exe', 'wireview_control_legacy.exe', 'wireview_host.exe',
                         'wireview_session.exe', 'wireview_vdd_broker.exe')) {
        $source = if ($variant -eq 'current' -and $name -eq 'wireview_host.exe') {
            Join-Path $workspace 'build/windows-120fps-profile/windows/host/wireview_host.exe'
        } else { Join-Path $runtime $name }
        Copy-Item -LiteralPath $source -Destination (Join-Path $destination $name) -Force
    }
    foreach ($folder in @('web', 'assets', 'data')) {
        Copy-Item -LiteralPath (Join-Path $runtime $folder) -Destination $destination -Recurse -Force
    }
    $tools = Join-Path $destination 'platform-tools'
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    foreach ($name in @('adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll', 'NOTICE.txt', 'source.properties')) {
        Copy-Item -LiteralPath (Join-Path $adbSource $name) -Destination $tools -Force
    }
    $licenses = Join-Path $destination 'licenses/WebView2'
    New-Item -ItemType Directory -Path $licenses -Force | Out-Null
    foreach ($name in @('LICENSE.txt', 'NOTICE.txt')) {
        Copy-Item -LiteralPath (Join-Path $workspace "external/webview2/sdk/$name") -Destination $licenses -Force
    }
    if ($variant -eq 'current') {
        $amfLicense = Join-Path $destination 'licenses/AMF'
        New-Item -ItemType Directory -Path $amfLicense -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $workspace 'external/amf/LICENSE.txt') -Destination $amfLicense -Force
    }
    Set-Content -LiteralPath (Join-Path $destination 'cursor-diagnostics.enabled') -Value $variant -Encoding ascii
    if ($variant -eq 'v1.1.20') {
        Copy-Item -LiteralPath (Join-Path $base 'v1.1.20-source/android/app/build/outputs/apk/debug/app-debug.apk') `
            -Destination (Join-Path $package 'WireView-v1.1.20-cursor-probe.apk') -Force
    }
    Write-Output "Ready: $destination"
}
