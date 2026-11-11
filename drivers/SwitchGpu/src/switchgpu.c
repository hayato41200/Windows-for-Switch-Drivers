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
 * PowerTest = 1; then, once (PowerTest is reset to 0 first), the GPU is
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
 * BAR0 only if the clamp is off, the clock on and the reset low. Unless
 * KeepOn = 1, the GPU is then powered down in the reverse order and every
 * register is put back as it was found.
 *
 * Before each step "Step" is written and the key flushed to disk, so after a
 * hang and a restart Status.cmd shows where it stopped.
 * Trace: HKLM\SYSTEM\CurrentControlSet\Services\switchgpu\Parameters
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#include <spb.h>

#define SWGPU_VERSION      0x00000402u   /* 0.4.2 */

#define CAR_PHYS           0x60006000ull
#define CAR_LEN            0x1000u
#define PMC_PHYS           0x7000E000ull /* page; registers at + 0x400 */
#define PMC_LEN            0x1000u
#define MC_PHYS            0x70019000ull
#define MC_LEN             0x1000u
#define BAR0_MAP_LEN       0x200000u    /* up to NV_PFB (0x100000) */

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
/* Stage B1 probe (PowerTest = 2): no firmware, nothing executed on the GPU. */
#define NV_PTOP_DEVICE_INFO 0x022700    /* 64 entries (nouveau gk104_top_parse) */
#define NV_PTOP_SCAL       0x022430    /* num GPCs, TPC/GPC, FBPs, ... (nvgpu hw_top) */
#define PMU_FALCON         0x10A000
#define FALCON_MAILBOX0    0x040
#define FALCON_MAILBOX1    0x044
#define FALCON_CPUCTL      0x100
#define FALCON_HWCFG       0x108       /* IMEM = ([8:0]) << 8, DMEM = ([17:9]) << 8 */
#define FALCON_DMACTL      0x10C
#define FALCON_ITFEN       0x048       /* bit 0: external interface on (nouveau sets it before DMA) */
#define FALCON_HWCFG1      0x12C
#define FALCON_SCTL        0x240
#define FALCON_DMEMC0      0x1C0       /* BIT24 auto-increment write, BIT25 read */
#define FALCON_DMEMD0      0x1C4
#define PMU_DEBUG          0xC08       /* bit 20 = debug-fused (nouveau gm200_pmu_flcn.debug) */
/* Stage B2a probe (PowerTest = 3): falcon DMA in physical mode, no firmware.
 * Falcon v4 DMA (open-gpu-kernel-modules dev_falcon_v4.h): */
#define FALCON_DMATRFBASE  0x110       /* address >> 8 */
#define FALCON_DMATRFMOFFS 0x114       /* DMEM/IMEM offset */
#define FALCON_DMATRFCMD   0x118       /* IDLE bit 1, IMEM bit 4, WRITE bit 5 (falcon -> memory), SIZE [10:8], CTXDMA [14:12] */
#define FALCON_DMATRFFBOFFS 0x11C      /* offset from DMATRFBASE */
#define FALCON_BIND_TRIGGER 0x200      /* PMU bind status query trigger */
#define FALCON_BIND_STATUS  0x20C      /* bind state in [14:12], 5 = bound */
#define FALCON_BIND_INST    0x480      /* instance-block address/target */
#define FALCON_CTX_ENABLE   0x090      /* bit 16 enables context binding */
#define FALCON_CTX_ACTION   0x058      /* bit 1: commit pending context bind */
#define DMATRFCMD_IDLE     (1u << 1)
#define DMATRFCMD_WRITE    (1u << 5)
#define DMATRFCMD_256B     (6u << 8)
#define DMA_IDX            4u          /* ctx dma index 4 = PHYS_SYS_NCOH in nouveau's PMU setup */
/* PMU FBIF at + 0xE00 (nouveau gm200_pmu_flcn_bind_inst); TRANSCFG(i) = + 4 i:
 * TARGET [1:0] (2 = noncoherent sysmem), MEM_TYPE bit 2 (1 = physical);
 * FBIF_CTL + 0x24 bit 7 = ALLOW_PHYS_NO_CTX (dev_fbif_v4.h). */
#define PMU_FBIF_TRANSCFG(i) (0xE00 + (i) * 4)
#define PMU_FBIF_CTL       0xE24
#define FBIF_PHYS_NCOH     6u
/* PMC_ENABLE units of the memory path (nvgpu hw_mc_gm20b: xbar bit 2, l2 bit 3,
 * pfb bit 20, hub bit 29; nvgpu gk20a_fb_reset resets these four together). */
#define PMC_ENABLE_MEMPATH ((1u << 2) | (1u << 3) | (1u << 20) | (1u << 29))
/* Tegra210 MC SMMU (Linux tegra210.c / tegra-smmu.c) */
#define MC_SMMU_CONFIG     0x010       /* bit 0 = SMMU on */
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
#define MC_SMMU_CLIENT_230 0x230       /* bit 24 GPUSRD, bit 25 GPUSWR */
#define MC_SMMU_CLIENT_B98 0xB98       /* bit 8 GPUSRD2, bit 9 GPUSWR2 */

typedef struct _SWGPU_DEVICE {
    WDFDEVICE      Device;
    PHYSICAL_ADDRESS Bar0Phys;
    ULONG          Bar0Len;
    volatile UCHAR *Bar0;
    volatile UCHAR *Car;
    volatile UCHAR *Pmc;
    volatile UCHAR *Mc;
    LARGE_INTEGER  VddId;
    LARGE_INTEGER  PmicId;
    WDFIOTARGET    Vdd;          /* MAX77621 0x1C */
    WDFIOTARGET    Pmic;         /* MAX77620 0x3C */
} SWGPU_DEVICE, *PSWGPU_DEVICE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWGPU_DEVICE, SwGpuGetDevice)

#define B2A_RETAINED_PAGE_PAIRS 8u
static PVOID g_B2aRetainedPages[B2A_RETAINED_PAGE_PAIRS][2];
static ULONG g_B2aRetainedPagePairs;

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD SwGpuEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE SwGpuEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE SwGpuEvtReleaseHardware;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT SwGpuEvtSelfManagedIoInit;

static WCHAR SwGpuParamsPath[256];

/* ---------------------------------------------------------------- trace */

static VOID Trace(PCWSTR Name, ULONG Value)
{
    if (SwGpuParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwGpuParamsPath, Name, REG_DWORD, &Value, sizeof(Value));
    }
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

static VOID RetainB2aPages(PVOID Inst, PVOID GpuPd)
{
    if (Inst == NULL && GpuPd == NULL) return;
    if (g_B2aRetainedPagePairs < B2A_RETAINED_PAGE_PAIRS) {
        g_B2aRetainedPages[g_B2aRetainedPagePairs][0] = Inst;
        g_B2aRetainedPages[g_B2aRetainedPagePairs][1] = GpuPd;
        ++g_B2aRetainedPagePairs;
        TraceDurable(L"B2aBindPagesRetained", g_B2aRetainedPagePairs);
    } else {
        /* Keep the pages allocated rather than risk freeing a live GPU context. */
        TraceDurable(L"B2aBindPagesRetainFull", g_B2aRetainedPagePairs);
    }
}

static VOID FreeB2aPagesAfterGpuReset(VOID)
{
    ULONG i;
    for (i = 0; i < g_B2aRetainedPagePairs; ++i) {
        if (g_B2aRetainedPages[i][0] != NULL)
            MmFreeContiguousMemorySpecifyCache(g_B2aRetainedPages[i][0], PAGE_SIZE, MmNonCached);
        if (g_B2aRetainedPages[i][1] != NULL)
            MmFreeContiguousMemorySpecifyCache(g_B2aRetainedPages[i][1], PAGE_SIZE, MmNonCached);
        g_B2aRetainedPages[i][0] = NULL;
        g_B2aRetainedPages[i][1] = NULL;
    }
    TraceDurable(L"B2aBindPagesFreedAfterReset", g_B2aRetainedPagePairs);
    g_B2aRetainedPagePairs = 0;
}

static VOID TraceBin(PCWSTR Name, PVOID Data, ULONG Length)
{
    if (SwGpuParamsPath[0] != L'\0' && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE, SwGpuParamsPath, Name, REG_BINARY, Data, Length);
    }
}

/* ------------------------------------------------------------------- MMIO */

static ULONG Rd(volatile UCHAR *Base, ULONG Off) { return READ_REGISTER_ULONG((volatile ULONG *)(Base + Off)); }
static VOID Wr(volatile UCHAR *Base, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG((volatile ULONG *)(Base + Off), V); }
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

/* Error values from a PRI read that nobody answered (0xBADFxxxx) or a dead bus. */
static BOOLEAN PriBad(ULONG v) { return (v & 0xFFFF0000u) == 0xBADF0000u || v == 0xFFFFFFFFu; }

/* nouveau gk20a_privring_init_privring_ring (B1 step 23, proven on hardware).
 * 0.3.14: shared with B2a, which never ran it: 0.3.13 showed the L2 (LTC)
 * registers answering with a PRI error (0xBADF3000) and 0 LTCs enumerated. */
static VOID PrivRingInit(volatile UCHAR *B, ULONG StepNo)
{
    Step(StepNo);
    Trace(L"RingStatus0Before", Rd(B, 0x120058));
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
    Trace(L"RingStatus0", Rd(B, 0x120058));
    Trace(L"RingCmd", Rd(B, 0x12004C));
    Trace(L"RingMaster", Rd(B, 0x122204));
    Trace(L"PmcEnableAfterRing", Rd(B, NV_PMC_ENABLE));
    TraceDurable(L"RingDone", StepNo);
}

