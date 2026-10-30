<#
.SYNOPSIS
  Bluetooth driver, stage 1 (link check), on the Switch's Windows (Administrator). Needs UEFI R390
  (DSDT BTH0; UART-D clocked and the BCM4356A3 powered).

  -Mode Install : pnputil /add-driver switchbt.inf /install, then report
  -Mode Status  : report BTH0 (ACPI\SWOA0B01), the driver trace (chip answers) and whether
                  Windows has the BthMini UART transport (MS_BTHX_BTHMINI in bth.inf)
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Bluetooth.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Bluetooth.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchbt\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0B01*' })
  if ($dev.Count -eq 0) { Log 'BTH0: NOT FOUND (ACPI\SWOA0B01). Is the Switch booted with UEFI R390?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object {
      if ($_.Value -is [string]) { '{0}={1}' -f $_.Name, $_.Value } else { '{0}=0x{1:X}' -f $_.Name, $_.Value } }
    Log ('driver trace: ' + ($vals -join ' '))
    if ($p.Result -eq 'ok') {
      Log ("chip: '{0}'  address {1}  maker 0x{2:X4} (000F = Broadcom)  LMP subversion 0x{3:X4}" -f $p.LocalName, $p.BdAddr, $p.Manufacturer, $p.LmpSubversion)
      Log 'RESULT: the Bluetooth chip answers.'
    } else {
      Log "RESULT: the chip did not answer ($($p.Result))."
    }
  } else { Log 'driver trace: (no Parameters key)' }
  $bth = Join-Path $env:windir 'INF\bth.inf'
  $mini = (Test-Path $bth) -and (Select-String -Path $bth -Pattern 'MS_BTHX_BTHMINI' -Quiet)
  $sysFile = Test-Path (Join-Path $Sys 'drivers\BthMini.sys')
  Log ("Windows BthMini: bth.inf has MS_BTHX_BTHMINI={0}  BthMini.sys={1}" -f $mini, $sysFile)
}

switch ($Mode) {
  'Install' {
    Log ''
    Log "=== Bluetooth Install  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
    $inf = Join-Path $Here 'switchbt.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Status
  }
  'Status' { Log ''; Log "=== Bluetooth Status  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"; Show-Status }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(?ms)Published Name\s*:\s*(oem\d+\.inf)\s*\r?\n\s*Original Name\s*:\s*switchbt\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Status
  }
}
