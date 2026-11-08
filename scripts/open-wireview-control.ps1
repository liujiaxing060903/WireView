[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
# Locate the most recent build tree instead of hardcoding windows-nmake:
# build-windows.ps1 places Release builds in build\windows-release (or a
# caller-supplied windows-release-* directory). Probe the known candidates
# in order and use the first one that actually contains the control center.
$buildRoot = Join-Path $repoRoot 'build'
$buildCandidates = [Collections.Generic.List[string]]::new()
$buildCandidates.Add((Join-Path $buildRoot 'windows-nmake'))
$buildCandidates.Add((Join-Path $buildRoot 'windows-release'))
if (Test-Path -LiteralPath $buildRoot) {
    Get-ChildItem -LiteralPath $buildRoot -Directory -Filter 'windows-release-*' |
        Sort-Object Name |
        ForEach-Object { $buildCandidates.Add($_.FullName) }
}
$controlDirectory = $buildCandidates |
    Where-Object { Test-Path -LiteralPath (Join-Path $_ 'windows\host\wireview_control.exe') } |
    Select-Object -First 1
if (-not $controlDirectory) {
    throw 'wireview_control.exe is missing. Run scripts\build-windows.ps1 first.'
}
$controlExecutable = Join-Path $controlDirectory 'windows\host\wireview_control.exe'

Start-Process -FilePath $controlExecutable -WorkingDirectory $repoRoot -WindowStyle Normal
