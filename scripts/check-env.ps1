[CmdletBinding()]
param()

$ErrorActionPreference = 'SilentlyContinue'

function Write-ToolStatus {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Name,
        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if (Test-Path -LiteralPath $Path) {
        Write-Output ("[OK]      {0}: {1}" -f $Name, $Path)
    }
    else {
        Write-Output ("[MISSING] {0}: {1}" -f $Name, $Path)
    }
}

$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = $null
if (Test-Path -LiteralPath $vswherePath) {
    $visualStudio = @(& $vswherePath -latest -property installationPath)[0]
}
if ($visualStudio) {
    Write-Output ("[OK]      {0}: {1}" -f 'Visual Studio', $visualStudio)
}
else {
    $visualStudio = '(no Visual Studio installation reported by vswhere)'
    Write-Output ("[MISSING] {0}: {1}" -f 'Visual Studio', $visualStudio)
}

$windowsKits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$latestSdkInclude = Get-ChildItem -LiteralPath (Join-Path $windowsKits 'Include') -Directory |
    Sort-Object Name -Descending |
    Select-Object -First 1
$sdkUmDirectory = if ($latestSdkInclude) {
    Join-Path $latestSdkInclude.FullName 'um'
}
else {
    Join-Path $windowsKits 'Include (no Windows SDK version found)'
}

$androidSdk = if ($env:ANDROID_SDK_ROOT) {
    $env:ANDROID_SDK_ROOT
}
elseif ($env:ANDROID_HOME) {
    $env:ANDROID_HOME
}
else {
    Join-Path $env:LOCALAPPDATA 'Android\Sdk'
}

Write-ToolStatus -Name 'MSBuild' -Path "$visualStudio\MSBuild\Current\Bin\MSBuild.exe"
Write-ToolStatus -Name 'MSVC' -Path "$visualStudio\VC\Tools\MSVC"
Write-ToolStatus -Name 'Windows SDK' -Path "$sdkUmDirectory\Windows.h"
Write-ToolStatus -Name 'WDK IddCx header' -Path "$sdkUmDirectory\iddcx.h"
Write-ToolStatus -Name 'WDK build integration' -Path "$visualStudio\MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsUserModeDriver10.0"
Write-ToolStatus -Name 'Android SDK' -Path "$androidSdk\platforms"
Write-ToolStatus -Name 'Android platform-tools' -Path "$androidSdk\platform-tools\adb.exe"
Write-ToolStatus -Name 'Android cmdline-tools' -Path "$androidSdk\cmdline-tools\latest\bin\sdkmanager.bat"

