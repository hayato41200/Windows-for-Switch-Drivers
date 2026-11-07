# T214 processor power backend

This is the in-progress Windows PoFx backend for retained-memory sleep on
Switch Lite (HDH-001). It uses the current Switchroot-derived BL31 through
PoFx `ProcessorHalt`; Windows supplies the saved CPU context and resume entry.
It does not call a context-losing SMC from an ordinary kernel worker.

The implemented processor states are WFI, CPU C7 (`0x00010007`) and coordinated
cluster CC6 (`0x01010010`). Deep states are gated on display-off and a locked,
non-overclocked PLLX. Peripherals and DRAM remain powered. SC7, S4 hibernation,
full display/storage context restoration are **not implemented by this
package**. The accompanying SwitchPowerButton 1.0.0.2 candidate supplies the
PMIC wake interrupt and preserves the original polling profile for older firmware. The backend alone is not a complete
low-power sleep implementation.

The ACPI binding is `SWOA0C01`; the boot service belongs to `Core Platform
Extensions`. PoFx cannot unregister a PEP, so servicing requires a restart.
`HaltAttempts`, `HaltReturns`, `LastHaltStatus` and `ProcessorCount` are fixed
registry values updated on display transitions by a worker. An acknowledged
PSCI return is not proof that the firmware granted cluster power-down.

The ARM64 build and actual-callback PoFx dependency/clock/PSCI host test pass. The PEP is not yet installed or physically
validated. Continue the shared work in the firmware repository's
[porting status](https://github.com/ruje0504-2/winx/blob/hdh001-hekate-sd/docs/hdh001-home/PORTING.md).

References: [Microsoft i.MX PEP](https://github.com/ms-iot/imx-iotcore/tree/public_preview/driver/power/imx6pep),
[PoFx ProcessorHalt](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/pepfx/nc-pepfx-pofxcallbackprocessorhalt),
and [pinned Switchroot ATF](https://github.com/LineageOS/android_hardware_nintendo_arm-trusted-firmware/blob/85081cab2050051ea6e93206ef0f80bc0676e613/plat/nvidia/tegra/soc/t210/plat_psci_handlers.c).
