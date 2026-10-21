[CmdletBinding()]
param(
    [ValidateRange(1, 1440)]
    [int]$DurationMinutes = 480,
    [switch]$EnableInput,
    [string]$StatusFile,
    [ValidateRange(0, 65535)]
    [int]$StreamWidth = 0,
    [ValidateRange(0, 65535)]
    [int]$StreamHeight = 0,
    [ValidateSet(60, 120)]
    [int]$StreamFrameRate = 60,
    [ValidateSet('Adb', 'WinUsb')]
    [string]$Transport = 'Adb',
    [switch]$DevicePreview,
    [switch]$DeviceProfileProbe,
    [string]$DisplayDevice,
    [switch]$UseParsecVdd
)

$ErrorActionPreference = 'Stop'

function Publish-Status {
    param(
        [string]$Message,
        [string]$State = 'message'
    )

    Write-Host $Message
    if ($StatusFile) {
        $safeMessage = $Message -replace "[`r`n]", ' '
        $inputMode = if ($EnableInput) { 'enabled' } else { 'log-only' }
        $display = if ($DisplayDevice) { $DisplayDevice } elseif ($UseParsecVdd) { 'new Parsec VDD' } else { 'primary display' }
        $statusLines = @(
            'version=1'
            "state=$State"
            "message=$safeMessage"
            "display=$display"
            "input_mode=$inputMode"
            "requested_width=$StreamWidth"
            "requested_height=$StreamHeight"
            "requested_fps=$StreamFrameRate"
            "transport=$($Transport.ToLowerInvariant())"
            'actual_width=0'
            'actual_height=0'
            'actual_fps=0'
            'client='
            "reconnects=$script:ReconnectCount"
            'first_frame_ms=0'
            'surface_samples=0'
            'surface_p50_ms=0'
            'surface_p95_ms=0'
            'delivered_frames=0'
            'output_fps=0'
            'bitrate_mbps=0'
            'decoder_name='
            'decoder_hardware=0'
            'decoder_software=0'
            'decoder_low_latency=0'
            'decoder_1080p60=0'
            'android_width=0'
            'android_height=0'
            'android_refresh_hz=0'
            'android_max_refresh_hz=0'
            'decoder_health_seen=0'
            'decoder_active=0'
            'decoder_first_frame=0'
            'decoder_received_frames=0'
            'decoder_queued_frames=0'
            'decoder_dropped_frames=0'
            'decoder_errors=0'
        )
        $temporaryStatusFile = "$StatusFile.script.tmp"
        [IO.File]::WriteAllLines(
            $temporaryStatusFile,
            $statusLines,
            [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporaryStatusFile -Destination $StatusFile -Force
    }
}

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
if ($Transport -eq 'Adb' -and -not $adb) {
    throw 'adb.exe was not found. Install Android SDK Platform Tools or set ANDROID_HOME.'
}

if (($StreamWidth -eq 0) -xor ($StreamHeight -eq 0)) {
    throw 'StreamWidth and StreamHeight must be set together, or both left at zero for the native desktop size.'
}
if ($DevicePreview -and $Transport -ne 'Adb') {
    throw 'DevicePreview currently uses the stable ADB connection and is unavailable on WinUsb.'
}
if ($DeviceProfileProbe -and $Transport -ne 'Adb') {
    throw 'DeviceProfileProbe currently uses the stable ADB connection.'
}
if ($StreamWidth -ne 0 -and (($StreamWidth % 2) -ne 0 -or ($StreamHeight % 2) -ne 0)) {
    throw 'StreamWidth and StreamHeight must both be even for H.264 NV12.'
}

$hostExecutable = Join-Path $repoRoot 'build\windows-nmake\windows\host\wireview_host.exe'
if (-not (Test-Path -LiteralPath $hostExecutable)) {
    throw 'wireview_host.exe is missing. Run scripts\build-windows.ps1 first.'
}

function Assert-CaptureDisplayAttached {
    if (-not $DisplayDevice) {
        return
    }
    $displayList = @(& $hostExecutable --list-displays 2>&1)
    if ($LASTEXITCODE -ne 0) {
        throw 'WireView could not enumerate Windows capture displays.'
    }
    $displayPattern = '^\s+' + [regex]::Escape($DisplayDevice) + '\s+\|'
    if (-not ($displayList | Where-Object { $_ -match $displayPattern })) {
        throw "Selected display $DisplayDevice is no longer attached. WireView stopped instead of falling back to primary or creating another VDD."
    }
}

function Get-AuthorizedSerial {
    $serials = @(
        & $adb devices |
            Select-Object -Skip 1 |
            Where-Object { $_ -match "\tdevice$" } |
            ForEach-Object { ($_ -split "\t")[0] }
    )
    if ($serials.Count -eq 1) {
        return $serials[0]
    }
    if ($serials.Count -gt 1) {
        Write-Warning 'WireView waits for exactly one authorized Android device.'
    }
    return $null
}

$deadline = [DateTime]::UtcNow.AddMinutes($DurationMinutes)
$logDirectory = Join-Path $env:TEMP 'WireView'
New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
$script:ReconnectCount = 0
if ($DeviceProfileProbe) {
    Publish-Status 'WireView is identifying the connected Android display.' 'identifying_device'
} elseif ($DevicePreview) {
    Publish-Status 'WireView is starting the device aspect-ratio preview.' 'starting_preview'
} elseif ($EnableInput) {
    Publish-Status 'WireView is starting with tablet input enabled.' 'starting'
} else {
    Publish-Status 'WireView is starting in safe mirror mode.' 'starting'
}
Write-Host 'WireView is running. Keep this window open; press Ctrl+C to stop.'

while ([DateTime]::UtcNow -lt $deadline) {
    if (-not $DevicePreview -and -not $DeviceProfileProbe) {
        Assert-CaptureDisplayAttached
    }
    if ($Transport -eq 'Adb') {
        $serial = Get-AuthorizedSerial
        if (-not $serial) {
            Publish-Status 'Waiting for one authorized USB Android device...' 'waiting_usb'
            Start-Sleep -Seconds 1
            continue
        }

        $packagePath = (& $adb -s $serial shell pm path com.wireview.receiver).Trim()
        if (-not $packagePath.StartsWith('package:')) {
            throw 'WireView is not installed on the connected device. Run the Android project from Android Studio once.'
        }

        & $adb -s $serial reverse tcp:39871 tcp:39871 | Out-Null
        & $adb -s $serial shell am start -n 'com.wireview.receiver/.MainActivity' | Out-Null
        Start-Sleep -Milliseconds 800
    }

    $remainingSeconds = [math]::Ceiling(($deadline - [DateTime]::UtcNow).TotalSeconds)
    $sessionSeconds = [math]::Min(1800, [math]::Max(1, $remainingSeconds))
    $hostArguments = if ($DeviceProfileProbe) {
        @('--probe-device-profile', '--status-file', $StatusFile)
    } elseif ($DevicePreview) {
        @('--device-preview', $sessionSeconds)
    } else {
        @('--stream-desktop', $sessionSeconds)
    }
    if ($Transport -eq 'WinUsb' -and -not $DevicePreview) {
        $hostArguments += @('--transport', 'winusb')
    }
    if (-not $EnableInput -and -not $DevicePreview -and -not $DeviceProfileProbe) {
        $hostArguments += '--input-log-only'
    }
    if (-not $DeviceProfileProbe -and $StreamWidth -ne 0) {
        $hostArguments += @('--size', "${StreamWidth}x${StreamHeight}")
    }
    if (-not $DeviceProfileProbe -and $StreamFrameRate -ne 60) {
        $hostArguments += @('--frame-rate', $StreamFrameRate)
    }
    if ($DisplayDevice -and -not $DevicePreview -and -not $DeviceProfileProbe) {
        $hostArguments += @('--display', $DisplayDevice)
    }
    if ($UseParsecVdd -and -not $DevicePreview) {
        if ($DisplayDevice) {
            throw 'UseParsecVdd creates and selects a new virtual display automatically; do not also set DisplayDevice.'
        }
        $hostArguments += '--parsec-vdd'
    }
    if ($StatusFile -and -not $DevicePreview -and -not $DeviceProfileProbe) {
        $hostArguments += @('--status-file', $StatusFile, '--reconnect-count', $script:ReconnectCount)
    }
    if ($StatusFile -and $DevicePreview) {
        $hostArguments += @('--status-file', $StatusFile)
    }

    $sessionId = [guid]::NewGuid().ToString('N')
    $outputLog = Join-Path $logDirectory "mirror-$sessionId.out.log"
    $errorLog = Join-Path $logDirectory "mirror-$sessionId.err.log"
    $captureLabel = if ($UseParsecVdd) { 'a new Parsec VDD display' } elseif ($DisplayDevice) { "display $DisplayDevice" } else { 'the primary display' }
    if ($DeviceProfileProbe) {
        Publish-Status 'WireView is identifying the connected Android display.' 'identifying_device'
    } elseif ($DevicePreview) {
        $previewLabel = if ($StreamWidth -eq 0) { 'native auto full screen' } else { "${StreamWidth}x${StreamHeight}" }
        Publish-Status "WireView is waiting for a device preview of $previewLabel at $StreamFrameRate FPS; no virtual display or video stream will start." 'waiting_preview'
    } elseif ($Transport -eq 'WinUsb') {
        Publish-Status "WireView is waiting for one already-enumerated AOA accessory to stream $captureLabel at $StreamFrameRate FPS. USB mode will not be switched automatically." 'waiting_usb'
    } else {
        Publish-Status "WireView is waiting to stream $captureLabel at $StreamFrameRate FPS." 'waiting_client'
    }
    # Invoke the host in this console process group. Ctrl+C is delivered to it
    # directly, unlike Start-Process -WindowStyle Hidden which can orphan the
    # encoder when the launcher is interrupted.
    & $hostExecutable @hostArguments 1> $outputLog 2> $errorLog
    $hostExitCode = $LASTEXITCODE

    $hostOutput = Get-Content -Raw $outputLog
    $hostErrors = Get-Content -Raw $errorLog
    $summary = $hostOutput -split "`r?`n" |
        Where-Object {
            $_ -match '^(Encode result:|Latency:|Android arrival-to-Surface latency:|Clock sync:|End-to-end frame latency:|Android input:)'
        }
    if ($summary) {
        Write-Host ($summary -join "`n")
    }
    if ($DeviceProfileProbe) {
        if ($hostExitCode -ne 0) {
            Publish-Status "Device profile probe failed (host exit code $hostExitCode)." 'identification_failed'
        }
        break
    }
    if ($hostExitCode -ne 0 -and [DateTime]::UtcNow -lt $deadline) {
        ++$script:ReconnectCount
        $errorSummary = ($hostErrors -split "`r?`n" | Where-Object { $_ }) | Select-Object -Last 1
        if ($errorSummary) {
            Write-Warning "WireView session ended: $errorSummary"
            Publish-Status 'Connection interrupted. Waiting for recovery...' 'recovering'
        } else {
            Write-Warning 'WireView session ended. Waiting for USB reconnect or Android app recovery...'
            Publish-Status 'Connection interrupted. Waiting for recovery...' 'recovering'
        }
        Start-Sleep -Seconds 1
    }
}

if (-not $DeviceProfileProbe) {
    Publish-Status 'WireView session time limit reached.' 'completed'
}
