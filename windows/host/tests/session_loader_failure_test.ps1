param(
    [Parameter(Mandatory)][string]$Session,
    [Parameter(Mandatory)][string]$AdbFixture,
    [Parameter(Mandatory)][string]$ApiFixture
)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('wireview-loader-' + [guid]::NewGuid().ToString('N'))
$previousEnvironment = @{}
$sessionProcess = $null
try {
    New-Item -ItemType Directory -Path $fixture -Force | Out-Null
    Copy-Item -LiteralPath $Session -Destination (Join-Path $fixture 'wireview_session.exe')
    Copy-Item -LiteralPath $AdbFixture -Destination (Join-Path $fixture 'adb.exe')
    Copy-Item -LiteralPath $ApiFixture -Destination (Join-Path $fixture 'AdbWinApi.dll')
    [IO.File]::WriteAllBytes((Join-Path $fixture 'AdbWinUsbApi.dll'), [byte[]]@(0))
    foreach ($name in @('ANDROID_HOME','ANDROID_SDK_ROOT','LOCALAPPDATA','PATH')) {
        $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $fixture, 'Process')
    }
    function Test-Probe([string]$Case, [int]$Expected) {
        $diagnostics = Join-Path $fixture ($Case + '.ini')
        $watch = [Diagnostics.Stopwatch]::StartNew()
        $script:sessionProcess = Start-Process -FilePath (Join-Path $fixture 'wireview_session.exe') `
            -ArgumentList ('--detect-device --diagnostics-file "' + $diagnostics + '"') `
            -WindowStyle Hidden -PassThru
        $null = $script:sessionProcess.Handle
        if (-not $script:sessionProcess.WaitForExit(5000)) {
            throw "$Case blocked on loader error instead of exiting promptly"
        }
        if ($script:sessionProcess.ExitCode -ne $Expected) {
            throw "$Case expected exit $Expected, got $($script:sessionProcess.ExitCode)"
        }
        $text = Get-Content -LiteralPath $diagnostics -Raw
        if ($text -notmatch 'diagnostics=adb=' -or ($Expected -eq 9 -and $text -notmatch 'exit=')) {
            throw "$Case lost the selected path or original process error"
        }
        Write-Host "$Case returned $Expected in $($watch.ElapsedMilliseconds) ms"
    }
    Test-Probe 'complete-no-phone' 3
    Remove-Item -LiteralPath (Join-Path $fixture 'AdbWinApi.dll')
    Test-Probe 'missing-api-first' 7
    Test-Probe 'missing-api-second' 7
    Test-Probe 'missing-api-third' 7
    [IO.File]::WriteAllBytes((Join-Path $fixture 'AdbWinApi.dll'), [byte[]]@(0))
    Test-Probe 'corrupt-api-loader-error' 9
}
finally {
    if ($script:sessionProcess -and -not $script:sessionProcess.HasExited) {
        # Kill only this fixture's child if an actual loader dialog blocked it.
        Get-CimInstance Win32_Process -Filter "ParentProcessId = $($script:sessionProcess.Id)" |
            Where-Object { $_.ExecutablePath -eq (Join-Path $fixture 'adb.exe') } |
            ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
        Stop-Process -Id $script:sessionProcess.Id -Force
    }
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith('wireview-loader-')) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    } else { throw 'Refusing to remove a fixture outside the temporary directory' }
}
