<#
.SYNOPSIS
  Display (screen) on the Switch's Windows (Administrator).

  -Mode Report  : stage 0, read only. Display adapters and their drivers,
                  monitors (EDID size), current mode and rotation, DPI,
                  powercfg /a, dxdiag summary (renderer = WARP?).
  -Mode Install : stage 1. Needs UEFI R408 (DSDT DISP = ACPI\SWOA0E01).
                  Removes the 1.0.0/1.0.1 root device and old packages, then
                  pnputil /add-driver switchdisplay.inf /install. Then reports.
  -Mode Status  : the display adapters and the switchdisplay trace.
  -Mode RotateTest : lists the display modes, then rotates with
                  ChangeDisplaySettingsEx (the API SwitchCenter uses) to 90, 180,
                  270 and back, 4 s each, printing each result code.
  -Mode Remove  : removes the old root device and the driver package. Windows
                  goes back to the Microsoft Basic Display Driver (restart).

  Log: C:\SwitchWOA-Display.txt
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
$HwId = 'ROOT\SWOA_DISPLAY'
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
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }
Log ''
Log "=== Display $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

function Show-Adapters {
  Log '--- display adapters (class Display)'
  # Registry first: WMI returned nothing on the Switch (2026-11-08 Report).
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
  try {
    $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction Stop | Where-Object { $_.ClassGuid -eq $guid })
    foreach ($d in $dev) {
      Log ("  wmi: {0} status={1} problem={2}" -f $d.PNPDeviceID, $d.Status, $d.ConfigManagerErrorCode)
    }
    if ($dev.Count -eq 0) { Log '  wmi: no Display devices' }
  } catch { Log "  wmi failed: $($_.Exception.Message)" }
  Log '  pnputil /enum-devices /class Display:'
  (& $Pnputil /enum-devices /class Display 2>&1) | ForEach-Object { Log "    $_" }
}
function Show-Trace {
  $p = Get-ItemProperty $Key -ErrorAction SilentlyContinue
  if (-not $p) { Log 'switchdisplay trace: (no Parameters key)'; return }
  $vals = $p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, [uint32]$_.Value }
  Log ('switchdisplay trace: ' + ($vals -join ' '))
  $src = @{ 0 = 'none (start refused)'; 1 = 'taken from Basic Display (POST)'; 2 = 'fixed 0xdfb80000' }
  if ($null -ne $p.FbSource) { Sum ('framebuffer: {0}' -f $src[[int]$p.FbSource]) }
  if ($null -ne $p.PostStatus) { Sum ('POST ownership: status 0x{0:X8}  {1}x{2} pitch {3} addr 0x{4:X8}' -f [uint32]$p.PostStatus, $p.PostWidth, $p.PostHeight, $p.PostPitch, [uint32]$p.PostAddrLo) }
  if ($null -ne $p.DcStartAddr) { Sum ('DC-A window A: start 0x{0:X8} size 0x{1:X8} stride 0x{2:X8} depth {3}' -f [uint32]$p.DcStartAddr, [uint32]$p.DcWinSize, [uint32]$p.DcLineStride, $p.DcColorDepth) }
  if ($null -ne $p.ResCount) { Sum ('resources: {0} (memory {1}: start 0x{2:X8} length 0x{3:X})' -f $p.ResCount, $p.ResMemCount, [uint32]$p.ResMemStart, [uint32]$p.ResMemLength) }
  if ($null -ne $p.Rotation) { Sum ('rotation (1=0, 2=90, 3=180, 4=270): {0}  source {1}x{2}' -f $p.Rotation, $p.SrcWidth, $p.SrcHeight) }
  if ($null -ne $p.Presents) { Sum ('presents: {0}  errors {1} (last 0x{2:X8})  commits {3}  power calls {4} (last 0x{5:X})' -f $p.Presents, $p.PresentErrors, [uint32]$p.LastPresentStatus, $p.Commits, $p.PowerCalls, [uint32]$p.LastPower) }
  $ddi = 'QueryChildRelations','QueryChildStatus','QueryDeviceDescriptor','QueryAdapterInfo','IsSupportedVidPn','RecommendFunctionalVidPn','EnumVidPnCofuncModality','SetVidPnSourceVisibility','CommitVidPn','UpdateActiveVidPnPresentPath','RecommendMonitorModes','QueryVidPnHWCapability','SetPowerState','SystemDisplayEnable','SetPointerPosition','Escape'
  $line = @()
  foreach ($n in $ddi) {
    $c = $p."N_$n"
    if ($null -ne $c) { $line += ('{0}={1}/{2}/{3:X}' -f $n, $c, $p."F_$n", [uint32]$p."S_$n") }
  }
  if ($line.Count) {
    Sum 'DDI calls (count/failed/last status):'
    for ($i = 0; $i -lt $line.Count; $i += 3) { Sum ('  ' + (($line[$i..([Math]::Min($i + 2, $line.Count - 1))]) -join '  ')) }
    Sum ('QAI types 0x{0:X}  child status types 0x{1:X}  EDID reads {2}  VidPN not supported {3}  reject code {4} a=0x{5:X} b=0x{6:X}  cofunc step {7}' -f [uint32]$p.QaiTypes, [uint32]$p.ChildStatusTypes, $p.EdidReads, $p.VidPnNotSupported, $p.RejCode, [uint32]$p.RejA, [uint32]$p.RejB, $p.CofuncStep)
  }  if ($null -ne $p.TgtVariant) { Sum ('signal form: target {0} (add 0x{1:X8}, assign 0x{2:X8})  monitor {3}   (9/10 = frequency not specified, 1-8 = form 0-7; 0xFF = all refused)' -f $p.TgtVariant, [uint32]$p.TgtAddSt, [uint32]$p.TgtAssignSt, $p.MonVariant) }
  if ($null -ne $p.TgtSt0) { Sum ('target form status 0-9: ' + ((0..9 | ForEach-Object { '{0:X}' -f [uint32]$p."TgtSt$_" }) -join ' ') + ('   EDID given {0}' -f $p.EdidGiven)) }
  if ($null -ne $p.PanelOffs) { Sum ('panel sleep (stage 2a): off {0}  on {1}  DSI map {2}  DSI status before off 0x{3:X} after off 0x{4:X} after on 0x{5:X}  (PanelPower={6})' -f $p.PanelOffs, $p.PanelOns, $p.DsiMapped, [uint32]$p.DsiStatusBeforeOff, [uint32]$p.DsiStatusAfterOff, [uint32]$p.DsiStatusAfterOn, $(if ($null -ne $p.PanelPower) { $p.PanelPower } else { 'default 1' })) }
  if ($null -ne $p.DcStops) { Sum ('DC stop (stage 2b): stops {0}  DC command after stop 0x{1:X} after start 0x{2:X}  DSI status after DC start 0x{3:X}  (DcStop={4})' -f $p.DcStops, [uint32]$p.DcCommandAfterStop, [uint32]$p.DcCommandAfterStart, [uint32]$p.DsiStatusAfterDcStart, $(if ($null -ne $p.DcStop) { $p.DcStop } else { 'default 1' })) }
  if ($p.GuardTripped) { Sum ('boot-loop guard tripped {0} time(s): the driver refused to start after crashes' -f $p.GuardTripped) }
}

