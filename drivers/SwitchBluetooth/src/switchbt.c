/*
 * Nintendo Switch Bluetooth driver, KMDF. Stage 1: link check.
 *
 * ACPI device BTH0 (ACPI\SWOA0B01), UEFI R390+:
 *   Memory32Fixed 0x70006300: UART-D (Tegra HSUART, 16550 layout, registers
 *     4 bytes apart). The BCM4356A3 Bluetooth core is on it (H4 transport).
 *   Interrupt GSI 122: UART-D (not used in stage 1).
 * UEFI R390 sets the pins, clocks UART-D from PLLP (408 MHz) and powers the
 * chip (PH4). The Broadcom ROM listens at 115200 8N1 after power-up.
 *
 * Stage 1 only checks the link: at start the driver sends HCI_Reset,
 * Read_Local_Version, Read_BD_ADDR and Read_Local_Name by polling the UART,
 * and records the answers under Services\switchbt\Parameters. Hooking the
 * chip to the Windows Bluetooth stack (BthMini) is the next stage.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>

#define UART_LENGTH       0x40

/* Tegra HSUART, in ULONG units. */
#define UART_THR          (0x00 / 4)
#define UART_IER          (0x04 / 4)
#define UART_FCR          (0x08 / 4)
#define UART_LCR          (0x0C / 4)
#define UART_MCR          (0x10 / 4)
#define UART_LSR          (0x14 / 4)
#define UART_DLL          (0x00 / 4)
#define UART_DLM          (0x04 / 4)
#define UART_LSR_DR       0x01u
#define UART_LSR_THRE     0x20u
#define UART_DIV_115200   221u          /* 408 MHz / 16 / 221 = 115384 baud */

/* HCI (H4): command 01 opcode(2) len params; event 04 code len params. */
#define HCI_COMMAND_PKT   0x01
#define HCI_EVENT_PKT     0x04
#define HCI_EV_COMPLETE   0x0E
#define HCI_RESET         0x0C03
#define HCI_READ_NAME     0x0C14
#define HCI_READ_VERSION  0x1001
#define HCI_READ_BD_ADDR  0x1009

typedef struct _SWBT_DEVICE {
    volatile ULONG *Uart;
} SWBT_DEVICE, *PSWBT_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBT_DEVICE, SwBtGetDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwBtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwBtEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwBtEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwBtEvtSelfManagedIoInit;

static WCHAR SwBtParamsPath[256];

static VOID SwBtTrace(PCWSTR Name, ULONG Value)
{
    if (SwBtParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static VOID SwBtTraceString(PCWSTR Name, PCWSTR Value)
{
    if (SwBtParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath, Name, REG_SZ, (PVOID)Value,
                                    (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR)));
    }
}

/* ------------------------------------------------------------------ UART */

static ULONG SwBtRead(PSWBT_DEVICE Dev, ULONG Reg)
{
    return READ_REGISTER_ULONG((PULONG)&Dev->Uart[Reg]);
}

static VOID SwBtWrite(PSWBT_DEVICE Dev, ULONG Reg, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)&Dev->Uart[Reg], Value);
}

static VOID SwBtUartInit(PSWBT_DEVICE Dev)
{
    SwBtWrite(Dev, UART_IER, 0);
    SwBtWrite(Dev, UART_LCR, 0x80);                 /* divisor latch */
    SwBtWrite(Dev, UART_DLL, UART_DIV_115200 & 0xFF);
    SwBtWrite(Dev, UART_DLM, UART_DIV_115200 >> 8);
    SwBtWrite(Dev, UART_LCR, 0x03);                 /* 8N1 */
    SwBtWrite(Dev, UART_FCR, 0x07);                 /* FIFOs on and cleared */
    SwBtWrite(Dev, UART_MCR, 0x02);                 /* RTS on */
}

static ULONGLONG SwBtNowMs(VOID)
{
    return KeQueryInterruptTime() / 10000;
}

static BOOLEAN SwBtSend(PSWBT_DEVICE Dev, const UCHAR *Buffer, ULONG Length)
{
    ULONG Index;
    for (Index = 0; Index < Length; ++Index) {
        ULONGLONG End = SwBtNowMs() + 50;
        while ((SwBtRead(Dev, UART_LSR) & UART_LSR_THRE) == 0) {
            if (SwBtNowMs() > End) {
                return FALSE;
            }
            KeStallExecutionProcessor(10);
        }
        SwBtWrite(Dev, UART_THR, Buffer[Index]);
    }
    return TRUE;
}

/*
 * Reads one HCI event into Buffer (04 code len params) within TimeoutMs.
 * The loop polls faster than 115200 baud fills the FIFO, so nothing is lost.
 */
static ULONG SwBtReadEvent(PSWBT_DEVICE Dev, UCHAR *Buffer, ULONG Max, ULONG TimeoutMs)
{
    ULONGLONG End = SwBtNowMs() + TimeoutMs;
    ULONG Got = 0, Need = 3;
    while (Got < Need) {
        UCHAR Byte;
        if ((SwBtRead(Dev, UART_LSR) & UART_LSR_DR) == 0) {
            if (SwBtNowMs() > End) {
                return 0;
            }
            KeStallExecutionProcessor(5);
            continue;
        }
        Byte = (UCHAR)SwBtRead(Dev, UART_THR);
        if (Got == 0 && Byte != HCI_EVENT_PKT) {
            continue;                   /* skip anything before an event */
        }
        if (Got < Max) {
            Buffer[Got] = Byte;
        }
        if (++Got == 3) {
            Need = 3 + Byte;
        }
    }
    return Got;
}

