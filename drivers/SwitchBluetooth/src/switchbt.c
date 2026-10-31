/*
 * Nintendo Switch Bluetooth driver, KMDF. Stage 2: Windows Bluetooth over BthMini.
 *
 * ACPI device BTH0 (ACPI\SWOA0B01), UEFI R390+:
 *   Memory32Fixed 0x70006300: UART-D (Tegra HSUART, 16550 layout, registers
 *     4 bytes apart). The BCM4356A3 Bluetooth core is on it (H4 transport).
 *   Interrupt GSI 122: UART-D.
 * UEFI R390 sets the pins, clocks UART-D from PLLP (408 MHz) and powers the
 * chip (PH4). The Broadcom ROM listens at 115200 8N1 after power-up.
 *
 * At each power-up (D0) the driver polls the chip once with interrupts off:
 * HCI_Reset, Read_Local_Version, Write_BD_ADDR (the ROM answers with the
 * placeholder AA:AA:AA:AA:AA:AA, so the driver makes one address per Switch
 * and keeps it in the registry), Read_BD_ADDR and Read_Local_Name. The
 * answers go to Services\switchbt\Parameters.
 *
 * Then it works as an extensible transport (bthxddi.h) under the inbox
 * BthMini: the driver creates one child device with the compatible ID
 * MS_BTHX_BTHMINI, so bth.inf puts BthMini and bthport on it. The child
 * forwards IOCTL_BTHX_* to this device:
 *   WRITE_HCI: one command or ACL packet; the ISR sends it from TxBuffer
 *     (16 bytes per transmitter-empty interrupt), the DPC completes it.
 *   READ_HCI: waits in the event or ACL queue; the ISR moves received bytes
 *     to a ring, the DPC cuts them into H4 packets and completes the oldest
 *     waiting read of that type (or keeps the packet until one comes).
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <initguid.h>
#include <bthxddi.h>

NTSYSAPI ULONG NTAPI RtlRandomEx(PULONG Seed);     /* ntoskrnl export, declared in ntifs.h */

#define UART_LENGTH       0x40

/* Tegra HSUART, in ULONG units. */
#define UART_THR          (0x00 / 4)
#define UART_IER          (0x04 / 4)
#define UART_IIR          (0x08 / 4)
#define UART_FCR          (0x08 / 4)
#define UART_LCR          (0x0C / 4)
#define UART_MCR          (0x10 / 4)
#define UART_LSR          (0x14 / 4)
#define UART_DLL          (0x00 / 4)
#define UART_DLM          (0x04 / 4)
#define UART_LSR_DR       0x01u
#define UART_LSR_THRE     0x20u
#define UART_IIR_NO_INT   0x01u
#define UART_IER_RX       0x15u         /* received data, line status, RX timeout */
#define UART_IER_TX       0x02u         /* transmitter empty */
#define UART_DIV_115200   221u          /* 408 MHz / 16 / 221 = 115384 baud */
#define UART_TX_BURST     16u           /* bytes per transmitter-empty interrupt (FIFO is 32) */

/* HCI (H4). */
#define HCI_COMMAND_PKT   0x01
#define HCI_ACL_PKT       0x02
#define HCI_SCO_PKT       0x03
#define HCI_EVENT_PKT     0x04
#define HCI_EV_COMPLETE   0x0E
#define HCI_RESET         0x0C03
#define HCI_READ_NAME     0x0C14
#define HCI_READ_VERSION  0x1001
#define HCI_READ_BD_ADDR  0x1009
#define BCM_WRITE_BD_ADDR 0xFC01
#define HCI_ACL_MAX_DATA  1021
#define HCI_MAX_PACKET    (4 + HCI_ACL_MAX_DATA)
#define NINTENDO_OUI      0x98B6E9u     /* the address is 98:B6:E9 + 3 bytes made once per Switch */

#define RX_RING_SIZE      8192u         /* power of two */
#define MAX_KEPT_PACKETS  64u
#define POOL_TAG          'tBwS'

/* Container ID for a device built into the PC (GUID_CONTAINERID_INTERNALLY_CONNECTED_DEVICE). */
DEFINE_GUID(SWBT_CONTAINER_INTERNAL, 0x00000000, 0x0000, 0x0000, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);

typedef struct _SWBT_PACKET {
    LIST_ENTRY Link;
    UCHAR Type;
    ULONG Length;
    UCHAR Data[1];
} SWBT_PACKET, *PSWBT_PACKET;

