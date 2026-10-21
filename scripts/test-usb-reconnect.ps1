[CmdletBinding()]
param(
    [ValidateRange(45, 600)]
    [int]$DurationSeconds = 90,
    [ValidateRange(0, 65535)]
    [int]$StreamWidth = 0,
    [ValidateRange(0, 65535)]
    [int]$StreamHeight = 0,
    [ValidateSet(60, 120)]
    [int]$StreamFrameRate = 60,
    [string]$DisplayDevice,
    [switch]$SimulateAppReconnect
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
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
    throw 'adb.exe was not found. Set ANDROID_HOME or install Android SDK Platform Tools.'
}

function Get-AuthorizedSerials {
    @(
        & $adb devices |
            Select-Object -Skip 1 |
            Where-Object { $_ -match "\tdevice$" } |
            ForEach-Object { ($_ -split "\t")[0] }
    )
}

$initialSerials = @(Get-AuthorizedSerials)
if ($initialSerials.Count -ne 1) {
    throw "Exactly one authorized Android device is required; found $($initialSerials.Count)."
}
$initialSerial = $initialSerials[0]
$packagePath = (& $adb -s $initialSerial shell pm path com.wireview.receiver).Trim()
if (-not $packagePath.StartsWith('package:')) {
    throw 'WireView is not installed. Run the android project from Android Studio first.'
}

$hostExecutable = Join-Path $repoRoot 'build\windows-nmake\windows\host\wireview_host.exe'
if (-not (Test-Path -LiteralPath $hostExecutable)) {
    throw 'wireview_host.exe is missing. Run scripts\build-windows.ps1 first.'
}
if (($StreamWidth -eq 0) -xor ($StreamHeight -eq 0)) {
    throw 'StreamWidth and StreamHeight must be set together, or both left at zero.'
}
if ($StreamWidth -ne 0 -and (($StreamWidth % 2) -ne 0 -or ($StreamHeight % 2) -ne 0)) {
    throw 'StreamWidth and StreamHeight must both be even for H.264 NV12.'
}

$initialDisplayList = @(& $hostExecutable --list-displays 2>&1)
$initialDisplayList | Write-Host
$initialDisplaySummary = $initialDisplayList | Select-Object -First 1
if ($initialDisplaySummary -notmatch '^Attached WireView capture displays: (\d+)$') {
    throw 'Could not read the initial Windows display count.'
}
$initialDisplayCount = [int]$Matches[1]
if ($DisplayDevice) {
    $displayPattern = '^\s+' + [regex]::Escape($DisplayDevice) + '\s+\|'
    if (-not ($initialDisplayList | Where-Object { $_ -match $displayPattern })) {
        throw "Requested display $DisplayDevice is not attached. The reconnect test will not fall back to primary."
    }
}

& $adb -s $initialSerial reverse tcp:39871 tcp:39871 | Out-Null
& $adb -s $initialSerial logcat -c
& $adb -s $initialSerial shell am start -n 'com.wireview.receiver/.MainActivity' | Out-Null

$logDirectory = Join-Path $env:TEMP 'WireView'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$logPrefix = Join-Path $logDirectory ("usb-reconnect-" + [guid]::NewGuid().ToString('N'))
$hostArguments = @('--reconnect-desktop', $DurationSeconds)
if ($StreamWidth -ne 0) {
    $hostArguments += @('--size', "$StreamWidth`x$StreamHeight")
}
if ($StreamFrameRate -ne 60) {
    $hostArguments += @('--frame-rate', $StreamFrameRate)
}
if ($DisplayDevice) {
    $hostArguments += @('--display', $DisplayDevice)
}
$hostProcess = Start-Process `
    -FilePath $hostExecutable `
    -ArgumentList $hostArguments `
    -WindowStyle Hidden `
    -RedirectStandardOutput "$logPrefix.out.log" `
    -RedirectStandardError "$logPrefix.err.log" `
    -PassThru

