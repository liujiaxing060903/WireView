[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

$workspace = Split-Path -Parent $PSScriptRoot
$output = Join-Path $workspace 'build\java-protocol-test'
$protocolSource = Join-Path $workspace 'android\app\src\main\java\com\wireview\receiver\Protocol.java'
$testSource = Join-Path $workspace 'android\protocol-test\com\wireview\receiver\ProtocolSelfTest.java'
$transportSources = @(
    (Join-Path $workspace 'android\app\src\main\java\com\wireview\receiver\TransportConnection.java'),
    (Join-Path $workspace 'android\app\src\main\java\com\wireview\receiver\TransportConnector.java'),
    (Join-Path $workspace 'android\app\src\main\java\com\wireview\receiver\TcpTransportConnector.java')
)
$transportTestSource = Join-Path $workspace 'android\protocol-test\com\wireview\receiver\TransportSelfTest.java'
$diagnosticSource = Join-Path $workspace 'android\app\src\main\java\com\wireview\receiver\PersistentDiagnosticLog.java'
$diagnosticTestSource = Join-Path $workspace 'android\protocol-test\com\wireview\receiver\PersistentDiagnosticLogSelfTest.java'

New-Item -ItemType Directory -Path $output -Force | Out-Null

javac -encoding UTF-8 -d $output $protocolSource $testSource $transportSources `
    $transportTestSource $diagnosticSource $diagnosticTestSource
if ($LASTEXITCODE -ne 0) {
    throw 'javac failed'
}

java -cp $output com.wireview.receiver.ProtocolSelfTest
if ($LASTEXITCODE -ne 0) {
    throw 'Java protocol self-test failed'
}

java -cp $output com.wireview.receiver.TransportSelfTest
if ($LASTEXITCODE -ne 0) {
    throw 'Java transport self-test failed'
}

java -cp $output com.wireview.receiver.PersistentDiagnosticLogSelfTest
if ($LASTEXITCODE -ne 0) {
    throw 'Java persistent diagnostic log self-test failed'
}
