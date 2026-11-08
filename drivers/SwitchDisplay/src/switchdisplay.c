/*
 * switchdisplay.sys - WDDM display-only (KMDOD) driver for the Nintendo Switch
 * (Tegra210) built-in 720x1280 DSI panel.
 *
 * Device: the DSDT DISP (ACPI\SWOA0E01, UEFI R408) with DC-A and DSI-A as
 * memory resources. The framebuffer must not be a resource (UEFI R407 listed it
 * and Windows did not boot); a root device got an empty POST (1.0.0/1.0.1).
 *
 * What it does:
 *   - takes the boot framebuffer that coreboot set up on DC-A window A
 *     (0xDFB80000, X8R8G8B8, pitch 2880) from the Basic Display Driver (POST)
 *   - one source mode 720x1280 for every rotation; target and monitor modes with
 *     frequencies "not specified", as in the KMDOD sample (real frequencies gave
 *     STATUS_GRAPHICS_INVALID_FREQUENCY, and a 60 Hz monitor mode left GDI with
 *     no modes, so ChangeDisplaySettingsEx failed with BADMODE)
 *   - write-combined framebuffer, only dirty rects copied, rotation 0/90/180/270
 *     in the copy (Flags.Rotate: the DWM surface is 1280x720)
 *   - screen off (monitor or adapter out of D0): DCS display off + sleep in, then
 *     the DC stops scanning; screen on: DC restarts, sleep out, display on
 *     (registry PanelPower=0 / DcStop=0 turn these off)
 *   - vsync: the DC-A vblank interrupt (DSDT DISP, UEFI R409) reported as the
 *     display-only vsync when Windows asks (ControlInterrupt; registry Vsync=0
 *     turns it off); without the interrupt Windows paces by its timer
 *   - boot-loop guard: two starts in a row that did not survive 60 s make the
 *     next start fail, so Windows keeps the Basic Display Driver
 *   - an EDID builder, off unless UseEdid=1 (with it Windows may raise the scale)
 *
 * Trace: HKLM\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters
 */#define DXGKDDI_INTERFACE_VERSION 0x4002 /* DXGKDDI_INTERFACE_VERSION_WDDM1_3 */
#include <ntddk.h>
#include <dispmprt.h>

#define SWD_VERSION        0x00010202u   /* 1.2.2 */
#define SWD_TAG            'DwsS'

/* Boot framebuffer (coreboot display.c / UEFI MemoryMap.h "Display Reserved") */
#define SWD_FB_PHYS        0xDFB80000ull
#define SWD_NATIVE_W       720u
#define SWD_NATIVE_H       1280u
#define SWD_NATIVE_PITCH   (SWD_NATIVE_W * 4u)
#define SWD_FB_RESERVED    0x480000u

/* Panel timing (L4T DT display-timings 720x1280-24) */
#define SWD_PCLK           78000000u
#define SWD_HTOTAL         1000u        /* 720 + 136 + 72 + 72 */
#define SWD_VTOTAL         1300u        /* 1280 + 10 + 9 + 1 */
#define SWD_WIDTH_MM       77u
#define SWD_HEIGHT_MM      137u

/* DC-A (read only) */
#define DC_A_PHYS          0x54200000ull
#define DC_MAP_SIZE        0x3000u
#define DC_WINDOW_HEADER   (0x42u * 4u)
#define DC_WIN_OPTIONS     (0x700u * 4u)
#define DC_WIN_COLOR_DEPTH (0x703u * 4u)
#define DC_WIN_SIZE        (0x705u * 4u)
#define DC_WIN_LINE_STRIDE (0x70Au * 4u)
#define DC_WINBUF_START    (0x800u * 4u)
/* stage 2b: stop / restart scanning, as hekate's _di_dc_video_disable_config
 * and its 'Start continuous display' sequence (bdk/display/di.inl). */
#define DC_CMD_DISPLAY_COMMAND_REG (0x32u * 4u)
#define DC_CMD_STATE_ACCESS_REG    (0x40u * 4u)
#define DC_CMD_STATE_CONTROL_REG   (0x41u * 4u)
#define DISP_CTRL_MODE_STOP        0u
#define DISP_CTRL_MODE_C_DISPLAY   (1u << 5)
#define GENERAL_ACT_REQ            (1u << 0)
#define GENERAL_UPDATE             (1u << 8)
/* stage 3: vblank interrupt (ENABLE sets the status bit, MASK delivers it, as
 * the Linux tegra dc driver does). Status bits are cleared by writing 1. */
#define DC_CMD_INT_STATUS_REG      (0x37u * 4u)
#define DC_CMD_INT_MASK_REG        (0x38u * 4u)
#define DC_CMD_INT_ENABLE_REG      (0x39u * 4u)
#define DC_INT_V_BLANK             (1u << 2)

/* DSI-A (stage 2): DCS short writes sent inside the running video stream, as
 * hekate's display_end() does for SET_DISPLAY_OFF (bdk/display/di.c). */
#define DSI_A_PHYS         0x54300000ull
#define DSI_MAP_SIZE       0x1000u
#define DSI_WR_DATA        (0x0Au * 4u)
#define DSI_STATUS_REG     (0x15u * 4u)
#define DSI_VIDEO_MODE_CTL (0x4Eu * 4u)
#define DSI_CMD_PKT_VID_ENABLE 1u
#define MIPI_DCS_SHORT_WRITE   0x05u
#define DCS_ENTER_SLEEP    0x10u
#define DCS_EXIT_SLEEP     0x11u
#define DCS_DISPLAY_OFF    0x28u
#define DCS_DISPLAY_ON     0x29u

#define GUARD_SECONDS      60
#define FLUSH_SECONDS      10

typedef struct _SWD_CONTEXT {
    PDEVICE_OBJECT       Pdo;
    DXGKRNL_INTERFACE    Dxgk;
    BOOLEAN              Started;

    /* framebuffer */
    PHYSICAL_ADDRESS     FbPhys;
    ULONG                FbPitch;
    ULONG                FbWidth;       /* native: 720 */
    ULONG                FbHeight;      /* native: 1280 */
    SIZE_T               FbMapSize;
    PUCHAR               FbVa;
    PUCHAR               DsiVa;         /* DSI-A registers (stage 2) */
    PUCHAR               DcVa;          /* DC-A registers (stage 2b) */
    BOOLEAN              PanelAsleep;
    BOOLEAN              DcStopped;
    BOOLEAN              HasInterrupt;  /* DSDT DISP has the DC-A interrupt (UEFI R409) */
    PUCHAR               IrqDc;         /* DC-A mapped from AddDevice to RemoveDevice, for the ISR */
    BOOLEAN              VsyncAllowed;  /* registry Vsync, default 1 */
    BOOLEAN              VsyncWant;     /* argument of SyncVsync */
    D3DDDIFORMAT         FbFormat;

    /* current mode */
    BOOLEAN              Active;        /* a source mode is committed */
    BOOLEAN              Visible;
    BOOLEAN              MonitorOff;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
    ULONG                SrcWidth;
    ULONG                SrcHeight;

    /* system display (bugcheck screen) */
    BOOLEAN              SystemDisplay;

    /* guard / trace thread */
    HANDLE               ThreadHandle;
    PKTHREAD             Thread;
    KEVENT               StopEvent;

    /* counters (flushed to the registry by the thread) */
    volatile LONG        Presents;
    volatile LONG        PresentErrors;
    volatile LONG        LastPresentStatus;
    volatile LONG        Commits;
    volatile LONG        PowerCalls;
    volatile LONG        LastPower;     /* (uid << 8) | Dx */
    volatile LONG        PanelOffs;
    volatile LONG        PanelOns;
    volatile LONG        DcStops;
    volatile LONG        VsyncInts;
    volatile LONG        IsrOther;
    volatile LONG        VsyncEnables;
    volatile LONG        VsyncDisables;
} SWD_CONTEXT, *PSWD_CONTEXT;

static WCHAR  gParamPathBuf[512];
static UNICODE_STRING gParamPath;
static UCHAR  gEdid[128];
static BOOLEAN gUseEdid;

/* Per-DDI call count, failure count and last status (Status.cmd shows them). */
enum { D_QueryChildRelations, D_QueryChildStatus, D_QueryDeviceDescriptor, D_QueryAdapterInfo, D_IsSupportedVidPn, D_RecommendFunctionalVidPn, D_EnumVidPnCofuncModality, D_SetVidPnSourceVisibility, D_CommitVidPn, D_UpdateActiveVidPnPresentPath, D_RecommendMonitorModes, D_QueryVidPnHWCapability, D_SetPowerState, D_SystemDisplayEnable, D_SetPointerPosition, D_Escape, D_COUNT };
static volatile LONG gCalls[D_COUNT], gFails[D_COUNT], gLastSt[D_COUNT];
static volatile LONG gNotSupported, gRejCode, gRejA, gRejB, gCofuncStep, gSupportSt;
static volatile LONG gCiCalls, gCiLast, gCiTypes, gCiLastSt;
static volatile LONG gIsrCalls, gIsrUnclaimed, gEarlyIntStatus, gEarlyIntMask, gEarlyIntEnable;
static const PCWSTR gNameN[D_COUNT] = { L"N_QueryChildRelations", L"N_QueryChildStatus", L"N_QueryDeviceDescriptor", L"N_QueryAdapterInfo", L"N_IsSupportedVidPn", L"N_RecommendFunctionalVidPn", L"N_EnumVidPnCofuncModality", L"N_SetVidPnSourceVisibility", L"N_CommitVidPn", L"N_UpdateActiveVidPnPresentPath", L"N_RecommendMonitorModes", L"N_QueryVidPnHWCapability", L"N_SetPowerState", L"N_SystemDisplayEnable", L"N_SetPointerPosition", L"N_Escape" };
static const PCWSTR gNameS[D_COUNT] = { L"S_QueryChildRelations", L"S_QueryChildStatus", L"S_QueryDeviceDescriptor", L"S_QueryAdapterInfo", L"S_IsSupportedVidPn", L"S_RecommendFunctionalVidPn", L"S_EnumVidPnCofuncModality", L"S_SetVidPnSourceVisibility", L"S_CommitVidPn", L"S_UpdateActiveVidPnPresentPath", L"S_RecommendMonitorModes", L"S_QueryVidPnHWCapability", L"S_SetPowerState", L"S_SystemDisplayEnable", L"S_SetPointerPosition", L"S_Escape" };
static const PCWSTR gNameF[D_COUNT] = { L"F_QueryChildRelations", L"F_QueryChildStatus", L"F_QueryDeviceDescriptor", L"F_QueryAdapterInfo", L"F_IsSupportedVidPn", L"F_RecommendFunctionalVidPn", L"F_EnumVidPnCofuncModality", L"F_SetVidPnSourceVisibility", L"F_CommitVidPn", L"F_UpdateActiveVidPnPresentPath", L"F_RecommendMonitorModes", L"F_QueryVidPnHWCapability", L"F_SetPowerState", L"F_SystemDisplayEnable", L"F_SetPointerPosition", L"F_Escape" };
static NTSTATUS Note(int i, NTSTATUS st)
{
    InterlockedIncrement(&gCalls[i]);
    if (!NT_SUCCESS(st)) InterlockedIncrement(&gFails[i]);
    InterlockedExchange(&gLastSt[i], (LONG)st);
    return st;
}
static VOID Reject(LONG code, LONG a, LONG b)
{
    InterlockedExchange(&gRejCode, code); InterlockedExchange(&gRejA, a); InterlockedExchange(&gRejB, b);
}

