# Switch Lite HDH-001

The Lite build uses the existing Tegra210Gpio and Tegra210I2c drivers and the FTM4 implementation in `drivers/SwitchLiteTouch`. The latter matches ACPI SWOA0502, initializes the controller in Windows, handles quick press/release events and retries HID submission. The public desktop checker is built from `touch-check.c`. Real-hardware testing has confirmed a touch press on the Windows setup dialog. The three-circle TOUCH PASS test is pending.

The build script also produces the existing SwitchPowerButton driver for the optional Lite I2C5/PMIC firmware profile. It reads MAX77620 ONOFFSTAT and ONOFFIRQ and submits HID System Power Down input. Use the firmware build with SWITCH_LITE_POWER=1 and stage switchpwrbtn.inf together with the I2C driver. The package compiles for ARM64; its physical button behavior is pending.

The companion Home firmware provides the GPIO interrupt resources, I²C3, touch power initialization and NTFS boot path. Its Hekate entry is **Lite Windows 11 Home**. [Firmware source](https://github.com/ruje0504-2/winx/tree/hdh001-hekate-sd)

## Compile the Lite driver set

Set WDK_INCLUDE to the WDK Include/version directory, WDK_LIB to Lib/version/km/ARM64, WDF_INCLUDE to Include/wdf/kmdf/1.15, and WDF_LIB to Lib/wdf/kmdf/ARM64/1.15. CLANG_CL and LLD_LINK select the tools. WDK_EXTRA_LIB can select the matching SPB, GPIO, VHF, and battery-class (`battc.lib`) libraries. The script builds GPIO, I²C, Lite touch, power button, fan, battery, and RTC.

```sh
sh docs/hdh001/build-drivers.sh . /path/to/output
python3 docs/hdh001/tests/test-ftm4.py
```

The existing `tools/Build-Driver.ps1` also accepts the Lite source via its Sources argument and VHF libraries via Libs. Sign the final SYS and catalog files with the same development certificate used by the target Windows image. Install the GPIO, I²C and touch packages into the offline ARM64 image with DISM, then place the checker at `Users/Public/Desktop/LiteTouchCheck.exe`. Add the power-button package when using the optional power-enabled firmware.

## SD system partition

The bounded-write profile is in `drivers/Tegra210Sdhc/winarm`. Use the existing target generator with a saved image to produce the private header, and build the optional WINARM profile as described in the SD README. The matching INF is version 0.3.2.5.

The returned in-memory MBR places the configured WINARM partition in slot 1 and sets its active flag. This lets the system-partition check used by Windows Setup accept the already-booted MBR volume. The physical card's identifiers and full write bounds remain part of the selected profile's checks.

For the interrupted Home installation tested here, Windows subsequently required its failed specialize state to be restored before retry. The firmware repository records those setup logs and the recovery steps.

## Shared fan and T214 clock control

`drivers/SwitchFan` now supports both the original SWOA0401 device and Lite SWOA0402. Its common fan/I2C code stays in the upstream source file; only the Mariko clock helper and handoff definition are under `src/t214`. The original device retains its backlight interface. Lite maps PWM1 only and rejects backlight commands.

On Lite, TMP451 uses the Mariko -12.5 C remote offset. The optional firmware handoff requests CPU 1996.8 MHz with MAX77812 M4 at 1120 mV, independently of whether the firmware completed the requested EMC change. The fan timer owns both cooling and CPU transitions. At 80 C or after repeated sensor-read failures it restores the saved boot CPU frequency; at 70 C it can resume the request. It does not touch MAX77812 M1/GPU registers.

Use the matching firmware's `cpu_oc`/`ram_oc` INI options and its LitePerformanceCheck application. Parameters records include CpuActualKhz, RamActualKhz, TempSoC, TempBoard, Duty, SensorFailures and status. Build with `docs/hdh001/build-drivers.sh`; the shared INF is version 1.0.0.4. The existing Erista CPU-clock driver is not loaded for the Lite profile.

The ARM64 build and host CPU-helper checks pass. Run the latter with `cc -std=c11 -Wall -Wextra -Werror drivers/SwitchFan/tests/test-t214-clock.c -o /tmp/test-t214-clock && /tmp/test-t214-clock`. Signed package validation is complete; physical clock, cooling and restart checks remain pending.

## Shared battery, RTC, and volume-button support

The companion firmware's `SWITCH_LITE_SYSTEM=1` option exposes the upstream BAT0, RTC0, and BTNS definitions on Lite. It requires the Lite fan/I2C1 and power/I2C5 profiles. Use `drivers/SwitchBattery` and `drivers/SwitchRtc` directly; no Lite-specific source copies are needed.

- `switchbattery` 1.0.0.6 reads MAX17050 (I2C1, 0x36) and BQ24193 status (I2C1, 0x6B). Lite exposes exactly those two connections. The optional third BM92T connection is absent, so the USB-C role-switching timer and charger writes are not activated. The original Erista three-connection path remains available.
- `switchrtc` 1.0.0.2 reads MAX77620 RTC (I2C5, 0x68). It learns a private Windows time offset after manual/NTP time changes and applies it on later starts, preserving the hardware calendar used by Horizon.
- Volume up/down use the Windows inbox ACPI0011 button driver through the existing GPIO driver: PX6/GPIO190 and PX7/GPIO191, with both-edge interrupts. This adds input support, not an audio endpoint.

The ARM64 build, signed SYS/catalog verification, and PnPUtil staging in ARM64 WinPE 22621.1 pass. Sign the final INF/SYS catalogs and use [Install-LiteSystem.cmd](Install-LiteSystem.cmd) beside the `switchfan`, `switchpwrbtn`, `switchbattery`, `switchrtc`, `winxdisplay`, and `switchaudio` package directories. Include LiteLandscape.exe for the default orientation. Run it with `online` from an elevated Home prompt, or `offline W:\` from ARM64 maintenance WinPE with WINARM's actual drive letter. Existing GPIO/I²C drivers and the Stage 6 test certificate are prerequisites. Battery readings, learned RTC time across boots, and physical volume-key input remain unverified on HDH-001.

## EMC failure and fan-floor correction

The first physical performance run reported CPU 998.4 MHz, failed BPMP initialization, and a running temperature timer at 34 C / 33 C with duty zero. SwitchFan 1.0.0.4 separates CPU preparation from EMC status and keeps the minimum fan duty for a valid configured CPU request. PMIC voltage/readback, CPU clock checks, and thermal fallback remain in place. The companion firmware corrects the ATOMICS trigger/result registers and maps the Mariko BPMP mailbox carveout. Target clock rates and cooling after this correction still await hardware confirmation.

## Shared audio and display integration

`drivers/SwitchAudio` remains the upstream PortCls/WaveRT implementation. Its ARM64 cross-build script uses `portcls.lib` and the same resident-section/native-driver linker settings as `tools/Build-Driver.ps1`. The companion firmware's `SWITCH_LITE_AUDIO=1` profile prepares APE, I2C1, RT5639, I2S1, and PV6 headphone detection using the shared R403 code. The target is stereo speaker/headphone playback and automatic jack switching; there is no microphone endpoint.

The display-only implementation remains in the [existing WinX source tree](https://github.com/ruje0504-2/winx/tree/hdh001-hekate-sd/winx-display), avoiding a second source copy here. Its Lite profile binds ACPI WINX0001 and uses software framebuffer rotation. The companion sign-in helper saves 1280x720 landscape once per user. It does not add GPU acceleration.

The combined firmware and ARM64 audio/display binaries build. The signed audio package stages successfully in ARM64 Windows. In the ARM64 VM, the display driver reports Started and rotation/restore succeed. The combined firmware and six complete INF/CAT/SYS packages have been deployed to the existing WINARM installation, preserving its completed setup state. Physical audio, orientation, and touch alignment are pending. The single Hekate entry remains **Lite Windows 11 Home**.
