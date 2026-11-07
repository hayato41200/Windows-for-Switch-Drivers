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

#define SWWIFI_VERSION      0x00020000u     /* 0.2.0 */

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

#define CORE_80211          0x812

static ULONG mCr4Base, mCr4Rev, mPcie2Rev, mCr4Wrap, mD11Wrap, mRamBytes;

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
    mRamBytes = Total;
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
    mCr4Base = mCr4Rev = mPcie2Rev = mCr4Wrap = mD11Wrap = mRamBytes = 0;
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
        if (CurId == CORE_ARM_CR4 && mCr4Base == 0) { mCr4Base = CurBase; mCr4Rev = CurRev; mCr4Wrap = CurWrap; } \
        if (CurId == CORE_80211 && mD11Wrap == 0) { mD11Wrap = CurWrap; } \
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

static VOID SwWifiFirmware(PSWWIFI_DEVICE Dev);

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
    if ((Chip & 0xFFFFu) != 0x4356u) {
        SwWifiTrace(L"Busy", 0);
        SwWifiTraceText(L"Result", L"chip ID is not 4356");
        SwWifiFlush();
        return;
    }
    SwWifiTraceText(L"Result", L"ok: BCM4356 answers in Windows");
    SwWifiFlush();

    /* 0.2.0: stage 2b, firmware download (only when the files are there). */
    SwWifiFirmware(Dev);
    SwWifiTrace(L"Busy", 0);
    SwWifiFlush();
}

/* ------------------------------------------------- firmware (stage 2b)
 *
 * As brcmfmac (Linux 4.9) brcmf_chip_attach + brcmf_pcie_download_fw_nvram:
 * halt the ARM CR4 and reset the D11 core (set_passive), copy the firmware to
 * device RAM at rambase, clear the last RAM word, put the NVRAM at the end of
 * RAM, write the reset vector at TCM 0 and release the CPU (set_active). The
 * firmware then writes the address of its shared area to the last RAM word.
 * The files are the user's own (not distributed): brcmfmac4356-pcie.bin and,
 * if there is one, brcmfmac4356-pcie.txt, in System32\drivers\switchwifi.
 * Not done (yet): the chip watchdog reset of brcmf_pcie_reset_device; UEFI
 * has just powered the chip, so it is still in its boot ROM.
 */
#define FW_DIR              L"\\SystemRoot\\System32\\drivers\\switchwifi\\"
#define FW_MAX              (1024u * 1024u)
#define NVRAM_MAX           (64u * 1024u)
#define POOL_TAG            'iWwS'
#define TCM_RAMBASE         0x180000u       /* brcmf_chip_tcm_rambase: BCM4356 */
#define AI_IOCTL            0x408
#define AI_RESET_CTL        0x800
#define AI_IOCTL_CLK        0x1u
#define AI_IOCTL_FGC        0x2u
#define CR4_IOCTL_CPUHALT   0x20u
#define D11_IOCTL_PHYCLOCKEN 0x4u
#define D11_IOCTL_PHYRESET  0x8u
#define PCIE2_CONFIGADDR    0x120
#define PCIE2_CONFIGDATA    0x124
#define FW_UP_MS            5000
#define SHARED_CONSOLE      20
#define CONSOLE_BUFADDR     8
#define CONSOLE_BUFSIZE     12
#define CONSOLE_WRITEIDX    16
#define CONSOLE_MAX         3000

static NTSTATUS SwWifiReadFile(PCWSTR Name, PUCHAR *Data, ULONG *Size, ULONG Max)
{
    WCHAR Path[128];
    UNICODE_STRING U;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Io;
    FILE_STANDARD_INFORMATION Info;
    HANDLE File;
    PUCHAR Buf;
    NTSTATUS Status;

    *Data = NULL;
    *Size = 0;
    Status = RtlStringCbPrintfW(Path, sizeof(Path), L"%s%s", FW_DIR, Name);
    if (!NT_SUCCESS(Status)) return Status;
    RtlInitUnicodeString(&U, Path);
    InitializeObjectAttributes(&Oa, &U, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Oa, &Io, NULL, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                          FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status)) return Status;
    Status = ZwQueryInformationFile(File, &Io, &Info, sizeof(Info), FileStandardInformation);
    if (NT_SUCCESS(Status) && (Info.EndOfFile.QuadPart <= 0 || Info.EndOfFile.QuadPart > Max)) {
        Status = STATUS_FILE_TOO_LARGE;
    }
    if (NT_SUCCESS(Status)) {
        ULONG Len = (ULONG)Info.EndOfFile.QuadPart;
        Buf = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, Len + 8, POOL_TAG);
        if (Buf == NULL) {
            Status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            RtlZeroMemory(Buf, Len + 8);
            Status = ZwReadFile(File, NULL, NULL, NULL, &Io, Buf, Len, NULL, NULL);
            if (NT_SUCCESS(Status) && Io.Information != Len) Status = STATUS_END_OF_FILE;
            if (NT_SUCCESS(Status)) { *Data = Buf; *Size = Len; } else { ExFreePoolWithTag(Buf, POOL_TAG); }
        }
    }
    ZwClose(File);
    return Status;
}

