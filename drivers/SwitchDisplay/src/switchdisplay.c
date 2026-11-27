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
 *   - 1.3.0, TV mirror over USB-C DP Alt Mode: when UEFI (R434H and later) left
 *     DC-B scanning out 1280x720 to SOR1, DC-B window A is pointed at a 1280x720
 *     buffer of this driver and every present is copied there too, unrotated: a
 *     landscape 1280x720 desktop fills the TV, a portrait one is shrunk to 405x720
 *     in the middle. Windows still sees one display. Registry TvMirror=0 turns it
 *     off.
 *   - 1.4.0, DP Alt hot-plug: switchbattery 1.0.0.7 enters DP Alt Mode when a
 *     hub/dock is plugged in under Windows and calls \Callback\SwitchWoaDpAlt
 *     (1 up / 0 down). This driver's thread then runs the UEFI R426 (DPAUX1,
 *     HPD, DPCD, EDID), R427 (PLLDP, SOR1 pads, link training) and R429/R433
 *     (DC-B power, PLLD2 74.25 MHz, 1280x720 raster, SOR1 attach) steps,
 *     ported unchanged in dpalt_port.inc, and then the TV window above. On
 *     unplug the window goes back, DC-B stops and SOR1/DC-B are held in reset.
 *     The SOR1 pad rail (MAX77620 LDO8) is turned on by UEFI R440H at boot.
 *     The bring-up log is in the registry value TvLog (Status.cmd shows it).
 *   - 1.5.0, TV mode from the EDID (UEFI R443): DC-B runs 1920x1080@60 when the
 *     TV lists it and it fits the DP link, else 1280x720@60. The 1280x720 TV
 *     buffer stays; DC-B window A scales it to the raster (DDA, point sampled;
 *     registry TvFilter=1 also sets the window's H/V filters, untested).
 *   - 1.6.0, TV audio: when the TV picture is on, SOR1 takes DP audio from the
 *     Tegra HDA codec (switchaudio 1.1.0 plays into it), as L4T hda_dc.c does
 *     for DP: HDA clocks on, AUDIO_CTRL source HDA, HBR advertised, DP audio
 *     h/vblank symbols from the mode and the link, DP_AUDIO_CTRL with the
 *     stream parameters from the codec, a fixed ELD (2 ch LPCM 48 kHz) and the
 *     presence bits (the codec's pin sense, switchaudio's jack). Registry
 *     TvAudio=0 turns it off.
 *     1.6.1: TvAudioState is reset to 0 at every start, so switchaudio can
 *     read it (6 = on, 7 = off after unplug) next to the codec's pin sense,
 *     which stayed 0 after an unplug and re-plug with 1.6.0.
 *   - 1.6.2: the boot-loop guard refuses one start and then lets the next
 *     one try (it latched for good before: after quick restarts the TV kept
 *     UEFI's portrait picture and got no audio set-up).
 *   - 1.7.0, extended display: the TV is a second monitor (child 1, target 1,
 *     source 1), reported connected while the TV picture is on, with one
 *     mode, the DC-B raster (1920x1080 or 1280x720; the TV buffer now has the
 *     raster's size, DDA 1.0). Windows then offers Extend / Duplicate / second
 *     screen only. Duplicate is source 0 -> targets 0 and 1: the TV gets the
 *     panel's picture upright and fitted, as the 1.3.0 mirror did. Either
 *     source can drive either target. Registry TvExtend=0 gives the 1.6.2
 *     behaviour (one display, the TV always mirrors it).
 *
 * Trace: HKLM\SYSTEM\CurrentControlSet\Services\switchdisplay\Parameters
 */#define DXGKDDI_INTERFACE_VERSION 0x4002 /* DXGKDDI_INTERFACE_VERSION_WDDM1_3 */
#include <ntddk.h>
#include <dispmprt.h>
#include <ntstrsafe.h>
#include <stdarg.h>
#include "../../SwitchGpu/src/swgpu-blit.h"

#define SWD_VERSION        0x00010A01u   /* 1.10.1 */
#define SWD_NOSRC          0xFFu         /* target not in the topology */
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

/* DC-A (panel) */
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

/* 1.3.0 TV mirror: DC-B (head 1) driving SOR1, set up by UEFI R434H+. The
 * disp2 clock and reset are checked in CAR first: DC-B registers must not be
 * touched while its clock is off. */
#define CAR_PHYS           0x60006000ull
#define CAR_MAP_SIZE       0x1000u
#define CAR_RST_DEVICES_L  0x04u
#define CAR_CLK_OUT_ENB_L  0x10u
#define CAR_DISP2_BIT      (1u << 26)
#define DC_B_PHYS          0x54240000ull
#define DC_DISP_WIN_OPTIONS (0x402u * 4u)
#define DC_DISP_ACTIVE     (0x409u * 4u)
#define DC_SOR1_ENABLE     (1u << 26)
#define DC_WIN_BUF_CTRL    (0x702u * 4u)
#define DC_WIN_POSITION    (0x704u * 4u)
#define DC_WIN_PRESCALED   (0x706u * 4u)
#define DC_WIN_H_INIT_DDA  (0x707u * 4u)
#define DC_WIN_V_INIT_DDA  (0x708u * 4u)
#define DC_WIN_DDA_INC     (0x709u * 4u)
#define DC_WIN_DV_CTRL     (0x70Eu * 4u)
#define DC_WIN_BLEND_1WIN  (0x716u * 4u)
#define DC_WINBUF_H_OFF    (0x806u * 4u)
#define DC_WINBUF_V_OFF    (0x808u * 4u)
#define DC_WINBUF_KIND     (0x80Bu * 4u)
#define DC_WINBUF_START_HI (0x80Du * 4u)
#define DC_WIN_A_SELECT    0x10u
#define WIN_A_UPDATE       (1u << 9)
#define WIN_A_ACT_REQ      (1u << 1)
#define DC_WIN_ENABLE      (1u << 30)
#define DC_DEPTH_B8G8R8A8  12u
#define TV_W               1280u
#define TV_H               720u
#define TV_PITCH           (TV_W * 4u)
#define TV_REGS            17
#define TV_ACT_720         ((720u << 16) | 1280u)
#define TV_ACT_1080        ((1080u << 16) | 1920u)
#define DC_WIN_H_FILTER    (1u << 8)
#define DC_WIN_V_FILTER    (1u << 10)

#define GUARD_SECONDS      60
#define FLUSH_SECONDS      10

typedef struct _SWD_FLIP {
    PUCHAR Second;
    PHYSICAL_ADDRESS Phys;
    PMDL Mdl;
    SIZE_T Bytes;
    BOOLEAN Mapped, Enabled;
    /* 1.10.0 (G2c): panel only. When UEFI's framebuffer has no GPU MDL (outside
     * the reserved range), an own first buffer replaces it as index 0. */
    PUCHAR First;
    PHYSICAL_ADDRESS FirstPhys;
    PMDL FirstMdl;
    volatile LONG Front, Pending;
    ULONG Flips, Timeouts, MaxUs, Polls, GpuForward, CpuForward, Fallbacks, Retained, Status;
} SWD_FLIP;

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
    BOOLEAN              GpuPresent, FbDmaUnsafe, TvDmaUnsafe;
    PMDL                 FbMdl, TvMdl;
    BOOLEAN              DoubleBuffer;
    volatile LONG        F4LockTimeouts;
    KEVENT               FbLock;
    SWD_FLIP             FbFlip, TvFlip;
    PFILE_OBJECT         GpuFile;
    PDEVICE_OBJECT volatile GpuTarget;
    volatile LONG        GpuPresents, GpuFailures, GpuFallbacks, GpuStreak, GpuLastStatus;
    volatile LONG64      GpuPresentUs, GpuFallbackUs;
    PUCHAR               DsiVa;         /* DSI-A registers (stage 2) */
    PUCHAR               DcVa;          /* DC-A registers (stage 2b) */
    BOOLEAN              PanelAsleep;
    BOOLEAN              DcStopped;
    BOOLEAN              HasInterrupt;  /* DSDT DISP has the DC-A interrupt (UEFI R409) */
    PUCHAR               IrqDc;         /* DC-A mapped from AddDevice to RemoveDevice, for the ISR */
    BOOLEAN              VsyncAllowed;  /* registry Vsync, default 1 */
    BOOLEAN              VsyncWant;     /* argument of SyncVsync */
    D3DDDIFORMAT         FbFormat;

    /* current mode. 1.7.0: two sources; PanelSrc / TvSrc say which source
     * drives target 0 (panel) / target 1 (TV), SWD_NOSRC when none. */
    BOOLEAN              Extend;        /* registry TvExtend, default 1 */
    BOOLEAN              Active;        /* the panel shows a source */
    BOOLEAN              Visible[2];
    BOOLEAN              MonitorOff;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;   /* of the panel path */
    ULONG                SrcW[2];
    ULONG                SrcH[2];
    ULONG                PanelSrc;
    ULONG                TvSrc;

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

    /* 1.3.0 TV mirror (DC-B) */
    PUCHAR               TvDc;          /* DC-B registers */
    PUCHAR               TvVa;          /* raster-size buffer (1.7.0), write-combined */
    PHYSICAL_ADDRESS     TvPhys;
    ULONG                TvW, TvH, TvPitch;   /* buffer size, kept after unplug for the modes */
    SIZE_T               TvSize;
    volatile LONG        TvConnected;   /* child 1 status */
    volatile LONG        TvIndicates, TvSrcPresents;
    volatile LONG        TvIndicateSt;
    ULONG                TvSaved[TV_REGS];
    LONG                 TvLastW, TvLastH;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION TvLastBaked;
    volatile LONG        TvPresents;
    KEVENT               TvLock;        /* 1.4.0: a mutex (synchronization event) for TvVa */

    /* 1.4.0 DP Alt hot-plug */
    KEVENT               DpEvent;
    volatile LONG        DpWant;        /* last switchbattery state: 1 up, 0 down */
    PCALLBACK_OBJECT     DpCbObject;
    PVOID                DpCbHandle;
    ULONG                DpFails;       /* bring-up failures since the last unplug */
    volatile LONG        DpUps, DpDowns, DpNotes;
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

#include "swd-gpu-present.inc"
#include "swd-flip.inc"
#include "swd-g2a.inc"

static VOID FlushCounters(PSWD_CONTEXT Ctx)
{
    if (Ctx->GpuPresent) SwdGpuFlushCounters(Ctx);
    if (Ctx->DoubleBuffer) SwdFlipFlush(Ctx);
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
    RegPutDword(L"TvPresents", (ULONG)Ctx->TvPresents);
    RegPutDword(L"DpNotes", (ULONG)Ctx->DpNotes);
    RegPutDword(L"TvConnected", (ULONG)Ctx->TvConnected);
    RegPutDword(L"TvIndicates", (ULONG)Ctx->TvIndicates);
    RegPutDword(L"TvIndicateSt", (ULONG)Ctx->TvIndicateSt);
    RegPutDword(L"TvSrcPresents", (ULONG)Ctx->TvSrcPresents);
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
static VOID DpHandle(PSWD_CONTEXT Ctx);

static VOID GuardThread(PVOID Context)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)Context;
    LARGE_INTEGER timeout;
    LONG elapsed = 0;
    BOOLEAN cleared = FALSE;

    timeout.QuadPart = -(LONGLONG)FLUSH_SECONDS * 10 * 1000 * 1000;
    for (;;) {
        PVOID objs[2];
        NTSTATUS st;
        objs[0] = &ctx->StopEvent;
        objs[1] = &ctx->DpEvent;            /* 1.4.0: DP Alt plug / unplug */
        st = KeWaitForMultipleObjects(2, objs, WaitAny, Executive, KernelMode, FALSE, &timeout, NULL);
        if (st == STATUS_WAIT_1) {
            DpHandle(ctx);
            FlushCounters(ctx);             /* 1.4.1: the 5 s notes kept the timeout from firing */
            /* Claude 1.7.3: with the TV attached the DP notes never let the
             * timeout fire, so 1.7.2 never connected to the GPU (connected 0).
             * Connect here too; it returns at once once connected. */
            if (ctx->GpuPresent) SwdGpuConnect(ctx);
            continue;
        }
        FlushCounters(ctx);
        if (st != STATUS_TIMEOUT) break;
        if (ctx->GpuPresent) SwdGpuConnect(ctx);
        if (ctx->GpuPresent) G2aMaybeRun(ctx);      /* Claude 1.9.0: G2a test, once, when asked */
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

/* 1.0.7/1.0.8 offered 1280x720 for 90/270 and refused Windows' 720x1280 + 270
 * ("could not be saved"); see SourceSizeFor. */

/* Target and monitor signal: active = total = 720x1280, frequencies not
 * specified (as the KMDOD sample). 1.0.3-1.0.10 tried real timings
 * (78 MHz, 1000x1300, 60 Hz) in several forms; Windows refused all of them for
 * the target, and a real monitor mode emptied the GDI mode list. */
static VOID FillSignalInfo(D3DKMDT_VIDEO_SIGNAL_INFO *s, ULONG W, ULONG H)
{
    RtlZeroMemory(s, sizeof(*s));
    s->VideoStandard = D3DKMDT_VSS_OTHER;
    s->ActiveSize.cx = W;
    s->ActiveSize.cy = H;
    s->TotalSize = s->ActiveSize;
    s->VSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->VSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->HSyncFreq.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->HSyncFreq.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->PixelRate = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    s->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;   /* VSyncFreqDivider stays 0 (not Miracast) */
}
/* 1.7.0: target 0 is the panel, target 1 the TV (its buffer size; 1280x720
 * before the first plug-in). */
static VOID TargetSize(PSWD_CONTEXT Ctx, ULONG Target, ULONG *W, ULONG *H)
{
    if (Target == 0) { *W = SWD_NATIVE_W; *H = SWD_NATIVE_H; return; }
    *W = Ctx->TvW ? Ctx->TvW : TV_W;
    *H = Ctx->TvH ? Ctx->TvH : TV_H;
}

static BOOLEAN TvScalingOk(D3DKMDT_VIDPN_PRESENT_PATH_SCALING s)
{
    return ScalingOk(s) || s == D3DKMDT_VPPS_CENTERED || s == D3DKMDT_VPPS_ASPECTRATIOCENTEREDMAX ||
           s == D3DKMDT_VPPS_STRETCHED;
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
    if (!Ctx->FbVa || Ctx->FbDmaUnsafe) return;
    if (Ctx->DoubleBuffer && Ctx->FbFlip.Second) {
        if (SwdFlipLock(&Ctx->FbLock)) {
            SwdFlipClear(Ctx, FALSE);
            SwdFlipUnlock(&Ctx->FbLock);
        } else InterlockedIncrement(&Ctx->F4LockTimeouts);
        return;
    }
    for (y = 0; y < Ctx->FbHeight; y++) {
        RtlZeroMemory(Ctx->FbVa + (SIZE_T)y * Ctx->FbPitch, (SIZE_T)Ctx->FbWidth * 4);
    }
    KeMemoryBarrier();
}

static VOID UnmapFb(PSWD_CONTEXT Ctx)
{
    if (Ctx->DoubleBuffer && Ctx->FbFlip.Second)
        SwdFlipFree(&Ctx->FbFlip, !Ctx->FbDmaUnsafe && SwdPanelPrimary(Ctx));
    if (Ctx->FbMdl != NULL && !Ctx->FbDmaUnsafe) IoFreeMdl(Ctx->FbMdl);
    Ctx->FbMdl = NULL;
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
static VOID BltRect(PSWD_CONTEXT Ctx, PUCHAR Dest, PUCHAR Forward, PUCHAR SrcBase, LONG SrcTop, LONG Pitch, RECT Rc,
                    LONG Ws, LONG Hs, D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rot)
{
    LONG Fw = (LONG)Ctx->FbWidth, Fh = (LONG)Ctx->FbHeight;
    LONG dl, dr, dt, db, dx, dy;
#define SRCPX(x, y) (((const ULONG *)(SrcBase + (SIZE_T)((y) - SrcTop) * (SIZE_T)Pitch))[(x)])
#define DSTROW(y)   ((ULONG *)(Dest + (SIZE_T)(y) * Ctx->FbPitch))

    if (Rc.left < 0) Rc.left = 0;
    if (Rc.top < 0) Rc.top = 0;
    if (Rc.right > Ws) Rc.right = Ws;
    if (Rc.bottom > Hs) Rc.bottom = Hs;
    if (Rc.left >= Rc.right || Rc.top >= Rc.bottom) return;

    switch (Rot) {
    case D3DKMDT_VPPR_ROTATE90:     /* dst(x', y') = (Hs-1-y, x) */
        dl = Hs - Rc.bottom; dr = Hs - Rc.top; dt = Rc.left; db = Rc.right;
        if (dr > Fw) dr = Fw; if (db > Fh) db = Fh;
        if (Forward) {
            SwdCopyRows(Dest, Forward, Ctx->FbPitch, dl, dt, dr, db);
            break;
        }
        for (dy = dt; dy < db; dy++) {
            ULONG *d = DSTROW(dy);
            for (dx = dl; dx < dr; dx++) d[dx] = SRCPX(dy, Hs - 1 - dx);
        }
        break;
    case D3DKMDT_VPPR_ROTATE180:    /* dst = (Ws-1-x, Hs-1-y) */
        dl = Ws - Rc.right; dr = Ws - Rc.left; dt = Hs - Rc.bottom; db = Hs - Rc.top;
        if (dr > Fw) dr = Fw; if (db > Fh) db = Fh;
        if (Forward) {
            SwdCopyRows(Dest, Forward, Ctx->FbPitch, dl, dt, dr, db);
            break;
        }
        for (dy = dt; dy < db; dy++) {
            ULONG *d = DSTROW(dy);
            for (dx = dl; dx < dr; dx++) d[dx] = SRCPX(Ws - 1 - dx, Hs - 1 - dy);
        }
        break;
    case D3DKMDT_VPPR_ROTATE270:    /* dst = (y, Ws-1-x) */
        dl = Rc.top; dr = Rc.bottom; dt = Ws - Rc.right; db = Ws - Rc.left;
        if (dr > Fw) dr = Fw; if (db > Fh) db = Fh;
        if (Forward) {
            SwdCopyRows(Dest, Forward, Ctx->FbPitch, dl, dt, dr, db);
            break;
        }
        for (dy = dt; dy < db; dy++) {
            ULONG *d = DSTROW(dy);
            for (dx = dl; dx < dr; dx++) d[dx] = SRCPX(Ws - 1 - dy, dx);
        }
        break;
    default:
        if (Rc.right > Fw) Rc.right = Fw;
        if (Rc.bottom > Fh) Rc.bottom = Fh;
        if (Rc.left >= Rc.right) break;
        if (Forward) {
            SwdCopyRows(Dest, Forward, Ctx->FbPitch, Rc.left, Rc.top, Rc.right, Rc.bottom);
            break;
        }
        for (dy = Rc.top; dy < Rc.bottom; dy++) {
            RtlCopyMemory(DSTROW(dy) + Rc.left, &SRCPX(Rc.left, dy), (SIZE_T)(Rc.right - Rc.left) * 4);
        }
        break;
    }
#undef SRCPX
#undef DSTROW
}

/* ------------------------------------------------------------ 1.3.0: TV mirror */

/* Window A registers written for the TV, in write order (WIN_OPTIONS last). */
static const ULONG gTvRegs[TV_REGS] = {
    DC_WIN_COLOR_DEPTH, DC_WIN_POSITION, DC_WIN_H_INIT_DDA, DC_WIN_V_INIT_DDA, DC_WIN_PRESCALED,
    DC_WIN_DDA_INC, DC_WIN_SIZE, DC_WIN_LINE_STRIDE, DC_WIN_BUF_CTRL, DC_WIN_DV_CTRL, DC_WINBUF_KIND,
    DC_WINBUF_START, DC_WINBUF_START_HI, DC_WINBUF_H_OFF, DC_WINBUF_V_OFF, DC_WIN_BLEND_1WIN, DC_WIN_OPTIONS
};

#define TVRD(o)    READ_REGISTER_ULONG((PULONG)(Ctx->TvDc + (o)))
#define TVWR(o, v) WRITE_REGISTER_ULONG((PULONG)(Ctx->TvDc + (o)), (v))

/* Writes window A of DC-B (assembly copy) and latches it at the next frame. */
static VOID TvWriteWindow(PSWD_CONTEXT Ctx, const ULONG *Values)
{
    ULONG hdr = TVRD(DC_WINDOW_HEADER), i;
    TVWR(DC_CMD_STATE_ACCESS_REG, 0);
    TVWR(DC_WINDOW_HEADER, DC_WIN_A_SELECT);
    for (i = 0; i < TV_REGS; i++) TVWR(gTvRegs[i], Values[i]);
    TVWR(DC_CMD_STATE_CONTROL_REG, GENERAL_UPDATE | WIN_A_UPDATE);
    TVWR(DC_CMD_STATE_CONTROL_REG, GENERAL_ACT_REQ | WIN_A_ACT_REQ);
    TVWR(DC_WINDOW_HEADER, hdr);
}

/* Copies source rect Rc of a Ws x Hs surface to the TV buffer, upright: Baked
 * is the rotation already in the surface (1.4.3: without Flags.Rotate the DWM
 * hands over the picture in panel orientation, so 180 came out upside down on
 * the TV). The upright picture goes 1:1 when it is 1280x720, otherwise
 * point-sampled to fit, centred. Only rows of Rc are read from the source
 * (the caller locked just those). The inverse mappings follow BltRect. */
static VOID TvBlt(PSWD_CONTEXT Ctx, PUCHAR Dest, PUCHAR Forward, PUCHAR SrcBase, LONG SrcTop, LONG Pitch, RECT Rc, LONG Ws, LONG Hs,
                  D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Baked)
{
    LONG uw, uh, outW, outH, x0, y0, dy, dx, dys, dye, dxs, dxe;
    RECT u;
#define TSRC(x, y) (((const ULONG *)(SrcBase + (SIZE_T)((y) - SrcTop) * (SIZE_T)Pitch))[(x)])
#define TDST(y)    ((ULONG *)(Dest + (SIZE_T)(y) * Ctx->TvPitch))

    if (Rc.left < 0) Rc.left = 0;
    if (Rc.top < 0) Rc.top = 0;
    if (Rc.right > Ws) Rc.right = Ws;
    if (Rc.bottom > Hs) Rc.bottom = Hs;
    if (Rc.left >= Rc.right || Rc.top >= Rc.bottom || Ws <= 0 || Hs <= 0) return;

    /* upright size and the upright rect covered by Rc. 1.4.4: the DWM bakes
     * 90/270 the other way round from BltRect (1.4.3 showed landscape upside
     * down on the TV), so the two inverse mappings are swapped. */
    if (Baked == D3DKMDT_VPPR_ROTATE90) Baked = D3DKMDT_VPPR_ROTATE270;
    else if (Baked == D3DKMDT_VPPR_ROTATE270) Baked = D3DKMDT_VPPR_ROTATE90;
    switch (Baked) {
    case D3DKMDT_VPPR_ROTATE90:     /* upright(a,b) = s(uh-1-b, a) */
        uw = Hs; uh = Ws;
        u.left = Rc.top; u.right = Rc.bottom; u.top = uh - Rc.right; u.bottom = uh - Rc.left;
        break;
    case D3DKMDT_VPPR_ROTATE270:    /* upright(a,b) = s(b, uw-1-a) */
        uw = Hs; uh = Ws;
        u.left = uw - Rc.bottom; u.right = uw - Rc.top; u.top = Rc.left; u.bottom = Rc.right;
        break;
    case D3DKMDT_VPPR_ROTATE180:    /* upright(a,b) = s(Ws-1-a, Hs-1-b) */
        uw = Ws; uh = Hs;
        u.left = Ws - Rc.right; u.right = Ws - Rc.left; u.top = Hs - Rc.bottom; u.bottom = Hs - Rc.top;
        break;
    default:
        Baked = D3DKMDT_VPPR_IDENTITY;
        uw = Ws; uh = Hs; u = Rc;
        break;
    }
#define USRC(a, b) (Baked == D3DKMDT_VPPR_ROTATE90  ? TSRC(uh - 1 - (b), (a)) : \
                    Baked == D3DKMDT_VPPR_ROTATE270 ? TSRC((b), uw - 1 - (a)) : \
                    Baked == D3DKMDT_VPPR_ROTATE180 ? TSRC(Ws - 1 - (a), Hs - 1 - (b)) : TSRC((a), (b)))

    if (uw == (LONG)Ctx->TvW && uh == (LONG)Ctx->TvH) {
        if (Forward) {
            SwdCopyRows(Dest, Forward, Ctx->TvPitch, u.left, u.top, u.right, u.bottom);
            return;
        }
        for (dy = u.top; dy < u.bottom; dy++) {
            ULONG *d = TDST(dy);
            if (Baked == D3DKMDT_VPPR_IDENTITY) {
                RtlCopyMemory(d + u.left, &TSRC(u.left, dy), (SIZE_T)(u.right - u.left) * 4);
            } else {
                for (dx = u.left; dx < u.right; dx++) d[dx] = USRC(dx, dy);
            }
        }
        return;
    }
    outH = (LONG)Ctx->TvH;
    outW = uw * (LONG)Ctx->TvH / uh;
    if (outW > (LONG)Ctx->TvW) { outW = (LONG)Ctx->TvW; outH = uh * (LONG)Ctx->TvW / uw; }
    if (outW <= 0 || outH <= 0) return;
    x0 = ((LONG)Ctx->TvW - outW) / 2;
    y0 = ((LONG)Ctx->TvH - outH) / 2;
    /* destination pixels whose sample falls inside u */
    dys = y0 + (u.top * outH + uh - 1) / uh;
    dye = y0 + (u.bottom * outH + uh - 1) / uh;
    dxs = x0 + (u.left * outW + uw - 1) / uw;
    dxe = x0 + (u.right * outW + uw - 1) / uw;
    if (dye > y0 + outH) dye = y0 + outH;
    if (dxe > x0 + outW) dxe = x0 + outW;
    if (Forward) {
        SwdCopyRows(Dest, Forward, Ctx->TvPitch, dxs, dys, dxe, dye);
        return;
    }
    for (dy = dys; dy < dye; dy++) {
        LONG sb = (dy - y0) * uh / outH;
        ULONG *d = TDST(dy);
        for (dx = dxs; dx < dxe; dx++) d[dx] = USRC((dx - x0) * uw / outW, sb);
    }
#undef USRC
#undef TSRC
#undef TDST
}

static VOID TvClear(PSWD_CONTEXT Ctx)
{
    if (Ctx->TvDmaUnsafe) return;
    ULONG y;
    for (y = 0; y < Ctx->TvH; y++) RtlZeroMemory(Ctx->TvVa + (SIZE_T)y * Ctx->TvPitch, Ctx->TvPitch);
}

/* StartDevice: take DC-B over if UEFI left it running for a TV. */
static VOID TvStartLocked(PSWD_CONTEXT Ctx);
static VOID TvAudioUp(ULONG Act);
static VOID TvLock(PSWD_CONTEXT Ctx)   { KeWaitForSingleObject(&Ctx->TvLock, Executive, KernelMode, FALSE, NULL); }
static VOID TvUnlock(PSWD_CONTEXT Ctx) { KeSetEvent(&Ctx->TvLock, IO_NO_INCREMENT, FALSE); }

static VOID TvStart(PSWD_CONTEXT Ctx)
{
    if (Ctx->DoubleBuffer) {
        if (!SwdFlipLock(&Ctx->TvLock)) { InterlockedIncrement(&Ctx->F4LockTimeouts); return; }
    } else TvLock(Ctx);
    if (!Ctx->TvVa) TvStartLocked(Ctx);
    TvUnlock(Ctx);
}

static VOID TvStartLocked(PSWD_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS pa, lo, hi, skip;
    PUCHAR car;
    ULONG rst, clk, cmd, wopt, act, i;
    ULONG v[TV_REGS];

    if (RegGetDword(L"TvMirror", 1) == 0) { RegPutDword(L"TvState", 1); return; }
    pa.QuadPart = (LONGLONG)CAR_PHYS;
    car = (PUCHAR)MmMapIoSpaceEx(pa, CAR_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!car) { RegPutDword(L"TvState", 2); return; }
    rst = READ_REGISTER_ULONG((PULONG)(car + CAR_RST_DEVICES_L));
    clk = READ_REGISTER_ULONG((PULONG)(car + CAR_CLK_OUT_ENB_L));
    MmUnmapIoSpace(car, CAR_MAP_SIZE);
    RegPutDword(L"TvCarRst", rst);
    RegPutDword(L"TvCarClk", clk);
    if ((clk & CAR_DISP2_BIT) == 0 || (rst & CAR_DISP2_BIT) != 0) { RegPutDword(L"TvState", 3); return; }

    pa.QuadPart = (LONGLONG)DC_B_PHYS;
    Ctx->TvDc = (PUCHAR)MmMapIoSpaceEx(pa, DC_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Ctx->TvDc) { RegPutDword(L"TvState", 2); return; }
    cmd = TVRD(DC_CMD_DISPLAY_COMMAND_REG);
    wopt = TVRD(DC_DISP_WIN_OPTIONS);
    act = TVRD(DC_DISP_ACTIVE);
    RegPutDword(L"TvDispCmd", cmd);
    RegPutDword(L"TvDispWinOpt", wopt);
    RegPutDword(L"TvActive", act);
    if ((cmd & DISP_CTRL_MODE_C_DISPLAY) == 0 || (wopt & DC_SOR1_ENABLE) == 0 ||
        (act != TV_ACT_720 && act != TV_ACT_1080)) {
        MmUnmapIoSpace(Ctx->TvDc, DC_MAP_SIZE);
        Ctx->TvDc = NULL;
        RegPutDword(L"TvState", 4);
        return;
    }

    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFFll;     /* START_HI stays 0 */
    skip.QuadPart = 0;
    /* 1.7.0: the buffer has the raster's size (the TV is a monitor of its
     * own now); 1280x720 scaled by the window if that much is not free. */
    Ctx->TvW = act & 0xFFFFu;
    Ctx->TvH = act >> 16;
    if (RegGetDword(L"TvNative", 1) == 0) { Ctx->TvW = TV_W; Ctx->TvH = TV_H; }
    Ctx->TvPitch = Ctx->TvW * 4u;
    Ctx->TvSize = (SIZE_T)Ctx->TvPitch * Ctx->TvH;
    Ctx->TvVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(Ctx->TvSize, lo, hi, skip, MmWriteCombined);
    if (!Ctx->TvVa && Ctx->TvW != TV_W) {
        Ctx->TvW = TV_W; Ctx->TvH = TV_H; Ctx->TvPitch = TV_PITCH;
        Ctx->TvSize = (SIZE_T)TV_PITCH * TV_H;
        Ctx->TvVa = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(Ctx->TvSize, lo, hi, skip, MmWriteCombined);
    }
    if (!Ctx->TvVa) {
        MmUnmapIoSpace(Ctx->TvDc, DC_MAP_SIZE);
        Ctx->TvDc = NULL;
        RegPutDword(L"TvState", 5);
        return;
    }
    Ctx->TvPhys = MmGetPhysicalAddress(Ctx->TvVa);
    if (Ctx->DoubleBuffer) Ctx->TvMdl = SwdOwnedScanoutMdl(Ctx->TvVa, Ctx->TvSize);
    else if (Ctx->GpuPresent && !gGpuDisabled)
        Ctx->TvMdl = SwdGpuScanoutMdl(Ctx->TvVa, Ctx->TvPhys, Ctx->TvSize);
    TvClear(Ctx);
    /* Until the desktop draws: the boot image, as UEFI showed it. */
    if (Ctx->FbVa) {
        RECT all;
        all.left = 0; all.top = 0; all.right = (LONG)Ctx->FbWidth; all.bottom = (LONG)Ctx->FbHeight;
        if (!Ctx->DoubleBuffer || SwdFlipLock(&Ctx->FbLock)) {
            PUCHAR panel = SwdFlipVa(Ctx, FALSE, Ctx->FbFlip.Front);
            if (!Ctx->DoubleBuffer || !Ctx->FbDmaUnsafe)
                TvBlt(Ctx, Ctx->TvVa, NULL, panel, 0, (LONG)Ctx->FbPitch, all, (LONG)Ctx->FbWidth, (LONG)Ctx->FbHeight, D3DKMDT_VPPR_IDENTITY);
            if (Ctx->DoubleBuffer) SwdFlipUnlock(&Ctx->FbLock);
        } else InterlockedIncrement(&Ctx->F4LockTimeouts);
    }
    Ctx->TvLastW = 0;
    Ctx->TvLastH = 0;
    KeMemoryBarrier();

    {
        ULONG hdr = TVRD(DC_WINDOW_HEADER);
        TVWR(DC_CMD_STATE_ACCESS_REG, 0);
        TVWR(DC_WINDOW_HEADER, DC_WIN_A_SELECT);
        for (i = 0; i < TV_REGS; i++) Ctx->TvSaved[i] = TVRD(gTvRegs[i]);
        TVWR(DC_WINDOW_HEADER, hdr);
    }
    v[0] = DC_DEPTH_B8G8R8A8;               /* same as the boot framebuffer on DC-A */
    v[1] = 0;                               /* position 0,0 */
    v[2] = 0;
    v[3] = 0;
    v[4] = (Ctx->TvH << 16) | Ctx->TvPitch; /* prescaled: H in bytes */
    /* 1.5.0: window = the DC-B raster; DDA as Linux tegra compute_dda_inc,
     * (in - 1) / (out - 1) in 4.12 (1.0 when the raster is 1280x720) */
    {
        ULONG ow = act & 0xFFFFu, oh = act >> 16;
        v[5] = ((((Ctx->TvH - 1u) * 0x1000u) / (oh - 1u)) << 16) | (((Ctx->TvW - 1u) * 0x1000u) / (ow - 1u));
        v[6] = act;
    }
    v[7] = Ctx->TvPitch;
    v[8] = 0;
    v[9] = 0;
    v[10] = 0;                              /* pitch linear */
    v[11] = Ctx->TvPhys.LowPart;
    v[12] = 0;
    v[13] = 0;
    v[14] = 0;
    v[15] = (1u << 24) | 255u;              /* opaque */
    v[16] = DC_WIN_ENABLE;
    if (Ctx->TvW != (act & 0xFFFFu) && RegGetDword(L"TvFilter", 0) != 0) v[16] |= DC_WIN_H_FILTER | DC_WIN_V_FILTER;
    TvWriteWindow(Ctx, v);
    if (Ctx->DoubleBuffer) SwdFlipAllocate(Ctx, TRUE);
    RegPutDword(L"TvSavedStart", Ctx->TvSaved[11]);
    RegPutDword(L"TvBufLo", Ctx->TvPhys.LowPart);
    RegPutDword(L"TvBufHi", (ULONG)Ctx->TvPhys.HighPart);
    RegPutDword(L"TvBufW", Ctx->TvW);
    RegPutDword(L"TvBufH", Ctx->TvH);
    RegPutDword(L"TvState", 6);
    InterlockedExchange(&Ctx->TvConnected, 1);
    TvAudioUp(act);
}

/* Stop: give window A back to what UEFI set (the boot framebuffer), wait for
 * DC-B to latch it, then free the buffer. */
static BOOLEAN TvStop(PSWD_CONTEXT Ctx)
{
    LARGE_INTEGER t;
    PUCHAR va;
    if (Ctx->DoubleBuffer) {
        BOOLEAN safe = Ctx->TvVa == NULL;
        SWD_FLIP *flip = &Ctx->TvFlip;
        if (!SwdFlipLock(&Ctx->TvLock)) {
            InterlockedIncrement(&Ctx->F4LockTimeouts);
            return FALSE;
        }
        if (Ctx->TvDc && Ctx->TvVa) {
            ULONGLONG saved = ((ULONGLONG)Ctx->TvSaved[12] << 32) | Ctx->TvSaved[11];
            if (flip->Second && !Ctx->TvDmaUnsafe) (VOID)SwdFlipRequest(Ctx, TRUE, 0);
            TvWriteWindow(Ctx, Ctx->TvSaved);
            /* Confirm that neither owned buffer can still be scanned. */
            safe = SwdFlipWait(Ctx->TvDc, saved, flip) && !Ctx->TvDmaUnsafe &&
                saved != (ULONGLONG)Ctx->TvPhys.QuadPart &&
                (!flip->Second || saved != (ULONGLONG)flip->Phys.QuadPart);
        }
        SwdFlipFree(flip, safe);
        if (safe) {
            if (Ctx->TvMdl) IoFreeMdl(Ctx->TvMdl);
            if (Ctx->TvVa) MmFreeContiguousMemorySpecifyCache(Ctx->TvVa, Ctx->TvSize, MmWriteCombined);
        }
        Ctx->TvVa = NULL;
        Ctx->TvMdl = NULL;
        Ctx->TvDmaUnsafe = FALSE;
        InterlockedExchange(&Ctx->TvConnected, 0);
        if (Ctx->TvDc) {
            MmUnmapIoSpace(Ctx->TvDc, DC_MAP_SIZE);
            Ctx->TvDc = NULL;
        }
        SwdFlipUnlock(&Ctx->TvLock);
        return TRUE;
    }
    TvLock(Ctx);                            /* Present no longer copies to the TV after this */
    va = Ctx->TvVa;
    Ctx->TvVa = NULL;
    if (Ctx->TvMdl != NULL && !Ctx->TvDmaUnsafe) IoFreeMdl(Ctx->TvMdl);
    Ctx->TvMdl = NULL;
    InterlockedExchange(&Ctx->TvConnected, 0);
    TvUnlock(Ctx);
    if (Ctx->TvDc && va) {
        TvWriteWindow(Ctx, Ctx->TvSaved);
        t.QuadPart = -50ll * 10 * 1000;     /* 3 frames */
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    }
    if (va) {
        if (!Ctx->TvDmaUnsafe)
            MmFreeContiguousMemorySpecifyCache(va, Ctx->TvSize, MmWriteCombined);
    }
    Ctx->TvDmaUnsafe = FALSE;
    if (Ctx->TvDc) {
        MmUnmapIoSpace(Ctx->TvDc, DC_MAP_SIZE);
        Ctx->TvDc = NULL;
    }
    return TRUE;
}

#undef TVRD
#undef TVWR

/* ------------------------------------------------------------ 1.4.0: DP Alt hot-plug */

/* UEFI types and helpers for dpalt_port.inc (generated from the UEFI source). */
typedef SIZE_T UINTN;
typedef WCHAR  CHAR16;
#define STATIC static
#define CONST  const
static PUCHAR gDpCar, gDpPmc, gDpAux, gDpSor, gDpDcb;
static WCHAR  gDpLog[12288];
static SIZE_T gDpLogLen;

static VOID DpStall(UINTN Us)
{
    if (Us >= 1000) {
        LARGE_INTEGER t;
        t.QuadPart = -(LONGLONG)Us * 10;
        KeDelayExecutionThread(KernelMode, FALSE, &t);
    } else {
        KeStallExecutionProcessor((ULONG)Us);
    }
}

static VOID DpLog(PCWSTR Fmt, ...)
{
    WCHAR line[512];
    va_list ap;
    SIZE_T n = 0;
    va_start(ap, Fmt);
    (VOID)RtlStringCbVPrintfW(line, sizeof(line), Fmt, ap);
    va_end(ap);
    (VOID)RtlStringCchLengthW(line, RTL_NUMBER_OF(line), &n);
    if (gDpLogLen + n + 1 < RTL_NUMBER_OF(gDpLog)) {
        RtlCopyMemory(&gDpLog[gDpLogLen], line, n * sizeof(WCHAR));
        gDpLogLen += n;
        gDpLog[gDpLogLen] = 0;
    }
}

#define CompareMem(a, b, n) (RtlCompareMemory((a), (b), (n)) == (SIZE_T)(n) ? 0 : 1)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-but-set-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#include "dpalt_port.inc"
#pragma clang diagnostic pop

#define CAR_PHYS_PAGE   0x60006000ull
#define PMC_PHYS_PAGE   0x7000E000ull       /* PMC registers start at +0x400 */
#define DPAUX1_PHYS     0x54040000ull
#define SOR1_PHYS       0x54580000ull

static VOID DpUnmap(VOID)
{
    if (gDpCar) { MmUnmapIoSpace(gDpCar, 0x1000); gDpCar = NULL; }
    if (gDpPmc) { MmUnmapIoSpace(gDpPmc - 0x400, 0x1000); gDpPmc = NULL; }
    if (gDpAux) { MmUnmapIoSpace(gDpAux, 0x1000); gDpAux = NULL; }
    if (gDpSor) { MmUnmapIoSpace(gDpSor, 0x1000); gDpSor = NULL; }
    if (gDpDcb) { MmUnmapIoSpace(gDpDcb, DC_MAP_SIZE); gDpDcb = NULL; }
}

static BOOLEAN DpMap(VOID)
{
    PHYSICAL_ADDRESS pa;
    PUCHAR pmc;
    if (gDpCar && gDpPmc && gDpAux && gDpSor && gDpDcb) return TRUE;
    DpUnmap();
    pa.QuadPart = (LONGLONG)CAR_PHYS_PAGE; gDpCar = (PUCHAR)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)PMC_PHYS_PAGE; pmc = (PUCHAR)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    gDpPmc = pmc ? pmc + 0x400 : NULL;
    pa.QuadPart = (LONGLONG)DPAUX1_PHYS;  gDpAux = (PUCHAR)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)SOR1_PHYS;    gDpSor = (PUCHAR)MmMapIoSpaceEx(pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    pa.QuadPart = (LONGLONG)DC_B_PHYS;    gDpDcb = (PUCHAR)MmMapIoSpaceEx(pa, DC_MAP_SIZE, PAGE_READWRITE | PAGE_NOCACHE);
    if (gDpCar && gDpPmc && gDpAux && gDpSor && gDpDcb) return TRUE;
    DpUnmap();
    return FALSE;
}

static VOID DpLogFlush(VOID)
{
    if (gParamPath.Buffer) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, gParamPath.Buffer, L"TvLog", REG_SZ, gDpLog,
                                    (ULONG)((gDpLogLen + 1) * sizeof(WCHAR)));
    }
}

/* ------------------------------------------------------------ 1.6.0: TV audio */

/* SOR registers (word offsets, L4T sor_regs.h). */
#define SOR_W_CLK_CNTRL         0x13u
#define SOR_W_LINKCTL0          0x4Cu
#define SOR_W_DP_AUDIO_CTRL     0x62u
#define SOR_W_DP_AUDIO_HBLANK   0x63u
#define SOR_W_DP_AUDIO_VBLANK   0x64u
#define SOR_W_AUDIO_CTRL        0xFCu
#define SOR_W_AUDIO_SPARE0      0xFEu
#define SOR_W_ELD_BUFWR         0x10Cu
#define SOR_W_HDA_PRESENCE      0x10Du
#define SOR_W_AUDIO_GEN_CTRL    0x117u
#define SOR_W_DP_CH_STATUS2     0x13Bu
#define SOR_AUDIO_CTRL_HDA      ((2u << 20) | (1u << 12))   /* source HDA, AFIFO flush */
#define SOR_DP_AUDIO_ON         ((1u << 31) | (0x1Fu << 16) | 1u) /* new settings, CA/SS/SF/CC/CT from HW, enable */

/* CAR: the HDA clocks (Linux tegra210: hda 51 MHz, hda2codec_2x 48 MHz from
 * pll_p; L4T DP: maud 102 MHz from pll_p; hda2hdmi is a gate). */
#define CARW(o)                 (((volatile ULONG *)gDpCar)[(o) / 4])
#define CAR_X_SOR1              (1u << 23)          /* CLK_OUT_ENB_X 0x280, RST_DEVICES_X 0x28C */
#define CAR_V_HDA               (1u << 29)
#define CAR_V_HDA2CODEC         (1u << 15)
#define CAR_W_HDA2HDMI          (1u << 0)
#define CAR_Y_MAUD              (1u << 10)
#define PMC_PG_SOR              (1u << 17)          /* PWRGATE_STATUS (PMC + 0x38) */

/* Shared with switchaudio 1.1.0 (same steps, same values): turn on, never off. */
static ULONG HdaClocksOn(VOID)
{
    ULONG before = (CARW(0x360) & (CAR_V_HDA | CAR_V_HDA2CODEC)) | (CARW(0x364) & CAR_W_HDA2HDMI) |
                   ((CARW(0x298) & CAR_Y_MAUD) ? 0x100u : 0u);
    if ((CARW(0x298) & CAR_Y_MAUD) == 0) {
        CARW(0x6D4) = 6u;                           /* maud: pll_p / 4 = 102 MHz */
        CARW(0x29C) = CAR_Y_MAUD;
    }
    if ((CARW(0x360) & CAR_V_HDA) == 0 || (CARW(0x358) & CAR_V_HDA) != 0) {
        CARW(0x428) = 14u;                          /* hda: pll_p / 8 = 51 MHz */
        CARW(0x3E4) = 15u;                          /* hda2codec_2x: pll_p / 8.5 = 48 MHz */
        CARW(0x440) = CAR_V_HDA | CAR_V_HDA2CODEC;
        CARW(0x448) = CAR_W_HDA2HDMI;
        KeStallExecutionProcessor(10);
        CARW(0x434) = CAR_V_HDA | CAR_V_HDA2CODEC;  /* RST_DEVICES_CLR_V */
        CARW(0x43C) = CAR_W_HDA2HDMI;               /* RST_DEVICES_CLR_W */
        KeStallExecutionProcessor(10);
    }
    return before;
}

/* A fixed ELD (L4T hda_dc.c layout): version 2, DP, "TV", one SAD = LPCM 2 ch
 * 48 kHz 16 bit. Windows never sees it (switchaudio has its own formats). */
static const UCHAR gTvEld[25] = {
    0x10, 0x00, 0x06, 0x00,                         /* ELD v2, baseline 21 bytes = 6 dwords */
    0x62,                                           /* CEA-861-D, monitor name 2 bytes */
    0x14,                                           /* 1 SAD, conn_type DP */
    0x00, 0x01,                                     /* sync delay, speakers FL/FR */
    0, 0, 0, 0, 0, 0, 0, 0,                         /* port id */
    0, 0, 0, 0,                                     /* manufacturer, product */
    'T', 'V',
    0x09, 0x04, 0x01,                               /* LPCM 2 ch, 48 kHz, 16 bit */
};

/* Called at the end of TvStartLocked (picture on). */
static VOID TvAudioUp(ULONG Act)
{
    volatile ULONG *sor;
    ULONG code, lanes, ef, pclk, hblank, hact, i, before;
    LONGLONG hs, vs;

    if (RegGetDword(L"TvAudio", 1) == 0) { RegPutDword(L"TvAudioState", 1); return; }
    if (!DpMap()) { RegPutDword(L"TvAudioState", 2); return; }
    if ((CARW(0x280) & CAR_X_SOR1) == 0 || (CARW(0x28C) & CAR_X_SOR1) != 0) { RegPutDword(L"TvAudioState", 3); return; }
    if ((((volatile ULONG *)gDpPmc)[0x38 / 4] & PMC_PG_SOR) == 0) { RegPutDword(L"TvAudioState", 4); return; }
    before = HdaClocksOn();
    RegPutDword(L"TvAudioClocks", before);
    sor = (volatile ULONG *)gDpSor;

    code = (sor[SOR_W_CLK_CNTRL] >> 2) & 0x1Fu;     /* 0x0A = 2.7 Gbps: link clock code * 27 MHz */
    i = (sor[SOR_W_LINKCTL0] >> 16) & 0x1Fu;        /* lane mask */
    for (lanes = 0; i; i >>= 1) lanes += i & 1u;
    ef = (sor[SOR_W_LINKCTL0] >> 14) & 1u;          /* enhanced framing */
    if (Act == TV_ACT_1080) { pclk = 148500; hblank = 280; hact = 1920; }
    else                    { pclk = 74250;  hblank = 370; hact = 1280; }
    if (code == 0 || lanes == 0) { RegPutDword(L"TvAudioState", 5); return; }
    /* L4T dp.c tegra_dc_dp_calc_config: link clock / pixel clock in kHz. */
    hs = (LONGLONG)(hblank - 7u) * (code * 27000u) / pclk - 3 * (LONGLONG)ef - (LONGLONG)(12u / lanes);
    vs = (LONGLONG)(hact - 25u) * (code * 27000u) / pclk - (LONGLONG)(36u / lanes) - 4;
    if (hs < 0) hs = 0;
    if (vs < 0) vs = 0;

    sor[SOR_W_HDA_PRESENCE] = 0;                    /* presence off while the ELD is written */
    sor[SOR_W_AUDIO_CTRL] = SOR_AUDIO_CTRL_HDA;
    sor[SOR_W_AUDIO_SPARE0] |= 1u << 27;            /* advertise HBR */
    sor[SOR_W_DP_AUDIO_HBLANK] = (sor[SOR_W_DP_AUDIO_HBLANK] & ~0x1FFFFu) | ((ULONG)hs & 0x1FFFFu);
    sor[SOR_W_DP_AUDIO_VBLANK] = (sor[SOR_W_DP_AUDIO_VBLANK] & ~0x1FFFFu) | ((ULONG)vs & 0x1FFFFu);
    sor[SOR_W_DP_AUDIO_CTRL] = SOR_DP_AUDIO_ON;
    sor[SOR_W_DP_CH_STATUS2] &= ~(1u << 31);        /* no channel status override */
    for (i = 0; i < 96; i++) sor[SOR_W_ELD_BUFWR] = (i << 8) | (i < sizeof(gTvEld) ? gTvEld[i] : 0u);
    sor[SOR_W_HDA_PRESENCE] = 3u;                   /* ELDV + PD */
    RegPutDword(L"TvAudioPresence", sor[SOR_W_HDA_PRESENCE]);
    RegPutDword(L"TvAudioUps", RegGetDword(L"TvAudioUps", 0) + 1);

    RegPutDword(L"TvAudioLink", (code << 8) | (lanes << 4) | ef);
    RegPutDword(L"TvAudioSym", ((ULONG)hs << 16) | ((ULONG)vs & 0xFFFFu));
    RegPutDword(L"TvAudioDevId", sor[SOR_W_AUDIO_GEN_CTRL] >> 16);
    RegPutDword(L"TvAudioCtrl", sor[SOR_W_DP_AUDIO_CTRL]);
    RegPutDword(L"TvAudioState", 6);
}

/* Unplug: presence off first, so switchaudio's jack goes to unplugged. */
static VOID TvAudioDown(VOID)
{
    if (!DpMap()) return;
    if ((CARW(0x280) & CAR_X_SOR1) == 0 || (CARW(0x28C) & CAR_X_SOR1) != 0) return;
    ((volatile ULONG *)gDpSor)[SOR_W_HDA_PRESENCE] = 0;
    ((volatile ULONG *)gDpSor)[SOR_W_DP_AUDIO_CTRL] = 0;
    RegPutDword(L"TvAudioState", 7);
}

/* 1.7.0: tell Windows the TV monitor (child 1) came or went. */
static VOID TvIndicate(PSWD_CONTEXT Ctx, BOOLEAN Connected)
{
    DXGK_CHILD_STATUS cs;
    NTSTATUS st;
    if (!Ctx->Extend || !Ctx->Started || !Ctx->Dxgk.DxgkCbIndicateChildStatus) return;
    RtlZeroMemory(&cs, sizeof(cs));
    cs.Type = StatusConnection;
    cs.ChildUid = 1;
    cs.HotPlug.Connected = Connected;
    st = Ctx->Dxgk.DxgkCbIndicateChildStatus(Ctx->Dxgk.DeviceHandle, &cs);
    InterlockedIncrement(&Ctx->TvIndicates);
    InterlockedExchange(&Ctx->TvIndicateSt, (LONG)st);
}

/* Unplugged: give the window back, stop DC-B, hold SOR1 and DC-B in reset
 * (the next bring-up starts both from reset, as in UEFI). */
static VOID DpTearDown(PSWD_CONTEXT Ctx)
{
    volatile ULONG *car;
    BOOLEAN was = Ctx->TvConnected != 0;
    TvAudioDown();
    if (!TvStop(Ctx)) return;               /* present still owns DC-B */
    if (was) TvIndicate(Ctx, FALSE);
    if (!DpMap()) return;
    car = (volatile ULONG *)gDpCar;
    if ((car[0x10 / 4] & (1u << 26)) != 0 && (car[0x04 / 4] & (1u << 26)) == 0) {
        volatile ULONG *dc = (volatile ULONG *)gDpDcb;
        dc[0x40] = 0;                                   /* STATE_ACCESS: assembly */
        dc[0x32] = DISP_CTRL_MODE_STOP;                 /* DISP_CMD */
        dc[0x41] = GENERAL_UPDATE;
        dc[0x41] = GENERAL_ACT_REQ;
        DpStall(40000);
    }
    car[0x290 / 4] = 1u << 23;                          /* RST_DEV_X_SET: SOR1 */
    car[0x300 / 4] = 1u << 26;                          /* RST_DEV_L_SET: disp2 */
    InterlockedIncrement(&Ctx->DpDowns);
    RegPutDword(L"DpDowns", (ULONG)Ctx->DpDowns);
}

/* Plugged in (switchbattery entered DP Alt Mode): the UEFI R426/R427/R429 steps. */
static VOID DpBringUp(PSWD_CONTEXT Ctx)
{
    ULONG step = 0;
    gDpLogLen = 0;
    gDpLog[0] = 0;
    mR426Ok = FALSE; mR427Ok = FALSE; mR429Ok = FALSE;
    mR426Step = NULL; mR427Step = NULL; mR429Step = NULL;
    mR426Hpd = FALSE; mR426DpcdOk = FALSE; mR426EdidOk = FALSE;
    DpLog(L"switchdisplay 1.7.0 DP Alt bring-up (attempt %u)\r\n", Ctx->DpFails + 1);
    if (!DpMap()) {
        DpLog(L"register mapping failed\r\n");
    } else {
        /* 1.4.1: the stage and the log so far go to the registry before each
         * stage, so a stage that never returns still shows where it is. */
        ULONGLONG t0 = KeQueryInterruptTime();
#define DP_STAGE(n) do { step = (n); DpLog(L"--- stage %u at %u ms\r\n", (ULONG)(n), \
        (ULONG)((KeQueryInterruptTime() - t0) / 10000)); DpLogFlush(); RegPutDword(L"DpLastStep", 0x100u | (n)); } while (0)
        DP_STAGE(1);
        R426DpAuxStage2a();
        if (mR426Ok) { DP_STAGE(2); R427DpLinkStage2b(); }
        if (mR427Ok) { DP_STAGE(3); R429DpVideoStage3(); }
        if (mR429Ok) { DP_STAGE(4); TvStart(Ctx); if (Ctx->TvVa) step = 5; }
        DpLog(L"--- done at %u ms\r\n", (ULONG)((KeQueryInterruptTime() - t0) / 10000));
#undef DP_STAGE
    }
    DpLog(L"result: step %u (1 DPAUX/HPD, 2 link, 3 DC-B, 4 window, 5 TV on)\r\n", step);
    DpLogFlush();
    RegPutDword(L"DpLastStep", step);
    if (step == 5) {
        TvIndicate(Ctx, TRUE);
        Ctx->DpFails = 0;
        InterlockedIncrement(&Ctx->DpUps);
        RegPutDword(L"DpUps", (ULONG)Ctx->DpUps);
    } else {
        Ctx->DpFails++;
        RegPutDword(L"DpFails", Ctx->DpFails);
    }
}

