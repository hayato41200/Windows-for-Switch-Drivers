/*
 * Nintendo Switch power button driver, KMDF + Virtual HID Framework (VHF).
 *
 * ACPI device PWRB (ACPI\SWOA0601):
 *   I2cSerialBus 0x3C on I2C5: MAX77620 PMIC. The power button drives its
 *   EN0 input; there is no GPIO for it on the SoC.
 * Every 50 ms the driver reads (Hekate bdk/power/max77620.h):
 *   ONOFFSTAT 0x15 bit 2 (EN0): 1 while the button is held.
 *   ONOFFIRQ  0x0B (clear on read): bit 3 EN0 rising, bit 2 EN0 falling,
 *     so a press shorter than one poll is still seen.
 * Changes are reported as a HID System Control "System Power Down" button;
 * Windows then applies the power button action from Power Options.
 * Holding the button for several seconds still powers the PMIC off by itself.
 *
 * Restart and shut down (driver version 2):
 * Windows on the Switch finishes its whole shutdown, but its last firmware
 * call never resets or powers off the console (it never reaches the UEFI
 * ResetSystem). This driver does it itself in EvtDeviceD0Exit (D3Final/D3,
 * WdfDeviceGetSystemPowerAction = a shutdown action), when the power manager
 * powers the devices down for S5; the file
 * systems and the registry hives are flushed before that.
 *
 *   PowerActionShutdownReset: reboot-to-payload (R2P), the same steps as
 *   switchroot bl31 (switch-atf plat/nvidia/tegra/soc/t210/plat_sip_calls.c
 *   r2p_reboot_to_payload, plat_psci_handlers.c tegra_soc_prepare_system_reset):
 *     CAR SCLK_BURST_POLICY (0x28) = 0x10000000, SCLK_RATE (0x30) = 0, 1 ms
 *     clear IRAM 0x40010000-0x4003FFFF, copy the payload to 0x40010000,
 *       write the 32-byte boot configuration at +0x94 (bl31 r2p_bcfg; 0 = menu)
 *     PMC_SCRATCH0 (0x50) |= WARMBOOT (bit 0)
 *     PMC_SCRATCH45 (0x234) = 0x2E38DFFF, PMC_SCRATCH46 (0x238) = 0x6001DC28
 *     PMC_SCRATCH33 (0x120) = 0x40010000, PMC_SCRATCH40 (0x13C) = 0x6000F208
 *     PMC_CNTRL (0x0) |= MAIN_RST (0x10)        (pmc.c RESET_ENABLE)
 *   Erista only (APB_MISC GP_HIDREV major < 2). The payload (hekate's
 *   bootloader/update.bin by default) is read from the SD's FAT partition
 *   while Windows runs, never at shutdown time. Without a payload the driver
 *   calls PSCI SYSTEM_RESET: switchroot's bl31 then does R2P with the payload
 *   hekate gave it (as switchroot u-boot's reset_misc), coreboot's bl31 does a
 *   plain reset.
 *
 *   PowerActionShutdown/ShutdownOff: MAX77620 ONOFFCNFG2 (0x42) &= ~SFT_RST_WK
 *   (bit 7), then ONOFFCNFG1 (0x41) = SFT_RST (bit 7): power down, no wake on
 *   the soft reset (Linux max77620_pm_power_off, hekate POWER_OFF_RESET).
 *   ONOFFCNFG1 PWR_OFF (bit 1) as a fallback. Through SPB first; if that fails
 *   (I2C controller already out of D0), the I2C5 registers (0x7000D000)
 *   directly in packet mode, as drivers/Tegra210I2c does. tegra210i2c has no
 *   D0Exit, so the controller is still clocked and configured.
 *
 * Before acting, the backlight PWM (0x7000A000 CSR_0) is switched off, so a
 * console that stays black with the backlight off shows the driver got there.
 *
 * Parameters (service key\Parameters; read at start and every 10 s):
 *   ResetEnable    1 (default) / 0 = leave restart and shut down to Windows
 *   RestartMethod  0 auto (payload: R2P, none: PSCI), 1 PMC only (R2P with a
 *                  payload, plain reset without), 2 PSCI SYSTEM_RESET,
 *                  3 plain PMC reset, 4 backlight off only (test)
 *   PowerOffMethod 0 PMIC (default), 1 backlight off only (test), 2 Windows
 *   PayloadPath    REG_SZ, default \Device\Harddisk0\Partition1\bootloader\update.bin
 *   RebootToId     REG_SZ, up to 7 printable ASCII characters: after a restart hekate
 *                  autoboots the hekate_ipl.ini entry with this id= key (hekate
 *                  boot_cfg_t: boot_cfg = AUTOBOOT_EN | FROM_ID, id[8]). Empty
 *                  (default): the hekate menu.
 *   RebootToIndex  DWORD, used when RebootToId is empty: autoboot entry N of
 *                  hekate_ipl.ini (the first entry after [config] is 1); 0 = menu.
 *   Holding VOL- while hekate starts still opens its menu. The boot configuration
 *   goes to the payload at +0x94 (R2P from Windows) or, before PSCI, to
 *   switchroot's bl31 through SiP 0xC2FFFE04 (R2P_SET_CFG).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include <vhf.h>

#define MAX77620_ONOFFIRQ    0x0B
#define MAX77620_ONOFFSTAT   0x15
#define ONOFFSTAT_EN0        0x04
#define ONOFFIRQ_EN0_F       0x04
#define ONOFFIRQ_EN0_R       0x08
#define POLL_MS              50
#define REPORT_ID_SYSTEM     1
#define DRIVER_VERSION       3

/* MAX77620 power-off (Linux include/linux/mfd/max77620.h) */
#define MAX77620_I2C_ADDR    0x3C
#define MAX77620_ONOFFCNFG1  0x41
#define MAX77620_ONOFFCNFG2  0x42
#define ONOFFCNFG1_SFT_RST   0x80
#define ONOFFCNFG1_PWR_OFF   0x02
#define ONOFFCNFG2_SFT_RST_WK 0x80

