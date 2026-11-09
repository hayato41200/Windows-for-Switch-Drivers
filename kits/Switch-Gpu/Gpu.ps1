<#
.SYNOPSIS
  GM20B GPU bring-up test, stage A (Administrator). Needs UEFI R411 (DSDT GPU0 =
  ACPI\SWOA0F01).

  -Mode Install   : pnputil /add-driver switchgpu.inf /install, then Status.
                    The driver only reads registers at start; it never powers
                    the GPU by itself.
  -Mode Status    : the GPU0 device and the switchgpu trace, decoded.
  -Mode PowerTest : sets PowerTest = 1, restarts GPU0 (the driver then powers
                    the GPU once, reads NV_PMC_BOOT_0 and PTIMER, and powers it
                    down again), waits, then Status. If Windows freezes, hold
                    the power button, boot again and run Status: "Step" shows
                    where it stopped.
  -Mode Remove    : removes the driver package.

  Log: C:\SwitchWOA-Gpu.txt
  This file must stay ASCII: Windows PowerShell 5.1 reads it as the ANSI code page.
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'PowerTest', 'Remove')][string]$Mode)

$ErrorActionPreference = 'Continue'
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Gpu.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters'
$Summary = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
function Sum([string]$s) { $Summary.Add($s); Log $s }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
Log ''
Log "=== Gpu $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Hex($v) { if ($null -eq $v) { 'n/a' } else { '0x{0:X8}' -f [uint32]$v } }
function MvText($code) {
  if ($null -eq $code -or ([uint32]$code -band 0xFF000000)) { return 'read failed' }
  $c = [uint32]$code
  '{0} mV {1}' -f [int]((606250 + 6250 * ($c -band 0x7F)) / 1000), $(if ($c -band 0x80) { '(EN bit on)' } else { '(EN bit off)' })
}
function Gpio($v) {
  if ($null -eq $v -or ([uint32]$v -band 0xFF000000)) { return 'read failed' }
  $g = [uint32]$v
  if ($g -band 2) { return ('input (0x{0:X2})' -f $g) }
  '{0} {1} (0x{2:X2})' -f $(if ($g -band 8) { 'output high' } else { 'output low' }), $(if ($g -band 1) { 'push-pull' } else { 'open-drain' }), $g
}
function Bit($v, $b) { if ($null -eq $v) { 'n/a' } elseif (([uint32]$v -shr $b) -band 1) { '1' } else { '0' } }

function Show-Device {
  $found = $false
  foreach ($i in @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0F01' -ErrorAction SilentlyContinue)) {
    $found = $true
    $p = Get-ItemProperty $i.PSPath -ErrorAction SilentlyContinue
    Sum ("device: ACPI\SWOA0F01\{0}  service={1}" -f $i.PSChildName, $p.Service)
  }
  if (-not $found) { Sum 'device: ACPI\SWOA0F01 not found (boot UEFI R411)' }
  (& $Pnputil /enum-devices /instanceid 'ACPI\SWOA0F01\0' 2>&1) | ForEach-Object { Log "  $_" }
}