try {
    Start-Sleep -Seconds 5
    if ($SimulateAppReconnect) {
        Write-Host '正在模拟 Android 应用/ADB reverse 中断，USB 线无需拔插。'
        & $adb -s $initialSerial reverse --remove tcp:39871 2>$null | Out-Null
        & $adb -s $initialSerial shell am force-stop com.wireview.receiver | Out-Null
        Start-Sleep -Seconds 2
        $reconnectedSerial = $initialSerial
    } else {
        Write-Host '请现在拔掉 USB 线，等脚本提示后再插回。'

        $disconnectDeadline = [DateTime]::UtcNow.AddSeconds(60)
        do {
            Start-Sleep -Milliseconds 500
            $stillConnected = (Get-AuthorizedSerials) -contains $initialSerial
        } while ($stillConnected -and [DateTime]::UtcNow -lt $disconnectDeadline)
        if ($stillConnected) {
            throw 'USB disconnect was not detected within 60 seconds.'
        }

        Write-Host '已检测到断开。请现在重新插入 USB 线，并在设备弹出授权提示时允许。'
        $reconnectDeadline = [DateTime]::UtcNow.AddSeconds(60)
        $reconnectedSerial = $null
        do {
            Start-Sleep -Milliseconds 500
            $authorizedSerials = @(Get-AuthorizedSerials)
            if ($authorizedSerials.Count -eq 1) {
                $reconnectedSerial = $authorizedSerials[0]
            }
        } while (-not $reconnectedSerial -and [DateTime]::UtcNow -lt $reconnectDeadline)
        if (-not $reconnectedSerial) {
            throw 'No authorized Android device reconnected within 60 seconds.'
        }
    }

    & $adb -s $reconnectedSerial reverse tcp:39871 tcp:39871 | Out-Null
    & $adb -s $reconnectedSerial shell am start -n 'com.wireview.receiver/.MainActivity' | Out-Null
    Write-Host 'ADB reverse 已恢复，正在等待 WireView 会话自动续传。'

    $hostProcess.WaitForExit()
    $hostOutput = Get-Content -Raw "$logPrefix.out.log"
    $hostErrors = Get-Content -Raw "$logPrefix.err.log"
    $hostOutput | Write-Host
    $hostErrors | Write-Host
    $handshakeCount = ([regex]::Matches($hostOutput, 'WireView handshake complete with:')).Count
    if ($handshakeCount -lt 2) {
        throw 'Windows Host did not observe two Android handshakes.'
    }
    if ($hostErrors -notmatch 'WireView desktop session interrupted') {
        throw 'Windows Host did not detect the expected disconnected session.'
    }
    if ($DisplayDevice) {
        $targetCount = ([regex]::Matches(
            $hostOutput,
            'Capture target: ' + [regex]::Escape($DisplayDevice))).Count
        if ($targetCount -lt 2) {
            throw "Windows Host did not reuse $DisplayDevice for both sessions."
        }
    }
    if ($hostOutput -match 'WireView created Parsec VDD display') {
        throw 'Reconnect unexpectedly entered the Parsec VDD creation path.'
    }

    $finalDisplayList = @(& $hostExecutable --list-displays 2>&1)
    $finalDisplayList | Write-Host
    $finalDisplaySummary = $finalDisplayList | Select-Object -First 1
    if ($finalDisplaySummary -notmatch '^Attached WireView capture displays: (\d+)$' -or
        [int]$Matches[1] -ne $initialDisplayCount) {
        throw 'Windows display topology changed during reconnect testing.'
    }
    if ($DisplayDevice -and
        -not ($finalDisplayList | Where-Object { $_ -match $displayPattern })) {
        throw "Requested display $DisplayDevice disappeared during reconnect testing."
    }
    $decoderLog = & $adb -s $reconnectedSerial logcat -d -s 'WireViewDecoder:V' 'AndroidRuntime:E' '*:S'
    $decoderLog | Write-Host
    if (($decoderLog -join "`n") -match 'Decoder failed while queuing a frame|Cannot configure decoder') {
        throw 'Android reported a decoder failure during reconnect testing.'
    }
    Write-Host 'WireView USB reconnect test passed.'
}
finally {
    if (-not $hostProcess.HasExited) {
        $hostProcess.Kill()
        $hostProcess.WaitForExit()
    }
}
