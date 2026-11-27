<#
.SYNOPSIS
  Sleep (modern standby, S0 Low Power Idle) diagnosis on the Switch's Windows
  (Administrator). Stage a0: read only. Nothing is installed and no power
  setting is changed.

  -Mode Report : sleep states (powercfg /a), power button action, display
                 driver, processor devices, CPU idle (C1/C2/C3) for 10 s,
                 battery, recent Kernel-Power events
  -Mode Timer  : turns the screen off for 2 min and compares three clocks
                 every 10 s: the stopwatch (QPC), the tick count (interrupt
                 time) and the Windows clock. Shows whether timers run late.
  -Mode Drain  : turns the screen off for -Minutes (default 10), samples the
                 CPU idle counters and the battery every 30 s, then reports how
                 much the battery dropped. Run it on battery (no charger).

  Log: C:\SwitchWOA-Sleep.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Report', 'Drain', 'Timer', 'Power', 'SendLogs')][string]$Mode,
      [int]$Minutes = 10,
      [string]$Label = '')

$ErrorActionPreference = 'Continue'
# Windows PowerShell on ARM64 can be x86: reach System32 via Sysnative.
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'powercfg.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Powercfg = Join-Path $Sys 'powercfg.exe'
$Log = 'C:\SwitchWOA-Sleep.txt'
$Summary = New-Object System.Collections.Generic.List[string]
function Log([string]$s) { Write-Host $s; Add-Content -Path $Log -Value $s -Encoding UTF8 }
function Sum([string]$s) { $Summary.Add($s); Log $s }
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run this from an elevated (Administrator) prompt.'; exit 1 }
# A touch on the console window starts QuickEdit selection, which pauses the
# script until Esc. Turn QuickEdit off for this window only (not saved).
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
Log ''
Log "=== Sleep $Mode  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

# ---------------------------------------------------------------- helpers
function Get-PowerIndex([string]$Sub, [string]$Setting) {
  # The last two 0x values of "powercfg /qh" are the AC and DC index
  # (language independent).
  $out = (& $Powercfg /qh SCHEME_CURRENT $Sub $Setting 2>&1) | Out-String
  $hex = [regex]::Matches($out, '0x[0-9A-Fa-f]{8}') | ForEach-Object { $_.Value }
  if ($hex.Count -ge 2) { return @([Convert]::ToUInt32($hex[-2].Substring(2), 16), [Convert]::ToUInt32($hex[-1].Substring(2), 16)) }
  return $null
}

# Processor Information can be missing; Processor (PerfOS) has the same
# C1/C2/C3 fields. The class that answered is kept in $script:IdleClass.
$script:IdleClass = $null
function Get-IdleRaw {
  $r = @{}
  foreach ($cls in @('Win32_PerfRawData_Counters_ProcessorInformation', 'Win32_PerfRawData_PerfOS_Processor')) {
    foreach ($p in @(Get-CimInstance $cls -ErrorAction SilentlyContinue)) {
      if ($p.Name -like '*_Total') { continue }
      $r[$p.Name] = $p
    }
    if ($r.Count -gt 0) {
      if ($script:IdleClass -ne $cls) { $script:IdleClass = $cls; Log "processor counters: $cls" }
      break
    }
  }
  if ($r.Count -eq 0 -and $script:IdleClass -ne 'none') { $script:IdleClass = 'none'; Log 'processor counters: none answered' }
  return $r
}