typedef struct _SWBT_DEVICE {
    volatile ULONG *Uart;
    WDFINTERRUPT Interrupt;
    BOOLEAN ChipOk;
    UCHAR BdAddr[6];                /* little endian, as HCI */

    /* Shared with the ISR (interrupt lock). */
    UCHAR RxRing[RX_RING_SIZE];
    ULONG RxHead;                   /* written by the ISR */
    ULONG RxTail;                   /* written by the DPC */
    ULONG RxOverflows;
    UCHAR TxBuffer[1 + HCI_MAX_PACKET + 8];
    ULONG TxLength;
    ULONG TxSent;
    BOOLEAN TxBusy;
    BOOLEAN TxDone;
    ULONG Ier;

    /* DPC and I/O (Lock). */
    WDFSPINLOCK Lock;
    WDFQUEUE TxQueue;
    WDFQUEUE EventQueue;
    WDFQUEUE AclQueue;
    WDFREQUEST TxRequest;
    ULONG TxRequestLength;
    LIST_ENTRY KeptEvents;
    LIST_ENTRY KeptAcl;
    ULONG KeptCount;
    ULONG Dropped;

    /* H4 parser (DPC only). */
    UCHAR RxPacket[1 + HCI_MAX_PACKET + 8];
    ULONG RxGot;
    ULONG RxNeed;
    BTHX_CAPABILITIES Caps;
} SWBT_DEVICE, *PSWBT_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWBT_DEVICE, SwBtGetDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwBtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwBtEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwBtEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY SwBtEvtD0Entry;
EVT_WDF_INTERRUPT_ISR SwBtEvtIsr;
EVT_WDF_INTERRUPT_DPC SwBtEvtDpc;
EVT_WDF_INTERRUPT_ENABLE SwBtEvtInterruptEnable;
EVT_WDF_INTERRUPT_DISABLE SwBtEvtInterruptDisable;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwBtEvtIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwBtEvtChildIoDeviceControl;

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

static BOOLEAN SwBtReadParam(PCWSTR Name, PULONG Value)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Result = 0;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Result;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (SwBtParamsPath[0] == L'\0' ||
        !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath, Table, NULL, NULL))) {
        return FALSE;
    }
    *Value = Result;
    return TRUE;
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
    SwBtWrite(Dev, UART_FCR, 0x07);                 /* FIFOs on and cleared, RX trigger 1 byte */
    SwBtWrite(Dev, UART_MCR, 0x02);                 /* RTS on */
}

static ULONGLONG SwBtNowMs(VOID)
{
    return KeQueryInterruptTime() / 10000;
}

/* ------------------------------------------------- polled HCI (D0 entry) */

static BOOLEAN SwBtPollSend(PSWBT_DEVICE Dev, const UCHAR *Buffer, ULONG Length)
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
 * The deadline holds even when the line never stops sending.
 */
