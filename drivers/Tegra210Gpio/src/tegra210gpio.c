/*
 * Tegra210 GPIO controller driver for Windows on the Nintendo Switch.
 * GpioClx (GPIO class extension) client, memory-mapped, KMDF.
 *
 * Hardware (TRM / Linux drivers/gpio/gpio-tegra.c, tegra210 soc data):
 *   MMIO 0x6000D000, 8 banks x 4 ports x 8 pins = 256 pins.
 *   GPIO number = port * 8 + bit; port P belongs to bank P / 4.
 *   Per port register = base + bank * 0x100 + (port % 4) * 4 + reg:
 *     CNF 0x00 (1 = GPIO, 0 = SFIO), OE 0x10, OUT 0x20, IN 0x30,
 *     INT_STA 0x40, INT_ENB 0x50, INT_LVL 0x60, INT_CLR 0x70.
 *   Masked writes at reg + 0x80: value = (mask << 8) | bits (8-bit ports).
 *   INT_LVL per pin: bit (pin) LVL/rising, bit (8 + pin) EDGE, bit (16 + pin) DELTA (both).
 *   One GIC interrupt per bank (8 interrupt resources, in bank order).
 *
 * GpioClx bank = Tegra bank (32 pins); bank pin p = port (p / 8), bit (p % 8).
 * Pull-up/down and pad settings live in the pinmux block and are left as the
 * firmware set them.
 *
 * 1.0.1.0: ActiveBoth (the volume buttons) is no longer the hardware DELTA
 * edge mode. A bounce can put the two edges of a press so close together
 * that only one interrupt is seen; the button driver (hidinterrupt) then has
 * press and release swapped until Windows restarts (VOL- did not repeat while
 * held, and went down when released). Now the pin is a level interrupt for
 * the level it is not at (low first), and each interrupt arms the other
 * level, so the interrupts always alternate press / release and follow the
 * real line. A passive timer writes PX6/PX7 counters to the Parameters key
 * every 2 s when they change.
 */
#include <ntddk.h>
#include <wdf.h>
#include <gpioclx.h>

#define TEGRA_GPIO_BANKS        8
#define TEGRA_GPIO_PINS_PER_BANK 32
#define TEGRA_GPIO_MMIO_SIZE    0x1000

#define REG_CNF      0x00
#define REG_OE       0x10
#define REG_OUT      0x20
#define REG_IN       0x30
#define REG_INT_STA  0x40
#define REG_INT_ENB  0x50
#define REG_INT_LVL  0x60
#define REG_INT_CLR  0x70
#define REG_MASKED   0x80

#define LVL_LEVEL_LOW    0x000000
#define LVL_LEVEL_HIGH   0x000001
#define LVL_EDGE_FALLING 0x000100
#define LVL_EDGE_RISING  0x000101
#define LVL_EDGE_BOTH    0x010100
#define LVL_PIN_MASK     0x010101

typedef struct _TEGRA_GPIO_CONTEXT {
    volatile UCHAR *Base;
    ULONG Length;
    ULONG ActiveBoth[TEGRA_GPIO_BANKS];     /* pins whose ActiveBoth is emulated with level flips */
    WDFTIMER TraceTimer;
} TEGRA_GPIO_CONTEXT, *PTEGRA_GPIO_CONTEXT;

typedef struct _TEGRA_GPIO_TIMER_CONTEXT {
    PTEGRA_GPIO_CONTEXT Gpio;
} TEGRA_GPIO_TIMER_CONTEXT, *PTEGRA_GPIO_TIMER_CONTEXT;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TEGRA_GPIO_TIMER_CONTEXT, TegraGpioTimerContext)

/* Volume buttons: bank 5, pins 30 (PX6, VOL+) and 31 (PX7, VOL-). Counters are
 * updated at any IRQL and copied to the registry by a passive timer. */