/* Tegra210 (switch-atf plat/nvidia/tegra/include/t210/tegra_def.h, drivers/pmc.h) */
#define T210_CAR_BASE        0x60006000ull
#define T210_CAR_SIZE        0x1000
#define CAR_SCLK_BURST_POLICY 0x028
#define CAR_SCLK_RATE        0x030
#define SCLK_BURST_POLICY_POR 0x10000000u
#define T210_PMC_BASE        0x7000E400ull
#define T210_PMC_SIZE        0x400
#define PMC_CNTRL            0x000
#define PMC_CNTRL_MAIN_RST   0x10u
#define PMC_SCRATCH0         0x050
#define PMC_SCRATCH0_WARMBOOT 0x01u
#define PMC_SCRATCH33        0x120
#define PMC_SCRATCH40        0x13C
#define PMC_SCRATCH45        0x234
#define PMC_SCRATCH46        0x238
#define T210_MISC_BASE       0x70000000ull
#define T210_MISC_SIZE       0x1000
#define MISC_GP_HIDREV       0x804
#define T210_IRAM_PAYLOAD    0x40010000ull
#define T210_IRAM_PAYLOAD_SIZE 0x30000u     /* to the end of IRAM, 0x40040000 */
#define PAYLOAD_BOOTCFG      0x94
#define PAYLOAD_BOOTCFG_SIZE 32             /* bl31 r2p_bcfg[4] */
#define PAYLOAD_MIN_SIZE     0x200
#define T210_PWM_BASE        0x7000A000ull
#define T210_PWM_SIZE        0x1000
#define PWM_CSR_0            0x000
#define T210_I2C5_BASE       0x7000D000ull
#define T210_I2C_SIZE        0x100
#define PSCI_SYSTEM_RESET    0x84000009ull
#define TEGRA_SIP_R2P_SET_CFG 0xC2FFFE04ull     /* switch-atf plat_sip_calls.c */

/* hekate boot_cfg_t (bdk/utils/types.h): boot_cfg, autoboot, autoboot_list, extra_cfg, id[8] */
#define BOOT_CFG_AUTOBOOT_EN 0x01
#define BOOT_CFG_FROM_ID     0x04
#define BOOT_ID_CHARS        7

/* Tegra I2C controller (drivers/Tegra210I2c) */
#define I2C_TX_FIFO          0x050
#define I2C_RX_FIFO          0x054
#define I2C_FIFO_CONTROL     0x05C
#define I2C_FIFO_STATUS      0x060
#define I2C_INT_STATUS       0x068
#define I2C_INT_ERR_MASK     0x3Cu
#define I2C_INT_DONE_MASK    0xC0u          /* ALL_PACKETS | PACKET_DONE */
#define I2C_HDR0_PACKET1     0x10010u
#define I2C_HDR_REPEAT_START (1u << 16)
#define I2C_HDR_READ         (1u << 19)

#define RESTART_AUTO         0
#define RESTART_PMC          1
#define RESTART_PSCI         2
#define RESTART_PLAIN        3
#define RESTART_MARKER       4
#define POWEROFF_PMIC        0
#define POWEROFF_MARKER      1
#define POWEROFF_WINDOWS     2
#define PAYLOAD_TRIES        24             /* every 5 s for 2 minutes */
#define PAYLOAD_PATH_CHARS   200

static const WCHAR SwPwrDefaultPayloadPath[] = L"\\Device\\Harddisk0\\Partition1\\bootloader\\update.bin";

/* System Control: bit 0 System Power Down (0x81), 7 bits padding. */
static UCHAR SwPwrReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x80, 0xA1, 0x01,
    0x85, REPORT_ID_SYSTEM,
    0x09, 0x81, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x95, 0x07, 0x81, 0x01,
    0xC0
};

