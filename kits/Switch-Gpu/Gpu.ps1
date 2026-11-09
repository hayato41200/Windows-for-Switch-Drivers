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
  -Mode ProbeTest : like PowerTest (PowerTest = 2), and while the GPU is on
                    also the stage B1 probe: topology table, PMU falcon
                    registers, a DMEM write/read test, priv ring init. No
                    firmware, nothing is started on the GPU.
  -Mode DmaTest   : like PowerTest (PowerTest = 3), and while the GPU is on
                    also the stage B2a probe: if the GPU is not behind the
                    SMMU, the PMU falcon's DMA copies 256 bytes from a Windows
                    buffer into DMEM and 256 bytes back, in physical mode.
                    No firmware, nothing runs on the falcon.
  -Mode Remove    : removes the driver package.

  Log: C:\SwitchWOA-Gpu.txt
  This file must stay ASCII: Windows PowerShell 5.1 reads it as the ANSI code page.
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'PowerTest', 'ProbeTest', 'DmaTest', 'Remove')][string]$Mode)

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
  $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' -and $_.Value -isnot [byte[]] } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
  Log ('switchgpu trace: ' + ($vals -join ' '))
  Sum ('switchgpu {0}.{1}.{2}  starts {3}  I2C open: VDD 0x{4:X8} PMIC 0x{5:X8}  resources: mem {6} i2c {7}' -f ($p.Version -shr 16), (($p.Version -shr 8) -band 0xFF), ($p.Version -band 0xFF), $p.StartCount, [uint32]$p.OpenVddStatus, [uint32]$p.OpenPmicStatus, $p.ResMem, $p.ResI2c)
  Sum ('MAX77621 VDD_GPU (I2C5 0x1C): chip id 0x{0:X2}/0x{1:X2}  CONTROL1 0x{2:X2}  CONTROL2 0x{3:X2}' -f [uint32]$p.ChipId1, [uint32]$p.ChipId2, [uint32]$p.Control1, [uint32]$p.Control2)
  Sum ('at start: VOUT {0}  VOUT_DVS {1}  GPIO6 {2}' -f (MvText $p.VoutAtStart), (MvText $p.VoutDvsAtStart), (Gpio $p.Gpio6AtStart))
  Sum ('at start: clamp (RG_CNTRL bit0) {0}  gpu clock {1}  pllg_ref {2}  gpu reset {3}  PLLP_OUTC {4}  fuse clock {5}  MC hot reset ctrl {6} stat {7}' -f (Bit $p.RgCntrlAtStart 0), (Bit $p.ClkXAtStart 24), (Bit $p.ClkXAtStart 29), (Bit $p.RstXAtStart 24), (Hex $p.PllpOutcAtStart), (Bit $p.ClkHAtStart 7), (Hex $p.McHotCtrlAtStart), (Hex $p.McHotStatAtStart))
  if ($p.TestRuns) {
    $steps = @{ 1 = 'read the regulator'; 2 = 'VDD_GPU on'; 3 = 'clocks on'; 4 = 'reset assert'; 5 = 'clamp off'; 6 = 'reset deassert'; 7 = 'MC flush'; 8 = 'first BAR0 read (BOOT_0)'; 9 = 'PTIMER and WPR reads'; 20 = 'B1: topology read'; 21 = 'B1: PMU falcon read'; 22 = 'B1: DMEM test'; 23 = 'B1: priv ring init'; 30 = 'B2a: SMMU check'; 31 = 'B2a: DMA setup'; 32 = 'B2a: DMA memory -> DMEM'; 33 = 'B2a: DMA DMEM -> memory'; 34 = 'B2a: DMA setup restore'; 35 = 'B2a: memory path read'; 36 = 'B2a: DMA try 1'; 37 = 'B2a: DMA try 2 (after PMU reset)'; 38 = 'B2a: DMA try 3 (after PMU reset)'; 10 = 'power down'; 11 = 'done'; 100 = 'done, GPU left on (KeepOn)' }
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
    if ($null -ne $p.PtopRead) {
      $pt = [byte[]]$p.Ptop
      $ent = @(); for ($i = 0; $i -lt $pt.Length; $i += 4) { $v = [BitConverter]::ToUInt32($pt, $i); if ($v) { $ent += ('{0:X8}' -f $v) } }
      Sum ('B1 topology (0x22700, nonzero): ' + ($ent -join ' '))
      $sc = [byte[]]$p.Scal
      $sv = @(); for ($i = 0; $i -lt $sc.Length; $i += 4) { $sv += ('{0:X}={1:X}' -f (0x22430 + $i), [BitConverter]::ToUInt32($sc, $i)) }
      Sum ('B1 scal: ' + ($sv -join ' '))
    }
    if ($null -ne $p.PmuRead) {
      $hw = [uint32]$p.PmuHwcfg
      $fuse = 'production'; if ([uint32]$p.PmuDebug -band 0x100000) { $fuse = 'debug-fused' }
      Sum ('B1 PMU falcon: HWCFG {0} (IMEM {1} KB, DMEM {2} KB)  HWCFG1 {3}  CPUCTL {4}  DMACTL {5}  SCTL {6}' -f (Hex $hw), ((($hw -band 0x1FF) * 256) / 1024), (((($hw -shr 9) -band 0x1FF) * 256) / 1024), (Hex $p.PmuHwcfg1), (Hex $p.PmuCpuctl), (Hex $p.PmuDmactl), (Hex $p.PmuSctl))
      Sum ('B1 PMU falcon: DEBUG {0} ({1})  MBOX {2} {3}' -f (Hex $p.PmuDebug), $fuse, (Hex $p.PmuMbox0), (Hex $p.PmuMbox1))
    }
    if ($null -ne $p.DmemOk) {
      $rb = [byte[]]$p.DmemReadBack
      $rv = @(); for ($i = 0; $i -lt $rb.Length; $i += 4) { $rv += ('{0:X8}' -f [BitConverter]::ToUInt32($rb, $i)) }
      Sum ('B1 DMEM write/read: {0} of 4 words OK  (read {1}; wrote 5A5A0001 A5A50002 12345678 FEDCBA98)' -f $p.DmemOk, ($rv -join ' '))
    }
    if ($null -ne $p.RingDone) {
      Sum ('B1 priv ring: status0 before {0} after {1}  cmd {2}  master {3}  PMC_ENABLE {4}' -f (Hex $p.RingStatus0Before), (Hex $p.RingStatus0), (Hex $p.RingCmd), (Hex $p.RingMaster), (Hex $p.PmcEnableAfterRing))
    }
    if ($null -ne $p.SmmuConfig) {
      Sum ('B2a SMMU: config {0}  GPU ASID {1}  clients 0x230 {2}  0xB98 {3}' -f (Hex $p.SmmuConfig), (Hex $p.SmmuGpuAsid), (Hex $p.SmmuClient230), (Hex $p.SmmuClientB98))
      if ($null -ne $p.SmmuGpuAsidBypass) { Sum ('B2a SMMU: GPU ASID during the test {0}  restored {1}' -f (Hex $p.SmmuGpuAsidBypass), (Hex $p.SmmuGpuAsidRestored)) }
      if ($p.SmmuStillTranslated) { Sum 'B2a SMMU: could not take the GPU out of the SMMU (write ignored); DMA tried anyway' }
      if ($null -ne $p.SmmuPtbData0) {
        $pd = [uint32]$p.SmmuPtbData0
        Sum ('B2a SMMU ASID 0 page directory: PTB_ASID was {0} -> {1}  PTB_DATA {2}  -> table at 0x{3:X} (attr bits 31..29 = {4})  MC int after clear {5}' -f (Hex $p.SmmuPtbAsidWas), (Hex $p.SmmuPtbAsid0), (Hex $pd), ([uint64]($pd -band 0x3FFFFF) * 4096), (($pd -shr 29) -band 7), (Hex $p.McIntStatusCleared))
      }
      if ($p.SmmuIdentityMap) {
        Sum ('B2a SMMU identity map for ASID 0: directory {0}  table {1}  PTB_DATA now {2}  restored {3}' -f (Hex $p.SmmuPdPhys), (Hex $p.SmmuPtPhys), (Hex $p.SmmuPtbDataNew), (Hex $p.SmmuPtbDataRestored))
      }
      if ($null -ne $p.McIntStatusBefore) { Sum ('B2a MC errors: before int {0} err {1} adr {2}  after read DMA int {3} err {4} adr {5}' -f (Hex $p.McIntStatusBefore), (Hex $p.McErrStatusBefore), (Hex $p.McErrAdrBefore), (Hex $p.McIntStatusAfterIn), (Hex $p.McErrStatusAfterIn), (Hex $p.McErrAdrAfterIn)) }
    }
    if ($null -ne $p.PmcEnableAfterMem) {
      Sum ('B2a memory path (XBAR, L2, PFB, HUB reset + on): PMC_ENABLE {0} -> {1}' -f (Hex $p.PmcEnableBeforeMem), (Hex $p.PmcEnableAfterMem))
    }
    if ($null -ne $p.B2aSetup) {
      Sum ('B2a setup: buffer phys {0}  TRANSCFG4 {1} -> {2}  FBIF_CTL {3} -> {4}  DMACTL was {5}' -f (Hex $p.B2aBufPhys), (Hex $p.B2aTranscfgWas), (Hex $p.B2aTranscfg), (Hex $p.B2aFbifCtlWas), (Hex $p.B2aFbifCtl), (Hex $p.B2aDmactlWas))
    }
    if ($null -ne $p.PmuFbifWindow) {
      $fw = [byte[]]$p.PmuFbifWindow
      $fv = @(); for ($i = 0; $i -lt $fw.Length; $i += 4) { $fv += ('{0:X3}={1:X}' -f (0xE00 + $i), [BitConverter]::ToUInt32($fw, $i)) }
      Sum ('B2a PMU FBIF window: ' + ($fv -join ' '))
    }
    if ($null -ne $p.B2aItfen) {
      Sum ('B2a falcon interface: ITFEN {0} -> {1}  DMACTL {2}  TRANSCFG4 {3}' -f (Hex $p.B2aItfenWas), (Hex $p.B2aItfen), (Hex $p.B2aDmactlNow), (Hex $p.B2aTranscfgNow))
    }
    foreach ($k in 1, 2, 3) {
      $okv = $p.PSObject.Properties["B2aV${k}Ok"]
      if ($okv) {
        $tn = @{ 1 = 'phys noncoherent sysmem (6)'; 2 = 'phys local FB (4)'; 3 = 'phys coherent sysmem (5)' }[$k]
        Sum ('B2a try {0} {1}: {2} of 64 OK  polls {3}  cmd {4}  falcon irq {5} (bit 5 = bus error)' -f $k, $tn, $okv.Value, (Hex $p.PSObject.Properties["B2aV${k}Polls"].Value), (Hex $p.PSObject.Properties["B2aV${k}Cmd"].Value), (Hex $p.PSObject.Properties["B2aV${k}Irq"].Value))
      }
    }
    if ($null -ne $p.PfifoIntr0) {
      Sum ('B2a PFIFO: intr0 {0}  mmu fault id {1}  fault0 inst {2} lo {3} hi {4} info {5}' -f (Hex $p.PfifoIntr0), (Hex $p.PfifoMmuFaultId), (Hex $p.PfifoFault0Inst), (Hex $p.PfifoFault0Lo), (Hex $p.PfifoFault0Hi), (Hex $p.PfifoFault0Info))
    }
    if ($null -ne $p.B2aInOk) {
      Sum ('B2a DMA memory -> DMEM: {0} of 64 words OK  polls {1}  cmd {2}  word0 {3} (want C0DE0000)  word63 {4}' -f $p.B2aInOk, (Hex $p.B2aInPolls), (Hex $p.B2aInCmd), (Hex $p.B2aInWord0), (Hex $p.B2aInWord63))
    }
    if ($null -ne $p.B2aOutOk) {
      Sum ('B2a DMA DMEM -> memory: {0} of 64 words OK  polls {1}  cmd {2}  word0 {3} (want 600D0000)  stray changes {4}' -f $p.B2aOutOk, (Hex $p.B2aOutPolls), (Hex $p.B2aOutCmd), (Hex $p.B2aOutWord0), $p.B2aOutSpill)
    }
    if ($null -ne $p.B2aResult) {
      $r = [uint32]$p.B2aResult
      $why = 'done'
      if ($r -eq 0xE0000030) { $why = 'skipped: the GPU is behind the SMMU' } elseif ($r -eq 0xE0000031) { $why = 'no memory' } elseif ($r -eq 0xE0000032) { $why = 'DMA never went idle (buffer kept)' } elseif ($r -eq 0xE0000033) { $why = 'DMA data did not match (write skipped if the read failed)' }
      Sum ('B2a result {0} ({1})' -f (Hex $r), $why)
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
  { $_ -eq 'PowerTest' -or $_ -eq 'ProbeTest' -or $_ -eq 'DmaTest' } {
    $tv = 1; if ($Mode -eq 'ProbeTest') { $tv = 2 }; if ($Mode -eq 'DmaTest') { $tv = 3 }
    New-Item -Path $Key -Force | Out-Null
    $old = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    if ($old) {
      foreach ($n in @($old.PSObject.Properties | Where-Object { $_.Name -match '^(B2a|Smmu|Mc[IE]|Pmu|Pfifo|Ptop|Scal|Dmem|Ring|Wpr|PmcEnable)' } | ForEach-Object { $_.Name })) {
        Remove-ItemProperty -Path $Key -Name $n -ErrorAction SilentlyContinue
      }
    }
    Set-ItemProperty -Path $Key -Name PowerTest -Value $tv -Type DWord
    Log "PowerTest = $tv; restarting ACPI\SWOA0F01\0 ..."
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