function Show-Report {
  Show-Adapters
  Log ''
  Log '--- monitors'
  foreach ($m in @(Get-CimInstance -Namespace root\wmi WmiMonitorBasicDisplayParams -ErrorAction SilentlyContinue)) {
    Sum ('monitor: {0}  {1} x {2} cm  active={3}' -f $m.InstanceName, $m.MaxHorizontalImageSize, $m.MaxVerticalImageSize, $m.Active)
  }
  foreach ($m in @(Get-CimInstance -Namespace root\wmi WmiMonitorID -ErrorAction SilentlyContinue)) {
    $n = ($m.UserFriendlyName | Where-Object { $_ -ne 0 } | ForEach-Object { [char]$_ }) -join ''
    Log ('  id: {0} name="{1}"' -f $m.InstanceName, $n)
  }
  Log ''
  Log '--- mode, rotation, DPI'
  try {
    Add-Type -Namespace SwoaDisp -Name Native -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct DEVMODE {
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
  public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
  public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
  public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
  [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
  public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
  public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
}
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
[DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr h);
[DllImport("gdi32.dll")] public static extern int GetDeviceCaps(IntPtr hdc, int i);
'@
    $dm = New-Object SwoaDisp.Native+DEVMODE
    $dm.dmSize = [System.Runtime.InteropServices.Marshal]::SizeOf($dm)
    Add-Type -AssemblyName System.Windows.Forms
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    Sum ('screen: {0}x{1}' -f $b.Width, $b.Height)
    if (-not [SwoaDisp.Native]::EnumDisplaySettingsW($null, -1, [ref]$dm)) { Log '  EnumDisplaySettings failed' }
    else {
      Sum ('current: {0}x{1} {2} Hz {3} bpp  orientation DMDO_{4}' -f $dm.dmPelsWidth, $dm.dmPelsHeight, $dm.dmDisplayFrequency, $dm.dmBitsPerPel, @('DEFAULT', '90', '180', '270')[$dm.dmDisplayOrientation])
    }
    $dc = [SwoaDisp.Native]::GetDC([IntPtr]::Zero)
    Sum ('DPI (this process): {0}  (96 = 100%)' -f [SwoaDisp.Native]::GetDeviceCaps($dc, 88))
  } catch { Log "  mode query failed: $_" }
  $lp = Get-ItemProperty 'HKCU:\Control Panel\Desktop' -Name LogPixels -ErrorAction SilentlyContinue
  if ($lp) { Sum ('scaling (LogPixels): {0}' -f $lp.LogPixels) }
  Log ''
  Log '--- powercfg /a'
  (& $Powercfg /a 2>&1) | ForEach-Object { Log "  $_" }
  $pa = (& $Powercfg /a 2>&1) | Out-String
  if ($pa -match '(?i)graphics|\u30B0\u30E9\u30D5\u30A3\u30C3\u30AF') { Sum 'powercfg /a mentions graphics (see the log)' }
  Log ''
  Log '--- dxdiag (about 20-40 s)'
  $tmp = Join-Path $env:TEMP 'swoa-dxdiag.txt'
  Remove-Item $tmp -ErrorAction SilentlyContinue
  try {
    $p = Start-Process -FilePath $Dxdiag -ArgumentList "/t `"$tmp`"" -PassThru -WindowStyle Hidden
    if (-not $p.WaitForExit(90000)) { Log '  dxdiag did not finish in 90 s'; try { $p.Kill() } catch { } }
  } catch { Log "  dxdiag failed: $_" }
  if (Test-Path $tmp) {
    $want = 'Card name|Manufacturer|Chip type|Driver Name|Driver File Version|Driver Model|Feature Levels|Current Mode|Native Mode|Monitor Name|DirectDraw Acceleration|Direct3D Acceleration|Hybrid Graphics'
    Get-Content $tmp | Where-Object { $_ -match "^\s*($want):" } | Select-Object -First 30 | ForEach-Object { Log "  $($_.Trim())" }
    $fl = Get-Content $tmp | Where-Object { $_ -match '^\s*Feature Levels:' } | Select-Object -First 1
    if ($fl) { Sum ("dxdiag: $($fl.Trim())") }
  }
  Log ''
  Show-Trace
}

# 1.0.0/1.0.1 root devices and every switchdisplay driver package.
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
function Get-OurDevices {
  @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains $HwId })
}

$SetupApi = @'
using System;
using System.Runtime.InteropServices;
public static class SwoaSetup {
  [StructLayout(LayoutKind.Sequential)]
  public struct SP_DEVINFO_DATA { public int cbSize; public Guid ClassGuid; public int DevInst; public IntPtr Reserved; }
  [DllImport("setupapi.dll", SetLastError = true)] static extern IntPtr SetupDiCreateDeviceInfoList(ref Guid g, IntPtr hwnd);
  [DllImport("setupapi.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  static extern bool SetupDiCreateDeviceInfoW(IntPtr set, string name, ref Guid g, string desc, IntPtr hwnd, int flags, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true)]
  static extern bool SetupDiSetDeviceRegistryPropertyW(IntPtr set, ref SP_DEVINFO_DATA d, int prop, byte[] buf, int size);
  [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiCallClassInstaller(int dif, IntPtr set, ref SP_DEVINFO_DATA d);
  [DllImport("setupapi.dll", SetLastError = true)] static extern bool SetupDiDestroyDeviceInfoList(IntPtr set);
  [DllImport("newdev.dll", SetLastError = true, CharSet = CharSet.Unicode)]
  public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr hwnd, string hwid, string inf, int flags, out bool reboot);
  public static int CreateRootDevice(string hwid) {
    Guid display = new Guid("4d36e968-e325-11ce-bfc1-08002be10318");
    IntPtr set = SetupDiCreateDeviceInfoList(ref display, IntPtr.Zero);
    if (set == new IntPtr(-1)) return Marshal.GetLastWin32Error();
    try {
      SP_DEVINFO_DATA d = new SP_DEVINFO_DATA(); d.cbSize = Marshal.SizeOf(d);
      // DICD_GENERATE_ID = 1 -> ROOT\SWOA_DISPLAY\0000
      if (!SetupDiCreateDeviceInfoW(set, "SWOA_DISPLAY", ref display, null, IntPtr.Zero, 1, ref d)) return Marshal.GetLastWin32Error();
      byte[] ids = System.Text.Encoding.Unicode.GetBytes(hwid + "\0\0");
      if (!SetupDiSetDeviceRegistryPropertyW(set, ref d, 1 /* SPDRP_HARDWAREID */, ids, ids.Length)) return Marshal.GetLastWin32Error();
      if (!SetupDiCallClassInstaller(0x19 /* DIF_REGISTERDEVICE */, set, ref d)) return Marshal.GetLastWin32Error();
      return 0;
    } finally { SetupDiDestroyDeviceInfoList(set); }
  }
}
'@

switch ($Mode) {
  'Report' { Show-Report }
  'Status' { Show-Adapters; Log ''; Show-Trace }
  'Install' {
    if (-not [Environment]::Is64BitProcess) { Sum 'This PowerShell is not 64-bit (ARM64); run Install.cmd from Explorer.'; break }
    $inf = Join-Path $Here 'switchdisplay.inf'
    if (-not (Test-Path $inf)) { Sum "missing: $inf"; break }
    $acpi = @(Get-ChildItem 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\SWOA0E01' -ErrorAction SilentlyContinue)
    # Without R408 the package is only staged; the next R408 boot installs it
    # on DISP (the old packages are deleted first, so 1.0.3 is the only match).
    if ($acpi.Count -eq 0) { Sum 'ACPI\SWOA0E01 has never been seen (boot UEFI R408 once first). Nothing installed.'; break }
    Remove-Old
    Log "pnputil /add-driver $inf /install"
    (& $Pnputil /add-driver $inf /install 2>&1) | ForEach-Object { Log "  $_" }
    Sum "pnputil exit=$LASTEXITCODE"
    Start-Sleep -Seconds 5
    Show-Adapters
    Log ''
    Show-Trace
    Sum 'Restart Windows, then run Status.cmd (the driver takes the screen over at boot).'
  }
  'RotateTest' {
    # The same struct and calls as SwitchCenter (AutoScreen.cs Display class),
    # in C#: the PowerShell DEVMODE of the first RotateTest failed even on a PC.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SwoaRot {
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
  static bool Read(int mode, out DevMode dm) { dm = new DevMode { Size = (ushort)Marshal.SizeOf(typeof(DevMode)) }; return EnumDisplaySettings(null, mode, ref dm); }
  public static string Mode(int i) { DevMode d; if (!Read(i, out d)) return null; return d.PelsWidth + "x" + d.PelsHeight + " " + d.BitsPerPel + " bpp " + d.DisplayFrequency + " Hz orientation " + d.DisplayOrientation; }
  public static int Orientation() { DevMode d; return Read(-1, out d) ? (int)d.DisplayOrientation : -1; }
  // ChangeDisplaySettingsEx result, or 100 = the current settings could not be read
  public static int Set(int o) {
    DevMode dm; if (!Read(-1, out dm)) return 100;
    if ((dm.DisplayOrientation + o) % 2 == 1) { uint w = dm.PelsWidth; dm.PelsWidth = dm.PelsHeight; dm.PelsHeight = w; }
    dm.DisplayOrientation = (uint)o;
    dm.Fields = 0x80 | 0x80000 | 0x100000;
    return ChangeDisplaySettingsEx(null, ref dm, IntPtr.Zero, 1, IntPtr.Zero);
  }
}
'@
    $codes = @{ 0 = 'SUCCESSFUL'; 1 = 'RESTART'; -1 = 'FAILED'; -2 = 'BADMODE'; -3 = 'NOTUPDATED'; -4 = 'BADFLAGS'; -5 = 'BADPARAM'; -6 = 'BADDUALVIEW'; 100 = 'CANNOT READ CURRENT' }
    Log '--- modes (EnumDisplaySettings)'
    for ($i = 0; $i -lt 40; $i++) { $m = [SwoaRot]::Mode($i); if (-not $m) { break }; Log "  ${i}: $m" }
    $c = [SwoaRot]::Mode(-1)
    Sum ("current: " + $(if ($c) { $c } else { 'EnumDisplaySettings(current) failed' }))
    $start = [SwoaRot]::Orientation()
    foreach ($o in @(1, 2, 3, [Math]::Max($start, 0))) {
      $r = [SwoaRot]::Set($o)
      Sum ('rotate to {0}: {1} {2} -> now {3}' -f $o, $r, $codes[$r], [SwoaRot]::Mode(-1))
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