/* ------------------------------------------------------------ registry */

static VOID RegPutDword(PCWSTR Name, ULONG Value)
{
    if (gParamPath.Length == 0) return;
    (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gParamPath.Buffer, Name, REG_DWORD, &Value, sizeof(Value));
}

static ULONG RegGetDword(PCWSTR Name, ULONG Default)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    UNICODE_STRING name;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
    PKEY_VALUE_PARTIAL_INFORMATION info = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG len = 0, value = Default;

    if (gParamPath.Length == 0) return Default;
    InitializeObjectAttributes(&oa, &gParamPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) return Default;
    RtlInitUnicodeString(&name, Name);
    if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, info, sizeof(buf), &len)) &&
        info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
        value = *(ULONG UNALIGNED *)info->Data;
    }
    ZwClose(key);
    return value;
}

static VOID FlushCounters(PSWD_CONTEXT Ctx)
{
    RegPutDword(L"Presents", (ULONG)Ctx->Presents);
    RegPutDword(L"PresentErrors", (ULONG)Ctx->PresentErrors);
    RegPutDword(L"LastPresentStatus", (ULONG)Ctx->LastPresentStatus);
    RegPutDword(L"Commits", (ULONG)Ctx->Commits);
    RegPutDword(L"PowerCalls", (ULONG)Ctx->PowerCalls);
    RegPutDword(L"LastPower", (ULONG)Ctx->LastPower);
    RegPutDword(L"PanelOffs", (ULONG)Ctx->PanelOffs);
    RegPutDword(L"PanelOns", (ULONG)Ctx->PanelOns);
    RegPutDword(L"DcStops", (ULONG)Ctx->DcStops);
    RegPutDword(L"VsyncInts", (ULONG)Ctx->VsyncInts);
    RegPutDword(L"IsrOther", (ULONG)Ctx->IsrOther);
    RegPutDword(L"VsyncEnables", (ULONG)Ctx->VsyncEnables);
    RegPutDword(L"VsyncDisables", (ULONG)Ctx->VsyncDisables);
    {
        int i;
        for (i = 0; i < D_COUNT; i++) {
            RegPutDword(gNameN[i], (ULONG)gCalls[i]);
            RegPutDword(gNameF[i], (ULONG)gFails[i]);
            RegPutDword(gNameS[i], (ULONG)gLastSt[i]);
        }
        RegPutDword(L"VidPnNotSupported", (ULONG)gNotSupported);
        RegPutDword(L"RejCode", (ULONG)gRejCode);
        RegPutDword(L"RejA", (ULONG)gRejA);
        RegPutDword(L"RejB", (ULONG)gRejB);
        RegPutDword(L"CofuncStep", (ULONG)gCofuncStep);
        RegPutDword(L"SupportSt", (ULONG)gSupportSt);
        RegPutDword(L"IsrCalls", (ULONG)gIsrCalls);
        RegPutDword(L"CiCalls", (ULONG)gCiCalls);
        RegPutDword(L"CiLast", (ULONG)gCiLast);
        RegPutDword(L"CiTypes", (ULONG)gCiTypes);
        RegPutDword(L"CiLastSt", (ULONG)gCiLastSt);
        RegPutDword(L"IsrUnclaimed", (ULONG)gIsrUnclaimed);
        RegPutDword(L"EarlyIntStatus", (ULONG)gEarlyIntStatus);
        RegPutDword(L"EarlyIntMask", (ULONG)gEarlyIntMask);
        RegPutDword(L"EarlyIntEnable", (ULONG)gEarlyIntEnable);    }
}

/* Clears the boot-loop guard after GUARD_SECONDS and flushes the counters
 * every FLUSH_SECONDS until the device stops. */
static VOID GuardThread(PVOID Context)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)Context;
    LARGE_INTEGER timeout;
    LONG elapsed = 0;
    BOOLEAN cleared = FALSE;

    timeout.QuadPart = -(LONGLONG)FLUSH_SECONDS * 10 * 1000 * 1000;
    for (;;) {
        NTSTATUS st = KeWaitForSingleObject(&ctx->StopEvent, Executive, KernelMode, FALSE, &timeout);
        FlushCounters(ctx);
        if (st != STATUS_TIMEOUT) break;
        elapsed += FLUSH_SECONDS;
        if (!cleared && elapsed >= GUARD_SECONDS) {
            RegPutDword(L"StartPending", 0);
            cleared = TRUE;
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID StopGuardThread(PSWD_CONTEXT Ctx)
{
    if (Ctx->Thread) {
        KeSetEvent(&Ctx->StopEvent, IO_NO_INCREMENT, FALSE);
        KeWaitForSingleObject(Ctx->Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(Ctx->Thread);
        Ctx->Thread = NULL;
    }
    if (Ctx->ThreadHandle) {
        ZwClose(Ctx->ThreadHandle);
        Ctx->ThreadHandle = NULL;
    }
}

/* ------------------------------------------------------------ EDID */

static VOID BuildEdid(VOID)
{
    UCHAR *e = gEdid;
    ULONG hblank = SWD_HTOTAL - SWD_NATIVE_W;     /* 280 */
    ULONG vblank = SWD_VTOTAL - SWD_NATIVE_H;     /* 20 */
    ULONG pclk10k = SWD_PCLK / 10000;             /* 7800 */
    static const UCHAR name[13] = { 'S','w','i','t','c','h',' ','P','a','n','e','l','\n' };
    ULONG i, sum = 0;
    UCHAR *d;

    RtlZeroMemory(gEdid, sizeof(gEdid));
    e[0] = 0x00; for (i = 1; i < 7; i++) e[i] = 0xFF; e[7] = 0x00;
    e[8] = 0x4E; e[9] = 0xEF;                     /* "SWO" */
    e[10] = 0x01; e[11] = 0x00;                   /* product 1 */
    e[16] = 1; e[17] = 36;                        /* week 1, 2026 */
    e[18] = 1; e[19] = 4;                         /* EDID 1.4 */
    e[20] = 0xA0;                                 /* digital, 8 bpc */
    e[21] = (UCHAR)((SWD_WIDTH_MM + 5) / 10);     /* cm */
    e[22] = (UCHAR)((SWD_HEIGHT_MM + 5) / 10);
    e[23] = 0x78;                                 /* gamma 2.2 */
    e[24] = 0x06;                                 /* sRGB, preferred timing is native */
    {   /* sRGB chromaticity */
        static const UCHAR c[10] = { 0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54 };
        RtlCopyMemory(&e[25], c, sizeof(c));
    }
    for (i = 38; i < 54; i++) e[i] = 0x01;        /* no standard timings */

    d = &e[54];                                   /* detailed timing */
    d[0] = (UCHAR)(pclk10k & 0xFF); d[1] = (UCHAR)(pclk10k >> 8);
    d[2] = (UCHAR)(SWD_NATIVE_W & 0xFF); d[3] = (UCHAR)(hblank & 0xFF);
    d[4] = (UCHAR)(((SWD_NATIVE_W >> 8) << 4) | (hblank >> 8));
    d[5] = (UCHAR)(SWD_NATIVE_H & 0xFF); d[6] = (UCHAR)(vblank & 0xFF);
    d[7] = (UCHAR)(((SWD_NATIVE_H >> 8) << 4) | (vblank >> 8));
    d[8] = 136; d[9] = 72;                        /* h front porch, h sync */
    d[10] = (UCHAR)((10 << 4) | 1);               /* v front porch 10, v sync 1 */
    d[11] = 0;
    d[12] = (UCHAR)(SWD_WIDTH_MM & 0xFF); d[13] = (UCHAR)(SWD_HEIGHT_MM & 0xFF);
    d[14] = (UCHAR)(((SWD_WIDTH_MM >> 8) << 4) | (SWD_HEIGHT_MM >> 8));
    d[17] = 0x18;                                 /* digital separate sync */

    d = &e[72];                                   /* monitor name */
    d[3] = 0xFC; RtlCopyMemory(&d[5], name, sizeof(name));
    d = &e[90];                                   /* range limits (1.0.5) */
    d[3] = 0xFD; d[5] = 50; d[6] = 75;            /* 50-75 Hz */
    d[7] = 30; d[8] = 90;                         /* 30-90 kHz */
    d[9] = 8;                                     /* 80 MHz */
    d[10] = 0x01; d[11] = 0x0A;                   /* range limits only */
    for (i = 12; i < 18; i++) d[i] = 0x20;
    d = &e[108]; d[3] = 0x10;                     /* dummy */

    for (i = 0; i < 127; i++) sum += e[i];
    e[127] = (UCHAR)(0x100 - (sum & 0xFF));
}

/* ------------------------------------------------------------ helpers */

static BOOLEAN IsRotated(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r)
{
    return r == D3DKMDT_VPPR_ROTATE90 || r == D3DKMDT_VPPR_ROTATE270;
}

static D3DKMDT_VIDPN_PRESENT_PATH_ROTATION NormalizeRotation(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r)
{
    switch (r) {
    case D3DKMDT_VPPR_ROTATE90:
    case D3DKMDT_VPPR_ROTATE180:
    case D3DKMDT_VPPR_ROTATE270:
        return r;
    default:
        return D3DKMDT_VPPR_IDENTITY;
    }
}

static BOOLEAN RotationKnown(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r)
{
    return r == D3DKMDT_VPPR_UNINITIALIZED || r == D3DKMDT_VPPR_IDENTITY || r == D3DKMDT_VPPR_ROTATE90 ||
           r == D3DKMDT_VPPR_ROTATE180 || r == D3DKMDT_VPPR_ROTATE270 ||
           r == D3DKMDT_VPPR_UNPINNED || r == D3DKMDT_VPPR_NOTSPECIFIED;
}

static BOOLEAN ScalingOk(D3DKMDT_VIDPN_PRESENT_PATH_SCALING s)
{
    return s == D3DKMDT_VPPS_UNINITIALIZED || s == D3DKMDT_VPPS_IDENTITY ||
           s == D3DKMDT_VPPS_UNPINNED || s == D3DKMDT_VPPS_NOTSPECIFIED;
}

static BOOLEAN RotationPinned(D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r)
{
    return r == D3DKMDT_VPPR_IDENTITY || r == D3DKMDT_VPPR_ROTATE90 ||
           r == D3DKMDT_VPPR_ROTATE180 || r == D3DKMDT_VPPR_ROTATE270;
}

/* 1.0.9: like the Basic Display sample, the source mode is always the panel's
 * own size (720x1280), also for 90/270. The DWM then presents a 1280x720
 * surface with Flags.Rotate set and the driver rotates it. 1.0.7/1.0.8 offered
 * 1280x720 for 90/270 and refused Windows' 720x1280 + 270 ("could not be saved"). */
static VOID SourceSizeFor(PSWD_CONTEXT Ctx, D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r, ULONG *W, ULONG *H)
{
    UNREFERENCED_PARAMETER(r);
    *W = Ctx->FbWidth; *H = Ctx->FbHeight;
}

/* Target and monitor signal: active = total = 720x1280, frequencies not
 * specified (as the KMDOD sample). 1.0.3-1.0.10 tried real timings
 * (78 MHz, 1000x1300, 60 Hz) in several forms; Windows refused all of them for
 * the target, and a real monitor mode emptied the GDI mode list. */
static VOID FillSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *s)
{
    RtlZeroMemory(s, sizeof(*s));
    s->VideoStandard = D3DKMDT_VSS_OTHER;
    s->ActiveSize.cx = SWD_NATIVE_W;
    s->ActiveSize.cy = SWD_NATIVE_H;
    s->TotalSize = s->ActiveSize;
    s->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;   /* VSyncFreqDivider stays 0 (not Miracast) */
}
static VOID FillSourceMode(D3DKMDT_VIDPN_SOURCE_MODE *m, ULONG W, ULONG H)
{
    m->Type = D3DKMDT_RMT_GRAPHICS;
    m->Format.Graphics.PrimSurfSize.cx = W;
    m->Format.Graphics.PrimSurfSize.cy = H;
    m->Format.Graphics.VisibleRegionSize = m->Format.Graphics.PrimSurfSize;
    m->Format.Graphics.Stride = W * 4;
    m->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
    m->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
    m->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
}

static VOID ClearFb(PSWD_CONTEXT Ctx)
{
    ULONG y;
    if (!Ctx->FbVa) return;
    for (y = 0; y < Ctx->FbHeight; y++) {
        RtlZeroMemory(Ctx->FbVa + (SIZE_T)y * Ctx->FbPitch, (SIZE_T)Ctx->FbWidth * 4);
    }
    KeMemoryBarrier();
}

static VOID UnmapFb(PSWD_CONTEXT Ctx)
{
    if (Ctx->FbVa) {
        MmUnmapIoSpace(Ctx->FbVa, Ctx->FbMapSize);
        Ctx->FbVa = NULL;
    }
    if (Ctx->DsiVa) {
        MmUnmapIoSpace(Ctx->DsiVa, DSI_MAP_SIZE);
        Ctx->DsiVa = NULL;
    }
    if (Ctx->DcVa) {
        MmUnmapIoSpace(Ctx->DcVa, DC_MAP_SIZE);
        Ctx->DcVa = NULL;
    }
}

static VOID ReadDcTrace(VOID)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR dc;

    pa.QuadPart = (LONGLONG)DC_A_PHYS;
    dc = (PUCHAR)MmMapIoSpaceEx(pa, DC_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!dc) { RegPutDword(L"DcMapped", 0); return; }
    RegPutDword(L"DcMapped", 1);
    RegPutDword(L"DcWindowHeader", READ_REGISTER_ULONG((PULONG)(dc + DC_WINDOW_HEADER)));
    RegPutDword(L"DcWinOptions", READ_REGISTER_ULONG((PULONG)(dc + DC_WIN_OPTIONS)));
    RegPutDword(L"DcColorDepth", READ_REGISTER_ULONG((PULONG)(dc + DC_WIN_COLOR_DEPTH)));
    RegPutDword(L"DcWinSize", READ_REGISTER_ULONG((PULONG)(dc + DC_WIN_SIZE)));
    RegPutDword(L"DcLineStride", READ_REGISTER_ULONG((PULONG)(dc + DC_WIN_LINE_STRIDE)));
    RegPutDword(L"DcStartAddr", READ_REGISTER_ULONG((PULONG)(dc + DC_WINBUF_START)));
    MmUnmapIoSpace(dc, DC_MAP_SIZE);
}

/* Copies source rect [l,r)x[t,b) (source coordinates) to the framebuffer with
 * the current rotation. Row is a function giving the source row pointer. */
static VOID BltRect(PSWD_CONTEXT Ctx, PUCHAR SrcBase, LONG SrcTop, LONG Pitch, RECT Rc,
                    LONG Ws, LONG Hs, D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rot)
{
    LONG Fw = (LONG)Ctx->FbWidth, Fh = (LONG)Ctx->FbHeight;
    LONG dl, dr, dt, db, dx, dy;
#define SRCPX(x, y) (((const ULONG *)(SrcBase + (SIZE_T)((y) - SrcTop) * (SIZE_T)Pitch))[(x)])
#define DSTROW(y)   ((ULONG *)(Ctx->FbVa + (SIZE_T)(y) * Ctx->FbPitch))

    if (Rc.left < 0) Rc.left = 0;
    if (Rc.top < 0) Rc.top = 0;
    if (Rc.right > Ws) Rc.right = Ws;
    if (Rc.bottom > Hs) Rc.bottom = Hs;
    if (Rc.left >= Rc.right || Rc.top >= Rc.bottom) return;

    switch (Rot) {
    case D3DKMDT_VPPR_ROTATE90:     /* dst(x', y') = (Hs-1-y, x) */
        dl = Hs - Rc.bottom; dr = Hs - Rc.top; dt = Rc.left; db = Rc.right;
        if (dr > Fw) dr = Fw; if (db > Fh) db = Fh;
        for (dy = dt; dy < db; dy++) {
            ULONG *d = DSTROW(dy);
            for (dx = dl; dx < dr; dx++) d[dx] = SRCPX(dy, Hs - 1 - dx);
        }
        break;
    case D3DKMDT_VPPR_ROTATE180:    /* dst = (Ws-1-x, Hs-1-y) */
        dl = Ws - Rc.right; dr = Ws - Rc.left; dt = Hs - Rc.bottom; db = Hs - Rc.top;
        if (dr > Fw) dr = Fw; if (db > Fh) db = Fh;
        for (dy = dt; dy < db; dy++) {
            ULONG *d = DSTROW(dy);
            for (dx = dl; dx < dr; dx++) d[dx] = SRCPX(Ws - 1 - dx, Hs - 1 - dy);
        }
        break;
    case D3DKMDT_VPPR_ROTATE270:    /* dst = (y, Ws-1-x) */
        dl = Rc.top; dr = Rc.bottom; dt = Ws - Rc.right; db = Ws - Rc.left;
        if (dr > Fw) dr = Fw; if (db > Fh) db = Fh;
        for (dy = dt; dy < db; dy++) {
            ULONG *d = DSTROW(dy);
            for (dx = dl; dx < dr; dx++) d[dx] = SRCPX(Ws - 1 - dy, dx);
        }
        break;
    default:
        if (Rc.right > Fw) Rc.right = Fw;
        if (Rc.bottom > Fh) Rc.bottom = Fh;
        if (Rc.left >= Rc.right) break;
        for (dy = Rc.top; dy < Rc.bottom; dy++) {
            RtlCopyMemory(DSTROW(dy) + Rc.left, &SRCPX(Rc.left, dy), (SIZE_T)(Rc.right - Rc.left) * 4);
        }
        break;
    }
#undef SRCPX
#undef DSTROW
}

/* ------------------------------------------------------------ stage 2: panel sleep */

static VOID SleepMs(ULONG Ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)Ms * 10 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

static VOID DsiDcs(PSWD_CONTEXT Ctx, ULONG Cmd, ULONG WaitMs)
{
    /* The packet goes out in the next blanking of the video stream; wait one
     * frame (16.7 ms) plus the panel's own delay. */
    WRITE_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_WR_DATA), (Cmd << 8) | MIPI_DCS_SHORT_WRITE);
    SleepMs(20 + WaitMs);
}