/* With the GPU powered and answering as GM20B: read the topology table and the
 * PMU falcon, try a DMEM write/read through the PIO port (nothing is started),
 * then run nouveau's gk20a_privring init and read the ring status. */
static VOID ProbeB1(PSWGPU_DEVICE Dev)
{
    static const ULONG pat[4] = { 0x5A5A0001u, 0xA5A50002u, 0x12345678u, 0xFEDCBA98u };
    ULONG ptop[64], scal[12], rb[4], i, ok = 0, hw;
    volatile UCHAR *B = Dev->Bar0;

    Step(20);   /* topology, scal */
    for (i = 0; i < 64; ++i) ptop[i] = Rd(B, NV_PTOP_DEVICE_INFO + i * 4);
    TraceBin(L"Ptop", ptop, sizeof(ptop));
    for (i = 0; i < 12; ++i) scal[i] = Rd(B, NV_PTOP_SCAL + i * 4);
    TraceBin(L"Scal", scal, sizeof(scal));
    TraceDurable(L"PtopRead", 1);

    Step(21);   /* PMU falcon registers */
    hw = Rd(B, PMU_FALCON + FALCON_HWCFG);
    Trace(L"PmuHwcfg", hw);
    Trace(L"PmuHwcfg1", Rd(B, PMU_FALCON + FALCON_HWCFG1));
    Trace(L"PmuCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    Trace(L"PmuDmactl", Rd(B, PMU_FALCON + FALCON_DMACTL));
    Trace(L"PmuSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
    Trace(L"PmuDebug", Rd(B, PMU_FALCON + PMU_DEBUG));
    Trace(L"PmuMbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
    Trace(L"PmuMbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
    TraceDurable(L"PmuRead", 1);

    if (!PriBad(hw) && hw != 0) {
        Step(22);   /* DMEM PIO write / read back at offset 0, then clear */
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24));
        for (i = 0; i < 4; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, pat[i]);
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25));
        for (i = 0; i < 4; ++i) { rb[i] = Rd(B, PMU_FALCON + FALCON_DMEMD0); if (rb[i] == pat[i]) ++ok; }
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 24));
        for (i = 0; i < 4; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, 0);
        TraceBin(L"DmemReadBack", rb, sizeof(rb));
        TraceDurable(L"DmemOk", ok);
    }

    PrivRingInit(B, 23);
}

/* ------------------------------------------------------ stage B2a probe */

static ULONG DmaWait(volatile UCHAR *B)
{
    ULONG i;
    for (i = 0; i < 2000; ++i) {
        if (Rd(B, PMU_FALCON + FALCON_DMATRFCMD) & DMATRFCMD_IDLE) return i;
        Us(5);
    }
    return 0xFFFFFFFFu;
}

/* GM200 PMU bind handshake from nouveau gm200_pmu_flcn_bind_inst/bind_stat.
 * Return the final state (0 means complete); expose the intermediate state
 * and poll counts so the probe can identify exactly where it stopped. */
static BOOLEAN PmuBindInst(volatile UCHAR *B, ULONG instAddress,
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
       0x40000000u | (3u << 28) | (instAddress >> 12)); /* SYS_NCOH */

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

/* With the GPU powered and answering as GM20B: check that the GPU is not
 * behind the SMMU, then let the PMU falcon's DMA engine copy 256 bytes from a
 * Windows buffer into DMEM and 256 bytes from DMEM back into the buffer, in
 * physical mode (no GPU page tables), driven from the CPU. Nothing runs on the
 * falcon. The buffer is 4 KB, physically contiguous, below 4 GB, uncached;
 * only bytes 0x100..0x1FF of it are a DMA target. */
/* 0.3.13 (read only): the GPU-side memory path state that nouveau programs
 * on GM20B before the first falcon DMA (fb/gm200.c, fb/gf100.c, ltc/gm200.c,
 * bus/gf100.c). 0.3.12 showed no MC interrupt from any GPU step, so the DMA
 * most likely stays inside the GPU. The order must match Gpu.ps1. */
static const ULONG g_GpuMemRegs[] = {
    0x000100,   /* PMC_INTR */
    0x000200,   /* PMC_ENABLE */
    0x001100,   /* PBUS interrupt status */
    0x001140,   /* PBUS interrupt enable */
    0x009084,   /* PBUS fault address */
    0x009088,   /* PBUS fault data */
    0x070004,   /* L2 invalidate control/status */
    0x070010,   /* L2 flush control/status */
    0x100C10,   /* FB sysmem flush page (>> 8) */
    0x100C80,   /* FB MMU control (big page) */
    0x100CC4,   /* FB MMU debug control */
    0x100CC8,   /* FB MMU debug write buffer (>> 8) */
    0x100CCC,   /* FB MMU debug read buffer (>> 8) */
    0x12006C,   /* LTC count */
    0x17E278,   /* LTC tag base */
    0x17E280,   /* LTS count (bits 31..28) */
    0x022438,   /* FBP count */
};

static VOID DumpGpuMem(volatile UCHAR *B, PCWSTR Name)
{
    ULONG v[sizeof(g_GpuMemRegs) / sizeof(g_GpuMemRegs[0])];
    ULONG i;
    for (i = 0; i < sizeof(g_GpuMemRegs) / sizeof(g_GpuMemRegs[0]); ++i) v[i] = Rd(B, g_GpuMemRegs[i]);
    TraceBin(Name, v, sizeof(v));
}

