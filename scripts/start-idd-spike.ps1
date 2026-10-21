[CmdletBinding()]
param(
    [ValidateRange(0, 3600)]
    [int]$DurationSeconds = 0
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -DurationSeconds $DurationSeconds"
    $startParameters = @{
        FilePath = $windowsPowerShell
        ArgumentList = $arguments
        Verb = 'RunAs'
        Wait = $true
        PassThru = $true
    }
    if ($DurationSeconds -gt 0) {
        $startParameters.WindowStyle = 'Hidden'
    }
    $process = Start-Process @startParameters
    if ($process.ExitCode -ne 0) {
        throw "Elevated WireView launcher failed with exit code $($process.ExitCode)."
    }
    return
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$launcher = Join-Path $repoRoot 'build\idd-package\x64\WireViewIddLauncher.exe'
if (-not (Test-Path -LiteralPath $launcher)) {
    throw 'The WireView launcher is missing. Run scripts\package-idd-spike.ps1 first.'
}

$launcherLog = Join-Path $repoRoot 'build\idd-package\x64\launcher-transcript.log'
Start-Transcript -Path $launcherLog -Force | Out-Null

Write-Host 'The virtual display exists while this launcher remains open.'
if ($DurationSeconds -gt 0) {
    Write-Host "The test display will be removed automatically after $DurationSeconds seconds."
    & $launcher --duration-seconds $DurationSeconds
} else {
    Write-Host "Press 'x' in the launcher to remove it."
    & $launcher
}
if ($LASTEXITCODE -ne 0) {
    throw "WireViewIddLauncher failed with exit code $LASTEXITCODE."
}
Stop-Transcript | Out-Null
