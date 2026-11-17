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

#define SWGPU_VERSION      0x00000424u   /* 0.4.36 */

#define CAR_PHYS           0x60006000ull
#define CAR_LEN            0x1000u
#define PMC_PHYS           0x7000E000ull /* page; registers at + 0x400 */
#define PMC_LEN            0x1000u
#define MC_PHYS            0x70019000ull
#define MC_LEN             0x1000u
#define BAR0_MAP_LEN       0x1000000u   /* Claude: full 16 MB BAR0 (ACPI); GR/FECS are at 0x400000+ */

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
#define FALCON_IMEMC0       0x180
#define FALCON_IMEMD0       0x184
#define FALCON_IMEMT0       0x188
#define FALCON_IMEMC_AINCR  (1u << 25)
#define DMATRFCMD_IDLE     (1u << 1)
#define DMATRFCMD_WRITE    (1u << 5)
#define DMATRFCMD_256B     (6u << 8)
#define DMA_IDX            4u          /* ctx dma index 4 = PHYS_SYS_NCOH in nouveau's PMU setup */
#define PMU_UCODE_DMA_IDX  0u          /* GK20A_PMU_DMAIDX_UCODE: physical local FB */
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
#define ACR_VM_ARENA_SIZE  0x80000u
#define ACR_VM_PGD_OFF     0x02000u    /* 8K-aligned; 8K entries x 8 bytes */
#define ACR_VM_PGT_OFF     0x12000u    /* 4K small-page table, 32768 x 8 bytes */
#define ACR_VM_INST_OFF    0x52000u
#define ACR_VM_IMAGE_OFF   0x53000u
#define ACR_GPU_VA_BASE    0x10000000u
#define ACR_SMMU_WINDOW    0x00400000u
#define ACR_WPR_BYTES      0x00040000u
#define ACR_GM20B_IOMMU_TAG (1ull << 34) /* nouveau_platform.c: gm20b iommu_bit */

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
    PHYSICAL_ADDRESS lo, hi, boundary, arenaPa;
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
    lo.QuadPart = 0;
    hi.QuadPart = 0xFFFFFFFFll;
    boundary.QuadPart = 0;
    g_AcrVm.WprBytes = WprBytes;
    g_AcrVm.WprBuffer = MmAllocateContiguousMemorySpecifyCache(
        WprBytes, lo, hi, boundary, MmNonCached);
    if (g_AcrVm.WprBuffer == NULL) return FALSE;
    RtlZeroMemory(g_AcrVm.WprBuffer, WprBytes);
    g_AcrVm.WprPa = MmGetPhysicalAddress(g_AcrVm.WprBuffer);
    if (g_AcrVm.WprPa.HighPart != 0 || (g_AcrVm.WprPa.LowPart & 0xFFFu) != 0) return FALSE;
    Trace(L"AcrWprBufferPaLo", g_AcrVm.WprPa.LowPart);
    Trace(L"AcrWprBufferPaHi", (ULONG)g_AcrVm.WprPa.HighPart);
    Trace(L"AcrWprBufferBytes", WprBytes);

    boundary.QuadPart = ACR_SMMU_WINDOW;
    g_AcrVm.Arena = MmAllocateContiguousMemorySpecifyCache(
        ACR_VM_ARENA_SIZE, lo, hi, boundary, MmNonCached);
    if (g_AcrVm.Arena == NULL) return FALSE;
    RtlZeroMemory(g_AcrVm.Arena, ACR_VM_ARENA_SIZE);
    arenaPa = MmGetPhysicalAddress(g_AcrVm.Arena);
    Trace(L"AcrArenaPa", arenaPa.LowPart);
    if (arenaPa.HighPart != 0 || (arenaPa.LowPart & 0xFFFu) != 0 ||
        arenaPa.LowPart > 0xFFFFFFFFu - ACR_VM_ARENA_SIZE) return FALSE;

    oldAsid = Rd(Dev->Mc, MC_SMMU_GPU_ASID);
    win = arenaPa.LowPart & ~(ACR_SMMU_WINDOW - 1u);
    arenaPages = ACR_VM_ARENA_SIZE >> 12;
    arenaFirst = (arenaPa.LowPart - win) >> 12;
    if (arenaFirst + arenaPages > (ACR_SMMU_WINDOW >> 12)) return FALSE;

    if (oldAsid & 0x80000000u) {
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
    } else {
        /* With the GPU SMMU disabled, PMU VM entries use system physical
         * addresses directly. */
        arenaIova = arenaPa.LowPart;
    }

    *ArenaIovaOut = arenaIova;
    *InstIovaOut = *ArenaIovaOut + ACR_VM_INST_OFF;
    Trace(L"AcrSmmuWin", win);
    Trace(L"AcrArenaIova", (ULONG)*ArenaIovaOut);
    Trace(L"AcrArenaIovaHi", (ULONG)(*ArenaIovaOut >> 32));
    Trace(L"AcrInstIova", (ULONG)*InstIovaOut);
    Trace(L"AcrInstIovaHi", (ULONG)(*InstIovaOut >> 32));
    Trace(L"AcrIommuTagHi", (oldAsid & 0x80000000u) ? 4u : 0u);
    Trace(L"AcrSmmuArenaFirst", arenaFirst);

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
    Trace(L"AcrSmallPdeHighHalf", 1u);
    Trace(L"AcrSocTablePolicy", 1u); /* experimental VIDMEM/VOL table route */
    inst[0x200 / sizeof(ULONGLONG)] =
        (((ULONGLONG)*ArenaIovaOut + ACR_VM_PGD_OFF) & ~0xFFFULL) | 4u;
    inst[0x208 / sizeof(ULONGLONG)] = 0xFFFFFFFFFFULL;
    KeMemoryBarrier();

    /* The PMU GPU-MMU tables above are themselves reached through the SoC
     * SMMU. Record the outer identity map at every page relevant to the PDE
     * fault, including the backing pages that contain the GPU tables. */
    Trace(L"AcrSmmuMode", (oldAsid & 0x80000000u) ? 1u : 0u);
    Trace(L"AcrSmmuGpuAsid", oldAsid);
    if (oldAsid & 0x80000000u) {
        smmuReadbackOk = TRUE;
        smmuPdeIndex = win >> 22;
        smmuPtPa = (ULONGLONG)arenaPa.LowPart + PAGE_SIZE;

        smmuPdeExpected = (ULONG)(smmuPtPa >> 12) |
                          SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
        smmuPdeRead = smmuPd[smmuPdeIndex];
        Trace(L"AcrSmmuPdeIndex", smmuPdeIndex);
        Trace(L"AcrSmmuPdeExpected", smmuPdeExpected);
        Trace(L"AcrSmmuPdeRead", smmuPdeRead);
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
        Trace(L"AcrSmmuPtbDataRead", Rd(Dev->Mc, MC_SMMU_PTB_DATA));
        smmuMapOk = smmuReadbackOk ? 1u : 0u;
    } else {
        /* With SMMU bypassed, the Falcon sees physical pages directly. */
        smmuMapOk = 2u;
    }
    TraceDurable(L"AcrSmmuMapReadback", smmuMapOk);

    Trace(L"AcrPgdIova", (ULONG)(*ArenaIovaOut + ACR_VM_PGD_OFF));
    Trace(L"AcrPgdIovaHi", (ULONG)((*ArenaIovaOut + ACR_VM_PGD_OFF) >> 32));
    Trace(L"AcrPgtIova", (ULONG)(*ArenaIovaOut + ACR_VM_PGT_OFF));
    Trace(L"AcrPgtIovaHi", (ULONG)((*ArenaIovaOut + ACR_VM_PGT_OFF) >> 32));
    Trace(L"AcrPdeIndex", pdeIndex);
    Trace(L"AcrPdeLo", (ULONG)gpuPgd[pdeIndex]);
    Trace(L"AcrPdeHi", (ULONG)(gpuPgd[pdeIndex] >> 32));
    Trace(L"AcrPteVa", ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF);
    Trace(L"AcrPteIndex", pteIndex);
    Trace(L"AcrPteLo", (ULONG)gpuPgt[pteIndex]);
    Trace(L"AcrPteHi", (ULONG)(gpuPgt[pteIndex] >> 32));
    Trace(L"AcrInstPdbLo", (ULONG)inst[0x200 / sizeof(ULONGLONG)]);
    Trace(L"AcrInstPdbHi", (ULONG)(inst[0x200 / sizeof(ULONGLONG)] >> 32));
    Trace(L"AcrInstLimitLo", (ULONG)inst[0x208 / sizeof(ULONGLONG)]);
    Trace(L"AcrInstLimitHi", (ULONG)(inst[0x208 / sizeof(ULONGLONG)] >> 32));
    g_AcrVm.Active = TRUE;
    return TRUE;
}

