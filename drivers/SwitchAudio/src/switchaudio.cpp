/*
 * Nintendo Switch audio driver (speaker), PortCls WaveRT. Stage 1.
 *
 * ACPI device AUD0 (ACPI\SWOA0C01), UEFI R402+:
 *   Memory32Fixed 0x702D0000 (0x2000): AHUB. ADMAIF at +0x000, XBAR at
 *     +0x800, I2S1 at +0x1000.
 *   Memory32Fixed 0x702E2000 (0x1000): ADMA, channel 0 at +0x000, global
 *     registers at +0xC00.
 *   I2cSerialBus 0x1C on I2C1: the ALC5639 codec (not used yet).
 *
 * UEFI R402 turns on the APE clocks (d_audio, CAR i2s0 for the I2S at
 * 0x702D1000), sets up the codec (speaker path, -18 dB) and leaves this
 * path running: XBAR I2S1 <- ADMAIF1, ADMAIF1 TX (16-bit pack), I2S1 as
 * master, 48 kHz, 16 bit, stereo. The driver only feeds ADMAIF1 through
 * ADMA channel 0 from the WaveRT buffer.
 *
 * Reading an APE register with the APE clocks off hangs the CPU (UEFI R395),
 * so StartDevice first checks the clocks, resets and the APE power partition
 * in CAR and PMC, and fails (Code 10) if UEFI did not prepare them.
 *
 * The WaveRT buffer is split into 8 periods (ADMA SRC_BUF 7, TC = one
 * period), ADMA runs it in continuous mode. ADMA counts finished periods in
 * XFER_STATUS (16 bits); the position inside the current period is estimated
 * from the performance counter (Linux reports the position per period).
 *
 * One render pin, no volume node: Windows scales the volume in software.
 * Diagnostics go to Services\switchaudio\Parameters.
 *
 * 1.0.0.2: 1.0.0.1 showed the speaker but played nothing. The registry now
 * gets the state of ADMA, ADMAIF1, XBAR and I2S1 at the start of the device
 * (Start*), 0 and 100 ms after each stream start (Run0*, Run100*) and at each
 * stop (Stop*, with the periods ADMA finished). Parameters\TestTone = 1 plays
 * a one-second tone from the driver's own buffer at the next device start.
 *
 * 1.0.0.3: the tone played, but Windows opened 58 streams and never ran one.
 * The buffer was MmNonCached, which ARM64 maps as Device memory; the audio
 * engine (audiodg) maps the buffer too and its unaligned or DC ZVA writes
 * fault there. Now MmWriteCombined (Normal non-cacheable: no cache, so ADMA
 * still sees the data). Buffers counts AllocateAudioBuffer calls, StateLog
 * keeps the last eight KS states (one hex digit each, newest last).
 *
 * 1.0.0.4: with 1.0.0.3 Windows took the buffer eight times and closed the
 * stream each time without a single state change. CallLog keeps the last
 * eight calls into the stream (one hex digit each, newest last, repeats
 * folded): 1 NewStream, 2 SetFormat, 3 AllocateAudioBuffer, 4 FreeAudioBuffer,
 * 5 GetHWLatency, 6 GetPositionRegister, 7 GetClockRegister, 8 GetPosition,
 * 9 SetState, A the stream freed. The buffer is now exactly the requested
 * size when that makes periods of at least 1024 bytes (5.3 ms).
 *
 * 1.0.0.5: with the requested size Windows played, but crunchy. XFER_STATUS
 * restarts from 0 when the channel is enabled, and the start value was read
 * before that (0x3FA left over), so the first update jumped 0xFCC0 periods
 * and the position stayed two periods off: Windows wrote where ADMA had
 * already read. The start value is now read after the enable.
 *
 * 1.0.0.6: clean, but with an occasional dropout. A period change was dated
 * at the poll that saw it, up to one engine period (10 ms) late, so the
 * position could lag ADMA by most of a period and Windows wrote where ADMA
 * had already fetched. The change is now dated where the nominal rate puts
 * it, kept between the previous poll and this one. The reported FIFO is one
 * period, for more headroom. Gaps counts polls that missed a whole period.
 *
 * 1.0.0.7: headphones. UEFI R403 powers the codec's headphone path (muted)
 * and makes PV6, the jack switch (low = plugged), a GPIO input. A thread
 * reads PV6 every 200 ms; after two equal readings it unmutes the output
 * the jack picks and mutes the other, over SPB (the I2cSerialBus of AUD0).
 * Windows still shows one endpoint. With an older UEFI (HPO MIX without
 * HPOVOL, or PV6 not a GPIO) the thread is not started: speaker only.
 * Traces: HpoMixer, JackGpio, Jack (1 = headphones), JackSwitches, JackSpb.
 *
 * 1.1.0: the TV over USB-C DP Alt Mode as a second endpoint ("TvWave" and
 * "TvTopology"), 48 kHz 16 bit stereo, through the Tegra HDA controller and its
 * HDMI/DP codec (see "TV through the HDA" below). Needs switchdisplay 1.6.0,
 * which sets SOR1 up for DP audio. The jack follows the codec's pin sense, so
 * Windows lists the TV only while the TV picture is on. Traces start with Tv;
 * TvStage 0x32 = endpoint registered. Parameters\TvTestTone = 1 plays one
 * second of 1 kHz to the TV at the next start; TvAudioOff = 1 leaves the HDA
 * alone (speaker only).
 *
 * 1.1.1: with 1.1.0 the TV endpoint was registered and the pin sense said
 * connected, but Windows listed no TV. Traces now count the jack property
 * calls (TvJackCalls/Gets/Last) and event subscriptions (TvEventAdds/Other),
 * and the jack event is sent again 5 s and 30 s after the start. TvTestTone is
 * now read by the jack thread (TvTone.cmd works without a device restart).
 *
 * 1.1.2: 1.1.1 showed the tone on the TV, and the endpoint "Digital Output"
 * in MMDevices as not present, with no jack property call and no event
 * subscription. The jack automation table moves from pin 1 to the topology
 * filter (SYSVAD layout); the event is generated as a filter event.
 *
 * 1.1.3: with 1.1.2 Windows asked about the jack and the endpoint became
 * "unplugged", but after an unplug and re-plug the pin sense stayed 0 while
 * the TV picture and SOR1 audio came back. Presence = pin sense OR
 * switchdisplay's TvAudioState == 6 (switchdisplay 1.6.1). TvSenseRaw and
 * TvDisplayState are traced at each change.
 */

#define INITGUID
/* The STATIC_ GUID initializers of ksmedia.h leave out the Data4 braces. */
#pragma clang diagnostic ignored "-Wmissing-braces"
#include <ntddk.h>
#include <portcls.h>
#include <ksmedia.h>
#include <ntstrsafe.h>
#include <spb.h>

#define POOL_TAG 'duAS'

/* -------------------------------------------------------------- registers */

#define CAR_PHYS                 0x60006000ULL
#define CAR_RST_DEVICES_L        0x004
#define CAR_CLK_OUT_ENB_L        0x010
#define CAR_RST_DEVICES_V        0x358
#define CAR_CLK_OUT_ENB_V        0x360
#define CAR_RST_DEVICES_Y        0x2A4
#define CAR_CLK_OUT_ENB_Y        0x298
#define CAR_L_I2S0               (1u << 30)
#define CAR_V_D_AUDIO            (1u << 10)
#define CAR_Y_APE                (1u << 6)
#define PMC_PHYS                 0x7000E400ULL
#define PMC_PWRGATE_STATUS       0x38
#define PMC_PG_AUD               (1u << 27)

#define AHUB_LENGTH              0x2000
#define ADMA_LENGTH              0x1000
#define ADMAIF_TX1_ENABLE        0x300
#define ADMAIF_TX1_STATUS        0x30C
#define ADMAIF_TX1_ACIF_CTRL     0x320
#define ADMAIF_GLOBAL_ENABLE     0x700
#define XBAR_I2S1_RX_MUX         0x840
#define I2S1_RX_ENABLE           0x1000
#define I2S1_RX_STATUS           0x100C
#define I2S1_ENABLE              0x1080
#define I2S1_CTRL                0x10A0
#define I2S1_RX_CIF_FIFO         0x1034
#define I2S1_STATUS              0x108C
#define ADMAIF_TX1_INT_STATUS    0x310

#define ADMA_CH_CMD              0x00
#define ADMA_CH_STATUS           0x0C
#define ADMA_CH_STATUS_PAUSED    (1u << 1)
#define ADMA_CH_CTRL             0x24
#define ADMA_CH_CTRL_PAUSE       (1u << 0)
#define ADMA_CH_CONFIG           0x28
#define ADMA_CH_FIFO_CTRL        0x2C
#define ADMA_CH_LOWER_SRC        0x34
#define ADMA_CH_LOWER_TRG        0x3C
#define ADMA_CH_TC               0x44
#define ADMA_CH_XFER_STATUS      0x54
#define ADMA_GLOBAL_CMD          0xC00
/* Request ADMAIF1 TX, memory to AHUB, continuous, flow control (as UEFI R398). */
#define ADMA_CTRL_TX1            ((1u << 28) | (4u << 12) | (2u << 8) | (1u << 1))
/* SRC_BUF = periods - 1, burst 16 words, WRR weight 1. */
#define ADMA_CONFIG(periods)     ((((periods) - 1u) << 28) | (5u << 20) | 1u)
#define ADMA_FIFO_TX3            (3u << 8)

#define SA_RATE                  48000u
#define SA_CHANNELS              2u
#define SA_BITS                  16u
#define SA_FRAME                 (SA_CHANNELS * SA_BITS / 8u)
#define SA_BYTES_PER_SEC         (SA_RATE * SA_FRAME)
#define SA_PERIODS               8u
#define SA_PERIOD_ALIGN          64u            /* one ADMA burst */
#define SA_MIN_PERIOD            1024u          /* 256 frames, 5.3 ms */

/* ------------------------------------------------------------ the hardware */

/* 1.0.0.7: the jack switch and the codec registers for the outputs. */
#define GPIO_PHYS                0x6000D000ull
#define GPIO_LENGTH              0x1000u
#define GPIO_PV_CNF              0x504u          /* port V: bank 5, port 1 */
#define GPIO_PV_OE               0x514u
#define GPIO_PV_IN               0x534u
#define GPIO_PV6                 (1u << 6)
#define RT5640_SPK_VOL           0x01
#define RT5640_HP_VOL            0x02
#define RT5640_HPO_MIXER         0x45
#define RT5640_M_HPVOL_HM        (1u << 13)
#define SA_SPK_VOL               0x1414u         /* -18 dB, as UEFI R402 */
#define SA_HP_VOL                0x1818u         /* -24 dB, as UEFI R403 */
#define SA_MUTE                  0x8080u

typedef struct _SA_HW {
    volatile UCHAR *Ahub;
    volatile UCHAR *Adma;
    KSPIN_LOCK Lock;
    BOOLEAN Ready;
    HANDLE Spb;                     /* the codec on I2C1 */
    volatile UCHAR *Gpio;
    PKTHREAD JackThread;
    KEVENT JackStop;
} SA_HW;

static SA_HW g_Hw;
static WCHAR g_ParamsPath[] = L"switchaudio\\Parameters";
static LONG g_Buffers;
static ULONG g_StateLog;
static ULONG g_CallLog;

static ULONG RdAhub(ULONG Offset) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Ahub + Offset)); }
static VOID WrAhub(ULONG Offset, ULONG Value) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Hw.Ahub + Offset), Value); }
static ULONG RdAdma(ULONG Offset) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Adma + Offset)); }
static VOID WrAdma(ULONG Offset, ULONG Value) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Hw.Adma + Offset), Value); }

static VOID SaTrace(PCWSTR Name, ULONG Value)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, g_ParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

static ULONG ReadPhys(ULONGLONG Address)
{
    PHYSICAL_ADDRESS Pa;
    volatile ULONG *Va;
    ULONG Value;
    Pa.QuadPart = (LONGLONG)Address;
    Va = (volatile ULONG *)MmMapIoSpaceEx(Pa, sizeof(ULONG), PAGE_READWRITE | PAGE_NOCACHE);
    if (Va == NULL) {
        return 0;
    }
    Value = READ_REGISTER_ULONG(Va);
    MmUnmapIoSpace((PVOID)Va, sizeof(ULONG));
    return Value;
}

/* Bit 0: APE partition on, 1: d_audio on and out of reset, 2: APE clock,
   3: I2S (CAR i2s0) clock. All four must be set before touching the AHUB. */