function Show-IdleDelta($a, $b, [string]$Tag, [switch]$Quiet) {
  # % times are PERF_100NSEC_TIMER: delta / delta(Timestamp_Sys100NS).
  $lines = @()
  $sumC1 = 0.0; $sumC2 = 0.0; $sumC3 = 0.0; $sumIdle = 0.0; $n = 0
  foreach ($k in ($b.Keys | Sort-Object)) {
    if (-not $a.ContainsKey($k)) { continue }
    $x = $a[$k]; $y = $b[$k]
    $dt = [double]($y.Timestamp_Sys100NS - $x.Timestamp_Sys100NS)
    if ($dt -le 0) { continue }
    $c1 = 100.0 * ($y.PercentC1Time - $x.PercentC1Time) / $dt
    $c2 = 100.0 * ($y.PercentC2Time - $x.PercentC2Time) / $dt
    $c3 = 100.0 * ($y.PercentC3Time - $x.PercentC3Time) / $dt
    $idle = 100.0 * ($y.PercentIdleTime - $x.PercentIdleTime) / $dt
    $sec = $dt / 1e7
    $t1 = ($y.C1TransitionsPersec - $x.C1TransitionsPersec) / $sec
    $t2 = ($y.C2TransitionsPersec - $x.C2TransitionsPersec) / $sec
    $t3 = ($y.C3TransitionsPersec - $x.C3TransitionsPersec) / $sec
    $lines += ('  cpu {0,-4} idle {1,5:N1}%  C1 {2,5:N1}% ({3,6:N0}/s)  C2 {4,5:N1}% ({5,5:N0}/s)  C3 {6,5:N1}% ({7,5:N0}/s)' -f $k, $idle, $c1, $t1, $c2, $t2, $c3, $t3)
    $sumC1 += $c1; $sumC2 += $c2; $sumC3 += $c3; $sumIdle += $idle; $n++
  }
  if (-not $Quiet) { foreach ($l in $lines) { Log $l } }
  if ($n -gt 0) {
    $line = ('{0}: idle {1:N1}%  C1 {2:N1}%  C2 {3:N1}%  C3 {4:N1}%  ({5} cores)' -f $Tag, ($sumIdle / $n), ($sumC1 / $n), ($sumC2 / $n), ($sumC3 / $n), $n)
    if ($Quiet) { Log $line } else { Sum $line }
  } else {
    Sum "${Tag}: no processor counters"
  }
}

# REG_DWORD comes back unsigned (UInt32): turn the driver's signed values back into signed numbers.
function To-Signed($v) { $n = [long]$v; if ($n -ge 2147483648) { $n -= 4294967296 }; return [int]$n }

function Get-Battery {
  $b = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue) | Select-Object -First 1
  $t = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\switchbattery\Parameters' -ErrorAction SilentlyContinue
  $o = [ordered]@{ Percent = $null; Status = $null; Mv = $null; Ma = $null; Mw = $null; Qh = $null; RepCap = $null; Time = Get-Date }
  if ($b) { $o.Percent = [int]$b.EstimatedChargeRemaining; $o.Status = [int]$b.BatteryStatus }
  if ($t -and $t.LastVoltage) { $o.Mv = [int]$t.LastVoltage }
  # switchbattery 1.0.0.9+: gauge AvgCurrent (signed, - = discharging) and power, every 10 s
  if ($t -and ($t.PSObject.Properties.Name -contains 'LastAvgCurrentMa')) { $o.Ma = To-Signed $t.LastAvgCurrentMa; $o.Mw = To-Signed $t.LastPowerMw }
  # switchbattery 1.0.0.10+: coulomb counter (mAh) and remaining capacity, for averages across modern standby
  if ($t -and ($t.PSObject.Properties.Name -contains 'LastQh')) { $o.Qh = To-Signed $t.LastQh; $o.RepCap = [int]$t.LastRepCapMah }
  return $o
}

