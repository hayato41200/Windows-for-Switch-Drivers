<#
.SYNOPSIS
  GM20B resident test kit (Administrator).
  -Mode Install / Remove : install or remove the driver package.
  -Mode Status : show Windows device status and driver summaries.
  -Mode Resident  : PowerTest = 6; execute through D5 and leave the channel and
                    GPU resident. ResidentStatus/ResidentStop use IOCTLs, never
                    a PnP restart; stop resets the GPU and frees its mappings.
  -Mode ResidentStatus : query safe resident registers through a read-only IOCTL.
  -Mode ResidentStop : stop the channel and power down through a bounded IOCTL.
  -Mode Submit : submit repeated 3D clears on the resident channel (E2); no power-up.
  -Mode Irq    : run 64 interrupt-driven clears on the resident channel (E3);
                 requires UEFI R447H or later and does not power up the GPU.
  -Mode Copy   : run 16 CE2 buffer copies on the resident channel.
  -Mode Blit   : verify pitched rectangles and benchmark GPU vs CPU.
  -Mode InstallDisplay / F3On / F3Off / F3Status : optional display test.
  -Mode F4On / F4Off : choose display double buffering after restart.
  -Mode Cycle  : five Resident / 8-clear / Stop cycles using one DMA pool.
  -Mode VerboseOn / VerboseOff : change driver logging at the next device start.

  Log: C:\SwitchWOA-Gpu.txt
  This file must stay ASCII: Windows PowerShell 5.1 reads it as the ANSI code page.
#>
param(
  [Parameter(Mandatory = $true)]
  [ValidateSet('Install', 'Status', 'Resident', 'ResidentStatus', 'ResidentStop',
               'Submit', 'Irq', 'Copy', 'Blit', 'Cycle', 'InstallDisplay',
               'F3On', 'F3Off', 'F3Status', 'F4On', 'F4Off', 'Remove', 'VerboseOn', 'VerboseOff')]
  [string]$Mode,
  [switch]$Detail
)

$ErrorActionPreference = 'Continue'
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Gpu.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters'
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
function Sum([string]$s) { Log $s }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
Log ''
Log "=== Gpu $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Hex($v) { if ($null -eq $v) { 'n/a' } else { '0x' + (Key32 $v) } }
function Key32($v) { if ($null -eq $v) { return '' }; $n = [long]$v; if ($n -lt 0) { $n += 4294967296 }; '{0:X8}' -f $n }
function Import-GpuFirmware {
  if (-not (Test-Path $Key)) { New-Item -Path $Key | Out-Null }  # 0.12.6: never -Force (it wipes an existing key)
  # The firmware is the user's own linux-firmware copy; the kit never ships it.
  # Where to look, in order: inside the kit, then fixed places on C:.
  # If none has it, reuse the firmware already in the registry.
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
      return $false
    }
    Sum 'firmware folder not found: using the existing registry copy'
    if ($Detail) {
      foreach ($f in $fwList) {
        $bytes = [byte[]]$have.($f[1])
        $sha = [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($bytes)).Replace('-', '')
        Sum ('firmware {0} (registry): {1} bytes  SHA256 {2}' -f $f[0], $bytes.Length, $sha)
      }
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
    if ($Detail) {
      $sha = (Get-FileHash $fp -Algorithm SHA256).Hash
      Sum ('firmware {0}: {1} bytes  SHA256 {2}' -f $f[0], $bytes.Length, $sha)
    }
    if ($f[2] -gt 0 -and $bytes.Length -ne $f[2]) {
      Sum ('firmware {0}: {1} bytes (expected {2})' -f $f[0], $bytes.Length, $f[2])
    }
    Set-ItemProperty -Path $Key -Name $f[1] -Value $bytes -Type Binary
  }
  return $fwOk
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

