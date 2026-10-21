[CmdletBinding()]
param(
    [ValidateRange(3, 1800)]
    [int]$DurationSeconds = 15,
    [string]$DecoderName,
    [switch]$InputLogOnly,
    [ValidateRange(0, 65535)]
    [int]$StreamWidth = 0,
    [ValidateRange(0, 65535)]
    [int]$StreamHeight = 0,
    [ValidateSet(60, 120)]
    [int]$StreamFrameRate = 60,
    [string]$DisplayDevice,
    [string]$StatusFile,
    [switch]$UseParsecVdd
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$adb = $null
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
foreach ($candidate in $adbCandidates) {
    if (Test-Path -LiteralPath $candidate) {
        $adb = $candidate
        break
    }
}
if (-not $adb) {
    throw 'adb.exe was not found. Set ANDROID_HOME or install Android SDK Platform Tools.'
}

$deviceLines = & $adb devices
$serials = @(
    $deviceLines |
        Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" } |
        ForEach-Object { ($_ -split "\t")[0] }
)
if ($serials.Count -ne 1) {
    throw "Exactly one authorized Android device is required; found $($serials.Count)."
}
$serial = $serials[0]

$packagePath = (& $adb -s $serial shell pm path com.wireview.receiver).Trim()
if (-not $packagePath.StartsWith('package:')) {
    throw 'WireView is not installed. Run the android project from Android Studio first.'
}

$hostExecutable = Join-Path $repoRoot 'build\windows-nmake\windows\host\wireview_host.exe'
if (-not (Test-Path -LiteralPath $hostExecutable)) {
    throw 'wireview_host.exe is missing. Run scripts\build-windows.ps1 first.'
}

& $adb -s $serial reverse tcp:39871 tcp:39871 | Out-Null
& $adb -s $serial logcat -c
& $adb -s $serial shell am force-stop com.wireview.receiver
Start-Sleep -Milliseconds 800
$activityArguments = @(
    '-s', $serial,
    'shell', 'am', 'start',
    '-n', 'com.wireview.receiver/.MainActivity'
)
if ($DecoderName) {
    $activityArguments += @('--es', 'decoder', $DecoderName)
}
& $adb @activityArguments | Out-Null

$activityReady = $false
for ($attempt = 0; $attempt -lt 20; $attempt++) {
    $activityState = & $adb -s $serial shell dumpsys activity activities
    if (($activityState -join "`n") -match '(?:mResumedActivity|topResumedActivity)\s*[:=].*com\.wireview\.receiver') {
        $activityReady = $true
        break
    }
    Start-Sleep -Milliseconds 200
}
if (-not $activityReady) {
    throw 'WireView Activity did not reach the resumed state on Android.'
}
# The resumed state precedes SurfaceView's surfaceCreated callback by a short
# interval on EMUI. Let the decoder Surface settle before accepting the stream.
Start-Sleep -Milliseconds 800

$hostArguments = @('--stream-desktop', $DurationSeconds)
if ($InputLogOnly) {
    Write-Host 'Input mode: log-only. Android touches will be counted but not injected into Windows.'
    $hostArguments += '--input-log-only'
}
if (($StreamWidth -eq 0) -xor ($StreamHeight -eq 0)) {
    throw 'StreamWidth and StreamHeight must be set together, or both left at zero for the native desktop size.'
}
if ($StreamWidth -ne 0) {
    if (($StreamWidth % 2) -ne 0 -or ($StreamHeight % 2) -ne 0) {
        throw 'StreamWidth and StreamHeight must both be even for H.264 NV12.'
    }
    Write-Host "Video mode: $StreamWidth x $StreamHeight at $StreamFrameRate FPS."
    $hostArguments += @('--size', "$StreamWidth`x$StreamHeight")
}
if ($StreamFrameRate -ne 60) {
    $hostArguments += @('--frame-rate', $StreamFrameRate)
}
if ($DisplayDevice) {
    $hostArguments += @('--display', $DisplayDevice)
}
if ($StatusFile) {
    $hostArguments += @('--status-file', $StatusFile)
}
if ($UseParsecVdd) {
    if ($DisplayDevice) {
        throw 'UseParsecVdd creates and selects a new virtual display automatically; do not also set DisplayDevice.'
    }
    $hostArguments += '--parsec-vdd'
}
$hostOutput = @(& $hostExecutable @hostArguments 2>&1)
$hostOutput | ForEach-Object { Write-Host $_ }
if ($LASTEXITCODE -ne 0) {
    throw "WireView real desktop stream failed with exit code $LASTEXITCODE."
}

$joinedHostOutput = $hostOutput -join "`n"
if ($joinedHostOutput -notmatch 'Android first-frame telemetry:') {
    throw 'Android did not send first-frame telemetry to the Windows Host.'
}
if ($joinedHostOutput -notmatch 'Android capabilities: decoder=') {
    throw 'Android did not report decoder and active display capabilities.'
}
if ($joinedHostOutput -notmatch 'Android decoder health: received=') {
    throw 'Android did not report live decoder health counters.'
}

$decoderLog = & $adb -s $serial logcat -d -s 'WireViewDecoder:V' 'AndroidRuntime:E' '*:S'
$decoderLog | Write-Host
$joinedLog = $decoderLog -join "`n"
if ($joinedLog -match 'Decoder failed while queuing a frame|Cannot configure decoder') {
    throw 'Android reported a decoder failure during the real-desktop stream.'
}

$dropCount = ([regex]::Matches($joinedLog, 'Dropping frame')).Count
Write-Host "WireView real desktop USB test passed; decoder input drops: $dropCount."
