[CmdletBinding()]
param(
    [ValidateRange(0, 3600)]
    [int]$StartDurationSeconds = 0
)

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -ne 'Desktop') {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -StartDurationSeconds $StartDurationSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "Windows PowerShell install process failed with exit code $LASTEXITCODE."
    }
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Installing the test driver requires an elevated PowerShell session.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'build\idd-package\x64'
$driverInf = Join-Path $packageRoot 'driver\WireViewIddDriver.inf'
$publishedNamePath = Join-Path $packageRoot 'installed-oem-inf.txt'
$installLog = Join-Path $packageRoot 'install-transcript.log'

Start-Transcript -Path $installLog -Force | Out-Null

if (-not (Test-Path -LiteralPath $driverInf)) {
    throw 'The driver package is missing. Run scripts\package-idd-spike.ps1 first.'
}

& (Join-Path $PSScriptRoot 'sign-idd-spike.ps1')
if ($LASTEXITCODE -ne 0) {
    throw "Driver signing failed with exit code $LASTEXITCODE."
}

$installOutput = @(& pnputil.exe /add-driver $driverInf /install 2>&1)
$installExitCode = $LASTEXITCODE
$installOutput | Write-Host
if ($installExitCode -ne 0) {
    throw "PnPUtil failed with exit code $installExitCode."
}

$publishedName = [regex]::Match(($installOutput -join "`n"), 'oem\d+\.inf', 'IgnoreCase').Value
if ($publishedName) {
    [IO.File]::WriteAllText($publishedNamePath, $publishedName.ToLowerInvariant())
    Write-Host "Published driver name: $publishedName"
}

Write-Host 'WireView IddCx driver package is staged. Run scripts\start-idd-spike.ps1 to create the temporary display.'

if ($StartDurationSeconds -gt 0) {
    $launcher = Join-Path $packageRoot 'WireViewIddLauncher.exe'
    Write-Host "Starting the WireView test display for $StartDurationSeconds seconds..."
    & $launcher --duration-seconds $StartDurationSeconds
    if ($LASTEXITCODE -ne 0) {
        throw "WireViewIddLauncher failed with exit code $LASTEXITCODE."
    }
}

Stop-Transcript | Out-Null
