param([Parameter(Mandatory)][string]$Script)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('wireview-adb-dependency-' + [guid]::NewGuid().ToString('N'))
$previousSdk = $env:ANDROID_HOME
try {
    $tools = Join-Path $fixture 'platform-tools'
    New-Item -ItemType Directory -Path $tools -Force | Out-Null
    # If the script actually tries to launch this invalid executable, the test
    # will fail with a different error. No real SDK/driver/device is modified.
    [IO.File]::WriteAllBytes((Join-Path $tools 'adb.exe'), [byte[]]@(0))
    $env:ANDROID_HOME = $fixture
    $stdout = Join-Path $fixture 'stdout.log'
    $stderr = Join-Path $fixture 'stderr.log'
    $arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $Script + '" -Transport Adb -DurationMinutes 1'
    $process = Start-Process powershell.exe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    if (-not $process.WaitForExit(5000)) {
        Stop-Process -Id $process.Id -Force
        throw 'Missing ADB dependencies caused retry/hang instead of failing fast'
    }
    $text = (Get-Content -LiteralPath $stdout,$stderr -Raw) -join "`n"
    if ($process.ExitCode -eq 0 -or $text -notmatch 'ADB dependency missing: AdbWinApi.dll') {
        throw "Missing-DLL preflight was not enforced before child launch: $text"
    }
    Write-Host 'Real PowerShell entry point rejects missing ADB DLL before launch.'
}
finally {
    $env:ANDROID_HOME = $previousSdk
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if ($resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
        [IO.Path]::GetFileName($resolved).StartsWith('wireview-adb-dependency-')) {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    } else { throw 'Refusing to remove a fixture outside the temporary test directory' }
}
