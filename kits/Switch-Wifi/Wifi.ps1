<#
.SYNOPSIS
  Wi-Fi stage 2a probe driver on the Switch's Windows (Administrator). Needs
  UEFI R405 (DSDT WLN0, ACPI\SWOA0D01).

  -Mode Install : pnputil /add-driver switchwifi.inf /install, then report
  -Mode Status  : report WLN0 and what the driver read (link, chip ID, cores)
  -Mode Retry   : clear the Busy flag left by an attempt that stopped Windows, then restart the device
  -Mode Remove  : remove the driver package
  -Mode Firmware: copy your own brcmfmac4356-pcie.bin (and .txt, if you have one)
                  from this folder to System32\drivers\switchwifi, then restart the device

  The firmware is not included (it may not be redistributed): copy it from the
  Linux on your Switch (/lib/firmware/brcm/) or from linux-firmware.

  Log: C:\SwitchWOA-Wifi.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Retry', 'Remove', 'Firmware')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Wifi.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchwifi\Parameters'
$Hwid = 'ACPI\SWOA0D01'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this as Administrator (right-click the .cmd > Run as administrator).'; exit 1 }

$FwDir = Join-Path $Sys 'drivers\switchwifi'
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
  if ($n -eq 0) { Log 'firmware: no brcmfmac4356-pcie.bin in this folder (stage 2b is skipped without it)' }
  return $n
}

