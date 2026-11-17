# Windows for Switch — Drivers

English | [日本語](README-ja.md)

Drivers and a control center (SwitchCenter) for Windows on ARM (ARM64) on the Nintendo Switch (Tegra210 / Erista).

The UEFI and DSDT live in [Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch). The drivers here use the devices that its UEFI (R367 or later) adds to the DSDT.

## Drivers

| Driver | Device | UEFI needed | What it does |
|---|---|---|---|
| `tegra210gpio` | ACPI\SWOA0101 | R367 | GPIO controller (GpioClx) |
| `tegra210i2c` | ACPI\SWOA0201 | R370 | I2C controller (SpbCx). I2C1 / I2C3 / I2C5 |
| `switchbattery` | ACPI\SWOA0301 | R371 (USB-C: R378) | Battery (battc); switches VBUS and charging when a USB-C device is plugged in or removed |
| `switchfan` | ACPI\SWOA0401 | R372 (brightness: R376) | Fan control by temperature, screen brightness, backlight off when the screen turns off |
| `switchtouch` | ACPI\SWOA0501 | R373 | Touch screen (VHF HID multi-touch) |
| `switchpwrbtn` | ACPI\SWOA0601 | R374 | Power button (VHF HID). Since 1.0.2.0 a Windows restart returns to hekate (reboot-to-payload, Erista) and shut down powers the console off (MAX77620). Details: `drivers/SwitchPowerButton` ([English](drivers/SwitchPowerButton/README.md), [日本語](drivers/SwitchPowerButton/README-ja.md)) |
| `switchcpu` | ACPI\SWOA0701 | R375 | CPU clock profiles (614–1766 MHz), thermal limits |
| `switchrtc` | ACPI\SWOA0801 | R377 | Clock (reads the MAX77620 RTC, remembers the offset and sets the time) |
| `switchbt` | ACPI\SWOA0B01 | R390 | Bluetooth (the BCM4356A3 on UART-D, connected to the inbox BthMini). 3 Mbps. Bring your own firmware patch `BCM4356A3.hcd` (not distributed). The range is very short for now (the Wi-Fi side is not running) |
| `switchwlan` | `ACPI\SWOA0D01` | R405 | Wi-Fi (beta). WPA2-Personal, DHCP, internet access. Wi-Fi 5 (802.11ac), 866/866 Mbit/s link. Tested on hardware on 2026-11-08: fast.com 55 Mbit/s down, 39 Mbit/s up. Bring your own firmware `brcmfmac4356(A3)-pcie.bin/.txt` (tested with 7.35.252.108). `switchwlan.sys` 0.6.13 |
| `switchaudio` (beta) | ACPI\SWOA0C01 | R403 (R402: speaker only) | Audio playback to the speakers and headphones (PortCls WaveRT, 48 kHz, 16-bit, stereo). Switches automatically when headphones are plugged in or removed. Windows sees one output, "Speakers", with one volume. No microphone |
| `switchsensor` | ACPI\SWOA0901 | R379 | Tilt sensor (automatic screen rotation). The light sensor was not found on this unit and is not used |
| `switchdisplay` (beta) | ACPI\SWOA0E01 | R408 | Built-in panel (WDDM display-only driver, replaces Basic Display). Rotation (Settings and SwitchCenter auto-rotate). Puts the panel to sleep and stops the display controller when the screen turns off. Drawing is still done by the CPU (no GPU acceleration). Replaced at the next boot (`kits/Switch-Display`) |
| `swoadiag` | — (only while loaded) | — | Diagnostic: reads the USB (XUSB) registers |
| `nvtsdhc` | ACPI\NVDA0101 | — (the SD loader adds the device) | SD host (sdport) for booting Windows 11 from SD. SwitchPatcher puts it on the SD (`kits/Switch-Sd`). If boot fails it shows a diagnostic code on a blue screen (see "Blue screen stop codes" below) |

## Tested OS

- Windows 11 Arm64 22H2 (build 22621)