/* Thread side: act on the last state switchbattery sent. */
static VOID DpHandle(PSWD_CONTEXT Ctx)
{
    if (Ctx->DpWant) {
        if (!Ctx->TvVa && Ctx->DpFails < 3) DpBringUp(Ctx);
    } else {
        if (Ctx->TvVa || (gDpCar && (((PULONG)gDpCar)[0x04 / 4] & (1u << 26)) == 0)) DpTearDown(Ctx);
        Ctx->DpFails = 0;
    }
}

/* \Callback\SwitchWoaDpAlt from switchbattery: Argument1 = 1 up / 0 down. */
static VOID DpCallback(PVOID Context, PVOID Argument1, PVOID Argument2)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)Context;
    UNREFERENCED_PARAMETER(Argument2);
    InterlockedIncrement(&ctx->DpNotes);
    InterlockedExchange(&ctx->DpWant, Argument1 ? 1 : 0);
    KeSetEvent(&ctx->DpEvent, IO_NO_INCREMENT, FALSE);
}

static VOID DpRegister(PSWD_CONTEXT Ctx)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    NTSTATUS st;
    if (RegGetDword(L"TvHotplug", 1) == 0) { RegPutDword(L"DpRegister", 1); return; }
    RtlInitUnicodeString(&name, L"\\Callback\\SwitchWoaDpAlt");
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_PERMANENT | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ExCreateCallback(&Ctx->DpCbObject, &oa, TRUE, TRUE);
    RegPutDword(L"DpRegister", (ULONG)st);
    if (!NT_SUCCESS(st)) { Ctx->DpCbObject = NULL; return; }
    Ctx->DpCbHandle = ExRegisterCallback(Ctx->DpCbObject, DpCallback, Ctx);
    if (!Ctx->DpCbHandle) {
        ObDereferenceObject(Ctx->DpCbObject);
        Ctx->DpCbObject = NULL;
        RegPutDword(L"DpRegister", 2);
    }
}

