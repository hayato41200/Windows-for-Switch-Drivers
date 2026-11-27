/*
 * Nintendo Switch battery miniclass driver (battc), KMDF.
 *
 * ACPI device BAT0 (ACPI\SWOA0301) has two I2cSerialBus connections on I2C1:
 *   0: MAX17050 fuel gauge (0x36): RepSOC 0x06 (1/256 %), VCELL 0x09
 *      (bits 15:3 x 0.625 mV), Current 0x0A (signed, 1.5625 uV / 5 mOhm =
 *      0.3125 mA, + = charging), Temp 0x08 (signed 1/256 C)
 *   1: BQ24193 charger (0x6B): SYS_STAT 0x08 (VBUS_STAT 7:6: 1 USB host,
 *      2 adapter, 3 OTG; CHRG_STAT 5:4: 1 pre, 2 fast, 3 done; PG bit 2)
 * Capacity is reported relative (0-100 %), like the gauge's RepSOC. Values
 * and register meanings match what UEFI R363+ prints on the device.
 * The structure follows Microsoft's SimBatt sample.
 *
 * v4: connection 2 (UEFI R378+) is the BM92T USB-C PD controller (0x18).
 * UEFI only sets the USB-C role once at boot, so a device plugged in under
 * Windows got no VBUS. Every 500 ms this driver now repeats UEFI's decision
 * (3 equal samples in a row):
 *   host (INSERT+SRC_MODE+OTG_INSERT, no VBUS from the partner): BQ REG01 OTG,
 *     i.e. the Switch supplies VBUS; unplug turns it off again
 *   sink (INSERT+VSAFE+!SRC_MODE+!DFP, a charger hub): charge mode, input
 *     limit from the PD contract, DR_SWAP to host when the source is DRD
 *     (the same steps as UEFI R363ConfigureSink)
 * VBUS is never enabled while the partner supplies power (VSAFE or BQ
 * VBUS_STAT set). XUSB itself is left running by UEFI R378 in every case.
 *
 * v6: a USB stick plugged in directly stopped being seen after many
 * plug-ins, until a reboot. The BQ boost stops on its own on an overload
 * (inrush at plug-in) and stays off: REG09 BOOST_FAULT, REG08 VBUS_STAT no
 * longer 3 (OTG), while REG01 still says OTG, so the state machine stayed in
 * host with no VBUS. In host the driver now checks REG08/REG09 every poll;
 * after 2 bad polls it turns the boost off and on again (REG01 charge, then
 * OTG), at most 3 times in a row and then once every 5 s. When a partner is
 * inserted but neither role is taken for 5 s, STATUS1/2 are traced.
 *
 * v7: USB-C DisplayPort Alt Mode (the UEFI R424/R425/R432 steps, done again in
 * Windows so a hub/dock plugged in after boot can drive a TV). After the sink
 * setup, when the Switch is DFP: Discover Identity; a partner answering as the
 * Nintendo dock (057E:2003) that also offers the standard DP SVID (FF01) and
 * any other DP partner enter standard DP; the real dock enters Nintendo mode
 * and gets the dock VDMs (state, LED, USB hub). Then DP_ENTER_MODE, pin D
 * (2 lanes) and DP_CFG_AND_START_HPD. This driver stays the only reader of the
 * BM92T (ALERT is read-to-clear). The state goes to switchdisplay through the
 * kernel callback \Callback\SwitchWoaDpAlt (Argument1 = 1 up / 0 down,
 * Argument2 = DP_STATUS), on every change and every 5 s while up. Registry
 * DpAlt=0 turns it off.
 *
 * v8: DP Alt Mode also on the host path (the Switch powers VBUS), for
 * USB-C -> HDMI adapters and hubs without their own power: 2 s after VBUS
 * is on (and at start when UEFI left VBUS on), the same steps run once per
 * plug-in. A partner without USB PD just fails Discover Identity (step 2).
 *
 * v9: every battery poll (10 s) also reads the gauge's Current (0x0A) and
 * AvgCurrent (0x0B, signed, 0.3125 mA with the 5 mOhm sense resistor, + =
 * charging) and writes them to Parameters as LastCurrentMa / LastAvgCurrentMa
 * (REG_DWORD holding a signed value), and LastPowerMw = AvgCurrent x VCELL.
 * Only for measuring standby power (sleep stage S4); Windows still gets an
 * unknown rate, as before.
 * v10: also LastQh (0x4D, the gauge's coulomb counter, signed, 1 mAh with
 * 5 mOhm) and LastRepCapMah (0x05), with LastGaugeTick (100 ns, interrupt
 * time) of the read: a test script compares two readings to get the average
 * current over a stretch Windows spent in modern standby, when user-mode
 * scripts are paused.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <batclass.h>
#include <spb.h>

#define GAUGE_REPSOC   0x06
#define GAUGE_TEMP     0x08
#define GAUGE_VCELL    0x09
#define GAUGE_CURRENT  0x0A
#define GAUGE_AVGCURRENT 0x0B
#define GAUGE_REPCAP   0x05
#define GAUGE_QH       0x4D
#define CHARGER_SYSSTAT 0x08
#define BATTERY_TAG    1
#define POLL_MS        10000

/* BM92T (Linux bm92txx / UEFI R288-R363) */
#define BM_STATUS1       0x03
#define BM_STATUS2       0x04
#define BM_COMMAND       0x05
#define BM_READ_PDOS_SRC 0x08
#define BM_CURRENT_PDO   0x28
#define BM_INSERT        (1u << 7)
#define BM_VSAFE         (1u << 10)
#define BM_SRC_MODE      (1u << 12)
#define BM_CMD_BUSY      (1u << 13)
#define BM_SPDSNK        (1u << 14)
#define BM_OTG_INSERT    (1u << 13)     /* STATUS2 */
#define BM_DR_SHIFT      8
#define BM_DR_UFP        1u
#define BM_DR_DFP        2u
#define BM_DR_SWAP_CMD   0x1818u
#define BM_PDO_DRD       (1u << 25)
/* BQ24193 */
#define BQ_INPUT_SRC     0x00
#define BQ_PWR_ON        0x01
#define BQ_TERM_TIMER    0x05
#define BQ_HIZ           0x80u
#define BQ_IINLIM_MASK   0x07u
#define BQ_RESET_BITS    0xC0u
#define BQ_MODE_MASK     0x30u
#define BQ_MODE_CHARGE   0x10u
#define BQ_MODE_OTG      0x20u
#define BQ_WATCHDOG_MASK 0x30u
#define USBC_POLL_MS     500
#define USBC_STREAK      3
#define BQ_FAULT         0x09
#define BQ_BOOST_FAULT   0x40u
#define BQ_VBUS_OTG      3u
#define USBC_BOOST_BAD   2              /* bad polls before the boost is restarted */
#define USBC_BOOST_QUICK 3              /* quick restarts, then one every USBC_BOOST_WAIT polls */
#define USBC_BOOST_WAIT  10
#define USBC_STUCK_POLLS 10
/* v7: DP Alt Mode (BM92T, UEFI R424) */
#define BM_ALERT_STATUS         0x02
#define BM_DP_STATUS            0x18
#define BM_DP_ALERT_EN          0x19
#define BM_INCOMING_VDM         0x50
#define BM_OUTGOING_VDM         0x60
#define BM_ALERT_CMD_DONE       (1u << 2)
#define BM_ALERT_VDM_RECEIVED   (1u << 11)
#define BM_CMD_SEND_VDM         0x1111u
#define BM_CMD_ACCEPT_VDM       0x1616u
#define BM_CMD_DP_ENTER_MODE    0x3131u
#define BM_CMD_DP_CFG_START_HPD 0x3636u
#define BM_DP_SIGNAL_ON         (1u << 7)
#define BM_DP_EN                (1u << 15)
#define VDO_DP_UFP_D            (1u << 0)
#define VDO_DP_U_UFP_D          (1u << 1)
#define VDO_DP_SUPPORT          (1u << 2)
#define VDO_DP_RECEPTACLE       (1u << 6)
#define VDO_DP_PIN_C            (1u << 2)
#define VDO_DP_PIN_D            (1u << 3)
#define DPALT_NOTIFY_POLLS      10              /* re-notify every 5 s while up */
#define DPALT_HOST_POLLS        4               /* v8: try DP 2 s after VBUS on */