static VOID DcSetMode(PSWD_CONTEXT Ctx, ULONG Mode)
{
    PUCHAR dc = Ctx->DcVa;
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_ACCESS_REG), 0);      /* assembly copy */
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_DISPLAY_COMMAND_REG), Mode);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG), GENERAL_UPDATE);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG), GENERAL_ACT_REQ);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG), GENERAL_UPDATE);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG), GENERAL_ACT_REQ);
}

/* Display off + sleep in. The DC keeps scanning and the DSI link stays in HS
 * video mode, so waking needs no panel init (stage 2a). switchfan.sys turns
 * the backlight off on its own. */
static VOID PanelSleep(PSWD_CONTEXT Ctx)
{
    if (!Ctx->DsiVa || Ctx->PanelAsleep || RegGetDword(L"PanelPower", 1) == 0) return;
    RegPutDword(L"DsiStatusBeforeOff", READ_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_STATUS_REG)));
    WRITE_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_VIDEO_MODE_CTL), DSI_CMD_PKT_VID_ENABLE);
    DsiDcs(Ctx, DCS_DISPLAY_OFF, 20);
    DsiDcs(Ctx, DCS_ENTER_SLEEP, 50);
    WRITE_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_VIDEO_MODE_CTL), 0);
    Ctx->PanelAsleep = TRUE;
    /* Stage 2b (1.0.12): with the panel asleep, stop the DC so it no longer
     * reads the framebuffer from memory 60 times a second. */
    if (Ctx->DcVa && RegGetDword(L"DcStop", 1) != 0) {
        SleepMs(20);
        DcSetMode(Ctx, DISP_CTRL_MODE_STOP);
        Ctx->DcStopped = TRUE;
        InterlockedIncrement(&Ctx->DcStops);
        SleepMs(20);
        RegPutDword(L"DcCommandAfterStop", READ_REGISTER_ULONG((PULONG)(Ctx->DcVa + DC_CMD_DISPLAY_COMMAND_REG)));
    }
    InterlockedIncrement(&Ctx->PanelOffs);
    RegPutDword(L"DsiStatusAfterOff", READ_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_STATUS_REG)));
}

/* Sleep out (the JDI panel needs 180 ms, as in coreboot/hekate) + display on. */
static VOID PanelWake(PSWD_CONTEXT Ctx)
{
    if (!Ctx->DsiVa || !Ctx->PanelAsleep) return;
    if (Ctx->DcStopped) {
        /* Restart scanning first: the DCS packets below ride the video stream. */
        DcSetMode(Ctx, DISP_CTRL_MODE_C_DISPLAY);
        Ctx->DcStopped = FALSE;
        SleepMs(50);
        RegPutDword(L"DcCommandAfterStart", READ_REGISTER_ULONG((PULONG)(Ctx->DcVa + DC_CMD_DISPLAY_COMMAND_REG)));
        RegPutDword(L"DsiStatusAfterDcStart", READ_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_STATUS_REG)));
    }
    WRITE_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_VIDEO_MODE_CTL), DSI_CMD_PKT_VID_ENABLE);
    DsiDcs(Ctx, DCS_EXIT_SLEEP, 180);
    DsiDcs(Ctx, DCS_DISPLAY_ON, 20);
    WRITE_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_VIDEO_MODE_CTL), 0);
    Ctx->PanelAsleep = FALSE;
    InterlockedIncrement(&Ctx->PanelOns);
    RegPutDword(L"DsiStatusAfterOn", READ_REGISTER_ULONG((PULONG)(Ctx->DsiVa + DSI_STATUS_REG)));
}

/* ------------------------------------------------------------ DDIs */

DXGKDDI_ADD_DEVICE SwdAddDevice;
NTSTATUS SwdAddDevice(IN_CONST_PDEVICE_OBJECT PhysicalDeviceObject, OUT_PPVOID MiniportDeviceContext)
{
    PSWD_CONTEXT ctx;
    if (!PhysicalDeviceObject || !MiniportDeviceContext) return STATUS_INVALID_PARAMETER;
    ctx = (PSWD_CONTEXT)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SWD_CONTEXT), SWD_TAG);
    if (!ctx) return STATUS_INSUFFICIENT_RESOURCES;
    ctx->Pdo = PhysicalDeviceObject;
    ctx->Rotation = D3DKMDT_VPPR_IDENTITY;
    ctx->Visible = TRUE;
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    /* 1.2.1: dxgkrnl may connect the DC-A interrupt (level, UEFI R409) before
     * StartDevice. 1.2.0 masked the DC interrupts only in StartDevice, and the
     * device failed with problem 43 right after starting. Map DC-A here and
     * silence it, so the line is quiet whenever the ISR can run. */
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (LONGLONG)DC_A_PHYS;
        ctx->IrqDc = (PUCHAR)MmMapIoSpaceEx(pa, DC_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        if (ctx->IrqDc) {
            InterlockedExchange(&gEarlyIntStatus, (LONG)READ_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_STATUS_REG)));
            InterlockedExchange(&gEarlyIntMask, (LONG)READ_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_MASK_REG)));
            InterlockedExchange(&gEarlyIntEnable, (LONG)READ_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_ENABLE_REG)));
            WRITE_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_MASK_REG), 0);
            WRITE_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_ENABLE_REG), 0);
            WRITE_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_STATUS_REG), 0xFFFFFFFFu);
        }
    }
    *MiniportDeviceContext = ctx;
    return STATUS_SUCCESS;
}