static BOOLEAN AcrVmBind(volatile UCHAR *B, ULONGLONG InstIova)
{
    ULONG a = 0, d = 0, ap = 0, dp = 0;
    BOOLEAN ok = PmuBindInst(B, InstIova, &a, &d, &ap, &dp);
    Trace(L"AcrBindInstWritten", 0x70000000u | (ULONG)(InstIova >> 12));
    Trace(L"AcrBindInstRead", Rd(B, PMU_FALCON + FALCON_BIND_INST));
    Trace(L"AcrBindAckState", a);
    Trace(L"AcrBindDoneState", d);
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
    Trace(L"AcrMmuInvSlotStatus", status);
    Trace(L"AcrMmuInvFreeSlots", (status & 0x00FF0000u) >> 16);
    Trace(L"AcrMmuInvPendingBefore", (status & 0x00008000u) ? 1u : 0u);
    if (polls == 2000) goto done;

    Wr(B, 0x100CB8, (ULONG)pdb);
    pdbRead = Rd(B, 0x100CB8);
    Trace(L"AcrMmuInvPdb", (ULONG)pdb);
    Trace(L"AcrMmuInvPdbRead", pdbRead);
    /* Nouveau programs 0x100CB8 without reading it back. Keep the readback
     * diagnostic only: this register may not return the written PDB value. */

    Wr(B, 0x100CBC, 0x80000001u); /* invalidate request + PAGE_ALL */
    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if (status & 0x00008000u) { queued = TRUE; break; }
        Us(5);
    }
    Trace(L"AcrMmuInvDonePolls", polls);
    Trace(L"AcrMmuInvDoneStatus", status);
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
    Trace(L"AcrMmuAllPdbBeforeStatus", status);
    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if ((status & 0x00FF0000u) != 0) break;
        Us(5);
    }
    Trace(L"AcrMmuAllPdbSlotPolls", polls);
    Trace(L"AcrMmuAllPdbSlotStatus", status);
    Trace(L"AcrMmuAllPdbFreeSlots", (status & 0x00FF0000u) >> 16);
    if (polls == 2000) {
        TraceDurable(L"AcrMmuAllPdbInvOk", 0);
        return FALSE;
    }

    Wr(B, 0x100CBC, 0x80000003u); /* trigger + PAGE_ALL + ALL_PDB */
    KeMemoryBarrier();
    status = Rd(B, 0x100C80);
    Trace(L"AcrMmuAllPdbRequest", 0x80000003u);
    Trace(L"AcrMmuAllPdbPostWriteStatus", status);
    for (polls = 0; polls < 2000; ++polls) {
        status = Rd(B, 0x100C80);
        if (status & 0x00008000u) {
            fifoEmptyObserved = TRUE;
            break;
        }
        Us(5);
    }
    Trace(L"AcrMmuAllPdbStatusPolls", polls);
    Trace(L"AcrMmuAllPdbStatus", status);
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
        Trace(L"AcrSmmuRestoreData", restoreData);
        TraceDurable(L"AcrSmmuRestored",
                     restoreData == g_AcrVm.OldPtbData && restoreAsid == g_AcrVm.OldPtbAsid ? 1u : 0u);
    } else {
        TraceDurable(L"AcrSmmuRestored", 1u); /* no SMMU page-table change */
    }
    if (g_AcrVm.WprBuffer != NULL)
        MmFreeContiguousMemorySpecifyCache(g_AcrVm.WprBuffer, g_AcrVm.WprBytes, MmNonCached);
    if (g_AcrVm.Arena != NULL)
        MmFreeContiguousMemorySpecifyCache(g_AcrVm.Arena, ACR_VM_ARENA_SIZE, MmNonCached);
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

static VOID WprPutLe64(PUCHAR P, ULONGLONG V)
{
    WprPutLe32(P, (ULONG)V);
    WprPutLe32(P + 4, (ULONG)(V >> 32));
}

