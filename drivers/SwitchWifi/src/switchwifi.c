/*
 * Nintendo Switch Wi-Fi (BCM4356 on Tegra210 PCIe port 1), KMDF. Stage 2a.
 *
 * Windows' pci.sys cannot enumerate the Tegra210 PCIe controller (its config
 * space is not ECAM), so UEFI R405+ brings the link up, assigns the endpoint
 * BARs and describes the device to Windows as ACPI\SWOA0D01:
 *   [0] Memory32Fixed 0x13400000, 32 KB   endpoint BAR0 (4 KB backplane window)
 *   [1] Memory32Fixed 0x13000000, 4 MB    endpoint BAR2 (device RAM/TCM)
 *   [2] Memory32Fixed 0x11FFF000, 4 KB    AFI config window = bus 1 dev 0 fn 0
 *   [3] Memory32Fixed 0x01001000, 4 KB    root port 1 registers
 *   [4] Memory32Fixed 0x01003800, 2 KB    AFI
 *
 * This version only checks that the link survives into Windows and that the
 * chip answers there: it reads the root port link state, the endpoint config
 * header, the ChipCommon chip ID, and walks the backplane enumeration ROM
 * (EROM) to list the cores. Results go to
 * HKLM\SYSTEM\CurrentControlSet\Services\switchwifi\Parameters.
 *
 * A dead endpoint makes an access raise an SError, which stops Windows. So
 * "Busy" is set (and flushed) before the first endpoint access and cleared
 * after the last one. If the driver starts and finds Busy=1, the previous
 * attempt stopped the machine; it then leaves the endpoint alone (Skipped=1)
 * so Windows still boots. Wifi.ps1 -Mode Retry clears it.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>

#define SWWIFI_VERSION      0x00010001u     /* 0.1.1 */

#define RP_LINK_STATUS      0x090           /* bit 29: data link up */
#define RP_XP_VC0           0xF00           /* bit 30: DL up */
#define AFI_FPCI_BAR0       0x030
#define CS_FPCI             0xFE100100u     /* bus 1 page, as UEFI R404 */
#define EP_ID_BCM4356       0x43EC14E4u
#define BCM_BAR0_WINDOW     0x80            /* config register: backplane address of the BAR0 window */
#define SI_ENUM_BASE        0x18000000u     /* ChipCommon */
#define CC_CHIPID           0x00
#define CC_CAPS             0x04
#define CC_EROMPTR          0xFC
#define EROM_MAX_WORDS      256

typedef struct _SWWIFI_DEVICE {
    volatile ULONG *Bar0;  ULONG Bar0Len;
    volatile ULONG *Bar2;  ULONG Bar2Len;
    volatile ULONG *Cfg;   ULONG CfgLen;
    volatile ULONG *Rp;    ULONG RpLen;
    volatile ULONG *Afi;   ULONG AfiLen;
} SWWIFI_DEVICE, *PSWWIFI_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWWIFI_DEVICE, SwWifiGetDevice)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwWifiEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwWifiEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwWifiEvtReleaseHardware;

static WCHAR SwWifiParamsPath[256];

/* ------------------------------------------------------------- registry */

static VOID SwWifiFlush(VOID)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    HANDLE Key;

    RtlInitUnicodeString(&Name, SwWifiParamsPath);
    InitializeObjectAttributes(&Oa, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_WRITE, &Oa))) {
        (VOID)ZwFlushKey(Key);
        ZwClose(Key);
    }
}

static VOID SwWifiTrace(PCWSTR Name, ULONG Value)
{
    if (SwWifiParamsPath[0] != L'\0') {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwWifiParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static VOID SwWifiTraceText(PCWSTR Name, PCWSTR Text)
{
    size_t Bytes;
    if (SwWifiParamsPath[0] != L'\0' && NT_SUCCESS(RtlStringCbLengthW(Text, 8192, &Bytes))) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwWifiParamsPath, Name, REG_SZ, (PVOID)Text, (ULONG)(Bytes + sizeof(WCHAR)));
    }
}

static VOID SwWifiTraceBinary(PCWSTR Name, PVOID Data, ULONG Bytes)
{
    if (SwWifiParamsPath[0] != L'\0') {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwWifiParamsPath, Name, REG_BINARY, Data, Bytes);
    }
}

static ULONG SwWifiQuery(PCWSTR Name, ULONG Default)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = Default;

    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (SwWifiParamsPath[0] == L'\0' ||
        !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwWifiParamsPath, Table, NULL, NULL))) {
        return Default;
    }
    return Value;
}

