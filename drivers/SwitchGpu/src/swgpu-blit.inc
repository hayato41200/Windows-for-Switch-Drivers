/* Included after F1. Reuse CE2, the resident GPFIFO and the E3 event.
 * The two-level SMMU and GPU small-page encodings match the GR arena.
 * Only borrowed mappings are transient. The page tables belong to E4. */
#define F2_WIDTH 1920u
#define F2_HEIGHT 1080u
#define F2_PITCH (F2_WIDTH * 4u)
#define F2_FRAME (F2_PITCH * F2_HEIGHT)
#define F2_REPEATS 20u
#define F2_BENCH_PUSH_OFF 0x0B0000u
#define F2_BENCH_PUSH_BYTES 0x10000u
#define F2_ERROR 0xE00000ABu
C_ASSERT(SWGPU_BLIT_MAX_RECTS * 11u + 21u < (PAGE_SIZE - 32u) / sizeof(ULONG));

C_ASSERT(GR_VM_PATCH_OFF + GR_VM_PATCH_BYTES <= F2_BENCH_PUSH_OFF);
C_ASSERT(F2_BENCH_PUSH_OFF + F2_BENCH_PUSH_BYTES <= GR_VM_PRIV_OFF);
C_ASSERT(F2_HEIGHT * 11u + 21u <= (F2_BENCH_PUSH_BYTES - 32u) / sizeof(ULONG));

typedef struct _F2_RUN {
    ULONG MaxWaitUs, MaxPolls, IrqWakes, MapPolls, UnmapPolls, AbortPolls;
    ULONG Pages[2];             /* Claude 0.15.0 (G3b): PTEs written per window, cleared at unmap */
    ULONG DstFirst;             /* 0.15.2 (G3c): first PTE of the destination in window 1 */
    BOOLEAN Persistent;         /* 0.15.2 (G3c): keep the destination mapped (F3 presents) */
    BOOLEAN Mapped, Issued;
} F2_RUN;

static BOOLEAN B3eIrqDrainBarrier(WDFINTERRUPT Interrupt, WDFCONTEXT Context);

/* The display path must never enter the regular two-second PRI ACK loop.
 * Check both stations after each GPU/BAR1 write and abort on a latched error. */
static BOOLEAN F2Write(PSWGPU_DEVICE Dev, volatile UCHAR *Base, ULONG Offset,
                       ULONG Value, ULONG (*writes)[3])
{
    (*writes)[0] = Offset;
    (*writes)[1] = Value;
    Wr(Base, Offset, Value);
    if (Base == Dev->Mc && (Offset == MC_SMMU_PTC_FLUSH || Offset == MC_SMMU_TLB_FLUSH))
        (*writes)[2] = Rd(Base, MC_SMMU_PTB_ASID);
    else if (Base == Dev->Car && Offset == CAR_RST_DEV_X_SET)
        (*writes)[2] = Rd(Base, CAR_RST_DEVICES_X);
    else if (Base == Dev->Bar0 && Offset == 0x100CBCu)
        (*writes)[2] = Rd(Base, 0x100C80u);
    else
        (*writes)[2] = Rd(Base, Offset);
    if (Base != Dev->Bar0 && Base != Dev->Bar1Cpu) return TRUE;
    return (Rd(Dev->Bar0, 0x00120058u) | Rd(Dev->Bar0, 0x0012005Cu)) == 0u;
}

#include "swgpu-gpcpll.inc"

typedef struct _SWGPU_F2_RESULT {
    ULONG Version, Result, Verified, GpuRounds, CpuRounds, GpuMBps, CpuMBps;
    ULONG MaxWaitUs, MaxPolls, IrqWakes, MapPolls, UnmapPolls, AbortPolls;
    ULONG GpuUs, CpuUs, FailOffset;
    ULONG GpuBatchUs, GpuLinesUs, GpuWcUs, CpuWcUs, BenchStage;
    ULONG PllCfg, PllCoeff, SelVco, Gpc2Out, BypassCtrl, Slowdown, RampStatus;
    ULONG PllCfg2, Dvfs0, Dvfs1, CntrCfg, CntrValue;
    ULONG OscCtrl, CarClkX, CarRstX, CarPllpOutc, RefKhz, Gpc2Khz, GpcKhz;
    ULONG Bypass, VoutRaw, VoutMv, MeasuredKhz, CntrSamples, IdleSlowdown;
    ULONG ClockSteps, RestoreResult;
    F2C_STEP Clock[F2C_STEPS];
} SWGPU_F2_RESULT;