static ULONG SaCheckApe(VOID)
{
    ULONG Ok = 0;
    if ((ReadPhys(PMC_PHYS + PMC_PWRGATE_STATUS) & PMC_PG_AUD) != 0) Ok |= 1;
    if ((ReadPhys(CAR_PHYS + CAR_CLK_OUT_ENB_V) & CAR_V_D_AUDIO) != 0 &&
        (ReadPhys(CAR_PHYS + CAR_RST_DEVICES_V) & CAR_V_D_AUDIO) == 0) Ok |= 2;
    if ((ReadPhys(CAR_PHYS + CAR_CLK_OUT_ENB_Y) & CAR_Y_APE) != 0 &&
        (ReadPhys(CAR_PHYS + CAR_RST_DEVICES_Y) & CAR_Y_APE) == 0) Ok |= 4;
    if ((ReadPhys(CAR_PHYS + CAR_CLK_OUT_ENB_L) & CAR_L_I2S0) != 0 &&
        (ReadPhys(CAR_PHYS + CAR_RST_DEVICES_L) & CAR_L_I2S0) == 0) Ok |= 8;
    return Ok;
}

static VOID SaAdmaStop(VOID)
{
    if (g_Hw.Adma != NULL) {
        WrAdma(ADMA_CH_CMD, 0);
    }
}

static VOID SaTraceAs(PCWSTR Prefix, PCWSTR Name, ULONG Value)
{
    WCHAR Full[48];
    if (NT_SUCCESS(RtlStringCchPrintfW(Full, ARRAYSIZE(Full), L"%s%s", Prefix, Name))) {
        SaTrace(Full, Value);
    }
}

/* 1.0.0.4: one hex digit per call into the stream, repeats folded. */
static VOID SaCall(ULONG Code)
{
    ULONG Old = g_CallLog, New;
    if ((Old & 0xF) == Code) {
        return;
    }
    New = (Old << 4) | Code;
    if (InterlockedCompareExchange((volatile LONG *)&g_CallLog, (LONG)New, (LONG)Old) == (LONG)Old &&
        KeGetCurrentIrql() == PASSIVE_LEVEL) {
        SaTrace(L"CallLog", New);
    }
}

/* 1.0.0.2: the state of the whole path, PASSIVE_LEVEL only. */
static VOID SaSnapshot(PCWSTR Prefix)
{
    if (g_Hw.Ahub == NULL || g_Hw.Adma == NULL) {
        return;
    }
    SaTraceAs(Prefix, L"AdmaStatus", RdAdma(ADMA_CH_STATUS));
    SaTraceAs(Prefix, L"AdmaXfer", RdAdma(ADMA_CH_XFER_STATUS));
    SaTraceAs(Prefix, L"AdmaSrc", RdAdma(ADMA_CH_LOWER_SRC));
    SaTraceAs(Prefix, L"AdmaTc", RdAdma(ADMA_CH_TC));
    SaTraceAs(Prefix, L"AdmaGlobal", RdAdma(ADMA_GLOBAL_CMD));
    SaTraceAs(Prefix, L"AdmaifEnable", RdAhub(ADMAIF_TX1_ENABLE));
    SaTraceAs(Prefix, L"AdmaifStatus", RdAhub(ADMAIF_TX1_STATUS));
    SaTraceAs(Prefix, L"AdmaifInt", RdAhub(ADMAIF_TX1_INT_STATUS));
    SaTraceAs(Prefix, L"Xbar", RdAhub(XBAR_I2S1_RX_MUX));
    SaTraceAs(Prefix, L"I2sEnable", RdAhub(I2S1_ENABLE));
    SaTraceAs(Prefix, L"I2sRxEnable", RdAhub(I2S1_RX_ENABLE));
    SaTraceAs(Prefix, L"I2sRxStatus", RdAhub(I2S1_RX_STATUS));
    SaTraceAs(Prefix, L"I2sStatus", RdAhub(I2S1_STATUS));
    SaTraceAs(Prefix, L"I2sRxFifo", RdAhub(I2S1_RX_CIF_FIFO));
}

static VOID SaSleepMs(ULONG Ms)
{
    LARGE_INTEGER Due;
    Due.QuadPart = -(LONGLONG)Ms * 10000;
    (VOID)KeDelayExecutionThread(KernelMode, FALSE, &Due);
}

static ULONG SaReadParam(PCWSTR Name)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 0;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, g_ParamsPath, Table, NULL, NULL))) {
        return 0;
    }
    return Value;
}

/* 1.0.0.2: Parameters\TestTone = 1 plays one second of 1 kHz (-18 dBFS) at the
   next start, from the driver's own buffer: checks the hardware path without
   the Windows audio engine. The value is cleared afterwards. */
static VOID SaTestTone(VOID)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    ULONG *Words;
    ULONG Index;
    /* One 1 kHz period at 48 kHz, 1/8 of full scale (the UEFI R400 tone). */
    static const SHORT Sine[48] = {
            0,   535,  1060,  1567,  2048,  2493,  2896,  3250,  3547,  3784,  3956,  4061,
         4096,  4061,  3956,  3784,  3547,  3250,  2896,  2493,  2048,  1567,  1060,   535,
            0,  -535, -1060, -1567, -2048, -2493, -2896, -3250, -3547, -3784, -3956, -4061,
        -4096, -4061, -3956, -3784, -3547, -3250, -2896, -2493, -2048, -1567, -1060,  -535,
    };

    SaTrace(L"TestTone", 0);
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Skip.QuadPart = 0;
    Words = (ULONG *)MmAllocateContiguousNodeMemory(PAGE_SIZE, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE,
                                                    MM_ANY_NODE_OK);
    if (Words == NULL) {
        SaTrace(L"ToneResult", 1);
        return;
    }
    for (Index = 0; Index < 48; ++Index) {
        Words[Index] = ((ULONG)(USHORT)Sine[Index] << 16) | (USHORT)Sine[Index];
    }
    SaSnapshot(L"ToneBefore");
    WrAhub(ADMAIF_GLOBAL_ENABLE, 1);
    WrAhub(ADMAIF_TX1_ENABLE, 1);
    WrAhub(I2S1_RX_ENABLE, 1);
    WrAhub(I2S1_ENABLE, 1);
    WrAdma(ADMA_GLOBAL_CMD, 1);
    WrAdma(ADMA_CH_CMD, 0);
    WrAdma(ADMA_CH_TC, 48 * sizeof(ULONG));
    WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1);
    WrAdma(ADMA_CH_LOWER_SRC, (ULONG)MmGetPhysicalAddress(Words).QuadPart);
    WrAdma(ADMA_CH_LOWER_TRG, 0);
    WrAdma(ADMA_CH_FIFO_CTRL, ADMA_FIFO_TX3);
    WrAdma(ADMA_CH_CONFIG, ADMA_CONFIG(1));
    WrAdma(ADMA_CH_CMD, 1);
    SaSleepMs(500);
    SaSnapshot(L"Tone");
    SaSleepMs(500);
    SaAdmaStop();
    SaSleepMs(5);
    MmFreeContiguousMemory(Words);
    SaTrace(L"ToneResult", 2);
}

/* ------------------------------------------------- 1.1.0: TV through the HDA */

/*
 * The TV's sound goes DC-B/SOR1 over DP from the Tegra HDA controller
 * (0x70030000, Linux hda_tegra.c) and its internal HDMI/DP codec (Tegra210
 * HDMI/DP, 0x10DE0029). switchdisplay 1.6.0 sets SOR1 up for DP audio and
 * writes the codec's presence bits when the TV picture is on; this driver runs
 * the controller (CORB/RIRB polled, one output stream, no interrupt) and reads
 * the presence as the pin sense for the TV endpoint's jack.
 *
 * The controller is not in the DSDT: it is mapped directly, after its clocks
 * are on (reading it with the clocks off hangs the CPU). It is in the SOR
 * power partition, which the built-in panel (DSI) keeps on.
 *
 * Tegra quirks (hda_tegra.c): registers only as 32-bit accesses, CORBRP reset
 * clears itself, no BDL entry may cross a 4 KB boundary.
 */
#define HDA_PHYS                 0x70030000ull
#define HDA_LENGTH               0x10000u
#define HDA_IPFS_CONFIG          0x180u
#define HDA_IPFS_FPCI_BAR0       0x080u
#define HDA_IPFS_INTR_MASK       0x188u
#define HDA_CFG_CMD              0x1004u
#define HDA_CFG_BAR0             0x1010u
#define HDA_REG                  0x8000u          /* the HDA registers (BAR0) */
#define HDA_GCAP                 0x00u
#define HDA_GCTL                 0x08u
#define HDA_WAKEEN_STATESTS      0x0Cu
#define HDA_INTCTL               0x20u
#define HDA_CORBLBASE            0x40u
#define HDA_CORBUBASE            0x44u
#define HDA_CORBWP_RP            0x48u
#define HDA_CORBCTL_STS_SIZE     0x4Cu
#define HDA_RIRBLBASE            0x50u
#define HDA_RIRBUBASE            0x54u
#define HDA_RIRBWP_RINTCNT       0x58u
#define HDA_RIRBCTL_STS_SIZE     0x5Cu
#define HDA_SD(n)                (0x80u + 0x20u * (n))
#define HDA_SD_CTL_STS           0x00u
#define HDA_SD_LPIB              0x04u
#define HDA_SD_CBL               0x08u
#define HDA_SD_LVI               0x0Cu
#define HDA_SD_FIFOS_FMT         0x10u
#define HDA_SD_BDPL              0x18u
#define HDA_SD_BDPU              0x1Cu
#define HDA_SD_SRST              (1u << 0)
#define HDA_SD_RUN               (1u << 1)
#define HDA_SD_STS_W1C           (0x1Cu << 24)
#define TV_STREAM_TAG            1u
#define TV_FORMAT                0x0011u          /* 48 kHz, 16 bit, 2 ch */
#define TV_BDL_MAX               256u

/* Codec verbs (HDA spec; NVIDIA scratch verbs from Linux patch_hdmi.c). */
#define V_GET_PARAM              0xF00u
#define V_GET_CONN_LIST          0xF02u
#define V_SET_CONN_SELECT        0x701u
#define V_SET_POWER              0x705u
#define V_SET_STREAM             0x706u
#define V_SET_PIN_CTL            0x707u
#define V_SET_PIN_SENSE          0x709u
#define V_GET_PIN_SENSE          0xF09u
#define V_SET_DIGI1              0x70Du
#define V_SET_CHANNELS           0x72Du
#define V_SET_DIP_INDEX          0x730u
#define V_SET_DIP_DATA           0x731u
#define V_SET_DIP_XMIT           0x732u
#define V_NV_GET_SCRATCH0        0xFA6u
#define V_NV_SCRATCH0_BYTE0      0xFA7u
#define V_NV_SCRATCH0_BYTE1      0xFA8u
#define V_NV_SCRATCH0_BYTE2      0xFA9u
#define V_NV_SCRATCH0_BYTE3      0xFAAu
#define P_VENDOR_ID              0x00u
#define P_SUBNODES               0x04u
#define P_FG_TYPE                0x05u
#define P_WIDGET_CAPS            0x09u
#define P_PIN_CAPS               0x0Cu
#define P_CONN_LEN               0x0Eu
#define TV_VERB_FAIL             0xFFFFFFFFu

typedef struct _TV_HW {
    volatile UCHAR *Base;           /* 0x70030000, HDA registers at +0x8000 */
    ULONG *Ring;                    /* one page: CORB (256 x 4) at 0, RIRB (256 x 8) at 0x800 */
    ULONG RingPhys;
    ULONG *Bdl;                     /* one page: up to 256 BDL entries */
    ULONG BdlPhys;
    ULONG CorbWp;
    ULONG RirbRp;
    ULONG Codec;                    /* codec address */
    ULONG Afg, Cvt, Pin;            /* widget NIDs */
    ULONG Sd;                       /* output stream descriptor index (= input stream count) */
    BOOLEAN Ready;                  /* codec found, widgets set up */
    BOOLEAN Registered;             /* TV endpoint registered */
    LONG Present;                   /* pin sense: TV picture on with audio */
    ULONG VerbFails;
    KGUARDED_MUTEX VerbLock;
    PKTHREAD Thread;
    KEVENT Stop;
    PVOID PortEvents;               /* IPortEvents of the TV topology port */
} TV_HW;

static TV_HW g_Tv;

static ULONG TvRd(ULONG Offset) { return READ_REGISTER_ULONG((volatile ULONG *)(g_Tv.Base + Offset)); }
static VOID TvWr(ULONG Offset, ULONG Value) { WRITE_REGISTER_ULONG((volatile ULONG *)(g_Tv.Base + Offset), Value); }
static ULONG HdaRd(ULONG Offset) { return TvRd(HDA_REG + Offset); }
static VOID HdaWr(ULONG Offset, ULONG Value) { TvWr(HDA_REG + Offset, Value); }