static BOOLEAN NvKeyChar(UCHAR C)
{
    return (C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_';
}

/* Text NVRAM to what the firmware reads (brcmf_fw_nvram_strip): one
 * "key=value\0" per entry, an extra NUL, padded to 4 bytes, then the length
 * token. Comments and blank lines go; "pcie/x/y/" and "N:" prefixes are cut. */
static ULONG SwWifiNvram(const UCHAR *In, ULONG InLen, UCHAR *Out, ULONG OutMax)
{
    static const char BoardRev[] = "boardrev=0xff";
    ULONG I = 0, O = 0, Len, Token;
    BOOLEAN HaveBoardRev = FALSE;

    while (I < InLen) {
        ULONG S = I, E, K;
        while (I < InLen && In[I] != '\n') I++;
        E = I;
        if (I < InLen) I++;
        while (S < E && (In[S] == ' ' || In[S] == '\t' || In[S] == '\r')) S++;
        while (E > S && (In[E - 1] == ' ' || In[E - 1] == '\t' || In[E - 1] == '\r')) E--;
        if (S == E || In[S] == '#') continue;
        if (E - S > 5 && RtlCompareMemory(In + S, "pcie/", 5) == 5) {
            ULONG Slashes = 0;
            while (S < E && Slashes < 3) { if (In[S] == '/') Slashes++; S++; }
        } else if (E - S > 2 && In[S] >= '0' && In[S] <= '9' && In[S + 1] == ':') {
            S += 2;
        }
        for (K = S; K < E && NvKeyChar(In[K]); K++) { }
        if (K == S || K >= E || In[K] != '=') continue;
        if (K - S == 7 && RtlCompareMemory(In + S, "devpath", 7) == 7) continue;
        if (K - S == 8 && RtlCompareMemory(In + S, "boardrev", 8) == 8) HaveBoardRev = TRUE;
        if (O + (E - S) + 1 + sizeof(BoardRev) + 8 > OutMax) break;
        RtlCopyMemory(Out + O, In + S, E - S);
        O += E - S;
        Out[O++] = 0;
    }
    if (!HaveBoardRev) {
        RtlCopyMemory(Out + O, BoardRev, sizeof(BoardRev));     /* includes the NUL */
        O += sizeof(BoardRev);
    }
    Len = (O + 1 + 3) & ~3u;
    while (O < Len) Out[O++] = 0;
    Token = Len / 4;
    Token = (~Token << 16) | (Token & 0xFFFFu);
    RtlCopyMemory(Out + O, &Token, 4);
    return O + 4;
}

static VOID SwWifiCoreDisable(PSWWIFI_DEVICE Dev, ULONG Wrap, ULONG PreReset, ULONG Reset)
{
    ULONG I;
    SwWifiSetWindow(Dev, Wrap);
    if ((Rd(Dev->Bar0, AI_RESET_CTL) & 1u) == 0) {
        Wr(Dev->Bar0, AI_IOCTL, PreReset | AI_IOCTL_FGC | AI_IOCTL_CLK);
        (VOID)Rd(Dev->Bar0, AI_IOCTL);
        Wr(Dev->Bar0, AI_RESET_CTL, 1u);
        KeStallExecutionProcessor(10);
        for (I = 0; I < 300 && (Rd(Dev->Bar0, AI_RESET_CTL) & 1u) == 0; I++) KeStallExecutionProcessor(1);
    }
    Wr(Dev->Bar0, AI_IOCTL, Reset | AI_IOCTL_FGC | AI_IOCTL_CLK);
    (VOID)Rd(Dev->Bar0, AI_IOCTL);
}

/* brcmf_chip_ai_resetcore */
static VOID SwWifiCoreReset(PSWWIFI_DEVICE Dev, ULONG Wrap, ULONG PreReset, ULONG Reset, ULONG PostReset)
{
    ULONG I;
    SwWifiCoreDisable(Dev, Wrap, PreReset, Reset);
    for (I = 0; I < 50 && (Rd(Dev->Bar0, AI_RESET_CTL) & 1u) != 0; I++) {
        Wr(Dev->Bar0, AI_RESET_CTL, 0);
        KeStallExecutionProcessor(40);
    }
    Wr(Dev->Bar0, AI_IOCTL, PostReset | AI_IOCTL_CLK);
    (VOID)Rd(Dev->Bar0, AI_IOCTL);
}

static VOID SwWifiTcmWrite(PSWWIFI_DEVICE Dev, ULONG Address, const UCHAR *Data, ULONG Len)
{
    ULONG I, W;
    for (I = 0; I + 4 <= Len; I += 4) {
        RtlCopyMemory(&W, Data + I, 4);
        Wr(Dev->Bar2, Address + I, W);
    }
    if (I < Len) {
        W = 0;
        RtlCopyMemory(&W, Data + I, Len - I);
        Wr(Dev->Bar2, Address + I, W);
    }
}

/* The firmware console (struct rte_console): a ring buffer of text in RAM. */
static VOID SwWifiConsole(PSWWIFI_DEVICE Dev, ULONG Shared)
{
    static WCHAR Text[CONSOLE_MAX + 1];
    ULONG Console, Buf, Size, Widx, N, I, Start, T = 0;

    Console = Rd(Dev->Bar2, Shared + SHARED_CONSOLE);
    SwWifiTrace(L"FwConsoleAddr", Console);
    if (Console == 0 || Console + 32 > Dev->Bar2Len) return;
    Buf = Rd(Dev->Bar2, Console + CONSOLE_BUFADDR);
    Size = Rd(Dev->Bar2, Console + CONSOLE_BUFSIZE);
    Widx = Rd(Dev->Bar2, Console + CONSOLE_WRITEIDX);
    SwWifiTrace(L"FwConsoleBuf", Buf);
    SwWifiTrace(L"FwConsoleSize", Size);
    SwWifiTrace(L"FwConsoleIdx", Widx);
    if (Size == 0 || Size > 0x10000 || Widx >= Size || Buf + Size > Dev->Bar2Len) return;
    N = (Size < CONSOLE_MAX) ? Size : CONSOLE_MAX;
    Start = (Widx + Size - N) % Size;
    for (I = 0; I < N; I++) {
        ULONG Off = (Start + I) % Size;
        UCHAR C = (UCHAR)(Rd(Dev->Bar2, Buf + (Off & ~3u)) >> ((Off & 3u) * 8));
        if (C == '\n') Text[T++] = L'\n';
        else if (C >= 0x20 && C < 0x7F) Text[T++] = (WCHAR)C;
    }
    Text[T] = L'\0';
    SwWifiTraceText(L"FwConsole", Text);
}

static VOID SwWifiFirmware(PSWWIFI_DEVICE Dev)
{
    PUCHAR Fw = NULL, NvText = NULL, Nv = NULL;
    ULONG FwLen = 0, NvTextLen = 0, NvLen = 0, ResetVector, I, Bad = 0, Written, Shared, Ms;
    ULONG RamEnd;
    NTSTATUS Status;

    SwWifiTrace(L"FwStage", 0);
    if (Dev->Bar2 == NULL || mCr4Wrap == 0 || mD11Wrap == 0 || mRamBytes == 0 ||
        TCM_RAMBASE + mRamBytes > Dev->Bar2Len) {
        SwWifiTraceText(L"FwResult", L"not tried: BAR2, the CR4/D11 wrappers or the RAM size are missing");
        return;
    }
    RamEnd = TCM_RAMBASE + mRamBytes;
    Status = SwWifiReadFile(L"brcmfmac4356-pcie.bin", &Fw, &FwLen, FW_MAX);
    SwWifiTrace(L"FwFileStatus", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        SwWifiTraceText(L"FwResult", L"not tried: no firmware file (System32\\drivers\\switchwifi\\brcmfmac4356-pcie.bin)");
        return;
    }
    SwWifiTrace(L"FwSize", FwLen);
    Status = SwWifiReadFile(L"brcmfmac4356-pcie.txt", &NvText, &NvTextLen, NVRAM_MAX);
    SwWifiTrace(L"NvFileStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        Nv = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, NVRAM_MAX + 64, POOL_TAG);
        if (Nv != NULL) NvLen = SwWifiNvram(NvText, NvTextLen, Nv, NVRAM_MAX + 64);
    }
    SwWifiTrace(L"NvSize", NvLen);
    if (FwLen + NvLen + 4 > mRamBytes) {
        SwWifiTraceText(L"FwResult", L"not tried: firmware + NVRAM do not fit in device RAM");
        goto Out;
    }

    /* "BAR1 window may not be sized properly" (brcmf_pcie_attach). */
    SwWifiStep(10);
    SwWifiTrace(L"FwStage", 10);
    SwWifiSetWindow(Dev, 0x18003000u);
    Wr(Dev->Bar0, PCIE2_CONFIGADDR, 0x4E0);
    Wr(Dev->Bar0, PCIE2_CONFIGDATA, Rd(Dev->Bar0, PCIE2_CONFIGDATA));

    /* set_passive: halt the CR4 (keeping its halt bit), reset D11. */
    SwWifiSetWindow(Dev, mCr4Wrap);
    I = Rd(Dev->Bar0, AI_IOCTL) & CR4_IOCTL_CPUHALT;
    SwWifiCoreReset(Dev, mCr4Wrap, I, CR4_IOCTL_CPUHALT, CR4_IOCTL_CPUHALT);
    SwWifiCoreReset(Dev, mD11Wrap, D11_IOCTL_PHYRESET | D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN, D11_IOCTL_PHYCLOCKEN);
    SwWifiSetWindow(Dev, mCr4Wrap);
    SwWifiTrace(L"Cr4IoctlHalted", Rd(Dev->Bar0, AI_IOCTL));
    SwWifiTrace(L"Cr4ResetHalted", Rd(Dev->Bar0, AI_RESET_CTL));
    SwWifiSetWindow(Dev, SI_ENUM_BASE);

    /* Firmware to RAM, verified. */
    SwWifiStep(11);
    SwWifiTrace(L"FwStage", 11);
    SwWifiTcmWrite(Dev, TCM_RAMBASE, Fw, FwLen);
    for (I = 0; I + 4 <= FwLen; I += 4096) {
        ULONG W;
        RtlCopyMemory(&W, Fw + I, 4);
        if (Rd(Dev->Bar2, TCM_RAMBASE + I) != W) Bad++;
    }
    SwWifiTrace(L"FwVerifyBad", Bad);
    RtlCopyMemory(&ResetVector, Fw, 4);
    SwWifiTrace(L"FwResetVector", ResetVector);

    SwWifiStep(12);
    SwWifiTrace(L"FwStage", 12);
    Wr(Dev->Bar2, RamEnd - 4, 0);
    if (NvLen != 0) {
        SwWifiTcmWrite(Dev, RamEnd - NvLen, Nv, NvLen);
    }
    Written = Rd(Dev->Bar2, RamEnd - 4);
    SwWifiTrace(L"FwSharedBefore", Written);

    /* set_active: reset vector at TCM 0, then release the CPU. */
    SwWifiStep(13);
    SwWifiTrace(L"FwStage", 13);
    Wr(Dev->Bar2, 0, ResetVector);
    SwWifiCoreReset(Dev, mCr4Wrap, CR4_IOCTL_CPUHALT, 0, 0);
    SwWifiSetWindow(Dev, SI_ENUM_BASE);

    SwWifiStep(14);
    SwWifiTrace(L"FwStage", 14);
    Shared = Written;
    for (Ms = 0; Ms < FW_UP_MS && Shared == Written; Ms += 50) {
        LARGE_INTEGER Delay;
        Delay.QuadPart = -50 * 10000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &Delay);
        Shared = Rd(Dev->Bar2, RamEnd - 4);
    }
    SwWifiTrace(L"FwUpMs", Ms);
    SwWifiTrace(L"FwShared", Shared);
    if (Shared == Written) {
        SwWifiTraceText(L"FwResult", L"firmware did not start (the shared address never changed)");
        goto Out;
    }

    SwWifiStep(15);
    SwWifiTrace(L"FwStage", 15);
    if (Shared >= TCM_RAMBASE && Shared + 64 <= RamEnd) {
        SwWifiTrace(L"FwSharedFlags", Rd(Dev->Bar2, Shared));
        SwWifiConsole(Dev, Shared);
        SwWifiTraceText(L"FwResult", L"ok: the firmware is running");
    } else {
        SwWifiTraceText(L"FwResult", L"the shared address is outside device RAM");
    }

Out:
    if (Fw) ExFreePoolWithTag(Fw, POOL_TAG);
    if (NvText) ExFreePoolWithTag(NvText, POOL_TAG);
    if (Nv) ExFreePoolWithTag(Nv, POOL_TAG);
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