typedef enum _USBC_STATE { UsbcIdle = 0, UsbcHost = 1, UsbcSink = 2 } USBC_STATE;

typedef struct _SWBAT_DEVICE {
    WDFIOTARGET Gauge;
    WDFIOTARGET Charger;
    WDFIOTARGET Pd;
    LARGE_INTEGER GaugeId, ChargerId, PdId;
    ULONG Connections;
    WDFWAITLOCK Lock;
    PVOID ClassHandle;
    WDFTIMER Timer;
    WDFTIMER UsbcTimer;
    volatile BOOLEAN Stopping;
    volatile LONG TimerRuns;
    BATTERY_STATUS Last;
    USBC_STATE Usbc;
    ULONG HostStreak, SinkStreak, GoneStreak;
    ULONG UsbcChanges;
    ULONG BoostBad, BoostRestartsInRow, BoostWait, BoostRestarts;
    ULONG StuckPolls;
    /* v7 DP Alt Mode */
    BOOLEAN DpAllowed;
    BOOLEAN DpUp;
    ULONG DpTries, DpUps, DpNotifyPolls;
    ULONG DpHostPolls;                          /* v8: polls since VBUS on */
    USHORT DpStatus;
    PCALLBACK_OBJECT DpCallback;
} SWBAT_DEVICE, *PSWBAT_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBAT_DEVICE, SwBatGetDevice)

typedef struct _SWBAT_TIMER {
    WDFDEVICE Device;
} SWBAT_TIMER, *PSWBAT_TIMER;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBAT_TIMER, SwBatGetTimer)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwBatEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwBatEvtPrepareHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwBatEvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP SwBatEvtSelfManagedIoCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwBatEvtIoDeviceControl;
EVT_WDF_TIMER SwBatEvtTimer;
EVT_WDF_TIMER SwBatEvtUsbcTimer;
BCLASS_QUERY_TAG_CALLBACK SwBatQueryTag;
BCLASS_QUERY_INFORMATION_CALLBACK SwBatQueryInformation;
BCLASS_SET_INFORMATION_CALLBACK SwBatSetInformation;
BCLASS_QUERY_STATUS_CALLBACK SwBatQueryStatus;
BCLASS_SET_STATUS_NOTIFY_CALLBACK SwBatSetStatusNotify;
BCLASS_DISABLE_STATUS_NOTIFY_CALLBACK SwBatDisableStatusNotify;

/* ------------------------------------------------------------ diagnostics */

static WCHAR SwBatParamsPath[256];

static VOID SwBatTrace(PCWSTR Name, ULONG Value)
{
    if (SwBatParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwBatParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static ULONG SwBatRegGet(PCWSTR Name, ULONG Default)
{
    RTL_QUERY_REGISTRY_TABLE Q[2];
    ULONG Value = Default, Def = Default;
    if (SwBatParamsPath[0] == L'\0') {
        return Default;
    }
    RtlZeroMemory(Q, sizeof(Q));
    Q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Q[0].Name = (PWSTR)Name;
    Q[0].EntryContext = &Value;
    Q[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    Q[0].DefaultData = &Def;
    Q[0].DefaultLength = sizeof(Def);
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwBatParamsPath, Q, NULL, NULL))) {
        return Default;
    }
    return Value;
}

/* ------------------------------------------------------------------- I2C */

static NTSTATUS SwBatOpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
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

/* Write the register number, then read Length bytes (repeated start). */
static NTSTATUS SwBatReadRegs(WDFIOTARGET Target, UCHAR Register, PUCHAR Data, ULONG Length)
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
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Data, Length);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 1 + Length) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwBatRead16(WDFIOTARGET Target, UCHAR Register, PUSHORT Value)
{
    UCHAR Buffer[2] = { 0, 0 };
    NTSTATUS Status = SwBatReadRegs(Target, Register, Buffer, 2);
    *Value = (USHORT)(Buffer[0] | (Buffer[1] << 8));
    return Status;
}

/* Write Register followed by Length (<= 2) data bytes. */
static NTSTATUS SwBatWriteRegs(WDFIOTARGET Target, UCHAR Register, const UCHAR *Data, ULONG Length)
{
    UCHAR Buffer[3];
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL || Length > 2) {
        return STATUS_INVALID_PARAMETER;
    }
    Buffer[0] = Register;
    RtlCopyMemory(&Buffer[1], Data, Length);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, 1 + Length);
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 1 + Length) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

/* v7: Buf[0] is the register, Len <= 16 bytes in total. */
static NTSTATUS SwBatWriteBlock(WDFIOTARGET Target, const UCHAR *Buf, ULONG Len)
{
    UCHAR Buffer[16];
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL || Len < 2 || Len > sizeof(Buffer)) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlCopyMemory(Buffer, Buf, Len);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, Len);
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(500);
    Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != Len) {
        Status = STATUS_DEVICE_PROTOCOL_ERROR;
    }
    return Status;
}

static NTSTATUS SwBatWrite8(WDFIOTARGET Target, UCHAR Register, UCHAR Value)
{
    return SwBatWriteRegs(Target, Register, &Value, 1);
}

static VOID SwBatSleepMs(ULONG Ms)
{
    LARGE_INTEGER Delay;
    Delay.QuadPart = -(LONGLONG)Ms * 10000;
    KeDelayExecutionThread(KernelMode, FALSE, &Delay);
}

/* ------------------------------------------------------- USB-C role (v4) */

/* BQ REG01 charge configuration: BQ_MODE_CHARGE or BQ_MODE_OTG (VBUS out). */
static NTSTATUS SwBatSetBqMode(PSWBAT_DEVICE Dev, UCHAR Mode)
{
    UCHAR Pwr = 0, Verify = 0;
    NTSTATUS Status = SwBatReadRegs(Dev->Charger, BQ_PWR_ON, &Pwr, 1);
    if (NT_SUCCESS(Status)) {
        Status = SwBatWrite8(Dev->Charger, BQ_PWR_ON, (UCHAR)((Pwr & ~(BQ_RESET_BITS | BQ_MODE_MASK)) | Mode));
    }
    if (NT_SUCCESS(Status)) {
        Status = SwBatReadRegs(Dev->Charger, BQ_PWR_ON, &Verify, 1);
    }
    if (NT_SUCCESS(Status) && (Verify & BQ_MODE_MASK) != Mode) {
        Status = STATUS_DEVICE_DATA_ERROR;
    }
    SwBatTrace(L"UsbcBqReg01", Verify);
    return Status;
}

