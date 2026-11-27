<#
  Turn one Switch device off or on (pnputil /disable-device, /enable-device) for power tests.
  -Name Wifi | Bt | Gpu | Audio | Sensor   -Action Off | On | Status
  Everything comes back on with -Action On (or a restart does not: disabled stays disabled).
  ASCII only, PowerShell 5.1.
#>
param([Parameter(Mandatory = $true)][ValidateSet('Wifi', 'Bt', 'Gpu', 'Audio', 'Sensor', 'All')][string]$Name,
      [Parameter(Mandatory = $true)][ValidateSet('Off', 'On', 'Status')][string]$Action)
$Sys = Join-Path $env:windir 'Sysnative'
if (-not (Test-Path (Join-Path $Sys 'pnputil.exe'))) { $Sys = Join-Path $env:windir 'System32' }
$Pnputil = Join-Path $Sys 'pnputil.exe'
$Ids = [ordered]@{ Wifi = 'ACPI\SWOA0D01'; Bt = 'ACPI\SWOA0B01'; Gpu = 'ACPI\SWOA0F01'; Audio = 'ACPI\SWOA0C01'; Sensor = 'ACPI\SWOA0901' }
$names = @($Name); if ($Name -eq 'All') { $names = @($Ids.Keys) }
foreach ($n in $names) {
  $devs = @(Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like ($Ids[$n] + '\*') -and ($_.Present -or $_.Status -ne 'Unknown') })
  if ($devs.Count -eq 0) { Write-Host "$n : no device $($Ids[$n])"; continue }
  foreach ($d in $devs) {
    if ($Action -eq 'Off') {
      if ($n -eq 'Wifi') { & (Join-Path $Sys 'sc.exe') stop wlansvc | Out-Null }   # as SwitchCenter: wlansvc holds the adapter
      & $Pnputil /disable-device "$($d.InstanceId)" | Out-Null
      if ($n -eq 'Wifi') { & (Join-Path $Sys 'sc.exe') start wlansvc | Out-Null }
    } elseif ($Action -eq 'On') {
      & $Pnputil /enable-device "$($d.InstanceId)" | Out-Null
    }
    $d2 = Get-PnpDevice -InstanceId $d.InstanceId -ErrorAction SilentlyContinue
    $cm = (Get-CimInstance Win32_PnPEntity -Filter ("PNPDeviceID='" + ($d.InstanceId -replace '\\', '\\') + "'") -ErrorAction SilentlyContinue).ConfigManagerErrorCode
    $state = if ($cm -eq 22) { 'OFF' } elseif ($cm -eq 0) { 'on' } else { 'problem ' + $cm }
    Write-Host ('{0,-7} {1,-10} {2}  {3}' -f $n, $state, $d2.FriendlyName, $d2.InstanceId)
  }
}
