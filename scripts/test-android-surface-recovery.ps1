[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][string]$DisplayDevice,
    [string]$BuildDirectory = 'build/windows-comment-fixes'
)

$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $env:LOCALAPPDATA 'Android/Sdk/platform-tools/adb.exe'
$hostExecutable = Join-Path $workspace "$BuildDirectory/windows/host/wireview_host.exe"
$testApk = Join-Path $workspace 'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk'
$output = Join-Path $workspace 'build/surface-recovery-device'
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach ($path in @($adb, $hostExecutable, $testApk)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing prerequisite: $path" }
}

# This test never creates/removes displays, changes system rotation settings,
# or injects Windows input. The serial and existing display must be explicit.
& $adb -s $Serial install -r $testApk
if ($LASTEXITCODE -ne 0) { throw 'Could not install the isolated test APK' }
& $adb -s $Serial reverse tcp:39871 tcp:39871
if ($LASTEXITCODE -ne 0) { throw 'ADB reverse failed' }
# Instrumentation restarts the target process. Stop the previous idle client
# first, otherwise it can steal Host's single connection just before restart.
& $adb -s $Serial shell am force-stop com.wireview.receiver
if ($LASTEXITCODE -ne 0) { throw 'Could not stop the previous test client' }
$process = $null
try {
    $process = Start-Process -FilePath $hostExecutable -WindowStyle Hidden -PassThru `
        -ArgumentList @('--stream-desktop', '12', '--display', $DisplayDevice,
            '--size', '1920x1080', '--input-log-only') `
        -RedirectStandardOutput (Join-Path $output 'host.log') `
        -RedirectStandardError (Join-Path $output 'host.err.log')
    # Keep the process handle before a short native child exits. Windows
    # PowerShell otherwise may discard its exit code when refreshing by PID.
    $null = $process.Handle
    $result = @(& $adb -s $Serial shell am instrument -w -r `
        'com.wireview.receiver.test/com.wireview.receiver.SurfaceRecoveryInstrumentation')
    $result | Write-Host
    if (($result -join "`n") -notmatch 'INSTRUMENTATION_CODE: -1') {
        throw 'Surface instrumentation failed'
    }
    if (-not $process.WaitForExit(5000)) { throw 'Host did not finish after the device test' }
    if ($process.ExitCode -ne 0) { throw "Host failed: $($process.ExitCode)" }
    $log = Get-Content -LiteralPath (Join-Path $output 'host.log') -Raw
    if ([regex]::Matches($log, 'WireView handshake complete with:').Count -ne 1 -or
        [regex]::Matches($log, 'Android first-frame telemetry:').Count -lt 2 -or
        [regex]::Matches($log, 'Android requested H.264 recovery keyframe; forcing next frame').Count -lt 2) {
        throw 'Missing proof of Surface recovery within the SAME connection and new recovery keyframe'
    }
    Write-Host "Real Surface/codec recovery passed; log: $output/host.log"
}
finally {
    if ($process -and -not $process.HasExited) {
        # Only the child created above; never stop another user's Host.
        Stop-Process -Id $process.Id -Force
    }
    & $adb -s $Serial uninstall 'com.wireview.receiver.test' | Out-Null
}