static VOID TvStall(ULONG Us)
{
    if (Us >= 1000 && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        SaSleepMs(Us / 1000);
    } else {
        KeStallExecutionProcessor(Us);
    }
}

/* Waits up to Ms for (HdaRd(Offset) & Mask) == Want. */
static BOOLEAN HdaPoll(ULONG Offset, ULONG Mask, ULONG Want, ULONG Ms)
{
    ULONG i;
    for (i = 0; i < Ms * 10; ++i) {
        if ((HdaRd(Offset) & Mask) == Want) return TRUE;
        KeStallExecutionProcessor(100);
    }
    return (HdaRd(Offset) & Mask) == Want;
}

/* CAR: the HDA clocks, as switchdisplay 1.6.0 (same values; on, never off).
   hda 51 MHz and hda2codec_2x 48 MHz from pll_p (Linux tegra210 init table),
   maud 102 MHz from pll_p (L4T hda_dc.c, DP), hda2hdmi a gate. */
static ULONG TvClocksOn(VOID)
{
    PHYSICAL_ADDRESS Pa;
    volatile ULONG *Car;
    ULONG Before;
    Pa.QuadPart = (LONGLONG)CAR_PHYS;
    Car = (volatile ULONG *)MmMapIoSpaceEx(Pa, 0x1000, PAGE_READWRITE | PAGE_NOCACHE);
    if (Car == NULL) return 0xFFFFFFFFu;
#define CARW(o) Car[(o) / 4]
    Before = (CARW(0x360) & ((1u << 29) | (1u << 15))) | (CARW(0x364) & 1u) | ((CARW(0x298) & (1u << 10)) ? 0x100u : 0u) |
             ((CARW(0x358) & (1u << 29)) ? 0x200u : 0u);
    if ((CARW(0x298) & (1u << 10)) == 0) {
        CARW(0x6D4) = 6u;
        CARW(0x29C) = 1u << 10;
    }
    if ((CARW(0x360) & (1u << 29)) == 0 || (CARW(0x358) & (1u << 29)) != 0) {
        CARW(0x428) = 14u;
        CARW(0x3E4) = 15u;
        CARW(0x440) = (1u << 29) | (1u << 15);
        CARW(0x448) = 1u;
        KeStallExecutionProcessor(10);
        CARW(0x434) = (1u << 29) | (1u << 15);
        CARW(0x43C) = 1u;
        KeStallExecutionProcessor(10);
    }
#undef CARW
    MmUnmapIoSpace((PVOID)Car, 0x1000);
    return Before;
}

/* One verb through CORB/RIRB, polled. PASSIVE_LEVEL, VerbLock held. */
static ULONG TvVerbLocked(ULONG Nid, ULONG Verb, ULONG Payload)
{
    ULONG Cmd, Wp, i;
    if (Verb <= 0xF) {
        Cmd = (g_Tv.Codec << 28) | (Nid << 20) | (Verb << 16) | (Payload & 0xFFFFu);   /* 4-bit verb */
    } else {
        Cmd = (g_Tv.Codec << 28) | (Nid << 20) | (Verb << 8) | (Payload & 0xFFu);
    }
    g_Tv.CorbWp = (g_Tv.CorbWp + 1) & 0xFFu;
    g_Tv.Ring[g_Tv.CorbWp] = Cmd;
    KeMemoryBarrier();
    HdaWr(HDA_CORBWP_RP, g_Tv.CorbWp);
    for (i = 0; i < 2000; ++i) {                    /* up to 200 ms */
        Wp = HdaRd(HDA_RIRBWP_RINTCNT) & 0xFFu;
        KeMemoryBarrier();
        while (g_Tv.RirbRp != Wp) {
            ULONG Res, Ext;
            g_Tv.RirbRp = (g_Tv.RirbRp + 1) & 0xFFu;
            Res = g_Tv.Ring[0x200 + g_Tv.RirbRp * 2];
            Ext = g_Tv.Ring[0x200 + g_Tv.RirbRp * 2 + 1];
            if ((Ext & 0x10u) == 0) {               /* not unsolicited */
                return Res;
            }
        }
        KeStallExecutionProcessor(100);
    }
    ++g_Tv.VerbFails;
    return TV_VERB_FAIL;
}

static ULONG TvVerb(ULONG Nid, ULONG Verb, ULONG Payload)
{
    ULONG Res;
    if (g_Tv.Base == NULL || g_Tv.Ring == NULL) return TV_VERB_FAIL;
    KeAcquireGuardedMutex(&g_Tv.VerbLock);
    Res = TvVerbLocked(Nid, Verb, Payload);
    KeReleaseGuardedMutex(&g_Tv.VerbLock);
    return Res;
}

static ULONG TvParam(ULONG Nid, ULONG Param) { return TvVerb(Nid, V_GET_PARAM, Param); }

/* Linux hda_tegra_init (FPCI wrapper), controller reset, CORB/RIRB. */
static NTSTATUS TvControllerStart(VOID)
{
    ULONG v, Statests;

    v = TvRd(HDA_IPFS_CONFIG);
    TvWr(HDA_IPFS_CONFIG, v | 1u);                                  /* EN_FPCI */
    v = TvRd(HDA_CFG_CMD);
    v &= ~(1u << 10);                                               /* interrupts not disabled */
    v |= (1u << 0) | (1u << 1) | (1u << 2) | (1u << 8);             /* IO, MEM, bus master, SERR */
    TvWr(HDA_CFG_CMD, v);
    TvWr(HDA_CFG_BAR0, 0xFFFFFFFFu);
    TvWr(HDA_CFG_BAR0, 1u << 14);
    TvWr(HDA_IPFS_FPCI_BAR0, 0x40u);
    TvWr(HDA_IPFS_INTR_MASK, TvRd(HDA_IPFS_INTR_MASK) | (1u << 16));

    SaTrace(L"TvGcap", HdaRd(HDA_GCAP));
    HdaWr(HDA_INTCTL, 0);                                           /* polled: no interrupts */
    HdaWr(HDA_CORBCTL_STS_SIZE, 0);                                 /* CORB and RIRB DMA off */
    HdaWr(HDA_RIRBCTL_STS_SIZE, 0);
    (VOID)HdaPoll(HDA_CORBCTL_STS_SIZE, 2u, 0, 10);
    (VOID)HdaPoll(HDA_RIRBCTL_STS_SIZE, 2u, 0, 10);
    HdaWr(HDA_GCTL, 0);                                             /* controller reset */
    if (!HdaPoll(HDA_GCTL, 1u, 0, 100)) { SaTrace(L"TvStage", 0x21); return STATUS_DEVICE_NOT_READY; }
    TvStall(1000);
    HdaWr(HDA_GCTL, 1u);
    if (!HdaPoll(HDA_GCTL, 1u, 1u, 100)) { SaTrace(L"TvStage", 0x22); return STATUS_DEVICE_NOT_READY; }
    TvStall(10000);                                                 /* codecs signal within 521 us; wait more */
    Statests = HdaRd(HDA_WAKEEN_STATESTS) >> 16;
    SaTrace(L"TvStatests", Statests);
    if ((Statests & 0x7FFFu) == 0) { SaTrace(L"TvStage", 0x23); return STATUS_NO_SUCH_DEVICE; }
    for (g_Tv.Codec = 0; (Statests & (1u << g_Tv.Codec)) == 0; ++g_Tv.Codec) {}
    g_Tv.Sd = (HdaRd(HDA_GCAP) >> 8) & 0xFu;                        /* output streams come after the inputs */

    HdaWr(HDA_CORBLBASE, g_Tv.RingPhys);
    HdaWr(HDA_CORBUBASE, 0);
    HdaWr(HDA_CORBCTL_STS_SIZE, 2u << 16);                          /* 256 entries */
    HdaWr(HDA_CORBWP_RP, 1u << 31);                                 /* CORBRP reset */
    (VOID)HdaPoll(HDA_CORBWP_RP, 0xFFFF0000u, 0, 10);               /* clears itself on Tegra */
    HdaWr(HDA_CORBWP_RP, 0);
    g_Tv.CorbWp = 0;
    HdaWr(HDA_RIRBLBASE, g_Tv.RingPhys + 0x800u);
    HdaWr(HDA_RIRBUBASE, 0);
    HdaWr(HDA_RIRBCTL_STS_SIZE, (2u << 16) | (0x05u << 8));         /* 256 entries, status cleared */
    HdaWr(HDA_RIRBWP_RINTCNT, (1u << 16) | (1u << 15));             /* RINTCNT 1, WP reset */
    g_Tv.RirbRp = 0;
    HdaWr(HDA_CORBCTL_STS_SIZE, (2u << 16) | 2u);                   /* CORB run */
    HdaWr(HDA_RIRBCTL_STS_SIZE, (2u << 16) | 2u);                   /* RIRB DMA on */
    SaTrace(L"TvCorbCtl", HdaRd(HDA_CORBCTL_STS_SIZE));
    return STATUS_SUCCESS;
}

/* Finds the audio function group, the first digital output converter and the first pin. */
static NTSTATUS TvCodecStart(VOID)
{
    ULONG Sub, Start, Count, Nid, Caps, Type, Conn, i, Index = 0;
    ULONG Vendor = TvParam(0, P_VENDOR_ID);
    SaTrace(L"TvVendor", Vendor);
    if (Vendor == TV_VERB_FAIL) { SaTrace(L"TvStage", 0x24); return STATUS_IO_TIMEOUT; }
    Sub = TvParam(0, P_SUBNODES);
    g_Tv.Afg = 0;
    for (Nid = (Sub >> 16) & 0xFFu, Count = Sub & 0xFFu; Count != 0; ++Nid, --Count) {
        if ((TvParam(Nid, P_FG_TYPE) & 0xFFu) == 1u) { g_Tv.Afg = Nid; break; }
    }
    if (g_Tv.Afg == 0) { SaTrace(L"TvStage", 0x25); return STATUS_NO_SUCH_DEVICE; }
    (VOID)TvVerb(g_Tv.Afg, V_SET_POWER, 0);                         /* D0 */
    TvStall(10000);
    Sub = TvParam(g_Tv.Afg, P_SUBNODES);
    Start = (Sub >> 16) & 0xFFu;
    Count = Sub & 0xFFu;
    g_Tv.Cvt = 0;
    g_Tv.Pin = 0;
    for (Nid = Start; Nid < Start + Count; ++Nid) {
        Caps = TvParam(Nid, P_WIDGET_CAPS);
        Type = (Caps >> 20) & 0xFu;
        if (Nid - Start < 8) {
            WCHAR Name[16];
            if (NT_SUCCESS(RtlStringCchPrintfW(Name, ARRAYSIZE(Name), L"TvWidget%02X", Nid))) SaTrace(Name, Caps);
        }
        if (Type == 0 && g_Tv.Cvt == 0) g_Tv.Cvt = Nid;
        if (Type == 4 && g_Tv.Pin == 0) g_Tv.Pin = Nid;
    }
    SaTrace(L"TvAfg", (g_Tv.Afg << 16) | Count);
    SaTrace(L"TvNids", (g_Tv.Cvt << 8) | g_Tv.Pin);
    if (g_Tv.Cvt == 0 || g_Tv.Pin == 0) { SaTrace(L"TvStage", 0x26); return STATUS_NO_SUCH_DEVICE; }

    /* Pin: pick the converter in its connection list (short form, 4 per verb). */
    Conn = TvParam(g_Tv.Pin, P_CONN_LEN) & 0x7Fu;
    for (i = 0; i < Conn && i < 16; i += 4) {
        ULONG List = TvVerb(g_Tv.Pin, V_GET_CONN_LIST, i), k;
        for (k = 0; k < 4 && i + k < Conn; ++k) {
            if (((List >> (8 * k)) & 0xFFu) == g_Tv.Cvt) Index = i + k;
        }
    }
    SaTrace(L"TvConn", (Conn << 8) | Index);
    if (Conn > 1) (VOID)TvVerb(g_Tv.Pin, V_SET_CONN_SELECT, Index);
    (VOID)TvVerb(g_Tv.Cvt, V_SET_POWER, 0);
    (VOID)TvVerb(g_Tv.Pin, V_SET_POWER, 0);
    (VOID)TvVerb(g_Tv.Pin, V_SET_PIN_CTL, 0x40u);                   /* out enable */
    (VOID)TvVerb(g_Tv.Cvt, V_SET_CHANNELS, 1u);                     /* 2 channels */
    (VOID)TvVerb(g_Tv.Cvt, V_SET_DIGI1, 1u);                        /* digital out enable */
    (VOID)TvVerb(g_Tv.Cvt, 0x2u, TV_FORMAT);                        /* SET_CONVERTER_FORMAT (4-bit verb) */
    SaTrace(L"TvPinCaps", TvParam(g_Tv.Pin, P_PIN_CAPS));
    /* The DP audio infoframe (Linux hdmi_setup_audio_infoframe, DP form): 2 ch, CA 0. */
    {
        static const UCHAR Ai[8] = { 0x84, 0x1B, 0x11 << 2, 0x01, 0x00, 0x00, 0x00, 0x00 };  /* type, len, ver, CC = 2 ch - 1 */
        (VOID)TvVerb(g_Tv.Pin, V_SET_DIP_INDEX, 0);
        for (i = 0; i < sizeof(Ai); ++i) (VOID)TvVerb(g_Tv.Pin, V_SET_DIP_DATA, Ai[i]);
        (VOID)TvVerb(g_Tv.Pin, V_SET_DIP_XMIT, 0xC0u);              /* send at every frame */
    }
    g_Tv.Ready = TRUE;
    return STATUS_SUCCESS;
}

