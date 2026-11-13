[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Serial,
    [string]$OutputPrefix = 'build/fps-presentation'
)

$ErrorActionPreference = 'Stop'
$workspace = Split-Path -Parent $PSScriptRoot
$adb = Join-Path $env:LOCALAPPDATA 'Android/Sdk/platform-tools/adb.exe'
$prefix = Join-Path $workspace $OutputPrefix
New-Item -ItemType Directory -Path (Split-Path -Parent $prefix) -Force | Out-Null
$layers = @(& $adb -s $Serial shell dumpsys SurfaceFlinger --list)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read SurfaceFlinger layers' }
$layer = $layers | Where-Object { $_ -like 'SurfaceView*com.wireview.receiver*(BLAST)*' } | Select-Object -First 1
if (-not $layer) { throw 'WireView video layer not found; start the stream first' }
$raw = @(& $adb -s $Serial shell "dumpsys SurfaceFlinger --latency '$layer'")
if ($LASTEXITCODE -ne 0) { throw 'Cannot read video presentation timestamps' }
$raw | Set-Content -LiteralPath "$prefix.txt"
$present = @($raw | ForEach-Object {
    $parts = $_.Trim() -split '\s+'
    if ($parts.Count -eq 3 -and $parts[1] -match '^\d+$') {
        $value = [long]$parts[1]
        if ($value -gt 0 -and $value -lt [long]::MaxValue) { $value }
    }
})
if ($present.Count -lt 2) { throw 'Presentation samples not ready; keep video playing' }
$intervals = @(for ($i = 1; $i -lt $present.Count; ++$i) {
    ($present[$i] - $present[$i - 1]) / 1000000.0
})
$sorted = @($intervals | Sort-Object)
$result = [pscustomobject]@{
    Layer = $layer
    Samples = $present.Count
    RefreshPeriodNs = [long]$raw[0]
    PresentFPS = [math]::Round(($present.Count - 1) * 1000000000.0 / ($present[-1] - $present[0]), 2)
    P50ms = [math]::Round($sorted[[int](($sorted.Count - 1) * 0.5)], 3)
    P95ms = [math]::Round($sorted[[int](($sorted.Count - 1) * 0.95)], 3)
}
$result | ConvertTo-Json | Set-Content -LiteralPath "$prefix.json"
$result
# This is compositor presentation, not proof that all pictures are distinct.
# Keep the same 120-cell video/photo comparison for the final display check.
