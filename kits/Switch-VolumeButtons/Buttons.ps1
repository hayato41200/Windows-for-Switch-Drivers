<#
.SYNOPSIS
  Volume buttons (tegra210gpio 1.0.1.0) on the Switch's Windows (Administrator).

  -Mode Install : pnputil /add-driver tegra210gpio.inf /install (1.0.1.0), then report
  -Mode Status  : the button counters the driver writes every 2 s, and what they mean
  -Mode Remove  : delete only the 1.0.1.0 package; Windows goes back to the older one

  Log: C:\SwitchWOA-VolumeButtons.txt
  ASCII only (Windows PowerShell 5.1).
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-VolumeButtons.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\tegra210gpio\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Right-click the .cmd and choose "Run as administrator".'; exit 1 }
Log ''
Log "=== VolumeButtons $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Get-Packages {
  $text = & $Pnputil /enum-drivers 2>&1 | Out-String
  $list = @()
  foreach ($b in ($text -split '(?=Published Name\s*:)')) {
    if ($b -match 'Original Name\s*:\s*tegra210gpio\.inf') {
      $oem = if ($b -match 'Published Name\s*:\s*(oem\d+\.inf)') { $Matches[1] } else { '' }
      $ver = if ($b -match 'Driver Version\s*:\s*(\S+\s+\S+)') { $Matches[1] } else { '' }
      $list += [pscustomobject]@{ Oem = $oem; Version = $ver }
    }
  }
  return $list
}

function Show-Status {
  $dev = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'ACPI\\SWOA0101%'" -ErrorAction SilentlyContinue
  foreach ($d in $dev) { Log ("GPI0: {0} status={1} problem={2}" -f $d.PNPDeviceID, $d.Status, $d.ConfigManagerErrorCode) }
  $btn = Get-CimInstance Win32_PnPEntity -Filter "PNPDeviceID LIKE 'ACPI\\ACPI0011%'" -ErrorAction SilentlyContinue
  foreach ($d in $btn) { Log ("buttons: {0} status={1} problem={2}" -f $d.PNPDeviceID, $d.Status, $d.ConfigManagerErrorCode) }
  foreach ($p in Get-Packages) { Log ("driver store: {0} {1}" -f $p.Oem, $p.Version) }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'driver trace: (no Parameters key)'; return }
  $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
  Log ('driver trace: ' + ($vals -join ' '))
  $ver = [uint32]$p.DriverVersion
  if ($ver -ne 0x101) { Log ("running driver is not 1.0.1.0 (DriverVersion=0x{0:X}). Restart Windows after Install." -f $ver); return }
  $in = [uint32]$p.X_IN; $lvl = [uint32]$p.X_INT_LVL
  foreach ($b in @(@{ N = 'VOL+'; P = 'VolUp'; Bit = 6 }, @{ N = 'VOL-'; P = 'VolDown'; Bit = 7 })) {
    $low = [uint32]$p.($b.P + 'Low'); $high = [uint32]$p.($b.P + 'High'); $irq = [uint32]$p.($b.P + 'Irq')
    $line = if (($in -shr $b.Bit) -band 1) { 'released' } else { 'pressed' }
    $armed = if (($lvl -shr $b.Bit) -band 1) { 'release' } else { 'press' }
    $ok = (($line -eq 'released') -and ($armed -eq 'press') -and ($low -eq $high)) -or
          (($line -eq 'pressed') -and ($armed -eq 'release') -and ($low -eq $high + 1))
    Log ("{0}: presses={1} releases={2} interrupts={3} line={4} waiting-for={5} -> {6}" -f $b.N, $low, $high, $irq, $line, $armed, $(if ($ok) { 'OK' } else { 'MISMATCH' }))
  }
  Log '(Counters are written every 2 s; press nothing for 3 s before Status.)'
}

switch ($Mode) {
  'Install' {
    $inf = Join-Path $Here 'tegra210gpio.inf'
    Log "pnputil /add-driver $inf /install"
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Show-Status
    Log 'Restart Windows now (the GPIO driver is replaced at the next start).'
  }
  'Status' { Show-Status }
  'Remove' {
    $all = @(Get-Packages)
    if (@($all | Where-Object { $_.Version -notlike '*1.0.1.0' }).Count -eq 0) {
      Log 'STOP: no older tegra210gpio package in the driver store; nothing removed.'
      $all = @()
    }
    foreach ($p in $all) {
      if ($p.Version -like '*1.0.1.0') {
        Log "pnputil /delete-driver $($p.Oem) /uninstall /force"
        (& $Pnputil /delete-driver $p.Oem /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
      }
    }
    Show-Status
    Log 'Restart Windows now.'
  }
}
Log "Log: $Log"