function Show-Trace {
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Sum 'switchgpu trace: (no Parameters key: driver never started)'; return }
  $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
  Log ('switchgpu trace: ' + ($vals -join ' '))
  Sum ('switchgpu {0}.{1}.{2}  starts {3}  I2C open: VDD 0x{4:X8} PMIC 0x{5:X8}  resources: mem {6} i2c {7}' -f ($p.Version -shr 16), (($p.Version -shr 8) -band 0xFF), ($p.Version -band 0xFF), $p.StartCount, [uint32]$p.OpenVddStatus, [uint32]$p.OpenPmicStatus, $p.ResMem, $p.ResI2c)
  Sum ('MAX77621 VDD_GPU (I2C5 0x1C): chip id 0x{0:X2}/0x{1:X2}  CONTROL1 0x{2:X2}  CONTROL2 0x{3:X2}' -f [uint32]$p.ChipId1, [uint32]$p.ChipId2, [uint32]$p.Control1, [uint32]$p.Control2)
  Sum ('at start: VOUT {0}  VOUT_DVS {1}  GPIO6 {2}' -f (MvText $p.VoutAtStart), (MvText $p.VoutDvsAtStart), (Gpio $p.Gpio6AtStart))
  Sum ('at start: clamp (RG_CNTRL bit0) {0}  gpu clock {1}  pllg_ref {2}  gpu reset {3}  PLLP_OUTC {4}  fuse clock {5}  MC hot reset ctrl {6} stat {7}' -f (Bit $p.RgCntrlAtStart 0), (Bit $p.ClkXAtStart 24), (Bit $p.ClkXAtStart 29), (Bit $p.RstXAtStart 24), (Hex $p.PllpOutcAtStart), (Bit $p.ClkHAtStart 7), (Hex $p.McHotCtrlAtStart), (Hex $p.McHotStatAtStart))
  if ($p.TestRuns) {
    $steps = @{ 1 = 'read the regulator'; 2 = 'VDD_GPU on'; 3 = 'clocks on'; 4 = 'reset assert'; 5 = 'clamp off'; 6 = 'reset deassert'; 7 = 'MC flush'; 8 = 'first BAR0 read (BOOT_0)'; 9 = 'PTIMER and WPR reads'; 10 = 'power down'; 11 = 'done'; 100 = 'done, GPU left on (KeepOn)' }
    Sum ('power test runs {0}: last step {1} ({2})  result {3}  took {4} us' -f $p.TestRuns, $p.Step, $steps[[int]$p.Step], (Hex $p.Result), $p.TestUs)
    Sum ('while on: VOUT {0}  GPIO6 {1}  PLLP_OUTC {2}  clamp {3}  gpu clock {4}  gpu reset {5}  MC flush polls {6}' -f (MvText $p.VoutOn), (Gpio $p.Gpio6On), (Hex $p.PllpOutcOn), (Bit $p.RgCntrlOn 0), (Bit $p.ClkXOn 24), (Bit $p.RstXOn 24), $p.McFlushPolls)
    if ($p.Boot0) {
      $b = [uint32]$p.Boot0
      Sum ('NV_PMC_BOOT_0 = 0x{0:X8}  chipset 0x{1:X3}{2}  rev 0x{3:X2}' -f $b, (($b -shr 20) -band 0x1FF), $(if ((($b -shr 20) -band 0x1FF) -eq 0x12B) { ' = GM20B' } else { ' (GM20B is 0x12B)' }), ($b -band 0xFF))
      Sum ('PMC_ENABLE {0}  PTIMER {1} -> {2} (1 ms apart: {3})  hi {4}' -f (Hex $p.PmcEnable), (Hex $p.Ptimer0), (Hex $p.Ptimer1), $(if ([uint32]$p.Ptimer1 -ne [uint32]$p.Ptimer0) { 'running' } else { 'NOT moving' }), (Hex $p.PtimerHi))
      if ($null -ne $p.WprStart) {
        $ws = [uint64]([uint32]$p.WprStart -band 0xFFFFFF00) * 256
        $wl = [uint64]([uint32]$p.WprLimit -band 0xFFFFFF00) * 256 + 0x20000
        Sum ('WPR seen by the GPU (0x100CD4): raw {0} / {1}  ->  0x{2:X} .. 0x{3:X} ({4} KB)' -f (Hex $p.WprStart), (Hex $p.WprLimit), $ws, $wl, ([int64]($wl - $ws) / 1024))
      }
    } else {
      Sum 'NV_PMC_BOOT_0: not read'
    }
    Sum ('after: VOUT {0}  GPIO6 {1}  clamp {2}  gpu clock {3}  gpu reset {4}' -f (MvText $p.VoutAfter), (Gpio $p.Gpio6After), (Bit $p.RgCntrlAfter 0), (Bit $p.ClkXAfter 24), (Bit $p.RstXAfter 24))
    if ($p.PowerTest) { Sum 'PowerTest is still 1: the test did not start (the driver did not run)' }
  } else {
    Sum 'power test: never run'
  }
}

switch ($Mode) {
  'Install' {
    $inf = Join-Path $Here 'switchgpu.inf'
    if (-not (Test-Path $inf)) { Sum "missing: $inf"; break }
    Log "pnputil /add-driver $inf /install"
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Sum "pnputil exit=$LASTEXITCODE"
    Start-Sleep -Seconds 3
    Show-Device; Log ''; Show-Trace
  }
  'Status' { Show-Device; Log ''; Show-Trace }
  'PowerTest' {
    New-Item -Path $Key -Force | Out-Null
    Set-ItemProperty -Path $Key -Name PowerTest -Value 1 -Type DWord
    Log 'PowerTest = 1; restarting ACPI\SWOA0F01\0 ...'
    $out = (& $Pnputil /restart-device 'ACPI\SWOA0F01\0' 2>&1)
    $out | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0) {
      Log '  restart-device failed; trying disable + enable'
      (& $Pnputil /disable-device 'ACPI\SWOA0F01\0' 2>&1) | ForEach-Object { Log "  $_" }
      (& $Pnputil /enable-device 'ACPI\SWOA0F01\0' 2>&1) | ForEach-Object { Log "  $_" }
    }
    Start-Sleep -Seconds 5
    Show-Device; Log ''; Show-Trace
  }
  'Remove' {
    $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
    foreach ($x in [regex]::Matches($oem, '(oem\d+\.inf)\s*\r?\n[^\r\n]*:\s*switchgpu\.inf')) {
      $name = $x.Groups[1].Value
      Log "pnputil /delete-driver $name /uninstall /force"
      (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
    }
    Show-Device
  }
}

Log ''
Log '=== SUMMARY'
foreach ($s in $Summary) { Write-Host $s }
