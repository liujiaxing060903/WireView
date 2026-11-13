[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual C++ toolchain not found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$lines = & $env:COMSPEC /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Loading the Visual C++ environment failed.' }
foreach ($line in $lines) {
    $separator = $line.IndexOf('=')
    if ($separator -le 0) { continue }
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    if ($name -ieq 'PATH') {
        if ($value -match '\\VC\\Tools\\MSVC\\') { $env:Path = $value }
    } else { [Environment]::SetEnvironmentVariable($name, $value, 'Process') }
}
$objects = Join-Path $repo 'build\gray-motion'
$output = Join-Path $repo 'release\WireView-灰块测试.exe'
New-Item -ItemType Directory -Path $objects, (Split-Path -Parent $output) -Force | Out-Null
& cl.exe /nologo /std:c++17 /O2 /MT /EHsc /W4 /utf-8 /DUNICODE /D_UNICODE `
    "/I$repo\windows\host\src" "$repo\tools\gray-motion\main.cpp" `
    "/Fo$objects\main.obj" "/Fe$output" /link /SUBSYSTEM:WINDOWS d3d11.lib dxgi.lib user32.lib
if ($LASTEXITCODE -ne 0) { throw 'Gray motion build failed.' }
Get-Item -LiteralPath $output | Select-Object FullName, Length
