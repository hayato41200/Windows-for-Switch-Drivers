# Tegra210Sdhc (nvtsdhc)

SD host miniport for `sdport.sys`, for the Switch's microSD slot (Tegra210 SDMMC1, `ACPI\NVDA0101`).

- Based on the SDHC miniport in Microsoft's Windows-driver-samples (`sd/miniport/sdhc`, MIT license).
- Tegra210 limits: 3.3 V, legacy/high speed only (`NV_T210_SAFE_BRINGUP`), no UHS-I voltage switch or tuning.
- `NV_T210_PIO_ONLY` (default on): all data goes through the data port, never DMA. On Windows 11 22621 the
  inbox sdbus could not mount the boot volume on this non-coherent controller (`_CCA = 0`).
- Build: `tools\Build-Driver.ps1 -Wdm -Name nvtsdhc -Sources drivers\Tegra210Sdhc\src\nvtsdhc.c -Libs sdport.lib -Inf drivers\Tegra210Sdhc\nvtsdhc.inf -OutDir build\Tegra210Sdhc`

The UEFI does not describe the SD host yet. `loader\sd-acpi-chainloader.zig` (built as `SDLOADAA64.EFI`,
placed at `EFI\Boot\bootaa64.efi`) installs `acpi\SDH0-Test.asl` as an SSDT and then starts Windows Boot Manager.

Status: not yet tested on hardware.
