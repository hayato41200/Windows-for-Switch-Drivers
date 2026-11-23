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
function RegPairs($v) {
  if ($null -eq $v) { return 'n/a' }
  $b = [byte[]]$v; if (($b.Length % 8) -ne 0) { return ('malformed bytes={0}' -f $b.Length) }
  $out = @()
  for ($i = 0; $i + 8 -le $b.Length; $i += 8) {
    $a = [BitConverter]::ToUInt32($b, $i); $x = [BitConverter]::ToUInt32($b, $i + 4)
    $out += ('{0:X8}={1:X8}' -f $a, $x)
  }
  return ($out -join ' ')
}
function RegTriples($v) {
  if ($null -eq $v) { return 'n/a' }
  $b = [byte[]]$v; if (($b.Length % 12) -ne 0) { return ('malformed bytes={0}' -f $b.Length) }
  $out = @()
  for ($i = 0; $i + 12 -le $b.Length; $i += 12) {
    $a = [BitConverter]::ToUInt32($b, $i); $e = [BitConverter]::ToUInt32($b, $i + 4); $x = [BitConverter]::ToUInt32($b, $i + 8)
    $out += ('{0:X8}:{1:X8}>{2:X8}' -f $a, $e, $x)
  }
  return ($out -join ' ')
}
function Words($v) {
  if ($null -eq $v) { return 'n/a' }
  $b = [byte[]]$v; if (($b.Length % 4) -ne 0) { return ('malformed bytes={0}' -f $b.Length) }
  $out = @()
  for ($i = 0; $i + 4 -le $b.Length; $i += 4) { $out += ('{0:X8}' -f [BitConverter]::ToUInt32($b, $i)) }
  return ($out -join ' ')
}

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
    $steps[76] = 'C1: query FECS golden, zcull, and PM image sizes'
    $steps[77] = 'C2a: import and validate GR init lists'
    $steps[78] = 'C2b: reset GR ctxsw state and apply bounded nonctx list'
    $steps[79] = 'C2c: validate and apply bounded sw_ctx list'
    $steps[80] = 'C2d: allocate and map isolated GR context backing'
    $steps[81] = 'C2e: bind FECS, apply global buffers and bundle init, save golden image'
    $steps[82] = 'C2e: FE power handshake, ctxsw reset, and SCC initialization'
    $steps[83] = 'C2e: GM20B fixed-topology floorsweep and zcull setup'
    $steps[84] = 'C2e: GR interrupts, FE HWW, and exception setup before FECS bind'
    $steps[85] = 'C2e: finish bundle state, bounded idle waits, and pre-save L2 flush'
    $steps[86] = 'C2e: replay sw_ctx after FECS channel bind in nvgpu order'
    $steps[87] = 'C2e: bounded nouveau-predicate GR idle checks at bundle and save boundaries'
    $steps[88] = 'C2e: GM20B nouveau generation order and true-idle checkpoints'
    $steps[89] = 'C2b: configure GM20B GPC MMU after nonctx and idle'
    $steps[90] = 'C2e: snapshot GPC/TPC PRI and stop on first FE exception change'
    $steps[91] = 'C2e: capture GM20B GR hang status snapshots'
    $steps[92] = 'C2e: initialize GM20B LTC active counts, slices, and interrupts'
    $steps[93] = 'C2e: publish active LTC count to the GM20B FB hub'
    $steps[94] = 'C2e: load GM20B default ZBC color and depth tables'
    $steps[95] = 'C2e: select nvgpu-supported zero-comptag CBC mode'
    $steps[96] = 'C2e: disable GM20B LG and SU read-coalesce before sw_ctx'
    $steps[97] = 'C2e: apply and verify GM20B BE active-LTC ROP settings'
    $steps[101] = 'C2e: set PE stream master and initialize fixed-topology zcull'
    $steps[102] = 'C2e: set Nouveau BE active-FBP count from hardware'
    $steps[103] = 'C2e: enable GR FIFO and semaphore access'
    $steps[104] = 'D1: inspect topology and initialize PFIFO/PBDMA'
    $steps[105] = 'D2: create and bind one empty GPFIFO channel'
    $steps[106] = 'D3: ring USERD GP_PUT through BAR1 and verify host semaphore release'
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
      $b3err = @{ '00000000' = 'OK'; 'E0000041' = 'firmware missing in the registry (run FwTest from the kit)'; 'E0000042' = 'bl.bin too short'; 'E0000043' = 'bl.bin header bad'; 'E0000044' = 'bl.bin descriptor bad'; 'E0000045' = 'ucode_load.bin too short'; 'E0000046' = 'ucode_load.bin header bad'; 'E0000047' = 'patch location outside the file'; 'E0000048' = 'load header outside the file'; 'E0000049' = 'app table bad'; 'E000004A' = 'signature/section offsets bad'; 'E000004B' = 'no memory'; 'E000004C' = 'bootloader does not fit IMEM'; 'E000004D' = 'no memory'; 'E0000061' = 'invalid WPR range'; 'E0000062' = 'built WPR is larger than the hardware protected region'; 'E0000063' = 'ACR descriptor is too short'; 'E0000064' = 'could not map WPR/VM memory'; 'E0000065' = 'PMU virtual-memory bind failed'; 'E0000066' = 'ACR DMA descriptor readback mismatch'; 'E0000067' = 'ACR boot timed out'; 'E0000068' = 'ACR boot mailbox indicated failure'; 'E0000069' = 'WPR CPU write/readback mismatch'; 'E000006A' = 'PMU PDB MMU invalidate failed'; 'E000006B' = 'all-PDB MMU invalidate could not observe FIFO empty'; 'E000006C' = 'MMU per-instance PDB page-size selection readback failed'; 'E000006D' = 'ACR did not leave PMU halted with empty mailboxes'; 'E000006E' = 'PMU DMEM size is outside the supported range'; 'E000006F' = 'PMU HS halt interrupt did not clear'; 'E0000070' = 'PMU LS INIT did not appear within 2000 polls (2 s)'; 'E0000071' = 'PMU INIT queue pointers stayed invalid for 2000 polls'; 'E0000072' = 'PMU INIT message header/type or PIO cursor mismatch'; 'E0000073' = 'PMU cmdline args DMEM readback/cursor mismatch'; 'E0000074' = 'WPR-advertised PMU argv does not match the runtime DMEM args location'; 'E0000075' = 'pre-ACR PFIFO MMU fault latches did not clear; ACR was skipped'; 'E0000076' = 'PMU DMACTL REQUIRE_CTX did not clear before LS start'; 'E0000077' = 'PMU loader WPR DMA address cannot be represented in the descriptor'; 'E0000078' = 'FECS bootstrap command, PMU reply, or LS-ready wait failed'; 'E0000079' = 'PMU INIT_WPR_REGION command, reply, or success status failed'; 'E000007A' = 'PMU did not dequeue the FECS bootstrap command before timeout'; 'E000007B' = 'PMU HALT observed before FECS bootstrap post'; 'E000007C' = 'PMU HALT observed immediately after FECS bootstrap post'; 'E000007D' = 'FECS CPUCTL HALT bit did not clear after CPUCTL_ALIAS start'; 'E000007E' = 'GPCCS inst/data firmware missing from registry'; 'E000007F' = 'GPCCS firmware is invalid or exceeds Falcon IMEM/DMEM capacity'; 'E0000080' = 'GPCCS CPUCTL HALT bit did not clear after start'; 'E0000081' = 'FECS size query mailbox stayed zero for 2000 polls'; 'E0000082' = 'FECS size query returned FAIL marker or invalid all-ones'; 'E0000083' = 'GR is disabled before C1 FECS method queries'; 'E0000084' = 'GR init-list blob missing or exceeds 128 KiB'; 'E0000085' = 'GR init-list blob is empty, malformed, or has an invalid register address'; 'E0000086' = 'GR is disabled before C2a list validation'; 'E0000087' = 'C2b GR reset, nonctx range/readback, scrub, or idle stage failed'; 'E0000088' = 'C2c sw_ctx validation, apply, or idle stage failed'; 'E0000089' = 'C2d GR arena allocation, bounds, topology, or PMC_ENABLE check failed'; 'E000008A' = 'C2d GR SMMU, GPU page-table, or instance-block readback failed'; 'E000008B' = 'C2e FECS method/bind or bounded GR idle wait timed out'; 'E000008C' = 'C2e FECS method failure, bundle validation, or golden image header mismatch'; 'E000008D' = 'C2e FE power/reset handshake or GR PDB invalidate failed'; 'E000008E' = 'C2e GR floorsweep/zcull or exception setup address validation/readback failed'; 'E000008F' = 'C2e FE HWW ESR is nonzero before setup or first bundle entry'; 'E0000090' = 'C2e bounded L2 flush or invalidate did not complete before golden header copy'; 'E0000091' = 'C2e GR engine remained busy through the bounded pre-save idle check'; 'E0000092' = 'C2e Nouveau GR idle predicate timed out at a generation checkpoint'; 'E0000093' = 'C2b GM20B GPC MMU register validation failed'; 'E0000094' = 'C2e FE/GR exception state changed during bundle replay'; 'E0000095' = 'priv-ring ACK_INTERRUPT command did not return to NO_CMD within 2000 polls'; 'E0000096' = 'C2e BE active-LTC ROP readback was invalid'; 'E0000097' = 'C2e active LTC count, CBC parameters, or LTC FS readback was invalid'; 'E0000098' = 'C2e FB active-LTC count readback was invalid'; 'E0000099' = 'C2e bounded ZBC idle, table validation, or ZBC readback failed'; 'E000009A' = 'C2e zero-comptag CBC base readback failed'; 'E000009B' = 'C2e bounded GM20B read-coalesce setup readback failed'; 'E000009C' = 'C2e early PE or zcull setup readback failed'; 'E000009D' = 'C2e Nouveau active-FBP ROP count setup failed'; 'E000009E' = 'C2e GR FIFO/semaphore access setup failed'; 'E000009F' = 'D1 GR-enable guard or PFIFO/PBDMA readback failed'; 'E00000A0' = 'D2 RAMFC, USERD, GPFIFO, channel bind, runlist, or bounded cleanup failed'; 'E00000A1' = 'D3 BAR1 USERD doorbell fault/readback or semaphore/GPFIFO progress failed'; 'E00000A2' = 'D4 class method, bounded GP_GET, L2 visibility, or completion semaphore failed'; 'E00000A3' = 'D4 context image/header prep or FECS watchdog/START_CTXSW method failed'; 'E00000A4' = 'D5 3D color clear, bounded channel progress, or surface readback failed' }
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
        if ($null -ne $p.B3cC1Result) {
          $c1Why = @{ '00000000' = 'OK'; 'E0000081' = 'timeout'; 'E0000082' = 'FECS FAIL marker/register value'; 'E0000083' = 'GR disabled' }[(Key32 $p.B3cC1Result)]; if (-not $c1Why) { $c1Why = '?' }
          Sum ('B3c C1 result {0} ({1}); PMC_ENABLE {2}; mailbox PASS/FAIL markers {3}/{4}; golden/zcull/PM image sizes {5}/{6}/{7}' -f (Hex $p.B3cC1Result), $c1Why, (Hex $p.B3cC1PmcEnable), (Hex $p.B3cC1MailboxPassValue), (Hex $p.B3cC1MailboxFailValue), (Hex $p.B3cC1GoldenImageSize), (Hex $p.B3cC1ZcullImageSize), (Hex $p.B3cC1PmImageSize))
          Sum 'B3c C1 size queries: FECS mailbox 0 clear=0xFFFFFFFF, data=0, push method address; each bounded to 2000 polls'
          Sum 'B3c C1 method addresses golden/zcull/PM = 0x10/0x16/0x25; data/push registers 0x409500/0x409504; result mailbox/clear 0x409800/0x409840'
        }
        if ($null -ne $p.B3cC2aResult) {
          $c2aWhy = $b3err[(Key32 $p.B3cC2aResult)]; if (-not $c2aWhy) { $c2aWhy = '?' }
          Sum ('B3c C2a result {0} ({1}); PMC_ENABLE {2}' -f (Hex $p.B3cC2aResult), $c2aWhy, (Hex $p.B3cC2aPmcEnable))
          Sum 'B3c C2a validates formats; C2b applies sw_nonctx; C2c applies sw_ctx AIV address/value writes; bundle/method IDs are not BAR0 writes'
          foreach ($w in @(@('sw_nonctx', 'B3cC2aSwNonctx'), @('sw_ctx', 'B3cC2aSwCtx'), @('sw_bundle_init', 'B3cC2aSwBundleInit'))) {
            if ($null -ne $p.($w[1])) {
              $nb = [byte[]]$p.($w[1]); $nv = @()
              for ($i = 0; $i + 4 -le $nb.Length; $i += 4) { $nv += ('{0:X8}' -f [BitConverter]::ToUInt32($nb, $i)) }
              Sum ('B3c C2a {0} trace words: {1}' -f $w[0], ($nv -join ' '))
            }
          }
        }
        if ($null -ne $p.B3cC2bResult) {
          $c2bWhy = $b3err[(Key32 $p.B3cC2bResult)]; if (-not $c2bWhy) { $c2bWhy = '?' }
          Sum ('B3c C2b result {0} ({1}); PMC_ENABLE {2}; sw_nonctx bytes/count/writes {3}/{4}/{5}; bad index/address {6}/{7}' -f (Hex $p.B3cC2bResult), $c2bWhy, (Hex $p.B3cC2bPmcEnable), (Hex $p.B3cC2bSwNonctxBytes), (Hex $p.B3cC2bSwNonctxCount), (Hex $p.B3cC2bSwNonctxWrites), (Hex $p.B3cC2bBadIndex), (Hex $p.B3cC2bBadAddress))
          Sum ('B3c C2b rejected GR writes / dropped / ACK timeouts: {0}/{1}/{2}; records: {3}' -f (Hex $p.B3cC2bPrivWriteErrorCount), (Hex $p.B3cC2bPrivWriteErrorDropped), (Hex $p.B3cC2bPrivAckTimeouts), (Words $p.B3cC2bPrivWriteErrors))
          Sum ('B3c C2b GPC MMU rejected writes / dropped / ACK timeouts: {0}/{1}/{2}; records: {3}' -f (Hex $p.B3cC2bGpcMmuPrivWriteErrorCount), (Hex $p.B3cC2bGpcMmuPrivWriteErrorDropped), (Hex $p.B3cC2bGpcMmuPrivAckTimeouts), (Words $p.B3cC2bGpcMmuPrivWriteErrors))
          Sum ('B3c C2b MC_ENABLE before/during/after reset {0}/{1}/{2}; FECS ctxsw reset assert/release {3}/{4}; force clock polls/readback {5}/{6}; auto clock polls/readback {7}/{8}; SCC readback {9}; scrub polls/bits FECS/GPCCS {10}/{11}/{12}; GR idle polls/status {13}/{14}/{15}' -f (Hex $p.B3cC2bMcEnableBeforeReset), (Hex $p.B3cC2bMcEnableDuringReset), (Hex $p.B3cC2bMcEnableAfterReset), (Hex $p.B3cC2bFecsCtxswResetAssert), (Hex $p.B3cC2bFecsCtxswResetRelease), $p.B3cC2bForceClockPolls, (Hex $p.B3cC2bForceClockReadback), $p.B3cC2bAutoClockPolls, (Hex $p.B3cC2bAutoClockReadback), (Hex $p.B3cC2bSccInitReadback), $p.B3cC2bScrubPolls, (Hex $p.B3cC2bFecsScrubBits), (Hex $p.B3cC2bGpccsScrubBits), $p.B3cC2bIdlePolls, (Hex $p.B3cC2bEngineStatus), (Hex $p.B3cC2bGrStatus))
          Sum ('B3c pre-C2b GPC/TPC/fuse/topology/ring snapshot: BADF registers {0}; address=value pairs {1}' -f (Hex $p.B3cC2bPreResetGpcPriBadfCount), (RegPairs $p.B3cC2bPreResetGpcPri))
          Sum ('B3c GPC MMU setup result/count/mismatches {0}/{1}/{2}; writes/readbacks (address:expected>actual) {3}' -f (Hex $p.B3cC2bGpcMmuResult), (Hex $p.B3cC2bGpcMmuWriteCount), (Hex $p.B3cC2bGpcMmuMismatchCount), (RegTriples $p.B3cC2bGpcMmuReadback))
          Sum 'B3c C2b applies only sw_nonctx AV records; allowed BAR0 windows are GR 0x400000-0x40FFFF, GPCS 0x418000-0x41FFFF, and two GPC PRI banks 0x500000-0x50FFFF'
        }
        if ($null -ne $p.B3cC2cResult) {
          $c2cWhy = $b3err[(Key32 $p.B3cC2cResult)]; if (-not $c2cWhy) { $c2cWhy = '?' }
          Sum ('B3c C2c result {0} ({1}); PMC_ENABLE {2}; sw_ctx bytes/count/writes {3}/{4}/{5}; bad index/address {6}/{7}; readback samples {8}; idle polls/status {9}/{10}/{11}' -f (Hex $p.B3cC2cResult), $c2cWhy, (Hex $p.B3cC2cPmcEnable), (Hex $p.B3cC2cSwCtxBytes), (Hex $p.B3cC2cSwCtxCount), (Hex $p.B3cC2cSwCtxWrites), (Hex $p.B3cC2cBadIndex), (Hex $p.B3cC2cBadAddress), (Hex $p.B3cC2cReadbackCount), $p.B3cC2cIdlePolls, (Hex $p.B3cC2cEngineStatus), (Hex $p.B3cC2cGrStatus))
          Sum ('B3c C2c rejected writes / dropped / ACK timeouts: {0}/{1}/{2}; records: {3}' -f (Hex $p.B3cC2cPrivWriteErrorCount), (Hex $p.B3cC2cPrivWriteErrorDropped), (Hex $p.B3cC2cPrivAckTimeouts), (Words $p.B3cC2cPrivWriteErrors))
          Sum 'B3c C2c follows FECS ready, C1 size queries, and C2a validation; nvgpu setup_hw writes sw_ctx addr/value directly and ignores AIV index'
        }
        if ($null -ne $p.B3cC2dResult) {
          $c2dWhy = $b3err[(Key32 $p.B3cC2dResult)]; if (-not $c2dWhy) { $c2dWhy = '?' }
          Sum ('B3c C2d result {0} ({1}); arena PA/bytes {2}/{3}; GPU VA/IOMMU-tagged IOVA {4}/{5}:{6}; outer SMMU mode/PDE before/expected/read/PTE expected/read {7}/{8}/{9}/{10}/{11}/{12}' -f (Hex $p.B3cC2dResult), $c2dWhy, (Hex $p.B3cC2dArenaPa), (Hex $p.B3cC2dArenaBytes), (Hex $p.B3cC2dGpuVa), (Hex $p.B3cC2dArenaIovaHi), (Hex $p.B3cC2dArenaIovaLo), (Hex $p.B3cC2dSmmuMode), (Hex $p.B3cC2dOuterPdeBefore), (Hex $p.B3cC2dOuterPdeExpected), (Hex $p.B3cC2dOuterPdeRead), (Hex $p.B3cC2dOuterPteExpected), (Hex $p.B3cC2dOuterPteRead))
          Sum ('B3c C2d map checks: separate from ACR {0}; outer PTE verified/bad {1}/{2}; GPU PTE verified/bad {3}/{4}; GPU PDE expected/read low {5}/{6}, high {7}/{8}' -f (Hex $p.B3cC2dArenaDistinct), (Hex $p.B3cC2dOuterPteVerified), (Hex $p.B3cC2dOuterPteBadIndex), (Hex $p.B3cC2dGpuPteVerified), (Hex $p.B3cC2dGpuPteBadIndex), (Hex $p.B3cC2dGpuPdeExpectedLo), (Hex $p.B3cC2dGpuPdeReadLo), (Hex $p.B3cC2dGpuPdeExpectedHi), (Hex $p.B3cC2dGpuPdeReadHi))
          Sum ('B3c C2d image/global bytes golden/zcull/PM/attrib/bundle/pagepool/priv {0}/{1}/{2}/{3}/{4}/{5}/{6}; VA golden/context/zcull/PM/attrib {7}/{8}/{9}/{10}/{11}' -f (Hex $p.B3cC2dGoldenBytes), (Hex $p.B3cC2dZcullBytes), (Hex $p.B3cC2dPmBytes), (Hex $p.B3cC2dAttribBytes), (Hex $p.B3cC2dBundleBytes), (Hex $p.B3cC2dPagepoolBytes), (Hex $p.B3cC2dPrivMapBytes), (Hex $p.B3cC2dGoldenVa), (Hex $p.B3cC2dChannelCtxVa), (Hex $p.B3cC2dZcullVa), (Hex $p.B3cC2dPmVa), (Hex $p.B3cC2dAttribVa))
          Sum ('B3c C2d instance PDB/limit/WFI {0}:{1}/{2}:{3}/{4}:{5}; golden commit VA {6}; GPU PTE samples binary; attrib patch target/value {7}/{8}; outer map restored {9}' -f (Hex $p.B3cC2dInstPdbHi), (Hex $p.B3cC2dInstPdbLo), (Hex $p.B3cC2dInstLimitHi), (Hex $p.B3cC2dInstLimitLo), (Hex $p.B3cC2dInstWfiPtrHi), (Hex $p.B3cC2dInstWfiPtrLo), (Hex $p.B3cC2dGoldenCommitVa), (Hex $p.B3cC2dAttribBaseReg), (Hex $p.B3cC2dAttribBaseField), (Hex $p.B3cC2dOuterMapRestored))
          Sum 'B3c C2d prepares bounded global/channel backing, a separate GPU PDB and RAMIN instance block; it does not bind FECS or save the golden image'
          Sum ('B3c C2d failure point / allocation retry: {0}/{1}' -f (Hex $p.B3cC2dFailPoint), (Hex $p.B3cC2dAllocRetry))
          if ($null -ne $p.B3cC2dKeepOnReset) { Sum 'KeepOn was overridden for C2d so GPU reset and arena cleanup could complete' }
        }
    if ($null -ne $p.B3cC2eResult) {
          $c2eWhy = $b3err[(Key32 $p.B3cC2eResult)]; if (-not $c2eWhy) { $c2eWhy = '?' }
          Sum ('B3c C2e result {0} ({1}); bind method/data/mailbox/polls {2}/{3}/{4}/{5}; globals written {6}; bundle entries/last method {7}/{8}:{9}; save method/data/mailbox/polls {10}/{11}/{12}/{13}' -f (Hex $p.B3cC2eResult), $c2eWhy, (Hex $p.B3cC2eBindMethod), (Hex $p.B3cC2eBindData), (Hex $p.B3cC2eBindMailbox), $p.B3cC2eBindPolls, (Hex $p.B3cC2eGlobalWriteCount), (Hex $p.B3cC2eBundleEntries), (Hex $p.B3cC2eBundleLastAddress), (Hex $p.B3cC2eBundleLastValue), (Hex $p.B3cC2eSaveMethod), (Hex $p.B3cC2eSaveData), (Hex $p.B3cC2eSaveMailbox), $p.B3cC2eSavePolls)
          Sum ('B3c C2e bundle baseline (ESR/info/intr/exception/GR status): {0}' -f (Words $p.B3cC2eBundleBaseline))
          Sum ('B3c C2e entry 0 (index/method/data/ESR/info/intr/exception/GR status): {0}' -f (Words $p.B3cC2eBundleEntry0State))
          Sum ('B3c C2e first changed entry found/index/method/data: {0}/{1}/{2}/{3}; state: {4}' -f (Hex $p.B3cC2eBundleFirstChangeFound), (Hex $p.B3cC2eBundleFirstChangeIndex), (Hex $p.B3cC2eBundleFirstChangeMethod), (Hex $p.B3cC2eBundleFirstChangeData), (Words $p.B3cC2eBundleFirstChangeState))
          Sum ('B3c C2e rejected writes / dropped / ACK timeouts: {0}/{1}/{2}; records: {3}' -f (Hex $p.B3cC2ePrivWriteErrorCount), (Hex $p.B3cC2ePrivWriteErrorDropped), (Hex $p.B3cC2ePrivAckTimeouts), (Words $p.B3cC2ePrivWriteErrors))
          Sum ('B3c C2e post-bind sw_ctx bytes/count/writes/readback/idle polls/result {0}/{1}/{2}/{3}/{4}/{5}; bad index/address {6}/{7}' -f (Hex $p.B3cC2ePostBindSwCtxBytes), (Hex $p.B3cC2ePostBindSwCtxCount), (Hex $p.B3cC2ePostBindSwCtxWrites), (Hex $p.B3cC2ePostBindSwCtxReadbackCount), (Hex $p.B3cC2ePostBindSwCtxIdlePolls), (Hex $p.B3cC2ePostBindSwCtxResult), (Hex $p.B3cC2ePostBindSwCtxBadIndex), (Hex $p.B3cC2ePostBindSwCtxBadAddress))
          Sum ('B3c C2e true GR idle after GO_IDLE: polls/ok/GR status/engine/FIFO {0}/{1}/{2}/{3}/{4}; before save: polls/ok/GR status/engine/FIFO {5}/{6}/{7}/{8}/{9}' -f (Hex $p.B3cC2eTrueIdleGoBundlePolls), (Hex $p.B3cC2eTrueIdleGoBundleOk), (Hex $p.B3cC2eTrueIdleGoBundleGrStatus), (Hex $p.B3cC2eTrueIdleGoBundleEngineStatus), (Hex $p.B3cC2eTrueIdleGoBundleFifoStatus), (Hex $p.B3cC2eTrueIdlePreSavePolls), (Hex $p.B3cC2eTrueIdlePreSaveOk), (Hex $p.B3cC2eTrueIdlePreSaveGrStatus), (Hex $p.B3cC2eTrueIdlePreSaveEngineStatus), (Hex $p.B3cC2eTrueIdlePreSaveFifoStatus))          Sum ('B3c C2e Nouveau GR-enabled state at GO_IDLE/save {0}/{1}; read-only MC 0x260 entry/exit {2}/{3}; GR 0x5044B0 entry/exit {4}/{5} (bit27 {6}/{7})' -f (Hex $p.B3cC2eTrueIdleGoBundleMcEnable), (Hex $p.B3cC2eTrueIdlePreSaveMcEnable), (Hex $p.B3cC2eMcUnk260AtEntry), (Hex $p.B3cC2eMcUnk260AtExit), (Hex $p.B3cC2eGr5044B0AtEntry), (Hex $p.B3cC2eGr5044B0AtExit), (Hex $p.B3cC2eGr5044B0Bit27AtEntry), (Hex $p.B3cC2eGr5044B0Bit27AtExit))

          Sum ('B3c C2e Nouveau idle checkpoints (poll/ok): sw_ctx {0}/{1}, setup {2}/{3}, timeout restore {4}/{5}, method-init {6}/{7}; pre-save MC enable {8}' -f (Hex $p.B3cC2eNouveauIdleAfterSwCtxPolls), (Hex $p.B3cC2eNouveauIdleAfterSwCtxOk), (Hex $p.B3cC2eNouveauIdleAfterSetupPolls), (Hex $p.B3cC2eNouveauIdleAfterSetupOk), (Hex $p.B3cC2eNouveauIdleAfterRestorePolls), (Hex $p.B3cC2eNouveauIdleAfterRestoreOk), (Hex $p.B3cC2eNouveauIdleAfterMethodPolls), (Hex $p.B3cC2eNouveauIdleAfterMethodOk), (Hex $p.B3cC2eTrueIdlePreSaveMcEnable))
          Sum ('B3c C2e first busy bundle index/method/engine/GR/FIFO {0}/{1}/{2}/{3}/{4}; FE timeout saved/restored {5}/{6}; MC 0x260 entry/exit {7}/{8}' -f (Hex $p.B3cC2eFirstBusyBundleIndex), (Hex $p.B3cC2eFirstBusyBundleMethod), (Hex $p.B3cC2eFirstBusyEngineStatus), (Hex $p.B3cC2eFirstBusyGrStatus), (Hex $p.B3cC2eFirstBusyFifoStatus), (Hex $p.B3cC2eFeGoIdleTimeoutSaved), (Hex $p.B3cC2eFeGoIdleTimeoutRestoredBeforeBundle), (Hex $p.B3cC2eMcUnk260AtEntry), (Hex $p.B3cC2eMcUnk260AtExit))
          Sum ('B3c C2e Nouveau idle raw GR/engine/FIFO: sw_ctx {0}/{1}/{2}, setup {3}/{4}/{5}, timeout restore {6}/{7}/{8}, method-init {9}/{10}/{11}; FE timeout {12}/{13}; channel header 1C/20/28/2C {14}/{15}/{16}/{17}' -f (Hex $p.B3cC2eNouveauIdleAfterSwCtxGrStatus), (Hex $p.B3cC2eNouveauIdleAfterSwCtxEngineStatus), (Hex $p.B3cC2eNouveauIdleAfterSwCtxFifoStatus), (Hex $p.B3cC2eNouveauIdleAfterSetupGrStatus), (Hex $p.B3cC2eNouveauIdleAfterSetupEngineStatus), (Hex $p.B3cC2eNouveauIdleAfterSetupFifoStatus), (Hex $p.B3cC2eNouveauIdleAfterRestoreGrStatus), (Hex $p.B3cC2eNouveauIdleAfterRestoreEngineStatus), (Hex $p.B3cC2eNouveauIdleAfterRestoreFifoStatus), (Hex $p.B3cC2eNouveauIdleAfterMethodGrStatus), (Hex $p.B3cC2eNouveauIdleAfterMethodEngineStatus), (Hex $p.B3cC2eNouveauIdleAfterMethodFifoStatus), (Hex $p.B3cC2eFeGoIdleTimeoutSaved), (Hex $p.B3cC2eFeGoIdleTimeoutRestoredBeforeBundle), (Hex $p.B3cC2eChannelHeader1C), (Hex $p.B3cC2eChannelHeader20), (Hex $p.B3cC2eChannelHeader28), (Hex $p.B3cC2eChannelHeader2C))
          Sum ('B3c C2e wait stage/index/polls {0}/{1}/{2}; GR/engine/FIFO status {3}/{4}/{5}; FE wait/success {6}/{7}; idle branch/FIFO ctx/ctxsw/GR busy {8}/{9}/{10}/{11}' -f (Hex $p.B3cC2eWaitStage), (Hex $p.B3cC2eWaitBundleIndex), (Hex $p.B3cC2eWaitPolls), (Hex $p.B3cC2eWaitGrStatus), (Hex $p.B3cC2eWaitEngineStatus), (Hex $p.B3cC2eWaitFifoEngineStatus), (Hex $p.B3cC2eWaitIsFe), (Hex $p.B3cC2eWaitSucceeded), (Hex $p.B3cC2eWaitIdleBranch), (Hex $p.B3cC2eWaitFifoCtxStatus), (Hex $p.B3cC2eWaitCtxswActive), (Hex $p.B3cC2eWaitGrBusy))
          Sum ('B3c C2e wait-history records/dropped/capacity {0}/{1}/{2}; each 10-word record is stage,index,polls,GR/engine/FIFO status,ctx,ctxsw,busy,branch flags' -f (Hex $p.B3cC2eWaitHistoryCount), (Hex $p.B3cC2eWaitHistoryDropped), (Hex $p.B3cC2eWaitHistoryCapacity))
          Sum ('B3c C2e pre-save GR status/status1/status2 {0}/{1}/{2}; engine/FIFO {3}/{4}; bits 0/5/7/FE-GI {5}/{6}/{7}/{8}; activity 0/1/2/3-present/4 {9}/{10}/{11}/{12}/{13}' -f (Hex $p.B3cC2ePreSaveGrStatus), (Hex $p.B3cC2ePreSaveGrStatus1), (Hex $p.B3cC2ePreSaveGrStatus2), (Hex $p.B3cC2ePreSaveGrEngineStatus), (Hex $p.B3cC2ePreSaveFifoEngineStatus), (Hex $p.B3cC2ePreSaveGrStatusBit0), (Hex $p.B3cC2ePreSaveGrStatusBit5), (Hex $p.B3cC2ePreSaveGrStatusBit7), (Hex $p.B3cC2ePreSaveGrStatusFeGi), (Hex $p.B3cC2ePreSaveGrActivity0), (Hex $p.B3cC2ePreSaveGrActivity1), (Hex $p.B3cC2ePreSaveGrActivity2), (Hex $p.B3cC2ePreSaveGrActivity3Present), (Hex $p.B3cC2ePreSaveGrActivity4))
          Sum ('B3c C2e exit GR status/status1/status2 {0}/{1}/{2}; activity 0/1/2/3-present/4 {3}/{4}/{5}/{6}/{7}; status bits 0/5/7/FE-GI {8}/{9}/{10}/{11}' -f (Hex $p.B3cC2ePostGrStatus), (Hex $p.B3cC2ePostGrStatus1), (Hex $p.B3cC2ePostGrStatus2), (Hex $p.B3cC2ePostGrActivity0), (Hex $p.B3cC2ePostGrActivity1), (Hex $p.B3cC2ePostGrActivity2), (Hex $p.B3cC2ePostGrActivity3Present), (Hex $p.B3cC2ePostGrActivity4), (Hex $p.B3cC2ePostGrStatusBit0), (Hex $p.B3cC2ePostGrStatusBit5), (Hex $p.B3cC2ePostGrStatusBit7), (Hex $p.B3cC2ePostGrStatusFeGi))
          Sum ('B3c C2e bundle tail idle stage/index/polls {0}/{1}/{2}; FE timeout restored {3}; method-init entries {4}; L2 flush/invalidate polls/status {5}/{6}/{7}/{8}' -f (Hex $p.B3cC2eWaitStage), (Hex $p.B3cC2eWaitBundleIndex), (Hex $p.B3cC2eWaitPolls), (Hex $p.B3cC2eFeGoIdleTimeoutRestored), (Hex $p.B3cC2eMethodInitCount), (Hex $p.B3cC2eL2FlushPolls), (Hex $p.B3cC2eL2FlushStatus), (Hex $p.B3cC2eL2InvalidatePolls), (Hex $p.B3cC2eL2InvalidateStatus))
          Sum ('B3c C2e FE power polls/status {0}/{1}; floorsweep writes/samples {2}/{3}; diagnostics PMC_ENABLE {4}' -f (Hex $p.B3cC2eForceOnPolls), (Hex $p.B3cC2eForceOnStatus), (Hex $p.B3cC2eFsWriteCount), (Hex $p.B3cC2eFsSampleCount), (Hex $p.B3cC2ePmcEnableAtDiagnostics))
          Sum ('B3c C2e BE ROP setup: TOP_NUM_LTCS raw/count {0}/{1}; write count {2}; readback (address,before,expected,actual) {3}' -f (Hex $p.B3cC2eTopNumLtcsRaw), (Hex $p.B3cC2eLtcCount), (Hex $p.B3cC2eRopSetupWriteCount), (Words $p.B3cC2eRopSetupReadback))
          Sum ('B3c C2e setup_hw additions: early PE/zcull writes {0} (addr,before,expected,actual) {1}; Nouveau FBP raw/count {2}/{3}, ROP writes {4}: {5}; FIFO access before/expected/read {6}' -f (Hex $p.B3cC2eEarlyZcullWriteCount), (Words $p.B3cC2eEarlyZcullReadback), (Hex $p.B3cC2eFbpCountRaw), (Hex $p.B3cC2eFbpCount), (Hex $p.B3cC2eNouveauRopWriteCount), (Words $p.B3cC2eNouveauRopReadback), (Words $p.B3cC2eFifoAccess))
          Sum ('B3c C2e LTC active/max raw {0}/{1}; CBC params/slices/cache bytes/tags {2}/{3}/{4}/{5}; LTC FS writes {6}: {7}' -f (Hex $p.B3cC2eLtcActiveCountRaw), (Hex $p.B3cC2eLtcMaxCount), (Hex $p.B3cC2eLtcCbcParam), (Hex $p.B3cC2eLtcSlicesPerLtc), (Hex $p.B3cC2eLtcCacheLineBytes), (Hex $p.B3cC2eLtcComptagsPerCacheline), (Hex $p.B3cC2eLtcFsWriteCount), (Words $p.B3cC2eLtcFsReadback))
          Sum ('B3c C2e FB active-LTC writes {0}: {1}; zero-comptag CBC configured/backing/write/readback {2}/{3}/{4}/{5}' -f (Hex $p.B3cC2eFbFsWriteCount), (Words $p.B3cC2eFbFsReadback), (Hex $p.B3cC2eCbcNoComptag), (Hex $p.B3cC2eCbcBackingBytes), (Hex $p.B3cC2eCbcWriteCount), (Words $p.B3cC2eCbcReadback))
          Sum ('B3c C2e ZBC idle polls/ok/writes/records {0}/{1}/{2}/{3}; PMU ZBC sync skipped {4}' -f (Hex $p.B3cC2eZbcIdlePolls), (Hex $p.B3cC2eZbcIdleOk), (Hex $p.B3cC2eZbcWriteCount), (Words $p.B3cC2eZbcReadback), (Hex $p.B3cC2eZbcPmuSyncSkipped))
          Sum ('B3c C2e read-coalesce before/mask/after {0}/{1}/{2}; LTC/FB/ZBC/CBC configured {3}/{4}/{5}/{6}; ZBC PMU sync skipped {7}; setup skipped flags LTC/CBC, ZBC, ROP {8}/{9}/{10}' -f (Hex $p.B3cC2eRdCoalesceBefore), (Hex $p.B3cC2eRdCoalesceMask), (Hex $p.B3cC2eRdCoalesceAfter), (Hex $p.B3cC2eLtcFsConfigured), (Hex $p.B3cC2eFbFsConfigured), (Hex $p.B3cC2eZbcConfigured), (Hex $p.B3cC2eCbcConfigured), (Hex $p.B3cC2eZbcPmuSyncSkipped), (Hex $p.B3cC2eLtcCbcSetupSkipped), (Hex $p.B3cC2eZbcSetupSkipped), (Hex $p.B3cC2eLtcBasedRopSetupSkipped))
          Sum ('B3c C2e golden VA/save WFI/magic {0}/{1}/{2}; FE ESR/info {3}/{4}; FECS CPUCTL/SCTL/IRQ/host-int {5}/{6}/{7}/{8}; GPCCS CPUCTL/SCTL {9}/{10}; PFIFO intr/fault id {11}/{12}; MC int/error/address {13}/{14}/{15}' -f (Hex $p.B3cC2eGoldenCommitVa), (Hex $p.B3cC2eWfiPtrDuringSave), (Hex $p.B3cC2eGoldenMagicOnExit), (Hex $p.B3cC2eFeHwwEsr), (Hex $p.B3cC2eFeHwwEsrInfo), (Hex $p.B3cC2eFecsCpuctl), (Hex $p.B3cC2eFecsSctl), (Hex $p.B3cC2eFecsIrqstat), (Hex $p.B3cC2eFecsHostIntStatus), (Hex $p.B3cC2eGpccsCpuctl), (Hex $p.B3cC2eGpccsSctl), (Hex $p.B3cC2ePfifoIntr0), (Hex $p.B3cC2eFaultId), (Hex $p.B3cC2eMcInt), (Hex $p.B3cC2eMcErr), (Hex $p.B3cC2eMcErrAdr))
          Sum ('B3c C2e save result/mailbox/polls {0}/{1}/{2}; FECS/GPCCS falcon mailboxes {3}/{4}' -f (Hex $p.B3cC2eResult), (Hex $p.B3cC2eSaveMailbox), $p.B3cC2eSavePolls, (Hex $p.B3cC2eFecsMailbox0), (Hex $p.B3cC2eGpccsMailbox0))
          Sum ('B3c C2e post GR/engine/FE power {0}/{1}/{2}; HWW info {3}; FECS/GPCCS PC counts {4}/{5}' -f (Hex $p.B3cC2ePostGrStatus), (Hex $p.B3cC2ePostGrEngineStatus), (Hex $p.B3cC2ePostFePowerMode), (Hex $p.B3cC2ePostFeHwwEsrInfo), (Hex $p.B3cC2ePostFecsTraceCount), (Hex $p.B3cC2ePostGpccsTraceCount))
          Sum ('B3c C2e pre-bundle ESR/info/raw404004 {0}/{1}/{2}; GR intr/exceptions/status1/status2 {3}/{4}/{5}/{6}/{7}/{8}; FECS host int status/clear/enable {9}/{10}/{11}; setup writes {12}' -f (Hex $p.B3cC2ePreBundleFeHwwEsr), (Hex $p.B3cC2ePreBundleFeHwwEsrInfo), (Hex $p.B3cC2ePreBundleFeInfoRaw404004), (Hex $p.B3cC2ePreBundleGrIntr), (Hex $p.B3cC2ePreBundleGrException), (Hex $p.B3cC2ePreBundleGrException1), (Hex $p.B3cC2ePreBundleGrException2), (Hex $p.B3cC2ePreBundleGrStatus1), (Hex $p.B3cC2ePreBundleGrStatus2), (Hex $p.B3cC2ePreBundleFecsHostIntStatus), (Hex $p.B3cC2ePreBundleFecsHostIntClear), (Hex $p.B3cC2ePreBundleFecsHostIntEnable), (Hex $p.B3cC2eExceptionSetupCount))
          Sum ('B3c C2e FE ESR before setup {0}; GR interrupt/exception enable masks {1}/{2}/{3}/{4}' -f (Hex $p.B3cC2ePreSetupFeEsr), (Hex $p.B3cC2ePostGrIntrEnable), (Hex $p.B3cC2ePostGrExceptionEnable), (Hex $p.B3cC2ePostGrException1Enable), (Hex $p.B3cC2ePostGrException2Enable))
          Sum ('B3c C2e GM20B GR status dumps: registers {0}; captured before/entry0/stop {1}/{2}/{3}' -f (Hex $p.B3cC2eGrStatusDumpRegisterCount), (Hex $p.B3cC2eGrStatusDumpBeforeCaptured), (Hex $p.B3cC2eGrStatusDumpEntry0Captured), (Hex $p.B3cC2eGrStatusDumpStopCaptured))
          $grDumpNames = @('NV_PGRAPH_STATUS','NV_PGRAPH_STATUS1','NV_PGRAPH_STATUS2','NV_PGRAPH_ENGINE_STATUS','NV_PGRAPH_GRFIFO_STATUS','NV_PGRAPH_GRFIFO_CONTROL','NV_PGRAPH_PRI_FECS_HOST_INT_STATUS','NV_PGRAPH_EXCEPTION','NV_PGRAPH_FECS_INTR','NV_PFIFO_ENGINE_STATUS_GR','NV_PGRAPH_ACTIVITY0','NV_PGRAPH_ACTIVITY1','NV_PGRAPH_ACTIVITY2','NV_PGRAPH_ACTIVITY4','NV_PGRAPH_PRI_SKED_ACTIVITY','NV_PGRAPH_PRI_GPC0_GPCCS_GPC_ACTIVITY0','NV_PGRAPH_PRI_GPC0_GPCCS_GPC_ACTIVITY1','NV_PGRAPH_PRI_GPC0_GPCCS_GPC_ACTIVITY2','NV_PGRAPH_PRI_GPC0_GPCCS_GPC_ACTIVITY3','NV_PGRAPH_PRI_GPC0_TPC0_TPCCS_TPC_ACTIVITY0','NV_PGRAPH_PRI_GPC0_TPC1_TPCCS_TPC_ACTIVITY0','NV_PGRAPH_PRI_GPC0_TPCS_TPCCS_TPC_ACTIVITY0','NV_PGRAPH_PRI_GPCS_GPCCS_GPC_ACTIVITY0','NV_PGRAPH_PRI_GPCS_GPCCS_GPC_ACTIVITY1','NV_PGRAPH_PRI_GPCS_GPCCS_GPC_ACTIVITY2','NV_PGRAPH_PRI_GPCS_GPCCS_GPC_ACTIVITY3','NV_PGRAPH_PRI_GPCS_TPC0_TPCCS_TPC_ACTIVITY0','NV_PGRAPH_PRI_GPCS_TPC1_TPCCS_TPC_ACTIVITY0','NV_PGRAPH_PRI_GPCS_TPCS_TPCCS_TPC_ACTIVITY0','NV_PGRAPH_PRI_BE0_BECS_BE_ACTIVITY0','NV_PGRAPH_PRI_BE1_BECS_BE_ACTIVITY0','NV_PGRAPH_PRI_BES_BECS_BE_ACTIVITY0','NV_PGRAPH_PRI_DS_MPIPE_STATUS','NV_PGRAPH_PRI_FE_GO_IDLE_ON_STATUS','NV_PGRAPH_PRI_FE_GO_IDLE_TIMEOUT','NV_PGRAPH_PRI_FE_GO_IDLE_CHECK','NV_PGRAPH_PRI_FE_GO_IDLE_INFO','NV_PGRAPH_PRI_GPC0_TPC0_TEX_M_TEX_SUBUNITS_STATUS','NV_PGRAPH_PRI_CWD_FS','NV_PGRAPH_PRI_FE_TPC_FS','NV_PGRAPH_PRI_CWD_GPC_TPC_ID_0','NV_PGRAPH_PRI_CWD_SM_ID_0','NV_PGRAPH_PRI_FECS_CTXSW_STATUS_FE_0','NV_PGRAPH_PRI_FECS_CTXSW_STATUS_1','NV_PGRAPH_PRI_GPC0_GPCCS_CTXSW_STATUS_GPC_0','NV_PGRAPH_PRI_GPC0_GPCCS_CTXSW_STATUS_1','NV_PGRAPH_PRI_FECS_CTXSW_IDLESTATE','NV_PGRAPH_PRI_GPC0_GPCCS_CTXSW_IDLESTATE','NV_PGRAPH_PRI_FECS_CURRENT_CTX','NV_PGRAPH_PRI_FECS_NEW_CTX','NV_PGRAPH_PRI_BE0_CROP_STATUS1','NV_PGRAPH_PRI_BES_CROP_STATUS1','NV_PGRAPH_PRI_BE0_ZROP_STATUS','NV_PGRAPH_PRI_BE0_ZROP_STATUS2','NV_PGRAPH_PRI_BES_ZROP_STATUS','NV_PGRAPH_PRI_BES_ZROP_STATUS2','NV_PGRAPH_PRI_BE0_BECS_BE_EXCEPTION','NV_PGRAPH_PRI_BE0_BECS_BE_EXCEPTION_EN','NV_PGRAPH_PRI_GPC0_GPCCS_GPC_EXCEPTION','NV_PGRAPH_PRI_GPC0_GPCCS_GPC_EXCEPTION_EN','NV_PGRAPH_PRI_GPC0_TPC0_TPCCS_TPC_EXCEPTION','NV_PGRAPH_PRI_GPC0_TPC0_TPCCS_TPC_EXCEPTION_EN')
          $grDumpSpecs = @(@('before bundle','B3cC2eGrStatusDumpBeforeBundle'), @('after entry 0','B3cC2eGrStatusDumpAfterEntry0'), @('stop point','B3cC2eGrStatusDumpAtStop'))
          $grDumpData = @{}
          foreach ($spec in $grDumpSpecs) {
            $blob = $p.($spec[1])
            if ($null -eq $blob) { continue }
            $bytes = [byte[]]$blob
            if ($bytes.Length -ne ($grDumpNames.Count * 8)) {
              Add-Content -Path $Log -Value ('C2e GR status dump {0}: malformed bytes={1}, expected={2}' -f $spec[0], $bytes.Length, ($grDumpNames.Count * 8)) -Encoding UTF8
              continue
            }
            $vals = @(); Add-Content -Path $Log -Value ('C2e GR status dump {0} (name=value):' -f $spec[0]) -Encoding UTF8
            for ($di = 0; $di -lt $grDumpNames.Count; $di++) {
              $addr = [BitConverter]::ToUInt32($bytes, $di * 8)
              $val = [BitConverter]::ToUInt32($bytes, $di * 8 + 4)
              $vals += [uint32]$val
              Add-Content -Path $Log -Value ('  {0}=0x{1:X8} (0x{2:X8})' -f $grDumpNames[$di], $val, $addr) -Encoding UTF8
            }
            $grDumpData[$spec[1]] = $vals
          }
          if ($grDumpData.ContainsKey('B3cC2eGrStatusDumpBeforeBundle') -and $grDumpData.ContainsKey('B3cC2eGrStatusDumpAfterEntry0')) {
            $delta = @(); $beforeVals = $grDumpData['B3cC2eGrStatusDumpBeforeBundle']; $entryVals = $grDumpData['B3cC2eGrStatusDumpAfterEntry0']
            for ($di = 0; $di -lt $grDumpNames.Count; $di++) {
              if ($beforeVals[$di] -ne $entryVals[$di]) { $delta += ('{0}=0x{1:X8}->0x{2:X8}' -f $grDumpNames[$di], $beforeVals[$di], $entryVals[$di]) }
            }
            if ($delta.Count -eq 0) { Sum 'B3c C2e GR status dump delta before/after entry 0: none' }
            else { Sum ('B3c C2e GR status dump delta before/after entry 0: {0}' -f ($delta -join '; ')) }
          } else { Sum 'B3c C2e GR status dump delta before/after entry 0: unavailable' }
          Sum 'B3c C2e activity registers are raw; referenced GM20B nvgpu headers do not define per-unit 3-bit state names'
          Sum 'C2e bind/save mailbox values follow gr_gk20a_fecs_ctx_bind_channel() and gr_gk20a_fecs_ctx_image_save(); bundle_init records use the GR pipe bundle ports'
          if ($null -ne $p.B3cC2ePreBundleGpcPri) {
            Sum ('B3c immediately before bundle: GPC/TPC/fuse/topology/ring BADF registers {0}; address=value pairs {1}' -f (Hex $p.B3cC2ePreBundleGpcPriBadfCount), (RegPairs $p.B3cC2ePreBundleGpcPri))
          }
        }
    if ($null -ne $p.B3cD1Result) {
          Sum ('B3c D1 PFIFO result {0}; TOP GPC/TPC/FBP/LTC {1}/{2}/{3}/{4}; device-info words {5}: {6}' -f (Hex $p.B3cD1Result), (Hex $p.B3cD1TopNumGpcs), (Hex $p.B3cD1TopTpcPerGpc), (Hex $p.B3cD1TopNumFbps), (Hex $p.B3cD1TopNumLtcs), (Hex $p.B3cD1TopDeviceInfoWords), (Words $p.B3cD1TopDeviceInfo))
          Sum ('B3c D1 MC PBDMA0 before/after {0}/{1}; FB timeout before/expected/after {2}/{3}/{4}; PBDMA timeout {5}/{6}' -f (Hex $p.B3cD1McEnablePbBefore), (Hex $p.B3cD1McEnablePbAfter), (Hex $p.B3cD1FbTimeoutBefore), (Hex $p.B3cD1FbTimeoutExpected), (Hex $p.B3cD1FbTimeoutAfter), (Hex $p.B3cD1PbdmaTimeoutBefore), (Hex $p.B3cD1PbdmaTimeoutAfter))
          Sum ('B3c D1 PFIFO interrupt/status before/after-clear/enable {0}/{1}/{2}/{3}; engine/PBDMA status {4}/{5}; PBDMA local intr/en {6}/{7}' -f (Hex $p.B3cD1FifoIntr0Before), (Hex $p.B3cD1FifoIntr0AfterClear), (Hex $p.B3cD1FifoIntrEn0Before), (Hex $p.B3cD1FifoIntrEn0After), (Hex $p.B3cD1FifoEngineStatus0Before), (Hex $p.B3cD1FifoPbdmaStatus0Before), (Hex $p.B3cD1PbdmaIntr0Before), (Hex $p.B3cD1PbdmaIntrEn0Before))
          Sum ('B3c D1 write readbacks (address,before,expected,actual): {0}; priv-ring rejected/dropped/ACK timeout {1}/{2}/{3}: {4}' -f (Words $p.B3cD1Writes), (Hex $p.B3cD1PrivWriteErrorCount), (Hex $p.B3cD1PrivWriteErrorDropped), (Hex $p.B3cD1PrivAckTimeouts), (Words $p.B3cD1PrivWriteErrors))
          Sum 'D1 enables only PBDMA0 and PFIFO-local interrupts; MC interrupt routing remains unchanged; no USERD or runlist is programmed'
        }
        if ($null -ne $p.B3cD2Result) {
          $d2Why = $b3err[(Key32 $p.B3cD2Result)]; if (-not $d2Why) { $d2Why = '?' }
          Sum ('B3c D2 result {0} ({1}); stage {2}; TOP GR engine/runlist {3}/{4}; channel enable/status after bind {5}/{6}; runlist polls/pending {7}/{8}' -f (Hex $p.B3cD2Result), $d2Why, (Hex $p.B3cD2Stage), (Hex $p.B3cD2GrEngineId), (Hex $p.B3cD2RunlistId), (Hex $p.B3cD2CcsrChannelEnableAfterBind), (Hex $p.B3cD2CcsrChannelStatusAfterBind), (Hex $p.B3cD2RunlistPolls), (Hex $p.B3cD2RunlistPendingAfter))
          Sum ('B3c D2 arena layout (offset,bytes,IOVA low/high): {0}; RAMFC offset/value/readback: {1}; WFI pointer: {2}' -f (Words $p.B3cD2MemoryLayout), (Words $p.B3cD2RamfcFields), (Words $p.B3cD2WfiPointer))
          Sum ('B3c D2 runlist entry: {0}; USERD/ring/pushbuffer samples: {1}; zero checks {2}/{3}/{4}, words {5}/{6}/{7}, bad offset {8}; GPFIFO slots {9}' -f (Words $p.B3cD2RunlistEntry), (Words $p.B3cD2ZeroSamples), (Hex $p.B3cD2UserdZeroOk), (Hex $p.B3cD2GpfifoEmptyOk), (Hex $p.B3cD2PushbufferZeroOk), (Hex $p.B3cD2UserdZeroWords), (Hex $p.B3cD2GpfifoZeroWords), (Hex $p.B3cD2PushbufferZeroWords), (Hex $p.B3cD2MemoryZeroBadOffset), (Hex $p.B3cD2GpfifoEntries))
          Sum ('B3c D2 MMIO writes (address,before,value,readback): {0}; priv-ring rejected/dropped/ACK timeout {1}/{2}/{3}; errors {4}' -f (Words $p.B3cD2Writes), (Hex $p.B3cD2PrivWriteErrorCount), (Hex $p.B3cD2PrivWriteErrorDropped), (Hex $p.B3cD2PrivAckTimeouts), (Words $p.B3cD2PrivWriteErrors))
          Sum ('B3c D2 final CCSR inst/channel enable/status {0}/{1}/{2}/{3}; runlist pending {4}; GR engine/PFIFO intr/PBDMA0 status/intr {5}/{6}/{7}/{8}; preempt polls/last {9}/{10}; empty-list polls/last {11}/{12}; cleanup error {13}' -f (Hex $p.B3cD2CcsrInstFinal), (Hex $p.B3cD2CcsrChannelFinal), (Hex $p.B3cD2CcsrChannelEnableFinal), (Hex $p.B3cD2CcsrChannelStatusFinal), (Hex $p.B3cD2RunlistPendingFinal), (Hex $p.B3cD2FifoEngineStatusFinal), (Hex $p.B3cD2FifoIntr0Final), (Hex $p.B3cD2Pbdma0StatusFinal), (Hex $p.B3cD2Pbdma0IntrFinal), (Hex $p.B3cD2PreemptPolls), (Hex $p.B3cD2PreemptLast), (Hex $p.B3cD2CleanupRunlistPolls), (Hex $p.B3cD2CleanupRunlistLast), (Hex $p.B3cD2CleanupError))
          Sum ('B3c D2 RAMFC acquire/subdevice {0}/{1}; PBDMA map/target before -> after runlist {2}/{3} -> {4}/{5}; FECS current/new context {6}/{7} -> {8}/{9}' -f (Hex $p.B3cD2RamfcAcquireExpected), (Hex $p.B3cD2RamfcSubdeviceExpected), (Hex $p.B3cD2PbdmaMapBeforeRunlist), (Hex $p.B3cD2PbdmaTargetBeforeRunlist), (Hex $p.B3cD2PbdmaMapAfterRunlist), (Hex $p.B3cD2PbdmaTargetAfterRunlist), (Hex $p.B3cD2FecsCurrentCtxBeforeRunlist), (Hex $p.B3cD2FecsNewCtxBeforeRunlist), (Hex $p.B3cD2FecsCurrentCtxAfterRunlist), (Hex $p.B3cD2FecsNewCtxAfterRunlist))
          if ($null -ne $p.B3cD2D4CtxPrepResult) { Sum ('B3c D4 Step 107 prep result {0} ({1}); golden copy/header/patch {2}/{3}/{4}; watchdog data/push/mb0 {5}/{6}/{7}; START_CTXSW data/push/mb1/polls/ok {8}/{9}/{10}/{11}/{12}' -f (Hex $p.B3cD2D4CtxPrepResult), $b3err[(Key32 $p.B3cD2D4CtxPrepResult)], (Hex $p.B3cD4CtxCopyMatch), (Hex $p.B3cD4HeaderMismatch), (Hex $p.B3cD4PatchVa), (Hex $p.B3cD4WatchdogMethodData), (Hex $p.B3cD4WatchdogMethodPush), (Hex $p.B3cD4WatchdogMailboxAfter), (Hex $p.B3cD4StartCtxswMethodData), (Hex $p.B3cD4StartCtxswMethodPush), (Hex $p.B3cD4StartCtxswMailboxAfter), (Hex $p.B3cD4StartCtxswPolls), (Hex $p.B3cD4StartCtxswOk)) }
          $d3Why = $b3err[(Key32 $p.B3cD3Result)]; if (-not $d3Why) { $d3Why = '?' }
          Sum ('B3c D3 result {0} ({1}); BAR1 phys/len/mapped {2}/{3}/{4}; alias VA/PTE idx {5}/{6}; PTE expected/read lo:hi {7}:{8}/{9}:{10}; BAR1 GP_PUT/readback/backing {11}/{12}/{13}; MMU fault before/after {14}/{15}; fifo intr0 after write {16}' -f (Hex $p.B3cD3Result), $d3Why, (Hex $p.B3cD3Bar1Phys), (Hex $p.B3cD3Bar1Len), (Hex $p.B3cD3Bar1Mapped), (Hex $p.B3cD3UserdAliasGpuVa), (Hex $p.B3cD3UserdAliasPteIndex), (Hex $p.B3cD3UserdAliasPteExpectedLo), (Hex $p.B3cD3UserdAliasPteExpectedHi), (Hex $p.B3cD3UserdAliasPteReadLo), (Hex $p.B3cD3UserdAliasPteReadHi), (Hex $p.B3cD3UserdGpPutBefore), (Hex $p.B3cD3Bar1GpPutReadback), (Hex $p.B3cD3BackingGpPutAfterBar1Write), (Hex $p.B3cD3FifoMmuFaultStatusBefore), (Hex $p.B3cD3FifoMmuFaultStatusAfterBar1Write), (Hex $p.B3cD3FifoIntr0AfterBar1Write))
          Sum ('B3c D3 GET polls {0}; USERD/PBDMA GP_GET {1}/{2}; semaphore {3}; PB interrupts before/after/int1 {4}/{5}/{6}; PBDMA status {7}; BAR1 write fault {8}; bit24 enabled {9}; INTEN0 {10}/{11}, stall unchanged {12}/{13}' -f (Hex $p.B3cD3GpGetPolls), (Hex $p.B3cD3UserdGpGet), (Hex $p.B3cD3PbGpGet), (Hex $p.B3cD3SemValue), (Hex $p.B3cD3PbIntr0Before), (Hex $p.B3cD3PbIntr0After), (Hex $p.B3cD3PbIntr1), (Hex $p.B3cD3PbdmaStatus), (Hex $p.B3cD3Bar1WriteFault), (Hex $p.B3cD3Bit24Enabled), (Hex $p.B3cD3PbdmaInten0After), (Hex $p.B3cD3PbdmaInten0Restored), (Hex $p.B3cD3PbdmaStallAfter), (Hex $p.B3cD3PbdmaStallBefore))
          Sum ('B3c D3 PB GET/PUT {0}/{1}; ref count {2}; PB method/data {3}/{4}; FIFO intr0 {5}; command dwords {6}; GPFIFO entry {7}; C2d fail point/retry {8}/{9}' -f (Hex $p.B3cD3PbGet), (Hex $p.B3cD3PbPut), (Hex $p.B3cD3RefCount), (Hex $p.B3cD3PbMethod0), (Hex $p.B3cD3PbData0), (Hex $p.B3cD3FifoIntr0), (Words $p.B3cD3PushbufferWords), (Words $p.B3cD3GpfifoEntry), (Hex $p.B3cC2dFailPoint), (Hex $p.B3cC2dAllocRetry))
          if ($null -ne $p.B3cD4Result) {
            $d4Why = $b3err[(Key32 $p.B3cD4Result)]; if (-not $d4Why) { $d4Why = '?' }
            Sum 'Steps 107-108 D4: copy golden image to channel context, initialize ctxsw header, then Maxwell B 3D + WFI semaphore'
            Sum ('B3c D4 context prep: bytes {0}; copy match {1}; patch VA/bytes/zero {2}/{3}/{4}; L2 polls {5}/{6}; header mismatch {7}; prep result {8}' -f (Hex $p.B3cD4CtxImageBytes), (Hex $p.B3cD4CtxCopyMatch), (Hex $p.B3cD4PatchVa), (Hex $p.B3cD4PatchBytes), (Hex $p.B3cD4PatchZeroOk), (Hex $p.B3cD4L2FlushPolls), (Hex $p.B3cD4L2InvalidatePolls), (Hex $p.B3cD4HeaderMismatch), (Hex $p.B3cD4CtxPrepResult))
            Sum ('B3c D4 result {0} ({1}); golden magic/context valid {2}/{3}; GP_GET polls/value {4}/{5}; GP_PUT {6}; PBDMA GP_GET {7}; PB GET/PUT {8}/{9}; WFI enabled {10}; completion semaphore words {11}; USERD ref {12}' -f (Hex $p.B3cD4Result), $d4Why, (Hex $p.B3cD4GoldenMagic), (Hex $p.B3cD4ContextValid), (Hex $p.B3cD4GpGetPolls), (Hex $p.B3cD4GpGet), (Hex $p.B3cD4GpPut), (Hex $p.B3cD4PbdmaGpGet), (Hex $p.B3cD4PbGet), (Hex $p.B3cD4PbPut), (Hex $p.B3cD4SemWfi), (Words $p.B3cD4ReportSem), (Hex $p.B3cD4RefCount))
            Sum ('B3c D4 PBDMA intr0/intr1/status {0}/{1}/{2}; FIFO intr0/MMU fault {3}/{4}; GR intr/exception/status {5}/{6}/{7}; FE ESR/FECS ctx/mailbox0 {8}/{9}/{10}; engine {11}; PB method/data {12}/{13}; write log {14}' -f (Hex $p.B3cD4PbIntr0), (Hex $p.B3cD4PbIntr1), (Hex $p.B3cD4PbdmaStatus), (Hex $p.B3cD4FifoIntr0), (Hex $p.B3cD4MmuFault), (Hex $p.B3cD4GrIntr), (Hex $p.B3cD4GrException), (Hex $p.B3cD4GrStatus), (Hex $p.B3cD4FeHwwEsr), (Hex $p.B3cD4FecsCurrentCtx), (Hex $p.B3cD4FecsMailbox0), (Hex $p.B3cD4EngineStatus), (Hex $p.B3cD4PbMethod0), (Hex $p.B3cD4PbData0), (Words $p.B3cD4Writes))
            Sum ('B3c D4 FECS CPUCTL/IRQ/MASK {0}/{1}/{2}; ctxsw1/FE0/new/current {3}/{4}/{5}/{6}; arb ADR/CMD/host-int {7}/{8}/{9}; GPCCS CPUCTL/status0/1 {10}/{11}/{12}; FIFO timeout {13}' -f (Hex $p.B3cD4FecsCpuctl), (Hex $p.B3cD4FecsIrqstat), (Hex $p.B3cD4FecsIrqmask), (Hex $p.B3cD4FecsCtxswStatus1), (Hex $p.B3cD4FecsCtxswStatusFe0), (Hex $p.B3cD4FecsNewCtx), (Hex $p.B3cD4FecsCurrentCtx), (Hex $p.B3cD4FecsArbCtxAdr), (Hex $p.B3cD4FecsArbCtxCmd), (Hex $p.B3cD4FecsHostIntStatus), (Hex $p.B3cD4GpccsCpuctl), (Hex $p.B3cD4GpccsGpcCtxswStatus0), (Hex $p.B3cD4GpccsGpcCtxswStatus1), (Hex $p.B3cD4FifoEngCtxswTimeout))
            Sum ('B3c D4 FECS ctxsw mailboxes {0}; FECS trace idx/count/write-error {1}/{2}/{3}; GPCCS trace idx/count/write-error {4}/{5}/{6}; diag register writes {7}; cleanup preempt polls/last {8}/{9}' -f (Words $p.B3cD4FecsMailboxWords), (Hex $p.B3cD4FecsTraceIdx), (Hex $p.B3cD4FecsTraceCount), (Hex $p.B3cD4FecsTraceWriteError), (Hex $p.B3cD4GpccsTraceIdx), (Hex $p.B3cD4GpccsTraceCount), (Hex $p.B3cD4GpccsTraceWriteError), (Words $p.B3cD4DiagWrites), (Hex $p.B3cD2PreemptPolls), (Hex $p.B3cD2PreemptLast))
            Sum ('B3c D4 cleanup FECS CPUCTL/IRQ/MASK/current/new {0}/{1}/{2}/{3}/{4}; GPCCS CPUCTL {5}; cleanup ctxsw mailboxes {6}' -f (Hex $p.B3cD4CleanupFecsCpuctl), (Hex $p.B3cD4CleanupFecsIrqstat), (Hex $p.B3cD4CleanupFecsIrqmask), (Hex $p.B3cD4CleanupFecsCurrentCtx), (Hex $p.B3cD4CleanupFecsNewCtx), (Hex $p.B3cD4CleanupGpccsCpuctl), (Words $p.B3cD4CleanupFecsMailboxWords))
            Sum ('B3c D4 cleanup FECS ctxsw1/FE0/arb ADR/CMD/host-int {0}/{1}/{2}/{3}/{4}; GPCCS ctxsw status0/1 {5}/{6}; FIFO timeout {7}' -f (Hex $p.B3cD4CleanupFecsCtxswStatus1), (Hex $p.B3cD4CleanupFecsCtxswStatusFe0), (Hex $p.B3cD4CleanupFecsArbCtxAdr), (Hex $p.B3cD4CleanupFecsArbCtxCmd), (Hex $p.B3cD4CleanupFecsHostIntStatus), (Hex $p.B3cD4CleanupGpccsGpcCtxswStatus0), (Hex $p.B3cD4CleanupGpccsGpcCtxswStatus1), (Hex $p.B3cD4CleanupFifoEngCtxswTimeout))
          }
        }
        if ($null -ne $p.B3cD5Result) {
          $d5Why = $b3err[(Key32 $p.B3cD5Result)]; if (-not $d5Why) { $d5Why = '?' }
          Sum 'Step 109 D5: Maxwell B 3D class B197 pitch-linear color clear plus WFI semaphore'
          Sum ('B3c D5 result {0} ({1}); RT offset/VA/bytes/pitch {2}/{3}/{4}/{5}; expected pixel {6}; GP_GET polls {7}; USERD/PBDMA GP_GET {8}/{9}; PB GET/PUT {10}/{11}; WFI/semaphore polls/value/words {12}/{13}/{14}/{15}' -f (Hex $p.B3cD5Result), $d5Why, (Hex $p.B3cD5RtOffset), (Hex $p.B3cD5RtGpuVa), (Hex $p.B3cD5RtBytes), (Hex $p.B3cD5Pitch), (Hex $p.B3cD5ExpectedPixel), (Hex $p.B3cD5GpGetPolls), (Hex $p.B3cD5UserdGpGet), (Hex $p.B3cD5PbGpGet), (Hex $p.B3cD5PbGet), (Hex $p.B3cD5PbPut), (Hex $p.B3cD5SemWfi), (Hex $p.B3cD5SemPolls), (Hex $p.B3cD5SemValue), (Words $p.B3cD5SemaphoreWords))
          Sum ('B3c D5 RT GPU PTE index/expected/read lo:hi {0}/{1}:{2}/{3}:{4}' -f (Hex $p.B3cD5RtPteIndex), (Hex $p.B3cD5RtPteExpectedLo), (Hex $p.B3cD5RtPteExpectedHi), (Hex $p.B3cD5RtPteReadLo), (Hex $p.B3cD5RtPteReadHi))
          Sum ('B3c D5 pixels expected/stale/other {0}/{1}/{2}; uniform/value/partial {3}/{4}/{5}; first/middle/last {6}/{7}/{8}; first mismatch offset/value {9}/{10}' -f (Hex $p.B3cD5ExpectedPixels), (Hex $p.B3cD5StalePixels), (Hex $p.B3cD5OtherPixels), (Hex $p.B3cD5Uniform), (Hex $p.B3cD5UniformValue), (Hex $p.B3cD5UniformPartial), (Hex $p.B3cD5FirstPixel), (Hex $p.B3cD5MiddlePixel), (Hex $p.B3cD5LastPixel), (Hex $p.B3cD5FirstMismatchOffset), (Hex $p.B3cD5FirstMismatchValue))
          Sum ('B3c D5 PBDMA intr0/intr1/status {0}/{1}/{2}; FIFO intr/MMU fault {3}/{4}; GR intr/exception/status {5}/{6}/{7}; engine {8}; FE/DS/SCC/PD/MME/SKED ESR {9}/{10}/{11}/{12}/{13}/{14}; checked writes {15}' -f (Hex $p.B3cD5PbIntr0), (Hex $p.B3cD5PbIntr1), (Hex $p.B3cD5PbdmaStatus), (Hex $p.B3cD5FifoIntr0), (Hex $p.B3cD5MmuFault), (Hex $p.B3cD5GrIntr), (Hex $p.B3cD5GrException), (Hex $p.B3cD5GrStatus), (Hex $p.B3cD5EngineStatus), (Hex $p.B3cD5FeHwwEsr), (Hex $p.B3cD5DsHwwEsr), (Hex $p.B3cD5SccHwwEsr), (Hex $p.B3cD5PdHwwEsr), (Hex $p.B3cD5MmeHwwEsr), (Hex $p.B3cD5SkedHwwEsr), (Hex $p.B3cD5WriteCount))
        }
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
        # Claude: the wait history can hold up to 2048 x 10 words. Show only a compact summary on screen;
        # the full table goes to the log file only, so the console stays readable in a photo.
        if ($null -ne $p.B3cC2eWaitHistory) {
          $hb = [byte[]]$p.B3cC2eWaitHistory; $hn = [int]($hb.Length / 40); $hBusy = 0; $hCtxsw = 0; $hRows = @()
          for ($r = 0; $r -lt $hn; $r++) {
            $rw = @(); for ($k = 0; $k -lt 10; $k++) { $rw += [BitConverter]::ToUInt32($hb, $r * 40 + $k * 4) }
            if ($rw[8] -ne 0) { $hBusy++ }; if ($rw[7] -ne 0) { $hCtxsw++ }
            $hRows += (($rw | ForEach-Object { '{0:X8}' -f $_ }) -join ' ')
          }
          Add-Content -Path $Log -Value ('C2e per-wait GR/FIFO snapshots (stage,index,polls,GRstatus,GRengine,FIFOengine,ctx,ctxsw,busy,branch):') -Encoding UTF8
          foreach ($hr in $hRows) { Add-Content -Path $Log -Value ('  ' + $hr) -Encoding UTF8 }
          Sum ('C2e wait history: records {0}, GR busy at idle-declare {1}, ctxsw active {2} (full table in the txt log)' -f $hn, $hBusy, $hCtxsw)
          $hFirst = [Math]::Max(0, $hn - 3)
          for ($r = $hFirst; $r -lt $hn; $r++) { Sum ('C2e wait history last [{0}]: {1}' -f $r, $hRows[$r]) }
        }
        foreach ($w in @(@('C2e post-bind sw_ctx readback (addr/index/expected/actual)', 'B3cC2ePostBindSwCtxReadback'), @('C2e global writes (addr/value)', 'B3cC2eGlobalWrites'), @('C2e timeslice writes (addr/before/mask/after)', 'B3cC2eTimesliceWrites'), @('C2e global commit readback (addr/expected/actual)', 'B3cC2eGlobalReadback'), @('C2e floorsweep readback (addr/expected/actual)', 'B3cC2eFsReadback'), @('C2e exception/interrupt setup writes (addr/value)', 'B3cC2eExceptionSetupWrites'), @('C2e FECS PC trace', 'B3cC2eFecsTracePc'), @('C2e post FECS PC trace', 'B3cC2ePostFecsTracePc'), @('C2e post GPCCS PC trace', 'B3cC2ePostGpccsTracePc'), @('C2e post MMU fault records', 'B3cC2ePostFaultRecords'), @('C2e golden image header samples (offset/value)', 'B3cC2eGoldenSamples'), @('C2e golden buffer first 16 words on exit', 'B3cC2eGoldenRawOnExit'), @('C2e FECS ctxsw mailboxes 0..7 on exit', 'B3cC2eFecsCtxswMailboxes'), @('C2e GPCCS ctxsw mailboxes 0..7 on exit', 'B3cC2eGpccsCtxswMailboxes'), @('C2e MMU fault records', 'B3cC2eFaultRecords'), @('D4 channel context header 0x000..0x0FC', 'B3cD4CtxHdr'), @('D4 FECS PC trace', 'B3cD4FecsPcTrace'), @('D4 GPCCS PC trace', 'B3cD4GpccsPcTrace'), @('D4 diagnostic FECS mailboxes 0..7', 'B3cD4FecsMailboxWords'), @('D4 diagnostic trace-selector writes', 'B3cD4DiagWrites'), @('D4 cleanup FECS PC trace', 'B3cD4CleanupFecsPcTrace'), @('D4 cleanup GPCCS PC trace', 'B3cD4CleanupGpccsPcTrace'), @('D4 cleanup channel context header', 'B3cD4CleanupCtxHdr'), @('D4 cleanup FECS mailboxes 0..7', 'B3cD4CleanupFecsMailboxWords'), @('D4 cleanup trace-selector writes', 'B3cD4CleanupDiagWrites'), @('D5 method/data trace', 'B3cD5Methods'), @('D5 GPFIFO entry', 'B3cD5GpfifoEntry'), @('D5 semaphore words', 'B3cD5SemaphoreWords'), @('D5 priv-ring write samples', 'B3cD5WriteSamples'), @('D5 GR status dump on failure', 'B3cD5GrStatusDump'), @('D5 D4 diagnostic register writes', 'B3cD5DiagWrites'), @('PMU checkpoint after WPR ACK', 'B3cPmuCheckpointAfterWprAck'), @('PMU checkpoint after GR enable', 'B3cPmuCheckpointAfterGrEnable'), @('PMU checkpoint before FECS secure-start mailbox sequence', 'B3cPmuCheckpointBeforeFecsStartMailboxSeq'), @('PMU checkpoint before bootstrap', 'B3cPmuCheckpointBeforeBootstrap'), @('C2b nonctx readback sample (addr/expected/actual)', 'B3cC2bReadback'), @('C2c sw_ctx readback sample (addr/index/expected/actual)', 'B3cC2cReadback'), @('C2d GPU PTE samples (index/expected/actual)', 'B3cC2dGpuPteSamples'), @('C2d backing samples (offset/expected/actual)', 'B3cC2dBackingSamples'), @('C2d outer SMMU PTE samples (index/before/after)', 'B3cC2dOuterPteBeforeAfter'), @('WPR init command', 'B3cFecsWprInitCommand'), @('bootstrap command', 'B3cFecsBootstrapCommand'), @('PMU PC trace after post', 'B3cFecsPmuTraceAfterPost'), @('PMU PC trace after wait', 'B3cFecsPmuTraceAfterWait'), @('HPQ tail sample polls', 'B3cFecsHpqSamplePolls'), @('HPQ tail samples', 'B3cFecsHpqTailSamples'), @('PMU IRQSTAT samples', 'B3cFecsPmuIrqSamples'), @('FECS PC trace', 'B3cFecsTracePc'), @('FECS ctxsw mailboxes 0..7', 'B3cFecsCtxswMailboxWords'), @('GPCCS PC trace', 'B3cGpccsTracePc'), @('GPCCS ctxsw mailboxes 0..7', 'B3cGpccsCtxswMailboxWords'), @('C1 golden query trace', 'B3cC1GoldenQueryWords'), @('C1 zcull query trace', 'B3cC1ZcullQueryWords'), @('C1 PM query trace', 'B3cC1PmQueryWords'), @('FECS MMU fault records', 'B3cFecsFaultRecs'), @('FECS WPR BLD words', 'B3cFecsBldWords'))) {
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
                       @('gr\gpccs_inst.bin', 'FwGpccsInst', 0), @('gr\gpccs_data.bin', 'FwGpccsData', 0),
                       @('gr\sw_ctx.bin', 'FwGrSwCtx', 0), @('gr\sw_nonctx.bin', 'FwGrSwNonctx', 0), @('gr\sw_bundle_init.bin', 'FwGrSwBundleInit', 0))
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
      if ($fwDir) {
        Remove-ItemProperty -Path $Key -Name FwGrSwCtx, FwGrSwNonctx, FwGrSwBundleInit -ErrorAction SilentlyContinue
      }
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
    Remove-ItemProperty -Path $Key -Name FwAcrBl, FwAcrUcodeLoad, FwPmuDesc, FwPmuImage, FwPmuSig, FwFecsBl, FwFecsInst, FwFecsData, FwFecsSig, FwGpccsInst, FwGpccsData, FwGrSwCtx, FwGrSwNonctx, FwGrSwBundleInit -ErrorAction SilentlyContinue
    Show-Device
  }
}

Log ''
Log '=== SUMMARY'
foreach ($s in $Summary) { Write-Host $s }