static VOID ProbeB2a(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PHYSICAL_ADDRESS lo, hi, none, pa;
    volatile ULONG *buf;
    ULONG cfg, asid, c230, cb98, tc, ctl, dmactl, i, ok, polls, spill, now;
    ULONG oldPtbAsid, oldPtb0, base;
    ULONG instPmuWas = 0, ctxEnableWas = 0, fbifWas[5] = { 0 }, itfenWas = 0;
    ULONG bindState = 0, bindPoll = 0, bindDoneState = 0, bindDonePolls = 0;
    BOOLEAN idle = TRUE, asidChanged = FALSE, stillTranslated = FALSE, ptbChanged = FALSE;
    BOOLEAN bindReady = FALSE, bindTouched = FALSE, itfenTouched = FALSE;
    volatile ULONG *pd = NULL, *pt = NULL;
    volatile ULONG *inst = NULL, *gpuPd = NULL;
    PHYSICAL_ADDRESS pdPa, ptPa, instPa, gpuPdPa;
    volatile ULONG *fbPg[3] = { NULL, NULL, NULL };
    ULONG fbRegWas[4] = { 0 };
    BOOLEAN fbTouched = FALSE, fifoTouched = FALSE;

    PrivRingInit(B, 29);
    /* 0.3.16: nvgpu resets and enables PFIFO (mc reset FIFO = PMC_ENABLE
     * bit 8) before the PMU boots, and the GPU MMU reports faults through
     * PFIFO, which read 0xBAD0DA00 so far. Reset it, enable it, clear its
     * interrupt status. Turned off again in step 34. */
    {
        ULONG en = Rd(B, NV_PMC_ENABLE);
        Trace(L"B2aFifoPmcWas", en);
        Wr(B, NV_PMC_ENABLE, en & ~0x100u);
        (VOID)Rd(B, NV_PMC_ENABLE);
        Us(20);
        Wr(B, NV_PMC_ENABLE, en | 0x100u);
        (VOID)Rd(B, NV_PMC_ENABLE);
        Us(20);
        fifoTouched = !(en & 0x100u);
        Trace(L"B2aFifoPmcNow", Rd(B, NV_PMC_ENABLE));
        Trace(L"B2aFifoIntr0Was", Rd(B, 0x2100));
        Wr(B, 0x2100, 0xFFFFFFFFu);
        TraceDurable(L"B2aFifoIntr0Clr", Rd(B, 0x2100));
    }
    Trace(L"B2aLtcNrAfterRing", Rd(B, 0x12006C));
    DumpGpuMem(B, L"B2aGpuMemAfterRing");

    Step(30);   /* SMMU */
    Trace(L"McHotStatB2aStart", Rd(Dev->Mc, MC_HOTRESET_STAT_1));
    cfg = Rd(Dev->Mc, MC_SMMU_CONFIG);
    asid = Rd(Dev->Mc, MC_SMMU_GPU_ASID);
    c230 = Rd(Dev->Mc, MC_SMMU_CLIENT_230);
    cb98 = Rd(Dev->Mc, MC_SMMU_CLIENT_B98);
    Trace(L"SmmuConfig", cfg);
    Trace(L"SmmuGpuAsid", asid);
    Trace(L"SmmuClient230", c230);
    Trace(L"SmmuClientB98", cb98);
    /* 0.3.0 on hardware: config and the client registers read 0xFFFFFFFF and
     * the GPU swgroup reads 0x80000000 (translated, ASID 0). Linux runs the
     * GPU in identity mode by clearing that enable bit (tegra_smmu_disable),
     * so do the same for the test only, and put it back afterwards. */
    if (asid & 0x80000000u) {
        Wr(Dev->Mc, MC_SMMU_GPU_ASID, asid & ~0x80000000u);
        now = Rd(Dev->Mc, MC_SMMU_GPU_ASID);
        TraceDurable(L"SmmuGpuAsidBypass", now);
        if (now & 0x80000000u) {
            /* 0.3.1 on hardware: the write is ignored (the SMMU registers look
             * locked to the secure world). Go on anyway: the read DMA cannot
             * harm anything, and the write DMA only runs if the read proves
             * the address reaches our buffer unchanged. */
            TraceDurable(L"SmmuStillTranslated", 1);
            stillTranslated = TRUE;
        } else {
            asidChanged = TRUE;
        }
    }
    /* 0.3.3: the page directory of ASID 0 (ATF leaves ASID_SECURITY = 0, so
     * the PTB registers should be usable from the normal world). */
    oldPtbAsid = Rd(Dev->Mc, MC_SMMU_PTB_ASID);
    Trace(L"SmmuPtbAsidWas", oldPtbAsid);
    Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
    Trace(L"SmmuPtbAsid0", Rd(Dev->Mc, MC_SMMU_PTB_ASID));
    oldPtb0 = Rd(Dev->Mc, MC_SMMU_PTB_DATA);
    TraceDurable(L"SmmuPtbData0", oldPtb0);
    Trace(L"McIntStatusBefore", Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"McErrStatusBefore", Rd(Dev->Mc, MC_ERR_STATUS));
    Trace(L"McErrAdrBefore", Rd(Dev->Mc, MC_ERR_ADR));
    /* 0.3.2 showed an old latched error (int 0x200); the error registers only
     * latch a new error after the status is cleared (write 1 to clear). */
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"McIntStatusCleared", Rd(Dev->Mc, MC_INTSTATUS));
    /* 0.3.9: does int 0x200 (ARBITRATION_EMEM) come back by itself? Wait
     * 10 ms with no GPU DMA, read it, then clear it again for the DMA. */
    Ms(10);
    Trace(L"McIntStatusIdle10ms", Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"McErrStatusIdle10ms", Rd(Dev->Mc, MC_ERR_STATUS));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    /* MC hot reset status bit 2 = the GPU client has no outstanding requests. */
    Trace(L"McHotStatBeforeDma", Rd(Dev->Mc, MC_HOTRESET_STAT_1));

    lo.QuadPart = 0; hi.QuadPart = 0xFFFFFFFFll; none.QuadPart = 0;
    buf = (volatile ULONG *)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, none, MmNonCached);
    if (buf == NULL) {
        if (asidChanged) Wr(Dev->Mc, MC_SMMU_GPU_ASID, asid);
        TraceDurable(L"B2aResult", 0xE0000031u);
        return;
    }
    pa = MmGetPhysicalAddress((PVOID)buf);
    Trace(L"B2aBufPhys", pa.LowPart);
    base = pa.LowPart & ~0x3FFFFFu;
    inst = (volatile ULONG *)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, none, MmNonCached);
    gpuPd = (volatile ULONG *)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, none, MmNonCached);
    if (inst != NULL && gpuPd != NULL) {
        instPa = MmGetPhysicalAddress((PVOID)inst);
        gpuPdPa = MmGetPhysicalAddress((PVOID)gpuPd);
        RtlZeroMemory((PVOID)inst, PAGE_SIZE);
        RtlZeroMemory((PVOID)gpuPd, PAGE_SIZE);
        /* gf100_vmm_join(): the instance block carries the page-directory
         * target/address at 0x200 and the VA limit at 0x208. B2a keeps using
         * physical DMA, so the GPU page directory is intentionally empty. */
        inst[0x200 / sizeof(ULONG)] = (gpuPdPa.LowPart & ~0xFFFu) | 3u; /* NCOH */
        inst[0x204 / sizeof(ULONG)] = 0;
        inst[0x208 / sizeof(ULONG)] = 0xFFFFFFFFu;
        inst[0x20C / sizeof(ULONG)] = 0;
        KeMemoryBarrier();
        Trace(L"B2aInstPhys", instPa.LowPart);
        Trace(L"B2aGpuPdPhys", gpuPdPa.LowPart);
        Trace(L"B2aInstPdbLo", inst[0x200 / sizeof(ULONG)]);
        Trace(L"B2aInstLimitLo", inst[0x208 / sizeof(ULONG)]);
        /* If the Tegra SMMU still translates GPU accesses, all GPU-fetched
         * objects must fit inside its single 4 MiB identity-mapped window. */
        bindReady = !stillTranslated ||
            ((instPa.LowPart & ~0x3FFFFFu) == base && (gpuPdPa.LowPart & ~0x3FFFFFu) == base);
        TraceDurable(L"B2aInstMapReady", bindReady ? 1 : 0);
    } else {
        bindReady = FALSE;
        TraceDurable(L"B2aInstAlloc", 0);
    }

    /* 0.3.3 on hardware: the GPU stays in ASID 0, whose page directory points
     * outside RAM (PTB_DATA 0x4020608C), but the PTB registers are writable.
     * So give ASID 0 a small identity map: one page directory and one page
     * table mapping the 4 MB around the buffer to itself (tegra-smmu format),
     * flush the SMMU caches, and put the old value back afterwards. */
    if (stillTranslated) {
        pd = (volatile ULONG *)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, none, MmNonCached);
        pt = (volatile ULONG *)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, none, MmNonCached);
        if (pd != NULL && pt != NULL) {
            pdPa = MmGetPhysicalAddress((PVOID)pd);
            ptPa = MmGetPhysicalAddress((PVOID)pt);
            for (i = 0; i < 1024; ++i) pd[i] = 0;
            for (i = 0; i < 1024; ++i) pt[i] = ((base >> 12) + i) | SMMU_ATTR_RWNS;
            pd[base >> 22] = (ULONG)(ptPa.QuadPart >> 12) | SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
            KeMemoryBarrier();
            Trace(L"SmmuPdPhys", pdPa.LowPart);
            Trace(L"SmmuPtPhys", ptPa.LowPart);
            Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
            Wr(Dev->Mc, MC_SMMU_PTB_DATA, (ULONG)(pdPa.QuadPart >> 12) | SMMU_ATTR_RWNS);
            Trace(L"SmmuPtbDataNew", Rd(Dev->Mc, MC_SMMU_PTB_DATA));
            Wr(Dev->Mc, MC_SMMU_PTC_FLUSH, 0);
            (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
            Wr(Dev->Mc, MC_SMMU_TLB_FLUSH, 0);
            (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
            ptbChanged = TRUE;
            TraceDurable(L"SmmuIdentityMap", 1);
        } else {
            bindReady = FALSE;
            TraceDurable(L"B2aInstMapReady", 0);
        }
    }
    for (i = 0; i < 64; ++i) buf[i] = 0xC0DE0000u + i * 0x01010101u;     /* source for memory -> DMEM */
    for (i = 64; i < 1024; ++i) buf[i] = 0xEEEEEEEEu;                      /* 0x100..0x1FF: DMEM -> memory target */
    KeMemoryBarrier();

    /* 0.3.15: nouveau's FB init for GM20B (gm200_fb_init and
     * gf100_fb_sysmem_flush_page_init): sysmem flush page at 0x100C10 and the
     * MMU debug write/read buffers at 0x100CC8/0x100CCC (address >> 8), size
     * field 0x100CC4 bits 18..17 = min(size >> 16, 2) = 0 for 4 KB pages.
     * 0.3.14 showed all of them 0. Pages come from inside the SMMU identity
     * window when the GPU is still translated. Put back in step 34; the pages
     * are kept until after the GPU reset. */
    {
        PHYSICAL_ADDRESS fl = lo, fh = hi, fpa[3];
        ULONG j, inMap = 1;
        if (stillTranslated) { fl.QuadPart = base; fh.QuadPart = (LONGLONG)base + 0x3FFFFF; }
        for (j = 0; j < 3; ++j)
            fbPg[j] = (volatile ULONG *)MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, fl, fh, none, MmNonCached);
        if (fbPg[0] != NULL && fbPg[1] != NULL && fbPg[2] != NULL) {
            for (j = 0; j < 3; ++j) {
                RtlZeroMemory((PVOID)fbPg[j], PAGE_SIZE);
                fpa[j] = MmGetPhysicalAddress((PVOID)fbPg[j]);
                if (stillTranslated && (fpa[j].LowPart & ~0x3FFFFFu) != base) inMap = 0;
            }
            KeMemoryBarrier();
            fbRegWas[0] = Rd(B, 0x100C10);
            fbRegWas[1] = Rd(B, 0x100CC8);
            fbRegWas[2] = Rd(B, 0x100CCC);
            fbRegWas[3] = Rd(B, 0x100CC4);
            fbTouched = TRUE;
            Wr(B, 0x100C10, (ULONG)(fpa[0].QuadPart >> 8));
            Wr(B, 0x100CC8, (ULONG)(fpa[1].QuadPart >> 8));
            Wr(B, 0x100CCC, (ULONG)(fpa[2].QuadPart >> 8));
            Wr(B, 0x100CC4, fbRegWas[3] & ~0x00060000u);
            Trace(L"B2aFbFlushPhys", fpa[0].LowPart);
            Trace(L"B2aFbDbgWrPhys", fpa[1].LowPart);
            Trace(L"B2aFbDbgRdPhys", fpa[2].LowPart);
            Trace(L"B2aFbInMap", inMap);
            Trace(L"B2aFbFlushNow", Rd(B, 0x100C10));
            Trace(L"B2aFbDbgWrNow", Rd(B, 0x100CC8));
            Trace(L"B2aFbDbgRdNow", Rd(B, 0x100CCC));
            TraceDurable(L"B2aFbDbgCtlNow", Rd(B, 0x100CC4));
        } else {
            TraceDurable(L"B2aFbAlloc", 0);
        }
    }
    /* 0.3.16: nvgpu gk20a_mm_fb_flush after the mm setup: flush_fb_flush
     * (0x070000) pending_busy, then poll outstanding/pending (bits 1, 0)
     * clear, 5 us steps, 100 tries. */
    {
        ULONG w;
        Wr(B, 0x070000, 1u);
        for (w = 0; w < 100 && (Rd(B, 0x070000) & 3u); ++w) Us(5);
        Trace(L"B2aFbFlushPolls", w);
        TraceDurable(L"B2aFbFlushReg", Rd(B, 0x070000));
    }

    /* 0.3.12 (read, then clear): 0.3.11 saw int 0x200 once, somewhere between
     * the idle check and the end of the first bind. Split that window. */
    Trace(L"McIntBeforeStep35", Rd(Dev->Mc, MC_INTSTATUS));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    Step(35);   /* 0.3.4 on hardware: still no DMA with an identity map, and
                 * PMC_ENABLE (0xC0012020) has none of XBAR, L2, PFB, HUB on:
                 * reset them as nvgpu does (off, wait, on). */
    /* 0.3.5 on hardware: writing those bits changed nothing (0xC0012020 stays):
     * GM20B has no enable bits for them (nouveau's gk104 reset table has only
     * FIFO and PMU). So only record PMC_ENABLE and the PMU FBIF window. */
    now = Rd(B, NV_PMC_ENABLE);
    Trace(L"PmcEnableBeforeMem", now);
    Trace(L"PmcEnableAfterMem", now);
    {
        ULONG fb[12];
        for (i = 0; i < 12; ++i) fb[i] = Rd(B, PMU_FALCON + 0xE00 + i * 4);
        TraceBin(L"PmuFbifWindow", fb, sizeof(fb));
    }

    Trace(L"McIntBeforeSetup", Rd(Dev->Mc, MC_INTSTATUS));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    Step(31);   /* physical DMA setup */
    tc = Rd(B, PMU_FALCON + PMU_FBIF_TRANSCFG(DMA_IDX));
    ctl = Rd(B, PMU_FALCON + PMU_FBIF_CTL);
    dmactl = Rd(B, PMU_FALCON + FALCON_DMACTL);
    Trace(L"B2aTranscfgWas", tc);
    Trace(L"B2aFbifCtlWas", ctl);
    Trace(L"B2aDmactlWas", dmactl);
    Wr(B, PMU_FALCON + PMU_FBIF_TRANSCFG(DMA_IDX), FBIF_PHYS_NCOH);
    Wr(B, PMU_FALCON + PMU_FBIF_CTL, ctl | 0x80u);
    Wr(B, PMU_FALCON + FALCON_DMACTL, 0);
    Trace(L"B2aTranscfg", Rd(B, PMU_FALCON + PMU_FBIF_TRANSCFG(DMA_IDX)));
    Trace(L"B2aFbifCtl", Rd(B, PMU_FALCON + PMU_FBIF_CTL));
    TraceDurable(L"B2aSetup", 1);

    Trace(L"McIntBeforeBind", Rd(Dev->Mc, MC_INTSTATUS));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    Step(39);   /* bind an instance block to the PMU before the DMA attempts */
    if (bindReady) {
        static const ULONG fbifOffsets[5] = { 0xE00, 0xE04, 0xE08, 0xE0C, 0xE10 };
        instPmuWas = Rd(B, PMU_FALCON + FALCON_BIND_INST);
        ctxEnableWas = Rd(B, PMU_FALCON + FALCON_CTX_ENABLE);
        itfenWas = Rd(B, PMU_FALCON + FALCON_ITFEN);
        itfenTouched = TRUE;
        for (i = 0; i < 5; ++i) fbifWas[i] = Rd(B, PMU_FALCON + fbifOffsets[i]);
        Trace(L"B2aBindInstWas", instPmuWas);
        Trace(L"B2aBindCtxEnableWas", ctxEnableWas);
        Trace(L"B2aItfenWas", itfenWas);
        TraceBin(L"B2aBindFbifWas", fbifWas, sizeof(fbifWas));

        /* Match gm200_pmu_flcn_bind_inst(): set the standard DMA indices,
         * enable the context and bind NCOH system memory at 0x480. */
        bindTouched = TRUE;
        bindReady = PmuBindInst(B, instPa.LowPart, &bindState, &bindDoneState,
                                &bindPoll, &bindDonePolls);
        Trace(L"B2aBindFbifNow0", Rd(B, PMU_FALCON + fbifOffsets[0]));
        Trace(L"B2aBindFbifNow1", Rd(B, PMU_FALCON + fbifOffsets[1]));
        Trace(L"B2aBindFbifNow2", Rd(B, PMU_FALCON + fbifOffsets[2]));
        Trace(L"B2aBindFbifNow3", Rd(B, PMU_FALCON + fbifOffsets[3]));
        Trace(L"B2aBindFbifNow4", Rd(B, PMU_FALCON + fbifOffsets[4]));
        Trace(L"B2aBindInstNow", Rd(B, PMU_FALCON + FALCON_BIND_INST));
        Trace(L"B2aBindCtxEnableNow", Rd(B, PMU_FALCON + FALCON_CTX_ENABLE));
        Trace(L"B2aBindAckState", bindState);
        Trace(L"B2aBindAckPolls", bindPoll);
        Trace(L"B2aBindDoneState", bindDoneState);
        Trace(L"B2aBindDonePolls", bindDonePolls);
        Trace(L"B2aItfenForBind", Rd(B, PMU_FALCON + FALCON_ITFEN));
        if (bindReady) Wr(B, PMU_FALCON + FALCON_DMACTL, dmactl);
        TraceDurable(L"B2aBindResult", bindReady ? 1 : 0xE0000034u);
    } else {
        TraceDurable(L"B2aBindResult", 0xE0000035u);
    }
    /* 0.3.11 (read only, then clear): did the bind step bring int 0x200 back? */
    Trace(L"McIntAfterBind", Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"McErrAfterBind", Rd(Dev->Mc, MC_ERR_STATUS));
    Trace(L"McErrAdrAfterBind", Rd(Dev->Mc, MC_ERR_ADR));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    DumpGpuMem(B, L"B2aGpuMemBefore");
    /* 0.3.17 (read only): PFIFO MMU fault state before the first try. */
    Trace(L"B2aFifoIntr0BeforeTries", Rd(B, 0x2100));
    Trace(L"B2aFifoFaultIdBeforeTries", Rd(B, 0x259C));

    {
        /* 0.3.6: try the FBIF target noncoherent sysmem (6), then local FB (4),
         * then coherent sysmem (5), resetting the PMU falcon (PMC_ENABLE bit 13
         * off/on, wait for the scrub) before each retry. After each attempt,
         * record the falcon interrupt status (bit 5 = EXTERR, a bus error) and
         * the PFIFO MMU fault registers. */
        static const ULONG targets[3] = { 6, 4, 5 };
        static const PCWSTR names[3][4] = {
            { L"B2aV1Polls", L"B2aV1Cmd", L"B2aV1Irq", L"B2aV1Ok" },
            { L"B2aV2Polls", L"B2aV2Cmd", L"B2aV2Irq", L"B2aV2Ok" },
            { L"B2aV3Polls", L"B2aV3Cmd", L"B2aV3Irq", L"B2aV3Ok" } };
        ULONG v, k, w;
        ok = 0;
        for (k = 0; bindReady && k < 3 && ok != 64; ++k) {
            Step(36 + k);
            if (k > 0) {
                ULONG en = Rd(B, NV_PMC_ENABLE);
                Wr(B, NV_PMC_ENABLE, en & ~(1u << 13));
                (VOID)Rd(B, NV_PMC_ENABLE);
                Us(20);
                Wr(B, NV_PMC_ENABLE, en | (1u << 13));
                (VOID)Rd(B, NV_PMC_ENABLE);
                for (w = 0; w < 2000 && (Rd(B, PMU_FALCON + FALCON_DMACTL) & 6u); ++w) Us(5);
                Wr(B, PMU_FALCON + PMU_FBIF_CTL, Rd(B, PMU_FALCON + PMU_FBIF_CTL) | 0x80u);
                bindReady = PmuBindInst(B, instPa.LowPart, &bindState, &bindDoneState,
                                        &bindPoll, &bindDonePolls);
                Trace(L"B2aRebindAckState", bindState);
                Trace(L"B2aRebindAckPolls", bindPoll);
                Trace(L"B2aRebindDoneState", bindDoneState);
                Trace(L"B2aRebindDonePolls", bindDonePolls);
                TraceDurable(L"B2aRebindResult", bindReady ? 1 : 0xE0000036u);
                if (!bindReady) break;
                Wr(B, PMU_FALCON + FALCON_DMACTL, dmactl);
            }
            Wr(B, PMU_FALCON + PMU_FBIF_TRANSCFG(DMA_IDX), targets[k]);
            /* 0.3.6 on hardware: no transfer started with any target and no
             * bus error: the falcon's external interface (ITFEN bit 0) may be
             * off after reset; nouveau's gm200_flcn_fw_load sets it first. */
            if (k == 0) Trace(L"B2aItfen", Rd(B, PMU_FALCON + FALCON_ITFEN));
            Wr(B, PMU_FALCON + FALCON_ITFEN, Rd(B, PMU_FALCON + FALCON_ITFEN) | 1u);
            if (k == 0) {
                Trace(L"B2aItfen", Rd(B, PMU_FALCON + FALCON_ITFEN));
                Trace(L"B2aDmactlNow", Rd(B, PMU_FALCON + FALCON_DMACTL));
                Trace(L"B2aTranscfgNow", Rd(B, PMU_FALCON + PMU_FBIF_TRANSCFG(DMA_IDX)));
            }
            Wr(B, PMU_FALCON + FALCON_DMEMC0, 0x100u | (1u << 24));
            for (i = 0; i < 64; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, 0);
            Wr(B, PMU_FALCON + 0x004, 0xFFFFFFFFu);        /* clear falcon interrupts */
            /* 0.3.11: MC int just before this try (for tries 2 and 3 this
             * covers the falcon reset and rebind), then clear it. */
            {
                static const PCWSTR mcBefore[3] = { L"McIntBeforeTry1", L"McIntBeforeTry2", L"McIntBeforeTry3" };
                Trace(mcBefore[k], Rd(Dev->Mc, MC_INTSTATUS));
                Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
            }
            Wr(B, PMU_FALCON + FALCON_DMATRFBASE, (ULONG)(pa.QuadPart >> 8));
            Wr(B, PMU_FALCON + FALCON_DMATRFMOFFS, 0x100);
            Wr(B, PMU_FALCON + FALCON_DMATRFFBOFFS, 0);
            Wr(B, PMU_FALCON + FALCON_DMATRFCMD, DMATRFCMD_256B | (DMA_IDX << 12));
            polls = DmaWait(B);
            {
                static const PCWSTR mcAfter[3][2] = {
                    { L"McIntAfterTry1", L"McErrAfterTry1" },
                    { L"McIntAfterTry2", L"McErrAfterTry2" },
                    { L"McIntAfterTry3", L"McErrAfterTry3" } };
                static const PCWSTR fifoAfter[3][2] = {
                    { L"B2aFifoIntr0AfterTry1", L"B2aFifoFaultIdAfterTry1" },
                    { L"B2aFifoIntr0AfterTry2", L"B2aFifoFaultIdAfterTry2" },
                    { L"B2aFifoIntr0AfterTry3", L"B2aFifoFaultIdAfterTry3" } };
                Trace(mcAfter[k][0], Rd(Dev->Mc, MC_INTSTATUS));
                Trace(mcAfter[k][1], Rd(Dev->Mc, MC_ERR_STATUS));
                Trace(fifoAfter[k][0], Rd(B, 0x2100));
                Trace(fifoAfter[k][1], Rd(B, 0x259C));
            }
            Trace(names[k][0], polls);
            Trace(names[k][1], Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
            Trace(names[k][2], Rd(B, PMU_FALCON + 0x008));
            Wr(B, PMU_FALCON + FALCON_DMEMC0, 0x100u | (1u << 25));
            ok = 0;
            for (i = 0; i < 64; ++i) {
                v = Rd(B, PMU_FALCON + FALCON_DMEMD0);
                if (i == 0) Trace(L"B2aInWord0", v);
                if (i == 63) Trace(L"B2aInWord63", v);
                if (v == 0xC0DE0000u + i * 0x01010101u) ++ok;
            }
            TraceDurable(names[k][3], ok);
            Trace(L"B2aInPolls", polls);
            Trace(L"B2aInCmd", Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
            idle = (polls != 0xFFFFFFFFu);
        }
        Trace(L"B2aGoodTarget", ok == 64 ? targets[k - 1] : 0);
        DumpGpuMem(B, L"B2aGpuMemAfter");
        /* 0.3.17 (read only): the MMU fault record of every unit whose bit is
         * set in the fault id register, plus unit 0x17 (PMU in nouveau's
         * gk104 table) always: inst, va lo, va hi, info at 0x2800 + unit * 0x10
         * (nouveau gk104_fifo_intr_mmu_fault_unit). Up to 6 records. */
        {
            ULONG rec[6][5], n = 0, id = Rd(B, 0x259C), u;
            for (u = 0; u < 32 && n < 6; ++u) {
                if (!(id & (1u << u)) && u != 0x17) continue;
                rec[n][0] = u;
                rec[n][1] = Rd(B, 0x2800 + u * 0x10);
                rec[n][2] = Rd(B, 0x2804 + u * 0x10);
                rec[n][3] = Rd(B, 0x2808 + u * 0x10);
                rec[n][4] = Rd(B, 0x280C + u * 0x10);
                ++n;
            }
            if (n) TraceBin(L"B2aFaultRecs", rec, n * 5 * sizeof(ULONG));
        }
        Trace(L"PfifoIntr0", Rd(B, 0x2100));
        Trace(L"PfifoMmuFaultId", Rd(B, 0x259C));
        Trace(L"PfifoFault0Inst", Rd(B, 0x2800));
        Trace(L"PfifoFault0Lo", Rd(B, 0x2804));
        Trace(L"PfifoFault0Hi", Rd(B, 0x2808));
        Trace(L"PfifoFault0Info", Rd(B, 0x280C));
        TraceDurable(L"B2aInOk", ok);
    }
    Trace(L"McIntStatusAfterIn", Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"McErrStatusAfterIn", Rd(Dev->Mc, MC_ERR_STATUS));
    Trace(L"McErrAdrAfterIn", Rd(Dev->Mc, MC_ERR_ADR));
    /* 0.3.9: bit 2 clear here = a GPU memory request reached the MC and is
     * still outstanding; bit 2 set = nothing is pending at the MC. */
    TraceDurable(L"McHotStatAfterDma", Rd(Dev->Mc, MC_HOTRESET_STAT_1));

    /* Write into memory only when the read proved the address reaches our buffer. */
    if (idle && ok == 64) {
        Step(33);   /* DMEM 0x200 -> memory + 0x100 */
        Wr(B, PMU_FALCON + FALCON_DMEMC0, 0x200u | (1u << 24));
        for (i = 0; i < 64; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, 0x600D0000u + i);
        Wr(B, PMU_FALCON + FALCON_DMATRFMOFFS, 0x200);
        Wr(B, PMU_FALCON + FALCON_DMATRFFBOFFS, 0x100);
        Wr(B, PMU_FALCON + FALCON_DMATRFCMD, DMATRFCMD_WRITE | DMATRFCMD_256B | (DMA_IDX << 12));
        polls = DmaWait(B);
        Trace(L"B2aOutPolls", polls);
        Trace(L"B2aOutCmd", Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
        if (polls == 0xFFFFFFFFu) idle = FALSE;
        Us(100);
        KeMemoryBarrier();
        ok = 0; spill = 0;
        for (i = 0; i < 64; ++i) if (buf[64 + i] == 0x600D0000u + i) ++ok;
        for (i = 128; i < 1024; ++i) if (buf[i] != 0xEEEEEEEEu) ++spill;
        for (i = 0; i < 64; ++i) if (buf[i] != 0xC0DE0000u + i * 0x01010101u) ++spill;
        Trace(L"B2aOutWord0", buf[64]);
        Trace(L"B2aOutSpill", spill);
        TraceDurable(L"B2aOutOk", ok);
    }

    Step(34);   /* put the DMA setup back, clear DMEM */
    if (bindTouched) {
        static const ULONG fbifOffsets[5] = { 0xE00, 0xE04, 0xE08, 0xE0C, 0xE10 };
        /* Restore configuration. The GPU reset in PowerTest releases the
         * active context; do not replay the action register at 0x058. */
        Wr(B, PMU_FALCON + FALCON_CTX_ENABLE, ctxEnableWas);
        for (i = 0; i < 5; ++i) Wr(B, PMU_FALCON + fbifOffsets[i], fbifWas[i]);
        Trace(L"B2aBindCtxEnableRestored", Rd(B, PMU_FALCON + FALCON_CTX_ENABLE));
        Trace(L"B2aBindInstActive", Rd(B, PMU_FALCON + FALCON_BIND_INST));
    }
    if (fifoTouched) {
        Wr(B, NV_PMC_ENABLE, Rd(B, NV_PMC_ENABLE) & ~0x100u);
        Trace(L"B2aFifoPmcRestored", Rd(B, NV_PMC_ENABLE));
    }
    if (fbTouched) {
        Wr(B, 0x100C10, fbRegWas[0]);
        Wr(B, 0x100CC8, fbRegWas[1]);
        Wr(B, 0x100CCC, fbRegWas[2]);
        Wr(B, 0x100CC4, fbRegWas[3]);
        Trace(L"B2aFbFlushRestored", Rd(B, 0x100C10));
    }
    /* Keep the FB pages until the GPU reset (the GPU may still hold them). */
    RetainB2aPages((PVOID)fbPg[0], (PVOID)fbPg[1]);
    RetainB2aPages((PVOID)fbPg[2], NULL);
    Wr(B, PMU_FALCON + PMU_FBIF_TRANSCFG(DMA_IDX), tc);
    Wr(B, PMU_FALCON + PMU_FBIF_CTL, ctl);
    Wr(B, PMU_FALCON + FALCON_DMACTL, dmactl);
    if (itfenTouched) Wr(B, PMU_FALCON + FALCON_ITFEN, itfenWas);
    Wr(B, PMU_FALCON + FALCON_DMEMC0, 0x100u | (1u << 24));
    for (i = 0; i < 128; ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, 0);
    /* A DMA that never went idle might still write: keep (leak) the buffer then. */
    if (ptbChanged) {
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
        Wr(Dev->Mc, MC_SMMU_PTB_DATA, oldPtb0);
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, oldPtbAsid);
        Wr(Dev->Mc, MC_SMMU_PTC_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_TLB_FLUSH, 0);
        (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, 0);
        Trace(L"SmmuPtbDataRestored", Rd(Dev->Mc, MC_SMMU_PTB_DATA));
        Wr(Dev->Mc, MC_SMMU_PTB_ASID, oldPtbAsid);
    }
    if (idle) {
        if (pd != NULL) MmFreeContiguousMemorySpecifyCache((PVOID)pd, PAGE_SIZE, MmNonCached);
        if (pt != NULL) MmFreeContiguousMemorySpecifyCache((PVOID)pt, PAGE_SIZE, MmNonCached);
        if (!bindTouched) {
            if (inst != NULL) MmFreeContiguousMemorySpecifyCache((PVOID)inst, PAGE_SIZE, MmNonCached);
            if (gpuPd != NULL) MmFreeContiguousMemorySpecifyCache((PVOID)gpuPd, PAGE_SIZE, MmNonCached);
        } else {
            RetainB2aPages((PVOID)inst, (PVOID)gpuPd);
            inst = NULL;
            gpuPd = NULL;
        }
    } else if (bindTouched) {
        RetainB2aPages((PVOID)inst, (PVOID)gpuPd);
        inst = NULL;
        gpuPd = NULL;
    }
    if (asidChanged) {
        Wr(Dev->Mc, MC_SMMU_GPU_ASID, asid);
        Trace(L"SmmuGpuAsidRestored", Rd(Dev->Mc, MC_SMMU_GPU_ASID));
    }
    if (idle) MmFreeContiguousMemorySpecifyCache((PVOID)buf, PAGE_SIZE, MmNonCached);
    TraceDurable(L"B2aResult", !idle ? 0xE0000032u : (ok == 64 ? 0 : 0xE0000033u));
}

/* ------------------------------------------------------------ power test */

/* ------------------------------------------------------ stage B3a probe */

/* 0.4.0: stage B3a. Read the ACR HS firmware that the kit copied from the
 * user's own linux-firmware (nvidia/gm20b/acr/bl.bin, acr/ucode_load.bin)
 * into the registry, check every header field against the blob sizes, build
 * the signed image in memory (nouveau nvkm_falcon_fw_ctor_hs and
 * nvkm_falcon_fw_patch), load the HS bootloader into the top of PMU IMEM by
 * PIO (gm200_flcn_fw_load / gm200_flcn_pio_imem_wr) and the bootloader DMEM
 * descriptor (flcn_bl_dmem_desc v0, gm20b_acr_hsfw_load_bld) at DMEM 0, and
 * read both back. The falcon CPU is NOT started: the bootloader would DMA the
 * image through a GPU virtual address (ctx_dma VIRT), which needs the PMU
 * address space first. The DMA bases in the descriptor stay 0 here. */
#define FALCON_BOOTVEC     0x104
#define FALCON_IMEMC0      0x180       /* BIT24 auto-increment write, BIT25 read, BIT28 secure */
#define FALCON_IMEMD0      0x184
#define FALCON_IMEMT0      0x188       /* tag of the next 256-byte block */
#define ACR_BL_MAX         0x2000u
#define ACR_UC_MAX         0x20000u
#define ACR_DMAIDX_VIRT    1u          /* nvgpu GK20A_PMU_DMAIDX_VIRT */
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
    if (SwGpuParamsPath[0] == L'\0') return NULL;
    RtlInitUnicodeString(&path, SwGpuParamsPath);
    InitializeObjectAttributes(&oa, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&key, KEY_READ, &oa))) return NULL;
    RtlInitUnicodeString(&name, Name);
    st = ZwQueryValueKey(key, &name, KeyValuePartialInformation, NULL, 0, &need);
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

