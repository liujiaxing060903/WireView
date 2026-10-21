[CmdletBinding(SupportsShouldProcess)]
param(
    [switch]$RemoveTestCertificate
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Removing the test driver requires an elevated PowerShell session.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$packageRoot = Join-Path $repoRoot 'build\idd-package\x64'
$publishedNamePath = Join-Path $packageRoot 'installed-oem-inf.txt'
$thumbprintPath = Join-Path $packageRoot 'WireViewDevelopmentTest.thumbprint'

if (-not (Test-Path -LiteralPath $publishedNamePath)) {
    throw 'The recorded published driver name is missing; refusing to guess which driver package to remove.'
}

$publishedName = [IO.File]::ReadAllText($publishedNamePath).Trim()
if ($publishedName -notmatch '^oem\d+\.inf$') {
    throw "Unexpected published driver name: $publishedName"
}

if ($PSCmdlet.ShouldProcess($publishedName, 'Remove the WireView test driver package')) {
    & pnputil.exe /delete-driver $publishedName /uninstall /force
    if ($LASTEXITCODE -ne 0) {
        throw "PnPUtil failed with exit code $LASTEXITCODE."
    }
    Remove-Item -Force -LiteralPath $publishedNamePath
}

if ($RemoveTestCertificate) {
    if (-not (Test-Path -LiteralPath $thumbprintPath)) {
        throw 'The recorded WireView test certificate thumbprint is missing.'
    }
    $thumbprint = [IO.File]::ReadAllText($thumbprintPath).Trim()
    if ($thumbprint -notmatch '^[0-9A-Fa-f]{40}$') {
        throw 'The recorded WireView test certificate thumbprint is invalid.'
    }

    foreach ($store in @('My', 'Root', 'TrustedPublisher')) {
        $certificate = Join-Path "Cert:\LocalMachine\$store" $thumbprint
        if ((Test-Path -LiteralPath $certificate) -and
            $PSCmdlet.ShouldProcess($certificate, 'Remove the WireView test certificate')) {
            Remove-Item -Force -LiteralPath $certificate
        }
    }
}
