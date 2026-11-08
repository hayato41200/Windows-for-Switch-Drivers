<#
.SYNOPSIS
  Display (screen) on the Switch's Windows (Administrator).

  -Mode Report     : read only. Display adapters and their drivers, monitors,
                     current mode and rotation, DPI, powercfg /a, dxdiag summary.
  -Mode Install    : switchdisplay. Needs UEFI R408 or later (DSDT DISP =
                     ACPI\SWOA0E01, seen at least once). Removes the old test
                     root device and every old switchdisplay package, then
                     pnputil /add-driver switchdisplay.inf /install.
  -Mode Status     : the display adapters and the switchdisplay trace.
  -Mode RotateTest : lists the display modes, then rotates with
                     ChangeDisplaySettingsEx (the API SwitchCenter uses) to 90,
                     180, 270 and back, 4 s each, printing each result code.
  -Mode Remove     : removes the driver package (and the old test root device).
                     Windows goes back to the Microsoft Basic Display Driver
                     after a restart.

  Log: C:\SwitchWOA-Display.txt
  This file must stay ASCII: Windows PowerShell 5.1 reads it as the ANSI code page.
#>
param([Parameter(Mandatory = $true)][ValidateSet('Report', 'Install', 'Status', 'Remove', 'RotateTest')][string]$Mode)

$ErrorActionPreference = 'Continue'
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Powercfg = Join-Path $Sys 'powercfg.exe'
$Dxdiag = Join-Path $Sys 'dxdiag.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Log = 'C:\SwitchWOA-Display.txt'
$Key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters'
$Summary = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
function Sum([string]$s) { $Summary.Add($s); Log $s }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }
# A touch on the console window starts QuickEdit selection and pauses the script.
try {
  Add-Type -Namespace SwoaCon -Name Native -MemberDefinition @'
[DllImport("kernel32.dll")] public static extern IntPtr GetStdHandle(int n);
[DllImport("kernel32.dll")] public static extern bool GetConsoleMode(IntPtr h, out uint m);
[DllImport("kernel32.dll")] public static extern bool SetConsoleMode(IntPtr h, uint m);
'@
  $h = [SwoaCon.Native]::GetStdHandle(-10)
  $m = [uint32]0
  if ([SwoaCon.Native]::GetConsoleMode($h, [ref]$m)) { [void][SwoaCon.Native]::SetConsoleMode($h, (($m -band (-bnot [uint32]0x40)) -bor 0x80)) }
} catch { }
# powercfg / pnputil print Japanese; a UTF-8 console keeps it readable.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

# Display settings, with the same struct and calls as SwitchCenter
# (AutoScreen.cs). A DEVMODE declared in PowerShell itself failed even on a PC.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SwoaDisp {
  [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
  public struct DevMode {
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
    public ushort SpecVersion, DriverVersion, Size, DriverExtra; public uint Fields;
    public int PositionX, PositionY; public uint DisplayOrientation, DisplayFixedOutput;
    public short Color, Duplex, YResolution, TTOption, Collate;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string FormName;
    public ushort LogPixels; public uint BitsPerPel, PelsWidth, PelsHeight, DisplayFlags, DisplayFrequency;
    public uint IcmMethod, IcmIntent, MediaType, DitherType, Reserved1, Reserved2, PanningWidth, PanningHeight;
  }
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettings(string device, int mode, ref DevMode dm);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int ChangeDisplaySettingsEx(string device, ref DevMode dm, IntPtr hwnd, uint flags, IntPtr param);
  [DllImport("user32.dll")] static extern IntPtr GetDC(IntPtr h);
  [DllImport("gdi32.dll")] static extern int GetDeviceCaps(IntPtr hdc, int i);
  static bool Read(int mode, out DevMode dm) { dm = new DevMode { Size = (ushort)Marshal.SizeOf(typeof(DevMode)) }; return EnumDisplaySettings(null, mode, ref dm); }
  // mode -1 = current settings; null when Windows has no such mode
  public static string Mode(int i) { DevMode d; if (!Read(i, out d)) return null; return d.PelsWidth + "x" + d.PelsHeight + " " + d.BitsPerPel + " bpp " + d.DisplayFrequency + " Hz orientation " + d.DisplayOrientation; }
  public static int Orientation() { DevMode d; return Read(-1, out d) ? (int)d.DisplayOrientation : -1; }
  public static int Dpi() { return GetDeviceCaps(GetDC(IntPtr.Zero), 88); }
  // ChangeDisplaySettingsEx result, or 100 = the current settings could not be read
  public static int Set(int o) {
    DevMode dm; if (!Read(-1, out dm)) return 100;
    if ((dm.DisplayOrientation + o) % 2 == 1) { uint w = dm.PelsWidth; dm.PelsWidth = dm.PelsHeight; dm.PelsHeight = w; }
    dm.DisplayOrientation = (uint)o;
    dm.Fields = 0x80 | 0x80000 | 0x100000;   // the fields SwitchCenter sets
    return ChangeDisplaySettingsEx(null, ref dm, IntPtr.Zero, 1, IntPtr.Zero);
  }
}
'@