/* Build a CPU-only GM20B WPR image from the staged Linux firmware blobs.
 * This deliberately does not map the buffer for GPU DMA, alter SMMU state,
 * enable the PMU, or ask ACR to consume it. The WPR addresses in both BLDs
 * are still relative to the beginning of the image and need the WPR-base
 * relocation performed by ACR before they can be used. */
static ULONG BuildWprB3b(ULONG PmuDmemSize)
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

    Step(46); /* parse firmware descriptors and construct the CPU-side WPR image */
    pmuDesc = ReadBinParam(L"FwPmuDesc", 0x20000u, &pmuDescLen);
    pmuImage = ReadBinParam(L"FwPmuImage", 0x20000u, &pmuImageLen);
    pmuSig = ReadBinParam(L"FwPmuSig", 0x20000u, &pmuSigLen);
    fecsBl = ReadBinParam(L"FwFecsBl", 0x20000u, &fecsBlLen);
    fecsInst = ReadBinParam(L"FwFecsInst", 0x20000u, &fecsInstLen);
    fecsData = ReadBinParam(L"FwFecsData", 0x20000u, &fecsDataLen);
    fecsSig = ReadBinParam(L"FwFecsSig", 0x20000u, &fecsSigLen);
    Trace(L"B3bPmuDescLen", pmuDescLen);
    Trace(L"B3bPmuImageLen", pmuImageLen);
    Trace(L"B3bPmuSigLen", pmuSigLen);
    Trace(L"B3bFecsBlLen", fecsBlLen);
    Trace(L"B3bFecsInstLen", fecsInstLen);
    Trace(L"B3bFecsDataLen", fecsDataLen);
    Trace(L"B3bFecsSigLen", fecsSigLen);
    /* Keep the 0.4.1 kit's existing SUMMARY field names usable. */
    Trace(L"B3aFwPmuDescLen", pmuDescLen);
    Trace(L"B3aFwPmuImageLen", pmuImageLen);
    Trace(L"B3aFwPmuSigLen", pmuSigLen);
    Trace(L"B3aFwFecsBlLen", fecsBlLen);
    Trace(L"B3aFwFecsInstLen", fecsInstLen);
    Trace(L"B3aFwFecsDataLen", fecsDataLen);
    Trace(L"B3aFwFecsSigLen", fecsSigLen);
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
    Trace(L"B3bPmuDescSize", descSize);
    Trace(L"B3bPmuImageSize", imageSize);
    Trace(L"B3bPmuBootSize", pmuBootSize);
    Trace(L"B3bPmuAppSize", pmuAppSize);
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
    Trace(L"B3bFecsStartTag", fecsStartTag);
    Trace(L"B3bFecsCodeOff", fecsCodeOff);
    Trace(L"B3bFecsCodeSize", fecsCodeSize);
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
    Trace(L"B3bPmuLsbOff", pmuLsbOff);
    Trace(L"B3bPmuImgOff", pmuImgOff);
    Trace(L"B3bPmuBldOff", pmuBldOff);
    Trace(L"B3bFecsLsbOff", fecsLsbOff);
    Trace(L"B3bFecsImgOff", fecsImgOff);
    Trace(L"B3bFecsBldOff", fecsBldOff);
    Trace(L"B3bWprUsed", totalUsed);
    Trace(L"B3bWprAlloc", allocLen);

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
    pmuBld[0] = DMA_IDX;
    pmuBld[1] = (pmuImgOff + pmuCodeOffset) >> 8;
    pmuBld[2] = pmuAppSize;
    pmuBld[3] = pmuAppCodeSize;
    pmuBld[4] = Le32(pmuDesc + 108); /* app_imem_entry */
    pmuBld[5] = (pmuImgOff + pmuDataOffset) >> 8;
    pmuBld[6] = pmuAppDataSize;
    pmuBld[7] = pmuBld[1];
    pmuBld[8] = 1;
    pmuBld[9] = PmuDmemSize - 44u; /* sizeof(struct nv_pmu_args) */
    for (i = 0; i < RTL_NUMBER_OF(pmuBld); ++i)
        WprPutLe32(wpr + pmuBldOff + i * 4u, pmuBld[i]);

    RtlZeroMemory(fecsBld, sizeof(fecsBld));
    fecsBld[8] = DMA_IDX;
    fecsBld[9] = (fecsImgOff + fecsBootPad) >> 8;
    fecsBld[10] = 0;
    fecsBld[11] = fecsInstPad;
    fecsBld[14] = 0;
    fecsBld[15] = (fecsImgOff + fecsBootPad + fecsInstPad) >> 8;
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
          Le32(wpr + pmuBldOff) == DMA_IDX && Le32(wpr + fecsBldOff + 32) == DMA_IDX &&
          RtlCompareMemory(wpr + pmuImgOff, pmuImage, imageSize) == imageSize &&
          RtlCompareMemory(wpr + fecsImgOff, fecsImage, fecsImageLen) == fecsImageLen) ? 1u : 0u;
    Trace(L"B3bPmuCodeDmaRel", pmuBld[1]);
    Trace(L"B3bPmuDataDmaRel", pmuBld[5]);
    Trace(L"B3bPmuArgv", pmuBld[9]);
    Trace(L"B3bFecsCodeDmaRel", fecsBld[9]);
    Trace(L"B3bFecsDataDmaRel", fecsBld[15]);
    TraceDurable(L"B3bWprCpuReadback", ok);
    result = ok ? 0 : 0xE0000060u;

