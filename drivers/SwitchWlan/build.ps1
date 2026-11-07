<#
.SYNOPSIS
  Build switchwlan (WiFiCx client): C++ glue (wlan.cpp) + C chip layer (bcm.c)
  against the 10.0.26100 WDK, for Windows 11 22H2 (WiFiCx 1.0, NetAdapterCx 2.2,
  KMDF 1.33). Output: ..\..\build\SwitchWlan
#>
param([switch]$NoSign)
$L = 'C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\km\arm64'
$I = '$Kits\Include\10.0.26100.0'
$args = @{
  Name          = 'switchwlan'
  Sources       = @("$PSScriptRoot\src\wlan.cpp", "$PSScriptRoot\src\bcm.c")
  OutDir        = "$PSScriptRoot\..\..\build\SwitchWlan"
  Inf           = "$PSScriptRoot\switchwlan.inf"
  Wdk           = '10.0.26100.0'
  Ntddi         = '0x0A00000C'
  KmdfVersion   = '1.33'
  Defines       = @('NET_VERSION_MAJOR=2', 'NET_VERSION_MINOR=2', 'WIFI_VERSION_MAJOR=1', 'WIFI_VERSION_MINOR=0')
  ExtraIncludes = @("$I\km\netcx\kmdf\adapter\2.2", "$I\shared\netcx\shared\1.0", "$I\km\wificx\1.0")
  Libs          = @("$L\netcx\kmdf\adapter\2.2\netadaptercxstub.lib", "$L\wificx\1.0\wificxstub.lib", "$L\ndis.lib")
}
if ($NoSign) { $args.NoSign = $true }
& "$PSScriptRoot\..\..\tools\Build-Driver.ps1" @args
