[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$workspace = Split-Path -Parent $PSScriptRoot
$output = Join-Path $workspace 'build\java-protocol-test'
$sourceRoot = Join-Path $workspace 'android\app\src\main\java\com\wireview\receiver'
$sources = @(
    'Protocol', 'TransportConnection', 'TransportConnector', 'TcpTransportConnector',
    'PersistentDiagnosticLog', 'PacketReadTrace', 'DecoderOutputPump', 'DecoderRecoveryState',
    'FramePresentationClock', 'SurfaceFrameMarker', 'ConnectionStatus'
) | ForEach-Object { Join-Path $sourceRoot ($_.ToString() + '.java') }
$testRoot = Join-Path $workspace 'android\protocol-test'
$tests = @(
    'ProtocolSelfTest', 'TransportSelfTest', 'PersistentDiagnosticLogSelfTest',
    'DecoderOutputPumpSelfTest', 'DecoderRecoverySelfTest', 'FramePresentationClockSelfTest',
    'SurfaceFrameMarkerSelfTest', 'ConnectionStatusSelfTest'
)
$testSources = $tests | ForEach-Object { Join-Path $testRoot ('com\wireview\receiver\' + $_ + '.java') }
$platformStubs = @(Get-ChildItem -LiteralPath (Join-Path $testRoot 'stubs') -Filter '*.java' -Recurse |
    ForEach-Object { $_.FullName })

New-Item -ItemType Directory -Path $output -Force | Out-Null

javac -encoding UTF-8 -d $output $platformStubs $sources $testSources
if ($LASTEXITCODE -ne 0) {
    throw 'javac failed'
}

foreach ($test in $tests) {
    java -cp $output ('com.wireview.receiver.' + $test)
    if ($LASTEXITCODE -ne 0) { throw "Java self-test failed: $test" }
}