done:
    TraceDurable(L"B3bResult", result);
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

static VOID ProbeB3a(PSWGPU_DEVICE Dev)
{
    volatile UCHAR *B = Dev->Bar0;
    PUCHAR bl = NULL, uc = NULL, img = NULL, boot = NULL;
    ULONG blLen = 0, ucLen = 0, i, ok, err = 0, sum;
    ULONG blMagic, blHdrOff, blDataOff, blDataSize, startTag, dmemLoadOff, codeOff, codeSize, bootSize;
    ULONG ucMagic, ucHdrOff, ucDataOff, ucDataSize;
    ULONG sigDbgOff, sigDbgSize, sigPrdOff, sigPrdSize, patchLoc, patchSig, ldOff, loc, sig;
    ULONG nsOff, nsSize, dataBase, dataSize, numApps, app0Off, app0Size, sigSrc;
    ULONG hwcfg, imemSize, dmemSize, imemBase, dbgFused;
    ULONG desc[19];

    Step(40);   /* firmware blobs from the registry */
    bl = ReadBinParam(L"FwAcrBl", ACR_BL_MAX, &blLen);
    uc = ReadBinParam(L"FwAcrUcodeLoad", ACR_UC_MAX, &ucLen);
    Trace(L"B3aBlLen", blLen);
    Trace(L"B3aUcLen", ucLen);
    if (bl == NULL || uc == NULL) { err = 0xE0000041u; goto out; }

    Step(41);   /* acr/bl.bin: nvfw_bin_hdr (24 bytes) + nvfw_bl_desc (24 bytes) */
    if (blLen < 24) { err = 0xE0000042u; goto out; }
    blMagic = Le32(bl);
    blHdrOff = Le32(bl + 12);
    blDataOff = Le32(bl + 16);
    blDataSize = Le32(bl + 20);
    Trace(L"B3aBlMagic", blMagic);
    Trace(L"B3aBlHdrOff", blHdrOff);
    Trace(L"B3aBlDataOff", blDataOff);
    Trace(L"B3aBlDataSize", blDataSize);
    if ((blMagic != 0x000010DEu && blMagic != 0x3B1D14F0u) ||
        !InBlob(blLen, blHdrOff, 24) || !InBlob(blLen, blDataOff, blDataSize)) { err = 0xE0000043u; goto out; }
    startTag = Le32(bl + blHdrOff);
    dmemLoadOff = Le32(bl + blHdrOff + 4);
    codeOff = Le32(bl + blHdrOff + 8);
    codeSize = Le32(bl + blHdrOff + 12);
    Trace(L"B3aBlStartTag", startTag);
    Trace(L"B3aBlDmemLoadOff", dmemLoadOff);
    Trace(L"B3aBlCodeOff", codeOff);
    Trace(L"B3aBlCodeSize", codeSize);
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
    Trace(L"B3aUcMagic", ucMagic);
    Trace(L"B3aUcHdrOff", ucHdrOff);
    Trace(L"B3aUcDataOff", ucDataOff);
    Trace(L"B3aUcDataSize", ucDataSize);
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
    Trace(L"B3aSigDbgOff", sigDbgOff);
    Trace(L"B3aSigDbgSize", sigDbgSize);
    Trace(L"B3aSigPrdOff", sigPrdOff);
    Trace(L"B3aSigPrdSize", sigPrdSize);
    Trace(L"B3aPatchLoc", patchLoc);
    Trace(L"B3aPatchSig", patchSig);
    Trace(L"B3aLoadHdrOff", ldOff);
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
    Trace(L"B3aLoc", loc);
    Trace(L"B3aSig", sig);
    if (!InBlob(ucLen, ldOff, 20)) { err = 0xE0000048u; goto out; }
    nsOff = Le32(uc + ldOff);
    nsSize = Le32(uc + ldOff + 4);
    dataBase = Le32(uc + ldOff + 8);
    dataSize = Le32(uc + ldOff + 12);
    numApps = Le32(uc + ldOff + 16);
    Trace(L"B3aNsOff", nsOff);
    Trace(L"B3aNsSize", nsSize);
    Trace(L"B3aDataBase", dataBase);
    Trace(L"B3aDataSize", dataSize);
    Trace(L"B3aNumApps", numApps);
    if (numApps == 0 || numApps > 8 || !InBlob(ucLen, ldOff + 20, numApps * 8)) { err = 0xE0000049u; goto out; }
    app0Off = Le32(uc + ldOff + 20);
    app0Size = Le32(uc + ldOff + 20 + numApps * 4);
    Trace(L"B3aApp0Off", app0Off);
    Trace(L"B3aApp0Size", app0Size);
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
    Trace(L"B3aDebugFused", dbgFused);
    sigSrc = (dbgFused ? sigDbgOff : sigPrdOff) + sig;
    img = (PUCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, ucDataSize, SWGPU_TAG);
    if (img == NULL) { err = 0xE000004Bu; goto out; }
    RtlCopyMemory(img, uc + ucDataOff, ucDataSize);
    RtlCopyMemory(img + loc, uc + sigSrc, sigPrdSize);
    Trace(L"B3aSigWord0", Le32(img + loc));
    Trace(L"B3aDmemSign", loc - dataBase);
    for (sum = 0, i = 0; i + 4 <= ucDataSize; i += 4) sum += Le32(img + i);
    Trace(L"B3aImgSum", sum);

    Step(44);   /* PMU reset, scrub, bootloader -> top of IMEM by PIO */
    PmuReset(B, L"B3aScrubPolls");
    hwcfg = Rd(B, PMU_FALCON + FALCON_HWCFG);
    imemSize = (hwcfg & 0x1FFu) << 8;
    dmemSize = ((hwcfg >> 9) & 0x1FFu) << 8;
    Trace(L"B3aImemSize", imemSize);
    Trace(L"B3aDmemSize", dmemSize);
    if (bootSize > imemSize || sizeof(desc) > dmemSize) { err = 0xE000004Cu; goto out; }
    imemBase = imemSize - bootSize;
    Trace(L"B3aImemBase", imemBase);
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
        if (i == 0) Trace(L"B3aImemWord0", v);
        if (v == Le32(boot + i)) ++ok;
    }
    Trace(L"B3aImemWords", bootSize / 4);
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
    Trace(L"B3aCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    Trace(L"B3aBootAddr", startTag << 8);

    /* CPU-only WPR layout and readback; no GPU DMA or ACR/PMU start. */
    (VOID)BuildWprB3b(dmemSize);

    /* Leave the PMU clean: reset it again (the CPU was never started). */
    PmuReset(B, L"B3aScrubPolls2");

out:
    TraceDurable(L"B3aResult", err);
    if (boot != NULL) ExFreePoolWithTag(boot, SWGPU_TAG);
    if (img != NULL) ExFreePoolWithTag(img, SWGPU_TAG);
    if (bl != NULL) ExFreePoolWithTag(bl, SWGPU_TAG);
    if (uc != NULL) ExFreePoolWithTag(uc, SWGPU_TAG);
}