/* The format the stream uses, for SOR1 (Linux tegra_hdmi_set_format); 0 = stopped. */
static VOID TvScratchFormat(ULONG Format)
{
    ULONG v = (TvVerb(g_Tv.Afg, V_NV_GET_SCRATCH0, 0) >> 24) & 0xFFu;
    (VOID)TvVerb(g_Tv.Afg, V_NV_SCRATCH0_BYTE0, Format & 0xFFu);
    (VOID)TvVerb(g_Tv.Afg, V_NV_SCRATCH0_BYTE1, (Format >> 8) & 0xFFu);
    (VOID)TvVerb(g_Tv.Afg, V_NV_SCRATCH0_BYTE2, 0);
    if (Format == 0) v &= ~0x40u; else v |= 0x40u;                  /* valid */
    v ^= 0x80u;                                                     /* trigger */
    (VOID)TvVerb(g_Tv.Afg, V_NV_SCRATCH0_BYTE3, v);
}

/* 1.1.3: switchdisplay 1.6.1 keeps its TV audio state in its registry
   (6 = on, reset to 0 at its start). */
static ULONG TvDisplayState(VOID)
{
    RTL_QUERY_REGISTRY_TABLE Table[2];
    ULONG Value = 0;
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)L"TvAudioState";
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_NONE;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, (PWSTR)L"switchdisplay\\Parameters", Table, NULL, NULL))) {
        return 0;
    }
    return Value;
}

static ULONG g_TvSenseRaw;

/* Pin sense: bit 31 presence (SOR1 HDA_PRESENCE.PD from switchdisplay).
   1.1.3: or switchdisplay says TV audio is on (the pin sense stayed 0 after
   an unplug and re-plug with 1.1.2). */
static BOOLEAN TvSense(VOID)
{
    ULONG Sense;
    if ((TvParam(g_Tv.Pin, P_PIN_CAPS) & (1u << 1)) != 0) {         /* trigger required */
        (VOID)TvVerb(g_Tv.Pin, V_SET_PIN_SENSE, 0);
        TvStall(1000);
    }
    Sense = TvVerb(g_Tv.Pin, V_GET_PIN_SENSE, 0);
    g_TvSenseRaw = Sense;
    if (Sense != TV_VERB_FAIL && (Sense & 0x80000000u) != 0) return TRUE;
    return TvDisplayState() == 6;
}

/* Stream descriptor: reset, BDL over the buffer (no entry across 4 KB), format. */
static BOOLEAN TvStreamSetup(ULONG Phys, ULONG Size)
{
    ULONG Sd = HDA_SD(g_Tv.Sd), n = 0, Off = 0;
    HdaWr(Sd + HDA_SD_CTL_STS, HDA_SD_STS_W1C);                     /* stopped, status cleared */
    HdaWr(Sd + HDA_SD_CTL_STS, HDA_SD_SRST);
    if (!HdaPoll(Sd + HDA_SD_CTL_STS, HDA_SD_SRST, HDA_SD_SRST, 10)) return FALSE;
    HdaWr(Sd + HDA_SD_CTL_STS, 0);
    if (!HdaPoll(Sd + HDA_SD_CTL_STS, HDA_SD_SRST, 0, 10)) return FALSE;
    while (Off < Size && n < TV_BDL_MAX) {
        ULONG Addr = Phys + Off;
        ULONG Len = PAGE_SIZE - (Addr & (PAGE_SIZE - 1));
        if (Len > Size - Off) Len = Size - Off;
        if (n == 0 && Len == Size) Len = Size / 2;                  /* at least two entries */
        g_Tv.Bdl[n * 4 + 0] = Addr;
        g_Tv.Bdl[n * 4 + 1] = 0;
        g_Tv.Bdl[n * 4 + 2] = Len;
        g_Tv.Bdl[n * 4 + 3] = 0;                                    /* no IOC: polled */
        Off += Len;
        ++n;
    }
    KeMemoryBarrier();
    HdaWr(Sd + HDA_SD_CBL, Size);
    HdaWr(Sd + HDA_SD_LVI, (HdaRd(Sd + HDA_SD_LVI) & 0xFFFF0000u) | (n - 1));
    HdaWr(Sd + HDA_SD_FIFOS_FMT, (TV_FORMAT << 16) | (HdaRd(Sd + HDA_SD_FIFOS_FMT) & 0xFFFFu));
    HdaWr(Sd + HDA_SD_BDPL, g_Tv.BdlPhys);
    HdaWr(Sd + HDA_SD_BDPU, 0);
    HdaWr(Sd + HDA_SD_CTL_STS, TV_STREAM_TAG << 20);
    SaTrace(L"TvBdl", n);
    return TRUE;
}

static VOID TvStreamRun(BOOLEAN Run)
{
    ULONG Sd = HDA_SD(g_Tv.Sd);
    if (!g_Tv.Ready) return;
    HdaWr(Sd + HDA_SD_CTL_STS, (TV_STREAM_TAG << 20) | (Run ? HDA_SD_RUN : 0u));
    if (!Run) (VOID)HdaPoll(Sd + HDA_SD_CTL_STS, HDA_SD_RUN, 0, 10);
}

static ULONG TvLpib(VOID) { return g_Tv.Ready ? HdaRd(HDA_SD(g_Tv.Sd) + HDA_SD_LPIB) : 0u; }

/* Start (PASSIVE): converter on the stream tag, the format to SOR1, DMA on. */
static BOOLEAN TvPlayStart(ULONG Phys, ULONG Size)
{
    if (!g_Tv.Ready) return FALSE;
    if (!TvStreamSetup(Phys, Size)) { SaTrace(L"TvStreamFail", 1); return FALSE; }
    (VOID)TvVerb(g_Tv.Cvt, 0x2u, TV_FORMAT);
    (VOID)TvVerb(g_Tv.Cvt, V_SET_STREAM, TV_STREAM_TAG << 4);
    TvScratchFormat(TV_FORMAT);
    TvStreamRun(TRUE);
    return TRUE;
}

static VOID TvPlayStop(VOID)
{
    if (!g_Tv.Ready) return;
    TvStreamRun(FALSE);
    (VOID)TvVerb(g_Tv.Cvt, V_SET_STREAM, 0);
    TvScratchFormat(0);
}

static VOID TvSnapshot(PCWSTR Prefix)
{
    ULONG Sd = HDA_SD(g_Tv.Sd);
    if (!g_Tv.Ready) return;
    SaTraceAs(Prefix, L"SdCtl", HdaRd(Sd + HDA_SD_CTL_STS));
    SaTraceAs(Prefix, L"Lpib", HdaRd(Sd + HDA_SD_LPIB));
    SaTraceAs(Prefix, L"Fifos", HdaRd(Sd + HDA_SD_FIFOS_FMT));
}

/* Parameters\TvTestTone = 1: one second of 1 kHz to the TV from the driver's
   own buffer at the next start (no Windows audio engine). Cleared afterwards. */
static VOID TvTestTone(VOID)
{
    PHYSICAL_ADDRESS Low, High, Skip;
    ULONG *Words, Index;
    static const SHORT Sine[48] = {
            0,   535,  1060,  1567,  2048,  2493,  2896,  3250,  3547,  3784,  3956,  4061,
         4096,  4061,  3956,  3784,  3547,  3250,  2896,  2493,  2048,  1567,  1060,   535,
            0,  -535, -1060, -1567, -2048, -2493, -2896, -3250, -3547, -3784, -3956, -4061,
        -4096, -4061, -3956, -3784, -3547, -3250, -2896, -2493, -2048, -1567, -1060,  -535,
    };
    SaTrace(L"TvTestTone", 0);
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Skip.QuadPart = 0;
    Words = (ULONG *)MmAllocateContiguousNodeMemory(PAGE_SIZE, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK);
    if (Words == NULL) { SaTrace(L"TvToneResult", 1); return; }
    for (Index = 0; Index < PAGE_SIZE / 4; ++Index) {               /* 1024 frames = 21.3 periods of 48 */
        SHORT s = Sine[Index % 48];
        Words[Index] = ((ULONG)(USHORT)s << 16) | (USHORT)s;
    }
    /* 1024 frames is not a whole number of sine periods: use 960 (20 periods). */
    if (TvPlayStart((ULONG)MmGetPhysicalAddress(Words).QuadPart, 960u * 4u)) {
        SaSleepMs(500);
        TvSnapshot(L"TvTone");
        SaSleepMs(500);
        TvPlayStop();
        SaTrace(L"TvToneResult", 2);
    } else {
        SaTrace(L"TvToneResult", 3);
    }
    MmFreeContiguousMemory(Words);
}

static VOID TvFree(VOID)
{
    if (g_Tv.Base != NULL && g_Tv.Ready) {
        TvStreamRun(FALSE);
        HdaWr(HDA_CORBCTL_STS_SIZE, 2u << 16);
        HdaWr(HDA_RIRBCTL_STS_SIZE, 2u << 16);
        TvStall(1000);
    }
    g_Tv.Ready = FALSE;
    if (g_Tv.Ring != NULL) { MmFreeContiguousMemory(g_Tv.Ring); g_Tv.Ring = NULL; }
    if (g_Tv.Bdl != NULL) { MmFreeContiguousMemory(g_Tv.Bdl); g_Tv.Bdl = NULL; }
    if (g_Tv.Base != NULL) { MmUnmapIoSpace((PVOID)g_Tv.Base, HDA_LENGTH); g_Tv.Base = NULL; }
}

/* Boot guard: TvPending is set (and flushed to disk) before the first HDA
   access and cleared when the codec is up. If a start never returned (the
   machine hung), the next start finds it set and leaves the HDA alone. */
static VOID TvSetPending(ULONG Value)
{
    UNICODE_STRING Name;
    OBJECT_ATTRIBUTES Oa;
    HANDLE Key;
    SaTrace(L"TvPending", Value);
    RtlInitUnicodeString(&Name, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\switchaudio\\Parameters");
    InitializeObjectAttributes(&Oa, &Name, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_WRITE, &Oa))) {
        (VOID)ZwFlushKey(Key);
        ZwClose(Key);
    }
}

