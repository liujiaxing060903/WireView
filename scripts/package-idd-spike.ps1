[CmdletBinding()]
param(
    [ValidateSet('Debug')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$spikeRoot = Join-Path $repoRoot 'windows\idd-spike'
$driverRoot = Join-Path $spikeRoot 'IddSampleDriver'
$launcherRoot = Join-Path $spikeRoot 'IddSampleApp'
$packageRoot = Join-Path $repoRoot 'build\idd-package\x64'
$driverPackage = Join-Path $packageRoot 'driver'
$packagedLauncher = Join-Path $packageRoot 'WireViewIddLauncher.exe'

& (Join-Path $PSScriptRoot 'build-idd-spike.ps1') -Configuration $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "IddCx build failed with exit code $LASTEXITCODE."
}

$driverBinary = Join-Path $driverRoot "x64\$Configuration\WireViewIddDriver.dll"
$launcherBinary = Join-Path $launcherRoot "x64\$Configuration\WireViewIddLauncher.exe"
$driverInf = Join-Path $driverRoot 'IddSampleDriver.inf'

foreach ($requiredFile in @($driverBinary, $launcherBinary, $driverInf)) {
    if (-not (Test-Path -LiteralPath $requiredFile)) {
        throw "Expected build output was not found: $requiredFile"
    }
}

New-Item -ItemType Directory -Force -Path $driverPackage | Out-Null
Copy-Item -Force -LiteralPath $driverBinary -Destination (Join-Path $driverPackage 'WireViewIddDriver.dll')
Copy-Item -Force -LiteralPath $driverInf -Destination (Join-Path $driverPackage 'WireViewIddDriver.inf')
Copy-Item -Force -LiteralPath $launcherBinary -Destination $packagedLauncher

$catalog = Join-Path $driverPackage 'WireViewIddDriver.cat'
if (Test-Path -LiteralPath $catalog) {
    Remove-Item -Force -LiteralPath $catalog
}

$nugetRoot = if ($env:NUGET_PACKAGES) {
    $env:NUGET_PACKAGES
} elseif ($env:USERPROFILE) {
    Join-Path $env:USERPROFILE '.nuget\packages'
} else {
    throw 'Unable to locate the NuGet package cache.'
}

$wdkRoot = Join-Path $nugetRoot 'microsoft.windows.wdk.x64\10.0.26100.6584\c'
$infVerifier = Join-Path $wdkRoot 'tools\10.0.26100.0\x64\infverif.exe'
$inf2Cat = Join-Path $wdkRoot 'bin\10.0.26100.0\x86\Inf2Cat.exe'

foreach ($tool in @($infVerifier, $inf2Cat)) {
    if (-not (Test-Path -LiteralPath $tool)) {
        throw "Required WDK packaging tool was not found: $tool"
    }
}

Write-Host 'Validating WireViewIddDriver.inf...'
& $infVerifier /w (Join-Path $driverPackage 'WireViewIddDriver.inf')
if ($LASTEXITCODE -ne 0) {
    throw "INF validation failed with exit code $LASTEXITCODE."
}

Write-Host 'Generating WireViewIddDriver.cat...'
& $inf2Cat "/driver:$driverPackage" '/os:10_X64' '/verbose'
if ($LASTEXITCODE -ne 0) {
    throw "Catalog generation failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path -LiteralPath $catalog)) {
    throw "Inf2Cat completed but did not create the expected catalog: $catalog"
}

Write-Host "Unsigned WireView IddCx package: $driverPackage"
Write-Host "Software-device launcher: $packagedLauncher"