function Initialize-ResidentControl {
  $src = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class SwGpuResidentControl {
  [StructLayout(LayoutKind.Sequential)] public struct Status {
    public UInt32 Version, Resident, Boot0, PtimerLoBefore, PtimerHi, PtimerLoAfter, PtimerDelta;
    public UInt32 PmcEnable, PrivRingStatus0, PrivRingStatus1, FecsMailbox0, FecsCpuctl;
    public UInt32 FifoEngineStatus, CcsrChannel, PbdmaStatus, PbdmaIntr0, GrIntr, GrStatus, MmuFaultStatus, ShutdownResult;
  }
  [StructLayout(LayoutKind.Sequential)] struct IfData { public Int32 cbSize; public Guid guid; public Int32 flags; public IntPtr reserved; }
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr w, UInt32 f);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, UInt32 i, ref IfData data);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref IfData data, IntPtr detail, UInt32 size, out UInt32 required, IntPtr devinfo);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFile(string n, UInt32 a, UInt32 share, IntPtr sec, UInt32 disp, UInt32 flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle h, UInt32 code, IntPtr input, UInt32 inSize, out Status output, UInt32 outSize, out UInt32 returned, IntPtr overlapped);
  [StructLayout(LayoutKind.Sequential)] public struct CycleResult {
    public UInt32 Version, StartResult, SubmitResult, StopResult, Rounds, MaxPolls;
    public UInt32 E4PoolResult, E4PoolBytes, E4PoolUsedPeak, E4PoolFallbacks, E4PoolAllocs;
    public UInt32 E4PoolFrees, E4PoolFailures, E4PoolUsed;
  }
  [DllImport("kernel32.dll", EntryPoint="DeviceIoControl", SetLastError=true)]
  static extern bool DeviceIoControlCycle(SafeFileHandle h, UInt32 code, IntPtr input, UInt32 inSize, out CycleResult output, UInt32 outSize, out UInt32 returned, IntPtr overlapped);
  public static CycleResult RunCycle() {
    return WithDevice<CycleResult>(delegate(SafeFileHandle h) {
      CycleResult r; UInt32 got;
      UInt32 size = (UInt32)Marshal.SizeOf(typeof(CycleResult));
      if (!DeviceIoControlCycle(h, 0x22E010u, IntPtr.Zero, 0, out r, size, out got, IntPtr.Zero))
        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
      if (got != size || ((r.Version & 0xFFFFFF00u) != 0xC00u && (r.Version & 0xFFFFFF00u) != 0xD00u))
        throw new InvalidOperationException("Unexpected SwitchGpu Cycle response");
      return r;
    });
  }
  [StructLayout(LayoutKind.Sequential)] public struct CopyResult {
    public UInt32 Version, Result, Rounds, BytesCopied, FailRound, LastSize;
    public UInt32 MismatchOffset, Expected, Actual, MaxWaitUs, MaxPolls, IrqWakes, IrqTimeouts;
    public UInt32 Pending, CeIntr, CeLaunchError, FifoIntr, FaultId, PbdmaIntr, GrIntr;
  }
  [DllImport("kernel32.dll", EntryPoint="DeviceIoControl", SetLastError=true)]
  static extern bool DeviceIoControlCopy(SafeFileHandle h, UInt32 code, IntPtr input, UInt32 inSize, out CopyResult output, UInt32 outSize, out UInt32 returned, IntPtr overlapped);
  public static CopyResult RunCopy() {
    return WithDevice<CopyResult>(delegate(SafeFileHandle h) {
      CopyResult r; UInt32 got;
      UInt32 size = (UInt32)Marshal.SizeOf(typeof(CopyResult));
      if (!DeviceIoControlCopy(h, 0x22E014u, IntPtr.Zero, 0, out r, size, out got, IntPtr.Zero))
        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
      if (got != size || ((r.Version & 0xFFFFFF00u) != 0xC00u && (r.Version & 0xFFFFFF00u) != 0xD00u))
        throw new InvalidOperationException("Unexpected SwitchGpu Copy response");
      return r;
    });
  }
  [StructLayout(LayoutKind.Sequential)] public struct ClockStep {
    public UInt32 TargetKhz, M, N, Pl, CvbMv, LockPolls, RampPolls, MeasuredKhz;
    public UInt32 Result, Verified, CachedUs, WcUs, FailOffset, Stage;
  }
  [StructLayout(LayoutKind.Sequential)] public struct BlitResult {
    public UInt32 Version, Result, Verified, GpuRounds, CpuRounds, GpuMBps, CpuMBps;
    public UInt32 MaxWaitUs, MaxPolls, IrqWakes, MapPolls, UnmapPolls, AbortPolls;
    public UInt32 GpuUs, CpuUs, FailOffset;
    public UInt32 GpuBatchUs, GpuLinesUs, GpuWcUs, CpuWcUs, BenchStage;
    public UInt32 PllCfg, PllCoeff, SelVco, Gpc2Out, BypassCtrl, Slowdown, RampStatus;
    public UInt32 PllCfg2, Dvfs0, Dvfs1, CntrCfg, CntrValue;
    public UInt32 OscCtrl, CarClkX, CarRstX, CarPllpOutc, RefKhz, Gpc2Khz, GpcKhz;
    public UInt32 Bypass, VoutRaw, VoutMv, MeasuredKhz, CntrSamples, IdleSlowdown;
    public UInt32 ClockSteps, RestoreResult;
    [MarshalAs(UnmanagedType.ByValArray, SizeConst=5)] public ClockStep[] Clock;
  }
  [DllImport("kernel32.dll", EntryPoint="DeviceIoControl", SetLastError=true)]
  static extern bool DeviceIoControlBlit(SafeFileHandle h, UInt32 code, IntPtr input, UInt32 inSize, out BlitResult output, UInt32 outSize, out UInt32 returned, IntPtr overlapped);
  public static BlitResult RunBlit() {
    return WithDevice<BlitResult>(delegate(SafeFileHandle h) {
      BlitResult r; UInt32 got;
      UInt32 size = (UInt32)Marshal.SizeOf(typeof(BlitResult));
      if (!DeviceIoControlBlit(h, 0x22E018u, IntPtr.Zero, 0, out r, size, out got, IntPtr.Zero))
        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
      if (got != size || ((r.Version & 0xFFFFFF00u) != 0xC00u && (r.Version & 0xFFFFFF00u) != 0xD00u))
        throw new InvalidOperationException("Unexpected SwitchGpu Blit response");
      return r;
    });
  }
  public static Status Run(bool stop) {
    return WithDevice<Status>(delegate(SafeFileHandle h) {
      Status s; UInt32 got;
      UInt32 code = stop ? 0x22E004u : 0x22E000u;
      if (!DeviceIoControl(h, code, IntPtr.Zero, 0, out s, (UInt32)Marshal.SizeOf(typeof(Status)), out got, IntPtr.Zero))
        throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
      return s;
    });
  }
  static T WithDevice<T>(Func<SafeFileHandle, T> action) {
    Guid g = new Guid("2cc9cb4d-6412-4f31-a781-5d781e094b63");
    IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
    if (set == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    try {
      for (UInt32 i=0; i<32; ++i) {
        IfData d = new IfData(); d.cbSize = Marshal.SizeOf(typeof(IfData));
        if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d)) break;
        UInt32 need; SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, out need, IntPtr.Zero);
        if (need < 8) continue;
        IntPtr b = Marshal.AllocHGlobal((int)need);
        try {
          Marshal.WriteInt32(b, IntPtr.Size == 8 ? 8 : 6);
          if (!SetupDiGetDeviceInterfaceDetail(set, ref d, b, need, out need, IntPtr.Zero)) continue;
          string path = Marshal.PtrToStringUni(IntPtr.Add(b, 4));
          using (SafeFileHandle h = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) continue;
            return action(h);
          }
        } finally { Marshal.FreeHGlobal(b); }
      }
      throw new InvalidOperationException("SwitchGpu resident interface was not found");
    } finally { SetupDiDestroyDeviceInfoList(set); }
  }
}
'@
  if (-not ('SwGpuResidentControl' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp }
}
function Invoke-ResidentControl([bool]$Stop) {
  Initialize-ResidentControl
  $s = [SwGpuResidentControl]::Run($Stop)
  $channelStatus = ([uint32]$s.CcsrChannel -shr 24) -band 0xF
  Sum ('resident {0}; PTIMER {1}:{2} -> {3} delta {4}; FECS mailbox0 {5}; channel raw/status {6}/{7}; PBDMA status/intr {8}/{9}; GR intr/status {10}/{11}; MMU fault {12}; shutdown {13}' -f `
    $s.Resident, (Hex $s.PtimerHi), (Hex $s.PtimerLoBefore), (Hex $s.PtimerLoAfter), (Hex $s.PtimerDelta), (Hex $s.FecsMailbox0), (Hex $s.CcsrChannel), (Hex $channelStatus), (Hex $s.PbdmaStatus), (Hex $s.PbdmaIntr0), (Hex $s.GrIntr), (Hex $s.GrStatus), (Hex $s.MmuFaultStatus), (Hex $s.ShutdownResult))
}

function Invoke-ResidentSubmit {
  $src = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class SwGpuSubmitControl {
  [StructLayout(LayoutKind.Sequential)] public struct Result {
    public UInt32 Version, ResultCode, Rounds, RingEntries, Wraps, FailRound, MaxPolls, TotalUs;
    public UInt32 FirstColor, LastColor, FirstSemaphore, LastSemaphore;
    public UInt32 GpGet, GpPut, Semaphore, Channel, PbdmaStatus, PbdmaIntr0, FifoIntr0;
    public UInt32 GrIntr, GrException, GrStatus, MmuFault, ClassError, TrappedAddr;
    public UInt32 TrappedDataLo, TrappedDataHi, PixelFirst, PixelMiddle, PixelLast;
  }
  [StructLayout(LayoutKind.Sequential)] struct IfData { public Int32 cbSize; public Guid guid; public Int32 flags; public IntPtr reserved; }
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr w, UInt32 f);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, UInt32 i, ref IfData data);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref IfData data, IntPtr detail, UInt32 size, out UInt32 required, IntPtr devinfo);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFile(string n, UInt32 a, UInt32 share, IntPtr sec, UInt32 disp, UInt32 flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle h, UInt32 code, IntPtr input, UInt32 inSize, out Result output, UInt32 outSize, out UInt32 returned, IntPtr overlapped);
  public static Result Run() {
    Guid g = new Guid("2cc9cb4d-6412-4f31-a781-5d781e094b63");
    IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
    if (set == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    try {
      for (UInt32 i=0; i<32; ++i) {
        IfData d = new IfData(); d.cbSize = Marshal.SizeOf(typeof(IfData));
        if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d)) break;
        UInt32 need; SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, out need, IntPtr.Zero);
        if (need < 8) continue;
        IntPtr b = Marshal.AllocHGlobal((int)need);
        try {
          Marshal.WriteInt32(b, IntPtr.Size == 8 ? 8 : 6);
          if (!SetupDiGetDeviceInterfaceDetail(set, ref d, b, need, out need, IntPtr.Zero)) continue;
          string path = Marshal.PtrToStringUni(IntPtr.Add(b, 4));
          using (SafeFileHandle h = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) continue;
            Result r; UInt32 got;
            if (!DeviceIoControl(h, 0x22E008u, IntPtr.Zero, 0, out r, (UInt32)Marshal.SizeOf(typeof(Result)), out got, IntPtr.Zero))
              throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return r;
          }
        } finally { Marshal.FreeHGlobal(b); }
      }
      throw new InvalidOperationException("SwitchGpu resident interface was not found");
    } finally { SetupDiDestroyDeviceInfoList(set); }
  }
}
'@
  if (-not ('SwGpuSubmitControl' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp }
  $r = [SwGpuSubmitControl]::Run()
  Sum ('E2 result {0}; rounds {1}; ring entries {2}; wraps {3}; fail round {4}; max polls {5}; total us {6}; colors {7}->{8}; semaphores {9}->{10}; GET/PUT {11}/{12}; pixels {13}/{14}/{15}' -f `
    (Hex $r.ResultCode), $r.Rounds, $r.RingEntries, $r.Wraps, $r.FailRound, $r.MaxPolls, $r.TotalUs, (Hex $r.FirstColor), (Hex $r.LastColor), (Hex $r.FirstSemaphore), (Hex $r.LastSemaphore), (Hex $r.GpGet), (Hex $r.GpPut), (Hex $r.PixelFirst), (Hex $r.PixelMiddle), (Hex $r.PixelLast))
  if ($r.ResultCode -ne 0) {
    Sum ('E2 failure state: semaphore {0}; channel {1}; PBDMA {2}/{3}; FIFO intr {4}; GR {5}/{6}/{7}; MMU {8}; class/trap {9}/{10}/{11}/{12}' -f `
      (Hex $r.Semaphore), (Hex $r.Channel), (Hex $r.PbdmaStatus), (Hex $r.PbdmaIntr0), (Hex $r.FifoIntr0), (Hex $r.GrIntr), (Hex $r.GrException), (Hex $r.GrStatus), (Hex $r.MmuFault), (Hex $r.ClassError), (Hex $r.TrappedAddr), (Hex $r.TrappedDataLo), (Hex $r.TrappedDataHi))
  }
}

function Invoke-ResidentIrq {
  $src = @'
using System;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
public static class SwGpuIrqControl {
  [StructLayout(LayoutKind.Sequential)] public struct Result {
    public UInt32 Version, ResultCode, Rounds, Waits, NonstallCount, StallCount, StallUnits;
    public UInt32 MaxWaitUs, TotalUs, Timeouts, HaveInterrupts;
    public UInt32 FirstMcIntr0, FirstMcIntr1, LastMcIntr0, LastMcIntr1;
    public UInt32 GpGet, GpPut, Semaphore, PixelFirst, PixelMiddle, PixelLast;
    public UInt32 Channel, PbdmaStatus, PbdmaIntr0, FifoIntr0, GrIntr, GrStatus, MmuFault;
  }
  [StructLayout(LayoutKind.Sequential)] struct IfData { public Int32 cbSize; public Guid guid; public Int32 flags; public IntPtr reserved; }
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr SetupDiGetClassDevs(ref Guid g, IntPtr e, IntPtr w, UInt32 f);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiEnumDeviceInterfaces(IntPtr s, IntPtr d, ref Guid g, UInt32 i, ref IfData data);
  [DllImport("setupapi.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool SetupDiGetDeviceInterfaceDetail(IntPtr s, ref IfData data, IntPtr detail, UInt32 size, out UInt32 required, IntPtr devinfo);
  [DllImport("setupapi.dll", SetLastError=true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr s);
  [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern SafeFileHandle CreateFile(string n, UInt32 a, UInt32 share, IntPtr sec, UInt32 disp, UInt32 flags, IntPtr template);
  [DllImport("kernel32.dll", SetLastError=true)] static extern bool DeviceIoControl(SafeFileHandle h, UInt32 code, IntPtr input, UInt32 inSize, out Result output, UInt32 outSize, out UInt32 returned, IntPtr overlapped);
  public static Result Run() {
    Guid g = new Guid("2cc9cb4d-6412-4f31-a781-5d781e094b63");
    IntPtr set = SetupDiGetClassDevs(ref g, IntPtr.Zero, IntPtr.Zero, 0x12);
    if (set == new IntPtr(-1)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    try {
      for (UInt32 i=0; i<32; ++i) {
        IfData d = new IfData(); d.cbSize = Marshal.SizeOf(typeof(IfData));
        if (!SetupDiEnumDeviceInterfaces(set, IntPtr.Zero, ref g, i, ref d)) break;
        UInt32 need; SetupDiGetDeviceInterfaceDetail(set, ref d, IntPtr.Zero, 0, out need, IntPtr.Zero);
        if (need < 8) continue;
        IntPtr b = Marshal.AllocHGlobal((int)need);
        try {
          Marshal.WriteInt32(b, IntPtr.Size == 8 ? 8 : 6);
          if (!SetupDiGetDeviceInterfaceDetail(set, ref d, b, need, out need, IntPtr.Zero)) continue;
          string path = Marshal.PtrToStringUni(IntPtr.Add(b, 4));
          using (SafeFileHandle h = CreateFile(path, 0xC0000000, 3, IntPtr.Zero, 3, 0, IntPtr.Zero)) {
            if (h.IsInvalid) continue;
            Result r; UInt32 got;
            if (!DeviceIoControl(h, 0x22E00Cu, IntPtr.Zero, 0, out r, (UInt32)Marshal.SizeOf(typeof(Result)), out got, IntPtr.Zero))
              throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            return r;
          }
        } finally { Marshal.FreeHGlobal(b); }
      }
      throw new InvalidOperationException("SwitchGpu resident interface was not found");
    } finally { SetupDiDestroyDeviceInfoList(set); }
  }
}
'@
  if (-not ('SwGpuIrqControl' -as [type])) { Add-Type -TypeDefinition $src -Language CSharp }
  $r = [SwGpuIrqControl]::Run()
  $irqText = 'interrupt resources present'; if ($r.HaveInterrupts -eq 0) { $irqText = 'no interrupt objects (R447H+ resources missing, or WdfInterruptCreate failed: see E3InterruptCreateStatus)' }
  Sum ('E3 result {0}; {1}; rounds {2}/64; waits {3}; nonstall/stall {4}/{5}; stall units {6}; max wait us {7}; total us {8}; timeouts {9}; MC first {10}/{11}, last {12}/{13}; semaphore {14}; pixels {15}/{16}/{17}' -f `
    (Hex $r.ResultCode), $irqText, $r.Rounds, $r.Waits, $r.NonstallCount, $r.StallCount, (Hex $r.StallUnits), $r.MaxWaitUs, $r.TotalUs, $r.Timeouts, (Hex $r.FirstMcIntr0), (Hex $r.FirstMcIntr1), (Hex $r.LastMcIntr0), (Hex $r.LastMcIntr1), (Hex $r.Semaphore), (Hex $r.PixelFirst), (Hex $r.PixelMiddle), (Hex $r.PixelLast))
}

function Show-F1Copy($c) {
  Sum ('F1 copy result {0}; rounds {1}/16; bytes {2}; max wait us {3}; max polls {4}; nonstall wakes/timeouts {5}/{6}' -f (Hex $c.Result), $c.Rounds, $c.BytesCopied, $c.MaxWaitUs, $c.MaxPolls, $c.IrqWakes, $c.IrqTimeouts)
  if ($c.Result -ne 0) {
    Sum ('F1 failure round {0}; size {1}; first mismatch offset/expected/actual {2}/{3}/{4}; pending {5}' -f $c.FailRound, $c.LastSize, (Hex $c.MismatchOffset), (Hex $c.Expected), (Hex $c.Actual), $c.Pending)
    Sum ('F1 CE2 intr/launch error {0}/{1}; FIFO intr/fault id {2}/{3}; PBDMA intr {4}; GR intr {5}' -f (Hex $c.CeIntr), (Hex $c.CeLaunchError), (Hex $c.FifoIntr), (Hex $c.FaultId), (Hex $c.PbdmaIntr), (Hex $c.GrIntr))
  }
}

function Show-E4Pool($p) {
  if ($null -ne $p.E4PoolResult) {
    Sum ('E4 pool result {0}; reserved/peak/in-use bytes {1}/{2}/{3}; checkouts/returns {4}/{5}; fallbacks {6}; failures {7}' -f (Hex $p.E4PoolResult), $p.E4PoolBytes, $p.E4PoolUsedPeak, $p.E4PoolUsed, $p.E4PoolAllocs, $p.E4PoolFrees, $p.E4PoolFallbacks, $p.E4PoolFailures)
  }
}

function Invoke-ResidentCycle {
  Initialize-ResidentControl
  $passed = 0
  for ($i = 1; $i -le 5; $i++) {
    try { $r = [SwGpuResidentControl]::RunCycle() }
    catch { Sum ('E4 cycle {0}/5 IOCTL failed: {1}' -f $i, $_.Exception.Message); break }
    Sum ('E4 cycle {0}/5 start/submit/stop {1}/{2}/{3}; rounds {4}/8; max polls {5}' -f $i, (Hex $r.StartResult), (Hex $r.SubmitResult), (Hex $r.StopResult), $r.Rounds, $r.MaxPolls)
    Show-E4Pool $r
    if ($r.StartResult -ne 0 -or $r.SubmitResult -ne 0 -or $r.StopResult -ne 0 -or $r.Rounds -ne 8 -or $r.E4PoolUsed -ne 0) { break }
    $passed++
  }
  Sum ('E4 cycle completed {0}/5' -f $passed)
}

function Show-Trace([string]$AlreadyPrinted) {
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Sum 'switchgpu trace: (no Parameters key: driver never started)'; return }
  Sum ('switchgpu {0}.{1}.{2}; starts {3}; I2C VDD/PMIC {4}/{5}; resources mem/I2C/IRQ {6}/{7}/{8}' -f ($p.Version -shr 16), (($p.Version -shr 8) -band 0xFF), ($p.Version -band 0xFF), $p.StartCount, (Hex $p.OpenVddStatus), (Hex $p.OpenPmicStatus), $p.ResMem, $p.ResI2c, $p.E3InterruptResourceCount)
  if ($null -ne $p.Step) { Sum ('Step {0}; result {1}; startup us {2}' -f $p.Step, (Hex $p.Result), $p.TestUs) }
  if ($null -ne $p.E5PtimerNsPer1ms) {
    # Tegra PTIMER runs from the 19.2 MHz oscillator but counts 32 ns per tick
    # (614400 per wall ms). nouveau gk20a also leaves it unprogrammed; the
    # driver uses it only for this display, never for timeouts.
    $rate = [double]$p.E5PtimerNsPer1ms / 1000000.0
    Sum ('E5 PTIMER delta over 1 ms: {0} ns (raw) = {1:N3} x wall clock; expected 614400 (19.2 MHz source in 32 ns steps); display only, not used for timeouts' -f $p.E5PtimerNsPer1ms, $rate)
  }
  if ($null -ne $p.E1Result -or $null -ne $p.B3eResidentActive) {
    $e1Why = ''; if ((Key32 $p.E1Result) -eq 'E00000A5') { $e1Why = 'resident state was not published' }
    Sum ('E1 resident result {0} {1}; active {2}; idle reference {3}; shutdown {4}; power-down {5}; PTIMER delta {6}; FECS mailbox0 {7}; channel {8}' -f (Hex $p.E1Result), $e1Why, (Hex $p.B3eResidentActive), (Hex $p.B3eIdleRefStatus), (Hex $p.B3eShutdownResult), (Hex $p.B3ePowerDownResult), (Hex $p.B3eStatusPtimerDelta), (Hex $p.B3eStatusFecsMailbox0), (Hex $p.B3eStatusCcsrChannel))
    Sum ('E1 status FIFO engine {0}; PBDMA status/intr {1}/{2}; GR intr/status {3}/{4}; MMU fault {5}; shutdown polls preempt/runlist {6}/{7}' -f (Hex $p.B3eStatusFifoEngineStatus), (Hex $p.B3eStatusPbdmaStatus), (Hex $p.B3eStatusPbdmaIntr0), (Hex $p.B3eStatusGrIntr), (Hex $p.B3eStatusGrStatus), (Hex $p.B3eStatusMmuFaultStatus), (Hex $p.B3eChannelCleanupPreemptPolls), (Hex $p.B3eChannelCleanupRunlistPolls))
  }
  if ($null -ne $p.E2Result -and $AlreadyPrinted -ne 'E2') {
    $e2Why = 'OK'; if ((Key32 $p.E2Result) -eq 'E00000A6') { $e2Why = 'round timeout, memory readback mismatch, or setup failure; GPU remains resident' }
    Sum ('E2 result {0} ({1}); completed {2}; ring {3}; wraps {4}; failed round {5}; max polls {6}; total us {7}; colors {8}->{9}; semaphores {10}->{11}' -f (Hex $p.E2Result), $e2Why, (Hex $p.E2Rounds), (Hex $p.E2RingEntries), (Hex $p.E2Wraps), (Hex $p.E2FailRound), (Hex $p.E2MaxPolls), (Hex $p.E2TotalUs), (Hex $p.E2FirstColor), (Hex $p.E2LastColor), (Hex $p.E2FirstSemaphore), (Hex $p.E2LastSemaphore))
  }
  Show-E4Pool $p
  if ($null -ne $p.E3Result -and $AlreadyPrinted -ne 'E3') {
    $e3Why = 'OK'; if ((Key32 $p.E3Result) -eq 'E00000A7') { $e3Why = 'no interrupt resources; use UEFI R447H or later' } elseif ((Key32 $p.E3Result) -eq 'E00000A8') { $e3Why = 'round timeout or clear/readback failure; GPU remains resident' } elseif ((Key32 $p.E3Result) -eq 'E00000A9') { $e3Why = 'interrupt setup/write failure' }
    Sum ('E3 result {0} ({1}); interrupts {2}; rounds {3}/64; waits {4}; nonstall/stall {5}/{6}; stall units {7}; max wait us {8}; total us {9}; timeouts {10}; MC first {11}/{12}, last {13}/{14}' -f (Hex $p.E3Result), $e3Why, (Hex $p.E3HaveInterrupts), (Hex $p.E3Rounds), (Hex $p.E3Waits), (Hex $p.E3NonstallCount), (Hex $p.E3StallCount), (Hex $p.E3StallUnits), (Hex $p.E3MaxWaitUs), (Hex $p.E3TotalUs), (Hex $p.E3Timeouts), (Hex $p.E3FirstMcIntr0), (Hex $p.E3FirstMcIntr1), (Hex $p.E3LastMcIntr0), (Hex $p.E3LastMcIntr1))
  }
  $errors = @($p.PSObject.Properties | Where-Object { $_.Name -match 'Priv.*(?:ErrorCount|ErrorDropped|AckTimeouts)$' -and $_.Value -ne 0 })
  if ($errors.Count) {
    $note = ''
    # Records: 16 DWORDs each, word 0 = rejected write address.
    $addrs = @()
    $blob = $p.B3cC2bPrivWriteErrors
    if ($blob -is [byte[]]) {
      for ($o = 0; $o + 4 -le $blob.Length; $o += 64) { $addrs += ('{0:X8}' -f [BitConverter]::ToUInt32($blob, $o)) }
    }
    if ($addrs.Count) { $note = ' (C2b rejected: ' + ($addrs -join ', ') + ')' }
    if ($errors.Count -eq 1 -and $errors[0].Name -eq 'B3cC2bPrivWriteErrorCount' -and (($addrs | Sort-Object) -join ',') -eq '00419DEC,00419DF4') {
      $note = ' (known: the sw_nonctx netlist writes 0 to 0x419DEC and 0x419DF4 and this GPC rejects them; same since 0.4.x, harmless)'
    }
    Sum ('priv-ring errors: ' + (($errors | ForEach-Object { '{0}={1}' -f $_.Name, $_.Value }) -join ' ') + $note)
  }
  if ($Detail -or ($p.Verbose -eq 1 -and $p.E5VerboseActive -eq 1)) {
    Log ('Detailed registry trace (driver verbose active: {0})' -f $p.E5VerboseActive)
    foreach ($prop in ($p.PSObject.Properties | Sort-Object Name)) {
      if ($prop.Name -match '^(PSPath|PSParentPath|PSChildName|PSDrive|PSProvider|Fw)') { continue }
      if ($prop.Value -is [byte[]]) {
        $bytes = [byte[]]$prop.Value
        Log ('  {0}: {1} bytes' -f $prop.Name, $bytes.Length)
        for ($i = 0; $i -lt $bytes.Length; $i += 32) {
          Log ('    {0:X4}  {1}' -f $i, [BitConverter]::ToString($bytes, $i, [Math]::Min(32, $bytes.Length - $i)))
        }
      } elseif ($prop.Value -is [int] -or $prop.Value -is [uint32]) {
        Log ('  {0} = {1}' -f $prop.Name, (Hex $prop.Value))
      }
    }
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
  { $_ -eq 'ResidentStatus' -or $_ -eq 'ResidentStop' } {
    try { Invoke-ResidentControl ($Mode -eq 'ResidentStop') }
    catch { Sum ('resident IOCTL failed: {0}' -f $_.Exception.Message) }
    Show-Device; Log ''; Show-Trace
  }
  'Blit' {
    try {
      Initialize-ResidentControl
      Remove-ItemProperty -Path $Key -Name F2cFailOffset, F2cFailPrevious, F2cFailMask, F2cFailWanted, F2cFailReadback, F2cFailWritten, F2cSelV0Read, F2cSelV1Read, F2cSelV2Read, F2cSelVariant, F2cSelPolls, F2cCfgAtSelFail, F2cCoeffAtSelFail, F2cOutAtSelFail, F2cBypassAtSelFail, F2cCfgBeforeEnable, F2cCfgAfterEnable, F2cCfgAtLock -ErrorAction SilentlyContinue
      $r = [SwGpuResidentControl]::RunBlit()
      Sum ('F2 blit result {0}; verified {1}/4; GPU {2} MB/s ({3}/20, {4} us); CPU {5} MB/s ({6}/20, {7} us); max wait {8} us; IRQ wakes {9}' -f (Hex $r.Result), $r.Verified, $r.GpuMBps, $r.GpuRounds, $r.GpuUs, $r.CpuMBps, $r.CpuRounds, $r.CpuUs, $r.MaxWaitUs, $r.IrqWakes)
      Sum ('F2 blit polls submit/map/unmap/abort {0}/{1}/{2}/{3}; mismatch offset {4}' -f $r.MaxPolls, $r.MapPolls, $r.UnmapPolls, $r.AbortPolls, (Hex $r.FailOffset))
      if ($r.Bypass -ne [uint32]::MaxValue) {
        $bypass = if ($r.Bypass -eq 1) { 'yes' } else { 'no' }
        $vout = if ($r.VoutRaw -eq [uint32]::MaxValue) { 'n/a' } else { '{0} mV' -f $r.VoutMv }
        $ref = if ($r.RefKhz -eq 0) { 'n/a' } else { $r.RefKhz }
        $gpc = if ($r.GpcKhz -eq 0) { 'n/a' } else { $r.GpcKhz }
        $gpc2 = if ($r.Gpc2Khz -eq 0) { 'n/a' } else { $r.Gpc2Khz }
        Sum ('F2b clock ref/GPC2/GPC {0}/{1}/{2} kHz nominal; bypass {3}; VOUT {4} ({5}); CFG/COEFF/SEL/OUT/BYPASS {6}/{7}/{8}/{9}/{10}' -f $ref, $gpc2, $gpc, $bypass, (Hex $r.VoutRaw), $vout, (Hex $r.PllCfg), (Hex $r.PllCoeff), (Hex $r.SelVco), (Hex $r.Gpc2Out), (Hex $r.BypassCtrl))
        $measured = if ($r.MeasuredKhz -eq 0) { 'n/a' } else { $r.MeasuredKhz }
        Sum ('F2b clock slowdown/ramp {0}/{1}; CFG2/DVFS0/1 {2}/{3}/{4}; CAR OSC/CLK_X/RST_X/PLLP_OUTC {5}/{6}/{7}/{8}; idle slowdown {9}; TRIM cfg/count {10}/{11}, samples {12}; prior measured GPC {13} kHz' -f (Hex $r.Slowdown), (Hex $r.RampStatus), (Hex $r.PllCfg2), (Hex $r.Dvfs0), (Hex $r.Dvfs1), (Hex $r.OscCtrl), (Hex $r.CarClkX), (Hex $r.CarRstX), (Hex $r.CarPllpOutc), (Hex $r.IdleSlowdown), (Hex $r.CntrCfg), (Hex $r.CntrValue), $r.CntrSamples, $measured)
      }
      $mbps = {
        param([long]$Bytes, [uint32]$Us)
        if ($Us -eq 0) { 'n/a' } else { '{0:F1}' -f ($Bytes / $Us) }
      }
      $frameBytes = 1920 * 1080 * 4
      $totalBytes = 20 * $frameBytes
      Sum ('F2b bench MB/s (us) CPU cached/WC {0} ({1}) / {2} ({3}); GPU a/b/c/WC {4} ({5}) / {6} ({7}) / {8} ({9}) / {10} ({11}); stage {12}' -f (& $mbps $totalBytes $r.CpuUs), $r.CpuUs, (& $mbps $totalBytes $r.CpuWcUs), $r.CpuWcUs, (& $mbps $totalBytes $r.GpuUs), $r.GpuUs, (& $mbps $totalBytes $r.GpuBatchUs), $r.GpuBatchUs, (& $mbps $frameBytes $r.GpuLinesUs), $r.GpuLinesUs, (& $mbps $totalBytes $r.GpuWcUs), $r.GpuWcUs, $r.BenchStage)
      $best = $null
      for ($i = 0; $i -lt [Math]::Min([int]$r.ClockSteps, $r.Clock.Length); $i++) {
        $step = $r.Clock[$i]
        $label = if ($step.TargetKhz -eq 0) { '0 kHz (bypass)' } else { '{0} kHz' -f $step.TargetKhz }
        $measured = if ($step.MeasuredKhz -eq 0) { 'n/a' } else { $step.MeasuredKhz }
        $stepResult = if ((Hex $step.Result) -eq '0xC00000BB') { 'skipped' } else { Hex $step.Result }
        Sum ('F2c step {0}; result {1}; CVB {2} mV; M/N/PL {3}/{4}/{5}; lock/ramp polls {6}/{7}; TRIM {8} kHz; verified {9}/4; cached/WC {10}/{11} MB/s ({12}/{13} us); stage {14}; mismatch {15}' -f $label, $stepResult, $step.CvbMv, $step.M, $step.N, $step.Pl, $step.LockPolls, $step.RampPolls, $measured, $step.Verified, (& $mbps $totalBytes $step.CachedUs), (& $mbps $totalBytes $step.WcUs), $step.CachedUs, $step.WcUs, $step.Stage, (Hex $step.FailOffset))
        if ($step.Result -eq 0 -and $step.Verified -eq 4 -and $step.CachedUs -gt 0 -and $step.WcUs -gt 0 -and
            ($null -eq $best -or $step.CachedUs -lt $best.CachedUs)) { $best = $step }
      }
      $pf = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($pf -and $null -ne $pf.F2cFailOffset) {
        Sum ('F2c failed write: offset {0}; previous {1}; mask {2}; wanted {3}; readback {4}; written {5}' -f (Hex $pf.F2cFailOffset), (Hex $pf.F2cFailPrevious), (Hex $pf.F2cFailMask), (Hex $pf.F2cFailWanted), (Hex $pf.F2cFailReadback), $pf.F2cFailWritten)
      }
      if ($pf -and $null -ne $pf.F2cCfgAtLock) {
        Sum ('F2c diag CFG before/after enable/at lock {0}/{1}/{2}; SEL readback v0/v1/v2 {3}/{4}/{5}; SEL variant {6} polls {7}; at SEL fail CFG/COEFF/OUT/BYPASS {8}/{9}/{10}/{11}' -f (Hex $pf.F2cCfgBeforeEnable), (Hex $pf.F2cCfgAfterEnable), (Hex $pf.F2cCfgAtLock), (Hex $pf.F2cSelV0Read), (Hex $pf.F2cSelV1Read), (Hex $pf.F2cSelV2Read), (Hex $pf.F2cSelVariant), $pf.F2cSelPolls, (Hex $pf.F2cCfgAtSelFail), (Hex $pf.F2cCoeffAtSelFail), (Hex $pf.F2cOutAtSelFail), (Hex $pf.F2cBypassAtSelFail))
      }
      if ($null -eq $best) {
        Sum ('F2c best n/a; bypass restore {0}' -f (Hex $r.RestoreResult))
      } else {
        $label = if ($best.TargetKhz -eq 0) { 'bypass' } else { '{0} kHz' -f $best.TargetKhz }
        Sum ('F2c best {0}; cached/WC {1}/{2} MB/s; bypass restore {3}' -f $label, (& $mbps $totalBytes $best.CachedUs), (& $mbps $totalBytes $best.WcUs), (Hex $r.RestoreResult))
      }
    } catch {
      Sum ('F2 blit IOCTL failed: {0}' -f $_.Exception.Message)
    }
    if ($Detail) { Show-Trace }
  }
  'InstallDisplay' {
    $inf = Join-Path $Here 'display\switchdisplay.inf'
    if (-not (Test-Path $inf)) { Sum "missing: $inf"; break }
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Sum "switchdisplay pnputil exit=$LASTEXITCODE"
  }
  { $_ -eq 'F3On' -or $_ -eq 'F3Off' } {
    $enable = [int]($Mode -eq 'F3On')
    if ($enable -and -not (Import-GpuFirmware)) { break }
    $displayKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters'
    foreach ($rk in @($Key, $displayKey)) { if (-not (Test-Path $rk)) { New-Item -Path $rk -ErrorAction Stop | Out-Null } }  # 0.12.6: -Force wiped the firmware and F3 values
    New-ItemProperty -Path $Key -Name AutoResident -PropertyType DWord -Value $enable -Force -ErrorAction Stop | Out-Null
    if ($enable -and $null -eq (Get-ItemProperty -LiteralPath $Key -Name IdlePowerOffMs -ErrorAction SilentlyContinue)) {
      New-ItemProperty -LiteralPath $Key -Name IdlePowerOffMs -PropertyType DWord -Value 10000 -ErrorAction Stop | Out-Null
    }
    New-ItemProperty -Path $displayKey -Name GpuPresent -PropertyType DWord -Value $enable -Force -ErrorAction Stop | Out-Null
    # 0.12.4: F2c hardware showed 460.8 MHz gives ~5 GB/s (CVB 950 mV, same as
    # 614.4 MHz which is memory bound); F3 runs the resident GPU there.
    $khz = if ($enable) { 460800 } else { 0 }
    New-ItemProperty -Path $Key -Name GpcKhz -PropertyType DWord -Value $khz -Force -ErrorAction Stop | Out-Null
    New-ItemProperty -Path $Key -Name AutoResidentPending -PropertyType DWord -Value 0 -Force -ErrorAction Stop | Out-Null
    New-ItemProperty -Path $Key -Name AutoResidentGuardTripped -PropertyType DWord -Value 0 -Force -ErrorAction Stop | Out-Null
    Sum ('F3 AutoResident / GpuPresent = {0}; GpcKhz = {1}. Restart Windows to apply.' -f $enable, $khz)
  }
  { $_ -eq 'F4On' -or $_ -eq 'F4Off' } {
    $enable = [int]($Mode -eq 'F4On')
    $displayKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters'
    if (-not (Test-Path -LiteralPath $displayKey)) {
      New-Item -Path $displayKey -ErrorAction Stop | Out-Null
    }
    New-ItemProperty -LiteralPath $displayKey -Name DoubleBuffer -PropertyType DWord -Value $enable -Force -ErrorAction Stop | Out-Null
    Sum ('F4 DoubleBuffer = {0}. Restart Windows to apply.' -f $enable)
  }
  'F3Status' {
    $gpu = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    $d = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters' -ErrorAction SilentlyContinue
    Sum ('F3 AutoResident {0}; GpuPresent {1}; display version {2}; connected {3}; disabled {4}' -f $gpu.AutoResident, $d.GpuPresent, (Hex $d.Version), $d.GpuConnected, $d.GpuDisabled)
    Sum ('F3 GPU not-ready why {0} (count {1}; offline resident/idle-ref {2}/{3}; bits 1=irq off 2=not resident 4=owner 8=reset 10=copy busy 20/40/80=channel/runlist/inst 100=BAR 200=ACR VM 400=GR VM 800=CE off 80000000=blit offline)' -f (Hex $gpu.F3NotReadyWhy), $gpu.F3NotReadyCount, $gpu.F3OfflineResident, $gpu.F3OfflineIdleRef)
    Sum ('F3 GPU clock requested {0} kHz; resident clock result {1}; CVB {2} mV; resident active {3}; boot guard tripped {4}' -f $gpu.GpcKhz, (Hex $gpu.F2cResidentClockResult), $gpu.F2cResidentClockCvbMv, $gpu.B3eResidentActive, $gpu.AutoResidentGuardTripped)
    $power = if ($null -eq $gpu.G1State) { 'n/a' } elseif ($gpu.B3eResidentActive -eq 1 -and $gpu.G1State -eq 1) { 'on' } else { 'off' }
    Sum ('G1 {0}; policy {1}; idle threshold {2} ms; offs/ons {3}/{4}; last off after {5} ms idle; last on {6} us; on failures {7}; disabled {8}; manual stop {9}; last E1 {10}; drain {11}' -f $power, $gpu.G1Policy, $gpu.G1IdlePowerOffMs, $gpu.G1PowerOffs, $gpu.G1PowerOns, $gpu.G1LastOffMs, $gpu.G1LastOnUs, $gpu.G1OnFailures, $gpu.G1Disabled, $gpu.G1ManualStop, (Hex $gpu.E1Result), (Hex $gpu.G1DrainStatus))
    Sum ('F3 boot start: E1 result {0}; step {1}; retry {2} -> {3}; firmware read fail stage {4} status {5} need/max {6}/{7}; firmware in registry: {8}' -f (Hex $gpu.E1Result), $gpu.Step, $gpu.AutoResidentRetry, (Hex $gpu.AutoResidentRetryResult), $gpu.FwReadFailStage, (Hex $gpu.FwReadFailStatus), $gpu.FwReadFailNeed, $gpu.FwReadFailMax, ((@('FwAcrBl','FwAcrUcodeLoad','FwPmuImage','FwFecsInst') | ForEach-Object { '{0}={1}' -f $_, $(if ($gpu -and $null -ne $gpu.$_) { ([byte[]]$gpu.$_).Length } else { 'missing' }) }) -join ' '))
    Sum ('F3 GPU presents {0}; CPU fallbacks {1}; failures {2}; not-ready {6}; too small for GPU {7}; no GPU destination {8}; average GPU/CPU us {3}/{4}; last status {5}' -f $d.GpuPresents, $d.GpuFallbacks, $d.GpuFailures, $d.GpuAverageUs, $d.GpuCpuAverageUs, (Hex $d.GpuLastStatus), $d.GpuNotReady, $d.GpuSmall, $d.GpuNoDst)
    Sum ('F4 DoubleBuffer {0}; active {1}; lock timeouts {2}; panel primary PA {3}; primary MDL {4}' -f $d.DoubleBuffer, $d.F4Active, $d.F4LockTimeouts, (Hex $d.F4PanelPrimaryLo), $d.F4PanelPrimaryMdl)
    foreach ($target in @('Panel','Tv')) {
      $prefix = 'F4' + $target
      Sum ('F4 {0} enabled/front {1}/{2}; flips {3}; timeouts {4}; max wait {5} us; last polls {6}; forward GPU/CPU {7}/{8}; fallbacks {9}; retained {10}; last status {11}' -f $target, $d.($prefix + 'Enabled'), $d.($prefix + 'Front'), $d.($prefix + 'Flips'), $d.($prefix + 'Timeouts'), $d.($prefix + 'MaxUs'), $d.($prefix + 'Polls'), $d.($prefix + 'GpuForward'), $d.($prefix + 'CpuForward'), $d.($prefix + 'Fallbacks'), $d.($prefix + 'Retained'), (Hex $d.($prefix + 'Status')))
    }
  }
  'Copy' {
    try {
      Initialize-ResidentControl
      Show-F1Copy ([SwGpuResidentControl]::RunCopy())
    } catch {
      Sum ('F1 copy result: IOCTL failed (resident channel required): {0}' -f $_.Exception.Message)
    }
    Show-Device
    if ($Detail) { Show-Trace }
  }
  'Cycle' {
    if (Import-GpuFirmware) { Invoke-ResidentCycle }
    Show-Device
    $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    if ($Detail -or ($p.Verbose -eq 1 -and $p.E5VerboseActive -eq 1)) { Show-Trace }
  }
  'Submit' {
    $oldE2 = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    if ($oldE2) {
      foreach ($n in @($oldE2.PSObject.Properties | Where-Object { $_.Name -match '^E2' } | ForEach-Object { $_.Name })) {
        Remove-ItemProperty -Path $Key -Name $n -ErrorAction SilentlyContinue
      }
    }
    try { Invoke-ResidentSubmit }
    catch { Sum ('E2 IOCTL failed (resident channel required): {0}' -f $_.Exception.Message) }
    Show-Device; Show-Trace 'E2'
  }
  'Irq' {
    $oldE3 = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    if ($oldE3) {
      foreach ($n in @($oldE3.PSObject.Properties | Where-Object { $_.Name -match '^E3' } | ForEach-Object { $_.Name })) {
        Remove-ItemProperty -Path $Key -Name $n -ErrorAction SilentlyContinue
      }
    }
    try { Invoke-ResidentIrq }
    catch { Sum ('E3 IOCTL failed (resident channel and R447H+ interrupts required): {0}' -f $_.Exception.Message) }
    Show-Device; Show-Trace 'E3'
  }
  'Resident' {
    if (-not (Test-Path $Key)) { New-Item -Path $Key | Out-Null }  # 0.12.6: never -Force (it wipes an existing key)
    $old = Get-ItemProperty $Key -ErrorAction SilentlyContinue
    if ($old) {
      foreach ($n in @($old.PSObject.Properties | Where-Object { $_.Name -match '^(Acr|E1|B3e|B2a|B3a|B3b|B3c|Smmu|Mc[IE]|McHotStat(Before|After)Dma|McHotStatAfter(Reset|Flush|Bar0)|McHotStatB2aStart|Pmu|Pfifo|Ptop|Scal|Dmem|Ring|Wpr|PmcEnable)' } | ForEach-Object { $_.Name })) {
        Remove-ItemProperty -Path $Key -Name $n -ErrorAction SilentlyContinue
      }
    }
    if (-not (Import-GpuFirmware)) { break }
    Remove-ItemProperty -Path $Key -Name E1Result,E5PtimerNsPer1ms -ErrorAction SilentlyContinue
    Set-ItemProperty -Path $Key -Name PowerTest -Value 6 -Type DWord
    Log "PowerTest = 6; restarting ACPI\SWOA0F01\0 ..."
    $out = (& $Pnputil /restart-device 'ACPI\SWOA0F01\0' 2>&1)
    $out | ForEach-Object { Log "  $_" }
    if ($LASTEXITCODE -ne 0) {
      Log '  restart-device failed; trying disable + enable'
      (& $Pnputil /disable-device 'ACPI\SWOA0F01\0' 2>&1) | ForEach-Object { Log "  $_" }
      (& $Pnputil /enable-device 'ACPI\SWOA0F01\0' 2>&1) | ForEach-Object { Log "  $_" }
    }
    $deadline = (Get-Date).AddSeconds(180)
    do {
      Start-Sleep -Seconds 1
      $q = Get-ItemProperty $Key -ErrorAction SilentlyContinue
      if ($null -ne $q.E1Result) { break }
    } while ((Get-Date) -lt $deadline)
    if ($null -eq $q -or $null -eq $q.E1Result) { Sum 'E1 completion trace not observed within 180 seconds' }
    Show-Device; Log ''; Show-Trace
  }
  { $_ -eq 'VerboseOn' -or $_ -eq 'VerboseOff' } {
    if (-not (Test-Path $Key)) { New-Item -Path $Key -ErrorAction Stop | Out-Null }  # 0.12.6: never -Force (it wipes an existing key)
    $enabled = [int]($Mode -eq 'VerboseOn')
    New-ItemProperty -Path $Key -Name Verbose -PropertyType DWord -Value $enabled -Force -ErrorAction Stop | Out-Null
    Sum ('Driver Verbose = {0}; applies on the next GPU device start.' -f $enabled)
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