static ULONG SwBtPollEvent(PSWBT_DEVICE Dev, UCHAR *Buffer, ULONG Max, ULONG TimeoutMs)
{
    ULONGLONG End = SwBtNowMs() + TimeoutMs;
    ULONG Got = 0, Need = 3;
    while (Got < Need) {
        UCHAR Byte;
        if (SwBtNowMs() > End) {
            return 0;
        }
        if ((SwBtRead(Dev, UART_LSR) & UART_LSR_DR) == 0) {
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

/* Sends a command; returns the length of its Command Complete event. */
static ULONG SwBtPollCommand(PSWBT_DEVICE Dev, USHORT Opcode, const UCHAR *Params, UCHAR ParamLength,
                             UCHAR *Event, ULONG Max)
{
    UCHAR Cmd[4 + 16];
    ULONG Tries, Length;
    if (ParamLength > 16) {
        return 0;
    }
    Cmd[0] = HCI_COMMAND_PKT;
    Cmd[1] = (UCHAR)Opcode;
    Cmd[2] = (UCHAR)(Opcode >> 8);
    Cmd[3] = ParamLength;
    if (ParamLength != 0) {
        RtlCopyMemory(&Cmd[4], Params, ParamLength);
    }
    if (!SwBtPollSend(Dev, Cmd, 4u + ParamLength)) {
        return 0;
    }
    for (Tries = 0; Tries < 4; ++Tries) {
        Length = SwBtPollEvent(Dev, Event, Max, 1000);
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

/* One address per Switch: 98:B6:E9 and 3 random bytes, made once and kept. */
static VOID SwBtChooseAddress(PSWBT_DEVICE Dev)
{
    ULONG Suffix;
    if (!SwBtReadParam(L"AddressSuffix", &Suffix)) {
        LARGE_INTEGER Pc = KeQueryPerformanceCounter(NULL);
        ULONG Seed = Pc.LowPart ^ (ULONG)KeQueryInterruptTime() ^ (ULONG)(ULONG_PTR)Dev;
        Suffix = RtlRandomEx(&Seed) & 0xFFFFFF;
        if (Suffix == 0 || Suffix == 0xFFFFFF) {
            Suffix = 0x5A5A5A;
        }
        SwBtTrace(L"AddressSuffix", Suffix);
    }
    Suffix &= 0xFFFFFF;
    Dev->BdAddr[0] = (UCHAR)Suffix;
    Dev->BdAddr[1] = (UCHAR)(Suffix >> 8);
    Dev->BdAddr[2] = (UCHAR)(Suffix >> 16);
    Dev->BdAddr[3] = (UCHAR)NINTENDO_OUI;
    Dev->BdAddr[4] = (UCHAR)(NINTENDO_OUI >> 8);
    Dev->BdAddr[5] = (UCHAR)(NINTENDO_OUI >> 16);
}

static BOOLEAN SwBtProbe(PSWBT_DEVICE Dev)
{
    UCHAR Event[260];
    WCHAR Text[256];
    ULONG Length, Index;

    SwBtUartInit(Dev);
    KeStallExecutionProcessor(100);
    for (Index = 0; Index < 4096 && (SwBtRead(Dev, UART_LSR) & UART_LSR_DR) != 0; ++Index) {
        (VOID)SwBtRead(Dev, UART_THR);
    }

    Length = SwBtPollCommand(Dev, HCI_RESET, NULL, 0, Event, sizeof(Event));
    SwBtTrace(L"HciReset", Length >= 7 ? Event[6] + 0x100u : 0);     /* 0x100 = answered, status 0 */
    if (Length < 7) {
        SwBtTraceString(L"Result", L"no answer to HCI_Reset");
        return FALSE;
    }

    Length = SwBtPollCommand(Dev, HCI_READ_VERSION, NULL, 0, Event, sizeof(Event));
    if (Length >= 15 && Event[6] == 0) {
        SwBtTrace(L"HciVersion", Event[7]);
        SwBtTrace(L"HciRevision", Event[8] | (Event[9] << 8));
        SwBtTrace(L"LmpVersion", Event[10]);
        SwBtTrace(L"Manufacturer", Event[11] | (Event[12] << 8));
        SwBtTrace(L"LmpSubversion", Event[13] | (Event[14] << 8));
    }

    SwBtChooseAddress(Dev);
    Length = SwBtPollCommand(Dev, BCM_WRITE_BD_ADDR, Dev->BdAddr, 6, Event, sizeof(Event));
    SwBtTrace(L"WriteBdAddr", Length >= 7 ? Event[6] + 0x100u : 0);

    Length = SwBtPollCommand(Dev, HCI_READ_BD_ADDR, NULL, 0, Event, sizeof(Event));
    if (Length >= 13 && Event[6] == 0) {
        (VOID)RtlStringCchPrintfW(Text, RTL_NUMBER_OF(Text), L"%02X:%02X:%02X:%02X:%02X:%02X",
                                  Event[12], Event[11], Event[10], Event[9], Event[8], Event[7]);
        SwBtTraceString(L"BdAddr", Text);
    }

    Length = SwBtPollCommand(Dev, HCI_READ_NAME, NULL, 0, Event, sizeof(Event));
    if (Length >= 8 && Event[6] == 0) {
        for (Index = 0; Index + 7 < Length && Index < RTL_NUMBER_OF(Text) - 1 && Event[7 + Index] != 0; ++Index) {
            UCHAR C = Event[7 + Index];
            Text[Index] = (C >= 0x20 && C < 0x7F) ? (WCHAR)C : L'?';
        }
        Text[Index] = L'\0';
        SwBtTraceString(L"LocalName", Text);
    }
    SwBtTraceString(L"Result", L"ok");
    return TRUE;
}

/* ---------------------------------------------------------- interrupt */

BOOLEAN SwBtEvtIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(WdfInterruptGetDevice(Interrupt));
    BOOLEAN Mine = FALSE, Wake = FALSE;
    ULONG Loops;
    UNREFERENCED_PARAMETER(MessageId);

    for (Loops = 0; Loops < 64; ++Loops) {
        ULONG Iir = SwBtRead(Dev, UART_IIR), Lsr, Count;
        if ((Iir & UART_IIR_NO_INT) != 0) {
            break;
        }
        Mine = TRUE;
        Lsr = SwBtRead(Dev, UART_LSR);
        for (Count = 0; Count < 64 && (Lsr & UART_LSR_DR) != 0; ++Count) {
            UCHAR Byte = (UCHAR)SwBtRead(Dev, UART_THR);
            if (Dev->RxHead - Dev->RxTail < RX_RING_SIZE) {
                Dev->RxRing[Dev->RxHead & (RX_RING_SIZE - 1)] = Byte;
                Dev->RxHead++;
            } else {
                Dev->RxOverflows++;
            }
            Wake = TRUE;
            Lsr = SwBtRead(Dev, UART_LSR);
        }
        if (Dev->TxBusy && (Lsr & UART_LSR_THRE) != 0) {
            for (Count = 0; Count < UART_TX_BURST && Dev->TxSent < Dev->TxLength; ++Count) {
                SwBtWrite(Dev, UART_THR, Dev->TxBuffer[Dev->TxSent++]);
            }
            if (Dev->TxSent == Dev->TxLength) {
                Dev->TxBusy = FALSE;
                Dev->TxDone = TRUE;
                Dev->Ier &= ~UART_IER_TX;
                SwBtWrite(Dev, UART_IER, Dev->Ier);
                Wake = TRUE;
            }
        } else if (!Dev->TxBusy && (Dev->Ier & UART_IER_TX) != 0) {
            Dev->Ier &= ~UART_IER_TX;
            SwBtWrite(Dev, UART_IER, Dev->Ier);
        }
    }
    if (Wake) {
        WdfInterruptQueueDpcForIsr(Interrupt);
    }
    return Mine;
}

NTSTATUS SwBtEvtInterruptEnable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(AssociatedDevice);
    UNREFERENCED_PARAMETER(Interrupt);
    Dev->RxHead = Dev->RxTail = 0;
    Dev->TxBusy = Dev->TxDone = FALSE;
    Dev->Ier = UART_IER_RX;
    SwBtWrite(Dev, UART_IER, Dev->Ier);
    return STATUS_SUCCESS;
}

NTSTATUS SwBtEvtInterruptDisable(WDFINTERRUPT Interrupt, WDFDEVICE AssociatedDevice)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(AssociatedDevice);
    UNREFERENCED_PARAMETER(Interrupt);
    Dev->Ier = 0;
    SwBtWrite(Dev, UART_IER, 0);
    return STATUS_SUCCESS;
}

/* -------------------------------------------------------- packets (DPC) */

/* Completes a READ_HCI with one packet. Lock held. */
static VOID SwBtCompleteRead(WDFREQUEST Request, UCHAR Type, const UCHAR *Data, ULONG Length)
{
    WDFMEMORY Memory;
    size_t Size = 0;
    PBTHX_HCI_READ_WRITE_CONTEXT Ctx;
    ULONG Bytes = FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) + Length;
    NTSTATUS Status = WdfRequestRetrieveOutputMemory(Request, &Memory);
    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
        return;
    }
    Ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)WdfMemoryGetBuffer(Memory, &Size);
    if (Size < Bytes) {
        WdfRequestCompleteWithInformation(Request, STATUS_BUFFER_TOO_SMALL, 0);
        return;
    }
    Ctx->Type = Type;
    Ctx->DataLen = Length;
    RtlCopyMemory(Ctx->Data, Data, Length);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Bytes);
}

