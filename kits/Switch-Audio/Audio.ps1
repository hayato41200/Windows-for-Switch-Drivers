<#
.SYNOPSIS
  Audio driver (speaker), stage 1, on the Switch's Windows (Administrator). Needs UEFI R402
  (DSDT AUD0; APE clocks on, codec set up, I2S1 running).

  -Mode Install : pnputil /add-driver switchaudio.inf /install, then report
  -Mode Status  : report AUD0 (ACPI\SWOA0C01), the driver trace and the Windows audio endpoints
  -Mode Remove  : remove the driver package
  -Mode Tone    : (1.0.0.2+) the driver plays one second of 1 kHz from its own buffer, without the
                  Windows audio engine (TestTone=1, then the device restarts), then report
  -Mode Check   : plays a Windows sound through the audio engine, lists audio errors from the
                  event logs (last hour), then report
  -Mode TvTone  : (1.1.1+) the driver plays one second of 1 kHz to the TV from its own buffer
                  (TvTestTone=1, picked up by the driver within a second), then report
  -Mode TvReset : (1.1.0+) clears TvPending (set when a start hung in the TV part), restarts the device
  -Mode TvOff   : (1.1.0+) TvAudioOff = 1: the driver leaves the HDA alone from the next boot (speaker only)
  -Mode TvOn    : (1.1.0+) TvAudioOff = 0: TV audio again from the next boot

  Log: C:\SwitchWOA-Audio.txt