Log ''
Log "=== Display $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Show-Adapters {
  Log '--- display adapters (class Display)'
  # The registry, not WMI: WMI returned nothing on the Switch.
  $guid = '{4d36e968-e325-11ce-bfc1-08002be10318}'
  $found = 0
  foreach ($bus in 'ROOT', 'ACPI', 'SWD') {
    foreach ($k in @(Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Enum\$bus" -ErrorAction SilentlyContinue)) {
      foreach ($i in @(Get-ChildItem $k.PSPath -ErrorAction SilentlyContinue)) {
        $p = Get-ItemProperty $i.PSPath -ErrorAction SilentlyContinue
        if ($p -and $p.ClassGUID -eq $guid) {
          $found++
          $desc = "$($p.DeviceDesc)" -replace '^@[^;]*;', ''
          Sum ("adapter: {0}\{1}\{2}  service={3}  '{4}'" -f $bus, $k.PSChildName, $i.PSChildName, $p.Service, $desc)
        }
      }
    }
  }
  if ($found -eq 0) { Sum 'adapter: none found in the registry (ROOT, ACPI, SWD)' }
  Log '  pnputil /enum-devices /class Display:'
  (& $Pnputil /enum-devices /class Display 2>&1) | ForEach-Object { Log "    $_" }
}

function Show-Trace {
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'switchdisplay trace: (no Parameters key)'; return }
  $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
  Log ('switchdisplay trace: ' + ($vals -join ' '))
  if ($null -ne $p.Version) { Sum ('switchdisplay {0}.{1}.{2}  starts {3}' -f ($p.Version -shr 16), (($p.Version -shr 8) -band 0xFF), ($p.Version -band 0xFF), $p.StartCount) }
  $src = @{ 0 = 'none (start refused)'; 1 = 'taken from Basic Display (POST)'; 2 = 'fixed 0xdfb80000' }
  if ($null -ne $p.FbSource) { Sum ('framebuffer: {0}' -f $src[[int]$p.FbSource]) }
  if ($null -ne $p.PostStatus) { Sum ('POST ownership: status 0x{0:X8}  {1}x{2} pitch {3} addr 0x{4:X8}' -f [uint32]$p.PostStatus, $p.PostWidth, $p.PostHeight, $p.PostPitch, [uint32]$p.PostAddrLo) }
  if ($null -ne $p.Rotation) { Sum ('rotation (1=0, 2=90, 3=180, 4=270): {0}  source {1}x{2}' -f $p.Rotation, $p.SrcWidth, $p.SrcHeight) }
  if ($null -ne $p.Presents) { Sum ('presents: {0}  errors {1} (last 0x{2:X8})  commits {3}  power calls {4} (last 0x{5:X})' -f $p.Presents, $p.PresentErrors, [uint32]$p.LastPresentStatus, $p.Commits, $p.PowerCalls, [uint32]$p.LastPower) }
  if ($null -ne $p.PanelOffs) { Sum ('screen off: panel sleep {0} / wake {1}  DC stops {2}  DSI status 0x{3:X}  (PanelPower={4}, DcStop={5})' -f $p.PanelOffs, $p.PanelOns, $p.DcStops, [uint32]$p.DsiStatusAfterOn, $(if ($null -ne $p.PanelPower) { $p.PanelPower } else { 'default 1' }), $(if ($null -ne $p.DcStop) { $p.DcStop } else { 'default 1' })) }
  if ($null -ne $p.HasInterrupt) { Sum ('vsync: DSDT interrupt {0} (vector 0x{1:X})  on {2} / off {3}  vblank interrupts {4}  other {5}  ISR calls {7} unclaimed {8}  DC int at AddDevice: status 0x{9:X} mask 0x{10:X} enable 0x{11:X}  (Vsync={6})' -f $(if ($p.HasInterrupt) { 'yes' } else { 'no (UEFI R408: Windows uses its timer)' }), [uint32]$p.ResIrqVector, $p.VsyncEnables, $p.VsyncDisables, $p.VsyncInts, $p.IsrOther, $(if ($null -ne $p.Vsync) { $p.Vsync } else { 'default 1' }), $p.IsrCalls, $p.IsrUnclaimed, [uint32]$p.EarlyIntStatus, [uint32]$p.EarlyIntMask, [uint32]$p.EarlyIntEnable) }
  if ($null -ne $p.CiCalls) { Sum ('ControlInterrupt: calls {0}  types 0x{1:X}  last type {2} enable {3} -> 0x{4:X8}' -f $p.CiCalls, [uint32]$p.CiTypes, ([uint32]$p.CiLast -shr 8), ([uint32]$p.CiLast -band 1), [uint32]$p.CiLastSt) }
  $ddi = 'QueryChildRelations','QueryChildStatus','QueryDeviceDescriptor','QueryAdapterInfo','IsSupportedVidPn','RecommendFunctionalVidPn','EnumVidPnCofuncModality','SetVidPnSourceVisibility','CommitVidPn','UpdateActiveVidPnPresentPath','RecommendMonitorModes','QueryVidPnHWCapability','SetPowerState','SystemDisplayEnable','SetPointerPosition','Escape'
  $failed = @()
  foreach ($n in $ddi) {
    $f = $p."F_$n"
    if ($f) { $failed += ('{0} {1}/{2} last 0x{3:X8}' -f $n, $f, $p."N_$n", [uint32]$p."S_$n") }
  }
  if ($failed.Count) { Sum ('DDI failures (failed/calls): ' + ($failed -join ', ')) } else { Sum 'DDI failures: none (QueryDeviceDescriptor fails by design: no EDID)' }
  if ($p.VidPnNotSupported) { Sum ('VidPN refused {0} time(s): reason {1} a=0x{2:X} b=0x{3:X}' -f $p.VidPnNotSupported, $p.RejCode, [uint32]$p.RejA, [uint32]$p.RejB) }
  if ($p.GuardTripped) { Sum ('boot-loop guard tripped {0} time(s): the driver refused to start after crashes' -f $p.GuardTripped) }
}