/* A whole H4 packet from the chip (without the type byte). Lock held. */
static VOID SwBtDeliver(PSWBT_DEVICE Dev, UCHAR Type, const UCHAR *Data, ULONG Length)
{
    WDFQUEUE Queue = Type == HCI_EVENT_PKT ? Dev->EventQueue : Dev->AclQueue;
    PLIST_ENTRY Kept = Type == HCI_EVENT_PKT ? &Dev->KeptEvents : &Dev->KeptAcl;
    WDFREQUEST Request;
    PSWBT_PACKET Packet;

    if (IsListEmpty(Kept) && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &Request))) {
        SwBtCompleteRead(Request, Type, Data, Length);
        return;
    }
    if (Dev->KeptCount >= MAX_KEPT_PACKETS) {
        Dev->Dropped++;
        return;
    }
    Packet = (PSWBT_PACKET)ExAllocatePool2(POOL_FLAG_NON_PAGED, FIELD_OFFSET(SWBT_PACKET, Data) + Length, POOL_TAG);
    if (Packet == NULL) {
        Dev->Dropped++;
        return;
    }
    Packet->Type = Type;
    Packet->Length = Length;
    RtlCopyMemory(Packet->Data, Data, Length);
    InsertTailList(Kept, &Packet->Link);
    Dev->KeptCount++;
}