typedef struct _SWPWR_DEVICE {
    LARGE_INTEGER I2cId;
    WDFIOTARGET I2c;
    WDFTIMER Timer;
    VHFHANDLE Vhf;
    volatile BOOLEAN Stopping;
    BOOLEAN Pressed;
    UCHAR Report[2];
    ULONG Failures;
    LONG Polls;
    LONG Presses;
    LONG Reports;
    /* restart and shut down */
    volatile UCHAR *Car, *Pmc, *Iram, *Pwm, *I2c5;
    ULONG Hidrev;
    BOOLEAN Erista;
    ULONG ResetEnable, RestartMethod, PowerOffMethod;
    WCHAR PayloadPath[PAYLOAD_PATH_CHARS];
    PUCHAR Payload;
    ULONG PayloadSize;        /* bytes in the file */
    ULONG PayloadTries;
    DECLSPEC_ALIGN(8) UCHAR BootCfg[PAYLOAD_BOOTCFG_SIZE];   /* hekate boot_cfg_t, first 32 bytes */
} SWPWR_DEVICE, *PSWPWR_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_DEVICE, SwPwrGetDevice)

typedef struct _SWPWR_TIMER {
    WDFDEVICE Device;
} SWPWR_TIMER, *PSWPWR_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWPWR_TIMER, SwPwrGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwPwrEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwPwrEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwPwrEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwPwrEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwPwrEvtSelfManagedIoCleanup;
EVT_WDF_DEVICE_D0_EXIT SwPwrEvtD0Exit;
EVT_WDF_TIMER SwPwrEvtTimer;

static WCHAR SwPwrParamsPath[256];

static VOID SwPwrTrace(PCWSTR Name, ULONG Value)
{
    if (SwPwrParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* ------------------------------------------------------------------ MMIO */

static ULONG Rd32(volatile UCHAR *Base, ULONG Offset)
{
    return READ_REGISTER_ULONG((volatile ULONG *)(Base + Offset));
}

static VOID Wr32(volatile UCHAR *Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Offset), Value);
}

static VOID SwPwrDsb(VOID)
{
    __asm__ volatile("dsb sy" : : : "memory");
}

static volatile UCHAR *SwPwrMap(ULONG64 Phys, SIZE_T Size)
{
    PHYSICAL_ADDRESS Pa;
    Pa.QuadPart = (LONGLONG)Phys;
    return (volatile UCHAR *)MmMapIoSpaceEx(Pa, Size, PAGE_READWRITE | PAGE_NOCACHE);
}

static VOID SwPwrUnmap(volatile UCHAR **Va, SIZE_T Size)
{
    if (*Va != NULL) {
        MmUnmapIoSpace((PVOID)*Va, Size);
        *Va = NULL;
    }
}

/* SMC Calling Convention: function in x0, result in x0; x4-x17 may be clobbered. */
static ULONG64 SwPwrSmc(ULONG64 Function, ULONG64 A1, ULONG64 A2, ULONG64 A3, ULONG64 A4)
{
    register ULONG64 X0 __asm__("x0") = Function;
    register ULONG64 X1 __asm__("x1") = A1;
    register ULONG64 X2 __asm__("x2") = A2;
    register ULONG64 X3 __asm__("x3") = A3;
    register ULONG64 X4 __asm__("x4") = A4;
    __asm__ volatile("smc #0"
                     : "+r"(X0), "+r"(X1), "+r"(X2), "+r"(X3), "+r"(X4)
                     :
                     : "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17", "memory");
    return X0;
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwPwrOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, Target);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

static NTSTATUS SwPwrReadReg(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwPwrWriteReg(WDFIOTARGET Target, UCHAR Register, UCHAR Value)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(1) Sequence;
    UCHAR Buffer[2];
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }
    Buffer[0] = Register;
    Buffer[1] = Value;
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 1);
    Sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, Buffer, 2);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/*
 * Direct I2C5 access for the power-off, used only when SPB fails at shutdown.
 * Polling, one packet per message, at any IRQL.
 */
static VOID SwPwrI2cFlush(volatile UCHAR *I2c)
{
    ULONG Waited;
    Wr32(I2c, I2C_INT_STATUS, 0xFFFFFFFF);
    Wr32(I2c, I2C_FIFO_CONTROL, Rd32(I2c, I2C_FIFO_CONTROL) | 0x3u);
    for (Waited = 0; Waited < 10000 && (Rd32(I2c, I2C_FIFO_CONTROL) & 0x3u) != 0; Waited += 5) {
        KeStallExecutionProcessor(5);
    }
}