DXGKDDI_START_DEVICE SwdStartDevice;
NTSTATUS SwdStartDevice(IN_CONST_PVOID MiniportDeviceContext, IN_PDXGK_START_INFO DxgkStartInfo,
                        IN_PDXGKRNL_INTERFACE DxgkInterface, OUT_PULONG NumberOfVideoPresentSources,
                        OUT_PULONG NumberOfChildren)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    DXGK_DISPLAY_INFORMATION info;
    NTSTATUS st;
    ULONG pending, starts;
    OBJECT_ATTRIBUTES oa;

    if (!ctx || !DxgkInterface || !NumberOfVideoPresentSources || !NumberOfChildren) return STATUS_INVALID_PARAMETER;
    ctx->Dxgk = *DxgkInterface;

    RegPutDword(L"Version", SWD_VERSION);
    {
        /* Values only the 1.0.x test builds wrote. */
        static const PCWSTR old[] = { L"TgtVariant", L"TgtAddSt", L"TgtAssignSt", L"MonVariant", L"QaiTypes",
            L"ChildStatusTypes", L"EdidReads", L"RotatePresents", L"TgtSt0", L"TgtSt1", L"TgtSt2", L"TgtSt3",
            L"TgtSt4", L"TgtSt5", L"TgtSt6", L"TgtSt7", L"TgtSt8", L"TgtSt9" };
        ULONG k;
        for (k = 0; k < sizeof(old) / sizeof(old[0]); k++)
            (VOID)RtlDeleteRegistryValue(RTL_REGISTRY_ABSOLUTE, gParamPath.Buffer, old[k]);
    }
    gUseEdid = RegGetDword(L"UseEdid", 0) != 0;
    RegPutDword(L"EdidGiven", gUseEdid ? 1 : 0);
    starts = RegGetDword(L"StartCount", 0) + 1;
    RegPutDword(L"StartCount", starts);

    /* Boot-loop guard. */
    pending = RegGetDword(L"StartPending", 0);
    if (pending >= 2 && RegGetDword(L"IgnoreGuard", 0) == 0) {
        RegPutDword(L"GuardTripped", RegGetDword(L"GuardTripped", 0) + 1);
        RegPutDword(L"LastStartStatus", (ULONG)STATUS_RETRY);
        return STATUS_UNSUCCESSFUL;
    }
    RegPutDword(L"StartPending", pending + 1);

    ReadDcTrace();
    {
        PHYSICAL_ADDRESS pa;
        pa.QuadPart = (LONGLONG)DSI_A_PHYS;
        ctx->DsiVa = (PUCHAR)MmMapIoSpaceEx(pa, DSI_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        pa.QuadPart = (LONGLONG)DC_A_PHYS;
        ctx->DcVa = (PUCHAR)MmMapIoSpaceEx(pa, DC_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
        ctx->DcStopped = FALSE;
        ctx->PanelAsleep = FALSE;
        RegPutDword(L"DsiMapped", ctx->DsiVa ? 1 : 0);
    }

    /* 1.0.1: which resources did the device get (the INF LogConfig)? */
    {
        DXGK_DEVICE_INFO di;
        ULONG nMem = 0, total = 0, f, r;
        RtlZeroMemory(&di, sizeof(di));
        st = ctx->Dxgk.DxgkCbGetDeviceInformation(ctx->Dxgk.DeviceHandle, &di);
        RegPutDword(L"DevInfoStatus", (ULONG)st);
        if (NT_SUCCESS(st) && di.TranslatedResourceList) {
            PCM_FULL_RESOURCE_DESCRIPTOR full = di.TranslatedResourceList->List;
            for (f = 0; f < di.TranslatedResourceList->Count; f++) {
                PCM_PARTIAL_RESOURCE_LIST part = &full->PartialResourceList;
                for (r = 0; r < part->Count; r++) {
                    PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &part->PartialDescriptors[r];
                    total++;
                    if (d->Type == CmResourceTypeMemory && nMem == 0) {
                        RegPutDword(L"ResMemStart", d->u.Memory.Start.LowPart);
                        RegPutDword(L"ResMemLength", d->u.Memory.Length);
                    }
                    if (d->Type == CmResourceTypeMemory) nMem++;
                    if (d->Type == CmResourceTypeInterrupt) {
                        ctx->HasInterrupt = TRUE;
                        RegPutDword(L"ResIrqVector", d->u.Interrupt.Vector);
                    }
                }
                full = (PCM_FULL_RESOURCE_DESCRIPTOR)&part->PartialDescriptors[part->Count];
            }
        }
        RegPutDword(L"ResCount", total);
        RegPutDword(L"HasInterrupt", ctx->HasInterrupt ? 1 : 0);
        ctx->VsyncAllowed = RegGetDword(L"Vsync", 1) != 0;
        if (ctx->DcVa) {
            /* Nothing delivered until Windows asks for vsync (ControlInterrupt). */
            WRITE_REGISTER_ULONG((PULONG)(ctx->DcVa + DC_CMD_INT_MASK_REG), 0);
            WRITE_REGISTER_ULONG((PULONG)(ctx->DcVa + DC_CMD_INT_ENABLE_REG), 0);
            WRITE_REGISTER_ULONG((PULONG)(ctx->DcVa + DC_CMD_INT_STATUS_REG), 0xFFFFFFFFu);
        }
        RegPutDword(L"ResMemCount", nMem);
    }

    /* Take the boot display over from the Basic Display Driver. */
    RtlZeroMemory(&info, sizeof(info));
    st = ctx->Dxgk.DxgkCbAcquirePostDisplayOwnership(ctx->Dxgk.DeviceHandle, &info);
    RegPutDword(L"PostStatus", (ULONG)st);
    RegPutDword(L"PostWidth", info.Width);
    RegPutDword(L"PostHeight", info.Height);
    RegPutDword(L"PostPitch", info.Pitch);
    RegPutDword(L"PostFormat", (ULONG)info.ColorFormat);
    RegPutDword(L"PostAddrLo", info.PhysicAddress.LowPart);
    RegPutDword(L"PostAddrHi", (ULONG)info.PhysicAddress.HighPart);

    if (NT_SUCCESS(st) && info.Width != 0 && info.Height != 0) {
        ctx->FbPhys = info.PhysicAddress;
        ctx->FbWidth = info.Width;
        ctx->FbHeight = info.Height;
        ctx->FbPitch = info.Pitch;
        ctx->FbFormat = info.ColorFormat;
        RegPutDword(L"FbSource", 1);
    } else if (RegGetDword(L"AllowFixedFb", 0) != 0) {
        ctx->FbPhys.QuadPart = (LONGLONG)SWD_FB_PHYS;
        ctx->FbWidth = SWD_NATIVE_W;
        ctx->FbHeight = SWD_NATIVE_H;
        ctx->FbPitch = SWD_NATIVE_PITCH;
        ctx->FbFormat = D3DDDIFMT_A8R8G8B8;
        RegPutDword(L"FbSource", 2);
    } else {
        RegPutDword(L"FbSource", 0);
        RegPutDword(L"LastStartStatus", (ULONG)STATUS_NOT_SUPPORTED);
        RegPutDword(L"StartPending", pending);   /* not a crash */
        return STATUS_UNSUCCESSFUL;
    }

    /* Only the 32-bit panel layout this driver knows. */
    if ((ctx->FbFormat != D3DDDIFMT_A8R8G8B8 && ctx->FbFormat != D3DDDIFMT_X8R8G8B8) ||
        ctx->FbPitch < ctx->FbWidth * 4 || (ULONGLONG)ctx->FbPitch * ctx->FbHeight > 0x01000000ull) {
        RegPutDword(L"LastStartStatus", (ULONG)STATUS_NOT_SUPPORTED);
        RegPutDword(L"StartPending", pending);
        return STATUS_UNSUCCESSFUL;
    }

    ctx->FbMapSize = (SIZE_T)ctx->FbPitch * ctx->FbHeight;
    ctx->FbVa = (PUCHAR)MmMapIoSpaceEx(ctx->FbPhys, ctx->FbMapSize, PAGE_READWRITE | PAGE_WRITECOMBINE);
    if (!ctx->FbVa) {
        RegPutDword(L"LastStartStatus", (ULONG)STATUS_INSUFFICIENT_RESOURCES);
        RegPutDword(L"StartPending", pending);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->SrcWidth = ctx->FbWidth;
    ctx->SrcHeight = ctx->FbHeight;
    ctx->Rotation = D3DKMDT_VPPR_IDENTITY;
    ctx->Active = TRUE;      /* the boot image stays until the first commit */
    ctx->Visible = TRUE;

    KeClearEvent(&ctx->StopEvent);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&ctx->ThreadHandle, THREAD_ALL_ACCESS, &oa, NULL, NULL, GuardThread, ctx);
    if (NT_SUCCESS(st)) {
        st = ObReferenceObjectByHandle(ctx->ThreadHandle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&ctx->Thread, NULL);
        if (!NT_SUCCESS(st)) ctx->Thread = NULL;
    } else {
        ctx->ThreadHandle = NULL;
    }

    *NumberOfVideoPresentSources = 1;
    *NumberOfChildren = 1;
    ctx->Started = TRUE;
    RegPutDword(L"LastStartStatus", 0);
    UNREFERENCED_PARAMETER(DxgkStartInfo);
    return STATUS_SUCCESS;
}

static VOID VsyncSet(PSWD_CONTEXT Ctx, BOOLEAN On);

static VOID StopCommon(PSWD_CONTEXT Ctx)
{
    VsyncSet(Ctx, FALSE);
    StopGuardThread(Ctx);
    FlushCounters(Ctx);
    UnmapFb(Ctx);
    Ctx->Started = FALSE;
    Ctx->Active = FALSE;
}

DXGKDDI_STOP_DEVICE SwdStopDevice;
NTSTATUS SwdStopDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    StopCommon(ctx);
    RegPutDword(L"StartPending", 0);    /* a clean stop is not a crash */
    return STATUS_SUCCESS;
}

DXGKDDI_STOP_DEVICE_AND_RELEASE_POST_DISPLAY_OWNERSHIP SwdStopAndRelease;
NTSTATUS SwdStopAndRelease(PVOID MiniportDeviceContext, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                           PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    if (TargetId != 0 || !DisplayInfo) return STATUS_INVALID_PARAMETER;
    /* Hand back the framebuffer in its native layout, with the panel awake. */
    PanelWake(ctx);
    ctx->Rotation = D3DKMDT_VPPR_IDENTITY;
    ClearFb(ctx);
    DisplayInfo->Width = ctx->FbWidth;
    DisplayInfo->Height = ctx->FbHeight;
    DisplayInfo->Pitch = ctx->FbPitch;
    DisplayInfo->ColorFormat = ctx->FbFormat;
    DisplayInfo->PhysicAddress = ctx->FbPhys;
    DisplayInfo->TargetId = 0;
    DisplayInfo->AcpiId = 0;
    StopCommon(ctx);
    RegPutDword(L"StartPending", 0);
    RegPutDword(L"Released", 1);
    return STATUS_SUCCESS;
}

