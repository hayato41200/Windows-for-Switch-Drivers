<#
.SYNOPSIS
  Builds switchdisplay.sys (WDDM display-only, plain WDM entry) against the
  10.0.26100 WDK (the 10.0.19041 kit has no d3dkmdt.h) and makes a test kit:
  build\SwitchDisplay-<ver>-testkit(.zip) = kits\Switch-Display + sys/inf/cat + test certificate.
  Not part of the OTA release yet.
#>
param([switch]$NoSign)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Build = Join-Path $Root $(if ($NoSign) { 'build\SwitchDisplay-unsigned' } else { 'build\SwitchDisplay' })
$Inf = Join-Path $PSScriptRoot 'switchdisplay.inf'
# libcntpr.lib: __security_push_cookie / __security_pop_cookie used by displib.lib on ARM64.
& (Join-Path $Root 'tools\Build-Driver.ps1') -Wdm -Name switchdisplay -Sources (Join-Path $PSScriptRoot 'src\switchdisplay.c') `
  -Libs displib.lib, libcntpr.lib -Inf $Inf -OutDir $Build -Wdk 10.0.26100.0 -Ntddi 0x0A00000C -NoSign:$NoSign

$ver = (Select-String -Path $Inf -Pattern '^\s*DriverVer\s*=\s*[^,]+,\s*(\S+)').Matches[0].Groups[1].Value
$suffix = if ($NoSign) { '-unsigned' } else { '' }
$kit = Join-Path $Root "build\SwitchDisplay-$ver-testkit$suffix"
if (Test-Path $kit) { Remove-Item $kit -Recurse -Force }
New-Item -ItemType Directory -Force $kit | Out-Null
Copy-Item (Join-Path $Root 'kits\Switch-Display\*') $kit -Recurse
foreach ($ext in 'sys', 'inf') { Copy-Item (Join-Path $Build "switchdisplay.$ext") $kit }
if ($NoSign) {
    $inf2cat = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x86\Inf2Cat.exe'
    & $inf2cat "/driver:$kit" '/os:10_VB_ARM64' '/uselocaltime'
    if ($LASTEXITCODE -ne 0) { throw 'Inf2Cat failed (switchdisplay)' }
    Set-Content (Join-Path $kit 'UNSIGNED.txt') 'Unsigned build. Sign before installing.' -Encoding ASCII
} else {
    Copy-Item (Join-Path $Build 'switchdisplay.cat') $kit
    Copy-Item (Join-Path $Root 'tools\cert\SwitchWOA-Test.cer') $kit
}
$zip = "$kit.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $kit '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Host "Test kit: $zip"
Write-Host ('switchdisplay.sys SHA256 {0}' -f (Get-FileHash -Algorithm SHA256 (Join-Path $Build 'switchdisplay.sys')).Hash)