/* Step marker: written and flushed before each risky access, so after a
 * stop the next boot shows where it happened. */
static VOID SwWifiStep(ULONG Step)
{
    SwWifiTrace(L"Step", Step);
    SwWifiFlush();
}

/* ------------------------------------------------------------- hardware */

static ULONG Rd(volatile ULONG *Base, ULONG Offset)
{
    return READ_REGISTER_ULONG((PULONG)((volatile UCHAR *)Base + Offset));
}

static VOID Wr(volatile ULONG *Base, ULONG Offset, ULONG Value)
{
    WRITE_REGISTER_ULONG((PULONG)((volatile UCHAR *)Base + Offset), Value);
}

static VOID SwWifiSetWindow(PSWWIFI_DEVICE Dev, ULONG Address)
{
    Wr(Dev->Cfg, BCM_BAR0_WINDOW, Address);
    (VOID)Rd(Dev->Cfg, BCM_BAR0_WINDOW);
}

#define CORE_ARM_CR4        0x83E
#define CORE_PCIE2          0x83C
#define CR4_CAP             0x04            /* bits 3:0 banks A (ATCM), 7:4 banks B (BTCM) */
#define CR4_BANKIDX         0x40
#define CR4_BANKINFO        0x44            /* bits 5:0 size in 8 KB units - 1 */

static ULONG mCr4Base, mCr4Rev, mPcie2Rev;

/* RAM size as brcmfmac brcmf_chip_cr4_set_ram_size: sum of the ATCM and BTCM
 * banks of the ARM CR4 core. The firmware is later copied into this RAM. */
static VOID SwWifiCr4Ram(PSWWIFI_DEVICE Dev)
{
    ULONG Cap, Banks, Bank, Total = 0;

    SwWifiSetWindow(Dev, mCr4Base);
    Cap = Rd(Dev->Bar0, CR4_CAP);
    Banks = (Cap & 0xFu) + ((Cap >> 4) & 0xFu);
    if (Banks > 16) Banks = 16;
    for (Bank = 0; Bank < Banks; Bank++) {
        Wr(Dev->Bar0, CR4_BANKIDX, Bank);
        Total += ((Rd(Dev->Bar0, CR4_BANKINFO) & 0x3Fu) + 1) * 8192u;
    }
    SwWifiSetWindow(Dev, SI_ENUM_BASE);
    SwWifiTrace(L"Cr4Base", mCr4Base);
    SwWifiTrace(L"Cr4Rev", mCr4Rev);
    SwWifiTrace(L"Cr4Cap", Cap);
    SwWifiTrace(L"RamBytes", Total);
    SwWifiTrace(L"Pcie2Rev", mPcie2Rev);
}

/* Walk the EROM (the AXI/DMP descriptor table, as brcmfmac
 * brcmf_chip_dmp_erom_scan) and list "core:rev@base" for each component. */