/* Sends a command without parameters; returns its Command Complete event length. */
static ULONG SwBtCommand(PSWBT_DEVICE Dev, USHORT Opcode, UCHAR *Event, ULONG Max)
{
    UCHAR Cmd[4] = { HCI_COMMAND_PKT, (UCHAR)Opcode, (UCHAR)(Opcode >> 8), 0 };
    ULONG Tries, Length;
    if (!SwBtSend(Dev, Cmd, sizeof(Cmd))) {
        return 0;
    }
    for (Tries = 0; Tries < 4; ++Tries) {
        Length = SwBtReadEvent(Dev, Event, Max, 1000);
        if (Length == 0) {
            return 0;
        }
        /* 04 0E len ncmd opcode(2) status ... */
        if (Length >= 7 && Event[1] == HCI_EV_COMPLETE && Event[4] == (UCHAR)Opcode &&
            Event[5] == (UCHAR)(Opcode >> 8)) {
            return Length;
        }
    }
    return 0;
}

static VOID SwBtProbe(PSWBT_DEVICE Dev)
{
    UCHAR Event[260];
    WCHAR Text[256];
    ULONG Length, Index;

    SwBtUartInit(Dev);
    KeStallExecutionProcessor(100);
    while ((SwBtRead(Dev, UART_LSR) & UART_LSR_DR) != 0) {
        (VOID)SwBtRead(Dev, UART_THR);
    }

    Length = SwBtCommand(Dev, HCI_RESET, Event, sizeof(Event));
    SwBtTrace(L"HciReset", Length >= 7 ? Event[6] + 0x100u : 0);     /* 0x100 = answered, status 0 */
    if (Length < 7) {
        SwBtTraceString(L"Result", L"no answer to HCI_Reset");
        return;
    }

    Length = SwBtCommand(Dev, HCI_READ_VERSION, Event, sizeof(Event));
    if (Length >= 15 && Event[6] == 0) {
        SwBtTrace(L"HciVersion", Event[7]);
        SwBtTrace(L"HciRevision", Event[8] | (Event[9] << 8));
        SwBtTrace(L"LmpVersion", Event[10]);
        SwBtTrace(L"Manufacturer", Event[11] | (Event[12] << 8));
        SwBtTrace(L"LmpSubversion", Event[13] | (Event[14] << 8));
    }

    Length = SwBtCommand(Dev, HCI_READ_BD_ADDR, Event, sizeof(Event));
    if (Length >= 13 && Event[6] == 0) {
        (VOID)RtlStringCchPrintfW(Text, RTL_NUMBER_OF(Text), L"%02X:%02X:%02X:%02X:%02X:%02X",
                                  Event[12], Event[11], Event[10], Event[9], Event[8], Event[7]);
        SwBtTraceString(L"BdAddr", Text);
    }

    Length = SwBtCommand(Dev, HCI_READ_NAME, Event, sizeof(Event));
    if (Length >= 8 && Event[6] == 0) {
        for (Index = 0; Index + 7 < Length && Index < RTL_NUMBER_OF(Text) - 1 && Event[7 + Index] != 0; ++Index) {
            UCHAR C = Event[7 + Index];
            Text[Index] = (C >= 0x20 && C < 0x7F) ? (WCHAR)C : L'?';
        }
        Text[Index] = L'\0';
        SwBtTraceString(L"LocalName", Text);
    }
    SwBtTraceString(L"Result", L"ok");
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwBtEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc != NULL && Desc->Type == CmResourceTypeMemory && Dev->Uart == NULL) {
            if (Desc->u.Memory.Length < UART_LENGTH) {
                return STATUS_DEVICE_CONFIGURATION_ERROR;
            }
            Dev->Uart = (volatile ULONG *)MmMapIoSpaceEx(Desc->u.Memory.Start, UART_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
            if (Dev->Uart == NULL) {
                return STATUS_INSUFFICIENT_RESOURCES;
            }
        }
    }
    SwBtTrace(L"HaveUart", Dev->Uart != NULL);
    return Dev->Uart == NULL ? STATUS_DEVICE_CONFIGURATION_ERROR : STATUS_SUCCESS;
}

NTSTATUS SwBtEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);

    if (Dev->Uart != NULL) {
        MmUnmapIoSpace((PVOID)Dev->Uart, UART_LENGTH);
        Dev->Uart = NULL;
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwBtEvtSelfManagedIoInit(WDFDEVICE Device)
{
    SwBtProbe(SwBtGetDevice(Device));
    return STATUS_SUCCESS;
}

NTSTATUS SwBtEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwBtEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwBtEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwBtEvtSelfManagedIoInit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBT_DEVICE);
    return WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwBtParamsPath)) {
        RtlCopyMemory(SwBtParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwBtParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath);
    }
    SwBtTrace(L"DriverVersion", 1);
    WDF_DRIVER_CONFIG_INIT(&Config, SwBtEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
