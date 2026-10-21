[CmdletBinding()]
param()

$ErrorActionPreference = 'Continue'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Inspecting the test display requires an elevated PowerShell session.'
}

$repoRoot = Split-Path -Parent $PSScriptRoot
$log = Join-Path $repoRoot 'build\idd-package\x64\inspect-transcript.log'
Start-Transcript -Path $log -Force | Out-Null

Write-Host '=== WireView devices ==='
Get-PnpDevice -PresentOnly |
    Where-Object { $_.FriendlyName -like '*WireView*' -or $_.InstanceId -like '*WireViewIdd*' } |
    Format-List Status, Class, FriendlyName, InstanceId, Problem, ProblemDescription

Write-Host '=== Display adapters ==='
& pnputil.exe /enum-devices /connected /class Display /deviceids

Write-Host '=== Monitors ==='
& pnputil.exe /enum-devices /connected /class Monitor /deviceids

Write-Host '=== Active WMI monitors ==='
Get-CimInstance -Namespace 'root\wmi' -ClassName WmiMonitorID |
    Select-Object InstanceName, Active |
    Format-Table -AutoSize

Write-Host '=== UMDF hosts ==='
Get-Process -Name WUDFHost -ErrorAction SilentlyContinue |
    Select-Object Id, StartTime, Path |
    Format-Table -AutoSize

Stop-Transcript | Out-Null
