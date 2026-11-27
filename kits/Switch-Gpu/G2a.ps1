<#
  G2a test (switchgpu 0.14.0 + switchdisplay 1.9.0): block-linear scanout and
  the display controller's transpose. ASCII only (PowerShell 5.1).
  -Mode On     : asks switchdisplay to run the test once after the next restart.
  -Mode Status : prints the results.
#>
param([Parameter(Mandatory = $true)][ValidateSet('On', 'Status')][string]$Mode)
$ErrorActionPreference = 'Stop'
$key = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters'
$gpuKey = 'HKLM:\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters'
$log = 'C:\SwitchWOA-G2a.txt'
function Hex($v) { if ($null -eq $v) { 'n/a' } else { $n = [long]$v; if ($n -lt 0) { $n += 4294967296 }; '0x{0:X8}' -f $n } }
function Out([string]$s) { Write-Host $s; Add-Content -Path $log -Value $s -Encoding ascii }
if ($Mode -eq 'On') {
  if (-not (Test-Path $key)) { New-Item -Path $key | Out-Null }
  $p = Get-ItemProperty $key
  foreach ($n in $p.PSObject.Properties.Name) { if ($n -like 'G2a*') { Remove-ItemProperty -Path $key -Name $n } }
  New-ItemProperty -Path $key -Name G2aTest -PropertyType DWord -Value 1 -Force | Out-Null
  Write-Host 'G2a test is set. Restart Windows. About 1 minute after sign-in the screen'
  Write-Host 'shows a test picture 4 times (6 s each, 1 to 4 yellow squares at the top).'
  Write-Host 'Note which number looks upright (white L top-left, red top-right, blue bottom-left).'
  Write-Host 'Then run G2aStatus.cmd.'
  exit 0
}
$d = Get-ItemProperty $key -ErrorAction SilentlyContinue
$g = Get-ItemProperty $gpuKey -ErrorAction SilentlyContinue
Out ''
Out ("=== G2a status  {0}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
Out ("versions: switchdisplay {0}  switchgpu {1}" -f (Hex $d.Version), (Hex $g.Version))
Out ("asked {0}  done {1}  step {2}  shown {3}  restored {4}" -f $d.G2aTest, $d.G2aDone, (Hex $d.G2aStep), $d.G2aShown, $d.G2aRestored)
Out ("GPU copy kind 0   : status {0}  bad pixels {1}  first bad offset {2}  tries {3}" -f (Hex $d.G2aKind0Status), $d.G2aKind0Bad, (Hex $d.G2aKind0FirstBad), $d.G2aGpuTries)
Out ("GPU copy kind 0xFE: status {0}  bad pixels {1}  first bad offset {2}" -f (Hex $d.G2aKindFEStatus), $d.G2aKindFEBad, (Hex $d.G2aKindFEFirstBad))
Out ("variants (0 = shown without error, 0xFFFFFFFF = not latched): 1 {0}  2 {1}  3 {2}  4 {3}" -f (Hex $d.G2aVariant1), (Hex $d.G2aVariant2), (Hex $d.G2aVariant3), (Hex $d.G2aVariant4))
Out ("saved window A: options {0}  size {1}  prescaled {2}  dda {3}  BL buffer {4}" -f (Hex $d.G2aSavedOptions), (Hex $d.G2aSavedSize), (Hex $d.G2aSavedPrescaled), (Hex $d.G2aSavedDda), (Hex $d.G2aBlPhysLo))
Out "log: $log"