/* Nobody services the BQ watchdog; Linux sets ti,watchdog-timeout=0 too. */
static VOID SwBatDisableBqWatchdog(PSWBAT_DEVICE Dev)
{
    UCHAR Timer = 0;
    if (NT_SUCCESS(SwBatReadRegs(Dev->Charger, BQ_TERM_TIMER, &Timer, 1)) && (Timer & BQ_WATCHDOG_MASK) != 0) {
        (VOID)SwBatWrite8(Dev->Charger, BQ_TERM_TIMER, (UCHAR)(Timer & ~BQ_WATCHDOG_MASK));
    }
}

static ULONG SwBatLe32(const UCHAR *P)
{
    return (ULONG)P[0] | ((ULONG)P[1] << 8) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 24);
}

/* Charger hub attached: UEFI R363ConfigureSink. Caller holds Dev->Lock. */
static NTSTATUS SwBatConfigureSink(PSWBAT_DEVICE Dev)
{
    static const ULONG Limits[8] = { 100, 150, 500, 900, 1200, 1500, 2000, 3000 };
    UCHAR Pdos[29], Cur[5], In = 0;
    USHORT Status1 = 0;
    ULONG Pdo1 = 0, Pdo, Mv = 0, Ma = 0, LimitMa, Code, I;
    BOOLEAN Contract = FALSE;
    NTSTATUS Status;

    Status = SwBatSetBqMode(Dev, BQ_MODE_CHARGE);       /* never leave VBUS out with a source attached */
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    if (NT_SUCCESS(SwBatReadRegs(Dev->Pd, BM_READ_PDOS_SRC, Pdos, sizeof(Pdos))) && Pdos[0] >= 4) {
        Pdo1 = SwBatLe32(&Pdos[1]);
    }
    for (I = 0; I < 10 && !Contract; ++I) {
        if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &Status1)) && (Status1 & BM_SPDSNK) != 0 &&
            NT_SUCCESS(SwBatReadRegs(Dev->Pd, BM_CURRENT_PDO, Cur, sizeof(Cur)))) {
            Pdo = SwBatLe32(&Cur[1]);
            if ((Pdo >> 30) == 0 && Pdo != 0) {
                Mv = ((Pdo >> 10) & 0x3FF) * 50;
                Ma = (Pdo & 0x3FF) * 10;
                Contract = TRUE;
                break;
            }
        }
        SwBatSleepMs(500);
    }
    SwBatTrace(L"UsbcContractMv", Mv);
    SwBatTrace(L"UsbcContractMa", Ma);

    /* Data role: be DFP (USB host). Swap only when the source offers DRD, like L4T. */
    if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &Status1)) &&
        ((Status1 >> BM_DR_SHIFT) & 3) == BM_DR_UFP && (Pdo1 & BM_PDO_DRD) != 0) {
        UCHAR Cmd[2] = { (UCHAR)(BM_DR_SWAP_CMD & 0xFF), (UCHAR)(BM_DR_SWAP_CMD >> 8) };
        if (NT_SUCCESS(SwBatWriteRegs(Dev->Pd, BM_COMMAND, Cmd, 2))) {
            for (I = 0; I < 20; ++I) {
                SwBatSleepMs(100);
                if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &Status1)) && (Status1 & BM_CMD_BUSY) == 0 &&
                    ((Status1 >> BM_DR_SHIFT) & 3) == BM_DR_DFP) {
                    break;
                }
            }
        }
    }
    SwBatTrace(L"UsbcDataRole", (Status1 >> BM_DR_SHIFT) & 3);

    /* Input current limit from the contract (L4T: 5/9 V 2 A, 12 V 1.5 A, 15 V 1.2 A). */
    if (!Contract) {
        LimitMa = 500;
    } else if (Mv <= 9500) {
        LimitMa = 2000;
    } else if (Mv <= 12500) {
        LimitMa = 1500;
    } else if (Mv <= 15500) {
        LimitMa = 1200;
    } else {
        LimitMa = 500;
    }
    if (Contract && Ma != 0 && Ma < LimitMa) {
        LimitMa = Ma;
    }
    for (Code = 7; Code > 0 && Limits[Code] > LimitMa; --Code) {}
    Status = SwBatReadRegs(Dev->Charger, BQ_INPUT_SRC, &In, 1);
    if (NT_SUCCESS(Status)) {
        Status = SwBatWrite8(Dev->Charger, BQ_INPUT_SRC, (UCHAR)((In & ~(BQ_HIZ | BQ_IINLIM_MASK)) | Code));
    }
    SwBatTrace(L"UsbcInputLimitMa", Limits[Code]);
    SwBatDisableBqWatchdog(Dev);
    return Status;
}

/*
 * v6: host with a partner inserted: is the BQ boost really giving VBUS?
 * REG09 latches faults, so it is read every poll (the read clears it).
 * Caller holds Dev->Lock.
 */
static VOID SwBatCheckBoost(PSWBAT_DEVICE Dev, UCHAR Sys)
{
    UCHAR Fault = 0;
    BOOLEAN Bad;
    NTSTATUS Status;

    (VOID)SwBatReadRegs(Dev->Charger, BQ_FAULT, &Fault, 1);
    Bad = ((Sys >> 6) & 3) != BQ_VBUS_OTG || (Fault & BQ_BOOST_FAULT) != 0;
    if (!Bad) {
        Dev->BoostBad = 0;
        Dev->BoostRestartsInRow = 0;
        Dev->BoostWait = 0;
        return;
    }
    if (Dev->BoostWait != 0) {
        Dev->BoostWait--;
        return;
    }
    if (++Dev->BoostBad < USBC_BOOST_BAD) {
        return;
    }
    Dev->BoostBad = 0;
    SwBatTrace(L"UsbcBoostSysStat", Sys);
    SwBatTrace(L"UsbcBoostFault", Fault);
    (VOID)SwBatSetBqMode(Dev, BQ_MODE_CHARGE);
    Status = SwBatSetBqMode(Dev, BQ_MODE_OTG);
    SwBatDisableBqWatchdog(Dev);
    SwBatTrace(L"UsbcBoostRestartStatus", (ULONG)Status);
    SwBatTrace(L"UsbcBoostRestarts", ++Dev->BoostRestarts);
    if (++Dev->BoostRestartsInRow >= USBC_BOOST_QUICK) {
        Dev->BoostWait = USBC_BOOST_WAIT;       /* a real short: do not hammer the boost */
    }
}

/* One 500 ms step of the role state machine. Caller holds Dev->Lock. */
/* ------------------------------------------------- DP Alt Mode (v7) */

static VOID SwBatDpNotify(PSWBAT_DEVICE Dev)
{
    if (Dev->DpCallback != NULL) {
        ExNotifyCallback(Dev->DpCallback, (PVOID)(ULONG_PTR)(Dev->DpUp ? 1 : 0), (PVOID)(ULONG_PTR)Dev->DpStatus);
    }
}