Bring your own Windows image and license. This repository does not contain Windows.

## Installing

1. Download the latest `SwitchDrivers-<version>.zip` and `SwitchCenter-<version>-arm64.zip` from [Releases](https://github.com/hayato41200/Windows-for-Switch-Drivers/releases).
2. **Test signing (first time only):**
   - Enable test signing as described in `Switch-Stage0`.
   - Add the included `SwitchWOA-Test.cer` to the Switch's "Trusted Root Certification Authorities" and "Trusted Publishers".
3. **Install the drivers:** extract the ZIP and use the script in each `Switch-*` folder, in this order. Each folder's README has the steps (the kit READMEs are in Japanese).
   1. GPIO (Stage1)
   2. I2C (Stage3)
   3. Battery
   4. Fan
   5. Touch
   6. Power button (includes restart to hekate and power-off at shut down; `Install.cmd`)
   7. CPU
   8. RTC
   9. Sensor
   10. Bluetooth (UEFI R390 or later; see the kit README for the firmware patch)
4. **Control center:** extract `SwitchCenter-<version>-arm64.zip` and run `SwitchCenter.exe`. From the notification area icon you can:
   - choose the CPU profile
   - set the screen brightness
   - check the status
   - change power settings
   - see the driver list

## SwitchPatcher (PC tool for the SD)

Put the Switch's SD in a PC, and SwitchPatcher installs the drivers, test mode and the control center into the Windows on the SD. It can also rewrite an ARM64 Windows image to the SD.

- **Get it:** `SwitchPatcher-<version>-x64.zip` in the releases
- **How to use:** `kits/Switch-Patcher/README-ja.md` (Japanese)
- **Design:** `docs/Design-SwitchPatcher-ja.md` (Japanese)
- **Status:** tested with a real SD and Switch (0.3.0, Windows 11 Arm64 22H2).

## Blue screen stop codes

Some drivers here raise a blue screen themselves, to leave the cause on the screen: when boot fails there is no log and no dump. A photo of the "Stop code:" line tells where it stopped.

### `0xD???????` (`nvtsdhc`, booting from SD)

Shown on a Windows 11 SD when the SD still cannot be read 2.5 seconds after the driver loaded. The code has the form `0xD S F CC EE K`, one meaning per digit.

| Part | Meaning |
|---|---|
| `D` | Marker (always D) |
| `S` | Stage. 0: the SD slot was never initialized. 1: no command was sent to the SD. 2: no command succeeded |
| `F` | Flags, added together. 1: card present, 2: card stable (both from the controller state at initialization). 4: an interrupt arrived. 8: the controller reset failed |
| `CC` | Last command sent (hex) |
| `EE` | Last command that failed (hex; `FF` = no error) |
| `K` | Kind of the last error. 0: none, 1: no command response, 2: command CRC, 3: command end bit or index, 4: no data, 5: data CRC, 6: data end bit, 7: other, 8: the request could not be sent |

Once commands go through (stage 3 and later), the card is only slow to get ready, and this blue screen is not raised.

Common cases:

- **`0xD0000FF0` (stage 0, nothing recorded):** the driver loaded, but Windows does not see the SD host device. The SD host device is added at every boot by `EFI\Boot\bootaa64.efi` on the ESP (`SDLOADAA64-NOCID.EFI`, put there by SwitchPatcher). Check:
  - that `EFI\Boot\bootaa64.efi` on the ESP is `SDLOADAA64-NOCID.EFI`. Reinstalling with SwitchPatcher is the sure way.
  - that the UEFI does not start `bootmgfw.efi` directly. Without the loader the SD host device is not added.
  - that you use the UEFI from [Windows-for-Switch](https://github.com/hayato41200/Windows-for-Switch). Other UEFIs have not been tested.
- **Stages 1 and 2 (`0xD1…`, `0xD2…`):** the device is visible, and the exchange with the SD stops. `K` is the kind of error, `EE` the command. Please report codes like this in an Issue, with the photo.

There is no setting to turn this blue screen off. When the code appears the SD cannot be read, so without it Windows would stop anyway because it cannot find the boot drive (`INACCESSIBLE_BOOT_DEVICE`).

### `0xDEADDEAD` (`switchaudio`)

An internal error in the audio driver (a function that must not be called was called). It should not happen. If it does, please report it in an Issue with the driver version.

### Others

- `WHEA_UNCORRECTABLE_ERROR`: happened with `nvtsdhc` 0.2.2 to 0.2.5. Replace it with a newer version.
- Other named codes (`INACCESSIBLE_BOOT_DEVICE` and so on) come from Windows itself. Please report them with your UEFI version, the driver versions and what you changed last.

## Building (PC)

You need:
- LLVM (clang-cl, lld-link)
- WDK headers and libraries (10.0.19041 and 10.0.26100)
- .NET SDK (able to target .NET 8)
- The test-signing certificate ("CN=SwitchWOA Test Driver Signing") in the certificate store. The private key is not in the repository.

```powershell
.\release\Make-Release.ps1 -Version 2026.10.26
```

This makes the four release files in `out\<version>\`:
- `SwitchDrivers-<version>.zip`
- `SwitchCenter-<version>-arm64.zip`
- `SwitchPatcher-<version>-x64.zip`
- `manifest.json`

Create a GitHub release (tag `drivers-<version>`) and attach the four files.

`-Beta` makes a pre-release (beta). Drivers marked `Beta` in the table are only in beta releases. When "Update channel" in the control center is set to beta, pre-releases are offered as updates too.

With the GitHub CLI (logged in with `gh auth login`), `-Publish` also creates the release and uploads the files. To publish files made earlier, use `-PublishOnly`.

## OTA (updates from the control center)

Design: `docs/Design-DriverRepo-OTA-ja.md` (Japanese). The control center reads `manifest.json` of the latest release in this repository and compares it with the installed drivers and versions. Before installing an update it checks three things:
- it was downloaded over HTTPS
- the SHA-256 matches
- it is signed with the test-signing certificate

Installing asks for administrator approval first. The control center, restarted as administrator, downloads the same release from GitHub again by itself, checks the three things, and installs it with `pnputil`.

How to use: in the control center's "Drivers" tab, "Check for updates" → look at the blue rows (updates available) → "Update". Available from control center v0.5.0.

GPIO and I2C are not replaced on the spot, since the other drivers sit on them. They are registered with Windows and replaced at the next boot (v0.5.1 and later).

The control center can update itself from the same tab when a new version exists (v0.5.1 and later). It checks the ZIP's SHA-256, overwrites itself and restarts.

Drivers for new devices can be shipped without changing the control center (v0.5.2 and later). It uses these fields of each driver in the manifest:
- `hwid`: the device to look for
- `nameJa`, `nameEn`: display names
- `installAt`: `boot` (replaced at the next boot) or `live` (installed on the spot)
- `minUefi`: shown as the UEFI version needed when the device is missing

To add a new driver, add one line to the table in `release/Make-Release.ps1`.

## Folders

| Path | Contents |
|---|---|
| `drivers/` | Source and INF of each driver |
| `kits/` | Install scripts and READMEs used on the Switch (the binaries are in the release ZIP) |
| `app/SwitchCenter/` | Control center (.NET 8, WinForms, win-arm64) |
| `app/SwitchPatcher/` | SwitchPatcher (.NET 8, WinForms, win-x64, for the PC) |
| `app/Shared/` | Release reading and checking, used by both |
| `tools/` | `Build-Driver.ps1` (builds KMDF drivers with clang-cl + lld-link and test-signs them), the test certificate's public key |
| `release/` | `Make-Release.ps1` |
| `docs/` | Design notes (Japanese) |

## Credits

Claude: thank you for helping with this GitHub repository, the READMEs and more.

## License

GPL-2.0 (same as Windows-for-Switch). Some of the register sequences follow Hekate and Linux (both GPL-2.0).
