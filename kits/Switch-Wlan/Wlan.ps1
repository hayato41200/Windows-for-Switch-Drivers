<#
.SYNOPSIS
  switchwlan (WiFiCx Wi-Fi driver) on the Switch's Windows (Administrator).
  Needs Windows 11 22H2+, UEFI R405+ (ACPI\SWOA0D01) and your own BCM4356
  firmware (brcmfmac4356-pcie.bin, and .txt if you have it).

  -Mode Install : remove the old probe driver (switchwifi), copy the firmware from
                  this folder, install switchwlan, then report
  -Mode Status  : driver trace + what Windows sees (netsh wlan)
  -Mode Retry   : clear Busy (left by an attempt that stopped Windows), restart the device
  -Mode Remove  : remove switchwlan

  Log: C:\SwitchWOA-Wlan.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Retry', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows ARM64 may be x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Netsh = Join-Path $Sys 'netsh.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Wlan.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchwlan\Parameters'
$Hwid = 'ACPI\SWOA0D01'
$FwDir = Join-Path $Sys 'drivers\switchwifi'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this as Administrator (right-click the .cmd > Run as administrator).'; exit 1 }
function Hex($v) { if ($null -eq $v) { '-' } else { '{0:X8}' -f [uint32]$v } }

function Remove-Package([string]$InfName) {
  $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
  foreach ($x in [regex]::Matches($oem, "(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*$([regex]::Escape($InfName))")) {
    $name = $x.Groups[1].Value
    Log "pnputil /delete-driver $name /uninstall /force   ($InfName)"
    (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
  }
}

function Copy-Firmware {
  $n = 0
  foreach ($f in 'brcmfmac4356-pcie.bin', 'brcmfmac4356-pcie.txt') {
    $src = @((Join-Path $Here $f), (Join-Path $Here "firmware\$f")) | Where-Object { Test-Path $_ } | Select-Object -First 1
    if ($src) {
      New-Item -ItemType Directory -Force $FwDir | Out-Null
      Copy-Item $src (Join-Path $FwDir $f) -Force
      Log ("firmware: copied {0} ({1} bytes)" -f $f, (Get-Item $src).Length); $n++
    }
  }
  if ($n -eq 0) {
    if (Test-Path (Join-Path $FwDir 'brcmfmac4356-pcie.bin')) { Log 'firmware: none in this folder; the copy already in System32\drivers\switchwifi is used' }
    else { Log 'firmware: MISSING. Put brcmfmac4356-pcie.bin in this folder and run Install again.' }
  }
}

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like "$Hwid*" })
  if ($dev.Count -eq 0) { Log "device: NOT FOUND ($Hwid). Is the Switch booted with UEFI R405 or later?" }
  foreach ($d in $dev) {
    Log ("device: name='{0}' status={1} problem={2} service={3}" -f $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'driver trace: (none: switchwlan has not run)'; return }
  Log ("chip   : {0}" -f $p.WlanResult)
  Log ("fw     : {0}  ver '{1}'" -f $p.FwResult, $p.IoctlVer)
  Log ("wificx : version={0} wifiInit={1} osWdi={2} caps dev/sta/band/phy={3}/{4}/{5}/{6}" -f (Hex $p.WlanVersion), (Hex $p.WlanWifiInit), (Hex $p.WlanOsWdiVersion),
    (Hex $p.WlanCapsDevice), (Hex $p.WlanCapsStation), (Hex $p.WlanCapsBand), (Hex $p.WlanCapsPhy))
  Log ("adapter: type={0} create={1} init={2} start={3}  busy={4} step={5}" -f $p.WlanAdapterType, (Hex $p.WlanAdapterCreate), (Hex $p.WlanAdapterInit), (Hex $p.WlanAdapterStart), $p.Busy, $p.Step)
  Log ("scans  : {0} begin={1} {2} ms, reported {3}  (escan events {4}, found {5})" -f $p.WlanScans, (Hex $p.WlanScanBeginStatus), $p.WlanScanMs, $p.WlanScanReported, $p.ScanEscanEvents, $p.ScanCount)
  if ($null -ne $p.WlanCommands) { Log ("cmds   : {0} unsupported {1} (last id {2})  [written when the driver stops]" -f $p.WlanCommands, $p.WlanUnsupported, $p.WlanLastUnsupported) }
  @($p.ScanList -split "`n" | Where-Object { $_.Trim() }) | Select-Object -First 6 | ForEach-Object { Log "  $_" }
  Log '--- netsh wlan show interfaces'
  (& $Netsh wlan show interfaces 2>&1) | Where-Object { $_ -match '\S' } | Select-Object -First 12 | ForEach-Object { Log "  $_" }
  Log '--- netsh wlan show networks (first 10 lines)'
  (& $Netsh wlan show networks 2>&1) | Where-Object { $_ -match '\S' } | Select-Object -First 10 | ForEach-Object { Log "  $_" }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Wlan Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Copy-Firmware
    Remove-Package 'switchwifi.inf'
    if (Test-Path $Key) { $k = Get-Item $Key; foreach ($n in $k.GetValueNames()) { Remove-ItemProperty $Key -Name $n -ErrorAction SilentlyContinue } }
    $inf = Join-Path $Here 'switchwlan.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 15
    Show-Status
  }
  'Status' { Log ''; Log "=== Wlan Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Retry' {
    Log ''
    Log "=== Wlan Retry  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    if (Test-Path $Key) { Set-ItemProperty $Key -Name Busy -Value 0 -Type DWord }
    foreach ($d in @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like "$Hwid*" })) {
      (& $Pnputil /restart-device $d.PNPDeviceID 2>&1) | ForEach-Object { Log "  $_" }
    }
    Start-Sleep -Seconds 15
    Show-Status
  }
  'Remove' { Remove-Package 'switchwlan.inf'; Show-Status }
}
