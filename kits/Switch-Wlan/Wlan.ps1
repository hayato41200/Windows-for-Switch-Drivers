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

# 0.6.x: the console's own Wi-Fi MAC from wifi-mac.txt (one line, "xx:xx:xx:xx:xx:xx"; only in the
# user's private kit, never in the repository) -> Parameters\MacAddress, read by the driver at start.
function Set-Mac {
  $f = Join-Path $Here 'wifi-mac.txt'
  if (-not (Test-Path $f)) { Log 'mac: no wifi-mac.txt here; the firmware default address is used'; return }
  $m = ((Get-Content $f -TotalCount 1) -replace '\s', '').ToUpper()
  if ($m -notmatch '^([0-9A-F]{2}[:-]){5}[0-9A-F]{2}$') { Log "mac: wifi-mac.txt is not xx:xx:xx:xx:xx:xx; ignored"; return }
  New-Item -Path $Key -Force | Out-Null
  Set-ItemProperty $Key -Name MacAddress -Value ($m -replace '-', ':') -Type String
  Log ("mac: MacAddress set from wifi-mac.txt ({0}:xx:xx:xx)" -f $m.Substring(0, 8))
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
  if ($null -ne $p.WlanCommands) { Log ("cmds   : {0} unsupported {1} (last id {2})  op={3} ticks={4}" -f $p.WlanCommands, $p.WlanUnsupported, $p.WlanLastUnsupported, $p.WlanPendingOp, $p.WlanTicks) }
  # 0.6.4: last WDI messages, oldest first (number = command id, iNN = indication id)
  if ($p.WlanCmdLog) { Log ("cmdlog : ({0}) {1}" -f $p.WlanCmdLogN, $p.WlanCmdLog) }
  # 0.6.6: frames between the chip and Windows, connect timing
  if ($p.WlanOsPath) { Log ("os     : {0}" -f $p.WlanOsPath) }
  # 0.6.11: what the association really is (PHY 4=OFDM 6=ERP 7=HT 8=VHT)
  if ($null -ne $p.WlanPhy) { Log ("link   : phy={0} bandwidth={1} MHz rate={2} Mbit/s chanspec={3}" -f $p.WlanPhy, $p.WlanBandwidth, ([double]$p.LinkRate500k / 2), $(if ($null -ne $p.LinkChanspec) { '{0:X4}' -f $p.LinkChanspec } else { '-' })) }
  if ($p.WlanFormat) { Log ("format : {0}" -f $p.WlanFormat) }
  if ($p.WlanRxDst) { Log ("rx dst : {0}  (unicast to the adapter MAC / multicast / unicast to another MAC)" -f $p.WlanRxDst) }
  @($p.ScanList -split "`n" | Where-Object { $_.Trim() }) | Select-Object -First 6 | ForEach-Object { Log "  $_" }
  # 0.6.x: MAC, connection, keys, data path (the chip layer writes these every few seconds while it runs)
  Log ("mac    : source={0} (0 firmware default, 1 MacAddress, 2 Windows)  {1}  setSt={2} try={3} nvram={4}" -f $p.MacSource, $p.MacOui, (Hex $p.MacSetSt), $p.MacTry, $p.MacNvram)
  Log ("join   : joins={0} st={1} via={2} sec={3}  link state={4} event={5} status={6} reason={7} events={8}" -f $p.ConnJoins, (Hex $p.ConnJoinSt), $p.ConnJoinVia,
    $(if ($p.ConnSecSt) { ($p.ConnSecSt | ForEach-Object { '{0:X2}' -f $_ }) -join '' } else { '-' }), $p.ConnLinkState, $p.ConnLinkEvent, $p.ConnLinkStatus, $p.ConnLinkReason, $p.ConnLinkEvents)
  Log ("assoc  : req ie {0} B, resp ie {1} B   keys set={2} st={3} eapolWait={4} ms" -f $p.ConnAssocReqLen, $p.ConnAssocRespLen, $p.KeysSet, (Hex $p.KeySt), $p.KeyWaitMs)
  Log ("flow   : state={0} (2 open) creates={1} status={2} {3} ms  data start={4}" -f $p.FlowState, $p.FlowCreates, (Hex $p.FlowStatus), $p.FlowCreateMs, (Hex $p.WlanDataStart))
  Log ("tx     : posted={0} done={1} err={2} noSlot={3} eapol={4} bytes={5}" -f $p.TxPosted, $p.TxDone, $p.TxStatusErr, $p.TxNoSlot, $p.TxEapol, $p.TxBytes)
  Log ("rx     : frames={0} eapol={1} bad={2} bytes={3}  posted={4}  unknownMsg={5} lastType={6}" -f $p.RxFrames, $p.RxEapol, $p.RxBad, $p.RxBytes, $p.DataRxPosted, $p.MsgUnknown, $p.MsgLastType)
  if ($p.ConnEvents) { Log ("events : {0}  (type/status/reason/flags)" -f $p.ConnEvents) }
  $svc = Get-CimInstance Win32_Service -Filter "Name='wlansvc'" -ErrorAction SilentlyContinue
  if ($svc) { Log ("wlansvc: state={0} start={1} pid={2}" -f $svc.State, $svc.StartMode, $svc.ProcessId) }
  if (-not $svc -or $svc.State -ne 'Running') { Log '--- (wlansvc is not running: netsh wlan is skipped)'; return }
  Log '--- netsh wlan show interfaces'
  (& $Netsh wlan show interfaces 2>&1) | Where-Object { $_ -match '\S' } | Select-Object -First 20 | ForEach-Object { Log "  $_" }
  $ip = Get-NetIPConfiguration -ErrorAction SilentlyContinue | Where-Object { $_.InterfaceDescription -like 'Nintendo Switch Wi-Fi*' } | Select-Object -First 1
  if ($ip) {
    Log ("--- IP: {0}  v4={1}  gateway={2}  dns={3}" -f $ip.InterfaceAlias, (($ip.IPv4Address | ForEach-Object { $_.IPAddress }) -join ','),
      (($ip.IPv4DefaultGateway | ForEach-Object { $_.NextHop }) -join ','), (($ip.DNSServer | Where-Object { $_.AddressFamily -eq 2 } | ForEach-Object { $_.ServerAddresses }) -join ','))
    $gw = @($ip.IPv4DefaultGateway | ForEach-Object { $_.NextHop }) | Select-Object -First 1
    $Ping = Join-Path $Sys 'ping.exe'
    foreach ($t in @($gw, '1.1.1.1') | Where-Object { $_ }) {
      Log "--- ping $t"
      (& $Ping -n 2 -w 1500 $t 2>&1) | Where-Object { $_ -match '\S' } | Select-Object -Last 4 | ForEach-Object { Log "  $_" }
    }
  } else { Log '--- IP: (the Wi-Fi interface has no IP configuration yet)' }
  # 0.6.6: why Windows gave up on the last connection (WLAN AutoConfig log), newest first
  Log '--- WLAN-AutoConfig events (newest first)'
  try {
    Get-WinEvent -LogName 'Microsoft-Windows-WLAN-AutoConfig/Operational' -MaxEvents 8 -ErrorAction Stop | ForEach-Object {
      $all = @($_.Message -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ })
      $why = @($all | Select-Object -Skip 1 | Where-Object { $_ -match 'Reason|Error|SSID|' + [char]0x7406 + [char]0x7531 + '|' + [char]0x30A8 + [char]0x30E9 + [char]0x30FC })
      Log ("  {0:HH:mm:ss} id={1} {2} | {3}" -f $_.TimeCreated, $_.Id, $all[0], (($why | Select-Object -First 3) -join ' | '))
    }
  } catch { Log "  (could not read the log: $($_.Exception.Message))" }
  Log '--- netsh wlan show networks (first 10 lines)'
  (& $Netsh wlan show networks 2>&1) | Where-Object { $_ -match '\S' } | Select-Object -First 10 | ForEach-Object { Log "  $_" }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Wlan Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    Copy-Firmware
    Remove-Package 'switchwifi.inf'
    # MacAddress stays (SwitchPatcher / SwitchCenter may have written it); Set-Mac replaces it from wifi-mac.txt.
    if (Test-Path $Key) { $k = Get-Item $Key; foreach ($n in $k.GetValueNames()) { if ($n -ne 'MacAddress') { Remove-ItemProperty $Key -Name $n -ErrorAction SilentlyContinue } } }
    Set-Mac
    $inf = Join-Path $Here 'switchwlan.inf'
    # 0.6.5: wlansvc keeps the adapter open; stop it so the old driver can be removed.
    $sc = Join-Path $Sys 'sc.exe'
    (& $sc stop wlansvc 2>&1) | Out-Null
    for ($i = 0; $i -lt 20 -and (Get-Service wlansvc -ErrorAction SilentlyContinue).Status -ne 'Stopped'; $i++) { Start-Sleep -Milliseconds 500 }
    Log ("  wlansvc before update: {0}" -f (Get-Service wlansvc -ErrorAction SilentlyContinue).Status)
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    $rc = $LASTEXITCODE
    Log "  exit=$rc"
    (& $sc start wlansvc 2>&1) | Out-Null
    if ($rc -eq 480 -or $rc -eq 3010) { Log '  *** The old driver could not be replaced while running. RESTART the Switch, then run Status.cmd (not Install). ***' }
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
