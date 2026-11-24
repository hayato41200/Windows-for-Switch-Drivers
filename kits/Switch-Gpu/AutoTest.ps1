# AutoTest.ps1 - one double-click for the whole E-stage test (0.9.0).
# Stage Install (default): Gpu.ps1 -Mode Install, register a one-shot logon task,
#   then restart Windows (the new driver needs a restart: Code 14).
# Stage After (run by the task after the next logon): remove the task first
#   (so a hang never repeats), then Resident -> Irq -> Submit -> Copy -> Blit -> ResidentStatus -> ResidentStop -> Cycle,
#   then print the summary lines and wait for Enter.
# Results are appended to C:\SwitchWOA-Gpu.txt as before; console output also goes
# to C:\SwitchWOA-GpuAutoTest.txt. ASCII only (PowerShell 5.1).
param([ValidateSet('Install', 'After', 'Cancel', 'SendLogs')][string]$Stage = 'Install')

$ErrorActionPreference = 'Continue'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Gpu = Join-Path $Here 'Gpu.ps1'
$Log = 'C:\SwitchWOA-Gpu.txt'
$TaskName = 'SwitchWOA-GpuAutoTest'

$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) { Write-Host 'Run AutoTest.cmd (it asks for administrator).'; Read-Host 'Press Enter'; exit 1 }

try { Start-Transcript -Path 'C:\SwitchWOA-GpuAutoTest.txt' -Append | Out-Null } catch { }

function Run-Gpu([string]$Mode) {
    Write-Host ''
    Write-Host ('########## ' + $Mode + '  ' + (Get-Date -Format 'HH:mm:ss') + ' ##########') -ForegroundColor Cyan
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $Gpu -Mode $Mode | Out-Host
    $code = $LASTEXITCODE
    Write-Host ('---------- ' + $Mode + ' done (exit ' + $code + ') ----------')
    return $code
}