DXGKDDI_REMOVE_DEVICE SwdRemoveDevice;
NTSTATUS SwdRemoveDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    if (ctx) {
        StopGuardThread(ctx);
        UnmapFb(ctx);
        if (ctx->IrqDc) {
            WRITE_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_MASK_REG), 0);
            MmUnmapIoSpace(ctx->IrqDc, DC_MAP_SIZE);
            ctx->IrqDc = NULL;
        }
        ExFreePoolWithTag(ctx, SWD_TAG);
    }
    return STATUS_SUCCESS;
}

DXGKDDI_DISPATCH_IO_REQUEST SwdDispatchIoRequest;
NTSTATUS SwdDispatchIoRequest(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG VidPnSourceId,
                              IN_PVIDEO_REQUEST_PACKET VideoRequestPacket)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(VidPnSourceId);
    UNREFERENCED_PARAMETER(VideoRequestPacket);
    return STATUS_UNSUCCESSFUL;
}

DXGKDDI_INTERRUPT_ROUTINE SwdInterruptRoutine;
BOOLEAN SwdInterruptRoutine(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG MessageNumber)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    ULONG st;
    UNREFERENCED_PARAMETER(MessageNumber);
    InterlockedIncrement(&gIsrCalls);
    if (!ctx || !ctx->IrqDc) { InterlockedIncrement(&gIsrUnclaimed); return FALSE; }
    st = READ_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_STATUS_REG)) &
         READ_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_MASK_REG));
    if (!st) { InterlockedIncrement(&gIsrUnclaimed); return FALSE; }
    WRITE_REGISTER_ULONG((PULONG)(ctx->IrqDc + DC_CMD_INT_STATUS_REG), st);
    if (st & DC_INT_V_BLANK) {
        DXGKARGCB_NOTIFY_INTERRUPT_DATA d;
        RtlZeroMemory(&d, sizeof(d));
        d.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_VSYNC;
        d.DisplayOnlyVsync.VidPnTargetId = 0;
        ctx->Dxgk.DxgkCbNotifyInterrupt(ctx->Dxgk.DeviceHandle, &d);
        ctx->Dxgk.DxgkCbQueueDpc(ctx->Dxgk.DeviceHandle);
        InterlockedIncrement(&ctx->VsyncInts);
    } else {
        InterlockedIncrement(&ctx->IsrOther);
    }
    return TRUE;
}

DXGKDDI_DPC_ROUTINE SwdDpcRoutine;
VOID SwdDpcRoutine(IN_CONST_PVOID MiniportDeviceContext)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    if (ctx) ctx->Dxgk.DxgkCbNotifyDpc(ctx->Dxgk.DeviceHandle);
}

/* Runs at the interrupt's IRQL (DxgkCbSynchronizeExecution), so the ISR never
 * sees a half-written ENABLE / MASK pair. */
static BOOLEAN SyncVsync(PVOID Context)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)Context;
    PUCHAR dc = ctx->IrqDc;
    ULONG en = READ_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_ENABLE_REG));
    ULONG mask = READ_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_MASK_REG));
    if (ctx->VsyncWant) {
        WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_STATUS_REG), DC_INT_V_BLANK);
        WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_ENABLE_REG), en | DC_INT_V_BLANK);
        WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_MASK_REG), mask | DC_INT_V_BLANK);
    } else {
        WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_MASK_REG), mask & ~DC_INT_V_BLANK);
        WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_ENABLE_REG), en & ~DC_INT_V_BLANK);
        WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_STATUS_REG), DC_INT_V_BLANK);
    }
    return TRUE;
}

static VOID VsyncSet(PSWD_CONTEXT Ctx, BOOLEAN On)
{
    BOOLEAN ret = FALSE;
    if (!Ctx->IrqDc || !Ctx->HasInterrupt) return;
    Ctx->VsyncWant = On;
    (VOID)Ctx->Dxgk.DxgkCbSynchronizeExecution(Ctx->Dxgk.DeviceHandle, SyncVsync, Ctx, 0, &ret);
}

/* Stage 3: Windows turns the display-only vsync interrupt on and off. Without
 * the DSDT interrupt (UEFI R408) or with Vsync=0, say "not implemented" and
 * Windows keeps pacing by its timer as before. */
DXGKDDI_CONTROLINTERRUPT SwdControlInterrupt;
NTSTATUS SwdControlInterrupt(IN_CONST_HANDLE hAdapter, IN_CONST_DXGK_INTERRUPT_TYPE InterruptType, IN_BOOLEAN EnableInterrupt)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    NTSTATUS st;
    BOOLEAN vsyncType = (InterruptType == DXGK_INTERRUPT_DISPLAYONLY_VSYNC || InterruptType == DXGK_INTERRUPT_CRTC_VSYNC);
    InterlockedIncrement(&gCiCalls);
    InterlockedExchange(&gCiLast, (LONG)(((ULONG)InterruptType << 8) | (EnableInterrupt ? 1u : 0u)));
    if ((ULONG)InterruptType < 32) InterlockedOr(&gCiTypes, (LONG)(1u << (ULONG)InterruptType));
    if (!EnableInterrupt) {
        /* 1.2.2: turning something off always succeeds. 1.2.0/1.2.1 answered
         * NOT_IMPLEMENTED to anything but DISPLAYONLY_VSYNC, and with the DSDT
         * interrupt (R409) the device then failed right after start (problem 43). */
        if (vsyncType) { VsyncSet(ctx, FALSE); InterlockedIncrement(&ctx->VsyncDisables); }
        st = STATUS_SUCCESS;
    } else if (!vsyncType || !ctx->HasInterrupt || !ctx->VsyncAllowed || !ctx->IrqDc) {
        st = STATUS_NOT_IMPLEMENTED;
    } else {
        VsyncSet(ctx, TRUE);
        InterlockedIncrement(&ctx->VsyncEnables);
        st = STATUS_SUCCESS;
    }
    InterlockedExchange(&gCiLastSt, (LONG)st);
    return st;
}

static NTSTATUS ImplQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext, PDXGK_CHILD_DESCRIPTOR ChildRelations,
                                ULONG ChildRelationsSize)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    if (!ChildRelations || ChildRelationsSize < sizeof(DXGK_CHILD_DESCRIPTOR)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(&ChildRelations[0], sizeof(DXGK_CHILD_DESCRIPTOR));
    ChildRelations[0].ChildDeviceType = TypeVideoOutput;
    ChildRelations[0].ChildCapabilities.HpdAwareness = HpdAwarenessAlwaysConnected;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_INTERNAL;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    ChildRelations[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    ChildRelations[0].AcpiUid = 0;
    ChildRelations[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

static NTSTATUS ImplQueryChildStatus(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus,
                             IN_BOOLEAN NonDestructiveOnly)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    if (!ChildStatus || ChildStatus->ChildUid != 0) return STATUS_INVALID_PARAMETER;
    switch (ChildStatus->Type) {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        /* D3DKMDT_MOA_NONE was reported. */
        return STATUS_INVALID_PARAMETER;
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS ImplQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid,
                                  INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    ULONG len;
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    if (ChildUid != 0 || !DeviceDescriptor) return STATUS_INVALID_PARAMETER;
    /* 1.0.6: no EDID unless UseEdid=1. With the EDID (1.0.3-1.0.5) every
     * target mode was refused with STATUS_GRAPHICS_INVALID_FREQUENCY while the
     * same signal was accepted as a monitor mode. The Basic Display sample
     * gives no EDID. */
    if (!gUseEdid) return STATUS_MONITOR_NO_DESCRIPTOR;
    if (DeviceDescriptor->DescriptorOffset >= sizeof(gEdid)) return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    len = sizeof(gEdid) - DeviceDescriptor->DescriptorOffset;
    if (len > DeviceDescriptor->DescriptorLength) len = DeviceDescriptor->DescriptorLength;
    if (!DeviceDescriptor->DescriptorBuffer || len == 0) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(DeviceDescriptor->DescriptorBuffer, gEdid + DeviceDescriptor->DescriptorOffset, len);
    return STATUS_SUCCESS;
}

static NTSTATUS ImplSetPowerState(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG DeviceUid,
                          IN_DEVICE_POWER_STATE DevicePowerState, IN_POWER_ACTION ActionType)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(ActionType);
    InterlockedIncrement(&ctx->PowerCalls);
    InterlockedExchange(&ctx->LastPower, (LONG)(((DeviceUid & 0xFFFFFF) << 8) | ((ULONG)DevicePowerState & 0xFF)));
    /* Stage 2a (1.0.11): the monitor (child 0) or the whole adapter going to
     * D1-D3 puts the panel to sleep; D0 wakes it. */
    if (DeviceUid == 0 || DeviceUid == DISPLAY_ADAPTER_HW_ID) {
        if (DeviceUid == 0) ctx->MonitorOff = (DevicePowerState != PowerDeviceD0);
        if (DevicePowerState == PowerDeviceD0) PanelWake(ctx); else PanelSleep(ctx);
    }
    return STATUS_SUCCESS;
}

DXGKDDI_NOTIFY_ACPI_EVENT SwdNotifyAcpiEvent;
NTSTATUS SwdNotifyAcpiEvent(IN_CONST_PVOID MiniportDeviceContext, IN_DXGK_EVENT_TYPE EventType, IN_ULONG Event,
                            IN_PVOID Argument, OUT_PULONG AcpiFlags)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(EventType);
    UNREFERENCED_PARAMETER(Event);
    UNREFERENCED_PARAMETER(Argument);
    if (AcpiFlags) *AcpiFlags = 0;
    return STATUS_SUCCESS;
}

DXGKDDI_RESET_DEVICE SwdResetDevice;
VOID SwdResetDevice(IN_CONST_PVOID MiniportDeviceContext)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
}

DXGKDDI_UNLOAD SwdUnload;
VOID SwdUnload(VOID)
{
}

DXGKDDI_QUERY_INTERFACE SwdQueryInterface;
NTSTATUS SwdQueryInterface(IN_CONST_PVOID MiniportDeviceContext, IN_PQUERY_INTERFACE QueryInterface)
{
    UNREFERENCED_PARAMETER(MiniportDeviceContext);
    UNREFERENCED_PARAMETER(QueryInterface);
    return STATUS_NOT_SUPPORTED;
}

DXGKDDI_CONTROL_ETW_LOGGING SwdControlEtwLogging;
VOID SwdControlEtwLogging(IN_BOOLEAN Enable, IN_ULONG Flags, IN_UCHAR Level)
{
    UNREFERENCED_PARAMETER(Enable);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(Level);
}

static NTSTATUS ImplQueryAdapterInfo(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_QUERYADAPTERINFO pQueryAdapterInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    switch (pQueryAdapterInfo->Type) {
    case DXGKQAITYPE_DRIVERCAPS: {
        DXGK_DRIVERCAPS *caps = (DXGK_DRIVERCAPS *)pQueryAdapterInfo->pOutputData;
        if (!caps || pQueryAdapterInfo->OutputDataSize < sizeof(DXGK_DRIVERCAPS)) return STATUS_BUFFER_TOO_SMALL;
        RtlZeroMemory(caps, sizeof(*caps));
        caps->WDDMVersion = DXGKDDI_WDDMv1_3;
        caps->HighestAcceptableAddress.QuadPart = -1;
        caps->SupportNonVGA = TRUE;
        caps->SupportSmoothRotation = TRUE;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_NOT_SUPPORTED;
    }
}

static NTSTATUS ImplSetPointerPosition(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETPOINTERPOSITION pSetPointerPosition)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPointerPosition);
    return STATUS_SUCCESS;
}