static VOID SwBatDpSetUp(PSWBAT_DEVICE Dev, BOOLEAN Up, USHORT DpStatus)
{
    BOOLEAN Changed = Dev->DpUp != Up;
    Dev->DpUp = Up;
    Dev->DpStatus = DpStatus;
    Dev->DpNotifyPolls = 0;
    if (Changed) {
        if (Up) {
            SwBatTrace(L"DpAltUps", ++Dev->DpUps);
        }
        SwBatTrace(L"DpAltUp", Up ? 1 : 0);
        SwBatTrace(L"DpAltDpStatus", DpStatus);
    }
    SwBatDpNotify(Dev);
}

static VOID SwBatDpWaitNotBusy(PSWBAT_DEVICE Dev)
{
    USHORT S1 = 0;
    ULONG I;
    for (I = 0; I < 40; ++I) {
        if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &S1)) && (S1 & BM_CMD_BUSY) == 0) {
            return;
        }
        SwBatSleepMs(5);
    }
}

static NTSTATUS SwBatDpCmd(PSWBAT_DEVICE Dev, USHORT Cmd)
{
    UCHAR B[2] = { (UCHAR)(Cmd & 0xFF), (UCHAR)(Cmd >> 8) };
    SwBatDpWaitNotBusy(Dev);
    return SwBatWriteRegs(Dev->Pd, BM_COMMAND, B, 2);
}

/* ALERT is read-to-clear: OR of all bits seen until one of Want, 0 on timeout. */
static USHORT SwBatDpWaitAlert(PSWBAT_DEVICE Dev, USHORT Want, ULONG TimeoutMs)
{
    USHORT A = 0, Seen = 0;
    ULONG T;
    for (T = 0; T < TimeoutMs; T += 10) {
        if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_ALERT_STATUS, &A))) {
            Seen |= A;
            if ((Seen & Want) != 0) {
                return Seen;
            }
        }
        SwBatSleepMs(10);
    }
    return 0;
}

/* One VDM: Msg[0] = OUTGOING_VDM, Msg[1] = length. Reply[29] = INCOMING_VDM. */
static BOOLEAN SwBatDpVdm(PSWBAT_DEVICE Dev, const UCHAR *Msg, ULONG Len, PUCHAR Reply)
{
    USHORT A = 0;
    RtlZeroMemory(Reply, 29);
    (VOID)SwBatRead16(Dev->Pd, BM_ALERT_STATUS, &A);         /* drop stale alerts */
    SwBatDpWaitNotBusy(Dev);
    if (!NT_SUCCESS(SwBatWriteBlock(Dev->Pd, Msg, Len)) || !NT_SUCCESS(SwBatDpCmd(Dev, BM_CMD_SEND_VDM))) {
        return FALSE;
    }
    if ((SwBatDpWaitAlert(Dev, BM_ALERT_VDM_RECEIVED, 1500) & BM_ALERT_VDM_RECEIVED) == 0) {
        return FALSE;
    }
    if (!NT_SUCCESS(SwBatDpCmd(Dev, BM_CMD_ACCEPT_VDM))) {
        return FALSE;
    }
    (VOID)SwBatDpWaitAlert(Dev, BM_ALERT_CMD_DONE, 1000);
    return NT_SUCCESS(SwBatReadRegs(Dev->Pd, BM_INCOMING_VDM, Reply, 29));
}

/* Standard DP SVID: does the partner offer a UFP_D DP mode? */
static BOOLEAN SwBatDpGenericModes(PSWBAT_DEVICE Dev)
{
    static const UCHAR DiscSvid[6] = { BM_OUTGOING_VDM, 4, 0x02, 0x80, 0x00, 0xFF };
    static const UCHAR DiscMode[6] = { BM_OUTGOING_VDM, 4, 0x03, 0x80, 0x01, 0xFF };
    UCHAR Vdm[29];
    if (!SwBatDpVdm(Dev, DiscSvid, sizeof(DiscSvid), Vdm) || !SwBatDpVdm(Dev, DiscMode, sizeof(DiscMode), Vdm)) {
        return FALSE;
    }
    return Vdm[0] > 4 && Vdm[1] == (0x40 | 0x03) && Vdm[3] == 0x01 && Vdm[4] == 0xFF &&
           (Vdm[5] & VDO_DP_UFP_D) != 0 && (Vdm[5] & VDO_DP_SUPPORT) != 0;
}

/* The UEFI R424 stage 1. Returns the step reached (traced as DpAltStep):
 * 1 not DFP, 2 no Discover Identity, 3 not a DP partner, 4 Nintendo mode not
 * entered, 5 DP_ENTER_MODE, 6 no pin C/D, 7 DP configure, 10 done. Caller
 * holds Dev->Lock. */