# Upload the logs to the PC share named in share.txt (written by GetKit).
# Without share.txt nothing is sent. Never fails the test.
function Send-Logs([string]$Tag) {
    $sf = Join-Path $Here 'share.txt'
    if (-not (Test-Path $sf)) { return }
    $share = (Get-Content $sf -TotalCount 1).Trim()
    if (-not $share) { return }
    try {
        $dir = Join-Path $share 'logs'
        if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        Copy-Item $Log (Join-Path $dir 'SwitchWOA-Gpu.txt') -Force -ErrorAction Stop
        if (Test-Path 'C:\SwitchWOA-GpuAutoTest.txt') { Copy-Item 'C:\SwitchWOA-GpuAutoTest.txt' (Join-Path $dir 'SwitchWOA-GpuAutoTest.txt') -Force -ErrorAction Stop }
        Set-Content -Path (Join-Path $dir 'LAST.txt') -Value ($Tag + '  ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding ascii
        Write-Host ('Logs sent to ' + $dir) -ForegroundColor Green
    } catch {
        Write-Host ('Could not send the logs to ' + $share + ': ' + $_.Exception.Message) -ForegroundColor Yellow
    }
}

function Remove-AutoTask {
    try { Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction Stop; Write-Host "task $TaskName removed" }
    catch { Write-Host "task $TaskName was not registered" }
}

if ($Stage -eq 'SendLogs') {
    try { Stop-Transcript | Out-Null } catch { }
    if (-not (Test-Path (Join-Path $Here 'share.txt'))) { Write-Host 'No share.txt here: run GetKit.cmd first, or copy the logs by hand.' }
    Send-Logs 'manual'
    Read-Host 'Press Enter'
    exit 0
}

if ($Stage -eq 'Cancel') {
    & shutdown.exe /a 2>$null
    Remove-AutoTask
    Write-Host 'Restart cancelled (if one was pending) and the automatic tests removed.'
    try { Stop-Transcript | Out-Null } catch { }
    Read-Host 'Press Enter'
    exit 0
}

if ($Stage -eq 'Install') {
    Add-Content -Path $Log -Value ('=== AutoTest Install  ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + '  kit ' + $Here) -Encoding UTF8
    Remove-AutoTask
    $installExit = Run-Gpu 'Install'
    if ($installExit -ne 0) {
        # 0.9.0 lesson: Gpu.ps1 failed before doing anything; do not restart.
        Write-Host ('STOP: Install failed (exit ' + $installExit + '). No restart, no automatic tests.') -ForegroundColor Red
        try { Stop-Transcript | Out-Null } catch { }
        Send-Logs 'install-failed'
        Read-Host 'Take a photo, then press Enter'
        exit 1
    }

    $user = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    $arg = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $Here 'AutoTest.ps1') + '" -Stage After'
    try {
        $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument $arg -WorkingDirectory $Here
        $trigger = New-ScheduledTaskTrigger -AtLogOn -User $user
        $principal = New-ScheduledTaskPrincipal -UserId $user -LogonType Interactive -RunLevel Highest
        # The Switch runs on battery: allow start and keep running on battery.
        $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 2)
        Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force -ErrorAction Stop | Out-Null
        Write-Host "task $TaskName registered: runs once after the next sign-in ($user)"
    } catch {
        Write-Host ('STOP: could not register the logon task: ' + $_.Exception.Message) -ForegroundColor Red
        Write-Host 'Restart by hand, then run Resident, Irq, Submit, Copy, Blit, ResidentStatus, ResidentStop, Cycle.'
        try { Stop-Transcript | Out-Null } catch { }
        Read-Host 'Press Enter'
        exit 1
    }

    Write-Host ''
    Write-Host 'Windows restarts in 30 seconds. After sign-in the tests start by themselves.' -ForegroundColor Yellow
    Write-Host 'To cancel the restart and the automatic tests: run AutoTestCancel.cmd now.'
    try { Stop-Transcript | Out-Null } catch { }
    & shutdown.exe /r /t 30 /c 'SwitchWOA GPU AutoTest: restart after driver install'
    Start-Sleep -Seconds 25
    exit 0
}

# ---- Stage After ----
Remove-AutoTask
Add-Content -Path $Log -Value ('=== AutoTest After  ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding UTF8
Write-Host 'GPU AutoTest: waiting 40 seconds for Windows and the driver to settle...' -ForegroundColor Yellow
for ($i = 40; $i -gt 0; $i -= 5) { Write-Host "  $i s"; Start-Sleep -Seconds 5 }

$startLine = 0
if (Test-Path $Log) { $startLine = (Get-Content $Log).Count }

foreach ($m in 'Resident', 'Irq', 'Submit', 'Copy', 'Blit', 'ResidentStatus', 'ResidentStop', 'Cycle') {
    $null = Run-Gpu $m
    Start-Sleep -Seconds 5
}

Write-Host ''
Write-Host '================ AutoTest SUMMARY ================' -ForegroundColor Green
if (Test-Path $Log) {
    $all = Get-Content $Log
    $new = $all | Select-Object -Skip $startLine
    $new | Where-Object { $_ -match '^=== Gpu ' -or $_ -match '^E1 resident result' -or $_ -match '^E[23] result' -or $_ -match '^F1 copy result' -or $_ -match '^F2 blit result' -or $_ -match '^E4 (cycle|pool) ' -or $_ -match '^resident [01];' } |
        Select-Object -Unique | ForEach-Object { Write-Host $_ }
}
Write-Host '=================================================='
Write-Host 'Log: C:\SwitchWOA-Gpu.txt   Console: C:\SwitchWOA-GpuAutoTest.txt'
Add-Content -Path $Log -Value ('=== AutoTest End  ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss')) -Encoding UTF8
try { Stop-Transcript | Out-Null } catch { }
Send-Logs 'done'
Read-Host 'Take a photo, then press Enter to close'