# ---------------------------------------------------------------- report
function Show-Report {
  $os = Get-CimInstance Win32_OperatingSystem
  $bios = Get-CimInstance Win32_BIOS -ErrorAction SilentlyContinue
  $ubr = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue).UBR
  Sum ('Windows {0}.{1}  ({2})  firmware: {3}' -f $os.BuildNumber, $ubr, $os.Caption, ($bios.SMBIOSBIOSVersion -join ' '))

  Log ''
  Log '--- powercfg /a'
  (& $Powercfg /a 2>&1) | ForEach-Object { Log "  $_" }

  $pw = Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Power' -ErrorAction SilentlyContinue
  $vals = @()
  foreach ($n in @('CsEnabled', 'PlatformAoAcOverride', 'PlatformRole', 'HibernateEnabled', 'HiberbootEnabled')) {
    $v = $pw.$n
    if ($null -eq $v) { $vals += "$n=-" } else { $vals += "$n=$v" }
  }
  Sum ('Power key: ' + ($vals -join ' '))

  Log ''
  Log '--- buttons and processor idle (AC / DC)'
  $names = @{ 0 = 'nothing'; 1 = 'sleep'; 2 = 'hibernate'; 3 = 'shut down'; 4 = 'display off' }
  foreach ($s in @(@('SUB_BUTTONS', 'PBUTTONACTION', 'power button'), @('SUB_BUTTONS', 'SBUTTONACTION', 'sleep button'))) {
    $ix = Get-PowerIndex $s[0] $s[1]
    if ($ix) { Sum ('{0}: AC {1} ({2}) / DC {3} ({4})' -f $s[2], $ix[0], $names[[int]$ix[0]], $ix[1], $names[[int]$ix[1]]) }
    else { Sum ('{0}: (not shown)' -f $s[2]) }
  }
  $ix = Get-PowerIndex 'SUB_PROCESSOR' 'IDLEDISABLE'
  if ($ix) { Sum ('processor idle disable: AC {0} / DC {1}  (0 = idle allowed)' -f $ix[0], $ix[1]) }
  $ix = Get-PowerIndex 'SUB_VIDEO' 'VIDEOIDLE'
  if ($ix) { Sum ('display off after: AC {0} s / DC {1} s' -f $ix[0], $ix[1]) }
  $ix = Get-PowerIndex 'SUB_SLEEP' 'STANDBYIDLE'
  if ($ix) { Sum ('sleep after: AC {0} s / DC {1} s' -f $ix[0], $ix[1]) }

  Log ''
  Log '--- display adapter'
  foreach ($v in @(Get-CimInstance Win32_VideoController -ErrorAction SilentlyContinue)) {
    Sum ('display: {0}  driver {1}  {2}x{3}' -f $v.Name, $v.DriverVersion, $v.CurrentHorizontalResolution, $v.CurrentVerticalResolution)
  }

  Log ''
  Log '--- processor devices (ACPI0007) and power devices (ACPI0010, PEP)'
  $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
           Where-Object { $_.PNPClass -eq 'Processor' -or $_.PNPDeviceID -like 'ACPI\ACPI0010*' -or $_.PNPDeviceID -like '*PEP*' })
  foreach ($d in $dev) {
    Log ('  {0}  status={1} problem={2} service={3}' -f $d.PNPDeviceID, $d.Status, $d.ConfigManagerErrorCode, $d.Service)
  }
  $cpu = @($dev | Where-Object { $_.PNPClass -eq 'Processor' })
  Sum ('processors: {0} found, services: {1}, other power devices: {2}' -f $cpu.Count,
       (($cpu | ForEach-Object { $_.Service } | Sort-Object -Unique) -join ','), ($dev.Count - $cpu.Count))

  Log ''
  Log '--- CPU idle states, 10 s (do not touch the Switch)'
  $a = Get-IdleRaw
  Start-Sleep -Seconds 10
  $b = Get-IdleRaw
  Show-IdleDelta $a $b 'CPU idle 10 s'

  Log ''
  Log '--- battery'
  $bt = Get-Battery
  Sum ('battery: {0}%  status {1} (1 = on battery, 2 = charger)  {2} mV' -f $bt.Percent, $bt.Status, $bt.Mv)
  if ($null -ne $bt.Ma) { Sum ('battery current now (screen on): {0} mA  {1} mW  (- = discharging)' -f $bt.Ma, $bt.Mw) }
  else { Sum 'battery current: n/a (needs switchbattery 1.0.0.9)' }

  Log ''
  Log '--- recent Kernel-Power events (42 sleep, 506/507 standby in/out, 105 AC change)'
  $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Power' } -MaxEvents 8 -ErrorAction SilentlyContinue)
  foreach ($e in $ev) {
    $msg = ($e.Message -replace '\s+', ' ')
    if ($msg.Length -gt 90) { $msg = $msg.Substring(0, 90) }
    Log ('  {0:MM-dd HH:mm} id {1}: {2}' -f $e.TimeCreated, $e.Id, $msg)
  }
  Sum ('Kernel-Power events: ' + ((@($ev | ForEach-Object { $_.Id }) | Select-Object -First 8) -join ' '))
}