/* StartDevice: everything up to a set-up codec, or nothing (the speaker still works). */
static BOOLEAN TvStart(VOID)
{
    PHYSICAL_ADDRESS Pa, Low, High, Skip;
    ULONG Pg, Enb, Rst;
    if (SaReadParam(L"TvAudioOff") != 0) { SaTrace(L"TvStage", 0x2F); return FALSE; }
    if (SaReadParam(L"TvPending") != 0) {
        SaTrace(L"TvStage", 0x2D);                  /* the last start hung: TV audio off until TvPending is 0 */
        return FALSE;
    }
    Pg = ReadPhys(PMC_PHYS + PMC_PWRGATE_STATUS);
    SaTrace(L"TvPmc", Pg);
    if ((Pg & (1u << 17)) == 0) { SaTrace(L"TvStage", 0x20); return FALSE; }   /* SOR partition off */
    TvSetPending(1);
    SaTrace(L"TvClocks", TvClocksOn());
    Enb = ReadPhys(CAR_PHYS + 0x360);
    Rst = ReadPhys(CAR_PHYS + 0x358);
    SaTrace(L"TvClockCheck", (Enb & ((1u << 29) | (1u << 15))) | ((Rst & ((1u << 29) | (1u << 15))) >> 1));
    if ((Enb & (1u << 29)) == 0 || (Rst & (1u << 29)) != 0) {
        SaTrace(L"TvStage", 0x2C);                  /* hda clock not on: do not touch it */
        TvSetPending(0);
        return FALSE;
    }
    Pa.QuadPart = (LONGLONG)HDA_PHYS;
    g_Tv.Base = (volatile UCHAR *)MmMapIoSpaceEx(Pa, HDA_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Skip.QuadPart = 0;
    g_Tv.Ring = (ULONG *)MmAllocateContiguousNodeMemory(PAGE_SIZE, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK);
    g_Tv.Bdl = (ULONG *)MmAllocateContiguousNodeMemory(PAGE_SIZE, Low, High, Skip, PAGE_READWRITE | PAGE_NOCACHE, MM_ANY_NODE_OK);
    if (g_Tv.Base == NULL || g_Tv.Ring == NULL || g_Tv.Bdl == NULL) {
        SaTrace(L"TvStage", 0x2E);
        TvFree();
        return FALSE;
    }
    RtlZeroMemory(g_Tv.Ring, PAGE_SIZE);
    RtlZeroMemory(g_Tv.Bdl, PAGE_SIZE);
    g_Tv.RingPhys = (ULONG)MmGetPhysicalAddress(g_Tv.Ring).QuadPart;
    g_Tv.BdlPhys = (ULONG)MmGetPhysicalAddress(g_Tv.Bdl).QuadPart;
    SaTrace(L"TvStage", 0x30);
    if (!NT_SUCCESS(TvControllerStart()) || !NT_SUCCESS(TvCodecStart())) {
        TvFree();
        TvSetPending(0);                            /* it failed but did not hang */
        return FALSE;
    }
    TvSetPending(0);
    g_Tv.Present = TvSense() ? 1 : 0;
    SaTrace(L"TvPresent", (ULONG)g_Tv.Present);
    SaTrace(L"TvVerbFails", g_Tv.VerbFails);
    SaTrace(L"TvStage", 0x31);
    if (SaReadParam(L"TvTestTone") != 0) {
        TvTestTone();
    }
    return TRUE;
}

/* ------------------------------------------------------------ C++ support */

void *__cdecl operator new(size_t Size, POOL_FLAGS Flags, ULONG Tag)
{
    return ExAllocatePool2(Flags, Size, Tag);
}
void __cdecl operator delete(void *P) { if (P != NULL) ExFreePoolWithTag(P, POOL_TAG); }
void __cdecl operator delete(void *P, size_t) { if (P != NULL) ExFreePoolWithTag(P, POOL_TAG); }
extern "C" int __cdecl _purecall(void) { KeBugCheckEx(0xDEADDEAD, 0x5A, 0, 0, 0); }

#define SA_NEW new (POOL_FLAG_NON_PAGED, POOL_TAG)

/* ------------------------------------------------------- filter descriptors */

static KSDATARANGE_AUDIO g_PcmRange = {
    { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_PCM),
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) },
    SA_CHANNELS, SA_BITS, SA_BITS, SA_RATE, SA_RATE
};
static PKSDATARANGE g_PcmRanges[] = { (PKSDATARANGE)&g_PcmRange };

static KSDATARANGE g_BridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO), STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)
};
static PKSDATARANGE g_BridgeRanges[] = { &g_BridgeRange };

/* Wave: pin 0 = render sink (from the audio engine), pin 1 = bridge out. */
static PCPIN_DESCRIPTOR g_WavePins[] = {
    { 1, 1, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_PcmRanges), g_PcmRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_SINK,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
};
static PCCONNECTION_DESCRIPTOR g_WaveConnections[] = { { PCFILTER_NODE, 0, PCFILTER_NODE, 1 } };
static GUID g_WaveCategories[3];
static PCFILTER_DESCRIPTOR g_WaveFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_WavePins), g_WavePins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL, ARRAYSIZE(g_WaveConnections), g_WaveConnections,
    ARRAYSIZE(g_WaveCategories), g_WaveCategories
};

/* Topology: pin 0 = bridge in (from the wave filter), pin 1 = the speaker. */
static PCPIN_DESCRIPTOR g_TopoPins[] = {
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_SPEAKER, NULL, { 0 } } },
};
static PCCONNECTION_DESCRIPTOR g_TopoConnections[] = { { PCFILTER_NODE, 0, PCFILTER_NODE, 1 } };
static GUID g_TopoCategories[2];
static PCFILTER_DESCRIPTOR g_TopoFilter = {
    0, NULL, sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_TopoPins), g_TopoPins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL, ARRAYSIZE(g_TopoConnections), g_TopoConnections,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};

/* 1.1.1: how Windows asks about the TV jack (Parameters TvJackGets etc.). */
static LONG g_TvJackCalls, g_TvJackGets, g_TvJackLast, g_TvEventAdds, g_TvEventOther;

/* 1.1.0, TV topology: pin 1 = HDMI out with a jack (presence from the codec pin sense). */
static NTSTATUS TvJackProperty(PPCPROPERTY_REQUEST Req)
{
    ULONG Need;
    InterlockedIncrement(&g_TvJackCalls);
    g_TvJackLast = (LONG)((Req->PropertyItem->Id << 8) | (Req->Verb & 0xFF) | (Req->InstanceSize << 16));
    if (Req->InstanceSize < sizeof(ULONG) || *(PULONG)Req->Instance != 1) {
        return STATUS_INVALID_PARAMETER;
    }
    if (Req->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Req->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
            PKSPROPERTY_DESCRIPTION D = (PKSPROPERTY_DESCRIPTION)Req->Value;
            D->AccessFlags = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
            D->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
            D->PropTypeSet.Set = KSPROPTYPESETID_General;
            D->PropTypeSet.Id = VT_ILLEGAL;
            D->PropTypeSet.Flags = 0;
            D->MembersListCount = 0;
            D->Reserved = 0;
            Req->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        } else if (Req->ValueSize >= sizeof(ULONG)) {
            *(PULONG)Req->Value = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET;
            Req->ValueSize = sizeof(ULONG);
        } else {
            return STATUS_BUFFER_TOO_SMALL;
        }
        return STATUS_SUCCESS;
    }
    if ((Req->Verb & KSPROPERTY_TYPE_GET) == 0) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    Need = sizeof(KSMULTIPLE_ITEM) +
           (Req->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2 ? sizeof(KSJACK_DESCRIPTION2) : sizeof(KSJACK_DESCRIPTION));
    if (Req->ValueSize == 0) {
        Req->ValueSize = Need;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Req->ValueSize < Need) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    {
        PKSMULTIPLE_ITEM Item = (PKSMULTIPLE_ITEM)Req->Value;
        Item->Count = 1;
        Item->Size = Need;
        if (Req->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2) {
            PKSJACK_DESCRIPTION2 J = (PKSJACK_DESCRIPTION2)(Item + 1);
            RtlZeroMemory(J, sizeof(*J));
            J->DeviceStateInfo = 0;
            J->JackCapabilities = JACKDESC2_PRESENCE_DETECT_CAPABILITY;
        } else {
            PKSJACK_DESCRIPTION J = (PKSJACK_DESCRIPTION)(Item + 1);
            RtlZeroMemory(J, sizeof(*J));
            J->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
            J->Color = 0;
            J->ConnectionType = eConnTypeOtherDigital;
            J->GeoLocation = eGeoLocHDMI;
            J->GenLocation = eGenLocPrimaryBox;
            J->PortConnection = ePortConnJack;
            J->IsConnected = g_Tv.Present != 0;
            InterlockedIncrement(&g_TvJackGets);
        }
    }
    Req->ValueSize = Need;
    return STATUS_SUCCESS;
}

static NTSTATUS TvJackEvent(PPCEVENT_REQUEST Req)
{
    if (Req->Verb == PCEVENT_VERB_ADD) {
        if (g_Tv.PortEvents == NULL || Req->EventEntry == NULL) {
            InterlockedIncrement(&g_TvEventOther);
            return STATUS_UNSUCCESSFUL;
        }
        ((PPORTEVENTS)g_Tv.PortEvents)->AddEventToEventList(Req->EventEntry);
        InterlockedIncrement(&g_TvEventAdds);
    } else {
        InterlockedAdd(&g_TvEventOther, 0x10000);
    }
    return STATUS_SUCCESS;
}

static PCPROPERTY_ITEM g_TvJackProps[] = {
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, TvJackProperty },
    { &KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION2, KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, TvJackProperty },
};
static PCEVENT_ITEM g_TvJackEvents[] = {
    { &KSEVENTSETID_PinCapsChange, KSEVENT_PINCAPS_JACKINFOCHANGE, KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT, TvJackEvent },
};
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(g_TvJackAutomation, g_TvJackProps, g_TvJackEvents);

static PCPIN_DESCRIPTOR g_TvTopoPins[] = {
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE,
        &KSCATEGORY_AUDIO, NULL, { 0 } } },
    { 0, 0, 0, NULL,
      { 0, NULL, 0, NULL, ARRAYSIZE(g_BridgeRanges), g_BridgeRanges, KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE,
        &KSNODETYPE_HDMI_INTERFACE, NULL, { 0 } } },
};
/* 1.1.2: the jack property and event on the FILTER (as SYSVAD), with KSP_PIN
   naming pin 1. On the pin (1.1.0/1.1.1) Windows never reached the handler
   and kept the endpoint "not present". */
static PCFILTER_DESCRIPTOR g_TvTopoFilter = {
    0, &g_TvJackAutomation, sizeof(PCPIN_DESCRIPTOR), ARRAYSIZE(g_TvTopoPins), g_TvTopoPins,
    sizeof(PCNODE_DESCRIPTOR), 0, NULL, ARRAYSIZE(g_TopoConnections), g_TopoConnections,
    ARRAYSIZE(g_TopoCategories), g_TopoCategories
};

static VOID SaInitDescriptors(VOID)
{
    g_WaveCategories[0] = KSCATEGORY_AUDIO;
    g_WaveCategories[1] = KSCATEGORY_RENDER;
    g_WaveCategories[2] = KSCATEGORY_REALTIME;
    g_TopoCategories[0] = KSCATEGORY_AUDIO;
    g_TopoCategories[1] = KSCATEGORY_TOPOLOGY;
}

static BOOLEAN SaFormatOk(PKSDATAFORMAT Format)
{
    PWAVEFORMATEX Wfx;
    if (Format == NULL || Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) ||
        !IsEqualGUIDAligned(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return FALSE;
    }
    Wfx = &((PKSDATAFORMAT_WAVEFORMATEX)Format)->WaveFormatEx;
    if (Wfx->nChannels != SA_CHANNELS || Wfx->nSamplesPerSec != SA_RATE || Wfx->wBitsPerSample != SA_BITS) {
        return FALSE;
    }
    if (Wfx->wFormatTag == WAVE_FORMAT_PCM) {
        return TRUE;
    }
    if (Wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        Format->FormatSize >= sizeof(KSDATAFORMAT) + sizeof(WAVEFORMATEXTENSIBLE)) {
        PWAVEFORMATEXTENSIBLE Ext = (PWAVEFORMATEXTENSIBLE)Wfx;
        return IsEqualGUIDAligned(Ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) && Ext->Samples.wValidBitsPerSample == SA_BITS;
    }
    return FALSE;
}

/* ------------------------------------------------------------- the stream */

class CSaStream final : public IMiniportWaveRTStream {
public:
    LONG m_Ref;
    PPORTWAVERTSTREAM m_PortStream;
    PMDL m_Mdl;
    ULONG m_BufferSize;
    ULONG m_Period;
    ULONG m_Phys;
    KSSTATE m_State;
    BOOLEAN m_Started;
    ULONG m_LastXfer;
    ULONGLONG m_Periods;            /* finished periods since the start */
    LONGLONG m_LastChange;          /* performance counter at the last period change */
    LONGLONG m_Frequency;
    ULONG m_LastOffset;
    ULONG m_Runs;
    ULONG m_PosCalls;
    LONGLONG m_LastPoll;            /* performance counter at the last look at XFER_STATUS */
    ULONG m_Gaps;                   /* polls that saw more than one period finish */