static ULONG SwBatDpAltEnter(PSWBAT_DEVICE Dev)
{
    static const UCHAR DiscId[6]   = { BM_OUTGOING_VDM, 4, 0x01, 0x80, 0x00, 0xFF };
    static const UCHAR EnterNin[6] = { BM_OUTGOING_VDM, 4, 0x04, 0x81, 0x7E, 0x05 };
    static const UCHAR Query[10]   = { BM_OUTGOING_VDM, 8, 0x00, 0x00, 0x7E, 0x05, 0x00, 0x01, 0x16, 0x00 };
    static const UCHAR HubOn[10]   = { BM_OUTGOING_VDM, 8, 0x00, 0x00, 0x7E, 0x05, 0x01, 0x01, 0x20, 0x00 };
    static const UCHAR Led[14]     = { BM_OUTGOING_VDM, 12, 0x00, 0x00, 0x7E, 0x05, 0x01, 0x01, 0x01, 0x00,
                                       64, 0, 0, 128 };
    UCHAR Vdm[29], Caps[5], Cfg[6];
    USHORT S1 = 0, Dp = 0, A, Vid, Pid;
    ULONG W0, W2, Type, I;
    BOOLEAN Dock, Generic;
    UCHAR Pin;

    SwBatTrace(L"DpAltTries", ++Dev->DpTries);
    if (!NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &S1)) || ((S1 >> BM_DR_SHIFT) & 3) != BM_DR_DFP) {
        return 1;
    }
    (VOID)SwBatWriteRegs(Dev->Pd, BM_DP_ALERT_EN, (const UCHAR *)"\0\0", 2);   /* as L4T */
    if (!SwBatDpVdm(Dev, DiscId, sizeof(DiscId), Vdm) || Vdm[1] != (0x40 | 0x01) || Vdm[0] < 16) {
        return 2;
    }
    W0 = SwBatLe32(&Vdm[5]);
    W2 = SwBatLe32(&Vdm[13]);
    Vid = (USHORT)(W0 & 0xFFFF);
    Type = (W0 >> 27) & 7;
    Pid = (USHORT)(W2 >> 16);
    SwBatTrace(L"DpAltPartner", ((ULONG)Vid << 16) | Pid);
    Dock = Type == 5 && Vid == 0x057E && Pid == 0x2003;
    /* R432: a hub answering as the dock gets HDMI only in standard DP. */
    Generic = SwBatDpGenericModes(Dev);
    if (!Generic) {
        if (!Dock) {
            return 3;
        }
        if (!SwBatDpVdm(Dev, EnterNin, sizeof(EnterNin), Vdm) || Vdm[1] != (0x40 | 0x04) || Vdm[2] != 0x81 ||
            Vdm[3] != 0x7E || Vdm[4] != 0x05) {
            return 4;
        }
    }
    SwBatTrace(L"DpAltGeneric", Generic ? 1 : 0);

    (VOID)SwBatRead16(Dev->Pd, BM_ALERT_STATUS, &A);
    if (!NT_SUCCESS(SwBatDpCmd(Dev, BM_CMD_DP_ENTER_MODE))) {
        return 5;
    }
    SwBatSleepMs(100);
    if ((SwBatDpWaitAlert(Dev, BM_ALERT_CMD_DONE, 3000) & BM_ALERT_CMD_DONE) == 0 ||
        !NT_SUCCESS(SwBatReadRegs(Dev->Pd, BM_INCOMING_VDM, Caps, sizeof(Caps)))) {
        return 5;
    }
    SwBatTrace(L"DpAltCaps", SwBatLe32(&Caps[1]));
    /* L4T: pin D (2 lanes) first, then C. The dock uses the receptacle field. */
    Cfg[0] = BM_OUTGOING_VDM; Cfg[1] = 4; Cfg[2] = VDO_DP_SUPPORT | VDO_DP_U_UFP_D;
    Cfg[3] = 0; Cfg[4] = 0; Cfg[5] = 0;
    Pin = 0;
    for (I = 0; I < 2 && Pin == 0; ++I) {
        UCHAR Try = (I == 0) ? VDO_DP_PIN_D : VDO_DP_PIN_C;
        if (!Generic) {
            if ((Caps[3] & Try) != 0) { Cfg[4] = Try; Pin = Try; }
        } else if ((Caps[1] & VDO_DP_RECEPTACLE) == 0) {
            if ((Caps[2] & Try) != 0) { Cfg[3] = Try; Pin = Try; }
        } else {
            if ((Caps[3] & Try) != 0) { Cfg[3] = Try; Pin = Try; }
        }
    }
    if (Caps[0] != 4 || Pin == 0) {
        return 6;
    }
    (VOID)SwBatRead16(Dev->Pd, BM_ALERT_STATUS, &A);
    SwBatDpWaitNotBusy(Dev);
    if (!NT_SUCCESS(SwBatWriteBlock(Dev->Pd, Cfg, sizeof(Cfg))) || !NT_SUCCESS(SwBatDpCmd(Dev, BM_CMD_DP_CFG_START_HPD))) {
        return 7;
    }
    A = SwBatDpWaitAlert(Dev, BM_ALERT_CMD_DONE, 3000);
    (VOID)SwBatRead16(Dev->Pd, BM_STATUS1, &S1);
    SwBatTrace(L"DpAltCfgStatus1", S1);
    if ((A & BM_ALERT_CMD_DONE) == 0 || (S1 & BM_INSERT) == 0 || ((S1 >> 4) & 7) != 0) {
        return 7;
    }
    if (!Generic) {
        /* Nintendo dock VDMs; failures do not undo DP. */
        (VOID)SwBatDpVdm(Dev, Query, sizeof(Query), Vdm);
        (VOID)SwBatDpVdm(Dev, Led, sizeof(Led), Vdm);
        SwBatSleepMs(500);
        for (I = 0; I < 10; ++I) {
            if (!SwBatDpVdm(Dev, HubOn, sizeof(HubOn), Vdm)) {
                break;
            }
            if (!(Vdm[6] == 2 && Vdm[7] == 0x21 && (Vdm[5] & 1) != 0)) {
                break;
            }
            SwBatSleepMs(250);
        }
    }
    /* Wait up to 3 s for the partner to raise the DP signal (HPD). */
    for (I = 0; I < 30; ++I) {
        if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_DP_STATUS, &Dp)) && (Dp & BM_DP_SIGNAL_ON) != 0) {
            break;
        }
        SwBatSleepMs(100);
    }
    SwBatDpSetUp(Dev, TRUE, Dp);
    return 10;
}

/* Sink state reached (UEFI or this driver): enter DP Alt Mode if the partner
 * has it and UEFI did not already. Caller holds Dev->Lock. */
static VOID SwBatDpAltCheck(PSWBAT_DEVICE Dev)
{
    USHORT Dp = 0;
    if (!Dev->DpAllowed || Dev->Pd == NULL) {
        return;
    }
    if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_DP_STATUS, &Dp)) && (Dp & BM_DP_EN) != 0) {
        SwBatTrace(L"DpAltStep", 11);                       /* already in DP mode (UEFI) */
        SwBatDpSetUp(Dev, TRUE, Dp);
        return;
    }
    SwBatTrace(L"DpAltStep", SwBatDpAltEnter(Dev));
}

static VOID SwBatUsbcPoll(PSWBAT_DEVICE Dev)
{
    USHORT S1 = 0, S2 = 0;
    UCHAR Sys = 0;
    BOOLEAN Inserted, HostSeen, SinkSeen, PartnerPower;
    USBC_STATE Old = Dev->Usbc;
    NTSTATUS Status;

    if (!NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &S1)) || !NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS2, &S2))) {
        Dev->HostStreak = Dev->SinkStreak = 0;
        return;
    }
    (VOID)SwBatReadRegs(Dev->Charger, CHARGER_SYSSTAT, &Sys, 1);
    Inserted = (S1 & BM_INSERT) != 0;
    HostSeen = (S1 & (BM_INSERT | BM_SRC_MODE)) == (BM_INSERT | BM_SRC_MODE) && (S2 & BM_OTG_INSERT) != 0;
    SinkSeen = (S1 & (BM_INSERT | BM_VSAFE)) == (BM_INSERT | BM_VSAFE) && (S1 & BM_SRC_MODE) == 0 &&
               ((S1 >> BM_DR_SHIFT) & 3) != BM_DR_DFP;
    PartnerPower = ((Sys >> 6) & 3) == 1 || ((Sys >> 6) & 3) == 2;     /* BQ sees input power */

    switch (Dev->Usbc) {
    case UsbcIdle:
        /* v6: inserted, but no role for a while: keep the PD state for the trace. */
        Dev->StuckPolls = Inserted ? Dev->StuckPolls + 1 : 0;
        if (Dev->StuckPolls == USBC_STUCK_POLLS) {
            SwBatTrace(L"UsbcStuckStatus1", S1);
            SwBatTrace(L"UsbcStuckStatus2", S2);
            SwBatTrace(L"UsbcStuckSysStat", Sys);
        }
        Dev->HostStreak = HostSeen ? Dev->HostStreak + 1 : 0;
        Dev->SinkStreak = SinkSeen ? Dev->SinkStreak + 1 : 0;
        if (Dev->SinkStreak >= USBC_STREAK) {
            Status = SwBatConfigureSink(Dev);
            SwBatTrace(L"UsbcSinkStatus", (ULONG)Status);
            Dev->Usbc = UsbcSink;
            SwBatDpAltCheck(Dev);
        } else if (Dev->HostStreak >= USBC_STREAK) {
            if ((S1 & BM_VSAFE) != 0 || PartnerPower) {
                SwBatTrace(L"UsbcVbusRefused", S1);         /* the partner already powers VBUS */
                Dev->HostStreak = 0;
            } else {
                Status = SwBatSetBqMode(Dev, BQ_MODE_OTG);
                SwBatDisableBqWatchdog(Dev);
                SwBatTrace(L"UsbcHostStatus", (ULONG)Status);
                if (NT_SUCCESS(Status)) {
                    Dev->Usbc = UsbcHost;
                    Dev->BoostBad = Dev->BoostRestartsInRow = Dev->BoostWait = 0;
                    Dev->DpHostPolls = 0;
                }
            }
        }
        break;
    case UsbcHost:
    case UsbcSink:
        Dev->GoneStreak = Inserted ? 0 : Dev->GoneStreak + 1;
        if (Dev->GoneStreak >= 2) {
            if (Dev->DpUp) {
                SwBatDpSetUp(Dev, FALSE, 0);
            }
            if (Dev->Usbc == UsbcHost) {
                SwBatTrace(L"UsbcVbusOffStatus", (ULONG)SwBatSetBqMode(Dev, BQ_MODE_CHARGE));
            }
            Dev->Usbc = UsbcIdle;
            Dev->HostStreak = Dev->SinkStreak = Dev->GoneStreak = Dev->StuckPolls = 0;
        } else {
            if (Dev->Usbc == UsbcHost && Inserted) {
                SwBatCheckBoost(Dev, Sys);
                /* v8: unpowered DP adapters: try DP Alt once, 2 s after VBUS on */
                if (!Dev->DpUp && Dev->DpHostPolls < DPALT_HOST_POLLS &&
                    ++Dev->DpHostPolls == DPALT_HOST_POLLS) {
                    SwBatDpAltCheck(Dev);
                }
            }
            if (!(Dev->DpUp && ++Dev->DpNotifyPolls >= DPALT_NOTIFY_POLLS)) {
                break;
            }
            {
                USHORT Dp = Dev->DpStatus;
                (VOID)SwBatRead16(Dev->Pd, BM_DP_STATUS, &Dp);
                SwBatDpSetUp(Dev, (Dp & BM_DP_EN) != 0, Dp);
            }
        }
        break;
    }
    if (Dev->Usbc != Old) {
        SwBatTrace(L"UsbcState", Dev->Usbc);
        SwBatTrace(L"UsbcStatus1", S1);
        SwBatTrace(L"UsbcStatus2", S2);
        SwBatTrace(L"UsbcChanges", ++Dev->UsbcChanges);
    }
}

