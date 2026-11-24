# publish-share.ps1 - put the newest SwitchGpu test kit on the PC share so the
# Switch can fetch it with GetKit.cmd. Writes only below D:\SwitchShare\gpu.
param([string]$Zip = '')
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Zip) { $Zip = (Get-ChildItem (Join-Path $Root 'build') -Filter 'SwitchGpu-*-testkit.zip' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName }
$share = 'D:\SwitchShare\gpu'
$kit = Join-Path $share 'kit'
New-Item -ItemType Directory -Force $share, (Join-Path $share 'logs') | Out-Null
if (Test-Path $kit) { Remove-Item $kit -Recurse -Force }
New-Item -ItemType Directory -Force $kit | Out-Null
Expand-Archive $Zip -DestinationPath $kit -Force
$hash = (Get-FileHash (Join-Path $kit 'switchgpu.sys') -Algorithm SHA256).Hash
Set-Content -Path (Join-Path $kit 'SHA256.txt') -Value $hash -Encoding ascii
Copy-Item (Join-Path $kit 'GetKit.cmd'), (Join-Path $kit 'GetKitOnly.cmd'), (Join-Path $kit 'GetKit.ps1') $share -Force
Write-Host "published $Zip -> $kit"
Write-Host "switchgpu.sys SHA256 $hash"