/* Hands kept packets to reads that came in since. Lock held. */
static VOID SwBtDrainKept(PSWBT_DEVICE Dev, PLIST_ENTRY Kept, WDFQUEUE Queue)
{
    WDFREQUEST Request;
    while (!IsListEmpty(Kept) && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Queue, &Request))) {
        PSWBT_PACKET Packet = CONTAINING_RECORD(RemoveHeadList(Kept), SWBT_PACKET, Link);
        Dev->KeptCount--;
        SwBtCompleteRead(Request, Packet->Type, Packet->Data, Packet->Length);
        ExFreePoolWithTag(Packet, POOL_TAG);
    }
}

static VOID SwBtFreeKept(PSWBT_DEVICE Dev)
{
    while (!IsListEmpty(&Dev->KeptEvents)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&Dev->KeptEvents), SWBT_PACKET, Link), POOL_TAG);
    }
    while (!IsListEmpty(&Dev->KeptAcl)) {
        ExFreePoolWithTag(CONTAINING_RECORD(RemoveHeadList(&Dev->KeptAcl), SWBT_PACKET, Link), POOL_TAG);
    }
    Dev->KeptCount = 0;
}

/* H4 parser: one received byte. Lock held. */
static VOID SwBtParse(PSWBT_DEVICE Dev, UCHAR Byte)
{
    UCHAR Type;
    if (Dev->RxGot == 0) {
        switch (Byte) {
        case HCI_EVENT_PKT: Dev->RxNeed = 1 + 2; break;      /* code, length */
        case HCI_ACL_PKT:   Dev->RxNeed = 1 + 4; break;      /* handle(2), length(2) */
        case HCI_SCO_PKT:   Dev->RxNeed = 1 + 3; break;      /* handle(2), length */
        default:            return;                          /* out of step: skip */
        }
    }
    Dev->RxPacket[Dev->RxGot++] = Byte;
    if (Dev->RxGot < Dev->RxNeed) {
        return;
    }
    /* At the end of the header, add the payload length. */
    Type = Dev->RxPacket[0];
    if (Type == HCI_EVENT_PKT && Dev->RxGot == 3) {
        Dev->RxNeed += Dev->RxPacket[2];
    } else if (Type == HCI_ACL_PKT && Dev->RxGot == 5) {
        ULONG Data = Dev->RxPacket[3] | (Dev->RxPacket[4] << 8);
        if (Data > HCI_ACL_MAX_DATA) {
            Dev->RxGot = 0;                                  /* corrupt: resynchronise */
            Dev->Dropped++;
            return;
        }
        Dev->RxNeed += Data;
    } else if (Type == HCI_SCO_PKT && Dev->RxGot == 4) {
        Dev->RxNeed += Dev->RxPacket[3];
    }
    if (Dev->RxGot < Dev->RxNeed) {
        return;
    }
    if (Type != HCI_SCO_PKT) {                               /* SCO over HCI is not offered */
        SwBtDeliver(Dev, Type, &Dev->RxPacket[1], Dev->RxGot - 1);
    }
    Dev->RxGot = 0;
}

/* Starts the next WRITE_HCI if the transmitter is free. Lock held. */
static VOID SwBtStartTx(PSWBT_DEVICE Dev)
{
    WDFREQUEST Request;
    WDFMEMORY Memory;
    size_t Size = 0;
    PBTHX_HCI_READ_WRITE_CONTEXT Ctx = NULL;

    while (Dev->TxRequest == NULL && NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Dev->TxQueue, &Request))) {
        NTSTATUS Status = WdfRequestRetrieveInputMemory(Request, &Memory);
        if (NT_SUCCESS(Status)) {
            Ctx = (PBTHX_HCI_READ_WRITE_CONTEXT)WdfMemoryGetBuffer(Memory, &Size);
            if (Size < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
                Ctx->DataLen > Size - FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
                Ctx->DataLen > HCI_MAX_PACKET || Ctx->DataLen == 0 ||
                (Ctx->Type != HciPacketCommand && Ctx->Type != HciPacketAclData)) {
                Status = STATUS_INVALID_PARAMETER;
            }
        }
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
            continue;
        }
        Dev->TxRequest = Request;
        Dev->TxRequestLength = Ctx->DataLen;
        WdfInterruptAcquireLock(Dev->Interrupt);
        Dev->TxBuffer[0] = Ctx->Type;
        RtlCopyMemory(&Dev->TxBuffer[1], Ctx->Data, Ctx->DataLen);
        Dev->TxLength = 1 + Ctx->DataLen;
        Dev->TxSent = 0;
        Dev->TxDone = FALSE;
        Dev->TxBusy = TRUE;
        Dev->Ier |= UART_IER_TX;                             /* fires at once: the FIFO is empty */
        SwBtWrite(Dev, UART_IER, Dev->Ier);
        WdfInterruptReleaseLock(Dev->Interrupt);
    }
}

