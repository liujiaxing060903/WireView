[CmdletBinding()]
param(
    [string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$adbCandidates = [Collections.Generic.List[string]]::new()
if ($env:ANDROID_HOME) {
    $adbCandidates.Add((Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'))
}
if ($env:ANDROID_SDK_ROOT) {
    $adbCandidates.Add((Join-Path $env:ANDROID_SDK_ROOT 'platform-tools\adb.exe'))
}
if ($env:LOCALAPPDATA) {
    $adbCandidates.Add((Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe'))
}
$adb = $adbCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $adb) {
    throw 'adb.exe was not found. Install Android SDK Platform Tools or set ANDROID_HOME.'
}

$serials = @(
    & $adb devices |
        Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" } |
        ForEach-Object { ($_ -split "\t")[0] }
)
if ($serials.Count -ne 1) {
    throw "Exactly one authorized Android device is required; found $($serials.Count)."
}
$serial = $serials[0]

if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $repoRoot 'build\android-diagnostics'
}
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outputFile = Join-Path $OutputDirectory "wireview-usb-$timestamp.log"

$sections = [Collections.Generic.List[string]]::new()
foreach ($name in @('usb-diagnostic.previous.log', 'usb-diagnostic.log')) {
    $savedErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $content = @(& $adb -s $serial shell run-as com.wireview.receiver cat "files/$name" 2>&1)
    $catExitCode = $LASTEXITCODE
    $ErrorActionPreference = $savedErrorActionPreference
    if ($catExitCode -eq 0) {
        $sections.Add("===== $name =====")
        foreach ($line in $content) {
            $sections.Add([string]$line)
        }
    }
}
if ($sections.Count -eq 0) {
    throw 'No WireView diagnostic log was available. Install and start the Debug APK first.'
}
[IO.File]::WriteAllLines($outputFile, $sections, [Text.UTF8Encoding]::new($false))
Write-Host "WireView Android diagnostics: $outputFile"
