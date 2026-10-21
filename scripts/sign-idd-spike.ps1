[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSEdition -ne 'Desktop') {
    $windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath
    if ($LASTEXITCODE -ne 0) {
        throw "Windows PowerShell signing process failed with exit code $LASTEXITCODE."
    }
    return
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Signing the test driver requires an elevated Windows PowerShell session.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'build\idd-package\x64'
$driverPackage = Join-Path $packageRoot 'driver'
$catalog = Join-Path $driverPackage 'WireViewIddDriver.cat'
$certificatePath = Join-Path $packageRoot 'WireViewDevelopmentTest.cer'
$thumbprintPath = Join-Path $packageRoot 'WireViewDevelopmentTest.thumbprint'

if (-not (Test-Path -LiteralPath $catalog)) {
    throw 'The catalog is missing. Run scripts\package-idd-spike.ps1 first.'
}

$subject = 'CN=WireView Development Test'
$certificate = Get-ChildItem -LiteralPath 'Cert:\LocalMachine\My' |
    Where-Object { $_.Subject -eq $subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if (-not $certificate) {
    $certificate = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject $subject `
        -FriendlyName 'WireView development driver test certificate' `
        -CertStoreLocation 'Cert:\LocalMachine\My' `
        -HashAlgorithm SHA256 `
        -KeyAlgorithm RSA `
        -KeyLength 3072 `
        -KeyExportPolicy Exportable `
        -NotAfter (Get-Date).AddYears(5)
}

Export-Certificate -Cert $certificate -FilePath $certificatePath -Force | Out-Null
Import-Certificate -FilePath $certificatePath -CertStoreLocation 'Cert:\LocalMachine\Root' | Out-Null
Import-Certificate -FilePath $certificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPublisher' | Out-Null

$kitsBin = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$signTool = Get-ChildItem -LiteralPath $kitsBin -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
    Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1

if (-not $signTool) {
    throw 'The x64 Windows SDK SignTool was not found.'
}

& $signTool sign /v /fd SHA256 /sm /s My /sha1 $certificate.Thumbprint $catalog
if ($LASTEXITCODE -ne 0) {
    throw "Catalog signing failed with exit code $LASTEXITCODE."
}

& $signTool verify /v /pa $catalog
if ($LASTEXITCODE -ne 0) {
    throw "Catalog signature verification failed with exit code $LASTEXITCODE."
}

[IO.File]::WriteAllText($thumbprintPath, $certificate.Thumbprint)
Write-Host "Signed catalog: $catalog"
Write-Host "Test certificate: $certificatePath"
Write-Host "Certificate thumbprint: $($certificate.Thumbprint)"