# ---------------------------------------------------------------- drain
function Run-Drain {
  $bt0 = Get-Battery
  if ($bt0.Status -eq 2) { Log 'NOTE: the charger is connected. The battery drop will not mean much. Unplug it and run again.' }
  Sum ('start: {0}%  {1} mV' -f $bt0.Percent, $bt0.Mv)
  Log "The screen turns off now for $Minutes min. Do not touch the Switch. If the screen stays off at the end, press the power button once (touch does not wake it)."
  Start-Sleep -Seconds 3
  $sw = [Diagnostics.Stopwatch]::StartNew()
  Add-Type -Namespace SwoaSleep -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
'@
  Log ('Add-Type: {0:N1} s' -f $sw.Elapsed.TotalSeconds)
  # HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2 = off
  [void][SwoaSleep.Native]::PostMessage([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr]2)
  $first = Get-IdleRaw
  $prev = $first
  # A fixed number of 30 s steps timed by the stopwatch, not by the clock:
  # the clock can jump (time sync, RTC offset) and must not stretch the test.
  $steps = [Math]::Max(1, [int]($Minutes * 2))
  $sw.Restart()
  $tStart = Get-Date
  $last = 0.0; $slowest = 0.0; $mws = @()
  for ($i = 1; $i -le $steps; $i++) {
    # Modern standby freezes this script and the stopwatch (QPC) does not count
    # during it: stop when the clock says the test time has passed.
    if (((Get-Date) - $tStart).TotalMinutes -ge $Minutes) { Log ('clock: {0:N1} min passed, ending the test' -f ((Get-Date) - $tStart).TotalMinutes); break }
    $wait = 30.0 * $i - $sw.Elapsed.TotalSeconds
    $wallLeft = $Minutes * 60.0 - ((Get-Date) - $tStart).TotalSeconds
    if ($wait -gt $wallLeft) { $wait = $wallLeft }
    if ($wait -gt 0) { Start-Sleep -Milliseconds ([int]($wait * 1000)) }
    $t0 = $sw.Elapsed.TotalSeconds
    if (($t0 - $last) -gt $slowest) { $slowest = $t0 - $last }
    $last = $t0
    $now = Get-IdleRaw
    $bt = Get-Battery
    Log ('{0}/{1}  {2:N0} s  clock {3:HH:mm:ss}  battery {4}%  {5} mV  {6} mA  {7} mW  (read {8:N1} s)' -f $i, $steps, $sw.Elapsed.TotalSeconds, (Get-Date), $bt.Percent, $bt.Mv, $bt.Ma, $bt.Mw, ($sw.Elapsed.TotalSeconds - $t0))
    if ($null -ne $bt.Mw -and $i -gt 1) { $mws += $bt.Mw }   # the first 30 s still holds screen-on samples
    Show-IdleDelta $prev $now '  30 s' -Quiet
    $prev = $now
  }
  # Turn the screen back on so the result can be read: SC_MONITORPOWER -1,
  # and a 1-pixel mouse move (input is what reliably wakes the display).
  [void][SwoaSleep.Native]::PostMessage([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr](-1))
  [SwoaSleep.Native]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero)
  [SwoaSleep.Native]::mouse_event(1, -1, 0, 0, [UIntPtr]::Zero)
  Sum ('longest 30 s step: {0:N0} s  (much more than 30 = timers late while the screen is off)' -f $slowest)
  Sum ('test time: {0:N1} min (stopwatch), {1:N1} min (clock)' -f ($sw.Elapsed.TotalMinutes + 0.0), ((Get-Date) - $bt0.Time).TotalMinutes)
  # The driver reads the gauge every 10 s: wait for a reading taken after the test.
  Log 'waiting 15 s for a fresh gauge reading...'
  Start-Sleep -Seconds 15
  $bt1 = Get-Battery
  Log ''
  Show-IdleDelta $first $prev "CPU idle, screen off $Minutes min"
  # The battery rate uses the clock: on the Switch the stopwatch (QPC) fell
  # behind real time while the screen was off (photos matched the clock).
  $min = ((Get-Date) - $bt0.Time).TotalMinutes
  Sum ('end: {0}%  {1} mV' -f $bt1.Percent, $bt1.Mv)
  if ($null -ne $bt0.Percent -and $null -ne $bt1.Percent -and $min -gt 0) {
    $d = $bt0.Percent - $bt1.Percent
    Sum ('drop: {0}% in {1:N1} min  = about {2:N1}% per hour' -f $d, $min, ($d * 60.0 / $min))
  }
  if ($null -ne $bt0.Mv -and $null -ne $bt1.Mv) { Sum ('voltage drop: {0} mV' -f ($bt0.Mv - $bt1.Mv)) }
  # Modern standby: Windows pauses this script while it is in standby, so the 30 s samples
  # stop. The gauge's coulomb counter (QH) still counts: use it for the whole test.
  $evs = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-Kernel-Power'; Id = 506, 507; StartTime = $bt0.Time } -ErrorAction SilentlyContinue | Sort-Object TimeCreated)
  $inSb = $null; $sbSec = 0.0; $sbN = 0
  foreach ($e in $evs) {
    if ($e.Id -eq 506) { $inSb = $e.TimeCreated; $sbN++ }
    elseif ($inSb) { $sbSec += ($e.TimeCreated - $inSb).TotalSeconds; $inSb = $null }
  }
  Sum ('modern standby: {0} time(s), {1:N1} min in standby (Kernel-Power 506/507)' -f $sbN, ($sbSec / 60))
  if ($null -ne $bt0.Qh -and $null -ne $bt1.Qh) {
    $dq = $bt1.Qh - $bt0.Qh
    if ($dq -gt 32767) { $dq -= 65536 } elseif ($dq -lt -32768) { $dq += 65536 }
    $h = ($bt1.Time - $bt0.Time).TotalHours
    $mv = ($bt0.Mv + $bt1.Mv) / 2.0
    if ($h -gt 0) {
      $ma = $dq / $h
      Sum ('coulomb counter: {0} mAh in {1:N1} min = average {2:N0} mA, about {3:N0} mW at {4:N0} mV  (RepCap {5} -> {6} mAh)' -f $dq, ($h * 60), $ma, ($ma * $mv / 1000), $mv, $bt0.RepCap, $bt1.RepCap)
      if ($ma -lt 0) { Sum ('  a full battery (about 16 Wh) would last about {0:N1} h at this rate' -f (16000.0 / (-$ma * $mv / 1000))) }
    }
  } else { Sum 'coulomb counter: n/a (needs switchbattery 1.0.0.10)' }
  if ($mws.Count -gt 0) {
    $avg = ($mws | Measure-Object -Average).Average
    $mn = ($mws | Measure-Object -Minimum).Minimum; $mx = ($mws | Measure-Object -Maximum).Maximum
    Sum ('power, screen off: average {0:N0} mW  (from {1} to {2} mW, {3} samples, - = discharging)' -f $avg, $mn, $mx, $mws.Count)
    if ($avg -lt 0) { Sum ('  a full battery (about 16 Wh) would last about {0:N1} h at this rate' -f (16000.0 / -$avg)) }
  } else { Sum 'power: n/a (needs switchbattery 1.0.0.9)' }
}