#define TEGRA_GPIO_BTN_BANK  5
#define TEGRA_GPIO_BTN_FIRST 30
#define TEGRA_GPIO_TRACE_N   24
static volatile LONG TegraGpioBtnIrq[2], TegraGpioBtnLow[2], TegraGpioBtnHigh[2];
static volatile LONG TegraGpioBtnEnable[2], TegraGpioBtnMask[2], TegraGpioBtnUnmask[2], TegraGpioBtnReconf[2];
static volatile LONG TegraGpioBtnLastEnable[2];
static ULONG TegraGpioTraceSnapshot[TEGRA_GPIO_TRACE_N];
static BOOLEAN TegraGpioTraceWritten;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD TegraGpioEvtDeviceAdd;
EVT_WDF_DRIVER_UNLOAD TegraGpioEvtDriverUnload;

/* Diagnostics: PASSIVE_LEVEL steps record DWORDs under
 * HKLM\SYSTEM\CurrentControlSet\Services\tegra210gpio\Parameters. */
static WCHAR TegraGpioParamsPath[256];

static VOID TegraGpioTrace(PCWSTR Name, ULONG Value)
{
    if (TegraGpioParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, TegraGpioParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static volatile ULONG *TegraGpioReg(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, ULONG Port, ULONG Reg)
{
    return (volatile ULONG *)(Ctx->Base + Bank * 0x100 + Port * 4 + Reg);
}

/* Masked write of one 8-bit port: only bits in Mask change, to Bits. */
static VOID TegraGpioMaskedWrite(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, ULONG Port, ULONG Reg, ULONG Mask, ULONG Bits)
{
    if ((Mask & 0xFF) != 0) {
        WRITE_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Bank, Port, Reg + REG_MASKED),
                             ((Mask & 0xFF) << 8) | (Bits & Mask & 0xFF));
    }
}

/* Apply a 32-bit bank mask through four per-port masked writes. */
static VOID TegraGpioMaskedWriteBank(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, ULONG Reg, ULONG64 Mask, ULONG64 Bits)
{
    ULONG Port;
    for (Port = 0; Port < 4; ++Port) {
        TegraGpioMaskedWrite(Ctx, Bank, Port, Reg, (ULONG)(Mask >> (Port * 8)), (ULONG)(Bits >> (Port * 8)));
    }
}

static ULONG64 TegraGpioReadBank(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, ULONG Reg)
{
    ULONG64 Value = 0;
    ULONG Port;
    for (Port = 0; Port < 4; ++Port) {
        Value |= (ULONG64)(READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Bank, Port, Reg)) & 0xFF) << (Port * 8);
    }
    return Value;
}

static ULONG TegraGpioLevelType(KINTERRUPT_MODE Mode, KINTERRUPT_POLARITY Polarity)
{
    if (Mode == LevelSensitive) {
        return (Polarity == InterruptActiveLow) ? LVL_LEVEL_LOW : LVL_LEVEL_HIGH;
    }
    if (Polarity == InterruptActiveBoth) {
        return LVL_EDGE_BOTH;
    }
    return (Polarity == InterruptActiveLow) ? LVL_EDGE_FALLING : LVL_EDGE_RISING;
}

/* INT_LVL has three fields per pin, so it is read-modify-written. GpioClx
 * serializes interrupt configuration per bank for memory-mapped clients. */
static VOID TegraGpioSetLevel(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, PIN_NUMBER Pin, ULONG Type)
{
    volatile ULONG *Lvl = TegraGpioReg(Ctx, Bank, Pin / 8, REG_INT_LVL);
    ULONG Bit = Pin % 8;
    ULONG Value = READ_REGISTER_ULONG((PULONG)Lvl);
    Value &= ~(LVL_PIN_MASK << Bit);
    Value |= Type << Bit;
    WRITE_REGISTER_ULONG((PULONG)Lvl, Value);
}