/* Build a GM20B WPR image from the staged Linux firmware blobs. Runtime PMU
 * loader_config addresses are physical WPR addresses (WPR base + image
 * offsets), as populated by nvgpu and relocated by nouveau before ACR.
 * WprBase=0 is used only for the CPU-only layout preview. */
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
    if (OutWpr != NULL && WprBase == 0) {
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
    Trace(L"B3cPmuAppVersion", Le32(pmuDesc + 12));
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
    /* The PMU ACR loader uses DMAIDX_UCODE (0), whose FBIF TRANSCFG is
     * physical local FB. DMA_IDX=4 is PHYS_SYS_NCOH for the separate B2a
     * system-memory DMA probe and would send these WPR addresses to sysmem. */
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
    Trace(L"B3cPmuCodeDmaBase", pmuBld[1]);
    Trace(L"B3bPmuDmaIdx", pmuBld[0]);
    Trace(L"B3cPmuDataDmaBase", pmuBld[5]);
    Trace(L"B3cPmuWprBaseLo", (ULONG)WprBase);
    Trace(L"B3cPmuWprBaseHi", (ULONG)(WprBase >> 32));
    Trace(L"B3cPmuDmaBaseAbsolute", WprBase != 0 ? 1u : 0u);
    Trace(L"B3bPmuArgv", pmuBld[9]);
    Trace(L"B3cFecsDmaIdx", fecsBld[8]);
    Trace(L"B3cFecsCodeDmaBase", fecsBld[9]);
    Trace(L"B3cFecsDataDmaBase", fecsBld[15]);
    Trace(L"B3cFecsDmaBaseAbsolute", WprBase != 0 ? 1u : 0u);
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
    Trace(L"B3cLsPmuPreCpuctl", cpuctl);
    Trace(L"B3cLsPmuPreSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
    Trace(L"B3cLsPmuPreMailbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
    Trace(L"B3cLsPmuPreMailbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
    if ((cpuctl & 0x50u) != 0x50u ||
        Rd(B, PMU_FALCON + FALCON_MAILBOX0) != 0 ||
        Rd(B, PMU_FALCON + FALCON_MAILBOX1) != 0) {
        result = 0xE000006Du;
        goto done;
    }
    hwcfg = Rd(B, PMU_FALCON + FALCON_HWCFG);
    dmemSize = ((hwcfg >> 9) & 0x1FFu) << 8;
    Trace(L"B3cLsPmuDmemSize", dmemSize);
    if (dmemSize < 256u || dmemSize > 0xFFFFu) {
        result = 0xE000006Eu;
        goto done;
    }
    Wr(B, PMU_FALCON + 0x004, 0x10u);
    irq = Rd(B, PMU_FALCON + 0x008);
    Trace(L"B3cLsPmuIrqAfterClear", irq);
    if (irq & 0x10u) {
        result = 0xE000006Fu;
        goto done;
    }
    for (i = 0; i < RTL_NUMBER_OF(aperture); ++i)
        Wr(B, PMU_FALCON + 0xE00 + i * 4u, aperture[i]);
    Trace(L"B3cLsPmuAperture0", Rd(B, PMU_FALCON + 0xE00));
    Trace(L"B3cLsPmuAperture4", Rd(B, PMU_FALCON + 0xE10));

    argsOff = dmemSize - sizeof(args);
    Trace(L"B3cLsPmuArgvAdvertised", PmuArgv);
    Trace(L"B3cLsPmuArgvComputed", argsOff);
    Trace(L"B3cLsPmuArgsSize", sizeof(args));
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
    Trace(L"B3cLsPmuArgsOff", argsOff);
    Trace(L"B3cLsPmuArgsCursor", argsCursor);
    TraceDurable(L"B3cLsPmuArgsOk", argsOk);
    TraceBin(L"B3cLsPmuArgvReadback", argsReadback, sizeof(argsReadback));
    if (!argsOk || argsCursor != argsOff + sizeof(args)) {
        result = 0xE0000073u;
        goto done;
    }

    Trace(L"B3cLsPmuItfenBeforeStart", Rd(B, PMU_FALCON + FALCON_ITFEN));
    Trace(L"B3cLsPmuCpuctlAliasBefore", Rd(B, PMU_FALCON + 0x130));
    /* ACR leaves DMACTL=1 on this device. Snapshot only; host writes are
     * ignored in this post-ACR secure state, as observed in 0.4.24. */
    Step(63);
    dmaCtlBeforeStart = Rd(B, PMU_FALCON + FALCON_DMACTL);
    Trace(L"B3cLsPmuDmaCtlBeforeStart", dmaCtlBeforeStart);
    dmaCtlAtStart = Rd(B, PMU_FALCON + FALCON_DMACTL);
    Trace(L"B3cLsPmuDmaCtlAtStart", dmaCtlAtStart);
    Wr(B, PMU_FALCON + 0x130, 2u); /* CPUCTL_ALIAS starts the LS RTOS. */
    Trace(L"B3cLsPmuCpuctlAliasAfter", Rd(B, PMU_FALCON + 0x130));
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
    Trace(L"B3cLsPmuInitBadPairs", initBadPairs);
    if (initBadPairs) {
        Trace(L"B3cLsPmuInitBadHead", initBadHead);
        Trace(L"B3cLsPmuInitBadTail", initBadTail);
        Trace(L"B3cLsPmuInitBadPoll", initBadPoll);
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
            Trace(L"B3cLsPmuInitHdr", initHdr);
            Trace(L"B3cLsPmuInitType", initType);
            Trace(L"B3cLsPmuInitPioCursor", initCursor);
            TraceBin(L"B3cLsPmuInitWords", initWords, sizeof(initWords));
            if ((initHdr & 0xFFu) != 7u || ((initHdr >> 8) & 0xFFu) != 42u ||
                initType != 0u || initCursor != tail + 44u)
                result = 0xE0000072u;
        }
    }
done:
    Step(60);
    Trace(L"B3cLsPmuIrqStat", Rd(B, PMU_FALCON + 0x008));
    Trace(L"B3cLsPmuInitPolls", polls);
    head = Rd(B, PMU_FALCON + 0x4C8);
    tail = Rd(B, PMU_FALCON + 0x4CC);
    Trace(L"B3cLsPmuMsgHead", head);
    Trace(L"B3cLsPmuMsgTail", tail);
    Trace(L"B3cLsPmuCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    Trace(L"B3cLsPmuSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
    Trace(L"B3cLsPmuMailbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
    Trace(L"B3cLsPmuMailbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
    /* Falcon register offsets follow the MIT nvgpu GK20A Falcon register map. */
    Trace(L"B3cLsPmuIrqMode", Rd(B, PMU_FALCON + 0x00C));
    Trace(L"B3cLsPmuIrqMask", Rd(B, PMU_FALCON + 0x018));
    Trace(L"B3cLsPmuIrqDest", Rd(B, PMU_FALCON + 0x01C));
    Trace(L"B3cLsPmuGptmrInt", Rd(B, PMU_FALCON + 0x028));
    Trace(L"B3cLsPmuGptmrVal", Rd(B, PMU_FALCON + 0x02C));
    Trace(L"B3cLsPmuPtimerLow", Rd(B, NV_PTIMER_TIME_0));
    Trace(L"B3cLsPmuPtimerHigh", Rd(B, NV_PTIMER_TIME_1));
    diagOff = argsOff >= 16u ? argsOff - 16u : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / 4u : 0u;
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI)
            diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        Trace(L"B3cLsPmuArgsWindowOff", diagOff);
        Trace(L"B3cLsPmuArgsWindowWords", diagCount);
        TraceBin(L"B3cLsPmuArgsWindow", diagWords, diagCount * sizeof(ULONG));
    }
    diagOff = (tail != 0xFFFFFFFFu && tail >= 16u) ? (tail & ~3u) - 16u : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / 4u : 0u;
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI)
            diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        Trace(L"B3cLsPmuQueueDataOff", diagOff);
        Trace(L"B3cLsPmuQueueDataWords", diagCount);
        TraceBin(L"B3cLsPmuQueueDataWindow", diagWords, diagCount * sizeof(ULONG));
    }
    TraceDurable(L"B3cLsPmuStepResult", result);
    traceIdx = Rd(B, PMU_FALCON + 0x148);
    traceNr = (traceIdx >> 16) & 0xFFu;
    Trace(L"B3cLsPmuTraceIdx", traceIdx);
    Trace(L"B3cLsPmuTraceNr", traceNr);
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
    Trace(L"B3cLsPmuImemHwcfg", imemHwcfg);
    Trace(L"B3cLsPmuImemSize", imemSize);
    Trace(L"B3cLsPmuImemTagBefore", Rd(B, PMU_FALCON + FALCON_IMEMT0));
    Trace(L"B3cLsPmuImemReadOff", imemOff);
    TraceDurable(L"B3cLsPmuImemReadOk", imemOk);
    if (imemOk && imemOff + sizeof(imemWords) <= imemSize) {
        Wr(B, PMU_FALCON + FALCON_IMEMC0,
           ((imemOff >> 8) << 8) | (((imemOff >> 2) & 0x3Fu) << 2) | FALCON_IMEMC_AINCR);
        Trace(L"B3cLsPmuImemTagSelected", Rd(B, PMU_FALCON + FALCON_IMEMT0));
        for (diagI = 0; diagI < RTL_NUMBER_OF(imemWords); ++diagI)
            imemWords[diagI] = Rd(B, PMU_FALCON + FALCON_IMEMD0);
        TraceBin(L"B3cLsPmuImemPcWindow", imemWords, sizeof(imemWords));
    }
    Trace(L"B3cLsPmuImemTagAfter", Rd(B, PMU_FALCON + FALCON_IMEMT0));
    Trace(L"B3cLsPmuDmaCmd", Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
    Trace(L"B3cLsPmuDmaBase", Rd(B, PMU_FALCON + FALCON_DMATRFBASE));
    Trace(L"B3cLsPmuDmaMoffs", Rd(B, PMU_FALCON + FALCON_DMATRFMOFFS));
    Trace(L"B3cLsPmuDmaFboffs", Rd(B, PMU_FALCON + FALCON_DMATRFFBOFFS));
    Trace(L"B3cLsPmuDmaCtl", Rd(B, PMU_FALCON + FALCON_DMACTL));
    Trace(L"B3cLsPmuBindStatus", Rd(B, PMU_FALCON + FALCON_BIND_STATUS));
    Trace(L"B3cLsPmuBindInst", Rd(B, PMU_FALCON + FALCON_BIND_INST));
    Trace(L"B3cLsPmuCtxEnable", Rd(B, PMU_FALCON + FALCON_CTX_ENABLE));
    Trace(L"B3cLsPmuCtxAction", Rd(B, PMU_FALCON + FALCON_CTX_ACTION));
    for (diagI = 0; diagI < RTL_NUMBER_OF(transcfg); ++diagI)
        transcfg[diagI] = Rd(B, PMU_FALCON + PMU_FBIF_TRANSCFG(diagI));
    TraceBin(L"B3cLsPmuTranscfg", transcfg, sizeof(transcfg));
    Trace(L"B3cLsPmuFbifCtl", Rd(B, PMU_FALCON + PMU_FBIF_CTL));
    Trace(L"B3cLsPmuPfifoIntr0", Rd(B, 0x2100));
    faultId = Rd(B, 0x259C);
    Trace(L"B3cLsPmuFaultId", faultId);
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
    Trace(L"B3cLsPmuFaultRecCount", faultNr);
    if (faultNr) TraceBin(L"B3cLsPmuFaultRecs", faultRecs, faultNr * sizeof(faultRecs[0]));
    Trace(L"B3cLsPmuMcInt", Rd(Dev->Mc, MC_INTSTATUS));
    Trace(L"B3cLsPmuMcErr", Rd(Dev->Mc, MC_ERR_STATUS));
    Trace(L"B3cLsPmuMcErrAdr", Rd(Dev->Mc, MC_ERR_ADR));
    /* Expand both bounded DMEM snapshots to 256 bytes for loader/runtime data. */
    diagOff = argsOff >= 0x80u ? argsOff - 0x80u : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / sizeof(ULONG) : 0u; /* Claude: no underflow on a bogus tail */
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI) diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        Trace(L"B3cLsPmuArgsWindowOff", diagOff);
        Trace(L"B3cLsPmuArgsWindowWords", diagCount);
        TraceBin(L"B3cLsPmuArgsWindow", diagWords, diagCount * sizeof(ULONG));
    }
    diagOff = (tail != 0xFFFFFFFFu && tail >= 0x80u) ? ((tail & ~3u) - 0x80u) : 0u;
    diagCount = (diagOff < dmemSize) ? (dmemSize - diagOff) / sizeof(ULONG) : 0u; /* Claude: no underflow on a bogus tail */
    if (diagCount > RTL_NUMBER_OF(diagWords)) diagCount = RTL_NUMBER_OF(diagWords);
    if (diagCount) {
        Wr(B, PMU_FALCON + FALCON_DMEMC0, (1u << 25) | diagOff);
        for (diagI = 0; diagI < diagCount; ++diagI) diagWords[diagI] = Rd(B, PMU_FALCON + FALCON_DMEMD0);
        Trace(L"B3cLsPmuQueueDataOff", diagOff);
        Trace(L"B3cLsPmuQueueDataWords", diagCount);
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
    Trace(L"B3cFecsInitHead", msgHead);
    Trace(L"B3cFecsInitTail", msgTail);
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
    Trace(L"B3cFecsHpqBase", hpqBase); Trace(L"B3cFecsHpqSize", hpqSize);
    Trace(L"B3cFecsHpqIndex", hpqIndex); Trace(L"B3cFecsMsgqBase", msgBase);
    Trace(L"B3cFecsMsgqSize", msgSize); Trace(L"B3cFecsInitCursor", cursor);
    if (hpqIndex >= 4u || hpqBase < 4u || msgBase < 4u || hpqSize < 32u || msgSize < 64u ||
        hpqEnd > dmemSize || msgEnd > dmemSize ||
        (hpqBase < msgEnd && msgBase < hpqEnd) || msgTail < msgBase || msgTail + 44u > msgEnd ||
        msgHead < msgBase || msgHead >= msgEnd) { result = 0xE0000078u; goto done; }
    Wr(B, PMU_FALCON + 0x4CC, msgTail + 44u); /* acknowledge the validated INIT */

    reg = 0x4A0u + hpqIndex * 4u;
    qhead = Rd(B, PMU_FALCON + reg);
    qtail = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
    hpqEnd = hpqBase + hpqSize;
    Trace(L"B3cFecsHpqHeadBefore", qhead); Trace(L"B3cFecsHpqTail", qtail);
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
    Trace(L"B3cFecsWprCommandHeadAfter", qhead);
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
    Trace(L"B3cFecsWprReplyHeader", wprReplyHdr); Trace(L"B3cFecsWprReplyCtrl", wprReplyCtrl);
    Trace(L"B3cFecsWprReplyCode", wprReplyCode);
    if ((wprReplyHdr & 0xFFu) != 0x0Au || ((wprReplyHdr >> 8) & 0xFFu) != 12u ||
        ((wprReplyHdr >> 24) & 0xFFu) != 1u || ((wprReplyHdr >> 18) & 1u) ||
        (wprReplyCtrl & 0xFFu) != 0u || wprReplyCode != 0u) {
        result = 0xE0000079u; goto done;
    }
    Wr(B, PMU_FALCON + 0x4CC, replyTail + 12u);
    Trace(L"B3cFecsHpqHeadAfterWprInit", Rd(B, PMU_FALCON + reg));
    Trace(L"B3cFecsHpqTailAfterWprInit", Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u));
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointAfterWprAck");

    Step(67);
    /* Claude: keep 'reg' as the HPQ head register offset; 0.4.28 reused it for
     * PMC_ENABLE, so Step 68 accessed PMU_FALCON + 0xC0013120 (bugcheck). */
    pmcEnable = Rd(B, NV_PMC_ENABLE);
    Trace(L"B3cFecsPmcEnableBefore", pmcEnable);
    Wr(B, NV_PMC_ENABLE, pmcEnable | (1u << 12));
    (VOID)Rd(B, NV_PMC_ENABLE);
    Trace(L"B3cFecsPmcEnableAfter", Rd(B, NV_PMC_ENABLE));
    if (!(Rd(B, NV_PMC_ENABLE) & (1u << 12))) { result = 0xE0000078u; goto done; }
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointAfterGrEnable");
    /* Retain 0x419A00 only as a legacy comparison. The GM20B FECS ctxsw
     * mailbox sequence is written immediately before the FECS start below. */
    Trace(L"B3cFecsLegacyMailbox419A00BeforeStartSeq", Rd(B, 0x419A00u));
    Trace(L"B3cFecsCtxswMailbox0BeforeStartSeq", Rd(B, 0x409800u));
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointBeforeFecsStartMailboxSeq");
    Step(68);
    qhead = Rd(B, PMU_FALCON + reg);
    qtail = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
    Trace(L"B3cFecsHpqHeadBeforeBootstrap", qhead); Trace(L"B3cFecsHpqTailBeforeBootstrap", qtail);
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
    Trace(L"B3cFecsBootstrapSeq", bootstrapSeq);
    Step(71);
    TraceB3cPmuCheckpoint(B, L"B3cPmuCheckpointBeforeBootstrap");
    if (Rd(B, PMU_FALCON + FALCON_CPUCTL) & 0x10u) { result = 0xE000007Bu; goto done; }
    Step(72);
    for (i = 0; i < RTL_NUMBER_OF(cmd); ++i) Wr(B, PMU_FALCON + FALCON_DMEMD0, cmd[i]);
    KeMemoryBarrier();
    qhead += sizeof(cmd);
    Wr(B, PMU_FALCON + reg, qhead);
    Trace(L"B3cFecsCommandHeadAfter", qhead);
    TraceBin(L"B3cFecsBootstrapCommand", cmd, sizeof(cmd));

    Step(69);
    Trace(L"B3cFecsPmuIrqStatAfterPost", Rd(B, PMU_FALCON + 0x008));
    Trace(L"B3cFecsPmuCpuctlAfterPost", Rd(B, PMU_FALCON + FALCON_CPUCTL));
    if (Rd(B, PMU_FALCON + FALCON_CPUCTL) & 0x10u) { result = 0xE000007Cu; goto done; }
    pmuTraceIdx = Rd(B, PMU_FALCON + 0x148);
    pmuTraceNr = (pmuTraceIdx >> 16) & 0xFFu;
    if (pmuTraceNr > RTL_NUMBER_OF(pmuPcNow)) pmuTraceNr = RTL_NUMBER_OF(pmuPcNow);
    Trace(L"B3cFecsPmuTraceIdxAfterPost", pmuTraceIdx);
    Trace(L"B3cFecsPmuTraceCountAfterPost", pmuTraceNr);
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
    Trace(L"B3cFecsHpqSampleCount", hpqSampleNr);
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
    Trace(L"B3cFecsReplyHeader", replyHdr); Trace(L"B3cFecsReplyCtrl", replyCtrl);
    Trace(L"B3cFecsReplyPayload", replyPayload);
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
    Trace(L"B3cFecsDmaCtlBeforeStartPrep", Rd(B, 0x40910Cu));
    gpccsInst = ReadBinParam(L"FwGpccsInst", 0x20000u, &gpccsInstLen);
    gpccsData = ReadBinParam(L"FwGpccsData", 0x20000u, &gpccsDataLen);
    Trace(L"B3cGpccsInstLen", gpccsInstLen);
    Trace(L"B3cGpccsDataLen", gpccsDataLen);
    if (gpccsInst == NULL || gpccsData == NULL) { result = 0xE000007Eu; goto done; }
    gpccsHwcfg = Rd(B, 0x41A108u);
    gpccsImemSize = (gpccsHwcfg & 0x1FFu) << 8;
    gpccsDmemSize = ((gpccsHwcfg >> 9) & 0x1FFu) << 8;
    gpccsImemWords = (gpccsInstLen + 3u) / 4u;
    /* Nouveau nvkm_falcon_v1_load_imem() pads code to a 0x40-word block. */
    gpccsImemPaddedWords = (gpccsImemWords + 0x3Fu) & ~0x3Fu;
    gpccsDmemWords = (gpccsDataLen + 3u) / 4u;
    Trace(L"B3cGpccsHwcfg", gpccsHwcfg);
    Trace(L"B3cGpccsImemSize", gpccsImemSize);
    Trace(L"B3cGpccsDmemSize", gpccsDmemSize);
    Trace(L"B3cGpccsImemSourceWords", gpccsImemWords);
    Trace(L"B3cGpccsImemPaddedWords", gpccsImemPaddedWords);
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
    Trace(L"B3cGpccsImemWordsWritten", gpccsImemPaddedWords);
    Trace(L"B3cGpccsDmemWordsWritten", gpccsDmemWords);

    Step(75);
    /* Match nvgpu gr_gm20b_load_ctxsw_ucode() secure-start order: clear
     * mailbox 0, write mailbox 1 = 1, then clear mailbox 6. Nouveau
     * gf100_gr_init_ctxctl_ext() separately clears MB0 and DMACTL before
     * starting GPCCS followed by FECS. */
    Trace(L"B3cFecsCtxswMailbox0BeforeSecureSeq", Rd(B, 0x409800u));
    Wr(B, 0x409840u, 0xFFFFFFFFu);
    Trace(L"B3cFecsCtxswMailbox0AfterClear", Rd(B, 0x409800u));
    Wr(B, 0x409804u, 1u);
    Trace(L"B3cFecsCtxswMailbox1Set", Rd(B, 0x409804u));
    Wr(B, 0x409858u, 0xFFFFFFFFu);
    Trace(L"B3cFecsCtxswMailboxClear6Snapshot", Rd(B, 0x409858u));
    Trace(L"B3cFecsCtxswMailbox6", Rd(B, 0x409818u));
    Trace(L"B3cFecsCtxswClearRegSnapshot", Rd(B, 0x409840u));
    Trace(L"B3cFecsCtxswMailbox0AfterSecureSeq", Rd(B, 0x409800u));
    Trace(L"B3cGpccsDmaCtlBeforeStart", Rd(B, 0x41A10Cu));
    Wr(B, 0x41A10Cu, 0u);
    Trace(L"B3cGpccsDmaCtlAfterStartPrep", Rd(B, 0x41A10Cu));
    Trace(L"B3cFecsDmaCtlBeforeStart", Rd(B, 0x40910Cu));
    Wr(B, 0x40910Cu, 0u);
    Trace(L"B3cFecsDmaCtlAfterStartPrep", Rd(B, 0x40910Cu));
    /* Nouveau's direct PIO path starts GPCCS without programming BOOTVEC. */
    gpccsBootvecBeforeStart = Rd(B, 0x41A104u);
    Trace(L"B3cGpccsBootvecBeforeStart", gpccsBootvecBeforeStart);
    gpccsCpuctlBeforeStart = Rd(B, 0x41A100u);
    gpccsStartReg = (gpccsCpuctlBeforeStart & 0x40u) ? 0x41A130u : 0x41A100u;
    Trace(L"B3cGpccsCpuctlBeforeStart", gpccsCpuctlBeforeStart);
    Trace(L"B3cGpccsStartUsesAlias", gpccsStartReg == 0x41A130u ? 1u : 0u);
    Wr(B, gpccsStartReg, 2u);
    gpccsCpuctlAfterStart = Rd(B, 0x41A100u);
    gpccsMailboxAfterStart = Rd(B, 0x41A040u);
    Trace(L"B3cGpccsCpuctlAfterStart", gpccsCpuctlAfterStart);
    Trace(L"B3cGpccsMailbox0AfterStart", gpccsMailboxAfterStart);
    Trace(L"B3cGpccsBootvecAfterStart", Rd(B, 0x41A104u));
    for (gpccsStartPolls = 0; gpccsStartPolls < 2000u; ++gpccsStartPolls) {
        gpccsCpuctlAfterStart = Rd(B, 0x41A100u);
        if (!(gpccsCpuctlAfterStart & 0x10u)) break;
        Ms(1);
    }
    Trace(L"B3cGpccsStartPolls", gpccsStartPolls);
    if (gpccsStartPolls == 2000u) { result = 0xE0000080u; goto done; }

    fecsCpuctlBeforeStart = Rd(B, 0x409100u);
    fecsStartReg = (fecsCpuctlBeforeStart & 0x40u) ? 0x409130u : 0x409100u;
    Trace(L"B3cFecsCpuctlBeforeStart", fecsCpuctlBeforeStart);
    Trace(L"B3cFecsStartUsesAlias", (fecsStartReg == 0x409130u) ? 1u : 0u);
    Wr(B, fecsStartReg, 2u); /* nvkm_falcon_v1_start(): CPUCTL_ALIAS when bit 6 is enabled */
    fecsCpuctlAfterStart = Rd(B, 0x409100u);
    fecsMailboxAfterStart = Rd(B, 0x409040u);
    Trace(L"B3cFecsCpuctlAfterStart", fecsCpuctlAfterStart);
    Trace(L"B3cFecsMailbox0AfterStart", fecsMailboxAfterStart);
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
    Trace(L"B3cFecsReadyMailbox", fecsReady); Trace(L"B3cFecsReadyFirstNonzero", fecsReadyFirstNonzero);
    Trace(L"B3cFecsReadyMailboxChanges", fecsReadyChangeCount); Trace(L"B3cFecsReadyPolls", polls);
    if (polls == 2000u) result = 0xE0000078u;
    Trace(L"B3cFecsLegacyMailbox419A00AfterWait", Rd(B, 0x419A00u));
    for (i = 0; i < RTL_NUMBER_OF(fecsMailboxWords); ++i) fecsMailboxWords[i] = Rd(B, 0x409800u + i * 4u);
    TraceBin(L"B3cFecsCtxswMailboxWords", fecsMailboxWords, sizeof(fecsMailboxWords));
done:
    Step(70);
    TraceDurable(L"B3cFecsStepResult", result);
    if (hpqIndex < 4u) {
        hpqTailAfterWait = Rd(B, PMU_FALCON + 0x4B0u + hpqIndex * 4u);
        Trace(L"B3cFecsHpqHeadAfterWait", Rd(B, PMU_FALCON + 0x4A0u + hpqIndex * 4u));
        Trace(L"B3cFecsHpqTailAfterWait", hpqTailAfterWait);
    } else {
        Trace(L"B3cFecsHpqHeadAfterWait", 0xFFFFFFFFu);
        Trace(L"B3cFecsHpqTailAfterWait", 0xFFFFFFFFu);
    }
    Trace(L"B3cFecsMsgHeadAfterWait", Rd(B, PMU_FALCON + 0x4C8));
    Trace(L"B3cFecsMsgTailAfterWait", Rd(B, PMU_FALCON + 0x4CC));
    Trace(L"B3cFecsPmuIrqStatAfterWait", Rd(B, PMU_FALCON + 0x008));
    pmuTraceIdx = Rd(B, PMU_FALCON + 0x148);
    pmuPcNrAfterWait = (pmuTraceIdx >> 16) & 0xFFu;
    if (pmuPcNrAfterWait > RTL_NUMBER_OF(pmuPcAfterWait)) pmuPcNrAfterWait = RTL_NUMBER_OF(pmuPcAfterWait);
    Trace(L"B3cFecsPmuTraceIdxAfterWait", pmuTraceIdx);
    Trace(L"B3cFecsPmuTraceCountAfterWait", pmuPcNrAfterWait);
    for (i = 0; i < pmuPcNrAfterWait; ++i) { Wr(B, PMU_FALCON + 0x148, i); pmuPcAfterWait[i] = Rd(B, PMU_FALCON + 0x14C); }
    if (pmuPcNrAfterWait) TraceBin(L"B3cFecsPmuTraceAfterWait", pmuPcAfterWait, pmuPcNrAfterWait * sizeof(ULONG));
    /* Touch GR/FECS registers only when GR is enabled (an early exit before
     * Step 67 leaves PMC_ENABLE bit 12 clear). */
    Trace(L"B3cFecsGrEnabledAtRead", (Rd(B, NV_PMC_ENABLE) >> 12) & 1u);
    if (Rd(B, NV_PMC_ENABLE) & (1u << 12)) {
    Trace(L"B3cFecsCpuctl", Rd(B, 0x409100u)); Trace(L"B3cFecsSctl", Rd(B, 0x409200u));
    Trace(L"B3cFecsMailbox0", Rd(B, 0x409040u)); Trace(L"B3cFecsMailbox1", Rd(B, 0x409044u));
    Trace(L"B3cFecsLegacyMailbox419A00Final", Rd(B, 0x419A00u));
    fecsTraceIdx = Rd(B, 0x409148u); fecsTraceNr = (fecsTraceIdx >> 16) & 0xFFu;
    if (fecsTraceNr > RTL_NUMBER_OF(fecsPc)) fecsTraceNr = RTL_NUMBER_OF(fecsPc);
    Trace(L"B3cFecsTraceIdx", fecsTraceIdx); Trace(L"B3cFecsTraceCount", fecsTraceNr);
    for (pcSlot = 0; pcSlot < fecsTraceNr; ++pcSlot) { Wr(B, 0x409148u, pcSlot); fecsPc[pcSlot] = Rd(B, 0x40914Cu); }
    if (fecsTraceNr) TraceBin(L"B3cFecsTracePc", fecsPc, fecsTraceNr * sizeof(ULONG));
    Trace(L"B3cFecsIrqStat", Rd(B, 0x409008u)); Trace(L"B3cFecsIrqMask", Rd(B, 0x409018u));
    Trace(L"B3cGpccsCpuctl", Rd(B, 0x41A100u)); Trace(L"B3cGpccsSctl", Rd(B, 0x41A240u));
    Trace(L"B3cGpccsFalconMailbox0", Rd(B, 0x41A040u)); Trace(L"B3cGpccsFalconMailbox1", Rd(B, 0x41A044u));
    Trace(L"B3cGpccsBootvecFinal", Rd(B, 0x41A104u));
    gpccsTraceIdx = Rd(B, 0x41A148u); gpccsTraceNr = (gpccsTraceIdx >> 16) & 0xFFu;
    if (gpccsTraceNr > RTL_NUMBER_OF(gpccsPc)) gpccsTraceNr = RTL_NUMBER_OF(gpccsPc);
    Trace(L"B3cGpccsTraceIdx", gpccsTraceIdx); Trace(L"B3cGpccsTraceCount", gpccsTraceNr);
    for (pcSlot = 0; pcSlot < gpccsTraceNr; ++pcSlot) { Wr(B, 0x41A148u, pcSlot); gpccsPc[pcSlot] = Rd(B, 0x41A14Cu); }
    if (gpccsTraceNr) TraceBin(L"B3cGpccsTracePc", gpccsPc, gpccsTraceNr * sizeof(ULONG));
    Trace(L"B3cGpccsIrqStat", Rd(B, 0x41A008u)); Trace(L"B3cGpccsIrqMask", Rd(B, 0x41A018u));
    for (i = 0; i < RTL_NUMBER_OF(gpccsCtxswMailboxWords); ++i) gpccsCtxswMailboxWords[i] = Rd(B, 0x41A800u + i * 4u);
    TraceBin(L"B3cGpccsCtxswMailboxWords", gpccsCtxswMailboxWords, sizeof(gpccsCtxswMailboxWords));
    }
    Trace(L"B3cFecsPfifoIntr0", Rd(B, 0x2100)); faultId = Rd(B, 0x259C); Trace(L"B3cFecsFaultId", faultId);
    for (unit = 0; unit < 32u && faultCount < RTL_NUMBER_OF(faultRecs); ++unit) if (faultId & (1u << unit)) {
        faultRecs[faultCount][0] = unit; faultRecs[faultCount][1] = Rd(B, 0x2800u + unit * 0x10u);
        faultRecs[faultCount][2] = Rd(B, 0x2804u + unit * 0x10u); faultRecs[faultCount][3] = Rd(B, 0x2808u + unit * 0x10u);
        faultRecs[faultCount][4] = Rd(B, 0x280Cu + unit * 0x10u); ++faultCount;
    }
    Trace(L"B3cFecsFaultRecCount", faultCount);
    if (faultCount) TraceBin(L"B3cFecsFaultRecs", faultRecs, faultCount * sizeof(faultRecs[0]));
    TraceDurable(L"B3cFecsResult", result);
    if (gpccsData != NULL) ExFreePoolWithTag(gpccsData, SWGPU_TAG);
    if (gpccsInst != NULL) ExFreePoolWithTag(gpccsInst, SWGPU_TAG);
    return result;
}