/* The ISR sent the whole packet. Lock held. */
static VOID SwBtCompleteTx(PSWBT_DEVICE Dev)
{
    WDFREQUEST Request = Dev->TxRequest;
    WDFMEMORY Memory;
    size_t Size = 0;
    ULONG_PTR Info = 0;
    Dev->TxRequest = NULL;
    if (NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &Memory))) {
        PULONG Out = (PULONG)WdfMemoryGetBuffer(Memory, &Size);
        if (Size >= sizeof(ULONG)) {
            *Out = Dev->TxRequestLength;                     /* bytes written, as the sample */
            Info = sizeof(ULONG);
        }
    }
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, Info);
}

VOID SwBtEvtDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(WdfInterruptGetDevice(Interrupt));
    UCHAR Chunk[256];
    BOOLEAN TxDone;
    UNREFERENCED_PARAMETER(AssociatedObject);

    WdfSpinLockAcquire(Dev->Lock);
    for (;;) {
        ULONG Count = 0, Index;
        WdfInterruptAcquireLock(Interrupt);
        while (Count < sizeof(Chunk) && Dev->RxTail != Dev->RxHead) {
            Chunk[Count++] = Dev->RxRing[Dev->RxTail & (RX_RING_SIZE - 1)];
            Dev->RxTail++;
        }
        TxDone = Dev->TxDone;
        Dev->TxDone = FALSE;
        WdfInterruptReleaseLock(Interrupt);
        for (Index = 0; Index < Count; ++Index) {
            SwBtParse(Dev, Chunk[Index]);
        }
        if (TxDone && Dev->TxRequest != NULL) {
            SwBtCompleteTx(Dev);
            SwBtStartTx(Dev);
        }
        if (Count == 0 && !TxDone) {
            break;
        }
    }
    WdfSpinLockRelease(Dev->Lock);
}

/* ------------------------------------------------------------------- I/O */

VOID SwBtEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(WdfIoQueueGetDevice(Queue));
    WDFMEMORY Memory;
    PVOID Buffer;
    size_t Size = 0;
    NTSTATUS Status;

    switch (Code) {
    case IOCTL_BTHX_GET_VERSION:
        if (OutLength < sizeof(BTHX_VERSION) || !NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &Memory))) {
            break;
        }
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(Buffer, &Microsoft_BTHX_DDI_Version, sizeof(BTHX_VERSION));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_VERSION));
        return;

    case IOCTL_BTHX_SET_VERSION:
        if (InLength < sizeof(BTHX_VERSION)) {
            break;
        }
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_BTHX_QUERY_CAPABILITIES:
        if (OutLength < sizeof(BTHX_CAPABILITIES) || !NT_SUCCESS(WdfRequestRetrieveOutputMemory(Request, &Memory))) {
            break;
        }
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(Buffer, &Dev->Caps, sizeof(BTHX_CAPABILITIES));
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(BTHX_CAPABILITIES));
        return;

    case IOCTL_BTHX_WRITE_HCI:
        if (!Dev->ChipOk) {
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
            return;
        }
        WdfSpinLockAcquire(Dev->Lock);
        Status = WdfRequestForwardToIoQueue(Request, Dev->TxQueue);
        if (NT_SUCCESS(Status)) {
            SwBtStartTx(Dev);
        }
        WdfSpinLockRelease(Dev->Lock);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
        }
        return;

    case IOCTL_BTHX_READ_HCI: {
        ULONG Type = 0;
        if (InLength < sizeof(UCHAR) || OutLength < FIELD_OFFSET(BTHX_HCI_READ_WRITE_CONTEXT, Data) ||
            !NT_SUCCESS(WdfRequestRetrieveInputMemory(Request, &Memory))) {
            break;
        }
        Buffer = WdfMemoryGetBuffer(Memory, &Size);
        RtlCopyMemory(&Type, Buffer, min(Size, sizeof(Type)));
        if (Type != HciPacketEvent && Type != HciPacketAclData) {
            break;
        }
        WdfSpinLockAcquire(Dev->Lock);
        Status = WdfRequestForwardToIoQueue(Request, Type == HciPacketEvent ? Dev->EventQueue : Dev->AclQueue);
        if (NT_SUCCESS(Status)) {
            if (Type == HciPacketEvent) {
                SwBtDrainKept(Dev, &Dev->KeptEvents, Dev->EventQueue);
            } else {
                SwBtDrainKept(Dev, &Dev->KeptAcl, Dev->AclQueue);
            }
        }
        WdfSpinLockRelease(Dev->Lock);
        if (!NT_SUCCESS(Status)) {
            WdfRequestComplete(Request, Status);
        }
        return;
    }

    default:
        break;
    }
    WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
}