static VOID SwWifiScanErom(PSWWIFI_DEVICE Dev, ULONG EromPtr)
{
    static ULONG Words[EROM_MAX_WORDS];
    static WCHAR Text[2048];
    ULONG Count = 0, I = 0, Cores = 0, Page = 0xFFFFFFFFu;
    ULONG CurId = 0, CurRev = 0, CurBase = 0, CurWrap = 0;
    BOOLEAN Have = FALSE;
    size_t Left = sizeof(Text);
    PWSTR Out = Text;

    Text[0] = L'\0';
    while (Count < EROM_MAX_WORDS) {
        ULONG Addr = EromPtr + Count * 4;
        if ((Addr & ~0xFFFu) != Page) {
            Page = Addr & ~0xFFFu;
            SwWifiSetWindow(Dev, Page);
        }
        Words[Count] = Rd(Dev->Bar0, Addr & 0xFFFu);
        if (Words[Count++] == 0x0000000Fu) {
            break;                              /* end of table */
        }
    }
    SwWifiSetWindow(Dev, SI_ENUM_BASE);
    SwWifiTraceBinary(L"Erom", Words, Count * sizeof(ULONG));
    SwWifiTrace(L"EromWords", Count);

#define FLUSH_CORE() do { if (Have) { \
        (VOID)RtlStringCbPrintfExW(Out, Left, &Out, &Left, 0, L"%03x:%u@%08x/%08x ", CurId, CurRev, CurBase, CurWrap); \
        if (CurId == CORE_ARM_CR4 && mCr4Base == 0) { mCr4Base = CurBase; mCr4Rev = CurRev; } \
        if (CurId == CORE_PCIE2 && mPcie2Rev == 0) { mPcie2Rev = CurRev; } \
        Cores++; } } while (0)

    while (I < Count) {
        ULONG V = Words[I++], Type = V & 0xFu;
        if (V == 0x0000000Fu) {
            break;
        }
        if (Type == 0x1u) {                     /* component (CIA, then CIB) */
            ULONG B;
            if (I >= Count) break;
            B = Words[I++];
            if ((B & 0xFu) != 0x1u) continue;
            FLUSH_CORE();
            Have = TRUE;
            CurId = (V >> 8) & 0xFFFu;
            CurRev = (B >> 24) & 0xFFu;
            CurBase = 0;
            CurWrap = 0;
        } else if ((Type & ~0x8u) == 0x5u) {    /* address descriptor */
            ULONG Base = V & ~0xFFFu, SlaveType = (V >> 6) & 3u, SizeType = (V >> 4) & 3u;
            if (V & 0x8u) I++;                  /* upper 32 bits */
            if (SizeType == 3u && I < Count) {
                ULONG Sz = Words[I++];
                if (Sz & 0x8u) I++;
            }
            if (Have && SlaveType == 0u && CurBase == 0 && ((V >> 8) & 0xFu) == 0u) CurBase = Base;
            if (Have && SlaveType >= 2u && CurWrap == 0) CurWrap = Base;
        }
        /* master port descriptors (type 3) carry nothing needed here */
    }
    FLUSH_CORE();
#undef FLUSH_CORE
    SwWifiTrace(L"Cores", Cores);
    SwWifiTraceText(L"CoreList", Text);
}

static VOID SwWifiProbe(PSWWIFI_DEVICE Dev)
{
    ULONG Lnk, Xp, Fpci, Id, Cmd, Cls, Bar0, Bar2, Win, Chip, Caps, Erom;

    SwWifiTrace(L"Version", SWWIFI_VERSION);
    SwWifiTrace(L"Skipped", 0);
    if (SwWifiQuery(L"Busy", 0) != 0) {
        /* The last attempt stopped the machine at "Step": leave it alone. */
        SwWifiTrace(L"Skipped", 1);
        SwWifiTraceText(L"Result", L"skipped: the previous attempt stopped Windows (see Step); run Wifi.ps1 -Mode Retry");
        return;
    }
    if (Dev->Bar0 == NULL || Dev->Cfg == NULL || Dev->Rp == NULL || Dev->Afi == NULL) {
        SwWifiTraceText(L"Result", L"missing resources (UEFI R405 or later needed)");
        return;
    }

    /* Root port registers only: always safe. */
    Lnk = Rd(Dev->Rp, RP_LINK_STATUS);
    Xp = Rd(Dev->Rp, RP_XP_VC0);
    Fpci = Rd(Dev->Afi, AFI_FPCI_BAR0);
    SwWifiTrace(L"RpLink", Lnk);
    SwWifiTrace(L"RpXp", Xp);
    SwWifiTrace(L"AfiFpci", Fpci);
    if ((Lnk & (1u << 29)) == 0 || (Xp & (1u << 30)) == 0) {
        SwWifiTraceText(L"Result", L"link down (UEFI did not bring up PCIe, or it went down)");
        SwWifiFlush();
        return;
    }

    SwWifiTrace(L"Busy", 1);
    SwWifiStep(1);
    if (Fpci != CS_FPCI) {
        Wr(Dev->Afi, AFI_FPCI_BAR0, CS_FPCI);
        (VOID)Rd(Dev->Afi, AFI_FPCI_BAR0);
    }
    Id = Rd(Dev->Cfg, 0x00);
    SwWifiTrace(L"EpId", Id);
    SwWifiStep(2);
    Cmd = Rd(Dev->Cfg, 0x04);
    Cls = Rd(Dev->Cfg, 0x08);
    Bar0 = Rd(Dev->Cfg, 0x10);
    Bar2 = Rd(Dev->Cfg, 0x18);
    Win = Rd(Dev->Cfg, BCM_BAR0_WINDOW);
    SwWifiTrace(L"EpCmd", Cmd);
    SwWifiTrace(L"EpClass", Cls);
    SwWifiTrace(L"EpBar0", Bar0);
    SwWifiTrace(L"EpBar2", Bar2);
    SwWifiTrace(L"EpWindow", Win);
    if (Id != EP_ID_BCM4356 || (Cmd & 2u) == 0 || (Bar0 & ~0xFu) != 0x13400000u) {
        SwWifiTrace(L"Busy", 0);
        SwWifiTraceText(L"Result", L"endpoint not set up as UEFI R405 leaves it (ID, memory enable or BAR0)");
        SwWifiFlush();
        return;
    }

    SwWifiStep(3);
    SwWifiSetWindow(Dev, SI_ENUM_BASE);
    Chip = Rd(Dev->Bar0, CC_CHIPID);
    Caps = Rd(Dev->Bar0, CC_CAPS);
    Erom = Rd(Dev->Bar0, CC_EROMPTR);
    SwWifiTrace(L"ChipId", Chip);
    SwWifiTrace(L"ChipCaps", Caps);
    SwWifiTrace(L"EromPtr", Erom);

    SwWifiStep(4);
    /* 0.1.1: 0.1.0 accepted only 0x180xxxxx; the BCM4356 EROM is at 0x1810D000. */
    if ((Erom & 0xFF000000u) == SI_ENUM_BASE) {
        SwWifiScanErom(Dev, Erom);
    }

    SwWifiStep(6);
    if (mCr4Base != 0) {
        SwWifiCr4Ram(Dev);
    }

    SwWifiStep(7);
    SwWifiTrace(L"Busy", 0);
    SwWifiTraceText(L"Result", ((Chip & 0xFFFFu) == 0x4356u) ? L"ok: BCM4356 answers in Windows" : L"chip ID is not 4356");
    SwWifiFlush();
}