static NTSTATUS PowerTest(PSWGPU_DEVICE Dev, BOOLEAN KeepOn, ULONG Mode)
{
    UCHAR vout0 = 0, dvs0 = 0, gpio0 = 0, v = 0;
    ULONG clkx0, clkh0, outc0, rg, clkx, rstx, boot0 = 0xDEADDEADu, t0 = 0, t1 = 0, th = 0, en = 0, i;
    BOOLEAN railOn = FALSE, clocksOn = FALSE, outcChanged = FALSE, fuseChanged = FALSE;
    NTSTATUS st;

    Step(1);    /* read what is there now */
    st = I2cRead(Dev->Vdd, MAX77621_VOUT, &vout0);
    if (NT_SUCCESS(st)) st = I2cRead(Dev->Vdd, MAX77621_VOUT_DVS, &dvs0);
    if (NT_SUCCESS(st)) st = I2cRead(Dev->Pmic, MAX77620_GPIO6, &gpio0);
    if (!NT_SUCCESS(st)) { Trace(L"Result", (ULONG)st); return st; }
    clkx0 = Rd(Dev->Car, CAR_CLK_OUT_ENB_X);
    clkh0 = Rd(Dev->Car, CAR_CLK_OUT_ENB_H);
    outc0 = Rd(Dev->Car, CAR_PLLP_OUTC);

    /* 0.3.11: start the run with a clean MC interrupt status (write 1 to
     * clear, as B2a already does), so the reads below show only new errors. */
    Trace(L"McIntBeforePowerUp", Rd(Dev->Mc, MC_INTSTATUS));
    Wr(Dev->Mc, MC_INTSTATUS, Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"McIntPowerUpCleared", Rd(Dev->Mc, MC_INTSTATUS));

    Step(2);    /* VDD_GPU 1.0 V, enable pin high */
    st = I2cWrite(Dev->Vdd, MAX77621_VOUT_DVS, VDD_GPU_1000MV);
    if (NT_SUCCESS(st)) st = I2cWrite(Dev->Vdd, MAX77621_VOUT, VDD_GPU_1000MV);
    if (NT_SUCCESS(st)) st = I2cWrite(Dev->Pmic, MAX77620_GPIO6, GPIO_PUSHPULL_HIGH);
    railOn = TRUE;      /* restore below even after a partial write */
    if (!NT_SUCCESS(st)) goto down;
    Ms(2);
    if (NT_SUCCESS(I2cRead(Dev->Pmic, MAX77620_GPIO6, &v))) Trace(L"Gpio6On", v);
    if (NT_SUCCESS(I2cRead(Dev->Vdd, MAX77621_VOUT, &v))) Trace(L"VoutOn", v);

    Step(3);    /* clocks */
    /* R410 test: the boot firmware leaves OUT5 enabled but undivided
     * (0x00030000 = 408 MHz); nvgpu runs the pwr clock at 204 MHz. */
    if ((outc0 & (PLLP_OUT5_RSTN | PLLP_OUT5_CLKEN | 0xFF000000u)) != (PLLP_OUT5_RSTN | PLLP_OUT5_CLKEN | PLLP_OUT5_204MHZ)) {
        Wr(Dev->Car, CAR_PLLP_OUTC, (outc0 & 0x0000FFFFu) | PLLP_OUT5_204MHZ | PLLP_OUT5_OVRRIDE |
                                    PLLP_OUT5_CLKEN | PLLP_OUT5_RSTN);
        outcChanged = TRUE;
    }
    if (!(clkh0 & CLK_H_FUSE)) { Wr(Dev->Car, CAR_CLK_ENB_H_SET, CLK_H_FUSE); fuseChanged = TRUE; }
    Wr(Dev->Car, CAR_CLK_ENB_X_SET, CLK_X_GPU | CLK_X_PLLG_REF);
    clocksOn = TRUE;
    Us(10);
    Trace(L"PllpOutcOn", Rd(Dev->Car, CAR_PLLP_OUTC));

    Step(4);    /* reset assert */
    Wr(Dev->Car, CAR_RST_DEV_X_SET, CLK_X_GPU);
    Us(10);

    Step(5);    /* clamp off */
    Wr(Dev->Pmc, PMC_GPU_RG_CNTRL, 0);
    (VOID)Rd(Dev->Pmc, PMC_GPU_RG_CNTRL);
    Us(10);
    Trace(L"McIntAfterClampOff", Rd(Dev->Mc, MC_INTSTATUS));

    Step(6);    /* reset deassert with the gpu clock briefly off (nvgpu) */
    Wr(Dev->Car, CAR_CLK_ENB_X_CLR, CLK_X_GPU);
    Wr(Dev->Car, CAR_RST_DEV_X_CLR, CLK_X_GPU);
    Wr(Dev->Car, CAR_CLK_ENB_X_SET, CLK_X_GPU);
    Us(10);
    /* 0.3.10 (read only): where does MC hot reset status bit 2 drop to 0?
     * 0.3.9 saw it 1 with the GPU off and 0 already before the B2a DMA. */
    Trace(L"McHotStatAfterReset", Rd(Dev->Mc, MC_HOTRESET_STAT_1));
    Trace(L"McIntAfterReset", Rd(Dev->Mc, MC_INTSTATUS));

    Step(7);    /* MC flush, flush done */
    Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) | MC_HOTRESET_GPU);
    for (i = 0; i < 500 && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(10);
    Trace(L"McFlushPolls", i);
    Us(10);
    Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_GPU);
    Us(10);
    Trace(L"McHotStatAfterFlush", Rd(Dev->Mc, MC_HOTRESET_STAT_1));

    rg = Rd(Dev->Pmc, PMC_GPU_RG_CNTRL);
    clkx = Rd(Dev->Car, CAR_CLK_OUT_ENB_X);
    rstx = Rd(Dev->Car, CAR_RST_DEVICES_X);
    Trace(L"RgCntrlOn", rg);
    Trace(L"ClkXOn", clkx);
    Trace(L"RstXOn", rstx);
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
        Trace(L"WprStart", Rd(Dev->Bar0, NV_PFB_MMU_WPR_INFO));
        Wr(Dev->Bar0, NV_PFB_MMU_WPR_INFO, 3);
        Trace(L"WprLimit", Rd(Dev->Bar0, NV_PFB_MMU_WPR_INFO));
    }
    Trace(L"PmcEnable", en);
    Trace(L"Ptimer0", t0);
    Trace(L"Ptimer1", t1);
    Trace(L"PtimerHi", th);
    Trace(L"McHotStatAfterBar0", Rd(Dev->Mc, MC_HOTRESET_STAT_1));
    Trace(L"McIntAfterBar0", Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"Chipset", (boot0 >> 20) & 0x1FF);
    Trace(L"ChipRev", boot0 & 0xFF);
    st = (((boot0 >> 20) & 0x1FF) == 0x12B) ? STATUS_SUCCESS : STATUS_DEVICE_HARDWARE_ERROR;
    Trace(L"Result", (ULONG)st);
    if (NT_SUCCESS(st) && Mode == 2) ProbeB1(Dev);
    if (NT_SUCCESS(st) && Mode == 3) ProbeB2a(Dev);
    if (NT_SUCCESS(st) && Mode == 4) ProbeB3a(Dev);
    if (KeepOn) { Step(100); return st; }