/* The child (BthMini) hands every BTHX request to this device. */
VOID SwBtEvtChildIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutLength, size_t InLength, ULONG Code)
{
    WDF_REQUEST_FORWARD_OPTIONS Options;
    WDFDEVICE Parent = WdfPdoGetParent(WdfIoQueueGetDevice(Queue));
    NTSTATUS Status = STATUS_INVALID_PARAMETER;
    UNREFERENCED_PARAMETER(OutLength);
    UNREFERENCED_PARAMETER(InLength);

    switch (Code) {
    case IOCTL_BTHX_GET_VERSION:
    case IOCTL_BTHX_SET_VERSION:
    case IOCTL_BTHX_QUERY_CAPABILITIES:
    case IOCTL_BTHX_WRITE_HCI:
    case IOCTL_BTHX_READ_HCI:
        WDF_REQUEST_FORWARD_OPTIONS_INIT(&Options);
        Options.Flags = WDF_REQUEST_FORWARD_OPTION_SEND_AND_FORGET;
        Status = WdfRequestForwardToParentDeviceIoQueue(Request, WdfDeviceGetDefaultQueue(Parent), &Options);
        break;
    default:
        break;
    }
    if (!NT_SUCCESS(Status)) {
        WdfRequestComplete(Request, Status);
    }
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

    WdfSpinLockAcquire(Dev->Lock);
    SwBtFreeKept(Dev);
    WdfSpinLockRelease(Dev->Lock);
    if (Dev->Uart != NULL) {
        MmUnmapIoSpace((PVOID)Dev->Uart, UART_LENGTH);
        Dev->Uart = NULL;
    }
    return STATUS_SUCCESS;
}

/* Interrupts are still off here: the probe polls. */
NTSTATUS SwBtEvtD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PSWBT_DEVICE Dev = SwBtGetDevice(Device);
    UNREFERENCED_PARAMETER(PreviousState);

    Dev->ChipOk = SwBtProbe(Dev);
    Dev->RxGot = 0;
    WdfSpinLockAcquire(Dev->Lock);
    SwBtFreeKept(Dev);
    WdfSpinLockRelease(Dev->Lock);
    SwBtTrace(L"ChipOk", Dev->ChipOk);
    return STATUS_SUCCESS;
}

