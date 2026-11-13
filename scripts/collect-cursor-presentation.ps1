[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Serial,
    [Parameter(Mandatory)][string]$HostDirectory,
    [Parameter(Mandatory)][int]$ControlPid,
    [int]$WaitSeconds = 600,
    [int]$CaptureSeconds = 120,
    [switch]$RecordPolls
)
$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $env:LOCALAPPDATA 'Android/Sdk/platform-tools/adb.exe'
$hostExecutable = Join-Path (Resolve-Path -LiteralPath $HostDirectory).Path 'wireview_host.exe'
$controlProcess = Get-Process -Id $ControlPid
$deadline = [DateTime]::UtcNow.AddSeconds($WaitSeconds)
$streamProcess = $null
while (-not $controlProcess.HasExited -and [DateTime]::UtcNow -lt $deadline) {
    $candidate = Get-CimInstance Win32_Process -Filter "Name='wireview_host.exe'" |
        Where-Object { $_.ExecutablePath -eq $hostExecutable -and $_.CommandLine -match '--stream-desktop' } |
        Select-Object -First 1
    if ($candidate) { $streamProcess = Get-Process -Id $candidate.ProcessId; break }
    Start-Sleep -Milliseconds 500
}
if (-not $streamProcess) { throw 'No desktop stream started during the measurement window' }
$outputDirectory = Join-Path $workspace 'build/latency-comparison/measurements'
New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
$output = Join-Path $outputDirectory ("surface-" + $streamProcess.Id + '.txt')
$writer = [IO.StreamWriter]::new($output, $false, [Text.UTF8Encoding]::new($false))
$pollWriter = if ($RecordPolls) {
    $path = Join-Path $outputDirectory ("surface-polls-" + $streamProcess.Id + '.csv')
    [IO.StreamWriter]::new($path, $false, [Text.UTF8Encoding]::new($false))
}
if ($pollWriter) { $pollWriter.WriteLine('poll_end_utc,read_ms,reported_period_ns,first_actual_ns,last_actual_ns,new_rows') }
$seen = [Collections.Generic.HashSet[string]]::new()
$script:videoLayer = $null
function Save-Presentation {
    if (-not $script:videoLayer) {
        $layers = @(& $adb -s $Serial shell dumpsys SurfaceFlinger --list)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate video presentation layers' }
        $script:videoLayer = $layers | Where-Object { $_ -like 'SurfaceView*com.wireview.receiver*(BLAST)*' } | Select-Object -First 1
    }
    if (-not $script:videoLayer) { return }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $raw = @(& $adb -s $Serial shell "dumpsys SurfaceFlinger --latency '$script:videoLayer'")
    $timer.Stop()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read compositor presentation times' }
    if ($raw.Count -lt 3) { $script:videoLayer = $null; return }
    $firstActual = 0L
    $lastActual = 0L
    $newRows = 0
    foreach ($line in $raw) {
        $parts = $line.Trim() -split '\s+'
        if ($parts.Count -eq 3 -and $parts[0] -match '^\d+$' -and $parts[1] -match '^\d+$') {
            $actual = [long]$parts[1]
            if ($actual -gt 0 -and $actual -lt [long]::MaxValue) {
                if ($firstActual -eq 0 -or $actual -lt $firstActual) { $firstActual = $actual }
                if ($actual -gt $lastActual) { $lastActual = $actual }
                if ($seen.Add($line)) { $writer.WriteLine($line); ++$newRows }
            }
        }
    }
    $writer.Flush()
    if ($pollWriter) {
        $pollWriter.WriteLine(('{0:o},{1},{2},{3},{4},{5}' -f [DateTime]::UtcNow,
            $timer.ElapsedMilliseconds, $raw[0].Trim(), $firstActual, $lastActual, $newRows))
        $pollWriter.Flush()
    }
}
try {
    Write-Output "Capturing actual compositor timestamps: $output"
    $captureDeadline = [DateTime]::UtcNow.AddSeconds($CaptureSeconds)
    while (-not $streamProcess.HasExited -and [DateTime]::UtcNow -lt $captureDeadline) {
        Save-Presentation
        Start-Sleep -Milliseconds 500
    }
    Save-Presentation
} finally {
    $writer.Dispose()
    if ($pollWriter) { $pollWriter.Dispose() }
}
Write-Output 'Presentation capture completed'