static BOOLEAN SwPwrI2cWaitDone(volatile UCHAR *I2c)
{
    ULONG Waited, Status;
    for (Waited = 0; Waited < 20000; Waited += 5) {
        Status = Rd32(I2c, I2C_INT_STATUS);
        if ((Status & I2C_INT_ERR_MASK) != 0) {
            return FALSE;
        }
        if ((Status & I2C_INT_DONE_MASK) != 0) {
            Wr32(I2c, I2C_INT_STATUS, Status);
            return TRUE;
        }
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

static BOOLEAN SwPwrI2cWrite(volatile UCHAR *I2c, UCHAR Address, UCHAR Register, UCHAR Value)
{
    SwPwrI2cFlush(I2c);
    Wr32(I2c, I2C_TX_FIFO, I2C_HDR0_PACKET1);
    Wr32(I2c, I2C_TX_FIFO, 2 - 1);
    Wr32(I2c, I2C_TX_FIFO, (ULONG)Address << 1);
    Wr32(I2c, I2C_TX_FIFO, ((ULONG)Value << 8) | Register);
    return SwPwrI2cWaitDone(I2c);
}

static BOOLEAN SwPwrI2cRead(volatile UCHAR *I2c, UCHAR Address, UCHAR Register, PUCHAR Value)
{
    ULONG Waited;
    SwPwrI2cFlush(I2c);
    Wr32(I2c, I2C_TX_FIFO, I2C_HDR0_PACKET1);
    Wr32(I2c, I2C_TX_FIFO, 1 - 1);
    Wr32(I2c, I2C_TX_FIFO, ((ULONG)Address << 1) | I2C_HDR_REPEAT_START);
    Wr32(I2c, I2C_TX_FIFO, Register);
    if (!SwPwrI2cWaitDone(I2c)) {
        return FALSE;
    }
    Wr32(I2c, I2C_TX_FIFO, I2C_HDR0_PACKET1);
    Wr32(I2c, I2C_TX_FIFO, 1 - 1);
    Wr32(I2c, I2C_TX_FIFO, ((ULONG)Address << 1) | I2C_HDR_READ);
    for (Waited = 0; Waited < 20000; Waited += 5) {
        if ((Rd32(I2c, I2C_INT_STATUS) & I2C_INT_ERR_MASK) != 0) {
            return FALSE;
        }
        if ((Rd32(I2c, I2C_FIFO_STATUS) & 0x0F) != 0) {          /* RX words available */
            *Value = (UCHAR)Rd32(I2c, I2C_RX_FIFO);
            return SwPwrI2cWaitDone(I2c);
        }
        KeStallExecutionProcessor(5);
    }
    return FALSE;
}

/* ---------------------------------------------------------------- button */

static VOID SwPwrReport(PSWPWR_DEVICE Dev, BOOLEAN Pressed)
{
    HID_XFER_PACKET Packet;
    NTSTATUS Status;

    Dev->Report[0] = REPORT_ID_SYSTEM;
    Dev->Report[1] = Pressed ? 0x01 : 0x00;
    Packet.reportBuffer = Dev->Report;
    Packet.reportBufferLen = sizeof(Dev->Report);
    Packet.reportId = REPORT_ID_SYSTEM;
    Status = VhfReadReportSubmit(Dev->Vhf, &Packet);
    if (NT_SUCCESS(Status)) {
        SwPwrTrace(L"Reports", (ULONG)InterlockedIncrement(&Dev->Reports));
    } else {
        SwPwrTrace(L"LastReportError", (ULONG)Status);
    }
}

/* ------------------------------------------------- configuration, payload */

static VOID SwPwrFreePayload(PSWPWR_DEVICE Dev)
{
    if (Dev->Payload != NULL) {
        ExFreePoolWithTag(Dev->Payload, 'RwpS');
        Dev->Payload = NULL;
    }
    Dev->PayloadSize = 0;
}

static VOID SwPwrReadConfig(PSWPWR_DEVICE Dev)
{
    DECLARE_CONST_UNICODE_STRING(NameEnable, L"ResetEnable");
    DECLARE_CONST_UNICODE_STRING(NameRestart, L"RestartMethod");
    DECLARE_CONST_UNICODE_STRING(NamePowerOff, L"PowerOffMethod");
    DECLARE_CONST_UNICODE_STRING(NamePath, L"PayloadPath");
    DECLARE_CONST_UNICODE_STRING(NameId, L"RebootToId");
    DECLARE_CONST_UNICODE_STRING(NameIndex, L"RebootToIndex");
    WCHAR Buffer[PAYLOAD_PATH_CHARS];
    WCHAR IdBuffer[16];
    DECLSPEC_ALIGN(8) UCHAR BootCfg[PAYLOAD_BOOTCFG_SIZE];
    UNICODE_STRING Path, IdString;
    BOOLEAN IdOk = TRUE;
    WDFKEY Key;
    ULONG Value, Index;

    RtlZeroMemory(BootCfg, sizeof(BootCfg));
    RtlZeroMemory(IdBuffer, sizeof(IdBuffer));

    Dev->ResetEnable = 1;
    Dev->RestartMethod = RESTART_AUTO;
    Dev->PowerOffMethod = POWEROFF_PMIC;
    RtlZeroMemory(Buffer, sizeof(Buffer));
    RtlCopyMemory(Buffer, SwPwrDefaultPayloadPath, sizeof(SwPwrDefaultPayloadPath));
    if (NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_READ, WDF_NO_OBJECT_ATTRIBUTES, &Key))) {
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &NameEnable, &Value))) {
            Dev->ResetEnable = Value;
        }
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &NameRestart, &Value))) {
            Dev->RestartMethod = Value;
        }
        if (NT_SUCCESS(WdfRegistryQueryULong(Key, &NamePowerOff, &Value))) {
            Dev->PowerOffMethod = Value;
        }
        /* Where hekate goes after a restart: an ini entry by its id= key, or by index, or the menu. */
        RtlInitEmptyUnicodeString(&IdString, IdBuffer, sizeof(IdBuffer) - sizeof(WCHAR));
        if (NT_SUCCESS(WdfRegistryQueryUnicodeString(Key, &NameId, NULL, &IdString))) {
            for (Index = 0; Index < IdString.Length / sizeof(WCHAR); ++Index) {
                if (IdBuffer[Index] <= L' ' || IdBuffer[Index] > L'~' || Index >= BOOT_ID_CHARS) {
                    IdOk = FALSE;           /* printable ASCII, at most 7 characters */
                }
            }
            if (IdOk && IdString.Length != 0) {
                BootCfg[0] = BOOT_CFG_AUTOBOOT_EN | BOOT_CFG_FROM_ID;
                for (Index = 0; Index < IdString.Length / sizeof(WCHAR); ++Index) {
                    BootCfg[4 + Index] = (UCHAR)IdBuffer[Index];
                }
            }
        }
        if (BootCfg[0] == 0 && NT_SUCCESS(WdfRegistryQueryULong(Key, &NameIndex, &Value)) && Value != 0 && Value < 256) {
            BootCfg[0] = BOOT_CFG_AUTOBOOT_EN;
            BootCfg[1] = (UCHAR)Value;      /* entry number in hekate_ipl.ini, first after [config] = 1 */
        }
        RtlInitEmptyUnicodeString(&Path, Buffer, sizeof(Buffer) - sizeof(WCHAR));
        if (NT_SUCCESS(WdfRegistryQueryUnicodeString(Key, &NamePath, NULL, &Path)) && Path.Length != 0) {
            Buffer[Path.Length / sizeof(WCHAR)] = L'\0';
        } else {
            RtlZeroMemory(Buffer, sizeof(Buffer));
            RtlCopyMemory(Buffer, SwPwrDefaultPayloadPath, sizeof(SwPwrDefaultPayloadPath));
        }
        WdfRegistryClose(Key);
    }
    if (RtlCompareMemory(Buffer, Dev->PayloadPath, sizeof(Buffer)) != sizeof(Buffer)) {
        RtlCopyMemory(Dev->PayloadPath, Buffer, sizeof(Buffer));
        SwPwrFreePayload(Dev);          /* path changed: load again */
        Dev->PayloadTries = 0;
    }
    SwPwrTrace(L"CfgResetEnable", Dev->ResetEnable);
    SwPwrTrace(L"CfgRestartMethod", Dev->RestartMethod);
    RtlCopyMemory(Dev->BootCfg, BootCfg, sizeof(BootCfg));
    SwPwrTrace(L"CfgPowerOffMethod", Dev->PowerOffMethod);
    SwPwrTrace(L"CfgBootCfg", *(PULONG)&BootCfg[0]);       /* boot_cfg, autoboot, list, extra */
    SwPwrTrace(L"CfgBootIdOk", IdOk);
}

