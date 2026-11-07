# Tegra210Sdhc (nvtsdhc)

SD host miniport for `sdport.sys`, for the Switch's microSD slot (Tegra210 SDMMC1, `ACPI\NVDA0101`).

- Based on the SDHC miniport in Microsoft's Windows-driver-samples (`sd/miniport/sdhc`, MIT license).
- Tegra210 limits: 3.3 V, legacy/high speed only (`NV_T210_SAFE_BRINGUP`), no UHS-I voltage switch or tuning.
- `NV_T210_PIO_ONLY` (default off): all data goes through the data port, never DMA. On Windows 11 22621 the
  inbox sdbus could not mount the boot volume on this non-coherent controller (`_CCA = 0`).
- Build: `tools\Build-Driver.ps1 -Wdm -Name nvtsdhc -Sources drivers\Tegra210Sdhc\src\nvtsdhc.c -Libs sdport.lib -Inf drivers\Tegra210Sdhc\nvtsdhc.inf -OutDir build\Tegra210Sdhc`

The UEFI does not describe the SD host yet. `loader\sd-acpi-chainloader.zig` (built as `SDLOADAA64.EFI`,
placed at `EFI\Boot\bootaa64.efi`) installs `acpi\SDH0-Test.asl` as an SSDT and then starts Windows Boot Manager.

Boot diagnostics (`NV_DIAG_STOP`, default on while bringing it up): if fewer than 8 blocks have been read
2.5 s after `DriverEntry`, the driver bugchecks with stop code `0xDSFCCEEK` (stage, flags, last command,
last error command, error kind; see the comment at the top of `src/nvtsdhc.c`). Card detect always reports
a card, since the Switch wires it to a GPIO.

## Switch Lite (HDH-001) read test

For a RAM-booted WinPE that only needs to read its deployment card, add
`-Defines NV_T210_LITE_READ_ONLY=1` to the PowerShell build command above and use a separate output directory.
Use firmware that exposes the external slot as `ACPI\NVDA0101` with `_CCA = 0`, leaves it at 3.3 V
and selects PLLP for SDMMC1. With WinX's SDM1 already present, load the boot manager directly;
adding the example SSDT would describe the same controller twice. Inject the resulting signed package
into the ARM64 WinPE image before boot.

The profile reads the firmware's CAR divider, uses 1- or 4-bit default-speed PIO up to 25 MHz,
and reports write protection to SDPORT. Data-out, block writes, write-protect changes and erase
are rejected before a request is queued. The diagnostic stop timer and crash-dump path are disabled.
For an on-device read check, place a marker file on the external FAT volume, boot to the WinPE RAM
disk, and read that file from the external volume. One HDH-001 running 22621.1 passed this check
with the development build. The default ADMA configuration was outside that test.

## Restricted WINARM writes on HDH-001

Build with `NV_T210_LITE_WINARM_WRITE=1`. The default card profile presents the 80 GiB NTFS partition `WINARM` as the sole, active MBR entry. Windows accepts block writes only inside that partition after checking its identity. Hekate still reads the physical FAT32 volume.

To also mount FAT32 in Windows, pass `--expose-fat32` to the generator. This experimental profile projects FAT32 as the second MBR entry and permits writes there only after validating its extent and boot sector. On the same Lite, showing FAT32 caused noticeable desktop lag. Hiding it made the interface much smoother. The cause still needs investigation.

The generator reads a saved whole-card image and validates its hybrid MBR, primary GPT and NTFS boot sector. The tested layout places WINARM in MBR slot 4 and GPT slot 11. Both profiles protect the physical partition tables and other partitions from writes. Keep the generated identifiers private.

Build BL33 and the SD miniport from **the same generated header**, and deploy them as a pair. Mixing a one-volume firmware with a two-volume miniport caused `INACCESSIBLE_BOOT_DEVICE` (0x7B). The card comparison used miniport 0.3.2.5 (WINARM only) and 0.3.2.6 (WINARM + FAT32). Source version 0.3.2.7 supports both profiles.

From the repository root in PowerShell:

```powershell
$sd = 'drivers/Tegra210Sdhc'
py -3 "$sd/winarm/configure-target.py" 'D:/images/test-card.img' "$sd/winarm/generated"
Copy-Item "$sd/winarm/generated/winarm-target.h" "$sd/src/"
./tools/Build-Driver.ps1 -Wdm -Name nvtsdhc -Sources "$sd/src/nvtsdhc.c" -Libs sdport.lib -Defines 'NV_T210_LITE_WINARM_WRITE=1' -Inf "$sd/winarm/nvtsdhc.inf" -OutDir build/WINARM-R3
```