    CSaStream(PPORTWAVERTSTREAM PortStream)
        : m_Ref(1), m_PortStream(PortStream), m_Mdl(NULL), m_BufferSize(0), m_Period(0), m_Phys(0),
          m_State(KSSTATE_STOP), m_Started(FALSE), m_LastXfer(0), m_Periods(0), m_LastChange(0),
          m_Frequency(1), m_LastOffset(0), m_Runs(0), m_PosCalls(0), m_LastPoll(0), m_Gaps(0)
    {
        m_PortStream->AddRef();
    }

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniportWaveRTStream)) {
            *Object = static_cast<IMiniportWaveRTStream *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) {
            KIRQL Irql;
            KeAcquireSpinLock(&g_Hw.Lock, &Irql);
            if (m_Started) {
                SaAdmaStop();
                m_Started = FALSE;
            }
            KeReleaseSpinLock(&g_Hw.Lock, Irql);
            m_PortStream->Release();
            delete this;
            SaCall(0xA);
        }
        return (ULONG)Ref;
    }

    IMP_IMiniportWaveRTStream;

    /* Called with g_Hw.Lock held. */
    VOID UpdatePeriods(LONGLONG Now)
    {
        ULONG Xfer = RdAdma(ADMA_CH_XFER_STATUS) & 0xFFFF;
        ULONG Delta = (Xfer - m_LastXfer) & 0xFFFF;
        if (Delta != 0) {
            /* Where the nominal rate puts the change, but between the polls. */
            LONGLONG Expected = m_LastChange +
                (LONGLONG)((ULONGLONG)Delta * m_Period * (ULONGLONG)m_Frequency / SA_BYTES_PER_SEC);
            if (Expected > Now) Expected = Now;
            if (Expected < m_LastPoll) Expected = m_LastPoll;
            if (Delta > 1) ++m_Gaps;
            m_Periods += Delta;
            m_LastXfer = Xfer;
            m_LastChange = Expected;
        }
        m_LastPoll = Now;
    }
};