DXGKDDI_SETPOINTERSHAPE SwdSetPointerShape;
NTSTATUS SwdSetPointerShape(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETPOINTERSHAPE pSetPointerShape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pSetPointerShape);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS ImplEscape(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_ESCAPE pEscape)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pEscape);
    return STATUS_NOT_SUPPORTED;
}

/* --- VidPN */

static NTSTATUS CheckPinnedModes(PSWD_CONTEXT Ctx, D3DKMDT_HVIDPN hVidPn, const DXGK_VIDPN_INTERFACE *Vidpn,
                                 const D3DKMDT_VIDPN_PRESENT_PATH *Path, BOOLEAN *Ok)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSrc = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *srcIf = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *srcMode = NULL;
    D3DKMDT_HVIDPNTARGETMODESET hTgt = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *tgtIf = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *tgtMode = NULL;
    NTSTATUS st;

    st = Vidpn->pfnAcquireSourceModeSet(hVidPn, Path->VidPnSourceId, &hSrc, &srcIf);
    if (!NT_SUCCESS(st)) return st;
    st = srcIf->pfnAcquirePinnedModeInfo(hSrc, &srcMode);
    if (NT_SUCCESS(st) && srcMode) {
        ULONG w = (ULONG)srcMode->Format.Graphics.PrimSurfSize.cx;
        ULONG h = (ULONG)srcMode->Format.Graphics.PrimSurfSize.cy;
        BOOLEAN native = (w == Ctx->FbWidth && h == Ctx->FbHeight);
        BOOLEAN swapped = (w == Ctx->FbHeight && h == Ctx->FbWidth);
        D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r = Path->ContentTransformation.Rotation;
        if (srcMode->Type != D3DKMDT_RMT_GRAPHICS ||
            (srcMode->Format.Graphics.PixelFormat != D3DDDIFMT_A8R8G8B8 &&
             srcMode->Format.Graphics.PixelFormat != D3DDDIFMT_X8R8G8B8)) {
            *Ok = FALSE;
            Reject(4, (LONG)srcMode->Type, (LONG)srcMode->Format.Graphics.PixelFormat);
        } else if (!native && !swapped) {
            *Ok = FALSE; Reject(5, (LONG)((w << 16) | h), (LONG)r);
        }
        srcIf->pfnReleaseModeInfo(hSrc, srcMode);
    }
    Vidpn->pfnReleaseSourceModeSet(hVidPn, hSrc);
    if (!*Ok) return STATUS_SUCCESS;

    st = Vidpn->pfnAcquireTargetModeSet(hVidPn, Path->VidPnTargetId, &hTgt, &tgtIf);
    if (!NT_SUCCESS(st)) return st;
    st = tgtIf->pfnAcquirePinnedModeInfo(hTgt, &tgtMode);
    if (NT_SUCCESS(st) && tgtMode) {
        if (tgtMode->VideoSignalInfo.ActiveSize.cx != SWD_NATIVE_W ||
            tgtMode->VideoSignalInfo.ActiveSize.cy != SWD_NATIVE_H) {
            *Ok = FALSE;
            Reject(6, (LONG)((tgtMode->VideoSignalInfo.ActiveSize.cx << 16) | tgtMode->VideoSignalInfo.ActiveSize.cy), 0);
        }
        tgtIf->pfnReleaseModeInfo(hTgt, tgtMode);
    }
    Vidpn->pfnReleaseTargetModeSet(hVidPn, hTgt);
    return STATUS_SUCCESS;
}

static NTSTATUS ImplIsSupportedVidPn(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_ISSUPPORTEDVIDPN pIsSupportedVidPn)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next = NULL;
    SIZE_T n = 0;
    BOOLEAN ok = TRUE;
    NTSTATUS st;

    pIsSupportedVidPn->IsVidPnSupported = FALSE;
    st = ctx->Dxgk.DxgkCbQueryVidPnInterface(pIsSupportedVidPn->hDesiredVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st)) return st;
    st = vidpn->pfnGetTopology(pIsSupportedVidPn->hDesiredVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st)) return st;
    st = topoIf->pfnGetNumPaths(topo, &n);
    if (!NT_SUCCESS(st)) return st;
    if (n == 0) { pIsSupportedVidPn->IsVidPnSupported = TRUE; return STATUS_SUCCESS; }

    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    while (NT_SUCCESS(st) && path && ok) {
        if (path->VidPnSourceId != 0 || path->VidPnTargetId != 0 ||
            !ScalingOk(path->ContentTransformation.Scaling) ||
            !RotationKnown(path->ContentTransformation.Rotation)) {
            ok = FALSE;
            Reject(1, (LONG)((path->VidPnSourceId << 16) | path->VidPnTargetId),
                   (LONG)(path->ContentTransformation.Rotation | (path->ContentTransformation.Scaling << 8)));
        } else {
            st = CheckPinnedModes(ctx, pIsSupportedVidPn->hDesiredVidPn, vidpn, path, &ok);
            if (!NT_SUCCESS(st)) { topoIf->pfnReleasePathInfo(topo, path); return st; }
        }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) ? NULL : next;
    }
    if (path) topoIf->pfnReleasePathInfo(topo, path);
    if (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) st = STATUS_SUCCESS;
    if (!NT_SUCCESS(st)) return st;
    pIsSupportedVidPn->IsVidPnSupported = ok;
    return STATUS_SUCCESS;
}

static NTSTATUS ImplRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter,
                                     IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST pRecommendFunctionalVidPn)
{
    UNREFERENCED_PARAMETER(hAdapter);
    UNREFERENCED_PARAMETER(pRecommendFunctionalVidPn);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static NTSTATUS EnsureSourceModes(PSWD_CONTEXT Ctx, D3DKMDT_HVIDPN hVidPn, const DXGK_VIDPN_INTERFACE *Vidpn,
                                  const D3DKMDT_VIDPN_PRESENT_PATH *Path)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *pinned = NULL;
    D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
    ULONG w, h;
    NTSTATUS st;

    st = Vidpn->pfnAcquireSourceModeSet(hVidPn, Path->VidPnSourceId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) return st;
    st = setIf->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(st) && pinned) {
        setIf->pfnReleaseModeInfo(hSet, pinned);
        Vidpn->pfnReleaseSourceModeSet(hVidPn, hSet);
        return STATUS_SUCCESS;           /* keep what the OS pinned */
    }
    Vidpn->pfnReleaseSourceModeSet(hVidPn, hSet);

    st = Vidpn->pfnCreateNewSourceModeSet(hVidPn, Path->VidPnSourceId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) return st;
    SourceSizeFor(Ctx, RotationPinned(Path->ContentTransformation.Rotation) ?
                  Path->ContentTransformation.Rotation : D3DKMDT_VPPR_IDENTITY, &w, &h);
    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (NT_SUCCESS(st)) {
        FillSourceMode(mode, w, h);
        st = setIf->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(st)) setIf->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(st)) st = Vidpn->pfnAssignSourceModeSet(hVidPn, Path->VidPnSourceId, hSet);
    if (!NT_SUCCESS(st)) Vidpn->pfnReleaseSourceModeSet(hVidPn, hSet);
    return st;
}

static NTSTATUS EnsureTargetModes(D3DKMDT_HVIDPN hVidPn, const DXGK_VIDPN_INTERFACE *Vidpn,
                                  const D3DKMDT_VIDPN_PRESENT_PATH *Path)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *mode = NULL;
    NTSTATUS st;

    st = Vidpn->pfnAcquireTargetModeSet(hVidPn, Path->VidPnTargetId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) return st;
    st = setIf->pfnAcquirePinnedModeInfo(hSet, &pinned);
    if (NT_SUCCESS(st) && pinned) {
        setIf->pfnReleaseModeInfo(hSet, pinned);
        Vidpn->pfnReleaseTargetModeSet(hVidPn, hSet);
        return STATUS_SUCCESS;
    }
    Vidpn->pfnReleaseTargetModeSet(hVidPn, hSet);

    st = Vidpn->pfnCreateNewTargetModeSet(hVidPn, Path->VidPnTargetId, &hSet, &setIf);
    if (!NT_SUCCESS(st)) return st;
    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (NT_SUCCESS(st)) {
        FillSignalInfo(&mode->VideoSignalInfo);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        st = setIf->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(st)) setIf->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(st)) st = Vidpn->pfnAssignTargetModeSet(hVidPn, Path->VidPnTargetId, hSet);    if (!NT_SUCCESS(st)) Vidpn->pfnReleaseTargetModeSet(hVidPn, hSet);
    return st;
}

static NTSTATUS ImplEnumVidPnCofuncModality(IN_CONST_HANDLE hAdapter,
                                    IN_CONST_PDXGKARG_ENUMVIDPNCOFUNCMODALITY_CONST pEnumCofuncModality)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    D3DKMDT_HVIDPN hVidPn = pEnumCofuncModality->hConstrainingVidPn;
    D3DKMDT_ENUMCOFUNCMODALITY_PIVOT_TYPE pivot = pEnumCofuncModality->EnumPivotType;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL, *next = NULL;
    NTSTATUS st;

    st = ctx->Dxgk.DxgkCbQueryVidPnInterface(hVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st)) return st;
    st = vidpn->pfnGetTopology(hVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st)) return st;

    st = topoIf->pfnAcquireFirstPathInfo(topo, &path);
    if (st == STATUS_GRAPHICS_DATASET_IS_EMPTY) return STATUS_SUCCESS;
    while (NT_SUCCESS(st) && path) {
        BOOLEAN isPivotPath = (pEnumCofuncModality->EnumPivot.VidPnSourceId == path->VidPnSourceId &&
                               pEnumCofuncModality->EnumPivot.VidPnTargetId == path->VidPnTargetId);
        if (!(pivot == D3DKMDT_EPT_VIDPNSOURCE && pEnumCofuncModality->EnumPivot.VidPnSourceId == path->VidPnSourceId)) {
            st = EnsureSourceModes(ctx, hVidPn, vidpn, path);
            if (!NT_SUCCESS(st)) InterlockedExchange(&gCofuncStep, 1);
        }
        if (NT_SUCCESS(st) &&
            !(pivot == D3DKMDT_EPT_VIDPNTARGET && pEnumCofuncModality->EnumPivot.VidPnTargetId == path->VidPnTargetId)) {
            st = EnsureTargetModes(hVidPn, vidpn, path);
            if (!NT_SUCCESS(st)) InterlockedExchange(&gCofuncStep, 2);
        }
        if (NT_SUCCESS(st)) {
            D3DKMDT_VIDPN_PRESENT_PATH local = *path;
            BOOLEAN changed = FALSE;
            if (!(pivot == D3DKMDT_EPT_SCALING && isPivotPath) &&
                (local.ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
                 local.ContentTransformation.Scaling == D3DKMDT_VPPS_UNINITIALIZED ||
                 local.ContentTransformation.Scaling == D3DKMDT_VPPS_NOTSPECIFIED)) {
                RtlZeroMemory(&local.ContentTransformation.ScalingSupport, sizeof(local.ContentTransformation.ScalingSupport));
                local.ContentTransformation.ScalingSupport.Identity = 1;
                changed = TRUE;
            }
            /* 1.0.8: report all four rotations also when the rotation is
             * already pinned (1.0.7 only did it while unpinned). */
            if (!(pivot == D3DKMDT_EPT_ROTATION && isPivotPath) &&
                !(local.ContentTransformation.RotationSupport.Identity && local.ContentTransformation.RotationSupport.Rotate90 &&
                  local.ContentTransformation.RotationSupport.Rotate180 && local.ContentTransformation.RotationSupport.Rotate270)) {
                RtlZeroMemory(&local.ContentTransformation.RotationSupport, sizeof(local.ContentTransformation.RotationSupport));
                local.ContentTransformation.RotationSupport.Identity = 1;
                local.ContentTransformation.RotationSupport.Rotate90 = 1;
                local.ContentTransformation.RotationSupport.Rotate180 = 1;
                local.ContentTransformation.RotationSupport.Rotate270 = 1;
                changed = TRUE;
            }
            if (changed) {
                st = topoIf->pfnUpdatePathSupportInfo(topo, &local);
                if (!NT_SUCCESS(st)) {
                    InterlockedExchange(&gSupportSt, (LONG)st);
                    if (RotationPinned(path->ContentTransformation.Rotation)) st = STATUS_SUCCESS;   /* keep 1.0.7 behaviour */
                }
            }
            if (!NT_SUCCESS(st)) InterlockedExchange(&gCofuncStep, 3);
        }
        if (!NT_SUCCESS(st)) { topoIf->pfnReleasePathInfo(topo, path); return st; }
        st = topoIf->pfnAcquireNextPathInfo(topo, path, &next);
        topoIf->pfnReleasePathInfo(topo, path);
        path = (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) ? NULL : next;
    }
    if (st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) st = STATUS_SUCCESS;
    return st;
}

