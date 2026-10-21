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

$visualStudio = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
$windowsKits = 'C:\Program Files (x86)\Windows Kits\10'
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
Write-ToolStatus -Name 'Windows SDK' -Path "$windowsKits\Include\10.0.26100.0\um\Windows.h"
Write-ToolStatus -Name 'WDK IddCx header' -Path "$windowsKits\Include\10.0.26100.0\um\iddcx.h"
Write-ToolStatus -Name 'WDK build integration' -Path "$visualStudio\MSBuild\Microsoft\VC\v170\Platforms\x64\PlatformToolsets\WindowsUserModeDriver10.0"
Write-ToolStatus -Name 'Android SDK' -Path "$androidSdk\platforms"
Write-ToolStatus -Name 'Android platform-tools' -Path "$androidSdk\platform-tools\adb.exe"
Write-ToolStatus -Name 'Android cmdline-tools' -Path "$androidSdk\cmdline-tools\latest\bin\sdkmanager.bat"