static VOID F2FlushOuter(PSWGPU_DEVICE Dev)
{
    ULONG writes[2][3];
    KeMemoryBarrier();
    F2Write(Dev, Dev->Mc, MC_SMMU_PTC_FLUSH, 0u, &writes[0]);
    (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
    F2Write(Dev, Dev->Mc, MC_SMMU_TLB_FLUSH, 0u, &writes[1]);
    (VOID)Rd(Dev->Mc, MC_SMMU_PTB_ASID);
}

static BOOLEAN F2Invalidate(PSWGPU_DEVICE Dev, PULONG PollCount)
{
    volatile UCHAR *b = Dev->Bar0;
    ULONG writes[2][3], i;
    ULONG pdb = (ULONG)(((g_GrVm.Iova + GR_VM_GPU_PGD_OFF) >> 12) << 4);
    *PollCount = 0u;
    for (i = 0; i < 200u && !(Rd(b, 0x100C80u) & 0x00FF0000u); ++i) Us(5u);
    *PollCount = i;
    if (i == 200u || !F2Write(Dev, b, 0x100CB8u, pdb, &writes[0]) ||
        !F2Write(Dev, b, 0x100CBCu, 0x80000001u, &writes[1])) return FALSE;
    for (i = 0; i < 200u && !(Rd(b, 0x100C80u) & 0x00008000u); ++i) Us(5u);
    *PollCount += i;
    return i != 200u;
}

static BOOLEAN F2MdlValid(PMDL Mdl)
{
    ULONG bytes, offset;
    if (Mdl == NULL || Mdl->Next != NULL ||
        (Mdl->MdlFlags & (MDL_PAGES_LOCKED | MDL_SOURCE_IS_NONPAGED_POOL | MDL_IO_SPACE)) == 0)
        return FALSE;
    bytes = MmGetMdlByteCount(Mdl);
    offset = MmGetMdlByteOffset(Mdl);
    return offset < PAGE_SIZE && bytes != 0u && bytes <= SWGPU_BLIT_WINDOW_BYTES - offset &&
        (SIZE_T)(USHORT)Mdl->Size >= sizeof(MDL) +
        ((offset + bytes + PAGE_SIZE - 1u) / PAGE_SIZE) * sizeof(PFN_NUMBER);
}

static BOOLEAN F2RequestValid(const SWGPU_BLIT_REQUEST *Req)
{
    ULONG i;
    if (Req->Version != SWGPU_BLIT_VERSION || Req->Src == Req->Dst ||
        Req->Count == 0u || Req->Count > SWGPU_BLIT_MAX_RECTS ||
        Req->SrcPitch == 0u || Req->DstPitch == 0u || !F2MdlValid(Req->Src) ||
        !F2MdlValid(Req->Dst) || (Req->Src->MdlFlags & MDL_IO_SPACE))
        return FALSE;
    if (Req->DstLayout == SWGPU_BLIT_DST_BLOCK) {
        /* Claude 0.14.0 (G2a): block-linear destination, origin in DstOffset. */
        ULONG blockRows = 8u << Req->DstBlockHeightLog2;
        if (Req->DstBlockHeightLog2 > 5u || (Req->DstPitch & 63u) || Req->DstPitch > 0xFFFFu ||
            Req->DstHeight == 0u || Req->DstHeight > 0xFFFFu || (Req->DstHeight % blockRows) ||
            (Req->DstKind != 0u && Req->DstKind != 0xFEu) ||
            SWGPU_BL_BYTES(Req->DstPitch, Req->DstHeight) > MmGetMdlByteCount(Req->Dst))
            return FALSE;
        for (i = 0u; i < Req->Count; ++i) {
            const SWGPU_BLIT_RECT *r = &Req->Rect[i];
            ULONG x = r->DstOffset & 0xFFFFu, y = r->DstOffset >> 16;
            if (!r->Rows || !r->RowBytes || r->RowBytes > Req->SrcPitch ||
                x + r->RowBytes > Req->DstPitch || y + r->Rows > Req->DstHeight ||
                (ULONGLONG)r->SrcOffset + (ULONGLONG)(r->Rows - 1u) * Req->SrcPitch +
                    r->RowBytes > MmGetMdlByteCount(Req->Src))
                return FALSE;
        }
        return TRUE;
    }
    if (Req->DstLayout != SWGPU_BLIT_DST_PITCH || Req->DstKind != 0u) return FALSE;
    for (i = 0u; i < Req->Count; ++i) {
        const SWGPU_BLIT_RECT *r = &Req->Rect[i];
        if (!r->Rows || r->Rows > 0xFFFFu || !r->RowBytes ||
            r->RowBytes > Req->SrcPitch || r->RowBytes > Req->DstPitch ||
            (ULONGLONG)r->SrcOffset + (ULONGLONG)(r->Rows - 1u) * Req->SrcPitch +
                r->RowBytes > MmGetMdlByteCount(Req->Src) ||
            (ULONGLONG)r->DstOffset + (ULONGLONG)(r->Rows - 1u) * Req->DstPitch +
                r->RowBytes > MmGetMdlByteCount(Req->Dst))
            return FALSE;
    }
    return TRUE;
}

static BOOLEAN F2CanRun(PSWGPU_DEVICE Dev)
{
    return Dev->ResidentActive && g_ResidentOwner == Dev && !Dev->BlitReset &&
           !Dev->CopyInFlight && Dev->ResidentChannelEnabled &&
           Dev->ResidentRunlistSubmitted && Dev->ResidentInstBound &&
           Dev->Bar0 != NULL && Dev->Bar1Cpu != NULL && Dev->Bar1MapLen >= BAR1_D3_MAP_LEN &&
           g_AcrVm.PtbChanged && g_AcrVm.Arena != NULL && g_GrVm.Active &&
           g_GrVm.PtbChanged && g_GrVm.Arena != NULL &&
           (Rd(Dev->Bar0, NV_PMC_ENABLE) & F1_CE_ENABLE) != 0u;
}

/* Claude 0.15.2 (G3c): the display's scanout buffers are the same few
 * physically contiguous ranges on every present, so map each once and keep
 * it. Window 1 has 3 persistent slots of 4 MiB at 4, 8 and 12 MiB; anything
 * else (non-contiguous, larger) uses the start of window 1 per present as
 * before. A slot counts as mapped only if its first and last GPU and SMMU
 * PTEs read back exactly as this buffer needs them, so a reset, a G1 power
 * cycle, an arena reused from the pool or a legacy map over it is noticed
 * and simply mapped again. */
#define G3_SLOT_PAGES 1024u
#define G3_SLOTS      3u
static ULONG g_G3NextSlot, g_G3SlotHits, g_G3SlotMisses;

static ULONGLONG G3GpuPte(ULONG Index, ULONG Kind)
{
    ULONGLONG iova = (GR_VM_BLIT_BASE + (ULONGLONG)Index * PAGE_SIZE) | ACR_GM20B_IOMMU_TAG;
    return (iova >> 8) | 1ull | ((ULONGLONG)Kind << 36);
}

static BOOLEAN G3DstSlot(const SWGPU_BLIT_REQUEST *Req, PPFN_NUMBER Pfns, ULONG Count, PULONG First)
{
    volatile ULONG *outerPt = (volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_BLIT_SMMU_PT);
    volatile ULONGLONG *gpuPt = (volatile ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_BLIT_GPU_PGT);
    const ULONG base = SWGPU_BLIT_WINDOW_BYTES / PAGE_SIZE;
    ULONG s, i;
    *First = base;
    if (Count == 0u || Count > G3_SLOT_PAGES) return FALSE;
    for (i = 1u; i < Count; ++i) if (Pfns[i] != Pfns[0] + i) return FALSE;
    for (s = 0u; s < G3_SLOTS; ++s) {
        ULONG f = base + (s + 1u) * G3_SLOT_PAGES, l = f + Count - 1u;
        if (outerPt[f] == ((ULONG)Pfns[0] | SMMU_ATTR_RWNS) &&
            outerPt[l] == ((ULONG)Pfns[Count - 1u] | SMMU_ATTR_RWNS) &&
            gpuPt[f] == G3GpuPte(f, Req->DstKind) && gpuPt[l] == G3GpuPte(l, Req->DstKind) &&
            (Count == G3_SLOT_PAGES || gpuPt[l + 1u] == 0u)) {
            *First = f;
            ++g_G3SlotHits;
            return TRUE;
        }
    }
    s = g_G3NextSlot++ % G3_SLOTS;
    *First = base + (s + 1u) * G3_SLOT_PAGES;
    /* A shorter buffer must not leave the old one's tail mapped. */
    RtlZeroMemory((PULONGLONG)gpuPt + *First, G3_SLOT_PAGES * sizeof(ULONGLONG));
    RtlZeroMemory((PULONG)outerPt + *First, G3_SLOT_PAGES * sizeof(ULONG));
    ++g_G3SlotMisses;
    return FALSE;
}

static BOOLEAN F2Map(PSWGPU_DEVICE Dev, const SWGPU_BLIT_REQUEST *Req, F2_RUN *Run)
{
    volatile ULONG *outerPd = (volatile ULONG *)g_AcrVm.Arena;
    volatile ULONG *outerPt = (volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_BLIT_SMMU_PT);
    volatile ULONGLONG *gpuPd = (volatile ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGD_OFF);
    volatile ULONGLONG *gpuPt = (volatile ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_BLIT_GPU_PGT);
    const ULONG outerIndex = GR_VM_BLIT_BASE >> 22, gpuIndex = GR_VM_BLIT_BASE >> 27;
    ULONG set, i;
    /* 0.15.2 (G3c): a persistent destination keeps the PDEs set between presents. */
    if (!Run->Persistent && gpuPd[gpuIndex] != 0u &&
        gpuPd[gpuIndex] != (((g_GrVm.Iova + GR_VM_BLIT_GPU_PGT) << 24) | (5ull << 32))) return FALSE;
    for (set = 0u; set < 2u; ++set) {
        PMDL mdl = set == 0u ? Req->Src : Req->Dst;
        PPFN_NUMBER pfns = MmGetMdlPfnArray(mdl);
        ULONG count = (MmGetMdlByteOffset(mdl) + MmGetMdlByteCount(mdl) + PAGE_SIZE - 1u) / PAGE_SIZE;
        for (i = 0u; i < count; ++i)
            if (pfns[i] >= (1ull << 22)) return FALSE;
    }
    for (set = 0u; set < 2u; ++set) {
        PMDL mdl = set == 0u ? Req->Src : Req->Dst;
        PPFN_NUMBER pfns = MmGetMdlPfnArray(mdl);
        ULONG count = (MmGetMdlByteOffset(mdl) + MmGetMdlByteCount(mdl) + PAGE_SIZE - 1u) / PAGE_SIZE;
        ULONG first = set * (SWGPU_BLIT_WINDOW_BYTES / PAGE_SIZE);
        if (set == 1u && Run->Persistent && G3DstSlot(Req, pfns, count, &first)) {
            Run->DstFirst = first;          /* already mapped there: nothing to write */
            continue;
        }
        if (set == 1u) Run->DstFirst = first;
        /* A persistent slot stays mapped: unmap clears only what is per present. */
        Run->Pages[set] = (set == 1u && Run->Persistent && first != SWGPU_BLIT_WINDOW_BYTES / PAGE_SIZE) ? 0u : count;
        for (i = 0u; i < count; ++i) {
            ULONGLONG iova = (GR_VM_BLIT_BASE + (ULONGLONG)(first + i) * PAGE_SIZE) | ACR_GM20B_IOMMU_TAG;
            outerPt[first + i] = (ULONG)pfns[i] | SMMU_ATTR_RWNS;
            /* GR arena's valid small PTE encoding; G2a: kind [43:36] of the destination. */
            gpuPt[first + i] = (iova >> 8) | 1ull | (set == 1u ? (ULONGLONG)Req->DstKind << 36 : 0ull);
        }
    }
    KeMemoryBarrier();
    for (i = 0u; i < GR_VM_BLIT_PDES; ++i) {
        ULONG ptPa = g_GrVm.Pa.LowPart + GR_VM_BLIT_SMMU_PT + i * PAGE_SIZE;
        outerPd[outerIndex + i] = (ptPa >> 12) | SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
    }
    gpuPd[gpuIndex] = ((g_GrVm.Iova + GR_VM_BLIT_GPU_PGT) << 24) | (5ull << 32);
    Run->Mapped = TRUE;
    F2FlushOuter(Dev);
    return F2Invalidate(Dev, &Run->MapPolls);
}

/* Revoke both namespaces before returning borrowed pages. The caller has
 * either seen the CE fence or reset and drained the GPU client. */
static BOOLEAN F2Unmap(PSWGPU_DEVICE Dev, F2_RUN *Run)
{
    PULONG outerPd = (PULONG)g_AcrVm.Arena;
    PULONGLONG gpuPd = (PULONGLONG)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGD_OFF);
    if (!Run->Mapped) return TRUE;
    if (!Run->Persistent || Dev->BlitReset) {   /* 0.15.2: persistent slots keep the PDEs */
        gpuPd[GR_VM_BLIT_BASE >> 27] = 0u;
        RtlZeroMemory(outerPd + (GR_VM_BLIT_BASE >> 22), GR_VM_BLIT_PDES * sizeof(ULONG));
    }
    /* Claude 0.15.0 (G3b): clear only the PTEs this run wrote (F2Map writes
     * exactly Pages[set] entries from each window start; the rest stay zero).
     * 0.14 cleared 256 KB + 32 KB per present: ~0.3 ms on hardware. */
    {
        ULONG set;
        for (set = 0u; set < 2u; ++set) {
            ULONG first = set * (SWGPU_BLIT_WINDOW_BYTES / PAGE_SIZE), n = Run->Pages[set];
            if (n > SWGPU_BLIT_WINDOW_BYTES / PAGE_SIZE) n = SWGPU_BLIT_WINDOW_BYTES / PAGE_SIZE;
            RtlZeroMemory((PULONGLONG)((PUCHAR)g_GrVm.Arena + GR_VM_BLIT_GPU_PGT) + first, (SIZE_T)n * sizeof(ULONGLONG));
            RtlZeroMemory((PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_BLIT_SMMU_PT) + first, (SIZE_T)n * sizeof(ULONG));
            Run->Pages[set] = 0u;
        }
    }
    F2FlushOuter(Dev);
    Run->Mapped = FALSE;
    return Dev->BlitReset || F2Invalidate(Dev, &Run->UnmapPolls);
}