/* Read the payload into nonpaged memory. PASSIVE_LEVEL, while the system runs. */
static NTSTATUS SwPwrLoadPayload(PSWPWR_DEVICE Dev)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Attributes;
    IO_STATUS_BLOCK Iosb;
    FILE_STANDARD_INFORMATION Info;
    LARGE_INTEGER Offset;
    HANDLE File = NULL;
    PUCHAR Buffer;
    ULONG Size;
    NTSTATUS Status;

    RtlInitUnicodeString(&Name, Dev->PayloadPath);
    InitializeObjectAttributes(&Attributes, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Attributes, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
                          FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status)) {
        SwPwrTrace(L"PayloadFileSize", Info.EndOfFile.HighPart != 0 ? 0xFFFFFFFF : Info.EndOfFile.LowPart);
        if (Info.EndOfFile.QuadPart > T210_IRAM_PAYLOAD_SIZE) {
            Status = STATUS_FILE_TOO_LARGE;
        } else if (Info.EndOfFile.QuadPart < PAYLOAD_MIN_SIZE) {
            Status = STATUS_INVALID_IMAGE_FORMAT;
        }
    }
    if (NT_SUCCESS(Status)) {
        Size = Info.EndOfFile.LowPart;
        Buffer = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, (Size + 3) & ~3u, 'RwpS');   /* zeroed */
        if (Buffer == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            Offset.QuadPart = 0;
            Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Buffer, Size, &Offset, NULL);
            if (NT_SUCCESS(Status) && Iosb.Information != Size) {
                Status = STATUS_END_OF_FILE;
            }
            if (NT_SUCCESS(Status)) {
                SwPwrFreePayload(Dev);
                Dev->Payload = Buffer;
                Dev->PayloadSize = Size;
                SwPwrTrace(L"PayloadWord0", *(PULONG)Buffer);
            } else {
                ExFreePoolWithTag(Buffer, 'RwpS');
            }
        }
    }
    ZwClose(File);
    return Status;
}

