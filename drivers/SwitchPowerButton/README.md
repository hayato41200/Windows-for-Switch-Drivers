# SwitchPowerButton (switchpwrbtn)

[日本語](README-ja.md)

KMDF driver for the Nintendo Switch (Erista) power button, plus restart-to-hekate and power-off
at the end of a Windows shutdown. Device `ACPI\SWOA0601` (UEFI R374 or later), the MAX77620 PMIC
at I2C5 address 0x3C. Install it with `kits/Switch-PowerButton`.

## Power button

- The button drives the PMIC's EN0 input; there is no SoC GPIO for it.
- Every 50 ms the driver reads ONOFFSTAT (0x15, bit 2 = held) and ONOFFIRQ (0x0B, clear on read,
  so a press shorter than one poll is still seen) through SPB.
- Presses are reported as a HID System Control "System Power Down" button through VHF (`vhf.sys`
  as a lower filter). Windows then applies the power button action from Power Options.
- Holding the button for several seconds still powers the PMIC off by itself.

## Restart and shut down (1.0.2.0)

Windows on the Switch completes its shutdown, but its last firmware call never resets or powers off
the console (it never reaches the UEFI ResetSystem, on either boot chain). The driver does it itself
in `EvtDeviceD0Exit`, when the power manager powers the devices down for S5; file systems and
registry hives are flushed before that. `WdfDeviceGetSystemPowerAction` tells which it is.

- **Restart** (`PowerActionShutdownReset`): reboot-to-payload, the same steps as switchroot's bl31
  (`r2p_reboot_to_payload`, `tegra_soc_prepare_system_reset`):
  1. CAR `SCLK_BURST_POLICY` (0x28) = 0x10000000, `SCLK_RATE` (0x30) = 0, wait 1 ms.
  2. Clear IRAM 0x40010000–0x4003FFFF, copy the payload to 0x40010000, zero the 32-byte boot
     configuration at +0x94.
  3. PMC `SCRATCH0` (0x50) |= 1 (warmboot), `SCRATCH45` (0x234) = 0x2E38DFFF, `SCRATCH46` (0x238)
     = 0x6001DC28, `SCRATCH33` (0x120) = 0x40010000, `SCRATCH40` (0x13C) = 0x6000F208.
  4. `PMC_CNTRL` (0x0) |= `MAIN_RST` (0x10).

  Erista only (APB_MISC `GP_HIDREV` major revision < 2). The payload (default
  `\Device\Harddisk0\Partition1\bootloader\update.bin`, hekate on the SD's FAT partition) is read
  into nonpaged memory while Windows runs, never at shutdown; 0x200 to 0x30000 bytes. Without a
  payload the driver calls PSCI `SYSTEM_RESET`; switchroot's bl31 then does R2P with the payload
  hekate gave it, coreboot's bl31 does a plain reset.
- **Shut down** (`PowerActionShutdown`/`ShutdownOff`): MAX77620 `ONOFFCNFG2` (0x42) &= ~`SFT_RST_WK`
  (bit 7), then `ONOFFCNFG1` (0x41) = `SFT_RST` (bit 7), as Linux `max77620_pm_power_off`;
  `PWR_OFF` (bit 1) as a fallback. Through SPB first, then the I2C5 registers (0x7000D000) directly
  if the I2C controller no longer takes requests.
- The backlight PWM (0x7000A000) is switched off first, as a marker: black with the backlight off
  means the driver got there.
- Other power actions (device removal, driver update, hibernate, sleep) do nothing.

Tested on hardware on 2026-11-17 (Erista, Windows 11): restart returns to the hekate menu both when
Windows was booted from hekate and from RCM (coreboot); shut down powers the console off (tested on
the hekate chain).

## Parameters

`HKLM\SYSTEM\CurrentControlSet\Services\switchpwrbtn\Parameters`, read at start and every 10 s.

| Value | Meaning |
|---|---|
| `ResetEnable` | 1 (default): the driver restarts and powers off. 0: leave it to Windows (hangs, as before) |
| `RestartMethod` | 0 auto (payload loaded: R2P; else PSCI), 1 PMC only, 2 PSCI `SYSTEM_RESET`, 3 plain PMC reset, 4 backlight off only (test) |
| `PowerOffMethod` | 0 PMIC (default), 1 backlight off only (test), 2 leave it to Windows |
| `FastStartup` | 0 (default) keep Windows fast startup off (since 1.0.4.0), 1 leave it alone. With fast startup on, Shut down is a hybrid hibernate: the console does not power off and comes back to the logon screen |
| `PayloadPath` | REG_SZ, NT path of the payload |
| `RebootToId` | REG_SZ, 1–7 printable ASCII characters, no spaces (1.0.3.0). After a restart hekate autoboots the `hekate_ipl.ini` entry whose `id=` key matches. Empty (default): the hekate menu |
| `RebootToIndex` | DWORD (1.0.3.0), used when `RebootToId` is empty: autoboot entry N of `hekate_ipl.ini` (the first entry after `[config]` is 1). 0: the menu |

The restart target is passed as hekate's boot configuration (`boot_cfg_t`, hekate `bdk/utils/types.h`):
`boot_cfg` = 0x05 (AUTOBOOT_EN | FROM_ID) with `id[8]`, or `boot_cfg` = 0x01 with `autoboot` = N. It is
written to the payload at +0x94 for R2P from Windows, or handed to switchroot's bl31 with SiP
0xC2FFFE04 (R2P_SET_CFG) before PSCI. Holding VOL- while hekate starts still opens its menu.

The driver writes its state to the same key (`DriverVersion`, `Hidrev`, `Erista`, `Mapped`,
`PayloadStatus`, `PayloadSize`, `PayloadWord0`, …); `kits/Switch-PowerButton/Status.cmd` shows them.

## Build

```powershell
.\tools\Build-Driver.ps1 -Name switchpwrbtn -Sources drivers\SwitchPowerButton\src\switchpwrbtn.c `
  -Inf drivers\SwitchPowerButton\switchpwrbtn.inf -OutDir build\SwitchPowerButton -Libs vhfkm.lib,libcntpr.lib `
  -ExtraIncludes '$Kits\Include\10.0.19041.0\km\spb\1.0','$Kits\Include\10.0.26100.0\shared'
```

`release\Make-Release.ps1` builds it as part of a release.