static VOID DpUnregister(PSWD_CONTEXT Ctx)
{
    if (Ctx->DpCbHandle) { ExUnregisterCallback(Ctx->DpCbHandle); Ctx->DpCbHandle = NULL; }
    if (Ctx->DpCbObject) { ObDereferenceObject(Ctx->DpCbObject); Ctx->DpCbObject = NULL; }
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
    ctx->Visible[0] = TRUE;
    ctx->Visible[1] = TRUE;
    KeInitializeEvent(&ctx->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&ctx->DpEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&ctx->TvLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&ctx->FbLock, SynchronizationEvent, TRUE);
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
    RegPutDword(L"TvAudioState", 0);        /* 1.6.1: switchaudio reads it (6 = TV audio on) */
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
        /* 1.6.2: refuse this start only. Before, the guard never cleared
         * itself (the clear needs a start that runs 60 s), so after a few
         * quick restarts the driver stayed off for good. */
        RegPutDword(L"StartPending", 0);
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

    ctx->DoubleBuffer = RegGetDword(L"DoubleBuffer", 0u) == 1u;
    ctx->GpuPresent = RegGetDword(L"GpuPresent", 0u) == 1u;
    if ((ctx->DoubleBuffer || (ctx->GpuPresent && !gGpuDisabled)) && SwdPanelMdlAllowed(ctx)) {
        ctx->FbMdl = SwdGpuScanoutMdl(ctx->FbVa, ctx->FbPhys, ctx->FbMapSize);
    }
    if (ctx->DoubleBuffer) {
        SwdFlipAllocate(ctx, FALSE);
        SwdFlipFlush(ctx);
    }
    ctx->Extend = RegGetDword(L"TvExtend", 1) != 0;
    RegPutDword(L"ExtendOn", ctx->Extend ? 1 : 0);
    ctx->SrcW[0] = ctx->SrcW[1] = ctx->FbWidth;
    ctx->SrcH[0] = ctx->SrcH[1] = ctx->FbHeight;
    ctx->PanelSrc = 0;
    ctx->TvSrc = ctx->Extend ? SWD_NOSRC : 0;  /* the TV keeps the boot image until Windows uses it */
    ctx->Rotation = D3DKMDT_VPPR_IDENTITY;
    ctx->Active = TRUE;      /* the boot image stays until the first commit */
    ctx->Visible[0] = TRUE;
    ctx->Visible[1] = TRUE;
    TvStart(ctx);

    KeClearEvent(&ctx->StopEvent);
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    st = PsCreateSystemThread(&ctx->ThreadHandle, THREAD_ALL_ACCESS, &oa, NULL, NULL, GuardThread, ctx);
    if (NT_SUCCESS(st)) {
        st = ObReferenceObjectByHandle(ctx->ThreadHandle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&ctx->Thread, NULL);
        if (!NT_SUCCESS(st)) ctx->Thread = NULL;
    } else {
        ctx->ThreadHandle = NULL;
    }

    DpRegister(ctx);                         /* 1.4.0: after the thread exists */

    *NumberOfVideoPresentSources = ctx->Extend ? 2 : 1;
    *NumberOfChildren = ctx->Extend ? 2 : 1;
    ctx->Started = TRUE;
    RegPutDword(L"F4Active", ctx->DoubleBuffer);
    RegPutDword(L"LastStartStatus", 0);
    UNREFERENCED_PARAMETER(DxgkStartInfo);
    return STATUS_SUCCESS;
}