static NTSTATUS ImplSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter,
                                     IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY pSetVidPnSourceVisibility)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    if (pSetVidPnSourceVisibility->VidPnSourceId != 0 &&
        pSetVidPnSourceVisibility->VidPnSourceId != D3DDDI_ID_ALL) return STATUS_INVALID_PARAMETER;
    if (!pSetVidPnSourceVisibility->Visible && ctx->Visible) ClearFb(ctx);
    ctx->Visible = pSetVidPnSourceVisibility->Visible;
    return STATUS_SUCCESS;
}

static NTSTATUS ImplCommitVidPn(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_COMMITVIDPN_CONST pCommitVidPn)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    D3DKMDT_HVIDPN hVidPn = pCommitVidPn->hFunctionalVidPn;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot = D3DKMDT_VPPR_IDENTITY;
    SIZE_T n = 0;
    ULONG w = 0, h = 0, ew, eh;
    NTSTATUS st;

    InterlockedIncrement(&ctx->Commits);
    if (pCommitVidPn->AffectedVidPnSourceId != 0 && pCommitVidPn->AffectedVidPnSourceId != D3DDDI_ID_ALL)
        return STATUS_INVALID_PARAMETER;

    st = ctx->Dxgk.DxgkCbQueryVidPnInterface(hVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st)) return st;
    st = vidpn->pfnGetTopology(hVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st)) return st;
    st = topoIf->pfnGetNumPathsFromSource(topo, 0, &n);
    if (!NT_SUCCESS(st) || n == 0) {
        /* Source 0 is not in the topology: nothing is shown. */
        if (ctx->Active) ClearFb(ctx);
        ctx->Active = FALSE;
        RegPutDword(L"CommitActive", 0);
        return STATUS_SUCCESS;
    }

    st = vidpn->pfnAcquireSourceModeSet(hVidPn, 0, &hSet, &setIf);
    if (!NT_SUCCESS(st)) return st;
    st = setIf->pfnAcquirePinnedModeInfo(hSet, &mode);
    if (NT_SUCCESS(st) && mode) {
        w = (ULONG)mode->Format.Graphics.PrimSurfSize.cx;
        h = (ULONG)mode->Format.Graphics.PrimSurfSize.cy;
        setIf->pfnReleaseModeInfo(hSet, mode);
    }
    vidpn->pfnReleaseSourceModeSet(hVidPn, hSet);
    if (w == 0 || h == 0) {
        if (ctx->Active) ClearFb(ctx);
        ctx->Active = FALSE;
        RegPutDword(L"CommitActive", 0);
        return STATUS_SUCCESS;
    }

    st = topoIf->pfnAcquirePathInfo(topo, 0, 0, &path);
    if (NT_SUCCESS(st) && path) {
        rot = NormalizeRotation(path->ContentTransformation.Rotation);
        topoIf->pfnReleasePathInfo(topo, path);
    }
    SourceSizeFor(ctx, rot, &ew, &eh);
    if (!((w == ew && h == eh) || (w == eh && h == ew))) return STATUS_GRAPHICS_INVALID_VIDPN_SOURCEMODESET;

    if (!ctx->Active || ctx->Rotation != rot || ctx->SrcWidth != w || ctx->SrcHeight != h) ClearFb(ctx);
    ctx->Rotation = rot;
    ctx->SrcWidth = w;
    ctx->SrcHeight = h;
    ctx->Active = TRUE;
    RegPutDword(L"CommitActive", 1);
    RegPutDword(L"Rotation", (ULONG)rot);
    RegPutDword(L"SrcWidth", w);
    RegPutDword(L"SrcHeight", h);
    return STATUS_SUCCESS;
}

static NTSTATUS ImplUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST pUpdateActiveVidPnPresentPath)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = &pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot;

    if (path->VidPnSourceId != 0 || path->VidPnTargetId != 0) return STATUS_INVALID_PARAMETER;
    if (!ScalingOk(path->ContentTransformation.Scaling)) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    if (!RotationKnown(path->ContentTransformation.Rotation)) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    rot = NormalizeRotation(path->ContentTransformation.Rotation);
    /* 1.0.8: DRIVERCAPS says SupportSmoothRotation, so Windows rotates with
     * this call alone, also 0<->90 (the desktop surface then changes from
     * 720x1280 to 1280x720). 1.0.7 refused that ("could not be saved"). */
    if (rot != ctx->Rotation) {
        ctx->Rotation = rot;
        ClearFb(ctx);
    }
    RegPutDword(L"Rotation", (ULONG)rot);
    RegPutDword(L"SrcWidth", ctx->SrcWidth);
    RegPutDword(L"SrcHeight", ctx->SrcHeight);
    return STATUS_SUCCESS;
}

static NTSTATUS ImplRecommendMonitorModes(IN_CONST_HANDLE hAdapter,
                                  IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST pRecommendMonitorModes)
{
    const DXGK_MONITORSOURCEMODESET_INTERFACE *setIf = pRecommendMonitorModes->pMonitorSourceModeSetInterface;
    D3DKMDT_HMONITORSOURCEMODESET hSet = pRecommendMonitorModes->hMonitorSourceModeSet;
    D3DKMDT_MONITOR_SOURCE_MODE *mode = NULL;
    NTSTATUS st;
    UNREFERENCED_PARAMETER(hAdapter);

    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (!NT_SUCCESS(st)) return st;
    FillSignalInfo(&mode->VideoSignalInfo);
    mode->ColorBasis = D3DKMDT_CB_SRGB;
    mode->ColorCoeffDynamicRanges.FirstChannel = 8;
    mode->ColorCoeffDynamicRanges.SecondChannel = 8;
    mode->ColorCoeffDynamicRanges.ThirdChannel = 8;
    mode->ColorCoeffDynamicRanges.FourthChannel = 0;
    mode->Origin = D3DKMDT_MCO_DRIVER;
    mode->Preference = D3DKMDT_MP_PREFERRED;
    st = setIf->pfnAddMode(hSet, mode);
    if (!NT_SUCCESS(st)) {
        setIf->pfnReleaseModeInfo(hSet, mode);
        if (st == STATUS_GRAPHICS_MODE_ALREADY_IN_MODESET) st = STATUS_SUCCESS;
    }
    return st;
}
static NTSTATUS ImplQueryVidPnHWCapability(IN_CONST_HANDLE i_hAdapter, INOUT_PDXGKARG_QUERYVIDPNHWCAPABILITY io_pVidPnHWCaps)
{
    UNREFERENCED_PARAMETER(i_hAdapter);
    if (io_pVidPnHWCaps->SourceId != 0 || io_pVidPnHWCaps->TargetId != 0) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&io_pVidPnHWCaps->VidPnHWCaps, sizeof(io_pVidPnHWCaps->VidPnHWCaps));
    io_pVidPnHWCaps->VidPnHWCaps.DriverRotation = 1;   /* rotation is done in the copy */
    return STATUS_SUCCESS;
}

static VOID GrowBounds(LONG *Top, LONG *Bottom, const RECT *R)
{
    if (R->top < *Top) *Top = R->top;
    if (R->bottom > *Bottom) *Bottom = R->bottom;
}

DXGKDDI_PRESENTDISPLAYONLY SwdPresentDisplayOnly;
NTSTATUS SwdPresentDisplayOnly(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_PRESENT_DISPLAYONLY pPresent)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    LONG top = MAXLONG, bottom = MINLONG;
    ULONG i;
    PMDL mdl;
    PUCHAR src, va;
    SIZE_T len;
    KPROCESSOR_MODE mode;
    NTSTATUS st = STATUS_SUCCESS;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot;
    LONG ws, hs;

    InterlockedIncrement(&ctx->Presents);
    if (pPresent->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    /* Drawing continues while the monitor is off (stage 1 keeps the panel
     * scanning), so the image is complete when it comes back on. */
    if (!ctx->FbVa || !ctx->Active || !ctx->Visible || ctx->SystemDisplay) return STATUS_SUCCESS;
    if (pPresent->BytesPerPixel != 4 || pPresent->Pitch <= 0 || !pPresent->pSource) {
        st = STATUS_INVALID_PARAMETER;
        goto out;
    }

    /* Surface shape: for 90/270 with Flags.Rotate the DWM surface is the mode
     * size swapped (1280x720), as in the Basic Display sample. */
    rot = pPresent->Flags.Rotate ? ctx->Rotation : D3DKMDT_VPPR_IDENTITY;
    if (IsRotated(rot)) { ws = (LONG)ctx->SrcHeight; hs = (LONG)ctx->SrcWidth; }
    else                { ws = (LONG)ctx->SrcWidth;  hs = (LONG)ctx->SrcHeight; }

    for (i = 0; i < pPresent->NumMoves; i++) GrowBounds(&top, &bottom, &pPresent->pMoves[i].DestRect);
    for (i = 0; i < pPresent->NumDirtyRects; i++) GrowBounds(&top, &bottom, &pPresent->pDirtyRect[i]);
    if (top < 0) top = 0;
    if (bottom > hs) bottom = hs;
    if (top >= bottom) return STATUS_SUCCESS;

    /* pSource is the DWM's surface, valid for this call only. Lock just the
     * rows that change. */
    va = (PUCHAR)pPresent->pSource + (SIZE_T)top * (SIZE_T)pPresent->Pitch;
    len = (SIZE_T)(bottom - top) * (SIZE_T)pPresent->Pitch;
    if (len > 0xFFFFFFFFull) { st = STATUS_INVALID_PARAMETER; goto out; }
    mdl = IoAllocateMdl(va, (ULONG)len, FALSE, FALSE, NULL);
    if (!mdl) { st = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    mode = ((ULONG_PTR)va >= (ULONG_PTR)MmSystemRangeStart) ? KernelMode : UserMode;
    __try {
        MmProbeAndLockPages(mdl, mode, IoReadAccess);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        st = GetExceptionCode();
    }
    if (!NT_SUCCESS(st)) { IoFreeMdl(mdl); goto out; }
    src = (PUCHAR)MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority | MdlMappingNoExecute);
    if (src) {
        /* The source already holds the final image, so a move is copied
         * from the source at its destination like a dirty rect. */
        for (i = 0; i < pPresent->NumMoves; i++) BltRect(ctx, src, top, pPresent->Pitch, pPresent->pMoves[i].DestRect, ws, hs, rot);
        for (i = 0; i < pPresent->NumDirtyRects; i++) BltRect(ctx, src, top, pPresent->Pitch, pPresent->pDirtyRect[i], ws, hs, rot);
        KeMemoryBarrier();
    } else {
        st = STATUS_INSUFFICIENT_RESOURCES;
    }
    MmUnlockPages(mdl);
    IoFreeMdl(mdl);

out:
    if (!NT_SUCCESS(st)) {
        InterlockedIncrement(&ctx->PresentErrors);
        InterlockedExchange(&ctx->LastPresentStatus, (LONG)st);
    }
    return st;
}