# ---------------------------------------------------------------- power (screen on)
# Screen stays on, so Windows does not enter modern standby and the script keeps running.
# Average power from the gauge: AvgCurrent every 10 s, and the coulomb counter over the run.
function Run-Power {
  $bt0 = Get-Battery
  if ($bt0.Status -eq 2) { Sum 'NOTE: the charger is connected: the numbers are not the battery drain. Unplug it and run again.' }
  if ($null -eq $bt0.Ma) { Sum 'power: n/a (needs switchbattery 1.0.0.10)'; return }
  if (-not $Label) {
    # Name the run after the Switch devices that are turned off (Devices.ps1).
    $ids = [ordered]@{ Wifi = 'ACPI\SWOA0D01'; Bt = 'ACPI\SWOA0B01'; Gpu = 'ACPI\SWOA0F01'; Audio = 'ACPI\SWOA0C01'; Sensor = 'ACPI\SWOA0901' }
    $off = @()
    foreach ($k in $ids.Keys) {
      # Win32_PnPEntity: ConfigManagerErrorCode 22 = disabled (Get-PnpDevice -PresentOnly left disabled devices out)
      $d = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like ($ids[$k] + '\*') }) | Select-Object -First 1
      if ($d -and $d.ConfigManagerErrorCode -eq 22) { $off += $k }
    }
    $Label = if ($off.Count) { 'off: ' + ($off -join ',') } else { 'all devices on' }
  }
  Sum ('Power test [{0}]  {1} min, screen on: do not touch the Switch' -f $Label, $Minutes)
  $mws = @()
  $steps = [Math]::Max(1, $Minutes * 6)
  for ($i = 1; $i -le $steps; $i++) {
    Start-Sleep -Seconds 10
    $bt = Get-Battery
    $mws += $bt.Mw
    Log ('{0}/{1}  {2:HH:mm:ss}  {3} mV  {4} mA  {5} mW' -f $i, $steps, (Get-Date), $bt.Mv, $bt.Ma, $bt.Mw)
  }
  $bt1 = Get-Battery
  $avg = ($mws | Measure-Object -Average).Average
  Sum ('{0}: average {1:N0} mW from AvgCurrent ({2} samples, from {3} to {4} mW)' -f $Label, $avg, $mws.Count, ($mws | Measure-Object -Minimum).Minimum, ($mws | Measure-Object -Maximum).Maximum)
  if ($null -ne $bt0.Qh -and $null -ne $bt1.Qh) {
    $dq = $bt1.Qh - $bt0.Qh
    if ($dq -gt 32767) { $dq -= 65536 } elseif ($dq -lt -32768) { $dq += 65536 }
    $h = ($bt1.Time - $bt0.Time).TotalHours
    $mv = ($bt0.Mv + $bt1.Mv) / 2.0
    Sum ('{0}: coulomb counter {1} mAh in {2:N1} min = about {3:N0} mW (1 mAh steps: rough for short runs)' -f $Label, $dq, ($h * 60), ($dq / $h * $mv / 1000))
  }
}

