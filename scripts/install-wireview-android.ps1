[CmdletBinding()]
param(
    [switch]$SkipBuild,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build-android.ps1')
}

$apkPath = Join-Path $repoRoot 'android\app\build\outputs\apk\debug\app-debug.apk'
if (-not (Test-Path -LiteralPath $apkPath)) {
    throw 'WireView Debug APK is missing. Run without -SkipBuild or build the Android project first.'
}

$adbCandidates = [Collections.Generic.List[string]]::new()
if ($env:ANDROID_HOME) {
    $adbCandidates.Add((Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'))
}
if ($env:ANDROID_SDK_ROOT) {
    $adbCandidates.Add((Join-Path $env:ANDROID_SDK_ROOT 'platform-tools\adb.exe'))
}
if ($env:LOCALAPPDATA) {
    $adbCandidates.Add((Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'))
}
$adb = $adbCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $adb) {
    throw 'adb.exe was not found. Install Android SDK Platform Tools or set ANDROID_HOME.'
}

$serials = @(
    & $adb devices |
        Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" } |
        ForEach-Object { ($_ -split "\t")[0] }
)
if ($serials.Count -ne 1) {
    throw "Exactly one authorized Android device is required; found $($serials.Count)."
}

$serial = $serials[0]
& $adb -s $serial install -r $apkPath
if ($LASTEXITCODE -ne 0) {
    throw "WireView APK installation failed with exit code $LASTEXITCODE."
}
if (-not $NoLaunch) {
    & $adb -s $serial shell am start -n 'com.wireview.receiver/.MainActivity' | Out-Null
}
Write-Host "WireView Android app is installed on $serial."