static NTSTATUS ImplSystemDisplayEnable(PVOID MiniportDeviceContext, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, UINT *Width, UINT *Height,
                                D3DDDIFORMAT *ColorFormat)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(Flags);
    if (TargetId != 0 || !ctx->FbVa) return STATUS_UNSUCCESSFUL;
    ctx->SystemDisplay = TRUE;
    *Width = ctx->FbWidth;
    *Height = ctx->FbHeight;
    *ColorFormat = D3DDDIFMT_A8R8G8B8;
    return STATUS_SUCCESS;
}

DXGKDDI_SYSTEM_DISPLAY_WRITE SwdSystemDisplayWrite;
VOID SwdSystemDisplayWrite(PVOID MiniportDeviceContext, PVOID Source, UINT SourceWidth, UINT SourceHeight,
                           UINT SourceStride, UINT PositionX, UINT PositionY)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    UINT y, w, h;
    if (!ctx->FbVa || !Source || PositionX >= ctx->FbWidth || PositionY >= ctx->FbHeight) return;
    w = SourceWidth;  if (w > ctx->FbWidth - PositionX) w = ctx->FbWidth - PositionX;
    h = SourceHeight; if (h > ctx->FbHeight - PositionY) h = ctx->FbHeight - PositionY;
    for (y = 0; y < h; y++) {
        RtlCopyMemory(ctx->FbVa + (SIZE_T)(PositionY + y) * ctx->FbPitch + (SIZE_T)PositionX * 4,
                      (PUCHAR)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
    }
}

/* ------------------------------------------------------------ DDI wrappers (count / fail / last status) */

DXGKDDI_QUERY_CHILD_RELATIONS SwdQueryChildRelations;
NTSTATUS SwdQueryChildRelations(IN_CONST_PVOID MiniportDeviceContext, PDXGK_CHILD_DESCRIPTOR ChildRelations, ULONG ChildRelationsSize)
{ return Note(D_QueryChildRelations, ImplQueryChildRelations(MiniportDeviceContext, ChildRelations, ChildRelationsSize)); }

DXGKDDI_QUERY_CHILD_STATUS SwdQueryChildStatus;
NTSTATUS SwdQueryChildStatus(IN_CONST_PVOID MiniportDeviceContext, INOUT_PDXGK_CHILD_STATUS ChildStatus, IN_BOOLEAN NonDestructiveOnly)
{
    return Note(D_QueryChildStatus, ImplQueryChildStatus(MiniportDeviceContext, ChildStatus, NonDestructiveOnly));
}

DXGKDDI_QUERY_DEVICE_DESCRIPTOR SwdQueryDeviceDescriptor;
NTSTATUS SwdQueryDeviceDescriptor(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG ChildUid, INOUT_PDXGK_DEVICE_DESCRIPTOR DeviceDescriptor)
{
    return Note(D_QueryDeviceDescriptor, ImplQueryDeviceDescriptor(MiniportDeviceContext, ChildUid, DeviceDescriptor));
}

DXGKDDI_SET_POWER_STATE SwdSetPowerState;
NTSTATUS SwdSetPowerState(IN_CONST_PVOID MiniportDeviceContext, IN_ULONG DeviceUid, IN_DEVICE_POWER_STATE DevicePowerState, IN_POWER_ACTION ActionType)
{ return Note(D_SetPowerState, ImplSetPowerState(MiniportDeviceContext, DeviceUid, DevicePowerState, ActionType)); }

DXGKDDI_QUERYADAPTERINFO SwdQueryAdapterInfo;
NTSTATUS SwdQueryAdapterInfo(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_QUERYADAPTERINFO pQueryAdapterInfo)
{
    return Note(D_QueryAdapterInfo, ImplQueryAdapterInfo(hAdapter, pQueryAdapterInfo));
}

DXGKDDI_SETPOINTERPOSITION SwdSetPointerPosition;
NTSTATUS SwdSetPointerPosition(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETPOINTERPOSITION pSetPointerPosition)
{ return Note(D_SetPointerPosition, ImplSetPointerPosition(hAdapter, pSetPointerPosition)); }

DXGKDDI_ESCAPE SwdEscape;
NTSTATUS SwdEscape(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_ESCAPE pEscape)
{ return Note(D_Escape, ImplEscape(hAdapter, pEscape)); }

DXGKDDI_ISSUPPORTEDVIDPN SwdIsSupportedVidPn;
NTSTATUS SwdIsSupportedVidPn(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_ISSUPPORTEDVIDPN pIsSupportedVidPn)
{
    NTSTATUS st = ImplIsSupportedVidPn(hAdapter, pIsSupportedVidPn);
    if (NT_SUCCESS(st) && !pIsSupportedVidPn->IsVidPnSupported) InterlockedIncrement(&gNotSupported);
    return Note(D_IsSupportedVidPn, st);
}

DXGKDDI_RECOMMENDFUNCTIONALVIDPN SwdRecommendFunctionalVidPn;
NTSTATUS SwdRecommendFunctionalVidPn(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST p)
{ return Note(D_RecommendFunctionalVidPn, ImplRecommendFunctionalVidPn(hAdapter, p)); }

DXGKDDI_ENUMVIDPNCOFUNCMODALITY SwdEnumVidPnCofuncModality;
NTSTATUS SwdEnumVidPnCofuncModality(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_ENUMVIDPNCOFUNCMODALITY_CONST p)
{ return Note(D_EnumVidPnCofuncModality, ImplEnumVidPnCofuncModality(hAdapter, p)); }

DXGKDDI_SETVIDPNSOURCEVISIBILITY SwdSetVidPnSourceVisibility;
NTSTATUS SwdSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY p)
{ return Note(D_SetVidPnSourceVisibility, ImplSetVidPnSourceVisibility(hAdapter, p)); }

DXGKDDI_COMMITVIDPN SwdCommitVidPn;
NTSTATUS SwdCommitVidPn(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_COMMITVIDPN_CONST p)
{ return Note(D_CommitVidPn, ImplCommitVidPn(hAdapter, p)); }

DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH SwdUpdateActiveVidPnPresentPath;
NTSTATUS SwdUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST p)
{ return Note(D_UpdateActiveVidPnPresentPath, ImplUpdateActiveVidPnPresentPath(hAdapter, p)); }

DXGKDDI_RECOMMENDMONITORMODES SwdRecommendMonitorModes;
NTSTATUS SwdRecommendMonitorModes(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST p)
{ return Note(D_RecommendMonitorModes, ImplRecommendMonitorModes(hAdapter, p)); }

DXGKDDI_QUERYVIDPNHWCAPABILITY SwdQueryVidPnHWCapability;
NTSTATUS SwdQueryVidPnHWCapability(IN_CONST_HANDLE i_hAdapter, INOUT_PDXGKARG_QUERYVIDPNHWCAPABILITY p)
{ return Note(D_QueryVidPnHWCapability, ImplQueryVidPnHWCapability(i_hAdapter, p)); }

DXGKDDI_SYSTEM_DISPLAY_ENABLE SwdSystemDisplayEnable;
NTSTATUS SwdSystemDisplayEnable(PVOID MiniportDeviceContext, D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, UINT *Width, UINT *Height, D3DDDIFORMAT *ColorFormat)
{ return Note(D_SystemDisplayEnable, ImplSystemDisplayEnable(MiniportDeviceContext, TargetId, Flags, Width, Height, ColorFormat)); }

/* ------------------------------------------------------------ entry */

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    KMDDOD_INITIALIZATION_DATA init;
    static const WCHAR suffix[] = L"\\Parameters";

    if (RegistryPath && RegistryPath->Length + sizeof(suffix) <= sizeof(gParamPathBuf)) {
        RtlCopyMemory(gParamPathBuf, RegistryPath->Buffer, RegistryPath->Length);
        RtlCopyMemory((PUCHAR)gParamPathBuf + RegistryPath->Length, suffix, sizeof(suffix));
        RtlInitUnicodeString(&gParamPath, gParamPathBuf);
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, gParamPathBuf);
    }
    BuildEdid();

    RtlZeroMemory(&init, sizeof(init));
    init.Version = DXGKDDI_INTERFACE_VERSION;
    init.DxgkDdiAddDevice = SwdAddDevice;
    init.DxgkDdiStartDevice = SwdStartDevice;
    init.DxgkDdiStopDevice = SwdStopDevice;
    init.DxgkDdiRemoveDevice = SwdRemoveDevice;
    init.DxgkDdiDispatchIoRequest = SwdDispatchIoRequest;
    init.DxgkDdiInterruptRoutine = SwdInterruptRoutine;
    init.DxgkDdiDpcRoutine = SwdDpcRoutine;
    init.DxgkDdiQueryChildRelations = SwdQueryChildRelations;
    init.DxgkDdiQueryChildStatus = SwdQueryChildStatus;
    init.DxgkDdiQueryDeviceDescriptor = SwdQueryDeviceDescriptor;
    init.DxgkDdiSetPowerState = SwdSetPowerState;
    init.DxgkDdiNotifyAcpiEvent = SwdNotifyAcpiEvent;
    init.DxgkDdiResetDevice = SwdResetDevice;
    init.DxgkDdiUnload = SwdUnload;
    init.DxgkDdiQueryInterface = SwdQueryInterface;
    init.DxgkDdiControlEtwLogging = SwdControlEtwLogging;
    init.DxgkDdiQueryAdapterInfo = SwdQueryAdapterInfo;
    init.DxgkDdiSetPointerPosition = SwdSetPointerPosition;
    init.DxgkDdiSetPointerShape = SwdSetPointerShape;
    init.DxgkDdiEscape = SwdEscape;
    init.DxgkDdiIsSupportedVidPn = SwdIsSupportedVidPn;
    init.DxgkDdiRecommendFunctionalVidPn = SwdRecommendFunctionalVidPn;
    init.DxgkDdiEnumVidPnCofuncModality = SwdEnumVidPnCofuncModality;
    init.DxgkDdiSetVidPnSourceVisibility = SwdSetVidPnSourceVisibility;
    init.DxgkDdiCommitVidPn = SwdCommitVidPn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = SwdUpdateActiveVidPnPresentPath;
    init.DxgkDdiRecommendMonitorModes = SwdRecommendMonitorModes;
    init.DxgkDdiQueryVidPnHWCapability = SwdQueryVidPnHWCapability;
    init.DxgkDdiPresentDisplayOnly = SwdPresentDisplayOnly;
    init.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = SwdStopAndRelease;
    init.DxgkDdiSystemDisplayEnable = SwdSystemDisplayEnable;
    init.DxgkDdiSystemDisplayWrite = SwdSystemDisplayWrite;
    init.DxgkDdiControlInterrupt = SwdControlInterrupt;

    return DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &init);
}
