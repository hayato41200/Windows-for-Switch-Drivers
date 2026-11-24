# GetKit.ps1 - fetch the newest GPU test kit from the PC over the network and
# start AutoTest, so the SD card does not have to be moved (Claude, 0.10.x).
# The PC publishes the kit to <share>\gpu\kit; AutoTest uploads the logs to
# <share>\gpu\logs when it finishes. ASCII only (PowerShell 5.1).
param([string]$Root = '', [switch]$NoTest)

$ErrorActionPreference = 'Stop'
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run GetKit.cmd (it asks for administrator).'; Read-Host 'Press Enter'; exit 1 }

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$candidates = @()
if ($Root) { $candidates += $Root }
$saved = Join-Path $Here 'share.txt'
if (Test-Path $saved) { $candidates += (Get-Content $saved -TotalCount 1).Trim() }
$candidates += '\\DESKTOP-9Q9MN0V\D\SwitchShare\gpu', '\\192.168.11.27\D\SwitchShare\gpu'

$share = $null
foreach ($c in $candidates) {
    if (-not $c) { continue }
    Write-Host "trying $c ..."
    try { if (Test-Path (Join-Path $c 'kit\AutoTest.cmd')) { $share = $c; break } } catch { }
}
if (-not $share) {
    Write-Host 'STOP: the PC share was not reachable.' -ForegroundColor Red
    Write-Host 'Open \\DESKTOP-9Q9MN0V\D in Explorer once (sign in and tick "Remember"), then run GetKit.cmd again.'
    Read-Host 'Press Enter'
    exit 1
}

$dst = 'C:\SwitchWOA-Gpu-current'
if (Test-Path $dst) { Remove-Item $dst -Recurse -Force }
New-Item -ItemType Directory -Force $dst | Out-Null
Copy-Item (Join-Path $share 'kit\*') $dst -Recurse -Force
Set-Content -Path (Join-Path $dst 'share.txt') -Value $share -Encoding ascii
Set-Content -Path $saved -Value $share -Encoding ascii

$ver = (Select-String -Path (Join-Path $dst 'switchgpu.inf') -Pattern 'DriverVer').Line
$hash = (Get-FileHash (Join-Path $dst 'switchgpu.sys') -Algorithm SHA256).Hash
Write-Host ''
Write-Host "kit from $share" -ForegroundColor Green
Write-Host "  $ver"
Write-Host "  switchgpu.sys SHA256 $hash"
$want = Join-Path $dst 'SHA256.txt'
if (Test-Path $want) {
    $expected = (Get-Content $want -TotalCount 1).Trim()
    if ($expected -ne $hash) { Write-Host 'STOP: SHA256 does not match SHA256.txt (copy incomplete?).' -ForegroundColor Red; Read-Host 'Press Enter'; exit 1 }
    Write-Host '  SHA256 matches SHA256.txt'
}
Write-Host ''
if ($NoTest) {
    Write-Host 'Kit copied to C:\SwitchWOA-Gpu-current (no AutoTest).' -ForegroundColor Yellow
    Read-Host 'Press Enter'
    exit 0
}
Write-Host 'Starting AutoTest from C:\SwitchWOA-Gpu-current ...' -ForegroundColor Yellow
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $dst 'AutoTest.ps1') -Stage Install
