[CmdletBinding()]
param(
    [ValidateSet('Debug')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$spikeRoot = Join-Path $repoRoot 'windows\idd-spike'
$restoreProject = Join-Path $spikeRoot 'WdkPackages.csproj'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$vsRoot = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsRoot) {
    throw 'Visual Studio with MSBuild and the C++ toolchain was not found.'
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$msbuild = Join-Path $vsRoot 'MSBuild\Current\Bin\MSBuild.exe'

Write-Host 'Restoring the signed Microsoft WDK package...'
& dotnet restore $restoreProject --nologo
if ($LASTEXITCODE -ne 0) {
    throw "WDK package restore failed with exit code $LASTEXITCODE."
}

# Import the x64 developer environment into this PowerShell process. Calling
# MSBuild directly avoids stale multi-node compiler processes observed on some
# Visual Studio 2022 installations.
$environmentLines = & $env:COMSPEC /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE."
}

foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

$commonArguments = @(
    '/m:1',
    '/nr:false',
    "/p:Configuration=$Configuration",
    '/p:Platform=x64',
    '/p:WindowsTargetPlatformVersion=10.0.26100.0',
    '/p:TrackFileAccess=false',
    '/v:minimal'
)

$projects = @(
    (Join-Path $spikeRoot 'IddSampleDriver\IddSampleDriver.vcxproj'),
    (Join-Path $spikeRoot 'IddSampleApp\IddSampleApp.vcxproj')
)

foreach ($project in $projects) {
    Write-Host "Building $([IO.Path]::GetFileName($project))..."
    & $msbuild $project @commonArguments
    if ($LASTEXITCODE -ne 0) {
        throw "$project failed with exit code $LASTEXITCODE."
    }
}

Write-Host 'IddCx spike build completed.'