/* -------------------------------------------------- restart and shut down */

static VOID SwPwrBacklightOff(PSWPWR_DEVICE Dev)
{
    if (Dev->Pwm != NULL) {
        Wr32(Dev->Pwm, PWM_CSR_0, 0);       /* duty 0, PWM disabled */
        SwPwrDsb();
    }
}

static VOID SwPwrRestart(PSWPWR_DEVICE Dev)
{
    ULONG Method = Dev->RestartMethod, Offset;
    BOOLEAN UsePayload;
    KIRQL OldIrql;

    if (Method == RESTART_AUTO) {
        Method = (Dev->Payload != NULL && Dev->Erista && Dev->Iram != NULL) ? RESTART_PMC : RESTART_PSCI;
    }
    SwPwrBacklightOff(Dev);
    if (Method == RESTART_MARKER) {
        return;
    }
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    if ((Method == RESTART_PMC || Method == RESTART_PLAIN) && Dev->Car != NULL && Dev->Pmc != NULL) {
        UsePayload = (BOOLEAN)(Method == RESTART_PMC && Dev->Payload != NULL && Dev->Erista && Dev->Iram != NULL);

        /* SCLK to its POR default so the PMC APB clock source does not change on reset. */
        Wr32(Dev->Car, CAR_SCLK_BURST_POLICY, SCLK_BURST_POLICY_POR);
        Wr32(Dev->Car, CAR_SCLK_RATE, 0);
        SwPwrDsb();
        KeStallExecutionProcessor(1000);

        if (UsePayload) {
            for (Offset = 0; Offset < T210_IRAM_PAYLOAD_SIZE; Offset += 4) {
                Wr32(Dev->Iram, Offset, 0);
            }
            for (Offset = 0; Offset < Dev->PayloadSize; Offset += 4) {
                Wr32(Dev->Iram, Offset, *(PULONG)(Dev->Payload + Offset));
            }
            for (Offset = 0; Offset < PAYLOAD_BOOTCFG_SIZE; Offset += 4) {
                Wr32(Dev->Iram, PAYLOAD_BOOTCFG + Offset, *(PULONG)&Dev->BootCfg[Offset]);
            }
            SwPwrDsb();
            Wr32(Dev->Pmc, PMC_SCRATCH0, Rd32(Dev->Pmc, PMC_SCRATCH0) | PMC_SCRATCH0_WARMBOOT);
            /* BootROM SDRAM init patched to an SVC right after the second write ... */
            Wr32(Dev->Pmc, PMC_SCRATCH45, 0x2E38DFFF);
            Wr32(Dev->Pmc, PMC_SCRATCH46, 0x6001DC28);
            /* ... and the SVC handler jumps to the payload in IRAM. */
            Wr32(Dev->Pmc, PMC_SCRATCH33, (ULONG)T210_IRAM_PAYLOAD);
            Wr32(Dev->Pmc, PMC_SCRATCH40, 0x6000F208);
            SwPwrDsb();
        }
        Wr32(Dev->Pmc, PMC_CNTRL, Rd32(Dev->Pmc, PMC_CNTRL) | PMC_CNTRL_MAIN_RST);
        SwPwrDsb();
        KeStallExecutionProcessor(100000);
    }
    /* PSCI method, or the PMC reset did not happen. */
    /* switchroot bl31 copies r2p_bcfg to the payload at +0x94; other firmware answers SMC_UNK. */
    if (Dev->BootCfg[0] != 0) {
        (VOID)SwPwrSmc(TEGRA_SIP_R2P_SET_CFG, *(PULONG64)&Dev->BootCfg[0], *(PULONG64)&Dev->BootCfg[8],
                       *(PULONG64)&Dev->BootCfg[16], *(PULONG64)&Dev->BootCfg[24]);
    }
    (VOID)SwPwrSmc(PSCI_SYSTEM_RESET, 0, 0, 0, 0);
    KeStallExecutionProcessor(100000);
    KeLowerIrql(OldIrql);
}