/* Start state: keep what UEFI set up (VBUS out = host). Caller holds Dev->Lock. */
static VOID SwBatUsbcStart(PSWBAT_DEVICE Dev)
{
    USHORT S1 = 0;
    UCHAR Pwr = 0;

    Dev->Usbc = UsbcIdle;
    if (NT_SUCCESS(SwBatReadRegs(Dev->Charger, BQ_PWR_ON, &Pwr, 1)) && (Pwr & BQ_MODE_MASK) == BQ_MODE_OTG) {
        Dev->Usbc = UsbcHost;                       /* GoneStreak turns VBUS off if nothing is there */
        Dev->DpHostPolls = 0;                       /* v8: UEFI R444 may have entered DP on this path */
    } else if (NT_SUCCESS(SwBatRead16(Dev->Pd, BM_STATUS1, &S1)) && (S1 & BM_INSERT) != 0 &&
               (S1 & BM_SRC_MODE) == 0 && (S1 & BM_VSAFE) != 0) {
        Dev->Usbc = UsbcSink;                       /* UEFI R363 already configured charging */
        SwBatDpAltCheck(Dev);                       /* v7: UEFI R424 may have entered DP too */
    }
    SwBatTrace(L"UsbcStartState", Dev->Usbc);
    SwBatTrace(L"UsbcState", Dev->Usbc);           /* v5: otherwise a value from an earlier boot stays */
    SwBatTrace(L"UsbcStartReg01", Pwr);
}

VOID SwBatEvtUsbcTimer(WDFTIMER Timer)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(SwBatGetTimer(Timer)->Device);
    WdfWaitLockAcquire(Dev->Lock, NULL);
    SwBatUsbcPoll(Dev);
    WdfWaitLockRelease(Dev->Lock);
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(USBC_POLL_MS));    /* one-shot timer, re-armed */
    }
}

/* v9: gauge current for standby measurements. Caller holds Dev->Lock. */
static VOID SwBatTraceCurrent(PSWBAT_DEVICE Dev, ULONG VoltageMv)
{
    USHORT Cur = 0, Avg = 0, Qh = 0, RepCap = 0;
    LONG CurMa, AvgMa;
    ULONGLONG Tick;

    if (!NT_SUCCESS(SwBatRead16(Dev->Gauge, GAUGE_CURRENT, &Cur)) ||
        !NT_SUCCESS(SwBatRead16(Dev->Gauge, GAUGE_AVGCURRENT, &Avg))) {
        return;
    }
    /* 1 LSB = 0.3125 mA = 5/16 mA */
    CurMa = ((LONG)(SHORT)Cur * 5) / 16;
    AvgMa = ((LONG)(SHORT)Avg * 5) / 16;
    SwBatTrace(L"LastCurrentMa", (ULONG)CurMa);
    SwBatTrace(L"LastAvgCurrentMa", (ULONG)AvgMa);
    SwBatTrace(L"LastPowerMw", (ULONG)(LONG)(((LONGLONG)AvgMa * (LONGLONG)VoltageMv) / 1000));
    /* v10: coulomb counter and remaining capacity, 1 mAh per LSB with 5 mOhm */
    if (NT_SUCCESS(SwBatRead16(Dev->Gauge, GAUGE_QH, &Qh)) && NT_SUCCESS(SwBatRead16(Dev->Gauge, GAUGE_REPCAP, &RepCap))) {
        Tick = KeQueryInterruptTime();
        SwBatTrace(L"LastQh", (ULONG)(LONG)(SHORT)Qh);
        SwBatTrace(L"LastRepCapMah", RepCap);
        SwBatTrace(L"LastGaugeTickLo", (ULONG)Tick);
        SwBatTrace(L"LastGaugeTickHi", (ULONG)(Tick >> 32));
    }
}

