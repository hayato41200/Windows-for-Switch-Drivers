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
  -Mode FwTest    : stage B3a (PowerTest = 4). Copies the user's own
                    linux-firmware nvidia\gm20b\acr\bl.bin and ucode_load.bin
                    (from .\firmware\nvidia\gm20b) into the registry; the
                    driver checks them, patches the signature in memory and
                    loads the ACR bootloader into PMU IMEM/DMEM by PIO. The
                    falcon is not started.
  -Mode AcrTest   : isolated one-shot ACR load (PowerTest = 5). This writes the
                    GM20B WPR and DMA page tables, binds PMU virtual memory and
                    starts signed ACR firmware. It always resets the GPU after
                    the bounded wait. Use only when prepared for a forced power
                    cycle if the GPU or system stops responding.
  -Mode Remove    : removes the driver package.

  Log: C:\SwitchWOA-Gpu.txt
  This file must stay ASCII: Windows PowerShell 5.1 reads it as the ANSI code page.
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'PowerTest', 'ProbeTest', 'DmaTest', 'FwTest', 'AcrTest', 'Remove')][string]$Mode)

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
function Key32($v) { if ($null -eq $v) { return '' }; $n = [long]$v; if ($n -lt 0) { $n += 4294967296 }; '{0:X8}' -f $n }
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
    $steps = @{ 1 = 'read the regulator'; 2 = 'VDD_GPU on'; 3 = 'clocks on'; 4 = 'reset assert'; 5 = 'clamp off'; 6 = 'reset deassert'; 7 = 'MC flush'; 8 = 'first BAR0 read (BOOT_0)'; 9 = 'PTIMER and WPR reads'; 20 = 'B1: topology read'; 21 = 'B1: PMU falcon read'; 22 = 'B1: DMEM test'; 23 = 'B1: priv ring init'; 29 = 'B2a: priv ring init'; 40 = 'B3a: firmware from registry'; 41 = 'B3a: bl.bin header'; 42 = 'B3a: ucode_load.bin headers'; 43 = 'B3a: signed image'; 44 = 'B3a: PMU reset + IMEM PIO'; 45 = 'B3a: DMEM descriptor'; 46 = 'B3b: CPU-side image (user code)'; 47 = 'B3c: read hardware WPR range'; 48 = 'B3c: build bounded WPR image'; 49 = 'B3c: prepare SMMU and PMU VM'; 50 = 'B3c: bind PMU instance block'; 51 = 'B3c: write ACR DMA descriptors'; 52 = 'B3c: legacy ACR step'; 53 = 'B3c: priv ring init'; 62 = 'B3c: snapshot and clear old PFIFO MMU faults'; 54 = 'B3c: invalidate selected PMU PDB'; 55 = 'B3c: invalidate all PMU PDBs'; 56 = 'B3c: one-shot ACR boot'; 57 = 'B3c: select per-instance PDB page size'; 58 = 'B3c: write advertised PMU argv and start LS PMU'; 59 = 'B3c: wait for and inspect PMU INIT'; 60 = 'B3c: read PMU post-wait diagnostics'; 61 = 'B3c: snapshot LS loader IMEM, DMA, FBIF, bind and faults'; 63 = 'B3c: snapshot DMACTL before LS start'; 64 = 'B3c: build PMU loader DMA bases from WPR address'; 65 = 'B3c: parse PMU INIT and validate command queues'; 66 = 'B3c: send INIT_WPR_REGION and wait for reply'; 67 = 'B3c: enable GR and snapshot FECS mailbox'; 68 = 'B3c: post FECS bootstrap command'; 69 = 'B3c: sample PMU and HPQ while waiting'; 70 = 'B3c: read final FECS, GPCCS and PMU state'; 71 = 'B3c: PMU checkpoint before FECS bootstrap (stop if halted)'; 72 = 'B3c: PMU state right after FECS bootstrap post'; 30 = 'B2a: SMMU check'; 31 = 'B2a: DMA setup'; 32 = 'B2a: DMA memory -> DMEM'; 33 = 'B2a: DMA DMEM -> memory'; 34 = 'B2a: DMA setup restore'; 35 = 'B2a: memory path read'; 36 = 'B2a: DMA try 1'; 37 = 'B2a: DMA try 2 (after PMU reset)'; 38 = 'B2a: DMA try 3 (after PMU reset)'; 39 = 'B2a: PMU instance bind'; 10 = 'power down'; 11 = 'done'; 100 = 'done, GPU left on (KeepOn)' }
    $steps[71] = 'B3c: PMU checkpoints around FECS preparation'
    $steps[72] = 'B3c: post 16-byte FECS bootstrap command'
    $steps[73] = 'B3c: prepare FECS DMA and validate GPCCS firmware'
    $steps[74] = 'B3c: PIO-load non-secure GPCCS IMEM and DMEM firmware'
    $steps[75] = 'B3c: ordered FECS mailbox setup, start GPCCS then FECS, wait ready'
    Sum ('power test runs {0}: last step {1} ({2})  result {3}  took {4} us' -f $p.TestRuns, $p.Step, $steps[[int]$p.Step], (Hex $p.Result), $p.TestUs)
    if ($null -ne $p.McHotStatAfterReset) {
      $pts = @(@('GPU off', $p.McHotStatAtStart), @('after reset', $p.McHotStatAfterReset), @('after flush', $p.McHotStatAfterFlush), @('after BAR0 reads', $p.McHotStatAfterBar0), @('B2a start', $p.McHotStatB2aStart), @('before DMA', $p.McHotStatBeforeDma), @('after DMA', $p.McHotStatAfterDma))
      $txt = @(); $first = $null
      foreach ($pt in $pts) {
        if ($null -eq $pt[1]) { continue }
        $v = [uint32]$pt[1]; $txt += ('{0} {1}' -f $pt[0], (($v -shr 2) -band 1))
        if ($pt[0] -ne 'GPU off' -and $null -eq $first -and -not ($v -band 4)) { $first = $pt[0] }
      }
      $why = 'stays 1 the whole time'
      if ($first -eq 'after reset' -or $first -eq 'after flush') { $why = ('drops to 0 at "{0}", before any GPU work: only a flush-done flag, says nothing about DMA' -f $first) }
      elseif ($first) { $why = ('first 0 at "{0}": something between the previous point and this one leaves a request open' -f $first) }
      Sum ('MC hot reset bit 2 over the run: {0} -> {1}' -f ($txt -join ', '), $why)
      if ($null -ne $p.McIntBeforePowerUp) {
        Sum ('MC int while powering up: old {0} -> cleared {1}, after clamp off {2}, after reset {3}, after BAR0 reads {4}' -f (Hex $p.McIntBeforePowerUp), (Hex $p.McIntPowerUpCleared), (Hex $p.McIntAfterClampOff), (Hex $p.McIntAfterReset), (Hex $p.McIntAfterBar0))
      } else {
        Sum ('MC int while powering up: after reset {0}  after BAR0 reads {1}' -f (Hex $p.McIntAfterReset), (Hex $p.McIntAfterBar0))
      }
    }
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
      $rw = 'B1'; if ([uint32]$p.RingDone -eq 29) { $rw = 'B2a' }; if ([uint32]$p.RingDone -eq 53) { $rw = 'B3c' }
      Sum ($rw + ' priv ring: status0 before {0} after {1}  cmd {2}  master {3}  PMC_ENABLE {4}' -f (Hex $p.RingStatus0Before), (Hex $p.RingStatus0), (Hex $p.RingCmd), (Hex $p.RingMaster), (Hex $p.PmcEnableAfterRing))
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
      if ($null -ne $p.McIntStatusIdle10ms) {
        $idleWhy = 'stays clear without DMA'; if ([uint32]$p.McIntStatusIdle10ms -ne 0) { $idleWhy = 'comes back WITHOUT any GPU DMA (background)' }
        Sum ('B2a MC int after 10 ms idle (no DMA): {0} err {1} -> {2}' -f (Hex $p.McIntStatusIdle10ms), (Hex $p.McErrStatusIdle10ms), $idleWhy)
      }
      if ($null -ne $p.McIntAfterBind) {
        $seq = @(@('buffer alloc', $p.McIntBeforeStep35), @('step 35', $p.McIntBeforeSetup), @('step 31 setup', $p.McIntBeforeBind), @('first bind', $p.McIntAfterBind), @('bind to try 1', $p.McIntBeforeTry1), @('try 1', $p.McIntAfterTry1), @('reset+rebind 2', $p.McIntBeforeTry2), @('try 2', $p.McIntAfterTry2), @('reset+rebind 3', $p.McIntBeforeTry3), @('try 3', $p.McIntAfterTry3))
        $txt = @(); $hit = @()
        foreach ($s in $seq) { if ($null -eq $s[1]) { continue }; $txt += ('{0} {1}' -f $s[0], (Hex $s[1])); if ([uint32]$s[1] -band 0x200) { $hit += $s[0] } }
        $why = 'int 0x200 never comes back'
        if ($hit.Count) { $why = ('0x200 at: ' + ($hit -join ', ')) }
        Sum ('B2a MC int step by step (each value = what the named step raised; cleared after each read): {0} -> {1}' -f ($txt -join ', '), $why)
        Sum ('B2a MC err latched: after bind {0} adr {1}  after try 1 {2}  try 2 {3}  try 3 {4}' -f (Hex $p.McErrAfterBind), (Hex $p.McErrAdrAfterBind), (Hex $p.McErrAfterTry1), (Hex $p.McErrAfterTry2), (Hex $p.McErrAfterTry3))
      }
      if ($null -ne $p.McHotStatAfterDma) {
        $hb = 4; if ($null -ne $p.McHotStatBeforeDma) { $hb = [uint32]$p.McHotStatBeforeDma -band 4 }
        $ha = [uint32]$p.McHotStatAfterDma -band 4
        if (-not $hb) { $hw = 'already 0 before the DMA: says nothing about the DMA' }
        elseif (-not $ha) { $hw = 'dropped during the DMA: a GPU request reached the MC' }
        else { $hw = 'stays 1: nothing pending at the MC' }
        Sum ('B2a MC hot reset status: before DMA {0}  after DMA {1} -> bit 2 {2}' -f (Hex $p.McHotStatBeforeDma), (Hex $p.McHotStatAfterDma), $hw)
      }
    }
    if ($null -ne $p.PmcEnableAfterMem) {
      Sum ('B2a memory path (XBAR, L2, PFB, HUB reset + on): PMC_ENABLE {0} -> {1}' -f (Hex $p.PmcEnableBeforeMem), (Hex $p.PmcEnableAfterMem))
    }
    if ($null -ne $p.B2aSetup) {
      Sum ('B2a setup: buffer phys {0}  TRANSCFG4 {1} -> {2}  FBIF_CTL {3} -> {4}  DMACTL was {5}' -f (Hex $p.B2aBufPhys), (Hex $p.B2aTranscfgWas), (Hex $p.B2aTranscfg), (Hex $p.B2aFbifCtlWas), (Hex $p.B2aFbifCtl), (Hex $p.B2aDmactlWas))
    }
    if ($null -ne $p.B2aBindResult) {
      $br = [uint32]$p.B2aBindResult; $bwhy = 'bind and status handshake completed'
      if ($br -eq 0xE0000034) { $bwhy = 'PMU bind status did not complete; DMA tries were skipped' }
      elseif ($br -eq 0xE0000035) { $bwhy = 'instance/page-directory allocation or SMMU map-window check failed; bind skipped' }
      Sum ('B2a PMU instance bind: result {0} ({1})  ITFEN was {2} before bind, {3} during bind' -f (Hex $br), $bwhy, (Hex $p.B2aItfenWas), (Hex $p.B2aItfenForBind))
      Sum ('B2a PMU bind handshake: ack state {0} after {1} polls; completion state {2} after {3} polls' -f (Hex $p.B2aBindAckState), $p.B2aBindAckPolls, (Hex $p.B2aBindDoneState), $p.B2aBindDonePolls)
      Sum ('B2a instance memory: physical {0}; GPU page directory {1}; PDB low {2}; VA limit low {3}; inside 4 MiB map {4}' -f (Hex $p.B2aInstPhys), (Hex $p.B2aGpuPdPhys), (Hex $p.B2aInstPdbLo), (Hex $p.B2aInstLimitLo), $p.B2aInstMapReady)
      Sum ('B2a PMU bind registers: 0x480 active {0}; context-enable after restore {1}' -f (Hex $p.B2aBindInstActive), (Hex $p.B2aBindCtxEnableRestored))
    }
    if ($null -ne $p.B2aRebindResult) {
      $rr = [uint32]$p.B2aRebindResult; $rwhy = 'PMU rebound after falcon reset'
      if ($rr -ne 1) { $rwhy = 'rebind failed; remaining DMA attempts were skipped' }
      Sum ('B2a PMU rebind after retry reset: result {0} ({1}); ack {2}; complete {3}' -f (Hex $rr), $rwhy, (Hex $p.B2aRebindAckState), (Hex $p.B2aRebindDoneState))
    }
    if ($null -ne $p.B2aBindPagesFreedAfterReset) {
      Sum ('B2a bound instance backing pages released after GPU reset: {0} pair(s)' -f $p.B2aBindPagesFreedAfterReset)
    }
    if ($null -ne $p.PmuFbifWindow) {
      $fw = [byte[]]$p.PmuFbifWindow
      $fv = @(); for ($i = 0; $i -lt $fw.Length; $i += 4) { $fv += ('{0:X3}={1:X}' -f (0xE00 + $i), [BitConverter]::ToUInt32($fw, $i)) }
      Sum ('B2a PMU FBIF window: ' + ($fv -join ' '))
    }
    if ($null -ne $p.B2aGpuMemAfterRing) {
      $gn2 = @('PMC_INTR 100', 'PMC_ENABLE 200', 'PBUS intr 1100', 'PBUS en 1140', 'PBUS fault adr 9084', 'PBUS fault data 9088', 'L2 inval 70004', 'L2 flush 70010', 'sysmem flush 100C10', 'MMU ctrl 100C80', 'MMU dbg ctrl 100CC4', 'MMU dbg wr 100CC8', 'MMU dbg rd 100CCC', 'LTC nr 12006C', 'tag base 17E278', 'LTS 17E280', 'FBP 22438')
      $gr = [byte[]]$p.B2aGpuMemAfterRing; $gl2 = @()
      for ($i = 0; $i -lt $gn2.Count -and ($i * 4 + 4) -le $gr.Length; ++$i) { $gl2 += ('{0}={1:X}' -f $gn2[$i], [BitConverter]::ToUInt32($gr, $i * 4)) }
      $lw = 'L2 still NOT reachable'; if (([BitConverter]::ToUInt32($gr, 14 * 4) -band 0xFFFF0000) -ne 0xBADF0000) { $lw = 'L2 registers answer now' }
      Sum ('B2a GPU memory path right after the priv ring init: ' + ($gl2 -join '  ') + ' -> ' + $lw)
    }
    if ($null -ne $p.B2aFifoPmcNow) {
      Sum ('B2a PFIFO enable (nvgpu order): PMC_ENABLE {0} -> {1}  PFIFO intr0 was {2} after clear {3}  restored PMC_ENABLE {4}' -f (Hex $p.B2aFifoPmcWas), (Hex $p.B2aFifoPmcNow), (Hex $p.B2aFifoIntr0Was), (Hex $p.B2aFifoIntr0Clr), (Hex $p.B2aFifoPmcRestored))
    }
    if ($null -ne $p.B2aFbFlushReg) {
      $fw2 = 'done'; if ([uint32]$p.B2aFbFlushPolls -ge 100) { $fw2 = 'TIMED OUT' }
      Sum ('B2a FB flush (0x070000): polls {0} ({1})  reg {2}' -f $p.B2aFbFlushPolls, $fw2, (Hex $p.B2aFbFlushReg))
    }
    if ($null -ne $p.B2aFbFlushPhys) {
      Sum ('B2a FB init (nouveau gm200): flush page {0} -> 100C10 {1}  dbg wr {2} -> 100CC8 {3}  dbg rd {4} -> 100CCC {5}  100CC4 {6}  inside SMMU map {7}  restored 100C10 {8}' -f (Hex $p.B2aFbFlushPhys), (Hex $p.B2aFbFlushNow), (Hex $p.B2aFbDbgWrPhys), (Hex $p.B2aFbDbgWrNow), (Hex $p.B2aFbDbgRdPhys), (Hex $p.B2aFbDbgRdNow), (Hex $p.B2aFbDbgCtlNow), $p.B2aFbInMap, (Hex $p.B2aFbFlushRestored))
    } elseif ($null -ne $p.B2aFbAlloc) { Sum 'B2a FB init: page allocation failed (skipped)' }
    if ($null -ne $p.B2aGpuMemBefore) {
      $gn = @('PMC_INTR 100', 'PMC_ENABLE 200', 'PBUS intr 1100', 'PBUS en 1140', 'PBUS fault adr 9084', 'PBUS fault data 9088', 'L2 inval 70004', 'L2 flush 70010', 'sysmem flush 100C10', 'MMU ctrl 100C80', 'MMU dbg ctrl 100CC4', 'MMU dbg wr 100CC8', 'MMU dbg rd 100CCC', 'LTC nr 12006C', 'tag base 17E278', 'LTS 17E280', 'FBP 22438')
      $gb = [byte[]]$p.B2aGpuMemBefore; $ga = $null; if ($null -ne $p.B2aGpuMemAfter) { $ga = [byte[]]$p.B2aGpuMemAfter }
      $gl = @()
      for ($i = 0; $i -lt $gn.Count -and ($i * 4 + 4) -le $gb.Length; ++$i) {
        $vb = [BitConverter]::ToUInt32($gb, $i * 4)
        $t = '{0}={1:X}' -f $gn[$i], $vb
        if ($null -ne $ga -and ($i * 4 + 4) -le $ga.Length) { $va = [BitConverter]::ToUInt32($ga, $i * 4); if ($va -ne $vb) { $t += ('->{0:X}' -f $va) } }
        $gl += $t
      }
      Sum ('B2a GPU memory path (before tries [->after if changed]): ' + ($gl -join '  '))
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
    if ($null -ne $p.B2aFifoIntr0BeforeTries) {
      $ft = @(('before tries', $p.B2aFifoIntr0BeforeTries, $p.B2aFifoFaultIdBeforeTries), ('after try 1', $p.B2aFifoIntr0AfterTry1, $p.B2aFifoFaultIdAfterTry1), ('after try 2', $p.B2aFifoIntr0AfterTry2, $p.B2aFifoFaultIdAfterTry2), ('after try 3', $p.B2aFifoIntr0AfterTry3, $p.B2aFifoFaultIdAfterTry3))
      $fs = @(); foreach ($f in $ft) { if ($null -ne $f[1]) { $fs += ('{0}: intr0 {1} id {2}' -f $f[0], (Hex $f[1]), (Hex $f[2])) } }
      Sum ('B2a PFIFO MMU fault timeline (bit 28 = MMU fault, 27 = dropped): ' + ($fs -join '; '))
    }
    if ($null -ne $p.B2aFaultRecs) {
      $reasons = @('PDE', 'PDE_SIZE', 'PTE', 'VA_LIMIT_VIOLATION', 'UNBOUND_INST_BLOCK', 'PRIV_VIOLATION', 'RO_VIOLATION', 'WO_VIOLATION', 'PITCH_MASK_VIOLATION', 'WORK_CREATION', 'UNSUPPORTED_APERTURE', 'COMPRESSION_FAILURE', 'UNSUPPORTED_KIND', 'REGION_VIOLATION', 'BOTH_PTES_VALID', 'POISONED')
      $units = @{ 0 = 'GR'; 3 = 'IFB'; 4 = 'BAR1'; 5 = 'BAR2'; 7 = 'PBDMA0'; 0x15 = 'CE0'; 0x16 = 'CE1'; 0x17 = 'PMU' }
      $fr = [byte[]]$p.B2aFaultRecs
      for ($o = 0; $o + 20 -le $fr.Length; $o += 20) {
        $u = [BitConverter]::ToUInt32($fr, $o); $fi = [BitConverter]::ToUInt32($fr, $o + 4); $flo = [BitConverter]::ToUInt32($fr, $o + 8); $fhi = [BitConverter]::ToUInt32($fr, $o + 12); $fty = [BitConverter]::ToUInt32($fr, $o + 16)
        $un = $units[[int]$u]; if (-not $un) { $un = '?' }
        $acc = 'read'; if ($fty -band 0x80) { $acc = 'write' }
        $hub = 'gpc'; if ($fty -band 0x40) { $hub = 'hub' }
        Sum ('B2a MMU fault record unit 0x{0:X2} ({1}): inst {2} (block 0x{3:X}) va 0x{4:X}{5:X8} info {6} -> type {7} ({8}) {9} {10} client 0x{11:X2} gpc {12}' -f $u, $un, (Hex $fi), ([uint64]$fi * 4096), $fhi, $flo, (Hex $fty), ($fty -band 0xF), $reasons[$fty -band 0xF], $acc, $hub, (($fty -shr 8) -band 0x1F), (($fty -shr 24) -band 0x1F))
      }
    }
    if ($null -ne $p.B3aResult) {
      $b3err = @{ '00000000' = 'OK'; 'E0000041' = 'firmware missing in the registry (run FwTest from the kit)'; 'E0000042' = 'bl.bin too short'; 'E0000043' = 'bl.bin header bad'; 'E0000044' = 'bl.bin descriptor bad'; 'E0000045' = 'ucode_load.bin too short'; 'E0000046' = 'ucode_load.bin header bad'; 'E0000047' = 'patch location outside the file'; 'E0000048' = 'load header outside the file'; 'E0000049' = 'app table bad'; 'E000004A' = 'signature/section offsets bad'; 'E000004B' = 'no memory'; 'E000004C' = 'bootloader does not fit IMEM'; 'E000004D' = 'no memory'; 'E0000061' = 'invalid WPR range'; 'E0000062' = 'built WPR is larger than the hardware protected region'; 'E0000063' = 'ACR descriptor is too short'; 'E0000064' = 'could not map WPR/VM memory'; 'E0000065' = 'PMU virtual-memory bind failed'; 'E0000066' = 'ACR DMA descriptor readback mismatch'; 'E0000067' = 'ACR boot timed out'; 'E0000068' = 'ACR boot mailbox indicated failure'; 'E0000069' = 'WPR CPU write/readback mismatch'; 'E000006A' = 'PMU PDB MMU invalidate failed'; 'E000006B' = 'all-PDB MMU invalidate could not observe FIFO empty'; 'E000006C' = 'MMU per-instance PDB page-size selection readback failed'; 'E000006D' = 'ACR did not leave PMU halted with empty mailboxes'; 'E000006E' = 'PMU DMEM size is outside the supported range'; 'E000006F' = 'PMU HS halt interrupt did not clear'; 'E0000070' = 'PMU LS INIT did not appear within 2000 polls (2 s)'; 'E0000071' = 'PMU INIT queue pointers stayed invalid for 2000 polls'; 'E0000072' = 'PMU INIT message header/type or PIO cursor mismatch'; 'E0000073' = 'PMU cmdline args DMEM readback/cursor mismatch'; 'E0000074' = 'WPR-advertised PMU argv does not match the runtime DMEM args location'; 'E0000075' = 'pre-ACR PFIFO MMU fault latches did not clear; ACR was skipped'; 'E0000076' = 'PMU DMACTL REQUIRE_CTX did not clear before LS start'; 'E0000077' = 'PMU loader WPR DMA address cannot be represented in the descriptor'; 'E0000078' = 'FECS bootstrap command, PMU reply, or LS-ready wait failed'; 'E0000079' = 'PMU INIT_WPR_REGION command, reply, or success status failed'; 'E000007A' = 'PMU did not dequeue the FECS bootstrap command before timeout'; 'E000007B' = 'PMU HALT observed before FECS bootstrap post'; 'E000007C' = 'PMU HALT observed immediately after FECS bootstrap post'; 'E000007D' = 'FECS CPUCTL HALT bit did not clear after CPUCTL_ALIAS start'; 'E000007E' = 'GPCCS inst/data firmware missing from registry'; 'E000007F' = 'GPCCS firmware is invalid or exceeds Falcon IMEM/DMEM capacity'; 'E0000080' = 'GPCCS CPUCTL HALT bit did not clear after start' }
      $why3 = $b3err[(Key32 $p.B3aResult)]; if (-not $why3) { $why3 = '?' }
      Sum ('B3c PMU app version {0}; argv in WPR {1}' -f (Hex $p.B3cPmuAppVersion), (Hex $p.B3bPmuArgv))
      Sum ('B3a firmware: bl.bin {0} bytes magic {1} (hdr {2} data {3}+{4})  start tag {5} dmem off {6} code {7}+{8}' -f $p.B3aBlLen, (Hex $p.B3aBlMagic), (Hex $p.B3aBlHdrOff), (Hex $p.B3aBlDataOff), (Hex $p.B3aBlDataSize), (Hex $p.B3aBlStartTag), (Hex $p.B3aBlDmemLoadOff), (Hex $p.B3aBlCodeOff), (Hex $p.B3aBlCodeSize))
      if ($null -ne $p.B3aUcMagic) {
        Sum ('B3a firmware: ucode_load.bin {0} bytes magic {1} (hdr {2} data {3}+{4})  sig prod {5}+{6} dbg {7}+{8}  patch {9}/{10} -> loc {11} sig {12}' -f $p.B3aUcLen, (Hex $p.B3aUcMagic), (Hex $p.B3aUcHdrOff), (Hex $p.B3aUcDataOff), (Hex $p.B3aUcDataSize), (Hex $p.B3aSigPrdOff), (Hex $p.B3aSigPrdSize), (Hex $p.B3aSigDbgOff), (Hex $p.B3aSigDbgSize), (Hex $p.B3aPatchLoc), (Hex $p.B3aPatchSig), (Hex $p.B3aLoc), (Hex $p.B3aSig))
      }
      if ($null -ne $p.B3aNsOff) {
        Sum ('B3a load header: non-secure code {0}+{1}  data {2}+{3}  apps {4}  app0 {5}+{6}' -f (Hex $p.B3aNsOff), (Hex $p.B3aNsSize), (Hex $p.B3aDataBase), (Hex $p.B3aDataSize), $p.B3aNumApps, (Hex $p.B3aApp0Off), (Hex $p.B3aApp0Size))
      }
      if ($null -ne $p.B3aDebugFused) {
        $sk = 'production'; if ([uint32]$p.B3aDebugFused -ne 0) { $sk = 'debug' }
        Sum ('B3a signed image: {0} fuse -> {0} signature patched at {1} (first word {2}, DMEM offset {3})  image word sum {4}' -f $sk, (Hex $p.B3aLoc), (Hex $p.B3aSigWord0), (Hex $p.B3aDmemSign), (Hex $p.B3aImgSum))
      }
      if ($null -ne $p.B3aImemOk) {
        Sum ('B3a PMU PIO: IMEM {0} DMEM {1}  scrub polls {2}  bootloader at IMEM {3} (tag {4}): {5} of {6} words read back OK (first {7})' -f (Hex $p.B3aImemSize), (Hex $p.B3aDmemSize), $p.B3aScrubPolls, (Hex $p.B3aImemBase), (Hex $p.B3aBlStartTag), $p.B3aImemOk, $p.B3aImemWords, (Hex $p.B3aImemWord0))
      }
      if ($null -ne $p.B3aDescOk) {
        Sum ('B3a BL descriptor at DMEM 0: {0} of 19 words read back OK  CPUCTL {1}  boot address {2}' -f $p.B3aDescOk, (Hex $p.B3aCpuctl), (Hex $p.B3aBootAddr))
      }
      if ($null -ne $p.B3aFwPmuImageLen) {
        Sum ('B3a other firmware in the registry (bytes): pmu desc {0} image {1} sig {2}  fecs bl {3} inst {4} data {5} sig {6}  gpccs inst/data {7}/{8}' -f $p.B3aFwPmuDescLen, $p.B3aFwPmuImageLen, $p.B3aFwPmuSigLen, $p.B3aFwFecsBlLen, $p.B3aFwFecsInstLen, $p.B3aFwFecsDataLen, $p.B3aFwFecsSigLen, $p.B3aFwGpccsInstLen, $p.B3aFwGpccsDataLen)
      }
      Sum ('B3a result {0} ({1})' -f (Hex $p.B3aResult), $why3)
    }
    if ($null -ne $p.B3bResult) {
      # 0.4.2: the user's B3b code; show its values as they are (no decoding).
      $bv = @(); $bb = @()
      foreach ($pr in $p.PSObject.Properties) {
        if ($pr.Name -notmatch '^B3b' -or $pr.Name -eq 'B3bResult') { continue }
        if ($pr.Value -is [byte[]]) { $bb += ('{0} ({1} bytes)' -f $pr.Name.Substring(3), $pr.Value.Length) }
        else { $bv += ('{0}={1:X}' -f $pr.Name.Substring(3), [uint32]$pr.Value) }
      }
      Sum ('B3b values: ' + ($bv -join '  '))
      if ($null -ne $p.B3bPmuDmaIdx) { Sum ('B3b PMU LS loader: DMA index {0}; argv {1}' -f (Hex $p.B3bPmuDmaIdx), (Hex $p.B3bPmuArgv)) }
      if ($null -ne $p.B3cPmuCodeDmaBase) { Sum ('B3c PMU loader DMA: code/data base {0}/{1}; WPR base {2}{3}; absolute addresses {4}' -f (Hex $p.B3cPmuCodeDmaBase), (Hex $p.B3cPmuDataDmaBase), (Hex $p.B3cPmuWprBaseHi), (Hex $p.B3cPmuWprBaseLo), $p.B3cPmuDmaBaseAbsolute) }
      if ($null -ne $p.B3bPmuBld) { $pb = [byte[]]$p.B3bPmuBld; $pl = @(); for ($i = 0; $i + 4 -le $pb.Length; $i += 4) { $pl += ('{0:X8}' -f [BitConverter]::ToUInt32($pb, $i)) }; Sum ('B3b PMU WPR loader-config words (DMA index is word 0, offset {0}): {1}' -f (Hex $p.B3bPmuBldOff), ($pl -join ' ')) }
      if ($bb.Count) { Sum ('B3b binary traces: ' + ($bb -join '  ')) }
      $b3bok = 'OK'; if ([uint32]$p.B3bResult -ne 0) { $b3bok = 'error code from the B3b code' }
      Sum ('B3b result {0} ({1})' -f (Hex $p.B3bResult), $b3bok)
    }
    if ($null -ne $p.B3cPreAcrFaultClearResult) {
      $fcWhy = $b3err[(Key32 $p.B3cPreAcrFaultClearResult)]; if (-not $fcWhy) { $fcWhy = '?' }
      Sum ('B3c pre-ACR PFIFO MMU fault clear: result {0} ({1}); intr0/id {2}/{3} -> {4}/{5}; polls {6}; old payload remained after ACK {7}' -f (Hex $p.B3cPreAcrFaultClearResult), $fcWhy, (Hex $p.B3cPreAcrFifoIntr0), (Hex $p.B3cPreAcrFaultId), (Hex $p.B3cPreAcrFifoIntr0AfterClear), (Hex $p.B3cPreAcrFaultIdAfterClear), $p.B3cPreAcrFaultClearPolls, (Hex $p.B3cPreAcrFaultDataPersist))
      foreach ($w in @(@('pre-ACK active fault records', 'B3cPreAcrFaultRecs'), @('fault data after ACK', 'B3cPreAcrFaultRecsAfterAck'))) {
        if ($null -ne $p.($w[1])) {
          $fb = [byte[]]$p.($w[1]); $fv = @()
          for ($i = 0; $i + 4 -le $fb.Length; $i += 4) { $fv += ('{0:X8}' -f [BitConverter]::ToUInt32($fb, $i)) }
          Sum ('B3c {0} (unit, inst, lo, hi, info): ' -f $w[0] + ($fv -join ' '))
        }
      }
    }
    if ($null -ne $p.AcrBootResult) {
      Sum ('ACR WPR range {0:X8}{1:X8}..{2:X8}{3:X8}; blob PA {4:X8}{5:X8} size {6}; CPU readback {7}; SMMU restored {8}' -f [uint32]$p.AcrWprStartHi, [uint32]$p.AcrWprStartLo, [uint32]$p.AcrWprEndHi, [uint32]$p.AcrWprEndLo, [uint32]$p.AcrWprBufferPaHi, [uint32]$p.AcrWprBufferPaLo, $p.AcrWprBufferBytes, $p.AcrWprCpuReadback, $p.AcrSmmuRestored)
      Sum ('ACR PMU VM bind {0}; boot polls {1}; CPUCTL {2}; mailbox0/1 {3}/{4}; IRQ {5}; PC {6}; result {7}' -f $p.AcrBindOk, $p.AcrBootPolls, (Hex $p.AcrBootCpuctl), (Hex $p.AcrBootMailbox0), (Hex $p.AcrBootMailbox1), (Hex $p.AcrBootIrq), (Hex $p.AcrBootPc), (Hex $p.AcrBootResult))
      if ($null -ne $p.AcrPdeIndex) {
        if ($null -ne $p.AcrPdbPageModeOk) {
          Sum ('ACR page geometry probe: MMU_CTRL {0} -> {1}; use PDB big-page size bit11 {2}; PDB big-page bit11=0 (128K); PGD index=VA>>27; DMACTL {3}, ITFEN {4}, TRANSCFG1 {5} (before bind; read only)' -f (Hex $p.AcrMmuPageCtrlBefore), (Hex $p.AcrMmuPageCtrlAfter), $p.AcrPdbPageModeOk, (Hex $p.AcrPreBootDmactl), (Hex $p.AcrPreBootItfen), (Hex $p.AcrPreBootTranscfg1))
        }
        if ($null -ne $p.AcrIommuTagHi) {
          Sum ('ACR IOMMU tag probe: bit34 tag high {0}; arena {1}:{2}; instance {3}:{4}; PGD {5}:{6}; PGT {7}:{8}; image {9}:{10}' -f (Hex $p.AcrIommuTagHi), (Hex $p.AcrArenaIovaHi), (Hex $p.AcrArenaIova), (Hex $p.AcrInstIovaHi), (Hex $p.AcrInstIova), (Hex $p.AcrPgdIovaHi), (Hex $p.AcrPgdIova), (Hex $p.AcrPgtIovaHi), (Hex $p.AcrPgtIova), (Hex $p.AcrImageIovaHi), (Hex $p.AcrImageIova))
          Sum ('ACR NEW_INSTBLK encoded/read {0}/{1} (readback diagnostic only)' -f (Hex $p.AcrBindInstWritten), (Hex $p.AcrBindInstRead))
        }
        Sum ('ACR VM walk: PGD[{0}]={1}:{2}; PTE[{3}]={4}:{5}; GM20B NCOH PTE aperture=0' -f (Hex $p.AcrPdeIndex), (Hex $p.AcrPdeHi), (Hex $p.AcrPdeLo), (Hex $p.AcrPteIndex), (Hex $p.AcrPteHi), (Hex $p.AcrPteLo))
        if ($p.AcrSmallPdeHighHalf) { Sum 'ACR PDE layout: 4K SPT in high dword, low dword absent; 128K big-page layout (directory shift 27)' }
        if ($p.AcrSocTablePolicy) {
          Sum ('ACR table-route probe: PDB {0}:{1}, VA limit {2}:{3}; PDB target VIDMEM=0/VOL=1, PDE target VIDMEM=1/VOL=1, invalidate target=0; PMU bind SYS_NCOH' -f (Hex $p.AcrInstPdbHi), (Hex $p.AcrInstPdbLo), (Hex $p.AcrInstLimitHi), (Hex $p.AcrInstLimitLo))
        }
      }
      if ($null -ne $p.AcrSmmuMapReadback) {
        $smmuMode = 'active identity-map readback'
        if ([uint32]$p.AcrSmmuMapReadback -eq 2) { $smmuMode = 'GPU SMMU bypass; direct physical addresses' }
        elseif ([uint32]$p.AcrSmmuMapReadback -eq 0) { $smmuMode = 'active identity-map mismatch observed' }
        Sum ('ACR outer SMMU: {0}; result {1}; GPU ASID {2}; PTB_DATA read {3}; PDE[{4}] expected/read {5}/{6}' -f $smmuMode, $p.AcrSmmuMapReadback, (Hex $p.AcrSmmuGpuAsid), (Hex $p.AcrSmmuPtbDataRead), (Hex $p.AcrSmmuPdeIndex), (Hex $p.AcrSmmuPdeExpected), (Hex $p.AcrSmmuPdeRead))
        if ([uint32]$p.AcrSmmuMapReadback -ne 2) {
          Sum ('ACR outer SMMU PTE expected/read: arena {0}/{1}; PGD {2}/{3}; PGT {4}/{5}' -f (Hex $p.AcrSmmuArenaPteExpected), (Hex $p.AcrSmmuArenaPteRead), (Hex $p.AcrSmmuPgdPteExpected), (Hex $p.AcrSmmuPgdPteRead), (Hex $p.AcrSmmuPgtPteExpected), (Hex $p.AcrSmmuPgtPteRead))
          Sum ('ACR outer SMMU PTE expected/read: instance {0}/{1}; image {2}/{3}' -f (Hex $p.AcrSmmuInstPteExpected), (Hex $p.AcrSmmuInstPteRead), (Hex $p.AcrSmmuImagePteExpected), (Hex $p.AcrSmmuImagePteRead))
        }
        Sum 'ACR outer SMMU readback checks CPU-visible table contents only; they do not prove device DMA.'
      }
      Sum ('ACR selected-PDB invalidate: {0}; free slots {1}; queue polls {2} status {3}; status bit 15 before {4}; PDB written {5} readback {6} (diagnostic); status polls {7} final {8}' -f $p.AcrMmuInvOk, $p.AcrMmuInvFreeSlots, $p.AcrMmuInvSlotPolls, (Hex $p.AcrMmuInvSlotStatus), $p.AcrMmuInvPendingBefore, (Hex $p.AcrMmuInvPdb), (Hex $p.AcrMmuInvPdbRead), $p.AcrMmuInvDonePolls, (Hex $p.AcrMmuInvDoneStatus))
      if ($null -ne $p.AcrMmuAllPdbInvOk) {
        Sum ('ACR all-PDB invalidate discriminator: queue empty observed {0}; free slots {1}; pre {2}; request {3}; post-write {4}; polls {5}; final {6} (status bit is not proof of request completion)' -f $p.AcrMmuAllPdbInvOk, $p.AcrMmuAllPdbFreeSlots, (Hex $p.AcrMmuAllPdbBeforeStatus), (Hex $p.AcrMmuAllPdbRequest), (Hex $p.AcrMmuAllPdbPostWriteStatus), $p.AcrMmuAllPdbStatusPolls, (Hex $p.AcrMmuAllPdbStatus))
      }
      Sum 'ACR test is one-shot; GPU reset follows the bounded wait. Check the final GPU state separately.'
    }
    if ($null -ne $p.B3cFifoPmcNow) {
      Sum ('B3c memory path before ACR: priv ring master {0}  LTC nr {1}  tag base {2}  PFIFO PMC_ENABLE {3} -> {4}  intr0 after clear {5}  FB flush polls {6} reg {7}' -f (Hex $p.RingMaster), (Hex $p.B3cLtcNr), (Hex $p.B3cTagBase), (Hex $p.B3cFifoPmcWas), (Hex $p.B3cFifoPmcNow), (Hex $p.B3cFifoIntr0Clr), $p.B3cFbFlushPolls, (Hex $p.B3cFbFlushReg))
    }
    if ($null -ne $p.B3cDiagDone) {
      $tp = ''
      if ($null -ne $p.B3cTracePc) { $tb = [byte[]]$p.B3cTracePc; $tl = @(); for ($i = 0; $i + 4 -le $tb.Length; $i += 4) { $tl += ('{0:X}' -f [BitConverter]::ToUInt32($tb, $i)) }; $tp = $tl -join ' ' }
      Sum ('B3c after the boot wait: trace idx {0} ({1} entries) PCs {2}  DMATRFCMD {3} base {4} moffs {5} fboffs {6}  DMACTL {7}' -f (Hex $p.B3cTraceIdx), $p.B3cTraceNr, $tp, (Hex $p.B3cDmaCmd), (Hex $p.B3cDmaBase), (Hex $p.B3cDmaMoffs), (Hex $p.B3cDmaFboffs), (Hex $p.B3cDmactl))
      Sum ('B3c faults during the boot: MC int {0} err {1} adr {2}  PFIFO intr0 {3} (bit 28 = MMU fault) fault id {4}' -f (Hex $p.B3cMcInt), (Hex $p.B3cMcErr), (Hex $p.B3cMcErrAdr), (Hex $p.B3cFifoIntr0), (Hex $p.B3cFifoFaultId))
      if ($null -ne $p.B3cPostAcrPmuSctl) {
        Sum ('B3c falcons after ACR: PMC_ENABLE {0} (GR bit 12 = {1})  PMU CPUCTL {2} SCTL {3} (mode bits 13:12 = {4}) mb0 {5} mb1 {6}' -f (Hex $p.B3cLsPmcEnable), (([uint32]$p.B3cLsPmcEnable -shr 12) -band 1), (Hex $p.B3cPostAcrPmuCpuctl), (Hex $p.B3cPostAcrPmuSctl), (([uint32]$p.B3cPostAcrPmuSctl -shr 12) -band 3), (Hex $p.B3cLsPmuMb0), (Hex $p.B3cLsPmuMb1))
        if ([uint32]$p.B3cLsGrRead -eq 1) {
          Sum ('B3c GR falcons after ACR: FECS CPUCTL {0} SCTL {1} (mode {2}) mb0 {3}  GPCCS CPUCTL {4} SCTL {5} (mode {6}) mb0 {7}' -f (Hex $p.B3cLsFecsCpuctl), (Hex $p.B3cLsFecsSctl), (([uint32]$p.B3cLsFecsSctl -shr 12) -band 3), (Hex $p.B3cLsFecsMb0), (Hex $p.B3cLsGpccsCpuctl), (Hex $p.B3cLsGpccsSctl), (([uint32]$p.B3cLsGpccsSctl -shr 12) -band 3), (Hex $p.B3cLsGpccsMb0))
        } else {
          Sum 'B3c GR falcons after ACR: not read (GR is not enabled in PMC_ENABLE; nothing was enabled)'
        }
      }
      if ($null -ne $p.B3cLsResult) {
        $lsWhy = $b3err[(Key32 $p.B3cLsResult)]; if (-not $lsWhy) { $lsWhy = '?' }
        if ($null -ne $p.B3cLsPmuDmaCtlBeforeStart) { Sum ('B3c LS start DMACTL snapshot: before start {0}; at CPUCTL_ALIAS write {1}' -f (Hex $p.B3cLsPmuDmaCtlBeforeStart), (Hex $p.B3cLsPmuDmaCtlAtStart)) }
        Sum ('B3c LS PMU start result {0} ({1}); INIT queue polls {2} head/tail {3}/{4}; CPUCTL {5} SCTL {6} mailbox0/1 {7}/{8}' -f (Hex $p.B3cLsResult), $lsWhy, $p.B3cLsPmuInitPolls, (Hex $p.B3cLsPmuMsgHead), (Hex $p.B3cLsPmuMsgTail), (Hex $p.B3cLsPmuCpuctl), (Hex $p.B3cLsPmuSctl), (Hex $p.B3cLsPmuMailbox0), (Hex $p.B3cLsPmuMailbox1))
        if ($p.B3cLsPmuInitBadPairs) { Sum ('B3c LS PMU INIT queue: transient bad head/tail pairs {0}; first {1}/{2} at poll {3}' -f $p.B3cLsPmuInitBadPairs, (Hex $p.B3cLsPmuInitBadHead), (Hex $p.B3cLsPmuInitBadTail), $p.B3cLsPmuInitBadPoll) }
        Sum ('B3c LS PMU setup: DMEM {0} bytes; IRQSTAT after clear {1}; aperture0/4 {2}/{3}; args readback {4} offset/cursor {5}/{6}; ITFEN {7}; CPUCTL_ALIAS before/after {8}/{9}' -f $p.B3cLsPmuDmemSize, (Hex $p.B3cLsPmuIrqAfterClear), (Hex $p.B3cLsPmuAperture0), (Hex $p.B3cLsPmuAperture4), $p.B3cLsPmuArgsOk, (Hex $p.B3cLsPmuArgsOff), (Hex $p.B3cLsPmuArgsCursor), (Hex $p.B3cLsPmuItfenBeforeStart), (Hex $p.B3cLsPmuCpuctlAliasBefore), (Hex $p.B3cLsPmuCpuctlAliasAfter))
        if ($null -ne $p.B3cLsPmuInitHdr) { Sum ('B3c LS PMU INIT header {0}; msg type {1}; PIO cursor {2}; Step result {3}' -f (Hex $p.B3cLsPmuInitHdr), (Hex $p.B3cLsPmuInitType), (Hex $p.B3cLsPmuInitPioCursor), (Hex $p.B3cLsPmuStepResult)) }
        $lp = ''; if ($null -ne $p.B3cLsPmuTracePc) { $lb = [byte[]]$p.B3cLsPmuTracePc; $ll = @(); for ($i = 0; $i + 4 -le $lb.Length; $i += 4) { $ll += ('{0:X}' -f [BitConverter]::ToUInt32($lb, $i)) }; $lp = $ll -join ' ' }
        Sum ('B3c LS PMU after wait: IRQSTAT {0}; PC trace idx {1} ({2} entries) PCs {3}' -f (Hex $p.B3cLsPmuIrqStat), (Hex $p.B3cLsPmuTraceIdx), $p.B3cLsPmuTraceNr, $lp)
        Sum ('B3c LS PMU diagnostic registers: IRQMODE/MASK/DEST {0}/{1}/{2}; GPTMR INT/VAL {3}/{4}; PTIMER low/high {5}/{6}' -f (Hex $p.B3cLsPmuIrqMode), (Hex $p.B3cLsPmuIrqMask), (Hex $p.B3cLsPmuIrqDest), (Hex $p.B3cLsPmuGptmrInt), (Hex $p.B3cLsPmuGptmrVal), (Hex $p.B3cLsPmuPtimerLow), (Hex $p.B3cLsPmuPtimerHigh))
        if ($null -ne $p.B3cLsPmuDmaCmd) {
          Sum ('B3c LS PMU loader DMA: cmd/base/moffs/fboffs {0}/{1}/{2}/{3}; DMACTL {4}; bind status/inst {5}/{6}; context enable/action {7}/{8}' -f (Hex $p.B3cLsPmuDmaCmd), (Hex $p.B3cLsPmuDmaBase), (Hex $p.B3cLsPmuDmaMoffs), (Hex $p.B3cLsPmuDmaFboffs), (Hex $p.B3cLsPmuDmaCtl), (Hex $p.B3cLsPmuBindStatus), (Hex $p.B3cLsPmuBindInst), (Hex $p.B3cLsPmuCtxEnable), (Hex $p.B3cLsPmuCtxAction))
          Sum ('B3c LS PMU FBIF: FBIF_CTL {0}; TRANSCFG words are listed below' -f (Hex $p.B3cLsPmuFbifCtl))
          Sum ('B3c LS PMU post-wait faults (asserted ID units only): PFIFO intr0 {0}; fault id {1}; current fault records {2}; MC int/err/adr {3}/{4}/{5}' -f (Hex $p.B3cLsPmuPfifoIntr0), (Hex $p.B3cLsPmuFaultId), $p.B3cLsPmuFaultRecCount, (Hex $p.B3cLsPmuMcInt), (Hex $p.B3cLsPmuMcErr), (Hex $p.B3cLsPmuMcErrAdr))
          Sum ('B3c LS PMU IMEM: HWCFG {0}; bytes {1}; PC-window offset {2}; read valid {3}; tag before/selected/after {4}/{5}/{6}' -f (Hex $p.B3cLsPmuImemHwcfg), (Hex $p.B3cLsPmuImemSize), (Hex $p.B3cLsPmuImemReadOff), $p.B3cLsPmuImemReadOk, (Hex $p.B3cLsPmuImemTagBefore), (Hex $p.B3cLsPmuImemTagSelected), (Hex $p.B3cLsPmuImemTagAfter))
        }
        Sum ('B3c LS PMU argv: advertised {0}; computed {1}; size {2}; location match {3}; write readback {4}' -f (Hex $p.B3cLsPmuArgvAdvertised), (Hex $p.B3cLsPmuArgvComputed), $p.B3cLsPmuArgsSize, $p.B3cLsPmuArgvMatch, $p.B3cLsPmuArgsOk)
        foreach ($w in @(@('argv words', 'B3cLsPmuArgvReadback', 'B3cLsPmuArgvComputed'), @('args context', 'B3cLsPmuArgsWindow', 'B3cLsPmuArgsWindowOff'), @('queue data', 'B3cLsPmuQueueDataWindow', 'B3cLsPmuQueueDataOff'), @('IMEM around last PC', 'B3cLsPmuImemPcWindow', 'B3cLsPmuImemReadOff'))) {
          if ($null -ne $p.($w[1])) {
            $wb = [byte[]]$p.($w[1]); $wl = @()
            for ($i = 0; $i + 4 -le $wb.Length; $i += 4) { $wl += ('{0:X8}' -f [BitConverter]::ToUInt32($wb, $i)) }
            Sum ('B3c LS PMU {0} window at {1}: {2}' -f $w[0], (Hex $p.($w[2])), ($wl -join ' '))
          }
        }
        if ($null -ne $p.B3cLsPmuFaultRecs) {
          $fw = [byte[]]$p.B3cLsPmuFaultRecs; $fl = @()
          for ($i = 0; $i + 4 -le $fw.Length; $i += 4) { $fl += ('{0:X8}' -f [BitConverter]::ToUInt32($fw, $i)) }
          Sum ('B3c LS PMU MMU fault records (unit, inst, lo, hi, info): ' + ($fl -join ' '))
        }
        if ($null -ne $p.B3cLsPmuTranscfg) {
          $tw = [byte[]]$p.B3cLsPmuTranscfg; $tl = @()
          for ($i = 0; $i + 4 -le $tw.Length; $i += 4) { $tl += ('{0:X8}' -f [BitConverter]::ToUInt32($tw, $i)) }
          Sum ('B3c LS PMU TRANSCFG words: ' + ($tl -join ' '))
        }
      }
      if ($null -ne $p.B3cFecsResult) {
        $frWhy = $b3err[(Key32 $p.B3cFecsResult)]; if (-not $frWhy) { $frWhy = '?' }
        Sum ('B3c FECS WPR loader: DMA index {0}; code/data DMA base {1}/{2}; absolute WPR bases {3}' -f (Hex $p.B3cFecsDmaIdx), (Hex $p.B3cFecsCodeDmaBase), (Hex $p.B3cFecsDataDmaBase), $p.B3cFecsDmaBaseAbsolute)
        Sum ('B3c FECS bootstrap result {0} ({1}); INIT queue HPQ base/size/index {2}/{3}/{4}; MSGQ base/size {5}/{6}' -f (Hex $p.B3cFecsResult), $frWhy, (Hex $p.B3cFecsHpqBase), (Hex $p.B3cFecsHpqSize), (Hex $p.B3cFecsHpqIndex), (Hex $p.B3cFecsMsgqBase), (Hex $p.B3cFecsMsgqSize))
        Sum ('B3c FECS WPR init reply header/control/code {0}/{1}/{2}; polls {3}; bootstrap reply header/control/payload {4}/{5}/{6}; polls {7}' -f (Hex $p.B3cFecsWprReplyHeader), (Hex $p.B3cFecsWprReplyCtrl), (Hex $p.B3cFecsWprReplyCode), $p.B3cFecsWprReplyPolls, (Hex $p.B3cFecsReplyHeader), (Hex $p.B3cFecsReplyCtrl), (Hex $p.B3cFecsReplyPayload), $p.B3cFecsReplyPolls)
        Sum ('B3c FECS HPQ head/tail after WPR init {0}/{1}; after bootstrap wait {2}/{3}; MSGQ head/tail after wait {4}/{5}' -f (Hex $p.B3cFecsHpqHeadAfterWprInit), (Hex $p.B3cFecsHpqTailAfterWprInit), (Hex $p.B3cFecsHpqHeadAfterWait), (Hex $p.B3cFecsHpqTailAfterWait), (Hex $p.B3cFecsMsgHeadAfterWait), (Hex $p.B3cFecsMsgTailAfterWait))
        Sum ('B3c FECS bootstrap sequence {0}; PMU CPUCTL/IRQSTAT immediately after post {1}/{2}; IRQSTAT after wait {3}; PMU PC trace idx/count after post {4}/{5}; after wait {6}/{7}; HPQ samples {8}' -f (Hex $p.B3cFecsBootstrapSeq), (Hex $p.B3cFecsPmuCpuctlAfterPost), (Hex $p.B3cFecsPmuIrqStatAfterPost), (Hex $p.B3cFecsPmuIrqStatAfterWait), (Hex $p.B3cFecsPmuTraceIdxAfterPost), (Hex $p.B3cFecsPmuTraceCountAfterPost), (Hex $p.B3cFecsPmuTraceIdxAfterWait), (Hex $p.B3cFecsPmuTraceCountAfterWait), $p.B3cFecsHpqSampleCount)
        Sum 'B3c PMU checkpoints: WPR ack -> GR enable -> before FECS start-mailbox sequence -> before bootstrap'
        Sum ('B3c FECS bootstrap command head {0}; ready mailbox {1}; first nonzero {2}; mailbox0 changes {3}; polls {4}; GR PMC_ENABLE before/after {5}/{6}' -f (Hex $p.B3cFecsCommandHeadAfter), (Hex $p.B3cFecsReadyMailbox), (Hex $p.B3cFecsReadyFirstNonzero), (Hex $p.B3cFecsReadyMailboxChanges), $p.B3cFecsReadyPolls, (Hex $p.B3cFecsPmcEnableBefore), (Hex $p.B3cFecsPmcEnableAfter))
        Sum ('B3c FECS pre-start mailbox sequence (clear0=FFFFFFFF, mailbox1=1, clear6=FFFFFFFF): mailbox0 after clear {0}; mailbox1 {1}; clear6 snapshot {2}; mailbox6 {3}; mailbox0 after sequence {4}; FECS DMACTL before/after {5}/{6}' -f (Hex $p.B3cFecsCtxswMailbox0AfterClear), (Hex $p.B3cFecsCtxswMailbox1Set), (Hex $p.B3cFecsCtxswMailboxClear6Snapshot), (Hex $p.B3cFecsCtxswMailbox6), (Hex $p.B3cFecsCtxswMailbox0AfterSecureSeq), (Hex $p.B3cFecsDmaCtlBeforeStart), (Hex $p.B3cFecsDmaCtlAfterStartPrep))
        Sum ('B3c GPCCS firmware inst/data bytes {0}/{1}; source/padded IMEM words {2}/{3}; DMEM words {4}; HWCFG IMEM/DMEM bytes {5}/{6}; start CPUCTL before/after {7}/{8}; alias {9}; mailbox0 {10}; BOOTVEC before/after {11}/{12}; HALT-clear polls {13}' -f (Hex $p.B3cGpccsInstLen), (Hex $p.B3cGpccsDataLen), (Hex $p.B3cGpccsImemSourceWords), (Hex $p.B3cGpccsImemWordsWritten), (Hex $p.B3cGpccsDmemWordsWritten), (Hex $p.B3cGpccsImemSize), (Hex $p.B3cGpccsDmemSize), (Hex $p.B3cGpccsCpuctlBeforeStart), (Hex $p.B3cGpccsCpuctlAfterStart), (Hex $p.B3cGpccsStartUsesAlias), (Hex $p.B3cGpccsMailbox0AfterStart), (Hex $p.B3cGpccsBootvecBeforeStart), (Hex $p.B3cGpccsBootvecAfterStart), $p.B3cGpccsStartPolls)
        Sum ('B3c FECS ctxsw ready at 0x409800: nvgpu expects exact 0x10, Nouveau waits for bit0; accept exact 0x10 or bit0 and track any nonzero/change; observed {0} after {1} polls; legacy 0x419A00 before/after {2}/{3}; FECS CPUCTL/SCTL/mailbox0/1 {4}/{5}/{6}/{7}; PC trace idx/count {8}/{9}; IRQSTAT/MASK {10}/{11}; PFIFO intr0/fault id {12}/{13}' -f (Hex $p.B3cFecsReadyMailbox), $p.B3cFecsReadyPolls, (Hex $p.B3cFecsLegacyMailbox419A00BeforeStartSeq), (Hex $p.B3cFecsLegacyMailbox419A00AfterWait), (Hex $p.B3cFecsCpuctl), (Hex $p.B3cFecsSctl), (Hex $p.B3cFecsMailbox0), (Hex $p.B3cFecsMailbox1), (Hex $p.B3cFecsTraceIdx), $p.B3cFecsTraceCount, (Hex $p.B3cFecsIrqStat), (Hex $p.B3cFecsIrqMask), (Hex $p.B3cFecsPfifoIntr0), (Hex $p.B3cFecsFaultId))
        Sum ('B3c GPCCS after wait: CPUCTL/SCTL {0}/{1}; Falcon mailbox0/1 {2}/{3}; BOOTVEC {4}; PC trace idx/count {5}/{6}; IRQSTAT/MASK {7}/{8}; ctxsw mailbox base 0x41A800' -f (Hex $p.B3cGpccsCpuctl), (Hex $p.B3cGpccsSctl), (Hex $p.B3cGpccsFalconMailbox0), (Hex $p.B3cGpccsFalconMailbox1), (Hex $p.B3cGpccsBootvecFinal), (Hex $p.B3cGpccsTraceIdx), $p.B3cGpccsTraceCount, (Hex $p.B3cGpccsIrqStat), (Hex $p.B3cGpccsIrqMask))
        Sum 'B3c Step 73 FECS DMA snapshot/GPCCS firmware validation; Step 74 GPCCS IMEM/DMEM PIO with 256-byte IMEM padding; Step 75 ordered mailbox setup, GPCCS then FECS start and ready wait'
        foreach ($w in @(@('PMU checkpoint after WPR ACK', 'B3cPmuCheckpointAfterWprAck'), @('PMU checkpoint after GR enable', 'B3cPmuCheckpointAfterGrEnable'), @('PMU checkpoint before FECS secure-start mailbox sequence', 'B3cPmuCheckpointBeforeFecsStartMailboxSeq'), @('PMU checkpoint before bootstrap', 'B3cPmuCheckpointBeforeBootstrap'), @('WPR init command', 'B3cFecsWprInitCommand'), @('bootstrap command', 'B3cFecsBootstrapCommand'), @('PMU PC trace after post', 'B3cFecsPmuTraceAfterPost'), @('PMU PC trace after wait', 'B3cFecsPmuTraceAfterWait'), @('HPQ tail sample polls', 'B3cFecsHpqSamplePolls'), @('HPQ tail samples', 'B3cFecsHpqTailSamples'), @('PMU IRQSTAT samples', 'B3cFecsPmuIrqSamples'), @('FECS PC trace', 'B3cFecsTracePc'), @('FECS ctxsw mailboxes 0..7', 'B3cFecsCtxswMailboxWords'), @('GPCCS PC trace', 'B3cGpccsTracePc'), @('GPCCS ctxsw mailboxes 0..7', 'B3cGpccsCtxswMailboxWords'), @('FECS MMU fault records', 'B3cFecsFaultRecs'), @('FECS WPR BLD words', 'B3cFecsBldWords'))) {
          if ($null -ne $p.($w[1])) {
            $wb = [byte[]]$p.($w[1]); $wl = @()
            for ($i = 0; $i + 4 -le $wb.Length; $i += 4) { $wl += ('{0:X8}' -f [BitConverter]::ToUInt32($wb, $i)) }
            Sum ('{0}: {1}' -f $w[0], ($wl -join ' '))
          }
        }
      }
      if ($null -ne $p.B3cFaultRecs) {
        $reasons3 = @('PDE', 'PDE_SIZE', 'PTE', 'VA_LIMIT_VIOLATION', 'UNBOUND_INST_BLOCK', 'PRIV_VIOLATION', 'RO_VIOLATION', 'WO_VIOLATION', 'PITCH_MASK_VIOLATION', 'WORK_CREATION', 'UNSUPPORTED_APERTURE', 'COMPRESSION_FAILURE', 'UNSUPPORTED_KIND', 'REGION_VIOLATION', 'BOTH_PTES_VALID', 'POISONED')
        $fr3 = [byte[]]$p.B3cFaultRecs
        for ($o = 0; $o + 20 -le $fr3.Length; $o += 20) {
          $u = [BitConverter]::ToUInt32($fr3, $o); $fi = [BitConverter]::ToUInt32($fr3, $o + 4); $flo = [BitConverter]::ToUInt32($fr3, $o + 8); $fhi = [BitConverter]::ToUInt32($fr3, $o + 12); $fty = [BitConverter]::ToUInt32($fr3, $o + 16)
          $acc = 'read'; if ($fty -band 0x80) { $acc = 'write' }
          $hub = 'gpc'; if ($fty -band 0x40) { $hub = 'hub' }
          Sum ('B3c MMU fault record unit 0x{0:X2}: inst {1} va 0x{2:X}{3:X8} info {4} -> type {5} ({6}) {7} {8} client 0x{9:X2}' -f $u, (Hex $fi), $fhi, $flo, (Hex $fty), ($fty -band 0xF), $reasons3[$fty -band 0xF], $acc, $hub, (($fty -shr 8) -band 0x1F))
        }
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
  { $_ -eq 'PowerTest' -or $_ -eq 'ProbeTest' -or $_ -eq 'DmaTest' -or $_ -eq 'FwTest' -or $_ -eq 'AcrTest' } {
    $tv = 1; if ($Mode -eq 'ProbeTest') { $tv = 2 }; if ($Mode -eq 'DmaTest') { $tv = 3 }; if ($Mode -eq 'FwTest') { $tv = 4 }; if ($Mode -eq 'AcrTest') { $tv = 5 }
    New-Item -Path $Key -Force | Out-Null
    $old = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    if ($old) {
      foreach ($n in @($old.PSObject.Properties | Where-Object { $_.Name -match '^(Acr|B2a|B3a|B3b|B3c|Smmu|Mc[IE]|McHotStat(Before|After)Dma|McHotStatAfter(Reset|Flush|Bar0)|McHotStatB2aStart|Pmu|Pfifo|Ptop|Scal|Dmem|Ring|Wpr|PmcEnable)' } | ForEach-Object { $_.Name })) {
        Remove-ItemProperty -Path $Key -Name $n -ErrorAction SilentlyContinue
      }
    }
    if ($Mode -eq 'FwTest' -or $Mode -eq 'AcrTest') {
      # The firmware is the user's own linux-firmware copy; the kit never ships it.
      # Where to look, in order: inside the kit, then fixed places on C:.
      # If none has it, reuse what an earlier FwTest left in the registry.
      $fwPlaces = @((Join-Path $Here 'firmware\nvidia\gm20b'), (Join-Path $Here 'firmware\gm20b'), (Join-Path $Here 'firmware'),
                    'C:\SwitchWOA\firmware\nvidia\gm20b', 'C:\firmware\nvidia\gm20b', 'C:\lib\firmware\nvidia\gm20b')
      $fwDir = $null
      foreach ($c in $fwPlaces) {
        if (Test-Path (Join-Path $c 'acr\bl.bin')) { $fwDir = $c; break }
      }
      $fwList = @(@('acr\bl.bin', 'FwAcrBl', 832), @('acr\ucode_load.bin', 'FwAcrUcodeLoad', 18592),
                       @('pmu\desc.bin', 'FwPmuDesc', 652), @('pmu\image.bin', 'FwPmuImage', 47872), @('pmu\sig.bin', 'FwPmuSig', 76),
                       @('gr\fecs_bl.bin', 'FwFecsBl', 576), @('gr\fecs_inst.bin', 'FwFecsInst', 17021), @('gr\fecs_data.bin', 'FwFecsData', 1964), @('gr\fecs_sig.bin', 'FwFecsSig', 76),
                       @('gr\gpccs_inst.bin', 'FwGpccsInst', 0), @('gr\gpccs_data.bin', 'FwGpccsData', 0))
      $fwOk = $true
      if (-not $fwDir) {
        $have = Get-ItemProperty $Key -ErrorAction SilentlyContinue
        $missing = @($fwList | Where-Object { -not ($have -and ($have.PSObject.Properties.Name -contains $_[1])) })
        if ($missing.Count) {
          Sum ('firmware not found. Put your own linux-firmware nvidia\gm20b folder in one of: ' + ($fwPlaces -join ' ; '))
          break
        }
        Sum 'firmware folder not found: using the copy an earlier FwTest left in the registry'
        foreach ($f in $fwList) {
          $bytes = [byte[]]$have.($f[1])
          $sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)).Replace('-', '')
          Sum ('firmware {0} (registry): {1} bytes  SHA256 {2}' -f $f[0], $bytes.Length, $sha)
        }
      }
      else { Sum "firmware folder: $fwDir" }
      foreach ($f in $fwList) {
        if (-not $fwDir) { break }
        $fp = Join-Path $fwDir $f[0]
        if (-not (Test-Path $fp)) { Sum "missing: $fp"; $fwOk = $false; break }
        $bytes = [IO.File]::ReadAllBytes($fp)
        $sha = (Get-FileHash $fp -Algorithm SHA256).Hash
        $note = ''; if ($f[2] -gt 0 -and $bytes.Length -ne $f[2]) { $note = (' (expected {0} bytes)' -f $f[2]) }
        Sum ('firmware {0}: {1} bytes{2}  SHA256 {3}' -f $f[0], $bytes.Length, $note, $sha)
        Set-ItemProperty -Path $Key -Name $f[1] -Value $bytes -Type Binary
      }
      if (-not $fwOk) { break }
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
    Remove-ItemProperty -Path $Key -Name FwAcrBl, FwAcrUcodeLoad, FwPmuDesc, FwPmuImage, FwPmuSig, FwFecsBl, FwFecsInst, FwFecsData, FwFecsSig, FwGpccsInst, FwGpccsData -ErrorAction SilentlyContinue
    Show-Device
  }
}

Log ''
Log '=== SUMMARY'
foreach ($s in $Summary) { Write-Host $s }
