<#
.SYNOPSIS
  Restart to hekate (reboot-to-payload) and power-off for Windows on the Switch.
  Installs switchpwrbtn version 2 (power button driver + restart/power-off) and
  sets its Parameters. Run from an elevated (Administrator) prompt.

  -Mode Install  : set PayloadPath (partition 1 of the boot disk), pnputil /add-driver /install, report
  -Mode Status   : driver version, payload, chip, methods, PMIC partition info
  -Mode Method   : -Restart Auto|Pmc|Psci|Plain|Marker  -PowerOff Pmic|Marker|Windows
  -Mode Enable   : ResetEnable=1 (default)
  -Mode Disable  : ResetEnable=0 (restart and shut down are left to Windows again)
  -Mode Path     : -Payload <path on partition 1, e.g. \payload.bin>
  -Mode RebootTo : where hekate goes after a restart. -Id SWOAHKT = autoboot the hekate_ipl.ini
                   entry with id=SWOAHKT; -Index 2 = autoboot entry 2; neither = the hekate menu

  Log: C:\SwitchWOA-Reset.txt
#>
param(
  [Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Method', 'Enable', 'Disable', 'Path', 'RebootTo')][string]$Mode,
  [ValidateSet('Auto', 'Pmc', 'Psci', 'Plain', 'Marker')][string]$Restart = 'Auto',
  [ValidateSet('Pmic', 'Marker', 'Windows')][string]$PowerOff = 'Pmic',
  [string]$Payload = '\bootloader\update.bin',
  [string]$Id = '',
  [int]$Index = 0
)

$ErrorActionPreference = 'Continue'
# Windows PowerShell on ARM64 may run as x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Reset.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchpwrbtn\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt (right click - Run as administrator).'; exit 1 }

function U32($v) { if ($null -eq $v) { return [uint32]0 } return [uint32]([int64]$v -band [int64]4294967295) }
function Ensure-Key { if (-not (Test-Path $Key)) { New-Item -Path $Key -Force | Out-Null } }
function Set-Dword([string]$name, [int]$value) { Ensure-Key; New-ItemProperty -Path $Key -Name $name -PropertyType DWord -Value $value -Force | Out-Null }

function Get-BootDisk {
  $c = Get-Partition -DriveLetter C -ErrorAction SilentlyContinue
  if ($c) { return [int]$c.DiskNumber }
  return 0
}

function Set-PayloadPath([string]$rel) {
  if (-not $rel.StartsWith('\')) { $rel = '\' + $rel }
  $disk = Get-BootDisk
  $p1 = Get-Partition -DiskNumber $disk -PartitionNumber 1 -ErrorAction SilentlyContinue
  if ($p1) {
    Log ("boot disk {0}, partition 1: size={1:N0} MB type={2} letter='{3}'" -f $disk, ($p1.Size / 1MB), $p1.Type, $p1.DriveLetter)
  } else {
    Log "boot disk $disk has no partition 1?"
  }
  $path = "\Device\Harddisk$disk\Partition1$rel"
  Ensure-Key
  New-ItemProperty -Path $Key -Name 'PayloadPath' -PropertyType String -Value $path -Force | Out-Null
  Log "PayloadPath = $path  (read only; the driver loads it within about 10 s)"
}

function Show-Status {
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0601*' })
  if ($dev.Count -eq 0) { Log 'PWRB: NOT FOUND (ACPI\SWOA0601)' }
  foreach ($d in $dev) {
    Log ("device: {0}  status={1} problem={2} service={3}" -f $d.PNPDeviceID, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'driver trace: (no Parameters key)'; return }
  $ver = $p.DriverVersion
  Log ("driver version: {0}  {1}" -f $ver, $(if ($ver -ge 2) { 'OK (restart/power-off support)' } else { 'OLD - run Install, or restart Windows once' }))
  Log ("chip: Hidrev=0x{0:X} Erista={1}  mapped=0x{2:X} (0x1F = all)" -f (U32 $p.Hidrev), $p.Erista, (U32 $p.Mapped))
  Log ("PayloadPath: {0}" -f $(if ($p.PayloadPath) { $p.PayloadPath } else { '(default) \Device\Harddisk0\Partition1\bootloader\update.bin' }))
  $st = (U32 $p.PayloadStatus)
  $why = @{ '00000000' = 'loaded'; 'C0000034' = 'file not found'; 'C000003A' = 'folder not found'; 'C000000E' = 'no such device';
            'C0000904' = 'too large (over 192 KB)'; 'C000007B' = 'too small'; 'C000000F' = 'no such file'; 'C0000022' = 'access denied' }
  $code = '{0:X8}' -f $st
  $text = if ($p.PayloadTries -eq $null) { 'not tried yet' } elseif ($why.ContainsKey($code)) { $why[$code] } else { 'see NTSTATUS' }
  Log ("payload: status=0x{0:X8} ({1}) size={2} fileSize={3} tries={4} word0=0x{5:X8}" -f $st, $text, $p.PayloadSize, $p.PayloadFileSize, $p.PayloadTries, (U32 $p.PayloadWord0))
  $rm = @{ 0 = 'Auto'; 1 = 'Pmc'; 2 = 'Psci'; 3 = 'Plain'; 4 = 'Marker' }
  $pm = @{ 0 = 'Pmic'; 1 = 'Marker'; 2 = 'Windows' }
  $bc = U32 $p.CfgBootCfg
  $after = if (($bc -band 0x04) -ne 0) { "autoboot the ini entry with id={0}" -f $p.RebootToId } elseif (($bc -band 0x01) -ne 0) { "autoboot ini entry {0}" -f (($bc -shr 8) -band 0xFF) } else { 'hekate menu' }
  if ($ver -lt 3) { $after = "hekate menu (choosing the entry needs driver version 3, 1.0.3.0)" }
  if ($p.CfgBootIdOk -eq 0) { $after += '  (RebootToId ignored: use 1-7 letters/digits, no spaces)' }
  Log ("after a restart: {0}  (hold VOL- while hekate starts for its menu)" -f $after)
  Log ("methods: ResetEnable={0} Restart={1} PowerOff={2}" -f $p.CfgResetEnable, $rm[[int]$p.CfgRestartMethod], $pm[[int]$p.CfgPowerOffMethod])
  if ($ver -ge 2) {
    $auto = if ([int]$p.CfgRestartMethod -ne 0) { $rm[[int]$p.CfgRestartMethod] } elseif ($st -eq 0 -and $p.PayloadSize -gt 0 -and $p.Erista -eq 1) { 'R2P from Windows (payload in IRAM, PMC reset)' } else { 'PSCI SYSTEM_RESET (bl31)' }
    Log "restart will use: $auto"
  }
  Log ("i2c: OpenI2cStatus=0x{0:X} FirstReadStatus=0x{1:X} Failures={2} (count at the last error; 0 is not written back) LastI2cError=0x{3:X8}" -f (U32 $p.OpenI2cStatus), (U32 $p.FirstReadStatus), $p.Failures, (U32 $p.LastI2cError))
}

Log ''
Log "=== Reset $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
switch ($Mode) {
  'Install' {
    Set-PayloadPath $Payload
    $inf = Join-Path $Here 'switchpwrbtn.inf'
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Log "  exit=$LASTEXITCODE"
    Log 'waiting 15 s for the driver to start and load the payload...'
    Start-Sleep -Seconds 15
    Show-Status
  }
  'Status' { Show-Status }
  'Method' {
    $r = @{ Auto = 0; Pmc = 1; Psci = 2; Plain = 3; Marker = 4 }[$Restart]
    $o = @{ Pmic = 0; Marker = 1; Windows = 2 }[$PowerOff]
    Set-Dword 'RestartMethod' $r
    Set-Dword 'PowerOffMethod' $o
    Log "RestartMethod=$Restart ($r) PowerOffMethod=$PowerOff ($o); the driver reads it within 10 s"
    Start-Sleep -Seconds 11
    Show-Status
  }
  'Enable' { Set-Dword 'ResetEnable' 1; Log 'ResetEnable=1'; Start-Sleep -Seconds 11; Show-Status }
  'Disable' { Set-Dword 'ResetEnable' 0; Log 'ResetEnable=0 (Windows handles restart/shut down again)'; Start-Sleep -Seconds 11; Show-Status }
  'Path' { Set-PayloadPath $Payload; Start-Sleep -Seconds 12; Show-Status }
  'RebootTo' {
    Ensure-Key
    if ($Id -ne '') {
      if ($Id -notmatch '^[!-~]{1,7}$') { Log "Id must be 1-7 printable ASCII characters without spaces: '$Id'"; exit 1 }
      New-ItemProperty -Path $Key -Name 'RebootToId' -PropertyType String -Value $Id -Force | Out-Null
      Set-Dword 'RebootToIndex' 0
      Log "RebootToId=$Id : add the line  id=$Id  to the Windows entry in bootloader\hekate_ipl.ini"
    } else {
      New-ItemProperty -Path $Key -Name 'RebootToId' -PropertyType String -Value '' -Force | Out-Null
      Set-Dword 'RebootToIndex' $Index
      if ($Index -gt 0) { Log "RebootToIndex=$Index (entry $Index of hekate_ipl.ini, the first after [config] is 1)" } else { Log 'after a restart: hekate menu' }
    }
    Start-Sleep -Seconds 11
    Show-Status
  }
}