/* Read gauge + charger into a BATTERY_STATUS. Caller holds Dev->Lock. */
static NTSTATUS SwBatReadStatus(PSWBAT_DEVICE Dev, PBATTERY_STATUS Out)
{
    USHORT Soc = 0, Vcell = 0;
    UCHAR Sys = 0;
    ULONG Percent, VbusStat, ChrgStat;
    BOOLEAN Online;
    NTSTATUS Status;

    Status = SwBatRead16(Dev->Gauge, GAUGE_REPSOC, &Soc);
    if (NT_SUCCESS(Status)) {
        Status = SwBatRead16(Dev->Gauge, GAUGE_VCELL, &Vcell);
    }
    if (!NT_SUCCESS(Status)) {
        SwBatTrace(L"LastGaugeError", (ULONG)Status);
        return Status;
    }
    Percent = Soc >> 8;
    if (Percent > 100) {
        Percent = 100;
    }
    if (!NT_SUCCESS(SwBatReadRegs(Dev->Charger, CHARGER_SYSSTAT, &Sys, 1))) {
        Sys = 0;                     /* unknown: treat as on battery */
    }
    VbusStat = (Sys >> 6) & 3;
    ChrgStat = (Sys >> 4) & 3;
    Online = (VbusStat == 1 || VbusStat == 2) && (Sys & 0x04) != 0;   /* OTG (3) = Switch supplies VBUS */

    Out->PowerState = 0;
    if (Online) {
        Out->PowerState |= BATTERY_POWER_ON_LINE;
        if (ChrgStat == 1 || ChrgStat == 2) {
            Out->PowerState |= BATTERY_CHARGING;
        }
    } else {
        Out->PowerState |= BATTERY_DISCHARGING;
    }
    Out->Capacity = Percent;
    Out->Voltage = ((ULONG)(Vcell >> 3) * 625u) / 1000u;
    Out->Rate = (LONG)BATTERY_UNKNOWN_RATE;
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------- battery class */

NTSTATUS SwBatQueryTag(PVOID Context, PULONG BatteryTag)
{
    PSWBAT_DEVICE Dev = (PSWBAT_DEVICE)Context;
    *BatteryTag = (Dev->Gauge != NULL) ? BATTERY_TAG : BATTERY_TAG_INVALID;
    return (Dev->Gauge != NULL) ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

static NTSTATUS SwBatReturn(PVOID Source, ULONG Size, PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    *ReturnedLength = Size;
    if (BufferLength < Size) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlCopyMemory(Buffer, Source, Size);
    return STATUS_SUCCESS;
}

NTSTATUS SwBatQueryInformation(PVOID Context, ULONG BatteryTag, BATTERY_QUERY_INFORMATION_LEVEL Level, LONG AtRate,
                               PVOID Buffer, ULONG BufferLength, PULONG ReturnedLength)
{
    PSWBAT_DEVICE Dev = (PSWBAT_DEVICE)Context;
    UNREFERENCED_PARAMETER(AtRate);

    *ReturnedLength = 0;
    if (BatteryTag != BATTERY_TAG) {
        return STATUS_NO_SUCH_DEVICE;
    }
    switch (Level) {
    case BatteryInformation: {
        BATTERY_INFORMATION Info;
        RtlZeroMemory(&Info, sizeof(Info));
        Info.Capabilities = BATTERY_SYSTEM_BATTERY | BATTERY_CAPACITY_RELATIVE;
        Info.Technology = 1;                       /* rechargeable */
        Info.Chemistry[0] = 'L'; Info.Chemistry[1] = 'I'; Info.Chemistry[2] = 'O'; Info.Chemistry[3] = 'N';
        Info.DesignedCapacity = 100;
        Info.FullChargedCapacity = 100;
        Info.DefaultAlert1 = 5;
        Info.DefaultAlert2 = 10;
        Info.CriticalBias = 0;
        Info.CycleCount = 0;
        return SwBatReturn(&Info, sizeof(Info), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryGranularityInformation: {
        BATTERY_REPORTING_SCALE Scale = { 1, 100 };
        return SwBatReturn(&Scale, sizeof(Scale), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryTemperature: {
        USHORT Raw = 0;
        ULONG Kelvin10;
        NTSTATUS Status;
        WdfWaitLockAcquire(Dev->Lock, NULL);
        Status = SwBatRead16(Dev->Gauge, GAUGE_TEMP, &Raw);
        WdfWaitLockRelease(Dev->Lock);
        if (!NT_SUCCESS(Status)) {
            return Status;
        }
        Kelvin10 = (ULONG)((((LONG)(SHORT)Raw) * 10) / 256 + 2732);
        return SwBatReturn(&Kelvin10, sizeof(Kelvin10), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryEstimatedTime: {
        ULONG Time = BATTERY_UNKNOWN_TIME;
        return SwBatReturn(&Time, sizeof(Time), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryDeviceName: {
        static const WCHAR Name[] = L"Nintendo Switch Battery";
        return SwBatReturn((PVOID)Name, sizeof(Name), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryManufactureName: {
        static const WCHAR Name[] = L"Nintendo";
        return SwBatReturn((PVOID)Name, sizeof(Name), Buffer, BufferLength, ReturnedLength);
    }
    case BatteryUniqueID: {
        static const WCHAR Name[] = L"SwitchBAT0";
        return SwBatReturn((PVOID)Name, sizeof(Name), Buffer, BufferLength, ReturnedLength);
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS SwBatSetInformation(PVOID Context, ULONG BatteryTag, BATTERY_SET_INFORMATION_LEVEL Level, PVOID Buffer)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryTag);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Buffer);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS SwBatQueryStatus(PVOID Context, ULONG BatteryTag, PBATTERY_STATUS BatteryStatus)
{
    PSWBAT_DEVICE Dev = (PSWBAT_DEVICE)Context;
    NTSTATUS Status;
    if (BatteryTag != BATTERY_TAG) {
        return STATUS_NO_SUCH_DEVICE;
    }
    WdfWaitLockAcquire(Dev->Lock, NULL);
    Status = SwBatReadStatus(Dev, BatteryStatus);
    if (NT_SUCCESS(Status)) {
        Dev->Last = *BatteryStatus;
    }
    WdfWaitLockRelease(Dev->Lock);
    return Status;
}

/* Status changes are found by the poll timer, which calls BatteryClassStatusNotify. */
NTSTATUS SwBatSetStatusNotify(PVOID Context, ULONG BatteryTag, PBATTERY_NOTIFY BatteryNotify)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(BatteryNotify);
    return (BatteryTag == BATTERY_TAG) ? STATUS_SUCCESS : STATUS_NO_SUCH_DEVICE;
}

NTSTATUS SwBatDisableStatusNotify(PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return STATUS_SUCCESS;
}

VOID SwBatEvtTimer(WDFTIMER Timer)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(SwBatGetTimer(Timer)->Device);
    BATTERY_STATUS Now;
    BOOLEAN Changed = FALSE;

    WdfWaitLockAcquire(Dev->Lock, NULL);
    if (NT_SUCCESS(SwBatReadStatus(Dev, &Now))) {
        Changed = (Now.PowerState != Dev->Last.PowerState) || (Now.Capacity != Dev->Last.Capacity);
        SwBatTrace(L"LastPowerState", Now.PowerState);
        SwBatTrace(L"LastCapacity", Now.Capacity);
        SwBatTrace(L"LastVoltage", Now.Voltage);
        SwBatTraceCurrent(Dev, Now.Voltage);
    }
    WdfWaitLockRelease(Dev->Lock);
    SwBatTrace(L"TimerRuns", (ULONG)InterlockedIncrement(&Dev->TimerRuns));
    if (Changed && Dev->ClassHandle != NULL) {
        (VOID)BatteryClassStatusNotify(Dev->ClassHandle);
    }
    if (!Dev->Stopping) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));    /* one-shot timer, re-armed */
    }
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwBatEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    Dev->Connections = 0;
    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeConnection &&
            Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            LARGE_INTEGER Id;
            Id.LowPart = Desc->u.Connection.IdLowPart;
            Id.HighPart = (LONG)Desc->u.Connection.IdHighPart;
            if (Dev->Connections == 0) {
                Dev->GaugeId = Id;
            } else if (Dev->Connections == 1) {
                Dev->ChargerId = Id;
            } else if (Dev->Connections == 2) {
                Dev->PdId = Id;                     /* BM92T, UEFI R378+ */
            }
            Dev->Connections++;
        }
    }
    SwBatTrace(L"I2cConnections", Dev->Connections);
    SwBatTrace(L"GaugeIdLow", Dev->GaugeId.LowPart);
    SwBatTrace(L"GaugeIdHigh", (ULONG)Dev->GaugeId.HighPart);
    SwBatTrace(L"ChargerIdLow", Dev->ChargerId.LowPart);
    return (Dev->Connections >= 2) ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

NTSTATUS SwBatEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(Device);
    BATTERY_MINIPORT_INFO Info;
    BATTERY_STATUS First;
    NTSTATUS Status;

    Status = SwBatOpenTarget(Device, Dev->GaugeId, &Dev->Gauge);
    SwBatTrace(L"OpenGaugeStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = SwBatOpenTarget(Device, Dev->ChargerId, &Dev->Charger);
    SwBatTrace(L"OpenChargerStatus", (ULONG)Status);    /* not fatal: unknown = on battery */

    WdfWaitLockAcquire(Dev->Lock, NULL);
    Status = SwBatReadStatus(Dev, &First);
    if (NT_SUCCESS(Status)) {
        Dev->Last = First;
    }
    WdfWaitLockRelease(Dev->Lock);
    SwBatTrace(L"FirstReadStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        SwBatTrace(L"FirstCapacity", First.Capacity);
        SwBatTrace(L"FirstVoltage", First.Voltage);
        SwBatTrace(L"FirstPowerState", First.PowerState);
    }

    RtlZeroMemory(&Info, sizeof(Info));
    Info.MajorVersion = BATTERY_CLASS_MAJOR_VERSION;
    Info.MinorVersion = BATTERY_CLASS_MINOR_VERSION;
    Info.Context = Dev;
    Info.QueryTag = SwBatQueryTag;
    Info.QueryInformation = SwBatQueryInformation;
    Info.SetInformation = SwBatSetInformation;
    Info.QueryStatus = SwBatQueryStatus;
    Info.SetStatusNotify = SwBatSetStatusNotify;
    Info.DisableStatusNotify = SwBatDisableStatusNotify;
    Info.Pdo = WdfDeviceWdmGetPhysicalDevice(Device);
    Info.DeviceName = NULL;
    Status = BatteryClassInitializeDevice(&Info, &Dev->ClassHandle);
    SwBatTrace(L"ClassInitStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        Dev->ClassHandle = NULL;
        return Status;
    }

    if (Dev->Timer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES Attributes;
        /* v3: v2 used a periodic passive-level timer, which never ran (no
         * LastPowerState trace, AC icon stuck after unplugging). KMDF does
         * not allow periodic timers at passive level, so use a one-shot
         * timer that the callback re-arms. */
        WDF_TIMER_CONFIG_INIT(&TimerConfig, SwBatEvtTimer);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBAT_TIMER);
        Attributes.ParentObject = Device;
        Attributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->Timer);
        SwBatTrace(L"TimerCreateStatus", (ULONG)Status);
        if (NT_SUCCESS(Status)) {
            SwBatGetTimer(Dev->Timer)->Device = Device;
        } else {
            Dev->Timer = NULL;
        }
    }
    Dev->Stopping = FALSE;
    if (Dev->Timer != NULL) {
        WdfTimerStart(Dev->Timer, WDF_REL_TIMEOUT_IN_MS(POLL_MS));
    }

    /* v4: USB-C role switching, only with the BM92T connection (R378+) and the charger. */
    if (Dev->Connections >= 3 && Dev->Charger != NULL && Dev->Pd == NULL) {
        Status = SwBatOpenTarget(Device, Dev->PdId, &Dev->Pd);
        SwBatTrace(L"OpenPdStatus", (ULONG)Status);
    }
    if (Dev->Pd != NULL) {
        Dev->DpAllowed = SwBatRegGet(L"DpAlt", 1) != 0;
        if (Dev->DpCallback == NULL) {
            UNICODE_STRING Name;
            OBJECT_ATTRIBUTES Oa;
            RtlInitUnicodeString(&Name, L"\\Callback\\SwitchWoaDpAlt");
            InitializeObjectAttributes(&Oa, &Name, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT | OBJ_KERNEL_HANDLE, NULL, NULL);
            SwBatTrace(L"DpAltCallbackStatus", (ULONG)ExCreateCallback(&Dev->DpCallback, &Oa, TRUE, TRUE));
        }
        WdfWaitLockAcquire(Dev->Lock, NULL);
        SwBatUsbcStart(Dev);
        WdfWaitLockRelease(Dev->Lock);
        if (Dev->UsbcTimer == NULL) {
            WDF_TIMER_CONFIG TimerConfig;
            WDF_OBJECT_ATTRIBUTES Attributes;
            WDF_TIMER_CONFIG_INIT(&TimerConfig, SwBatEvtUsbcTimer);
            TimerConfig.AutomaticSerialization = FALSE;
            WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBAT_TIMER);
            Attributes.ParentObject = Device;
            Attributes.ExecutionLevel = WdfExecutionLevelPassive;
            Status = WdfTimerCreate(&TimerConfig, &Attributes, &Dev->UsbcTimer);
            SwBatTrace(L"UsbcTimerCreateStatus", (ULONG)Status);
            if (NT_SUCCESS(Status)) {
                SwBatGetTimer(Dev->UsbcTimer)->Device = Device;
            } else {
                Dev->UsbcTimer = NULL;
            }
        }
        if (Dev->UsbcTimer != NULL) {
            WdfTimerStart(Dev->UsbcTimer, WDF_REL_TIMEOUT_IN_MS(USBC_POLL_MS));
        }
    }
    return STATUS_SUCCESS;
}

VOID SwBatEvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(Device);
    Dev->Stopping = TRUE;
    if (Dev->Timer != NULL) {
        WdfTimerStop(Dev->Timer, TRUE);
    }
    if (Dev->UsbcTimer != NULL) {
        WdfTimerStop(Dev->UsbcTimer, TRUE);
    }
    if (Dev->DpCallback != NULL) {
        ObDereferenceObject(Dev->DpCallback);       /* the object itself stays (OBJ_PERMANENT) */
        Dev->DpCallback = NULL;
    }
    if (Dev->ClassHandle != NULL) {
        (VOID)BatteryClassUnload(Dev->ClassHandle);
        Dev->ClassHandle = NULL;
    }
}

VOID SwBatEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputLength, size_t InputLength, ULONG IoControlCode)
{
    PSWBAT_DEVICE Dev = SwBatGetDevice(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_NOT_SUPPORTED;
    UNREFERENCED_PARAMETER(OutputLength);
    UNREFERENCED_PARAMETER(InputLength);
    UNREFERENCED_PARAMETER(IoControlCode);

    if (Dev->ClassHandle != NULL) {
        /* battc completes the IRP itself unless it does not handle it. */
        Status = BatteryClassIoctl(Dev->ClassHandle, WdfRequestWdmGetIrp(Request));
    }
    if (Status == STATUS_NOT_SUPPORTED) {
        WdfRequestComplete(Request, Status);
    }
}

NTSTATUS SwBatEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDFDEVICE Device;
    PSWBAT_DEVICE Dev;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwBatEvtPrepareHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwBatEvtSelfManagedIoInit;
    Pnp.EvtDeviceSelfManagedIoCleanup = SwBatEvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBAT_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = SwBatGetDevice(Device);
    Status = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoDeviceControl = SwBatEvtIoDeviceControl;
    return WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwBatParamsPath)) {
        RtlCopyMemory(SwBatParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwBatParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwBatParamsPath);
    }
    SwBatTrace(L"DriverVersion", 10);

    WDF_DRIVER_CONFIG_INIT(&Config, SwBatEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
