param(
    [Parameter(Mandatory = $true)]
    [string]$Script
)

$ErrorActionPreference = 'Stop'

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    $Script,
    [ref]$tokens,
    [ref]$parseErrors)
if ($parseErrors.Count -ne 0) {
    throw "start-wireview.ps1 has parser errors: $($parseErrors[0].Message)"
}

$transportParameter = $ast.ParamBlock.Parameters |
    Where-Object { $_.Name.VariablePath.UserPath -eq 'Transport' } |
    Select-Object -First 1
if (-not $transportParameter) {
    throw 'start-wireview.ps1 does not declare Transport.'
}
if ($transportParameter.DefaultValue.SafeGetValue() -ne 'Adb') {
    throw 'start-wireview.ps1 must keep Adb as the default transport.'
}

$validateSet = $transportParameter.Attributes |
    Where-Object { $_.TypeName.FullName -eq 'ValidateSet' } |
    Select-Object -First 1
if (-not $validateSet) {
    throw 'Transport does not have a ValidateSet attribute.'
}
$allowed = @($validateSet.PositionalArguments | ForEach-Object { $_.SafeGetValue() })
if ($allowed.Count -ne 2 -or $allowed -notcontains 'Adb' -or $allowed -notcontains 'WinUsb') {
    throw 'Transport must allow exactly Adb and WinUsb.'
}

$source = Get-Content -LiteralPath $Script -Raw
if ($source -notmatch "\`$Transport -eq 'Adb' -and -not \`$adb") {
    throw 'ADB availability is not scoped to the Adb transport.'
}
if ($source -notmatch "@\('--transport', 'winusb'\)") {
    throw 'WinUsb does not add the explicit Host transport argument.'
}

Write-Host 'WireView start script transport contract passed.'
