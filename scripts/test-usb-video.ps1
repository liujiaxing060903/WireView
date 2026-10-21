[CmdletBinding()]
param(
    [string]$DecoderName,
    [switch]$ForceLowLatency
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

$assetDirectory = Join-Path $repoRoot 'build\test-assets'
$testStream = Join-Path $assetDirectory 'test-25fps.h264'
$expectedSha256 = 'A9504CDEFDE1136A36D87FFC610CE5368108AD0B6EB693532ABC56C0C991E0EC'

if (-not (Test-Path -LiteralPath $testStream) -or
    (Get-FileHash -Algorithm SHA256 $testStream).Hash -ne $expectedSha256) {
    New-Item -ItemType Directory -Force -Path $assetDirectory | Out-Null
    $uri = 'https://android.googlesource.com/platform/external/chromium_org/+/21a33e9/media/test/data/test-25fps.h264?format=TEXT'
    Write-Host 'Downloading the public Android/Chromium H.264 test stream...'
    $encoded = (Invoke-WebRequest -UseBasicParsing -Uri $uri).Content.Trim()
    [IO.File]::WriteAllBytes($testStream, [Convert]::FromBase64String($encoded))
    $actualSha256 = (Get-FileHash -Algorithm SHA256 $testStream).Hash
    if ($actualSha256 -ne $expectedSha256) {
        throw "Unexpected test stream SHA-256: $actualSha256"
    }
}

$hostExecutable = Join-Path $repoRoot 'build\windows-nmake\windows\host\wireview_host.exe'
if (-not (Test-Path -LiteralPath $hostExecutable)) {
    throw 'wireview_host.exe is missing. Build the windows-debug CMake preset first.'
}

& $adb -s $serial reverse tcp:39871 tcp:39871 | Out-Null
& $adb -s $serial logcat -c
& $adb -s $serial shell am force-stop com.wireview.receiver
$activityArguments = @(
    '-s', $serial,
    'shell', 'am', 'start',
    '-n', 'com.wireview.receiver/.MainActivity'
)
if ($DecoderName) {
    $activityArguments += @('--es', 'decoder', $DecoderName)
}
if ($ForceLowLatency) {
    $activityArguments += @('--ez', 'force_low_latency', 'true')
}
& $adb @activityArguments | Out-Null

& $hostExecutable --stream $testStream
if ($LASTEXITCODE -ne 0) {
    throw "WireView host stream test failed with exit code $LASTEXITCODE."
}

$decoderLog = & $adb -s $serial logcat -d -s 'WireViewDecoder:V' 'AndroidRuntime:E' '*:S'
$decoderLog | Write-Host
if (($decoderLog -join "`n") -notmatch 'First decoded frame released to Surface') {
    throw 'The host sent the stream, but Android did not report a decoded Surface frame.'
}

Write-Host 'WireView USB H.264 first-frame test passed.'