/* ------------------------------------------------------------- PnP */

NTSTATUS SwWifiEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWWIFI_DEVICE Dev = SwWifiGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index, Mem = 0;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; Index++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        volatile ULONG *Va;
        if (Desc == NULL || Desc->Type != CmResourceTypeMemory) {
            continue;
        }
        Va = (volatile ULONG *)MmMapIoSpaceEx(Desc->u.Memory.Start, Desc->u.Memory.Length, PAGE_READWRITE | PAGE_NOCACHE);
        if (Va == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        switch (Mem++) {
        case 0: Dev->Bar0 = Va; Dev->Bar0Len = Desc->u.Memory.Length; break;
        case 1: Dev->Bar2 = Va; Dev->Bar2Len = Desc->u.Memory.Length; break;
        case 2: Dev->Cfg = Va;  Dev->CfgLen = Desc->u.Memory.Length;  break;
        case 3: Dev->Rp = Va;   Dev->RpLen = Desc->u.Memory.Length;   break;
        case 4: Dev->Afi = Va;  Dev->AfiLen = Desc->u.Memory.Length;  break;
        default: MmUnmapIoSpace((PVOID)Va, Desc->u.Memory.Length); break;
        }
    }
    SwWifiTrace(L"MemResources", Mem);
    SwWifiProbe(Dev);
    return STATUS_SUCCESS;
}

NTSTATUS SwWifiEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWWIFI_DEVICE Dev = SwWifiGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);

    if (Dev->Bar0) { MmUnmapIoSpace((PVOID)Dev->Bar0, Dev->Bar0Len); Dev->Bar0 = NULL; }
    if (Dev->Bar2) { MmUnmapIoSpace((PVOID)Dev->Bar2, Dev->Bar2Len); Dev->Bar2 = NULL; }
    if (Dev->Cfg)  { MmUnmapIoSpace((PVOID)Dev->Cfg, Dev->CfgLen);   Dev->Cfg = NULL; }
    if (Dev->Rp)   { MmUnmapIoSpace((PVOID)Dev->Rp, Dev->RpLen);     Dev->Rp = NULL; }
    if (Dev->Afi)  { MmUnmapIoSpace((PVOID)Dev->Afi, Dev->AfiLen);   Dev->Afi = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS SwWifiEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwWifiEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwWifiEvtReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWWIFI_DEVICE);
    return WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;

    if (RegistryPath->Length / sizeof(WCHAR) + 12 < RTL_NUMBER_OF(SwWifiParamsPath)) {
        RtlCopyMemory(SwWifiParamsPath, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory(&SwWifiParamsPath[RegistryPath->Length / sizeof(WCHAR)], L"\\Parameters", sizeof(L"\\Parameters"));
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwWifiParamsPath);
    }
    WDF_DRIVER_CONFIG_INIT(&Config, SwWifiEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