# ---------------------------------------------------------------- timer
function Run-Timer {
  Sum ('QPC frequency: {0} Hz' -f [Diagnostics.Stopwatch]::Frequency)
  $bcd = (& (Join-Path $Sys 'bcdedit.exe') /enum '{current}' 2>&1) | Where-Object { $_ -match 'platform|dynamictick|tscsync|hypervisor' }
  if ($bcd) { foreach ($l in $bcd) { Sum ('bcd: ' + ($l -replace '\s+', ' ')) } } else { Sum 'bcd: no timer options set' }
  Add-Type -Namespace SwoaTimer -Name Native -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern void mouse_event(uint f, int dx, int dy, uint d, UIntPtr e);
[DllImport("kernel32.dll")] public static extern ulong GetTickCount64();
'@
  Log 'The screen turns off now for 2 min. Do not touch the Switch. If the screen stays off at the end, press the power button once.'
  Start-Sleep -Seconds 3
  [void][SwoaTimer.Native]::PostMessage([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr]2)
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $tc0 = [SwoaTimer.Native]::GetTickCount64(); $c0 = Get-Date
  $ps = 0.0; $pt = $tc0; $pc = $c0; $worst = 0.0
  for ($i = 1; $i -le 12; $i++) {
    Start-Sleep -Seconds 10
    $s1 = $sw.Elapsed.TotalSeconds; $t1 = [SwoaTimer.Native]::GetTickCount64(); $c1 = Get-Date
    $dc = ($c1 - $pc).TotalSeconds
    if ($dc -gt $worst) { $worst = $dc }
    Log ('{0,2}/12  asked 10 s:  stopwatch {1,6:N1} s  tick {2,6:N1} s  clock {3,6:N1} s' -f $i, ($s1 - $ps), (($t1 - $pt) / 1000.0), $dc)
    $ps = $s1; $pt = $t1; $pc = $c1
  }
  [void][SwoaTimer.Native]::PostMessage([IntPtr]0xFFFF, 0x0112, [IntPtr]0xF170, [IntPtr](-1))
  [SwoaTimer.Native]::mouse_event(1, 1, 0, 0, [UIntPtr]::Zero)
  [SwoaTimer.Native]::mouse_event(1, -1, 0, 0, [UIntPtr]::Zero)
  Sum ('2 min asked:  stopwatch {0:N0} s  tick {1:N0} s  clock {2:N0} s' -f $sw.Elapsed.TotalSeconds, (([SwoaTimer.Native]::GetTickCount64() - $tc0) / 1000.0), ((Get-Date) - $c0).TotalSeconds)
  Sum ('longest 10 s sleep by the clock: {0:N0} s  (about 10 = timers on time)' -f $worst)
}

# Claude 2026-11-25: copy the log to the PC share named in share.txt (next to this script).
function Send-Log {
  $sf = Join-Path $PSScriptRoot 'share.txt'
  if (-not (Test-Path $sf)) { Write-Host 'No share.txt: copy C:\SwitchWOA-Sleep.txt by hand.'; return }
  $share = (Get-Content $sf -TotalCount 1).Trim()
  if (-not $share) { return }
  try {
    $dir = Join-Path $share 'logs'
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir -ErrorAction Stop | Out-Null }
    Copy-Item $Log (Join-Path $dir 'SwitchWOA-Sleep.txt') -Force -ErrorAction Stop
    Set-Content -Path (Join-Path $dir 'LAST.txt') -Value ($Mode + '  ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding ascii
    Write-Host ('Log sent to ' + $dir) -ForegroundColor Green
  } catch {
    Write-Host ('Could not send the log to ' + $share + ': ' + $_.Exception.Message) -ForegroundColor Yellow
  }
}

if ($Mode -eq 'SendLogs') { Send-Log; return }

switch ($Mode) {
  'Report' { Show-Report }
  'Drain'  { Run-Drain }
  'Timer'  { Run-Timer }
  'Power'  { Run-Power }
}

Write-Host ''
Write-Host '================ SUMMARY (please take a photo) ================'
foreach ($s in $Summary) { Write-Host $s }
Write-Host "Full log: $Log"
Send-Log
