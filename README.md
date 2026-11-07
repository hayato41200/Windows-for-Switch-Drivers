# Windows ARM64 drivers for Nintendo Switch Lite

Drivers for the **HDH-001 (Tegra T214)** port of Windows 11 ARM64. They use the ACPI devices exposed by the [WinX companion firmware](https://github.com/ruje0504-2/winx). The tested system runs Windows 11 23H2 (22631.2861).

## Drivers on hardware

| Driver | Result on Switch Lite |
| --- | --- |
| `nvtsdhc` | Boots from the 80 GiB NTFS `WINARM` volume. Writes and cold reads passed. FAT32 is hidden by default. The [SDR104 / ADMA2 candidate](drivers/Tegra210Sdhc/README.md#t214-sdr104-candidate) 0.4.0.1 reaches Windows; negotiated SDR104 mode and throughput still need readback. |
| `switchwifi` 0.5.5 | Automatically reconnected to a saved WPA2-Personal network. Windows obtained a DHCP lease and default gateway, and the owner confirmed Internet access. |
| `switchbt` 1.0.0.13 | Bluetooth works on the trimmed Pro installation. Range remains unmeasured. |
| GPIO, I²C3 and FTM4 touch | The shared controllers and Lite touch driver handle taps in Windows Setup. |
| `switchsio` (analog sticks and gamepad) | Windows detected the HID gamepad and mouse and the driver received controller input. Both sticks are exposed to generic HID games. In-game use awaits testing. |
| `switchsio` desktop controls | Left stick moves the pointer and ZR/ZL click. Capture toggles desktop mode and **+** opens the On-Screen Keyboard (0.2.2 on the card). Version 0.2.3 adds right-stick scrolling, R3 middle click and D-pad keyboard arrows. Hardware test pending. [Button mapping](drivers/SwitchSensor/README.md) |
| `switchaudio` 1.0.0.9 | The built-in speakers play audio. Headphones and jack switching await testing. |
| `switchfan` 1.0.0.5 | The fan and panel brightness controls work. Readbacks showed 1996.8 MHz CPU and 1866 MHz RAM. |

Windows uses CPU software rendering only. The Tegra GPU driver for Windows has not been implemented.

The USB-C host package is on the Windows 23H2 card and awaits hardware testing. [USB-C host](drivers/TegraXusb/README.md)

Battery, RTC and power-lifecycle results are tracked in the [porting notes](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md).

## Building and installing

- Start with the [HDH-001 build and install guide](docs/hdh001/README.md) for the ARM64 toolchain, test signing and device packages.
- Use the same card-specific header for both the Windows SD miniport and WinX BL33. The default exposes only `WINARM`. The optional `--expose-fat32` profile also mounts FAT32, but caused noticeable desktop lag in a same-card comparison and needs optimization. [SD miniport build](drivers/Tegra210Sdhc/README.md#restricted-winarm-writes-on-hdh-001)
- For Wi-Fi installation, required board firmware and driver diagnostics, see the [SwitchWifi guide](drivers/SwitchWifi/README.md).

## Upstream

Shared driver implementations, SwitchCenter and SwitchPatcher come from [hayato41200/Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers). The upstream project documents the original T210/Erista packages and credits Claude for assistance with its repository and README. Some register sequences follow Hekate and Linux. See the [GPL-2.0 license](LICENSE) and individual source notices.