down:
    Step(10);   /* power down, put everything back */
    if (clocksOn) {
        Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) | MC_HOTRESET_GPU);
        for (i = 0; i < 500 && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(10);
        Us(10);
        Wr(Dev->Pmc, PMC_GPU_RG_CNTRL, 1);
        (VOID)Rd(Dev->Pmc, PMC_GPU_RG_CNTRL);
        Us(10);
        Wr(Dev->Car, CAR_RST_DEV_X_SET, CLK_X_GPU);
        Us(10);
        FreeB2aPagesAfterGpuReset();
        Wr(Dev->Car, CAR_CLK_ENB_X_CLR, (CLK_X_GPU | CLK_X_PLLG_REF) & ~clkx0);
        Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_GPU);
        if (fuseChanged) Wr(Dev->Car, CAR_CLK_ENB_H_CLR, CLK_H_FUSE);
        if (outcChanged) Wr(Dev->Car, CAR_PLLP_OUTC, outc0);
        Us(10);
    }
    if (railOn) {
        (VOID)I2cWrite(Dev->Pmic, MAX77620_GPIO6, gpio0);
        (VOID)I2cWrite(Dev->Vdd, MAX77621_VOUT, vout0);
        (VOID)I2cWrite(Dev->Vdd, MAX77621_VOUT_DVS, dvs0);
    }
    Step(11);
    return st;
}