For WinPE deployment, use the matching R3 BL33, generated startnet.cmd and the signed test package. Verify the final SYS and INF against their catalog using a test certificate valid at the firmware's cold-start clock. The hardware build and test steps are in the [R3 source tree](https://github.com/ruje0504-2/winx/tree/b44da7475fb281d6e4390d976f8f8e0098154d87/docs/hdh001-winarm-r3). One HDH-001 verified the 4 KiB receipt file after shutdown and a second boot. [Hardware report](https://github.com/steck0714/winx/issues/5)

### T214 SDR104 candidate

The `NV_T214_SDR104=1` bounded WINARM profile adds B01 SDMMC1 signaling
voltage switching, pad calibration, hardware CMD19 tuning and a 199.68 MHz
PLLC4 OUT2 clock. SDR50, DDR50 and proprietary DDR200 are not advertised.
It requires the accompanying `tegra210i2c` 1.0.0.5 package: both drivers
serialize I2C5 access while the boot miniport adjusts the SD LDO2 rail.

Large requests can use a private noncached, below-4-GiB ADMA2 buffer. The
existing MBR presentation and per-card write bounds are applied before
completion; the default WINARM-only view is unchanged. Allocation or mapping
constraints keep PIO available. Controller errors are returned to SDPORT
and a failed DMA operation requires a completed DAT reset before reuse.

Hardware result for 0.4.0.1: Windows reached the desktop and the returned
SYSTEM hive reports `BusSpeed=7` (SDR104), `Signaling18V=1`, `BusWidth=4`,
`ClockKhzActual=199680`, `Tuned=1`, `TuneStatus=0`, 128 tuning iterations
and tap 64. ADMA2 completed 1,867,466 blocks with `AdmaErrors=0`; voltage
switching and pad calibration recorded no failures or fallbacks. These are
driver/host readbacks from the observed run, not a standalone throughput
benchmark or extended integrity/stress qualification.

ARM64 builds and host DMA/filter/write-bound tests pass. Read `Signaling18V`,
`Tuned`, `TuningTap`, `ClockKhzActual`, `AdmaBlocks` and `AdmaErrors` under
`Services\nvtsdhc\Parameters\Diag`.

Hardware references: [Hekate SDMMC](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/storage/sdmmc_driver.c),
[Hekate clock setup](https://github.com/CTCaer/hekate/blob/v6.5.3/bdk/soc/clock.c),
and [Switchroot Linux](https://github.com/CTCaer/switch-l4t-kernel-4.9/blob/linux-dev/drivers/mmc/host/sdhci-tegra.c).

Version 0.4.0.1 repairs a reset-all power-state mismatch: the card's external
rail may remain on while SDHCI `POWER_CONTROL` has reset to zero. A subsequent
3.3 V request now restores and checks the controller power bits instead of
returning early. The regression reproduces that cleared-register state and
also checks a real power cycle when leaving 1.8 V. This fix passes the host
regression and ARM64 build; the owner subsequently reached Windows with 0.4.0.1. Its signaling, tuning
and ADMA readbacks are recorded above; sustained throughput is still unmeasured.

### FAT32-visible and resume candidate (0.4.0.2)

The owner requested another FAT32-visible trial using SDR104/ADMA2, preserving
the hidden profile. Both builds share the same validated write bounds. DMA
read completion now dispatches identity checks only for the few relevant
metadata sectors, rather than once per returned sector. Tests compare the
result with the previous sector-by-sector path across 1–128-block reads in
both partition layouts. The card's FAT32 FSInfo signatures and free-count
field were valid; no FSInfo metadata is rewritten or fabricated.

The miniport also saves the quiesced SDHCI host/power/clock/interrupt settings
and T214 tuning registers. Resume restores configuration with SDCLK held off
until the internal clock is stable; commands, FIFO contents, DMA pointers and
old interrupt status are never replayed. A clock failure closes the DMA gate.
This covers controller context loss with card power retained, not a card power
cycle or a complete SC7 platform restore. Fixed `ContextSaves`,
`ContextRestores` and `ResumeStatus` values join the existing diagnostic key.

The ARM64 builds and ASan/UBSan DMA/filter/write-bound/context regressions pass.
This is a performance and resume candidate: physical desktop responsiveness
and repeated sleep/wake still require measurement. Use the matching
FAT32-visible header in BL33 and the miniport; do not mix the two layouts.
