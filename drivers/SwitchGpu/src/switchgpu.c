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

#define SWGPU_VERSION      0x00000101u   /* 0.1.1 */

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

/* ------------------------------------------------------------ power test */

static NTSTATUS PowerTest(PSWGPU_DEVICE Dev, BOOLEAN KeepOn)
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

    Step(6);    /* reset deassert with the gpu clock briefly off (nvgpu) */
    Wr(Dev->Car, CAR_CLK_ENB_X_CLR, CLK_X_GPU);
    Wr(Dev->Car, CAR_RST_DEV_X_CLR, CLK_X_GPU);
    Wr(Dev->Car, CAR_CLK_ENB_X_SET, CLK_X_GPU);
    Us(10);

    Step(7);    /* MC flush, flush done */
    Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) | MC_HOTRESET_GPU);
    for (i = 0; i < 500 && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(10);
    Trace(L"McFlushPolls", i);
    Us(10);
    Wr(Dev->Mc, MC_HOTRESET_CTRL_1, Rd(Dev->Mc, MC_HOTRESET_CTRL_1) & ~MC_HOTRESET_GPU);
    Us(10);

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
    Trace(L"Chipset", (boot0 >> 20) & 0x1FF);
    Trace(L"ChipRev", boot0 & 0xFF);
    st = (((boot0 >> 20) & 0x1FF) == 0x12B) ? STATUS_SUCCESS : STATUS_DEVICE_HARDWARE_ERROR;
    Trace(L"Result", (ULONG)st);
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

    Trace(L"Version", SWGPU_VERSION);
    Trace(L"StartCount", ReadParam(L"StartCount", 0) + 1);
    st = OpenTarget(Device, Dev->VddId, &Dev->Vdd);
    Trace(L"OpenVddStatus", (ULONG)st);
    st = OpenTarget(Device, Dev->PmicId, &Dev->Pmic);
    Trace(L"OpenPmicStatus", (ULONG)st);

    Snapshot(Dev, FALSE);

    if (ReadParam(L"PowerTest", 0) == 1) {
        BOOLEAN keep = ReadParam(L"KeepOn", 0) == 1;
        LARGE_INTEGER t0, t1, f;
        TraceDurable(L"PowerTest", 0);      /* once: a hang does not repeat at the next boot */
        Trace(L"Result", 0xFFFFFFFFu);
        Trace(L"Boot0", 0);
        t0 = KeQueryPerformanceCounter(&f);
        (VOID)PowerTest(Dev, keep);
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