/* ------------------------------------------------------------------- PnP */

NTSTATUS SwGpuEvtPrepareHardware(WDFDEVICE Device, WDFCMRESLIST Raw, WDFCMRESLIST Translated)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    ULONG Count = WdfCmResourceListGetCount(Translated), Index, Mem = 0, I2c = 0;
    PHYSICAL_ADDRESS Pa;
    UNREFERENCED_PARAMETER(Raw);

    for (Index = 0; Index < Count; ++Index) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR Desc = WdfCmResourceListGetDescriptor(Translated, Index);
        if (Desc == NULL) continue;
        if (Desc->Type == CmResourceTypeMemory) {
            if (Mem == 0) { Dev->Bar0Phys = Desc->u.Memory.Start; Dev->Bar0Len = Desc->u.Memory.Length; }
            ++Mem;
        } else if (Desc->Type == CmResourceTypeConnection &&
                   Desc->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                   Desc->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C) {
            LARGE_INTEGER *Id = (I2c == 0) ? &Dev->VddId : &Dev->PmicId;    /* DSDT order */
            if (I2c < 2) { Id->LowPart = Desc->u.Connection.IdLowPart; Id->HighPart = (LONG)Desc->u.Connection.IdHighPart; }
            ++I2c;
        }
    }
    Trace(L"ResMem", Mem);
    Trace(L"ResI2c", I2c);
    Trace(L"Bar0Lo", Dev->Bar0Phys.LowPart);
    if (Mem < 1 || I2c < 2 || Dev->Bar0Len < BAR0_MAP_LEN) return STATUS_DEVICE_CONFIGURATION_ERROR;

    /* Mapping BAR0 is safe; only reading it while the GPU is off is not. */
    Dev->Bar0 = (volatile UCHAR *)MmMapIoSpaceEx(Dev->Bar0Phys, BAR0_MAP_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)CAR_PHYS;
    Dev->Car = (volatile UCHAR *)MmMapIoSpaceEx(Pa, CAR_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)PMC_PHYS;
    Dev->Pmc = (volatile UCHAR *)MmMapIoSpaceEx(Pa, PMC_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    Pa.QuadPart = (LONGLONG)MC_PHYS;
    Dev->Mc = (volatile UCHAR *)MmMapIoSpaceEx(Pa, MC_LEN, PAGE_READWRITE | PAGE_NOCACHE);
    if (!Dev->Bar0 || !Dev->Car || !Dev->Pmc || !Dev->Mc) return STATUS_INSUFFICIENT_RESOURCES;
    return STATUS_SUCCESS;
}

NTSTATUS SwGpuEvtReleaseHardware(WDFDEVICE Device, WDFCMRESLIST Translated)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    UNREFERENCED_PARAMETER(Translated);
    if (Dev->Bar0) { MmUnmapIoSpace((PVOID)Dev->Bar0, BAR0_MAP_LEN); Dev->Bar0 = NULL; }
    if (Dev->Car)  { MmUnmapIoSpace((PVOID)Dev->Car, CAR_LEN);       Dev->Car = NULL; }
    if (Dev->Pmc)  { MmUnmapIoSpace((PVOID)Dev->Pmc, PMC_LEN);       Dev->Pmc = NULL; }
    if (Dev->Mc)   { MmUnmapIoSpace((PVOID)Dev->Mc, MC_LEN);         Dev->Mc = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS SwGpuEvtSelfManagedIoInit(WDFDEVICE Device)
{
    PSWGPU_DEVICE Dev = SwGpuGetDevice(Device);
    NTSTATUS st;
    ULONG mode;

    Trace(L"Version", SWGPU_VERSION);
    Trace(L"StartCount", ReadParam(L"StartCount", 0) + 1);
    st = OpenTarget(Device, Dev->VddId, &Dev->Vdd);
    Trace(L"OpenVddStatus", (ULONG)st);
    st = OpenTarget(Device, Dev->PmicId, &Dev->Pmic);
    Trace(L"OpenPmicStatus", (ULONG)st);

    Snapshot(Dev, FALSE);

    mode = ReadParam(L"PowerTest", 0);
    if (mode >= 1 && mode <= 4) {     /* 2 = B1 probe, 3 = B2a DMA probe, 4 = B3a ACR firmware load */
        BOOLEAN keep = ReadParam(L"KeepOn", 0) == 1;
        LARGE_INTEGER t0, t1, f;
        TraceDurable(L"PowerTest", 0);      /* once: a hang does not repeat at the next boot */
        Trace(L"Result", 0xFFFFFFFFu);
        Trace(L"Boot0", 0);
        t0 = KeQueryPerformanceCounter(&f);
        (VOID)PowerTest(Dev, keep, mode);
        t1 = KeQueryPerformanceCounter(NULL);
        Trace(L"TestUs", (ULONG)((t1.QuadPart - t0.QuadPart) * 1000000 / (f.QuadPart ? f.QuadPart : 1)));
        Snapshot(Dev, TRUE);
        Trace(L"TestRuns", ReadParam(L"TestRuns", 0) + 1);
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
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &Pnp);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&Attributes, SWGPU_DEVICE);
    Status = WdfDeviceCreate(&DeviceInit, &Attributes, &Device);
    if (!NT_SUCCESS(Status)) return Status;
    SwGpuGetDevice(Device)->Device = Device;
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
