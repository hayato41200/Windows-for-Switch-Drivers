# Windows ARM64 drivers for Nintendo Switch Lite

Drivers for the **HDH-001 (Tegra T214)** port of Windows 11 ARM64. They use the ACPI devices exposed by the [WinX companion firmware](https://github.com/ruje0504-2/winx). The tested system runs Windows 11 23H2 (22631.2861).

## Drivers on hardware

| Driver | Result on Switch Lite |
| --- | --- |
| `nvtsdhc` | Boots from the 80 GiB NTFS `WINARM` volume. Writes and cold reads passed. FAT32 is hidden by default. |
| `switchwifi` 0.5.5 | Automatically reconnected to a saved WPA2-Personal network. Windows obtained a DHCP lease and default gateway, and the owner confirmed Internet access. |
| `switchbt` 1.0.0.13 | Bluetooth works on the trimmed Pro installation. Range remains unmeasured. |
| GPIO, I²C3 and FTM4 touch | The shared controllers and Lite touch driver handle taps in Windows Setup. |
| `switchaudio` 1.0.0.9 | The built-in speakers play audio. Headphones and jack switching await testing. |
| `switchfan` 1.0.0.5 | The fan and panel brightness controls work. Readbacks showed 1996.8 MHz CPU and 1866 MHz RAM. |

Battery, RTC, built-in controls and Windows USB-C host support are tracked in the [porting notes](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md).

## Building and installing

- Start with the [HDH-001 build and install guide](docs/hdh001/README.md) for the ARM64 toolchain, test signing and device packages.
- Use the same card-specific header for both the Windows SD miniport and WinX BL33. The default exposes only `WINARM`. The optional `--expose-fat32` profile also mounts FAT32, but caused noticeable desktop lag in a same-card comparison and needs optimization. [SD miniport build](drivers/Tegra210Sdhc/README.md#restricted-winarm-writes-on-hdh-001)
- For Wi-Fi installation, required board firmware and driver diagnostics, see the [SwitchWifi guide](drivers/SwitchWifi/README.md).

## Upstream

Shared driver implementations, SwitchCenter and SwitchPatcher come from [hayato41200/Windows-for-Switch-Drivers](https://github.com/hayato41200/Windows-for-Switch-Drivers). The upstream project documents the original T210/Erista packages and credits Claude for assistance with its repository and README. Some register sequences follow Hekate and Linux. See the [GPL-2.0 license](LICENSE) and individual source notices.