/* Abort only GPU DMA. Rail/clock restoration stays in ResidentStop. If
 * the reset or MC client drain cannot be confirmed, quarantine the backing
 * instead of handing potentially live DMA pages back to the allocator. */
static BOOLEAN F2Abort(PSWGPU_DEVICE Dev, F2_RUN *Run)
{
    ULONG writes[5][3], i;
    BOOLEAN reset;
    InterlockedExchange(&Dev->BlitOnline, 0);
    InterlockedExchange(&Dev->E3IsrActive, 0);
    if (Dev->StallInterrupt != NULL)
        (VOID)WdfInterruptSynchronize(Dev->StallInterrupt, B3eIrqDrainBarrier, NULL);
    if (Dev->NonstallInterrupt != NULL)
        (VOID)WdfInterruptSynchronize(Dev->NonstallInterrupt, B3eIrqDrainBarrier, NULL);
    (VOID)F2Write(Dev, Dev->Bar0, PMC_INTR_EN_STALL, 0u, &writes[0]);
    (VOID)F2Write(Dev, Dev->Bar0, PMC_INTR_EN_NONSTALL, 0u, &writes[1]);
    (VOID)F2cReturnToBypass(Dev);
    (VOID)F2Write(Dev, Dev->Mc, MC_HOTRESET_CTRL_1,
        Rd(Dev->Mc, MC_HOTRESET_CTRL_1) | MC_HOTRESET_GPU, &writes[2]);
    for (i = 0u; i < 200u && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(5u);
    Run->AbortPolls = i;
    (VOID)F2Write(Dev, Dev->Pmc, PMC_GPU_RG_CNTRL, 1u, &writes[3]);
    Us(10u);
    (VOID)F2Write(Dev, Dev->Car, CAR_RST_DEV_X_SET, CLK_X_GPU, &writes[4]);
    reset = (writes[4][2] & CLK_X_GPU) != 0u;
    for (i = 0u; i < 200u && !(Rd(Dev->Mc, MC_HOTRESET_STAT_1) & MC_HOTRESET_GPU); ++i) Us(5u);
    Run->AbortPolls += i;
    Dev->BlitReset = TRUE;  /* BAR0 is no longer usable. */
    Dev->BlitUnsafe = !reset || i == 200u;
    return !Dev->BlitUnsafe;
}

static BOOLEAN F2L2(PSWGPU_DEVICE Dev, ULONG (*writes)[3], PULONG Polls)
{
    ULONG i, step;
    const ULONG offsets[2] = {0x70010u, 0x70004u};
    *Polls = 0u;
    for (step = 0u; step < RTL_NUMBER_OF(offsets); ++step) {
        if (!F2Write(Dev, Dev->Bar0, offsets[step], 1u, &writes[step])) return FALSE;
        for (i = 0u; i < 200u && (Rd(Dev->Bar0, offsets[step]) & 3u); ++i) Us(5u);
        *Polls += i;
        if (i == 200u) return FALSE;
    }
    return TRUE;
}

static VOID F2SampleWait(F2_RUN *Run, LARGE_INTEGER Start, LONGLONG Frequency, ULONG Polls)
{
    ULONGLONG us = (ULONGLONG)(KeQueryPerformanceCounter(NULL).QuadPart - Start.QuadPart)
        * 1000000ull / (ULONGLONG)Frequency;
    if (us > Run->MaxWaitUs) Run->MaxWaitUs = us > MAXULONG ? MAXULONG : (ULONG)us;
    if (Polls > Run->MaxPolls) Run->MaxPolls = Polls;
}

/* Sequence from clb0b5.h/clb06f.h and F1. Launch non-pipelined rectangles
 * in channel order, release the CE fence on the last, then acquire it in
 * the host stream. The host completion/IRQ is after that acquire. */
static NTSTATUS F2SubmitPlan(PSWGPU_DEVICE Dev, const SWGPU_BLIT_REQUEST *Req,
                            F2_RUN *Run, BOOLEAN NeedIrq, ULONG Repeat, BOOLEAN SplitLines)
{
    const BOOLEAN batch = Repeat > 1u || SplitLines;
    const ULONG pushOff = batch ? F2_BENCH_PUSH_OFF : GR_VM_F1_PUSH_OFF;
    const ULONG pushBytes = batch ? F2_BENCH_PUSH_BYTES : PAGE_SIZE;
    const ULONG waitMs = batch ? 2000u : 45u;
    const ULONG launchCount = SplitLines ? Req->Rect[0].Rows : Req->Count * Repeat;
    const ULONG pushVa = GR_VM_GPU_VA + pushOff;
    const ULONG ceSemVa = pushVa + pushBytes - 32u, hostSemVa = ceSemVa + 16u;
    const ULONG getOffset = BAR1_D3_LOW_VA + 0x88u, putOffset = BAR1_D3_LOW_VA + 0x8Cu;
    const ULONG ringEntries = PAGE_SIZE / 8u;
    PULONG push = (PULONG)((PUCHAR)g_GrVm.Arena + pushOff);
    PULONG ring = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_GPFIFO_OFF);
    volatile ULONG *sem = (volatile ULONG *)((PUCHAR)push + pushBytes - 32u);
    ULONG put = Rd(Dev->Bar1Cpu, putOffset), next, n = 0u, i, polls = 0u, l2Polls;
    ULONG writes[3][3];
    LARGE_INTEGER start, freq, timeout;
    BOOLEAN useIrq = Dev->InterruptSourcesEnabled &&
        (Rd(Dev->Bar0, PMC_INTR_EN_NONSTALL) & 1u) &&
        (Rd(Dev->Bar0, FIFO_INTR_EN_1) & FIFO_INTR_0_CHANNEL_INTR);
    NTSTATUS status = STATUS_IO_TIMEOUT;
    const BOOLEAN block = Req->DstLayout == SWGPU_BLIT_DST_BLOCK;
    if (Repeat == 0u || Repeat > F2_REPEATS || (SplitLines && Req->Count != 1u) ||
        (block && (Repeat != 1u || SplitLines)) ||
        launchCount > ((pushBytes - 32u) / sizeof(ULONG) - 21u) / (block ? 18u : 11u))
        return STATUS_INVALID_PARAMETER;
    if (NeedIrq && !useIrq) return STATUS_DEVICE_NOT_READY;
    if (put >= ringEntries || Rd(Dev->Bar1Cpu, getOffset) != put) return STATUS_DEVICE_BUSY;
    if (Rd(Dev->Bar0, 0x0000259Cu) || (Rd(Dev->Bar0, FIFO_INTR_0) & 0x10000000u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    next = (put + 1u) % ringEntries;
    sem[0] = 0u; sem[4] = 0u;
    push[n++] = 0x60010000u | (F1_COPY_SUBCHANNEL << 13);
    push[n++] = 0xB0B5u;
    push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x0240u, 3u);
    push[n++] = 0u; push[n++] = ceSemVa; push[n++] = 0xF2000001u;
    for (i = 0u; i < launchCount; ++i) {
        const SWGPU_BLIT_RECT *r = &Req->Rect[SplitLines ? 0u : i % Req->Count];
        const ULONG row = SplitLines ? i : 0u;
        if (block) {
            /* clb0b5.h SET_DST_BLOCK_SIZE..SET_DST_ORIGIN: WIDTH ONE_GOB, HEIGHT
             * log2 GOBs [7:4], DEPTH ONE_GOB, GOB_HEIGHT FERMI_8 [15:12] = 1. */
            push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x070Cu, 6u);
            push[n++] = (Req->DstBlockHeightLog2 << 4) | (1u << 12);
            push[n++] = Req->DstPitch; push[n++] = Req->DstHeight; push[n++] = 1u; push[n++] = 0u;
            push[n++] = r->DstOffset;               /* origin: Y [31:16], X bytes [15:0] */
        }
        push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x0400u, 8u);
        push[n++] = 0u;
        push[n++] = GR_VM_BLIT_BASE + MmGetMdlByteOffset(Req->Src) + r->SrcOffset + row * Req->SrcPitch;
        push[n++] = 0u;
        push[n++] = GR_VM_BLIT_BASE + Run->DstFirst * PAGE_SIZE + MmGetMdlByteOffset(Req->Dst) +
            (block ? 0u : r->DstOffset + row * Req->DstPitch);
        push[n++] = Req->SrcPitch; push[n++] = Req->DstPitch;
        push[n++] = r->RowBytes; push[n++] = SplitLines ? 1u : r->Rows;
        push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x0300u, 1u);
        /* LAUNCH_DMA: bit 8 DST_MEMORY_LAYOUT 1 = pitch, 0 = block linear. */
        push[n++] = (i + 1u == launchCount ? 0x38Eu : 0x386u) & (block ? ~0x100u : ~0u);
    }
    push[n++] = F1Method(0u, 0x0010u, 4u);
    push[n++] = 0u; push[n++] = ceSemVa; push[n++] = 0xF2000001u; push[n++] = 1u;
    push[n++] = F1Method(0u, 0x0010u, 4u);
    push[n++] = 0u; push[n++] = hostSemVa; push[n++] = 0xF2000002u; push[n++] = 0x01100002u;
    if (useIrq) {
        push[n++] = 0x20010008u; push[n++] = 0u;
        KeClearEvent(&Dev->E3Event);
    }
    ring[put * 2u] = pushVa;
    ring[put * 2u + 1u] = n << 10; /* GP_ENTRY1_LENGTH [30:10], in dwords */
    KeMemoryBarrier();
    if (!F2L2(Dev, writes, &l2Polls)) {
        if (l2Polls > Run->MaxPolls) Run->MaxPolls = l2Polls;
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    polls = l2Polls;
    start = KeQueryPerformanceCounter(&freq);
    Run->Issued = TRUE;
    if (!F2Write(Dev, Dev->Bar1Cpu, putOffset, next, &writes[2]) || writes[2][2] != next) {
        F2SampleWait(Run, start, freq.QuadPart, polls);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    if (useIrq) {
        timeout.QuadPart = -(LONGLONG)waitMs * 10000ll;
        if (KeWaitForSingleObject(&Dev->E3Event, Executive, KernelMode, FALSE, &timeout) == STATUS_SUCCESS)
            ++Run->IrqWakes;
    }
    for (i = 0u; i < (batch ? 2000u : 1000u); ++i) {
        if (!F2L2(Dev, writes, &l2Polls)) {
            polls += l2Polls;
            status = STATUS_DEVICE_HARDWARE_ERROR;
            break;
        }
        polls += l2Polls + 1u;
        KeMemoryBarrier();
        if (sem[0] == 0xF2000001u && sem[4] == 0xF2000002u && Rd(Dev->Bar1Cpu, getOffset) == next) {
            Run->Issued = FALSE;
            status = STATUS_SUCCESS;
            break;
        }
        if (Rd(Dev->Bar0, 0x0000259Cu) ||
            KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart >= freq.QuadPart * waitMs / 1000)
            break;
        if (batch) Ms(1u);
        else Us(10u);
    }
    F2SampleWait(Run, start, freq.QuadPart, polls);
    return status;
}

static NTSTATUS F2Submit(PSWGPU_DEVICE Dev, const SWGPU_BLIT_REQUEST *Req,
                         F2_RUN *Run, BOOLEAN NeedIrq)
{
    return F2SubmitPlan(Dev, Req, Run, NeedIrq, 1u, FALSE);
}

static NTSTATUS F2Finish(PSWGPU_DEVICE Dev, F2_RUN *Run, NTSTATUS Status)
{
    if (Run->Issued && !F2Abort(Dev, Run)) return SWGPU_BLIT_DMA_UNSAFE;
    if (!F2Unmap(Dev, Run)) {
        /* No issued DMA survives, but do not reuse uncertain TLB state. */
        (VOID)F2Abort(Dev, Run);
        return STATUS_DEVICE_HARDWARE_ERROR;
    }
    return Status;
}

static BOOLEAN F2Verify(PUCHAR Src, PUCHAR Dst, const SWGPU_BLIT_REQUEST *Req, PULONG Fail)
{
    const SWGPU_BLIT_RECT *r = &Req->Rect[0];
    ULONG row = 0u, i, pos = 0u;
    for (; row < r->Rows; ++row) {
        ULONG first = r->DstOffset + row * Req->DstPitch;
        ULONG src = r->SrcOffset + row * Req->SrcPitch;
        for (; pos < first; ++pos)
            if (Dst[pos] != 0u) { *Fail = pos; return FALSE; }
        for (i = 0u; i < r->RowBytes; ++i, ++pos)
            if (Dst[pos] != Src[src + i]) { *Fail = pos; return FALSE; }
    }
    for (; pos < F2_FRAME; ++pos)
        if (Dst[pos] != 0u) { *Fail = pos; return FALSE; }
    return TRUE;
}

static ULONG F2ElapsedUs(LARGE_INTEGER Start, LARGE_INTEGER Frequency)
{
    ULONGLONG us = (ULONGLONG)(KeQueryPerformanceCounter(NULL).QuadPart - Start.QuadPart) *
        1000000ull / (ULONGLONG)Frequency.QuadPart;
    return us > MAXULONG ? MAXULONG : (ULONG)us;
}

static ULONG F2PfnBreaks(PMDL Mdl)
{
    PPFN_NUMBER pfns = MmGetMdlPfnArray(Mdl);
    ULONG pages = (MmGetMdlByteOffset(Mdl) + MmGetMdlByteCount(Mdl) + PAGE_SIZE - 1u) / PAGE_SIZE;
    ULONG i, breaks = 0u;
    for (i = 1u; i < pages; ++i)
        if (pfns[i] != pfns[i - 1u] + 1u) ++breaks;
    return breaks;
}

/* Nouveau gm20b_clk_init()/pl_to_div(), GM20B hw_trim_gm20b.h.
 * The integer PLL result is nominal in the presence of SDM/DFS or ramping.
 * Only decode documented output-divider encodings. As described by
 * nvgpu gm20b_clk_get_gpcclk_clock_counter(), TRIM counts GPC cycles in
 * a programmed window of reference cycles. Observe an existing window
 * only; do not start a measurement or modify idle slowdown. */
static VOID F2ReadClock(PSWGPU_DEVICE Dev, SWGPU_F2_RESULT *Out)
{
    ULONG m, n, pl, divider, window;
    Out->PllCfg = Rd(Dev->Bar0, 0x137000u);
    Out->PllCoeff = Rd(Dev->Bar0, 0x137004u);
    Out->PllCfg2 = Rd(Dev->Bar0, 0x13700Cu);
    Out->Dvfs0 = Rd(Dev->Bar0, 0x137010u);
    Out->Dvfs1 = Rd(Dev->Bar0, 0x137014u);
    Out->Slowdown = Rd(Dev->Bar0, 0x13701Cu);
    Out->IdleSlowdown = Rd(Dev->Bar0, 0x020160u);
    Out->SelVco = Rd(Dev->Bar0, 0x137100u);
    Out->Gpc2Out = Rd(Dev->Bar0, 0x137250u);
    Out->BypassCtrl = Rd(Dev->Bar0, 0x137340u);
    Out->RampStatus = Rd(Dev->Bar0, 0x1328A0u);
    Out->CntrCfg = Rd(Dev->Bar0, 0x134124u);
    Out->CntrValue = Rd(Dev->Bar0, 0x134128u);
    Out->OscCtrl = Rd(Dev->Car, 0x050u);
    Out->CarClkX = Rd(Dev->Car, CAR_CLK_OUT_ENB_X);
    Out->CarRstX = Rd(Dev->Car, CAR_RST_DEVICES_X);
    Out->CarPllpOutc = Rd(Dev->Car, CAR_PLLP_OUTC);
    Out->VoutRaw = Dev->VoutOnRaw;
    if (Out->VoutRaw != MAXULONG)
        Out->VoutMv = (606250u + 6250u * (Out->VoutRaw & 0x7Fu) + 500u) / 1000u;

    /* The Tegra210 GPU ref is CAR PLL_G_REF. Assume its input is PLL_REF:
     * Switch OSC is 38.4 MHz (OSC_CTRL[31:28] = 5), with the CAR PLL
     * reference divider in OSC_CTRL[27:26]. This is distinct from CLK_M. */
    if ((Out->OscCtrl >> 28) == 5u)
        Out->RefKhz = 38400u >> ((Out->OscCtrl >> 26) & 3u);
    Out->Bypass = !(Out->SelVco & 1u) || (Out->BypassCtrl & 1u);
    m = Out->PllCoeff & 0xFFu;
    n = (Out->PllCoeff >> 8) & 0xFFu;
    pl = (Out->PllCoeff >> 16) & 0x3Fu;
    divider = (Out->Gpc2Out >> 8) & 0x3Fu;
    /* VCODIV encodings 0 and 2 mean /1 and /2. Nouveau's GPC clock
     * rate is half of GPC2CLK. Bypass source frequency is unverified. */
    if (!Out->Bypass && (Out->PllCfg & 3u) == 1u && m && n && pl &&
        (divider == 0u || divider == 2u)) {
        Out->Gpc2Khz = Out->RefKhz * n / (m * pl * (divider ? 2u : 1u));
        Out->GpcKhz = Out->Gpc2Khz / 2u;
    }

    window = Out->CntrCfg & 0x3FFFu;
    if (Out->RefKhz >= 12000u && window != 0u && window <= 1024u &&
        (Out->CntrCfg & 0x01100000u) == 0x00100000u) {
        ULONG first, second;
        Us(200u);
        first = Rd(Dev->Bar0, 0x134128u);
        Us(100u);
        second = Rd(Dev->Bar0, 0x134128u);
        Out->CntrValue = second;
        Out->CntrSamples = 2u;
        if (Rd(Dev->Bar0, 0x134124u) == Out->CntrCfg && first == second &&
            (second & 0xFFFFFu) != 0u && (second & 0xFFFFFu) != 0xFFFFFu)
            Out->MeasuredKhz = (ULONG)((ULONGLONG)Out->RefKhz * (second & 0xFFFFFu) / window);
    }
}

/* Keep the full row copy out-of-line. KeMemoryBarrier exposes the stores
 * before sampling the round marker in the destination, including WC stores. */
static __declspec(noinline) VOID F2CpuCopyFrame(PUCHAR Src, PUCHAR Dst)
{
    ULONG row;
    for (row = 0u; row < F2_HEIGHT; ++row)
        RtlCopyMemory(Dst + row * F2_PITCH, Src + row * F2_PITCH, F2_PITCH);
    KeMemoryBarrier();
}

static NTSTATUS F2CpuBenchmark(PUCHAR Src, PUCHAR Dst, ULONG PatternBase,
                               PULONG Elapsed, PULONG Rounds)
{
    LARGE_INTEGER start, freq;
    ULONG i;
    start = KeQueryPerformanceCounter(&freq);
    for (i = 0u; i < F2_REPEATS; ++i) {
        ULONG pattern = PatternBase + i;
        *(PULONG)Src = pattern;
        F2CpuCopyFrame(Src, Dst);
        if (*(volatile ULONG *)Dst != pattern) return STATUS_DATA_ERROR;
        ++*Rounds;
    }
    *Elapsed = F2ElapsedUs(start, freq);
    return STATUS_SUCCESS;
}

/* Each test uses the same 8,294,400-byte allocations. Subrects use a
 * smaller destination pitch and distinct odd pixel/row origins. */
static const SWGPU_BLIT_RECT F2Rects[] = {
    {0u, 0u, F2_PITCH, F2_HEIGHT},
    {13u * F2_PITCH + 17u * 4u, 19u * (F2_PITCH - 16u) + 23u * 4u, 640u * 4u, 360u},
    {37u * F2_PITCH + 3u * 4u, 71u * (F2_PITCH - 16u) + 5u * 4u, 511u * 4u, 1u},
    {29u * F2_PITCH + 31u * 4u, 43u * (F2_PITCH - 16u) + 53u * 4u, 4u, 701u}
};

static NTSTATUS F2CheckRects(PSWGPU_DEVICE Dev, SWGPU_BLIT_REQUEST *Req, F2_RUN *Run,
                             PUCHAR Src, PUCHAR Dst, PULONG Verified, PULONG FailOffset)
{
    ULONG i;
    for (i = 0u; i < RTL_NUMBER_OF(F2Rects); ++i) {
        NTSTATUS status;
        Req->DstPitch = i == 0u ? F2_PITCH : F2_PITCH - 16u;
        Req->Rect[0] = F2Rects[i];
        RtlZeroMemory(Dst, F2_FRAME);
        KeFlushIoBuffers(Req->Dst, FALSE, TRUE);
        status = F2Submit(Dev, Req, Run, FALSE);
        if (!NT_SUCCESS(status)) return status;
        KeFlushIoBuffers(Req->Dst, TRUE, TRUE);
        if (!F2Verify(Src, Dst, Req, FailOffset)) return STATUS_DATA_ERROR;
        ++*Verified;
    }
    Req->DstPitch = F2_PITCH;
    Req->Rect[0] = F2Rects[0];
    return STATUS_SUCCESS;
}

static NTSTATUS F2TimeCopies(PSWGPU_DEVICE Dev, SWGPU_BLIT_REQUEST *Req, F2_RUN *Run,
                              PUCHAR Src, PUCHAR Dst, BOOLEAN Cached, PULONG Elapsed,
                              PULONG Rounds, PULONG FailOffset)
{
    LARGE_INTEGER start, freq;
    ULONG i;
    RtlZeroMemory(Dst, F2_FRAME);
    KeMemoryBarrier();
    if (Cached) KeFlushIoBuffers(Req->Dst, FALSE, TRUE);
    start = KeQueryPerformanceCounter(&freq);
    for (i = 0u; i < F2_REPEATS; ++i) {
        NTSTATUS status = F2Submit(Dev, Req, Run, FALSE);
        if (!NT_SUCCESS(status)) return status;
        ++*Rounds;
    }
    *Elapsed = F2ElapsedUs(start, freq);
    if (Cached) KeFlushIoBuffers(Req->Dst, TRUE, TRUE);
    return F2Verify(Src, Dst, Req, FailOffset) ? STATUS_SUCCESS : STATUS_DATA_ERROR;
}

static NTSTATUS F2cBenchRate(PSWGPU_DEVICE Dev, SWGPU_BLIT_REQUEST *Req, F2_RUN *Run,
                              PUCHAR Src, PUCHAR Cached, PUCHAR Wc, PMDL CachedMdl,
                              PMDL WcMdl, F2C_STEP *Clock)
{
    ULONG rounds = 0u;
    NTSTATUS status;
    Clock->Stage = 7u;
    status = F2cMeasure(Dev, Clock);
    if (!NT_SUCCESS(status)) return status;
    Clock->Stage = 8u;
    Req->Dst = CachedMdl;
    if (!F2Map(Dev, Req, Run)) return STATUS_DEVICE_HARDWARE_ERROR;
    status = F2CheckRects(Dev, Req, Run, Src, Cached, &Clock->Verified, &Clock->FailOffset);
    if (!NT_SUCCESS(status)) return status;
    Clock->Stage = 9u;
    status = F2TimeCopies(Dev, Req, Run, Src, Cached, TRUE, &Clock->CachedUs, &rounds, &Clock->FailOffset);
    if (!NT_SUCCESS(status)) return status;
    Clock->Stage = 10u;
    status = F2Finish(Dev, Run, STATUS_SUCCESS);
    if (!NT_SUCCESS(status)) return status;
    Req->Dst = WcMdl;
    if (!F2Map(Dev, Req, Run)) return STATUS_DEVICE_HARDWARE_ERROR;
    rounds = 0u;
    status = F2TimeCopies(Dev, Req, Run, Src, Wc, FALSE, &Clock->WcUs, &rounds, &Clock->FailOffset);
    if (!NT_SUCCESS(status)) return status;
    status = F2Finish(Dev, Run, STATUS_SUCCESS);
    if (NT_SUCCESS(status)) Clock->Stage = 0u;
    return status;
}

static const ULONG F2cLadder[] = {153600u, 307200u, 460800u, 614400u};
C_ASSERT(RTL_NUMBER_OF(F2cLadder) + 1u == F2C_STEPS);

static VOID F2Benchmark(PSWGPU_DEVICE Dev, SWGPU_F2_RESULT *Out)
{
    SWGPU_BLIT_REQUEST req = {0};
    F2_RUN run = {0};
    PUCHAR src = NULL, dst = NULL, wcDst = NULL;
    PMDL cachedMdl = NULL, wcMdl = NULL;
    PHYSICAL_ADDRESS low = {0}, high, skip = {0};
    LARGE_INTEGER start, freq;
    ULONG i, wcCpuRounds = 0u, wcGpuRounds = 0u, speedo;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    Out->FailOffset = MAXULONG;
    Out->Bypass = Out->VoutRaw = MAXULONG;
    Step(116);
    if (!F2CanRun(Dev)) { status = STATUS_DEVICE_NOT_READY; goto done; }
    Out->ClockSteps = 1u;
    if (!F2cPark(Dev)) { status = STATUS_DEVICE_HARDWARE_ERROR; goto done; }
    F2ReadClock(Dev, Out);
    if (Out->Bypass != 1u || (Out->PllCfg & F2C_ENABLE)) {
        status = STATUS_DEVICE_CONFIGURATION_ERROR; goto done;
    }
    Out->BenchStage = 1u;
    src = ExAllocatePool2(POOL_FLAG_NON_PAGED, F2_FRAME, SWGPU_TAG);
    dst = ExAllocatePool2(POOL_FLAG_NON_PAGED, F2_FRAME, SWGPU_TAG);
    if (src == NULL || dst == NULL) goto done;
    req.Src = IoAllocateMdl(src, F2_FRAME, FALSE, FALSE, NULL);
    cachedMdl = req.Dst = IoAllocateMdl(dst, F2_FRAME, FALSE, FALSE, NULL);
    if (req.Src == NULL || req.Dst == NULL) goto done;
    MmBuildMdlForNonPagedPool(req.Src);
    MmBuildMdlForNonPagedPool(req.Dst);
    Trace(L"F2SrcPfnBreaks", F2PfnBreaks(req.Src));
    Trace(L"F2DstPfnBreaks", F2PfnBreaks(req.Dst));
    req.Version = SWGPU_BLIT_VERSION;
    req.Count = 1u;
    req.SrcPitch = req.DstPitch = F2_PITCH;
    req.Rect[0] = F2Rects[0];
    if (!F2RequestValid(&req) || !F2Map(Dev, &req, &run)) {
        status = STATUS_DEVICE_HARDWARE_ERROR; goto done;
    }
    for (i = 0u; i < F2_FRAME; ++i) src[i] = F1Pattern(7u, i);
    KeFlushIoBuffers(req.Src, FALSE, TRUE);
    status = F2CheckRects(Dev, &req, &run, src, dst, &Out->Verified, &Out->FailOffset);
    if (!NT_SUCCESS(status)) goto done;
    Out->BenchStage = 2u;
    status = F2TimeCopies(Dev, &req, &run, src, dst, TRUE,
                          &Out->GpuUs, &Out->GpuRounds, &Out->FailOffset);
    if (!NT_SUCCESS(status)) goto done;
    (VOID)KeQueryPerformanceCounter(&freq);
    Out->BenchStage = 3u;
    RtlZeroMemory(dst, F2_FRAME);
    KeFlushIoBuffers(cachedMdl, FALSE, TRUE);
    start = KeQueryPerformanceCounter(NULL);
    status = F2SubmitPlan(Dev, &req, &run, FALSE, F2_REPEATS, FALSE);
    if (!NT_SUCCESS(status)) goto done;
    Out->GpuBatchUs = F2ElapsedUs(start, freq);
    KeFlushIoBuffers(cachedMdl, TRUE, TRUE);
    if (!F2Verify(src, dst, &req, &Out->FailOffset)) { status = STATUS_DATA_ERROR; goto done; }

    Out->BenchStage = 4u;
    RtlZeroMemory(dst, F2_FRAME);
    KeFlushIoBuffers(cachedMdl, FALSE, TRUE);
    start = KeQueryPerformanceCounter(NULL);
    status = F2SubmitPlan(Dev, &req, &run, FALSE, 1u, TRUE);
    if (!NT_SUCCESS(status)) goto done;
    Out->GpuLinesUs = F2ElapsedUs(start, freq);
    KeFlushIoBuffers(cachedMdl, TRUE, TRUE);
    if (!F2Verify(src, dst, &req, &Out->FailOffset)) { status = STATUS_DATA_ERROR; goto done; }
    status = F2Finish(Dev, &run, STATUS_SUCCESS);
    if (!NT_SUCCESS(status)) goto done;

    Out->BenchStage = 5u;
    status = F2CpuBenchmark(src, dst, 0xCA000000u, &Out->CpuUs, &Out->CpuRounds);
    if (NT_SUCCESS(status) && !F2Verify(src, dst, &req, &Out->FailOffset))
        status = STATUS_DATA_ERROR;
    if (!NT_SUCCESS(status)) goto done;

    Out->BenchStage = 6u;
    status = STATUS_INSUFFICIENT_RESOURCES;
    high.QuadPart = 0xFFFFFFFFll;
    wcMdl = MmAllocatePagesForMdlEx(low, high, skip, F2_FRAME, MmWriteCombined,
                                     MM_ALLOCATE_FULLY_REQUIRED);
    if (wcMdl == NULL || MmGetMdlByteCount(wcMdl) != F2_FRAME) goto done;
    wcDst = MmMapLockedPagesSpecifyCache(wcMdl, KernelMode, MmWriteCombined, NULL,
                                          FALSE, NormalPagePriority | MdlMappingNoExecute);
    if (wcDst == NULL) goto done;
    req.Dst = wcMdl;
    KeFlushIoBuffers(req.Src, FALSE, TRUE);
    if (!F2RequestValid(&req) || !F2Map(Dev, &req, &run)) {
        status = STATUS_DEVICE_HARDWARE_ERROR; goto done;
    }
    status = F2TimeCopies(Dev, &req, &run, src, wcDst, FALSE,
                          &Out->GpuWcUs, &wcGpuRounds, &Out->FailOffset);
    if (!NT_SUCCESS(status)) goto done;
    status = F2Finish(Dev, &run, STATUS_SUCCESS);
    if (!NT_SUCCESS(status)) goto done;

    Out->BenchStage = 7u;
    status = F2CpuBenchmark(src, wcDst, 0xCB000000u, &Out->CpuWcUs, &wcCpuRounds);
    if (NT_SUCCESS(status) && !F2Verify(src, wcDst, &req, &Out->FailOffset))
        status = STATUS_DATA_ERROR;
    if (!NT_SUCCESS(status)) goto done;
    Out->BenchStage = 0u;
    Out->Clock[0].Verified = Out->Verified;
    Out->Clock[0].CachedUs = Out->GpuUs;
    Out->Clock[0].WcUs = Out->GpuWcUs;
    Out->Clock[0].MeasuredKhz = Out->MeasuredKhz;
    Out->Clock[0].FailOffset = MAXULONG;
    KeFlushIoBuffers(req.Src, FALSE, TRUE);

    Step(117);
    speedo = F2cReadSpeedo();
    for (i = 0u; i < RTL_NUMBER_OF(F2cLadder); ++i) {
        F2C_STEP *clock = &Out->Clock[Out->ClockSteps++];
        clock->TargetKhz = F2cLadder[i];
        clock->FailOffset = MAXULONG;
        TraceDurable(L"F2cCurrentTargetKhz", clock->TargetKhz);
        status = F2cProgram(Dev, speedo, clock);
        clock->Result = (ULONG)status;
        if (status == STATUS_NOT_SUPPORTED) {
            status = STATUS_SUCCESS; /* CVB or unsupported power-on prerequisites */
            continue;
        }
        if (NT_SUCCESS(status)) {
            status = F2cBenchRate(Dev, &req, &run, src, dst, wcDst, cachedMdl, wcMdl, clock);
            clock->Result = (ULONG)status;
        }
        if (!NT_SUCCESS(status)) break;
    }

done:
    if (Out->ClockSteps == 1u && !NT_SUCCESS(status)) {
        Out->Clock[0].Result = (ULONG)status;
        Out->Clock[0].Verified = Out->Verified;
        Out->Clock[0].Stage = Out->BenchStage;
        Out->Clock[0].FailOffset = Out->FailOffset;
    }
    if (run.Mapped && status != SWGPU_BLIT_DMA_UNSAFE)
        status = F2Finish(Dev, &run, status);
    if (Dev->GpcManaged &&
        (Dev->BlitReset || !(NT_SUCCESS(status) ? F2cPark(Dev) : F2cReturnToBypass(Dev)))) {
        Out->RestoreResult = STATUS_DEVICE_HARDWARE_ERROR;
        if (!Dev->BlitReset) (VOID)F2Abort(Dev, &run);
        if (NT_SUCCESS(status)) status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    TraceErrorBin(L"F2cClockSteps", Out->Clock, Out->ClockSteps * sizeof(Out->Clock[0]));
    Trace(L"F2cRestoreResult", Out->RestoreResult);
    /* If reset cannot be confirmed, keep the pages implicated in DMA pinned. */
    if (status != SWGPU_BLIT_DMA_UNSAFE || req.Dst != wcMdl) {
        if (wcDst != NULL) MmUnmapLockedPages(wcDst, wcMdl);
        if (wcMdl != NULL) {
            MmFreePagesFromMdl(wcMdl);
            ExFreePool(wcMdl);
        }
    } else if (wcDst != NULL) {
        /* The mapping can go away while its MDL continues to pin the pages. */
        MmUnmapLockedPages(wcDst, wcMdl);
    }
    if (src != NULL && status != SWGPU_BLIT_DMA_UNSAFE) ExFreePoolWithTag(src, SWGPU_TAG);
    if (dst != NULL && (status != SWGPU_BLIT_DMA_UNSAFE || req.Dst != cachedMdl))
        ExFreePoolWithTag(dst, SWGPU_TAG);
    if (req.Src != NULL) IoFreeMdl(req.Src);
    if (cachedMdl != NULL) IoFreeMdl(cachedMdl);
    if (Out->GpuUs != 0u) Out->GpuMBps = F2_REPEATS * F2_FRAME / Out->GpuUs;
    if (Out->CpuUs != 0u) Out->CpuMBps = F2_REPEATS * F2_FRAME / Out->CpuUs;
    Out->Result = NT_SUCCESS(status) ? 0u : F2_ERROR;
    Out->MaxWaitUs = run.MaxWaitUs;
    Out->MaxPolls = run.MaxPolls;
    Out->IrqWakes = run.IrqWakes;
    Out->MapPolls = run.MapPolls;
    Out->UnmapPolls = run.UnmapPolls;
    Out->AbortPolls = run.AbortPolls;
    Trace(L"F2Status", (ULONG)status);
#define F2_TRACE(Field) Trace(L"F2" #Field, Out->Field)
    F2_TRACE(Verified); F2_TRACE(GpuRounds); F2_TRACE(CpuRounds); F2_TRACE(GpuMBps);
    F2_TRACE(CpuMBps); F2_TRACE(MaxWaitUs); F2_TRACE(MaxPolls); F2_TRACE(IrqWakes);
    F2_TRACE(MapPolls); F2_TRACE(UnmapPolls); F2_TRACE(AbortPolls);
    F2_TRACE(GpuUs); F2_TRACE(CpuUs); F2_TRACE(FailOffset);
    F2_TRACE(GpuBatchUs); F2_TRACE(GpuLinesUs); F2_TRACE(GpuWcUs); F2_TRACE(CpuWcUs);
    F2_TRACE(BenchStage);
    F2_TRACE(PllCfg); F2_TRACE(PllCoeff); F2_TRACE(SelVco); F2_TRACE(Gpc2Out);
    F2_TRACE(BypassCtrl); F2_TRACE(Slowdown); F2_TRACE(RampStatus);
    F2_TRACE(PllCfg2); F2_TRACE(Dvfs0); F2_TRACE(Dvfs1); F2_TRACE(CntrCfg); F2_TRACE(CntrValue);
    F2_TRACE(OscCtrl); F2_TRACE(CarClkX); F2_TRACE(CarRstX); F2_TRACE(CarPllpOutc);
    F2_TRACE(RefKhz); F2_TRACE(Gpc2Khz); F2_TRACE(GpcKhz); F2_TRACE(Bypass);
    F2_TRACE(VoutRaw); F2_TRACE(VoutMv); F2_TRACE(MeasuredKhz); F2_TRACE(CntrSamples);
    F2_TRACE(IdleSlowdown);
#undef F2_TRACE
    TraceDurable(L"F2Result", Out->Result);
}

/* Claude 0.15.0 (G3a): where a present's ~1 ms goes. Sums of microseconds
 * for map, submit + wait, finish (unmap), and bytes; flushed every 64. */
static ULONGLONG g_G3Us[3], g_G3Bytes;
static ULONG g_G3Max[3], g_G3Count;

static VOID G3Account(const SWGPU_BLIT_REQUEST *Req, LARGE_INTEGER T0, LARGE_INTEGER T1,
                      LARGE_INTEGER T2, LARGE_INTEGER T3, LONGLONG Freq)
{
    LONGLONG d[3];
    ULONG i;
    d[0] = T1.QuadPart - T0.QuadPart; d[1] = T2.QuadPart - T1.QuadPart; d[2] = T3.QuadPart - T2.QuadPart;
    for (i = 0u; i < 3u; ++i) {
        ULONG us = (ULONG)(d[i] * 1000000ll / Freq);
        g_G3Us[i] += us;
        if (us > g_G3Max[i]) g_G3Max[i] = us;
    }
    for (i = 0u; i < Req->Count; ++i) g_G3Bytes += (ULONGLONG)Req->Rect[i].RowBytes * Req->Rect[i].Rows;
    if ((++g_G3Count & 63u) == 0u) {
        Trace(L"G3Count", g_G3Count);
        Trace(L"G3MapAvgUs", (ULONG)(g_G3Us[0] / g_G3Count));
        Trace(L"G3SubmitAvgUs", (ULONG)(g_G3Us[1] / g_G3Count));
        Trace(L"G3FinishAvgUs", (ULONG)(g_G3Us[2] / g_G3Count));
        Trace(L"G3MapMaxUs", g_G3Max[0]);
        Trace(L"G3SubmitMaxUs", g_G3Max[1]);
        Trace(L"G3FinishMaxUs", g_G3Max[2]);
        Trace(L"G3AvgKB", (ULONG)(g_G3Bytes / g_G3Count / 1024u));
        Trace(L"G3SlotHits", g_G3SlotHits);
        Trace(L"G3SlotMisses", g_G3SlotMisses);
    }
}

static NTSTATUS F3Present(PSWGPU_DEVICE Dev, const SWGPU_BLIT_REQUEST *Req)
{
    F2_RUN run = {0};
    NTSTATUS status;
    LARGE_INTEGER t0, t1, t2, t3, freq;
    if (!Dev->InterruptSourcesEnabled || !F2CanRun(Dev)) {
        /* Claude 0.12.6: F3 on hardware got 18 NOT_READY while resident.
         * Record which condition failed (bit set = condition not met). */
        ULONG why = 0u;
        if (!Dev->InterruptSourcesEnabled) why |= 0x1u;
        if (!Dev->ResidentActive) why |= 0x2u;
        if (g_ResidentOwner != Dev) why |= 0x4u;
        if (Dev->BlitReset) why |= 0x8u;
        if (Dev->CopyInFlight) why |= 0x10u;
        if (!Dev->ResidentChannelEnabled) why |= 0x20u;
        if (!Dev->ResidentRunlistSubmitted) why |= 0x40u;
        if (!Dev->ResidentInstBound) why |= 0x80u;
        if (Dev->Bar0 == NULL || Dev->Bar1Cpu == NULL || Dev->Bar1MapLen < BAR1_D3_MAP_LEN) why |= 0x100u;
        if (!g_AcrVm.PtbChanged || g_AcrVm.Arena == NULL) why |= 0x200u;
        if (!g_GrVm.Active || !g_GrVm.PtbChanged || g_GrVm.Arena == NULL) why |= 0x400u;
        if (Dev->Bar0 != NULL && (Rd(Dev->Bar0, NV_PMC_ENABLE) & F1_CE_ENABLE) == 0u) why |= 0x800u;
        Trace(L"F3NotReadyWhy", why);
        Trace(L"F3NotReadyCount", ReadParam(L"F3NotReadyCount", 0u) + 1u);
        return STATUS_DEVICE_NOT_READY;
    }
    if (Req->DstPitch > SWGPU_BLIT_WINDOW_BYTES || !F2RequestValid(Req))
        return STATUS_INVALID_PARAMETER;
    run.Persistent = TRUE;                  /* 0.15.2 (G3c) */
    t0 = KeQueryPerformanceCounter(&freq);
    KeFlushIoBuffers(Req->Src, FALSE, TRUE);
    /* The scanout destinations are write-combined. The CPU barrier before
     * submit and CE/L2 completion before return order both producers. */
    if (F2Map(Dev, Req, &run)) {
        t1 = KeQueryPerformanceCounter(NULL);
        status = F2Submit(Dev, Req, &run, TRUE);
    } else {
        t1 = KeQueryPerformanceCounter(NULL);
        status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    t2 = KeQueryPerformanceCounter(NULL);
    status = F2Finish(Dev, &run, status);
    t3 = KeQueryPerformanceCounter(NULL);
    if (NT_SUCCESS(status)) G3Account(Req, t0, t1, t2, t3, freq.QuadPart);
    Dev->BlitLastWaitUs = run.MaxWaitUs;
    Dev->BlitLastPolls = run.MaxPolls + run.MapPolls + run.UnmapPolls + run.AbortPolls;
    return status;
}
