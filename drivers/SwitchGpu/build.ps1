<#
.SYNOPSIS
  Builds switchgpu.sys (GM20B bring-up test, KMDF 1.15, SpbCx) and makes a test kit:
  build\SwitchGpu-<ver>-testkit(.zip) = kits\Switch-Gpu + sys/inf/cat + test certificate.
  Not part of the OTA release.
#>
param([switch]$NoSign)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Build = Join-Path $Root $(if ($NoSign) { 'build\SwitchGpu-unsigned' } else { 'build\SwitchGpu' })
$Inf = Join-Path $PSScriptRoot 'switchgpu.inf'
& (Join-Path $Root 'tools\Build-Driver.ps1') -Name switchgpu -Sources (Join-Path $PSScriptRoot 'src\switchgpu.c') `
  -Inf $Inf -OutDir $Build -ExtraIncludes '$Kits\Include\10.0.19041.0\km\spb\1.0', '$Kits\Include\10.0.26100.0\shared' -NoSign:$NoSign

$ver = (Select-String -Path $Inf -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value
$suffix = if ($NoSign) { '-unsigned' } else { '' }
$displayDir = Join-Path $Root 'drivers\SwitchDisplay'
& (Join-Path $displayDir 'build.ps1') -NoSign:$NoSign
$displayVer = (Select-String -Path (Join-Path $displayDir 'switchdisplay.inf') -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value
$displayKit = Join-Path $Root "build\SwitchDisplay-$displayVer-testkit$suffix"
$kit = Join-Path $Root "build\SwitchGpu-$ver-testkit$suffix"
if (Test-Path $kit) { Remove-Item $kit -Recurse -Force }
New-Item -ItemType Directory -Force $kit | Out-Null
Copy-Item (Join-Path $Root 'kits\Switch-Gpu\*') $kit -Recurse
foreach ($ext in 'sys', 'inf') { Copy-Item (Join-Path $Build "switchgpu.$ext") $kit }
if ($NoSign) {
    $inf2cat = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe'
    & $inf2cat "/driver:$kit" '/os:10_VB_ARM64' '/uselocaltime'
    if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed (switchgpu)' }
    Set-Content (Join-Path $kit 'UNSIGNED.txt') 'Unsigned build. Sign both packages before installing.' -Encoding ASCII
} else {
    Copy-Item (Join-Path $Build 'switchgpu.cat') $kit
    Copy-Item (Join-Path $Root 'tools\cert\SwitchWOA-Test.cer') $kit
}
$display = New-Item -ItemType Directory -Path (Join-Path $kit 'display')
foreach ($ext in 'sys', 'cat', 'inf') { Copy-Item (Join-Path $displayKit "switchdisplay.$ext") $display.FullName }
$zip = "$kit.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $kit '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Test kit: $zip"
Write-Host ('switchgpu.sys SHA256 {0}' -f (Get-FileHash -Algorithm SHA256 (Join-Path $Build 'switchgpu.sys')).Hash)