static LONG TegraGpioBtnIndex(BANK_ID Bank, PIN_NUMBER Pin)
{
    if (Bank == TEGRA_GPIO_BTN_BANK && Pin >= TEGRA_GPIO_BTN_FIRST && Pin < TEGRA_GPIO_BTN_FIRST + 2) {
        return (LONG)(Pin - TEGRA_GPIO_BTN_FIRST);
    }
    return -1;
}

/* Emulated ActiveBoth: a level interrupt for the level the line is not at.
 * Low first, so a button already held when the interrupt is connected is
 * reported as a press (hidinterrupt starts from "released"). */
static VOID TegraGpioArmActiveBoth(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, PIN_NUMBER Pin)
{
    Ctx->ActiveBoth[Bank] |= 1u << Pin;
    TegraGpioSetLevel(Ctx, Bank, Pin, LVL_LEVEL_LOW);
}

/* ISR path (ClearActiveInterrupts), for each emulated pin that fired: the line
 * reached the armed level, so arm the other one. If the line has already gone
 * back, the new level asserts at once and that edge is not lost. */
static VOID TegraGpioFlipActiveBoth(PTEGRA_GPIO_CONTEXT Ctx, BANK_ID Bank, PIN_NUMBER Pin)
{
    volatile ULONG *Lvl = TegraGpioReg(Ctx, Bank, Pin / 8, REG_INT_LVL);
    ULONG Bit = Pin % 8;
    ULONG Value = READ_REGISTER_ULONG((PULONG)Lvl);
    BOOLEAN WasHigh = (Value & (LVL_LEVEL_HIGH << Bit)) != 0;
    LONG Btn = TegraGpioBtnIndex(Bank, Pin);
    Value &= ~(LVL_PIN_MASK << Bit);
    Value |= (WasHigh ? LVL_LEVEL_LOW : LVL_LEVEL_HIGH) << Bit;
    WRITE_REGISTER_ULONG((PULONG)Lvl, Value);
    if (Btn >= 0) {
        InterlockedIncrement(WasHigh ? &TegraGpioBtnHigh[Btn] : &TegraGpioBtnLow[Btn]);
    }
}

/* Every 2 s: write the button counters and port X when something changed. */
static VOID TegraGpioEvtTraceTimer(WDFTIMER Timer)
{
    static const PCWSTR Names[TEGRA_GPIO_TRACE_N] = {
        L"VolUpIrq", L"VolUpLow", L"VolUpHigh", L"VolUpEnable", L"VolUpMask", L"VolUpUnmask", L"VolUpReconfigure", L"VolUpLastEnable",
        L"VolDownIrq", L"VolDownLow", L"VolDownHigh", L"VolDownEnable", L"VolDownMask", L"VolDownUnmask", L"VolDownReconfigure", L"VolDownLastEnable",
        L"X_CNF", L"X_OE", L"X_IN", L"X_INT_STA", L"X_INT_ENB", L"X_INT_LVL", L"ActiveBothBank5", L"TraceVersion" };
    PTEGRA_GPIO_CONTEXT Ctx = TegraGpioTimerContext(Timer)->Gpio;
    ULONG Now[TEGRA_GPIO_TRACE_N];
    ULONG Index;
    if (Ctx == NULL || Ctx->Base == NULL) {
        return;
    }
    for (Index = 0; Index < 2; ++Index) {
        Now[Index * 8 + 0] = (ULONG)TegraGpioBtnIrq[Index];
        Now[Index * 8 + 1] = (ULONG)TegraGpioBtnLow[Index];
        Now[Index * 8 + 2] = (ULONG)TegraGpioBtnHigh[Index];
        Now[Index * 8 + 3] = (ULONG)TegraGpioBtnEnable[Index];
        Now[Index * 8 + 4] = (ULONG)TegraGpioBtnMask[Index];
        Now[Index * 8 + 5] = (ULONG)TegraGpioBtnUnmask[Index];
        Now[Index * 8 + 6] = (ULONG)TegraGpioBtnReconf[Index];
        Now[Index * 8 + 7] = (ULONG)TegraGpioBtnLastEnable[Index];
    }
    Now[16] = READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, 5, 3, REG_CNF));
    Now[17] = READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, 5, 3, REG_OE));
    Now[18] = READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, 5, 3, REG_IN));
    Now[19] = READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, 5, 3, REG_INT_STA));
    Now[20] = READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, 5, 3, REG_INT_ENB));
    Now[21] = READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, 5, 3, REG_INT_LVL));
    Now[22] = Ctx->ActiveBoth[5];
    Now[23] = 0x0101;
    for (Index = 0; Index < TEGRA_GPIO_TRACE_N; ++Index) {
        if (!TegraGpioTraceWritten || Now[Index] != TegraGpioTraceSnapshot[Index]) {
            TegraGpioTraceSnapshot[Index] = Now[Index];
            TegraGpioTrace(Names[Index], Now[Index]);
        }
    }
    TegraGpioTraceWritten = TRUE;
}