static VOID VsyncSet(PSWD_CONTEXT Ctx, BOOLEAN On);

static VOID StopCommon(PSWD_CONTEXT Ctx)
{
    VsyncSet(Ctx, FALSE);
    DpUnregister(Ctx);
    StopGuardThread(Ctx);
    FlushCounters(Ctx);
    if (Ctx->GpuPresent) SwdGpuDisconnect(Ctx);
    TvStop(Ctx);
    DpUnmap();
    UnmapFb(Ctx);
    Ctx->Started = FALSE;
    if (Ctx->DoubleBuffer) SwdFlipFlush(Ctx);
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
    if (ctx->DoubleBuffer && ctx->FbFlip.Second && !SwdPanelPrimary(ctx))
        return STATUS_IO_TIMEOUT;
    if (ctx->DoubleBuffer && ctx->FbFlip.Second) {
        ctx->FbFlip.Enabled = FALSE;
        ctx->FbFlip.Front = 0;
    }
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
        DpUnregister(ctx);
        StopGuardThread(ctx);
        if (ctx->GpuPresent) SwdGpuDisconnect(ctx);
        TvStop(ctx);
        DpUnmap();
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
        if (ctx->Extend && ctx->TvSrc != SWD_NOSRC && ctx->TvConnected) {
            /* 1.7.0: the TV has no interrupt of its own here; DC-B runs at
             * 60 Hz too, so the panel's vblank paces it. */
            RtlZeroMemory(&d, sizeof(d));
            d.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_VSYNC;
            d.DisplayOnlyVsync.VidPnTargetId = 1;
            ctx->Dxgk.DxgkCbNotifyInterrupt(ctx->Dxgk.DeviceHandle, &d);
        }
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
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    ULONG n = ctx->Extend ? 2u : 1u;
    if (!ChildRelations || ChildRelationsSize < n * sizeof(DXGK_CHILD_DESCRIPTOR)) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(ChildRelations, n * sizeof(DXGK_CHILD_DESCRIPTOR));
    if (n == 2) {
        /* 1.7.0: the TV over USB-C DP Alt Mode. */
        ChildRelations[1].ChildDeviceType = TypeVideoOutput;
        ChildRelations[1].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
        ChildRelations[1].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_DISPLAYPORT_EXTERNAL;
        ChildRelations[1].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
        ChildRelations[1].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
        ChildRelations[1].AcpiUid = 0;
        ChildRelations[1].ChildUid = 1;
    }
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
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)MiniportDeviceContext;
    UNREFERENCED_PARAMETER(NonDestructiveOnly);
    if (!ChildStatus || ChildStatus->ChildUid > (ctx->Extend ? 1u : 0u)) return STATUS_INVALID_PARAMETER;
    switch (ChildStatus->Type) {
    case StatusConnection:
        ChildStatus->HotPlug.Connected = ChildStatus->ChildUid == 0 ? TRUE : (ctx->TvConnected != 0);
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
    if (ChildUid > 1 || !DeviceDescriptor) return STATUS_INVALID_PARAMETER;
    /* 1.7.0: no EDID for the TV either (the panel's lesson: an EDID made
     * Windows refuse target modes without frequencies). */
    if (ChildUid == 1) return STATUS_MONITOR_NO_DESCRIPTOR;
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
        if (ctx->DoubleBuffer && !SwdFlipLock(&ctx->FbLock)) {
            InterlockedIncrement(&ctx->F4LockTimeouts);
            return STATUS_DEVICE_BUSY;
        }
        if (DevicePowerState == PowerDeviceD0) PanelWake(ctx); else PanelSleep(ctx);
        if (ctx->DoubleBuffer) SwdFlipUnlock(&ctx->FbLock);
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

/* 1.7.0: the source that drives target T in a topology (SWD_NOSRC: none). */
static ULONG SourceOfTarget(D3DKMDT_HVIDPNTOPOLOGY Topo, const DXGK_VIDPNTOPOLOGY_INTERFACE *TopoIf, ULONG T)
{
    D3DDDI_VIDEO_PRESENT_SOURCE_ID s = 0;
    if (!NT_SUCCESS(TopoIf->pfnGetPathSourceFromTarget(Topo, T, &s)) || s > 1) return SWD_NOSRC;
    return (ULONG)s;
}

/* 1.0.9: like the Basic Display sample, a source that drives the panel has
 * the panel's own size (720x1280), also for 90/270. The DWM then presents a
 * 1280x720 surface with Flags.Rotate set and the driver rotates it.
 * 1.7.0: a source that drives only the TV has the TV's size. */
static VOID SourceSizeFor(PSWD_CONTEXT Ctx, BOOLEAN DrivesPanel, ULONG *W, ULONG *H)
{
    if (DrivesPanel) { *W = Ctx->FbWidth; *H = Ctx->FbHeight; }
    else TargetSize(Ctx, 1, W, H);
}

static ULONG MaxSource(PSWD_CONTEXT Ctx) { return Ctx->Extend ? 1u : 0u; }

static NTSTATUS CheckPinnedModes(PSWD_CONTEXT Ctx, D3DKMDT_HVIDPN hVidPn, const DXGK_VIDPN_INTERFACE *Vidpn,
                                 const D3DKMDT_VIDPN_PRESENT_PATH *Path, BOOLEAN *Ok)
{
    D3DKMDT_HVIDPNSOURCEMODESET hSrc = NULL;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *srcIf = NULL;
    const D3DKMDT_VIDPN_SOURCE_MODE *srcMode = NULL;
    D3DKMDT_HVIDPNTARGETMODESET hTgt = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *tgtIf = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *tgtMode = NULL;
    ULONG tw, th;
    NTSTATUS st;

    st = Vidpn->pfnAcquireSourceModeSet(hVidPn, Path->VidPnSourceId, &hSrc, &srcIf);
    if (!NT_SUCCESS(st)) return st;
    st = srcIf->pfnAcquirePinnedModeInfo(hSrc, &srcMode);
    if (NT_SUCCESS(st) && srcMode) {
        ULONG w = (ULONG)srcMode->Format.Graphics.PrimSurfSize.cx;
        ULONG h = (ULONG)srcMode->Format.Graphics.PrimSurfSize.cy;
        BOOLEAN native = (w == Ctx->FbWidth && h == Ctx->FbHeight);
        BOOLEAN swapped = (w == Ctx->FbHeight && h == Ctx->FbWidth);
        BOOLEAN tv;
        D3DKMDT_VIDPN_PRESENT_PATH_ROTATION r = Path->ContentTransformation.Rotation;
        TargetSize(Ctx, 1, &tw, &th);
        tv = Ctx->Extend && w == tw && h == th;
        if (srcMode->Type != D3DKMDT_RMT_GRAPHICS ||
            (srcMode->Format.Graphics.PixelFormat != D3DDDIFMT_A8R8G8B8 &&
             srcMode->Format.Graphics.PixelFormat != D3DDDIFMT_X8R8G8B8)) {
            *Ok = FALSE;
            Reject(4, (LONG)srcMode->Type, (LONG)srcMode->Format.Graphics.PixelFormat);
        } else if (!native && !swapped && !tv) {
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
        TargetSize(Ctx, Path->VidPnTargetId, &tw, &th);
        if ((ULONG)tgtMode->VideoSignalInfo.ActiveSize.cx != tw ||
            (ULONG)tgtMode->VideoSignalInfo.ActiveSize.cy != th) {
            *Ok = FALSE;
            Reject(6, (LONG)((tgtMode->VideoSignalInfo.ActiveSize.cx << 16) | tgtMode->VideoSignalInfo.ActiveSize.cy),
                   (LONG)Path->VidPnTargetId);
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
        BOOLEAN scalingOk = path->VidPnTargetId == 1 ? TvScalingOk(path->ContentTransformation.Scaling)
                                                     : ScalingOk(path->ContentTransformation.Scaling);
        if (path->VidPnSourceId > MaxSource(ctx) || path->VidPnTargetId > MaxSource(ctx) || !scalingOk ||
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
                                  D3DKMDT_HVIDPNTOPOLOGY Topo, const DXGK_VIDPNTOPOLOGY_INTERFACE *TopoIf,
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
    SourceSizeFor(Ctx, SourceOfTarget(Topo, TopoIf, 0) == Path->VidPnSourceId, &w, &h);
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

static NTSTATUS EnsureTargetModes(PSWD_CONTEXT Ctx, D3DKMDT_HVIDPN hVidPn, const DXGK_VIDPN_INTERFACE *Vidpn,
                                  const D3DKMDT_VIDPN_PRESENT_PATH *Path)
{
    D3DKMDT_HVIDPNTARGETMODESET hSet = NULL;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *setIf = NULL;
    const D3DKMDT_VIDPN_TARGET_MODE *pinned = NULL;
    D3DKMDT_VIDPN_TARGET_MODE *mode = NULL;
    ULONG w, h;
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
    TargetSize(Ctx, Path->VidPnTargetId, &w, &h);
    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (NT_SUCCESS(st)) {
        FillSignalInfo(&mode->VideoSignalInfo, w, h);
        mode->Preference = D3DKMDT_MP_PREFERRED;
        st = setIf->pfnAddMode(hSet, mode);
        if (!NT_SUCCESS(st)) setIf->pfnReleaseModeInfo(hSet, mode);
    }
    if (NT_SUCCESS(st)) st = Vidpn->pfnAssignTargetModeSet(hVidPn, Path->VidPnTargetId, hSet);
    if (!NT_SUCCESS(st)) Vidpn->pfnReleaseTargetModeSet(hVidPn, hSet);
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
        BOOLEAN tvPath = path->VidPnTargetId == 1;
        /* the TV path rotates only in Duplicate, where it follows the panel */
        BOOLEAN allRot = !tvPath || SourceOfTarget(topo, topoIf, 0) == path->VidPnSourceId;
        if (!(pivot == D3DKMDT_EPT_VIDPNSOURCE && pEnumCofuncModality->EnumPivot.VidPnSourceId == path->VidPnSourceId)) {
            st = EnsureSourceModes(ctx, hVidPn, vidpn, topo, topoIf, path);
            if (!NT_SUCCESS(st)) InterlockedExchange(&gCofuncStep, 1);
        }
        if (NT_SUCCESS(st) &&
            !(pivot == D3DKMDT_EPT_VIDPNTARGET && pEnumCofuncModality->EnumPivot.VidPnTargetId == path->VidPnTargetId)) {
            st = EnsureTargetModes(ctx, hVidPn, vidpn, path);
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
                if (tvPath) {
                    /* the TV copy fits any source into the TV buffer */
                    local.ContentTransformation.ScalingSupport.Centered = 1;
                    local.ContentTransformation.ScalingSupport.AspectRatioCenteredMax = 1;
                }
                changed = TRUE;
            }
            /* 1.0.8: report all four rotations also when the rotation is
             * already pinned (1.0.7 only did it while unpinned). */
            if (!(pivot == D3DKMDT_EPT_ROTATION && isPivotPath)) {
                D3DKMDT_VIDPN_PRESENT_PATH_ROTATION_SUPPORT want;
                RtlZeroMemory(&want, sizeof(want));
                want.Identity = 1;
                if (allRot) { want.Rotate90 = 1; want.Rotate180 = 1; want.Rotate270 = 1; }
                if (RtlCompareMemory(&want, &local.ContentTransformation.RotationSupport, sizeof(want)) != sizeof(want)) {
                    local.ContentTransformation.RotationSupport = want;
                    changed = TRUE;
                }
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

static VOID TvClearLocked(PSWD_CONTEXT Ctx)
{
    if (Ctx->DoubleBuffer) {
        if (!SwdFlipLock(&Ctx->TvLock)) { InterlockedIncrement(&Ctx->F4LockTimeouts); return; }
    } else TvLock(Ctx);
    if (Ctx->TvVa) {
        if (Ctx->DoubleBuffer && Ctx->TvFlip.Second) SwdFlipClear(Ctx, TRUE);
        else TvClear(Ctx);
    }
    Ctx->TvLastW = 0;
    Ctx->TvLastH = 0;
    TvUnlock(Ctx);
}

static NTSTATUS ImplSetVidPnSourceVisibility(IN_CONST_HANDLE hAdapter,
                                     IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY pSetVidPnSourceVisibility)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    ULONG id = pSetVidPnSourceVisibility->VidPnSourceId, s;
    BOOLEAN vis = pSetVidPnSourceVisibility->Visible;
    if (id != D3DDDI_ID_ALL && id > MaxSource(ctx)) return STATUS_INVALID_PARAMETER;
    for (s = 0; s <= MaxSource(ctx); s++) {
        if (id != D3DDDI_ID_ALL && id != s) continue;
        if (!vis && ctx->Visible[s]) {
            if (s == ctx->PanelSrc) ClearFb(ctx);
            if (ctx->Extend && s == ctx->TvSrc) TvClearLocked(ctx);
        }
        ctx->Visible[s] = vis;
    }
    return STATUS_SUCCESS;
}

/* 1.7.0: the whole topology is read at every commit: which source drives the
 * panel and which the TV, their pinned sizes and the panel path's rotation. */
static NTSTATUS ImplCommitVidPn(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_COMMITVIDPN_CONST pCommitVidPn)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    D3DKMDT_HVIDPN hVidPn = pCommitVidPn->hFunctionalVidPn;
    const DXGK_VIDPN_INTERFACE *vidpn;
    D3DKMDT_HVIDPNTOPOLOGY topo;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topoIf;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = NULL;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot = D3DKMDT_VPPR_IDENTITY;
    ULONG w[2] = { 0, 0 }, h[2] = { 0, 0 }, ps, ts, s;
    NTSTATUS st;

    InterlockedIncrement(&ctx->Commits);
    if (pCommitVidPn->AffectedVidPnSourceId != D3DDDI_ID_ALL && pCommitVidPn->AffectedVidPnSourceId > MaxSource(ctx))
        return STATUS_INVALID_PARAMETER;

    st = ctx->Dxgk.DxgkCbQueryVidPnInterface(hVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vidpn);
    if (!NT_SUCCESS(st)) return st;
    st = vidpn->pfnGetTopology(hVidPn, &topo, &topoIf);
    if (!NT_SUCCESS(st)) return st;
    ps = SourceOfTarget(topo, topoIf, 0);
    ts = ctx->Extend ? SourceOfTarget(topo, topoIf, 1) : SWD_NOSRC;

    for (s = 0; s <= MaxSource(ctx); s++) {
        D3DKMDT_HVIDPNSOURCEMODESET hSet = NULL;
        const DXGK_VIDPNSOURCEMODESET_INTERFACE *setIf = NULL;
        const D3DKMDT_VIDPN_SOURCE_MODE *mode = NULL;
        if (s != ps && s != ts) continue;
        st = vidpn->pfnAcquireSourceModeSet(hVidPn, s, &hSet, &setIf);
        if (!NT_SUCCESS(st)) return st;
        st = setIf->pfnAcquirePinnedModeInfo(hSet, &mode);
        if (NT_SUCCESS(st) && mode) {
            w[s] = (ULONG)mode->Format.Graphics.PrimSurfSize.cx;
            h[s] = (ULONG)mode->Format.Graphics.PrimSurfSize.cy;
            setIf->pfnReleaseModeInfo(hSet, mode);
        }
        vidpn->pfnReleaseSourceModeSet(hVidPn, hSet);
    }
    if (ps != SWD_NOSRC && (w[ps] == 0 || h[ps] == 0)) ps = SWD_NOSRC;
    if (ts != SWD_NOSRC && (w[ts] == 0 || h[ts] == 0)) ts = SWD_NOSRC;

    if (ps != SWD_NOSRC) {
        st = topoIf->pfnAcquirePathInfo(topo, ps, 0, &path);
        if (NT_SUCCESS(st) && path) {
            rot = NormalizeRotation(path->ContentTransformation.Rotation);
            topoIf->pfnReleasePathInfo(topo, path);
        }
        if (!((w[ps] == ctx->FbWidth && h[ps] == ctx->FbHeight) || (w[ps] == ctx->FbHeight && h[ps] == ctx->FbWidth)))
            return STATUS_GRAPHICS_INVALID_VIDPN_SOURCEMODESET;
    }

    /* panel */
    if (ps == SWD_NOSRC) {
        /* Nothing is shown on the panel. */
        if (ctx->Active) ClearFb(ctx);
        ctx->Active = FALSE;
    } else {
        if (!ctx->Active || ctx->PanelSrc != ps || ctx->Rotation != rot || ctx->SrcW[ps] != w[ps] || ctx->SrcH[ps] != h[ps])
            ClearFb(ctx);
        ctx->Rotation = rot;
        ctx->Active = TRUE;
    }
    for (s = 0; s < 2; s++) if (w[s] && h[s]) { ctx->SrcW[s] = w[s]; ctx->SrcH[s] = h[s]; }
    ctx->PanelSrc = ps;

    /* TV */
    if (ctx->Extend && ts != ctx->TvSrc) {
        ctx->TvSrc = ts;
        TvClearLocked(ctx);
    }

    RegPutDword(L"CommitActive", ctx->Active ? 1 : 0);
    RegPutDword(L"PanelSrc", ps);
    RegPutDword(L"TvSrc", ctx->Extend ? ts : 0);
    RegPutDword(L"Rotation", (ULONG)rot);
    RegPutDword(L"SrcWidth", ctx->SrcW[0]);
    RegPutDword(L"SrcHeight", ctx->SrcH[0]);
    RegPutDword(L"Src1Width", ctx->SrcW[1]);
    RegPutDword(L"Src1Height", ctx->SrcH[1]);
    return STATUS_SUCCESS;
}

static NTSTATUS ImplUpdateActiveVidPnPresentPath(IN_CONST_HANDLE hAdapter,
    IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST pUpdateActiveVidPnPresentPath)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    const D3DKMDT_VIDPN_PRESENT_PATH *path = &pUpdateActiveVidPnPresentPath->VidPnPresentPathInfo;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot;

    if (path->VidPnSourceId > MaxSource(ctx) || path->VidPnTargetId > MaxSource(ctx)) return STATUS_INVALID_PARAMETER;
    if (!RotationKnown(path->ContentTransformation.Rotation)) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    if (path->VidPnTargetId == 1) {
        /* the TV copy is always upright and fitted */
        return TvScalingOk(path->ContentTransformation.Scaling) ? STATUS_SUCCESS : STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    }
    if (!ScalingOk(path->ContentTransformation.Scaling)) return STATUS_GRAPHICS_VIDPN_MODALITY_NOT_SUPPORTED;
    rot = NormalizeRotation(path->ContentTransformation.Rotation);
    /* 1.0.8: DRIVERCAPS says SupportSmoothRotation, so Windows rotates with
     * this call alone, also 0<->90 (the desktop surface then changes from
     * 720x1280 to 1280x720). 1.0.7 refused that ("could not be saved"). */
    if (rot != ctx->Rotation) {
        ctx->Rotation = rot;
        ClearFb(ctx);
        if (ctx->Extend && ctx->TvSrc == path->VidPnSourceId) TvClearLocked(ctx);
    }
    RegPutDword(L"Rotation", (ULONG)rot);
    return STATUS_SUCCESS;
}

static NTSTATUS ImplRecommendMonitorModes(IN_CONST_HANDLE hAdapter,
                                  IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST pRecommendMonitorModes)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    const DXGK_MONITORSOURCEMODESET_INTERFACE *setIf = pRecommendMonitorModes->pMonitorSourceModeSetInterface;
    D3DKMDT_HMONITORSOURCEMODESET hSet = pRecommendMonitorModes->hMonitorSourceModeSet;
    D3DKMDT_MONITOR_SOURCE_MODE *mode = NULL;
    ULONG w, h;
    NTSTATUS st;

    TargetSize(ctx, pRecommendMonitorModes->VideoPresentTargetId == 1 ? 1u : 0u, &w, &h);
    st = setIf->pfnCreateNewModeInfo(hSet, &mode);
    if (!NT_SUCCESS(st)) return st;
    FillSignalInfo(&mode->VideoSignalInfo, w, h);
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
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)i_hAdapter;
    if (io_pVidPnHWCaps->SourceId > MaxSource(ctx) || io_pVidPnHWCaps->TargetId > MaxSource(ctx)) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(&io_pVidPnHWCaps->VidPnHWCaps, sizeof(io_pVidPnHWCaps->VidPnHWCaps));
    io_pVidPnHWCaps->VidPnHWCaps.DriverRotation = 1;   /* rotation is done in the copy */
    if (io_pVidPnHWCaps->TargetId == 1) io_pVidPnHWCaps->VidPnHWCaps.DriverScaling = 1;   /* the TV copy fits */
    return STATUS_SUCCESS;
}

static VOID GrowBounds(LONG *Top, LONG *Bottom, const RECT *R)
{
    if (R->top < *Top) *Top = R->top;
    if (R->bottom > *Bottom) *Bottom = R->bottom;
}

static VOID SwdPresentForward(PSWD_CONTEXT Ctx, const DXGKARG_PRESENT_DISPLAYONLY *Present,
                              BOOLEAN Tv, LONG Index, LONG W, LONG H,
                              D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                              BOOLEAN IdentityGpu, BOOLEAN Full)
{
    SWD_FLIP *flip = Tv ? &Ctx->TvFlip : &Ctx->FbFlip;
    PUCHAR from = SwdFlipVa(Ctx, Tv, Index), to = SwdFlipVa(Ctx, Tv, 1 - Index);
    PMDL primary = SwdFlipMdl0(Ctx, Tv);
    PMDL srcMdl = Index ? flip->Mdl : primary;
    PMDL dstMdl = Index ? primary : flip->Mdl;
    ULONG pitch = Tv ? Ctx->TvPitch : Ctx->FbPitch;
    ULONG pass, i;
    KeMemoryBarrier();
    if (!Tv && Ctx->DcStopped) {
        RtlCopyMemory(to, from, flip->Bytes);
        ++flip->CpuForward;
        return;
    }
    if (flip->Enabled) {
        if (!SwdFlipComplete(Ctx, Tv, Index)) return;
    } else if (!flip->Pending) return;

    if (Full) {
        RtlCopyMemory(to, from, flip->Bytes);
    } else if (flip->Enabled && IdentityGpu && !gGpuDisabled && Ctx->GpuTarget &&
               srcMdl && !(srcMdl->MdlFlags & MDL_IO_SPACE) && dstMdl) {
        DXGKARG_PRESENT_DISPLAYONLY layout = *Present;
        NTSTATUS status;
        layout.Pitch = pitch;
        status = SwdGpuRects(Ctx, &layout, srcMdl, dstMdl, 0, H, W, pitch);
        if (NT_SUCCESS(status)) { ++flip->GpuForward; return; }
        if (status == SWGPU_BLIT_DMA_UNSAFE) {
            if (Tv) Ctx->TvDmaUnsafe = TRUE;
            else Ctx->FbDmaUnsafe = TRUE;
            SwdFlipFallback(flip, (ULONG)status);
            InterlockedExchange(&gGpuDisabled, 1);
            return;
        }
    }
    if (!Full) {
        for (pass = 0u; pass < 2u; ++pass) {
            ULONG count = pass == 0u ? Present->NumMoves : Present->NumDirtyRects;
            for (i = 0u; i < count; ++i) {
                RECT r = pass == 0u ? Present->pMoves[i].DestRect : Present->pDirtyRect[i];
                if (Tv) TvBlt(Ctx, to, from, NULL, 0, 0, r, W, H, Rotation);
                else BltRect(Ctx, to, from, NULL, 0, 0, r, W, H, Rotation);
            }
        }
    }
    ++flip->CpuForward;
}

DXGKDDI_PRESENTDISPLAYONLY SwdPresentDisplayOnly;
NTSTATUS SwdPresentDisplayOnly(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_PRESENT_DISPLAYONLY pPresent)
{
    PSWD_CONTEXT ctx = (PSWD_CONTEXT)hAdapter;
    LONG top = MAXLONG, bottom = MINLONG;
    ULONG i, sid;
    PMDL mdl;
    PUCHAR src, va;
    SIZE_T len;
    KPROCESSOR_MODE mode;
    NTSTATUS st = STATUS_SUCCESS;
    D3DKMDT_VIDPN_PRESENT_PATH_ROTATION rot, srcRot;
    BOOLEAN toPanel, toTv, keepMdl = FALSE, cpuFallback = FALSE, gpuSuccess = FALSE;
    LARGE_INTEGER cpuFreq, cpuStart;
    ULONG cpuUs = 0u, gpuUs = 0u;
    LONG ws, hs;

    InterlockedIncrement(&ctx->Presents);
    sid = pPresent->VidPnSourceId;
    if (sid > MaxSource(ctx)) return STATUS_INVALID_PARAMETER;
    /* Drawing continues while the monitor is off (stage 1 keeps the panel
     * scanning), so the image is complete when it comes back on. */
    if (!ctx->FbVa || !ctx->Visible[sid] || ctx->SystemDisplay) return STATUS_SUCCESS;
    toPanel = ctx->Active && ctx->PanelSrc == sid;
    toTv = ctx->Extend ? (ctx->TvSrc == sid) : toPanel;   /* without Extend: the 1.3.0 mirror */
    if (!toPanel && !toTv) return STATUS_SUCCESS;
    if (pPresent->BytesPerPixel != 4 || pPresent->Pitch <= 0 || !pPresent->pSource) {
        st = STATUS_INVALID_PARAMETER;
        goto out;
    }

    /* Surface shape: for 90/270 with Flags.Rotate the DWM surface is the mode
     * size swapped (1280x720), as in the Basic Display sample. Only the
     * panel path rotates. */
    srcRot = (sid == ctx->PanelSrc) ? ctx->Rotation : D3DKMDT_VPPR_IDENTITY;
    rot = pPresent->Flags.Rotate ? srcRot : D3DKMDT_VPPR_IDENTITY;
    if (IsRotated(rot)) { ws = (LONG)ctx->SrcH[sid]; hs = (LONG)ctx->SrcW[sid]; }
    else                { ws = (LONG)ctx->SrcW[sid]; hs = (LONG)ctx->SrcH[sid]; }

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
        BOOLEAN panelLocked = toPanel && ctx->DoubleBuffer && SwdFlipLock(&ctx->FbLock);
        if (toPanel && ctx->DoubleBuffer && !panelLocked) {
            InterlockedIncrement(&ctx->F4LockTimeouts);
            st = STATUS_DEVICE_BUSY;
        }
        if (toPanel && (!ctx->DoubleBuffer || panelLocked) && !ctx->FbDmaUnsafe) {
            LONG index = ctx->FbFlip.Second ? SwdFlipDrawIndex(ctx, FALSE) : 0;
            PUCHAR dest = SwdFlipVa(ctx, FALSE, index);
            PMDL targetMdl = index ? ctx->FbFlip.Mdl : SwdFlipMdl0(ctx, FALSE);
            BOOLEAN eligible = ctx->GpuPresent && rot == D3DKMDT_VPPR_IDENTITY &&
                ws > 0 && hs > 0 && ws <= (LONG)ctx->FbWidth && hs <= (LONG)ctx->FbHeight;
            if (!eligible || !SwdGpuTry(ctx, pPresent, mdl, targetMdl,
                    top, bottom, ws, ctx->FbPitch, FALSE, &keepMdl, &gpuSuccess, &gpuUs)) {
                if (eligible) cpuStart = KeQueryPerformanceCounter(&cpuFreq);
                for (i = 0; i < pPresent->NumMoves; i++) BltRect(ctx, dest, NULL, src, top, pPresent->Pitch, pPresent->pMoves[i].DestRect, ws, hs, rot);
                for (i = 0; i < pPresent->NumDirtyRects; i++) BltRect(ctx, dest, NULL, src, top, pPresent->Pitch, pPresent->pDirtyRect[i], ws, hs, rot);
                if (eligible) { cpuFallback = TRUE; cpuUs += SwdGpuUs(cpuStart, cpuFreq); }
            }
            if (ctx->FbFlip.Second && !ctx->FbDmaUnsafe)
                SwdPresentForward(ctx, pPresent, FALSE, index, ws, hs, rot, eligible, FALSE);
        }
        if (panelLocked) SwdFlipUnlock(&ctx->FbLock);
        if (toTv && (!ctx->DoubleBuffer || SwdFlipLock(&ctx->TvLock))) {
            if (!ctx->DoubleBuffer) TvLock(ctx);
            if (ctx->TvVa && !ctx->TvDmaUnsafe) {
                /* The TV gets the picture upright: with Flags.Rotate the surface is
                 * upright already; without it the rotation is baked in (1.4.3). */
                D3DKMDT_VIDPN_PRESENT_PATH_ROTATION baked = pPresent->Flags.Rotate ? D3DKMDT_VPPR_IDENTITY : srcRot;
                LONG tw = IsRotated(baked) ? hs : ws, th = IsRotated(baked) ? ws : hs;
                LONG index = ctx->TvFlip.Second ? SwdFlipDrawIndex(ctx, TRUE) : 0;
                PUCHAR dest = SwdFlipVa(ctx, TRUE, index);
                PMDL targetMdl = index ? ctx->TvFlip.Mdl : ctx->TvMdl;
                BOOLEAN shapeChanged = tw != ctx->TvLastW || th != ctx->TvLastH ||
                    (ctx->DoubleBuffer && baked != ctx->TvLastBaked);
                if (shapeChanged) {
                    if (ctx->TvFlip.Second) RtlZeroMemory(dest, ctx->TvSize);
                    else TvClear(ctx);
                    ctx->TvLastW = tw;
                    ctx->TvLastH = th;
                    ctx->TvLastBaked = baked;
                }
                {
                    BOOLEAN eligible = ctx->GpuPresent && baked == D3DKMDT_VPPR_IDENTITY &&
                        ws > 0 && hs > 0 && ws == (LONG)ctx->TvW && hs == (LONG)ctx->TvH;
                    if (!eligible || !SwdGpuTry(ctx, pPresent, mdl, targetMdl,
                            top, bottom, ws, ctx->TvPitch, TRUE, &keepMdl, &gpuSuccess, &gpuUs)) {
                        if (eligible) cpuStart = KeQueryPerformanceCounter(&cpuFreq);
                        for (i = 0; i < pPresent->NumMoves; i++) TvBlt(ctx, dest, NULL, src, top, pPresent->Pitch, pPresent->pMoves[i].DestRect, ws, hs, baked);
                        for (i = 0; i < pPresent->NumDirtyRects; i++) TvBlt(ctx, dest, NULL, src, top, pPresent->Pitch, pPresent->pDirtyRect[i], ws, hs, baked);
                        if (eligible) { cpuFallback = TRUE; cpuUs += SwdGpuUs(cpuStart, cpuFreq); }
                    }
                    if (ctx->TvFlip.Second && !ctx->TvDmaUnsafe)
                        SwdPresentForward(ctx, pPresent, TRUE, index, ws, hs, baked, eligible, shapeChanged);
                }
                InterlockedIncrement(&ctx->TvPresents);
                if (!toPanel) InterlockedIncrement(&ctx->TvSrcPresents);
            }
            TvUnlock(ctx);
        } else if (toTv) {
            InterlockedIncrement(&ctx->F4LockTimeouts);
            st = STATUS_DEVICE_BUSY;
        }
        KeMemoryBarrier();
    } else {
        st = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (gpuSuccess) {
        InterlockedIncrement(&ctx->GpuPresents);
        InterlockedAdd64(&ctx->GpuPresentUs, gpuUs);
    }
    if (cpuFallback) {
        InterlockedIncrement(&ctx->GpuFallbacks);
        InterlockedAdd64(&ctx->GpuFallbackUs, cpuUs);
    }
    if (!keepMdl) {
        MmUnlockPages(mdl);
        IoFreeMdl(mdl);
    }

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
    LONG front;
    PUCHAR dest;
    if (!ctx->FbVa || ctx->FbDmaUnsafe || !Source || PositionX >= ctx->FbWidth || PositionY >= ctx->FbHeight) return;
    front = ctx->FbFlip.Front;
    dest = SwdFlipVa(ctx, FALSE, front);
    w = SourceWidth;  if (w > ctx->FbWidth - PositionX) w = ctx->FbWidth - PositionX;
    h = SourceHeight; if (h > ctx->FbHeight - PositionY) h = ctx->FbHeight - PositionY;
    for (y = 0; y < h; y++) {
        SIZE_T offset = (SIZE_T)(PositionY + y) * ctx->FbPitch + (SIZE_T)PositionX * 4;
        RtlCopyMemory(dest + offset, (PUCHAR)Source + (SIZE_T)y * SourceStride, (SIZE_T)w * 4);
        /* A bugcheck may interrupt ACT_REQ before we publish the new index.
         * Write both candidates while that hardware latch is in flight. */
        if (ctx->FbFlip.Pending && ctx->FbFlip.Second)
            RtlCopyMemory(SwdFlipVa(ctx, FALSE, 1 - front) + offset,
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
    /* 1.4.2: not registered. With ControlInterrupt and no interrupt in the
     * DSDT (R408+), dxgkrnl stopped the adapter right after start (problem 43,
     * 0xC00000E5) from 1.2.0 on; 1.1.0 did not register it. */

    return DxgkInitializeDisplayOnlyDriver(DriverObject, RegistryPath, &init);
}
