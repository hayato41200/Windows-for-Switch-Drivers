<#
.SYNOPSIS
  Audio driver (speaker), stage 1, on the Switch's Windows (Administrator). Needs UEFI R402
  (DSDT AUD0; APE clocks on, codec set up, I2S1 running).

  -Mode Install : pnputil /add-driver switchaudio.inf /install, then report
  -Mode Status  : report AUD0 (ACPI\SWOA0C01), the driver trace and the Windows audio endpoints
  -Mode Remove  : remove the driver package

  Log: C:\SwitchWOA-Audio.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on Windows 10 ARM64 is x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Audio.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchaudio\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0C01*' })
  if ($dev.Count -eq 0) { Log 'AUD0: NOT FOUND (ACPI\SWOA0C01). Is the Switch booted with UEFI R402?' }
  foreach ($d in $dev) {
    Log ("device: {0}  name='{1}' status={2} problem={3} service={4}" -f $d.PNPDeviceID, $d.Name, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if ($p) {
    $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, $_.Value }
    Log ('driver trace: ' + ($vals -join ' '))
    # Stage: 1 started, 2 hardware ready, 3 endpoints registered; 0x11 = APE clocks off (UEFI older than
    # R402), 0x12 = I2S1/XBAR not set up, 0x10 = ACPI resources missing.
    switch ($p.Stage) {
      3 { Log 'RESULT: the driver runs. Look for "Speakers" in Settings > System > Sound.' }
      0x11 { Log ("RESULT: the APE clocks are not on (ApeCheck=0x{0:X}, needs 0xF). Boot with UEFI R402." -f $p.ApeCheck) }
      0x12 { Log 'RESULT: the APE clocks are on but UEFI did not set up I2S1. Boot with UEFI R402.' }
      default { Log "RESULT: the driver stopped at stage $($p.Stage)." }
    }
  } else { Log 'driver trace: (no Parameters key)' }
  $ep = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPClass -eq 'AudioEndpoint' })
  foreach ($e in $ep) { Log ("endpoint: '{0}' status={1} problem={2}" -f $e.Name, $e.Status, $e.ConfigManagerErrorCode) }
  if ($ep.Count -eq 0) { Log 'endpoint: none' }
  foreach ($s in 'Audiosrv', 'AudioEndpointBuilder') {
    $svc = Get-Service $s -ErrorAction SilentlyContinue
    if ($svc) { Log ("{0}: {1} ({2})" -f $s, $svc.Status, $svc.StartType) }
  }
}

Log "==== $(Get-Date -Format s)  Audio.ps1 -Mode $Mode"
switch ($Mode) {
  'Install' {
    $inf = Join-Path $Here 'switchaudio.inf'
    Log (& $Pnputil /add-driver $inf /install | Out-String)
    Start-Sleep -Seconds 3
    Show-Status
  }
  'Status' { Show-Status }
  'Remove' {
    $pkgs = (& $Pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}" | Where-Object { $_ -match 'switchaudio\.inf' }
    foreach ($b in $pkgs) {
      if ($b -match '(oem\d+\.inf)') { Log (& $Pnputil /delete-driver $Matches[1] /uninstall /force | Out-String) }
    }
    Show-Status
  }
}