function Show-Report {
  Show-Adapters
  Log ''
  Log '--- mode, rotation, DPI'
  $c = [SwoaDisp]::Mode(-1)
  Sum ('current: ' + $(if ($c) { $c } else { 'EnumDisplaySettings(current) failed' }))
  Sum ('DPI (this process): {0}  (96 = 100%)' -f [SwoaDisp]::Dpi())
  $lp = Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction SilentlyContinue
  if ($lp) { Sum ('scaling (LogPixels): {0}' -f $lp.LogPixels) }
  Log ''
  Log '--- powercfg /a'
  $pa = (& $Powercfg /a 2>&1)
  $pa | ForEach-Object { Log "  $_" }
  if (($pa | Out-String) -match '(?i)graphics|\u30B0\u30E9\u30D5\u30A3\u30C3\u30AF') { Sum 'powercfg /a mentions graphics (see the log)' }
  Log ''
  Log '--- dxdiag (about 20-40 s)'
  $tmp = Join-Path $env:TEMP 'swoa-dxdiag.txt'
  Remove-Item $tmp -ErrorAction SilentlyContinue
  try {
    $p = Start-Process -FilePath $Dxdiag -ArgumentList "/t `"$tmp`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Log '  dxdiag did not finish in 90 s'; try { $p.Kill() } catch { } }
  } catch { Log "  dxdiag failed: $_" }
  if (Test-Path $tmp) {
    $want = 'Card name|Manufacturer|Chip type|Driver Name|Driver File Version|Driver Model|Feature Levels|Current Mode|Native Mode|Monitor Name|DirectDraw Acceleration|Direct3D Acceleration'
    Get-Content $tmp | Where-Object { $_ -match "^\s*($want):" } | Select-Object -First 30 | ForEach-Object { Log "  $($_.Trim())" }
    $fl = Get-Content $tmp | Where-Object { $_ -match '^\s*Feature Levels:' } | Select-Object -First 1
    if ($fl) { Sum ("dxdiag: $($fl.Trim())") }
  }
  Log ''
  Show-Trace
}

# The old test root device (switchdisplay 1.0.0/1.0.1) and every switchdisplay package.
function Remove-Old {
  foreach ($i in @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ROOT\SWOA_DISPLAY' -ErrorAction SilentlyContinue)) {
    $id = "ROOT\SWOA_DISPLAY\$($i.PSChildName)"
    Log "pnputil /remove-device $id"
    (& $Pnputil /remove-device $id 2>&1) | ForEach-Object { Log "  $_" }
  }
  $oem = & $Pnputil /enum-drivers 2>&1 | Out-String
  # Language independent: the line after 'oemNN.inf' ends with the original name.
  foreach ($x in [regex]::Matches($oem, '(oem\d+\.inf)\s*\r?\n[^\r\n]*:\s*switchdisplay\.inf')) {
    $name = $x.Groups[1].Value
    Log "pnputil /delete-driver $name /uninstall /force"
    (& $Pnputil /delete-driver $name /uninstall /force 2>&1) | ForEach-Object { Log "  $_" }
  }
}

# Windows' own view: why a display device was stopped (problem 43 and the like).
function Show-Events {
  Log ''
  Log '--- events (last 24 h): display, dxgkrnl, PnP'
  $since = (Get-Date).AddHours(-24)
  $n = 0
  foreach ($logName in 'System', 'Microsoft-Windows-DxgKrnl-Admin', 'Microsoft-Windows-DxgKrnl-Operational', 'Microsoft-Windows-Kernel-PnP/Configuration') {
    try {
      $ev = @(Get-WinEvent -FilterHashtable @{ LogName = $logName; StartTime = $since } -MaxEvents 400 -ErrorAction Stop |
              Where-Object { $_.ProviderName -match 'Dxg|Display' -or $_.Message -match 'SWOA0E01|switchdisplay' } |
              Select-Object -First 12)
    } catch { $ev = @() }
    foreach ($e in $ev) {
      $n++
      $msg = ("$($e.Message)" -replace '\s+', ' ')
      if ($msg.Length -gt 300) { $msg = $msg.Substring(0, 300) + '...' }
      Log ('  {0:MM-dd HH:mm:ss} [{1}] {2} id {3} level {4}: {5}' -f $e.TimeCreated, $logName, $e.ProviderName, $e.Id, $e.LevelDisplayName, $msg)
      if ($msg -match 'SWOA0E01|switchdisplay' -and $e.Level -le 3) { Sum ('event: {0:MM-dd HH:mm} {1} id {2}: {3}' -f $e.TimeCreated, $e.ProviderName, $e.Id, $msg.Substring(0, [Math]::Min(160, $msg.Length))) }
    }
  }
  if ($n -eq 0) { Log '  (none found)' }
}

switch ($Mode) {
  'Report' { Show-Report }
  'Status' { Show-Adapters; Log ''; Show-Trace; Show-Events }
  'Install' {
    if (-not [Environment]::Is64BitProcess) { Sum 'This PowerShell is not 64-bit (ARM64); run Install.cmd from Explorer.'; break }
    $inf = Join-Path $Here 'switchdisplay.inf'
    if (-not (Test-Path $inf)) { Sum "missing: $inf"; break }
    # DISP must have been seen once (UEFI R408). Booted with an older UEFI, the
    # package is only staged and the next R408 boot installs it.
    $acpi = @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0E01' -ErrorAction SilentlyContinue)
    if ($acpi.Count -eq 0) { Sum 'ACPI\SWOA0E01 has never been seen (boot UEFI R408 once first). Nothing installed.'; break }
    Remove-Old
    Log "pnputil /add-driver $inf /install"
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Sum "pnputil exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Adapters
    Log ''
    Show-Trace
    Sum 'Restart Windows, then run Status.cmd.'
  }
  'RotateTest' {
    $codes = @{ 0 = 'SUCCESSFUL'; 1 = 'RESTART'; -1 = 'FAILED'; -2 = 'BADMODE'; -3 = 'NOTUPDATED'; -4 = 'BADFLAGS'; -5 = 'BADPARAM'; -6 = 'BADDUALVIEW'; 100 = 'CANNOT READ CURRENT' }
    Log '--- modes (EnumDisplaySettings)'
    for ($i = 0; $i -lt 40; $i++) { $m = [SwoaDisp]::Mode($i); if (-not $m) { break }; Log "  ${i}: $m" }
    $c = [SwoaDisp]::Mode(-1)
    Sum ('current: ' + $(if ($c) { $c } else { 'EnumDisplaySettings(current) failed' }))
    $start = [SwoaDisp]::Orientation()
    foreach ($o in @(1, 2, 3, [Math]::Max($start, 0))) {
      $r = [SwoaDisp]::Set($o)
      Sum ('rotate to {0}: {1} {2} -> now {3}' -f $o, $r, $codes[$r], [SwoaDisp]::Mode(-1))
      Start-Sleep -Seconds 4
    }
    Show-Trace
  }
  'Remove' {
    Remove-Old
    Show-Adapters
    Sum 'Restart Windows. The Microsoft Basic Display Driver is used again.'
  }
}

Log ''
Log '=== SUMMARY'
foreach ($s in $Summary) { Write-Host $s }