static VOID SwPwrPowerOff(PSWPWR_DEVICE Dev)
{
    LARGE_INTEGER Delay;
    UCHAR Cfg2 = 0;
    KIRQL OldIrql;
    NTSTATUS Status;

    if (Dev->PowerOffMethod == POWEROFF_WINDOWS) {
        return;
    }
    SwPwrBacklightOff(Dev);
    if (Dev->PowerOffMethod == POWEROFF_MARKER) {
        return;
    }
    Delay.QuadPart = WDF_REL_TIMEOUT_IN_MS(300);

    /* 1. Through SPB, if the I2C controller still takes requests. */
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFCNFG2, &Cfg2);
    if (NT_SUCCESS(Status)) {
        Status = SwPwrWriteReg(Dev->I2c, MAX77620_ONOFFCNFG2, (UCHAR)(Cfg2 & ~ONOFFCNFG2_SFT_RST_WK));
    }
    if (NT_SUCCESS(Status)) {
        (VOID)SwPwrWriteReg(Dev->I2c, MAX77620_ONOFFCNFG1, ONOFFCNFG1_SFT_RST);
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
    }

    /* 2. Still running: the I2C5 registers directly. */
    if (Dev->I2c5 == NULL) {
        return;
    }
    KeRaiseIrql(HIGH_LEVEL, &OldIrql);
    if (SwPwrI2cRead(Dev->I2c5, MAX77620_I2C_ADDR, MAX77620_ONOFFCNFG2, &Cfg2)) {
        (VOID)SwPwrI2cWrite(Dev->I2c5, MAX77620_I2C_ADDR, MAX77620_ONOFFCNFG2, (UCHAR)(Cfg2 & ~ONOFFCNFG2_SFT_RST_WK));
    }
    (VOID)SwPwrI2cWrite(Dev->I2c5, MAX77620_I2C_ADDR, MAX77620_ONOFFCNFG1, ONOFFCNFG1_SFT_RST);
    KeStallExecutionProcessor(300000);
    (VOID)SwPwrI2cWrite(Dev->I2c5, MAX77620_I2C_ADDR, MAX77620_ONOFFCNFG1, ONOFFCNFG1_PWR_OFF);
    KeStallExecutionProcessor(300000);
    KeLowerIrql(OldIrql);
}

NTSTATUS SwPwrEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    POWER_ACTION Action;

    /* S5 normally comes as D3Final; accept D3 too, the system power action decides. */
    if (TargetState != WdfPowerDeviceD3Final && TargetState != WdfPowerDeviceD3) {
        return STATUS_SUCCESS;
    }
    Action = WdfDeviceGetSystemPowerAction(Device);
    if (Dev->ResetEnable == 0 ||
        (Action != PowerActionShutdownReset && Action != PowerActionShutdown && Action != PowerActionShutdownOff)) {
        return STATUS_SUCCESS;          /* device removal, driver update, hibernate */
    }
    /* The registry is closed by now: no traces from here on. */
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Action == PowerActionShutdownReset) {
        SwPwrRestart(Dev);
    } else {
        SwPwrPowerOff(Dev);
    }
    return STATUS_SUCCESS;              /* only reached if nothing happened: Windows goes on */
}

/* ----------------------------------------------------------------- timer */

VOID SwPwrEvtTimer(WDFTIMER Timer)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(SwPwrGetTimer(Timer)->Device);
    UCHAR Irq = 0, Stat = 0;
    BOOLEAN Held;
    LONG Polls;
    NTSTATUS Status;

    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);
    if (NT_SUCCESS(Status)) {
        Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    }
    if (NT_SUCCESS(Status)) {
        Dev->Failures = 0;
        Held = (Stat & ONOFFSTAT_EN0) != 0;
        if (Held != Dev->Pressed) {
            if (Held) {
                SwPwrTrace(L"Presses", (ULONG)InterlockedIncrement(&Dev->Presses));
            }
            Dev->Pressed = Held;
            SwPwrReport(Dev, Held);
        } else if (!Held && (Irq & ONOFFIRQ_EN0_R) != 0) {
            /* pressed and released between two polls */
            SwPwrTrace(L"Presses", (ULONG)InterlockedIncrement(&Dev->Presses));
            SwPwrReport(Dev, TRUE);
            SwPwrReport(Dev, FALSE);
        }
        if (Irq != 0) {
            SwPwrTrace(L"LastOnOffIrq", Irq);
        }
    } else if (++Dev->Failures <= 3 || (Dev->Failures % 100) == 0) {
        SwPwrTrace(L"LastI2cError", (ULONG)Status);
        SwPwrTrace(L"Failures", Dev->Failures);
    }
    Polls = InterlockedIncrement(&Dev->Polls);
    if ((Polls % 200) == 0) {
        SwPwrTrace(L"Polls", (ULONG)Polls);
        SwPwrTrace(L"LastOnOffStat", Stat);
        SwPwrReadConfig(Dev);                       /* every 10 s */
    }
    /* Payload: first poll, then every 5 s until it loads (the SD volume may come up later). */
    if (Dev->Payload == NULL && Dev->PayloadTries < PAYLOAD_TRIES && (Polls % 100) == 1) {
        Status = SwPwrLoadPayload(Dev);
        SwPwrTrace(L"PayloadTries", ++Dev->PayloadTries);
        SwPwrTrace(L"PayloadStatus", (ULONG)Status);
        SwPwrTrace(L"PayloadSize", Dev->PayloadSize);
    }
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot passive timer, re-armed */
    }
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwPwrEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index, Mapped = 0;
    volatile UCHAR *Misc;
    UNREFERENCED_PARAMETER(Raw);

    Misc = SwPwrMap(T210_MISC_BASE, T210_MISC_SIZE);
    if (Misc != NULL) {
        Dev->Hidrev = Rd32(Misc, MISC_GP_HIDREV);
        SwPwrUnmap(&Misc, T210_MISC_SIZE);
    }
    /* chip id 0x21 (T210), major revision < 2: Erista. Mariko (T210B01) cannot do R2P. */
    Dev->Erista = (BOOLEAN)(((Dev->Hidrev >> 8) & 0xFF) == 0x21 && ((Dev->Hidrev >> 4) & 0xF) < 2);
    Dev->Car = SwPwrMap(T210_CAR_BASE, T210_CAR_SIZE);
    Dev->Pmc = SwPwrMap(T210_PMC_BASE, T210_PMC_SIZE);
    Dev->Iram = SwPwrMap(T210_IRAM_PAYLOAD, T210_IRAM_PAYLOAD_SIZE);
    Dev->Pwm = SwPwrMap(T210_PWM_BASE, T210_PWM_SIZE);
    Dev->I2c5 = SwPwrMap(T210_I2C5_BASE, T210_I2C_SIZE);
    Mapped = (Dev->Car != NULL ? 1u : 0) | (Dev->Pmc != NULL ? 2u : 0) | (Dev->Iram != NULL ? 4u : 0) |
             (Dev->Pwm != NULL ? 8u : 0) | (Dev->I2c5 != NULL ? 16u : 0);
    SwPwrTrace(L"Hidrev", Dev->Hidrev);
    SwPwrTrace(L"Erista", Dev->Erista);
    SwPwrTrace(L"Mapped", Mapped);              /* 0x1F = CAR, PMC, IRAM, PWM, I2C5 */

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            Dev->I2cId.LowPart = Desc->u.Connection.IdLowPart;
            Dev->I2cId.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            return STATUS_SUCCESS;
        }
    }
    return STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS SwPwrEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    SwPwrUnmap(&Dev->Car, T210_CAR_SIZE);
    SwPwrUnmap(&Dev->Pmc, T210_PMC_SIZE);
    SwPwrUnmap(&Dev->Iram, T210_IRAM_PAYLOAD_SIZE);
    SwPwrUnmap(&Dev->Pwm, T210_PWM_SIZE);
    SwPwrUnmap(&Dev->I2c5, T210_I2C_SIZE);
    return STATUS_SUCCESS;
}