static VOID ProbeB3a(PSWGPU_DEVICE Dev, BOOLEAN RunAcr)
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
    ULONG bootPolls, pmuArgv = 0, fecsErr = 0;
    BOOLEAN acrAttempted = FALSE;
    ULONGLONG arenaIova = 0, instIova = 0;
    ULONGLONG wprStart, wprEnd;
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

    if (RunAcr) {
        ULONGLONG *acrDesc;
        /* 0.4.6: the memory-path setup that B2a proved necessary (priv ring,
         * PFIFO reset/enable in nvgpu order, FB flush). Without the priv ring
         * the L2 answered with PRI errors and falcon DMA never completed. The
         * whole GPU is reset after this test, so nothing is put back here. */
        PrivRingInit(B, 53);
        Trace(L"B3cLtcNr", Rd(B, 0x12006C));
        Trace(L"B3cTagBase", Rd(B, 0x17E278));
        {
            ULONG en = Rd(B, NV_PMC_ENABLE), w;
            Trace(L"B3cFifoPmcWas", en);
            Wr(B, NV_PMC_ENABLE, en & ~0x100u);
            (VOID)Rd(B, NV_PMC_ENABLE);
            Us(20);
            Wr(B, NV_PMC_ENABLE, en | 0x100u);
            (VOID)Rd(B, NV_PMC_ENABLE);
            Us(20);
            Trace(L"B3cFifoPmcNow", Rd(B, NV_PMC_ENABLE));
            Wr(B, 0x2100, 0xFFFFFFFFu);
            Trace(L"B3cFifoIntr0Clr", Rd(B, 0x2100));
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
        Trace(L"AcrWprRawStart", wprRawStart);
        Trace(L"AcrWprRawLimit", wprRawLimit);
        Trace(L"AcrWprStartLo", (ULONG)wprStart);
        Trace(L"AcrWprStartHi", (ULONG)(wprStart >> 32));
        Trace(L"AcrWprEndLo", (ULONG)wprEnd);
        Trace(L"AcrWprEndHi", (ULONG)(wprEnd >> 32));
        if (wprStart == 0 || wprEnd <= wprStart || wprEnd - wprStart != ACR_WPR_BYTES) {
            err = 0xE0000061u; goto out;
        }

        Step(48);   /* build WPR in CPU memory and verify it fits the protected region */
        Step(64);   /* PMU LS loader DMA bases include the physical WPR base */
        err = BuildWprB3b(dmemSize, wprStart, &wprImage, &wprAllocLen, &wprUsed, &pmuArgv);
        Trace(L"AcrWprBuildAlloc", wprAllocLen);
        Trace(L"AcrWprBuildUsed", wprUsed);
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
        Trace(L"AcrWprCpuCopyBytes", wprUsed);

        /* GM20B's setup differs from GM200: point the descriptor at the
         * allocated INST-target WPR blob instead of programming region_props. */
        acrDesc = (ULONGLONG *)(img + dataBase);
        WprPutLe32((PUCHAR)acrDesc + 0x240, wprUsed); /* ucode_blob_size */
        WprPutLe64((PUCHAR)acrDesc + 0x248, (ULONGLONG)g_AcrVm.WprPa.QuadPart);
        Trace(L"AcrDescWprBytes", wprUsed);
        Trace(L"AcrDescWprBaseLo", g_AcrVm.WprPa.LowPart);
        Trace(L"AcrDescWprBaseHi", (ULONG)g_AcrVm.WprPa.HighPart);
        RtlCopyMemory((PUCHAR)g_AcrVm.Arena + ACR_VM_IMAGE_OFF, img, ucDataSize);
        KeMemoryBarrier();
        Trace(L"AcrImageIova", (ULONG)(arenaIova + ACR_VM_IMAGE_OFF));
        Trace(L"AcrImageIovaHi", (ULONG)((arenaIova + ACR_VM_IMAGE_OFF) >> 32));
        Trace(L"AcrImageBytes", ucDataSize);

        /* GM20B fb_gm20b.c: gm20b_fb_set_mmu_page_size() selects the
         * per-instance PDB big-page bit. Until this bit is set, our RAMIN
         * bit 11=0 is not sufficient to select the 128K table geometry.
         * See COMPARE-B3c-ja.md and NOTICE-GM20B-MMU.txt in the kit. */
        Step(57);
        {
            ULONG mmuBefore = Rd(B, 0x100C80), mmuAfter;
            Trace(L"AcrMmuPageCtrlBefore", mmuBefore);
            Wr(B, 0x100C80, mmuBefore | 0x00000800u);
            mmuAfter = Rd(B, 0x100C80);
            Trace(L"AcrMmuPageCtrlAfter", mmuAfter);
            Trace(L"AcrPreBootDmactl", Rd(B, PMU_FALCON + FALCON_DMACTL));
            Trace(L"AcrPreBootItfen", Rd(B, PMU_FALCON + FALCON_ITFEN));
            Trace(L"AcrPreBootTranscfg1", Rd(B, PMU_FALCON + 0xE04));
            TraceDurable(L"AcrPdbPageModeOk", (mmuAfter & 0x800u) ? 1u : 0u);
            if (!(mmuAfter & 0x800u)) { err = 0xE000006Cu; goto out; }
        }

        Step(50);   /* bind the PMU instance block to the GM20B virtual address space */
        if (!AcrVmBind(B, instIova)) { err = 0xE0000065u; goto out; }

        Step(51);   /* write the virtual DMA addresses into flcn_bl_dmem_desc */
        desc[9] = (ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF) >> 8;
        desc[15] = (ACR_GPU_VA_BASE + ACR_VM_IMAGE_OFF + dataBase) >> 8;
        Trace(L"AcrDescCodeDmaBase", desc[9]);
        Trace(L"AcrDescDataDmaBase", desc[15]);
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
            Trace(L"B3cPreAcrFifoIntr0", intrBefore);
            Trace(L"B3cPreAcrFaultId", idBefore);
            for (u = 0; u < 32; ++u) {
                if (!(idBefore & (1u << u))) continue;
                faultUnits[faultCount] = u;
                recBefore[faultCount][0] = u;
                for (j = 1; j < 5; ++j)
                    recBefore[faultCount][j] = Rd(B, 0x2800 + u * 0x10 + (j - 1) * 4);
                ++faultCount;
            }
            Trace(L"B3cPreAcrFaultRecCount", faultCount);
            if (faultCount)
                TraceBin(L"B3cPreAcrFaultRecs", recBefore, faultCount * sizeof(recBefore[0]));
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
            Trace(L"B3cPreAcrFaultIdAfterClear", idAfter);
            Trace(L"B3cPreAcrFifoIntr0AfterClear", intrAfter);
            Trace(L"B3cPreAcrFaultClearPolls", clearPolls);
            if (faultCount) {
                for (u = 0; u < faultCount; ++u) {
                    recAfter[u][0] = faultUnits[u];
                    for (j = 1; j < 5; ++j)
                        recAfter[u][j] = Rd(B, 0x2800 + faultUnits[u] * 0x10 + (j - 1) * 4);
                }
                TraceBin(L"B3cPreAcrFaultRecsAfterAck", recAfter, faultCount * sizeof(recAfter[0]));
                Trace(L"B3cPreAcrFaultDataPersist", RtlCompareMemory(recBefore, recAfter,
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
        Trace(L"AcrBootCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
        Trace(L"AcrBootMailbox0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
        Trace(L"AcrBootMailbox1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
        Trace(L"AcrBootIrq", Rd(B, PMU_FALCON + 0x004));
        Trace(L"AcrBootSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
        Trace(L"AcrBootPc", Rd(B, PMU_FALCON + 0x148));
        /* 0.4.6 (read only, apart from selecting the trace slot like nouveau's
         * gm200_flcn_tracepc): where the falcon is, its DMA state, and any
         * MC/SMMU or GPU MMU fault raised during the boot. */
        {
            ULONG tidx = Rd(B, PMU_FALCON + 0x148), nr, sp, pcs[8], rec[6][5], n = 0, id, u;
            nr = (tidx >> 16) & 0xFFu;
            Trace(L"B3cTraceIdx", tidx);
            Trace(L"B3cTraceNr", nr);
            if (nr > 8) nr = 8;
            for (sp = 0; sp < nr; ++sp) {
                Wr(B, PMU_FALCON + 0x148, sp);
                pcs[sp] = Rd(B, PMU_FALCON + 0x14C);
            }
            if (nr) TraceBin(L"B3cTracePc", pcs, nr * sizeof(ULONG));
            Trace(L"B3cDmaCmd", Rd(B, PMU_FALCON + FALCON_DMATRFCMD));
            Trace(L"B3cDmaBase", Rd(B, PMU_FALCON + FALCON_DMATRFBASE));
            Trace(L"B3cDmaMoffs", Rd(B, PMU_FALCON + FALCON_DMATRFMOFFS));
            Trace(L"B3cDmaFboffs", Rd(B, PMU_FALCON + FALCON_DMATRFFBOFFS));
            Trace(L"B3cDmactl", Rd(B, PMU_FALCON + FALCON_DMACTL));
            Trace(L"B3cMcInt", Rd(Dev->Mc, MC_INTSTATUS));
            Trace(L"B3cMcErr", Rd(Dev->Mc, MC_ERR_STATUS));
            Trace(L"B3cMcErrAdr", Rd(Dev->Mc, MC_ERR_ADR));
            Trace(L"B3cFifoIntr0", Rd(B, 0x2100));
            id = Rd(B, 0x259C);
            Trace(L"B3cFifoFaultId", id);
            for (u = 0; u < 32 && n < 6; ++u) {
                if (!(id & (1u << u))) continue;
                rec[n][0] = u;
                rec[n][1] = Rd(B, 0x2800 + u * 0x10);
                rec[n][2] = Rd(B, 0x2804 + u * 0x10);
                rec[n][3] = Rd(B, 0x2808 + u * 0x10);
                rec[n][4] = Rd(B, 0x280C + u * 0x10);
                ++n;
            }
            if (n) TraceBin(L"B3cFaultRecs", rec, n * 5 * sizeof(ULONG));
            /* 0.4.17 (read only): what state ACR left the LS falcons in.
             * SCTL bits [13:12] report the falcon security mode. The GR
             * falcons (FECS 0x409000, GPCCS 0x41A000) are read only when
             * PMC_ENABLE already has GR (bit 12) on; nothing is enabled here. */
            {
                ULONG pmcEn = Rd(B, NV_PMC_ENABLE);
                Trace(L"B3cLsPmcEnable", pmcEn);
                Trace(L"B3cPostAcrPmuCpuctl", Rd(B, PMU_FALCON + FALCON_CPUCTL));
                Trace(L"B3cPostAcrPmuSctl", Rd(B, PMU_FALCON + FALCON_SCTL));
                Trace(L"B3cLsPmuMb0", Rd(B, PMU_FALCON + FALCON_MAILBOX0));
                Trace(L"B3cLsPmuMb1", Rd(B, PMU_FALCON + FALCON_MAILBOX1));
                if (pmcEn & 0x1000u) {
                    Trace(L"B3cLsFecsCpuctl", Rd(B, 0x409000 + FALCON_CPUCTL));
                    Trace(L"B3cLsFecsSctl", Rd(B, 0x409000 + FALCON_SCTL));
                    Trace(L"B3cLsFecsMb0", Rd(B, 0x409000 + FALCON_MAILBOX0));
                    Trace(L"B3cLsGpccsCpuctl", Rd(B, 0x41A000 + FALCON_CPUCTL));
                    Trace(L"B3cLsGpccsSctl", Rd(B, 0x41A000 + FALCON_SCTL));
                    Trace(L"B3cLsGpccsMb0", Rd(B, 0x41A000 + FALCON_MAILBOX0));
                    Trace(L"B3cLsGrRead", 1);
                } else {
                    Trace(L"B3cLsGrRead", 0);
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
            }
        }
    } else {
        /* CPU-only WPR layout and readback; no GPU DMA or ACR/PMU start. */
        (VOID)BuildWprB3b(dmemSize, 0, NULL, NULL, NULL, NULL);
    }

    /* In launch mode the surrounding PowerTest path asserts GPU reset before
     * releasing the WPR/VM backing pages; otherwise reset the idle PMU here. */
    if (!RunAcr) PmuReset(B, L"B3aScrubPolls2");

out:
    if (RunAcr && !acrAttempted) TraceDurable(L"AcrBootResult", err);
    TraceDurable(L"B3aResult", err);
    if (boot != NULL) ExFreePoolWithTag(boot, SWGPU_TAG);
    if (wprImage != NULL) ExFreePoolWithTag(wprImage, SWGPU_TAG);
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

    /* ACR may leave signed Falcon firmware running. Always reset the GPU and
     * release the temporary DMA mappings before returning from the opt-in run. */
    if (Mode == 5) KeepOn = FALSE;

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
    if (NT_SUCCESS(st) && Mode == 4) ProbeB3a(Dev, FALSE);
    if (NT_SUCCESS(st) && Mode == 5) ProbeB3a(Dev, TRUE);
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
        AcrVmCleanupAfterGpuReset(Dev);
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
    if (mode >= 1 && mode <= 5) {     /* 5 = isolated B3c signed ACR boot */
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