static NTSTATUS SwBtCreateChild(WDFDEVICE Device)
{
    DECLARE_CONST_UNICODE_STRING(HardwareId, L"SWOA\\BTH_UART_H4");
    DECLARE_CONST_UNICODE_STRING(CompatibleId, L"MS_BTHX_BTHMINI");
    DECLARE_CONST_UNICODE_STRING(InstanceId, L"0");
    DECLARE_CONST_UNICODE_STRING(Description, L"Nintendo Switch Bluetooth radio");
    DECLARE_CONST_UNICODE_STRING(Location, L"Nintendo Switch UART-D");
    PWDFDEVICE_INIT Init;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDF_DEVICE_PNP_CAPABILITIES Caps;
    WDFDEVICE Child;
    UNICODE_STRING Container;
    NTSTATUS Status;

    Init = WdfPdoInitAllocate(Device);
    if (Init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    WdfDeviceInitSetDeviceType(Init, FILE_DEVICE_BUS_EXTENDER);
    Status = WdfPdoInitAssignDeviceID(Init, &HardwareId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddHardwareID(Init, &HardwareId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddCompatibleID(Init, &CompatibleId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAssignInstanceID(Init, &InstanceId);
    if (NT_SUCCESS(Status)) Status = WdfPdoInitAddDeviceText(Init, &Description, &Location, 0x409);
    if (NT_SUCCESS(Status) && NT_SUCCESS(RtlStringFromGUID(&SWBT_CONTAINER_INTERNAL, &Container))) {
        Status = WdfPdoInitAssignContainerID(Init, &Container);
        RtlFreeUnicodeString(&Container);
    }
    if (!NT_SUCCESS(Status)) {
        WdfDeviceInitFree(Init);
        return Status;
    }
    WdfPdoInitSetDefaultLocale(Init, 0x409);
    WdfPdoInitAllowForwardingRequestToParent(Init);

    Status = WdfDeviceCreate(&Init, WDF_NO_OBJECT_ATTRIBUTES, &Child);
    if (!NT_SUCCESS(Status)) {
        if (Init != NULL) {
            WdfDeviceInitFree(Init);
        }
        return Status;
    }
    WDF_DEVICE_PNP_CAPABILITIES_INIT(&Caps);
    Caps.Removable = WdfFalse;
    Caps.SurpriseRemovalOK = WdfTrue;
    Caps.UniqueID = WdfTrue;
    WdfDeviceSetPnpCapabilities(Child, &Caps);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = SwBtEvtChildIoDeviceControl;
    QueueConfig.EvtIoDeviceControl = SwBtEvtChildIoDeviceControl;
    Status = WdfIoQueueCreate(Child, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    return WdfFdoAddStaticChild(Device, Child);
}

static NTSTATUS SwBtCreateManualQueue(WDFDEVICE Device, WDFQUEUE *Queue)
{
    WDF_IO_QUEUE_CONFIG Config;
    WDF_IO_QUEUE_CONFIG_INIT(&Config, WdfIoQueueDispatchManual);
    Config.PowerManaged = WdfFalse;
    return WdfIoQueueCreate(Device, &Config, WDF_NO_OBJECT_ATTRIBUTES, Queue);
}

NTSTATUS SwBtEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDF_INTERRUPT_CONFIG InterruptConfig;
    WDF_IO_QUEUE_CONFIG QueueConfig;
    WDFDEVICE Device;
    PSWBT_DEVICE Dev;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_BUS_EXTENDER);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwBtEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwBtEvtReleaseHardware;
    Pnp.EvtDeviceD0Entry = SwBtEvtD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWBT_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Dev = SwBtGetDevice(Device);
    InitializeListHead(&Dev->KeptEvents);
    InitializeListHead(&Dev->KeptAcl);
    Dev->Caps.MaxAclTransferInSize = HCI_MAX_PACKET;
    Dev->Caps.ScoSupport = ScoSupportNone;
    Dev->Caps.MaxScoChannels = 0;
    Dev->Caps.IsDeviceIdleCapable = FALSE;
    Dev->Caps.IsDeviceWakeCapable = FALSE;

    Status = WdfSpinLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Dev->Lock);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_INTERRUPT_CONFIG_INIT(&InterruptConfig, SwBtEvtIsr, SwBtEvtDpc);
    InterruptConfig.EvtInterruptEnable = SwBtEvtInterruptEnable;
    InterruptConfig.EvtInterruptDisable = SwBtEvtInterruptDisable;
    Status = WdfInterruptCreate(Device, &InterruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &Dev->Interrupt);
    SwBtTrace(L"InterruptStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchParallel);
    QueueConfig.EvtIoInternalDeviceControl = SwBtEvtIoDeviceControl;
    QueueConfig.EvtIoDeviceControl = SwBtEvtIoDeviceControl;
    QueueConfig.PowerManaged = WdfFalse;
    Status = WdfIoQueueCreate(Device, &QueueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (NT_SUCCESS(Status)) Status = SwBtCreateManualQueue(Device, &Dev->TxQueue);
    if (NT_SUCCESS(Status)) Status = SwBtCreateManualQueue(Device, &Dev->EventQueue);
    if (NT_SUCCESS(Status)) Status = SwBtCreateManualQueue(Device, &Dev->AclQueue);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    Status = SwBtCreateChild(Device);
    SwBtTrace(L"ChildStatus", (ULONG)Status);
    return Status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwBtParamsPath)) {
        RtlCopyMemory(SwBtParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwBtParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwBtParamsPath);
    }
    SwBtTrace(L"DriverVersion", 2);
    WDF_DRIVER_CONFIG_INIT(&Config, SwBtEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
