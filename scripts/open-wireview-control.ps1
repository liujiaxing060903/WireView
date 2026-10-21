[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$controlExecutable = Join-Path $repoRoot 'build\windows-nmake\windows\host\wireview_control.exe'
if (-not (Test-Path -LiteralPath $controlExecutable)) {
    throw 'wireview_control.exe is missing. Run scripts\build-windows.ps1 first.'
}

Start-Process -FilePath $controlExecutable -WorkingDirectory $repoRoot -WindowStyle Normal
