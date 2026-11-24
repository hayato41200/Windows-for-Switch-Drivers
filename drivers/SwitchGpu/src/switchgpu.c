/*
 * switchgpu.sys - GM20B GPU bring-up test driver (stage A), KMDF.
 *
 * ACPI device GPU0 (ACPI\SWOA0F01, UEFI R410), resources in this order:
 *   Memory32Fixed 0x57000000 (BAR0, PRI), Memory32Fixed 0x58000000 (BAR1),
 *   I2cSerialBus 0x1C on I2C5: MAX77621 VDD_GPU (VOUT 0x00, VOUT_DVS 0x01,
 *     CONTROL1 0x02, CONTROL2 0x03, CHIPID1 0x04, CHIPID2 0x05;
 *     606.25 mV + 6.25 mV x code, bit 7 = enable),
 *   I2cSerialBus 0x3C on I2C5: MAX77620 (GPIO6 = register 0x3C drives the
 *     MAX77621 EN pin; 0x09 = push-pull output high, 0x01 = low).
 * CAR (0x60006000), PMC (0x7000E400) and MC (0x70019000) are mapped at their
 * fixed addresses, as other Switch drivers do; only the GPU's own bits are
 * written (CAR through the SET/CLR registers).
 *
 * At every start the driver only READS: PMC_GPU_RG_CNTRL, the GPU clock and
 * reset bits, PLLP_OUTC, the fuse clock, the MC hot reset bits, the MAX77621
 * registers and MAX77620 GPIO6. Nothing is written unless the registry value
 * PowerTest is 6 or Cycle is requested. The GPU is
 * powered as L4T nvgpu gm20b_tegra_unrailgate does:
 *   VDD_GPU 1.0 V (VOUT = VOUT_DVS = 0xBF) and GPIO6 high, 2 ms;
 *   clocks: gpu gate (CLK_X 24), pllg_ref (CLK_X 29), pll_p_out5 204 MHz
 *     (PLLP_OUTC), fuse (CLK_H 7); 10 us;
 *   GPU reset assert (RST_X 24), 10 us;
 *   clamp off: PMC_GPU_RG_CNTRL (PMC + 0x2D4) = 0, 10 us;
 *   gpu clock off, reset deassert, gpu clock on;
 *   MC flush (MC + 0x970 bit 2, wait MC + 0x974 bit 2), 10 us, flush done;
 * then NV_PMC_BOOT_0 (BAR0 + 0), PTIMER (BAR0 + 0x9400 / 0x9410) and the WPR the
 * GPU sees (NV_PFB 0x100CD4, index 2 / 3, as nouveau's gm200_acr_wpr_check) are read,
 * BAR0 only if the clamp is off, the clock on and the reset low. E1 retains
 * the GPU on success. Resident controls use admin-only device IOCTLs.
 *
 * Before each step "Step" is written and the key flushed to disk, so after a
 * hang and a restart Status.cmd shows where it stopped.
 * Trace: HKLM\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>
#include "swgpu-blit.h"

#define SWGPU_VERSION      0x00000D00u   /* 0.13.0 */

#define CAR_PHYS           0x60006000ull
#define CAR_LEN            0x1000u
#define PMC_PHYS           0x7000E000ull /* page; registers at + 0x400 */
#define PMC_LEN            0x1000u
#define MC_PHYS            0x70019000ull
#define MC_LEN             0x1000u
#define BAR0_MAP_LEN       0x1000000u   /* Claude: full 16 MB BAR0 (ACPI); GR/FECS are at 0x400000+ */
#define BAR1_D3_LOW_VA     0x00100000u
#define BAR1_D3_MAP_LEN    (BAR1_D3_LOW_VA + PAGE_SIZE)

#define CAR_CLK_OUT_ENB_H  0x014
#define CAR_CLK_ENB_H_SET  0x328
#define CAR_CLK_ENB_H_CLR  0x32C
#define CAR_CLK_OUT_ENB_X  0x280
#define CAR_CLK_ENB_X_SET  0x284
#define CAR_CLK_ENB_X_CLR  0x288
#define CAR_RST_DEVICES_X  0x28C
#define CAR_RST_DEV_X_SET  0x290
#define CAR_RST_DEV_X_CLR  0x294
#define CAR_PLLP_OUTC      0x67C
#define CLK_X_GPU          (1u << 24)
#define CLK_X_PLLG_REF     (1u << 29)
#define CLK_H_FUSE         (1u << 7)
#define PLLP_OUT5_RSTN     (1u << 16)
#define PLLP_OUT5_CLKEN    (1u << 17)
#define PLLP_OUT5_OVRRIDE  (1u << 18)
#define PLLP_OUT5_204MHZ   (2u << 24)    /* 7.1 fixed point: 2 -> /2 of PLLP 408 MHz */

#define PMC_GPU_RG_CNTRL   (0x400 + 0x2D4)
#define MC_HOTRESET_CTRL_1 0x970
#define MC_HOTRESET_STAT_1 0x974
#define MC_HOTRESET_GPU    (1u << 2)

#define MAX77621_VOUT      0x00
#define MAX77621_VOUT_DVS  0x01
#define MAX77621_CONTROL1  0x02
#define MAX77621_CONTROL2  0x03
#define MAX77621_CHIPID1   0x04
#define MAX77621_CHIPID2   0x05
#define VDD_GPU_1000MV     0xBFu         /* enable | (1000000 - 606250) / 6250 = 63 */
#define MAX77620_GPIO6     0x3C
#define GPIO_PUSHPULL_HIGH 0x09u

#define NV_PMC_BOOT_0      0x0000
#define NV_PMC_ENABLE      0x0200
#define NV_PTIMER_TIME_0   0x9400
#define NV_PTIMER_TIME_1   0x9410
/* The WPR the GPU sees (from the MC carveouts sent with SEND_CFG_TO_GPU):
 * write index 2 -> start >> 8 in [31:8], index 3 -> limit (nouveau gm200_acr_wpr_check). */
#define NV_PFB_MMU_WPR_INFO 0x100CD4
#define NV_PTOP_DEVICE_INFO 0x022700    /* 64 entries (nouveau gk104_top_parse) */
#define PMU_FALCON         0x10A000
#define FALCON_MAILBOX0    0x040
#define FALCON_MAILBOX1    0x044
#define FALCON_CPUCTL      0x100
#define FALCON_HWCFG       0x108       /* IMEM = ([8:0]) << 8, DMEM = ([17:9]) << 8 */
#define FALCON_DMACTL      0x10C
#define FALCON_ITFEN       0x048       /* bit 0: external interface on (nouveau sets it before DMA) */
#define FALCON_SCTL        0x240
#define FALCON_DMEMC0      0x1C0       /* BIT24 auto-increment write, BIT25 read */
#define FALCON_DMEMD0      0x1C4
#define PMU_DEBUG          0xC08       /* bit 20 = debug-fused (nouveau gm200_pmu_flcn.debug) */
/* Falcon DMA registers used by ACR and its diagnostics. */
#define FALCON_DMATRFBASE  0x110       /* address >> 8 */
#define FALCON_DMATRFMOFFS 0x114       /* DMEM/IMEM offset */
#define FALCON_DMATRFCMD   0x118       /* IDLE bit 1, IMEM bit 4, WRITE bit 5 (falcon -> memory), SIZE [10:8], CTXDMA [14:12] */
#define FALCON_DMATRFFBOFFS 0x11C      /* offset from DMATRFBASE */
#define FALCON_BIND_TRIGGER 0x200      /* PMU bind status query trigger */
#define FALCON_BIND_STATUS  0x20C      /* bind state in [14:12], 5 = bound */
#define FALCON_BIND_INST    0x480      /* instance-block address/target */
#define FALCON_CTX_ENABLE   0x090      /* bit 16 enables context binding */
#define FALCON_CTX_ACTION   0x058      /* bit 1: commit pending context bind */
#define FALCON_IMEMC0       0x180
#define FALCON_IMEMD0       0x184
#define FALCON_IMEMT0       0x188
#define FALCON_IMEMC_AINCR  (1u << 25)
#define PMU_UCODE_DMA_IDX  0u          /* GK20A_PMU_DMAIDX_UCODE: physical local FB */
/* PMU FBIF at + 0xE00 (nouveau gm200_pmu_flcn_bind_inst); TRANSCFG(i) = + 4 i:
 * TARGET [1:0] (2 = noncoherent sysmem), MEM_TYPE bit 2 (1 = physical);
 * FBIF_CTL + 0x24 bit 7 = ALLOW_PHYS_NO_CTX (dev_fbif_v4.h). */
#define PMU_FBIF_TRANSCFG(i) (0xE00 + (i) * 4)
#define PMU_FBIF_CTL       0xE24
/* PMC_ENABLE units of the memory path (nvgpu hw_mc_gm20b: xbar bit 2, l2 bit 3,
 * pfb bit 20, hub bit 29; nvgpu gk20a_fb_reset resets these four together). */
/* Tegra210 MC SMMU (Linux tegra210.c / tegra-smmu.c) */
#define MC_INTSTATUS       0x000       /* MC error interrupts (Linux tegra mc.c) */
#define MC_ERR_STATUS      0x008
#define MC_ERR_ADR         0x00C
#define MC_SMMU_PTB_ASID   0x01C       /* select ASID for PTB_DATA (tegra-smmu) */
#define MC_SMMU_PTB_DATA   0x020       /* [31:29] attr, [21:0] page-directory pfn */
#define MC_SMMU_TLB_FLUSH  0x030       /* 0 = flush all (ATF MC_SMMU_TLB_FLUSH_ALL) */
#define MC_SMMU_PTC_FLUSH  0x034       /* 0 = flush all (ATF MC_SMMU_PTC_FLUSH_ALL) */
#define SMMU_ATTR_RWNS     0xE0000000u /* readable, writable, non-secure (tegra-smmu) */
#define SMMU_PDE_NEXT      (1u << 28)  /* PDE points to a page table */
#define MC_SMMU_GPU_ASID   0xAAC       /* bit 31 = GPU swgroup translated */

/* E1 resident controls use a device interface rather than a PnP restart:
 * restarting the device invokes ReleaseHardware and intentionally shuts the
 * resident GPU down. The interface is restricted to SYSTEM and Administrators. */
#define IOCTL_SWGPU_RESIDENT_STATUS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_RESIDENT_STOP   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_SUBMIT_TEST     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_IRQ_TEST        CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_CYCLE_TEST      CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_COPY_TEST       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_BLIT_TEST       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x806, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* GM20B NV_PMC / PFIFO interrupt source registers (nvgpu hw_mc_gm20b.h). */
#define PMC_INTR_STATUS_STALL       0x00000100u
#define PMC_INTR_STATUS_NONSTALL    0x00000104u
#define PMC_INTR_MASK_STALL         0x00000640u
#define PMC_INTR_MASK_NONSTALL      0x00000644u
#define PMC_INTR_EN_STALL           0x00000140u
#define PMC_INTR_EN_NONSTALL        0x00000144u
#define PMC_INTR_PFIFO_PENDING      0x00000100u
#define PMC_INTR_PGRAPH_PENDING     0x00001000u /* Claude 0.7.0: GR intr id 12 from this GPU's top_device_info (8006183E: intr 12, reset 12); 0x200000 was wrong */
#define FIFO_INTR_0                 0x00002100u
#define FIFO_INTR_0_CHANNEL_INTR    0x80000000u
#define FIFO_INTR_EN_1              0x00002528u /* Claude 0.7.3: nonstall routing (nvgpu fifo_intr_en_1 = CHANNEL_INTR) */
#define GR_INTR_NONSTALL            0x00400120u
#define GR_INTR_NONSTALL_TRAP       0x00000002u

typedef struct _SWGPU_RESIDENT_STATUS {
    ULONG Version;
    ULONG Resident;
    ULONG Boot0;
    ULONG PtimerLoBefore;
    ULONG PtimerHi;
    ULONG PtimerLoAfter;
    ULONG PtimerDelta;
    ULONG PmcEnable;
    ULONG PrivRingStatus0;
    ULONG PrivRingStatus1;
    ULONG FecsMailbox0;
    ULONG FecsCpuctl;
    ULONG FifoEngineStatus;
    ULONG CcsrChannel;
    ULONG PbdmaStatus;
    ULONG PbdmaIntr0;
    ULONG GrIntr;
    ULONG GrStatus;
    ULONG MmuFaultStatus;
    ULONG ShutdownResult;
} SWGPU_RESIDENT_STATUS, *PSWGPU_RESIDENT_STATUS;

typedef struct _SWGPU_E2_RESULT {
    ULONG Version, Result, Rounds, RingEntries, Wraps, FailRound, MaxPolls, TotalUs;
    ULONG FirstColor, LastColor, FirstSemaphore, LastSemaphore;
    ULONG GpGet, GpPut, Semaphore, Channel, PbdmaStatus, PbdmaIntr0, FifoIntr0;
    ULONG GrIntr, GrException, GrStatus, MmuFault, ClassError, TrappedAddr;
    ULONG TrappedDataLo, TrappedDataHi, PixelFirst, PixelMiddle, PixelLast;
} SWGPU_E2_RESULT, *PSWGPU_E2_RESULT;

typedef struct _SWGPU_E3_RESULT {
    ULONG Version, Result, Rounds, Waits, NonstallCount, StallCount, StallUnits;
    ULONG MaxWaitUs, TotalUs, Timeouts, HaveInterrupts;
    ULONG FirstMcIntr0, FirstMcIntr1, LastMcIntr0, LastMcIntr1;
    ULONG GpGet, GpPut, Semaphore, PixelFirst, PixelMiddle, PixelLast;
    ULONG Channel, PbdmaStatus, PbdmaIntr0, FifoIntr0, GrIntr, GrStatus, MmuFault;
} SWGPU_E3_RESULT, *PSWGPU_E3_RESULT;

typedef struct _SWGPU_E4_RESULT {
    ULONG Version, StartResult, SubmitResult, StopResult, Rounds, MaxPolls;
    ULONG PoolResult, PoolBytes, PoolUsedPeak, PoolFallbacks, PoolAllocs;
    ULONG PoolFrees, PoolFailures, PoolUsed;
} SWGPU_E4_RESULT, *PSWGPU_E4_RESULT;

typedef struct _SWGPU_F1_RESULT {
    ULONG Version, Result, Rounds, BytesCopied, FailRound, LastSize;
    ULONG MismatchOffset, Expected, Actual, MaxWaitUs, MaxPolls, IrqWakes, IrqTimeouts;
    ULONG Pending, CeIntr, CeLaunchError, FifoIntr, FaultId, PbdmaIntr, GrIntr;
} SWGPU_F1_RESULT, *PSWGPU_F1_RESULT;

typedef enum _SWGPU_POOL_REGION {
    SwGpuPoolGr, SwGpuPoolAcr, SwGpuPoolWpr, SwGpuPoolRegionCount
} SWGPU_POOL_REGION;

typedef struct _SWGPU_DMA_POOL {
    PVOID Block[SwGpuPoolRegionCount];
    BOOLEAN InUse[SwGpuPoolRegionCount];
    ULONG Result, Bytes, Used, UsedPeak, Fallbacks, Allocs, Frees, Failures;
} SWGPU_DMA_POOL;

typedef struct _SWGPU_RESIDENT_POWER {
    UCHAR Vout;
    UCHAR VoutDvs;
    UCHAR Gpio6;
    BOOLEAN Valid;
    BOOLEAN RailOn;
    BOOLEAN ClocksOn;
    BOOLEAN OutcChanged;
    BOOLEAN FuseChanged;
    ULONG ClkX;
    ULONG ClkH;
    ULONG PllOutc;
    ULONG McHotResetCtrl;
} SWGPU_RESIDENT_POWER, *PSWGPU_RESIDENT_POWER;

typedef struct _SWGPU_DEVICE {
    WDFDEVICE      Device;
    PHYSICAL_ADDRESS Bar0Phys;
    ULONG          Bar0Len;
    volatile UCHAR *Bar0;
    PHYSICAL_ADDRESS Bar1Phys;
    ULONG          Bar1Len;
    ULONG          Bar1MapLen;
    volatile UCHAR *Bar1Cpu;
    volatile UCHAR *Car;
    ULONG          GpcBootOut, GpcBootBypass;
    BOOLEAN        GpcManaged;
    ULONG          VoutOnRaw;    /* MAXULONG if the power-on read failed */
    volatile UCHAR *Pmc;
    volatile UCHAR *Mc;
    LARGE_INTEGER  VddId;
    LARGE_INTEGER  PmicId;
    WDFIOTARGET    Vdd;          /* MAX77621 0x1C */
    WDFIOTARGET    Pmic;         /* MAX77620 0x3C */
    BOOLEAN        ResidentActive;
    BOOLEAN        ResidentChannelEnabled;
    BOOLEAN        ResidentRunlistSubmitted;
    BOOLEAN        ResidentInstBound;
    BOOLEAN        ResidentIdleRefHeld;
    WDFINTERRUPT   StallInterrupt;
    WDFINTERRUPT   NonstallInterrupt;
    BOOLEAN        HaveInterruptResources;
    BOOLEAN        InterruptSourcesEnabled;
    SWGPU_DMA_POOL DmaPool;
    ULONG          InterruptResourceCount;
    volatile LONG  E3IsrActive;
    volatile LONG  E3NonstallCount;
    volatile LONG  E3StallCount;
    volatile LONG  E3StallUnits;
    volatile LONG  E3FirstMcIntr0;
    volatile LONG  E3FirstMcIntr1;
    volatile LONG  E3LastMcIntr0;
    volatile LONG  E3LastMcIntr1;
    volatile LONG  E3FirstSet;
    KEVENT         E3Event;
    BOOLEAN        CopyInFlight;
    volatile LONG  BlitGate;
    volatile LONG  BlitOnline;
    /* G1 scheduler fields under PowerLock. Hardware state under BlitGate. */
    WDFTIMER       PowerTimer;
    WDFWORKITEM    PowerWork;
    KSPIN_LOCK     PowerLock;
    KEVENT         PowerWorkDone;
    ULONG          IdlePowerOffMs;
    BOOLEAN        PowerPolicy, PowerPaused, PowerFinal, PowerD0;
    BOOLEAN        PowerQueued, PowerWakeWanted, PowerDisabled, PowerManualStop;
    volatile LONG64 LastBlitTime;
    ULONGLONG      PowerWakeStart;
    LARGE_INTEGER  PowerWakeQpc;
    ULONG          PowerOffs, PowerOns, LastOffMs, LastOnUs, OnFailures, OnStreak;
    BOOLEAN        BlitReset;
    BOOLEAN        BlitUnsafe;
    ULONG          BlitLastWaitUs, BlitLastPolls;
    ULONG          ResidentRunlistEmptyValue;
    ULONG          ResidentChannelId;
    ULONG          ResidentShutdownResult;
    SWGPU_RESIDENT_POWER ResidentPower;
} SWGPU_DEVICE, *PSWGPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWGPU_DEVICE, SwGpuGetDevice)

/* g_AcrVm/g_GrVm back a single ACPI GPU instance. The resident owner records
 * which device keeps these shared arenas and mappings live. */
static PSWGPU_DEVICE g_ResidentOwner;


DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwGpuEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwGpuEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwGpuEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwGpuEvtSelfManagedIoInit;
EVT_WDF_DEVICE_D0_EXIT SwGpuEvtD0Exit;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SwGpuEvtIoDeviceControl;
EVT_WDF_INTERRUPT_ISR SwGpuEvtStallInterruptIsr;
EVT_WDF_INTERRUPT_ISR SwGpuEvtNonstallInterruptIsr;
EVT_WDF_INTERRUPT_DPC SwGpuEvtInterruptDpc;

static WCHAR SwGpuParamsPath[256];
static BOOLEAN g_Verbose;
static ULONG B3eResidentShutdown(PSWGPU_DEVICE Dev, BOOLEAN DrainIsrs);
static ULONG B3eEnableInterruptSources(PSWGPU_DEVICE Dev);

/* ---------------------------------------------------------------- trace */

static VOID Trace(PCWSTR Name, ULONG Value)
{
    if (SwGpuParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwGpuParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
}

/* Keep MMIO read ordering while suppressing detailed registry writes. */
static VOID TraceDetail(PCWSTR Name, ULONG Value)
{
    if (g_Verbose) Trace(Name, Value);
}

static ULONG ReadParam(PCWSTR Name, ULONG Default)
{
    ULONG Value = Default, Def = Default;
    RTL_QUERY_REGISTRY_TABLE Table[2];
    RtlZeroMemory(Table, sizeof(Table));
    Table[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    Table[0].Name = (PWSTR)Name;
    Table[0].EntryContext = &Value;
    Table[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    Table[0].DefaultData = &Def;
    Table[0].DefaultLength = sizeof(ULONG);
    if (SwGpuParamsPath[0] == L'\0' ||
        !NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, SwGpuParamsPath, Table, NULL, NULL))) {
        return Default;
    }
    return Value;
}

/* Write and flush to disk, so a hang right after still leaves the mark. */
static VOID TraceDurable(PCWSTR Name, ULONG Value)
{
    UNICODE_STRING Path;
    OBJECT_ATTRIBUTES Oa;
    HANDLE Key;
    Trace(Name, Value);
    RtlInitUnicodeString(&Path, SwGpuParamsPath);
    InitializeObjectAttributes(&Oa, &Path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (NT_SUCCESS(ZwOpenKey(&Key, KEY_WRITE, &Oa))) {
        (VOID)ZwFlushKey(Key);
        ZwClose(Key);
    }
}

static VOID Step(ULONG N) { TraceDurable(L"Step", N); }

static VOID TraceErrorBin(PCWSTR Name, PVOID Data, ULONG Length)
{
    if (SwGpuParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwGpuParamsPath, Name, REG_BINARY, Data, Length);
    }
}

#define TraceBin(Name, Data, Length) do { \
    if (g_Verbose) TraceErrorBin((Name), (Data), (Length)); \
} while (0)

/* ------------------------------------------------------------------- MMIO */

static ULONG Rd(volatile UCHAR *Base, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off)); }
static VOID Wr(volatile UCHAR *Base, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), V); }

/* The resident gate is checked before any BAR0 access. This keeps an asserted
 * level vector harmless while the GPU is off or during a non-resident run. */
static BOOLEAN B3eRecordMcInterrupt(PSWGPU_DEVICE Dev, ULONG Intr0, ULONG Intr1,
                                   BOOLEAN Stall)
{
    if (InterlockedCompareExchange(&Dev->E3FirstSet, 1, 0) == 0) {
        InterlockedExchange(&Dev->E3FirstMcIntr0, (LONG)Intr0);
        InterlockedExchange(&Dev->E3FirstMcIntr1, (LONG)Intr1);
    }
    InterlockedExchange(&Dev->E3LastMcIntr0, (LONG)Intr0);
    InterlockedExchange(&Dev->E3LastMcIntr1, (LONG)Intr1);
    if (Stall) {
        InterlockedIncrement(&Dev->E3StallCount);
        InterlockedOr(&Dev->E3StallUnits, (LONG)Intr0);
    } else {
        InterlockedIncrement(&Dev->E3NonstallCount);
    }
    return TRUE;
}

static BOOLEAN B3eInterruptIsr(WDFINTERRUPT Interrupt, BOOLEAN Stall)
{
    WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    volatile UCHAR *B;
    ULONG intr0, intr1, fifoIntr, grIntr, grAck;
    BOOLEAN handled = FALSE;

    if (Dev == NULL || InterlockedCompareExchange(&Dev->E3IsrActive, 0, 0) == 0 ||
        !Dev->ResidentActive || Dev->Bar0 == NULL)
        return FALSE;
    B = Dev->Bar0;
    intr0 = Rd(B, PMC_INTR_STATUS_STALL);
    intr1 = Rd(B, PMC_INTR_STATUS_NONSTALL);
    if (Stall) {
        if (intr0 == 0u) return FALSE;
        (VOID)B3eRecordMcInterrupt(Dev, intr0, intr1, TRUE);
        /* ISR direct-write allowlist only: PMC INTR_EN_0 (0x140) masks stall;
         * PFIFO INTR_0 (0x2100) W1C-acks CHANNEL_INTR; GR INTR_NONSTALL
         * (0x400120) W1C-acks the nonstall trap; PMC INTR_EN_1 (0x144) masks an
         * unacked nonstall source (Claude 0.7.0). No other ISR writes. */
        Wr(B, PMC_INTR_EN_STALL, 0u);
        (VOID)Rd(B, PMC_INTR_EN_STALL);
        return TRUE;
    }

    if ((intr1 & PMC_INTR_PFIFO_PENDING) != 0u) {
        fifoIntr = Rd(B, FIFO_INTR_0);
        if ((fifoIntr & FIFO_INTR_0_CHANNEL_INTR) != 0u) {
            /* PFIFO CHANNEL_INTR is W1C (nvgpu fifo_nonstall_isr path). */
            Wr(B, FIFO_INTR_0, FIFO_INTR_0_CHANNEL_INTR);
            handled = TRUE;
        }
    }
    if ((intr1 & PMC_INTR_PGRAPH_PENDING) != 0u) {
        grIntr = Rd(B, GR_INTR_NONSTALL);
        grAck = grIntr & GR_INTR_NONSTALL_TRAP;
        if (grAck != 0u) {
            /* GR nonstall trap is W1C, as gr_gk20a_handle_nonstall(). */
            Wr(B, GR_INTR_NONSTALL, grAck);
            handled = TRUE;
        }
    }
    if (!handled) {
        /* Claude 0.7.0: a level line with a pending source we do not ack would
         * storm. Mask INTR_EN_1 (allowlisted write) and claim it; E3 then times
         * out and the recorded status shows which bit it was. */
        if (intr1 == 0u) return FALSE;
        (VOID)B3eRecordMcInterrupt(Dev, intr0, intr1, FALSE);
        Wr(B, PMC_INTR_EN_NONSTALL, 0u);
        (VOID)Rd(B, PMC_INTR_EN_NONSTALL);
        return TRUE;
    }
    (VOID)B3eRecordMcInterrupt(Dev, intr0, intr1, FALSE);
    (VOID)WdfInterruptQueueDpcForIsr(Interrupt);
    return TRUE;
}

BOOLEAN SwGpuEvtStallInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);
    return B3eInterruptIsr(Interrupt, TRUE);
}

BOOLEAN SwGpuEvtNonstallInterruptIsr(WDFINTERRUPT Interrupt, ULONG MessageId)
{
    UNREFERENCED_PARAMETER(MessageId);
    return B3eInterruptIsr(Interrupt, FALSE);
}

VOID SwGpuEvtInterruptDpc(WDFINTERRUPT Interrupt, WDFOBJECT AssociatedObject)
{
    WDFDEVICE Device = (WDFDEVICE)AssociatedObject;
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    UNREFERENCED_PARAMETER(Interrupt);
    if (Dev != NULL) KeSetEvent(&Dev->E3Event, IO_NO_INCREMENT, FALSE);
}
static VOID Us(ULONG Microseconds) { KeStallExecutionProcessor(Microseconds); }

static VOID Ms(ULONG Milliseconds)
{
    LARGE_INTEGER t;
    t.QuadPart = -(LONGLONG)Milliseconds * 10 * 1000;
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* -------------------------------------------------------------------- I2C */

static NTSTATUS OpenTarget(WDFDEVICE Device, LARGE_INTEGER Id, WDFIOTARGET *Target)
{
    WCHAR PathBuffer[64];
    UNICODE_STRING Path;
    WDF_IO_TARGET_OPEN_PARAMS Open;
    NTSTATUS Status;

    RtlInitEmptyUnicodeString(&Path, PathBuffer, sizeof(PathBuffer));
    Status = RtlUnicodeStringPrintf(&Path, L"\\Device\\RESOURCE_HUB\\%016I64x", (ULONG64)Id.QuadPart);
    if (!NT_SUCCESS(Status)) return Status;
    Status = WdfIoTargetCreate(Device, WDF_NO_OBJECT_ATTRIBUTES, Target);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&Open, &Path, GENERIC_READ | GENERIC_WRITE);
    Status = WdfIoTargetOpen(*Target, &Open);
    if (!NT_SUCCESS(Status)) {
        WdfObjectDelete(*Target);
        *Target = NULL;
    }
    return Status;
}

static NTSTATUS I2cRead(WDFIOTARGET Target, UCHAR Register, PUCHAR Value)
{
    SPB_TRANSFER_LIST_AND_ENTRIES(2) Sequence;
    PSPB_TRANSFER_LIST_ENTRY Entries = Sequence.List.Transfers;
    WDF_MEMORY_DESCRIPTOR Input;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) return STATUS_DEVICE_NOT_READY;
    SPB_TRANSFER_LIST_INIT(&Sequence.List, 2);
    Entries[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, &Register, 1);
    Entries[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, Value, 1);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Input, &Sequence, sizeof(Sequence));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendIoctlSynchronously(Target, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &Input, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != 2) Status = STATUS_DEVICE_PROTOCOL_ERROR;
    return Status;
}

static NTSTATUS I2cWrite(WDFIOTARGET Target, UCHAR Register, UCHAR Value)
{
    UCHAR Buffer[2] = { Register, Value };
    WDF_MEMORY_DESCRIPTOR Memory;
    WDF_REQUEST_SEND_OPTIONS Options;
    ULONG_PTR Bytes = 0;
    NTSTATUS Status;

    if (Target == NULL) return STATUS_DEVICE_NOT_READY;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&Memory, Buffer, sizeof(Buffer));
    WDF_REQUEST_SEND_OPTIONS_INIT(&Options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    Options.Timeout = WDF_REL_TIMEOUT_IN_MS(200);
    Status = WdfIoTargetSendWriteSynchronously(Target, NULL, &Memory, NULL, &Options, &Bytes);
    if (NT_SUCCESS(Status) && Bytes != sizeof(Buffer)) Status = STATUS_DEVICE_PROTOCOL_ERROR;
    return Status;
}

/* Reads a register into the trace as PrefixName; returns 0xFFFFFFFF on error. */
static ULONG I2cTrace(WDFIOTARGET Target, UCHAR Register, PCWSTR Name)
{
    UCHAR v = 0;
    NTSTATUS st = I2cRead(Target, Register, &v);
    ULONG out = NT_SUCCESS(st) ? v : (0xFF000000u | ((ULONG)st & 0xFFFFFFu));
    Trace(Name, out);
    return NT_SUCCESS(st) ? v : 0xFFFFFFFFu;
}

/* ------------------------------------------------------------- snapshots */

static VOID Snapshot(PSWGPU_DEVICE Dev, BOOLEAN After)
{
#define N(a, b) (After ? L##b : L##a)
    if (Dev->ResidentActive) { TraceDetail(L"B3eSnapshotSkipped", After ? 2u : 1u); return; }
    Trace(N("RgCntrlAtStart",  "RgCntrlAfter"),  Rd(Dev->Pmc, PMC_GPU_RG_CNTRL));
    Trace(N("ClkXAtStart",     "ClkXAfter"),     Rd(Dev->Car, CAR_CLK_OUT_ENB_X));
    Trace(N("RstXAtStart",     "RstXAfter"),     Rd(Dev->Car, CAR_RST_DEVICES_X));
    Trace(N("PllpOutcAtStart", "PllpOutcAfter"), Rd(Dev->Car, CAR_PLLP_OUTC));
    Trace(N("ClkHAtStart",     "ClkHAfter"),     Rd(Dev->Car, CAR_CLK_OUT_ENB_H));
    Trace(N("McHotCtrlAtStart","McHotCtrlAfter"),Rd(Dev->Mc, MC_HOTRESET_CTRL_1));
    Trace(N("McHotStatAtStart","McHotStatAfter"),Rd(Dev->Mc, MC_HOTRESET_STAT_1));
    (VOID)I2cTrace(Dev->Vdd, MAX77621_VOUT,     N("VoutAtStart",    "VoutAfter"));
    (VOID)I2cTrace(Dev->Vdd, MAX77621_VOUT_DVS, N("VoutDvsAtStart", "VoutDvsAfter"));
    (VOID)I2cTrace(Dev->Pmic, MAX77620_GPIO6,   N("Gpio6AtStart",   "Gpio6After"));
    if (!After) {
        (VOID)I2cTrace(Dev->Vdd, MAX77621_CONTROL1, L"Control1");
        (VOID)I2cTrace(Dev->Vdd, MAX77621_CONTROL2, L"Control2");
        (VOID)I2cTrace(Dev->Vdd, MAX77621_CHIPID1,  L"ChipId1");
        (VOID)I2cTrace(Dev->Vdd, MAX77621_CHIPID2,  L"ChipId2");
    }
#undef N
}

/* ------------------------------------------------------- stage B1 probe */

/* nouveau gk20a_privring_init_privring_ring. Initialize before ACR DMA. */
static VOID PrivRingInit(volatile UCHAR *B, ULONG StepNo)
{
    Step(StepNo);
    TraceDetail(L"RingStatus0Before", Rd(B, 0x120058));
    Wr(B, 0x137250, Rd(B, 0x137250) & ~0x3Fu);
    Wr(B, NV_PMC_ENABLE, Rd(B, NV_PMC_ENABLE) & ~0x20u);
    Us(20);
    Wr(B, NV_PMC_ENABLE, Rd(B, NV_PMC_ENABLE) | 0x20u);
    Wr(B, 0x12004C, 0x4);
    Wr(B, 0x122204, 0x2);
    (VOID)Rd(B, 0x122204);
    Wr(B, 0x122354, 0x800);
    Wr(B, 0x128328, 0x800);
    Wr(B, 0x124320, 0x800);
    Us(10);
    TraceDetail(L"RingStatus0", Rd(B, 0x120058));
    TraceDetail(L"RingCmd", Rd(B, 0x12004C));
    TraceDetail(L"RingMaster", Rd(B, 0x122204));
    TraceDetail(L"PmcEnableAfterRing", Rd(B, NV_PMC_ENABLE));
    TraceDurable(L"RingDone", StepNo);
}

/* GM200 PMU bind handshake from nouveau gm200_pmu_flcn_bind_inst/bind_stat.
 * Return the final state (0 means complete); expose the intermediate state
 * and poll counts so the probe can identify exactly where it stopped. */
static BOOLEAN PmuBindInst(volatile UCHAR *B, ULONGLONG instAddress,
                           PULONG AckState, PULONG DoneState,
                           PULONG AckPolls, PULONG DonePolls)
{
    static const ULONG offsets[5] = { 0xE00, 0xE04, 0xE08, 0xE0C, 0xE10 };
    static const ULONG values[5] = { 4, 0, 4, 5, 6 };
    ULONG i, state;

    /* nouveau enables the Falcon external interface before binding. */
    Wr(B, PMU_FALCON + FALCON_ITFEN,
       Rd(B, PMU_FALCON + FALCON_ITFEN) | 0x00000001u);
    for (i = 0; i < 5; ++i) Wr(B, PMU_FALCON + offsets[i], values[i]);
    Wr(B, PMU_FALCON + FALCON_CTX_ENABLE,
       Rd(B, PMU_FALCON + FALCON_CTX_ENABLE) | 0x00010000u);
    Wr(B, PMU_FALCON + FALCON_BIND_INST,
       0x40000000u | (3u << 28) | (ULONG)(instAddress >> 12)); /* SYS_NCOH */

    Wr(B, PMU_FALCON + FALCON_BIND_TRIGGER, 0x0000030Eu);
    state = (Rd(B, PMU_FALCON + FALCON_BIND_STATUS) & 0x00007000u) >> 12;
    for (*AckPolls = 0; *AckPolls < 2000 && state != 5; ++*AckPolls) {
        Us(5);
        Wr(B, PMU_FALCON + FALCON_BIND_TRIGGER, 0x0000030Eu);
        state = (Rd(B, PMU_FALCON + FALCON_BIND_STATUS) & 0x00007000u) >> 12;
    }
    *AckState = state;
    *DoneState = state;
    *DonePolls = 0;
    if (state != 5) return FALSE;

    Wr(B, PMU_FALCON + 0x004, 0x00000008u); /* clear bind interrupt */
    Wr(B, PMU_FALCON + FALCON_CTX_ACTION,
       Rd(B, PMU_FALCON + FALCON_CTX_ACTION) | 0x00000002u);
    for (*DonePolls = 0; *DonePolls < 2000; ++*DonePolls) {
        Wr(B, PMU_FALCON + FALCON_BIND_TRIGGER, 0x0000030Eu);
        state = (Rd(B, PMU_FALCON + FALCON_BIND_STATUS) & 0x00007000u) >> 12;
        if (state == 0) break;
        Us(5);
    }
    *DoneState = state;
    return state == 0;
}

#define FALCON_BOOTVEC     0x104
#define FALCON_IMEMC0      0x180       /* BIT24 auto-increment write, BIT25 read, BIT28 secure */
#define FALCON_IMEMD0      0x184
#define FALCON_IMEMT0      0x188       /* tag of the next 256-byte block */
#define ACR_BL_MAX         0x2000u
#define ACR_UC_MAX         0x20000u
#define ACR_DMAIDX_VIRT    1u          /* nvgpu GK20A_PMU_DMAIDX_VIRT */
#define ACR_VM_ARENA_SIZE  0x80000u
#define ACR_VM_PGD_OFF     0x02000u    /* 8K-aligned; 8K entries x 8 bytes */
#define ACR_VM_PGT_OFF     0x12000u    /* 4K small-page table, 32768 x 8 bytes */
#define ACR_VM_INST_OFF    0x52000u
#define ACR_VM_IMAGE_OFF   0x53000u
#define ACR_GPU_VA_BASE    0x10000000u
#define ACR_SMMU_WINDOW    0x00400000u
#define ACR_WPR_BYTES      0x00040000u
#define ACR_GM20B_IOMMU_TAG (1ull << 34) /* nouveau_platform.c: gm20b iommu_bit */

/* C2d owns a separate contiguous GR arena and GPU instance. The outer
 * SMMU directory may reference this arena, but the ACR arena and instance
 * block remain dedicated to the ACR/PMU path. */
#define GR_VM_ARENA_SIZE   0x248000u   /* 2 MiB + 256 KiB GPU PGT + 8 outer SMMU PGTs */
#define GR_VM_CTX_OFF      0x000000u
#define GR_VM_GLOBAL_OFF   0x020000u /* golden context image */
#define GR_VM_BUNDLE_OFF   0x040000u
#define GR_VM_PAGEPOOL_OFF 0x042000u
#define GR_VM_ATTRIB_OFF   0x050000u
#define GR_VM_ZCULL_OFF    0x098000u
#define GR_VM_PM_OFF       0x0A9000u
#define GR_VM_PATCH_OFF    0x0AA000u /* channel FECS patch-context backing */
#define GR_VM_PATCH_BYTES  PAGE_SIZE
#define GR_VM_PRIV_OFF     0x100000u
#define GR_VM_GPU_PGD_OFF  0x180000u
#define GR_VM_GPU_PGT_OFF  0x190000u
#define GR_VM_INST_OFF     0x1D0000u
#define GR_VM_SMMU_PGT_OFF 0x1D1000u
#define GR_VM_D2_USERD_OFF 0x1D2000u
#define GR_VM_D2_GPFIFO_OFF 0x1D3000u
#define GR_VM_D2_RUNLIST_OFF 0x1D4000u
#define GR_VM_D2_PUSHBUF_OFF 0x1D5000u
#define GR_VM_D2_RT_OFF      0x1D6000u /* D5 pitch-linear 64x64x4 render target */
#define GR_VM_F1_PUSH_OFF    0x1DA000u /* Dedicated copy pushbuffer and completion words */
#define GR_VM_F1_SRC_OFF     0x1E0000u /* 64 KiB, mapped by the existing arena PTEs */
#define GR_VM_F1_DST_OFF     0x1F0000u /* 64 KiB, ends at the end of the arena */
#define GR_VM_F1_BYTES       0x10000u
#define GR_VM_BLIT_GPU_PGT  0x200000u
#define GR_VM_BLIT_SMMU_PT  0x240000u
#define GR_VM_BLIT_BASE     0x20000000u  /* 128 MiB GPU PDE boundary; same 32-bit outer IOVA */
#define GR_VM_BLIT_PDES     8u           /* 32 MiB, 4 MiB per outer SMMU page table */
#define GR_VM_GPU_PGT_BAR1_OFF 0x1A0000u /* spare 64 KiB-aligned segment within the PGT backing */
#define GR_VM_GPU_VA       0x18000000u
#define GR_VM_PAGE_COUNT   (GR_VM_ARENA_SIZE >> 12)

C_ASSERT(GR_VM_D2_RT_OFF + 64u * 64u * sizeof(ULONG) <= GR_VM_F1_PUSH_OFF);
C_ASSERT(GR_VM_F1_PUSH_OFF + PAGE_SIZE <= GR_VM_F1_SRC_OFF);
C_ASSERT(GR_VM_F1_SRC_OFF + GR_VM_F1_BYTES <= GR_VM_F1_DST_OFF);
C_ASSERT(GR_VM_F1_DST_OFF + GR_VM_F1_BYTES <= GR_VM_ARENA_SIZE);
C_ASSERT(GR_VM_F1_DST_OFF + GR_VM_F1_BYTES <= GR_VM_BLIT_GPU_PGT);
C_ASSERT(GR_VM_BLIT_GPU_PGT + 0x40000u == GR_VM_BLIT_SMMU_PT);
C_ASSERT(GR_VM_BLIT_SMMU_PT + GR_VM_BLIT_PDES * PAGE_SIZE == GR_VM_ARENA_SIZE);

/* E4 DMA allocation inventory (Resident, Submit and Irq):
 * - GR: 0x248000 bytes, page aligned, below 4 GiB, within one 4 MiB window,
 *   MmNonCached. Contains GR/SMMU/BAR1 tables, instances, channel and golden
 *   contexts, global buffers, USERD, GPFIFO, runlist, pushbuffer/semaphores
 *   and the 16 KiB render target at the GR_VM_* offsets above.
 * - ACR: 512 KiB, same constraints. Contains SMMU/GPU tables, PMU instance
 *   and HS ACR image. The other Falcon uploads use the existing WPR/PIO paths.
 * - WPR input: wprUsed (at most 256 KiB), page aligned, below 4 GiB,
 *   MmNonCached, no boundary requirement. Holds the PMU/FECS LS images.
 * Firmware assembly, registry blobs and diagnostic arrays use ordinary
 * ExAllocatePool2 memory and are not DMA backing. Submit/Irq allocate no
 * additional contiguous memory.
 *
 * Fixed regions are reserved independently at PrepareHardware so a failed
 * reservation leaves the others usable. PnP and the sequential IOCTL queue
 * serialize checkouts/returns. Return DMA backing only after GPU reset and
 * restoration of the SMMU mappings, before ReleaseHardware frees the regions.
 */
static const ULONG g_DmaPoolRegionBytes[SwGpuPoolRegionCount] = {
    GR_VM_ARENA_SIZE, ACR_VM_ARENA_SIZE, ACR_WPR_BYTES
};

static VOID E4TracePool(const SWGPU_DMA_POOL *Pool)
{
    Trace(L"E4PoolResult", Pool->Result);
    Trace(L"E4PoolBytes", Pool->Bytes);
    Trace(L"E4PoolUsedPeak", Pool->UsedPeak);
    Trace(L"E4PoolFallbacks", Pool->Fallbacks);
    Trace(L"E4PoolAllocs", Pool->Allocs);
    Trace(L"E4PoolFrees", Pool->Frees);
    Trace(L"E4PoolFailures", Pool->Failures);
    Trace(L"E4PoolUsed", Pool->Used);
}

static PVOID E4AllocateContiguous(SWGPU_POOL_REGION Region, ULONG Bytes)
{
    PHYSICAL_ADDRESS low, high, boundary;
    low.QuadPart = 0;
    high.QuadPart = 0xFFFFFFFFll;
    boundary.QuadPart = Region == SwGpuPoolWpr ? 0 : ACR_SMMU_WINDOW;
    return MmAllocateContiguousMemorySpecifyCache(Bytes, low, high, boundary, MmNonCached);
}

static VOID E4ReservePool(PSWGPU_DEVICE Dev)
{
    SWGPU_DMA_POOL *pool = &Dev->DmaPool;
    SWGPU_POOL_REGION region;
    RtlZeroMemory(pool, sizeof(*pool));
    for (region = SwGpuPoolGr; region < SwGpuPoolRegionCount; ++region) {
        ULONG bytes = g_DmaPoolRegionBytes[region];
        pool->Block[region] = E4AllocateContiguous(region, bytes);
        if (pool->Block[region] != NULL && region != SwGpuPoolWpr &&
            MmGetPhysicalAddress(pool->Block[region]).QuadPart > 0xFFFFFFFFll - bytes) {
            /* The existing arena range checks require a 32-bit exclusive end. */
            MmFreeContiguousMemorySpecifyCache(pool->Block[region], bytes, MmNonCached);
            pool->Block[region] = NULL;
        }
        if (pool->Block[region] == NULL) {
            pool->Result |= 1u << region;
            ++pool->Failures;
        } else {
            pool->Bytes += bytes;
        }
    }
    E4TracePool(pool);
}

static PVOID E4Checkout(PSWGPU_DEVICE Dev, SWGPU_POOL_REGION Region, ULONG Bytes)
{
    SWGPU_DMA_POOL *pool = &Dev->DmaPool;
    PVOID block;
    if (Bytes == 0 || Bytes > g_DmaPoolRegionBytes[Region] || pool->InUse[Region]) {
        ++pool->Failures;
        return NULL;
    }
    block = pool->Block[Region];
    if (block != NULL) {
        pool->InUse[Region] = TRUE;
        pool->Used += g_DmaPoolRegionBytes[Region];
        if (pool->Used > pool->UsedPeak) pool->UsedPeak = pool->Used;
        ++pool->Allocs;
        RtlZeroMemory(block, g_DmaPoolRegionBytes[Region]);
    } else {
        ++pool->Fallbacks;
        block = E4AllocateContiguous(Region, Bytes);
        if (block != NULL) {
            pool->InUse[Region] = TRUE;
            RtlZeroMemory(block, Bytes);
        } else {
            ++pool->Failures;
        }
    }
    return block;
}

static VOID E4Return(PSWGPU_DEVICE Dev, SWGPU_POOL_REGION Region, PVOID Block, ULONG Bytes)
{
    SWGPU_DMA_POOL *pool = &Dev->DmaPool;
    if (Block == NULL) return;
    pool->InUse[Region] = FALSE;
    if (Block == pool->Block[Region]) {
        pool->Used -= g_DmaPoolRegionBytes[Region];
        ++pool->Frees;
    } else {
        MmFreeContiguousMemorySpecifyCache(Block, Bytes, MmNonCached);
    }
}

static VOID E4ReleasePool(PSWGPU_DEVICE Dev)
{
    SWGPU_DMA_POOL *pool = &Dev->DmaPool;
    SWGPU_POOL_REGION region;
    for (region = SwGpuPoolGr; region < SwGpuPoolRegionCount; ++region) {
        if (pool->InUse[region]) {
            Trace(L"E4PoolReleaseBorrowed", pool->Used);
            continue;
        }
        if (pool->Block[region] != NULL) {
            MmFreeContiguousMemorySpecifyCache(pool->Block[region],
                g_DmaPoolRegionBytes[region], MmNonCached);
            pool->Block[region] = NULL;
        }
    }
}

typedef struct _ACR_VM_STATE {
    PVOID Arena;
    PVOID WprBuffer;
    ULONG OldPtbAsid;
    ULONG OldPtbData;
    PHYSICAL_ADDRESS WprPa;
    ULONG WprBytes;
    BOOLEAN PtbChanged;
    BOOLEAN Active;
} ACR_VM_STATE;

static ACR_VM_STATE g_AcrVm;

typedef struct _GR_VM_STATE {
    PVOID Arena;
    PHYSICAL_ADDRESS Pa;
    ULONGLONG Iova;
    ULONG SmmuWin;
    ULONG SmmuPteFirst;
    ULONG SmmuPdeIndex;
    ULONG OldSmmuPde;
    ULONG GoldenBytes;
    ULONG ZcullBytes;
    ULONG PmBytes;
    ULONG OldSmmuPtes[GR_VM_PAGE_COUNT];
    BOOLEAN PtbChanged;
    BOOLEAN SharedSmmuPt;
    BOOLEAN Active;
} GR_VM_STATE;

static GR_VM_STATE g_GrVm;

#define B3C_PRIV_RING_LOG_CAPACITY 64u
#define B3C_PRIV_RING_LOG_WORDS 16u
static ULONG g_B3cPrivRingWriteLog[B3C_PRIV_RING_LOG_CAPACITY][B3C_PRIV_RING_LOG_WORDS];
static ULONG g_B3cPrivRingWriteLogCount;
static ULONG g_B3cPrivRingWriteLogDropped;
static ULONG g_B3cPrivRingAckTimeouts;
static ULONG g_B3cPrivRingLastAckPolls;
static ULONG g_B3cPrivRingLastCommand;

/* Read back one identity-mapped Tegra-SMMU page. This is diagnostic only:
 * a mismatch is recorded but does not change the ACR experiment flow. */
static BOOLEAN AcrTraceSmmuPte(volatile ULONG *Pt, ULONG Win, ULONG Pa,
                               PCWSTR IndexName, PCWSTR ExpectedName,
                               PCWSTR ReadName)
{
    ULONG index, expected, actual;

    if (Pa < Win || ((Pa - Win) >> 12) >= 1024u) return FALSE;
    index = (Pa - Win) >> 12;
    expected = (Pa >> 12) | SMMU_ATTR_RWNS;
    actual = Pt[index];
    Trace(IndexName, index);
    Trace(ExpectedName, expected);
    Trace(ReadName, actual);
    return actual == expected;
}

/* GM20B stores its WPR image in ordinary INST-target memory and passes the
 * physical base/size in flcn_acr_desc.  The 0x100CD4 WPR range is only a
 * capacity check; it is not CPU-writable backing memory.  Set up a separate
 * CPU-writable blob plus the bounded PMU GPU VA space. */
static BOOLEAN AcrVmPrepare(PSWGPU_DEVICE Dev, ULONG WprBytes, ULONGLONG *ArenaIovaOut,
                            ULONGLONG *InstIovaOut)
{
    PHYSICAL_ADDRESS arenaPa;
    volatile ULONG *smmuPd, *smmuPt;
    ULONGLONG *gpuPgd, *gpuPgt, *inst;
    ULONG win, arenaFirst, arenaPages;
    ULONGLONG arenaIova;
    ULONG i, oldAsid, oldPtbAsid, oldPtbData;
    ULONG pdeIndex, pteIndex;
    ULONG smmuPdeExpected, smmuPdeRead, smmuMapOk = 0, smmuPdeIndex;
    ULONGLONG attrs;
    ULONGLONG smmuPtPa;
    BOOLEAN smmuReadbackOk;

    RtlZeroMemory(&g_AcrVm, sizeof(g_AcrVm));
    g_AcrVm.WprBytes = WprBytes;
    g_AcrVm.WprBuffer = E4Checkout(Dev, SwGpuPoolWpr, WprBytes);
    if (g_AcrVm.WprBuffer == NULL) return FALSE;
    g_AcrVm.WprPa = MmGetPhysicalAddress(g_AcrVm.WprBuffer);
    if (g_AcrVm.WprPa.HighPart != 0 || (g_AcrVm.WprPa.LowPart & 0xFFFu) != 0) return FALSE;
    TraceDetail(L"AcrWprBufferPaLo", g_AcrVm.WprPa.LowPart);
    TraceDetail(L"AcrWprBufferPaHi", (ULONG)g_AcrVm.WprPa.HighPart);
    TraceDetail(L"AcrWprBufferBytes", WprBytes);

    g_AcrVm.Arena = E4Checkout(Dev, SwGpuPoolAcr, ACR_VM_ARENA_SIZE);
    if (g_AcrVm.Arena == NULL) return FALSE;
    arenaPa = MmGetPhysicalAddress(g_AcrVm.Arena);
    TraceDetail(L"AcrArenaPa", arenaPa.LowPart);
    if (arenaPa.HighPart != 0 || (arenaPa.LowPart & 0xFFFu) != 0 ||
        arenaPa.LowPart > 0xFFFFFFFFu - ACR_VM_ARENA_SIZE) return FALSE;

    oldAsid = Rd(Dev->Mc, MC_SMMU_GPU_ASID);
    win = arenaPa.LowPart & ~(ACR_SMMU_WINDOW - 1u);
    arenaPages = ACR_VM_ARENA_SIZE >> 12;
    arenaFirst = (arenaPa.LowPart - win) >> 12;
    if (arenaFirst + arenaPages > (ACR_SMMU_WINDOW >> 12)) return FALSE;

    Trace(L"AcrSmmuMode", (oldAsid & 0x80000000u) ? 1u : 0u);
    if ((oldAsid & 0x80000000u) == 0u) return FALSE;
    {
        /* The existing GPU SMMU path translates ASID 0. Preserve its prior
         * page-table selection and restore it only after the GPU reset. */
        oldPtbAsid = Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
        oldPtbData = Rd(Dev->Mc, MC_SMMU_PTB_DATA);
        g_AcrVm.OldPtbAsid = oldPtbAsid;
        g_AcrVm.OldPtbData = oldPtbData;

        smmuPd = (volatile ULONG *)((PUCHAR)g_AcrVm.Arena + 0);
        smmuPt = (volatile ULONG *)((PUCHAR)g_AcrVm.Arena + PAGE_SIZE);
        for (i = 0; i < 1024; ++i) { smmuPd[i] = 0; smmuPt[i] = 0; }
        for (i = 0; i < arenaPages; ++i)
            smmuPt[arenaFirst + i] = ((arenaPa.LowPart >> 12) + i) | SMMU_ATTR_RWNS;
        arenaPa = MmGetPhysicalAddress(g_AcrVm.Arena);
        smmuPd[win >> 22] = (ULONG)((arenaPa.QuadPart + PAGE_SIZE) >> 12) |
                            SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
        KeMemoryBarrier();

        Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
        Wr(Dev->Mc, MC_SMMU_PTB_DATA, (ULONG)(arenaPa.QuadPart >> 12) | SMMU_ATTR_RWNS);
        Wr(Dev->Mc, MC_SMMU_PTC_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_TLB_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        g_AcrVm.PtbChanged = TRUE;
        /* gk20a_instobj_ctor_iommu tags device addresses with iommu_bit;
         * GM20B uses bit 34. The SMMU directory/PTEs still use untagged
         * IOVA/physical addresses. See NOTICE-GM20B-MMU.txt. */
        arenaIova = (ULONGLONG)arenaPa.LowPart | ACR_GM20B_IOMMU_TAG;
        if (Rd(Dev->Mc, MC_SMMU_PTB_DATA) !=
            ((ULONG)(arenaPa.QuadPart >> 12) | SMMU_ATTR_RWNS)) return FALSE;
    }

    *ArenaIovaOut = arenaIova;
    *InstIovaOut = *ArenaIovaOut + ACR_VM_INST_OFF;
    TraceDetail(L"AcrSmmuWin", win);
    TraceDetail(L"AcrArenaIova", (ULONG)*ArenaIovaOut);
    TraceDetail(L"AcrArenaIovaHi", (ULONG)(*ArenaIovaOut >> 32));
    TraceDetail(L"AcrInstIova", (ULONG)*InstIovaOut);
    TraceDetail(L"AcrInstIovaHi", (ULONG)(*InstIovaOut >> 32));
    TraceDetail(L"AcrIommuTagHi", (oldAsid & 0x80000000u) ? 4u : 0u);
    TraceDetail(L"AcrSmmuArenaFirst", arenaFirst);

    gpuPgd = (ULONGLONG *)((PUCHAR)g_AcrVm.Arena + ACR_VM_PGD_OFF);
    gpuPgt = (ULONGLONG *)((PUCHAR)g_AcrVm.Arena + ACR_VM_PGT_OFF);
    inst = (ULONGLONG *)((PUCHAR)g_AcrVm.Arena + ACR_VM_INST_OFF);
    RtlZeroMemory(gpuPgd, 0x10000u);
    RtlZeroMemory(gpuPgt, 0x40000u);
    RtlZeroMemory(inst, PAGE_SIZE);
    /* GM20B uses gk20a_vmm_aper(): NCOH maps to aperture 0 in PTEs.
     * Do not copy the GM200/GF100 aperture value (3) here. */
    attrs = 1ull; /* valid; aperture=0, vol=0, ro=0, priv=0, kind=0 */
    for (i = 0; i < arenaPages; ++i) {
        ULONGLONG iova = (ULONGLONG)*ArenaIovaOut + ((ULONGLONG)i << 12);
        gpuPgt[((ACR_GPU_VA_BASE >> 12) & 0x7FFFu) + i] = (iova >> 8) | attrs;
    }
    pdeIndex = ACR_GPU_VA_BASE >> 27;
    /* Report the entry for the first ACR image byte, the address faulted at
     * in the 0.4.6 run (VA 0x10053000), not merely the start of the mapping. */
    pteIndex = ((ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF) >> 12) & 0x7FFFu;
    /* gf100_vmm_pgd_pde(): pt[1] is the 4K SPT, encoded in the high
     * dword; the low dword selects the large-page table and stays absent.
     * 0.4.14 probes the SoC DRAM policy: VIDMEM + VOL for table walks.
     * Keep the 128K big-page layout (RAMIN bit 11 clear, PDE shift 27).
     * Adapted from Linux nouveau v6.12, Copyright 2017 Red Hat Inc. (MIT).
     * See NOTICE-GM20B-MMU.txt in the test kit for the upstream notice. */
    gpuPgd[pdeIndex] =
        (((ULONGLONG)*ArenaIovaOut + ACR_VM_PGT_OFF) << 24) | (5ull << 32);
    TraceDetail(L"AcrSmallPdeHighHalf", 1u);
    TraceDetail(L"AcrSocTablePolicy", 1u); /* experimental VIDMEM/VOL table route */
    inst[0x200 / sizeof(ULONGLONG)] =
        (((ULONGLONG)*ArenaIovaOut + ACR_VM_PGD_OFF) & ~0xFFFULL) | 4u;
    inst[0x208 / sizeof(ULONGLONG)] = 0xFFFFFFFFFFULL;
    KeMemoryBarrier();

    /* The PMU GPU-MMU tables above are themselves reached through the SoC
     * SMMU. Record the outer identity map at every page relevant to the PDE
     * fault, including the backing pages that contain the GPU tables. */
    TraceDetail(L"AcrSmmuGpuAsid", oldAsid);
    {
        smmuReadbackOk = TRUE;
        smmuPdeIndex = win >> 22;
        smmuPtPa = (ULONGLONG)arenaPa.LowPart + PAGE_SIZE;

        smmuPdeExpected = (ULONG)(smmuPtPa >> 12) |
                          SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
        smmuPdeRead = smmuPd[smmuPdeIndex];
        TraceDetail(L"AcrSmmuPdeIndex", smmuPdeIndex);
        TraceDetail(L"AcrSmmuPdeExpected", smmuPdeExpected);
        TraceDetail(L"AcrSmmuPdeRead", smmuPdeRead);
        smmuReadbackOk = (smmuPdeRead == smmuPdeExpected);
        smmuReadbackOk = AcrTraceSmmuPte(smmuPt, win, arenaPa.LowPart,
                                L"AcrSmmuArenaPteIndex", L"AcrSmmuArenaPteExpected",
                                L"AcrSmmuArenaPteRead") && smmuReadbackOk;
        smmuReadbackOk = AcrTraceSmmuPte(smmuPt, win, arenaPa.LowPart + ACR_VM_PGD_OFF,
                                L"AcrSmmuPgdPteIndex", L"AcrSmmuPgdPteExpected",
                                L"AcrSmmuPgdPteRead") && smmuReadbackOk;
        smmuReadbackOk = AcrTraceSmmuPte(smmuPt, win, arenaPa.LowPart + ACR_VM_PGT_OFF,
                                L"AcrSmmuPgtPteIndex", L"AcrSmmuPgtPteExpected",
                                L"AcrSmmuPgtPteRead") && smmuReadbackOk;
        smmuReadbackOk = AcrTraceSmmuPte(smmuPt, win, arenaPa.LowPart + ACR_VM_INST_OFF,
                                L"AcrSmmuInstPteIndex", L"AcrSmmuInstPteExpected",
                                L"AcrSmmuInstPteRead") && smmuReadbackOk;
        smmuReadbackOk = AcrTraceSmmuPte(smmuPt, win, arenaPa.LowPart + ACR_VM_IMAGE_OFF,
                                L"AcrSmmuImagePteIndex", L"AcrSmmuImagePteExpected",
                                L"AcrSmmuImagePteRead") && smmuReadbackOk;
        TraceDetail(L"AcrSmmuPtbDataRead", Rd(Dev->Mc, MC_SMMU_PTB_DATA));
        smmuMapOk = smmuReadbackOk ? 1u : 0u;
    }
    TraceDurable(L"AcrSmmuMapReadback", smmuMapOk);

    TraceDetail(L"AcrPgdIova", (ULONG)(*ArenaIovaOut + ACR_VM_PGD_OFF));
    TraceDetail(L"AcrPgdIovaHi", (ULONG)((*ArenaIovaOut + ACR_VM_PGD_OFF) >> 32));
    TraceDetail(L"AcrPgtIova", (ULONG)(*ArenaIovaOut + ACR_VM_PGT_OFF));
    TraceDetail(L"AcrPgtIovaHi", (ULONG)((*ArenaIovaOut + ACR_VM_PGT_OFF) >> 32));
    TraceDetail(L"AcrPdeIndex", pdeIndex);
    TraceDetail(L"AcrPdeLo", (ULONG)gpuPgd[pdeIndex]);
    TraceDetail(L"AcrPdeHi", (ULONG)(gpuPgd[pdeIndex] >> 32));
    TraceDetail(L"AcrPteVa", ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF);
    TraceDetail(L"AcrPteIndex", pteIndex);
    TraceDetail(L"AcrPteLo", (ULONG)gpuPgt[pteIndex]);
    TraceDetail(L"AcrPteHi", (ULONG)(gpuPgt[pteIndex] >> 32));
    TraceDetail(L"AcrInstPdbLo", (ULONG)inst[0x200 / sizeof(ULONGLONG)]);
    TraceDetail(L"AcrInstPdbHi", (ULONG)(inst[0x200 / sizeof(ULONGLONG)] >> 32));
    TraceDetail(L"AcrInstLimitLo", (ULONG)inst[0x208 / sizeof(ULONGLONG)]);
    TraceDetail(L"AcrInstLimitHi", (ULONG)(inst[0x208 / sizeof(ULONGLONG)] >> 32));
    g_AcrVm.Active = TRUE;
    return TRUE;
}

static BOOLEAN AcrVmBind(volatile UCHAR *B, ULONGLONG InstIova)
{
    ULONG a = 0, d = 0, ap = 0, dp = 0;
    BOOLEAN ok = PmuBindInst(B, InstIova, &a, &d, &ap, &dp);
    TraceDetail(L"AcrBindInstWritten", 0x70000000u | (ULONG)(InstIova >> 12));
    TraceDetail(L"AcrBindInstRead", Rd(B, PMU_FALCON + FALCON_BIND_INST));
    TraceDetail(L"AcrBindAckState", a);
    TraceDetail(L"AcrBindDoneState", d);
    Trace(L"AcrBindAckPolls", ap);
    Trace(L"AcrBindDonePolls", dp);
    TraceDurable(L"AcrBindOk", ok ? 1u : 0u);
    return ok;
}

/* Match nouveau's gf100_vmm_invalidate(): 0x100C80[23:16] is the available
 * invalidate-slot count. Wait boundedly for a nonzero count, select this PDB
 * in 0x100CB8, request PAGE_ALL via 0x100CBC, and wait for it to queue. */
static BOOLEAN AcrMmuInvalidatePdb(volatile UCHAR *B, ULONGLONG PgdIova)
{
    /* Match the experimental SoC DRAM PDB target (VIDMEM=0). The PMU's
     * separate NEW_INSTBLK target remains SYS_NCOH as in gm20b bootstrap. */
    ULONGLONG pdb = (((ULONGLONG)PgdIova >> 12) << 4);
    ULONG status = 0, pdbRead = 0, polls;
    BOOLEAN queued = FALSE;

    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if ((status & 0x00FF0000u) != 0) break;
        Us(5);
    }
    Trace(L"AcrMmuInvSlotPolls", polls);
    TraceDetail(L"AcrMmuInvSlotStatus", status);
    TraceDetail(L"AcrMmuInvFreeSlots", (status & 0x00FF0000u) >> 16);
    TraceDetail(L"AcrMmuInvPendingBefore", (status & 0x00008000u) ? 1u : 0u);
    if (polls == 2000) goto done;

    Wr(B, 0x100CB8, (ULONG)pdb);
    pdbRead = Rd(B, 0x100CB8);
    TraceDetail(L"AcrMmuInvPdb", (ULONG)pdb);
    TraceDetail(L"AcrMmuInvPdbRead", pdbRead);
    /* Nouveau programs 0x100CB8 without reading it back. Keep the readback
     * diagnostic only: this register may not return the written PDB value. */

    Wr(B, 0x100CBC, 0x80000001u); /* invalidate request + PAGE_ALL */
    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if (status & 0x00008000u) { queued = TRUE; break; }
        Us(5);
    }
    Trace(L"AcrMmuInvDonePolls", polls);
    TraceDetail(L"AcrMmuInvDoneStatus", status);
done:
    TraceDurable(L"AcrMmuInvOk", queued ? 1u : 0u);
    return queued;
}

/* Diagnostic discriminator for a stale/incorrect per-PDB translation: after
 * the normal selected-PDB PAGE_ALL request, issue PAGE_ALL + ALL_PDB.  The
 * status bit is reported as an observation only; it may already be set before
 * the request and is not treated as proof that this particular request ran. */
static BOOLEAN AcrMmuInvalidateAllPdbs(volatile UCHAR *B)
{
    ULONG status = 0, polls;
    BOOLEAN fifoEmptyObserved = FALSE;

    status = Rd(B, 0x100C80);
    TraceDetail(L"AcrMmuAllPdbBeforeStatus", status);
    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if ((status & 0x00FF0000u) != 0) break;
        Us(5);
    }
    Trace(L"AcrMmuAllPdbSlotPolls", polls);
    TraceDetail(L"AcrMmuAllPdbSlotStatus", status);
    TraceDetail(L"AcrMmuAllPdbFreeSlots", (status & 0x00FF0000u) >> 16);
    if (polls == 2000) {
        TraceDurable(L"AcrMmuAllPdbInvOk", 0);
        return FALSE;
    }

    Wr(B, 0x100CBC, 0x80000003u); /* trigger + PAGE_ALL + ALL_PDB */
    KeMemoryBarrier();
    status = Rd(B, 0x100C80);
    TraceDetail(L"AcrMmuAllPdbRequest", 0x80000003u);
    TraceDetail(L"AcrMmuAllPdbPostWriteStatus", status);
    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if (status & 0x00008000u) {
            fifoEmptyObserved = TRUE;
            break;
        }
        Us(5);
    }
    Trace(L"AcrMmuAllPdbStatusPolls", polls);
    TraceDetail(L"AcrMmuAllPdbStatus", status);
    TraceDurable(L"AcrMmuAllPdbInvOk", fifoEmptyObserved ? 1u : 0u);
    return fifoEmptyObserved;
}

/* Called only after the GPU block reset, so no Falcon DMA can still reference
 * these backing pages. */
static VOID AcrVmCleanupAfterGpuReset(PSWGPU_DEVICE Dev)
{
    ULONG restoreData, restoreAsid;
    if (g_AcrVm.PtbChanged) {
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
        Wr(Dev->Mc, MC_SMMU_PTB_DATA, g_AcrVm.OldPtbData);
        Wr(Dev->Mc, MC_SMMU_PTC_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_TLB_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        restoreData = Rd(Dev->Mc, MC_SMMU_PTB_DATA);
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, g_AcrVm.OldPtbAsid);
        restoreAsid = Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        TraceDetail(L"AcrSmmuRestoreData", restoreData);
        TraceDurable(L"AcrSmmuRestored",
                     restoreData == g_AcrVm.OldPtbData && restoreAsid == g_AcrVm.OldPtbAsid ? 1u : 0u);
    } else {
        TraceDurable(L"AcrSmmuRestored", 1u); /* no SMMU page-table change */
    }
    E4Return(Dev, SwGpuPoolWpr, g_AcrVm.WprBuffer, g_AcrVm.WprBytes);
    E4Return(Dev, SwGpuPoolAcr, g_AcrVm.Arena, ACR_VM_ARENA_SIZE);
    RtlZeroMemory(&g_AcrVm, sizeof(g_AcrVm));
}
#define SWGPU_TAG          'uGwS'

static PUCHAR ReadBinParam(PCWSTR Name, ULONG Max, PULONG Len)
{
    UNICODE_STRING path, name;
    OBJECT_ATTRIBUTES oa;
    HANDLE key;
    ULONG need = 0;
    PKEY_VALUE_PARTIAL_INFORMATION kv;
    PUCHAR out = NULL;
    NTSTATUS st;

    *Len = 0;
    if (SwGpuParamsPath[0] == L'\0') { Trace(L"FwReadFailStage", 1u); return NULL; }
    RtlInitUnicodeString(&path, SwGpuParamsPath);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    st = ZwOpenKey(&key, KEY_READ, &oa);
    if (!NT_SUCCESS(st)) {
        /* Claude 0.12.5: F3 boot start failed with E0000041 (no ACR blobs). */
        Trace(L"FwReadFailStage", 2u);
        Trace(L"FwReadFailStatus", (ULONG)st);
        return NULL;
    }
    RtlInitUnicodeString(&name, Name);
    st = ZwQueryValueKey(key, &name, KeyValuePartialInformation, NULL, 0, &need);
    if (!((st == STATUS_BUFFER_TOO_SMALL || st == STATUS_BUFFER_OVERFLOW) &&
          need > 0 && need <= Max + sizeof(KEY_VALUE_PARTIAL_INFORMATION))) {
        Trace(L"FwReadFailStage", 3u);
        Trace(L"FwReadFailStatus", (ULONG)st);
        Trace(L"FwReadFailNeed", need);
        Trace(L"FwReadFailMax", Max);
        Trace(L"FwReadFailNameLen", (ULONG)name.Length);
    }
    if ((st == STATUS_BUFFER_TOO_SMALL || st == STATUS_BUFFER_OVERFLOW) &&
        need > 0 && need <= Max + sizeof(KEY_VALUE_PARTIAL_INFORMATION)) {
        kv = (PKEY_VALUE_PARTIAL_INFORMATION)ExAllocatePool2(POOL_FLAG_PAGED, need, SWGPU_TAG);
        if (kv != NULL) {
            if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, kv, need, &need)) &&
                kv->Type == REG_BINARY && kv->DataLength > 0 && kv->DataLength <= Max) {
                out = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, kv->DataLength, SWGPU_TAG);
                if (out != NULL) {
                    RtlCopyMemory(out, kv->Data, kv->DataLength);
                    *Len = kv->DataLength;
                }
            }
            ExFreePoolWithTag(kv, SWGPU_TAG);
        }
    }
    ZwClose(key);
    return out;
}

static ULONG Le32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* TRUE when [Off, Off + Size) lies inside a blob of Len bytes (no overflow). */
static BOOLEAN InBlob(ULONG Len, ULONG Off, ULONG Size)
{
    return Off <= Len && Size <= Len - Off;
}

static VOID PmuReset(volatile UCHAR *B, PCWSTR PollName)
{
    ULONG en = Rd(B, NV_PMC_ENABLE), w;
    Wr(B, NV_PMC_ENABLE, en & ~(1u << 13));
    (VOID)Rd(B, NV_PMC_ENABLE);
    Us(20);
    Wr(B, NV_PMC_ENABLE, en | (1u << 13));
    (VOID)Rd(B, NV_PMC_ENABLE);
    for (w = 0; w < 2000 && (Rd(B, PMU_FALCON + FALCON_DMACTL) & 6u); ++w) Us(5);
    Trace(PollName, w);
}

static VOID WprPutLe32(PUCHAR P, ULONG V)
{
    P[0] = (UCHAR)V;
    P[1] = (UCHAR)(V >> 8);
    P[2] = (UCHAR)(V >> 16);
    P[3] = (UCHAR)(V >> 24);
}

static VOID WprPutLe64(PUCHAR P, ULONGLONG V)
{
    WprPutLe32(P, (ULONG)V);
    WprPutLe32(P + 4, (ULONG)(V >> 32));
}

/* Build a GM20B WPR image from the staged Linux firmware blobs. Runtime PMU
 * loader_config addresses are physical WPR addresses (WPR base + image
 * offsets), as populated by nvgpu and relocated by nouveau before ACR.
 * WprBase is the protected WPR base read from the device. */
static ULONG BuildWprB3b(ULONG PmuDmemSize, ULONGLONG WprBase, PUCHAR *OutWpr, PULONG OutAllocLen, PULONG OutUsed, PULONG OutPmuArgv)
{
    PUCHAR pmuDesc = NULL, pmuImage = NULL, pmuSig = NULL;
    PUCHAR fecsBl = NULL, fecsInst = NULL, fecsData = NULL, fecsSig = NULL;
    PUCHAR fecsImage = NULL, wpr = NULL;
    ULONG pmuDescLen = 0, pmuImageLen = 0, pmuSigLen = 0;
    ULONG fecsBlLen = 0, fecsInstLen = 0, fecsDataLen = 0, fecsSigLen = 0;
    ULONG blHdrOff, blDataOff, blDataSize, blDescOff;
    ULONG fecsStartTag, fecsCodeSize, fecsCodeOff;
    ULONG pmuBootStart, pmuBootSize, pmuBootImem, pmuAppStart, pmuAppSize;
    ULONG pmuAppCodeOff, pmuAppCodeSize, pmuAppDataOff, pmuAppDataSize;
    ULONG pmuCodeOffset, pmuDataOffset, pmuUcodeSize, pmuDataSize;
    ULONG pmuLsbOff, pmuImgOff, pmuBldOff, fecsLsbOff, fecsImgOff, fecsBldOff;
    ULONG fecsBootPad, fecsInstPad, fecsDataPad, fecsImageLen, fecsUcodeSize;
    ULONG fecsDataAreaSize, totalUsed, allocLen, i, ok, result = 0xE0000051u;
    ULONG pmuBld[13], fecsBld[19];
    ULONG descSize, imageSize;
    ULONGLONG pmuCodeAddr, pmuDataAddr;

    if (OutWpr != NULL) *OutWpr = NULL;
    if (OutAllocLen != NULL) *OutAllocLen = 0;
    if (OutUsed != NULL) *OutUsed = 0;
    if (OutPmuArgv != NULL) *OutPmuArgv = 0;
    if (WprBase == 0) {
        result = 0xE0000077u;
        goto done;
    }

    Step(46); /* parse firmware descriptors and construct the CPU-side WPR image */
    pmuDesc = ReadBinParam(L"FwPmuDesc", 0x20000u, &pmuDescLen);
    pmuImage = ReadBinParam(L"FwPmuImage", 0x20000u, &pmuImageLen);
    pmuSig = ReadBinParam(L"FwPmuSig", 0x20000u, &pmuSigLen);
    fecsBl = ReadBinParam(L"FwFecsBl", 0x20000u, &fecsBlLen);
    fecsInst = ReadBinParam(L"FwFecsInst", 0x20000u, &fecsInstLen);
    fecsData = ReadBinParam(L"FwFecsData", 0x20000u, &fecsDataLen);
    fecsSig = ReadBinParam(L"FwFecsSig", 0x20000u, &fecsSigLen);
    TraceDetail(L"B3bPmuDescLen", pmuDescLen);
    TraceDetail(L"B3bPmuImageLen", pmuImageLen);
    TraceDetail(L"B3bPmuSigLen", pmuSigLen);
    TraceDetail(L"B3bFecsBlLen", fecsBlLen);
    TraceDetail(L"B3bFecsInstLen", fecsInstLen);
    TraceDetail(L"B3bFecsDataLen", fecsDataLen);
    TraceDetail(L"B3bFecsSigLen", fecsSigLen);
    /* Keep the 0.4.1 kit's existing SUMMARY field names usable. */
    TraceDetail(L"B3aFwPmuDescLen", pmuDescLen);
    TraceDetail(L"B3aFwPmuImageLen", pmuImageLen);
    TraceDetail(L"B3aFwPmuSigLen", pmuSigLen);
    TraceDetail(L"B3aFwFecsBlLen", fecsBlLen);
    TraceDetail(L"B3aFwFecsInstLen", fecsInstLen);
    TraceDetail(L"B3aFwFecsDataLen", fecsDataLen);
    TraceDetail(L"B3aFwFecsSigLen", fecsSigLen);
    if (pmuDesc == NULL || pmuImage == NULL || pmuSig == NULL || fecsBl == NULL ||
        fecsInst == NULL || fecsData == NULL || fecsSig == NULL) {
        result = 0xE0000052u; goto done;
    }

    /* nvfw_ls_desc_head: 132-byte fixed header, followed by overlay data. */
    if (pmuDescLen < 132 || pmuImageLen == 0 || pmuSigLen != 76 || fecsSigLen != 76 ||
        fecsBlLen < 24 || fecsInstLen == 0 || fecsDataLen == 0 ||
        Le32(pmuSig + 72) != 0 || Le32(fecsSig + 72) != 2) {
        result = 0xE0000053u; goto done;
    }
    descSize = Le32(pmuDesc + 0);
    imageSize = Le32(pmuDesc + 4);
    pmuBootStart = Le32(pmuDesc + 80);
    pmuBootSize = Le32(pmuDesc + 84);
    pmuBootImem = Le32(pmuDesc + 88);
    pmuAppStart = Le32(pmuDesc + 96);
    pmuAppSize = Le32(pmuDesc + 100);
    pmuAppCodeOff = Le32(pmuDesc + 116);
    pmuAppCodeSize = Le32(pmuDesc + 120);
    pmuAppDataOff = Le32(pmuDesc + 124);
    pmuAppDataSize = Le32(pmuDesc + 128);
    TraceDetail(L"B3bPmuDescSize", descSize);
    TraceDetail(L"B3bPmuImageSize", imageSize);
    TraceDetail(L"B3cPmuAppVersion", Le32(pmuDesc + 12));
    TraceDetail(L"B3bPmuBootSize", pmuBootSize);
    TraceDetail(L"B3bPmuAppSize", pmuAppSize);
    if (descSize < 132 || descSize > pmuDescLen || imageSize == 0 || imageSize > pmuImageLen ||
        pmuBootSize == 0 || pmuAppSize == 0 ||
        !InBlob(imageSize, pmuBootStart, pmuBootSize) ||
        !InBlob(imageSize, pmuAppStart, pmuAppSize) ||
        !InBlob(pmuAppSize, pmuAppCodeOff, pmuAppCodeSize) ||
        !InBlob(pmuAppSize, pmuAppDataOff, pmuAppDataSize) ||
        pmuAppCodeSize == 0 || pmuAppDataSize == 0 ||
        pmuBootSize > 0x1FFFFu || pmuAppSize > 0x1FFFFu ||
        PmuDmemSize < 44) {
        result = 0xE0000054u; goto done;
    }
    pmuCodeOffset = pmuAppStart + pmuAppCodeOff;
    pmuDataOffset = pmuAppStart + pmuAppDataOff;
    if (!InBlob(imageSize, pmuCodeOffset, pmuAppCodeSize) ||
        !InBlob(imageSize, pmuDataOffset, pmuAppDataSize)) {
        result = 0xE0000055u; goto done;
    }
    pmuBootSize = (pmuBootSize + 0xFFu) & ~0xFFu;
    pmuAppSize = (pmuAppSize + 0xFFu) & ~0xFFu;
    pmuUcodeSize = ((pmuAppDataOff + 0xFFu) & ~0xFFu) + pmuBootSize;
    if (pmuUcodeSize > pmuAppSize + pmuBootSize) {
        result = 0xE0000056u; goto done;
    }
    pmuDataSize = pmuAppSize + pmuBootSize - pmuUcodeSize;

    /* nvfw_bin_hdr + nvfw_bl_desc; Nouveau takes the bootloader bytes from
     * hdr.data_offset, then appends INST and DATA with 256-byte zero padding. */
    if (!InBlob(fecsBlLen, 0, 24)) { result = 0xE0000057u; goto done; }
    blHdrOff = Le32(fecsBl + 12);
    blDataOff = Le32(fecsBl + 16);
    blDataSize = Le32(fecsBl + 20);
    if (!InBlob(fecsBlLen, blHdrOff, 24) || !InBlob(fecsBlLen, blDataOff, blDataSize)) {
        result = 0xE0000058u; goto done;
    }
    blDescOff = blHdrOff;
    fecsStartTag = Le32(fecsBl + blDescOff);
    fecsCodeOff = Le32(fecsBl + blDescOff + 8);
    fecsCodeSize = Le32(fecsBl + blDescOff + 12);
    TraceDetail(L"B3bFecsStartTag", fecsStartTag);
    TraceDetail(L"B3bFecsCodeOff", fecsCodeOff);
    TraceDetail(L"B3bFecsCodeSize", fecsCodeSize);
    if (fecsCodeSize == 0 || fecsCodeSize > 0x1FFFFu ||
        !InBlob(blDataSize, 0, fecsCodeSize) || fecsStartTag > 0xFFFFu ||
        fecsInstLen > 0x1FFFFu || fecsDataLen > 0x1FFFFu) {
        result = 0xE0000059u; goto done;
    }
    fecsBootPad = (fecsCodeSize + 0xFFu) & ~0xFFu;
    fecsInstPad = (fecsInstLen + 0xFFu) & ~0xFFu;
    fecsDataPad = (fecsDataLen + 0xFFu) & ~0xFFu;
    if (fecsBootPad > 0x20000u - fecsInstPad ||
        fecsBootPad + fecsInstPad > 0x20000u - fecsDataPad) {
        result = 0xE000005Au; goto done;
    }
    fecsImageLen = fecsBootPad + fecsInstPad + fecsDataPad;
    fecsUcodeSize = fecsBootPad + fecsInstPad;
    fecsDataAreaSize = fecsBootPad + fecsInstPad + fecsDataPad - fecsUcodeSize;

    /* Match gm200_acr_wpr_layout: 11 headers reserved, then LSB/image/BLD
     * entries aligned to 256/4096/256 bytes. WPR allocation is 0x40000-aligned. */
    pmuLsbOff = 11u * 20u;
    pmuLsbOff = (pmuLsbOff + 0xFFu) & ~0xFFu;
    pmuImgOff = (pmuLsbOff + 124u + 0xFFFu) & ~0xFFFu;
    if (imageSize > 0x40000u - pmuImgOff) { result = 0xE000005Bu; goto done; }
    pmuBldOff = (pmuImgOff + imageSize + 0xFFu) & ~0xFFu;
    fecsLsbOff = pmuBldOff + 256u;
    fecsImgOff = (fecsLsbOff + 124u + 0xFFFu) & ~0xFFFu;
    if (fecsImageLen > 0x40000u - fecsImgOff) { result = 0xE000005Cu; goto done; }
    fecsBldOff = (fecsImgOff + fecsImageLen + 0xFFu) & ~0xFFu;
    if (fecsBldOff > 0x40000u - 256u) { result = 0xE000005Du; goto done; }
    totalUsed = fecsBldOff + 256u;
    allocLen = (totalUsed + 0x3FFFFu) & ~0x3FFFFu;
    if (allocLen != 0x40000u) { result = 0xE000005Eu; goto done; }
    TraceDetail(L"B3bPmuLsbOff", pmuLsbOff);
    TraceDetail(L"B3bPmuImgOff", pmuImgOff);
    TraceDetail(L"B3bPmuBldOff", pmuBldOff);
    TraceDetail(L"B3bFecsLsbOff", fecsLsbOff);
    TraceDetail(L"B3bFecsImgOff", fecsImgOff);
    TraceDetail(L"B3bFecsBldOff", fecsBldOff);
    TraceDetail(L"B3bWprUsed", totalUsed);
    TraceDetail(L"B3bWprAlloc", allocLen);
    if (OutUsed != NULL) *OutUsed = totalUsed;

    wpr = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, allocLen, SWGPU_TAG);
    fecsImage = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, fecsImageLen, SWGPU_TAG);
    if (wpr == NULL || fecsImage == NULL) { result = 0xE000005Fu; goto done; }
    RtlZeroMemory(wpr, allocLen);
    RtlZeroMemory(fecsImage, fecsImageLen);
    RtlCopyMemory(wpr + pmuImgOff, pmuImage, imageSize);
    RtlCopyMemory(fecsImage, fecsBl + blDataOff, fecsCodeSize);
    RtlCopyMemory(fecsImage + fecsBootPad, fecsInst, fecsInstLen);
    RtlCopyMemory(fecsImage + fecsBootPad + fecsInstPad, fecsData, fecsDataLen);
    RtlCopyMemory(wpr + fecsImgOff, fecsImage, fecsImageLen);

    /* Two WPR v0 records (PMU id 0, FECS id 2), followed by the invalid id. */
    WprPutLe32(wpr + 0, 0); WprPutLe32(wpr + 4, pmuLsbOff);
    WprPutLe32(wpr + 8, 0); WprPutLe32(wpr + 12, 0); WprPutLe32(wpr + 16, 1);
    WprPutLe32(wpr + 20, 2); WprPutLe32(wpr + 24, fecsLsbOff);
    WprPutLe32(wpr + 28, 0); WprPutLe32(wpr + 32, 1); WprPutLe32(wpr + 36, 1);
    WprPutLe32(wpr + 40, 0xFFFFFFFFu);

    RtlCopyMemory(wpr + pmuLsbOff, pmuSig, 76);
    WprPutLe32(wpr + pmuLsbOff + 76, pmuImgOff);
    WprPutLe32(wpr + pmuLsbOff + 80, pmuUcodeSize);
    WprPutLe32(wpr + pmuLsbOff + 84, pmuDataSize);
    WprPutLe32(wpr + pmuLsbOff + 88, pmuBootSize);
    WprPutLe32(wpr + pmuLsbOff + 92, pmuBootImem);
    WprPutLe32(wpr + pmuLsbOff + 96, pmuBldOff);
    WprPutLe32(wpr + pmuLsbOff + 100, 256);
    WprPutLe32(wpr + pmuLsbOff + 104, pmuAppStart + pmuAppCodeOff);
    WprPutLe32(wpr + pmuLsbOff + 108, pmuAppCodeSize);
    WprPutLe32(wpr + pmuLsbOff + 112, pmuAppStart + pmuAppDataOff);
    WprPutLe32(wpr + pmuLsbOff + 116, pmuAppDataSize);
    WprPutLe32(wpr + pmuLsbOff + 120, 4); /* DMACTL_REQ_CTX */

    RtlCopyMemory(wpr + fecsLsbOff, fecsSig, 76);
    WprPutLe32(wpr + fecsLsbOff + 76, fecsImgOff);
    WprPutLe32(wpr + fecsLsbOff + 80, fecsUcodeSize);
    WprPutLe32(wpr + fecsLsbOff + 84, fecsDataAreaSize);
    WprPutLe32(wpr + fecsLsbOff + 88, fecsBootPad);
    WprPutLe32(wpr + fecsLsbOff + 92, fecsStartTag << 8);
    WprPutLe32(wpr + fecsLsbOff + 96, fecsBldOff);
    WprPutLe32(wpr + fecsLsbOff + 100, 256);
    WprPutLe32(wpr + fecsLsbOff + 104, fecsBootPad);
    WprPutLe32(wpr + fecsLsbOff + 108, fecsInstPad);
    WprPutLe32(wpr + fecsLsbOff + 112, fecsBootPad + fecsInstPad);
    WprPutLe32(wpr + fecsLsbOff + 116, fecsDataPad);
    WprPutLe32(wpr + fecsLsbOff + 120, 0);

    RtlZeroMemory(pmuBld, sizeof(pmuBld));
    /* DMAIDX_UCODE (0) sends these addresses to the protected local WPR. */
    pmuBld[0] = PMU_UCODE_DMA_IDX;
    pmuCodeAddr = WprBase + (ULONGLONG)pmuImgOff + pmuCodeOffset;
    pmuDataAddr = WprBase + (ULONGLONG)pmuImgOff + pmuDataOffset;
    if ((pmuCodeAddr >> 8) > 0xFFFFFFFFull || (pmuDataAddr >> 8) > 0xFFFFFFFFull) {
        result = 0xE0000077u;
        goto done;
    }
    pmuBld[1] = (ULONG)(pmuCodeAddr >> 8);
    pmuBld[2] = pmuAppSize;
    pmuBld[3] = pmuAppCodeSize;
    pmuBld[4] = Le32(pmuDesc + 108); /* app_imem_entry */
    pmuBld[5] = (ULONG)(pmuDataAddr >> 8);
    pmuBld[6] = pmuAppDataSize;
    pmuBld[7] = pmuBld[1];
    pmuBld[8] = 1;
    pmuBld[9] = PmuDmemSize - 44u; /* sizeof(struct nv_pmu_args) */
    for (i = 0; i < RTL_NUMBER_OF(pmuBld); ++i)
        WprPutLe32(wpr + pmuBldOff + i * 4u, pmuBld[i]);

    RtlZeroMemory(fecsBld, sizeof(fecsBld));
    /* GM20B FECS LS loader uses DMAIDX_UCODE and physical WPR addresses.
     * nvgpu gm20b_gr_acr_bld_write() builds this descriptor from WPR base;
     * Nouveau gm20b_gr_acr_bld_patch() applies the equivalent relocation. */
    fecsBld[8] = PMU_UCODE_DMA_IDX;
    if (((WprBase + (ULONGLONG)fecsImgOff + fecsBootPad) >> 8) > 0xFFFFFFFFull ||
        ((WprBase + (ULONGLONG)fecsImgOff + fecsBootPad + fecsInstPad) >> 8) > 0xFFFFFFFFull) {
        result = 0xE0000077u;
        goto done;
    }
    fecsBld[9] = (ULONG)((WprBase + (ULONGLONG)fecsImgOff + fecsBootPad) >> 8);
    fecsBld[10] = 0;
    fecsBld[11] = fecsInstPad;
    fecsBld[14] = 0;
    fecsBld[15] = (ULONG)((WprBase + (ULONGLONG)fecsImgOff + fecsBootPad + fecsInstPad) >> 8);
    fecsBld[16] = fecsDataPad;
    for (i = 0; i < RTL_NUMBER_OF(fecsBld); ++i)
        WprPutLe32(wpr + fecsBldOff + i * 4u, fecsBld[i]);

    /* CPU-only integrity/readback checks; this is not a GPU DMA proof. */
    TraceBin(L"B3bWprHeaders", wpr, 60);
    TraceBin(L"B3bPmuLsb", wpr + pmuLsbOff, 124);
    TraceBin(L"B3bPmuBld", wpr + pmuBldOff, sizeof(pmuBld));
    TraceBin(L"B3bFecsLsb", wpr + fecsLsbOff, 124);
    TraceBin(L"B3bFecsBld", wpr + fecsBldOff, sizeof(fecsBld));
    ok = (Le32(wpr) == 0 && Le32(wpr + 20) == 2 && Le32(wpr + 40) == 0xFFFFFFFFu &&
          Le32(wpr + pmuLsbOff + 72) == 0 && Le32(wpr + fecsLsbOff + 72) == 2 &&
          Le32(wpr + pmuLsbOff + 76) == pmuImgOff &&
          Le32(wpr + fecsLsbOff + 76) == fecsImgOff &&
          Le32(wpr + pmuBldOff) == PMU_UCODE_DMA_IDX && Le32(wpr + fecsBldOff + 32) == PMU_UCODE_DMA_IDX &&
          Le32(wpr + fecsBldOff + 36) == fecsBld[9] &&
          Le32(wpr + fecsBldOff + 60) == fecsBld[15] &&
          RtlCompareMemory(wpr + pmuImgOff, pmuImage, imageSize) == imageSize &&
          RtlCompareMemory(wpr + fecsImgOff, fecsImage, fecsImageLen) == fecsImageLen) ? 1u : 0u;
    TraceDetail(L"B3cPmuCodeDmaBase", pmuBld[1]);
    TraceDetail(L"B3bPmuDmaIdx", pmuBld[0]);
    TraceDetail(L"B3cPmuDataDmaBase", pmuBld[5]);
    TraceDetail(L"B3cPmuWprBaseLo", (ULONG)WprBase);
    TraceDetail(L"B3cPmuWprBaseHi", (ULONG)(WprBase >> 32));
    TraceDetail(L"B3cPmuDmaBaseAbsolute", WprBase != 0 ? 1u : 0u);
    TraceDetail(L"B3bPmuArgv", pmuBld[9]);
    TraceDetail(L"B3cFecsDmaIdx", fecsBld[8]);
    TraceDetail(L"B3cFecsCodeDmaBase", fecsBld[9]);
    TraceDetail(L"B3cFecsDataDmaBase", fecsBld[15]);
    TraceDetail(L"B3cFecsDmaBaseAbsolute", WprBase != 0 ? 1u : 0u);
    TraceBin(L"B3cFecsBldWords", fecsBld, sizeof(fecsBld));
    TraceDurable(L"B3bWprCpuReadback", ok);
    result = ok ? 0 : 0xE0000060u;

done:
    TraceDurable(L"B3bResult", result);
    if (result == 0 && OutWpr != NULL && OutAllocLen != NULL) {
        *OutWpr = wpr;
        *OutAllocLen = allocLen;
        if (OutPmuArgv != NULL) *OutPmuArgv = pmuBld[9];
        wpr = NULL;
    }
    if (wpr != NULL) ExFreePoolWithTag(wpr, SWGPU_TAG);
    if (fecsImage != NULL) ExFreePoolWithTag(fecsImage, SWGPU_TAG);
    if (fecsSig != NULL) ExFreePoolWithTag(fecsSig, SWGPU_TAG);
    if (fecsData != NULL) ExFreePoolWithTag(fecsData, SWGPU_TAG);
    if (fecsInst != NULL) ExFreePoolWithTag(fecsInst, SWGPU_TAG);
    if (fecsBl != NULL) ExFreePoolWithTag(fecsBl, SWGPU_TAG);
    if (pmuSig != NULL) ExFreePoolWithTag(pmuSig, SWGPU_TAG);
    if (pmuImage != NULL) ExFreePoolWithTag(pmuImage, SWGPU_TAG);
    if (pmuDesc != NULL) ExFreePoolWithTag(pmuDesc, SWGPU_TAG);
    return result;
}

/* The ACR PMU loader_config advertises argv = DMEM - sizeof(nv_pmu_args)
 * (44 bytes, as in nouveau gm20b_pmu_acr_bld_write). Write that ABI at the
 * advertised address. mesa tg_gpu_pmu_start's 32-byte cmdline_args_v3 is a
 * different host-start path and must not override this WPR loader argv. */
static ULONG B3cStartPmuLs(PSWGPU_DEVICE Dev, ULONG PmuArgv)
{
    volatile UCHAR *B = Dev->Bar0;
    static const ULONG aperture[5] = { 4, 0, 4, 5, 6 };
    /* struct nv_pmu_args (44 bytes): reserved, freq, trace fields, secure_mode
     * at byte 24, remaining privilege/GC6 fields zero. */
    ULONG args[11] = { 0, 204000000u, 0, 0, 0, 0, 1, 0, 0, 0, 0 };
    ULONG argsReadback[11];
    ULONG cpuctl, irq, hwcfg, dmemSize = 0, argsOff = 0, i, polls = 0, head, tail, result = 0;
    ULONG initBadPairs = 0, initBadHead = 0, initBadTail = 0, initBadPoll = 0;
    ULONG dmaCtlBeforeStart, dmaCtlAtStart;
    ULONG traceIdx, traceNr, traceSlot, tracePc[8], argsOk, argsCursor;
    ULONG initWords[11], initHdr, initType, initCursor;
    ULONG diagWords[64], diagOff, diagCount, diagI;
    ULONG imemHwcfg, imemSize, imemOff, imemWords[16], imemOk;
    ULONG transcfg[8], faultRecs[32][5], faultNr, faultId, faultUnit;

    Step(58);
    cpuctl = Rd(B, PMU_FALCON + FALCON_CPUCTL);
    TraceDetail(L"B3cLsPmuPreCpuctl", cpuctl);
    TraceDetail(L"B3cLsPmuPreSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
    TraceDetail(L"B3cLsPmuPreMailbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
    TraceDetail(L"B3cLsPmuPreMailbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
    if ((cpuctl & 0x50u) != 0x50u ||
        Rd(B, PMU_FALCON + FALCON_MAILBOX0) != 0 ||
        Rd(B, PMU_FALCON + FALCON_MAILBOX1) != 0) {
        result = 0xE000006Du;
        goto done;
    }
    hwcfg = Rd(B, PMU_FALCON + FALCON_HWCFG);
    dmemSize = ((hwcfg >> 9) & 0x1FFu) << 8;
    TraceDetail(L"B3cLsPmuDmemSize", dmemSize);
    if (dmemSize < 256u || dmemSize > 0xFFFFu) {
        result = 0xE000006Eu;
        goto done;
    }
    Wr(B, PMU_FALCON + 0x004, 0x10u);
    irq = Rd(B, PMU_FALCON + 0x008);
    TraceDetail(L"B3cLsPmuIrqAfterClear", irq);
    if (irq & 0x10u) {
        result = 0xE000006Fu;
        goto done;
    }
    for (i = 0; i < RTL_NUMBER_OF(aperture); ++i)
        Wr(B, PMU_FALCON + 0xE00 + i * 4u, aperture[i]);
    TraceDetail(L"B3cLsPmuAperture0", Rd(B, PMU_FALCON + 0xE00));
    TraceDetail(L"B3cLsPmuAperture4", Rd(B, PMU_FALCON + 0xE10));

    argsOff = dmemSize - sizeof(args);
    TraceDetail(L"B3cLsPmuArgvAdvertised", PmuArgv);
    TraceDetail(L"B3cLsPmuArgvComputed", argsOff);
    TraceDetail(L"B3cLsPmuArgsSize", sizeof(args));
    TraceDurable(L"B3cLsPmuArgvMatch", argsOff == PmuArgv ? 1u : 0u);
    if (argsOff != PmuArgv) {
        result = 0xE0000074u;
        goto done;
    }
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | argsOff);
    for (i = 0; i < RTL_NUMBER_OF(args); ++i)
        Wr(B, PMU_FALCON + FALCON_DMEMD0, args[i]);
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | argsOff);
    argsOk = 1;
    for (i = 0; i < RTL_NUMBER_OF(args); ++i) {
        argsReadback[i] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        if (argsReadback[i] != args[i]) argsOk = 0;
    }
    argsCursor = Rd(B, PMU_FALCON + FALCON_DMEMC0) & 0xFFFFu;
    TraceDetail(L"B3cLsPmuArgsOff", argsOff);
    TraceDetail(L"B3cLsPmuArgsCursor", argsCursor);
    TraceDurable(L"B3cLsPmuArgsOk", argsOk);
    TraceBin(L"B3cLsPmuArgvReadback", argsReadback, sizeof(argsReadback));
    if (!argsOk || argsCursor != argsOff + sizeof(args)) {
        result = 0xE0000073u;
        goto done;
    }

    TraceDetail(L"B3cLsPmuItfenBeforeStart", Rd(B, PMU_FALCON + FALCON_ITFEN));
    TraceDetail(L"B3cLsPmuCpuctlAliasBefore", Rd(B, PMU_FALCON + 0x130));
    /* ACR leaves DMACTL=1 on this device. Snapshot only; host writes are
     * ignored in this post-ACR secure state, as observed in 0.4.24. */
    Step(63);
    dmaCtlBeforeStart = Rd(B, PMU_FALCON + FALCON_DMACTL);
    TraceDetail(L"B3cLsPmuDmaCtlBeforeStart", dmaCtlBeforeStart);
    dmaCtlAtStart = Rd(B, PMU_FALCON + FALCON_DMACTL);
    TraceDetail(L"B3cLsPmuDmaCtlAtStart", dmaCtlAtStart);
    Wr(B, PMU_FALCON + 0x130, 2u); /* CPUCTL_ALIAS starts the LS RTOS. */
    TraceDetail(L"B3cLsPmuCpuctlAliasAfter", Rd(B, PMU_FALCON + 0x130));
    Step(59);
    for (polls = 0; polls < 2000u; ++polls) {
        head = Rd(B, PMU_FALCON + 0x4C8);
        tail = Rd(B, PMU_FALCON + 0x4CC);
        if (head == 0xFFFFFFFFu || tail == 0xFFFFFFFFu) {
            result = 0xE0000071u;
            goto done;
        }
        if (head != tail) {
            if ((head & 3u) || (tail & 3u) || head > argsOff || tail >= argsOff ||
                head < tail || head - tail < 44u) {
                /* Claude 0.4.36: the PMU may still be setting up its queue
                 * pointers; a mixed head/tail pair seen once is not fatal.
                 * Record it and keep polling; fail only if it never settles. */
                if (initBadPairs == 0u) { initBadHead = head; initBadTail = tail; initBadPoll = polls; }
                ++initBadPairs;
                Ms(1);
                continue;
            }
            break; /* INIT is published; parsing/acknowledgment is a later stage. */
        }
        Ms(1);
    }
    TraceDetail(L"B3cLsPmuInitBadPairs", initBadPairs);
    if (initBadPairs) {
        TraceDetail(L"B3cLsPmuInitBadHead", initBadHead);
        TraceDetail(L"B3cLsPmuInitBadTail", initBadTail);
        TraceDetail(L"B3cLsPmuInitBadPoll", initBadPoll);
    }
    if (polls == 2000u) result = initBadPairs ? 0xE0000071u : 0xE0000070u;
    else {
        tail = Rd(B, PMU_FALCON + 0x4CC);
        if (tail > argsOff || argsOff - tail < 44u) result = 0xE0000071u;
        else {
            Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | tail);
            for (i = 0; i < RTL_NUMBER_OF(initWords); ++i)
                initWords[i] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
            initCursor = Rd(B, PMU_FALCON + FALCON_DMEMC0) & 0xFFFFu;
            initHdr = initWords[0];
            initType = initWords[1] & 0xFFu;
            TraceDetail(L"B3cLsPmuInitHdr", initHdr);
            TraceDetail(L"B3cLsPmuInitType", initType);
            TraceDetail(L"B3cLsPmuInitPioCursor", initCursor);
            TraceBin(L"B3cLsPmuInitWords", initWords, sizeof(initWords));
            if ((initHdr & 0xFFu) != 7u || ((initHdr >> 8) & 0xFFu) != 42u ||
                initType != 0u || initCursor != tail + 44u)
                result = 0xE0000072u;
        }
    }
done:
    Step(60);
    TraceDetail(L"B3cLsPmuIrqStat", Rd(B, PMU_FALCON + 0x008));
    Trace(L"B3cLsPmuInitPolls", polls);
    head = Rd(B, PMU_FALCON + 0x4C8);
    tail = Rd(B, PMU_FALCON + 0x4CC);
    TraceDetail(L"B3cLsPmuMsgHead", head);
    TraceDetail(L"B3cLsPmuMsgTail", tail);
    TraceDetail(L"B3cLsPmuCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    TraceDetail(L"B3cLsPmuSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
    TraceDetail(L"B3cLsPmuMailbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
    TraceDetail(L"B3cLsPmuMailbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
    /* Falcon register offsets follow the MIT nvgpu GK20A Falcon register map. */
    TraceDetail(L"B3cLsPmuIrqMode", Rd(B, PMU_FALCON + 0x00C));
    TraceDetail(L"B3cLsPmuIrqMask", Rd(B, PMU_FALCON + 0x018));
    TraceDetail(L"B3cLsPmuIrqDest", Rd(B, PMU_FALCON + 0x01C));
    TraceDetail(L"B3cLsPmuGptmrInt", Rd(B, PMU_FALCON + 0x028));
    TraceDetail(L"B3cLsPmuGptmrVal", Rd(B, PMU_FALCON + 0x02C));
    TraceDetail(L"B3cLsPmuPtimerLow", Rd(B, NV_PTIMER_TIME_0));
    TraceDetail(L"B3cLsPmuPtimerHigh", Rd(B, NV_PTIMER_TIME_1));
    diagOff = argsOff >= 16u ? argsOff - 16u : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / 4u : 0u;
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI)
            diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        TraceDetail(L"B3cLsPmuArgsWindowOff", diagOff);
        TraceDetail(L"B3cLsPmuArgsWindowWords", diagCount);
        TraceBin(L"B3cLsPmuArgsWindow", diagWords, diagCount * sizeof(ULONG));
    }
    diagOff = (tail != 0xFFFFFFFFu && tail >= 16u) ? (tail & ~3u) - 16u : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / 4u : 0u;
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI)
            diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        TraceDetail(L"B3cLsPmuQueueDataOff", diagOff);
        TraceDetail(L"B3cLsPmuQueueDataWords", diagCount);
        TraceBin(L"B3cLsPmuQueueDataWindow", diagWords, diagCount * sizeof(ULONG));
    }
    TraceDurable(L"B3cLsPmuStepResult", result);
    traceIdx = Rd(B, PMU_FALCON + 0x148);
    traceNr = (traceIdx >> 16) & 0xFFu;
    TraceDetail(L"B3cLsPmuTraceIdx", traceIdx);
    TraceDetail(L"B3cLsPmuTraceNr", traceNr);
    if (traceNr > RTL_NUMBER_OF(tracePc)) traceNr = RTL_NUMBER_OF(tracePc);
    for (traceSlot = 0; traceSlot < traceNr; ++traceSlot) {
        Wr(B, PMU_FALCON + 0x148, traceSlot);
        tracePc[traceSlot] = Rd(B, PMU_FALCON + 0x14C);
    }
    if (traceNr) TraceBin(L"B3cLsPmuTracePc", tracePc, traceNr * sizeof(ULONG));
    /* Step 61 is read-only with respect to Falcon execution. The IMEM PIO
     * cursor is selected using the nvgpu GK20A read path (IMEMC block/offset,
     * auto-increment; then IMEMD reads). It helps classify PC 0x3256 as the
     * ACR-loaded loader or a later image without assuming either. */
    Step(61);
    imemHwcfg = Rd(B, PMU_FALCON + FALCON_HWCFG);
    imemSize = (imemHwcfg & 0x1FFu) << 8;
    imemOk = (traceNr != 0 && tracePc[traceNr - 1] < imemSize) ? 1u : 0u;
    imemOff = imemOk ? (tracePc[traceNr - 1] & ~0x3Fu) : 0u;
    TraceDetail(L"B3cLsPmuImemHwcfg", imemHwcfg);
    TraceDetail(L"B3cLsPmuImemSize", imemSize);
    TraceDetail(L"B3cLsPmuImemTagBefore", Rd(B, PMU_FALCON + FALCON_IMEMT0));
    TraceDetail(L"B3cLsPmuImemReadOff", imemOff);
    TraceDurable(L"B3cLsPmuImemReadOk", imemOk);
    if (imemOk && imemOff + sizeof(imemWords) <= imemSize) {
        Wr(B, PMU_FALCON + FALCON_IMEMC0,
           ((imemOff >> 8) << 8) | (((imemOff >> 2) & 0x3Fu) << 2) | FALCON_IMEMC_AINCR);
        TraceDetail(L"B3cLsPmuImemTagSelected", Rd(B, PMU_FALCON + FALCON_IMEMT0));
        for (diagI = 0; diagI < RTL_NUMBER_OF(imemWords); ++diagI)
            imemWords[diagI] = Rd(B, PMU_FALCON + FALCON_IMEMD0);
        TraceBin(L"B3cLsPmuImemPcWindow", imemWords, sizeof(imemWords));
    }
    TraceDetail(L"B3cLsPmuImemTagAfter", Rd(B, PMU_FALCON + FALCON_IMEMT0));
    TraceDetail(L"B3cLsPmuDmaCmd", Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
    TraceDetail(L"B3cLsPmuDmaBase", Rd(B, PMU_FALCON + FALCON_DMATRFBASE));
    TraceDetail(L"B3cLsPmuDmaMoffs", Rd(B, PMU_FALCON + FALCON_DMATRFMOFFS));
    TraceDetail(L"B3cLsPmuDmaFboffs", Rd(B, PMU_FALCON + FALCON_DMATRFFBOFFS));
    TraceDetail(L"B3cLsPmuDmaCtl", Rd(B, PMU_FALCON + FALCON_DMACTL));
    TraceDetail(L"B3cLsPmuBindStatus", Rd(B, PMU_FALCON + FALCON_BIND_STATUS));
    TraceDetail(L"B3cLsPmuBindInst", Rd(B, PMU_FALCON + FALCON_BIND_INST));
    TraceDetail(L"B3cLsPmuCtxEnable", Rd(B, PMU_FALCON + FALCON_CTX_ENABLE));
    TraceDetail(L"B3cLsPmuCtxAction", Rd(B, PMU_FALCON + FALCON_CTX_ACTION));
    for (diagI = 0; diagI < RTL_NUMBER_OF(transcfg); ++diagI)
        transcfg[diagI] = Rd(B, PMU_FALCON + PMU_FBIF_TRANSCFG(diagI));
    TraceBin(L"B3cLsPmuTranscfg", transcfg, sizeof(transcfg));
    TraceDetail(L"B3cLsPmuFbifCtl", Rd(B, PMU_FALCON + PMU_FBIF_CTL));
    TraceDetail(L"B3cLsPmuPfifoIntr0", Rd(B, 0x2100));
    faultId = Rd(B, 0x259C);
    TraceDetail(L"B3cLsPmuFaultId", faultId);
    faultNr = 0;
    for (faultUnit = 0; faultUnit < 32; ++faultUnit) {
        if (!(faultId & (1u << faultUnit)) && faultUnit != 0x17u) continue;
        faultRecs[faultNr][0] = faultUnit;
        faultRecs[faultNr][1] = Rd(B, 0x2800 + faultUnit * 0x10);
        faultRecs[faultNr][2] = Rd(B, 0x2804 + faultUnit * 0x10);
        faultRecs[faultNr][3] = Rd(B, 0x2808 + faultUnit * 0x10);
        faultRecs[faultNr][4] = Rd(B, 0x280C + faultUnit * 0x10);
        ++faultNr;
    }
    TraceDetail(L"B3cLsPmuFaultRecCount", faultNr);
    if (faultNr) TraceErrorBin(L"B3cLsPmuFaultRecs", faultRecs, faultNr * sizeof(faultRecs[0]));
    TraceDetail(L"B3cLsPmuMcInt", Rd(Dev->Mc, MC_INTSTATUS));
    TraceDetail(L"B3cLsPmuMcErr", Rd(Dev->Mc, MC_ERR_STATUS));
    TraceDetail(L"B3cLsPmuMcErrAdr", Rd(Dev->Mc, MC_ERR_ADR));
    /* Expand both bounded DMEM snapshots to 256 bytes for loader/runtime data. */
    diagOff = argsOff >= 0x80u ? argsOff - 0x80u : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / sizeof(ULONG) : 0u; /* Claude: no underflow on a bogus tail */
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI) diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        TraceDetail(L"B3cLsPmuArgsWindowOff", diagOff);
        TraceDetail(L"B3cLsPmuArgsWindowWords", diagCount);
        TraceBin(L"B3cLsPmuArgsWindow", diagWords, diagCount * sizeof(ULONG));
    }
    diagOff = (tail != 0xFFFFFFFFu && tail >= 0x80u) ? ((tail & ~3u) - 0x80u) : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / sizeof(ULONG) : 0u; /* Claude: no underflow on a bogus tail */
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI) diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        TraceDetail(L"B3cLsPmuQueueDataOff", diagOff);
        TraceDetail(L"B3cLsPmuQueueDataWords", diagCount);
        TraceBin(L"B3cLsPmuQueueDataWindow", diagWords, diagCount * sizeof(ULONG));
    }
    TraceDurable(L"B3cLsPmuStartResult", result);
    return result;
}

/* Consume PMU INIT, submit one ACR bootstrap-falcon command for FECS, and
 * wait for the reply plus the nvgpu FECS context-switch ready mailbox. */
/* Match nvgpu's Falcon dump: EXCI is Falcon register index 28, read through
 * ICD_CMD/ICD_RDATA. The MMIO writes only select the trace/ICD read target. */
static VOID TraceB3cPmuCheckpoint(volatile UCHAR *B, PCWSTR Name)
{
    ULONG snap[7];
    snap[0] = Rd(B, PMU_FALCON + FALCON_CPUCTL);
    snap[1] = Rd(B, PMU_FALCON + 0x008u);
    snap[2] = Rd(B, PMU_FALCON + 0x040u);
    snap[3] = Rd(B, PMU_FALCON + 0x044u);
    snap[4] = Rd(B, PMU_FALCON + 0x148u);
    Wr(B, PMU_FALCON + 0x148u, 0u);
    snap[5] = Rd(B, PMU_FALCON + 0x14Cu);
    Wr(B, PMU_FALCON + 0x200u, 0x1C08u); /* ICD read EXCI (index 28) */
    snap[6] = Rd(B, PMU_FALCON + 0x20Cu);
    TraceBin(Name, snap, sizeof(snap));
}

/* nvgpu gr_gk20a_init_ctx_state() uses mailbox 0 as a nonzero-return
 * handshake for size discovery (ok=0, NOT_EQUAL; fail condition skipped).
 * gr_gk20a_submit_fecs_method_op() clears mailbox 0, writes method data,
 * then pushes the method address. Keep each query bounded and record all
 * inputs, mailbox state and poll count for C1 diagnosis. */
static ULONG B3cFecsQuerySize(volatile UCHAR *B, ULONG methodAddr,
    PCWSTR traceName, PULONG imageSize)
{
    ULONG query[11];
    ULONG polls;

    query[0] = methodAddr;
    query[1] = 0u;          /* method data */
    query[2] = 0u;          /* mailbox ID */
    query[3] = 0xFFFFFFFFu; /* mailbox clear value */
    query[4] = 0u;          /* nvgpu mailbox.ok for NOT_EQUAL */
    query[5] = 0u;          /* nvgpu mailbox.fail; fail condition is SKIP */
    query[6] = Rd(B, 0x409800u);

    Wr(B, 0x409840u, 0xFFFFFFFFu);
    query[7] = Rd(B, 0x409800u);
    Wr(B, 0x409500u, 0u);
    Wr(B, 0x409504u, methodAddr & 0xFFFu);
    KeMemoryBarrier();

    query[8] = 0u;
    for (polls = 0; polls < 2000u; ++polls) {
        query[8] = Rd(B, 0x409800u);
        if (query[8] == 0xFFFFFFFFu) {
            query[9] = polls;
            query[10] = 0xE0000082u;
            TraceBin(traceName, query, sizeof(query));
            return 0xE0000082u;
        }
        if (query[8] != 0u) break;
        Ms(1);
    }
    query[9] = polls;
    if (polls == 2000u) {
        query[10] = 0xE0000081u;
        TraceBin(traceName, query, sizeof(query));
        return 0xE0000081u;
    }
    if (query[8] == 2u) {
        query[10] = 0xE0000082u; /* hw_gr_gm20b.h FAIL marker */
        TraceBin(traceName, query, sizeof(query));
        return 0xE0000082u;
    }
    query[10] = 0u;
    TraceBin(traceName, query, sizeof(query));
    *imageSize = query[8];
    return 0u;
}

/* Stage C1 only queries FECS sizes after successful FECS ctxsw readiness.
 * It does not allocate, bind, or write a GR context. */
static ULONG B3cQueryFecsImageSizes(PSWGPU_DEVICE Dev, PULONG goldenOut,
                                    PULONG zcullOut, PULONG pmOut)
{
    volatile UCHAR *B = Dev->Bar0;
    ULONG pmcEnable, result = 0;
    ULONG goldenImageSize = 0, zcullImageSize = 0, pmImageSize = 0;

    Step(76);
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cC1PmcEnable", pmcEnable);
    TraceDetail(L"B3cC1MailboxPassValue", 1u); /* gr_fecs_ctxsw_mailbox_value_pass_v() */
    TraceDetail(L"B3cC1MailboxFailValue", 2u); /* gr_fecs_ctxsw_mailbox_value_fail_v() */
    if (!(pmcEnable & (1u << 12))) {
        result = 0xE0000083u;
        goto done;
    }

    /* hw_gr_gm20b.h: gr_fecs_method_push_adr_discover_image_size_v(). */
    result = B3cFecsQuerySize(B, 0x10u, L"B3cC1GoldenQueryWords", &goldenImageSize);
    TraceDetail(L"B3cC1GoldenImageSize", goldenImageSize);
    if (result != 0u) goto done;

    /* hw_gr_gm20b.h: gr_fecs_method_push_adr_discover_zcull_image_size_v(). */
    result = B3cFecsQuerySize(B, 0x16u, L"B3cC1ZcullQueryWords", &zcullImageSize);
    TraceDetail(L"B3cC1ZcullImageSize", zcullImageSize);
    if (result != 0u) goto done;

    /* hw_gr_gm20b.h: gr_fecs_method_push_adr_discover_pm_image_size_v(). */
    result = B3cFecsQuerySize(B, 0x25u, L"B3cC1PmQueryWords", &pmImageSize);
    TraceDetail(L"B3cC1PmImageSize", pmImageSize);

done:
    if (result == 0u) {
        *goldenOut = goldenImageSize;
        *zcullOut = zcullImageSize;
        *pmOut = pmImageSize;
    }
    TraceDurable(L"B3cC1Result", result);
    return result;
}

/* C2a: validate the raw GM20B init-list records without touching GR registers.
 * nvgpu gr_gk20a_init_ctx_vars() stores sw_non_ctx_load and sw_bundle_init as
 * addr/value records and sw_ctx_load as addr/index/value records. The files
 * are bounded individually, parsed one at a time, and always freed here.
 * C2b applies only nonctx during gk20a_init_gr_reset_enable_hw(); sw_ctx is
 * deferred to setup_hw, while bundle init belongs to
 * gr_gk20a_init_golden_ctx_image() after FECS context binding. */
static ULONG B3cValidateOneGrInitList(PCWSTR valueName, PCWSTR traceName,
                                      ULONG recordBytes, BOOLEAN registerList)
{
    PUCHAR data;
    ULONG length, count, result = 0, trace[8] = { 0 }, entryIndex;
    ULONG firstOffset, lastOffset;

    data = ReadBinParam(valueName, 0x20000u, &length);
    if (data == NULL) {
        trace[0] = 0;
        trace[1] = recordBytes;
        trace[7] = 0xE0000084u;
        TraceBin(traceName, trace, sizeof(trace));
        return 0xE0000084u;
    }
    if (recordBytes == 0 || length == 0 || (length % recordBytes) != 0) {
        trace[0] = length;
        trace[1] = recordBytes;
        trace[7] = 0xE0000085u;
        TraceBin(traceName, trace, sizeof(trace));
        ExFreePoolWithTag(data, SWGPU_TAG);
        return 0xE0000085u;
    }

    count = length / recordBytes;
    for (entryIndex = 0; entryIndex < count; ++entryIndex) {
        ULONG entryOffset = entryIndex * recordBytes;
        ULONG registerAddress = Le32(data + entryOffset);
        /* Claude: sw_bundle_init holds bundle/method addresses (0x38..0x1E100
         * in linux-firmware gm20b, 704 of 952 not dword aligned), not BAR0
         * register offsets, so only register lists get the alignment check. */
        if ((registerList && (registerAddress & 3u) != 0) || registerAddress > 0x00FFFFFCu) {
            trace[0] = length;
            trace[1] = recordBytes;
            trace[2] = count;
            trace[3] = entryIndex;
            trace[4] = registerAddress;
            trace[7] = 0xE0000085u;
            TraceBin(traceName, trace, sizeof(trace));
            ExFreePoolWithTag(data, SWGPU_TAG);
            return 0xE0000085u;
        }
    }
    firstOffset = 0;
    lastOffset = length - recordBytes;
    trace[0] = length;
    trace[1] = recordBytes;
    trace[2] = count;
    trace[3] = Le32(data + firstOffset); /* first register address */
    if (recordBytes == 12u) {
        trace[4] = Le32(data + firstOffset + 4u); /* AIV index */
        trace[5] = Le32(data + firstOffset + 8u); /* AIV value */
    } else {
        trace[5] = Le32(data + firstOffset + 4u); /* AV value */
    }
    trace[6] = Le32(data + lastOffset); /* last register address */
    TraceBin(traceName, trace, sizeof(trace));
    ExFreePoolWithTag(data, SWGPU_TAG);
    return result;
}

static ULONG B3cValidateGrInitLists(PSWGPU_DEVICE Dev)
{
    ULONG result;
    ULONG pmcEnable;

    Step(77);
    pmcEnable = Rd(Dev->Bar0, NV_PMC_ENABLE);
    TraceDetail(L"B3cC2aPmcEnable", pmcEnable);
    if (!(pmcEnable & (1u << 12))) {
        TraceDurable(L"B3cC2aResult", 0xE0000086u);
        return 0xE0000086u;
    }

    /* sw_nonctx and sw_bundle_init use 8-byte AV records; sw_ctx uses
     * 12-byte AIV records. These are read-only format checks at this stage. */
    result = B3cValidateOneGrInitList(L"FwGrSwNonctx", L"B3cC2aSwNonctx", 8u, TRUE);
    if (result == 0)
        result = B3cValidateOneGrInitList(L"FwGrSwCtx", L"B3cC2aSwCtx", 12u, TRUE);
    if (result == 0)
        result = B3cValidateOneGrInitList(L"FwGrSwBundleInit", L"B3cC2aSwBundleInit", 8u, FALSE);
    TraceDurable(L"B3cC2aResult", result);
    return result;
}

/* C2c: nvgpu gk20a_init_gr_setup_hw() applies sw_ctx AIV entries as direct
 * address/value writes (the entry index is not used by that loop). Validate
 * every address before the first write and keep the same bounded GM20B PRI
 * windows used by C2b. */
static BOOLEAN B3cIsGrPriAddress(ULONG address);

/* Switchroot nvgpu common/priv_ring/priv_ring_gm20b.c:
 * gm20b_priv_ring_isr() snapshots ringmaster and station error registers,
 * issues ACK_INTERRUPT, and waits for the command field to return to NO_CMD.
 * The headers name the registers and command encodings, but do not describe
 * priv_error_info/code subfields; retain those fields raw. */
static VOID B3cPrivRingResetWriteLog(VOID)
{
    RtlZeroMemory(g_B3cPrivRingWriteLog, sizeof(g_B3cPrivRingWriteLog));
    g_B3cPrivRingWriteLogCount = 0u;
    g_B3cPrivRingWriteLogDropped = 0u;
    g_B3cPrivRingAckTimeouts = 0u;
    g_B3cPrivRingLastAckPolls = 0u;
    g_B3cPrivRingLastCommand = 0u;
}

static BOOLEAN B3cPrivRingAfterGrWrite(volatile UCHAR *B,
                                        ULONG writeAddress,
                                        ULONG writeValue)
{
    ULONG intr0, intr1;
    ULONG sysAdr, sysWrdat, sysInfo, sysCode;
    ULONG gpcAdr, gpcWrdat, gpcInfo, gpcCode;
    ULONG ackNeeded, ackPolls = 0xFFFFFFFFu, commandAfter = 0u;
    ULONG *entry;

    intr0 = Rd(B, 0x00120058u);
    intr1 = Rd(B, 0x0012005Cu);
    sysAdr = Rd(B, 0x00122120u);
    sysWrdat = Rd(B, 0x00122124u);
    sysInfo = Rd(B, 0x00122128u);
    sysCode = Rd(B, 0x0012212Cu);
    gpcAdr = Rd(B, 0x00128120u);
    gpcWrdat = Rd(B, 0x00128124u);
    gpcInfo = Rd(B, 0x00128128u);
    gpcCode = Rd(B, 0x0012812Cu);
    /* The interrupt status is the event qualifier. Station error registers
     * may retain the previous payload after ACK, so do not attribute a stale
     * payload to every subsequent write when both status words are clear. */
    ackNeeded = (intr0 | intr1) != 0u;

    if (ackNeeded) {
        Wr(B, 0x0012004Cu, 0x2u); /* ACK_INTERRUPT (GM20B header value). */
        KeMemoryBarrier();
        for (ackPolls = 0; ackPolls < 2000u; ++ackPolls) {
            commandAfter = Rd(B, 0x0012004Cu) & 0x3Fu;
            if (commandAfter == 0u) break; /* NO_CMD */
            Ms(1);
        }
        g_B3cPrivRingLastAckPolls = ackPolls;
        g_B3cPrivRingLastCommand = commandAfter;
        if (ackPolls == 2000u) ++g_B3cPrivRingAckTimeouts;
    }

    if (ackNeeded) {
        if (g_B3cPrivRingWriteLogCount < B3C_PRIV_RING_LOG_CAPACITY) {
            entry = g_B3cPrivRingWriteLog[g_B3cPrivRingWriteLogCount++];
            entry[0] = writeAddress;
            entry[1] = writeValue;
            entry[2] = intr0;
            entry[3] = intr1;
            entry[4] = sysAdr;
            entry[5] = sysWrdat;
            entry[6] = sysInfo;
            entry[7] = sysCode;
            entry[8] = gpcAdr;
            entry[9] = gpcWrdat;
            entry[10] = gpcInfo;
            entry[11] = gpcCode;
            entry[12] = ackPolls;
            entry[13] = commandAfter;
            entry[14] = 1u;
            entry[15] = (ackPolls == 2000u) ? 1u : 0u;
        } else {
            ++g_B3cPrivRingWriteLogDropped;
        }
    }
    return ackPolls != 2000u;
}

static VOID B3cPrivRingTraceWriteLog(PCWSTR countName,
                                     PCWSTR droppedName,
                                     PCWSTR timeoutName,
                                     PCWSTR recordsName)
{
    Trace(countName, g_B3cPrivRingWriteLogCount);
    Trace(droppedName, g_B3cPrivRingWriteLogDropped);
    Trace(timeoutName, g_B3cPrivRingAckTimeouts);
    Trace(L"B3cPrivRingLastAckPolls", g_B3cPrivRingLastAckPolls);
    TraceDetail(L"B3cPrivRingLastCommand", g_B3cPrivRingLastCommand);
    if (g_B3cPrivRingWriteLogCount != 0u)
        TraceErrorBin(recordsName, g_B3cPrivRingWriteLog,
            g_B3cPrivRingWriteLogCount * B3C_PRIV_RING_LOG_WORDS * sizeof(ULONG));
}

static BOOLEAN B3cPrivRingDrainBeforeWrites(volatile UCHAR *B)
{
    /* Sentinel origin distinguishes a pre-existing latched error from a
     * rejection caused by the first list register write. */
    return B3cPrivRingAfterGrWrite(B, 0xFFFFFFFFu, 0xFFFFFFFFu);
}

static ULONG B3cApplyGrCtxList(PSWGPU_DEVICE Dev, BOOLEAN postBind)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR data = NULL;
    ULONG length = 0, count = 0, result = 0, entryIndex;
    ULONG grAddr, grIndex, grValue, badAddr = 0, badIndex = 0xFFFFFFFFu;
    ULONG pmcEnable, writeCount = 0, sampleCount = 0, sample[8][4] = { 0 };
    ULONG waitPoll = 0, engineStatus = 0, grStatus = 0;
    ULONG c2cEngineStatusOff = 0x0040060Cu;
    ULONG c2cGrStatusOff = 0x00400700u;

    Step(postBind ? 86u : 79u);
    if (!postBind) B3cPrivRingResetWriteLog();
    if (!B3cPrivRingDrainBeforeWrites(B)) {
        result = 0xE0000095u; goto done;
    }
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    Trace(postBind ? L"B3cC2ePostBindSwCtxPmcEnable" : L"B3cC2cPmcEnable", pmcEnable);
    if (!(pmcEnable & (1u << 12))) { result = 0xE0000088u; goto done; }

    data = ReadBinParam(L"FwGrSwCtx", 0x20000u, &length);
    Trace(postBind ? L"B3cC2ePostBindSwCtxBytes" : L"B3cC2cSwCtxBytes", length);
    if (data == NULL || length == 0u || (length % 12u) != 0u) {
        result = 0xE0000088u; goto done;
    }
    count = length / 12u;
    Trace(postBind ? L"B3cC2ePostBindSwCtxCount" : L"B3cC2cSwCtxCount", count);
    /* Validate the entire list before touching any GR register. */
    for (entryIndex = 0; entryIndex < count; ++entryIndex) {
        grAddr = Le32(data + entryIndex * 12u);
        grIndex = Le32(data + entryIndex * 12u + 4u);
        if ((grAddr & 3u) != 0u || !B3cIsGrPriAddress(grAddr)) {
            badAddr = grAddr; badIndex = entryIndex;
            Trace(postBind ? L"B3cC2ePostBindSwCtxBadEntryIndexValue" : L"B3cC2cBadEntryIndexValue", grIndex);
            result = 0xE0000088u; goto done;
        }
    }

    for (entryIndex = 0; entryIndex < count; ++entryIndex) {
        grAddr = Le32(data + entryIndex * 12u);
        grIndex = Le32(data + entryIndex * 12u + 4u);
        grValue = Le32(data + entryIndex * 12u + 8u);
        /* gk20a_init_gr_setup_hw() writes l[i].addr = l[i].value; it does
         * not use AIV index. Record the index for comparison/diagnostics. */
        Wr(B, grAddr, grValue);
        if (!B3cPrivRingAfterGrWrite(B, grAddr, grValue)) {
            badAddr = grAddr; badIndex = entryIndex;
            result = 0xE0000095u; goto done;
        }
        ++writeCount;
        if (entryIndex < RTL_NUMBER_OF(sample)) {
            sample[entryIndex][0] = grAddr;
            sample[entryIndex][1] = grIndex;
            sample[entryIndex][2] = grValue;
            sample[entryIndex][3] = 0;
            sampleCount = entryIndex + 1u;
        }
    }
    KeMemoryBarrier();
    for (entryIndex = 0; entryIndex < sampleCount; ++entryIndex)
        sample[entryIndex][3] = Rd(B, sample[entryIndex][0]);
    Trace(postBind ? L"B3cC2ePostBindSwCtxWrites" : L"B3cC2cSwCtxWrites", writeCount);
    Trace(postBind ? L"B3cC2ePostBindSwCtxReadbackCount" : L"B3cC2cReadbackCount", sampleCount);
    if (sampleCount) TraceBin(postBind ? L"B3cC2ePostBindSwCtxReadback" : L"B3cC2cReadback", sample, sampleCount * sizeof(sample[0]));

    /* Observe the setup_hw idle boundary with a finite ceiling. */
    for (waitPoll = 0; waitPoll < 2000u; ++waitPoll) {
        engineStatus = Rd(B, c2cEngineStatusOff);
        grStatus = Rd(B, c2cGrStatusOff);
        if (!(engineStatus & 1u) && !(grStatus & 0x6u)) break;
        Ms(1);
    }
    Trace(postBind ? L"B3cC2ePostBindSwCtxIdlePolls" : L"B3cC2cIdlePolls", waitPoll);
    Trace(postBind ? L"B3cC2ePostBindSwCtxEngineStatus" : L"B3cC2cEngineStatus", engineStatus);
    Trace(postBind ? L"B3cC2ePostBindSwCtxGrStatus" : L"B3cC2cGrStatus", grStatus);
    if (waitPoll == 2000u) result = 0xE0000088u;

done:
    Trace(postBind ? L"B3cC2ePostBindSwCtxBadIndex" : L"B3cC2cBadIndex", badIndex);
    Trace(postBind ? L"B3cC2ePostBindSwCtxBadAddress" : L"B3cC2cBadAddress", badAddr);
    B3cPrivRingTraceWriteLog(
        postBind ? L"B3cC2ePrivWriteErrorCount" : L"B3cC2cPrivWriteErrorCount",
        postBind ? L"B3cC2ePrivWriteErrorDropped" : L"B3cC2cPrivWriteErrorDropped",
        postBind ? L"B3cC2ePrivAckTimeouts" : L"B3cC2cPrivAckTimeouts",
        postBind ? L"B3cC2ePrivWriteErrors" : L"B3cC2cPrivWriteErrors");
    if (data != NULL) ExFreePoolWithTag(data, SWGPU_TAG);
    TraceDurable(postBind ? L"B3cC2ePostBindSwCtxResult" : L"B3cC2cResult", result);
    return result;
}

/* The nvgpu golden path binds the channel FECS context before replaying
 * sw_ctx_load; this helper is called a second time there for that ordering. */
static ULONG B3cC2eWaitNouveauGrIdle(volatile UCHAR *B, ULONG waitStage,
                                     ULONG bundleIndex, PULONG state)
{
    ULONG polls, grStatus = 0, engineStatus = 0, fifoStatus = 0;
    ULONG mcEnable = 0, ctxswActive = 0, grBusy = 0;
    for (polls = 0; polls < 2000u; ++polls) {
        grStatus = Rd(B, 0x00400700u);
        fifoStatus = Rd(B, 0x00002640u);
        ctxswActive = (fifoStatus & 0x00008000u) != 0u;
        engineStatus = Rd(B, 0x0040060Cu);
        grBusy = engineStatus & 1u;
        mcEnable = Rd(B, NV_PMC_ENABLE);
        /* nouveau gf100_gr_wait_idle(): GR disabled in MC, or both GR idle
         * and no ctxsw. FIFO ctx_status INVALID is deliberately not a bypass. */
        if (!(mcEnable & 0x1000u) || (!grBusy && !ctxswActive)) break;
        Ms(1);
    }
    state[0] = waitStage; state[1] = bundleIndex; state[2] = polls;
    state[3] = grStatus; state[4] = engineStatus; state[5] = fifoStatus;
    state[6] = mcEnable;
    return (polls < 2000u) ? 1u : 0u;
}

/* Read-only C2b/C2e topology snapshot. Addresses come from the unmodified MIT
 * nvgpu GM20B headers hw_gr_gm20b.h, hw_fuse_gm20b.h, hw_top_gm20b.h, and
 * hw_pri_ringmaster/ringstation headers. The 0x5044b0 read is retained because
 * the hardware log returned BADF5040 there; it is not used as a write target. */
static VOID B3cCaptureGpcPriDiag(volatile UCHAR *B, PCWSTR traceName,
                                 PCWSTR badCountName, PULONG badfCountOut)
{
    static const ULONG offsets[] = {
        0x00022430u, 0x00022434u, 0x00022438u, 0x0002243Cu, 0x00022440u,
        0x00022444u, 0x00022448u, 0x0002244Cu, 0x00022450u, 0x00022454u,
        0x00022458u, 0x0002245Cu, 0x00021C1Cu, 0x00021C38u, 0x00021C3Cu,
        0x00021838u, 0x0002183Cu, 0x00409604u,
        0x0012004Cu, 0x00120050u, 0x00120058u, 0x0012005Cu, 0x00120060u,
        0x00120078u, 0x00128120u, 0x00128124u, 0x00128128u, 0x0012812Cu,
        0x00122120u, 0x00122124u, 0x00122128u, 0x0012212Cu,
        0x00502C80u, 0x00502C90u, 0x00504500u, 0x00504508u, 0x00504238u,
        0x00504088u, 0x00504698u, 0x0050469Cu, 0x005046A4u, 0x00504D00u,
        0x00419D00u, 0x0041AC80u, 0x00419EA4u, 0x005044B0u
    };
    ULONG pairs[RTL_NUMBER_OF(offsets)][2];
    ULONG i, badfCount = 0;

    for (i = 0; i < RTL_NUMBER_OF(offsets); ++i) {
        ULONG value = Rd(B, offsets[i]);
        pairs[i][0] = offsets[i];
        pairs[i][1] = value;
        if ((value & 0xFFFF0000u) == 0xBADF0000u) ++badfCount;
    }
    Trace(badCountName, badfCount);
    TraceBin(traceName, pairs, sizeof(pairs));
    if (badfCountOut != NULL) *badfCountOut = badfCount;
}

/* GM20B gm20b_gr_init_gpc_mmu(), called by gk20a_gr_init() after the validated
 * sw_nonctx writes, memory scrub and GR idle boundary. Secure ACR is active,
 * so Nouveau's non-secure-only 0x100ce4 bypass is intentionally omitted. */
static ULONG B3cInitGpcMmu(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    ULONG values[7][3] = { 0 };
    ULONG i, mismatchCount = 0, result = 0;

    Step(89);
    values[0][0] = 0x00418880u;
    values[0][1] = Rd(B, 0x00100C80u) & 0xF000187Fu;
    values[1][0] = 0x00418890u; values[1][1] = 0u;
    values[2][0] = 0x00418894u; values[2][1] = 0u;
    values[3][0] = 0x004188B0u; values[3][1] = Rd(B, 0x00100CC4u);
    values[4][0] = 0x004188B4u; values[4][1] = Rd(B, 0x00100CC8u);
    values[5][0] = 0x004188B8u; values[5][1] = Rd(B, 0x00100CCCu);
    values[6][0] = 0x004188ACu; values[6][1] = Rd(B, 0x00100800u);

    TraceDetail(L"B3cC2bGpcMmuSourceMmuCtrl", Rd(B, 0x00100C80u));
    TraceDetail(L"B3cC2bGpcMmuSourceDebugCtl", Rd(B, 0x00100CC4u));
    TraceDetail(L"B3cC2bGpcMmuSourceDebugWr", Rd(B, 0x00100CC8u));
    TraceDetail(L"B3cC2bGpcMmuSourceDebugRd", Rd(B, 0x00100CCCu));
    TraceDetail(L"B3cC2bGpcMmuSourceFbConfig", Rd(B, 0x00100800u));
    B3cPrivRingResetWriteLog();
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }

    for (i = 0; i < RTL_NUMBER_OF(values); ++i) {
        if (!B3cIsGrPriAddress(values[i][0])) {
            result = 0xE0000093u;
            goto done;
        }
    }
    for (i = 0; i < RTL_NUMBER_OF(values); ++i) {
        Wr(B, values[i][0], values[i][1]);
        if (!B3cPrivRingAfterGrWrite(B, values[i][0], values[i][1])) {
            result = 0xE0000095u;
            goto done;
        }
        values[i][2] = Rd(B, values[i][0]);
        if (values[i][2] != values[i][1]) ++mismatchCount;
    }
    KeMemoryBarrier();
    TraceDetail(L"B3cC2bGpcMmuWriteCount", RTL_NUMBER_OF(values));
    TraceDetail(L"B3cC2bGpcMmuMismatchCount", mismatchCount);
    TraceBin(L"B3cC2bGpcMmuReadback", values, sizeof(values));
    if (mismatchCount != 0u) result = 0xE0000093u;

done:
    B3cPrivRingTraceWriteLog(L"B3cC2bGpcMmuPrivWriteErrorCount",
        L"B3cC2bGpcMmuPrivWriteErrorDropped", L"B3cC2bGpcMmuPrivAckTimeouts",
        L"B3cC2bGpcMmuPrivWriteErrors");
    TraceDurable(L"B3cC2bGpcMmuResult", result);
    return result;
}

/* C2d allocates a bounded, independent GR address space and the backing that
 * nvgpu alloc_global_ctx_buffers()/alloc_gr_ctx() prepare before the golden
 * image. It writes only the new outer/GR page tables and RAMIN instance block;
 * FECS bind_pointer and golden-save methods remain deferred to C2e. */
static ULONG B3cPrepareGrContext(PSWGPU_DEVICE Dev, ULONG goldenBytes,
                                 ULONG zcullBytes, ULONG pmBytes)
{
    PHYSICAL_ADDRESS acrArenaPa, smmuPtPa;
    volatile UCHAR *B = Dev->Bar0;
    volatile ULONG *smmuPd = NULL, *smmuPt = NULL;
    ULONGLONG *gpuPgd, *gpuPgt, *bar1GpuPgt, *inst;
    ULONG pmcEnable, result = 0, i, pageCount = GR_VM_PAGE_COUNT;
    ULONG failPoint = 0u, allocRetry = 0u;
    ULONG grWindowBase, grPageFirst, grPteExpected, grPteRead;
    ULONG outerPdeExpected, outerPdeRead, gpuPteIndex, gpuPdeIndex, outerBadIndex = 0xFFFFFFFFu;
    ULONG gpuBadIndex = 0xFFFFFFFFu;
    ULONG goldenAligned, zcullAligned, pmAligned, privBytes = 0x80000u;
    ULONG betaDefault = 0x400u, betaGranularity = 0x20u;
    ULONG alphaDefault = 0x800u, alphaGranularity = 0x20u;
    ULONG maxTpcCount = 2u, attribBytes, bundleBytes = 0x18u * 0x100u;
    ULONG pagepoolBytes = 0x80u * 0x100u;
    ULONG sample[8][3] = { 0 }, sampleCount = 0;
    ULONG backingSample[16][3] = { 0 };
    ULONG instPdbLo, instPdbHi, instLimitLo, instLimitHi;
    ULONG bar1PdeIndex, bar1PteIndex;
    ULONGLONG gpuPdb, gpuPdeExpected, wfiGpuVa, bar1PdeExpected, bar1PteExpected;
    ULONG *instWords;

    Step(80);
    RtlZeroMemory(&g_GrVm, sizeof(g_GrVm));
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cC2dPmcEnable", pmcEnable);
    if (!(pmcEnable & (1u << 12))) { failPoint = 1u; result = 0xE0000089u; goto done; }
    if (!g_AcrVm.Active || !g_AcrVm.PtbChanged || goldenBytes == 0 || zcullBytes == 0 || pmBytes == 0 ||
        goldenBytes > 0x20000u || zcullBytes > 0x20000u || pmBytes > 0x10000u) {
        failPoint = 2u; result = 0xE0000089u; goto done;
    }

    /* GM20B gr_gm20b_calc_global_ctx_buffer_size(): beta is 0x400 + half,
     * alpha is 0x800 + half; each size is multiplied by its 0x20 granularity
     * and the 2-TPC topology. Header defaults/granularities are from the
     * user's unmodified MIT hw_gr_gm20b.h. */
    attribBytes = (betaDefault + (betaDefault >> 1)) * betaGranularity * maxTpcCount +
                  (alphaDefault + (alphaDefault >> 1)) * alphaGranularity * maxTpcCount;
    goldenAligned = (goldenBytes + 0xFFFu) & ~0xFFFu;
    zcullAligned = (zcullBytes + 0xFFFu) & ~0xFFFu;
    pmAligned = (pmBytes + 0xFFFu) & ~0xFFFu;
    TraceDetail(L"B3cC2dGoldenBytes", goldenBytes);
    TraceDetail(L"B3cC2dZcullBytes", zcullBytes);
    TraceDetail(L"B3cC2dPmBytes", pmBytes);
    TraceDetail(L"B3cC2dAttribBytes", attribBytes);
    TraceDetail(L"B3cC2dBundleBytes", bundleBytes);
    TraceDetail(L"B3cC2dPagepoolBytes", pagepoolBytes);
    TraceDetail(L"B3cC2dPrivMapBytes", privBytes);
    TraceDetail(L"B3cC2dTopologyTpcs", maxTpcCount);
    TraceDetail(L"B3cC2dChannelCtxBytes", goldenAligned);
    TraceDetail(L"B3cC2dZcullAligned", zcullAligned);
    TraceDetail(L"B3cC2dPmAligned", pmAligned);
    if (attribBytes != 0x48000u || bundleBytes != 0x1800u || pagepoolBytes != 0x8000u ||
        GR_VM_PM_OFF + pmAligned > GR_VM_ARENA_SIZE ||
        GR_VM_PM_OFF + pmAligned > GR_VM_PATCH_OFF ||
        GR_VM_PATCH_OFF + GR_VM_PATCH_BYTES > GR_VM_PRIV_OFF ||
        GR_VM_GPU_PGT_BAR1_OFF + PAGE_SIZE > GR_VM_INST_OFF ||
        ((BAR1_D3_LOW_VA >> 12) & 0x7FFFu) >= PAGE_SIZE / sizeof(ULONGLONG)) {
        failPoint = 3u; result = 0xE0000089u; goto done;
    }

    g_GrVm.Arena = E4Checkout(Dev, SwGpuPoolGr, GR_VM_ARENA_SIZE);
    if (g_GrVm.Arena == NULL) {
        allocRetry = 1u;
        g_GrVm.Arena = E4Checkout(Dev, SwGpuPoolGr, GR_VM_ARENA_SIZE);
        TraceDetail(L"B3cC2dAllocRetry", allocRetry);
        if (g_GrVm.Arena == NULL) { failPoint = 4u; result = 0xE0000089u; goto done; }
    }
    g_GrVm.GoldenBytes = goldenBytes;
    g_GrVm.ZcullBytes = zcullBytes;
    g_GrVm.PmBytes = pmBytes;
    g_GrVm.Pa = MmGetPhysicalAddress(g_GrVm.Arena);
    if (g_GrVm.Pa.HighPart != 0 || (g_GrVm.Pa.LowPart & 0xFFFu) != 0 ||
        g_GrVm.Pa.LowPart > 0xFFFFFFFFu - GR_VM_ARENA_SIZE) {
        failPoint = 5u; result = 0xE0000089u; goto done;
    }
    {
        PHYSICAL_ADDRESS acrPa = MmGetPhysicalAddress(g_AcrVm.Arena);
        ULONG acrEnd = acrPa.LowPart + ACR_VM_ARENA_SIZE;
        ULONG grEnd = g_GrVm.Pa.LowPart + GR_VM_ARENA_SIZE;
        if (g_GrVm.Pa.LowPart < acrEnd && acrPa.LowPart < grEnd) {
            failPoint = 6u; result = 0xE0000089u; goto done;
        }
        TraceDetail(L"B3cC2dArenaDistinct", 1u);
    }
    g_GrVm.SmmuWin = g_GrVm.Pa.LowPart & ~(ACR_SMMU_WINDOW - 1u);
    g_GrVm.SmmuPteFirst = (g_GrVm.Pa.LowPart - g_GrVm.SmmuWin) >> 12;
    pageCount = GR_VM_ARENA_SIZE >> 12;
    if (g_GrVm.SmmuPteFirst + pageCount > (ACR_SMMU_WINDOW >> 12)) {
        failPoint = 7u; result = 0xE0000089u; goto done;
    }
    {
        smmuPd = (volatile ULONG *)((PUCHAR)g_AcrVm.Arena);
        g_GrVm.SmmuPdeIndex = g_GrVm.SmmuWin >> 22;
        g_GrVm.OldSmmuPde = smmuPd[g_GrVm.SmmuPdeIndex];
        acrArenaPa = MmGetPhysicalAddress(g_AcrVm.Arena);
        grWindowBase = acrArenaPa.LowPart & ~(ACR_SMMU_WINDOW - 1u);
        if (g_GrVm.SmmuWin == grWindowBase) {
            smmuPt = (volatile ULONG *)((PUCHAR)g_AcrVm.Arena + PAGE_SIZE);
            g_GrVm.SharedSmmuPt = TRUE;
        } else {
            smmuPt = (volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_SMMU_PGT_OFF);
        }
        for (i = 0; i < pageCount; ++i) {
            g_GrVm.OldSmmuPtes[i] = smmuPt[g_GrVm.SmmuPteFirst + i];
            smmuPt[g_GrVm.SmmuPteFirst + i] = ((g_GrVm.Pa.LowPart >> 12) + i) | SMMU_ATTR_RWNS;
        }
        if (!g_GrVm.SharedSmmuPt) {
            ULONGLONG grSmmuPtPa = (ULONGLONG)g_GrVm.Pa.LowPart + GR_VM_SMMU_PGT_OFF;
            smmuPd[g_GrVm.SmmuPdeIndex] = (ULONG)(grSmmuPtPa >> 12) |
                                            SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
            g_GrVm.PtbChanged = TRUE;
        }
        KeMemoryBarrier();
        Wr(Dev->Mc, MC_SMMU_PTC_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_TLB_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        g_GrVm.PtbChanged = TRUE;
        smmuPtPa = MmGetPhysicalAddress((PVOID)smmuPt);
        outerPdeExpected = (smmuPtPa.LowPart >> 12) | SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
        outerPdeRead = smmuPd[g_GrVm.SmmuPdeIndex];
        TraceDetail(L"B3cC2dOuterPdeIndex", g_GrVm.SmmuPdeIndex);
        TraceDetail(L"B3cC2dOuterPdeBefore", g_GrVm.OldSmmuPde);
        TraceDetail(L"B3cC2dOuterPdeExpected", outerPdeExpected);
        TraceDetail(L"B3cC2dOuterPdeRead", outerPdeRead);
        if (outerPdeRead != outerPdeExpected) { failPoint = 8u; result = 0xE000008Au; goto done; }
        for (i = 0; i < pageCount; ++i) {
            ULONG outerExpected = ((g_GrVm.Pa.LowPart >> 12) + i) | SMMU_ATTR_RWNS;
            if (smmuPt[g_GrVm.SmmuPteFirst + i] != outerExpected) {
                outerBadIndex = g_GrVm.SmmuPteFirst + i;
                break;
            }
        }
        TraceDetail(L"B3cC2dOuterPteVerified", i);
        TraceDetail(L"B3cC2dOuterPteBadIndex", outerBadIndex);
        if (i != pageCount) { failPoint = 9u; result = 0xE000008Au; goto done; }
        {
            ULONG oldPteSample[3][3];
            oldPteSample[0][0] = g_GrVm.SmmuPteFirst;
            oldPteSample[1][0] = g_GrVm.SmmuPteFirst + (pageCount >> 1);
            oldPteSample[2][0] = g_GrVm.SmmuPteFirst + pageCount - 1u;
            for (i = 0; i < 3; ++i) {
                oldPteSample[i][1] = g_GrVm.OldSmmuPtes[i == 0 ? 0 : (i == 1 ? pageCount >> 1 : pageCount - 1u)];
                oldPteSample[i][2] = smmuPt[oldPteSample[i][0]];
            }
            TraceBin(L"B3cC2dOuterPteBeforeAfter", oldPteSample, sizeof(oldPteSample));
        }
    }
    g_GrVm.Iova = (ULONGLONG)g_GrVm.Pa.LowPart | ACR_GM20B_IOMMU_TAG;

    /* Fill the isolated GPU 4 KiB page table, PDB and RAMIN context pointer.
     * Layout follows nvgpu global buffers and W4T's MIT cross-check layout. */
    gpuPgd = (ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGD_OFF);
    gpuPgt = (ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGT_OFF);
    inst = (ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF);
    instWords = (ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF);
    for (i = 0; i < (0x10000u / sizeof(ULONGLONG)); ++i) gpuPgd[i] = 0;
    for (i = 0; i < (0x40000u / sizeof(ULONGLONG)); ++i) gpuPgt[i] = 0;
    gpuPdeIndex = GR_VM_GPU_VA >> 27;
    gpuPdeExpected = (((ULONGLONG)g_GrVm.Iova + GR_VM_GPU_PGT_OFF) << 24) | (5ull << 32);
    gpuPgd[gpuPdeIndex] = gpuPdeExpected;
    gpuPteIndex = (GR_VM_GPU_VA >> 12) & 0x7FFFu;
    for (i = 0; i < pageCount; ++i)
        gpuPgt[gpuPteIndex + i] = ((g_GrVm.Iova + ((ULONGLONG)i << 12)) >> 8) | 1ull;
    /* Map USERD at a BAR1-visible low VA using a distinct PDE. The compact
     * second-level table occupies a page in an otherwise unused part of the
     * current 256 KiB PGT backing, avoiding the main mapping's PTE index 0x100. */
    bar1PdeIndex = BAR1_D3_LOW_VA >> 27;
    bar1PteIndex = (BAR1_D3_LOW_VA >> 12) & 0x7FFFu;
    bar1GpuPgt = (ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGT_BAR1_OFF);
    bar1PdeExpected = (((ULONGLONG)g_GrVm.Iova + GR_VM_GPU_PGT_BAR1_OFF) << 24) | (5ull << 32);
    bar1PteExpected = ((g_GrVm.Iova + GR_VM_D2_USERD_OFF) >> 8) | 1ull;
    gpuPgd[bar1PdeIndex] = bar1PdeExpected;
    bar1GpuPgt[bar1PteIndex] = bar1PteExpected;
    gpuPdb = ((g_GrVm.Iova + GR_VM_GPU_PGD_OFF) & ~0xFFFULL) | 4u;
    inst[0x200 / sizeof(ULONGLONG)] = gpuPdb;
    inst[0x208 / sizeof(ULONGLONG)] = 0xFFFFFFFFFFULL;
    wfiGpuVa = (ULONGLONG)GR_VM_GPU_VA + GR_VM_CTX_OFF;
    /* nvgpu gr_gk20a_commit_inst() first commits the channel context VA before
     * gr_gk20a_init_golden_ctx_image() temporarily commits GOLDEN_CTX_VA.
     * Word 132 carries target/mode + VA[31:12]; word 133 carries VA[39:32]. */
    instWords[132] = (ULONG)((wfiGpuVa & 0xFFFFF000ULL) | 4u);
    instWords[133] = (ULONG)((wfiGpuVa >> 32) & 0xFFu);
    KeMemoryBarrier();
    if (gpuPgd[gpuPdeIndex] != gpuPdeExpected) { failPoint = 10u; result = 0xE000008Au; goto done; }
    if (gpuPgd[bar1PdeIndex] != bar1PdeExpected ||
        bar1GpuPgt[bar1PteIndex] != bar1PteExpected) {
        failPoint = 15u; result = 0xE000008Au; goto done;
    }
    for (i = 0; i < pageCount; ++i) {
        ULONGLONG gpuPteExpected = ((g_GrVm.Iova + ((ULONGLONG)i << 12)) >> 8) | 1ull;
        if (gpuPgt[gpuPteIndex + i] != gpuPteExpected) {
            gpuBadIndex = gpuPteIndex + i;
            break;
        }
    }
    TraceDetail(L"B3cC2dGpuPteVerified", i);
    TraceDetail(L"B3cC2dGpuPteBadIndex", gpuBadIndex);
    if (i != pageCount) { failPoint = 11u; result = 0xE000008Au; goto done; }

    instPdbLo = (ULONG)inst[0x200 / sizeof(ULONGLONG)];
    instPdbHi = (ULONG)(inst[0x200 / sizeof(ULONGLONG)] >> 32);
    instLimitLo = (ULONG)inst[0x208 / sizeof(ULONGLONG)];
    instLimitHi = (ULONG)(inst[0x208 / sizeof(ULONGLONG)] >> 32);
    TraceDetail(L"B3cC2dArenaPa", g_GrVm.Pa.LowPart);
    TraceDetail(L"B3cC2dArenaBytes", GR_VM_ARENA_SIZE);
    TraceDetail(L"B3cC2dArenaIovaLo", (ULONG)g_GrVm.Iova);
    TraceDetail(L"B3cC2dArenaIovaHi", (ULONG)(g_GrVm.Iova >> 32));
    TraceDetail(L"B3cC2dGpuVa", GR_VM_GPU_VA);
    TraceDetail(L"B3cC2dGpuPdeIndex", gpuPdeIndex);
    TraceDetail(L"B3cC2dGpuPdeExpectedLo", (ULONG)gpuPdeExpected);
    TraceDetail(L"B3cC2dGpuPdeExpectedHi", (ULONG)(gpuPdeExpected >> 32));
    TraceDetail(L"B3cC2dGpuPdeReadLo", (ULONG)gpuPgd[gpuPdeIndex]);
    TraceDetail(L"B3cC2dGpuPdeReadHi", (ULONG)(gpuPgd[gpuPdeIndex] >> 32));
    TraceDetail(L"B3cD3UserdAliasGpuVa", BAR1_D3_LOW_VA);
    TraceDetail(L"B3cD3UserdAliasPdeIndex", bar1PdeIndex);
    TraceDetail(L"B3cD3UserdAliasPdeExpectedLo", (ULONG)bar1PdeExpected);
    TraceDetail(L"B3cD3UserdAliasPdeExpectedHi", (ULONG)(bar1PdeExpected >> 32));
    TraceDetail(L"B3cD3UserdAliasPdeReadLo", (ULONG)gpuPgd[bar1PdeIndex]);
    TraceDetail(L"B3cD3UserdAliasPdeReadHi", (ULONG)(gpuPgd[bar1PdeIndex] >> 32));
    TraceDetail(L"B3cD3UserdAliasPteIndex", bar1PteIndex);
    TraceDetail(L"B3cD3UserdAliasPteExpectedLo", (ULONG)bar1PteExpected);
    TraceDetail(L"B3cD3UserdAliasPteExpectedHi", (ULONG)(bar1PteExpected >> 32));
    TraceDetail(L"B3cD3UserdAliasPteReadLo", (ULONG)bar1GpuPgt[bar1PteIndex]);
    TraceDetail(L"B3cD3UserdAliasPteReadHi", (ULONG)(bar1GpuPgt[bar1PteIndex] >> 32));
    TraceDetail(L"B3cC2dGpuPdeLo", (ULONG)gpuPgd[gpuPdeIndex]);
    TraceDetail(L"B3cC2dGpuPdeHi", (ULONG)(gpuPgd[gpuPdeIndex] >> 32));
    TraceDetail(L"B3cC2dInstPdbLo", instPdbLo);
    TraceDetail(L"B3cC2dInstPdbHi", instPdbHi);
    TraceDetail(L"B3cC2dInstLimitLo", instLimitLo);
    TraceDetail(L"B3cC2dInstLimitHi", instLimitHi);
    TraceDetail(L"B3cC2dInstWfiPtrLo", instWords[132]);
    TraceDetail(L"B3cC2dInstWfiPtrHi", instWords[133]);
    TraceDetail(L"B3cC2dGoldenCommitVa", GR_VM_GPU_VA + GR_VM_GLOBAL_OFF);
    TraceDetail(L"B3cC2dGoldenVa", GR_VM_GPU_VA + GR_VM_GLOBAL_OFF);
    TraceDetail(L"B3cC2dBundleVa", GR_VM_GPU_VA + GR_VM_BUNDLE_OFF);
    TraceDetail(L"B3cC2dPagepoolVa", GR_VM_GPU_VA + GR_VM_PAGEPOOL_OFF);
    TraceDetail(L"B3cC2dAttribVa", GR_VM_GPU_VA + GR_VM_ATTRIB_OFF);
    TraceDetail(L"B3cC2dAttribBaseReg", 0x00418810u);
    TraceDetail(L"B3cC2dAttribBaseField",
          (((GR_VM_GPU_VA + GR_VM_ATTRIB_OFF) >> 12) & 0x0FFFFFFFu) | 0x80000000u);
    TraceDetail(L"B3cC2dPrivMapVa", GR_VM_GPU_VA + GR_VM_PRIV_OFF);
    TraceDetail(L"B3cC2dChannelCtxVa", GR_VM_GPU_VA + GR_VM_CTX_OFF);
    TraceDetail(L"B3cC2dZcullVa", GR_VM_GPU_VA + GR_VM_ZCULL_OFF);
    TraceDetail(L"B3cC2dPmVa", GR_VM_GPU_VA + GR_VM_PM_OFF);
    TraceDetail(L"B3cC2dPatchVa", GR_VM_GPU_VA + GR_VM_PATCH_OFF);
    TraceDetail(L"B3cC2dPatchBytes", GR_VM_PATCH_BYTES);

    /* The whole arena was zeroed. Read both edges of each backing allocation
     * so the next run distinguishes a valid mapped allocation from only a
     * plausible virtual-address calculation. */
    backingSample[0][0] = GR_VM_CTX_OFF;       backingSample[0][1] = 0;
    backingSample[1][0] = GR_VM_CTX_OFF + goldenAligned - 4u; backingSample[1][1] = 0;
    backingSample[2][0] = GR_VM_GLOBAL_OFF;    backingSample[2][1] = 0;
    backingSample[3][0] = GR_VM_GLOBAL_OFF + goldenAligned - 4u; backingSample[3][1] = 0;
    backingSample[4][0] = GR_VM_ATTRIB_OFF;    backingSample[4][1] = 0;
    backingSample[5][0] = GR_VM_ATTRIB_OFF + attribBytes - 4u; backingSample[5][1] = 0;
    backingSample[6][0] = GR_VM_PRIV_OFF;      backingSample[6][1] = 0;
    backingSample[7][0] = GR_VM_PRIV_OFF + privBytes - 4u; backingSample[7][1] = 0;
    backingSample[8][0] = GR_VM_BUNDLE_OFF;    backingSample[8][1] = 0;
    backingSample[9][0] = GR_VM_BUNDLE_OFF + bundleBytes - 4u; backingSample[9][1] = 0;
    backingSample[10][0] = GR_VM_PAGEPOOL_OFF; backingSample[10][1] = 0;
    backingSample[11][0] = GR_VM_PAGEPOOL_OFF + pagepoolBytes - 4u; backingSample[11][1] = 0;
    backingSample[12][0] = GR_VM_ZCULL_OFF;    backingSample[12][1] = 0;
    backingSample[13][0] = GR_VM_ZCULL_OFF + zcullAligned - 4u; backingSample[13][1] = 0;
    backingSample[14][0] = GR_VM_PM_OFF;      backingSample[14][1] = 0;
    backingSample[15][0] = GR_VM_PM_OFF + pmAligned - 4u; backingSample[15][1] = 0;
    for (i = 0; i < RTL_NUMBER_OF(backingSample); ++i) {
        backingSample[i][2] = *(volatile ULONG *)((PUCHAR)g_GrVm.Arena + backingSample[i][0]);
        if (backingSample[i][2] != backingSample[i][1]) { failPoint = 12u; result = 0xE000008Au; }
    }
    TraceBin(L"B3cC2dBackingSamples", backingSample, sizeof(backingSample));

    /* Verify the first, middle and final PTE plus RAMIN fields and outer map. */
    sampleCount = 3;
    sample[0][0] = gpuPteIndex;
    sample[0][1] = (ULONG)((g_GrVm.Iova >> 8) | 1ull);
    sample[0][2] = (ULONG)gpuPgt[gpuPteIndex];
    sample[1][0] = gpuPteIndex + (pageCount >> 1);
    sample[1][1] = (ULONG)((g_GrVm.Iova + ((ULONGLONG)(pageCount >> 1) << 12)) >> 8) | 1u;
    sample[1][2] = (ULONG)gpuPgt[gpuPteIndex + (pageCount >> 1)];
    sample[2][0] = gpuPteIndex + pageCount - 1u;
    sample[2][1] = (ULONG)(((g_GrVm.Iova + ((ULONGLONG)(pageCount - 1u) << 12)) >> 8) | 1ull);
    sample[2][2] = (ULONG)gpuPgt[gpuPteIndex + pageCount - 1u];
    TraceBin(L"B3cC2dGpuPteSamples", sample, sampleCount * sizeof(sample[0]));
    TraceDurable(L"B3cC2dSmmuMode", 1u);
    {
        grPageFirst = g_GrVm.SmmuPteFirst;
        grPteExpected = ((g_GrVm.Pa.LowPart >> 12) + (pageCount >> 1)) | SMMU_ATTR_RWNS;
        grPteRead = smmuPt[grPageFirst + (pageCount >> 1)];
        TraceDetail(L"B3cC2dOuterPteIndex", grPageFirst + (pageCount >> 1));
        TraceDetail(L"B3cC2dOuterPteExpected", grPteExpected);
        TraceDetail(L"B3cC2dOuterPteRead", grPteRead);
        if (grPteRead != grPteExpected) { failPoint = 13u; result = 0xE000008Au; goto done; }
    }
    if (instPdbLo != (ULONG)gpuPdb || instPdbHi != (ULONG)(gpuPdb >> 32) ||
        instLimitLo != 0xFFFFFFFFu || instLimitHi != 0xFFu ||
        instWords[132] != (ULONG)((wfiGpuVa & 0xFFFFF000ULL) | 4u) ||
        instWords[133] != (ULONG)((wfiGpuVa >> 32) & 0xFFu) ||
        sample[0][1] != sample[0][2] || sample[1][1] != sample[1][2] ||
        sample[2][1] != sample[2][2]) {
        failPoint = 14u; result = 0xE000008Au; goto done;
    }
    if (result != 0u) goto done;
    g_GrVm.Active = TRUE;

done:
    TraceDetail(L"B3cC2dFailPoint", failPoint);
    TraceDetail(L"B3cC2dAllocRetry", allocRetry);
    TraceDurable(L"B3cC2dResult", result);
    return result;
}

/* Match nvgpu gr_gk20a_wait_idle() and gr_gk20a_wait_fe_idle(): GR is idle
 * for INVALID FIFO context or (!GR busy && !ctxsw); FE is idle when only the
 * FE method-lower field is zero. The current GM20B path uses GR engine slot 0. */
static ULONG B3cC2eWait(volatile UCHAR *B, ULONG waitStage, ULONG bundleIndex,
                        BOOLEAN waitFe, PULONG state, PULONG history,
                        PULONG historyCount, PULONG historyDropped,
                        ULONG historyCapacity)
{
    ULONG polls, grStatus = 0, grEngineStatus = 0, fifoEngineStatus = 0;
    ULONG ctxStatus = 0, ctxswActive = 0, grBusy = 0, idle, idleBranch = 0;
    for (polls = 0; polls < 2000u; ++polls) {
        grStatus = Rd(B, 0x00400700u);
        /* Snapshot FIFO/engine even on FE waits so each wait event records
         * whether GR was truly idle, not just whether the FE method cleared. */
        fifoEngineStatus = Rd(B, 0x00002640u); /* FIFO GR engine slot 0 */
        ctxStatus = (fifoEngineStatus >> 13) & 7u;
        ctxswActive = (fifoEngineStatus & 0x8000u) != 0u;
        grEngineStatus = Rd(B, 0x0040060Cu);
        grBusy = grEngineStatus & 1u;
        if (waitFe) {
            if (((grStatus >> 2) & 1u) == 0u) { idleBranch = 3u; break; }
        } else {
            if (ctxStatus == 0u) { idleBranch = 1u; break; }
            if (!grBusy && !ctxswActive) { idleBranch = 2u; break; }
        }
        Ms(1);
    }
    idle = (polls < 2000u) ? 1u : 0u;
    state[0] = waitStage; state[1] = bundleIndex; state[2] = polls;
    state[3] = grStatus; state[4] = grEngineStatus;
    state[5] = fifoEngineStatus; state[6] = waitFe ? 1u : 0u;
    state[7] = idle; state[8] = idleBranch; state[9] = ctxStatus;
    state[10] = ctxswActive; state[11] = grBusy;
    if (history != NULL && historyCount != NULL && historyDropped != NULL) {
        if (*historyCount < historyCapacity) {
            PULONG record = history + (*historyCount * 10u);
            record[0] = waitStage; record[1] = bundleIndex; record[2] = polls;
            record[3] = grStatus; record[4] = grEngineStatus;
            record[5] = fifoEngineStatus; record[6] = ctxStatus;
            record[7] = ctxswActive; record[8] = grBusy;
            record[9] = idleBranch | (waitFe ? 0x100u : 0u);
            ++(*historyCount);
        } else {
            ++(*historyDropped);
        }
    }
    return idle;
}

/* Captured on every C2e exit, before the normal GPU reset path. */
static VOID B3cC2eCaptureDiagnostics(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    ULONG i, id, count = 0, idx, nr, pc[8] = { 0 }, rec[6][5] = { 0 };
    ULONG fecsCtxsw[8] = { 0 }, gpccsCtxsw[8] = { 0 };
    ULONG goldenHead[16] = { 0 };
    ULONG value;
    value = Rd(B, 0x00404000u);
    TraceDetail(L"B3cC2ePostFeHwwEsr", value); TraceDetail(L"B3cC2eFeHwwEsr", value);
    value = Rd(B, 0x004041B0u);
    TraceDetail(L"B3cC2ePostFeHwwEsrInfo", value); TraceDetail(L"B3cC2eFeHwwEsrInfo", value);
    TraceDetail(L"B3cC2ePostFeInfoRaw404004", Rd(B, 0x00404004u));
    TraceDetail(L"B3cC2ePostGrIntr", Rd(B, 0x00400100u));
    TraceDetail(L"B3cC2ePostGrIntrEnable", Rd(B, 0x0040013Cu));
    TraceDetail(L"B3cC2ePostGrException", Rd(B, 0x00400108u));
    TraceDetail(L"B3cC2ePostGrExceptionEnable", Rd(B, 0x00400138u));
    TraceDetail(L"B3cC2ePostGrException1", Rd(B, 0x00400118u));
    TraceDetail(L"B3cC2ePostGrException1Enable", Rd(B, 0x00400130u));
    TraceDetail(L"B3cC2ePostGrException2", Rd(B, 0x0040011Cu));
    TraceDetail(L"B3cC2ePostGrException2Enable", Rd(B, 0x00400134u));
    TraceDetail(L"B3cC2ePostGrStatus1", Rd(B, 0x00400604u));
    TraceDetail(L"B3cC2ePostGrStatus2", Rd(B, 0x00400608u));
    TraceDetail(L"B3cC2eGrStatus1", Rd(B, 0x00400604u));
    TraceDetail(L"B3cC2eGrStatus2", Rd(B, 0x00400608u));
    value = Rd(B, 0x00409C18u);
    TraceDetail(L"B3cC2ePostFecsHostIntStatus", value); TraceDetail(L"B3cC2eFecsHostIntStatus", value);
    TraceDetail(L"B3cC2ePostFecsHostIntClear", Rd(B, 0x00409C20u));
    TraceDetail(L"B3cC2ePostFecsHostIntEnable", Rd(B, 0x00409C24u));
    TraceDetail(L"B3cC2ePostMemfmtHwwEsr", Rd(B, 0x00404600u));
    TraceDetail(L"B3cC2ePostSccHwwEsr", Rd(B, 0x00408030u));
    TraceDetail(L"B3cC2ePostMmeHwwEsr", Rd(B, 0x00404490u));
    TraceDetail(L"B3cC2ePostPdHwwEsr", Rd(B, 0x00406018u));
    TraceDetail(L"B3cC2ePostSkedHwwEsr", Rd(B, 0x00407020u));
    TraceDetail(L"B3cC2ePostDsHwwEsr", Rd(B, 0x00405840u));
    value = Rd(B, 0x00400700u);
    TraceDetail(L"B3cC2ePostGrStatus", value); TraceDetail(L"B3cC2eGrStatus", value);
    TraceDetail(L"B3cC2ePostGrStatusBit0", value & 1u);
    TraceDetail(L"B3cC2ePostGrStatusBit5", (value >> 5) & 1u);
    TraceDetail(L"B3cC2ePostGrStatusBit7", (value >> 7) & 1u);
    TraceDetail(L"B3cC2ePostGrStatusFeGi", (value >> 21) & 1u);
    value = Rd(B, 0x0040060Cu);
    TraceDetail(L"B3cC2ePostGrEngineStatus", value); TraceDetail(L"B3cC2eGrEngineStatus", value);
    TraceDetail(L"B3cC2ePostFifoEngineStatus", Rd(B, 0x00002640u));
    TraceDetail(L"B3cC2ePostGrActivity0", Rd(B, 0x00400380u));
    TraceDetail(L"B3cC2ePostGrActivity1", Rd(B, 0x00400384u));
    TraceDetail(L"B3cC2ePostGrActivity2", Rd(B, 0x00400388u));
    TraceDetail(L"B3cC2ePostGrActivity3Present", 0u); /* no gr_activity_3_r() in GM20B header */
    TraceDetail(L"B3cC2ePostGrActivity4", Rd(B, 0x00400390u));
    TraceDetail(L"B3cC2ePostFeGoIdle", Rd(B, 0x00404154u));
    TraceDetail(L"B3cC2ePostFeGoIdleOnStatus", Rd(B, 0x00404150u));
    TraceDetail(L"B3cC2ePostFePowerMode", Rd(B, 0x00404170u));
    TraceDetail(L"B3cC2ePostFeGoIdleCheck", Rd(B, 0x00404158u));
    TraceDetail(L"B3cC2ePostFeGoIdleInfo", Rd(B, 0x00404194u));
    value = Rd(B, 0x00409100u);
    TraceDetail(L"B3cC2ePostFecsCpuctl", value); TraceDetail(L"B3cC2eFecsCpuctl", value);
    value = Rd(B, 0x00409040u);
    TraceDetail(L"B3cC2ePostFecsMailbox0", value); TraceDetail(L"B3cC2eFecsMailbox0", value);
    value = Rd(B, 0x00409044u);
    TraceDetail(L"B3cC2ePostFecsMailbox1", value); TraceDetail(L"B3cC2eFecsMailbox1", value);
    value = Rd(B, 0x00409008u);
    TraceDetail(L"B3cC2ePostFecsIrqstat", value); TraceDetail(L"B3cC2eFecsIrqstat", value);
    TraceDetail(L"B3cC2ePostFecsIrqmask", Rd(B, 0x0040900Cu));
    value = Rd(B, 0x00409240u);
    TraceDetail(L"B3cC2ePostFecsSctl", value); TraceDetail(L"B3cC2eFecsSctl", value);
    for (i = 0; i < RTL_NUMBER_OF(fecsCtxsw); ++i) {
        fecsCtxsw[i] = Rd(B, 0x00409800u + i * 4u);
        gpccsCtxsw[i] = Rd(B, 0x0041A800u + i * 4u);
    }
    TraceBin(L"B3cC2eFecsCtxswMailboxes", fecsCtxsw, sizeof(fecsCtxsw));
    TraceBin(L"B3cC2eGpccsCtxswMailboxes", gpccsCtxsw, sizeof(gpccsCtxsw));
    idx = Rd(B, 0x00409148u); nr = (idx >> 16) & 0xFFu;
    if (nr > RTL_NUMBER_OF(pc)) nr = RTL_NUMBER_OF(pc);
    TraceDetail(L"B3cC2ePostFecsTraceIdx", idx); TraceDetail(L"B3cC2ePostFecsTraceCount", nr);
    for (i = 0; i < nr; ++i) { Wr(B, 0x00409148u, i); pc[i] = Rd(B, 0x0040914Cu); }
    if (nr) TraceBin(L"B3cC2ePostFecsTracePc", pc, nr * sizeof(ULONG));
    value = Rd(B, 0x0041A100u);
    TraceDetail(L"B3cC2ePostGpccsCpuctl", value); TraceDetail(L"B3cC2eGpccsCpuctl", value);
    value = Rd(B, 0x0041A240u);
    TraceDetail(L"B3cC2ePostGpccsSctl", value); TraceDetail(L"B3cC2eGpccsSctl", value);
    value = Rd(B, 0x0041A040u);
    TraceDetail(L"B3cC2ePostGpccsMailbox0", value); TraceDetail(L"B3cC2eGpccsMailbox0", value);
    value = Rd(B, 0x0041A044u);
    TraceDetail(L"B3cC2ePostGpccsMailbox1", value); TraceDetail(L"B3cC2eGpccsMailbox1", value);
    TraceDetail(L"B3cC2ePostGpccsIrqstat", Rd(B, 0x0041A008u));
    TraceDetail(L"B3cC2ePostGpccsIrqmask", Rd(B, 0x0041A00Cu));
    idx = Rd(B, 0x0041A148u); nr = (idx >> 16) & 0xFFu;
    if (nr > RTL_NUMBER_OF(pc)) nr = RTL_NUMBER_OF(pc);
    TraceDetail(L"B3cC2ePostGpccsTraceIdx", idx); TraceDetail(L"B3cC2ePostGpccsTraceCount", nr);
    for (i = 0; i < nr; ++i) { Wr(B, 0x0041A148u, i); pc[i] = Rd(B, 0x0041A14Cu); }
    if (nr) TraceBin(L"B3cC2ePostGpccsTracePc", pc, nr * sizeof(ULONG));
    value = Rd(B, 0x2100u);
    TraceDetail(L"B3cC2ePostPfifoIntr0", value); TraceDetail(L"B3cC2ePfifoIntr0", value);
    id = Rd(B, 0x259Cu); TraceDetail(L"B3cC2ePostFaultId", id); TraceDetail(L"B3cC2eFaultId", id);
    for (i = 0; i < 32u && count < RTL_NUMBER_OF(rec); ++i) {
        if (!(id & (1u << i))) continue;
        rec[count][0] = i; rec[count][1] = Rd(B, 0x2800u + i * 0x10u);
        rec[count][2] = Rd(B, 0x2804u + i * 0x10u);
        rec[count][3] = Rd(B, 0x2808u + i * 0x10u);
        rec[count][4] = Rd(B, 0x280Cu + i * 0x10u); ++count;
    }
    TraceDetail(L"B3cC2ePostFaultCount", count); TraceDetail(L"B3cC2eFaultCount", count);
    if (count) {
        TraceErrorBin(L"B3cC2ePostFaultRecords", rec, count * sizeof(rec[0]));
        TraceErrorBin(L"B3cC2eFaultRecords", rec, count * sizeof(rec[0]));
    }
    value = Rd(Dev->Mc, MC_INTSTATUS);
    TraceDetail(L"B3cC2ePostMcInt", value); TraceDetail(L"B3cC2eMcInt", value);
    value = Rd(Dev->Mc, MC_ERR_STATUS);
    TraceDetail(L"B3cC2ePostMcErr", value); TraceDetail(L"B3cC2eMcErr", value);
    value = Rd(Dev->Mc, MC_ERR_ADR);
    TraceDetail(L"B3cC2ePostMcErrAdr", value); TraceDetail(L"B3cC2eMcErrAdr", value);
    if (g_GrVm.Active && g_GrVm.Arena != NULL) {
        for (i = 0; i < RTL_NUMBER_OF(goldenHead); ++i)
            goldenHead[i] = *(volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + i * sizeof(ULONG));
        TraceBin(L"B3cC2eGoldenRawOnExit", goldenHead, sizeof(goldenHead));
        /* Claude: goldenHead holds only the first 16 words; read the 0xFC magic directly. */
        TraceDetail(L"B3cC2eGoldenMagicOnExit",
              *(volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0xFCu));
    }
}

static VOID B3cC2eCapturePreBundle(volatile UCHAR *B)
{
    TraceDetail(L"B3cC2ePreBundleFeHwwEsr", Rd(B, 0x00404000u));
    TraceDetail(L"B3cC2ePreBundleFeHwwEsrInfo", Rd(B, 0x004041B0u));
    TraceDetail(L"B3cC2ePreBundleFeInfoRaw404004", Rd(B, 0x00404004u));
    TraceDetail(L"B3cC2ePreBundleGrIntr", Rd(B, 0x00400100u));
    TraceDetail(L"B3cC2ePreBundleGrIntrEnable", Rd(B, 0x0040013Cu));
    TraceDetail(L"B3cC2ePreBundleGrException", Rd(B, 0x00400108u));
    TraceDetail(L"B3cC2ePreBundleGrExceptionEnable", Rd(B, 0x00400138u));
    TraceDetail(L"B3cC2ePreBundleGrException1", Rd(B, 0x00400118u));
    TraceDetail(L"B3cC2ePreBundleGrException1Enable", Rd(B, 0x00400130u));
    TraceDetail(L"B3cC2ePreBundleGrException2", Rd(B, 0x0040011Cu));
    TraceDetail(L"B3cC2ePreBundleGrException2Enable", Rd(B, 0x00400134u));
    TraceDetail(L"B3cC2ePreBundleGrStatus1", Rd(B, 0x00400604u));
    TraceDetail(L"B3cC2ePreBundleGrStatus2", Rd(B, 0x00400608u));
    TraceDetail(L"B3cC2ePreBundleFecsHostIntStatus", Rd(B, 0x00409C18u));
    TraceDetail(L"B3cC2ePreBundleFecsHostIntClear", Rd(B, 0x00409C20u));
    TraceDetail(L"B3cC2ePreBundleFecsHostIntEnable", Rd(B, 0x00409C24u));
}

static VOID B3cC2eCaptureBeforeExceptionSetup(volatile UCHAR *B)
{
    TraceDetail(L"B3cC2eBeforeSetupFeHwwEsr", Rd(B, 0x00404000u));
    TraceDetail(L"B3cC2eBeforeSetupFeHwwEsrInfo", Rd(B, 0x004041B0u));
    TraceDetail(L"B3cC2eBeforeSetupFeInfoRaw404004", Rd(B, 0x00404004u));
    TraceDetail(L"B3cC2eBeforeSetupGrIntr", Rd(B, 0x00400100u));
    TraceDetail(L"B3cC2eBeforeSetupGrIntrEnable", Rd(B, 0x0040013Cu));
    TraceDetail(L"B3cC2eBeforeSetupGrException", Rd(B, 0x00400108u));
    TraceDetail(L"B3cC2eBeforeSetupGrExceptionEnable", Rd(B, 0x00400138u));
    TraceDetail(L"B3cC2eBeforeSetupGrException1", Rd(B, 0x00400118u));
    TraceDetail(L"B3cC2eBeforeSetupGrException1Enable", Rd(B, 0x00400130u));
    TraceDetail(L"B3cC2eBeforeSetupGrException2", Rd(B, 0x0040011Cu));
    TraceDetail(L"B3cC2eBeforeSetupGrException2Enable", Rd(B, 0x00400134u));
    TraceDetail(L"B3cC2eBeforeSetupGrStatus1", Rd(B, 0x00400604u));
    TraceDetail(L"B3cC2eBeforeSetupGrStatus2", Rd(B, 0x00400608u));
    TraceDetail(L"B3cC2eBeforeSetupFecsHostIntStatus", Rd(B, 0x00409C18u));
    TraceDetail(L"B3cC2eBeforeSetupFecsHostIntClear", Rd(B, 0x00409C20u));
    TraceDetail(L"B3cC2eBeforeSetupFecsHostIntEnable", Rd(B, 0x00409C24u));
}

/* Mirror gk20a_mm_l2_flush(g, true): flush dirty L2 lines, then invalidate.
 * Both operations use the GM20B hw_flush registers and a finite poll bound. */
static BOOLEAN B3cC2eL2Flush(volatile UCHAR *B, PULONG state)
{
    ULONG polls, status = 0;
    Wr(B, 0x00070010u, 1u); /* flush_l2_flush_dirty_pending_busy_f() */
    for (polls = 0; polls < 2000u; ++polls) {
        status = Rd(B, 0x00070010u);
        if ((status & 3u) == 0u) break;
        Us(5);
    }
    state[0] = polls; state[1] = status;
    if (polls == 2000u) { state[2] = 0u; state[3] = 0u; return FALSE; }

    Wr(B, 0x00070004u, 1u); /* flush_l2_system_invalidate_pending_busy_f() */
    for (polls = 0; polls < 2000u; ++polls) {
        status = Rd(B, 0x00070004u);
        if ((status & 3u) == 0u) break;
        Us(5);
    }
    state[2] = polls; state[3] = status;
    return (polls < 2000u) ? TRUE : FALSE;
}

/* Exact register set emitted by MIT nvgpu gr_gm20b_dump_gr_status_regs().
 * The target has one GPC and two TPCs, so both conditional TPC1 reads are
 * included. Each trace is address/value DWORD pairs in this source order. */
#define B3C_GR_STATUS_DUMP_REGS 62u
static VOID B3cCaptureGrStatusDump(volatile UCHAR *B, PCWSTR Name, PULONG Pairs)
{
    static const ULONG offsets[B3C_GR_STATUS_DUMP_REGS] = {
        0x00400700u, 0x00400604u, 0x00400608u, 0x0040060Cu,
        0x00400504u, 0x00400500u, 0x00409C18u, 0x00400108u,
        0x00400144u, 0x00002640u, 0x00400380u, 0x00400384u,
        0x00400388u, 0x00400390u, 0x00407054u, 0x00502C80u,
        0x00502C84u, 0x00502C88u, 0x00502C8Cu, 0x00504500u,
        0x00504D00u, 0x00501D00u, 0x0041AC80u, 0x0041AC84u,
        0x0041AC88u, 0x0041AC8Cu, 0x0041C500u, 0x0041CD00u,
        0x00419D00u, 0x00410200u, 0x00410600u, 0x00408A00u,
        0x00405858u, 0x00404150u, 0x00404154u, 0x00404158u,
        0x00404194u, 0x00504238u, 0x00405B00u, 0x004041C4u,
        0x00405B60u, 0x00405BA0u, 0x00409C00u, 0x00409400u,
        0x00502C04u, 0x00502400u, 0x00409420u, 0x00502420u,
        0x00409B00u, 0x00409B04u, 0x00410134u, 0x00408934u,
        0x00410048u, 0x0041004Cu, 0x00408848u, 0x0040884Cu,
        0x00410204u, 0x00410208u, 0x00502C90u, 0x00502C94u,
        0x00504508u, 0x0050450Cu
    };
    ULONG i;
    for (i = 0; i < B3C_GR_STATUS_DUMP_REGS; ++i) {
        Pairs[i * 2u] = offsets[i];
        Pairs[i * 2u + 1u] = Rd(B, offsets[i]);
    }
    TraceErrorBin(Name, Pairs, B3C_GR_STATUS_DUMP_REGS * 2u * sizeof(ULONG));
}

/* C2e: follow nvgpu gr_gk20a_init_golden_ctx_image() and Nouveau
 * gm20b_grctx_generate_main() after FECS is ready.
 * GM20B registers/method encodings were checked against the unmodified MIT
 * hw_gr_gm20b.h and hw_ctxsw_prog_gm20b.h. sw_bundle_init entries go through
 * PIPE_BUNDLE_DATA/ADDRESS (gk20a_init_sw_bundle), never as BAR0 offsets.
 * The address/value table mirrors the GM20B global-buffer commits in
 * gr_gm20b.c; only fixed GR PRI addresses are written here. */
static ULONG B3cCreateGoldenContext(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR bundle = NULL;
    ULONG bundleBytes = 0, result = 0, pmcEnable, polls, i, lastData = 0;
    ULONG *instWords, instBlockPa, bindData, saveData, mailbox, bundleMethodId, value;
    ULONG written = 0, sample[8][3] = { 0 }, sampleCount = 0;
    ULONG timesliceTrace[6][4] = { 0 };
    ULONG waitState[12] = { 0 };
    PULONG waitHistory = NULL;
    ULONG waitHistoryCount = 0, waitHistoryDropped = 0, waitHistoryCapacity = 2048u;
    ULONG trueIdleAfterSwCtx[7] = { 0 }, trueIdleAfterSetup[7] = { 0 };
    ULONG trueIdleAfterRestore[7] = { 0 }, trueIdleAfterMethodInit[7] = { 0 };
    ULONG trueIdleGoBundle[7] = { 0 }, trueIdlePreSave[7] = { 0 };
    ULONG trueIdleGoBundleOk = 0, trueIdlePreSaveOk = 0;
    ULONG trueIdleAfterSwCtxOk = 0, trueIdleAfterSetupOk = 0;
    ULONG trueIdleAfterRestoreOk = 0, trueIdleAfterMethodInitOk = 0;
    ULONG firstBusyIndex = 0xFFFFFFFFu, firstBusyMethod = 0;
    ULONG firstBusyEngineStatus = 0, firstBusyGrStatus = 0, firstBusyFifoStatus = 0;
    ULONG feGoIdleTimeoutSaved = 0, feGoIdleTimeoutSavedValid = 0;
    ULONG mcUnk260AtEntry = 0;
    ULONG gr5044b0AtEntry = 0, gr5044b0AtExit = 0;
    ULONG l2FlushState[4] = { 0 };
    ULONG fePowerStatus = 0, resetAssertRead = 0, resetReleaseRead = 0;
    ULONG preBundleFeEsr = 0;
    ULONG bundleBaseline[5] = { 0 }, bundleEntry0[8] = { 0 };
    ULONG grDumpBefore[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0 };
    ULONG grDumpEntry0[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0 };
    ULONG grDumpStop[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0 };
    ULONG grDumpBeforeCaptured = 0, grDumpEntry0Captured = 0, grDumpStopCaptured = 0;
    ULONG bundleFirstChange[8] = { 0 }, bundleFirstChangeIndex = 0xFFFFFFFFu;
    ULONG bundleFirstChangeFound = 0;
    ULONG bundleFeEsr, bundleFeInfo, bundleGrIntr, bundleGrException, bundleGrStatus;
    ULONG preSaveGrStatus, preSaveGrStatus1, preSaveGrStatus2;
    ULONG preSaveGrEngineStatus, preSaveFifoEngineStatus;
    ULONG goldenHdr[64], goldenSamples[8][2] = { 0 };
    ULONG fecsTraceIdx, fecsTraceCount, fecsPc[8] = { 0 };
    ULONG faultId, faultCount = 0, unit, faultRecs[6][5] = { 0 };
    ULONG fsSample[12][3] = { 0 }, fsSampleCount = 0, fsWrites = 0;
    ULONG ropWrites[3][4] = { 0 }, ropWriteCount = 0;
    ULONG earlyZcullWrites[6][4] = { 0 }, earlyZcullWriteCount = 0;
    ULONG nouveauRopWrites[2][4] = { 0 }, nouveauRopWriteCount = 0;
    ULONG fifoCtlTrace[3] = { 0 }, fbpCountRaw = 0, fbpCount = 0;
    ULONG ltcCountRaw = 0, ltcCountDetected = 0;
    ULONG ltcMaxCount = 0, ltcActiveCountRaw = 0, ltcParamRaw = 0;
    ULONG ltcSlicesPerLtc = 0, ltcCacheLineBytes = 0, ltcTagsPerCacheLine = 0;
    ULONG ltcFsWrites[4][4] = { 0 }, ltcFsWriteCount = 0;
    ULONG fbFsWrites[1][4] = { 0 }, fbFsWriteCount = 0;
    ULONG cbcWrites[1][4] = { 0 }, cbcWriteCount = 0;
    ULONG zbcWrites[48][4] = { 0 }, zbcWriteCount = 0;
    ULONG zbcColorDs[3][4] = { {0u,0u,0u,0x3F800000u}, {0u,0u,0u,0u},
                                {0x3F800000u,0x3F800000u,0x3F800000u,0x3F800000u} };
    ULONG zbcColorL2[3][4] = { {0xFF000000u,0u,0u,0u}, {0u,0u,0u,0u},
                                {0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu,0xFFFFFFFFu} };
    ULONG zbcColorFmt[3] = { 0x28u, 1u, 2u };
    ULONG zbcDepth[2] = { 0x3F800000u, 0u };
    ULONG zbcColorIndex, zbcChanIndex, zbcLtcIndex, zbcAddress, zbcValue;
    ULONG zbcIdleState[7] = { 0 }, zbcIdleOk = 0;
    ULONG cbcNoComptag = 1u;
    ULONG ltcFsConfigured = 0, fbFsConfigured = 0;
    ULONG zbcConfigured = 0, cbcConfigured = 0, beRopConfigured = 0;
    ULONG rdCoalesceBefore = 0, rdCoalesceAfter = 0, rdCoalesceMask = 0x14u;
    ULONG bundleBase, pagepoolBase, attribBase, bundleCount = 0;
    ULONG preBundleGpcPriBadfCount = 0;
    struct { ULONG Address; ULONG Value; } globalWrites[] = {
        {0x0040800Cu, 0x00000800u}, {0x00408010u, 0x80000080u},
        {0x00419004u, 0x00000420u}, {0x00419008u, 0x00000080u},
        {0x004064CCu, 0x80000080u}, {0x00418E30u, 0x80000080u},
        {0x00408004u, 0x00000400u}, {0x00408008u, 0x80000018u},
        {0x00418E24u, 0x00000400u}, {0x00418E28u, 0x80000018u},
        {0x004064C8u, 0x00C001C0u},
        {0x00418810u, 0x80018050u}, {0x00419848u, 0x10018050u},
        {0x00419C2Cu, 0x10018050u}, {0x00405830u, 0x04000800u},
        {0x004064C4u, 0x0200FFFFu}, {0x005030C0u, 0x00000800u},
        {0x005030F4u, 0x00000000u}, {0x005030E4u, 0x00001000u},
        {0x005030F8u, 0x00000C00u}, {0x00418EA0u, 0x02AA0800u}
    };

    Step(81);
    B3cPrivRingResetWriteLog();
    mcUnk260AtEntry = Rd(B, 0x00000260u);
    TraceDetail(L"B3cC2eMcUnk260AtEntry", mcUnk260AtEntry);
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cC2ePmcEnable", pmcEnable);
    if (!(pmcEnable & (1u << 12)) || !g_GrVm.Active || g_GrVm.Arena == NULL) {
        result = 0xE000008Bu; goto done;
    }
    gr5044b0AtEntry = Rd(B, 0x005044B0u);
    TraceDetail(L"B3cC2eGr5044B0AtEntry", gr5044b0AtEntry);
    TraceDetail(L"B3cC2eGr5044B0Bit27AtEntry", (gr5044b0AtEntry >> 27) & 1u);
    if (g_Verbose) waitHistory = (PULONG)ExAllocatePool2(POOL_FLAG_NON_PAGED,
        waitHistoryCapacity * 10u * sizeof(ULONG), SWGPU_TAG);
    if (waitHistory != NULL) RtlZeroMemory(waitHistory,
        waitHistoryCapacity * 10u * sizeof(ULONG));
    TraceDetail(L"B3cC2eWaitHistoryCapacity", waitHistory != NULL ? waitHistoryCapacity : 0u);

    /* Step 82: nvgpu's FE-on, ctxsw reset pulse, FE-auto, then SCC clear. */
    Step(82);
    Wr(B, 0x00404170u, 0x12u); /* REQ_SEND | MODE_FORCE_ON */
    for (polls = 0; polls < 200u; ++polls) {
        fePowerStatus = Rd(B, 0x00404170u);
        if (((fePowerStatus >> 4) & 1u) == 0u) break;
        Us(10);
    }
    waitState[0] = 10u; waitState[1] = 0xFFFFFFFFu; waitState[2] = polls;
    waitState[3] = fePowerStatus; waitState[6] = 0u; waitState[7] = (polls < 200u);
    Trace(L"B3cC2eForceOnPolls", polls); TraceDetail(L"B3cC2eForceOnStatus", fePowerStatus);
    if (polls == 200u) { result = 0xE000008Du; goto done; }
    Wr(B, 0x00409614u, 0x70u);
    resetAssertRead = Rd(B, 0x00409614u);
    Us(10);
    Wr(B, 0x00409614u, 0x770u);
    resetReleaseRead = Rd(B, 0x00409614u);
    Us(10);
    TraceDetail(L"B3cC2eCtxswResetAssert", resetAssertRead);
    TraceDetail(L"B3cC2eCtxswResetRelease", resetReleaseRead);
    if (resetAssertRead != 0x70u || resetReleaseRead != 0x770u) {
        result = 0xE000008Du; goto done;
    }
    Wr(B, 0x00404170u, 0x10u); /* REQ_SEND | MODE_AUTO */
    for (polls = 0; polls < 200u; ++polls) {
        fePowerStatus = Rd(B, 0x00404170u);
        if (((fePowerStatus >> 4) & 1u) == 0u) break;
        Us(10);
    }
    waitState[0] = 11u; waitState[1] = 0xFFFFFFFFu; waitState[2] = polls;
    waitState[3] = fePowerStatus; waitState[6] = 0u; waitState[7] = (polls < 200u);
    Trace(L"B3cC2eAutoPolls", polls); TraceDetail(L"B3cC2eAutoStatus", fePowerStatus);
    if (polls == 200u) { result = 0xE000008Du; goto done; }
    Wr(B, 0x0040802Cu, 1u); /* SCC RAM clear */
    TraceDetail(L"B3cC2eSccInit", Rd(B, 0x0040802Cu));
    bundleBase = (GR_VM_GPU_VA + GR_VM_BUNDLE_OFF) >> 8;
    pagepoolBase = (GR_VM_GPU_VA + GR_VM_PAGEPOOL_OFF) >> 8;
    attribBase = (GR_VM_GPU_VA + GR_VM_ATTRIB_OFF) >> 12;
    globalWrites[0].Value = pagepoolBase;
    globalWrites[1].Value = 0x80000080u;
    globalWrites[2].Value = pagepoolBase;
    globalWrites[3].Value = 0x00000080u;
    globalWrites[4].Value = 0x80000080u;
    globalWrites[5].Value = 0x80000080u;
    globalWrites[6].Value = bundleBase;
    globalWrites[7].Value = 0x80000018u;
    globalWrites[8].Value = bundleBase;
    globalWrites[9].Value = 0x80000018u;
    globalWrites[11].Value = attribBase | 0x80000000u;
    globalWrites[12].Value = attribBase | 0x10000000u;
    globalWrites[13].Value = attribBase | 0x10000000u;
    for (i = 0; i < RTL_NUMBER_OF(globalWrites); ++i) {
        if ((globalWrites[i].Address & 3u) != 0u ||
            !B3cIsGrPriAddress(globalWrites[i].Address)) {
            result = 0xE000008Cu; goto done;
        }
    }
    instWords = (ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF);
    /* Claude: FECS must see the instance block through the same GPU-visible
     * address as the C2d PDB (IOMMU-tagged IOVA when the SMMU is active),
     * like the ACR PMU bind (inst 0x704FDC68 carries the tag bit 22). */
    instBlockPa = (ULONG)(((g_GrVm.Iova + GR_VM_INST_OFF) >> 12) & 0x0FFFFFFFu);
    bindData = instBlockPa | 0xB0000000u;
    TraceDetail(L"B3cC2eInstIovaPage", instBlockPa);
    saveData = bindData;
    /* Claude: invalidate the GPU MMU for the new GR PDB before FECS first
     * walks it, using the same bounded helper as the ACR path. */
    if (!AcrMmuInvalidatePdb(B, g_GrVm.Iova + GR_VM_GPU_PGD_OFF)) {
        TraceDetail(L"B3cC2eGrPdbInvalidate", 0u);
        result = 0xE000008Du; goto done;
    }
    TraceDetail(L"B3cC2eGrPdbInvalidate", 1u);
    /* Steps 98-100 follow the front of nvgpu gk20a_init_gr_setup_hw():
     * PE stream-master + zcull setup, Nouveau's GM20B ROP active-FBP count,
     * then GPFIFO/semaphore access.  The fixed 1-GPC/2-TPC values reuse the
     * previously validated zcull map; no unbounded topology is accepted. */
    Step(101);
    earlyZcullWrites[0][0] = 0x00503018u;
    earlyZcullWrites[0][1] = Rd(B, earlyZcullWrites[0][0]);
    earlyZcullWrites[0][2] = earlyZcullWrites[0][1] | 1u;
    earlyZcullWrites[1][0] = 0x00418980u;
    earlyZcullWrites[1][1] = Rd(B, earlyZcullWrites[1][0]);
    earlyZcullWrites[1][2] = 0x10u;
    earlyZcullWrites[2][0] = 0x00500914u;
    earlyZcullWrites[2][1] = Rd(B, earlyZcullWrites[2][0]);
    earlyZcullWrites[2][2] = 0x00000201u;
    earlyZcullWrites[3][0] = 0x00500910u;
    earlyZcullWrites[3][1] = Rd(B, earlyZcullWrites[3][0]);
    earlyZcullWrites[3][2] = 0x00040002u;
    earlyZcullWrites[4][0] = 0x00500918u;
    earlyZcullWrites[4][1] = Rd(B, earlyZcullWrites[4][0]);
    earlyZcullWrites[4][2] = 0x00400000u; /* ceil(0x800000 / 2 TPCs) */
    earlyZcullWrites[5][0] = 0x0041BFD4u;
    earlyZcullWrites[5][1] = Rd(B, earlyZcullWrites[5][0]);
    earlyZcullWrites[5][2] = 0x00400000u;
    for (i = 0; i < RTL_NUMBER_OF(earlyZcullWrites); ++i) {
        if ((earlyZcullWrites[i][0] & 3u) ||
            !B3cIsGrPriAddress(earlyZcullWrites[i][0])) {
            result = 0xE000009Cu; goto done;
        }
    }
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    for (i = 0; i < RTL_NUMBER_OF(earlyZcullWrites); ++i) {
        Wr(B, earlyZcullWrites[i][0], earlyZcullWrites[i][2]);
        if (!B3cPrivRingAfterGrWrite(B, earlyZcullWrites[i][0], earlyZcullWrites[i][2])) {
            result = 0xE0000095u; goto done;
        }
        earlyZcullWrites[i][3] = Rd(B, earlyZcullWrites[i][0]);
        ++earlyZcullWriteCount;
        /* Claude: 0.4.55 HW showed zcull registers that do not read back the
         * written value (0x500910 adds bit 30, 0x500918 and broadcast 0x41BFD4
         * read 0). The same values were written by the old Step 83 without
         * stopping, and the priv ring reported no rejection, so a readback
         * difference here is recorded (earlyZcullWrites) but not fatal. */
    }
    Step(102);
    fbpCountRaw = Rd(B, 0x00120074u);
    fbpCount = fbpCountRaw & 0xFu;
    nouveauRopWrites[0][0] = 0x00408850u;
    nouveauRopWrites[1][0] = 0x00408958u;
    for (i = 0; i < RTL_NUMBER_OF(nouveauRopWrites); ++i) {
        nouveauRopWrites[i][1] = Rd(B, nouveauRopWrites[i][0]);
        nouveauRopWrites[i][2] = (nouveauRopWrites[i][1] & ~0xFu) | fbpCount;
        if ((nouveauRopWrites[i][0] & 3u) ||
            !B3cIsGrPriAddress(nouveauRopWrites[i][0])) {
            result = 0xE000009Du; goto done;
        }
    }
    /* Claude: 0.4.55 HW read 0x120074 = 0. Do not program 0 active units;
     * skip Nouveau's FBP-count variant and let Step 92 (nvgpu, LTC count 2,
     * verified on 0.4.54) set ZROP/CROP. */
    TraceDetail(L"B3cC2eNouveauRopSkippedZeroFbp", fbpCount == 0u ? 1u : 0u);
    if (fbpCount != 0u && !B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    for (i = 0; fbpCount != 0u && i < RTL_NUMBER_OF(nouveauRopWrites); ++i) {
        Wr(B, nouveauRopWrites[i][0], nouveauRopWrites[i][2]);
        if (!B3cPrivRingAfterGrWrite(B, nouveauRopWrites[i][0], nouveauRopWrites[i][2])) {
            result = 0xE0000095u; goto done;
        }
        nouveauRopWrites[i][3] = Rd(B, nouveauRopWrites[i][0]);
        ++nouveauRopWriteCount;
        if (nouveauRopWrites[i][3] != nouveauRopWrites[i][2]) {
            result = 0xE000009Du; goto done;
        }
    }
    Step(103);
    fifoCtlTrace[0] = Rd(B, 0x00400500u);
    fifoCtlTrace[1] = 0x00010001u;
    if (!B3cIsGrPriAddress(0x00400500u)) { result = 0xE000009Eu; goto done; }
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    Wr(B, 0x00400500u, fifoCtlTrace[1]);
    if (!B3cPrivRingAfterGrWrite(B, 0x00400500u, fifoCtlTrace[1])) {
        result = 0xE0000095u; goto done;
    }
    fifoCtlTrace[2] = Rd(B, 0x00400500u);
    if (fifoCtlTrace[2] != fifoCtlTrace[1]) { result = 0xE000009Eu; goto done; }
    /* nvgpu gk20a_init_gr_setup_hw() exception/interrupt section. Keep it
     * before the FECS bind/golden-generation sequence, as setup_hw precedes
     * gr_gk20a_init_golden_ctx_image(). MC/CPU interrupt outputs stay off. */
    Step(84);
    if (g_Verbose) B3cC2eCaptureBeforeExceptionSetup(B);
    preBundleFeEsr = Rd(B, 0x00404000u);
    TraceDetail(L"B3cC2ePreSetupFeEsr", preBundleFeEsr);
    if (preBundleFeEsr & 0x3FFFFFFFu) {
        result = 0xE000008Fu; goto done;
    }
    {
        ULONG setupAddr[20], setupValue[20], setupTrace[20][2];
        ULONG setupCount = 0, setupIndex;
        setupAddr[setupCount] = 0x00400100u; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x0040013Cu; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x00409C24u; setupValue[setupCount++] = 0x000F0002u;
        setupAddr[setupCount] = 0x00404000u; setupValue[setupCount++] = 0xC0000000u;
        setupAddr[setupCount] = 0x00404600u; setupValue[setupCount++] = 0xC0000000u;
        setupAddr[setupCount] = 0x00408030u; setupValue[setupCount++] = 0xC0000000u;
        setupAddr[setupCount] = 0x00404490u; setupValue[setupCount++] = 0xC0000000u;
        setupAddr[setupCount] = 0x00406018u; setupValue[setupCount++] = 0xC0000000u;
        setupAddr[setupCount] = 0x00407020u; setupValue[setupCount++] = 0x40000000u;
        setupAddr[setupCount] = 0x00405840u; setupValue[setupCount++] = 0xC0000000u;
        setupAddr[setupCount] = 0x00419E44u; setupValue[setupCount++] = 0x00DFFFFEu;
        setupAddr[setupCount] = 0x00419E4Cu; setupValue[setupCount++] = 0x00000005u;
        setupAddr[setupCount] = 0x00419D0Cu; setupValue[setupCount++] = 0x00000003u;
        setupAddr[setupCount] = 0x0041AC94u; setupValue[setupCount++] = 0x00020000u;
        setupAddr[setupCount] = 0x00400108u; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x00400138u; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x00400118u; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x00400130u; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x0040011Cu; setupValue[setupCount++] = 0xFFFFFFFFu;
        setupAddr[setupCount] = 0x00400134u; setupValue[setupCount++] = 0xFFFFFFFFu;
        for (setupIndex = 0; setupIndex < setupCount; ++setupIndex) {
            if ((setupAddr[setupIndex] & 3u) ||
                !B3cIsGrPriAddress(setupAddr[setupIndex])) {
                result = 0xE000008Eu; goto done;
            }
        }
        TraceDetail(L"B3cC2eExceptionSetupCount", setupCount);
        for (setupIndex = 0; setupIndex < setupCount; ++setupIndex) {
            setupTrace[setupIndex][0] = setupAddr[setupIndex];
            setupTrace[setupIndex][1] = setupValue[setupIndex];
        }
        TraceBin(L"B3cC2eExceptionSetupWrites", setupTrace,
                 setupCount * sizeof(setupTrace[0]));
        if (!B3cPrivRingDrainBeforeWrites(B)) {
            result = 0xE0000095u; goto done;
        }
        for (setupIndex = 0; setupIndex < setupCount; ++setupIndex) {
            Wr(B, setupAddr[setupIndex], setupValue[setupIndex]);
            if (!B3cPrivRingAfterGrWrite(B, setupAddr[setupIndex], setupValue[setupIndex])) {
                result = 0xE0000095u; goto done;
            }
        }
        TraceDetail(L"B3cC2eGrIntrAfterEnable", Rd(B, 0x00400100u));
        TraceDetail(L"B3cC2eGrIntrEnableReadback", Rd(B, 0x0040013Cu));
        TraceDetail(L"B3cC2eFecsHostIntStatusAfterEnable", Rd(B, 0x00409C18u));
        TraceDetail(L"B3cC2eFecsHostIntClearAfterEnable", Rd(B, 0x00409C20u));
        TraceDetail(L"B3cC2eFecsHostIntEnableReadback", Rd(B, 0x00409C24u));
        TraceDetail(L"B3cC2eExceptionAfterEnable", Rd(B, 0x00400108u));
        TraceDetail(L"B3cC2eException1AfterEnable", Rd(B, 0x00400118u));
        TraceDetail(L"B3cC2eException2AfterEnable", Rd(B, 0x0040011Cu));
        TraceDetail(L"B3cC2eFeHwwEsrAfterEnable", Rd(B, 0x00404000u));
        TraceDetail(L"B3cC2eMemfmtHwwEsrAfterEnable", Rd(B, 0x00404600u));
        TraceDetail(L"B3cC2eSccHwwEsrAfterEnable", Rd(B, 0x00408030u));
        TraceDetail(L"B3cC2eMmeHwwEsrAfterEnable", Rd(B, 0x00404490u));
        TraceDetail(L"B3cC2ePdHwwEsrAfterEnable", Rd(B, 0x00406018u));
        TraceDetail(L"B3cC2eSkedHwwEsrAfterEnable", Rd(B, 0x00407020u));
        TraceDetail(L"B3cC2eDsHwwEsrAfterEnable", Rd(B, 0x00405840u));
        TraceDetail(L"B3cC2eFeHwwInfoDecodeAvailable", 0u);
    }
    TraceDetail(L"B3cC2eBindMethod", 3u); TraceDetail(L"B3cC2eBindData", bindData);
    TraceDetail(L"B3cC2eBindMailboxClear", 0x30u);
    Wr(B, 0x00409840u, 0x30u);
    Wr(B, 0x00409500u, bindData);
    Wr(B, 0x00409504u, 3u);
    KeMemoryBarrier();
    for (polls = 0; polls < 2000u; ++polls) {
        mailbox = Rd(B, 0x00409800u);
        if (mailbox & 0x30u) break;
        Ms(1);
    }
    TraceDetail(L"B3cC2eBindMailbox", mailbox); Trace(L"B3cC2eBindPolls", polls);
    waitState[0] = 20u; waitState[1] = 0xFFFFFFFFu; waitState[2] = polls;
    waitState[3] = mailbox; waitState[7] = (polls < 2000u);
    if (polls == 2000u) { result = 0xE000008Bu; goto done; }
    if (mailbox & 0x20u) { result = 0xE000008Cu; goto done; }
    if (!(mailbox & 0x10u)) { result = 0xE000008Cu; goto done; }
    if (!B3cC2eWait(B, 30u, 0xFFFFFFFFu, FALSE, waitState, waitHistory, &waitHistoryCount, &waitHistoryDropped, waitHistoryCapacity)) {
        result = 0xE000008Bu; goto done;
    }

    /* nouveau ctxgf100.c gf100_grctx_generate() initializes the channel
     * image header before invoking gm20b_grctx_generate_main(). */
    *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x1Cu) = 1u;
    *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x20u) = 0u;
    *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x28u) = 0u;
    *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x2Cu) = 0u;
    TraceDetail(L"B3cC2eChannelHeader1C", *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x1Cu));
    TraceDetail(L"B3cC2eChannelHeader20", *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x20u));
    TraceDetail(L"B3cC2eChannelHeader28", *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x28u));
    TraceDetail(L"B3cC2eChannelHeader2C", *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0x2Cu));
    KeMemoryBarrier();

    /* Step 92: gm20b_ltc_init_fs_state() establishes active LTC/slice state
     * before ZBC and CBC setup. TOP_NUM_LTCS is the max count; the ringmaster
     * register's count field is the actual active LTC count. */
    Step(92);
    ltcCountRaw = Rd(B, 0x00022454u);
    ltcActiveCountRaw = Rd(B, 0x0012006Cu);
    ltcMaxCount = ltcCountRaw;
    ltcCountDetected = ltcActiveCountRaw & 0x1Fu;
    ltcParamRaw = Rd(B, 0x0017E280u);
    ltcSlicesPerLtc = (ltcParamRaw >> 28) & 0xFu;
    ltcCacheLineBytes = 512u << ((ltcParamRaw >> 24) & 0xFu);
    ltcTagsPerCacheLine = ltcParamRaw & 0xFFFFu;
    TraceDetail(L"B3cC2eTopNumLtcsRaw", ltcCountRaw);
    TraceDetail(L"B3cC2eLtcActiveCountRaw", ltcActiveCountRaw);
    TraceDetail(L"B3cC2eLtcMaxCount", ltcMaxCount);
    TraceDetail(L"B3cC2eLtcCount", ltcCountDetected);
    TraceDetail(L"B3cC2eLtcCbcParam", ltcParamRaw);
    TraceDetail(L"B3cC2eLtcSlicesPerLtc", ltcSlicesPerLtc);
    TraceDetail(L"B3cC2eLtcCacheLineBytes", ltcCacheLineBytes);
    TraceDetail(L"B3cC2eLtcComptagsPerCacheline", ltcTagsPerCacheLine);
    if (ltcCountDetected == 0u || ltcCountDetected > ltcMaxCount ||
        ltcSlicesPerLtc == 0u || ltcTagsPerCacheLine == 0u) {
        result = 0xE0000097u; goto done;
    }
    ltcFsWrites[0][0] = 0x0017E27Cu;
    ltcFsWrites[0][1] = Rd(B, ltcFsWrites[0][0]);
    ltcFsWrites[0][2] = ltcCountDetected;
    ltcFsWrites[1][0] = 0x0017E000u;
    ltcFsWrites[1][1] = Rd(B, ltcFsWrites[1][0]);
    ltcFsWrites[1][2] = ltcCountDetected;
    ltcFsWrites[2][0] = 0x0017E318u;
    ltcFsWrites[2][1] = Rd(B, 0x00140518u);
    ltcFsWrites[2][2] = ltcFsWrites[2][1] | (1u << 15);
    ltcFsWrites[3][0] = 0x0017E20Cu;
    ltcFsWrites[3][1] = Rd(B, ltcFsWrites[3][0]);
    ltcFsWrites[3][2] = ltcFsWrites[3][1] & ~((1u << 20) | (1u << 21) | (1u << 30));
    for (i = 0; i < RTL_NUMBER_OF(ltcFsWrites); ++i) {
        if ((ltcFsWrites[i][0] & 3u) || ltcFsWrites[i][0] < 0x00140000u ||
            ltcFsWrites[i][0] >= 0x00180000u) {
            result = 0xE0000097u; goto done;
        }
    }
    for (i = 0; i < RTL_NUMBER_OF(ltcFsWrites); ++i) {
        Wr(B, ltcFsWrites[i][0], ltcFsWrites[i][2]);
        ltcFsWrites[i][3] = Rd(B, ltcFsWrites[i][0]);
        ++ltcFsWriteCount;
        if (ltcFsWrites[i][3] != ltcFsWrites[i][2]) {
            result = 0xE0000097u; goto done;
        }
    }
    TraceBin(L"B3cC2eLtcFsReadback", ltcFsWrites, sizeof(ltcFsWrites));
    TraceDetail(L"B3cC2eLtcFsWriteCount", ltcFsWriteCount);
    ltcFsConfigured = 1u;

    /* Step 93: fb_gm20b_init_fs_state() publishes the active LTC count to
     * the FB hub. The physical-secure bypass is non-secure-boot only. */
    Step(93);
    fbFsWrites[0][0] = 0x00100800u;
    fbFsWrites[0][1] = Rd(B, fbFsWrites[0][0]);
    fbFsWrites[0][2] = ltcCountDetected;
    Wr(B, fbFsWrites[0][0], fbFsWrites[0][2]);
    fbFsWrites[0][3] = Rd(B, fbFsWrites[0][0]);
    ++fbFsWriteCount;
    TraceBin(L"B3cC2eFbFsReadback", fbFsWrites, sizeof(fbFsWrites));
    TraceDetail(L"B3cC2eFbFsWriteCount", fbFsWriteCount);
    if (fbFsWrites[0][3] != fbFsWrites[0][2]) {
        result = 0xE0000098u; goto done;
    }
    fbFsConfigured = 1u;

    /* Step 94: nvgpu setup_hw loads the ZBC tables before CBC and sw_ctx.
     * Recreate the GM20B default color/depth entries using its exact L2 DSTG
     * and GR DS register sequence. Entry zero is reserved by GK20A. */
    Step(94);
    zbcIdleOk = B3cC2eWaitNouveauGrIdle(B, 0x5A424349u, 0xFFFFFFFFu, zbcIdleState);
    Trace(L"B3cC2eZbcIdlePolls", zbcIdleState[2]);
    TraceDetail(L"B3cC2eZbcIdleOk", zbcIdleOk);
    if (!zbcIdleOk) { result = 0xE0000099u; goto done; }
    zbcWriteCount = 0u;
#define B3C_C2E_ZBC_ADD(a, v) do { \
        if (zbcWriteCount >= RTL_NUMBER_OF(zbcWrites)) { result = 0xE0000099u; goto done; } \
        zbcWrites[zbcWriteCount][0] = (a); \
        zbcWrites[zbcWriteCount][1] = (v); \
        zbcWrites[zbcWriteCount][2] = Rd(B, (a)); \
        ++zbcWriteCount; \
    } while (0)
    for (zbcColorIndex = 0; zbcColorIndex < 3u; ++zbcColorIndex) {
        zbcLtcIndex = zbcColorIndex + 1u; /* GK20A_STARTOF_ZBC_TABLE */
        B3C_C2E_ZBC_ADD(0x0017E338u, zbcLtcIndex);
        for (zbcChanIndex = 0; zbcChanIndex < 4u; ++zbcChanIndex)
            B3C_C2E_ZBC_ADD(0x0017E33Cu + zbcChanIndex * 4u,
                            zbcColorL2[zbcColorIndex][zbcChanIndex]);
        B3C_C2E_ZBC_ADD(0x00405804u, zbcColorDs[zbcColorIndex][0]);
        B3C_C2E_ZBC_ADD(0x00405808u, zbcColorDs[zbcColorIndex][1]);
        B3C_C2E_ZBC_ADD(0x0040580Cu, zbcColorDs[zbcColorIndex][2]);
        B3C_C2E_ZBC_ADD(0x00405810u, zbcColorDs[zbcColorIndex][3]);
        B3C_C2E_ZBC_ADD(0x00405814u, zbcColorFmt[zbcColorIndex]);
        B3C_C2E_ZBC_ADD(0x00405820u, zbcLtcIndex);
        B3C_C2E_ZBC_ADD(0x00405824u, 0x4u); /* C select | write | trigger */
    }
    for (zbcColorIndex = 0; zbcColorIndex < 2u; ++zbcColorIndex) {
        zbcLtcIndex = zbcColorIndex + 1u; /* independent depth table index */
        B3C_C2E_ZBC_ADD(0x0017E338u, zbcLtcIndex);
        B3C_C2E_ZBC_ADD(0x0017E34Cu, zbcDepth[zbcColorIndex]);
        B3C_C2E_ZBC_ADD(0x00405818u, zbcDepth[zbcColorIndex]);
        B3C_C2E_ZBC_ADD(0x0040581Cu, 1u); /* FP32 depth format */
        B3C_C2E_ZBC_ADD(0x00405820u, zbcLtcIndex);
        B3C_C2E_ZBC_ADD(0x00405824u, 0x5u); /* Z select | write | trigger */
    }
#undef B3C_C2E_ZBC_ADD
    if (zbcWriteCount != RTL_NUMBER_OF(zbcWrites)) {
        result = 0xE0000099u; goto done;
    }
    for (i = 0; i < zbcWriteCount; ++i) {
        zbcAddress = zbcWrites[i][0];
        if ((zbcAddress & 3u) ||
            !((zbcAddress >= 0x00140000u && zbcAddress < 0x00180000u) ||
              B3cIsGrPriAddress(zbcAddress))) {
            result = 0xE0000099u; goto done;
        }
    }
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    for (i = 0; i < zbcWriteCount; ++i) {
        zbcAddress = zbcWrites[i][0];
        zbcValue = zbcWrites[i][1];
        Wr(B, zbcAddress, zbcValue);
        if (B3cIsGrPriAddress(zbcAddress) &&
            !B3cPrivRingAfterGrWrite(B, zbcAddress, zbcValue)) {
            result = 0xE0000095u; goto done;
        }
        zbcWrites[i][3] = Rd(B, zbcAddress);
        if (zbcAddress != 0x00405824u && zbcWrites[i][3] != zbcValue) {
            result = 0xE0000099u; goto done;
        }
    }
    TraceBin(L"B3cC2eZbcReadback", zbcWrites, sizeof(zbcWrites));
    TraceDetail(L"B3cC2eZbcWriteCount", zbcWriteCount);
    TraceDetail(L"B3cC2eZbcPmuSyncSkipped", 1u); /* ELPG ZBC-save path is not enabled here. */
    zbcConfigured = 1u;

    /* Step 95: nvgpu's comptag path accepts max_comptag_mem == 0: init_comptags
     * returns without allocation and cbc_ctrl is a no-op for zero backing.
     * Select this bounded no-comptag mode; do not invent a CBC buffer size. */
    Step(95);
    cbcNoComptag = 1u;
    cbcWrites[0][0] = 0x0017E278u;
    cbcWrites[0][1] = Rd(B, cbcWrites[0][0]);
    cbcWrites[0][2] = 0u; /* gm20b_ltc_init_cbc() base for zero-sized store */
    Wr(B, cbcWrites[0][0], cbcWrites[0][2]);
    cbcWrites[0][3] = Rd(B, cbcWrites[0][0]);
    ++cbcWriteCount;
    TraceBin(L"B3cC2eCbcReadback", cbcWrites, sizeof(cbcWrites));
    TraceDetail(L"B3cC2eCbcWriteCount", cbcWriteCount);
    TraceDetail(L"B3cC2eCbcNoComptag", cbcNoComptag);
    TraceDetail(L"B3cC2eCbcBackingBytes", 0u);
    if (cbcWrites[0][3] != cbcWrites[0][2]) { result = 0xE000009Au; goto done; }
    cbcConfigured = 1u;

    /* Step 96: gm20a_gr_disable_rd_coalesce(), called by setup_hw before
     * sw_ctx_load, clears LG and SU read-coalesce bits in TEX_M_DBG2. */
    Step(96);
    rdCoalesceBefore = Rd(B, 0x00419A3Cu);
    Wr(B, 0x00419A3Cu, rdCoalesceBefore & ~rdCoalesceMask);
    if (!B3cPrivRingAfterGrWrite(B, 0x00419A3Cu,
                                 rdCoalesceBefore & ~rdCoalesceMask)) {
        result = 0xE0000095u; goto done;
    }
    rdCoalesceAfter = Rd(B, 0x00419A3Cu);
    TraceDetail(L"B3cC2eRdCoalesceBefore", rdCoalesceBefore);
    TraceDetail(L"B3cC2eRdCoalesceMask", rdCoalesceMask);
    TraceDetail(L"B3cC2eRdCoalesceAfter", rdCoalesceAfter);
    if (rdCoalesceAfter & rdCoalesceMask) { result = 0xE000009Bu; goto done; }

    /* nvgpu gr_gk20a_init_golden_ctx_image() binds FECS, waits idle, then
     * writes sw_ctx_load before committing global buffers and bundle init. */
    result = B3cApplyGrCtxList(Dev, TRUE);
    if (result != 0u) goto done;
    /* Nouveau waits idle after sw_ctx while the production FE timeout is
     * still active; only then does gm20b_grctx_generate_main() save/disable it. */
    Step(88);
    trueIdleAfterSwCtxOk = B3cC2eWaitNouveauGrIdle(B, 0x53574358u, 0xFFFFFFFFu, trueIdleAfterSwCtx);
    if (!trueIdleAfterSwCtxOk) { result = 0xE0000092u; goto done; }
    feGoIdleTimeoutSaved = Rd(B, 0x00404154u);
    feGoIdleTimeoutSavedValid = 1u;
    TraceDetail(L"B3cC2eFeGoIdleTimeoutSaved", feGoIdleTimeoutSaved);
    Wr(B, 0x00404154u, 0u);
    TraceDetail(L"B3cC2eFeGoIdleTimeoutDisabled", Rd(B, 0x00404154u));

    /* GM20B global backing commits and enabled timeslice state. */
    TraceBin(L"B3cC2eGlobalWrites", globalWrites, sizeof(globalWrites));
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    for (i = 0; i < RTL_NUMBER_OF(globalWrites); ++i) {
        Wr(B, globalWrites[i].Address, globalWrites[i].Value);
        if (!B3cPrivRingAfterGrWrite(B, globalWrites[i].Address, globalWrites[i].Value)) {
            result = 0xE0000095u; goto done;
        }
        ++written;
        if (i < RTL_NUMBER_OF(sample)) {
            sample[i][0] = globalWrites[i].Address;
            sample[i][1] = globalWrites[i].Value;
            sample[i][2] = 0;
            sampleCount = i + 1u;
        }
    }
    timesliceTrace[0][0] = 0x00418C6Cu; timesliceTrace[0][2] = 0x1u;
    timesliceTrace[1][0] = 0x0041980Cu; timesliceTrace[1][2] = 0x10u;
    timesliceTrace[2][0] = 0x0041BE08u; timesliceTrace[2][2] = 0x4u;
    timesliceTrace[3][0] = 0x004064C0u; timesliceTrace[3][2] = 0x80000000u;
    timesliceTrace[4][0] = 0x00405800u; timesliceTrace[4][2] = 0x08000000u;
    timesliceTrace[5][0] = 0x00419C00u; timesliceTrace[5][2] = 0x8u;
    for (i = 0; i < RTL_NUMBER_OF(timesliceTrace); ++i) {
        timesliceTrace[i][1] = Rd(B, timesliceTrace[i][0]);
        timesliceTrace[i][3] = timesliceTrace[i][1] | timesliceTrace[i][2];
        Wr(B, timesliceTrace[i][0], timesliceTrace[i][3]);
        if (!B3cPrivRingAfterGrWrite(B, timesliceTrace[i][0], timesliceTrace[i][3])) {
            result = 0xE0000095u; goto done;
        }
    }
    TraceBin(L"B3cC2eTimesliceWrites", timesliceTrace, sizeof(timesliceTrace));
    TraceDetail(L"B3cC2eGlobalWriteCount", written);
    for (i = 0; i < sampleCount; ++i) sample[i][2] = Rd(B, sample[i][0]);
    TraceBin(L"B3cC2eGlobalReadback", sample, sampleCount * sizeof(sample[0]));
    for (i = 0; i < sampleCount; ++i) {
        if (sample[i][1] != sample[i][2]) { result = 0xE000008Cu; goto done; }
    }

    /* Step 83: single-GPC/two-TPC GM20B SM/TPC and ZCULL state. Along with
     * the existing floorsweep writes, ctxgm20b.c's gm20b_grctx_generate_main()
     * uses 4064d0..4ec, 405b00, 408908 and 4041c4 before its idle boundary. */
    Step(83);
    {
        ULONG fsAddr[64], fsValue[64], fsCount = 0;
        fsAddr[fsCount] = 0x00406028u; fsValue[fsCount++] = 2u;
        fsAddr[fsCount] = 0x00405870u; fsValue[fsCount++] = 2u;
        fsAddr[fsCount] = 0x004041C4u; fsValue[fsCount++] = 3u;
        fsAddr[fsCount] = 0x00405B00u; fsValue[fsCount++] = 0x0201u;
        fsAddr[fsCount] = 0x00405B60u; fsValue[fsCount++] = 0x00000100u;
        /* Single-GPC/two-TPC mapping setup from the nvgpu GM20B/GK20A
         * floorsweep path: row offset 1, two entries, all map to GPC0. */
        fsAddr[fsCount] = 0x00418BB8u; fsValue[fsCount++] = 0x00000201u;
        fsAddr[fsCount] = 0x0041BFD0u; fsValue[fsCount++] = 0x00700201u;
        fsAddr[fsCount] = 0x0041BFE4u; fsValue[fsCount++] = 0u;
        fsAddr[fsCount] = 0x004078BCu; fsValue[fsCount++] = 0x00000201u;
        for (i = 0; i < 6u; ++i) {
            fsAddr[fsCount] = 0x00418B08u + i * 4u; fsValue[fsCount++] = 0u;
            fsAddr[fsCount] = 0x0041BF00u + i * 4u; fsValue[fsCount++] = 0u;
            fsAddr[fsCount] = 0x0040780Cu + i * 4u; fsValue[fsCount++] = 0u;
        }
        for (i = 0; i < 8u; ++i) {
            fsAddr[fsCount] = 0x004064D0u + i * 4u; fsValue[fsCount++] = 0u;
        }
        for (i = 0; i < 2u; ++i) {
            fsAddr[fsCount] = 0x00504698u + i * 0x800u; fsValue[fsCount++] = i;
            fsAddr[fsCount] = 0x00500C10u + i * 4u; fsValue[fsCount++] = i;
            fsAddr[fsCount] = 0x00504088u + i * 0x800u; fsValue[fsCount++] = i;
        }
        for (i = 0; i < 6u; ++i) {
            fsAddr[fsCount] = 0x00405BA0u + i * 4u;
            fsValue[fsCount++] = (i == 0u) ? 0x100u : 0u;
        }
        /* Nvgpu uses TOP_NUM_LTCS as max_ltc_count and the priv-ring
         * enumeration result as active ltc_count; the latter is 0x12006C. */
        ltcCountRaw = Rd(B, 0x00022454u);
        ltcActiveCountRaw = Rd(B, 0x0012006Cu);
        ltcMaxCount = ltcCountRaw;
        ltcCountDetected = ltcActiveCountRaw & 0x1Fu;
        TraceDetail(L"B3cC2eTopNumLtcsRaw", ltcCountRaw);
        TraceDetail(L"B3cC2eLtcActiveCountRaw", ltcActiveCountRaw);
        TraceDetail(L"B3cC2eLtcMaxCount", ltcMaxCount);
        TraceDetail(L"B3cC2eLtcCount", ltcCountDetected);
        for (i = 0; i < fsCount; ++i) {
            if ((fsAddr[i] & 3u) || !B3cIsGrPriAddress(fsAddr[i])) {
                result = 0xE000008Eu; goto done;
            }
        }
        for (i = 0; i < fsCount; ++i) {
            Wr(B, fsAddr[i], fsValue[i]); ++fsWrites;
            if (!B3cPrivRingAfterGrWrite(B, fsAddr[i], fsValue[i])) {
                result = 0xE0000095u; goto done;
            }
            if (i < RTL_NUMBER_OF(fsSample)) {
                fsSample[i][0] = fsAddr[i]; fsSample[i][1] = fsValue[i];
                fsSample[i][2] = Rd(B, fsAddr[i]); fsSampleCount = i + 1u;
            }
        }
        TraceDetail(L"B3cC2eFsWriteCount", fsWrites);
        TraceDetail(L"B3cC2eFsSampleCount", fsSampleCount);
        TraceBin(L"B3cC2eFsReadback", fsSample, fsSampleCount * sizeof(fsSample[0]));
        for (i = 0; i < fsSampleCount; ++i) {
            if (fsSample[i][1] != fsSample[i][2]) { result = 0xE000008Eu; goto done; }
        }
        TraceDetail(L"B3cC2ePreemptionInitPresent", 0u); /* GM20B ops have no init_preemption_state hook here. */
    }
    /* Step 97: GM20B gr_gm20b_init_fs_state() BE tail. The source writes
     * zrop/crop active-LTC counts, then ORs comp_vdc_4to2_disable into the
     * current BE0 crop debug value. Keep this as its own bounded/readback
     * checked point. */
    Step(97);
    if (ltcCountDetected == 0u || ltcCountDetected > 0xFu) {
        result = 0xE0000096u; goto done;
    }
    ropWrites[0][0] = 0x00408850u;
    ropWrites[0][1] = Rd(B, ropWrites[0][0]);
    ropWrites[0][2] = ltcCountDetected;
    ropWrites[1][0] = 0x00408958u;
    ropWrites[1][1] = Rd(B, ropWrites[1][0]);
    ropWrites[1][2] = ltcCountDetected;
    ropWrites[2][0] = 0x00408908u;
    ropWrites[2][1] = Rd(B, 0x00410108u);
    ropWrites[2][2] = ropWrites[2][1] | 0x80000000u;
    for (i = 0; i < RTL_NUMBER_OF(ropWrites); ++i) {
        if ((ropWrites[i][0] & 3u) || !B3cIsGrPriAddress(ropWrites[i][0])) {
            result = 0xE0000096u; goto done;
        }
    }
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    for (i = 0; i < RTL_NUMBER_OF(ropWrites); ++i) {
        Wr(B, ropWrites[i][0], ropWrites[i][2]);
        if (!B3cPrivRingAfterGrWrite(B, ropWrites[i][0], ropWrites[i][2])) {
            result = 0xE0000095u; goto done;
        }
        ropWrites[i][3] = Rd(B, ropWrites[i][0]);
        ++ropWriteCount;
        if (ropWrites[i][3] != ropWrites[i][2]) {
            result = 0xE0000096u; goto done;
        }
    }
    TraceBin(L"B3cC2eRopSetupReadback", ropWrites, sizeof(ropWrites));
    TraceDetail(L"B3cC2eRopSetupWriteCount", ropWriteCount);
    /* Nouveau gk104 uses FBP count (0x120074) in the low nibble; GM20B
     * nvgpu gr_gm20b_init_fs_state() separately writes active LTC count. */
    beRopConfigured = 1u;
    TraceDetail(L"B3cC2eLtcCbcSetupSkipped", cbcConfigured ? 0u : 1u);
    TraceDetail(L"B3cC2eZbcSetupSkipped", zbcConfigured ? 0u : 1u);
    TraceDetail(L"B3cC2eLtcBasedRopSetupSkipped", (ltcFsConfigured && fbFsConfigured && beRopConfigured) ? 0u : 1u);
    if (!B3cC2eWait(B, 35u, 0xFFFFFFFFu, FALSE, waitState, waitHistory, &waitHistoryCount, &waitHistoryDropped, waitHistoryCapacity)) {
        result = 0xE000008Bu; goto done;
    }
    trueIdleAfterSetupOk = B3cC2eWaitNouveauGrIdle(B, 0x53455455u, 0xFFFFFFFFu, trueIdleAfterSetup);
    TraceDetail(L"B3cC2eNouveauIdleAfterSetupOk", trueIdleAfterSetupOk);
    if (!trueIdleAfterSetupOk) { result = 0xE0000092u; goto done; }

    bundle = ReadBinParam(L"FwGrSwBundleInit", 0x20000u, &bundleBytes);
    TraceDetail(L"B3cC2eBundleBytes", bundleBytes);
    if (bundle == NULL || bundleBytes == 0u || (bundleBytes % 8u) != 0u) {
        result = 0xE000008Cu; goto done;
    }
    bundleCount = bundleBytes / 8u;
    /* GM20B gm20b_grctx_generate_main() restores the saved FE_GO_IDLE
     * timeout before method-init and bundle-init replay. */
    if (feGoIdleTimeoutSavedValid) {
        Wr(B, 0x00404154u, feGoIdleTimeoutSaved);
        KeMemoryBarrier();
    }
    TraceDetail(L"B3cC2eFeGoIdleTimeoutRestoredBeforeBundle", Rd(B, 0x00404154u));
    trueIdleAfterRestoreOk = B3cC2eWaitNouveauGrIdle(B, 0x52535452u, 0xFFFFFFFFu, trueIdleAfterRestore);
    TraceDetail(L"B3cC2eNouveauIdleAfterTimeoutRestoreOk", trueIdleAfterRestoreOk);
    if (!trueIdleAfterRestoreOk) { result = 0xE0000092u; goto done; }
    /* GM20B has no sw_method_init.bin; Nouveau's method-init pack is empty.
     * Preserve its following idle boundary before gf100_gr_icmd(). */
    TraceDetail(L"B3cC2eMethodInitCount", 0u);
    trueIdleAfterMethodInitOk = B3cC2eWaitNouveauGrIdle(B, 0x4D544844u, 0xFFFFFFFFu, trueIdleAfterMethodInit);
    TraceDetail(L"B3cC2eNouveauIdleAfterMethodInitOk", trueIdleAfterMethodInitOk);
    if (!trueIdleAfterMethodInitOk) { result = 0xE0000092u; goto done; }
    Wr(B, 0x00400208u, 0x80000000u); /* pipe mode override */
    if (g_Verbose) B3cC2eCapturePreBundle(B);
    preBundleFeEsr = Rd(B, 0x00404000u);
    TraceDetail(L"B3cC2ePreBundleFeEsr", preBundleFeEsr);
    if (preBundleFeEsr & 0x3FFFFFFFu) {
        result = 0xE000008Fu; goto done;
    }
    Step(90);
    B3cCaptureGpcPriDiag(B, L"B3cC2ePreBundleGpcPri", L"B3cC2ePreBundleGpcPriBadfCount",
                         &preBundleGpcPriBadfCount);
    Step(91);
    if (g_Verbose) B3cCaptureGrStatusDump(B, L"B3cC2eGrStatusDumpBeforeBundle", grDumpBefore);
    grDumpBeforeCaptured = 1u;
    bundleBaseline[0] = Rd(B, 0x00404000u);
    bundleBaseline[1] = Rd(B, 0x004041B0u);
    bundleBaseline[2] = Rd(B, 0x00400100u);
    bundleBaseline[3] = Rd(B, 0x00400108u);
    bundleBaseline[4] = Rd(B, 0x00400700u);
    TraceBin(L"B3cC2eBundleBaseline", bundleBaseline, sizeof(bundleBaseline));
    for (i = 0; i < bundleBytes / 8u; ++i) {
        bundleMethodId = Le32(bundle + i * 8u);
        value = Le32(bundle + i * 8u + 4u);
        if (i == 0u || value != lastData) {
            Wr(B, 0x00400204u, value);
            lastData = value;
        }
        Wr(B, 0x00400200u, bundleMethodId);
        /* Snapshot the FE/GR exception state immediately after each posted
         * method, before any bounded idle wait can obscure the first fault. */
        bundleFeEsr = Rd(B, 0x00404000u);
        bundleFeInfo = Rd(B, 0x004041B0u);
        bundleGrIntr = Rd(B, 0x00400100u);
        bundleGrException = Rd(B, 0x00400108u);
        bundleGrStatus = Rd(B, 0x00400700u);
        if (i == 0u) {
            bundleEntry0[0] = i;
            bundleEntry0[1] = bundleMethodId;
            bundleEntry0[2] = value;
            bundleEntry0[3] = bundleFeEsr;
            bundleEntry0[4] = bundleFeInfo;
            bundleEntry0[5] = bundleGrIntr;
            bundleEntry0[6] = bundleGrException;
            bundleEntry0[7] = bundleGrStatus;
            TraceBin(L"B3cC2eBundleEntry0State", bundleEntry0, sizeof(bundleEntry0));
            if (g_Verbose) B3cCaptureGrStatusDump(B, L"B3cC2eGrStatusDumpAfterEntry0", grDumpEntry0);
            grDumpEntry0Captured = 1u;
        }
        if (bundleFirstChangeFound == 0u &&
            (bundleFeEsr != bundleBaseline[0] ||
             bundleFeInfo != bundleBaseline[1] ||
             bundleGrIntr != bundleBaseline[2] ||
             bundleGrException != bundleBaseline[3]))  /* Claude: GR status is recorded but not a stop condition; GR is always busy after entry 0, so including it stopped at entry 0 every time. */ {
            bundleFirstChangeFound = 1u;
            bundleFirstChangeIndex = i;
            bundleFirstChange[0] = i;
            bundleFirstChange[1] = bundleMethodId;
            bundleFirstChange[2] = value;
            bundleFirstChange[3] = bundleFeEsr;
            bundleFirstChange[4] = bundleFeInfo;
            bundleFirstChange[5] = bundleGrIntr;
            bundleFirstChange[6] = bundleGrException;
            bundleFirstChange[7] = bundleGrStatus;
            TraceErrorBin(L"B3cC2eBundleFirstChangeState", bundleFirstChange,
                     sizeof(bundleFirstChange));
            TraceDetail(L"B3cC2eBundleFirstChangeIndex", bundleFirstChangeIndex);
            TraceDetail(L"B3cC2eBundleFirstChangeMethod", bundleMethodId);
            TraceDetail(L"B3cC2eBundleFirstChangeData", value);
            TraceDetail(L"B3cC2eBundleFirstChangeFeEsr", bundleFeEsr);
            TraceDetail(L"B3cC2eBundleFirstChangeFeInfo", bundleFeInfo);
            TraceDetail(L"B3cC2eBundleFirstChangeGrIntr", bundleGrIntr);
            TraceDetail(L"B3cC2eBundleFirstChangeGrException", bundleGrException);
            TraceDetail(L"B3cC2eBundleFirstChangeGrStatus", bundleGrStatus);
            result = 0xE0000094u;
            B3cCaptureGrStatusDump(B, L"B3cC2eGrStatusDumpAtStop", grDumpStop);
            grDumpStopCaptured = 1u;
            goto done;
        }
        if (firstBusyIndex == 0xFFFFFFFFu) {
            firstBusyEngineStatus = Rd(B, 0x0040060Cu);
            if (firstBusyEngineStatus & 1u) {
                firstBusyIndex = i;
                firstBusyMethod = bundleMethodId;
                firstBusyGrStatus = Rd(B, 0x00400700u);
                firstBusyFifoStatus = Rd(B, 0x00002640u);
            }
        }
        if ((bundleMethodId & 0xFFFFu) == 0xE100u) {
            Step(87);
            trueIdleGoBundleOk = B3cC2eWaitNouveauGrIdle(B, 0xE100u, i, trueIdleGoBundle);
        }
        /* Nouveau gf100_gr_icmd() submits the method first, waits for GR
         * after GO_IDLE, then waits for FE method-lower to clear. */
        if (!B3cC2eWait(B, 50u, i, TRUE, waitState, waitHistory, &waitHistoryCount, &waitHistoryDropped, waitHistoryCapacity)) {
            result = 0xE000008Bu; goto done;
        }
    }
    Step(85);
    Wr(B, 0x00400208u, 0u); /* nvgpu disables pipe mode override first */
    TraceDetail(L"B3cC2ePipeOverrideAfterBundle", Rd(B, 0x00400208u));
    if (!B3cC2eWait(B, 60u, bundleCount, FALSE, waitState, waitHistory, &waitHistoryCount, &waitHistoryDropped, waitHistoryCapacity)) {
        if (feGoIdleTimeoutSavedValid) Wr(B, 0x00404154u, feGoIdleTimeoutSaved);
        result = 0xE000008Bu; goto done;
    }
    if (feGoIdleTimeoutSavedValid) Wr(B, 0x00404154u, feGoIdleTimeoutSaved);
    TraceDetail(L"B3cC2eFeGoIdleTimeoutRestored", Rd(B, 0x00404154u));
    Trace(L"B3cC2eFinalIdlePolls", waitState[2]);
    TraceDetail(L"B3cC2eFinalGrStatus", waitState[3]);
    TraceDetail(L"B3cC2eFinalEngineStatus", waitState[4]);
    TraceDetail(L"B3cC2eBundleEntries", bundleBytes / 8u);
    TraceDetail(L"B3cC2eBundleLastAddress", bundleMethodId);
    TraceDetail(L"B3cC2eBundleLastValue", value);
    TraceDetail(L"B3cC2eBundleFirstChangeFound", bundleFirstChangeFound);
    TraceDetail(L"B3cC2eBundleFirstChangeIndex", bundleFirstChangeIndex);
    TraceBin(L"B3cC2eBundleEntry0State", bundleEntry0, sizeof(bundleEntry0));
    TraceDetail(L"B3cC2eMethodInitCount", 0u); /* GM20B linux-firmware has no sw_method_init.bin */
    if (!B3cC2eWait(B, 61u, bundleCount, FALSE, waitState, waitHistory, &waitHistoryCount, &waitHistoryDropped, waitHistoryCapacity)) {
        result = 0xE000008Bu; goto done;
    }
    if (!B3cC2eL2Flush(B, l2FlushState)) {
        Trace(L"B3cC2eL2FlushPolls", l2FlushState[0]);
        TraceDetail(L"B3cC2eL2FlushStatus", l2FlushState[1]);
        Trace(L"B3cC2eL2InvalidatePolls", l2FlushState[2]);
        TraceDetail(L"B3cC2eL2InvalidateStatus", l2FlushState[3]);
        result = 0xE0000090u; goto done;
    }
    Trace(L"B3cC2eL2FlushPolls", l2FlushState[0]);
    TraceDetail(L"B3cC2eL2FlushStatus", l2FlushState[1]);
    Trace(L"B3cC2eL2InvalidatePolls", l2FlushState[2]);
    TraceDetail(L"B3cC2eL2InvalidateStatus", l2FlushState[3]);

    /* nvgpu flushes/invalidates L2, copies the FECS header from the channel
     * context, sets zcull no-ctxsw and ptr=0, then commits GOLDEN_CTX_VA. */
    RtlCopyMemory(goldenHdr,
        (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF, sizeof(goldenHdr));
    RtlCopyMemory((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF,
        goldenHdr, sizeof(goldenHdr));
    *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0x1Cu) = 1u;
    *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0x20u) = 0u;
    instWords[132] = (GR_VM_GPU_VA + GR_VM_GLOBAL_OFF) | 4u;
    instWords[133] = 0u;
    KeMemoryBarrier();
    TraceDetail(L"B3cC2eGoldenCommitVa", GR_VM_GPU_VA + GR_VM_GLOBAL_OFF);
    TraceDetail(L"B3cC2eWfiPtrDuringSave", instWords[132]);
    /* Snapshot immediately before submitting FECS golden-save method 9. */
    preSaveGrStatus = Rd(B, 0x00400700u);
    preSaveGrStatus1 = Rd(B, 0x00400604u);
    preSaveGrStatus2 = Rd(B, 0x00400608u);
    preSaveGrEngineStatus = Rd(B, 0x0040060Cu);
    preSaveFifoEngineStatus = Rd(B, 0x00002640u);
    TraceDetail(L"B3cC2ePreSaveGrStatus", preSaveGrStatus);
    TraceDetail(L"B3cC2ePreSaveGrStatus1", preSaveGrStatus1);
    TraceDetail(L"B3cC2ePreSaveGrStatus2", preSaveGrStatus2);
    TraceDetail(L"B3cC2ePreSaveGrEngineStatus", preSaveGrEngineStatus);
    TraceDetail(L"B3cC2ePreSaveFifoEngineStatus", preSaveFifoEngineStatus);
    TraceDetail(L"B3cC2ePreSaveGrBusyBit", preSaveGrEngineStatus & 1u);
    TraceDetail(L"B3cC2ePreSaveGrStatusBit0", preSaveGrStatus & 1u);
    TraceDetail(L"B3cC2ePreSaveGrStatusBit5", (preSaveGrStatus >> 5) & 1u);
    TraceDetail(L"B3cC2ePreSaveGrStatusBit7", (preSaveGrStatus >> 7) & 1u);
    TraceDetail(L"B3cC2ePreSaveGrStatusFeGi", (preSaveGrStatus >> 21) & 1u);
    TraceDetail(L"B3cC2ePreSaveGrActivity0", Rd(B, 0x00400380u));
    TraceDetail(L"B3cC2ePreSaveGrActivity1", Rd(B, 0x00400384u));
    TraceDetail(L"B3cC2ePreSaveGrActivity2", Rd(B, 0x00400388u));
    TraceDetail(L"B3cC2ePreSaveGrActivity3Present", 0u);
    TraceDetail(L"B3cC2ePreSaveGrActivity4", Rd(B, 0x00400390u));
    Step(87);
    trueIdlePreSaveOk = B3cC2eWaitNouveauGrIdle(B, 0x53415645u, bundleCount, trueIdlePreSave); /* 'SAVE' */
    if (!trueIdlePreSaveOk) { result = 0xE0000091u; goto done; }
    TraceDetail(L"B3cC2eSaveMethod", 9u); TraceDetail(L"B3cC2eSaveData", saveData);
    TraceDetail(L"B3cC2eSaveMailboxClear", 3u);
    Wr(B, 0x00409840u, 3u);
    Wr(B, 0x00409500u, saveData);
    Wr(B, 0x00409504u, 9u);
    KeMemoryBarrier();
    for (polls = 0; polls < 2000u; ++polls) {
        mailbox = Rd(B, 0x00409800u);
        if (mailbox & 3u) break;
        Ms(1);
    }
    TraceDetail(L"B3cC2eSaveMailbox", mailbox); Trace(L"B3cC2eSavePolls", polls);
    waitState[0] = 70u; waitState[1] = bundleCount; waitState[2] = polls;
    waitState[3] = mailbox; waitState[7] = (polls < 2000u);
    if (polls == 2000u) { result = 0xE000008Bu; goto done; }
    if (mailbox & 2u) { result = 0xE000008Cu; goto done; }
    if (!(mailbox & 1u)) { result = 0xE000008Cu; goto done; }

    goldenSamples[0][0] = 0x00u;
    goldenSamples[0][1] = *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0x00u);
    goldenSamples[1][0] = 0x1Cu; goldenSamples[1][1] = *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0x1Cu);
    goldenSamples[2][0] = 0x20u; goldenSamples[2][1] = *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0x20u);
    goldenSamples[3][0] = 0xFCu; goldenSamples[3][1] = *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0xFCu);
    goldenSamples[4][0] = 0x100u; goldenSamples[4][1] = *(ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF + 0x100u);
    TraceDetail(L"B3cC2eGoldenMagic", goldenSamples[3][1]);
    TraceBin(L"B3cC2eGoldenSamples", goldenSamples, sizeof(goldenSamples));
    if (goldenSamples[3][1] != 0x600DC0DEu) { result = 0xE000008Cu; goto done; }

    instWords[132] = 0u;
    instWords[133] = 0u;
    KeMemoryBarrier();
    TraceDetail(L"B3cC2eWfiPtrAfterSaveLo", instWords[132]);
    TraceDetail(L"B3cC2eWfiPtrAfterSaveHi", instWords[133]);
    mailbox = Rd(B, 0x00409B00u) & ~0x80000000u;
    Wr(B, 0x00409B00u, mailbox); /* Nouveau clears FECS current-context valid */
    KeMemoryBarrier();
    TraceDetail(L"B3cC2eFecsCurrentCtxAfterUnbind", Rd(B, 0x00409B00u));
    TraceDetail(L"B3cC2eFeHwwEsr", Rd(B, 0x00404000u));
    TraceDetail(L"B3cC2eFecsCpuctl", Rd(B, 0x00409100u));
    TraceDetail(L"B3cC2eFecsSctl", Rd(B, 0x00409240u));
    TraceDetail(L"B3cC2eFecsMailbox0", Rd(B, 0x00409040u));
    TraceDetail(L"B3cC2eFecsMailbox1", Rd(B, 0x00409044u));
    TraceDetail(L"B3cC2eFecsIrqstat", Rd(B, 0x00409008u));
    TraceDetail(L"B3cC2eFecsIrqmask", Rd(B, 0x0040900Cu));
    fecsTraceIdx = Rd(B, 0x00409148u);
    fecsTraceCount = (fecsTraceIdx >> 16) & 0xFFu;
    if (fecsTraceCount > RTL_NUMBER_OF(fecsPc)) fecsTraceCount = RTL_NUMBER_OF(fecsPc);
    TraceDetail(L"B3cC2eFecsTraceIdx", fecsTraceIdx);
    TraceDetail(L"B3cC2eFecsTraceCount", fecsTraceCount);
    for (i = 0; i < fecsTraceCount; ++i) {
        Wr(B, 0x00409148u, i);
        fecsPc[i] = Rd(B, 0x0040914Cu);
    }
    if (fecsTraceCount) TraceBin(L"B3cC2eFecsTracePc", fecsPc, fecsTraceCount * sizeof(ULONG));
    TraceDetail(L"B3cC2eGpccsCpuctl", Rd(B, 0x0041A100u));
    TraceDetail(L"B3cC2eGpccsSctl", Rd(B, 0x0041A240u));
    TraceDetail(L"B3cC2eGpccsMailbox0", Rd(B, 0x0041A040u));
    TraceDetail(L"B3cC2eGpccsMailbox1", Rd(B, 0x0041A044u));
    TraceDetail(L"B3cC2ePfifoIntr0", Rd(B, 0x2100u));
    faultId = Rd(B, 0x259Cu);
    TraceDetail(L"B3cC2eFaultId", faultId);
    for (unit = 0; unit < 32u && faultCount < RTL_NUMBER_OF(faultRecs); ++unit) {
        if (!(faultId & (1u << unit))) continue;
        faultRecs[faultCount][0] = unit;
        faultRecs[faultCount][1] = Rd(B, 0x2800u + unit * 0x10u);
        faultRecs[faultCount][2] = Rd(B, 0x2804u + unit * 0x10u);
        faultRecs[faultCount][3] = Rd(B, 0x2808u + unit * 0x10u);
        faultRecs[faultCount][4] = Rd(B, 0x280Cu + unit * 0x10u);
        ++faultCount;
    }
    TraceDetail(L"B3cC2eFaultCount", faultCount);
    if (faultCount) TraceErrorBin(L"B3cC2eFaultRecords", faultRecs, faultCount * sizeof(faultRecs[0]));
    TraceDetail(L"B3cC2eMcInt", Rd(Dev->Mc, MC_INTSTATUS));
    TraceDetail(L"B3cC2eMcErr", Rd(Dev->Mc, MC_ERR_STATUS));
    TraceDetail(L"B3cC2eMcErrAdr", Rd(Dev->Mc, MC_ERR_ADR));

done:
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    if (!grDumpStopCaptured && (pmcEnable & (1u << 12))) {
        if (g_Verbose || result != 0u) B3cCaptureGrStatusDump(B, L"B3cC2eGrStatusDumpAtStop", grDumpStop);
        grDumpStopCaptured = 1u;
    }
    TraceDetail(L"B3cC2eGrStatusDumpRegisterCount", B3C_GR_STATUS_DUMP_REGS);
    TraceDetail(L"B3cC2eGrStatusDumpBeforeCaptured", grDumpBeforeCaptured);
    TraceDetail(L"B3cC2eGrStatusDumpEntry0Captured", grDumpEntry0Captured);
    TraceDetail(L"B3cC2eGrStatusDumpStopCaptured", grDumpStopCaptured);
    Trace(L"B3cC2eNouveauIdleAfterSwCtxPolls", trueIdleAfterSwCtx[2]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSwCtxGrStatus", trueIdleAfterSwCtx[3]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSwCtxEngineStatus", trueIdleAfterSwCtx[4]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSwCtxFifoStatus", trueIdleAfterSwCtx[5]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSwCtxMcEnable", trueIdleAfterSwCtx[6]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSwCtxOk", trueIdleAfterSwCtxOk);
    Trace(L"B3cC2eNouveauIdleAfterSetupPolls", trueIdleAfterSetup[2]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSetupGrStatus", trueIdleAfterSetup[3]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSetupEngineStatus", trueIdleAfterSetup[4]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSetupFifoStatus", trueIdleAfterSetup[5]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSetupMcEnable", trueIdleAfterSetup[6]);
    TraceDetail(L"B3cC2eNouveauIdleAfterSetupOk", trueIdleAfterSetupOk);
    Trace(L"B3cC2eNouveauIdleAfterRestorePolls", trueIdleAfterRestore[2]);
    TraceDetail(L"B3cC2eNouveauIdleAfterRestoreGrStatus", trueIdleAfterRestore[3]);
    TraceDetail(L"B3cC2eNouveauIdleAfterRestoreEngineStatus", trueIdleAfterRestore[4]);
    TraceDetail(L"B3cC2eNouveauIdleAfterRestoreFifoStatus", trueIdleAfterRestore[5]);
    TraceDetail(L"B3cC2eNouveauIdleAfterRestoreMcEnable", trueIdleAfterRestore[6]);
    TraceDetail(L"B3cC2eNouveauIdleAfterRestoreOk", trueIdleAfterRestoreOk);
    Trace(L"B3cC2eNouveauIdleAfterMethodPolls", trueIdleAfterMethodInit[2]);
    TraceDetail(L"B3cC2eNouveauIdleAfterMethodGrStatus", trueIdleAfterMethodInit[3]);
    TraceDetail(L"B3cC2eNouveauIdleAfterMethodEngineStatus", trueIdleAfterMethodInit[4]);
    TraceDetail(L"B3cC2eNouveauIdleAfterMethodFifoStatus", trueIdleAfterMethodInit[5]);
    TraceDetail(L"B3cC2eNouveauIdleAfterMethodMcEnable", trueIdleAfterMethodInit[6]);
    TraceDetail(L"B3cC2eNouveauIdleAfterMethodOk", trueIdleAfterMethodInitOk);
    TraceDetail(L"B3cC2eFirstBusyBundleIndex", firstBusyIndex);
    TraceDetail(L"B3cC2eFirstBusyBundleMethod", firstBusyMethod);
    TraceDetail(L"B3cC2eFirstBusyEngineStatus", firstBusyEngineStatus);
    TraceDetail(L"B3cC2eFirstBusyGrStatus", firstBusyGrStatus);
    TraceDetail(L"B3cC2eFirstBusyFifoStatus", firstBusyFifoStatus);
    TraceDetail(L"B3cC2eTrueIdleGoBundleStage", trueIdleGoBundle[0]);
    TraceDetail(L"B3cC2eTrueIdleGoBundleIndex", trueIdleGoBundle[1]);
    Trace(L"B3cC2eTrueIdleGoBundlePolls", trueIdleGoBundle[2]);
    TraceDetail(L"B3cC2eTrueIdleGoBundleGrStatus", trueIdleGoBundle[3]);
    TraceDetail(L"B3cC2eTrueIdleGoBundleEngineStatus", trueIdleGoBundle[4]);
    TraceDetail(L"B3cC2eTrueIdleGoBundleFifoStatus", trueIdleGoBundle[5]);
    TraceDetail(L"B3cC2eTrueIdleGoBundleMcEnable", trueIdleGoBundle[6]);
    TraceDetail(L"B3cC2eTrueIdleGoBundleOk", trueIdleGoBundleOk);
    TraceDetail(L"B3cC2eTrueIdlePreSaveStage", trueIdlePreSave[0]);
    TraceDetail(L"B3cC2eTrueIdlePreSaveBundleIndex", trueIdlePreSave[1]);
    Trace(L"B3cC2eTrueIdlePreSavePolls", trueIdlePreSave[2]);
    TraceDetail(L"B3cC2eTrueIdlePreSaveGrStatus", trueIdlePreSave[3]);
    TraceDetail(L"B3cC2eTrueIdlePreSaveEngineStatus", trueIdlePreSave[4]);
    TraceDetail(L"B3cC2eTrueIdlePreSaveFifoStatus", trueIdlePreSave[5]);
    TraceDetail(L"B3cC2eTrueIdlePreSaveMcEnable", trueIdlePreSave[6]);
    TraceDetail(L"B3cC2eTrueIdlePreSaveOk", trueIdlePreSaveOk);
    TraceDetail(L"B3cC2eWaitHistoryCount", waitHistoryCount);
    TraceDetail(L"B3cC2eWaitHistoryDropped", waitHistoryDropped);
    TraceDetail(L"B3cC2eWaitHistoryCapacity", waitHistory != NULL ? waitHistoryCapacity : 0u);
    if (waitHistory != NULL && waitHistoryCount != 0u)
        TraceBin(L"B3cC2eWaitHistory", waitHistory,
            waitHistoryCount * 10u * sizeof(ULONG));
    TraceDetail(L"B3cC2eWaitStage", waitState[0]);
    TraceDetail(L"B3cC2eWaitBundleIndex", waitState[1]);
    Trace(L"B3cC2eWaitPolls", waitState[2]);
    TraceDetail(L"B3cC2eWaitGrStatus", waitState[3]);
    TraceDetail(L"B3cC2eWaitEngineStatus", waitState[4]);
    TraceDetail(L"B3cC2eWaitFifoEngineStatus", waitState[5]);
    TraceDetail(L"B3cC2eWaitIsFe", waitState[6]);
    TraceDetail(L"B3cC2eWaitSucceeded", waitState[7]);
    TraceDetail(L"B3cC2eWaitIdleBranch", waitState[8]);
    TraceDetail(L"B3cC2eWaitFifoCtxStatus", waitState[9]);
    TraceDetail(L"B3cC2eWaitCtxswActive", waitState[10]);
    TraceDetail(L"B3cC2eWaitGrBusy", waitState[11]);
    TraceDetail(L"B3cC2eTopNumLtcsRaw", ltcCountRaw);
    TraceDetail(L"B3cC2eLtcActiveCountRaw", ltcActiveCountRaw);
    TraceDetail(L"B3cC2eLtcMaxCount", ltcMaxCount);
    TraceDetail(L"B3cC2eLtcCount", ltcCountDetected);
    TraceDetail(L"B3cC2eLtcCbcParam", ltcParamRaw);
    TraceDetail(L"B3cC2eLtcSlicesPerLtc", ltcSlicesPerLtc);
    TraceDetail(L"B3cC2eLtcCacheLineBytes", ltcCacheLineBytes);
    TraceDetail(L"B3cC2eLtcComptagsPerCacheline", ltcTagsPerCacheLine);
    TraceDetail(L"B3cC2eLtcFsConfigured", ltcFsConfigured);
    TraceDetail(L"B3cC2eLtcFsWriteCount", ltcFsWriteCount);
    TraceBin(L"B3cC2eLtcFsReadback", ltcFsWrites, sizeof(ltcFsWrites));
    TraceDetail(L"B3cC2eFbFsConfigured", fbFsConfigured);
    TraceDetail(L"B3cC2eFbFsWriteCount", fbFsWriteCount);
    TraceBin(L"B3cC2eFbFsReadback", fbFsWrites, sizeof(fbFsWrites));
    TraceDetail(L"B3cC2eZbcConfigured", zbcConfigured);
    TraceDetail(L"B3cC2eZbcWriteCount", zbcWriteCount);
    TraceBin(L"B3cC2eZbcReadback", zbcWrites, sizeof(zbcWrites));
    TraceDetail(L"B3cC2eZbcPmuSyncSkipped", 1u);
    TraceDetail(L"B3cC2eCbcConfigured", cbcConfigured);
    TraceDetail(L"B3cC2eCbcNoComptag", cbcNoComptag);
    TraceDetail(L"B3cC2eCbcBackingBytes", 0u);
    TraceDetail(L"B3cC2eCbcWriteCount", cbcWriteCount);
    TraceBin(L"B3cC2eCbcReadback", cbcWrites, sizeof(cbcWrites));
    TraceDetail(L"B3cC2eRopSetupWriteCount", ropWriteCount);
    TraceBin(L"B3cC2eRopSetupReadback", ropWrites, sizeof(ropWrites));
    TraceDetail(L"B3cC2eEarlyZcullWriteCount", earlyZcullWriteCount);
    TraceBin(L"B3cC2eEarlyZcullReadback", earlyZcullWrites, sizeof(earlyZcullWrites));
    TraceDetail(L"B3cC2eFbpCountRaw", fbpCountRaw);
    TraceDetail(L"B3cC2eFbpCount", fbpCount);
    TraceDetail(L"B3cC2eNouveauRopWriteCount", nouveauRopWriteCount);
    TraceBin(L"B3cC2eNouveauRopReadback", nouveauRopWrites, sizeof(nouveauRopWrites));
    TraceBin(L"B3cC2eFifoAccess", fifoCtlTrace, sizeof(fifoCtlTrace));
    TraceDetail(L"B3cC2eLtcCbcSetupSkipped", cbcConfigured ? 0u : 1u);
    TraceDetail(L"B3cC2eZbcSetupSkipped", zbcConfigured ? 0u : 1u);
    TraceDetail(L"B3cC2eLtcBasedRopSetupSkipped", (ltcFsConfigured && fbFsConfigured && beRopConfigured) ? 0u : 1u);
    TraceBin(L"B3cC2eBundleBaseline", bundleBaseline, sizeof(bundleBaseline));
    TraceDetail(L"B3cC2eBundleFirstChangeFound", bundleFirstChangeFound);
    TraceDetail(L"B3cC2eBundleFirstChangeIndex", bundleFirstChangeIndex);
    TraceBin(L"B3cC2eBundleEntry0State", bundleEntry0, sizeof(bundleEntry0));
    if (bundleFirstChangeFound)
        TraceErrorBin(L"B3cC2eBundleFirstChangeState", bundleFirstChange,
                 sizeof(bundleFirstChange));
    B3cPrivRingTraceWriteLog(L"B3cC2ePrivWriteErrorCount",
        L"B3cC2ePrivWriteErrorDropped", L"B3cC2ePrivAckTimeouts",
        L"B3cC2ePrivWriteErrors");
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cC2ePmcEnableAtDiagnostics", pmcEnable);
    if (pmcEnable & (1u << 12)) {
        if (g_Verbose || result != 0u) B3cC2eCaptureDiagnostics(Dev);
        gr5044b0AtExit = Rd(B, 0x005044B0u);
        TraceDetail(L"B3cC2eGr5044B0AtExit", gr5044b0AtExit);
        TraceDetail(L"B3cC2eGr5044B0Bit27AtExit", (gr5044b0AtExit >> 27) & 1u);
    }
    TraceDetail(L"B3cC2eMcUnk260AtExit", Rd(B, 0x00000260u));
    if (bundle != NULL) ExFreePoolWithTag(bundle, SWGPU_TAG);
    if (waitHistory != NULL) ExFreePoolWithTag(waitHistory, SWGPU_TAG);
    Wr(B, 0x00400208u, 0u);
    if (feGoIdleTimeoutSavedValid) Wr(B, 0x00404154u, feGoIdleTimeoutSaved);
    TraceDurable(L"B3cC2eResult", result);
    return result;
}

/* D1: expose the GM20B PFIFO/PBDMA hardware setup after the golden-context
 * path has passed.  The topology and state reads are diagnostic only.  The
 * small write set follows Nouveau gk104_fifo_init_pbdmas(),
 * gk208_fifo_init_pbdma_timeout(), and gk104_fifo_init(); no runlist or USERD
 * is published until D2 owns bounded backing for those structures. */
static ULONG B3cInitFifoD1(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    ULONG result = 0;
    ULONG topNumGpcs, topTpcPerGpc, topNumFbps, topNumLtcs;
    ULONG topInfo[64] = { 0 };
    ULONG topCount = RTL_NUMBER_OF(topInfo), i;
    ULONG mcPbEnableBefore, fifoIntrBefore, fifoIntrEnBefore;
    ULONG fifoEngineStatusBefore, fifoPbdmaStatusBefore;
    ULONG pbdmaStatusBefore, pbdmaIntrBefore, pbdmaIntrEnBefore;
    ULONG mcPbEnableAfter = 0, fbTimeoutBefore = 0, fbTimeoutExpected = 0;
    ULONG fbTimeoutAfter = 0, pbdmaTimeoutBefore = 0, pbdmaTimeoutAfter = 0;
    ULONG fifoIntrAfterClear = 0, fifoIntrEnAfter = 0;
    ULONG writes[5][4] = { 0 };
    ULONG grEnabled;

    Step(104);
    B3cPrivRingResetWriteLog();
    grEnabled = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cD1PmcEnable", grEnabled);

    topNumGpcs = Rd(B, 0x0022430u);
    topTpcPerGpc = Rd(B, 0x0022434u);
    topNumFbps = Rd(B, 0x0022438u);
    topNumLtcs = Rd(B, 0x0022454u);
    TraceDetail(L"B3cD1TopNumGpcs", topNumGpcs);
    TraceDetail(L"B3cD1TopTpcPerGpc", topTpcPerGpc);
    TraceDetail(L"B3cD1TopNumFbps", topNumFbps);
    TraceDetail(L"B3cD1TopNumLtcs", topNumLtcs);
    for (i = 0; i < topCount; ++i) {
        topInfo[i] = Rd(B, 0x0022700u + i * 4u);
        if ((topInfo[i] & 0x80000000u) == 0u && i != 0u) {
            ++i;
            break;
        }
    }
    topCount = i;
    TraceDetail(L"B3cD1TopDeviceInfoWords", topCount);
    if (topCount != 0u) TraceBin(L"B3cD1TopDeviceInfo", topInfo, topCount * sizeof(ULONG));

    mcPbEnableBefore = Rd(B, 0x00000204u);
    fifoIntrBefore = Rd(B, 0x00002100u);
    fifoIntrEnBefore = Rd(B, 0x00002140u);
    fifoEngineStatusBefore = Rd(B, 0x00002640u);
    fifoPbdmaStatusBefore = Rd(B, 0x00003080u);
    pbdmaStatusBefore = Rd(B, 0x00040100u);
    pbdmaIntrBefore = Rd(B, 0x00040108u);
    pbdmaIntrEnBefore = Rd(B, 0x0004010Cu);
    TraceDetail(L"B3cD1McEnablePbBefore", mcPbEnableBefore);
    TraceDetail(L"B3cD1FifoIntr0Before", fifoIntrBefore);
    TraceDetail(L"B3cD1FifoIntrEn0Before", fifoIntrEnBefore);
    TraceDetail(L"B3cD1FifoEngineStatus0Before", fifoEngineStatusBefore);
    TraceDetail(L"B3cD1FifoPbdmaStatus0Before", fifoPbdmaStatusBefore);
    TraceDetail(L"B3cD1PbdmaStatus0Before", pbdmaStatusBefore);
    TraceDetail(L"B3cD1PbdmaIntr0Before", pbdmaIntrBefore);
    TraceDetail(L"B3cD1PbdmaIntrEn0Before", pbdmaIntrEnBefore);

    if (!(grEnabled & (1u << 12))) { result = 0xE000009Fu; goto done; }
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }

    /* GM20B exposes PBDMA0 as MC_ENABLE_PB bit 0.  Nouveau derives this mask
     * from its runlist/device topology and writes it to 0x204. */
    writes[0][0] = 0x00000204u; writes[0][1] = mcPbEnableBefore;
    writes[0][2] = 0x00000001u;
    Wr(B, writes[0][0], writes[0][2]);
    if (!B3cPrivRingAfterGrWrite(B, writes[0][0], writes[0][2])) { result = 0xE0000095u; goto done; }
    mcPbEnableAfter = Rd(B, writes[0][0]); writes[0][3] = mcPbEnableAfter;
    if ((mcPbEnableAfter & 1u) == 0u) { result = 0xE000009Fu; goto done; }

    /* Nouveau gk104_fifo_init_pbdmas() applies this masked FB timeout policy. */
    fbTimeoutBefore = Rd(B, 0x00002A04u);
    fbTimeoutExpected = (fbTimeoutBefore & ~0xBFFFFFFFu) | 0xBFFFFFFFu;
    writes[1][0] = 0x00002A04u; writes[1][1] = fbTimeoutBefore;
    writes[1][2] = fbTimeoutExpected;
    Wr(B, writes[1][0], writes[1][2]);
    if (!B3cPrivRingAfterGrWrite(B, writes[1][0], writes[1][2])) { result = 0xE0000095u; goto done; }
    fbTimeoutAfter = Rd(B, writes[1][0]); writes[1][3] = fbTimeoutAfter;
    if ((fbTimeoutAfter & 0xBFFFFFFFu) != 0xBFFFFFFFu) { result = 0xE000009Fu; goto done; }

    /* GM20B's Nouveau function table selects gk208_fifo_init_pbdma_timeout().
     * Its one active PBDMA gets the documented 0x0000ffff timeout value. */
    pbdmaTimeoutBefore = Rd(B, 0x0004012Cu);
    writes[2][0] = 0x0004012Cu; writes[2][1] = pbdmaTimeoutBefore;
    writes[2][2] = pbdmaTimeoutBefore | 0x0000FFFFu; /* Claude: nouveau gk208_fifo_init_pbdma_timeout() masks only bits 15:0 */
    Wr(B, writes[2][0], writes[2][2]);
    if (!B3cPrivRingAfterGrWrite(B, writes[2][0], writes[2][2])) { result = 0xE0000095u; goto done; }
    pbdmaTimeoutAfter = Rd(B, writes[2][0]); writes[2][3] = pbdmaTimeoutAfter;
    if (pbdmaTimeoutAfter != writes[2][2]) { result = 0xE000009Fu; goto done; }

    /* gk104_fifo_init() clears pending PFIFO causes and enables local
     * interrupts; MC/CPU interrupt routing remains untouched for polling. */
    writes[3][0] = 0x00002100u; writes[3][1] = fifoIntrBefore;
    writes[3][2] = 0xFFFFFFFFu;
    Wr(B, writes[3][0], writes[3][2]);
    if (!B3cPrivRingAfterGrWrite(B, writes[3][0], writes[3][2])) { result = 0xE0000095u; goto done; }
    fifoIntrAfterClear = Rd(B, writes[3][0]); writes[3][3] = fifoIntrAfterClear;

    writes[4][0] = 0x00002140u; writes[4][1] = fifoIntrEnBefore;
    writes[4][2] = 0x7FFFFFFFu;
    Wr(B, writes[4][0], writes[4][2]);
    if (!B3cPrivRingAfterGrWrite(B, writes[4][0], writes[4][2])) { result = 0xE0000095u; goto done; }
    fifoIntrEnAfter = Rd(B, writes[4][0]); writes[4][3] = fifoIntrEnAfter;
    /* Claude: only implemented bits latch (hardware 0.4.57 read 0x79810101).
     * Require no bits outside the write and the PBDMA (29) / MMU fault (28) bits. */
    if ((fifoIntrEnAfter & ~writes[4][2]) != 0u ||
        (fifoIntrEnAfter & 0x30000000u) != 0x30000000u) { result = 0xE000009Fu; goto done; }
    KeMemoryBarrier();

done:
    TraceDetail(L"B3cD1McEnablePbAfter", mcPbEnableAfter);
    TraceDetail(L"B3cD1FbTimeoutBefore", fbTimeoutBefore);
    TraceDetail(L"B3cD1FbTimeoutExpected", fbTimeoutExpected);
    TraceDetail(L"B3cD1FbTimeoutAfter", fbTimeoutAfter);
    TraceDetail(L"B3cD1PbdmaTimeoutBefore", pbdmaTimeoutBefore);
    TraceDetail(L"B3cD1PbdmaTimeoutAfter", pbdmaTimeoutAfter);
    TraceDetail(L"B3cD1FifoIntr0AfterClear", fifoIntrAfterClear);
    TraceDetail(L"B3cD1FifoIntrEn0After", fifoIntrEnAfter);
    TraceBin(L"B3cD1Writes", writes, sizeof(writes));
    Trace(L"B3cD1Result", result);
    B3cPrivRingTraceWriteLog(L"B3cD1PrivWriteErrorCount",
        L"B3cD1PrivWriteErrorDropped", L"B3cD1PrivAckTimeouts",
        L"B3cD1PrivWriteErrors");
    return result;
}

/* A single channel uses only memory inside the C2d arena.  RAMFC offsets and
 * values below follow MIT Nouveau gk104_chan_ramfc_write()/chan_bind(); the
 * two-DWORD runlist entry follows gk104_runl_insert_chan().  Register fields
 * are cross-checked against the MIT GM20B hw_ram/hw_ccsr/hw_fifo headers. */
static BOOLEAN B3cD2Write(volatile UCHAR *B, ULONG writeTable[64][4],
                           ULONG *writeCount, ULONG writeAddress,
                           ULONG writeValue)
{
    ULONG slot;
    if (*writeCount >= 64u) return FALSE;
    slot = *writeCount;
    writeTable[slot][0] = writeAddress;
    writeTable[slot][1] = Rd(B, writeAddress);
    writeTable[slot][2] = writeValue;
    Wr(B, writeAddress, writeValue);
    KeMemoryBarrier();
    if (!B3cPrivRingAfterGrWrite(B, writeAddress, writeValue)) {
        writeTable[slot][3] = Rd(B, writeAddress);
        ++(*writeCount);
        return FALSE;
    }
    writeTable[slot][3] = Rd(B, writeAddress);
    ++(*writeCount);
    return TRUE;
}

/* D5 L2 maintenance uses a bounded writes[] ledger because the semaphore
 * visibility poll may request up to 2000 flush/invalidate pairs. */
static BOOLEAN B3cD5Write(volatile UCHAR *B, ULONG (*writeTable)[4],
                           ULONG *writeCount, ULONG writeCapacity,
                           ULONG writeAddress, ULONG writeValue)
{
    ULONG slot;
    if (writeTable == NULL || writeCount == NULL || *writeCount >= writeCapacity)
        return FALSE;
    slot = *writeCount;
    writeTable[slot][0] = writeAddress;
    writeTable[slot][1] = Rd(B, writeAddress);
    writeTable[slot][2] = writeValue;
    Wr(B, writeAddress, writeValue);
    KeMemoryBarrier();
    if (!B3cPrivRingAfterGrWrite(B, writeAddress, writeValue)) {
        writeTable[slot][3] = Rd(B, writeAddress);
        ++(*writeCount);
        return FALSE;
    }
    writeTable[slot][3] = Rd(B, writeAddress);
    ++(*writeCount);
    return TRUE;
}

static BOOLEAN B3cD5L2Flush(volatile UCHAR *B, ULONG (*writes)[4],
                             ULONG *writeCount, ULONG writeCapacity, PULONG state)
{
    ULONG polls, status = 0u;
    if (!B3cD5Write(B, writes, writeCount, writeCapacity, 0x00070010u, 1u))
        return FALSE;
    for (polls = 0u; polls < 2000u; ++polls) {
        status = Rd(B, 0x00070010u);
        if ((status & 3u) == 0u) break;
        Us(5u);
    }
    state[0] = polls; state[1] = status;
    if (polls == 2000u ||
        !B3cD5Write(B, writes, writeCount, writeCapacity, 0x00070004u, 1u)) {
        state[2] = 0u; state[3] = 0u;
        return FALSE;
    }
    for (polls = 0u; polls < 2000u; ++polls) {
        status = Rd(B, 0x00070004u);
        if ((status & 3u) == 0u) break;
        Us(5u);
    }
    state[2] = polls; state[3] = status;
    return polls < 2000u;
}

/* Capture the FECS/GPCCS and channel-context state when a host context switch
 * stalls. ICD trace-index writes are routed through B3cD2Write so each write
 * is recorded and checked by the priv-ring handler. Addresses/fields follow
 * the GM20B nvgpu hw_gr_gm20b.h names. */
static VOID B3cD4CaptureCtxswDiag(volatile UCHAR *B, PUCHAR ctxHeader,
                                  ULONG writes[64][4], ULONG *writeCount,
                                  BOOLEAN cleanup)
{
    static const ULONG fecsRegs[13] = {
        0x00409100u, 0x00409008u, 0x0040900Cu, 0x00409400u,
        0x00409C00u, 0x00409B04u, 0x00409B00u, 0x00409A10u,
        0x00409A0Cu, 0x00409C18u, 0x00502C04u, 0x00502C00u,
        0x00002A0Cu
    };
    static const PCWSTR fecsNames[2][13] = {
        { L"B3cD4FecsCpuctl", L"B3cD4FecsIrqstat", L"B3cD4FecsIrqmask",
          L"B3cD4FecsCtxswStatus1", L"B3cD4FecsCtxswStatusFe0",
          L"B3cD4FecsNewCtx", L"B3cD4FecsCurrentCtx", L"B3cD4FecsArbCtxAdr",
          L"B3cD4FecsArbCtxCmd", L"B3cD4FecsHostIntStatus",
          L"B3cD4GpccsGpcCtxswStatus0", L"B3cD4GpccsGpcCtxswStatus1",
          L"B3cD4FifoEngCtxswTimeout" },
        { L"B3cD4CleanupFecsCpuctl", L"B3cD4CleanupFecsIrqstat",
          L"B3cD4CleanupFecsIrqmask", L"B3cD4CleanupFecsCtxswStatus1",
          L"B3cD4CleanupFecsCtxswStatusFe0", L"B3cD4CleanupFecsNewCtx",
          L"B3cD4CleanupFecsCurrentCtx", L"B3cD4CleanupFecsArbCtxAdr",
          L"B3cD4CleanupFecsArbCtxCmd", L"B3cD4CleanupFecsHostIntStatus",
          L"B3cD4CleanupGpccsGpcCtxswStatus0",
          L"B3cD4CleanupGpccsGpcCtxswStatus1",
          L"B3cD4CleanupFifoEngCtxswTimeout" }
    };
    static const PCWSTR fecsMailboxNames[2][8] = {
        { L"B3cD4FecsMailbox0", L"B3cD4FecsMailbox1", L"B3cD4FecsMailbox2",
          L"B3cD4FecsMailbox3", L"B3cD4FecsMailbox4", L"B3cD4FecsMailbox5",
          L"B3cD4FecsMailbox6", L"B3cD4FecsMailbox7" },
        { L"B3cD4CleanupFecsMailbox0", L"B3cD4CleanupFecsMailbox1",
          L"B3cD4CleanupFecsMailbox2", L"B3cD4CleanupFecsMailbox3",
          L"B3cD4CleanupFecsMailbox4", L"B3cD4CleanupFecsMailbox5",
          L"B3cD4CleanupFecsMailbox6", L"B3cD4CleanupFecsMailbox7" }
    };
    PCWSTR fecsTraceIdxName = cleanup ? L"B3cD4CleanupFecsTraceIdx" : L"B3cD4FecsTraceIdx";
    PCWSTR fecsTraceCountName = cleanup ? L"B3cD4CleanupFecsTraceCount" : L"B3cD4FecsTraceCount";
    PCWSTR fecsTracePcName = cleanup ? L"B3cD4CleanupFecsPcTrace" : L"B3cD4FecsPcTrace";
    PCWSTR fecsTraceWriteErrorName = cleanup ? L"B3cD4CleanupFecsTraceWriteError" : L"B3cD4FecsTraceWriteError";
    PCWSTR gpccsCpuctlName = cleanup ? L"B3cD4CleanupGpccsCpuctl" : L"B3cD4GpccsCpuctl";
    PCWSTR gpccsTraceIdxName = cleanup ? L"B3cD4CleanupGpccsTraceIdx" : L"B3cD4GpccsTraceIdx";
    PCWSTR gpccsTraceCountName = cleanup ? L"B3cD4CleanupGpccsTraceCount" : L"B3cD4GpccsTraceCount";
    PCWSTR gpccsTracePcName = cleanup ? L"B3cD4CleanupGpccsPcTrace" : L"B3cD4GpccsPcTrace";
    PCWSTR gpccsTraceWriteErrorName = cleanup ? L"B3cD4CleanupGpccsTraceWriteError" : L"B3cD4GpccsTraceWriteError";
    PCWSTR ctxHeaderName = cleanup ? L"B3cD4CleanupCtxHdr" : L"B3cD4CtxHdr";
    ULONG values[RTL_NUMBER_OF(fecsRegs)];
    ULONG mailbox[8] = { 0u }, tracePc[8] = { 0u };
    ULONG traceIdx, traceCount, i, traceWriteError = 0u;
    ULONG nameSet = cleanup ? 1u : 0u;

    for (i = 0u; i < RTL_NUMBER_OF(fecsRegs); ++i) {
        values[i] = Rd(B, fecsRegs[i]);
        Trace(fecsNames[nameSet][i], values[i]);
    }
    for (i = 0u; i < RTL_NUMBER_OF(mailbox); ++i) {
        mailbox[i] = Rd(B, 0x00409800u + i * 4u);
        Trace(fecsMailboxNames[nameSet][i], mailbox[i]);
    }
    TraceBin(cleanup ? L"B3cD4CleanupFecsMailboxWords" : L"B3cD4FecsMailboxWords",
             mailbox, sizeof(mailbox));
    traceIdx = Rd(B, 0x00409148u);
    traceCount = (traceIdx >> 16) & 0xFFu;
    if (traceCount > RTL_NUMBER_OF(tracePc)) traceCount = RTL_NUMBER_OF(tracePc);
    Trace(fecsTraceIdxName, traceIdx);
    Trace(fecsTraceCountName, traceCount);
    for (i = 0u; i < traceCount; ++i) {
        if (!B3cD2Write(B, writes, writeCount, 0x00409148u, i)) {
            traceWriteError = 1u;
            break;
        }
        tracePc[i] = Rd(B, 0x0040914Cu);
    }
    Trace(fecsTraceWriteErrorName, traceWriteError);
    TraceBin(fecsTracePcName, tracePc, i * sizeof(ULONG));

    Trace(gpccsCpuctlName, Rd(B, 0x0041A100u));
    traceIdx = Rd(B, 0x0041A148u);
    traceCount = (traceIdx >> 16) & 0xFFu;
    if (traceCount > RTL_NUMBER_OF(tracePc)) traceCount = RTL_NUMBER_OF(tracePc);
    Trace(gpccsTraceIdxName, traceIdx);
    Trace(gpccsTraceCountName, traceCount);
    RtlZeroMemory(tracePc, sizeof(tracePc));
    for (i = 0u; i < traceCount; ++i) {
        if (!B3cD2Write(B, writes, writeCount, 0x0041A148u, i)) {
            traceWriteError = 1u;
            break;
        }
        tracePc[i] = Rd(B, 0x0041A14Cu);
    }
    Trace(gpccsTraceWriteErrorName, traceWriteError);
    TraceBin(gpccsTracePcName, tracePc, i * sizeof(ULONG));
    if (ctxHeader != NULL) TraceBin(ctxHeaderName, ctxHeader, 0x100u);
    TraceBin(cleanup ? L"B3cD4CleanupDiagWrites" : L"B3cD4DiagWrites",
             writes, (*writeCount) * sizeof(writes[0]));
}

/* Mirror the per-channel half of nvgpu gr_gk20a_load_golden_ctx_image().
 * C2e saved the golden image at GLOBAL_OFF; D2's instance block points to
 * CTX_OFF, so copy the image there and populate its ctxsw header before the
 * first host-driven GR context switch. */
static ULONG B3cPrepareD4ChannelContext(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR golden, channelCtx;
    PULONG header, patch;
    ULONG result = 0u, writes[64][4] = { { 0u } }, writeCount = 0u;
    ULONG l2FlushPolls = 0u, l2InvalidatePolls = 0u, l2Status = 0u;
    ULONG patchZeroOk = 1u, copyMatch = 0u, pmHeader = 0u;
    ULONG i, mismatch = 0u, pmcEnable = 0u;
    ULONG watchdogMailboxAfter = 0u, ctxswMailboxAfter = 0u;
    ULONG ctxswPolls = 0u, ctxswLast = 0u, ctxswStartOk = 0u;

    Step(107);
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    if (!g_GrVm.Active || g_GrVm.Arena == NULL ||
        g_GrVm.GoldenBytes == 0u || g_GrVm.GoldenBytes > 0x20000u ||
        g_GrVm.GoldenBytes > GR_VM_GLOBAL_OFF ||
        GR_VM_CTX_OFF + g_GrVm.GoldenBytes > GR_VM_GLOBAL_OFF ||
        GR_VM_PATCH_OFF + GR_VM_PATCH_BYTES > GR_VM_PRIV_OFF ||
        (pmcEnable & (1u << 12)) == 0u) {
        result = 0xE00000A3u;
        goto done;
    }
    if (!B3cPrivRingDrainBeforeWrites(B) ||
        !B3cD2Write(B, writes, &writeCount, 0x00070010u, 1u)) {
        result = 0xE00000A3u;
        goto done;
    }
    for (l2FlushPolls = 0u; l2FlushPolls < 2000u; ++l2FlushPolls) {
        l2Status = Rd(B, 0x00070010u);
        if ((l2Status & 3u) == 0u) break;
        Us(5u);
    }
    if (l2FlushPolls == 2000u ||
        !B3cD2Write(B, writes, &writeCount, 0x00070004u, 1u)) {
        result = 0xE00000A3u;
        goto done;
    }
    for (l2InvalidatePolls = 0u; l2InvalidatePolls < 2000u; ++l2InvalidatePolls) {
        l2Status = Rd(B, 0x00070004u);
        if ((l2Status & 3u) == 0u) break;
        Us(5u);
    }
    if (l2InvalidatePolls == 2000u) {
        result = 0xE00000A3u;
        goto done;
    }

    golden = (PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF;
    channelCtx = (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF;
    header = (PULONG)channelCtx;
    patch = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_PATCH_OFF);
    RtlCopyMemory(channelCtx, golden, g_GrVm.GoldenBytes);
    copyMatch = RtlCompareMemory(channelCtx, golden, g_GrVm.GoldenBytes) == g_GrVm.GoldenBytes ? 1u : 0u;
    RtlZeroMemory(patch, GR_VM_PATCH_BYTES);

    /* ctxsw_prog_main_image_* offsets verified in hw_ctxsw_prog_gm20b.h:
     * zero save/restore patch ops; point patch access at this channel's
     * bounded patch page; use allow-all because the diagnostic context has no
     * nvgpu-generated access whitelist; retain no-ctxsw ZCULL/PM modes. */
    header[0x10u / 4u] = 0u; /* patch_count */
    header[0x14u / 4u] = (ULONG)((GR_VM_GPU_VA + GR_VM_PATCH_OFF) & 0xFFFFFFFFu);
    header[0x18u / 4u] = (ULONG)(((ULONGLONG)GR_VM_GPU_VA + GR_VM_PATCH_OFF) >> 32);
    header[0x1Cu / 4u] = 1u; /* zcull_mode_no_ctxsw */
    header[0x20u / 4u] = 0u; /* zcull pointer */
    pmHeader = header[0x28u / 4u] & ~0x7u;
    header[0x28u / 4u] = pmHeader; /* pm_mode_no_ctxsw */
    header[0x2Cu / 4u] = 0u; /* PM pointer */
    header[0x3Cu / 4u] &= ~0x8u; /* verification features disabled */
    header[0xA0u / 4u] = 0u; /* priv access map mode allow_all */
    header[0xA4u / 4u] = GR_VM_GPU_VA + GR_VM_PRIV_OFF;
    header[0xA8u / 4u] = 0u;
    header[0xF4u / 4u] = 0u; /* num_save_ops */
    header[0xF8u / 4u] = 0u; /* num_restore_ops */
    KeMemoryBarrier();
    /* Claude 0.4.75: the CPU just rewrote the channel ctx image; flush and
     * invalidate GPU L2 so FECS does not load stale lines from C2. */
    {
        ULONG postCopyL2[4] = { 0 };
        ULONG postCopyOk = B3cC2eL2Flush(Dev->Bar0, postCopyL2) ? 1u : 0u;
        TraceDetail(L"B3cD4PostCopyL2Ok", postCopyOk);
        Trace(L"B3cD4PostCopyL2InvalidatePolls", postCopyL2[2]);
        if (!postCopyOk) result = 0xE00000A3u;
    }

    for (i = 0u; i < GR_VM_PATCH_BYTES / sizeof(ULONG); ++i) {
        if (patch[i] != 0u) { patchZeroOk = 0u; break; }
    }
    if (!copyMatch || !patchZeroOk || header[0xFCu / 4u] != 0x600DC0DEu ||
        header[0x10u / 4u] != 0u || header[0x14u / 4u] != GR_VM_GPU_VA + GR_VM_PATCH_OFF ||
        header[0x18u / 4u] != 0u || header[0x1Cu / 4u] != 1u ||
        header[0x20u / 4u] != 0u || header[0x28u / 4u] != 0u ||
        header[0x2Cu / 4u] != 0u || header[0xA0u / 4u] != 0u ||
        header[0xA4u / 4u] != GR_VM_GPU_VA + GR_VM_PRIV_OFF ||
        header[0xA8u / 4u] != 0u || header[0xF4u / 4u] != 0u ||
        header[0xF8u / 4u] != 0u || (header[0x3Cu / 4u] & 0x8u) != 0u)
        mismatch = 1u;
    if (mismatch) result = 0xE00000A3u;
    if (result == 0u) {
        /* Match gr_gk20a_init_ctx_state(): set FECS watchdog timeout through
         * its method interface. The reference pushes this method without a
         * mailbox wait. Then match gr_gk20a_enable_ctxsw()/gr_gk20a_ctrl_ctxsw
         * and wait for the documented mailbox-1 PASS/FAIL response. */
        if (!B3cD2Write(B, writes, &writeCount, 0x00409840u, 0xFFFFFFFFu) ||
            !B3cD2Write(B, writes, &writeCount, 0x00409500u, 0x7FFFFFFFu) ||
            !B3cD2Write(B, writes, &writeCount, 0x00409504u, 0x00000021u)) {
            result = 0xE00000A3u;
        }
        watchdogMailboxAfter = Rd(B, 0x00409800u);
        if (result == 0u &&
            (!B3cD2Write(B, writes, &writeCount, 0x00409844u, 0xFFFFFFFFu) ||
             !B3cD2Write(B, writes, &writeCount, 0x00409500u, 0xFFFFFFFFu) ||
             !B3cD2Write(B, writes, &writeCount, 0x00409504u, 0x00000039u))) {
            result = 0xE00000A3u;
        }
        if (result == 0u) {
            for (ctxswPolls = 0u; ctxswPolls < 2000u; ++ctxswPolls) {
                ctxswLast = Rd(B, 0x00409804u);
                if (ctxswLast == 1u || ctxswLast == 2u) break;
                Us(1u);
            }
            ctxswMailboxAfter = Rd(B, 0x00409804u);
            ctxswStartOk = ctxswMailboxAfter == 1u ? 1u : 0u;
            if (ctxswPolls == 2000u || ctxswMailboxAfter != 1u)
                result = 0xE00000A3u;
        }
    }

done:
    TraceDetail(L"B3cD4CtxImageBytes", g_GrVm.GoldenBytes);
    TraceDetail(L"B3cD4CtxPmcEnable", pmcEnable);
    TraceDetail(L"B3cD4CtxCopyMatch", copyMatch);
    TraceDetail(L"B3cD4PatchVa", GR_VM_GPU_VA + GR_VM_PATCH_OFF);
    TraceDetail(L"B3cD4PatchBytes", GR_VM_PATCH_BYTES);
    TraceDetail(L"B3cD4PatchZeroOk", patchZeroOk);
    Trace(L"B3cD4L2FlushPolls", l2FlushPolls);
    Trace(L"B3cD4L2InvalidatePolls", l2InvalidatePolls);
    TraceDetail(L"B3cD4L2Status", l2Status);
    TraceDetail(L"B3cD4HeaderMismatch", mismatch);
    TraceDetail(L"B3cD4WatchdogMethodData", 0x7FFFFFFFu);
    TraceDetail(L"B3cD4WatchdogMethodPush", 0x00000021u);
    TraceDetail(L"B3cD4WatchdogMailboxAfter", watchdogMailboxAfter);
    TraceDetail(L"B3cD4StartCtxswMethodData", 0xFFFFFFFFu);
    TraceDetail(L"B3cD4StartCtxswMethodPush", 0x00000039u);
    TraceDetail(L"B3cD4StartCtxswMailboxAfter", ctxswMailboxAfter);
    Trace(L"B3cD4StartCtxswPolls", ctxswPolls);
    TraceDetail(L"B3cD4StartCtxswOk", ctxswStartOk);
    if (g_GrVm.Arena != NULL) {
        TraceBin(L"B3cD4CtxHdr", (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF, 0x100u);
        TraceDetail(L"B3cD4GoldenMagic", *(volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0xFCu));
    }
    TraceBin(L"B3cD4CtxPrepWrites", writes, sizeof(writes));
    B3cPrivRingTraceWriteLog(L"B3cD4CtxPrepPrivWriteErrorCount",
        L"B3cD4CtxPrepPrivWriteErrorDropped", L"B3cD4CtxPrepPrivAckTimeouts",
        L"B3cD4CtxPrepPrivWriteErrors");
    TraceDurable(L"B3cD4CtxPrepResult", result);
    return result;
}

/* One host-only Kepler+ submission: SET_REFERENCE followed by SEMAPHORE
 * RELEASE. Method encodings are checked against NVIDIA's public GM20B/PBDMA
 * definitions. The semaphore occupies the final 16 bytes of this mapped PB
 * page, so it cannot overlap the seven command dwords. */
static ULONG B3cRunD3(PSWGPU_DEVICE Dev, PULONG userd, PULONG gpfifo,
                      PULONG pushbuffer, ULONGLONG pushbufferGpuVa)
{
    volatile UCHAR *B = Dev->Bar0;
    volatile ULONG *semSlot;
    volatile ULONG *bar1GpPut = NULL;
    ULONGLONG semGpuVa = (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE - 16u;
    ULONG result = 0u, polls = 0u, userdGpGet = 0u, pbGpGet = 0u;
    ULONG pbGet = 0u, pbPut = 0u, semValue = 0u, refCount = 0u;
    ULONG pbIntr0Before = 0u, pbIntr0After = 0u, pbIntr1 = 0u, method0 = 0u;
    ULONG data0 = 0u, fifoIntr0 = 0u, pbdmaStatus = 0u;
    ULONG pushWords[7] = { 0 }, gpWords[2] = { 0 }, userdGpPutBefore = 0u;
    ULONG bar1GpPutReadback = 0u, backingGpPutAfterBar1 = 0u;
    ULONG fifoMmuFaultBefore = 0u, fifoMmuFaultAfter = 0u, fifoIntrAfterBar1 = 0u;
    ULONG bar1WriteFault = 0u;
    ULONG i;

    Step(106);
    TraceDetail(L"B3cD3Bar1Phys", Dev->Bar1Phys.LowPart);
    TraceDetail(L"B3cD3Bar1PhysHi", (ULONG)Dev->Bar1Phys.HighPart);
    TraceDetail(L"B3cD3Bar1Len", Dev->Bar1Len);
    TraceDetail(L"B3cD3Bar1MapLen", Dev->Bar1MapLen);
    TraceDetail(L"B3cD3Bar1Mapped", Dev->Bar1Cpu != NULL ? 1u : 0u);
    if (userd == NULL || gpfifo == NULL || pushbuffer == NULL ||
        pushbufferGpuVa == 0u || (pushbufferGpuVa & 3u) != 0u ||
        semGpuVa + 16u > (ULONGLONG)GR_VM_GPU_VA + GR_VM_ARENA_SIZE ||
        Dev->Bar1Cpu == NULL || Dev->Bar1MapLen < BAR1_D3_MAP_LEN) {
        result = 0xE00000A1u;
        goto done;
    }
    bar1GpPut = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu);
    semSlot = (volatile ULONG *)((PUCHAR)pushbuffer + PAGE_SIZE - 16u);
    pushWords[0] = 0x20010014u; /* INC, count 1, subch 0, SET_REFERENCE 0x50 */
    pushWords[1] = 0x00001234u;
    pushWords[2] = 0x20040004u; /* INC, count 4, subch 0, SEMAPHOREA 0x10 */
    pushWords[3] = (ULONG)(semGpuVa >> 32);
    pushWords[4] = (ULONG)semGpuVa;
    pushWords[5] = 0xD3D3D3D3u;
    pushWords[6] = 0x01100002u; /* RELEASE, 4-byte payload, release_wfi disabled */
    for (i = 0; i < RTL_NUMBER_OF(pushWords); ++i) pushbuffer[i] = pushWords[i];
    for (i = 0; i < 4u; ++i) semSlot[i] = 0u;
    gpWords[0] = (ULONG)pushbufferGpuVa & ~3u; /* Claude 0.4.68: GP entry word0 holds GET address bits 31:2 in place (nouveau ib: lower_32_bits(offset)) */
    gpWords[1] = (ULONG)((pushbufferGpuVa >> 32) & 0xFFu) | (7u << 10);
    gpfifo[0] = gpWords[0];
    gpfifo[1] = gpWords[1];
    pbIntr0Before = Rd(B, 0x00040108u);
    userdGpPutBefore = userd[0x8Cu >> 2];
    KeMemoryBarrier();
    fifoMmuFaultBefore = Rd(B, 0x00002A30u);
    TraceDetail(L"B3cD3FifoMmuFaultStatusBefore", fifoMmuFaultBefore);
    TraceDetail(L"B3cD3Bar1GpPutOffset", BAR1_D3_LOW_VA + 0x8Cu);
    TraceDetail(L"B3cD3Bar1GpPutValue", 1u);
    *bar1GpPut = 1u; /* nvgpu gk20a_bar1_writel(USERD GP_PUT) is the host doorbell. */
    KeMemoryBarrier();
    bar1GpPutReadback = *bar1GpPut;
    backingGpPutAfterBar1 = userd[0x8Cu >> 2];
    fifoMmuFaultAfter = Rd(B, 0x00002A30u);
    fifoIntrAfterBar1 = Rd(B, 0x00002100u);
    TraceDetail(L"B3cD3Bar1GpPutReadback", bar1GpPutReadback);
    TraceDetail(L"B3cD3BackingGpPutAfterBar1Write", backingGpPutAfterBar1);
    TraceDetail(L"B3cD3FifoMmuFaultStatusAfterBar1Write", fifoMmuFaultAfter);
    TraceDetail(L"B3cD3FifoIntr0AfterBar1Write", fifoIntrAfterBar1);
    if (bar1GpPutReadback != 1u ||
        (bar1GpPutReadback & 0xFFF00000u) == 0xBAD00000u ||
        /* Claude 0.4.71: 0.4.70 hw read BAR1 back as 1 while the sysmem
         * backing still read 0 (host keeps snooped USERD; the channel was
         * loaded ON_PBDMA right after). Backing is traced, not required. */
        (fifoMmuFaultAfter != fifoMmuFaultBefore) ||
        (fifoIntrAfterBar1 & 0x10000000u) != 0u) {
        bar1WriteFault = 1u;
        result = 0xE00000A1u;
        goto done;
    }

    for (polls = 0; polls < 2000u; ++polls) {
        userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u); /* Claude 0.4.71: USERD GP_GET via BAR1 */
        pbGpGet = Rd(B, 0x00040014u);
        semValue = semSlot[0];
        if (userdGpGet == 1u && pbGpGet == 1u && semValue == 0xD3D3D3D3u) break;
        fifoMmuFaultAfter = Rd(B, 0x00002A30u);
        fifoIntrAfterBar1 = Rd(B, 0x00002100u);
        if ((fifoMmuFaultAfter != fifoMmuFaultBefore) ||
            (fifoIntrAfterBar1 & 0x10000000u) != 0u) {
            bar1WriteFault = 1u;
            result = 0xE00000A1u;
            break;
        }
        Us(1u);
    }
    userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u); /* Claude 0.4.71 */
    pbGpGet = Rd(B, 0x00040014u);
    pbGet = Rd(B, 0x00040018u);
    pbPut = Rd(B, 0x0004005Cu);
    semValue = semSlot[0];
    TraceDetail(L"B3cD3SemValueBeforeL2Flush", semValue);
    /* Claude 0.4.73: 0.4.72 hw consumed the entry (GP_GET 1, PB GET == PUT
     * after 7 dwords) but the CPU still read 0: the host semaphore release
     * is held in GPU L2 (sysmem PTE not volatile). Flush/invalidate L2 the
     * same way as C2e (gk20a_mm_l2_flush), then re-read. */
    {
        ULONG l2State[4] = { 0 };
        ULONG l2Ok = B3cC2eL2Flush(B, l2State) ? 1u : 0u;
        KeMemoryBarrier();
        TraceDetail(L"B3cD3L2FlushOk", l2Ok);
        Trace(L"B3cD3L2FlushPolls", l2State[0]);
        TraceDetail(L"B3cD3L2FlushStatus", l2State[1]);
        Trace(L"B3cD3L2InvalidatePolls", l2State[2]);
        TraceDetail(L"B3cD3L2InvalidateStatus", l2State[3]);
    }
    semValue = semSlot[0];
    TraceDetail(L"B3cD3SemWord1", semSlot[1]);
    TraceDetail(L"B3cD3SemWord2", semSlot[2]);
    TraceDetail(L"B3cD3SemWord3", semSlot[3]);
    refCount = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x48u); /* Claude 0.4.73: USERD ref via BAR1 */
    TraceDetail(L"B3cD3RefCountBacking", userd[0x48u >> 2]);
    pbIntr0After = Rd(B, 0x00040108u);
    pbIntr1 = Rd(B, 0x00040148u);
    method0 = Rd(B, 0x000400C0u);
    data0 = Rd(B, 0x000400C4u);
    fifoIntr0 = Rd(B, 0x00002100u);
    pbdmaStatus = Rd(B, 0x00003080u);
    if (userdGpGet != 1u || pbGpGet != 1u || semValue != 0xD3D3D3D3u)
        result = 0xE00000A1u;

done:
    Trace(L"B3cD3GpGetPolls", polls);
    TraceDetail(L"B3cD3UserdGpGet", userdGpGet);
    TraceDetail(L"B3cD3PbGpGet", pbGpGet);
    TraceDetail(L"B3cD3PbGet", pbGet);
    TraceDetail(L"B3cD3PbPut", pbPut);
    TraceDetail(L"B3cD3SemValue", semValue);
    TraceDetail(L"B3cD3RefCount", refCount);
    TraceDetail(L"B3cD3PbIntr0Before", pbIntr0Before);
    TraceDetail(L"B3cD3PbIntr0After", pbIntr0After);
    TraceDetail(L"B3cD3PbIntr1", pbIntr1);
    TraceDetail(L"B3cD3PbMethod0", method0);
    TraceDetail(L"B3cD3PbData0", data0);
    TraceDetail(L"B3cD3FifoIntr0", fifoIntr0);
    TraceDetail(L"B3cD3PbdmaStatus", pbdmaStatus);
    TraceDetail(L"B3cD3PushbufferGpuVa", (ULONG)pushbufferGpuVa);
    TraceDetail(L"B3cD3SemGpuVa", (ULONG)semGpuVa);
    TraceBin(L"B3cD3PushbufferWords", pushWords, sizeof(pushWords));
    TraceBin(L"B3cD3GpfifoEntry", gpWords, sizeof(gpWords));
    TraceDetail(L"B3cD3UserdGpPutBefore", userdGpPutBefore);
    TraceDetail(L"B3cD3UserdGpPutAfter", userd != NULL ? userd[0x8Cu >> 2] : 0u);
    TraceDetail(L"B3cD3Bar1GpPutReadback", bar1GpPutReadback);
    TraceDetail(L"B3cD3BackingGpPutAfterBar1Write", backingGpPutAfterBar1);
    TraceDetail(L"B3cD3FifoMmuFaultStatusBefore", fifoMmuFaultBefore);
    TraceDetail(L"B3cD3FifoMmuFaultStatusAfterBar1Write", fifoMmuFaultAfter);
    TraceDetail(L"B3cD3FifoIntr0AfterBar1Write", fifoIntrAfterBar1);
    TraceDetail(L"B3cD3Bar1WriteFault", bar1WriteFault);
    TraceDetail(L"B3cD3BackingGpPutFinal", userd != NULL ? userd[0x8Cu >> 2] : 0u);
    TraceDetail(L"B3cD3Bar1GpGetFinal", bar1GpPut != NULL ? *(volatile ULONG *)((volatile UCHAR *)bar1GpPut - 4u) : 0u);
    TraceDurable(L"B3cD3Result", result);
    return result;
}

/* D4 submits a second GPFIFO packet on the channel that passed D3.  The
 * Maxwell B 3D class header (NVIDIA open-gpu-doc classes/3d/clb197.h) defines
 * SET_OBJECT=0x0000 and NO_OPERATION=0x0100.  After selecting class 0xB197,
 * issue that harmless GR method and an engine-ordered host semaphore release
 * (WFI enabled).  L2 maintenance writes use the same priv-ring checked table
 * as the D2 register writes. */
static ULONG B3cRunD4(PSWGPU_DEVICE Dev, PULONG userd, PULONG gpfifo,
                      PULONG pushbuffer, ULONGLONG pushbufferGpuVa)
{
    volatile UCHAR *B = Dev->Bar0;
    volatile ULONG *bar1GpPut = NULL;
    volatile ULONG *semSlot = NULL;
    PULONG instWords = NULL;
    ULONGLONG semGpuVa = (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE - 32u;
    ULONG result = 0u, grEnabled = 0u, polls = 0u;
    ULONG expectedCtxLo = ((ULONG)GR_VM_GPU_VA + GR_VM_CTX_OFF) | 4u;
    ULONG expectedCtxHi = 0u, goldenMagic = 0u, ctxPointerOk = 0u;
    ULONG userdGpGet = 0u, bar1GpPutReadback = 0u;
    ULONG pbGpGet = 0u, pbGet = 0u, pbPut = 0u;
    ULONG semValue = 0u, refCount = 0u, pbIntr0 = 0u, pbIntr1 = 0u;
    ULONG pbMethod0 = 0u, pbData0 = 0u, fifoIntr0 = 0u, mmuFault = 0u;
    ULONG grIntr = 0u, grException = 0u, grStatus = 0u, feHwwEsr = 0u;
    ULONG fecsCurrentCtx = 0u, fecsMailbox0 = 0u, engineStatus = 0u;
    ULONG gpWords[2] = { 0u, 0u };
    ULONG pushWords[9] = { 0u };
    ULONG reportWords[4] = { 0u, 0u, 0u, 0u };
    ULONG writes[64][4] = { { 0u } };
    ULONG writeCount = 0u, l2FlushPolls = 0u, l2InvalidatePolls = 0u;
    ULONG l2Status = 0u, pbdmaStatus = 0u, diagCaptured = 0u;
    ULONG i;

    Step(108);
    grEnabled = Rd(B, NV_PMC_ENABLE);
    if (g_GrVm.Arena != NULL) {
        instWords = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF);
        goldenMagic = *(volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF + 0xFCu);
        ctxPointerOk = (instWords[132] == expectedCtxLo &&
                        instWords[133] == expectedCtxHi &&
                        goldenMagic == 0x600DC0DEu) ? 1u : 0u;
    }
    TraceDetail(L"B3cD4InstCtxExpectedLo", expectedCtxLo);
    TraceDetail(L"B3cD4InstCtxExpectedHi", expectedCtxHi);
    TraceDetail(L"B3cD4InstCtxActualLo", instWords != NULL ? instWords[132] : 0u);
    TraceDetail(L"B3cD4InstCtxActualHi", instWords != NULL ? instWords[133] : 0u);
    TraceDetail(L"B3cD4GoldenMagic", goldenMagic);
    TraceDetail(L"B3cD4ContextValid", ctxPointerOk);
    if ((grEnabled & (1u << 12)) == 0u || userd == NULL || gpfifo == NULL ||
        pushbuffer == NULL || pushbufferGpuVa == 0u || Dev->Bar1Cpu == NULL ||
        Dev->Bar1MapLen < BAR1_D3_MAP_LEN || !g_GrVm.Active ||
        instWords == NULL || ctxPointerOk == 0u ||
        semGpuVa + sizeof(reportWords) >
            (ULONGLONG)GR_VM_GPU_VA + GR_VM_ARENA_SIZE) {
        result = 0xE00000A2u;
        goto done;
    }
    bar1GpPut = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu);
    semSlot = (volatile ULONG *)((PUCHAR)pushbuffer + PAGE_SIZE - 32u);

    /* SET_OBJECT(class B197), B197_NO_OPERATION, then host SEMAPHORE RELEASE.
     * Header lengths are in dwords; GPFIFO entry 1 is the second 8-byte slot. */
    pushWords[0] = 0x20010000u; /* INC, count 1, subch 0, SET_OBJECT */
    pushWords[1] = 0x0000B197u;
    pushWords[2] = 0x20010040u; /* INC, count 1, subch 0, NO_OPERATION 0x100 */
    pushWords[3] = 0u;
    pushWords[4] = 0x20040004u; /* INC, count 4, subch 0, SEMAPHOREA 0x10 */
    pushWords[5] = (ULONG)(semGpuVa >> 32);
    pushWords[6] = (ULONG)semGpuVa;
    pushWords[7] = 0xD4D4D4D4u;
    pushWords[8] = 0x01000002u; /* RELEASE, four-word payload, release_wfi enabled */
    for (i = 0u; i < RTL_NUMBER_OF(pushWords); ++i) pushbuffer[i] = pushWords[i];
    for (i = 0u; i < RTL_NUMBER_OF(reportWords); ++i) semSlot[i] = 0u;
    gpWords[0] = (ULONG)pushbufferGpuVa & ~3u;
    gpWords[1] = (ULONG)((pushbufferGpuVa >> 32) & 0xFFu) | (9u << 10);
    gpfifo[2] = gpWords[0];
    gpfifo[3] = gpWords[1];
    KeMemoryBarrier();

    if (!B3cPrivRingDrainBeforeWrites(B)) {
        result = 0xE00000A2u;
        goto done;
    }
    *bar1GpPut = 2u;
    KeMemoryBarrier();
    bar1GpPutReadback = *bar1GpPut;
    if (bar1GpPutReadback != 2u || Rd(B, 0x00002A30u) != 0u ||
        (Rd(B, 0x00002100u) & 0x10000000u) != 0u) {
        result = 0xE00000A2u;
        goto done;
    }

    for (polls = 0u; polls < 2000u; ++polls) {
        userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
        pbGpGet = Rd(B, 0x00040014u);
        if (userdGpGet == 2u && pbGpGet == 2u) break;
        Us(1u);
    }
    userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
    pbGpGet = Rd(B, 0x00040014u);
    if (userdGpGet != 2u || pbGpGet != 2u) {
        result = 0xE00000A2u;
        goto done;
    }

    /* Keep L2 maintenance bounded and record each register write through the
     * common write/priv-ring trace. */
    if (!B3cD2Write(B, writes, &writeCount, 0x00070010u, 1u)) {
        result = 0xE00000A2u;
        goto done;
    }
    for (l2FlushPolls = 0u; l2FlushPolls < 2000u; ++l2FlushPolls) {
        l2Status = Rd(B, 0x00070010u);
        if ((l2Status & 3u) == 0u) break;
        Us(5u);
    }
    if (l2FlushPolls == 2000u ||
        !B3cD2Write(B, writes, &writeCount, 0x00070004u, 1u)) {
        result = 0xE00000A2u;
        goto done;
    }
    for (l2InvalidatePolls = 0u; l2InvalidatePolls < 2000u; ++l2InvalidatePolls) {
        l2Status = Rd(B, 0x00070004u);
        if ((l2Status & 3u) == 0u) break;
        Us(5u);
    }
    if (l2InvalidatePolls == 2000u) {
        result = 0xE00000A2u;
        goto done;
    }
    KeMemoryBarrier();
    semValue = semSlot[0];
    /* Claude 0.4.76: GP_GET == 2 only means the entry was fetched. The WFI
     * release is written after GR finishes the ctx load and the methods, so
     * 0.4.75 read too early (all slots 0 while GR was already idle at the
     * diag dump). Poll the slot with an L2 flush/invalidate per round,
     * bounded at 2000 rounds x 10 us plus the flush polls. */
    {
        ULONG semPolls, l2Fails = 0u;
        for (semPolls = 0u; semPolls < 2000u && semValue != 0xD4D4D4D4u; ++semPolls) {
            ULONG l2Round[4] = { 0 };
            if (!B3cC2eL2Flush(B, l2Round)) ++l2Fails;
            KeMemoryBarrier();
            semValue = semSlot[0];
            if (semValue == 0xD4D4D4D4u) break;
            Us(10u);
        }
        Trace(L"B3cD4SemPolls", semPolls);
        TraceDetail(L"B3cD4SemL2Fails", l2Fails);
        TraceDetail(L"B3cD4SemEngineStatusAfterPoll", Rd(B, 0x00002640u));
        TraceDetail(L"B3cD4SemGrStatusAfterPoll", Rd(B, 0x00400700u));
    }
    refCount = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x48u);
    if (semValue != 0xD4D4D4D4u || userdGpGet != 2u || pbGpGet != 2u) {
        result = 0xE00000A2u;
        B3cD4CaptureCtxswDiag(B, (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF,
                              writes, &writeCount, FALSE);
        diagCaptured = 1u;
    }

done:
    if (result != 0u && userdGpGet == 2u && !diagCaptured && g_GrVm.Arena != NULL) {
        B3cD4CaptureCtxswDiag(B, (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF,
                              writes, &writeCount, FALSE);
        diagCaptured = 1u;
    }
    if (Dev->Bar1Cpu != NULL && Dev->Bar1MapLen >= BAR1_D3_MAP_LEN) {
        if (bar1GpPut == NULL)
            bar1GpPut = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu);
        bar1GpPutReadback = *bar1GpPut;
        userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
        refCount = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x48u);
    }
    pbGet = Rd(B, 0x00040018u);
    pbPut = Rd(B, 0x0004005Cu);
    pbIntr0 = Rd(B, 0x00040108u);
    pbIntr1 = Rd(B, 0x00040148u);
    pbdmaStatus = Rd(B, 0x00003080u);
    pbMethod0 = Rd(B, 0x000400C0u);
    pbData0 = Rd(B, 0x000400C4u);
    fifoIntr0 = Rd(B, 0x00002100u);
    mmuFault = Rd(B, 0x00002A30u);
    grIntr = (grEnabled & (1u << 12)) ? Rd(B, 0x00400100u) : 0u;
    grException = (grEnabled & (1u << 12)) ? Rd(B, 0x00400108u) : 0u;
    grStatus = (grEnabled & (1u << 12)) ? Rd(B, 0x00400700u) : 0u;
    feHwwEsr = (grEnabled & (1u << 12)) ? Rd(B, 0x00404000u) : 0u;
    fecsCurrentCtx = (grEnabled & (1u << 12)) ? Rd(B, 0x00409B00u) : 0u;
    fecsMailbox0 = (grEnabled & (1u << 12)) ? Rd(B, 0x00409800u) : 0u;
    engineStatus = Rd(B, 0x00002640u);
    if (semSlot != NULL) {
        for (i = 0u; i < RTL_NUMBER_OF(reportWords); ++i) reportWords[i] = semSlot[i];
        semValue = reportWords[0];
    }
    TraceDetail(L"B3cD4GpGet", userdGpGet);
    TraceDetail(L"B3cD4GpPut", bar1GpPutReadback);
    TraceDetail(L"B3cD4PbGet", pbGet);
    TraceDetail(L"B3cD4PbPut", pbPut);
    TraceDetail(L"B3cD4PbdmaGpGet", pbGpGet);
    TraceDetail(L"B3cD4SemWfi", pushWords[8] == 0x01000002u ? 1u : 0u);
    TraceBin(L"B3cD4ReportSem", reportWords, sizeof(reportWords));
    TraceDetail(L"B3cD4PbIntr0", pbIntr0);
    TraceDetail(L"B3cD4PbIntr1", pbIntr1);
    TraceDetail(L"B3cD4PbMethod0", pbMethod0);
    TraceDetail(L"B3cD4PbData0", pbData0);
    TraceDetail(L"B3cD4FifoIntr0", fifoIntr0);
    TraceDetail(L"B3cD4MmuFault", mmuFault);
    TraceDetail(L"B3cD4GrIntr", grIntr);
    TraceDetail(L"B3cD4GrException", grException);
    TraceDetail(L"B3cD4GrStatus", grStatus);
    TraceDetail(L"B3cD4FeHwwEsr", feHwwEsr);
    TraceDetail(L"B3cD4FecsCurrentCtx", fecsCurrentCtx);
    TraceDetail(L"B3cD4FecsMailbox0", fecsMailbox0);
    TraceDetail(L"B3cD4EngineStatus", engineStatus);
    Trace(L"B3cD4GpGetPolls", polls);
    Trace(L"B3cD4L2FlushPolls", l2FlushPolls);
    Trace(L"B3cD4L2InvalidatePolls", l2InvalidatePolls);
    TraceDetail(L"B3cD4L2Status", l2Status);
    TraceDetail(L"B3cD4PbdmaStatus", pbdmaStatus);
    TraceBin(L"B3cD4PushbufferWords", pushWords, sizeof(pushWords));
    TraceBin(L"B3cD4GpfifoEntry", gpWords, sizeof(gpWords));
    TraceBin(L"B3cD4Writes", writes, sizeof(writes));
    TraceDetail(L"B3cD4RefCount", refCount);
    if (grIntr != 0u) {
        ULONG grDump[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0u };
        B3cCaptureGrStatusDump(B, L"B3cD4GrStatusDump", grDump);
    }
    TraceDurable(L"B3cD4Result", result);
    return result;
}

/* D5: issue a pitch-linear Maxwell B 3D color clear through channel 0.
 * The method offsets/data are from NVIDIA MIT open-gpu-doc classes/3d/clb197.h.
 * The host semaphore is kept in the already mapped pushbuffer page, separate
 * from the 16 KiB render target at GR_VM_D2_RT_OFF. */
static ULONG B3cRunD5(PSWGPU_DEVICE Dev, PULONG userd, PULONG gpfifo,
                      PULONG pushbuffer, ULONGLONG pushbufferGpuVa)
{
    volatile UCHAR *B = Dev->Bar0;
    volatile ULONG *bar1GpPut = NULL;
    volatile ULONG *semSlot = NULL;
    PULONG renderTarget = NULL;
    PULONGLONG gpuPgt = NULL;
    ULONGLONG renderGpuVa = (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_RT_OFF;
    ULONGLONG semGpuVa = (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE - 64u;
    ULONG result = 0u, grEnabled = 0u, cursor = 0u, methodCount = 0u;
    ULONG gpWords[2] = { 0u, 0u }, methodTrace[40][2] = { { 0u } };
    ULONG semWords[4] = { 0u };
    ULONG gpGetPolls = 0u, semPolls = 0u, l2Fails = 0u;
    ULONG userdGpGet = 0u, pbGpGet = 0u, pbGet = 0u, pbPut = 0u;
    ULONG semValue = 0u, pbIntr0 = 0u, pbIntr1 = 0u, pbMethod0 = 0u;
    ULONG pbData0 = 0u, fifoIntr0 = 0u, mmuFault = 0u, pbdmaStatus = 0u;
    ULONG grIntr = 0u, grException = 0u, grStatus = 0u, engineStatus = 0u;
    ULONG feEsr = 0u, dsEsr = 0u, sccEsr = 0u, pdEsr = 0u, mmeEsr = 0u, skedEsr = 0u;
    ULONG expected = 0xFFFF00FFu, expectedPixels = 0u, stalePixels = 0u;
    ULONG rtPteIndex = 0u, rtPteExpectedLo = 0u, rtPteReadLo = 0u;
    ULONGLONG rtPteExpected = 0u, rtPteRead = 0u;
    ULONG otherPixels = 0u, firstPixel = 0u, middlePixel = 0u, lastPixel = 0u;
    ULONG firstMismatchOffset = 0xFFFFFFFFu, firstMismatchValue = 0u;
    ULONG uniformValue = 0u, uniform = 1u;
    ULONG (*writes)[4] = NULL;
    ULONG diagWrites[64][4] = { { 0u } }, diagWriteCount = 0u;
    ULONG writeSamples[8][4] = { { 0u } };
    ULONG writeCount = 0u, writeCapacity = 8192u, surfaceScanned = 0u;
    ULONG l2State[4] = { 0u }, i;

    Step(109); /* D4 already uses Step 108 in B3cRunD4. */
    B3cPrivRingResetWriteLog();
    grEnabled = Rd(B, NV_PMC_ENABLE);
    if ((grEnabled & (1u << 12)) == 0u || userd == NULL || gpfifo == NULL ||
        pushbuffer == NULL || pushbufferGpuVa == 0u || Dev->Bar1Cpu == NULL ||
        Dev->Bar1MapLen < BAR1_D3_MAP_LEN || !g_GrVm.Active || g_GrVm.Arena == NULL ||
        GR_VM_D2_RT_OFF + 64u * 64u * sizeof(ULONG) > GR_VM_ARENA_SIZE ||
        semGpuVa + 16u > (ULONGLONG)GR_VM_GPU_VA + GR_VM_ARENA_SIZE) {
        result = 0xE00000A4u;
        goto done;
    }
    writes = (ULONG (*)[4])ExAllocatePool2(POOL_FLAG_NON_PAGED,
        (SIZE_T)writeCapacity * sizeof(writes[0]), SWGPU_TAG);
    if (writes == NULL) { result = 0xE00000A4u; goto done; }
    RtlZeroMemory(writes, (SIZE_T)writeCapacity * sizeof(writes[0]));
    bar1GpPut = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu);
    semSlot = (volatile ULONG *)((PUCHAR)pushbuffer + PAGE_SIZE - 64u);
    renderTarget = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_RT_OFF);
    gpuPgt = (PULONGLONG)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGT_OFF);
    rtPteIndex = (ULONG)((renderGpuVa >> 12) & 0x7FFFu);
    rtPteExpected = ((g_GrVm.Iova + GR_VM_D2_RT_OFF) >> 8) | 1ull;
    rtPteExpectedLo = (ULONG)rtPteExpected;
    rtPteRead = gpuPgt[rtPteIndex];
    rtPteReadLo = (ULONG)rtPteRead;
    if (rtPteRead != rtPteExpected) {
        result = 0xE00000A4u; goto done;
    }

    /* Prefill distinctly from zero and make the CPU contents visible before
     * the GPU's write; every 32-bit pixel is initialized within the arena. */
    for (i = 0u; i < 64u * 64u; ++i) renderTarget[i] = 0x11111111u;
    for (i = 0u; i < 4u; ++i) semSlot[i] = 0u;
    if (!B3cD5Write(B, writes, &writeCount, writeCapacity, 0x00070010u, 1u)) {
        result = 0xE00000A4u; goto done;
    }
    for (i = 0u; i < 2000u; ++i) {
        l2State[1] = Rd(B, 0x00070010u);
        if ((l2State[1] & 3u) == 0u) break;
        Us(5u);
    }
    l2State[0] = i;
    if (i == 2000u || !B3cD5Write(B, writes, &writeCount, writeCapacity, 0x00070004u, 1u)) {
        result = 0xE00000A4u; goto done;
    }
    for (i = 0u; i < 2000u; ++i) {
        l2State[3] = Rd(B, 0x00070004u);
        if ((l2State[3] & 3u) == 0u) break;
        Us(5u);
    }
    l2State[2] = i;
    if (i == 2000u) { result = 0xE00000A4u; goto done; }

    /* Each INC packet below is a single Maxwell B 3D method.  SET_OBJECT
     * reasserts the already bound B197 class on subchannel zero. */
#define D5_METHOD(m, v) do { \
        if (cursor + 2u >= PAGE_SIZE / sizeof(ULONG) - 16u || methodCount >= RTL_NUMBER_OF(methodTrace)) { result = 0xE00000A4u; goto done; } \
        pushbuffer[cursor++] = 0x20010000u | ((ULONG)(m) >> 2); \
        pushbuffer[cursor++] = (ULONG)(v); \
        methodTrace[methodCount][0] = (ULONG)(m); methodTrace[methodCount][1] = (ULONG)(v); ++methodCount; \
    } while (0)
    D5_METHOD(0x0000u, 0x0000B197u); /* SET_OBJECT */
    D5_METHOD(0x121Cu, 0x00000001u); /* SET_CT_SELECT: one target, target 0 */
    D5_METHOD(0x0800u, (ULONG)(renderGpuVa >> 32)); /* SET_COLOR_TARGET_A(0) */
    D5_METHOD(0x0804u, (ULONG)renderGpuVa); /* SET_COLOR_TARGET_B(0) */
    D5_METHOD(0x0808u, 256u); /* SET_COLOR_TARGET_WIDTH(0): pitch layout takes the pitch in bytes (64 px x 4), as nouveau nvc0_validate_fb (Claude 0.4.77) */
    D5_METHOD(0x080Cu, 64u); /* SET_COLOR_TARGET_HEIGHT(0) */
    D5_METHOD(0x0810u, 0xCFu); /* SET_COLOR_TARGET_FORMAT(0): A8R8G8B8 */
    D5_METHOD(0x0814u, 0x00001000u); /* SET_COLOR_TARGET_MEMORY(0): pitch layout */
    D5_METHOD(0x0818u, 1u); /* SET_COLOR_TARGET_THIRD_DIMENSION(0) */
    D5_METHOD(0x081Cu, 0u); /* SET_COLOR_TARGET_ARRAY_PITCH(0): one layer, zero layer stride */
    D5_METHOD(0x0820u, 0u); /* SET_COLOR_TARGET_LAYER(0) */
    D5_METHOD(0x0FE0u, 0u); /* SET_ZT_A: no depth surface */
    D5_METHOD(0x0FE4u, 0u); /* SET_ZT_B */
    D5_METHOD(0x1538u, 0u); /* SET_ZT_SELECT: zero targets */
    D5_METHOD(0x0FF4u, 0x00400000u); /* SET_SURFACE_CLIP_HORIZONTAL: 0..64 */
    D5_METHOD(0x0FF8u, 0x00400000u); /* SET_SURFACE_CLIP_VERTICAL: 0..64 */
    D5_METHOD(0x0DF8u, 0u); /* SET_WINDOW_OFFSET_X */
    D5_METHOD(0x0DFCu, 0u); /* SET_WINDOW_OFFSET_Y */
    D5_METHOD(0x0E00u, 0u); /* SET_SCISSOR_ENABLE(0): disabled */
    D5_METHOD(0x10F8u, 0x00000010u); /* SET_CLEAR_SURFACE_CONTROL: use clear rect */
    D5_METHOD(0x0D6Cu, 0x00400000u); /* SET_CLEAR_RECT_HORIZONTAL: [0,64) */
    D5_METHOD(0x0D70u, 0x00400000u); /* SET_CLEAR_RECT_VERTICAL: [0,64) */
    D5_METHOD(0x0D80u, 0x3F800000u); /* SET_COLOR_CLEAR_VALUE(0): R=1 */
    D5_METHOD(0x0D84u, 0x00000000u); /* SET_COLOR_CLEAR_VALUE(1): G=0 */
    D5_METHOD(0x0D88u, 0x3F800000u); /* SET_COLOR_CLEAR_VALUE(2): B=1 */
    D5_METHOD(0x0D8Cu, 0x3F800000u); /* SET_COLOR_CLEAR_VALUE(3): A=1 */
    /* Claude 0.4.79: 0.4.78 hw trapped CLASS_ERROR code 4 at method 0x0FA4
     * data 0x10 (gr_trapped_addr). The sample-mask / MRT / AA methods
     * (0x0FA4, 0x0FA8, 0x0FAC, 0x0FB8) are not needed for a 1x clear and
     * were removed; the golden-context defaults stay in effect. */
    D5_METHOD(0x19D0u, 0x0000003Cu); /* CLEAR_SURFACE: RGBA, target 0, layer 0 */
    if (cursor + 6u >= PAGE_SIZE / sizeof(ULONG) - 16u) { result = 0xE00000A4u; goto done; }
    pushbuffer[cursor++] = 0x20040004u; /* host SEMAPHOREA..D, release_wfi enabled */
    pushbuffer[cursor++] = (ULONG)(semGpuVa >> 32);
    pushbuffer[cursor++] = (ULONG)semGpuVa;
    pushbuffer[cursor++] = 0xD5D5D5D5u;
    pushbuffer[cursor++] = 0x01000002u;
#undef D5_METHOD
    methodTrace[methodCount][0] = 0xFFFFFFFFu;
    methodTrace[methodCount][1] = 0xD5D5D5D5u;
    ++methodCount;
    KeMemoryBarrier();
    gpWords[0] = (ULONG)pushbufferGpuVa & ~3u;
    gpWords[1] = (ULONG)((pushbufferGpuVa >> 32) & 0xFFu) | (cursor << 10);
    gpfifo[4] = gpWords[0]; gpfifo[5] = gpWords[1];
    KeMemoryBarrier();
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE00000A4u; goto done; }
    *bar1GpPut = 3u;
    KeMemoryBarrier();
    if (*bar1GpPut != 3u || Rd(B, 0x00002A30u) != 0u ||
        (Rd(B, 0x00002100u) & 0x10000000u) != 0u) {
        result = 0xE00000A4u; goto done;
    }
    for (gpGetPolls = 0u; gpGetPolls < 2000u; ++gpGetPolls) {
        userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
        pbGpGet = Rd(B, 0x00040014u);
        if (userdGpGet == 3u && pbGpGet == 3u) break;
        Us(1u);
    }
    userdGpGet = *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
    pbGpGet = Rd(B, 0x00040014u);
    pbGet = Rd(B, 0x00040018u); pbPut = Rd(B, 0x0004005Cu);
    if (userdGpGet != 3u || pbGpGet != 3u) { result = 0xE00000A4u; goto done; }
    for (semPolls = 0u; semPolls < 2000u && semValue != 0xD5D5D5D5u; ++semPolls) {
        ULONG roundState[4] = { 0u };
        if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, roundState)) ++l2Fails;
        KeMemoryBarrier(); semValue = semSlot[0];
        if (semValue == 0xD5D5D5D5u) break;
        Us(10u);
    }
    if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) ++l2Fails;
    KeMemoryBarrier();
    for (i = 0u; i < 64u * 64u; ++i) {
        ULONG pixel = renderTarget[i];
        if (i == 0u) firstPixel = pixel;
        else if (pixel != firstPixel) uniform = 0u;
        if (i == 2048u) middlePixel = pixel;
        if (i == 4095u) lastPixel = pixel;
        if (pixel == expected) ++expectedPixels;
        else if (pixel == 0x11111111u) ++stalePixels;
        else {
            ++otherPixels;
            if (firstMismatchOffset == 0xFFFFFFFFu) { firstMismatchOffset = i * sizeof(ULONG); firstMismatchValue = pixel; }
        }
    }
    surfaceScanned = 1u;
    uniformValue = firstPixel;
    if (semValue == 0xD5D5D5D5u && expectedPixels == 4096u) result = 0u;
    else result = 0xE00000A4u;

done:
    if (renderTarget != NULL && surfaceScanned == 0u) {
        if (writes != NULL) {
            if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) ++l2Fails;
        }
        KeMemoryBarrier();
        for (i = 0u; i < 64u * 64u; ++i) {
            ULONG pixel = renderTarget[i];
            if (i == 0u) firstPixel = pixel;
            else if (pixel != firstPixel) uniform = 0u;
            if (i == 2048u) middlePixel = pixel;
            if (i == 4095u) lastPixel = pixel;
            if (pixel == expected) ++expectedPixels;
            else if (pixel == 0x11111111u) ++stalePixels;
            else {
                ++otherPixels;
                if (firstMismatchOffset == 0xFFFFFFFFu) {
                    firstMismatchOffset = i * sizeof(ULONG); firstMismatchValue = pixel;
                }
            }
        }
        surfaceScanned = 1u;
        uniformValue = firstPixel;
    }
    userdGpGet = (Dev->Bar1Cpu != NULL && Dev->Bar1MapLen >= BAR1_D3_MAP_LEN) ?
        *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u) : userdGpGet;
    pbGpGet = Rd(B, 0x00040014u); pbGet = Rd(B, 0x00040018u); pbPut = Rd(B, 0x0004005Cu);
    pbIntr0 = Rd(B, 0x00040108u); pbIntr1 = Rd(B, 0x00040148u);
    pbMethod0 = Rd(B, 0x000400C0u); pbData0 = Rd(B, 0x000400C4u);
    fifoIntr0 = Rd(B, 0x00002100u); mmuFault = Rd(B, 0x00002A30u);
    pbdmaStatus = Rd(B, 0x00003080u); engineStatus = Rd(B, 0x00002640u);
    if ((grEnabled & (1u << 12)) != 0u) {
        grIntr = Rd(B, 0x00400100u); grException = Rd(B, 0x00400108u); grStatus = Rd(B, 0x00400700u);
        feEsr = Rd(B, 0x00404000u); dsEsr = Rd(B, 0x0040C000u); sccEsr = Rd(B, 0x00405800u);
        pdEsr = Rd(B, 0x00410A00u); mmeEsr = Rd(B, 0x00403000u); skedEsr = Rd(B, 0x00407000u);
    }
    if (semSlot != NULL) {
        for (i = 0u; i < RTL_NUMBER_OF(semWords); ++i) semWords[i] = semSlot[i];
        semValue = semWords[0];
    }
    TraceDetail(L"B3cD5Step", 109u); TraceDetail(L"B3cD5RtOffset", GR_VM_D2_RT_OFF);
    TraceDetail(L"B3cD5RtGpuVa", (ULONG)renderGpuVa); TraceDetail(L"B3cD5RtBytes", 64u * 64u * sizeof(ULONG));
    TraceDetail(L"B3cD5RtPteIndex", rtPteIndex); TraceDetail(L"B3cD5RtPteExpectedLo", rtPteExpectedLo);
    TraceDetail(L"B3cD5RtPteExpectedHi", (ULONG)(rtPteExpected >> 32));
    TraceDetail(L"B3cD5RtPteReadLo", rtPteReadLo); TraceDetail(L"B3cD5RtPteReadHi", (ULONG)(rtPteRead >> 32));
    TraceDetail(L"B3cD5Pitch", 256u); TraceDetail(L"B3cD5ExpectedPixel", expected);
    Trace(L"B3cD5GpGetPolls", gpGetPolls); Trace(L"B3cD5SemPolls", semPolls);
    Trace(L"B3cD5L2Polls", l2State[0]); Trace(L"B3cD5L2InvalidatePolls", l2State[2]); TraceDetail(L"B3cD5L2Fails", l2Fails);
    TraceDetail(L"B3cD5UserdGpGet", userdGpGet); TraceDetail(L"B3cD5PbGpGet", pbGpGet);
    TraceDetail(L"B3cD5PbGet", pbGet); TraceDetail(L"B3cD5PbPut", pbPut); TraceDetail(L"B3cD5SemValue", semValue);
    TraceDetail(L"B3cD5SemWfi", 1u); TraceBin(L"B3cD5SemaphoreWords", semWords, sizeof(semWords));
    TraceDetail(L"B3cD5ExpectedPixels", expectedPixels); TraceDetail(L"B3cD5StalePixels", stalePixels);
    TraceDetail(L"B3cD5OtherPixels", otherPixels); TraceDetail(L"B3cD5Uniform", uniform);
    TraceDetail(L"B3cD5UniformPartial", (uniform != 0u && expectedPixels != 4096u) ? 1u : 0u);
    TraceDetail(L"B3cD5UniformValue", uniformValue); TraceDetail(L"B3cD5FirstPixel", firstPixel);
    TraceDetail(L"B3cD5MiddlePixel", middlePixel); TraceDetail(L"B3cD5LastPixel", lastPixel);
    TraceDetail(L"B3cD5FirstMismatchOffset", firstMismatchOffset); TraceDetail(L"B3cD5FirstMismatchValue", firstMismatchValue);
    TraceDetail(L"B3cD5PbIntr0", pbIntr0); TraceDetail(L"B3cD5PbIntr1", pbIntr1);
    TraceDetail(L"B3cD5PbMethod0", pbMethod0); TraceDetail(L"B3cD5PbData0", pbData0);
    TraceDetail(L"B3cD5FifoIntr0", fifoIntr0); TraceDetail(L"B3cD5MmuFault", mmuFault); TraceDetail(L"B3cD5PbdmaStatus", pbdmaStatus);
    TraceDetail(L"B3cD5GrIntr", grIntr); TraceDetail(L"B3cD5GrException", grException); TraceDetail(L"B3cD5GrStatus", grStatus);
    /* Claude 0.4.78: 0.4.77 hw raised GR intr bit 20 (CLASS_ERROR). Record
     * the nvgpu gk20a_gr_isr trap set (read-only): gr_class_error (code),
     * gr_trapped_addr (method 13:2, subch 18:16, status 31), trapped data. */
    TraceDetail(L"B3cD5GrClassError", Rd(B, 0x00400110u));
    TraceDetail(L"B3cD5GrTrappedAddr", Rd(B, 0x00400704u));
    TraceDetail(L"B3cD5GrTrappedMethod", (Rd(B, 0x00400704u) & 0x3FFCu));
    TraceDetail(L"B3cD5GrTrappedDataLo", Rd(B, 0x00400708u));
    TraceDetail(L"B3cD5GrTrappedDataHi", Rd(B, 0x0040070Cu));
    TraceDetail(L"B3cD5GrFeObjectTable0", Rd(B, 0x00404200u));
    TraceDetail(L"B3cD5EngineStatus", engineStatus); TraceDetail(L"B3cD5FeHwwEsr", feEsr);
    TraceDetail(L"B3cD5DsHwwEsr", dsEsr); TraceDetail(L"B3cD5SccHwwEsr", sccEsr);
    TraceDetail(L"B3cD5PdHwwEsr", pdEsr); TraceDetail(L"B3cD5MmeHwwEsr", mmeEsr); TraceDetail(L"B3cD5SkedHwwEsr", skedEsr);
    TraceDetail(L"B3cD5WriteCount", writeCount);
    for (i = 0u; i < writeCount && i < RTL_NUMBER_OF(writeSamples); ++i)
        RtlCopyMemory(writeSamples[i], writes[i], sizeof(writeSamples[i]));
    TraceBin(L"B3cD5Methods", methodTrace, methodCount * sizeof(methodTrace[0]));
    TraceBin(L"B3cD5GpfifoEntry", gpWords, sizeof(gpWords));
    TraceBin(L"B3cD5WriteSamples", writeSamples, sizeof(writeSamples));
    if (result != 0u) {
        ULONG grDump[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0u };
        if ((grEnabled & (1u << 12)) != 0u) {
            B3cCaptureGrStatusDump(B, L"B3cD5GrStatusDump", grDump);
            if (g_GrVm.Arena != NULL)
                B3cD4CaptureCtxswDiag(B, (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF,
                    diagWrites, &diagWriteCount, FALSE);
        }
    }
    if (writes != NULL)
        TraceBin(L"B3cD5PrivRingWrites", writes, writeCount * sizeof(writes[0]));
    TraceBin(L"B3cD5DiagWrites", diagWrites, diagWriteCount * sizeof(diagWrites[0]));
    B3cPrivRingTraceWriteLog(L"B3cD5PrivErrorCount", L"B3cD5PrivErrorDropped",
        L"B3cD5PrivAckTimeouts", L"B3cD5PrivErrors");
    TraceDurable(L"B3cD5Result", result);
    if (writes != NULL) ExFreePoolWithTag(writes, SWGPU_TAG);
    return result;
}

/* E2 runs sequentially on the already-resident D2 channel. Reusing the single
 * mapped pushbuffer/RT pages is safe because each round waits for GP_GET and
 * the WFI semaphore before overwriting them. Ring size is the 4 KiB GPFIFO
 * page (512 8-byte entries), and GP_PUT is advanced modulo that size. */
static ULONG B3eResidentSubmitTest(PSWGPU_DEVICE Dev, PSWGPU_E2_RESULT Out, BOOLEAN Quick)
{
    volatile UCHAR *B = Dev != NULL ? Dev->Bar0 : NULL;
    volatile ULONG *bar1GpPut;
    volatile ULONG *bar1GpGet;
    volatile ULONG *semSlot;
    PULONG ring;
    PULONG push;
    PULONG pixels;
    ULONG (*writes)[4] = NULL;
    ULONG (*history)[4] = NULL;
    ULONG writeCount = 0u, writeCapacity = 16u;
    ULONG ringEntries = PAGE_SIZE / 8u, rounds = Quick ? 8u : 2u * (PAGE_SIZE / 8u) + 3u;
    ULONG put = 0u, newPut = 0u, gpGet = 0u, pbGpGet = 0u;
    ULONG round = 0u, doneRounds = 0u, wraps = 0u, maxPolls = 0u;
    ULONG gpPolls = 0u, semPolls = 0u, l2State[4] = { 0u };
    ULONG cursor = 0u, colorIndex = 0u, color = 0u, semaphore = 0u, semValue = 0u;
    ULONG expectedFirst = 0u, expectedMiddle = 0u, expectedLast = 0u;
    ULONG gotFirst = 0u, gotMiddle = 0u, gotLast = 0u;
    ULONG result = 0u, failure = 0u, grEnabled = 0u;
    ULONG channel = 0u, pbdmaStatus = 0u, pbdmaIntr0 = 0u, fifoIntr0 = 0u;
    ULONG grIntr = 0u, grException = 0u, grStatus = 0u, mmuFault = 0u;
    ULONG classError = 0u, trappedAddr = 0u, trappedDataLo = 0u, trappedDataHi = 0u;
    ULONG totalWriteCount = 0u, i = 0u;
    LARGE_INTEGER qpcStart, qpcEnd, qpcFrequency;
    ULONGLONG elapsedTicks = 0u, totalUs = 0u;

    if (Out == NULL) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    Out->RingEntries = ringEntries;
    Out->Rounds = rounds;
    if (Dev == NULL || !Dev->ResidentActive || g_ResidentOwner != Dev) {
        result = STATUS_DEVICE_NOT_READY;
        goto done;
    }
    Step(113);
    B3cPrivRingResetWriteLog();
    grEnabled = Rd(B, NV_PMC_ENABLE);
    if ((grEnabled & (1u << 12)) == 0u || !g_GrVm.Active || g_GrVm.Arena == NULL ||
        Dev->Bar1Cpu == NULL || Dev->Bar1MapLen < BAR1_D3_MAP_LEN ||
        !Dev->ResidentChannelEnabled || !Dev->ResidentRunlistSubmitted ||
        !Dev->ResidentInstBound || ringEntries < 8u || rounds > 4096u ||
        GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_RT_OFF + 64u * 64u * sizeof(ULONG) > GR_VM_ARENA_SIZE ||
        GR_VM_D2_GPFIFO_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_USERD_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE) {
        result = 0xE00000A6u;
        goto done;
    }
    ring = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_GPFIFO_OFF);
    push = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_PUSHBUF_OFF);
    pixels = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_RT_OFF);
    semSlot = (volatile ULONG *)((PUCHAR)push + PAGE_SIZE - 64u);
    bar1GpPut = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu);
    bar1GpGet = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
    writes = (ULONG (*)[4])ExAllocatePool2(POOL_FLAG_NON_PAGED,
        writeCapacity * sizeof(writes[0]), SWGPU_TAG);
    if (g_Verbose) history = (ULONG (*)[4])ExAllocatePool2(POOL_FLAG_NON_PAGED,
        rounds * sizeof(history[0]), SWGPU_TAG);
    if (writes == NULL || (g_Verbose && history == NULL)) { result = 0xE00000A6u; goto done; }
    RtlZeroMemory(writes, writeCapacity * sizeof(writes[0]));
    if (history != NULL) RtlZeroMemory(history, rounds * sizeof(history[0]));
    put = *bar1GpPut;
    if (put >= ringEntries) { result = 0xE00000A6u; goto done; }
    qpcStart = KeQueryPerformanceCounter(&qpcFrequency);
    Trace(L"E2StartGpPut", put);
    Trace(L"E2RingEntries", ringEntries);
    Trace(L"E2PlannedRounds", rounds);

    for (round = 0u; round < rounds; ++round) {
        /* The known-good D5 clear maps float 1.0 to a full 8-bit channel.
         * Cycle all eight RGB bit patterns; alpha remains opaque. */
        cursor = 0u;
        colorIndex = round & 7u;
        color = 0xFF000000u |
            ((colorIndex & 1u) ? 0x00FF0000u : 0u) |
            ((colorIndex & 2u) ? 0x0000FF00u : 0u) |
            ((colorIndex & 4u) ? 0x000000FFu : 0u);
        semaphore = 0xE2000000u | (round + 1u);
        semValue = 0u;
        if (round == 0u) { Out->FirstColor = color; Out->FirstSemaphore = semaphore; }
        Out->LastColor = color; Out->LastSemaphore = semaphore;
        expectedFirst = color; expectedMiddle = color; expectedLast = color;
        for (i = 0u; i < 64u * 64u; ++i) pixels[i] = 0x11111111u;
        for (i = 0u; i < 4u; ++i) semSlot[i] = 0u;

#define E2_METHOD(method, value) do { \
        if (cursor + 2u >= PAGE_SIZE / sizeof(ULONG) - 16u) { failure = 1u; goto round_fail; } \
        push[cursor++] = 0x20010000u | ((ULONG)(method) >> 2); \
        push[cursor++] = (ULONG)(value); \
    } while (0)
        E2_METHOD(0x0000u, 0x0000B197u); /* SET_OBJECT: Maxwell B 3D */
        E2_METHOD(0x121Cu, 1u); /* SET_CT_SELECT: one color target, target 0 */
        E2_METHOD(0x0800u, 0u); /* SET_COLOR_TARGET_A: high address bits */
        E2_METHOD(0x0804u, (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_RT_OFF); /* target VA */
        E2_METHOD(0x0808u, 256u); /* pitch bytes, as validated by D5 */
        E2_METHOD(0x080Cu, 64u); E2_METHOD(0x0810u, 0xCFu);
        E2_METHOD(0x0814u, 0x1000u); E2_METHOD(0x0818u, 1u);
        E2_METHOD(0x081Cu, 0u); E2_METHOD(0x0820u, 0u);
        E2_METHOD(0x0FE0u, 0u); E2_METHOD(0x0FE4u, 0u);
        E2_METHOD(0x1538u, 0u); E2_METHOD(0x0FF4u, 0x00400000u);
        E2_METHOD(0x0FF8u, 0x00400000u); E2_METHOD(0x0DF8u, 0u);
        E2_METHOD(0x0DFCu, 0u); E2_METHOD(0x0E00u, 0u);
        E2_METHOD(0x10F8u, 0x10u); E2_METHOD(0x0D6Cu, 0x00400000u);
        E2_METHOD(0x0D70u, 0x00400000u);
        E2_METHOD(0x0D80u, (colorIndex & 1u) ? 0x3F800000u : 0u); /* R */
        E2_METHOD(0x0D84u, (colorIndex & 2u) ? 0x3F800000u : 0u); /* G */
        E2_METHOD(0x0D88u, (colorIndex & 4u) ? 0x3F800000u : 0u); /* B */
        E2_METHOD(0x0D8Cu, 0x3F800000u); /* A = 1.0 */
        E2_METHOD(0x19D0u, 0x3Cu); /* CLEAR_SURFACE: RGBA target 0 */
#undef E2_METHOD
        if (cursor + 5u >= PAGE_SIZE / sizeof(ULONG) - 16u) { failure = 1u; goto round_fail; }
        push[cursor++] = 0x20040004u; /* host SEMAPHOREA..D, release_wfi enabled */
        push[cursor++] = 0u; /* semaphore VA high */
        push[cursor++] = (ULONG)((ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE - 64u);
        push[cursor++] = semaphore;
        push[cursor++] = 0x01000002u;
        KeMemoryBarrier();

        /* Each packet is fetched from this one reusable PB segment. */
        i = put * 2u;
        ring[i] = (ULONG)((ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF) & ~3u;
        ring[i + 1u] = cursor << 10; /* 32-bit VA, dword count in bits 19:10 */
        KeMemoryBarrier();
        writeCount = 0u;
        l2State[0] = l2State[1] = l2State[2] = l2State[3] = 0u;
        if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) {
            failure = 2u; goto round_fail;
        }
        totalWriteCount += writeCount;
        newPut = put + 1u;
        if (newPut == ringEntries) { newPut = 0u; ++wraps; }
        KeMemoryBarrier();
        *bar1GpPut = newPut;
        KeMemoryBarrier();
        if (*bar1GpPut != newPut || Rd(B, 0x00002A30u) != 0u ||
            (Rd(B, 0x00002100u) & 0x10000000u) != 0u) {
            failure = 3u; goto round_fail;
        }
        for (gpPolls = 0u; gpPolls < 2000u; ++gpPolls) {
            gpGet = *bar1GpGet;
            pbGpGet = Rd(B, 0x00040014u);
            if (gpGet == newPut && pbGpGet == newPut) break;
            Us(1u);
        }
        if (gpPolls > maxPolls) maxPolls = gpPolls;
        if (gpPolls == 2000u) { failure = 4u; goto round_fail; }
        for (semPolls = 0u; semPolls < 2000u; ++semPolls) {
            writeCount = 0u;
            if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) {
                failure = 2u; goto round_fail;
            }
            totalWriteCount += writeCount;
            KeMemoryBarrier();
            semValue = semSlot[0];
            if (semValue == semaphore) break;
            Us(10u);
        }
        if (semPolls > maxPolls) maxPolls = semPolls;
        if (semPolls == 2000u) { failure = 5u; goto round_fail; }
        writeCount = 0u;
        if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) {
            failure = 2u; goto round_fail;
        }
        totalWriteCount += writeCount;
        KeMemoryBarrier();
        gotFirst = pixels[0]; gotMiddle = pixels[2048]; gotLast = pixels[4095];
        if (history != NULL) {
            history[round][0] = round; history[round][1] = gpPolls;
            history[round][2] = semPolls; history[round][3] = newPut;
        }
        if (gotFirst != expectedFirst || gotMiddle != expectedMiddle || gotLast != expectedLast) {
            failure = 6u; goto round_fail;
        }
        ++doneRounds;
        put = newPut;
        continue;

round_fail:
        if (history != NULL) {
            history[round][0] = round; history[round][1] = gpPolls;
            history[round][2] = semPolls; history[round][3] = newPut;
        }
        result = 0xE00000A6u;
        Out->FailRound = round + 1u;
        break;
    }
    if (round == rounds) result = 0u;
    qpcEnd = KeQueryPerformanceCounter(NULL);
    elapsedTicks = (ULONGLONG)(qpcEnd.QuadPart - qpcStart.QuadPart);
    if (qpcFrequency.QuadPart > 0) {
        totalUs = (elapsedTicks / (ULONGLONG)qpcFrequency.QuadPart) * 1000000ull +
            ((elapsedTicks % (ULONGLONG)qpcFrequency.QuadPart) * 1000000ull) /
            (ULONGLONG)qpcFrequency.QuadPart;
    }
    Out->Rounds = doneRounds;
    Out->Wraps = wraps;
    Out->MaxPolls = maxPolls;
    Out->TotalUs = (ULONG)(totalUs > 0xFFFFFFFFull ? 0xFFFFFFFFu : totalUs);
    Out->GpPut = (Dev->Bar1Cpu != NULL && Dev->Bar1MapLen >= BAR1_D3_MAP_LEN) ?
        *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu) : put;
    Out->GpGet = (Dev->Bar1Cpu != NULL && Dev->Bar1MapLen >= BAR1_D3_MAP_LEN) ?
        *(volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u) : gpGet;
    Out->Semaphore = semValue;
    Out->PixelFirst = gotFirst; Out->PixelMiddle = gotMiddle; Out->PixelLast = gotLast;
    Out->Result = result;
    Trace(L"E2Result", result); Trace(L"E2Rounds", doneRounds);
    Trace(L"E2RingEntries", ringEntries); Trace(L"E2Wraps", wraps);
    Trace(L"E2FailRound", Out->FailRound); Trace(L"E2MaxPolls", maxPolls);
    Trace(L"E2TotalUs", Out->TotalUs); Trace(L"E2FirstColor", Out->FirstColor);
    Trace(L"E2LastColor", Out->LastColor); Trace(L"E2FirstSemaphore", Out->FirstSemaphore);
    Trace(L"E2LastSemaphore", Out->LastSemaphore); Trace(L"E2GpGet", Out->GpGet);
    Trace(L"E2GpPut", Out->GpPut); Trace(L"E2Semaphore", Out->Semaphore);
    Trace(L"E2FailureStage", failure);
    Trace(L"E2PixelFirst", gotFirst); Trace(L"E2PixelMiddle", gotMiddle); Trace(L"E2PixelLast", gotLast);
    Trace(L"E2WriteCount", totalWriteCount);
    if (result != 0u && B != NULL) {
        channel = Rd(B, 0x00800004u); pbdmaStatus = Rd(B, 0x00003080u);
        pbdmaIntr0 = Rd(B, 0x00040108u); fifoIntr0 = Rd(B, 0x00002100u);
        mmuFault = Rd(B, 0x00002A30u);
        grEnabled = Rd(B, NV_PMC_ENABLE);
        if (grEnabled & (1u << 12)) {
            grIntr = Rd(B, 0x00400100u); grException = Rd(B, 0x00400108u);
            grStatus = Rd(B, 0x00400700u); classError = Rd(B, 0x00400110u);
            trappedAddr = Rd(B, 0x00400704u); trappedDataLo = Rd(B, 0x00400708u);
            trappedDataHi = Rd(B, 0x0040070Cu);
            {
                ULONG grDump[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0u };
                B3cCaptureGrStatusDump(B, L"E2GrStatusDump", grDump);
            }
        }
        Out->Channel = channel; Out->PbdmaStatus = pbdmaStatus; Out->PbdmaIntr0 = pbdmaIntr0;
        Out->FifoIntr0 = fifoIntr0;
        Out->GrIntr = grIntr; Out->GrException = grException; Out->GrStatus = grStatus;
        Out->MmuFault = mmuFault; Out->ClassError = classError; Out->TrappedAddr = trappedAddr;
        Out->TrappedDataLo = trappedDataLo; Out->TrappedDataHi = trappedDataHi;
        Trace(L"E2FailGpGet", Out->GpGet); Trace(L"E2FailGpPut", Out->GpPut);
        Trace(L"E2FailSemaphore", Out->Semaphore); Trace(L"E2FailChannel", channel);
        Trace(L"E2FailPbdmaStatus", pbdmaStatus); Trace(L"E2FailPbdmaIntr0", pbdmaIntr0);
        Trace(L"E2FailFifoIntr0", fifoIntr0);
        Trace(L"E2FailGrIntr", grIntr); Trace(L"E2FailGrException", grException);
        Trace(L"E2FailGrStatus", grStatus); Trace(L"E2FailMmuFault", mmuFault);
        Trace(L"E2FailClassError", classError); Trace(L"E2FailTrappedAddr", trappedAddr);
        Trace(L"E2FailTrappedDataLo", trappedDataLo); Trace(L"E2FailTrappedDataHi", trappedDataHi);
    }
    TraceBin(L"E2RoundHistory", history, (round < rounds ? round + 1u : rounds) * sizeof(history[0]));
    if (writes != NULL) { TraceBin(L"E2LastWrites", writes, writeCount * sizeof(writes[0])); }
    B3cPrivRingTraceWriteLog(L"E2PrivErrorCount", L"E2PrivErrorDropped",
        L"E2PrivAckTimeouts", L"E2PrivErrors");
done:
    Out->Result = result;
    Trace(L"E2Result", result); Trace(L"E2Rounds", Out->Rounds);
    Trace(L"E2RingEntries", Out->RingEntries); Trace(L"E2Wraps", Out->Wraps);
    Trace(L"E2FailRound", Out->FailRound); Trace(L"E2MaxPolls", Out->MaxPolls);
    Trace(L"E2TotalUs", Out->TotalUs); Trace(L"E2FirstColor", Out->FirstColor);
    Trace(L"E2LastColor", Out->LastColor); Trace(L"E2FirstSemaphore", Out->FirstSemaphore);
    Trace(L"E2LastSemaphore", Out->LastSemaphore);
    Trace(L"E2FailureStage", failure); Trace(L"E2GpGet", Out->GpGet);
    Trace(L"E2GpPut", Out->GpPut); Trace(L"E2Semaphore", Out->Semaphore);
    Trace(L"E2PixelFirst", Out->PixelFirst); Trace(L"E2PixelMiddle", Out->PixelMiddle);
    Trace(L"E2PixelLast", Out->PixelLast);
    if (writes != NULL) ExFreePoolWithTag(writes, SWGPU_TAG);
    if (history != NULL) ExFreePoolWithTag(history, SWGPU_TAG);
    return result;
}

/* Claude 0.4.61: 0.4.60 bind error code 0x2 = SNOOP_WITHOUT_BAR1 (name from
 * nouveau's bind-reason table).  Bind BAR1 to the GR instance block
 * (bus_bar1_block 0x1704, virtual mode, sys_mem_ncoh) before the channel
 * bind, poll bus_bind_status (0x1710) bounded, and restore it on exit. */
static ULONG g_D2Bar1Before = 0u;
static ULONG g_D2Bar1Bound = 0u;
/* Claude 0.4.63: 0.4.62 got bind error 0x2 for all 6 aperture combinations,
 * so the target is not the cause.  nvgpu gk20a_init_fifo_setup_hw() programs
 * fifo_bar1_base (0x2254) = BAR1-visible USERD GPU VA >> 12 | valid (bit 28)
 * before channel bind. D3 maps a low-VA alias for the CPU BAR1 aperture. */
static ULONG g_D2UserdBar1Before = 0u;
static ULONG g_D2UserdBar1Set = 0u;
static ULONG g_D2PbdmaInten0Before = 0u;
static ULONG g_D2PbdmaMasksSaved = 0u;

/* Shared D2 channel teardown. Normal one-shot execution calls this from its
 * exit path; E1 invokes the same routine later while the channel is resident. */
static ULONG B3cD2CleanupPublishedState(PSWGPU_DEVICE Dev, ULONG writes[64][4],
    PULONG writeCount, BOOLEAN channelEnabled, BOOLEAN runlistSubmitted,
    BOOLEAN instBound, ULONG channelId, ULONG runlistEmptyValue,
    PULONG outPreemptPolls, PULONG outPreemptLast,
    PULONG outRunlistPolls, PULONG outRunlistLast,
    BOOLEAN d4WasRun, ULONG diagWrites[64][4], PULONG diagWriteCount)
{
    volatile UCHAR *B = Dev->Bar0;
    ULONG error = 0u, value = 0u, preemptPolls = 0u, preemptLast = 0u;
    ULONG runlistPolls = 0u, runlistLast = 0u, restorePolls = 0u, restoreStatus = 0u;
    ULONG channelReg = 0x00800004u, instReg = 0x00800000u;
    ULONG preemptReg = 0x00002634u, runlistReg = 0x00002274u;
    ULONG pendingReg = 0x00002284u, pbdmaIntrReg = 0x00040108u;
    if (outPreemptPolls) *outPreemptPolls = 0u;
    if (outPreemptLast) *outPreemptLast = 0u;
    if (outRunlistPolls) *outRunlistPolls = 0u;
    if (outRunlistLast) *outRunlistLast = 0u;
    if (B == NULL || writes == NULL || writeCount == NULL) return 0x80000000u;

    if (channelEnabled) {
        value = Rd(B, channelReg) | 0x00000800u;
        if (!B3cD2Write(B, writes, writeCount, channelReg, value)) error |= 1u;
        {
            ULONG intr0 = Rd(B, pbdmaIntrReg), intr1 = Rd(B, 0x00040148u);
            TraceDetail(L"B3cD2CleanupPbdmaIntr0", intr0);
            TraceDetail(L"B3cD2CleanupPbdmaIntr1", intr1);
            if (intr0 != 0u && !B3cD2Write(B, writes, writeCount, 0x000400C0u, 0x80600008u)) error |= 512u;
            if (intr0 != 0u && !B3cD2Write(B, writes, writeCount, pbdmaIntrReg, intr0)) error |= 512u;
            if (intr1 != 0u && !B3cD2Write(B, writes, writeCount, 0x00040148u, intr1)) error |= 512u;
            TraceDetail(L"B3cD2CleanupPbdmaIntr0AfterClear", Rd(B, pbdmaIntrReg));
        }
        if (!B3cD2Write(B, writes, writeCount, preemptReg, channelId)) error |= 2u;
        for (preemptPolls = 0u; preemptPolls < 2000u; ++preemptPolls) {
            preemptLast = Rd(B, preemptReg);
            if ((preemptLast & 0x00100000u) == 0u) break;
            Us(1000u);
        }
        if (preemptPolls == 2000u) {
            error |= 4u;
            if (d4WasRun && g_GrVm.Arena != NULL && diagWrites != NULL && diagWriteCount != NULL)
                B3cD4CaptureCtxswDiag(B, (PUCHAR)g_GrVm.Arena + GR_VM_CTX_OFF,
                    diagWrites, diagWriteCount, TRUE);
        }
        Trace(L"B3cD2PreemptPolls", preemptPolls); TraceDetail(L"B3cD2PreemptLast", preemptLast);
        TraceDetail(L"B3cD2PreemptPbdmaIntr0", Rd(B, pbdmaIntrReg));
        TraceDetail(L"B3cD2PreemptPbdmaStatusReg", Rd(B, 0x00040100u));
        TraceDetail(L"B3cD2PreemptCcsrChannel", Rd(B, channelReg));
        TraceDetail(L"B3cD2PreemptEngineStatus", Rd(B, 0x00002640u));
        TraceDetail(L"B3cD2PreemptFifoIntr0", Rd(B, 0x00002100u));
        TraceDetail(L"B3cD2PreemptPbdmaStatus", Rd(B, 0x00003080u));
    }
    if (runlistSubmitted) {
        if (!B3cD2Write(B, writes, writeCount, runlistReg, runlistEmptyValue)) error |= 8u;
        for (runlistPolls = 0u; runlistPolls < 2000u; ++runlistPolls) {
            runlistLast = Rd(B, pendingReg);
            if ((runlistLast & 0x00100000u) == 0u) break;
            Us(1000u);
        }
        if (runlistPolls == 2000u) error |= 16u;
        Trace(L"B3cD2CleanupRunlistPolls", runlistPolls);
        TraceDetail(L"B3cD2CleanupRunlistLast", runlistLast);
    }
    if (instBound && !B3cD2Write(B, writes, writeCount, instReg, 0u)) error |= 32u;
    if (g_D2PbdmaMasksSaved) {
        if (!B3cD2Write(B, writes, writeCount, 0x0004010Cu, g_D2PbdmaInten0Before)) error |= 1024u;
        TraceDetail(L"B3cD3PbdmaInten0Restored", Rd(B, 0x0004010Cu));
        TraceDetail(L"B3cD3PbdmaStallUnchanged", Rd(B, 0x0004013Cu));
        g_D2PbdmaMasksSaved = 0u;
    }
    if (g_D2UserdBar1Set) {
        if (!B3cD2Write(B, writes, writeCount, 0x00002254u, g_D2UserdBar1Before)) error |= 256u;
        TraceDetail(L"B3cD2UserdBar1Final", Rd(B, 0x00002254u));
        g_D2UserdBar1Set = 0u;
    }
    if (g_D2Bar1Bound) {
        if (!B3cD2Write(B, writes, writeCount, 0x00001704u, g_D2Bar1Before)) error |= 64u;
        for (restorePolls = 0u; restorePolls < 2000u; ++restorePolls) {
            restoreStatus = Rd(B, 0x00001710u);
            if ((restoreStatus & 3u) == 0u) break;
            Us(1000u);
        }
        if (restorePolls == 2000u) error |= 128u;
        Trace(L"B3cD2Bar1RestorePolls", restorePolls);
        TraceDetail(L"B3cD2Bar1Final", Rd(B, 0x00001704u));
        g_D2Bar1Bound = 0u;
    }
    TraceDetail(L"B3eChannelCleanupError", error);
    Trace(L"B3eChannelCleanupPreemptPolls", preemptPolls);
    Trace(L"B3eChannelCleanupRunlistPolls", runlistPolls);
    if (outPreemptPolls) *outPreemptPolls = preemptPolls;
    if (outPreemptLast) *outPreemptLast = preemptLast;
    if (outRunlistPolls) *outRunlistPolls = runlistPolls;
    if (outRunlistLast) *outRunlistLast = runlistLast;
    return error;
}

static ULONG B3cInitFifoD2(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PULONG ramfc = NULL;
    PULONG userd = NULL;
    PULONG gpfifo = NULL;
    PULONG runlist = NULL;
    PULONG pushbuffer = NULL;
    PULONG instWords = NULL;
    ULONG result = 0, stage = 0, grEnabled, topInfo[3] = { 0 };
    ULONG grEngineId = 0, runlistId = 0, chid = 0;
    ULONG ccsrInstReg = 0x00800000u, ccsrChannelReg = 0x00800004u;
    ULONG fifoRunlistBaseReg = 0x00002270u, fifoRunlistReg = 0x00002274u;
    ULONG fifoRunlistPendingReg = 0x00002284u;
    ULONG fifoIntrReg = 0x00002100u, pbdmaStatusReg = 0x00040100u;
    ULONG pbdmaIntrReg = 0x00040108u, fifoEngineStatusReg = 0x00002640u;
    ULONG ccsrInstValue = 0, ccsrChannelBefore = 0, ccsrChannelValue = 0;
    ULONG runlistBaseValue = 0, runlistSubmitValue = 0, runlistEmptyValue = 0;
    ULONG engineStatus = 0, channelRead = 0, instRead = 0, pendingRead = 0;
    ULONG fifoIntrAfter = 0, pbdmaStatusAfter = 0, pbdmaIntrAfter = 0;
    ULONG runlistPolls = 0, runlistLast = 0, preemptPolls = 0, preemptLast = 0;
    ULONG cleanupRunlistPolls = 0, cleanupRunlistLast = 0;
    ULONG userdZeroOk = 0, ringZeroOk = 0, pushbufferZeroOk = 0;
    ULONG userdZeroWords = 0, ringZeroWords = 0, pushbufferZeroWords = 0;
    ULONG zeroBadOffset = 0xFFFFFFFFu;
    ULONG memoryLayout[5][4] = { 0 }, ramfcTrace[15][3] = { 0 };
    ULONG wfiTrace[2][3] = { 0 }, runlistTrace[2][3] = { 0 };
    ULONG zeroSamples[3][4] = { 0 }, writes[64][4] = { 0 };
    ULONG privBound = 0, channelEnableIssued = 0, runlistSubmitted = 0;
    ULONG cleanupError = 0, writeCount = 0, i;
    ULONG pbdmaInten0Before = 0u, pbdmaStallBefore = 0u;
    ULONG pbdmaInten0Init = 0u;
    ULONG d3Result = 0u, d4Result = 0u, d4Run = 0u, d5Result = 0u;
    ULONG diagWrites[64][4] = { { 0u } }, diagWriteCount = 0u;
    ULONGLONG userdIova = 0, runlistIova = 0;
    ULONGLONG instIova = 0, gpfifoGpuVa = 0;
    ULONG ramfcValues[15][2] = { 0 };
    ULONG ramfcCount = RTL_NUMBER_OF(ramfcValues);
    ULONG instPage = 0, runlistPage = 0, limit2 = 9u;
    ULONG expectedWfiLo = 0, expectedWfiHi = 0;

    Step(105);
    B3cPrivRingResetWriteLog();
    g_D2PbdmaMasksSaved = 0u;
    grEnabled = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cD2PmcEnable", grEnabled);
    if (!(grEnabled & (1u << 12)) || !g_GrVm.Active || g_GrVm.Arena == NULL) {
        result = 0xE00000A0u; stage = 1u; goto done;
    }
    if (!B3cPrivRingDrainBeforeWrites(B)) {
        result = 0xE00000A0u; stage = 2u; goto done;
    }

    /* TOP device-info header carries the hardware engine and runlist enums.
     * GM20B reports the graphics engine on this one-GR device. */
    for (i = 0; i < RTL_NUMBER_OF(topInfo); ++i)
        topInfo[i] = Rd(B, 0x00022700u + i * 4u);
    grEngineId = (topInfo[0] >> 26) & 0xFu;
    runlistId = (topInfo[0] >> 21) & 0xFu;
    TraceBin(L"B3cD2TopDeviceInfo", topInfo, sizeof(topInfo));
    TraceDetail(L"B3cD2TopEntryType", topInfo[0] & 3u);
    TraceDetail(L"B3cD2GrEngineId", grEngineId);
    TraceDetail(L"B3cD2RunlistId", runlistId);
    if ((topInfo[0] & 3u) != 2u || grEngineId >= 16u || runlistId >= 16u) {
        result = 0xE00000A0u; stage = 3u; goto done;
    }

    if (GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_RUNLIST_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_GPFIFO_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_USERD_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_INST_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        ((g_GrVm.Iova + GR_VM_D2_RUNLIST_OFF) >> 12) > 0x0FFFFFFFu) {
        result = 0xE00000A0u; stage = 4u; goto done;
    }
    ramfc = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF);
    userd = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_USERD_OFF);
    gpfifo = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_GPFIFO_OFF);
    runlist = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_RUNLIST_OFF);
    pushbuffer = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_PUSHBUF_OFF);
    instWords = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF);
    userdIova = g_GrVm.Iova + GR_VM_D2_USERD_OFF;
    runlistIova = g_GrVm.Iova + GR_VM_D2_RUNLIST_OFF;
    instIova = g_GrVm.Iova + GR_VM_INST_OFF;
    gpfifoGpuVa = (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_GPFIFO_OFF;
    instPage = (ULONG)((instIova >> 12) & 0x0FFFFFFFu);
    runlistPage = (ULONG)(runlistIova >> 12);
    fifoRunlistPendingReg = 0x00002284u + runlistId * 8u;
    fifoEngineStatusReg = 0x00002640u + grEngineId * 8u;
    ccsrInstReg += chid * 8u;
    ccsrChannelReg += chid * 8u;

    /* C2e uses a temporary golden-image WFI pointer and clears it on exit.
     * Restore the C2d channel-context pointer before exposing RAMIN to PFIFO. */
    expectedWfiLo = (GR_VM_GPU_VA + GR_VM_CTX_OFF) | 4u;
    expectedWfiHi = 0u;
    instWords[132] = expectedWfiLo;
    instWords[133] = expectedWfiHi;
    wfiTrace[0][0] = 132u * sizeof(ULONG); wfiTrace[0][1] = expectedWfiLo;
    wfiTrace[0][2] = instWords[132];
    wfiTrace[1][0] = 133u * sizeof(ULONG); wfiTrace[1][1] = expectedWfiHi;
    wfiTrace[1][2] = instWords[133];
    KeMemoryBarrier();

    /* A 4-KiB RAMFC starts at the instance-block base; the GPU page-directory
     * words at 0x200..0x20c remain untouched.  Zero all bounded channel storage. */
    RtlZeroMemory((PUCHAR)ramfc, 0x200u);
    RtlZeroMemory((PUCHAR)userd, PAGE_SIZE);
    RtlZeroMemory((PUCHAR)gpfifo, PAGE_SIZE);
    RtlZeroMemory((PUCHAR)runlist, PAGE_SIZE);
    RtlZeroMemory((PUCHAR)pushbuffer, PAGE_SIZE);

    ramfcValues[0][0] = 0x008u; ramfcValues[0][1] = ((ULONG)userdIova & ~0x1FFu) | 0x3u; /* Claude 0.4.60: USERD target sys_mem_ncoh (pbdma_userd_target_sys_mem_ncoh_f) */
    ramfcValues[1][0] = 0x00Cu; ramfcValues[1][1] = (ULONG)(userdIova >> 32);
    ramfcValues[2][0] = 0x010u; ramfcValues[2][1] = 0x0000FACEu;
    /* nvgpu channel_gk20a_setup_ramfc(): acquire retry/timeout fields only;
     * bit 31 is outside the GM20B timeout field mask and must remain clear. */
    ramfcValues[3][0] = 0x030u; ramfcValues[3][1] = 0x7FFFF902u;
    ramfcValues[4][0] = 0x048u; ramfcValues[4][1] = (ULONG)gpfifoGpuVa;
    ramfcValues[5][0] = 0x04Cu; ramfcValues[5][1] = (ULONG)(gpfifoGpuVa >> 32) | (limit2 << 16);
    ramfcValues[6][0] = 0x084u; ramfcValues[6][1] = 0x20400000u;
    /* nvgpu channel_gk20a_setup_ramfc() writes subdevice id 1, ACTIVE and
     * CHANNEL_DMA_ENABLE. 0xFFF was an invalid ID in the previous image. */
    ramfcValues[7][0] = 0x094u; ramfcValues[7][1] = 0x30000001u;
    ramfcValues[8][0] = 0x09Cu; ramfcValues[8][1] = 0x00000100u;
    ramfcValues[9][0] = 0x0ACu; ramfcValues[9][1] = 0x0000001Fu;
    ramfcValues[10][0] = 0x0E4u; ramfcValues[10][1] = 0x00000020u;
    ramfcValues[11][0] = 0x0E8u; ramfcValues[11][1] = chid;
    ramfcValues[12][0] = 0x0B8u; ramfcValues[12][1] = 0xF8000000u;
    ramfcValues[13][0] = 0x0F8u; ramfcValues[13][1] = 0x10003080u;
    ramfcValues[14][0] = 0x0FCu; ramfcValues[14][1] = 0x10000010u;
    for (i = 0; i < ramfcCount; ++i) {
        ULONG wordOffset = ramfcValues[i][0] >> 2;
        ramfc[wordOffset] = ramfcValues[i][1];
        ramfcTrace[i][0] = ramfcValues[i][0];
        ramfcTrace[i][1] = ramfcValues[i][1];
        ramfcTrace[i][2] = ramfc[wordOffset];
        if (ramfcTrace[i][1] != ramfcTrace[i][2]) {
            result = 0xE00000A0u; stage = 5u; goto done;
        }
    }
    runlist[0] = chid; /* Nouveau gk104_runl_insert_chan(): plain channel entry. */
    runlist[1] = 0u;
    KeMemoryBarrier();
    runlistTrace[0][0] = 0u; runlistTrace[0][1] = chid; runlistTrace[0][2] = runlist[0];
    runlistTrace[1][0] = 4u; runlistTrace[1][1] = 0u; runlistTrace[1][2] = runlist[1];
    zeroSamples[0][0] = GR_VM_D2_USERD_OFF; zeroSamples[0][1] = userd[0];
    zeroSamples[0][2] = userd[64]; zeroSamples[0][3] = userd[127];
    zeroSamples[1][0] = GR_VM_D2_GPFIFO_OFF; zeroSamples[1][1] = gpfifo[0];
    zeroSamples[1][2] = gpfifo[64]; zeroSamples[1][3] = gpfifo[1023];
    zeroSamples[2][0] = GR_VM_D2_PUSHBUF_OFF; zeroSamples[2][1] = pushbuffer[0];
    zeroSamples[2][2] = pushbuffer[64]; zeroSamples[2][3] = pushbuffer[1023];
    for (i = 0; i < PAGE_SIZE / sizeof(ULONG); ++i) {
        if (userd[i] != 0u) { zeroBadOffset = GR_VM_D2_USERD_OFF + i * sizeof(ULONG); break; }
        ++userdZeroWords;
    }
    if (zeroBadOffset == 0xFFFFFFFFu) {
        for (i = 0; i < PAGE_SIZE / sizeof(ULONG); ++i) {
            if (gpfifo[i] != 0u) { zeroBadOffset = GR_VM_D2_GPFIFO_OFF + i * sizeof(ULONG); break; }
            ++ringZeroWords;
        }
    }
    if (zeroBadOffset == 0xFFFFFFFFu) {
        for (i = 0; i < PAGE_SIZE / sizeof(ULONG); ++i) {
            if (pushbuffer[i] != 0u) { zeroBadOffset = GR_VM_D2_PUSHBUF_OFF + i * sizeof(ULONG); break; }
            ++pushbufferZeroWords;
        }
    }
    userdZeroOk = userdZeroWords == PAGE_SIZE / sizeof(ULONG);
    ringZeroOk = ringZeroWords == PAGE_SIZE / sizeof(ULONG);
    pushbufferZeroOk = pushbufferZeroWords == PAGE_SIZE / sizeof(ULONG);
    memoryLayout[0][0] = GR_VM_INST_OFF; memoryLayout[0][1] = 0x1000u;
    memoryLayout[0][2] = (ULONG)instIova; memoryLayout[0][3] = (ULONG)(instIova >> 32);
    memoryLayout[1][0] = GR_VM_D2_USERD_OFF; memoryLayout[1][1] = PAGE_SIZE;
    memoryLayout[1][2] = (ULONG)userdIova; memoryLayout[1][3] = (ULONG)(userdIova >> 32);
    memoryLayout[2][0] = GR_VM_D2_GPFIFO_OFF; memoryLayout[2][1] = PAGE_SIZE;
    memoryLayout[2][2] = (ULONG)(g_GrVm.Iova + GR_VM_D2_GPFIFO_OFF);
    memoryLayout[2][3] = (ULONG)((g_GrVm.Iova + GR_VM_D2_GPFIFO_OFF) >> 32);
    memoryLayout[3][0] = GR_VM_D2_RUNLIST_OFF; memoryLayout[3][1] = PAGE_SIZE;
    memoryLayout[3][2] = (ULONG)runlistIova; memoryLayout[3][3] = (ULONG)(runlistIova >> 32);
    memoryLayout[4][0] = GR_VM_D2_PUSHBUF_OFF; memoryLayout[4][1] = PAGE_SIZE;
    memoryLayout[4][2] = (ULONG)(g_GrVm.Iova + GR_VM_D2_PUSHBUF_OFF);
    memoryLayout[4][3] = (ULONG)((g_GrVm.Iova + GR_VM_D2_PUSHBUF_OFF) >> 32);
    TraceBin(L"B3cD2MemoryLayout", memoryLayout, sizeof(memoryLayout));
    TraceBin(L"B3cD2RamfcFields", ramfcTrace, sizeof(ramfcTrace));
    TraceBin(L"B3cD2RunlistEntry", runlistTrace, sizeof(runlistTrace));
    TraceBin(L"B3cD2ZeroSamples", zeroSamples, sizeof(zeroSamples));
    TraceDetail(L"B3cD2UserdZeroOk", userdZeroOk);
    TraceDetail(L"B3cD2GpfifoEmptyOk", ringZeroOk);
    TraceDetail(L"B3cD2PushbufferZeroOk", pushbufferZeroOk);
    TraceDetail(L"B3cD2UserdZeroWords", userdZeroWords);
    TraceDetail(L"B3cD2GpfifoZeroWords", ringZeroWords);
    TraceDetail(L"B3cD2PushbufferZeroWords", pushbufferZeroWords);
    TraceDetail(L"B3cD2MemoryZeroBadOffset", zeroBadOffset);
    TraceDetail(L"B3cD2GpfifoEntries", PAGE_SIZE / 8u);
    TraceDetail(L"B3cD2RunlistBaseIovaLo", (ULONG)runlistIova);
    TraceDetail(L"B3cD2RunlistBaseIovaHi", (ULONG)(runlistIova >> 32));
    TraceDetail(L"B3cD2GpfifoGpuVa", (ULONG)gpfifoGpuVa);
    TraceDetail(L"B3cD2UserdIovaLo", (ULONG)userdIova);
    TraceDetail(L"B3cD2UserdIovaHi", (ULONG)(userdIova >> 32));
    TraceDetail(L"B3cD2RamfcAcquireExpected", ramfcValues[3][1]);
    TraceDetail(L"B3cD2RamfcSubdeviceExpected", ramfcValues[7][1]);
    if (!userdZeroOk || !ringZeroOk || !pushbufferZeroOk ||
        runlistTrace[0][2] != chid || runlistTrace[1][2] != 0u ||
        instWords[132] != expectedWfiLo || instWords[133] != expectedWfiHi) {
        result = 0xE00000A0u; stage = 6u; goto done;
    }
    TraceBin(L"B3cD2WfiPointer", wfiTrace, sizeof(wfiTrace));

    /* Claude 0.4.59: 0.4.58 with target VID memory raised PFIFO BIND_ERROR
     * (fifo_intr_0 bit0).  Use the aperture FECS accepted in stage C
     * (sys_mem_ncoh, 3 << 28), bind=1. */
    ccsrInstValue = instPage | 0xB0000000u;
    runlistBaseValue = 0x30000000u | runlistPage; /* system memory, noncoherent */
    runlistSubmitValue = (runlistId << 20) | 1u;
    runlistEmptyValue = (runlistId << 20);
    ccsrChannelBefore = Rd(B, ccsrChannelReg);
    ccsrChannelValue = (ccsrChannelBefore & ~0x000F0000u) | (runlistId << 16);
    TraceDetail(L"B3cD2CcsrInstValue", ccsrInstValue);
    TraceDetail(L"B3cD2RunlistBaseValue", runlistBaseValue);
    TraceDetail(L"B3cD2RunlistSubmitValue", runlistSubmitValue);
    TraceDetail(L"B3cD2CcsrChannelBefore", ccsrChannelBefore);

    /* Publish runlist memory, assign this channel to the TOP-selected runlist,
     * bind the shared RAMIN block, and enable channel 0.  No GPFIFO entry is
     * written or submitted in D2. */
    stage = 7u;
    if (!B3cD2Write(B, writes, &writeCount, fifoRunlistBaseReg, runlistBaseValue)) {
        result = 0xE00000A0u; goto done;
    }
    if (writes[writeCount - 1u][3] != runlistBaseValue) {
        result = 0xE00000A0u; stage = 8u; goto done;
    }
    if (!B3cD2Write(B, writes, &writeCount, ccsrChannelReg, ccsrChannelValue)) {
        result = 0xE00000A0u; stage = 9u; goto done;
    }
    if ((writes[writeCount - 1u][3] & 0x000F0000u) != (runlistId << 16)) {
        result = 0xE00000A0u; stage = 10u; goto done;
    }
    /* PBDMA INTR_0 bit 24 was ENG_RESET, caused by CE2 remaining in reset.
     * D1 now releases CE2 with PFIFO. Keep the hardware stall mask untouched
     * and use Nouveau gk104_runq_init()'s INTEN0 value; do not mask bit 24. */
    {
        pbdmaStallBefore = Rd(B, 0x0004013Cu);
        pbdmaInten0Before = Rd(B, 0x0004010Cu);
        pbdmaInten0Init = 0xFFFFFEFFu;
        g_D2PbdmaInten0Before = pbdmaInten0Before;
        g_D2PbdmaMasksSaved = 1u;
        TraceDetail(L"B3cD2PbIntrStallBefore", pbdmaStallBefore);
        TraceDetail(L"B3cD2PbIntrEn0Before", Rd(B, 0x0004010Cu));
        TraceDetail(L"B3cD2PbIntrEn1Before", Rd(B, 0x0004014Cu));
        TraceDetail(L"B3cD3PbdmaStallBefore", pbdmaStallBefore);
        TraceDetail(L"B3cD3PbdmaInten0Before", pbdmaInten0Before);
        if (!B3cD2Write(B, writes, &writeCount, 0x00040108u, 0xFFFFFFFFu) ||
            !B3cD2Write(B, writes, &writeCount, 0x00040148u, 0xFFFFFFFFu) ||
            !B3cD2Write(B, writes, &writeCount, 0x0004010Cu, pbdmaInten0Init) ||
            !B3cD2Write(B, writes, &writeCount, 0x0004014Cu, 0xFFFFFFFFu)) {
            result = 0xE00000A0u; stage = 30u; goto done;
        }
        TraceDetail(L"B3cD2PbIntrStallAfter", Rd(B, 0x0004013Cu));
        TraceDetail(L"B3cD2PbIntrEn0After", Rd(B, 0x0004010Cu));
        TraceDetail(L"B3cD2PbIntr0AfterInit", Rd(B, 0x00040108u));
        TraceDetail(L"B3cD3PbdmaStallAfter", Rd(B, 0x0004013Cu));
        TraceDetail(L"B3cD3PbdmaInten0After", Rd(B, 0x0004010Cu));
        TraceDetail(L"B3cD3Bit24Enabled", ((Rd(B, 0x0004010Cu) & 0x01000000u) != 0u) ? 1u : 0u);
    }
    /* Claude 0.4.60: find which write raises PFIFO BIND_ERROR.  Record
     * fifo_intr_0 and fifo_intr_bind_error (0x252C, read only) before the
     * instance bind, clear a stale bit0, then read both again after. */
    fifoIntrAfter = Rd(B, fifoIntrReg);
    TraceDetail(L"B3cD2FifoIntr0BeforeInstWrite", fifoIntrAfter);
    TraceDetail(L"B3cD2BindErrorInfoBeforeInstWrite", Rd(B, 0x0000252Cu));
    if ((fifoIntrAfter & 1u) != 0u) {
        if (!B3cD2Write(B, writes, &writeCount, fifoIntrReg, 1u)) {
            result = 0xE00000A0u; stage = 18u; goto done;
        }
        TraceDetail(L"B3cD2FifoIntr0AfterStaleClear", Rd(B, fifoIntrReg));
    }
    {
        ULONG bar1Reg = 0x00001704u, bindStatusReg = 0x00001710u;
        ULONG bar1Value = instPage | 0x30000000u | 0x80000000u;
        ULONG bindStatus = 0u, bindPolls = 0u;
        g_D2Bar1Before = Rd(B, bar1Reg);
        TraceDetail(L"B3cD2Bar1Before", g_D2Bar1Before);
        TraceDetail(L"B3cD2Bar1Value", bar1Value);
        g_D2Bar1Bound = 1u;
        if (!B3cD2Write(B, writes, &writeCount, bar1Reg, bar1Value)) {
            result = 0xE00000A0u; stage = 19u; goto done;
        }
        for (bindPolls = 0; bindPolls < 2000u; ++bindPolls) {
            bindStatus = Rd(B, bindStatusReg);
            if ((bindStatus & 0x3u) == 0u) break;
            Us(1000u);
        }
        TraceDetail(L"B3cD2Bar1Readback", writes[writeCount - 1u][3]);
        Trace(L"B3cD2Bar1BindPolls", bindPolls);
        TraceDetail(L"B3cD2Bar1BindStatus", bindStatus);
        TraceDetail(L"B3cD2FifoIntr0AfterBar1", Rd(B, fifoIntrReg));
        if (bindPolls == 2000u) { result = 0xE00000A0u; stage = 20u; goto done; }
    }
    {
        ULONG userdBar1Value = (BAR1_D3_LOW_VA >> 12) | 0x10000000u;
        g_D2UserdBar1Before = Rd(B, 0x00002254u);
        TraceDetail(L"B3cD2UserdBar1Before", g_D2UserdBar1Before);
        TraceDetail(L"B3cD2UserdBar1Value", userdBar1Value);
        g_D2UserdBar1Set = 1u;
        if (!B3cD2Write(B, writes, &writeCount, 0x00002254u, userdBar1Value)) {
            result = 0xE00000A0u; stage = 29u; goto done;
        }
        TraceDetail(L"B3cD2UserdBar1Readback", writes[writeCount - 1u][3]);
        TraceDetail(L"B3cD2FifoIntr0AfterUserdBar1", Rd(B, fifoIntrReg));
    }
    /* Claude 0.4.62: 0.4.61 still got bind error 0x2 with BAR1 bound.  Probe
     * BAR1 target {ncoh 3, vid 0} x channel-inst/USERD target {vid 0, coh 2,
     * ncoh 3} in one run: clear intr0 bit0, bind, record intr0 + 0x252C,
     * unbind, record again.  Then use the first combination without error.
     * Only 0x1704, 0x800000 (chid 0), 0x2100 are written; every write is
     * priv-ring checked; every BAR1 rebind wait is bounded. */
    {
        static const ULONG bar1Targets[2] = { 3u, 0u };
        static const ULONG instTargets[3] = { 0u, 2u, 3u };
        ULONG probe[6][4] = { 0 };
        ULONG bi, ti, n = 0u, chosen = 0xFFFFFFFFu, polls, st;
        ULONG probeBar1Value, probeInstValue;
        for (bi = 0; bi < 2u; ++bi) {
            probeBar1Value = instPage | (bar1Targets[bi] << 28) | 0x80000000u;
            if (!B3cD2Write(B, writes, &writeCount, 0x00001704u, probeBar1Value)) {
                result = 0xE00000A0u; stage = 22u; goto done;
            }
            for (polls = 0; polls < 2000u; ++polls) {
                st = Rd(B, 0x00001710u);
                if ((st & 0x3u) == 0u) break;
                Us(1000u);
            }
            if (polls == 2000u) { result = 0xE00000A0u; stage = 23u; goto done; }
            for (ti = 0; ti < 3u; ++ti) {
                ramfc[0x008u >> 2] = ((ULONG)userdIova & ~0x1FFu) | instTargets[ti];
                KeMemoryBarrier();
                if (!B3cD2Write(B, writes, &writeCount, fifoIntrReg, 1u)) {
                    result = 0xE00000A0u; stage = 24u; goto done;
                }
                probeInstValue = instPage | (instTargets[ti] << 28) | 0x80000000u;
                if (!B3cD2Write(B, writes, &writeCount, ccsrInstReg, probeInstValue)) {
                    result = 0xE00000A0u; stage = 25u; goto done;
                }
                probe[n][0] = (bar1Targets[bi] << 4) | instTargets[ti];
                probe[n][1] = Rd(B, fifoIntrReg);
                probe[n][2] = Rd(B, 0x0000252Cu);
                if (!B3cD2Write(B, writes, &writeCount, ccsrInstReg, 0u)) {
                    result = 0xE00000A0u; stage = 26u; goto done;
                }
                probe[n][3] = Rd(B, fifoIntrReg);
                if (!B3cD2Write(B, writes, &writeCount, fifoIntrReg, 1u)) {
                    result = 0xE00000A0u; stage = 24u; goto done;
                }
                if (chosen == 0xFFFFFFFFu && (probe[n][1] & 1u) == 0u) chosen = n;
                ++n;
            }
        }
        TraceDetail(L"B3cD2ProbeCount", n);
        {
            static const PCWSTR probeNames[6][4] = {
                { L"B3cD2Probe0Combo", L"B3cD2Probe0Intr0AfterBind", L"B3cD2Probe0BindErrorInfo", L"B3cD2Probe0Intr0AfterUnbind" },
                { L"B3cD2Probe1Combo", L"B3cD2Probe1Intr0AfterBind", L"B3cD2Probe1BindErrorInfo", L"B3cD2Probe1Intr0AfterUnbind" },
                { L"B3cD2Probe2Combo", L"B3cD2Probe2Intr0AfterBind", L"B3cD2Probe2BindErrorInfo", L"B3cD2Probe2Intr0AfterUnbind" },
                { L"B3cD2Probe3Combo", L"B3cD2Probe3Intr0AfterBind", L"B3cD2Probe3BindErrorInfo", L"B3cD2Probe3Intr0AfterUnbind" },
                { L"B3cD2Probe4Combo", L"B3cD2Probe4Intr0AfterBind", L"B3cD2Probe4BindErrorInfo", L"B3cD2Probe4Intr0AfterUnbind" },
                { L"B3cD2Probe5Combo", L"B3cD2Probe5Intr0AfterBind", L"B3cD2Probe5BindErrorInfo", L"B3cD2Probe5Intr0AfterUnbind" }
            };
            for (bi = 0; bi < n && bi < 6u; ++bi) {
                for (ti = 0; ti < 4u; ++ti) Trace(probeNames[bi][ti], probe[bi][ti]);
            }
        }
        TraceDetail(L"B3cD2ProbeChosen", chosen);
        if (chosen == 0xFFFFFFFFu) {
            ramfc[0x008u >> 2] = ((ULONG)userdIova & ~0x1FFu) | 0x3u;
            result = 0xE00000A0u; stage = 21u; goto done;
        }
        {
            ULONG bar1T = probe[chosen][0] >> 4, instT = probe[chosen][0] & 0xFu;
            ramfc[0x008u >> 2] = ((ULONG)userdIova & ~0x1FFu) | instT;
            KeMemoryBarrier();
            if (!B3cD2Write(B, writes, &writeCount, 0x00001704u,
                            instPage | (bar1T << 28) | 0x80000000u)) {
                result = 0xE00000A0u; stage = 27u; goto done;
            }
            for (polls = 0; polls < 2000u; ++polls) {
                st = Rd(B, 0x00001710u);
                if ((st & 0x3u) == 0u) break;
                Us(1000u);
            }
            if (polls == 2000u) { result = 0xE00000A0u; stage = 23u; goto done; }
            ccsrInstValue = instPage | (instT << 28) | 0x80000000u;
            runlistBaseValue = (instT << 28) | runlistPage;
            if (!B3cD2Write(B, writes, &writeCount, fifoRunlistBaseReg, runlistBaseValue)) {
                result = 0xE00000A0u; stage = 28u; goto done;
            }
            TraceDetail(L"B3cD2ChosenCcsrInstValue", ccsrInstValue);
            TraceDetail(L"B3cD2ChosenRunlistBaseValue", runlistBaseValue);
        }
    }
    privBound = 1u;
    if (!B3cD2Write(B, writes, &writeCount, ccsrInstReg, ccsrInstValue)) {
        result = 0xE00000A0u; stage = 11u; goto done;
    }
    instRead = writes[writeCount - 1u][3];
    fifoIntrAfter = Rd(B, fifoIntrReg);
    TraceDetail(L"B3cD2CcsrInstReadAfterWrite", instRead);
    TraceDetail(L"B3cD2FifoIntr0AfterInstWrite", fifoIntrAfter);
    TraceDetail(L"B3cD2BindErrorInfoAfterInstWrite", Rd(B, 0x0000252Cu));
    TraceDetail(L"B3cD2RamfcUserdLo", ramfc[0x008u >> 2]);
    /* Claude 0.4.59: compare pointer + target only (bind may read back 0);
     * a PFIFO BIND_ERROR (bit0) is the real failure signal. */
    if ((instRead & 0x3FFFFFFFu) != (ccsrInstValue & 0x3FFFFFFFu) ||
        (fifoIntrAfter & 1u) != 0u) {
        result = 0xE00000A0u; stage = 12u; goto done;
    }
    channelEnableIssued = 1u;
    channelRead = Rd(B, ccsrChannelReg);
    if (!B3cD2Write(B, writes, &writeCount, ccsrChannelReg,
                    channelRead | 0x00000400u)) {
        result = 0xE00000A0u; stage = 13u; goto done;
    }
    channelRead = writes[writeCount - 1u][3];
    TraceDetail(L"B3cD2PbIntr0AfterEnable", Rd(B, 0x00040108u));
    if ((channelRead & 1u) == 0u) {
        result = 0xE00000A0u; stage = 14u; goto done;
    }
    runlistSubmitted = 1u;
    /* Read-only candidate checks immediately before and after scheduling:
     * PBDMA routing/target, interrupt policy, and the FECS context left by C2. */
    TraceDetail(L"B3cD2PbdmaMapBeforeRunlist", Rd(B, 0x00002390u));
    TraceDetail(L"B3cD2PbdmaTargetBeforeRunlist", Rd(B, 0x000400ACu));
    TraceDetail(L"B3cD2PbdmaStatusBeforeRunlist", Rd(B, pbdmaStatusReg));
    TraceDetail(L"B3cD2PbdmaIntr0BeforeRunlist", Rd(B, pbdmaIntrReg));
    TraceDetail(L"B3cD2PbdmaIntrEn0BeforeRunlist", Rd(B, 0x0004010Cu));
    TraceDetail(L"B3cD2PbdmaIntrStallBeforeRunlist", Rd(B, 0x0004013Cu));
    TraceDetail(L"B3cD2FecsCurrentCtxBeforeRunlist", Rd(B, 0x00409B00u));
    TraceDetail(L"B3cD2FecsNewCtxBeforeRunlist", Rd(B, 0x00409B04u));
    TraceDetail(L"B3cD2FifoEngineStatusBeforeRunlist", Rd(B, fifoEngineStatusReg));
    TraceDetail(L"B3cD2RunlistPendingBeforeSubmit", Rd(B, fifoRunlistPendingReg));
    if (!B3cD2Write(B, writes, &writeCount, fifoRunlistReg, runlistSubmitValue)) {
        result = 0xE00000A0u; stage = 15u; goto done;
    }
    runlistLast = Rd(B, fifoRunlistPendingReg);
    for (runlistPolls = 0; runlistPolls < 2000u; ++runlistPolls) {
        runlistLast = Rd(B, fifoRunlistPendingReg);
        if ((runlistLast & 0x00100000u) == 0u) break;
        Us(1000u);
    }
    Trace(L"B3cD2RunlistPolls", runlistPolls);
    TraceDetail(L"B3cD2RunlistPendingAfter", runlistLast);
    TraceDetail(L"B3cD2PbIntr0AfterRunlist", Rd(B, 0x00040108u));
    TraceDetail(L"B3cD2PbdmaMapAfterRunlist", Rd(B, 0x00002390u));
    TraceDetail(L"B3cD2PbdmaTargetAfterRunlist", Rd(B, 0x000400ACu));
    TraceDetail(L"B3cD2FecsCurrentCtxAfterRunlist", Rd(B, 0x00409B00u));
    TraceDetail(L"B3cD2FecsNewCtxAfterRunlist", Rd(B, 0x00409B04u));
    if (runlistPolls == 2000u) { result = 0xE00000A0u; stage = 16u; goto done; }
    stage = 17u;
    engineStatus = Rd(B, fifoEngineStatusReg);
    channelRead = Rd(B, ccsrChannelReg);
    instRead = Rd(B, ccsrInstReg);
    fifoIntrAfter = Rd(B, fifoIntrReg);
    pbdmaStatusAfter = Rd(B, pbdmaStatusReg);
    pbdmaIntrAfter = Rd(B, pbdmaIntrReg);
    TraceDetail(L"B3cD2FifoEngineStatusAfterBind", engineStatus);
    TraceDetail(L"B3cD2CcsrChannelAfterBind", channelRead);
    TraceDetail(L"B3cD2CcsrChannelEnableAfterBind", channelRead & 1u);
    TraceDetail(L"B3cD2CcsrChannelStatusAfterBind", (channelRead >> 24) & 0xFu);
    TraceDetail(L"B3cD2CcsrInstAfterBind", instRead);
    TraceDetail(L"B3cD2FifoIntr0AfterBind", fifoIntrAfter);
    TraceDetail(L"B3cD2Pbdma0StatusAfterBind", pbdmaStatusAfter);
    TraceDetail(L"B3cD2Pbdma0IntrAfterBind", pbdmaIntrAfter);
    /* Claude 0.4.69: with PBDMA bit 24 masked (0.4.68) the empty channel is
     * saved off the PBDMA and reads back enabled + status 0 (idle); 0.4.67
     * and earlier held it ON_PBDMA (5). Both are valid for posting GP_PUT. */
    TraceDetail(L"B3cD2ChannelStatusAccepted", (channelRead >> 24) & 0xFu);
    if ((channelRead & 1u) == 0u ||
        (((channelRead >> 24) & 0xFu) != 5u && ((channelRead >> 24) & 0xFu) != 0u)) {
        result = 0xE00000A0u; stage = 31u; goto done;
    }
    /* Claude 0.4.64: 0.4.63 reached here (channel ON_PBDMA) with PBDMA0
     * intr_0 bit 24 pending, and the cleanup preempt then timed out.  Dump
     * PBDMA0 state (read only, hw_pbdma_gm20b.h offsets) for diagnosis. */
    {
        static const ULONG pbRegs[14] = {
            0x00040000u, 0x00040010u, 0x00040014u, 0x00040018u, 0x00040050u,
            0x0004005Cu, 0x00040084u, 0x000400C0u, 0x000400C4u, 0x000400E4u,
            0x00040120u, 0x00040148u, 0x00003080u, 0x0000252Cu };
        static const PCWSTR pbNames[14] = {
            L"B3cD2PbGpPut", L"B3cD2PbSignature", L"B3cD2PbGpGet", L"B3cD2PbGet",
            L"B3cD2PbGpFetch", L"B3cD2PbPut", L"B3cD2PbPbHeader", L"B3cD2PbMethod0",
            L"B3cD2PbData0", L"B3cD2PbHceCtrl", L"B3cD2PbChannel", L"B3cD2PbIntr1",
            L"B3cD2FifoPbdmaStatus0", L"B3cD2BindErrorInfoAfterRunlist" };
        ULONG k;
        for (k = 0; k < 14u; ++k) Trace(pbNames[k], Rd(B, pbRegs[k]));
        TraceDetail(L"B3cD2UserdGpGet", userd[0x88u >> 2]);
        /* Claude 0.4.66: PBDMA intr_0 bit 24 is "[ENGINE]" in nvgpu's
         * pbdma_intr_fault_type_desc[].  Dump GR / FECS / FIFO state (read
         * only) right after the runlist to see which engine side is unhappy. */
        {
            static const ULONG grRegs[14] = {
                0x00400100u, 0x00400108u, 0x0040060Cu, 0x00400700u, 0x00404000u,
                0x00409C18u, 0x00409800u, 0x00409804u, 0x00409B00u, 0x00002640u,
                0x000025A0u, 0x00002A30u, 0x0004011Cu, 0x00040130u };
            static const PCWSTR grNames[14] = {
                L"B3cD2EnGrIntr", L"B3cD2EnGrException", L"B3cD2EnGrEngineStatus",
                L"B3cD2EnGrStatus", L"B3cD2EnFeHwwEsr", L"B3cD2EnFecsHostInt",
                L"B3cD2EnFecsMailbox0", L"B3cD2EnFecsMailbox1", L"B3cD2EnFecsCurrentCtx",
                L"B3cD2EnFifoEngineStatus0", L"B3cD2EnFifoIntrPbdmaId",
                L"B3cD2EnFifoMmuFaultStatus", L"B3cD2EnPb11C", L"B3cD2EnPb130" };
            ULONG g;
            for (g = 0; g < 14u; ++g) Trace(grNames[g], Rd(B, grRegs[g]));
        }
        TraceDetail(L"B3cD2UserdGpPut", userd[0x8Cu >> 2]);
    }

    /* D3 runs while the channel is ON_PBDMA. Its failure is recorded, then
     * control always continues through the existing D2 cleanup path. */
    d3Result = B3cRunD3(Dev, userd, gpfifo, pushbuffer,
                        (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF);
    Trace(L"B3cD2D3Result", d3Result);
    if (d3Result != 0u) { result = d3Result; stage = 18u; }
    else {
        ULONG ctxPrepResult = B3cPrepareD4ChannelContext(Dev);
        Trace(L"B3cD2D4CtxPrepResult", ctxPrepResult);
        if (ctxPrepResult != 0u) { result = ctxPrepResult; stage = 19u; }
        else {
            d4Run = 1u;
            d4Result = B3cRunD4(Dev, userd, gpfifo, pushbuffer,
                                (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF);
            Trace(L"B3cD2D4Result", d4Result);
            if (d4Result != 0u) { result = d4Result; stage = 20u; }
            else {
                d5Result = B3cRunD5(Dev, userd, gpfifo, pushbuffer,
                    (ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF);
                Trace(L"B3cD2D5Result", d5Result);
                if (d5Result != 0u) { result = d5Result; stage = 21u; }
            }
        }
    }

done:
    Dev->ResidentChannelEnabled = channelEnableIssued ? TRUE : FALSE;
    Dev->ResidentRunlistSubmitted = runlistSubmitted ? TRUE : FALSE;
    Dev->ResidentInstBound = privBound ? TRUE : FALSE;
    Dev->ResidentRunlistEmptyValue = runlistEmptyValue;
    Dev->ResidentChannelId = chid;
    if (result == 0u) {
        Dev->ResidentActive = TRUE;
        g_ResidentOwner = Dev;
        TraceDurable(L"B3eResidentActive", 1u);
        TraceDetail(L"B3eResidentChannelEnabled", channelEnableIssued);
        TraceDetail(L"B3eResidentRunlistSubmitted", runlistSubmitted);
    } else {
        cleanupError = B3cD2CleanupPublishedState(Dev, writes, &writeCount,
            channelEnableIssued ? TRUE : FALSE, runlistSubmitted ? TRUE : FALSE,
            privBound ? TRUE : FALSE, chid, runlistEmptyValue,
            &preemptPolls, &preemptLast, &cleanupRunlistPolls, &cleanupRunlistLast,
            d4Run ? TRUE : FALSE,
            diagWrites, &diagWriteCount);
        Dev->ResidentChannelEnabled = FALSE;
        Dev->ResidentRunlistSubmitted = FALSE;
        Dev->ResidentInstBound = FALSE;
    }
    if (cleanupError != 0u && result == 0u) result = 0xE00000A0u;
    channelRead = Rd(B, ccsrChannelReg);
    instRead = Rd(B, ccsrInstReg);
    pendingRead = Rd(B, fifoRunlistPendingReg);
    engineStatus = Rd(B, fifoEngineStatusReg);
    fifoIntrAfter = Rd(B, fifoIntrReg);
    pbdmaStatusAfter = Rd(B, pbdmaStatusReg);
    pbdmaIntrAfter = Rd(B, pbdmaIntrReg);
    TraceDetail(L"B3cD2BindErrorInfoFinal", Rd(B, 0x0000252Cu));
    TraceDetail(L"B3cD2Stage", stage);
    Trace(L"B3cD2Result", result);
    TraceDetail(L"B3cD2CleanupError", cleanupError);
    TraceDetail(L"B3cD2WriteCount", writeCount);
    TraceBin(L"B3cD2Writes", writes, sizeof(writes));
    TraceBin(L"B3cD2RamfcFields", ramfcTrace, sizeof(ramfcTrace));
    TraceBin(L"B3cD2MemoryLayout", memoryLayout, sizeof(memoryLayout));
    TraceBin(L"B3cD2WfiPointer", wfiTrace, sizeof(wfiTrace));
    TraceBin(L"B3cD2RunlistEntry", runlistTrace, sizeof(runlistTrace));
    TraceBin(L"B3cD2ZeroSamples", zeroSamples, sizeof(zeroSamples));
    Trace(L"B3cD2RunlistPolls", runlistPolls);
    TraceDetail(L"B3cD2RunlistLast", runlistLast);
    Trace(L"B3cD2PreemptPolls", preemptPolls);
    TraceDetail(L"B3cD2PreemptLast", preemptLast);
    Trace(L"B3cD2CleanupRunlistPolls", cleanupRunlistPolls);
    TraceDetail(L"B3cD2CleanupRunlistLast", cleanupRunlistLast);
    TraceDetail(L"B3cD2CcsrInstFinal", instRead);
    TraceDetail(L"B3cD2CcsrChannelFinal", channelRead);
    TraceDetail(L"B3cD2CcsrChannelEnableFinal", channelRead & 1u);
    TraceDetail(L"B3cD2CcsrChannelStatusFinal", (channelRead >> 24) & 0xFu);
    TraceDetail(L"B3cD2RunlistPendingFinal", pendingRead);
    TraceDetail(L"B3cD2FifoEngineStatusFinal", engineStatus);
    TraceDetail(L"B3cD2FifoIntr0Final", fifoIntrAfter);
    TraceDetail(L"B3cD2Pbdma0StatusFinal", pbdmaStatusAfter);
    TraceDetail(L"B3cD2Pbdma0IntrFinal", pbdmaIntrAfter);
    B3cPrivRingTraceWriteLog(L"B3cD2PrivWriteErrorCount",
        L"B3cD2PrivWriteErrorDropped", L"B3cD2PrivAckTimeouts",
        L"B3cD2PrivWriteErrors");
    return result;
}

/* Only called after the existing GPU block reset; restore exactly the outer
 * mapping entries C2d changed before releasing its separate arena. */
static VOID GrVmCleanupAfterGpuReset(PSWGPU_DEVICE Dev)
{
    volatile ULONG *smmuPd, *smmuPt;
    ULONG i, readback = 1;
    if (g_GrVm.PtbChanged && g_GrVm.Arena != NULL && g_AcrVm.Arena != NULL) {
        smmuPd = (volatile ULONG *)g_AcrVm.Arena;
        if (g_GrVm.SharedSmmuPt) smmuPt = (volatile ULONG *)((PUCHAR)g_AcrVm.Arena + PAGE_SIZE);
        else smmuPt = (volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_SMMU_PGT_OFF);
        for (i = 0; i < GR_VM_PAGE_COUNT; ++i) {
            smmuPt[g_GrVm.SmmuPteFirst + i] = g_GrVm.OldSmmuPtes[i];
            if (smmuPt[g_GrVm.SmmuPteFirst + i] != g_GrVm.OldSmmuPtes[i]) readback = 0;
        }
        if (!g_GrVm.SharedSmmuPt) smmuPd[g_GrVm.SmmuPdeIndex] = g_GrVm.OldSmmuPde;
        KeMemoryBarrier();
        Wr(Dev->Mc, MC_SMMU_PTC_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_TLB_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        if (smmuPd[g_GrVm.SmmuPdeIndex] != g_GrVm.OldSmmuPde)
            readback = 0;
        TraceDurable(L"B3cC2dOuterMapRestored", readback);
    } else {
        TraceDurable(L"B3cC2dOuterMapRestored", 1u);
    }
    E4Return(Dev, SwGpuPoolGr, g_GrVm.Arena, GR_VM_ARENA_SIZE);
    RtlZeroMemory(&g_GrVm, sizeof(g_GrVm));
}

/* C2b: mirror the reset-enable portion of nvgpu gk20a_init_gr_reset_enable_hw()
 * before FECS/GPCCS ctxsw ucode startup. Only the GM20B GR global/GPCS PRI
 * windows and its two GPC PRI windows are accepted for the firmware list;
 * this deliberately excludes all other BAR0 blocks. Register offsets below
 * are kept separate from values read from BAR0. */
static BOOLEAN B3cIsGrPriAddress(ULONG address)
{
    if ((address >= 0x00400000u && address < 0x00410000u) ||
        (address >= 0x00418000u && address < 0x00420000u) ||
        (address >= 0x00500000u && address < 0x00510000u))
        return TRUE;
    return FALSE;
}

static ULONG B3cRunC2b(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR data = NULL;
    ULONG length = 0, count, result = 0, entryIndex, waitPoll;
    ULONG grAddr, grValue, badAddr = 0, badIndex = 0xFFFFFFFFu;
    ULONG pmcEnable, grPowerRead = 0, grResetAssertRead, grResetReleaseRead;
    ULONG gpcPriBadfCount = 0;
    ULONG mcValue, mcDisabled, mcEnabled, sccInitRead;
    ULONG scrubFecs = 0, scrubGpccs = 0, engineStatus = 0, grStatus = 0;
    ULONG writes = 0, sampleCount = 0, sample[8][3] = { 0 };
    ULONG c2bSwNonctxResetCtlOff = 0x00409614u;
    ULONG c2bGrPowerModeOff = 0x00404170u;
    ULONG c2bSccInitOff = 0x0040802Cu;
    ULONG c2bFecsDmaCtlOff = 0x0040910Cu;
    ULONG c2bGpccsDmaCtlOff = 0x0041A10Cu;
    ULONG c2bGrEngineStatusOff = 0x0040060Cu;
    ULONG c2bGrStatusOff = 0x00400700u;
    ULONG c2bMcEnableOff = 0x00000200u;
    ULONG c2bEngineResetMask = 0x18001000u;

    Step(78);
    B3cPrivRingResetWriteLog();
    B3cCaptureGpcPriDiag(B, L"B3cC2bPreResetGpcPri", L"B3cC2bPreResetGpcPriBadfCount",
                         &gpcPriBadfCount);
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cC2bPmcEnable", pmcEnable);
    if (!(pmcEnable & (1u << 12))) { result = 0xE0000087u; goto done; }

    data = ReadBinParam(L"FwGrSwNonctx", 0x20000u, &length);
    TraceDetail(L"B3cC2bSwNonctxBytes", length);
    if (data == NULL || length == 0 || (length % 8u) != 0) {
        result = 0xE0000087u; goto done;
    }
    count = length / 8u;
    /* Validate the entire user-provided list before the first MMIO write. */
    for (entryIndex = 0; entryIndex < count; ++entryIndex) {
        grAddr = Le32(data + entryIndex * 8u);
        if ((grAddr & 3u) != 0 || !B3cIsGrPriAddress(grAddr)) {
            badAddr = grAddr; badIndex = entryIndex;
            result = 0xE0000087u; goto done;
        }
    }
    TraceDetail(L"B3cC2bSwNonctxCount", count);
    TraceDetail(L"B3cC2bSwNonctxFirstAddr", Le32(data));

    /* nvgpu gk20a_init_gr_prepare() -> gk20a_reset(): reset PGRAPH, BLG,
     * and PERFMON, keeping all unrelated PMC_ENABLE bits (including PMU).
     * hw_mc_gm20b.h confirms MC_ENABLE=0x200 and masks 0x1000/0x08000000/
     * 0x10000000. This reset is bounded by fixed delays and readback checks. */
    mcValue = Rd(B, c2bMcEnableOff);
    TraceDetail(L"B3cC2bMcEnableBeforeReset", mcValue);
    Wr(B, c2bMcEnableOff, mcValue & ~c2bEngineResetMask);
    mcDisabled = Rd(B, c2bMcEnableOff);
    TraceDetail(L"B3cC2bMcEnableDuringReset", mcDisabled);
    if (mcDisabled & c2bEngineResetMask) { result = 0xE0000087u; goto done; }
    Us(20);
    mcValue = Rd(B, c2bMcEnableOff);
    Wr(B, c2bMcEnableOff, mcValue | c2bEngineResetMask);
    mcEnabled = Rd(B, c2bMcEnableOff);
    TraceDetail(L"B3cC2bMcEnableAfterReset", mcEnabled);
    Us(20);
    if ((mcEnabled & c2bEngineResetMask) != c2bEngineResetMask || !(mcEnabled & (1u << 12))) {
        result = 0xE0000087u; goto done;
    }

    /* nvgpu gr_gk20a_ctx_reset(g, 0): request forced-on FE clocks, then
     * assert context reset (0x70), wait 20 us, and release it (0x770).
     * Addresses/values are verified in hw_gr_gm20b.h (gr_fe_pwr_mode_* and
     * gr_fecs_ctxsw_reset_ctl_*). */
    Wr(B, c2bGrPowerModeOff, 0x12u);
    for (waitPoll = 0; waitPoll < 2000u; ++waitPoll) {
        grPowerRead = Rd(B, c2bGrPowerModeOff);
        if (((grPowerRead >> 4) & 1u) == 0u) break;
        Us(10);
    }
    Trace(L"B3cC2bForceClockPolls", waitPoll);
    TraceDetail(L"B3cC2bForceClockReadback", grPowerRead);
    if (waitPoll == 2000u) { result = 0xE0000087u; goto done; }

    Wr(B, c2bSwNonctxResetCtlOff, 0x70u);
    grResetAssertRead = Rd(B, c2bSwNonctxResetCtlOff);
    Us(20);
    Wr(B, c2bSwNonctxResetCtlOff, 0x770u);
    grResetReleaseRead = Rd(B, c2bSwNonctxResetCtlOff);
    Us(20);
    TraceDetail(L"B3cC2bFecsCtxswResetAssert", grResetAssertRead);
    TraceDetail(L"B3cC2bFecsCtxswResetRelease", grResetReleaseRead);

    /* Return FE clocks to automatic mode and wait for the request to finish. */
    Wr(B, c2bGrPowerModeOff, 0x10u);
    for (waitPoll = 0; waitPoll < 2000u; ++waitPoll) {
        grPowerRead = Rd(B, c2bGrPowerModeOff);
        if (((grPowerRead >> 4) & 1u) == 0u) break;
        Us(10);
    }
    Trace(L"B3cC2bAutoClockPolls", waitPoll);
    TraceDetail(L"B3cC2bAutoClockReadback", grPowerRead);
    if (waitPoll == 2000u) { result = 0xE0000087u; goto done; }

    /* nvgpu gk20a_init_gr_reset_enable_hw() clears SCC RAM after ctxsw reset,
     * then writes each validated non-context AV record. */
    Wr(B, c2bSccInitOff, 1u);
    sccInitRead = Rd(B, c2bSccInitOff);
    TraceDetail(L"B3cC2bSccInitReadback", sccInitRead);
    if (!B3cPrivRingDrainBeforeWrites(B)) { result = 0xE0000095u; goto done; }
    for (entryIndex = 0; entryIndex < count; ++entryIndex) {
        grAddr = Le32(data + entryIndex * 8u);
        grValue = Le32(data + entryIndex * 8u + 4u);
        Wr(B, grAddr, grValue);
        if (!B3cPrivRingAfterGrWrite(B, grAddr, grValue)) {
            badAddr = grAddr; badIndex = entryIndex;
            result = 0xE0000095u; goto done;
        }
        ++writes;
        if (entryIndex < RTL_NUMBER_OF(sample)) {
            sample[entryIndex][0] = grAddr;
            sample[entryIndex][1] = grValue;
            sample[entryIndex][2] = 0;
            sampleCount = entryIndex + 1u;
        }
    }
    KeMemoryBarrier();
    for (entryIndex = 0; entryIndex < sampleCount; ++entryIndex)
        sample[entryIndex][2] = Rd(B, sample[entryIndex][0]);
    TraceDetail(L"B3cC2bSwNonctxWrites", writes);
    TraceDetail(L"B3cC2bReadbackCount", sampleCount);
    if (sampleCount) TraceBin(L"B3cC2bReadback", sample, sampleCount * sizeof(sample[0]));

    /* Match nvgpu gr_gk20a_wait_mem_scrubbing() with a finite 2 s ceiling. */
    for (waitPoll = 0; waitPoll < 2000u; ++waitPoll) {
        scrubFecs = Rd(B, c2bFecsDmaCtlOff) & 0x6u;
        scrubGpccs = Rd(B, c2bGpccsDmaCtlOff) & 0x6u;
        if (scrubFecs == 0u && scrubGpccs == 0u) break;
        Ms(1);
    }
    Trace(L"B3cC2bScrubPolls", waitPoll);
    TraceDetail(L"B3cC2bFecsScrubBits", scrubFecs);
    TraceDetail(L"B3cC2bGpccsScrubBits", scrubGpccs);
    if (waitPoll == 2000u) { result = 0xE0000087u; goto done; }

    /* This probe has no GPFIFO channel yet; use the GR engine-busy and FE
     * method busy bits nvgpu checks in its idle helpers, with a hard limit. */
    for (waitPoll = 0; waitPoll < 2000u; ++waitPoll) {
        engineStatus = Rd(B, c2bGrEngineStatusOff);
        grStatus = Rd(B, c2bGrStatusOff);
        if (!(engineStatus & 1u) && !(grStatus & 0x6u)) break;
        Ms(1);
    }
    Trace(L"B3cC2bIdlePolls", waitPoll);
    TraceDetail(L"B3cC2bEngineStatus", engineStatus);
    TraceDetail(L"B3cC2bGrStatus", grStatus);
    if (waitPoll == 2000u) { result = 0xE0000087u; goto done; }

done:
    B3cPrivRingTraceWriteLog(L"B3cC2bPrivWriteErrorCount",
        L"B3cC2bPrivWriteErrorDropped", L"B3cC2bPrivAckTimeouts",
        L"B3cC2bPrivWriteErrors");
    TraceDetail(L"B3cC2bBadIndex", badIndex);
    TraceDetail(L"B3cC2bBadAddress", badAddr);
    if (data != NULL) ExFreePoolWithTag(data, SWGPU_TAG);
    TraceDurable(L"B3cC2bResult", result);
    return result;
}

static ULONG B3cStartFecsLs(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR gpccsInst = NULL, gpccsData = NULL;
    ULONG gpccsInstLen = 0, gpccsDataLen = 0;
    ULONG words[11], wprCmd[4] = { 0x0103100Au, 0u, 1u, 0u };
    /* bootstrap_falcon: 4-byte header, cmd_type + 3 pad, u32 flags,
     * u32 falcon_id. RESET_YES is zero; FECS is falcon ID 2; sizeof = 16. */
    ULONG cmd[4] = { 0x0203100Au, 0x00000001u, 0u, 2u };
    ULONG result = 0, dmemSize, msgHead, msgTail, hpqSize, hpqBase, hpqIndex;
    ULONG msgSize, msgBase, msgEnd, hpqEnd, cursor, i, reg, qhead, qtail, pmcEnable;
    ULONG replyHead = 0, replyTail = 0, replyHdr = 0, replyCtrl = 0, replyPayload = 0, polls = 0;
    ULONG wprReplyHdr = 0, wprReplyCtrl = 0, wprReplyCode = 0, hpqTailAfterWait = 0;
    ULONG bootstrapSeq = 0, hpqSampleNr = 0, hpqTailSamples[8], hpqIrqSamples[8], hpqSamplePolls[8];
    ULONG fecsStartPolls = 0, fecsCpuctlBeforeStart = 0, fecsStartReg = 0;
    ULONG fecsCpuctlAfterStart = 0, fecsMailboxAfterStart = 0;
    ULONG pmuTraceIdx, pmuTraceNr, pmuPcNow[8], pmuPcAfterWait[8], pmuPcNrAfterWait;
    ULONG fecsReady = 0, fecsReadyFirstNonzero = 0, fecsReadyPrev = 0, fecsReadyChangeCount = 0;
    ULONG fecsTraceIdx, fecsTraceNr, fecsPc[8], faultId, faultCount = 0;
    ULONG gpccsHwcfg, gpccsImemSize, gpccsDmemSize, gpccsImemWords, gpccsImemPaddedWords, gpccsDmemWords;
    ULONG fwWord, fwByte, gpccsCpuctlBeforeStart = 0, gpccsStartReg = 0, gpccsCpuctlAfterStart = 0;
    ULONG gpccsStartPolls = 0, gpccsMailboxAfterStart = 0, fecsMailboxWords[8], gpccsCtxswMailboxWords[8];
    ULONG gpccsTraceIdx, gpccsTraceNr, gpccsPc[8], gpccsBootvecBeforeStart = 0;
    ULONG faultRecs[32][5], unit, pcSlot;

    hpqIndex = 0;
    hpqTailAfterWait = 0xFFFFFFFFu;
    Step(65);
    dmemSize = ((Rd(B, PMU_FALCON + FALCON_HWCFG) >> 9) & 0x1FFu) << 8;
    msgHead = Rd(B, PMU_FALCON + 0x4C8);
    msgTail = Rd(B, PMU_FALCON + 0x4CC);
    TraceDetail(L"B3cFecsInitHead", msgHead);
    TraceDetail(L"B3cFecsInitTail", msgTail);
    if (dmemSize < 256u || msgHead == 0xFFFFFFFFu || msgTail == 0xFFFFFFFFu ||
        (msgHead & 3u) || (msgTail & 3u) || msgHead < msgTail || msgHead - msgTail < 44u ||
        msgTail > dmemSize - 44u) { result = 0xE0000078u; goto done; }
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | msgTail);
    for (i = 0; i < RTL_NUMBER_OF(words); ++i) words[i] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    cursor = Rd(B, PMU_FALCON + FALCON_DMEMC0) & 0xFFFFu;
    if ((words[0] & 0xFFFFu) != 0x2A07u || (words[1] & 0xFFu) != 0u || cursor != msgTail + 44u) {
        result = 0xE0000078u; goto done;
    }
    hpqSize = words[2] & 0xFFFFu; hpqBase = words[2] >> 16; hpqIndex = words[3] & 0xFFu;
    msgSize = words[8] & 0xFFFFu; msgBase = words[8] >> 16;
    hpqEnd = hpqBase + hpqSize; msgEnd = msgBase + msgSize;
    TraceDetail(L"B3cFecsHpqBase", hpqBase); TraceDetail(L"B3cFecsHpqSize", hpqSize);
    TraceDetail(L"B3cFecsHpqIndex", hpqIndex); TraceDetail(L"B3cFecsMsgqBase", msgBase);
    TraceDetail(L"B3cFecsMsgqSize", msgSize); TraceDetail(L"B3cFecsInitCursor", cursor);
    if (hpqIndex >= 4u || hpqBase < 4u || msgBase < 4u || hpqSize < 32u || msgSize < 64u ||
        hpqEnd > dmemSize || msgEnd > dmemSize ||
        (hpqBase < msgEnd && msgBase < hpqEnd) || msgTail < msgBase || msgTail + 44u > msgEnd ||
        msgHead < msgBase || msgHead >= msgEnd) { result = 0xE0000078u; goto done; }
    Wr(B, PMU_FALCON + 0x4CC, msgTail + 44u); /* acknowledge the validated INIT */

    reg = 0x4A0u + hpqIndex * 4u;
    qhead = Rd(B, PMU_FALCON + reg);
    qtail = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
    hpqEnd = hpqBase + hpqSize;
    TraceDetail(L"B3cFecsHpqHeadBefore", qhead); TraceDetail(L"B3cFecsHpqTail", qtail);
    if ((qhead & 3u) || (qtail & 3u) || qhead < hpqBase || qhead >= hpqEnd || qtail < hpqBase || qtail >= hpqEnd) {
        result = 0xE0000078u; goto done;
    }
    Step(66);
    /* INIT_WPR_REGION (region 1, offset 0) is required before bootstrap.
     * mesa-switch-woa post_wpr() publishes it by updating HPQ head; it does
     * not use a separate doorbell register. Keep one dword empty in the ring. */
    if (qhead >= qtail && hpqEnd - qhead < 20u) {
        if (qtail <= hpqBase + 16u) { result = 0xE0000079u; goto done; }
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | qhead);
        Wr(B, PMU_FALCON + FALCON_DMEMD0, 4u);
        qhead = hpqBase;
    }
    if (qhead < qtail && qtail - qhead <= 16u) { result = 0xE0000079u; goto done; }
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | qhead);
    for (i = 0; i < RTL_NUMBER_OF(wprCmd); ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, wprCmd[i]);
    KeMemoryBarrier();
    qhead += sizeof(wprCmd);
    Wr(B, PMU_FALCON + reg, qhead);
    TraceDetail(L"B3cFecsWprCommandHeadAfter", qhead);
    TraceBin(L"B3cFecsWprInitCommand", wprCmd, sizeof(wprCmd));
    for (polls = 0; polls < 2000u; ++polls) {
        replyHead = Rd(B, PMU_FALCON + 0x4C8); replyTail = Rd(B, PMU_FALCON + 0x4CC);
        if (replyHead == 0xFFFFFFFFu || replyTail == 0xFFFFFFFFu || (replyHead & 3u) || (replyTail & 3u) ||
            replyHead < msgBase || replyHead >= msgEnd || replyTail < msgBase || replyTail >= msgEnd) {
            result = 0xE0000079u; goto done;
        }
        if (replyHead != replyTail) break;
        Ms(1);
    }
    Trace(L"B3cFecsWprReplyPolls", polls);
    if (polls == 2000u) { result = 0xE0000079u; goto done; }
    if (replyHead < replyTail || replyHead - replyTail < 12u) { result = 0xE0000079u; goto done; }
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | replyTail);
    wprReplyHdr = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    wprReplyCtrl = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    wprReplyCode = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    TraceDetail(L"B3cFecsWprReplyHeader", wprReplyHdr); TraceDetail(L"B3cFecsWprReplyCtrl", wprReplyCtrl);
    TraceDetail(L"B3cFecsWprReplyCode", wprReplyCode);
    if ((wprReplyHdr & 0xFFu) != 0x0Au || ((wprReplyHdr >> 8) & 0xFFu) != 12u ||
        ((wprReplyHdr >> 24) & 0xFFu) != 1u || ((wprReplyHdr >> 18) & 1u) ||
        (wprReplyCtrl & 0xFFu) != 0u || wprReplyCode != 0u) {
        result = 0xE0000079u; goto done;
    }
    Wr(B, PMU_FALCON + 0x4CC, replyTail + 12u);
    TraceDetail(L"B3cFecsHpqHeadAfterWprInit", Rd(B, PMU_FALCON + reg));
    TraceDetail(L"B3cFecsHpqTailAfterWprInit", Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u));
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointAfterWprAck");

    Step(67);
    /* Claude: keep 'reg' as the HPQ head register offset; 0.4.28 reused it for
     * PMC_ENABLE, so Step 68 accessed PMU_FALCON + 0xC0013120 (bugcheck). */
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cFecsPmcEnableBefore", pmcEnable);
    Wr(B, NV_PMC_ENABLE, pmcEnable | (1u << 12));
    (VOID)Rd(B, NV_PMC_ENABLE);
    TraceDetail(L"B3cFecsPmcEnableAfter", Rd(B, NV_PMC_ENABLE));
    if (!(Rd(B, NV_PMC_ENABLE) & (1u << 12))) { result = 0xE0000078u; goto done; }
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointAfterGrEnable");
    result = B3cRunC2b(Dev);
    if (result != 0u) goto done;
    result = B3cInitGpcMmu(Dev);
    if (result != 0u) goto done;
    /* Retain 0x419A00 only as a legacy comparison. The GM20B FECS ctxsw
     * mailbox sequence is written immediately before the FECS start below. */
    TraceDetail(L"B3cFecsLegacyMailbox419A00BeforeStartSeq", Rd(B, 0x419A00u));
    TraceDetail(L"B3cFecsCtxswMailbox0BeforeStartSeq", Rd(B, 0x409800u));
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointBeforeFecsStartMailboxSeq");
    Step(68);
    qhead = Rd(B, PMU_FALCON + reg);
    qtail = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
    TraceDetail(L"B3cFecsHpqHeadBeforeBootstrap", qhead); TraceDetail(L"B3cFecsHpqTailBeforeBootstrap", qtail);
    if ((qhead & 3u) || (qtail & 3u) || qhead < hpqBase || qhead >= hpqEnd || qtail < hpqBase || qtail >= hpqEnd) {
        result = 0xE0000078u; goto done;
    }
    if (qhead >= qtail && hpqEnd - qhead < sizeof(cmd) + 4u) {
        if (qtail <= hpqBase + sizeof(cmd)) { result = 0xE0000078u; goto done; }
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | qhead);
        Wr(B, PMU_FALCON + FALCON_DMEMD0, 4u);
        qhead = hpqBase;
    }
    if (qhead < qtail && qtail - qhead <= sizeof(cmd)) { result = 0xE0000078u; goto done; }
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | qhead);
    bootstrapSeq = ((wprReplyHdr >> 24) & 0xFFu) + 1u;
    if (bootstrapSeq == 0 || bootstrapSeq > 0xFFu) bootstrapSeq = 1u;
    cmd[0] = (cmd[0] & 0x00FFFFFFu) | (bootstrapSeq << 24);
    TraceDetail(L"B3cFecsBootstrapSeq", bootstrapSeq);
    Step(71);
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointBeforeBootstrap");
    if (Rd(B, PMU_FALCON + FALCON_CPUCTL) & 0x10u) { result = 0xE000007Bu; goto done; }
    Step(72);
    for (i = 0; i < RTL_NUMBER_OF(cmd); ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, cmd[i]);
    KeMemoryBarrier();
    qhead += sizeof(cmd);
    Wr(B, PMU_FALCON + reg, qhead);
    TraceDetail(L"B3cFecsCommandHeadAfter", qhead);
    TraceBin(L"B3cFecsBootstrapCommand", cmd, sizeof(cmd));

    Step(69);
    TraceDetail(L"B3cFecsPmuIrqStatAfterPost", Rd(B, PMU_FALCON + 0x008));
    TraceDetail(L"B3cFecsPmuCpuctlAfterPost", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    if (Rd(B, PMU_FALCON + FALCON_CPUCTL) & 0x10u) { result = 0xE000007Cu; goto done; }
    pmuTraceIdx = Rd(B, PMU_FALCON + 0x148);
    pmuTraceNr = (pmuTraceIdx >> 16) & 0xFFu;
    if (pmuTraceNr > RTL_NUMBER_OF(pmuPcNow)) pmuTraceNr = RTL_NUMBER_OF(pmuPcNow);
    TraceDetail(L"B3cFecsPmuTraceIdxAfterPost", pmuTraceIdx);
    TraceDetail(L"B3cFecsPmuTraceCountAfterPost", pmuTraceNr);
    for (i = 0; i < pmuTraceNr; ++i) { Wr(B, PMU_FALCON + 0x148, i); pmuPcNow[i] = Rd(B, PMU_FALCON + 0x14C); }
    if (pmuTraceNr) TraceBin(L"B3cFecsPmuTraceAfterPost", pmuPcNow, pmuTraceNr * sizeof(ULONG));
    for (polls = 0; polls < 2000u; ++polls) {
        replyHead = Rd(B, PMU_FALCON + 0x4C8); replyTail = Rd(B, PMU_FALCON + 0x4CC);
        if (replyHead == 0xFFFFFFFFu || replyTail == 0xFFFFFFFFu || (replyHead & 3u) || (replyTail & 3u) ||
            replyHead < msgBase || replyHead >= msgEnd || replyTail < msgBase || replyTail >= msgEnd) {
            result = 0xE0000078u; goto done;
        }
        if ((polls % 250u) == 0u && hpqSampleNr < RTL_NUMBER_OF(hpqTailSamples)) {
            hpqSamplePolls[hpqSampleNr] = polls;
            hpqTailSamples[hpqSampleNr] = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
            hpqIrqSamples[hpqSampleNr] = Rd(B, PMU_FALCON + 0x008);
            ++hpqSampleNr;
        }
        if (replyHead != replyTail) break;
        Ms(1);
    }
    TraceDetail(L"B3cFecsHpqSampleCount", hpqSampleNr);
    if (hpqSampleNr) {
        TraceBin(L"B3cFecsHpqSamplePolls", hpqSamplePolls, hpqSampleNr * sizeof(ULONG));
        TraceBin(L"B3cFecsHpqTailSamples", hpqTailSamples, hpqSampleNr * sizeof(ULONG));
        TraceBin(L"B3cFecsPmuIrqSamples", hpqIrqSamples, hpqSampleNr * sizeof(ULONG));
    }
    Trace(L"B3cFecsReplyPolls", polls);
    if (polls == 2000u) {
        result = (Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u) != qhead) ? 0xE000007Au : 0xE0000078u;
        goto done;
    }
    /* The response is the ACR unit's 12-byte reply; reject wrap/short records. */
    if (replyHead < replyTail || replyHead - replyTail < 12u) { result = 0xE0000078u; goto done; }
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | replyTail);
    replyHdr = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    replyCtrl = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    replyPayload = Rd(B, PMU_FALCON + FALCON_DMEMD0);
    TraceDetail(L"B3cFecsReplyHeader", replyHdr); TraceDetail(L"B3cFecsReplyCtrl", replyCtrl);
    TraceDetail(L"B3cFecsReplyPayload", replyPayload);
    if ((replyHdr & 0xFFu) != 0x0Au || ((replyHdr >> 8) & 0xFFu) != 12u ||
        ((replyHdr >> 24) & 0xFFu) != bootstrapSeq || ((replyHdr >> 18) & 1u) ||
        replyPayload != 2u) {
        result = 0xE0000078u; goto done;
    }
    Wr(B, PMU_FALCON + 0x4CC, replyTail + 12u);
    /* GPCCS is non-secure on GM20B. Nouveau gf100_gr_init_ctxctl_ext() calls
     * gf100_gr_init_fw() for GPCCS, then starts GPCCS before FECS; nvgpu
     * gr_gm20b_load_ctxsw_ucode() uses the direct Falcon IMEM/DMEM path too. */
    Step(73);
    TraceDetail(L"B3cFecsDmaCtlBeforeStartPrep", Rd(B, 0x40910Cu));
    gpccsInst = ReadBinParam(L"FwGpccsInst", 0x20000u, &gpccsInstLen);
    gpccsData = ReadBinParam(L"FwGpccsData", 0x20000u, &gpccsDataLen);
    TraceDetail(L"B3cGpccsInstLen", gpccsInstLen);
    TraceDetail(L"B3cGpccsDataLen", gpccsDataLen);
    if (gpccsInst == NULL || gpccsData == NULL) { result = 0xE000007Eu; goto done; }
    gpccsHwcfg = Rd(B, 0x41A108u);
    gpccsImemSize = (gpccsHwcfg & 0x1FFu) << 8;
    gpccsDmemSize = ((gpccsHwcfg >> 9) & 0x1FFu) << 8;
    gpccsImemWords = (gpccsInstLen + 3u) / 4u;
    /* Nouveau nvkm_falcon_v1_load_imem() pads code to a 0x40-word block. */
    gpccsImemPaddedWords = (gpccsImemWords + 0x3Fu) & ~0x3Fu;
    gpccsDmemWords = (gpccsDataLen + 3u) / 4u;
    TraceDetail(L"B3cGpccsHwcfg", gpccsHwcfg);
    TraceDetail(L"B3cGpccsImemSize", gpccsImemSize);
    TraceDetail(L"B3cGpccsDmemSize", gpccsDmemSize);
    TraceDetail(L"B3cGpccsImemSourceWords", gpccsImemWords);
    TraceDetail(L"B3cGpccsImemPaddedWords", gpccsImemPaddedWords);
    if (!gpccsInstLen || !gpccsDataLen || gpccsInstLen > 0x20000u || gpccsDataLen > 0x20000u ||
        gpccsImemSize < 4u || gpccsDmemSize < 4u || gpccsImemPaddedWords > gpccsImemSize / 4u ||
        gpccsDmemWords > gpccsDmemSize / 4u) { result = 0xE000007Fu; goto done; }
    Step(74);
    /* Nouveau gf100_gr_init_fw() loads both segments at offset zero.
     * nvkm_falcon_v1_load_imem() advances tags and pads to 0x40 words. */
    Wr(B, 0x41A1C0u, (1u << 24));
    for (i = 0; i < gpccsDmemWords; ++i) {
        fwWord = 0;
        for (fwByte = 0; fwByte < 4u && i * 4u + fwByte < gpccsDataLen; ++fwByte)
            fwWord |= ((ULONG)gpccsData[i * 4u + fwByte]) << (fwByte * 8u);
        Wr(B, 0x41A1C4u, fwWord);
    }
    Wr(B, 0x41A180u, (1u << 24));
    for (i = 0; i < gpccsImemPaddedWords; ++i) {
        fwWord = 0;
        for (fwByte = 0; fwByte < 4u && i * 4u + fwByte < gpccsInstLen; ++fwByte)
            fwWord |= ((ULONG)gpccsInst[i * 4u + fwByte]) << (fwByte * 8u);
        if ((i & 0x3Fu) == 0u) Wr(B, 0x41A188u, i >> 6);
        Wr(B, 0x41A184u, fwWord);
    }
    TraceDetail(L"B3cGpccsImemWordsWritten", gpccsImemPaddedWords);
    TraceDetail(L"B3cGpccsDmemWordsWritten", gpccsDmemWords);

    Step(75);
    /* Match nvgpu gr_gm20b_load_ctxsw_ucode() secure-start order: clear
     * mailbox 0, write mailbox 1 = 1, then clear mailbox 6. Nouveau
     * gf100_gr_init_ctxctl_ext() separately clears MB0 and DMACTL before
     * starting GPCCS followed by FECS. */
    TraceDetail(L"B3cFecsCtxswMailbox0BeforeSecureSeq", Rd(B, 0x409800u));
    Wr(B, 0x409840u, 0xFFFFFFFFu);
    TraceDetail(L"B3cFecsCtxswMailbox0AfterClear", Rd(B, 0x409800u));
    Wr(B, 0x409804u, 1u);
    TraceDetail(L"B3cFecsCtxswMailbox1Set", Rd(B, 0x409804u));
    Wr(B, 0x409858u, 0xFFFFFFFFu);
    TraceDetail(L"B3cFecsCtxswMailboxClear6Snapshot", Rd(B, 0x409858u));
    TraceDetail(L"B3cFecsCtxswMailbox6", Rd(B, 0x409818u));
    TraceDetail(L"B3cFecsCtxswClearRegSnapshot", Rd(B, 0x409840u));
    TraceDetail(L"B3cFecsCtxswMailbox0AfterSecureSeq", Rd(B, 0x409800u));
    TraceDetail(L"B3cGpccsDmaCtlBeforeStart", Rd(B, 0x41A10Cu));
    Wr(B, 0x41A10Cu, 0u);
    TraceDetail(L"B3cGpccsDmaCtlAfterStartPrep", Rd(B, 0x41A10Cu));
    TraceDetail(L"B3cFecsDmaCtlBeforeStart", Rd(B, 0x40910Cu));
    Wr(B, 0x40910Cu, 0u);
    TraceDetail(L"B3cFecsDmaCtlAfterStartPrep", Rd(B, 0x40910Cu));
    /* Nouveau's direct PIO path starts GPCCS without programming BOOTVEC. */
    gpccsBootvecBeforeStart = Rd(B, 0x41A104u);
    TraceDetail(L"B3cGpccsBootvecBeforeStart", gpccsBootvecBeforeStart);
    gpccsCpuctlBeforeStart = Rd(B, 0x41A100u);
    gpccsStartReg = (gpccsCpuctlBeforeStart & 0x40u) ? 0x41A130u : 0x41A100u;
    TraceDetail(L"B3cGpccsCpuctlBeforeStart", gpccsCpuctlBeforeStart);
    TraceDetail(L"B3cGpccsStartUsesAlias", gpccsStartReg == 0x41A130u ? 1u : 0u);
    Wr(B, gpccsStartReg, 2u);
    gpccsCpuctlAfterStart = Rd(B, 0x41A100u);
    gpccsMailboxAfterStart = Rd(B, 0x41A040u);
    TraceDetail(L"B3cGpccsCpuctlAfterStart", gpccsCpuctlAfterStart);
    TraceDetail(L"B3cGpccsMailbox0AfterStart", gpccsMailboxAfterStart);
    TraceDetail(L"B3cGpccsBootvecAfterStart", Rd(B, 0x41A104u));
    for (gpccsStartPolls = 0; gpccsStartPolls < 2000u; ++gpccsStartPolls) {
        gpccsCpuctlAfterStart = Rd(B, 0x41A100u);
        if (!(gpccsCpuctlAfterStart & 0x10u)) break;
        Ms(1);
    }
    Trace(L"B3cGpccsStartPolls", gpccsStartPolls);
    if (gpccsStartPolls == 2000u) { result = 0xE0000080u; goto done; }

    fecsCpuctlBeforeStart = Rd(B, 0x409100u);
    fecsStartReg = (fecsCpuctlBeforeStart & 0x40u) ? 0x409130u : 0x409100u;
    TraceDetail(L"B3cFecsCpuctlBeforeStart", fecsCpuctlBeforeStart);
    TraceDetail(L"B3cFecsStartUsesAlias", (fecsStartReg == 0x409130u) ? 1u : 0u);
    Wr(B, fecsStartReg, 2u); /* nvkm_falcon_v1_start(): CPUCTL_ALIAS when bit 6 is enabled */
    fecsCpuctlAfterStart = Rd(B, 0x409100u);
    fecsMailboxAfterStart = Rd(B, 0x409040u);
    TraceDetail(L"B3cFecsCpuctlAfterStart", fecsCpuctlAfterStart);
    TraceDetail(L"B3cFecsMailbox0AfterStart", fecsMailboxAfterStart);
    for (fecsStartPolls = 0; fecsStartPolls < 2000u; ++fecsStartPolls) {
        fecsCpuctlAfterStart = Rd(B, 0x409100u);
        if (!(fecsCpuctlAfterStart & 0x10u)) break;
        Ms(1);
    }
    Trace(L"B3cFecsStartPolls", fecsStartPolls);
    if (fecsStartPolls == 2000u) { result = 0xE000007Du; goto done; }
    for (polls = 0; polls < 2000u; ++polls) {
        fecsReady = Rd(B, 0x409800u);
        if (fecsReady != fecsReadyPrev) { fecsReadyPrev = fecsReady; ++fecsReadyChangeCount; }
        if (fecsReady != 0u && fecsReadyFirstNonzero == 0u) fecsReadyFirstNonzero = fecsReady;
        if (fecsReady == 0x10u || (fecsReady & 1u)) break;
        Ms(1);
    }
    TraceDetail(L"B3cFecsReadyMailbox", fecsReady); TraceDetail(L"B3cFecsReadyFirstNonzero", fecsReadyFirstNonzero);
    TraceDetail(L"B3cFecsReadyMailboxChanges", fecsReadyChangeCount); Trace(L"B3cFecsReadyPolls", polls);
    if (polls == 2000u) result = 0xE0000078u;
    TraceDetail(L"B3cFecsLegacyMailbox419A00AfterWait", Rd(B, 0x419A00u));
    for (i = 0; i < RTL_NUMBER_OF(fecsMailboxWords); ++i) fecsMailboxWords[i] = Rd(B, 0x409800u + i * 4u);
    TraceBin(L"B3cFecsCtxswMailboxWords", fecsMailboxWords, sizeof(fecsMailboxWords));
done:
    Step(70);
    TraceDurable(L"B3cFecsStepResult", result);
    if (hpqIndex < 4u) {
        hpqTailAfterWait = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
        TraceDetail(L"B3cFecsHpqHeadAfterWait", Rd(B, PMU_FALCON + 0x4A0u + hpqIndex * 4u));
        TraceDetail(L"B3cFecsHpqTailAfterWait", hpqTailAfterWait);
    } else {
        TraceDetail(L"B3cFecsHpqHeadAfterWait", 0xFFFFFFFFu);
        TraceDetail(L"B3cFecsHpqTailAfterWait", 0xFFFFFFFFu);
    }
    TraceDetail(L"B3cFecsMsgHeadAfterWait", Rd(B, PMU_FALCON + 0x4C8));
    TraceDetail(L"B3cFecsMsgTailAfterWait", Rd(B, PMU_FALCON + 0x4CC));
    TraceDetail(L"B3cFecsPmuIrqStatAfterWait", Rd(B, PMU_FALCON + 0x008));
    pmuTraceIdx = Rd(B, PMU_FALCON + 0x148);
    pmuPcNrAfterWait = (pmuTraceIdx >> 16) & 0xFFu;
    if (pmuPcNrAfterWait > RTL_NUMBER_OF(pmuPcAfterWait)) pmuPcNrAfterWait = RTL_NUMBER_OF(pmuPcAfterWait);
    TraceDetail(L"B3cFecsPmuTraceIdxAfterWait", pmuTraceIdx);
    TraceDetail(L"B3cFecsPmuTraceCountAfterWait", pmuPcNrAfterWait);
    for (i = 0; i < pmuPcNrAfterWait; ++i) { Wr(B, PMU_FALCON + 0x148, i); pmuPcAfterWait[i] = Rd(B, PMU_FALCON + 0x14C); }
    if (pmuPcNrAfterWait) TraceBin(L"B3cFecsPmuTraceAfterWait", pmuPcAfterWait, pmuPcNrAfterWait * sizeof(ULONG));
    /* Touch GR/FECS registers only when GR is enabled (an early exit before
     * Step 67 leaves PMC_ENABLE bit 12 clear). */
    TraceDetail(L"B3cFecsGrEnabledAtRead", (Rd(B, NV_PMC_ENABLE) >> 12) & 1u);
    if (Rd(B, NV_PMC_ENABLE) & (1u << 12)) {
    TraceDetail(L"B3cFecsCpuctl", Rd(B, 0x409100u)); TraceDetail(L"B3cFecsSctl", Rd(B, 0x409240u));
    TraceDetail(L"B3cFecsMailbox0", Rd(B, 0x409040u)); TraceDetail(L"B3cFecsMailbox1", Rd(B, 0x409044u));
    TraceDetail(L"B3cFecsLegacyMailbox419A00Final", Rd(B, 0x419A00u));
    fecsTraceIdx = Rd(B, 0x409148u); fecsTraceNr = (fecsTraceIdx >> 16) & 0xFFu;
    if (fecsTraceNr > RTL_NUMBER_OF(fecsPc)) fecsTraceNr = RTL_NUMBER_OF(fecsPc);
    TraceDetail(L"B3cFecsTraceIdx", fecsTraceIdx); TraceDetail(L"B3cFecsTraceCount", fecsTraceNr);
    for (pcSlot = 0; pcSlot < fecsTraceNr; ++pcSlot) { Wr(B, 0x409148u, pcSlot); fecsPc[pcSlot] = Rd(B, 0x40914Cu); }
    if (fecsTraceNr) TraceBin(L"B3cFecsTracePc", fecsPc, fecsTraceNr * sizeof(ULONG));
    TraceDetail(L"B3cFecsIrqStat", Rd(B, 0x409008u)); TraceDetail(L"B3cFecsIrqMask", Rd(B, 0x409018u));
    TraceDetail(L"B3cGpccsCpuctl", Rd(B, 0x41A100u)); TraceDetail(L"B3cGpccsSctl", Rd(B, 0x41A240u));
    TraceDetail(L"B3cGpccsFalconMailbox0", Rd(B, 0x41A040u)); TraceDetail(L"B3cGpccsFalconMailbox1", Rd(B, 0x41A044u));
    TraceDetail(L"B3cGpccsBootvecFinal", Rd(B, 0x41A104u));
    gpccsTraceIdx = Rd(B, 0x41A148u); gpccsTraceNr = (gpccsTraceIdx >> 16) & 0xFFu;
    if (gpccsTraceNr > RTL_NUMBER_OF(gpccsPc)) gpccsTraceNr = RTL_NUMBER_OF(gpccsPc);
    TraceDetail(L"B3cGpccsTraceIdx", gpccsTraceIdx); TraceDetail(L"B3cGpccsTraceCount", gpccsTraceNr);
    for (pcSlot = 0; pcSlot < gpccsTraceNr; ++pcSlot) { Wr(B, 0x41A148u, pcSlot); gpccsPc[pcSlot] = Rd(B, 0x41A14Cu); }
    if (gpccsTraceNr) TraceBin(L"B3cGpccsTracePc", gpccsPc, gpccsTraceNr * sizeof(ULONG));
    TraceDetail(L"B3cGpccsIrqStat", Rd(B, 0x41A008u)); TraceDetail(L"B3cGpccsIrqMask", Rd(B, 0x41A018u));
    for (i = 0; i < RTL_NUMBER_OF(gpccsCtxswMailboxWords); ++i) gpccsCtxswMailboxWords[i] = Rd(B, 0x41A800u + i * 4u);
    TraceBin(L"B3cGpccsCtxswMailboxWords", gpccsCtxswMailboxWords, sizeof(gpccsCtxswMailboxWords));
    }
    TraceDetail(L"B3cFecsPfifoIntr0", Rd(B, 0x2100)); faultId = Rd(B, 0x259C); TraceDetail(L"B3cFecsFaultId", faultId);
    for (unit = 0; unit < 32u && faultCount < RTL_NUMBER_OF(faultRecs); ++unit) if (faultId & (1u << unit)) {
        faultRecs[faultCount][0] = unit; faultRecs[faultCount][1] = Rd(B, 0x2800u + unit * 0x10u);
        faultRecs[faultCount][2] = Rd(B, 0x2804u + unit * 0x10u); faultRecs[faultCount][3] = Rd(B, 0x2808u + unit * 0x10u);
        faultRecs[faultCount][4] = Rd(B, 0x280Cu + unit * 0x10u); ++faultCount;
    }
    TraceDetail(L"B3cFecsFaultRecCount", faultCount);
    if (faultCount) TraceErrorBin(L"B3cFecsFaultRecs", faultRecs, faultCount * sizeof(faultRecs[0]));
    TraceDurable(L"B3cFecsResult", result);
    if (gpccsData != NULL) ExFreePoolWithTag(gpccsData, SWGPU_TAG);
    if (gpccsInst != NULL) ExFreePoolWithTag(gpccsInst, SWGPU_TAG);
    return result;
}

static ULONG StartGpuFirmware(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR bl = NULL, uc = NULL, img = NULL, boot = NULL, wprImage = NULL;
    ULONG blLen = 0, ucLen = 0, i, ok, err = 0, sum;
    ULONG blMagic, blHdrOff, blDataOff, blDataSize, startTag, dmemLoadOff, codeOff, codeSize, bootSize;
    ULONG ucMagic, ucHdrOff, ucDataOff, ucDataSize;
    ULONG sigDbgOff, sigDbgSize, sigPrdOff, sigPrdSize, patchLoc, patchSig, ldOff, loc, sig;
    ULONG nsOff, nsSize, dataBase, dataSize, numApps, app0Off, app0Size, sigSrc;
    ULONG hwcfg, imemSize, dmemSize, imemBase, dbgFused;
    ULONG wprRawStart, wprRawLimit, wprAllocLen = 0, wprUsed = 0;
    ULONG bootPolls, pmuArgv = 0, fecsErr = 0, c1Err = 0, c2aErr = 0, c2cErr = 0, c2dErr = 0, c2eErr = 0, d1Err = 0, d2Err = 0;
    ULONG c1GoldenBytes = 0, c1ZcullBytes = 0, c1PmBytes = 0;
    BOOLEAN acrAttempted = FALSE;
    ULONGLONG arenaIova = 0, instIova = 0;
    ULONGLONG wprStart, wprEnd;
    ULONG desc[19];

    Step(40);   /* firmware blobs from the registry */
    bl = ReadBinParam(L"FwAcrBl", ACR_BL_MAX, &blLen);
    uc = ReadBinParam(L"FwAcrUcodeLoad", ACR_UC_MAX, &ucLen);
    TraceDetail(L"B3aBlLen", blLen);
    TraceDetail(L"B3aUcLen", ucLen);
    if (bl == NULL || uc == NULL) { err = 0xE0000041u; goto out; }

    Step(41);   /* acr/bl.bin: nvfw_bin_hdr (24 bytes) + nvfw_bl_desc (24 bytes) */
    if (blLen < 24) { err = 0xE0000042u; goto out; }
    blMagic = Le32(bl);
    blHdrOff = Le32(bl + 12);
    blDataOff = Le32(bl + 16);
    blDataSize = Le32(bl + 20);
    TraceDetail(L"B3aBlMagic", blMagic);
    TraceDetail(L"B3aBlHdrOff", blHdrOff);
    TraceDetail(L"B3aBlDataOff", blDataOff);
    TraceDetail(L"B3aBlDataSize", blDataSize);
    if ((blMagic != 0x000010DEu && blMagic != 0x3B1D14F0u) ||
        !InBlob(blLen, blHdrOff, 24) || !InBlob(blLen, blDataOff, blDataSize)) { err = 0xE0000043u; goto out; }
    startTag = Le32(bl + blHdrOff);
    dmemLoadOff = Le32(bl + blHdrOff + 4);
    codeOff = Le32(bl + blHdrOff + 8);
    codeSize = Le32(bl + blHdrOff + 12);
    TraceDetail(L"B3aBlStartTag", startTag);
    TraceDetail(L"B3aBlDmemLoadOff", dmemLoadOff);
    TraceDetail(L"B3aBlCodeOff", codeOff);
    TraceDetail(L"B3aBlCodeSize", codeSize);
    if (codeSize == 0 || codeSize > 0x1000 || startTag > 0xFFFF || !InBlob(blDataSize, codeOff, codeSize)) {
        err = 0xE0000044u; goto out;
    }
    bootSize = (codeSize + 0xFFu) & ~0xFFu;

    Step(42);   /* acr/ucode_load.bin: bin hdr + nvfw_hs_header + nvfw_hs_load_header */
    if (ucLen < 24) { err = 0xE0000045u; goto out; }
    ucMagic = Le32(uc);
    ucHdrOff = Le32(uc + 12);
    ucDataOff = Le32(uc + 16);
    ucDataSize = Le32(uc + 20);
    TraceDetail(L"B3aUcMagic", ucMagic);
    TraceDetail(L"B3aUcHdrOff", ucHdrOff);
    TraceDetail(L"B3aUcDataOff", ucDataOff);
    TraceDetail(L"B3aUcDataSize", ucDataSize);
    if ((ucMagic != 0x000010DEu && ucMagic != 0x3B1D14F0u) ||
        !InBlob(ucLen, ucHdrOff, 32) || !InBlob(ucLen, ucDataOff, ucDataSize) || ucDataSize == 0) {
        err = 0xE0000046u; goto out;
    }
    sigDbgOff = Le32(uc + ucHdrOff);
    sigDbgSize = Le32(uc + ucHdrOff + 4);
    sigPrdOff = Le32(uc + ucHdrOff + 8);
    sigPrdSize = Le32(uc + ucHdrOff + 12);
    patchLoc = Le32(uc + ucHdrOff + 16);
    patchSig = Le32(uc + ucHdrOff + 20);
    ldOff = Le32(uc + ucHdrOff + 24);
    TraceDetail(L"B3aSigDbgOff", sigDbgOff);
    TraceDetail(L"B3aSigDbgSize", sigDbgSize);
    TraceDetail(L"B3aSigPrdOff", sigPrdOff);
    TraceDetail(L"B3aSigPrdSize", sigPrdSize);
    TraceDetail(L"B3aPatchLoc", patchLoc);
    TraceDetail(L"B3aPatchSig", patchSig);
    TraceDetail(L"B3aLoadHdrOff", ldOff);
    /* NVIDIA's standard format (0x10de) stores the patch location and the
     * signature index indirectly; the early nouveau format stores them. */
    if (ucMagic == 0x000010DEu) {
        if (!InBlob(ucLen, patchLoc, 4) || !InBlob(ucLen, patchSig, 4)) { err = 0xE0000047u; goto out; }
        loc = Le32(uc + patchLoc);
        sig = Le32(uc + patchSig);
    } else {
        loc = patchLoc;
        sig = patchSig;
    }
    TraceDetail(L"B3aLoc", loc);
    TraceDetail(L"B3aSig", sig);
    if (!InBlob(ucLen, ldOff, 20)) { err = 0xE0000048u; goto out; }
    nsOff = Le32(uc + ldOff);
    nsSize = Le32(uc + ldOff + 4);
    dataBase = Le32(uc + ldOff + 8);
    dataSize = Le32(uc + ldOff + 12);
    numApps = Le32(uc + ldOff + 16);
    TraceDetail(L"B3aNsOff", nsOff);
    TraceDetail(L"B3aNsSize", nsSize);
    TraceDetail(L"B3aDataBase", dataBase);
    TraceDetail(L"B3aDataSize", dataSize);
    TraceDetail(L"B3aNumApps", numApps);
    if (numApps == 0 || numApps > 8 || !InBlob(ucLen, ldOff + 20, numApps * 8)) { err = 0xE0000049u; goto out; }
    app0Off = Le32(uc + ldOff + 20);
    app0Size = Le32(uc + ldOff + 20 + numApps * 4);
    TraceDetail(L"B3aApp0Off", app0Off);
    TraceDetail(L"B3aApp0Size", app0Size);
    if (sigPrdSize == 0 || sigPrdSize > 0x100 || (sigPrdSize & 3u) || sigDbgSize != sigPrdSize ||
        sig > ucLen || !InBlob(ucLen, sigPrdOff, sig) ||
        !InBlob(ucLen, sigPrdOff + sig, sigPrdSize) ||
        !InBlob(ucLen, sigDbgOff, sig) || !InBlob(ucLen, sigDbgOff + sig, sigDbgSize) ||
        !InBlob(ucDataSize, loc, sigPrdSize) || !InBlob(ucDataSize, nsOff, nsSize) ||
        !InBlob(ucDataSize, app0Off, app0Size) || !InBlob(ucDataSize, dataBase, dataSize) ||
        loc < dataBase) {
        err = 0xE000004Au; goto out;
    }

    Step(43);   /* signed image in memory: data section + the signature for this fuse */
    dbgFused = (Rd(B, PMU_FALCON + PMU_DEBUG) & 0x00100000u) ? 1 : 0;
    TraceDetail(L"B3aDebugFused", dbgFused);
    sigSrc = (dbgFused ? sigDbgOff : sigPrdOff) + sig;
    img = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, ucDataSize, SWGPU_TAG);
    if (img == NULL) { err = 0xE000004Bu; goto out; }
    RtlCopyMemory(img, uc + ucDataOff, ucDataSize);
    RtlCopyMemory(img + loc, uc + sigSrc, sigPrdSize);
    TraceDetail(L"B3aSigWord0", Le32(img + loc));
    TraceDetail(L"B3aDmemSign", loc - dataBase);
    for (sum = 0, i = 0; i + 4 <= ucDataSize; i += 4) sum += Le32(img + i);
    TraceDetail(L"B3aImgSum", sum);

    Step(44);   /* PMU reset, scrub, bootloader -> top of IMEM by PIO */
    PmuReset(B, L"B3aScrubPolls");
    hwcfg = Rd(B, PMU_FALCON + FALCON_HWCFG);
    imemSize = (hwcfg & 0x1FFu) << 8;
    dmemSize = ((hwcfg >> 9) & 0x1FFu) << 8;
    TraceDetail(L"B3aImemSize", imemSize);
    TraceDetail(L"B3aDmemSize", dmemSize);
    if (bootSize > imemSize || sizeof(desc) > dmemSize) { err = 0xE000004Cu; goto out; }
    imemBase = imemSize - bootSize;
    TraceDetail(L"B3aImemBase", imemBase);
    boot = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, bootSize, SWGPU_TAG);  /* zeroed */
    if (boot == NULL) { err = 0xE000004Du; goto out; }
    RtlCopyMemory(boot, bl + blDataOff + codeOff, codeSize);
    Wr(B, PMU_FALCON + FALCON_IMEMC0, (1u << 24) | imemBase);
    for (i = 0; i < bootSize; i += 4) {
        if ((i & 0xFFu) == 0) Wr(B, PMU_FALCON + FALCON_IMEMT0, startTag + (i >> 8));
        Wr(B, PMU_FALCON + FALCON_IMEMD0, Le32(boot + i));
    }
    Wr(B, PMU_FALCON + FALCON_IMEMC0, (1u << 25) | imemBase);
    for (ok = 0, i = 0; i < bootSize; i += 4) {
        ULONG v = Rd(B, PMU_FALCON + FALCON_IMEMD0);
        if (i == 0) TraceDetail(L"B3aImemWord0", v);
        if (v == Le32(boot + i)) ++ok;
    }
    TraceDetail(L"B3aImemWords", bootSize / 4);
    TraceDurable(L"B3aImemOk", ok);

    Step(45);   /* flcn_bl_dmem_desc v0 at DMEM 0 (DMA bases left 0: no PMU VM yet) */
    RtlZeroMemory(desc, sizeof(desc));
    desc[8] = ACR_DMAIDX_VIRT;                  /* ctx_dma */
    desc[9] = 0;                                /* code_dma_base: GPU VA >> 8, not mapped yet */
    desc[10] = nsOff;                           /* non_sec_code_off */
    desc[11] = nsSize;                          /* non_sec_code_size */
    desc[12] = (app0Off + 0xFFu) & ~0xFFu;      /* sec_code_off = ALIGN(apps[0], 0x100) */
    desc[13] = app0Size;                        /* sec_code_size */
    desc[14] = 0;                               /* code_entry_point */
    desc[15] = 0;                               /* data_dma_base: (VA + data_dma_base) >> 8 */
    desc[16] = dataSize;                        /* data_size */
    TraceBin(L"B3aBlDesc", desc, sizeof(desc));
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | 0);
    for (i = 0; i < 19; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, desc[i]);
    Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | 0);
    for (ok = 0, i = 0; i < 19; ++i) if (Rd(B, PMU_FALCON + FALCON_DMEMD0) == desc[i]) ++ok;
    TraceDurable(L"B3aDescOk", ok);
    TraceDetail(L"B3aCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    TraceDetail(L"B3aBootAddr", startTag << 8);

    {
        ULONGLONG *acrDesc;
        /* Initialize the memory path (priv ring, PFIFO reset/enable in
         * nvgpu order, FB flush). Without the priv ring
         * the L2 answered with PRI errors and falcon DMA never completed. The
         * whole GPU is reset after this test, so nothing is put back here. */
        PrivRingInit(B, 53);
        TraceDetail(L"B3cLtcNr", Rd(B, 0x12006C));
        TraceDetail(L"B3cTagBase", Rd(B, 0x17E278));
        {
            ULONG en = Rd(B, NV_PMC_ENABLE), w;
            TraceDetail(L"B3cFifoPmcWas", en);
            /* Claude 0.4.72: nvgpu gk20a_init_fifo_reset_enable_hw() resets
             * PFIFO together with CE2 (mc_enable_ce2, bit 21). CE2 shares
             * runlist 0 with GR on GM20B; leaving it in reset made PBDMA0
             * latch INTR_0 bit 24 = ENG_RESET (open-gpu-doc dev_pbdma.ref)
             * as soon as the channel was loaded, which froze the PBDMA. */
            Wr(B, NV_PMC_ENABLE, en & ~(0x100u | 0x200000u));
            (VOID)Rd(B, NV_PMC_ENABLE);
            Us(20);
            Wr(B, NV_PMC_ENABLE, en | 0x100u | 0x200000u);
            (VOID)Rd(B, NV_PMC_ENABLE);
            Us(20);
            TraceDetail(L"B3cFifoPmcNow", Rd(B, NV_PMC_ENABLE));
            Wr(B, 0x2100, 0xFFFFFFFFu);
            TraceDetail(L"B3cFifoIntr0Clr", Rd(B, 0x2100));
            Wr(B, 0x070000, 1u);
            for (w = 0; w < 100 && (Rd(B, 0x070000) & 3u); ++w) Us(5);
            Trace(L"B3cFbFlushPolls", w);
            TraceDurable(L"B3cFbFlushReg", Rd(B, 0x070000));
        }
        Step(47);   /* hardware WPR range (GM200 ACR setup uses selectors 2/3) */
        Wr(B, NV_PFB_MMU_WPR_INFO, 2);
        wprRawStart = Rd(B, NV_PFB_MMU_WPR_INFO);
        Wr(B, NV_PFB_MMU_WPR_INFO, 3);
        wprRawLimit = Rd(B, NV_PFB_MMU_WPR_INFO);
        wprStart = ((ULONGLONG)(wprRawStart & 0xFFFFFF00u)) << 8;
        wprEnd = (((ULONGLONG)(wprRawLimit & 0xFFFFFF00u)) << 8) + 0x20000ull;
        TraceDetail(L"AcrWprRawStart", wprRawStart);
        TraceDetail(L"AcrWprRawLimit", wprRawLimit);
        TraceDetail(L"AcrWprStartLo", (ULONG)wprStart);
        TraceDetail(L"AcrWprStartHi", (ULONG)(wprStart >> 32));
        TraceDetail(L"AcrWprEndLo", (ULONG)wprEnd);
        TraceDetail(L"AcrWprEndHi", (ULONG)(wprEnd >> 32));
        if (wprStart == 0 || wprEnd <= wprStart || wprEnd - wprStart != ACR_WPR_BYTES) {
            err = 0xE0000061u; goto out;
        }

        Step(48);   /* build WPR in CPU memory and verify it fits the protected region */
        Step(64);   /* PMU LS loader DMA bases include the physical WPR base */
        err = BuildWprB3b(dmemSize, wprStart, &wprImage, &wprAllocLen, &wprUsed, &pmuArgv);
        TraceDetail(L"AcrWprBuildAlloc", wprAllocLen);
        TraceDetail(L"AcrWprBuildUsed", wprUsed);
        if (err != 0) goto out;
        if (wprUsed > ACR_WPR_BYTES || wprAllocLen < wprUsed) {
            err = 0xE0000062u; goto out;
        }
        if (dataSize < 0x250u || !InBlob(ucDataSize, dataBase, dataSize) ||
            !InBlob(ucDataSize, dataBase, 0x250u)) {
            err = 0xE0000063u; goto out;
        }

        Step(49);   /* allocate CPU-writable WPR backing and prepare PMU VM */
        if (!AcrVmPrepare(Dev, wprUsed, &arenaIova, &instIova)) {
            err = 0xE0000064u; goto out;
        }
        RtlCopyMemory(g_AcrVm.WprBuffer, wprImage, wprUsed);
        KeMemoryBarrier();
        ok = (ULONG)(RtlCompareMemory(g_AcrVm.WprBuffer, wprImage, wprUsed) == wprUsed);
        TraceDurable(L"AcrWprCpuReadback", ok);
        if (!ok) { err = 0xE0000069u; goto out; }
        TraceDetail(L"AcrWprCpuCopyBytes", wprUsed);

        /* GM20B's setup differs from GM200: point the descriptor at the
         * allocated INST-target WPR blob instead of programming region_props. */
        acrDesc = (ULONGLONG *)(img + dataBase);
        WprPutLe32((PUCHAR)acrDesc + 0x240, wprUsed); /* ucode_blob_size */
        WprPutLe64((PUCHAR)acrDesc + 0x248, (ULONGLONG)g_AcrVm.WprPa.QuadPart);
        TraceDetail(L"AcrDescWprBytes", wprUsed);
        TraceDetail(L"AcrDescWprBaseLo", g_AcrVm.WprPa.LowPart);
        TraceDetail(L"AcrDescWprBaseHi", (ULONG)g_AcrVm.WprPa.HighPart);
        RtlCopyMemory((PUCHAR)g_AcrVm.Arena + ACR_VM_IMAGE_OFF, img, ucDataSize);
        KeMemoryBarrier();
        TraceDetail(L"AcrImageIova", (ULONG)(arenaIova + ACR_VM_IMAGE_OFF));
        TraceDetail(L"AcrImageIovaHi", (ULONG)((arenaIova + ACR_VM_IMAGE_OFF) >> 32));
        TraceDetail(L"AcrImageBytes", ucDataSize);

        /* GM20B fb_gm20b.c: gm20b_fb_set_mmu_page_size() selects the
         * per-instance PDB big-page bit. Until this bit is set, our RAMIN
         * bit 11=0 is not sufficient to select the 128K table geometry.
         * See COMPARE-B3c-ja.md and NOTICE-GM20B-MMU.txt in the kit. */
        Step(57);
        {
            ULONG mmuBefore = Rd(B, 0x100C80), mmuAfter;
            TraceDetail(L"AcrMmuPageCtrlBefore", mmuBefore);
            Wr(B, 0x100C80, mmuBefore | 0x00000800u);
            mmuAfter = Rd(B, 0x100C80);
            TraceDetail(L"AcrMmuPageCtrlAfter", mmuAfter);
            TraceDetail(L"AcrPreBootDmactl", Rd(B, PMU_FALCON + FALCON_DMACTL));
            TraceDetail(L"AcrPreBootItfen", Rd(B, PMU_FALCON + FALCON_ITFEN));
            TraceDetail(L"AcrPreBootTranscfg1", Rd(B, PMU_FALCON + 0xE04));
            TraceDurable(L"AcrPdbPageModeOk", (mmuAfter & 0x800u) ? 1u : 0u);
            if (!(mmuAfter & 0x800u)) { err = 0xE000006Cu; goto out; }
        }

        Step(50);   /* bind the PMU instance block to the GM20B virtual address space */
        if (!AcrVmBind(B, instIova)) { err = 0xE0000065u; goto out; }

        Step(51);   /* write the virtual DMA addresses into flcn_bl_dmem_desc */
        desc[9] = (ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF) >> 8;
        desc[15] = (ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF + dataBase) >> 8;
        TraceDetail(L"AcrDescCodeDmaBase", desc[9]);
        TraceDetail(L"AcrDescDataDmaBase", desc[15]);
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24) | 0);
        for (i = 0; i < 19; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, desc[i]);
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | 0);
        for (ok = 0, i = 0; i < 19; ++i) if (Rd(B, PMU_FALCON + FALCON_DMEMD0) == desc[i]) ++ok;
        TraceDurable(L"AcrDescOk", ok);
        if (ok != 19) { err = 0xE0000066u; goto out; }

        Step(54);   /* invalidate the selected PDB after table construction/bind */
        if (!AcrMmuInvalidatePdb(B, arenaIova + ACR_VM_PGD_OFF)) {
            err = 0xE000006Au; goto out;
        }

        Step(55);   /* discriminator: flush all PDBs after the selected-PDB flush */
        if (!AcrMmuInvalidateAllPdbs(B)) { err = 0xE000006Bu; goto out; }

        /* Nouveau gk104_fifo_intr() snapshots asserted fault units, writes
         * each unit bit to FAULT_ID (0x259C), then ACKs PFIFO INTR0. The data
         * registers can retain their payload after ACK; only a set ID bit is
         * a current record. Clear stale MMU-fault latches immediately before
         * the single ACR launch and verify that the validity bits went away. */
        Step(62);   /* Claude: 53 is already the B3c priv ring init step */
        {
            ULONG intrBefore = Rd(B, 0x2100), idBefore = Rd(B, 0x259C);
            ULONG intrAfter = intrBefore, idAfter = idBefore, faultCount = 0;
            ULONG faultUnits[32], recBefore[32][5], recAfter[32][5];
            ULONG u, j, clearPolls;
            BOOLEAN cleared = FALSE;
            TraceDetail(L"B3cPreAcrFifoIntr0", intrBefore);
            TraceDetail(L"B3cPreAcrFaultId", idBefore);
            for (u = 0; u < 32; ++u) {
                if (!(idBefore & (1u << u))) continue;
                faultUnits[faultCount] = u;
                recBefore[faultCount][0] = u;
                for (j = 1; j < 5; ++j)
                    recBefore[faultCount][j] = Rd(B, 0x2800 + u * 0x10 + (j - 1) * 4);
                ++faultCount;
            }
            TraceDetail(L"B3cPreAcrFaultRecCount", faultCount);
            if (faultCount)
                TraceErrorBin(L"B3cPreAcrFaultRecs", recBefore, faultCount * sizeof(recBefore[0]));
            for (u = 0; u < 32; ++u)
                if (idBefore & (1u << u)) Wr(B, 0x259C, 1u << u);
            if (intrBefore & 0x18000000u) Wr(B, 0x2100, intrBefore & 0x18000000u);
            for (clearPolls = 0; clearPolls < 100u; ++clearPolls) {
                idAfter = Rd(B, 0x259C);
                intrAfter = Rd(B, 0x2100);
                if (idAfter == 0 && !(intrAfter & 0x18000000u)) {
                    cleared = TRUE;
                    break;
                }
                Us(10);
            }
            TraceDetail(L"B3cPreAcrFaultIdAfterClear", idAfter);
            TraceDetail(L"B3cPreAcrFifoIntr0AfterClear", intrAfter);
            Trace(L"B3cPreAcrFaultClearPolls", clearPolls);
            if (faultCount) {
                for (u = 0; u < faultCount; ++u) {
                    recAfter[u][0] = faultUnits[u];
                    for (j = 1; j < 5; ++j)
                        recAfter[u][j] = Rd(B, 0x2800 + faultUnits[u] * 0x10 + (j - 1) * 4);
                }
                TraceErrorBin(L"B3cPreAcrFaultRecsAfterAck", recAfter, faultCount * sizeof(recAfter[0]));
                TraceDetail(L"B3cPreAcrFaultDataPersist", RtlCompareMemory(recBefore, recAfter,
                    faultCount * sizeof(recBefore[0])) == faultCount * sizeof(recBefore[0]) ? 1u : 0u);
            }
            TraceDurable(L"B3cPreAcrFaultClearResult", cleared ? 0u : 0xE0000075u);
            if (!cleared) { err = 0xE0000075u; goto out; }
        }

        Step(56);   /* one-shot secure ACR boot; bounded wait, no automatic retry */
        acrAttempted = TRUE;
        Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));   /* 0.4.6: fresh MC errors for the boot */
        Wr(B, PMU_FALCON + FALCON_MAILBOX0, 0xCAFEBEEFu);
        Wr(B, PMU_FALCON + FALCON_BOOTVEC, startTag << 8);
        Wr(B, PMU_FALCON + FALCON_CPUCTL, 2u);
        for (bootPolls = 0; bootPolls < 2000; ++bootPolls) {
            if (Rd(B, PMU_FALCON + FALCON_CPUCTL) & 0x10u) break;
            Ms(1);
        }
        Trace(L"AcrBootPolls", bootPolls);
        TraceDetail(L"AcrBootCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
        TraceDetail(L"AcrBootMailbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
        TraceDetail(L"AcrBootMailbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
        TraceDetail(L"AcrBootIrq", Rd(B, PMU_FALCON + 0x004));
        TraceDetail(L"AcrBootSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
        TraceDetail(L"AcrBootPc", Rd(B, PMU_FALCON + 0x148));
        /* 0.4.6 (read only, apart from selecting the trace slot like nouveau's
         * gm200_flcn_tracepc): where the falcon is, its DMA state, and any
         * MC/SMMU or GPU MMU fault raised during the boot. */
        {
            ULONG tidx = Rd(B, PMU_FALCON + 0x148), nr, sp, pcs[8], rec[6][5], n = 0, id, u;
            nr = (tidx >> 16) & 0xFFu;
            TraceDetail(L"B3cTraceIdx", tidx);
            TraceDetail(L"B3cTraceNr", nr);
            if (nr > 8) nr = 8;
            for (sp = 0; sp < nr; ++sp) {
                Wr(B, PMU_FALCON + 0x148, sp);
                pcs[sp] = Rd(B, PMU_FALCON + 0x14C);
            }
            if (nr) TraceBin(L"B3cTracePc", pcs, nr * sizeof(ULONG));
            TraceDetail(L"B3cDmaCmd", Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
            TraceDetail(L"B3cDmaBase", Rd(B, PMU_FALCON + FALCON_DMATRFBASE));
            TraceDetail(L"B3cDmaMoffs", Rd(B, PMU_FALCON + FALCON_DMATRFMOFFS));
            TraceDetail(L"B3cDmaFboffs", Rd(B, PMU_FALCON + FALCON_DMATRFFBOFFS));
            TraceDetail(L"B3cDmactl", Rd(B, PMU_FALCON + FALCON_DMACTL));
            TraceDetail(L"B3cMcInt", Rd(Dev->Mc, MC_INTSTATUS));
            TraceDetail(L"B3cMcErr", Rd(Dev->Mc, MC_ERR_STATUS));
            TraceDetail(L"B3cMcErrAdr", Rd(Dev->Mc, MC_ERR_ADR));
            TraceDetail(L"B3cFifoIntr0", Rd(B, 0x2100));
            id = Rd(B, 0x259C);
            TraceDetail(L"B3cFifoFaultId", id);
            for (u = 0; u < 32 && n < 6; ++u) {
                if (!(id & (1u << u))) continue;
                rec[n][0] = u;
                rec[n][1] = Rd(B, 0x2800 + u * 0x10);
                rec[n][2] = Rd(B, 0x2804 + u * 0x10);
                rec[n][3] = Rd(B, 0x2808 + u * 0x10);
                rec[n][4] = Rd(B, 0x280C + u * 0x10);
                ++n;
            }
            if (n) TraceErrorBin(L"B3cFaultRecs", rec, n * 5 * sizeof(ULONG));
            /* 0.4.17 (read only): what state ACR left the LS falcons in.
             * SCTL bits [13:12] report the falcon security mode. The GR
             * falcons (FECS 0x409000, GPCCS 0x41A000) are read only when
             * PMC_ENABLE already has GR (bit 12) on; nothing is enabled here. */
            {
                ULONG pmcEn = Rd(B, NV_PMC_ENABLE);
                TraceDetail(L"B3cLsPmcEnable", pmcEn);
                TraceDetail(L"B3cPostAcrPmuCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
                TraceDetail(L"B3cPostAcrPmuSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
                TraceDetail(L"B3cLsPmuMb0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
                TraceDetail(L"B3cLsPmuMb1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
                if (pmcEn & 0x1000u) {
                    TraceDetail(L"B3cLsFecsCpuctl", Rd(B, 0x409000 + FALCON_CPUCTL));
                    TraceDetail(L"B3cLsFecsSctl", Rd(B, 0x409000 + FALCON_SCTL));
                    TraceDetail(L"B3cLsFecsMb0", Rd(B, 0x409000 + FALCON_MAILBOX0));
                    TraceDetail(L"B3cLsGpccsCpuctl", Rd(B, 0x41A000 + FALCON_CPUCTL));
                    TraceDetail(L"B3cLsGpccsSctl", Rd(B, 0x41A000 + FALCON_SCTL));
                    TraceDetail(L"B3cLsGpccsMb0", Rd(B, 0x41A000 + FALCON_MAILBOX0));
                    TraceDetail(L"B3cLsGrRead", 1);
                } else {
                    TraceDetail(L"B3cLsGrRead", 0);
                }
            }
            TraceDurable(L"B3cDiagDone", 1);
        }
        if (bootPolls == 2000) err = 0xE0000067u;
        else if (Rd(B, PMU_FALCON + FALCON_MAILBOX0) != 0) err = 0xE0000068u;
        else {
            Wr(B, PMU_FALCON + 0x004, 0x10u);
            err = 0;
        }
        TraceDurable(L"AcrBootResult", err);
        if (err == 0) {
            err = B3cStartPmuLs(Dev, pmuArgv);
            TraceDurable(L"B3cLsResult", err);
            if (err == 0) {
                fecsErr = B3cStartFecsLs(Dev);
                if (fecsErr != 0) err = fecsErr;
                else {
                    c1Err = B3cQueryFecsImageSizes(Dev, &c1GoldenBytes,
                                                   &c1ZcullBytes, &c1PmBytes);
                    if (c1Err != 0) err = c1Err;
                    else {
                        c2aErr = B3cValidateGrInitLists(Dev);
                        if (c2aErr != 0) err = c2aErr;
                        else {
                            c2cErr = B3cApplyGrCtxList(Dev, FALSE);
                            if (c2cErr != 0) err = c2cErr;
                            else {
                                c2dErr = B3cPrepareGrContext(Dev, c1GoldenBytes,
                                                             c1ZcullBytes, c1PmBytes);
                                if (c2dErr != 0) err = c2dErr;
                                else {
                                    c2eErr = B3cCreateGoldenContext(Dev);
                                    if (c2eErr != 0) err = c2eErr;
                                    else {
                                        d1Err = B3cInitFifoD1(Dev);
                                        if (d1Err != 0) err = d1Err;
                                        else {
                                            d2Err = B3cInitFifoD2(Dev);
                                            if (d2Err != 0) err = d2Err;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

out:
    if (!acrAttempted) TraceDurable(L"AcrBootResult", err);
    TraceDurable(L"B3aResult", err);
    if (boot != NULL) ExFreePoolWithTag(boot, SWGPU_TAG);
    if (wprImage != NULL) ExFreePoolWithTag(wprImage, SWGPU_TAG);
    if (img != NULL) ExFreePoolWithTag(img, SWGPU_TAG);
    if (bl != NULL) ExFreePoolWithTag(bl, SWGPU_TAG);
    if (uc != NULL) ExFreePoolWithTag(uc, SWGPU_TAG);
    return err;
}

static ULONG B3eResidentIrqTest(PSWGPU_DEVICE Dev, PSWGPU_E3_RESULT Out)
{
    volatile UCHAR *B = Dev != NULL ? Dev->Bar0 : NULL;
    volatile ULONG *bar1GpPut, *bar1GpGet, *semSlot;
    PULONG ring, push, pixels;
    ULONG (*writes)[4] = NULL;
    ULONG writeCapacity = 512u, writeCount = 0u;
    ULONG rounds = 64u, ringEntries = PAGE_SIZE / 8u;
    ULONG round = 0u, cursor, colorIndex, color, semaphore, put, newPut, get;
    ULONG l2State[4] = { 0u }, gotSemaphore = 0u, pixelFirst = 0u, pixelMiddle = 0u, pixelLast = 0u;
    ULONG result = 0u, failureStage = 0u, completed = 0u, timeoutCount = 0u, waitCount = 0u;
    ULONG timeoutWorkDone = 0u;
    ULONG i, e3Result;
    LARGE_INTEGER totalStart, totalEnd, waitStart, waitEnd, frequency, waitLimit;
    ULONGLONG totalTicks, waitTicks, totalUs, waitUs, maxWaitUs = 0u;

    if (Out == NULL) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    Out->HaveInterrupts = (Dev != NULL && Dev->HaveInterruptResources &&
                           Dev->InterruptSourcesEnabled) ? 1u : 0u;
    if (Dev == NULL || !Dev->ResidentActive || g_ResidentOwner != Dev ||
        !Dev->ResidentChannelEnabled || !Dev->ResidentRunlistSubmitted ||
        !Dev->ResidentInstBound || B == NULL || Dev->Bar1Cpu == NULL ||
        Dev->Bar1MapLen < BAR1_D3_MAP_LEN || !g_GrVm.Active || g_GrVm.Arena == NULL) {
        result = STATUS_DEVICE_NOT_READY;
        goto done;
    }
    Step(114);
    if (!Dev->HaveInterruptResources || !Dev->InterruptSourcesEnabled) {
        /* No descriptors is the supported pre-R447H case. If descriptors
         * exist but WDF could not create both interrupt objects, report setup
         * failure separately so a broken R447H resource map is visible. */
        result = (Dev->InterruptResourceCount == 0u) ? 0xE00000A7u : 0xE00000A9u;
        goto done;
    }
    B3cPrivRingResetWriteLog();
    if ((Rd(B, NV_PMC_ENABLE) & (1u << 12)) == 0u || ringEntries == 0u ||
        GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_GPFIFO_OFF + PAGE_SIZE > GR_VM_ARENA_SIZE ||
        GR_VM_D2_RT_OFF + 64u * 64u * sizeof(ULONG) > GR_VM_ARENA_SIZE) {
        result = 0xE00000A8u; failureStage = 1u; goto done;
    }
    ring = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_GPFIFO_OFF);
    push = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_PUSHBUF_OFF);
    pixels = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_RT_OFF);
    semSlot = (volatile ULONG *)((PUCHAR)push + PAGE_SIZE - 64u);
    bar1GpPut = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x8Cu);
    bar1GpGet = (volatile ULONG *)(Dev->Bar1Cpu + BAR1_D3_LOW_VA + 0x88u);
    writes = (ULONG (*)[4])ExAllocatePool2(POOL_FLAG_NON_PAGED,
        writeCapacity * sizeof(writes[0]), SWGPU_TAG);
    if (writes == NULL) { result = 0xE00000A8u; failureStage = 2u; goto done; }
    RtlZeroMemory(writes, writeCapacity * sizeof(writes[0]));
    put = *bar1GpPut;
    if (put >= ringEntries) { result = 0xE00000A8u; failureStage = 3u; goto done; }
    InterlockedExchange(&Dev->E3NonstallCount, 0);
    InterlockedExchange(&Dev->E3StallCount, 0);
    InterlockedExchange(&Dev->E3StallUnits, 0);
    InterlockedExchange(&Dev->E3FirstMcIntr0, 0);
    InterlockedExchange(&Dev->E3FirstMcIntr1, 0);
    InterlockedExchange(&Dev->E3LastMcIntr0, 0);
    InterlockedExchange(&Dev->E3LastMcIntr1, 0);
    InterlockedExchange(&Dev->E3FirstSet, 0);
    totalStart = KeQueryPerformanceCounter(&frequency);
    waitLimit.QuadPart = -1000000LL; /* 100 ms, relative, one bounded wait per round. */

    for (round = 0u; round < rounds; ++round) {
        cursor = 0u;
        colorIndex = round & 7u;
        color = 0xFF000000u |
            ((colorIndex & 1u) ? 0x00FF0000u : 0u) |
            ((colorIndex & 2u) ? 0x0000FF00u : 0u) |
            ((colorIndex & 4u) ? 0x000000FFu : 0u);
        semaphore = 0xE3000000u | (round + 1u);
        for (i = 0u; i < 4096u; ++i) pixels[i] = 0x11111111u;
        for (i = 0u; i < 4u; ++i) semSlot[i] = 0u;
#define E3_METHOD(method, value) do { \
        if (cursor + 2u >= PAGE_SIZE / sizeof(ULONG) - 16u) { failureStage = 4u; goto e3_round_fail; } \
        push[cursor++] = 0x20010000u | ((ULONG)(method) >> 2); \
        push[cursor++] = (ULONG)(value); \
    } while (0)
        /* D5 known-good MAXWELL_B 3D clear methods (clb197.h). */
        E3_METHOD(0x0000u, 0x0000B197u);
        E3_METHOD(0x121Cu, 1u); E3_METHOD(0x0800u, 0u);
        E3_METHOD(0x0804u, (ULONG)((ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_RT_OFF));
        E3_METHOD(0x0808u, 256u); E3_METHOD(0x080Cu, 64u);
        E3_METHOD(0x0810u, 0xCFu); E3_METHOD(0x0814u, 0x1000u);
        E3_METHOD(0x0818u, 1u); E3_METHOD(0x081Cu, 0u); E3_METHOD(0x0820u, 0u);
        E3_METHOD(0x0FE0u, 0u); E3_METHOD(0x0FE4u, 0u);
        E3_METHOD(0x1538u, 0u); E3_METHOD(0x0FF4u, 0x00400000u);
        E3_METHOD(0x0FF8u, 0x00400000u); E3_METHOD(0x0DF8u, 0u);
        E3_METHOD(0x0DFCu, 0u); E3_METHOD(0x0E00u, 0u);
        E3_METHOD(0x10F8u, 0x10u); E3_METHOD(0x0D6Cu, 0x00400000u);
        E3_METHOD(0x0D70u, 0x00400000u);
        E3_METHOD(0x0D80u, (colorIndex & 1u) ? 0x3F800000u : 0u);
        E3_METHOD(0x0D84u, (colorIndex & 2u) ? 0x3F800000u : 0u);
        E3_METHOD(0x0D88u, (colorIndex & 4u) ? 0x3F800000u : 0u);
        E3_METHOD(0x0D8Cu, 0x3F800000u);
        E3_METHOD(0x19D0u, 0x3Cu);
#undef E3_METHOD
        if (cursor + 8u >= PAGE_SIZE / sizeof(ULONG) - 16u) {
            failureStage = 4u; goto e3_round_fail;
        }
        /* Host SEMAPHORE RELEASE with WFI; then NVB06F NON_STALL_INT
         * (host method 0x20, open-gpu-doc dev_pbdma.ref). */
        push[cursor++] = 0x20040004u;
        push[cursor++] = 0u;
        push[cursor++] = (ULONG)((ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF + PAGE_SIZE - 64u);
        push[cursor++] = semaphore;
        push[cursor++] = 0x01000002u;
        /* Claude 0.7.3: no SET_OBJECT B06F on subchannel 1 (0.7.2 hardware: GR_INTR exception
         * 0x200000 as a stall interrupt). NON_STALL_INT is a host method; subchannel 0. */
        push[cursor++] = 0x20010008u; push[cursor++] = 0u; /* NON_STALL_INT, method 0x20 */
        KeMemoryBarrier();
        ring[put * 2u] = (ULONG)((ULONGLONG)GR_VM_GPU_VA + GR_VM_D2_PUSHBUF_OFF) & ~3u;
        ring[put * 2u + 1u] = cursor << 10;
        KeMemoryBarrier();
        writeCount = 0u; l2State[0] = l2State[1] = l2State[2] = l2State[3] = 0u;
        if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) {
            failureStage = 5u; goto e3_round_fail;
        }
        newPut = (put + 1u) % ringEntries;
        KeClearEvent(&Dev->E3Event);
        KeMemoryBarrier();
        *bar1GpPut = newPut;
        KeMemoryBarrier();
        if (*bar1GpPut != newPut || Rd(B, 0x00002A30u) != 0u ||
            (Rd(B, 0x00002100u) & 0x10000000u) != 0u) {
            failureStage = 6u; goto e3_round_fail;
        }
        ++waitCount;
        waitStart = KeQueryPerformanceCounter(NULL);
        e3Result = (ULONG)KeWaitForSingleObject(&Dev->E3Event, Executive,
                                                 KernelMode, FALSE, &waitLimit);
        waitEnd = KeQueryPerformanceCounter(NULL);
        waitTicks = (ULONGLONG)(waitEnd.QuadPart - waitStart.QuadPart);
        waitUs = frequency.QuadPart > 0 ? (waitTicks * 1000000ull) / (ULONGLONG)frequency.QuadPart : 0u;
        if (waitUs > maxWaitUs) maxWaitUs = waitUs;
        if (e3Result != STATUS_SUCCESS) {
            ++timeoutCount;
            /* Timeout diagnosis intentionally reads the semaphore only once. */
            writeCount = 0u;
            if (B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) {
                KeMemoryBarrier(); gotSemaphore = semSlot[0];
            } else gotSemaphore = semSlot[0];
            timeoutWorkDone = (gotSemaphore == semaphore) ? 1u : 0u;
            failureStage = 7u;
            goto e3_round_fail;
        }
        writeCount = 0u;
        if (!B3cD5L2Flush(B, writes, &writeCount, writeCapacity, l2State)) {
            failureStage = 8u; goto e3_round_fail;
        }
        KeMemoryBarrier();
        gotSemaphore = semSlot[0];
        pixelFirst = pixels[0]; pixelMiddle = pixels[2048]; pixelLast = pixels[4095];
        get = *bar1GpGet;
        if (get != newPut || gotSemaphore != semaphore || pixelFirst != color ||
            pixelMiddle != color || pixelLast != color) {
            failureStage = 9u; goto e3_round_fail;
        }
        ++completed;
        put = newPut;
        continue;

e3_round_fail:
        result = 0xE00000A8u;
        Out->Timeouts = timeoutCount;
        Trace(L"E3FailRound", round + 1u);
        break;
    }
    totalEnd = KeQueryPerformanceCounter(NULL);
    totalTicks = (ULONGLONG)(totalEnd.QuadPart - totalStart.QuadPart);
    totalUs = frequency.QuadPart > 0 ? (totalTicks * 1000000ull) / (ULONGLONG)frequency.QuadPart : 0u;
    Out->TotalUs = (ULONG)(totalUs > 0xFFFFFFFFull ? 0xFFFFFFFFu : totalUs);
    Out->MaxWaitUs = (ULONG)(maxWaitUs > 0xFFFFFFFFull ? 0xFFFFFFFFu : maxWaitUs);
    Out->Rounds = completed; Out->Waits = waitCount; Out->Timeouts = timeoutCount;
    Out->GpPut = *bar1GpPut; Out->GpGet = *bar1GpGet;
    Out->Semaphore = gotSemaphore;
    Out->PixelFirst = pixelFirst; Out->PixelMiddle = pixelMiddle; Out->PixelLast = pixelLast;
    if (round == rounds) result = 0u;

done:
    Out->Result = result;
    Out->HaveInterrupts = (Dev != NULL && Dev->HaveInterruptResources && Dev->InterruptSourcesEnabled) ? 1u : 0u;
    if (Dev != NULL) {
        Out->NonstallCount = (ULONG)InterlockedCompareExchange(&Dev->E3NonstallCount, 0, 0);
        Out->StallCount = (ULONG)InterlockedCompareExchange(&Dev->E3StallCount, 0, 0);
        Out->StallUnits = (ULONG)InterlockedCompareExchange(&Dev->E3StallUnits, 0, 0);
        Out->FirstMcIntr0 = (ULONG)InterlockedCompareExchange(&Dev->E3FirstMcIntr0, 0, 0);
        Out->FirstMcIntr1 = (ULONG)InterlockedCompareExchange(&Dev->E3FirstMcIntr1, 0, 0);
        Out->LastMcIntr0 = (ULONG)InterlockedCompareExchange(&Dev->E3LastMcIntr0, 0, 0);
        Out->LastMcIntr1 = (ULONG)InterlockedCompareExchange(&Dev->E3LastMcIntr1, 0, 0);
        if (B != NULL && Dev->ResidentActive) {
            Out->Channel = Rd(B, 0x00800004u); Out->PbdmaStatus = Rd(B, 0x00003080u);
            Out->PbdmaIntr0 = Rd(B, 0x00040108u); Out->FifoIntr0 = Rd(B, FIFO_INTR_0);
            Out->MmuFault = Rd(B, 0x00002A30u);
            if (Rd(B, NV_PMC_ENABLE) & (1u << 12)) {
                Out->GrIntr = Rd(B, 0x00400100u); Out->GrStatus = Rd(B, 0x00400700u);
                /* Claude 0.7.3: read-only GR error detail (exception, class error, trapped addr/data). */
                Trace(L"E3GrException", Rd(B, 0x00400108u));
                Trace(L"E3GrClassError", Rd(B, 0x00400110u));
                Trace(L"E3GrTrappedAddr", Rd(B, 0x00400704u));
                Trace(L"E3GrTrappedDataLo", Rd(B, 0x00400708u));
            }
            Trace(L"E3PmcIntr0AtEnd", Rd(B, PMC_INTR_STATUS_STALL));
            Trace(L"E3PmcIntr1AtEnd", Rd(B, PMC_INTR_STATUS_NONSTALL));
            Trace(L"E3FifoIntrEn1AtEnd", Rd(B, FIFO_INTR_EN_1));
        }
    }
    TraceDurable(L"E3Result", result); Trace(L"E3Rounds", Out->Rounds);
    Trace(L"E3Waits", Out->Waits);
    Trace(L"E3NonstallCount", Out->NonstallCount); Trace(L"E3StallCount", Out->StallCount);
    Trace(L"E3StallUnits", Out->StallUnits); Trace(L"E3MaxWaitUs", Out->MaxWaitUs);
    Trace(L"E3TotalUs", Out->TotalUs); Trace(L"E3Timeouts", Out->Timeouts);
    Trace(L"E3HaveInterrupts", Out->HaveInterrupts);
    Trace(L"E3FirstMcIntr0", Out->FirstMcIntr0); Trace(L"E3FirstMcIntr1", Out->FirstMcIntr1);
    Trace(L"E3LastMcIntr0", Out->LastMcIntr0); Trace(L"E3LastMcIntr1", Out->LastMcIntr1);
    Trace(L"E3GpGet", Out->GpGet); Trace(L"E3GpPut", Out->GpPut);
    Trace(L"E3Semaphore", Out->Semaphore); Trace(L"E3PixelFirst", Out->PixelFirst);
    Trace(L"E3PixelMiddle", Out->PixelMiddle); Trace(L"E3PixelLast", Out->PixelLast);
    Trace(L"E3FailureStage", failureStage); Trace(L"E3Channel", Out->Channel);
    Trace(L"E3TimeoutWorkDone", timeoutWorkDone);
    Trace(L"E3PbdmaStatus", Out->PbdmaStatus); Trace(L"E3PbdmaIntr0", Out->PbdmaIntr0);
    Trace(L"E3FifoIntr0", Out->FifoIntr0); Trace(L"E3GrIntr", Out->GrIntr);
    Trace(L"E3GrStatus", Out->GrStatus); Trace(L"E3MmuFault", Out->MmuFault);
    if (result != 0u && B != NULL && Dev != NULL && Dev->ResidentActive &&
        (Rd(B, NV_PMC_ENABLE) & (1u << 12)) != 0u) {
        ULONG dump[B3C_GR_STATUS_DUMP_REGS * 2u] = { 0u };
        B3cCaptureGrStatusDump(B, L"E3GrStatusDump", dump);
    }
    if (writes != NULL) TraceBin(L"E3PrivWrites", writes, writeCount * sizeof(writes[0]));
    B3cPrivRingTraceWriteLog(L"E3PrivErrorCount", L"E3PrivErrorDropped",
        L"E3PrivAckTimeouts", L"E3PrivErrors");
    if (writes != NULL) ExFreePoolWithTag(writes, SWGPU_TAG);
    return result;
}

/* F1: Maxwell CE2/GR copy on the existing GR runlist. Nouveau's GM20B
 * device table selects gm200_ce for CE2. On the Switch, deko3d binds Copy
 * and 3D on one GPU channel. Use a spare subchannel, with no RAMFC changes. */
#define F1_CE_ENABLE         (1u << 21)
#define F1_CE_INTR_ENABLE    0x00106904u
#define F1_CE_INTR_STATUS    0x00106908u
#define F1_CE_LAUNCH_ERROR   0x00106F14u
#define F1_COPY_SUBCHANNEL   4u
#define F1_COPY_ERROR        0xE00000AAu

static ULONG F1Method(ULONG Subchannel, ULONG MethodBytes, ULONG Count)
{
    return 0x20000000u | (Count << 16) | (Subchannel << 13) | (MethodBytes >> 2);
}

static UCHAR F1Pattern(ULONG Round, ULONG Offset)
{
    return (UCHAR)((Offset * 73u) ^ (Offset >> 8) ^ (Round * 29u + 1u));
}

/* BAR1 GP_PUT is a USERD doorbell. Log the BAR1-relative offset separately
 * from the BAR0 L2 writes, and check the priv ring just as for BAR0. */
static BOOLEAN F1Publish(PSWGPU_DEVICE Dev, ULONG NewPut, ULONG (*write)[4])
{
    const ULONG putOffset = BAR1_D3_LOW_VA + 0x8Cu;
    BOOLEAN ok;
    (*write)[0] = putOffset;
    (*write)[1] = Rd(Dev->Bar1Cpu, putOffset);
    (*write)[2] = NewPut;
    Dev->CopyInFlight = TRUE;
    KeMemoryBarrier();
    Wr(Dev->Bar1Cpu, putOffset, NewPut);
    KeMemoryBarrier();
    (*write)[3] = Rd(Dev->Bar1Cpu, putOffset);
    ok = B3cPrivRingAfterGrWrite(Dev->Bar0, putOffset, NewPut);
    return ok && (*write)[3] == NewPut;
}

static VOID F1CaptureFailure(volatile UCHAR *B, PSWGPU_F1_RESULT Out)
{
    ULONG records[8][5], count = 0u, unit, word;
    ULONG enable = Rd(B, NV_PMC_ENABLE);
    if ((enable & F1_CE_ENABLE) != 0u) {
        Out->CeIntr = Rd(B, F1_CE_INTR_STATUS);
        Out->CeLaunchError = Rd(B, F1_CE_LAUNCH_ERROR);
        Trace(L"F1CeIntrEnable", Rd(B, F1_CE_INTR_ENABLE));
    }
    Out->FifoIntr = Rd(B, FIFO_INTR_0);
    Out->FaultId = Rd(B, 0x0000259Cu);
    Out->PbdmaIntr = Rd(B, 0x00040108u);
    if ((enable & PMC_INTR_PGRAPH_PENDING) != 0u) {
        Out->GrIntr = Rd(B, 0x00400100u);
        Trace(L"F1GrException", Rd(B, 0x00400108u));
        Trace(L"F1GrClassError", Rd(B, 0x00400110u));
        Trace(L"F1GrTrappedAddr", Rd(B, 0x00400704u));
        Trace(L"F1GrTrappedData", Rd(B, 0x00400708u));
    }
    for (unit = 0u; unit < 32u && count < RTL_NUMBER_OF(records); ++unit) {
        if ((Out->FaultId & (1u << unit)) == 0u) continue;
        records[count][0] = unit;
        for (word = 0u; word < 4u; ++word)
            records[count][1u + word] = Rd(B, 0x00002800u + unit * 16u + word * 4u);
        ++count;
    }
    if (count != 0u) TraceErrorBin(L"F1FaultRecords", records, count * sizeof(records[0]));
    Trace(L"F1PmcEnable", enable);
    Trace(L"F1PmcIntr0", Rd(B, PMC_INTR_STATUS_STALL));
    Trace(L"F1PmcIntr1", Rd(B, PMC_INTR_STATUS_NONSTALL));
    Trace(L"F1MmuFaultStatus", Rd(B, 0x00002A30u));
    Trace(L"F1PbdmaIntr1", Rd(B, 0x00040148u));
    Trace(L"F1FifoEngine0", Rd(B, 0x00002640u));
    Trace(L"F1FifoEngine1", Rd(B, 0x00002644u));
}

static VOID F1CopyTest(PSWGPU_DEVICE Dev, PSWGPU_F1_RESULT Out)
{
    static const ULONG sizes[16] = {
        0x1000u, 0x1040u, 0x2000u, 0x3000u, 0x4000u, 0x5040u, 0x6000u, 0x7000u,
        0x8000u, 0x9000u, 0xA000u, 0xB000u, 0xC000u, 0xD040u, 0xE000u, 0x10000u
    };
    const ULONG pushVa = GR_VM_GPU_VA + GR_VM_F1_PUSH_OFF;
    const ULONG ceSemVa = pushVa + PAGE_SIZE - 32u, hostSemVa = ceSemVa + 16u;
    const ULONG ringEntries = PAGE_SIZE / 8u;
    const ULONG putOffset = BAR1_D3_LOW_VA + 0x8Cu, getOffset = BAR1_D3_LOW_VA + 0x88u;
    volatile UCHAR *B = Dev->Bar0;
    volatile ULONG *semaphores;
    PULONG ring, push;
    PUCHAR src, dst;
    ULONG writes[2][4] = { { 0u } }, doorbellWrite[4] = { 0u };
    ULONG l2State[4] = { 0u }, writeCount = 0u, stage = 1u, get = 0u, put = 0u;
    ULONG round, i, polls, ceSeen = 0u, hostSeen = 0u;
    ULONG flushMax = 0u, invalidateMax = 0u;
    LARGE_INTEGER frequency, waitLimit;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    Out->Result = F1_COPY_ERROR;
    Out->MismatchOffset = MAXULONG;
    B3cPrivRingResetWriteLog();
    Step(115);
    if (B == NULL || Dev->CopyInFlight || Dev->Bar1Cpu == NULL ||
        Dev->Bar1MapLen < BAR1_D3_MAP_LEN || !Dev->ResidentChannelEnabled ||
        !Dev->ResidentRunlistSubmitted || !Dev->ResidentInstBound ||
        !g_AcrVm.PtbChanged || !g_GrVm.PtbChanged || !g_GrVm.Active || g_GrVm.Arena == NULL ||
        (Rd(B, NV_PMC_ENABLE) & F1_CE_ENABLE) == 0u)
        goto done;
    ring = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_GPFIFO_OFF);
    push = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_F1_PUSH_OFF);
    semaphores = (volatile ULONG *)((PUCHAR)push + PAGE_SIZE - 32u);
    src = (PUCHAR)g_GrVm.Arena + GR_VM_F1_SRC_OFF;
    dst = (PUCHAR)g_GrVm.Arena + GR_VM_F1_DST_OFF;
    put = Rd(Dev->Bar1Cpu, putOffset);
    get = Rd(Dev->Bar1Cpu, getOffset);
    stage = 2u;
    if (put >= ringEntries || get != put || Rd(B, 0x00002A30u) != 0u ||
        (Rd(B, FIFO_INTR_0) & 0x10000000u) != 0u)
        goto done;
    (VOID)KeQueryPerformanceCounter(&frequency);
    waitLimit.QuadPart = -1000000LL; /* 100 ms for nonstall event. */

    for (round = 0u; round < RTL_NUMBER_OF(sizes); ++round) {
        ULONG size = sizes[round];
        ULONG cePayload = 0xF1000000u | (round + 1u);
        ULONG hostPayload = 0xF1FF0000u | (round + 1u);
        ULONG cursor = 0u, newPut = (put + 1u) % ringEntries;
        LARGE_INTEGER started, now;
        ULONGLONG elapsedUs;
        BOOLEAN complete = FALSE, flushOk = TRUE, useIrq;

        Out->FailRound = round + 1u;
        Out->LastSize = size;
        useIrq = Dev->InterruptSourcesEnabled &&
            (Rd(B, PMC_INTR_EN_NONSTALL) & 1u) != 0u &&
            (Rd(B, FIFO_INTR_EN_1) & FIFO_INTR_0_CHANNEL_INTR) != 0u;
        for (i = 0u; i < GR_VM_F1_BYTES; ++i) src[i] = F1Pattern(round, i);
        RtlZeroMemory(dst, GR_VM_F1_BYTES);
        semaphores[0] = 0u;
        semaphores[4] = 0u;

        if (round == 0u) {
            /* Non-incrementing single-word SET_OBJECT, as on Switch in
             * deko3d's BindEngine(Copy). The payload is the class alone. */
            push[cursor++] = 0x60010000u | (F1_COPY_SUBCHANNEL << 13);
            push[cursor++] = 0xB0B5u; /* MAXWELL_DMA_COPY_A */
        }
        /* NVIDIA clb0b5: virtual input/output, 1D pitch copy. LINE_COUNT
         * is 1, and pitch fields are ignored with MULTI_LINE_ENABLE clear. */
        push[cursor++] = F1Method(F1_COPY_SUBCHANNEL, 0x0400u, 8u);
        push[cursor++] = 0u; /* OFFSET_IN_UPPER */
        push[cursor++] = GR_VM_GPU_VA + GR_VM_F1_SRC_OFF;
        push[cursor++] = 0u; /* OFFSET_OUT_UPPER */
        push[cursor++] = GR_VM_GPU_VA + GR_VM_F1_DST_OFF;
        push[cursor++] = size; /* PITCH_IN */
        push[cursor++] = size; /* PITCH_OUT */
        push[cursor++] = size; /* LINE_LENGTH_IN, bytes */
        push[cursor++] = 1u; /* LINE_COUNT */
        push[cursor++] = F1Method(F1_COPY_SUBCHANNEL, 0x0240u, 3u); /* CE SET_SEMAPHORE_A/B/PAYLOAD */
        push[cursor++] = 0u;
        push[cursor++] = ceSemVa;
        push[cursor++] = cePayload;
        push[cursor++] = F1Method(F1_COPY_SUBCHANNEL, 0x0300u, 1u); /* LAUNCH_DMA */
        push[cursor++] = 0x18Eu; /* nonpipelined, flush, 1-word CE release, pitch/pitch */

        /* Host semaphore acquire must observe CE completion before the
         * trailing host release and interrupt. NVB06F host methods are on
         * subchannel 0 and do not require a SET_OBJECT for B06F. */
        push[cursor++] = F1Method(0u, 0x0010u, 4u);
        push[cursor++] = 0u;
        push[cursor++] = ceSemVa;
        push[cursor++] = cePayload;
        push[cursor++] = 1u; /* ACQUIRE equality */
        push[cursor++] = F1Method(0u, 0x0010u, 4u);
        push[cursor++] = 0u;
        push[cursor++] = hostSemVa;
        push[cursor++] = hostPayload;
        push[cursor++] = 0x01100002u; /* 4-byte RELEASE, CE acquire already ordered it */
        if (useIrq) {
            push[cursor++] = 0x20010008u; /* NON_STALL_INTERRUPT */
            push[cursor++] = 0u;
            KeClearEvent(&Dev->E3Event);
        }
        ring[put * 2u] = pushVa;
        ring[put * 2u + 1u] = cursor << 10;
        KeMemoryBarrier();
        stage = 3u;
        writeCount = 0u;
        if (!B3cD5L2Flush(B, writes, &writeCount, RTL_NUMBER_OF(writes), l2State))
            goto done;
        if (l2State[0] > flushMax) flushMax = l2State[0];
        if (l2State[2] > invalidateMax) invalidateMax = l2State[2];
        stage = 4u;
        started = KeQueryPerformanceCounter(NULL);
        if (!F1Publish(Dev, newPut, &doorbellWrite)) goto done;
        if (useIrq) {
            if (KeWaitForSingleObject(&Dev->E3Event, Executive, KernelMode, FALSE,
                    &waitLimit) == STATUS_SUCCESS)
                ++Out->IrqWakes;
            else
                ++Out->IrqTimeouts;
        }
        /* Verification also serves as fallback if the wake was lost or
         * nonstall is disabled. Bound it by rounds and elapsed time. */
        for (polls = 0u; polls < 2000u; ++polls) {
            writeCount = 0u;
            flushOk = B3cD5L2Flush(B, writes, &writeCount, RTL_NUMBER_OF(writes), l2State);
            if (l2State[0] > flushMax) flushMax = l2State[0];
            if (l2State[2] > invalidateMax) invalidateMax = l2State[2];
            if (!flushOk) break;
            KeMemoryBarrier();
            ceSeen = semaphores[0];
            hostSeen = semaphores[4];
            get = Rd(Dev->Bar1Cpu, getOffset);
            complete = ceSeen == cePayload && hostSeen == hostPayload && get == newPut;
            now = KeQueryPerformanceCounter(NULL);
            if (complete || (Rd(B, FIFO_INTR_0) & 0x10000000u) != 0u ||
                now.QuadPart - started.QuadPart >= frequency.QuadPart / 10)
                break;
            Us(10u);
        }
        if (polls > Out->MaxPolls) Out->MaxPolls = polls;
        now = KeQueryPerformanceCounter(NULL);
        elapsedUs = (ULONGLONG)(now.QuadPart - started.QuadPart) * 1000000ull /
            (ULONGLONG)frequency.QuadPart;
        if (elapsedUs > Out->MaxWaitUs)
            Out->MaxWaitUs = elapsedUs > MAXULONG ? MAXULONG : (ULONG)elapsedUs;
        stage = flushOk ? 6u : 5u;
        if (!complete) goto done;
        Dev->CopyInFlight = FALSE;
        stage = 7u;
        if (Rd(B, 0x00002A30u) != 0u || (Rd(B, FIFO_INTR_0) & 0x10000000u) != 0u)
            goto done;
        for (i = 0u; i < GR_VM_F1_BYTES; ++i) {
            UCHAR expected = i < size ? F1Pattern(round, i) : 0u;
            UCHAR actual = dst[i];
            if (actual != expected) {
                Out->MismatchOffset = i;
                Out->Expected = expected;
                Out->Actual = actual;
                stage = 8u;
                goto done;
            }
        }
        ++Out->Rounds;
        Out->BytesCopied += size;
        put = newPut;
    }
    Out->Result = 0u;
    Out->FailRound = 0u;
    stage = 0u;

done:
    Out->Pending = Dev->CopyInFlight ? 1u : 0u;
    if (Out->Result != 0u && B != NULL) F1CaptureFailure(B, Out);
#define F1_TRACE(Field) Trace(L"F1" #Field, Out->Field)
    F1_TRACE(Rounds); F1_TRACE(BytesCopied); F1_TRACE(FailRound); F1_TRACE(LastSize);
    F1_TRACE(MismatchOffset); F1_TRACE(Expected); F1_TRACE(Actual); F1_TRACE(MaxWaitUs);
    F1_TRACE(MaxPolls); F1_TRACE(IrqWakes); F1_TRACE(IrqTimeouts); F1_TRACE(Pending);
    F1_TRACE(CeIntr); F1_TRACE(CeLaunchError); F1_TRACE(FifoIntr); F1_TRACE(FaultId);
    F1_TRACE(PbdmaIntr); F1_TRACE(GrIntr);
#undef F1_TRACE
    Trace(L"F1Stage", stage);
    Trace(L"F1LastGpGet", get);
    Trace(L"F1LastCeSemaphore", ceSeen);
    Trace(L"F1LastHostSemaphore", hostSeen);
    Trace(L"F1L2FlushMaxPolls", flushMax);
    Trace(L"F1L2InvalidateMaxPolls", invalidateMax);
    TraceBin(L"F1Bar1Doorbell", doorbellWrite, sizeof(doorbellWrite));
    TraceBin(L"F1L2Writes", writes, writeCount * sizeof(writes[0]));
    B3cPrivRingTraceWriteLog(L"F1PrivErrorCount", L"F1PrivErrorDropped",
        L"F1PrivAckTimeouts", L"F1PrivErrors");
    TraceDurable(L"F1Result", Out->Result);
}

#include "swgpu-blit.inc"

static ULONG B3ePowerDownGpu(PSWGPU_DEVICE Dev, PSWGPU_RESIDENT_POWER Power)
{
    ULONG i, error = 0u;
    NTSTATUS writeStatus;
    if (Power == NULL || !Power->Valid) return 0u;
    TraceDurable(L"B3ePowerDownBegin", 1u);
    if (Power->ClocksOn) {
        if (Dev->GpcManaged && !Dev->BlitReset && !F2cPark(Dev)) error |= 0x20u;
        Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) | MC_HOTRESET_GPU);
        for (i = 0; i < 500u && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(10);
        Trace(L"B3eMcFlushPolls", i);
        if (i == 500u) error |= 1u;
        Us(10);
        Wr(Dev->Pmc, PMC_GPU_RG_CNTRL, 1u);
        (VOID)Rd(Dev->Pmc, PMC_GPU_RG_CNTRL);
        Us(10);
        Wr(Dev->Car, CAR_RST_DEV_X_SET, CLK_X_GPU);
        Us(10);
        Wr(Dev->Car, CAR_CLK_ENB_X_CLR, (CLK_X_GPU | CLK_X_PLLG_REF) & ~Power->ClkX);
        Wr(Dev->Mc, MC_HOTRESET_CTRL_1,
            (Rd(Dev->Mc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_GPU) |
            (Power->McHotResetCtrl & MC_HOTRESET_GPU));
        if (Power->FuseChanged) Wr(Dev->Car, CAR_CLK_ENB_H_CLR, CLK_H_FUSE);
        if (Power->OutcChanged) Wr(Dev->Car, CAR_PLLP_OUTC, Power->PllOutc);
        Us(10);
        Power->ClocksOn = FALSE;
        Dev->GpcManaged = FALSE;
    }
    if (Power->RailOn) {
        writeStatus = I2cWrite(Dev->Pmic, MAX77620_GPIO6, Power->Gpio6);
        Trace(L"B3ePowerDownGpioStatus", (ULONG)writeStatus); if (!NT_SUCCESS(writeStatus)) error |= 2u;
        writeStatus = I2cWrite(Dev->Vdd, MAX77621_VOUT, Power->Vout);
        Trace(L"B3ePowerDownVoutStatus", (ULONG)writeStatus); if (!NT_SUCCESS(writeStatus)) error |= 4u;
        writeStatus = I2cWrite(Dev->Vdd, MAX77621_VOUT_DVS, Power->VoutDvs);
        Trace(L"B3ePowerDownDvsStatus", (ULONG)writeStatus); if (!NT_SUCCESS(writeStatus)) error |= 8u;
        Power->RailOn = FALSE;
    }
    if (!Dev->BlitUnsafe) {
        GrVmCleanupAfterGpuReset(Dev);
        AcrVmCleanupAfterGpuReset(Dev);
    } else {
        error |= 16u; /* Quarantined backing stays reserved. */
    }
    Dev->CopyInFlight = FALSE;
    Dev->BlitReset = Dev->BlitUnsafe;
    E4TracePool(&Dev->DmaPool);
    Power->Valid = FALSE;
    Trace(L"B3ePowerDownError", error);
    TraceDurable(L"B3ePowerDownDone", 1u);
    return error;
}

/* Source selection follows nvgpu gk20a_pm_finalize_poweron(): PFIFO/PGRAPH
 * on nonstall, and PGRAPH/PFIFO/PRIV_RING/LTC/PBUS on stall. Only PMC BAR0
 * registers are used; the Tegra MC block/routing is never touched here. */
static ULONG B3eEnableInterruptSources(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev != NULL ? Dev->Bar0 : NULL;
    ULONG writes[12][4] = { { 0u } }, writeCount = 0u;
    ULONG result = 0u;
    if (Dev == NULL || B == NULL || !Dev->HaveInterruptResources ||
        Dev->StallInterrupt == NULL || Dev->NonstallInterrupt == NULL)
        return 0u;
    InterlockedExchange(&Dev->E3NonstallCount, 0);
    InterlockedExchange(&Dev->E3StallCount, 0);
    InterlockedExchange(&Dev->E3StallUnits, 0);
    InterlockedExchange(&Dev->E3FirstMcIntr0, 0);
    InterlockedExchange(&Dev->E3FirstMcIntr1, 0);
    InterlockedExchange(&Dev->E3LastMcIntr0, 0);
    InterlockedExchange(&Dev->E3LastMcIntr1, 0);
    InterlockedExchange(&Dev->E3FirstSet, 0);
    KeClearEvent(&Dev->E3Event);
    B3cPrivRingResetWriteLog();
    /* Claude 0.7.3: PFIFO CHANNEL_INTR reaches PMC INTR_1 only with FIFO_INTR_EN_1 bit 31
     * (0.7.2 hardware: FIFO_INTR_0 0x80000000 latched while PMC INTR_1 stayed 0).
     * Clear the stale latch (W1C) first, then route it. */
    if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), FIFO_INTR_0, FIFO_INTR_0_CHANNEL_INTR) ||
        !B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), FIFO_INTR_EN_1, FIFO_INTR_0_CHANNEL_INTR)) {
        result = 0xE00000A9u;
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), FIFO_INTR_EN_1, 0u);
        goto done;
    }
    if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes),
                    PMC_INTR_MASK_STALL,
                    PMC_INTR_PFIFO_PENDING | PMC_INTR_PGRAPH_PENDING |
                    0x40000000u | 0x02000000u | 0x10000000u) ||
        !B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes),
                    PMC_INTR_MASK_NONSTALL,
                    PMC_INTR_PFIFO_PENDING | PMC_INTR_PGRAPH_PENDING)) {
        result = 0xE00000A9u;
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_STALL, 0u);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_NONSTALL, 0u);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_MASK_STALL, 0u);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_MASK_NONSTALL, 0u);
        goto done;
    }
    InterlockedExchange(&Dev->E3IsrActive, 1);
    if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_STALL, 1u) ||
        !B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_NONSTALL, 1u)) {
        InterlockedExchange(&Dev->E3IsrActive, 0);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_MASK_STALL, 0u);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_MASK_NONSTALL, 0u);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_STALL, 0u);
        (VOID)B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_NONSTALL, 0u);
        result = 0xE00000A9u;
        goto done;
    }
    Dev->InterruptSourcesEnabled = TRUE;
done:
    Trace(L"E3InterruptSourceResult", result);
    Trace(L"E3HaveInterrupts", Dev->HaveInterruptResources ? 1u : 0u);
    TraceBin(L"E3InterruptWrites", writes, writeCount * sizeof(writes[0]));
    B3cPrivRingTraceWriteLog(L"E3PrivErrorCount", L"E3PrivErrorDropped",
        L"E3PrivAckTimeouts", L"E3PrivErrors");
    return result;
}

static BOOLEAN B3eIrqDrainBarrier(WDFINTERRUPT Interrupt, WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(Context);
    return TRUE;
}

static ULONG B3eDisableInterruptSources(PSWGPU_DEVICE Dev, BOOLEAN DrainIsrs)
{
    volatile UCHAR *B = Dev != NULL ? Dev->Bar0 : NULL;
    ULONG writes[8][4] = { { 0u } }, writeCount = 0u;
    ULONG result = 0u;
    if (Dev == NULL) return STATUS_INVALID_PARAMETER;
    if (B != NULL) {
        B3cPrivRingResetWriteLog();
        if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_STALL, 0u)) result = 0xE00000A9u;
        if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_EN_NONSTALL, 0u)) result = 0xE00000A9u;
        if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_MASK_STALL, 0u)) result = 0xE00000A9u;
        if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), PMC_INTR_MASK_NONSTALL, 0u)) result = 0xE00000A9u;
        if (!B3cD5Write(B, writes, &writeCount, RTL_NUMBER_OF(writes), FIFO_INTR_EN_1, 0u)) result = 0xE00000A9u;
    }
    InterlockedExchange(&Dev->E3IsrActive, 0);
    /* During a live IOCTL, drain any ISR that passed the gate before reset.
     * KMDF manages the vectors across D0 transitions. This keeps them usable
     * for another Resident start in the same hardware lifetime. */
    if (DrainIsrs) {
        if (Dev->StallInterrupt != NULL)
            (VOID)WdfInterruptSynchronize(Dev->StallInterrupt, B3eIrqDrainBarrier, NULL);
        if (Dev->NonstallInterrupt != NULL)
            (VOID)WdfInterruptSynchronize(Dev->NonstallInterrupt, B3eIrqDrainBarrier, NULL);
    }
    Dev->InterruptSourcesEnabled = FALSE;
    Trace(L"E3InterruptDisableResult", result);
    TraceBin(L"E3InterruptDisableWrites", writes, writeCount * sizeof(writes[0]));
    B3cPrivRingTraceWriteLog(L"E3ShutdownPrivErrorCount", L"E3ShutdownPrivErrorDropped",
        L"E3ShutdownPrivAckTimeouts", L"E3ShutdownPrivErrors");
    return result;
}

static NTSTATUS StartResident(PSWGPU_DEVICE Dev)
{
    UCHAR vout0 = 0, dvs0 = 0, gpio0 = 0, v = 0;
    ULONG clkx0, clkh0, outc0, rg, clkx, rstx, boot0 = 0xDEADDEADu, t0 = 0, t1 = 0, th = 0, en = 0, i;
    ULONG e1Result = 0u;
    BOOLEAN railOn = FALSE, clocksOn = FALSE, outcChanged = FALSE, fuseChanged = FALSE;
    NTSTATUS st;

    if (g_ResidentOwner != NULL || Dev->BlitUnsafe || g_GrVm.Arena != NULL) {
        TraceDurable(L"E1Result", STATUS_DEVICE_BUSY);
        return STATUS_DEVICE_BUSY;
    }
    Dev->ResidentActive = FALSE;
    Dev->ResidentShutdownResult = 0u;
    TraceDurable(L"B3aResult", 0xFFFFFFFFu);
    TraceDurable(L"B3eResidentActive", 0u);

    Step(1);    /* read what is there now */
    st = I2cRead(Dev->Vdd, MAX77621_VOUT, &vout0);
    if (NT_SUCCESS(st)) st = I2cRead(Dev->Vdd, MAX77621_VOUT_DVS, &dvs0);
    if (NT_SUCCESS(st)) st = I2cRead(Dev->Pmic, MAX77620_GPIO6, &gpio0);
    if (!NT_SUCCESS(st)) {
        Trace(L"Result", (ULONG)st);
        TraceDurable(L"E1Result", (ULONG)st);
        return st;
    }
    clkx0 = Rd(Dev->Car, CAR_CLK_OUT_ENB_X);
    clkh0 = Rd(Dev->Car, CAR_CLK_OUT_ENB_H);
    outc0 = Rd(Dev->Car, CAR_PLLP_OUTC);
    Dev->ResidentPower.Vout = vout0;
    Dev->ResidentPower.VoutDvs = dvs0;
    Dev->ResidentPower.Gpio6 = gpio0;
    Dev->ResidentPower.ClkX = clkx0;
    Dev->ResidentPower.ClkH = clkh0;
    Dev->ResidentPower.PllOutc = outc0;
    Dev->ResidentPower.McHotResetCtrl = Rd(Dev->Mc, MC_HOTRESET_CTRL_1);
    Dev->ResidentPower.Valid = TRUE;
    Dev->ResidentPower.RailOn = FALSE;
    Dev->ResidentPower.ClocksOn = FALSE;
    Dev->ResidentPower.OutcChanged = FALSE;
    Dev->ResidentPower.FuseChanged = FALSE;

    /* Clear stale GPU bring-up error status before recording new errors. */
    TraceDetail(L"McIntBeforePowerUp", Rd(Dev->Mc, MC_INTSTATUS));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    TraceDetail(L"McIntPowerUpCleared", Rd(Dev->Mc, MC_INTSTATUS));

    Step(2);    /* VDD_GPU 1.0 V, enable pin high */
    st = I2cWrite(Dev->Vdd, MAX77621_VOUT_DVS, VDD_GPU_1000MV);
    if (NT_SUCCESS(st)) st = I2cWrite(Dev->Vdd, MAX77621_VOUT, VDD_GPU_1000MV);
    if (NT_SUCCESS(st)) st = I2cWrite(Dev->Pmic, MAX77620_GPIO6, GPIO_PUSHPULL_HIGH);
    railOn = TRUE;      /* restore below even after a partial write */
    Dev->ResidentPower.RailOn = TRUE;
    if (!NT_SUCCESS(st)) goto down;
    Ms(2);
    if (NT_SUCCESS(I2cRead(Dev->Pmic, MAX77620_GPIO6, &v))) TraceDetail(L"Gpio6On", v);
    Dev->VoutOnRaw = MAXULONG;
    if (NT_SUCCESS(I2cRead(Dev->Vdd, MAX77621_VOUT, &v))) {
        Dev->VoutOnRaw = v;
        TraceDetail(L"VoutOn", v);
    }

    Step(3);    /* clocks */
    /* R410 test: the boot firmware leaves OUT5 enabled but undivided
     * (0x00030000 = 408 MHz); nvgpu runs the pwr clock at 204 MHz. */
    if ((outc0 & (PLLP_OUT5_RSTN | PLLP_OUT5_CLKEN | 0xFF000000u)) != (PLLP_OUT5_RSTN | PLLP_OUT5_CLKEN | PLLP_OUT5_204MHZ)) {
        Wr(Dev->Car, CAR_PLLP_OUTC, (outc0 & 0x0000FFFFu) | PLLP_OUT5_204MHZ | PLLP_OUT5_OVRRIDE |
                                    PLLP_OUT5_CLKEN | PLLP_OUT5_RSTN);
        outcChanged = TRUE;
        Dev->ResidentPower.OutcChanged = TRUE;
    }
    if (!(clkh0 & CLK_H_FUSE)) { Wr(Dev->Car, CAR_CLK_ENB_H_SET, CLK_H_FUSE); fuseChanged = TRUE; Dev->ResidentPower.FuseChanged = TRUE; }
    Wr(Dev->Car, CAR_CLK_ENB_X_SET, CLK_X_GPU | CLK_X_PLLG_REF);
    clocksOn = TRUE;
    Dev->ResidentPower.ClocksOn = TRUE;
    Us(10);
    TraceDetail(L"PllpOutcOn", Rd(Dev->Car, CAR_PLLP_OUTC));

    Step(4);    /* reset assert */
    Wr(Dev->Car, CAR_RST_DEV_X_SET, CLK_X_GPU);
    Us(10);

    Step(5);    /* clamp off */
    Wr(Dev->Pmc, PMC_GPU_RG_CNTRL, 0);
    (VOID)Rd(Dev->Pmc, PMC_GPU_RG_CNTRL);
    Us(10);
    TraceDetail(L"McIntAfterClampOff", Rd(Dev->Mc, MC_INTSTATUS));

    Step(6);    /* reset deassert with the gpu clock briefly off (nvgpu) */
    Wr(Dev->Car, CAR_CLK_ENB_X_CLR, CLK_X_GPU);
    Wr(Dev->Car, CAR_RST_DEV_X_CLR, CLK_X_GPU);
    Wr(Dev->Car, CAR_CLK_ENB_X_SET, CLK_X_GPU);
    Us(10);
    TraceDetail(L"McHotStatAfterReset", Rd(Dev->Mc, MC_HOTRESET_STAT_1));
    TraceDetail(L"McIntAfterReset", Rd(Dev->Mc, MC_INTSTATUS));

    Step(7);    /* MC flush, flush done */
    Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) | MC_HOTRESET_GPU);
    for (i = 0; i < 500 && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(10);
    Trace(L"McFlushPolls", i);
    Us(10);
    Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_GPU);
    Us(10);
    TraceDetail(L"McHotStatAfterFlush", Rd(Dev->Mc, MC_HOTRESET_STAT_1));

    rg = Rd(Dev->Pmc, PMC_GPU_RG_CNTRL);
    clkx = Rd(Dev->Car, CAR_CLK_OUT_ENB_X);
    rstx = Rd(Dev->Car, CAR_RST_DEVICES_X);
    TraceDetail(L"RgCntrlOn", rg);
    TraceDetail(L"ClkXOn", clkx);
    TraceDetail(L"RstXOn", rstx);
    if ((rg & 1) || !(clkx & CLK_X_GPU) || (rstx & CLK_X_GPU) || Dev->Bar0 == NULL) {
        Trace(L"Result", 0xE0000001u);      /* not safe to touch BAR0 */
        st = STATUS_DEVICE_NOT_READY;
        goto down;
    }

    Step(8);    /* first BAR0 read: NV_PMC_BOOT_0 */
    boot0 = Rd(Dev->Bar0, NV_PMC_BOOT_0);
    TraceDurable(L"Boot0", boot0);

    Step(9);    /* more of BAR0 */
    en = Rd(Dev->Bar0, NV_PMC_ENABLE);
    t0 = Rd(Dev->Bar0, NV_PTIMER_TIME_0);
    th = Rd(Dev->Bar0, NV_PTIMER_TIME_1);
    Ms(1);
    t1 = Rd(Dev->Bar0, NV_PTIMER_TIME_0);
    TraceDurable(L"PtimerRead", 1);
    if (((boot0 >> 20) & 0x1FF) == 0x12B) {   /* only write into a GPU that answered as GM20B */
        Wr(Dev->Bar0, NV_PFB_MMU_WPR_INFO, 2);
        TraceDetail(L"WprStart", Rd(Dev->Bar0, NV_PFB_MMU_WPR_INFO));
        Wr(Dev->Bar0, NV_PFB_MMU_WPR_INFO, 3);
        TraceDetail(L"WprLimit", Rd(Dev->Bar0, NV_PFB_MMU_WPR_INFO));
    }
    TraceDetail(L"PmcEnable", en);
    TraceDetail(L"Ptimer0", t0);
    TraceDetail(L"Ptimer1", t1);
    TraceDetail(L"PtimerHi", th);
    TraceDetail(L"McHotStatAfterBar0", Rd(Dev->Mc, MC_HOTRESET_STAT_1));
    TraceDetail(L"McIntAfterBar0", Rd(Dev->Mc, MC_INTSTATUS));
    TraceDetail(L"Chipset", (boot0 >> 20) & 0x1FF);
    TraceDetail(L"ChipRev", boot0 & 0xFF);
    st = (((boot0 >> 20) & 0x1FF) == 0x12B) ? STATUS_SUCCESS : STATUS_DEVICE_HARDWARE_ERROR;
    Trace(L"Result", (ULONG)st);
    if (NT_SUCCESS(st)) {
        e1Result = StartGpuFirmware(Dev);
        if (e1Result != 0u) st = STATUS_DEVICE_HARDWARE_ERROR;
        else if (!Dev->ResidentActive) {
            e1Result = 0xE00000A5u;
            st = STATUS_DEVICE_HARDWARE_ERROR;
        }
    }
    if (NT_SUCCESS(st) && Dev->ResidentActive) {
        ULONG gpcKhz = ReadParam(L"GpcKhz", 0u);
        F2C_STEP clock = {0};
        clock.TargetKhz = gpcKhz;
        if (gpcKhz != 0u) {
            clock.Result = F2cProgram(Dev, F2cReadSpeedo(), &clock);
            if (!NT_SUCCESS((NTSTATUS)clock.Result) && !F2cReturnToBypass(Dev)) {
                TraceDurable(L"F2cResidentClockResult", clock.Result);
                (VOID)B3eResidentShutdown(Dev, TRUE);
                TraceDurable(L"E1Result", STATUS_DEVICE_HARDWARE_ERROR);
                return STATUS_DEVICE_HARDWARE_ERROR;
            }
        }
        Trace(L"F2cResidentClockKhz", gpcKhz);
        Trace(L"F2cResidentClockCvbMv", clock.CvbMv);
        Trace(L"F2cResidentLockPolls", clock.LockPolls);
        Trace(L"F2cResidentRampPolls", clock.RampPolls);
        TraceDurable(L"F2cResidentClockResult", clock.Result);
        {
            /* Claude 0.7.0: enable the GPU interrupt sources only once the GPU is
             * resident. A failure leaves them off (E3 then reports it); the
             * resident GPU itself stays usable as in 0.6.0. */
            ULONG irqResult = B3eEnableInterruptSources(Dev);
            TraceDurable(L"E3IrqEnableResult", irqResult);
        }
        TraceDurable(L"E1Result", 0u);
        TraceDurable(L"B3eResidentActive", 1u);
        E4TracePool(&Dev->DmaPool);
        Step(110);
        return st;
    }
down:
    Step(10);   /* power down, put everything back */
    Dev->ResidentPower.ClocksOn = clocksOn;
    Dev->ResidentPower.RailOn = railOn;
    Dev->ResidentPower.OutcChanged = outcChanged;
    Dev->ResidentPower.FuseChanged = fuseChanged;
    {
        ULONG powerError = B3ePowerDownGpu(Dev, &Dev->ResidentPower);
        Trace(L"B3ePowerDownResult", powerError);
    }
    if (Dev->ResidentIdleRefHeld) {
        WdfDeviceResumeIdle(Dev->Device);
        Dev->ResidentIdleRefHeld = FALSE;
    }
    Step(11);
    TraceDurable(L"E1Result", e1Result != 0u ? e1Result : (ULONG)st);
    Dev->ResidentActive = FALSE;
    Dev->ResidentPower.Valid = FALSE;
    return st;
}

/* ------------------------------------------------------------------- PnP */

static ULONG B3eResidentShutdown(PSWGPU_DEVICE Dev, BOOLEAN DrainIsrs)
{
    ULONG writes[64][4] = { { 0u } }, diagWrites[64][4] = { { 0u } };
    ULONG writeCount = 0u, diagWriteCount = 0u, error = 0u;
    ULONG preemptPolls = 0u, preemptLast = 0u, runlistPolls = 0u, runlistLast = 0u;
    if (Dev == NULL || !Dev->ResidentActive) {
        if (Dev != NULL) Trace(L"B3eShutdownNoop", 1u);
        return Dev ? Dev->ResidentShutdownResult : STATUS_INVALID_PARAMETER;
    }
    if (g_ResidentOwner != NULL && g_ResidentOwner != Dev) {
        TraceDurable(L"B3eShutdownOwnerMismatch", 1u);
        return STATUS_DEVICE_BUSY;
    }
    TraceDurable(L"B3eShutdownBegin", 1u);
    Step(111);
    InterlockedExchange(&Dev->BlitOnline, 0);
    if (!Dev->BlitReset) {
        /* Stop GPU interrupt generation and drain ISR access before reset. */
        (VOID)B3eDisableInterruptSources(Dev, DrainIsrs);
        error = B3cD2CleanupPublishedState(Dev, writes, &writeCount,
            Dev->ResidentChannelEnabled, Dev->ResidentRunlistSubmitted,
            Dev->ResidentInstBound, Dev->ResidentChannelId,
            Dev->ResidentRunlistEmptyValue, &preemptPolls, &preemptLast,
            &runlistPolls, &runlistLast, TRUE, diagWrites, &diagWriteCount);
    } else {
        Dev->InterruptSourcesEnabled = FALSE;
        g_D2Bar1Bound = g_D2UserdBar1Set = g_D2PbdmaMasksSaved = 0u;
    }
    TraceBin(L"B3eShutdownWrites", writes, sizeof(writes));
    TraceBin(L"B3eShutdownDiagWrites", diagWrites, sizeof(diagWrites));
    B3cPrivRingTraceWriteLog(L"B3eShutdownPrivErrorCount",
        L"B3eShutdownPrivErrorDropped", L"B3eShutdownPrivAckTimeouts",
        L"B3eShutdownPrivErrors");
    Dev->ResidentChannelEnabled = FALSE;
    Dev->ResidentRunlistSubmitted = FALSE;
    Dev->ResidentInstBound = FALSE;
    Trace(L"B3eShutdownPreemptPolls", preemptPolls);
    Trace(L"B3eShutdownPreemptLast", preemptLast);
    Trace(L"B3eShutdownRunlistPolls", runlistPolls);
    Trace(L"B3eShutdownRunlistLast", runlistLast);
    {
        ULONG powerError = B3ePowerDownGpu(Dev, &Dev->ResidentPower);
        Trace(L"B3ePowerDownResult", powerError);
        error |= (powerError << 16);
    }
    if (Dev->ResidentIdleRefHeld) {
        WdfDeviceResumeIdle(Dev->Device);
        Dev->ResidentIdleRefHeld = FALSE;
    }
    Dev->ResidentActive = FALSE;
    g_ResidentOwner = NULL;
    Dev->ResidentShutdownResult = error;
    TraceDurable(L"B3eShutdownResult", error);
    TraceDurable(L"B3eResidentActive", 0u);
    return error;
}

#include "swgpu-idle.inc"

static VOID B3eResidentStatus(PSWGPU_DEVICE Dev, PSWGPU_RESIDENT_STATUS Out)
{
    volatile UCHAR *B = Dev->Bar0;
    ULONG pmc = 0u, lo0 = 0u, lo1 = 0u, hi = 0u;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    Out->Resident = Dev->ResidentActive && !Dev->BlitReset ? 1u : 0u;
    Trace(L"F3LastWaitUs", Dev->BlitLastWaitUs);
    Trace(L"F3LastPolls", Dev->BlitLastPolls);
    Trace(L"F3Reset", Dev->BlitReset);
    Out->ShutdownResult = Dev->ResidentShutdownResult;
    if (B == NULL || Out->Resident == 0u) {
        Trace(L"B3eStatusResident", Out->Resident);
        return;
    }
    Out->Boot0 = Rd(B, NV_PMC_BOOT_0);
    lo0 = Rd(B, NV_PTIMER_TIME_0);
    hi = Rd(B, NV_PTIMER_TIME_1);
    lo1 = Rd(B, NV_PTIMER_TIME_0);
    Out->PtimerLoBefore = lo0; Out->PtimerHi = hi; Out->PtimerLoAfter = lo1;
    Out->PtimerDelta = lo1 - lo0;
    pmc = Rd(B, NV_PMC_ENABLE); Out->PmcEnable = pmc;
    Out->PrivRingStatus0 = Rd(B, 0x0012005Cu);
    Out->PrivRingStatus1 = Rd(B, 0x00120060u);
    Out->FecsMailbox0 = Rd(B, 0x00409800u);
    Out->FecsCpuctl = Rd(B, 0x00409100u);
    Out->FifoEngineStatus = Rd(B, 0x00002640u);
    Out->CcsrChannel = Rd(B, 0x00800004u);
    Out->PbdmaStatus = Rd(B, 0x00003080u);
    Out->PbdmaIntr0 = Rd(B, 0x00040108u);
    Out->MmuFaultStatus = Rd(B, 0x000025A0u);
    if (pmc & (1u << 12)) {
        Out->GrIntr = Rd(B, 0x00400100u);
        Out->GrStatus = Rd(B, 0x00400700u);
    }
#define STATUS_TRACE(name, field) Trace(L"B3eStatus" name, Out->field)
    STATUS_TRACE(L"Resident", Resident); STATUS_TRACE(L"Boot0", Boot0);
    STATUS_TRACE(L"PtimerLoBefore", PtimerLoBefore); STATUS_TRACE(L"PtimerHi", PtimerHi);
    STATUS_TRACE(L"PtimerLoAfter", PtimerLoAfter); STATUS_TRACE(L"PtimerDelta", PtimerDelta);
    STATUS_TRACE(L"PmcEnable", PmcEnable); STATUS_TRACE(L"PrivRingStatus0", PrivRingStatus0);
    STATUS_TRACE(L"PrivRingStatus1", PrivRingStatus1); STATUS_TRACE(L"FecsMailbox0", FecsMailbox0);
    STATUS_TRACE(L"FecsCpuctl", FecsCpuctl); STATUS_TRACE(L"FifoEngineStatus", FifoEngineStatus);
    STATUS_TRACE(L"CcsrChannel", CcsrChannel); STATUS_TRACE(L"PbdmaStatus", PbdmaStatus);
    STATUS_TRACE(L"PbdmaIntr0", PbdmaIntr0); STATUS_TRACE(L"GrIntr", GrIntr);
    STATUS_TRACE(L"GrStatus", GrStatus); STATUS_TRACE(L"MmuFaultStatus", MmuFaultStatus);
    STATUS_TRACE(L"ShutdownResult", ShutdownResult);
#undef STATUS_TRACE
}

static VOID SwGpuDispatchIoctl(WDFQUEUE Queue, WDFREQUEST Request,
    size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode)
{
    WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    PSWGPU_RESIDENT_STATUS out = NULL;
    PSWGPU_E2_RESULT e2Out = NULL;
    PSWGPU_E3_RESULT e3Out = NULL;
    NTSTATUS status;
    UNREFERENCED_PARAMETER(InputBufferLength);
    if (Dev->BlitReset && IoControlCode != IOCTL_SWGPU_RESIDENT_STATUS &&
        IoControlCode != IOCTL_SWGPU_RESIDENT_STOP) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }
    if (IoControlCode == IOCTL_SWGPU_BLIT_TEST) {
        SWGPU_F2_RESULT *result = NULL;
        if (!Dev->ResidentActive) {
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*result), (PVOID *)&result, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        F2Benchmark(Dev, result);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*result));
        return;
    }
    if (IoControlCode == IOCTL_SWGPU_COPY_TEST) {
        PSWGPU_F1_RESULT copy = NULL;
        if (!Dev->ResidentActive || g_ResidentOwner != Dev) {
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
            return;
        }
        if (Dev->CopyInFlight) {
            WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*copy), (PVOID *)&copy, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        F1CopyTest(Dev, copy);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*copy));
        return;
    }
    if (Dev->CopyInFlight &&
        (IoControlCode == IOCTL_SWGPU_SUBMIT_TEST || IoControlCode == IOCTL_SWGPU_IRQ_TEST)) {
        WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
        return;
    }
    if (IoControlCode == IOCTL_SWGPU_CYCLE_TEST) {
        PSWGPU_E4_RESULT cycle = NULL;
        SWGPU_E2_RESULT submit;
        const SWGPU_DMA_POOL *pool = &Dev->DmaPool;

        if (Dev->ResidentActive || g_ResidentOwner != NULL) {
            WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*cycle),
            (PVOID *)&cycle, NULL);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        RtlZeroMemory(cycle, sizeof(*cycle));
        cycle->Version = SWGPU_VERSION;
        cycle->SubmitResult = STATUS_DEVICE_NOT_READY;
        cycle->StopResult = STATUS_DEVICE_NOT_READY;
        /* The power-managed sequential queue holds D0 through start, test
         * and stop. Keep all three in one request so other IOCTLs cannot
         * interleave with the temporary resident session. */
        status = StartResident(Dev);
        cycle->StartResult = NT_SUCCESS(status) ? 0u : ReadParam(L"E1Result", (ULONG)status);
        if (NT_SUCCESS(status) && Dev->ResidentActive) {
            cycle->SubmitResult = B3eResidentSubmitTest(Dev, &submit, TRUE);
            cycle->Rounds = submit.Rounds;
            cycle->MaxPolls = submit.MaxPolls;
        }
        if (Dev->ResidentActive)
            cycle->StopResult = B3eResidentShutdown(Dev, TRUE);
        cycle->PoolResult = pool->Result;
        cycle->PoolBytes = pool->Bytes;
        cycle->PoolUsedPeak = pool->UsedPeak;
        cycle->PoolFallbacks = pool->Fallbacks;
        cycle->PoolAllocs = pool->Allocs;
        cycle->PoolFrees = pool->Frees;
        cycle->PoolFailures = pool->Failures;
        cycle->PoolUsed = pool->Used;
        Trace(L"E4CycleStartResult", cycle->StartResult);
        Trace(L"E4CycleSubmitResult", cycle->SubmitResult);
        Trace(L"E4CycleStopResult", cycle->StopResult);
        Trace(L"E4CycleRounds", cycle->Rounds);
        Trace(L"E4CycleMaxPolls", cycle->MaxPolls);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*cycle));
        return;
    }
    if (IoControlCode == IOCTL_SWGPU_IRQ_TEST) {
        if (!Dev->ResidentActive || g_ResidentOwner != Dev) {
            Trace(L"E3IoctlNotReady", STATUS_DEVICE_NOT_READY);
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY); return;
        }
        if (OutputBufferLength < sizeof(SWGPU_E3_RESULT)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SWGPU_E3_RESULT),
            (PVOID *)&e3Out, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        TraceDurable(L"B3eControlMode", 10u);
        (VOID)B3eResidentIrqTest(Dev, e3Out);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*e3Out));
        return;
    }
    if (IoControlCode == IOCTL_SWGPU_SUBMIT_TEST) {
        if (!Dev->ResidentActive || g_ResidentOwner != Dev) {
            Trace(L"E2IoctlNotReady", STATUS_DEVICE_NOT_READY);
            WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY); return;
        }
        if (OutputBufferLength < sizeof(SWGPU_E2_RESULT)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SWGPU_E2_RESULT),
            (PVOID *)&e2Out, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        TraceDurable(L"B3eControlMode", 9u);
        (VOID)B3eResidentSubmitTest(Dev, e2Out, FALSE);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*e2Out));
        return;
    }
    if (IoControlCode != IOCTL_SWGPU_RESIDENT_STATUS &&
        IoControlCode != IOCTL_SWGPU_RESIDENT_STOP) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST); return;
    }
    if (OutputBufferLength < sizeof(SWGPU_RESIDENT_STATUS)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL); return;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SWGPU_RESIDENT_STATUS),
        (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
    if (IoControlCode == IOCTL_SWGPU_RESIDENT_STOP) {
        TraceDurable(L"B3eControlMode", 8u);
        B3eResidentStatus(Dev, out);
        G1ManualStop(Dev);
        out->ShutdownResult = B3eResidentShutdown(Dev, TRUE);
        G1TraceState(Dev);
        Trace(L"B3eStatusShutdownResult", out->ShutdownResult);
    } else {
        Step(112);
        if (Dev->ResidentActive && !Dev->BlitReset && Dev->Bar0 != NULL) {
            ULONG timerBefore = Rd(Dev->Bar0, NV_PTIMER_TIME_0);
            KeStallExecutionProcessor(1000u);
            Trace(L"E5PtimerNsPer1ms", Rd(Dev->Bar0, NV_PTIMER_TIME_0) - timerBefore);
        }
        TraceDurable(L"B3eControlMode", 7u);
        B3eResidentStatus(Dev, out);
    }
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(*out));
}

VOID SwGpuEvtIoDeviceControl(WDFQUEUE Queue, WDFREQUEST Request,
    size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice(WdfIoQueueGetDevice(Queue));
    if (InterlockedCompareExchange(&dev->BlitGate, 1, 0) != 0) {
        WdfRequestComplete(Request, STATUS_DEVICE_BUSY);
        return;
    }
    SwGpuDispatchIoctl(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode);
    InterlockedExchange(&dev->BlitGate, 0);
}

/* Complete this kernel-only IRP inline. A present must not queue behind a
 * benchmark. Both dispatch paths share the same gate, including Stop. */
static NTSTATUS SwGpuBlitInternal(WDFDEVICE Device, PIRP Irp)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice(Device);
    PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    if (stack->Parameters.DeviceIoControl.IoControlCode == IOCTL_SWGPU_PRESENT_BLIT &&
        Irp->RequestorMode == KernelMode && KeGetCurrentIrql() == PASSIVE_LEVEL &&
        stack->Parameters.DeviceIoControl.InputBufferLength == sizeof(SWGPU_BLIT_REQUEST) &&
        Irp->AssociatedIrp.SystemBuffer != NULL) {
        status = STATUS_DEVICE_NOT_READY;
        if (InterlockedCompareExchange(&dev->BlitOnline, 0, 0) == 0) {
            if (!G1RequestWake(dev)) {
                /* Keep the offline diagnostics for the legacy policy. */
                Trace(L"F3NotReadyWhy", 0x80000000u);
                Trace(L"F3NotReadyCount", ReadParam(L"F3NotReadyCount", 0u) + 1u);
                Trace(L"F3OfflineResident", dev->ResidentActive ? 1u : 0u);
                Trace(L"F3OfflineIdleRef", dev->ResidentIdleRefHeld ? 1u : 0u);
            }
        } else {
            status = STATUS_DEVICE_BUSY;
            if (InterlockedCompareExchange(&dev->BlitGate, 1, 0) == 0) {
                status = InterlockedCompareExchange(&dev->BlitOnline, 0, 0) != 0
                    ? F3Present(dev, (const SWGPU_BLIT_REQUEST *)Irp->AssociatedIrp.SystemBuffer)
                    : STATUS_DEVICE_NOT_READY;
                if (NT_SUCCESS(status))
                    InterlockedExchange64(&dev->LastBlitTime, (LONG64)KeQueryInterruptTime());
                InterlockedExchange(&dev->BlitGate, 0);
            }
        }
    }
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = 0u;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

static BOOLEAN SwGpuDrainBlit(PSWGPU_DEVICE Dev, ULONG TimeoutMs)
{
    ULONG polls;
    BOOLEAN acquired = FALSE;
    ULONGLONG start = KeQueryInterruptTime();
    InterlockedExchange(&Dev->BlitOnline, 0);
    for (polls = 1u; ; ++polls) {
        acquired = InterlockedCompareExchange(&Dev->BlitGate, 1, 0) == 0;
        if (acquired || KeQueryInterruptTime() - start >= TimeoutMs * 10000ull ||
            polls == TimeoutMs) break;
        Ms(1u);
    }
    Trace(L"F3DrainPolls", polls);
    Trace(L"F3DrainStatus", acquired ? 0u : (ULONG)STATUS_IO_TIMEOUT);
    return acquired;
}

static NTSTATUS SwGpuEvtD0Ready(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice(Device);
    KIRQL oldIrql;
    UNREFERENCED_PARAMETER(PreviousState);
    KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
    dev->PowerD0 = TRUE;
    dev->PowerPaused = FALSE;
    if (!dev->PowerFinal && dev->ResidentActive && !dev->BlitReset)
        InterlockedExchange(&dev->BlitOnline, 1);
    KeReleaseSpinLock(&dev->PowerLock, oldIrql);
    G1ResumeScheduler(dev);
    return STATUS_SUCCESS;
}

static NTSTATUS SwGpuEvtD0Quiesce(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice(Device);
    return G1Pause(dev, TargetState == WdfPowerDeviceD3Final) ?
           STATUS_SUCCESS : STATUS_DEVICE_BUSY;
}

NTSTATUS SwGpuEvtD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    if (TargetState == WdfPowerDeviceD3Final && !G1Pause(Dev, TRUE))
        return STATUS_DEVICE_BUSY;
    if (!SwGpuDrainBlit(Dev, TargetState == WdfPowerDeviceD3Final ? G1_DRAIN_MS : 200u))
        return STATUS_DEVICE_BUSY;
    if (TargetState == WdfPowerDeviceD3Final) {
        (VOID)B3eResidentShutdown(Dev, FALSE);
        G1ReleaseIdleIfOff(Dev);
        G1TraceState(Dev);
    }
    InterlockedExchange(&Dev->BlitGate, 0);
    if (TargetState != WdfPowerDeviceD3Final) G1ResumeScheduler(Dev);
    return STATUS_SUCCESS;
}

NTSTATUS SwGpuEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index, Mem = 0, I2c = 0;
    PHYSICAL_ADDRESS Pa;
    PCM_PARTIAL_RESOURCE_DESCRIPTOR irqRaw[2] = { NULL, NULL };
    PCM_PARTIAL_RESOURCE_DESCRIPTOR irqTranslated[2] = { NULL, NULL };
    ULONG interruptCount = 0u;
    NTSTATUS interruptStatus = STATUS_SUCCESS;

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc == NULL) continue;
        if (Desc->Type == CmResourceTypeMemory) {
            if (Mem == 0) { Dev->Bar0Phys = Desc->u.Memory.Start; Dev->Bar0Len = Desc->u.Memory.Length; }
            else if (Mem == 1) { Dev->Bar1Phys = Desc->u.Memory.Start; Dev->Bar1Len = Desc->u.Memory.Length; }
            ++Mem;
        } else if (Desc->Type == CmResourceTypeConnection &&
                   Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                   Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            LARGE_INTEGER *Id = (I2c == 0) ? &Dev->VddId : &Dev->PmicId;    /* DSDT order */
            if (I2c < 2) { Id->LowPart = Desc->u.Connection.IdLowPart; Id->HighPart = (LONG)Desc->u.Connection.IdHighPart; }
            ++I2c;
        } else if (Desc->Type == CmResourceTypeInterrupt) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR rawDesc =
                Index < WdfCmResourceListGetCount(Raw) ? WdfCmResourceListGetDescriptor(Raw, Index) : NULL;
            if (interruptCount < RTL_NUMBER_OF(irqTranslated)) {
                irqRaw[interruptCount] = rawDesc;
                irqTranslated[interruptCount] = Desc;
            }
            ++interruptCount;
        }
    }
    Trace(L"ResMem", Mem);
    Trace(L"ResI2c", I2c);
    TraceDetail(L"Bar0Lo", Dev->Bar0Phys.LowPart);
    TraceDetail(L"Bar1Lo", Dev->Bar1Phys.LowPart);
    TraceDetail(L"Bar1Len", Dev->Bar1Len);
    if (Mem < 2 || I2c < 2 || Dev->Bar0Len < BAR0_MAP_LEN ||
        Dev->Bar1Len < BAR1_D3_MAP_LEN) return STATUS_DEVICE_CONFIGURATION_ERROR;

    /* Mapping BAR0 is safe; only reading it while the GPU is off is not. */
    Dev->Bar0 = (volatile UCHAR *)MmMapIoSpaceEx(Dev->Bar0Phys, BAR0_MAP_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Dev->Bar1MapLen = BAR1_D3_MAP_LEN;
    Dev->Bar1Cpu = (volatile UCHAR *)MmMapIoSpaceEx(Dev->Bar1Phys, Dev->Bar1MapLen,
                                                     PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)CAR_PHYS;
    Dev->Car = (volatile UCHAR *)MmMapIoSpaceEx(Pa, CAR_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)PMC_PHYS;
    Dev->Pmc = (volatile UCHAR *)MmMapIoSpaceEx(Pa, PMC_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)MC_PHYS;
    Dev->Mc = (volatile UCHAR *)MmMapIoSpaceEx(Pa, MC_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Dev->Bar0 || !Dev->Bar1Cpu || !Dev->Car || !Dev->Pmc || !Dev->Mc)
        return STATUS_INSUFFICIENT_RESOURCES;
    Dev->HaveInterruptResources = FALSE;
    Dev->InterruptSourcesEnabled = FALSE;
    Dev->InterruptResourceCount = 0u;
    InterlockedExchange(&Dev->E3IsrActive, 0);
    Dev->StallInterrupt = NULL;
    Dev->NonstallInterrupt = NULL;
    Trace(L"E3InterruptResourceCount", interruptCount);
    Dev->InterruptResourceCount = interruptCount;
    if (interruptCount >= 2u && irqRaw[0] != NULL && irqRaw[1] != NULL) {
        WDF_INTERRUPT_CONFIG interruptConfig;
        WDF_OBJECT_ATTRIBUTES interruptAttributes;
        WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, SwGpuEvtStallInterruptIsr, NULL);
        interruptConfig.EvtInterruptDpc = SwGpuEvtInterruptDpc;
        interruptConfig.InterruptRaw = irqRaw[0];
        interruptConfig.InterruptTranslated = irqTranslated[0];
        /* Claude 0.7.2: ShareVector stays WdfUseDefault; in PrepareHardware anything else is
           STATUS_INVALID_DEVICE_STATE (0xC0000184). The framework uses the resource's share disposition. */
        interruptConfig.AutomaticSerialization = FALSE;
        /* Claude 0.7.1: no ParentObject (0xC020020F STATUS_WDF_PARENT_ASSIGNMENT_NOT_ALLOWED);
           an interrupt's parent is always its device. */
        WDF_OBJECT_ATTRIBUTES_INIT(&interruptAttributes);
        interruptStatus = WdfInterruptCreate(Device, &interruptConfig,
                                              &interruptAttributes, &Dev->StallInterrupt);
        if (NT_SUCCESS(interruptStatus)) {
            WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, SwGpuEvtNonstallInterruptIsr, NULL);
            interruptConfig.EvtInterruptDpc = SwGpuEvtInterruptDpc;
            interruptConfig.InterruptRaw = irqRaw[1];
            interruptConfig.InterruptTranslated = irqTranslated[1];
            /* Claude 0.7.2: ShareVector stays WdfUseDefault; in PrepareHardware anything else is
           STATUS_INVALID_DEVICE_STATE (0xC0000184). The framework uses the resource's share disposition. */
            interruptConfig.AutomaticSerialization = FALSE;
            interruptStatus = WdfInterruptCreate(Device, &interruptConfig,
                                                  &interruptAttributes, &Dev->NonstallInterrupt);
        }
        if (NT_SUCCESS(interruptStatus)) {
            Dev->HaveInterruptResources = TRUE;
        } else {
            if (Dev->StallInterrupt != NULL) { WdfObjectDelete(Dev->StallInterrupt); Dev->StallInterrupt = NULL; }
            if (Dev->NonstallInterrupt != NULL) { WdfObjectDelete(Dev->NonstallInterrupt); Dev->NonstallInterrupt = NULL; }
        }
    }
    Trace(L"E3InterruptCreateStatus", (ULONG)interruptStatus);
    Trace(L"E3HaveInterrupts", Dev->HaveInterruptResources ? 1u : 0u);
    E4ReservePool(Dev);
    {
        KIRQL oldIrql;
        KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
        Dev->PowerFinal = FALSE;
        KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
    }
    return STATUS_SUCCESS;
}

NTSTATUS SwGpuEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (!G1Pause(Dev, TRUE)) return STATUS_DEVICE_BUSY;
    if (!SwGpuDrainBlit(Dev, G1_DRAIN_MS)) return STATUS_DEVICE_BUSY;
    (VOID)B3eResidentShutdown(Dev, FALSE);
    G1ReleaseIdleIfOff(Dev);
    G1TraceState(Dev);
    E4ReleasePool(Dev);
    if (Dev->StallInterrupt != NULL) { WdfObjectDelete(Dev->StallInterrupt); Dev->StallInterrupt = NULL; }
    if (Dev->NonstallInterrupt != NULL) { WdfObjectDelete(Dev->NonstallInterrupt); Dev->NonstallInterrupt = NULL; }
    Dev->HaveInterruptResources = FALSE;
    if (Dev->Bar0) { MmUnmapIoSpace((PVOID)Dev->Bar0, BAR0_MAP_LEN); Dev->Bar0 = NULL; }
    if (Dev->Bar1Cpu) { MmUnmapIoSpace((PVOID)Dev->Bar1Cpu, Dev->Bar1MapLen); Dev->Bar1Cpu = NULL; }
    if (Dev->Car)  { MmUnmapIoSpace((PVOID)Dev->Car, CAR_LEN);       Dev->Car = NULL; }
    if (Dev->Pmc)  { MmUnmapIoSpace((PVOID)Dev->Pmc, PMC_LEN);       Dev->Pmc = NULL; }
    if (Dev->Mc)   { MmUnmapIoSpace((PVOID)Dev->Mc, MC_LEN);         Dev->Mc = NULL; }
    InterlockedExchange(&Dev->BlitGate, 0);
    return STATUS_SUCCESS;
}

NTSTATUS SwGpuEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    NTSTATUS st;
    ULONG mode;
    BOOLEAN autoResident;

    g_Verbose = ReadParam(L"Verbose", 0u) != 0u;
    Trace(L"E5VerboseActive", g_Verbose ? 1u : 0u);
    Trace(L"Version", SWGPU_VERSION);
    Trace(L"StartCount", ReadParam(L"StartCount", 0) + 1);
    st = OpenTarget(Device, Dev->VddId, &Dev->Vdd);
    Trace(L"OpenVddStatus", (ULONG)st);
    st = OpenTarget(Device, Dev->PmicId, &Dev->Pmic);
    Trace(L"OpenPmicStatus", (ULONG)st);

    Snapshot(Dev, FALSE);

    mode = ReadParam(L"PowerTest", 0);
    /* Claude 0.12.4: boot guard for the persistent AutoResident (F3). If the
     * previous auto start never finished (hang/reset during GPU bring-up),
     * turn AutoResident and the clock request off instead of looping. */
    if (mode != 6u && ReadParam(L"AutoResident", 0u) == 1u &&
        ReadParam(L"AutoResidentPending", 0u) == 1u) {
        TraceDurable(L"AutoResident", 0u);
        TraceDurable(L"GpcKhz", 0u);
        TraceDurable(L"AutoResidentPending", 0u);
        TraceDurable(L"AutoResidentGuardTripped", 1u);
    }
    autoResident = ReadParam(L"AutoResident", 0u) == 1u;
    if (autoResident && ReadParam(L"IdlePowerOffMs", G1_DEFAULT_IDLE_MS) != 0u)
        Dev->PowerDisabled = G1BootLatch(FALSE);
    if (mode == 6u || (autoResident && !Dev->PowerDisabled)) {
        LARGE_INTEGER t0, t1, f;
        ULONG bootUs;
        BOOLEAN autoStart = mode != 6u;
        TraceDurable(L"PowerTest", 0);
        if (autoStart) TraceDurable(L"AutoResidentPending", 1u);
        Trace(L"Result", 0xFFFFFFFFu);
        TraceDetail(L"Boot0", 0);
        t0 = KeQueryPerformanceCounter(&f);
        if (SwGpuDrainBlit(Dev, 200u)) {
            st = B3eStartOnline(Dev, autoStart, FALSE);
            t1 = KeQueryPerformanceCounter(NULL);
            Snapshot(Dev, TRUE);
            InterlockedExchange(&Dev->BlitGate, 0);
        } else {
            st = STATUS_DEVICE_BUSY;
            TraceDurable(L"E1Result", (ULONG)st);
            t1 = KeQueryPerformanceCounter(NULL);
        }
        bootUs = (ULONG)((t1.QuadPart - t0.QuadPart) * 1000000 / (f.QuadPart ? f.QuadPart : 1));
        Trace(L"TestUs", bootUs);
        Trace(L"TestRuns", ReadParam(L"TestRuns", 0) + 1);
        if (autoStart) TraceDurable(L"AutoResidentPending", 0u);
        G1StartPolicy(Dev, autoResident, st, bootUs);
    } else {
        G1StartPolicy(Dev, autoResident, STATUS_DEVICE_NOT_READY, 0u);
    }
    return STATUS_SUCCESS;      /* the device always starts: it only reports */
}

NTSTATUS SwGpuEvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS Pnp;
    WDF_OBJECT_ATTRIBUTES Attributes;
    WDFDEVICE Device;
    NTSTATUS Status;
    UNREFERENCED_PARAMETER(Driver);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&Pnp);
    Pnp.EvtDevicePrepareHardware = SwGpuEvtPrepareHardware;
    Pnp.EvtDeviceReleaseHardware = SwGpuEvtReleaseHardware;
    Pnp.EvtDeviceSelfManagedIoInit = SwGpuEvtSelfManagedIoInit;
    Pnp.EvtDeviceD0Exit = SwGpuEvtD0Exit;
    Pnp.EvtDeviceD0EntryPostInterruptsEnabled = SwGpuEvtD0Ready;
    Pnp.EvtDeviceD0ExitPreInterruptsDisabled = SwGpuEvtD0Quiesce;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_UNKNOWN);
    /* Claude 0.5.1: an unnamed PnP FDO rejects WdfDeviceInitAssignSDDLString (0xC0000079).
       Admin-only access now comes from the INF [SwGpu_Inst.NT.HW] Security value. */
    Status = WdfDeviceInitAssignWdmIrpPreprocessCallback(DeviceInit, SwGpuBlitInternal,
        IRP_MJ_INTERNAL_DEVICE_CONTROL, NULL, 0);
    if (!NT_SUCCESS(Status)) return Status;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWGPU_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;
    SwGpuGetDevice(Device)->Device = Device;
    G1CreateObjects(SwGpuGetDevice(Device));
    KeInitializeEvent(&SwGpuGetDevice(Device)->E3Event, SynchronizationEvent, FALSE);
    Status = WdfDeviceCreateDeviceInterface(Device, &SwGpuBlitDeviceInterface, NULL);
    if (!NT_SUCCESS(Status)) return Status;
    {
        WDF_IO_QUEUE_CONFIG QueueConfig;
        WDF_OBJECT_ATTRIBUTES QueueAttributes;
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&QueueConfig, WdfIoQueueDispatchSequential);
        QueueConfig.PowerManaged = WdfTrue;
        QueueConfig.EvtIoDeviceControl = SwGpuEvtIoDeviceControl;
        WDF_OBJECT_ATTRIBUTES_INIT(&QueueAttributes);
        QueueAttributes.ExecutionLevel = WdfExecutionLevelPassive;
        Status = WdfIoQueueCreate(Device, &QueueConfig, &QueueAttributes, WDF_NO_HANDLE);
        if (!NT_SUCCESS(Status)) return Status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG Config;
    UNICODE_STRING Params;

    RtlInitEmptyUnicodeString(&Params, SwGpuParamsPath, sizeof(SwGpuParamsPath) - sizeof(WCHAR));
    if (NT_SUCCESS(RtlUnicodeStringCopy(&Params, RegistryPath)) &&
        NT_SUCCESS(RtlUnicodeStringCatString(&Params, L"\\Parameters"))) {
        SwGpuParamsPath[Params.Length / sizeof(WCHAR)] = L'\0';
        (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE, SwGpuParamsPath);
    } else {
        SwGpuParamsPath[0] = L'\0';
    }
    WDF_DRIVER_CONFIG_INIT(&Config, SwGpuEvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &Config, WDF_NO_HANDLE);
}