/* ---------------------------------------------------------------- GpioClx */

static NTSTATUS TegraGpioPrepareController(WDFDEVICE Device, PVOID Context, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    ULONG Count = WdfCmResourceListGetCount(ResourcesTranslated);
    ULONG Index;
    NTSTATUS Status = STATUS_SUCCESS;
    UNREFERENCED_PARAMETER(ResourcesRaw);

    TegraGpioTrace(L"PrepareResourceCount", Count);
    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(ResourcesTranslated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeMemory && Ctx->Base == NULL) {
            if (Desc->u.Memory.Length < TEGRA_GPIO_MMIO_SIZE) {
                Status = STATUS_DEVICE_CONFIGURATION_ERROR;
                break;
            }
            Ctx->Base = (volatile UCHAR *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length,
                                                          PAGE_READWRITE | PAGE_NOCACHE);
            if (Ctx->Base == NULL) {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
            Ctx->Length = Desc->u.Memory.Length;
            TegraGpioTrace(L"MmioLength", Ctx->Length);
        }
    }
    if (NT_SUCCESS(Status) && Ctx->Base == NULL) {
        Status = STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (NT_SUCCESS(Status) && Ctx->TraceTimer == NULL) {
        WDF_TIMER_CONFIG TimerConfig;
        WDF_OBJECT_ATTRIBUTES TimerAttributes;
        WDF_TIMER_CONFIG_INIT_PERIODIC(&TimerConfig, TegraGpioEvtTraceTimer, 2000);
        TimerConfig.AutomaticSerialization = FALSE;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&TimerAttributes, TEGRA_GPIO_TIMER_CONTEXT);
        TimerAttributes.ParentObject = Device;
        TimerAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        if (NT_SUCCESS(WdfTimerCreate(&TimerConfig, &TimerAttributes, &Ctx->TraceTimer))) {
            TegraGpioTimerContext(Ctx->TraceTimer)->Gpio = Ctx;
            WdfTimerStart(Ctx->TraceTimer, WDF_REL_TIMEOUT_IN_MS(2000));
        } else {
            Ctx->TraceTimer = NULL;
        }
    }
    TegraGpioTrace(L"PrepareStatus", (ULONG)Status);
    return Status;
}