STDMETHODIMP_(NTSTATUS) CSaStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    SaCall(2);
    return SaFormatOk(DataFormat) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CSaStream::SetState(KSSTATE State)
{
    KIRQL Irql;
    LARGE_INTEGER Freq;
    LONGLONG Now = KeQueryPerformanceCounter(&Freq).QuadPart;
    ULONG Wait;
    BOOLEAN Starting = FALSE, Stopping = FALSE;
    ULONGLONG StoppedPeriods = 0;

    SaCall(9);

    KeAcquireSpinLock(&g_Hw.Lock, &Irql);
    switch (State) {
    case KSSTATE_RUN:
        if (m_Mdl == NULL || m_Period == 0) {
            break;
        }
        if (!m_Started) {
            /* The path UEFI left running; make sure it still is. */
            WrAhub(ADMAIF_GLOBAL_ENABLE, 1);
            WrAhub(ADMAIF_TX1_ENABLE, 1);
            WrAhub(I2S1_RX_ENABLE, 1);
            WrAhub(I2S1_ENABLE, 1);
            WrAdma(ADMA_GLOBAL_CMD, 1);
            WrAdma(ADMA_CH_CMD, 0);
            WrAdma(ADMA_CH_TC, m_Period);
            WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1);
            WrAdma(ADMA_CH_LOWER_SRC, m_Phys);
            WrAdma(ADMA_CH_LOWER_TRG, 0);
            WrAdma(ADMA_CH_FIFO_CTRL, ADMA_FIFO_TX3);
            WrAdma(ADMA_CH_CONFIG, ADMA_CONFIG(SA_PERIODS));
            m_Periods = 0;
            m_LastOffset = 0;
            m_Frequency = Freq.QuadPart;
            m_LastChange = Now;
            m_LastPoll = Now;
            m_Gaps = 0;
            WrAdma(ADMA_CH_CMD, 1);
            /* XFER_STATUS restarts from 0 on the enable: read it after. */
            m_LastXfer = RdAdma(ADMA_CH_XFER_STATUS) & 0xFFFF;
            m_Started = TRUE;
            Starting = TRUE;
        } else {
            WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1);       /* pause bit off */
            m_LastChange = Now;
            m_LastPoll = Now;
        }
        break;
    case KSSTATE_PAUSE:
        if (m_Started && m_State == KSSTATE_RUN) {
            UpdatePeriods(Now);
            WrAdma(ADMA_CH_CTRL, ADMA_CTRL_TX1 | ADMA_CH_CTRL_PAUSE);
            for (Wait = 0; Wait < 100 && (RdAdma(ADMA_CH_STATUS) & ADMA_CH_STATUS_PAUSED) == 0; ++Wait) {
                KeStallExecutionProcessor(10);
            }
        }
        break;
    case KSSTATE_STOP:
    case KSSTATE_ACQUIRE:
        if (m_Started) {
            UpdatePeriods(Now);
            StoppedPeriods = m_Periods;
            Stopping = TRUE;
            SaAdmaStop();
            m_Started = FALSE;
        }
        m_Periods = 0;
        m_LastOffset = 0;
        break;
    default:
        break;
    }
    m_State = State;
    g_StateLog = (g_StateLog << 4) | ((ULONG)State & 0xF);
    KeReleaseSpinLock(&g_Hw.Lock, Irql);
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        SaTrace(L"StateLog", g_StateLog);
    }

    /* 1.0.0.2: what each start and stop looked like (KS state changes come at PASSIVE_LEVEL). */
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        if (State == KSSTATE_RUN && Starting) {
            SaTrace(L"Runs", ++m_Runs);
            SaSnapshot(L"Run0");
            SaSleepMs(100);
            SaSnapshot(L"Run100");
        } else if (State == KSSTATE_STOP || State == KSSTATE_ACQUIRE) {
            if (Stopping) {
                SaTrace(L"StopPeriods", (ULONG)StoppedPeriods);
                SaTrace(L"StopPositionCalls", m_PosCalls);
                SaTrace(L"StopGaps", m_Gaps);
                SaSnapshot(L"Stop");
            }
        }
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaStream::GetPosition(PKSAUDIO_POSITION Position)
{
    KIRQL Irql;
    ULONG Offset = 0;

    SaCall(8);
    KeAcquireSpinLock(&g_Hw.Lock, &Irql);
    ++m_PosCalls;
    if (m_Started && m_Period != 0) {
        LONGLONG Now = KeQueryPerformanceCounter(NULL).QuadPart;
        ULONGLONG Inside = 0;
        UpdatePeriods(Now);
        if (m_State == KSSTATE_RUN && m_Frequency > 0) {
            Inside = (ULONGLONG)(Now - m_LastChange) * SA_BYTES_PER_SEC / (ULONGLONG)m_Frequency;
            Inside -= Inside % SA_FRAME;
            if (Inside > m_Period - SA_FRAME) {
                Inside = m_Period - SA_FRAME;
            }
        }
        Offset = (ULONG)((m_Periods % SA_PERIODS) * m_Period + Inside);
        /* Never step back inside the same period (a late counter update). */
        if (Offset < m_LastOffset && m_LastOffset - Offset < m_Period) {
            Offset = m_LastOffset;
        }
        m_LastOffset = Offset;
    }
    KeReleaseSpinLock(&g_Hw.Lock, Irql);
    Position->PlayOffset = Offset;
    Position->WriteOffset = (Offset + m_Period) % (m_BufferSize != 0 ? m_BufferSize : 1);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaStream::AllocateAudioBuffer(ULONG RequestedSize, PMDL *AudioBufferMdl, ULONG *ActualSize,
                                                       ULONG *OffsetFromFirstPage, MEMORY_CACHING_TYPE *CacheType)
{
    PHYSICAL_ADDRESS Low, High, Phys;
    ULONG Period = (RequestedSize + SA_PERIODS - 1) / SA_PERIODS;
    PMDL Mdl;
    PVOID Va;

    SaCall(3);
    if (m_Mdl != NULL) {
        return STATUS_DEVICE_BUSY;
    }
    if (Period < SA_MIN_PERIOD) Period = SA_MIN_PERIOD;
    Period = (Period + SA_PERIOD_ALIGN - 1) & ~(SA_PERIOD_ALIGN - 1);   /* 64 bytes: whole frames too */
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;                  /* ADMA takes 32-bit addresses */
    Mdl = m_PortStream->AllocateContiguousPagesForMdl(Low, High, (SIZE_T)Period * SA_PERIODS);
    if (Mdl == NULL) {
        SaTrace(L"BufferFail", Period * SA_PERIODS);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Phys = m_PortStream->GetPhysicalPageAddress(Mdl, 0);
    Va = m_PortStream->MapAllocatedPages(Mdl, MmWriteCombined);
    if (Va != NULL) {
        RtlZeroMemory(Va, (SIZE_T)Period * SA_PERIODS);
        m_PortStream->UnmapAllocatedPages(Va, Mdl);
    }
    m_Mdl = Mdl;
    m_Period = Period;
    m_BufferSize = Period * SA_PERIODS;
    m_Phys = (ULONG)Phys.QuadPart;
    *AudioBufferMdl = Mdl;
    *ActualSize = m_BufferSize;
    *OffsetFromFirstPage = 0;
    *CacheType = MmWriteCombined;
    SaTrace(L"Buffers", (ULONG)InterlockedIncrement(&g_Buffers));
    SaTrace(L"Requested", RequestedSize);
    SaTrace(L"BufferSize", m_BufferSize);
    SaTrace(L"BufferPhys", m_Phys);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CSaStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(BufferSize);
    SaCall(4);
    if (AudioBufferMdl != NULL) {
        KIRQL Irql;
        KeAcquireSpinLock(&g_Hw.Lock, &Irql);
        if (m_Started) {
            SaAdmaStop();
            m_Started = FALSE;
        }
        KeReleaseSpinLock(&g_Hw.Lock, Irql);
        m_PortStream->FreePagesFromMdl(AudioBufferMdl);
    }
    m_Mdl = NULL;
    m_Period = 0;
    m_BufferSize = 0;
}

STDMETHODIMP_(VOID) CSaStream::GetHWLatency(KSRTAUDIO_HWLATENCY *hwLatency)
{
    SaCall(5);
    /* ADMA FIFO plus ADMAIF, and the position moving in period steps. */
    hwLatency->FifoSize = m_Period > 768 ? m_Period : 768;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CSaStream::GetPositionRegister(KSRTAUDIO_HWREGISTER *Register)
{
    SaCall(6);
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CSaStream::GetClockRegister(KSRTAUDIO_HWREGISTER *Register)
{
    SaCall(7);
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

/* ------------------------------------------------------ 1.1.0: TV stream */

/* The HDA stream reads the WaveRT buffer through its BDL; LPIB is the exact
   position. SetState comes at PASSIVE_LEVEL (WaveRT), so codec verbs are fine. */
class CTvStream final : public IMiniportWaveRTStream {
public:
    LONG m_Ref;
    PPORTWAVERTSTREAM m_PortStream;
    PMDL m_Mdl;
    ULONG m_BufferSize;
    ULONG m_Phys;
    KSSTATE m_State;
    BOOLEAN m_Started;
    ULONG m_Runs;

    CTvStream(PPORTWAVERTSTREAM PortStream)
        : m_Ref(1), m_PortStream(PortStream), m_Mdl(NULL), m_BufferSize(0), m_Phys(0), m_State(KSSTATE_STOP),
          m_Started(FALSE), m_Runs(0)
    {
        m_PortStream->AddRef();
    }

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniportWaveRTStream)) {
            *Object = static_cast<IMiniportWaveRTStream *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) {
            if (m_Started) {
                if (KeGetCurrentIrql() == PASSIVE_LEVEL) TvPlayStop(); else TvStreamRun(FALSE);
                m_Started = FALSE;
            }
            m_PortStream->Release();
            delete this;
        }
        return (ULONG)Ref;
    }

    IMP_IMiniportWaveRTStream;
};

STDMETHODIMP_(NTSTATUS) CTvStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    return SaFormatOk(DataFormat) ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CTvStream::SetState(KSSTATE State)
{
    switch (State) {
    case KSSTATE_RUN:
        if (m_Mdl == NULL) break;
        if (!m_Started) {
            m_Started = TvPlayStart(m_Phys, m_BufferSize);
            SaTrace(L"TvRuns", ++m_Runs);
            if (m_Started) {
                TvSnapshot(L"TvRun0");
                SaSleepMs(100);
                TvSnapshot(L"TvRun100");
            }
        } else {
            TvStreamRun(TRUE);
        }
        break;
    case KSSTATE_PAUSE:
        if (m_Started && m_State == KSSTATE_RUN) TvStreamRun(FALSE);
        break;
    case KSSTATE_STOP:
    case KSSTATE_ACQUIRE:
        if (m_Started) {
            TvSnapshot(L"TvStop");
            TvPlayStop();
            m_Started = FALSE;
        }
        break;
    default:
        break;
    }
    m_State = State;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CTvStream::GetPosition(PKSAUDIO_POSITION Position)
{
    ULONG Offset = 0;
    if (m_Started && m_BufferSize != 0) {
        Offset = TvLpib() % m_BufferSize;
        Offset -= Offset % SA_FRAME;
    }
    Position->PlayOffset = Offset;
    Position->WriteOffset = m_BufferSize != 0 ? (Offset + 512u) % m_BufferSize : 0;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CTvStream::AllocateAudioBuffer(ULONG RequestedSize, PMDL *AudioBufferMdl, ULONG *ActualSize,
                                                       ULONG *OffsetFromFirstPage, MEMORY_CACHING_TYPE *CacheType)
{
    PHYSICAL_ADDRESS Low, High;
    ULONG Size = (RequestedSize + 127u) & ~127u;    /* HDA: 128-byte units, whole frames */
    PMDL Mdl;
    PVOID Va;
    if (m_Mdl != NULL) return STATUS_DEVICE_BUSY;
    if (Size < 2048u) Size = 2048u;
    Low.QuadPart = 0;
    High.QuadPart = 0xFFFFFFFFLL;
    Mdl = m_PortStream->AllocateContiguousPagesForMdl(Low, High, Size);
    if (Mdl == NULL) {
        SaTrace(L"TvBufferFail", Size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Va = m_PortStream->MapAllocatedPages(Mdl, MmWriteCombined);
    if (Va != NULL) {
        RtlZeroMemory(Va, Size);
        m_PortStream->UnmapAllocatedPages(Va, Mdl);
    }
    m_Mdl = Mdl;
    m_BufferSize = Size;
    m_Phys = (ULONG)m_PortStream->GetPhysicalPageAddress(Mdl, 0).QuadPart;
    *AudioBufferMdl = Mdl;
    *ActualSize = Size;
    *OffsetFromFirstPage = 0;
    *CacheType = MmWriteCombined;
    SaTrace(L"TvRequested", RequestedSize);
    SaTrace(L"TvBufferSize", Size);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CTvStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize)
{
    UNREFERENCED_PARAMETER(BufferSize);
    if (AudioBufferMdl != NULL) {
        if (m_Started) {
            TvPlayStop();
            m_Started = FALSE;
        }
        m_PortStream->FreePagesFromMdl(AudioBufferMdl);
    }
    m_Mdl = NULL;
    m_BufferSize = 0;
}

STDMETHODIMP_(VOID) CTvStream::GetHWLatency(KSRTAUDIO_HWLATENCY *hwLatency)
{
    ULONG Fifo = g_Tv.Ready ? (HdaRd(HDA_SD(g_Tv.Sd) + HDA_SD_FIFOS_FMT) & 0xFFFFu) + 1u : 256u;
    hwLatency->FifoSize = Fifo;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CTvStream::GetPositionRegister(KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CTvStream::GetClockRegister(KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

/* --------------------------------------------------------- wave miniport */

class CSaWave final : public IMiniportWaveRT {
public:
    LONG m_Ref;
    LONG m_Streams;
    BOOLEAN m_Tv;
    CSaWave(BOOLEAN Tv) : m_Ref(1), m_Streams(0), m_Tv(Tv) {}

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniport) ||
            IsEqualGUIDAligned(Iid, IID_IMiniportWaveRT)) {
            *Object = static_cast<IMiniportWaveRT *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) delete this;
        return (ULONG)Ref;
    }

    IMP_IMiniportWaveRT;
};

STDMETHODIMP_(NTSTATUS) CSaWave::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &g_WaveFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaWave::DataRangeIntersection(ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
                                                       ULONG OutputBufferLength, PVOID ResultantFormat,
                                                       PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;                 /* PortCls's default handler */
}

STDMETHODIMP_(NTSTATUS) CSaWave::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVERT Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaWave::NewStream(PMINIPORTWAVERTSTREAM *Stream, PPORTWAVERTSTREAM PortStream, ULONG Pin,
                                           BOOLEAN Capture, PKSDATAFORMAT DataFormat)
{
    CSaStream *New;
    *Stream = NULL;
    if (Capture || Pin != 0 || !SaFormatOk(DataFormat)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (m_Tv) {
        CTvStream *Tv = SA_NEW CTvStream(PortStream);
        if (Tv == NULL) return STATUS_INSUFFICIENT_RESOURCES;
        *Stream = Tv;
        SaTrace(L"TvStreams", (ULONG)InterlockedIncrement(&m_Streams));
        return STATUS_SUCCESS;
    }
    New = SA_NEW CSaStream(PortStream);
    if (New == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *Stream = New;
    SaCall(1);
    SaTrace(L"Streams", (ULONG)InterlockedIncrement(&m_Streams));
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaWave::GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Version = DEVICE_DESCRIPTION_VERSION;
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = FALSE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PNPBus;
    DeviceDescription->MaximumLength = 0x10000;
    return STATUS_SUCCESS;
}

/* ----------------------------------------------------- topology miniport */

class CSaTopo final : public IMiniportTopology {
public:
    LONG m_Ref;
    BOOLEAN m_Tv;
    CSaTopo(BOOLEAN Tv) : m_Ref(1), m_Tv(Tv) {}

    STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID Iid, PVOID *Object)
    {
        if (IsEqualGUIDAligned(Iid, IID_IUnknown) || IsEqualGUIDAligned(Iid, IID_IMiniport) ||
            IsEqualGUIDAligned(Iid, IID_IMiniportTopology)) {
            *Object = static_cast<IMiniportTopology *>(this);
            AddRef();
            return STATUS_SUCCESS;
        }
        *Object = NULL;
        return STATUS_INVALID_PARAMETER;
    }
    STDMETHODIMP_(ULONG) AddRef() { return (ULONG)InterlockedIncrement(&m_Ref); }
    STDMETHODIMP_(ULONG) Release()
    {
        LONG Ref = InterlockedDecrement(&m_Ref);
        if (Ref == 0) delete this;
        return (ULONG)Ref;
    }

    IMP_IMiniportTopology;
};

STDMETHODIMP_(NTSTATUS) CSaTopo::GetDescription(PPCFILTER_DESCRIPTOR *Description)
{
    *Description = m_Tv ? &g_TvTopoFilter : &g_TopoFilter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CSaTopo::DataRangeIntersection(ULONG PinId, PKSDATARANGE DataRange, PKSDATARANGE MatchingDataRange,
                                                       ULONG OutputBufferLength, PVOID ResultantFormat,
                                                       PULONG ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    UNREFERENCED_PARAMETER(ResultantFormatLength);
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS) CSaTopo::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTTOPOLOGY Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    if (m_Tv && g_Tv.PortEvents == NULL) {
        PVOID Events = NULL;
        NTSTATUS Status = Port->QueryInterface(IID_IPortEvents, &Events);
        SaTrace(L"TvPortEvents", (ULONG)Status);
        if (NT_SUCCESS(Status)) g_Tv.PortEvents = Events;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------- 1.1.0: TV jack */

/* Reads the pin sense every 500 ms; a change goes to Windows as a jack event. */
static VOID TvJackThread(PVOID Context)
{
    LARGE_INTEGER Period;
    ULONG Changes = 0, Loops = 0, Shown = 0xFFFFFFFFu;
    UNREFERENCED_PARAMETER(Context);
    Period.QuadPart = -500LL * 10000;
    while (KeWaitForSingleObject(&g_Tv.Stop, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT) {
        LONG Now = TvSense() ? 1 : 0;
        BOOLEAN Kick = FALSE;
        ++Loops;
        /* 1.1.1: TvTone.cmd sets TvTestTone = 1; play it here, no device restart needed. */
        if ((Loops & 1) == 0 && SaReadParam(L"TvTestTone") != 0) {
            TvTestTone();
        }
        /* 1.1.1: tell Windows again 5 s and 30 s after the start, in case it asked
           before switchdisplay set the presence and missed the change. */
        if (Loops == 10 || Loops == 60) Kick = TRUE;
        {
            ULONG Sum = (ULONG)g_TvJackCalls + ((ULONG)g_TvJackGets << 8) + ((ULONG)g_TvEventAdds << 16) + (ULONG)g_TvEventOther;
            if (Sum != Shown) {
                Shown = Sum;
                SaTrace(L"TvJackCalls", (ULONG)g_TvJackCalls);
                SaTrace(L"TvJackGets", (ULONG)g_TvJackGets);
                SaTrace(L"TvJackLast", (ULONG)g_TvJackLast);
                SaTrace(L"TvEventAdds", (ULONG)g_TvEventAdds);
                SaTrace(L"TvEventOther", (ULONG)g_TvEventOther);
            }
        }
        if (Kick && g_Tv.PortEvents != NULL) {
            ((PPORTEVENTS)g_Tv.PortEvents)->GenerateEventList((GUID *)&KSEVENTSETID_PinCapsChange,
                                                             KSEVENT_PINCAPS_JACKINFOCHANGE, FALSE, (ULONG)-1, FALSE, (ULONG)-1);
            SaTrace(L"TvKicks", Loops);
        }
        if (Now != g_Tv.Present) {
            InterlockedExchange(&g_Tv.Present, Now);
            SaTrace(L"TvPresent", (ULONG)Now);
            SaTrace(L"TvSenseRaw", g_TvSenseRaw);
            SaTrace(L"TvDisplayState", TvDisplayState());
            SaTrace(L"TvJackChanges", ++Changes);
            if (g_Tv.PortEvents != NULL) {
                ((PPORTEVENTS)g_Tv.PortEvents)->GenerateEventList((GUID *)&KSEVENTSETID_PinCapsChange,
                                                                 KSEVENT_PINCAPS_JACKINFOCHANGE, FALSE, (ULONG)-1, FALSE, (ULONG)-1);
            }
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID TvJackStart(VOID)
{
    HANDLE Thread;
    NTSTATUS Status;
    KeClearEvent(&g_Tv.Stop);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, TvJackThread, NULL);
    if (NT_SUCCESS(Status)) {
        Status = ObReferenceObjectByHandle(Thread, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&g_Tv.Thread, NULL);
        ZwClose(Thread);
    }
    if (!NT_SUCCESS(Status)) g_Tv.Thread = NULL;
    SaTrace(L"TvJackThread", (ULONG)Status);
}

static VOID TvStopAll(VOID)
{
    if (g_Tv.Thread != NULL) {
        KeSetEvent(&g_Tv.Stop, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(g_Tv.Thread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_Tv.Thread);
        g_Tv.Thread = NULL;
    }
    if (g_Tv.PortEvents != NULL) {
        ((PPORTEVENTS)g_Tv.PortEvents)->Release();
        g_Tv.PortEvents = NULL;
    }
    TvFree();
    g_Tv.Registered = FALSE;
}

/* ---------------------------------------------------------------- adapter */

static NTSTATUS SaInstallSubdevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PCWSTR Name, REFCLSID PortClsid,
                                   PUNKNOWN Miniport, PRESOURCELIST ResourceList, PPORT *PortOut)
{
    PPORT Port = NULL;
    NTSTATUS Status = PcNewPort(&Port, PortClsid);
    if (NT_SUCCESS(Status)) {
        Status = Port->Init(DeviceObject, Irp, Miniport, NULL, ResourceList);
    }
    if (NT_SUCCESS(Status)) {
        Status = PcRegisterSubdevice(DeviceObject, (PWSTR)Name, Port);
    }
    if (NT_SUCCESS(Status)) {
        *PortOut = Port;
    } else if (Port != NULL) {
        Port->Release();
    }
    return Status;
}

/* ------------------------------------------------------------- headphones */

static NTSTATUS SaCodecWrite(UCHAR Reg, USHORT Value)
{
    IO_STATUS_BLOCK Iosb;
    UCHAR Buf[3] = { Reg, (UCHAR)(Value >> 8), (UCHAR)Value };
    return ZwWriteFile(g_Hw.Spb, NULL, NULL, NULL, &Iosb, Buf, sizeof(Buf), NULL, NULL);
}

static NTSTATUS SaCodecRead(UCHAR Reg, USHORT *Value)
{
    IO_STATUS_BLOCK Iosb;
    UCHAR Data[2] = { 0, 0 };
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    NTSTATUS Status;

    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Sequence.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Reg, 1);
    Sequence.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Data, 2);
    Status = ZwDeviceIoControlFile(g_Hw.Spb, NULL, NULL, NULL, &Iosb, IOCTL_SPB_EXECUTE_SEQUENCE,
                                   &Sequence, sizeof(Sequence), NULL, 0);
    *Value = (USHORT)((Data[0] << 8) | Data[1]);
    return Status;
}

static NTSTATUS SaSpbOpen(PRESOURCELIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Conn = ResourceList->FindTranslatedEntry(CmResourceTypeConnection, 0);
    WCHAR PathBuf[64];
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    NTSTATUS Status;

    if (Conn == NULL) {
        return STATUS_NOT_FOUND;
    }
    Status = RtlStringCchPrintfW(PathBuf, ARRAYSIZE(PathBuf), L"\\Device\\RESOURCE_HUB\\%016I64x",
                                 ((ULONG64)Conn->u.Connection.IdHighPart << 32) | Conn->u.Connection.IdLowPart);
    if (!NT_SUCCESS(Status)) {
        return Status;
    }
    RtlInitUnicodeString(&Path, PathBuf);
    InitializeObjectAttributes(&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    return ZwCreateFile(&g_Hw.Spb, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &Oa, &Iosb, NULL, 0, 0, FILE_OPEN,
                        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
}

/* Unmute the output the jack picks, the other one first muted. */
static NTSTATUS SaJackApply(BOOLEAN Headphones)
{
    NTSTATUS Status;
    if (Headphones) {
        Status = SaCodecWrite(RT5640_SPK_VOL, SA_SPK_VOL | SA_MUTE);
        if (NT_SUCCESS(Status)) Status = SaCodecWrite(RT5640_HP_VOL, SA_HP_VOL);
    } else {
        Status = SaCodecWrite(RT5640_HP_VOL, SA_HP_VOL | SA_MUTE);
        if (NT_SUCCESS(Status)) Status = SaCodecWrite(RT5640_SPK_VOL, SA_SPK_VOL);
    }
    return Status;
}

static VOID SaJackThread(PVOID Context)
{
    LARGE_INTEGER Period;
    ULONG Applied = 2, Last = 2, Switches = 0;

    UNREFERENCED_PARAMETER(Context);
    Period.QuadPart = -200LL * 10000;
    while (KeWaitForSingleObject(&g_Hw.JackStop, Executive, KernelMode, FALSE, &Period) == STATUS_TIMEOUT) {
        ULONG Now = (READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_IN)) & GPIO_PV6) == 0 ? 1 : 0;
        if (Now == Last && Now != Applied) {         /* two equal readings, 200 ms apart */
            NTSTATUS Status = SaJackApply(Now != 0);
            SaTrace(L"JackSpb", (ULONG)Status);
            if (NT_SUCCESS(Status)) {
                Applied = Now;
                SaTrace(L"Jack", Now);
                SaTrace(L"JackSwitches", ++Switches);
            }
        }
        Last = Now;
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID SaJackStop(VOID)
{
    if (g_Hw.JackThread != NULL) {
        KeSetEvent(&g_Hw.JackStop, IO_NO_INCREMENT, FALSE);
        (VOID)KeWaitForSingleObject(g_Hw.JackThread, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(g_Hw.JackThread);
        g_Hw.JackThread = NULL;
    }
    if (g_Hw.Spb != NULL) {
        ZwClose(g_Hw.Spb);
        g_Hw.Spb = NULL;
    }
    if (g_Hw.Gpio != NULL) {
        MmUnmapIoSpace((PVOID)g_Hw.Gpio, GPIO_LENGTH);
        g_Hw.Gpio = NULL;
    }
}

/* Only with UEFI R403 or later: the headphone path set up, PV6 a GPIO input. */
static VOID SaJackStart(PRESOURCELIST ResourceList)
{
    PHYSICAL_ADDRESS Pa;
    USHORT Hpo = 0;
    ULONG Cnf, Oe;
    HANDLE Thread;
    NTSTATUS Status;

    Status = SaSpbOpen(ResourceList);
    SaTrace(L"JackSpb", (ULONG)Status);
    if (!NT_SUCCESS(Status)) {
        g_Hw.Spb = NULL;
        return;
    }
    Status = SaCodecRead(RT5640_HPO_MIXER, &Hpo);
    SaTrace(L"HpoMixer", NT_SUCCESS(Status) ? Hpo : 0xFFFF0000u | (ULONG)(Status & 0xFFFF));
    Pa.QuadPart = (LONGLONG)GPIO_PHYS;
    g_Hw.Gpio = (volatile UCHAR *)MmMapIoSpaceEx(Pa, GPIO_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    if (!NT_SUCCESS(Status) || (Hpo & RT5640_M_HPVOL_HM) != 0 || g_Hw.Gpio == NULL) {
        SaJackStop();
        return;
    }
    Cnf = READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_CNF));
    Oe = READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_OE));
    SaTrace(L"JackGpio", (Cnf & 0xFF) | ((Oe & 0xFF) << 8) |
                         ((READ_REGISTER_ULONG((volatile ULONG *)(g_Hw.Gpio + GPIO_PV_IN)) & 0xFF) << 16));
    if ((Cnf & GPIO_PV6) == 0 || (Oe & GPIO_PV6) != 0) {
        SaJackStop();
        return;
    }
    KeClearEvent(&g_Hw.JackStop);
    Status = PsCreateSystemThread(&Thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, SaJackThread, NULL);
    if (NT_SUCCESS(Status)) {
        Status = ObReferenceObjectByHandle(Thread, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID *)&g_Hw.JackThread, NULL);
        ZwClose(Thread);
    }
    if (!NT_SUCCESS(Status)) {
        g_Hw.JackThread = NULL;
        SaJackStop();
    }
}

/* 1.1.0: the TV endpoint, only when the HDA codec came up. A failure here
   leaves the speaker alone (TvStage says where it stopped). */
static VOID TvRegister(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList)
{
    PPORT WavePort = NULL, TopoPort = NULL;
    CSaWave *Wave;
    CSaTopo *Topo;
    NTSTATUS Status;

    if (!TvStart()) return;
    Wave = SA_NEW CSaWave(TRUE);
    Topo = SA_NEW CSaTopo(TRUE);
    if (Wave == NULL || Topo == NULL) {
        if (Wave != NULL) Wave->Release();
        if (Topo != NULL) Topo->Release();
        TvStopAll();
        return;
    }
    Status = SaInstallSubdevice(DeviceObject, Irp, L"TvWave", CLSID_PortWaveRT, static_cast<IMiniportWaveRT *>(Wave),
                                ResourceList, &WavePort);
    SaTrace(L"TvWaveStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        Status = SaInstallSubdevice(DeviceObject, Irp, L"TvTopology", CLSID_PortTopology,
                                    static_cast<IMiniportTopology *>(Topo), ResourceList, &TopoPort);
        SaTrace(L"TvTopoStatus", (ULONG)Status);
    }
    if (NT_SUCCESS(Status)) {
        Status = PcRegisterPhysicalConnection(DeviceObject, WavePort, 1, TopoPort, 0);
        SaTrace(L"TvConnectStatus", (ULONG)Status);
    }
    Wave->Release();
    Topo->Release();
    if (WavePort != NULL) WavePort->Release();
    if (TopoPort != NULL) TopoPort->Release();
    if (NT_SUCCESS(Status)) {
        g_Tv.Registered = TRUE;
        SaTrace(L"TvStage", 0x32);
        TvJackStart();
    }
}

static NTSTATUS SaStartDevice(PDEVICE_OBJECT DeviceObject, PIRP Irp, PRESOURCELIST ResourceList)
{
    PCM_PARTIAL_RESOURCE_DESCRIPTOR Mem0, Mem1;
    ULONG Ape;
    PPORT WavePort = NULL, TopoPort = NULL;
    CSaWave *Wave;
    CSaTopo *Topo;
    NTSTATUS Status;

    (VOID)RtlCreateRegistryKey(RTL_REGISTRY_SERVICES, g_ParamsPath);
    SaTrace(L"Stage", 1);
    Mem0 = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, 0);
    Mem1 = ResourceList->FindTranslatedEntry(CmResourceTypeMemory, 1);
    if (Mem0 == NULL || Mem1 == NULL || Mem0->u.Memory.Length < AHUB_LENGTH || Mem1->u.Memory.Length < ADMA_LENGTH) {
        SaTrace(L"Stage", 0x10);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    Ape = SaCheckApe();
    SaTrace(L"ApeCheck", Ape);
    if (Ape != 0xF) {
        SaTrace(L"Stage", 0x11);                   /* UEFI older than R402: do not touch the AHUB */
        return STATUS_DEVICE_NOT_READY;
    }
    if (g_Hw.Ahub == NULL) {
        g_Hw.Ahub = (volatile UCHAR *)MmMapIoSpaceEx(Mem0->u.Memory.Start, AHUB_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (g_Hw.Adma == NULL) {
        g_Hw.Adma = (volatile UCHAR *)MmMapIoSpaceEx(Mem1->u.Memory.Start, ADMA_LENGTH, PAGE_READWRITE | PAGE_NOCACHE);
    }
    if (g_Hw.Ahub == NULL || g_Hw.Adma == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    SaAdmaStop();
    SaTrace(L"I2sEnable", RdAhub(I2S1_ENABLE));
    SaTrace(L"I2sCtrl", RdAhub(I2S1_CTRL));
    SaTrace(L"Xbar", RdAhub(XBAR_I2S1_RX_MUX));
    SaTrace(L"AdmaifCtrl", RdAhub(ADMAIF_TX1_ACIF_CTRL));
    if (RdAhub(I2S1_ENABLE) == 0 || RdAhub(XBAR_I2S1_RX_MUX) != 1) {
        SaTrace(L"Stage", 0x12);                   /* clocks on but the path is not set up */
        return STATUS_DEVICE_NOT_READY;
    }
    SaTrace(L"Stage", 2);
    SaSnapshot(L"Start");
    if (SaReadParam(L"TestTone") != 0) {
        SaTestTone();
    }

    Wave = SA_NEW CSaWave(FALSE);
    Topo = SA_NEW CSaTopo(FALSE);
    if (Wave == NULL || Topo == NULL) {
        if (Wave != NULL) Wave->Release();
        if (Topo != NULL) Topo->Release();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Status = SaInstallSubdevice(DeviceObject, Irp, L"Wave", CLSID_PortWaveRT, static_cast<IMiniportWaveRT *>(Wave),
                                ResourceList, &WavePort);
    SaTrace(L"WaveStatus", (ULONG)Status);
    if (NT_SUCCESS(Status)) {
        Status = SaInstallSubdevice(DeviceObject, Irp, L"Topology", CLSID_PortTopology,
                                    static_cast<IMiniportTopology *>(Topo), ResourceList, &TopoPort);
        SaTrace(L"TopoStatus", (ULONG)Status);
    }
    if (NT_SUCCESS(Status)) {
        Status = PcRegisterPhysicalConnection(DeviceObject, WavePort, 1, TopoPort, 0);
        SaTrace(L"ConnectStatus", (ULONG)Status);
    }
    Wave->Release();                               /* the ports keep their own references */
    Topo->Release();
    if (WavePort != NULL) WavePort->Release();
    if (TopoPort != NULL) TopoPort->Release();
    if (NT_SUCCESS(Status)) {
        g_Hw.Ready = TRUE;
        SaTrace(L"Stage", 3);
        SaJackStart(ResourceList);
        TvRegister(DeviceObject, Irp, ResourceList);
    }
    return Status;
}

static PDRIVER_DISPATCH g_PcPnp;

static NTSTATUS SaDispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UCHAR Minor = IoGetCurrentIrpStackLocation(Irp)->MinorFunction;
    NTSTATUS Status;
    if (Minor == IRP_MN_STOP_DEVICE || Minor == IRP_MN_REMOVE_DEVICE || Minor == IRP_MN_SURPRISE_REMOVAL) {
        KIRQL Irql;
        SaJackStop();
        TvStopAll();
        KeAcquireSpinLock(&g_Hw.Lock, &Irql);
        SaAdmaStop();
        KeReleaseSpinLock(&g_Hw.Lock, Irql);
    }
    Status = g_PcPnp(DeviceObject, Irp);
    if (Minor == IRP_MN_REMOVE_DEVICE) {
        if (g_Hw.Ahub != NULL) MmUnmapIoSpace((PVOID)g_Hw.Ahub, AHUB_LENGTH);
        if (g_Hw.Adma != NULL) MmUnmapIoSpace((PVOID)g_Hw.Adma, ADMA_LENGTH);
        g_Hw.Ahub = NULL;
        g_Hw.Adma = NULL;
        g_Hw.Ready = FALSE;
    }
    return Status;
}

static NTSTATUS SaAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
    return PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, (PCPFNSTARTDEVICE)SaStartDevice, 4, 0);
}

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    KeInitializeSpinLock(&g_Hw.Lock);
    KeInitializeEvent(&g_Hw.JackStop, NotificationEvent, FALSE);
    KeInitializeEvent(&g_Tv.Stop, NotificationEvent, FALSE);
    KeInitializeGuardedMutex(&g_Tv.VerbLock);
    SaInitDescriptors();
    Status = PcInitializeAdapterDriver(DriverObject, RegistryPath, (PDRIVER_ADD_DEVICE)SaAddDevice);
    if (NT_SUCCESS(Status)) {
        g_PcPnp = DriverObject->MajorFunction[IRP_MJ_PNP];
        DriverObject->MajorFunction[IRP_MJ_PNP] = SaDispatchPnp;
    }
    return Status;
}