#>
param([Parameter(Mandatory = $true)][ValidateSet('Install', 'Status', 'Remove', 'Tone', 'Check', 'TvTone', 'TvReset', 'TvOff', 'TvOn')][string]$Mode)

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
    $props = @($p.PSObject.Properties | Where-Object { $_.Name -notmatch '^PS' })
    # 1.0.0.2: register snapshots, one line per moment (Start, Run0, Run100, Stop, ToneBefore, Tone).
    $snap = @{}
    $rest = @()
    # 1.0.0.4: CallLog digits: 1 NewStream 2 SetFormat 3 AllocateAudioBuffer 4 FreeAudioBuffer
    # 5 GetHWLatency 6 GetPositionRegister 7 GetClockRegister 8 GetPosition 9 SetState A freed.
    foreach ($q in $props) {
      if ($q.Name -match '^Tv') { continue }
      if ($q.Name -match '^(ToneBefore|Tone|Run100|Run0|Start|Stop)(Adma\w*|Xbar|I2s\w*)$') {
        if (-not $snap[$Matches[1]]) { $snap[$Matches[1]] = @() }
        $snap[$Matches[1]] += ('{0}=0x{1:X}' -f $Matches[2], $q.Value)
      } else { $rest += ('{0}=0x{1:X}' -f $q.Name, $q.Value) }
    }
    Log ('driver trace: ' + ($rest -join ' '))
    foreach ($g in 'Start', 'Run0', 'Run100', 'Stop', 'ToneBefore', 'Tone') {
      if ($snap[$g]) { Log ("  {0}: {1}" -f $g, ($snap[$g] -join ' ')) }
    }
    # Stage: 1 started, 2 hardware ready, 3 endpoints registered; 0x11 = APE clocks off (UEFI older than
    # R402), 0x12 = I2S1/XBAR not set up, 0x10 = ACPI resources missing.
    switch ($p.Stage) {
      3 { Log 'RESULT: the driver runs. Look for "Speakers" in Settings > System > Sound.' }
      0x11 { Log ("RESULT: the APE clocks are not on (ApeCheck=0x{0:X}, needs 0xF). Boot with UEFI R402." -f $p.ApeCheck) }
      0x12 { Log 'RESULT: the APE clocks are on but UEFI did not set up I2S1. Boot with UEFI R402.' }
      default { Log "RESULT: the driver stopped at stage $($p.Stage)." }
    }
    # 1.0.0.7: headphones (UEFI R403 sets up the codec's headphone path and the jack switch PV6).
    if ($null -ne $p.Jack) {
      Log ("JACK: {0} (switched {1} times)" -f $(if ($p.Jack -eq 1) { 'headphones' } else { 'speaker' }), $p.JackSwitches)
    } elseif ($null -ne $p.HpoMixer -and ($p.HpoMixer -band 0x2000) -ne 0) {
      Log 'JACK: off, the headphone path is not set up. Boot with UEFI R403.'
    } elseif ($null -ne $p.JackSpb -and $p.JackSpb -ne 0) {
      Log ("JACK: off, the codec could not be opened over I2C (0x{0:X})." -f $p.JackSpb)
    }
    # 1.1.0: the TV (HDA). TvStage: 0x20 SOR partition off, 0x21/0x22 controller reset stuck,
    # 0x23 no codec, 0x24 codec does not answer, 0x25/0x26 widgets not found, 0x2C hda clock off,
    # 0x2D skipped (the last start hung; -Mode TvReset), 0x2E no memory, 0x2F TvAudioOff=1,
    # 0x30 controller, 0x31 codec ready, 0x32 TV endpoint registered.
    $tv = @($props | Where-Object { $_.Name -match '^Tv' } | ForEach-Object { '{0}=0x{1:X}' -f $_.Name, $_.Value })
    if ($tv.Count) { Log ('TV trace: ' + ($tv -join ' ')) }
    if ($null -ne $p.TvStage) {
      switch ($p.TvStage) {
        0x32 { Log ("TV: endpoint registered, TV {0} (codec 0x{1:X})" -f $(if ($p.TvPresent -eq 1) { 'connected' } else { 'not connected' }), [uint32]$p.TvVendor) }
        0x2D { Log 'TV: skipped, the last start did not finish. Run -Mode TvReset to try again.' }
        default { Log ("TV: stopped at stage 0x{0:X}" -f $p.TvStage) }
      }
    }
  } else { Log 'driver trace: (no Parameters key)' }
  # 1.1.1: every render endpoint Windows knows, with its state (also unplugged/disabled ones).
  $mm = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render'
  $states = @{ 1 = 'active'; 2 = 'disabled'; 4 = 'not present'; 8 = 'unplugged' }
  foreach ($k in @(Get-ChildItem $mm -ErrorAction SilentlyContinue)) {
    $st = (Get-ItemProperty $k.PSPath -ErrorAction SilentlyContinue).DeviceState
    $pp = Get-ItemProperty (Join-Path $k.PSPath 'Properties') -ErrorAction SilentlyContinue
    $name = $pp.'{a45c254e-df1c-4efd-8020-67d146a850e0},2'
    $dev = $pp.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
    $s2 = $states[[int]($st -band 0xF)]
    if (-not $s2) { $s2 = 'state' }
    Log ("render endpoint: '{0}' ({1})  {2} 0x{3:X}" -f $name, $dev, $s2, [uint32]$st)
  }
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
  'Check' {
    $wav = Join-Path $env:windir 'Media\Windows Notify System Generic.wav'
    Log "Listen: playing $wav through the Windows audio engine."
    try { (New-Object Media.SoundPlayer $wav).PlaySync(); Log 'play: no error' }
    catch { Log ('play: ' + $_.Exception.Message) }
    $since = (Get-Date).AddHours(-1)
    $ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'Application'; StartTime = $since } -ErrorAction SilentlyContinue |
      Where-Object { $_.Message -match 'audiodg|audiosrv|AudioSes|AudioEng' })
    $ev += @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; StartTime = $since; Level = 1, 2, 3 } -ErrorAction SilentlyContinue |
      Where-Object { $_.Message -match 'audio|switchaudio' })
    if ($ev.Count -eq 0) { Log 'events: no audio errors in the last hour' }
    foreach ($e in ($ev | Sort-Object TimeCreated -Descending | Select-Object -First 6)) {
      $m = ($e.Message -replace '\s+', ' ')
      if ($m.Length -gt 300) { $m = $m.Substring(0, 300) }
      Log ("event: {0:HH:mm:ss} {1} id={2}: {3}" -f $e.TimeCreated, $e.ProviderName, $e.Id, $m)
    }
    Show-Status
  }
  'Tone' {
    if (-not (Test-Path $Key)) { New-Item -Path $Key | Out-Null }
    Set-ItemProperty -Path $Key -Name TestTone -Value 1 -Type DWord
    $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0C01*' })
    foreach ($d in $dev) { Log (& $Pnputil /restart-device $d.PNPDeviceID | Out-String) }
    Log 'Listen: a one-second 1 kHz tone should play now.'
    Start-Sleep -Seconds 3
    Show-Status
  }
  'TvTone' {
    if (-not (Test-Path $Key)) { New-Item -Path $Key | Out-Null }
    Set-ItemProperty -Path $Key -Name TvTestTone -Value 1 -Type DWord
    # 1.1.1: the driver's jack thread picks it up within a second (no device restart).
    Log 'Listen: a one-second 1 kHz tone should play on the TV now.'
    Start-Sleep -Seconds 4
    Show-Status
  }
  'TvOff' {
    if (-not (Test-Path $Key)) { New-Item -Path $Key | Out-Null }
    Set-ItemProperty -Path $Key -Name TvAudioOff -Value 1 -Type DWord
    Log 'TV audio OFF from the next boot (TvAudioOff = 1). Restart Windows now.'
  }
  'TvOn' {
    if (-not (Test-Path $Key)) { New-Item -Path $Key | Out-Null }
    Set-ItemProperty -Path $Key -Name TvAudioOff -Value 0 -Type DWord
    Log 'TV audio ON from the next boot (TvAudioOff = 0). Restart Windows now.'
  }
  'TvReset' {
    if (Test-Path $Key) { Set-ItemProperty -Path $Key -Name TvPending -Value 0 -Type DWord }
    $dev = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue | Where-Object { $_.PNPDeviceID -like 'ACPI\SWOA0C01*' })
    foreach ($d in $dev) { Log (& $Pnputil /restart-device $d.PNPDeviceID | Out-String) }
    Start-Sleep -Seconds 4
    Show-Status
  }
  'Remove' {
    $pkgs = (& $Pnputil /enum-drivers | Out-String) -split "(\r?\n){2,}" | Where-Object { $_ -match 'switchaudio\.inf' }
    foreach ($b in $pkgs) {
      if ($b -match '(oem\d+\.inf)') { Log (& $Pnputil /delete-driver $Matches[1] /uninstall /force | Out-String) }
    }
    Show-Status
  }
}