static NTSTATUS TegraGpioReleaseController(WDFDEVICE Device, PVOID Context)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    UNREFERENCED_PARAMETER(Device);
    if (Ctx->TraceTimer != NULL) {
        WdfTimerStop(Ctx->TraceTimer, TRUE);
        WdfObjectDelete(Ctx->TraceTimer);
        Ctx->TraceTimer = NULL;
    }
    if (Ctx->Base != NULL) {
        MmUnmapIoSpace((PVOID)Ctx->Base, Ctx->Length);
        Ctx->Base = NULL;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioStartController(PVOID Context, BOOLEAN RestoreContext, WDF_POWER_DEVICE_STATE PreviousPowerState)
{
    UNREFERENCED_PARAMETER(Context);
    TegraGpioTrace(L"StartCalled", 1);
    UNREFERENCED_PARAMETER(RestoreContext);
    UNREFERENCED_PARAMETER(PreviousPowerState);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioStopController(PVOID Context, BOOLEAN SaveContext, WDF_POWER_DEVICE_STATE TargetState)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(SaveContext);
    UNREFERENCED_PARAMETER(TargetState);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioQueryControllerBasicInformation(PVOID Context, PCLIENT_CONTROLLER_BASIC_INFORMATION Info)
{
    UNREFERENCED_PARAMETER(Context);
    TegraGpioTrace(L"QueryBasicCalled", 1);
    Info->Version = GPIO_CONTROLLER_BASIC_INFORMATION_VERSION;
    Info->Size = sizeof(*Info);
    Info->TotalPins = TEGRA_GPIO_BANKS * TEGRA_GPIO_PINS_PER_BANK;
    Info->NumberOfPinsPerBank = TEGRA_GPIO_PINS_PER_BANK;
    Info->DeviceIdleTimeout = 0;
    Info->Flags.AsULONG = 0;
    Info->Flags.MemoryMappedController = 1;
    Info->Flags.FormatIoRequestsAsMasks = 1;
    Info->Flags.EmulateDebouncing = 1;
    return STATUS_SUCCESS;
}

/* Bank n uses the n-th interrupt resource (the DSDT lists them in bank
 * order). GpioClx hands over the resource lists, and the mapping is an index
 * into that whole list (the memory descriptor comes first), not a count of
 * interrupts only. Version 1 of the driver returned 0..7 and got Code 10. */
static NTSTATUS TegraGpioQuerySetControllerInformation(PVOID Context, PCLIENT_CONTROLLER_QUERY_SET_INFORMATION_INPUT Input,
                                                       PCLIENT_CONTROLLER_QUERY_SET_INFORMATION_OUTPUT Output)
{
    USHORT Bank = 0;
    ULONG Index, Count;
    WDFCMRESLIST List;
    UNREFERENCED_PARAMETER(Context);

    TegraGpioTrace(L"QuerySetType", (ULONG)Input->RequestType);
    if (Input->RequestType != QueryBankInterruptBindingInformation || Output == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    TegraGpioTrace(L"QuerySetInputSize", Input->Size);
    TegraGpioTrace(L"QuerySetTotalBanks", Input->BankInterruptBinding.TotalBanks);
    if (Input->BankInterruptBinding.TotalBanks > TEGRA_GPIO_BANKS) {
        return STATUS_INVALID_PARAMETER;
    }
    List = Input->BankInterruptBinding.ResourcesTranslated;
    Count = WdfCmResourceListGetCount(List);
    for (Index = 0; Index < Count && Bank < Input->BankInterruptBinding.TotalBanks; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(List, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeInterrupt) {
            Output->BankInterruptBinding.ResourceMapping[Bank++] = Index;
        }
    }
    TegraGpioTrace(L"QuerySetBoundBanks", Bank);
    TegraGpioTrace(L"QuerySetBank0Index", (Bank > 0) ? Output->BankInterruptBinding.ResourceMapping[0] : 0xFFFF);
    while (Bank < Input->BankInterruptBinding.TotalBanks) {
        Output->BankInterruptBinding.ResourceMapping[Bank++] = GPIO_BANK_INTERRUPT_BINDING_RESERVED_INDEX;
    }
    Output->Version = GPIO_BANK_INTERRUPT_BINDING_INFORMATION_OUTPUT_VERSION;
    Output->Size = (USHORT)(FIELD_OFFSET(CLIENT_CONTROLLER_QUERY_SET_INFORMATION_OUTPUT, BankInterruptBinding.ResourceMapping) +
                            Input->BankInterruptBinding.TotalBanks * sizeof(ULONG));
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioConnectIoPins(PVOID Context, PGPIO_CONNECT_IO_PINS_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    ULONG Index;
    if (Params->ConnectMode != ConnectModeInput && Params->ConnectMode != ConnectModeOutput) {
        return STATUS_NOT_SUPPORTED;
    }
    for (Index = 0; Index < Params->PinCount; ++Index) {
        PIN_NUMBER Pin = Params->PinNumberTable[Index];
        ULONG Port = Pin / 8, Bit = 1u << (Pin % 8);
        if (Pin >= TEGRA_GPIO_PINS_PER_BANK) {
            return STATUS_INVALID_PARAMETER;
        }
        TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_OE, Bit, (Params->ConnectMode == ConnectModeOutput) ? Bit : 0);
        TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_CNF, Bit, Bit);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioDisconnectIoPins(PVOID Context, PGPIO_DISCONNECT_IO_PINS_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    ULONG Index;
    /* Leave the pin in GPIO mode but stop driving it. */
    for (Index = 0; Index < Params->PinCount; ++Index) {
        PIN_NUMBER Pin = Params->PinNumberTable[Index];
        if (Pin < TEGRA_GPIO_PINS_PER_BANK && Params->DisconnectMode == ConnectModeOutput) {
            TegraGpioMaskedWrite(Ctx, Params->BankId, Pin / 8, REG_OE, 1u << (Pin % 8), 0);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioReadPinsMask(PVOID Context, PGPIO_READ_PINS_MASK_PARAMETERS Params)
{
    *Params->PinValues = TegraGpioReadBank((PTEGRA_GPIO_CONTEXT)Context, Params->BankId, REG_IN);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioWritePinsMask(PVOID Context, PGPIO_WRITE_PINS_MASK_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    TegraGpioMaskedWriteBank(Ctx, Params->BankId, REG_OUT, Params->SetMask, Params->SetMask);
    TegraGpioMaskedWriteBank(Ctx, Params->BankId, REG_OUT, Params->ClearMask, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioEnableInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    PIN_NUMBER Pin = Params->PinNumber;
    ULONG Port = Pin / 8, Bit = 1u << (Pin % 8);
    LONG Btn = TegraGpioBtnIndex(Params->BankId, Pin);
    if (Pin >= TEGRA_GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Btn >= 0) {
        InterlockedIncrement(&TegraGpioBtnEnable[Btn]);
        InterlockedExchange(&TegraGpioBtnLastEnable[Btn], (LONG)(((ULONG)Params->InterruptMode << 8) | (ULONG)Params->Polarity));
    }
    TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_INT_ENB, Bit, 0);
    TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_OE, Bit, 0);
    TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_CNF, Bit, Bit);
    if (Params->Polarity == InterruptActiveBoth) {
        TegraGpioArmActiveBoth(Ctx, Params->BankId, Pin);
    } else {
        Ctx->ActiveBoth[Params->BankId] &= ~(1u << Pin);
        TegraGpioSetLevel(Ctx, Params->BankId, Pin, TegraGpioLevelType(Params->InterruptMode, Params->Polarity));
    }
    WRITE_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Params->BankId, Port, REG_INT_CLR), Bit);
    TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_INT_ENB, Bit, Bit);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioDisableInterrupt(PVOID Context, PGPIO_DISABLE_INTERRUPT_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    PIN_NUMBER Pin = Params->PinNumber;
    ULONG Port = Pin / 8, Bit = 1u << (Pin % 8);
    if (Pin >= TEGRA_GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }
    TegraGpioMaskedWrite(Ctx, Params->BankId, Port, REG_INT_ENB, Bit, 0);
    WRITE_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Params->BankId, Port, REG_INT_CLR), Bit);
    Ctx->ActiveBoth[Params->BankId] &= ~(1u << Pin);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioMaskInterrupts(PVOID Context, PGPIO_MASK_INTERRUPT_PARAMETERS Params)
{
    TegraGpioMaskedWriteBank((PTEGRA_GPIO_CONTEXT)Context, Params->BankId, REG_INT_ENB, Params->PinMask, 0);
    if (Params->BankId == TEGRA_GPIO_BTN_BANK) {
        if (Params->PinMask & (1ull << TEGRA_GPIO_BTN_FIRST)) InterlockedIncrement(&TegraGpioBtnMask[0]);
        if (Params->PinMask & (1ull << (TEGRA_GPIO_BTN_FIRST + 1))) InterlockedIncrement(&TegraGpioBtnMask[1]);
    }
    Params->FailedMask = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioUnmaskInterrupt(PVOID Context, PGPIO_ENABLE_INTERRUPT_PARAMETERS Params)
{
    PIN_NUMBER Pin = Params->PinNumber;
    ULONG Bit = 1u << (Pin % 8);
    LONG Btn = TegraGpioBtnIndex(Params->BankId, Pin);
    if (Pin >= TEGRA_GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Btn >= 0) {
        InterlockedIncrement(&TegraGpioBtnUnmask[Btn]);
    }
    TegraGpioMaskedWrite((PTEGRA_GPIO_CONTEXT)Context, Params->BankId, Pin / 8, REG_INT_ENB, Bit, Bit);
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioQueryActiveInterrupts(PVOID Context, PGPIO_QUERY_ACTIVE_INTERRUPTS_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    Params->ActiveMask = TegraGpioReadBank(Ctx, Params->BankId, REG_INT_STA) &
                         TegraGpioReadBank(Ctx, Params->BankId, REG_INT_ENB) & Params->EnabledMask;
    if (Params->BankId == TEGRA_GPIO_BTN_BANK) {
        if (Params->ActiveMask & (1ull << TEGRA_GPIO_BTN_FIRST)) InterlockedIncrement(&TegraGpioBtnIrq[0]);
        if (Params->ActiveMask & (1ull << (TEGRA_GPIO_BTN_FIRST + 1))) InterlockedIncrement(&TegraGpioBtnIrq[1]);
    }
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioClearActiveInterrupts(PVOID Context, PGPIO_CLEAR_ACTIVE_INTERRUPTS_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    ULONG Port;
    ULONG Flip = (ULONG)Params->ClearActiveMask & Ctx->ActiveBoth[Params->BankId];
    PIN_NUMBER Pin;
    /* Emulated ActiveBoth pins: arm the other level before clearing, so the
     * level the line is at now stops asserting. */
    for (Pin = 0; Flip != 0; ++Pin, Flip >>= 1) {
        if (Flip & 1) {
            TegraGpioFlipActiveBoth(Ctx, Params->BankId, Pin);
        }
    }
    for (Port = 0; Port < 4; ++Port) {
        ULONG Bits = (ULONG)(Params->ClearActiveMask >> (Port * 8)) & 0xFF;
        if (Bits != 0) {
            WRITE_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Params->BankId, Port, REG_INT_CLR), Bits);
        }
    }
    Params->FailedClearMask = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS TegraGpioReconfigureInterrupt(PVOID Context, PGPIO_RECONFIGURE_INTERRUPTS_PARAMETERS Params)
{
    PTEGRA_GPIO_CONTEXT Ctx = (PTEGRA_GPIO_CONTEXT)Context;
    LONG Btn = TegraGpioBtnIndex(Params->BankId, Params->PinNumber);
    if (Params->PinNumber >= TEGRA_GPIO_PINS_PER_BANK) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Btn >= 0) {
        InterlockedIncrement(&TegraGpioBtnReconf[Btn]);
    }
    if (Params->Polarity == InterruptActiveBoth) {
        /* Arm the level the line is not at now. */
        BOOLEAN High = (READ_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Params->BankId, Params->PinNumber / 8, REG_IN)) &
                        (1u << (Params->PinNumber % 8))) != 0;
        Ctx->ActiveBoth[Params->BankId] |= 1u << Params->PinNumber;
        TegraGpioSetLevel(Ctx, Params->BankId, Params->PinNumber, High ? LVL_LEVEL_LOW : LVL_LEVEL_HIGH);
    } else {
        Ctx->ActiveBoth[Params->BankId] &= ~(1u << Params->PinNumber);
        TegraGpioSetLevel(Ctx, Params->BankId, Params->PinNumber, TegraGpioLevelType(Params->InterruptMode, Params->Polarity));
    }
    WRITE_REGISTER_ULONG((PULONG)TegraGpioReg(Ctx, Params->BankId, Params->PinNumber / 8, REG_INT_CLR),
                         1u << (Params->PinNumber % 8));
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ KMDF */

NTSTATUS TegraGpioEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    NTSTATUS Status;

    Status = GPIO_CLX_ProcessAddDevicePreDeviceCreate(Driver, DeviceInit, &Attributes);
    TegraGpioTrace(L"AddPreStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    TegraGpioTrace(L"AddCreateStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = GPIO_CLX_ProcessAddDevicePostDeviceCreate(Driver, Device);
    TegraGpioTrace(L"AddPostStatus", (ULONG)Status);
    return Status;
}

VOID TegraGpioEvtDriverUnload(WDFDRIVER Driver)
{
    (VOID)GPIO_CLX_UnregisterClient(Driver);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    WDFDRIVER Driver;
    GPIO_CLIENT_REGISTRATION_PACKET Packet;
    NTSTATUS Status;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(TegraGpioParamsPath)) {
        RtlCopyMemory(TegraGpioParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&TegraGpioParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, TegraGpioParamsPath);
    }
    TegraGpioTrace(L"DriverVersion", 0x0101);

    WDF_DRIVER_CONFIG_INIT(&Config, TegraGpioEvtDeviceAdd);
    Config.EvtDriverUnload = TegraGpioEvtDriverUnload;
    Status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, &Driver);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }

    RtlZeroMemory(&Packet, sizeof(Packet));
    Packet.Version = GPIO_CLIENT_VERSION;
    Packet.Size = sizeof(Packet);
    Packet.ControllerContextSize = sizeof(TEGRA_GPIO_CONTEXT);
    Packet.CLIENT_PrepareController = TegraGpioPrepareController;
    Packet.CLIENT_ReleaseController = TegraGpioReleaseController;
    Packet.CLIENT_StartController = TegraGpioStartController;
    Packet.CLIENT_StopController = TegraGpioStopController;
    Packet.CLIENT_QueryControllerBasicInformation = TegraGpioQueryControllerBasicInformation;
    Packet.CLIENT_QuerySetControllerInformation = TegraGpioQuerySetControllerInformation;
    Packet.CLIENT_EnableInterrupt = TegraGpioEnableInterrupt;
    Packet.CLIENT_DisableInterrupt = TegraGpioDisableInterrupt;
    Packet.CLIENT_UnmaskInterrupt = TegraGpioUnmaskInterrupt;
    Packet.CLIENT_MaskInterrupts = TegraGpioMaskInterrupts;
    Packet.CLIENT_QueryActiveInterrupts = TegraGpioQueryActiveInterrupts;
    Packet.CLIENT_ClearActiveInterrupts = TegraGpioClearActiveInterrupts;
    Packet.CLIENT_ConnectIoPins = TegraGpioConnectIoPins;
    Packet.CLIENT_DisconnectIoPins = TegraGpioDisconnectIoPins;
    Packet.CLIENT_ReadGpioPinsUsingMask = TegraGpioReadPinsMask;
    Packet.CLIENT_WriteGpioPinsUsingMask = TegraGpioWritePinsMask;
    Packet.CLIENT_ReconfigureInterrupt = TegraGpioReconfigureInterrupt;

    Status = GPIO_CLX_RegisterClient(Driver, &Packet, RegistryPath);
    TegraGpioTrace(L"RegisterStatus", (ULONG)Status);
    return Status;
}
