[CmdletBinding()]
param(
    [string]$BuildDirectory,
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) {
    throw 'Visual Studio with the C++ toolchain was not found.'
}

$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$environmentLines = & $env:COMSPEC /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvars64.bat failed with exit code $LASTEXITCODE."
}

$developerPath = $null
foreach ($line in $environmentLines) {
    $separator = $line.IndexOf('=')
    if ($separator -gt 0) {
        $name = $line.Substring(0, $separator)
        $value = $line.Substring($separator + 1)
        if ($name -ieq 'PATH') {
            if ($value -match '\\VC\\Tools\\MSVC\\') {
                $developerPath = $value
            }
            continue
        }
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}

# Codex can provide both PATH and Path entries to child cmd.exe processes.
# vcvars64 updates the former while the latter retains the old value, so choose
# the developer path explicitly instead of letting the last duplicate win.
if (-not $developerPath) {
    throw 'vcvars64.bat did not return a Visual C++ developer PATH.'
}
$env:Path = $developerPath

if (-not (Get-Command nmake.exe -ErrorAction SilentlyContinue)) {
    throw 'nmake.exe was not available after loading the Visual C++ environment.'
}

Push-Location $repoRoot
try {
    if ($BuildDirectory -or $Configuration -eq 'Release') {
        $resolvedBuildDirectory = if ($BuildDirectory -and [IO.Path]::IsPathRooted($BuildDirectory)) {
            $BuildDirectory
        } elseif ($BuildDirectory) {
            Join-Path $repoRoot $BuildDirectory
        } else {
            Join-Path $repoRoot ('build\windows-' + $Configuration.ToLowerInvariant())
        }
        & cmake -S $repoRoot -B $resolvedBuildDirectory -G 'NMake Makefiles' `
            "-DCMAKE_BUILD_TYPE=$Configuration" -DBUILD_TESTING=ON
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed with exit code $LASTEXITCODE."
        }
        & cmake --build $resolvedBuildDirectory
        if ($LASTEXITCODE -ne 0) {
            throw "Windows build failed with exit code $LASTEXITCODE."
        }
        & ctest --test-dir $resolvedBuildDirectory --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "Windows tests failed with exit code $LASTEXITCODE."
        }
    } else {
        & cmake --preset windows-debug
        if ($LASTEXITCODE -ne 0) {
            throw "CMake configure failed with exit code $LASTEXITCODE."
        }

        & cmake --build --preset windows-debug
        if ($LASTEXITCODE -ne 0) {
            throw "Windows build failed with exit code $LASTEXITCODE."
        }

        & ctest --preset windows-debug
        if ($LASTEXITCODE -ne 0) {
            throw "Windows tests failed with exit code $LASTEXITCODE."
        }
    }
}
finally {
    Pop-Location
}

Write-Host "WireView Windows $Configuration build and tests completed."