function Hex($v) { if ($null -eq $v) { '-' } else { '{0:X8}' -f [uint32]$v } }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like "$Hwid*" })
  if ($dev.Count -eq 0) { Log "WLN0: NOT FOUND ($Hwid). Is the Switch booted with UEFI R405?" }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'driver trace: (no Parameters key: the driver has not run)'; return }
  Log ("result : {0}" -f $p.Result)
  Log ("version={0} mem={1} busy={2} skipped={3} step={4}" -f (Hex $p.Version), $p.MemResources, $p.Busy, $p.Skipped, $p.Step)
  Log ("link   : RP LNK={0} XP={1} AFI FPCI={2}" -f (Hex $p.RpLink), (Hex $p.RpXp), (Hex $p.AfiFpci))
  Log ("config : ID={0} CMD={1} CLASS={2} BAR0={3} BAR2={4} WIN={5}" -f (Hex $p.EpId), (Hex $p.EpCmd), (Hex $p.EpClass), (Hex $p.EpBar0), (Hex $p.EpBar2), (Hex $p.EpWindow))
  Log ("chip   : ID={0} CAPS={1} EROM={2} words={3} cores={4}" -f (Hex $p.ChipId), (Hex $p.ChipCaps), (Hex $p.EromPtr), $p.EromWords, $p.Cores)
  if ($null -ne $p.RamBytes) {
    Log ("arm cr4: base={0} rev={1} cap={2} RAM={3} KB  pcie2 rev={4}" -f (Hex $p.Cr4Base), $p.Cr4Rev, (Hex $p.Cr4Cap), [int]($p.RamBytes / 1024), $p.Pcie2Rev)
  }
  if ($null -ne $p.FwResult) {
    Log ("fw     : {0}" -f $p.FwResult)
    Log ("fw     : stage={0} size={1} nvram={2} verifyBad={3} rstvec={4} halted ioctl={5} reset={6}" -f $p.FwStage, $p.FwSize, $p.NvSize, $p.FwVerifyBad, (Hex $p.FwResetVector), (Hex $p.Cr4IoctlHalted), (Hex $p.Cr4ResetHalted))
    Log ("fw     : shared before={0} after={1} in {2} ms flags={3} console={4}" -f (Hex $p.FwSharedBefore), (Hex $p.FwShared), $p.FwUpMs, (Hex $p.FwSharedFlags), (Hex $p.FwConsoleAddr))
  }
  if ($null -ne $p.MsgRingInfo) {
    Log ("msg    : {0}" -f $p.MsgResult)
    Log ("msg    : ringinfo={0} ringmem={1} h2dW={2} d2hW={3} subq={4} rxoff={5} dma={6} posted={7} ctlR={8} mbint={9}" -f (Hex $p.MsgRingInfo), (Hex $p.MsgRingMem), (Hex $p.MsgH2dW), (Hex $p.MsgD2hW), $p.MsgMaxSubQueues, $p.MsgRxDataOffset, (Hex $p.MsgDmaPa), $p.MsgPosted, $p.MsgCtlSubmitR, (Hex $p.MsgMailboxInt))
    Log ("msg    : completions={0} events={1} unknown={2} lasttype={3} scratch={4}" -f $p.MsgCompletions, $p.MsgEvents, $p.MsgUnknown, $p.MsgLastType, (Hex $p.MsgScratch))
    foreach ($pf in 'Pre', 'Post', 'Ioctl') {
      if ($null -ne $p."${pf}CtlSubR") {
        Log ("diag {0,-5}: ctlR={1} ctlW={2} cmplW={3} mbint={4} afi={5}/{6} mc int={7} err={8} adr={9} smmu={10} afiasid={11}" -f $pf,
          $p."${pf}CtlSubR", $p."${pf}CtlSubW", $p."${pf}CtlCmplW", (Hex $p."${pf}MbInt"), (Hex $p."${pf}AfiCode"), (Hex $p."${pf}AfiSig"),
          (Hex $p."${pf}McInt"), (Hex $p."${pf}McErr"), (Hex $p."${pf}McErrAdr"), (Hex $p."${pf}SmmuCfg"), (Hex $p."${pf}SmmuAfi"))
      }
    }
    Log ("ioctl  : GET_VERSION ret={0} st={1} value={2}" -f $p.IoctlVersionRet, $p.IoctlVersionStatus, $p.IoctlVersion)
    Log ("ioctl  : ver ret={0} st={1} '{2}'" -f $p.IoctlVerRet, $p.IoctlVerStatus, $p.IoctlVer)
    Log ("ioctl  : cur_etheraddr ret={0} st={1} {2}" -f $p.IoctlMacRet, $p.IoctlMacStatus, $p.IoctlMacOui)
  }
  if ($p.CoreList -and $null -eq $p.FwResult) {
    Log 'cores (id:rev@base/wrapper):'
    $line = ''
    foreach ($c in ($p.CoreList -split ' ' | Where-Object { $_ })) {
      if (($line + $c).Length -gt 70) { Log "  $line"; $line = '' }
      $line += "$c "
    }
    if ($line) { Log "  $line" }
  }
  if ($p.FwConsole) {
    Log 'firmware console (last lines):'
    $lines = @($p.FwConsole -split "`n" | Where-Object { $_.Trim() })
    $lines | Select-Object -Last 12 | ForEach-Object { Log "  $_" }
  }
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Wi-Fi Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    [void](Copy-Firmware)
    # A new driver is a new attempt: drop the old trace (and its Busy flag).
    if (Test-Path $Key) {
      $k = Get-Item $Key
      foreach ($n in $k.GetValueNames()) { Remove-ItemProperty $Key -Name $n -ErrorAction SilentlyContinue }
    }
    $inf = Join-Path $Here 'switchwifi.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Firmware' {
    Log ''
    Log "=== Wi-Fi Firmware  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    if ((Copy-Firmware) -gt 0) {
      if (Test-Path $Key) { Set-ItemProperty $Key -Name Busy -Value 0 -Type DWord }
      foreach ($d in @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like "$Hwid*" })) {
        (& $Pnputil /restart-device $d.PNPDeviceID 2>&1) | ForEach-Object { Log "  $_" }
      }
      Start-Sleep -Seconds 8
    }
    Show-Status
  }
  'Status' { Log ''; Log "=== Wi-Fi Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Retry' {
    Log ''
    Log "=== Wi-Fi Retry  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    if (Test-Path $Key) { Set-ItemProperty $Key -Name Busy -Value 0 -Type DWord }
    $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like "$Hwid*" })
    foreach ($d in $dev) {
      (& $Pnputil /restart-device $d.PNPDeviceID 2>&1) | ForEach-Object { Log "  $_" }
    }
    Start-Sleep -Seconds 3
    Show-Status
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchwifi\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