NTSTATUS SwPwrEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    VHF_CONFIG Config;
    UCHAR Stat = 0, Irq = 0;
    NTSTATUS Status;

    SwPwrReadConfig(Dev);
    if (Dev->I2c == NULL) {
        Status = SwPwrOpenTarget(Device, Dev->I2cId, &Dev->I2c);
        SwPwrTrace(L"OpenI2cStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
    }
    Status = SwPwrReadReg(Dev->I2c, MAX77620_ONOFFSTAT, &Stat);
    SwPwrTrace(L"FirstReadStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    (VOID)SwPwrReadReg(Dev->I2c, MAX77620_ONOFFIRQ, &Irq);    /* drop edges latched before Windows */
    SwPwrTrace(L"OnOffStatAtStart", Stat);
    SwPwrTrace(L"OnOffIrqAtStart", Irq);
    Dev->Pressed = (Stat & ONOFFSTAT_EN0) != 0;

    if (Dev->Vhf == NULL) {
        VHF_CONFIG_INIT(&Config, WdfDeviceWdmGetDeviceObject(Device), sizeof(SwPwrReportDescriptor),
                        SwPwrReportDescriptor);
        Config.VhfClientContext = Dev;
        Config.VendorID = 0x057E;       /* Nintendo */
        Config.ProductID = 0x5601;
        Config.VersionNumber = 0x0001;
        Status = VhfCreate(&Config, &Dev->Vhf);
        SwPwrTrace(L"VhfCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Vhf = NULL;
            return Status;
        }
        Status = VhfStart(Dev->Vhf);
        SwPwrTrace(L"VhfStartStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            VhfDelete(Dev->Vhf, TRUE);
            Dev->Vhf = NULL;
            return Status;
        }
    }
    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwPwrEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwPwrTrace(L"TimerCreateStatus", (ULONG)Status);
        if (!NT_SUCCESS(Status)) {
            Dev->Timer = NULL;
            return Status;
        }
        SwPwrGetTimer(Dev->Timer)->Device = Device;
    }
    Dev->Stopping = FALSE;
    WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    return STATUS_SUCCESS;
}

VOID SwPwrEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWPWR_DEVICE Dev = SwPwrGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Dev->Vhf != NULL) {
        VhfDelete(Dev->Vhf, TRUE);
        Dev->Vhf = NULL;
    }
    if (Dev->I2c != NULL) {
        WdfIoTargetClose(Dev->I2c);
        WdfObjectDelete(Dev->I2c);
        Dev->I2c = NULL;
    }
    SwPwrFreePayload(Dev);
}

NTSTATUS SwPwrEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwPwrEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwPwrEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwPwrEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwPwrEvtSelfManagedIoCleanup;
    Pnp.EvtDeviceD0Exit = SwPwrEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWPWR_DEVICE);
    return WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwPwrParamsPath)) {
        RtlCopyMemory(SwPwrParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwPwrParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwPwrParamsPath);
    }
    SwPwrTrace(L"DriverVersion", DRIVER_VERSION);
    WDF_DRIVER_CONFIG_INIT(&Config, SwPwrEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
