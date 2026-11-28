/* Claude 0.17.0 (H2a): one GPU channel per VA space, user pushbuffers, fences.
 * Claude 0.18.0 (H2c): fault/hang recovery, see H2RecoverGpu.
 *
 * Included after swgpu-h1.inc. A handle that owns an H1 VA space (slot s) can
 * create one channel, chid s + 1, on the GR runlist next to the resident
 * kernel channel (chid 0). The channel has its own page directory:
 *
 *   PDE 3 (0x18000000)  own table: GR context copy (golden image) and patch
 *                       page, the shared GR global buffers and priv access map
 *                       (all privileged PTEs), then non-privileged GPFIFO,
 *                       trailer and fence pages at H2_CTL_VA.
 *   PDE 8 (0x40000000)  the slot's H1 user table (same pages as H1).
 *
 * Nothing else is mapped, so a user pushbuffer can reach only its own H1
 * allocations and its own control pages. The kernel appends a host
 * semaphore release (with WFI) after every user pushbuffer; WAIT compares
 * that fence. The channel block (117 pages, NonCached) is reached by the
 * GPU through the H1 meta SMMU window at IOVA 0x48000000.
 *
 * While a channel exists G1 keeps the GPU on. A resident stop, reset or
 * device stop loses the channels: SUBMIT then returns STATUS_DEVICE_REMOVED
 * and cleanup only frees memory (the GPU was reset). Hangs and MMU faults of
 * a user channel are recovered in H2c; here WAIT just times out. */

#define IOCTL_SWGPU_H2_CHANNEL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80C, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_H2_SUBMIT  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80D, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_H2_WAIT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80E, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define H2_BLOCK_PAGES     117u
#define H2_BLK_PGD         0x00000u   /* 64 KiB, 8192 PDEs */
#define H2_BLK_PT3         0x10000u   /* 256 KiB small-page table for PDE 3 */
#define H2_BLK_INST        0x50000u
#define H2_BLK_CTX         0x51000u   /* 128 KiB GR context */
#define H2_BLK_PATCH       0x71000u
#define H2_BLK_GPFIFO      0x72000u
#define H2_BLK_TRAILER     0x73000u
#define H2_BLK_FENCE       0x74000u
#define H2_META_FIRST      H1_META_MAPPED          /* meta SMMU index of slot 0's block */
#define H2_CTL_VA          (GR_VM_GPU_VA + 0x200000u)  /* GPFIFO, trailer +4K, fence +8K */
#define H2_GPFIFO_ENTRIES  512u                    /* limit2 = 9 */
#define H2_TRAILERS        128u
#define H2_MAX_INFLIGHT    64u
#define H2_TRAILER_DWORDS  5u
#define H2_LOST            STATUS_DEVICE_REMOVED
#define PTE_PRIV           2ull
#define H2_FAULTED         STATUS_IO_DEVICE_ERROR   /* 0.18.0: this channel was blamed for a fault/hang */
#define H2_HANG_MS         3000u
#define H2_RECOVER_BURST   5u
#define H2_REASON_FAULT    1u
#define H2_REASON_HANG     2u
#define H2_REASON_RESET    3u
#define H2_REASON_G1       4u

C_ASSERT(H2_BLK_FENCE + PAGE_SIZE == H2_BLOCK_PAGES * PAGE_SIZE);
C_ASSERT(H2_META_FIRST + H1_SLOTS * H2_BLOCK_PAGES <= 1024u);
C_ASSERT(GR_VM_PATCH_OFF + PAGE_SIZE <= 0x100000u);
C_ASSERT(((H2_CTL_VA >> 27) == (GR_VM_GPU_VA >> 27)));

typedef struct _SWGPU_H2_CHANNEL_IN { ULONG Version, Op; } SWGPU_H2_CHANNEL_IN;  /* Op 1 create, 2 destroy */
typedef struct _SWGPU_H2_CHANNEL_OUT {
    ULONG Version, Result, Chid, Slot, GpfifoEntries, RunlistCount, RunlistPolls, PreemptPolls;
    ULONG CcsrChannel, CcsrInst, EngineStatus, FifoIntr, PbdmaIntr, MmuFault, CtxMagic, PrivPtes;
} SWGPU_H2_CHANNEL_OUT;
typedef struct _SWGPU_H2_SUBMIT_IN { ULONG Version, PushVa, PushDwords, Flags; } SWGPU_H2_SUBMIT_IN; /* Flags bit0: wait 2 s */
typedef struct _SWGPU_H2_SUBMIT_OUT {
    ULONG Version, Result, Fence, Completed, Put, Get, WaitUs, CcsrChannel;
    ULONG EngineStatus, FifoIntr, PbdmaIntr, MmuFault, GrIntr, FecsCtx, CeIntr, Lost;
} SWGPU_H2_SUBMIT_OUT;
typedef struct _SWGPU_H2_WAIT_IN { ULONG Version, Fence, TimeoutMs; } SWGPU_H2_WAIT_IN;
typedef struct _SWGPU_H2_WAIT_OUT { ULONG Version, Result, Completed, WaitUs, Flushes, Lost, Recovered, Reserved; } SWGPU_H2_WAIT_OUT;

typedef struct _H2_REC { ULONG PushVa, PushDwords; } H2_REC;   /* 0.18.0: for replay */

typedef struct _H2_CHANNEL {
    ULONG Chid, Slot, Gen, Put, Fence;
    PMDL Mdl;
    PUCHAR Block;          /* kernel VA, NonCached */
    ULONG BlockIova;       /* untagged IOVA of the block */
    BOOLEAN Enabled, InRunlist, Bound;
    BOOLEAN Priv, Faulted;                 /* 0.18.0 */
    ULONG Reattached, LastGet, LastCompleted;
    ULONGLONG LastProgress;                /* interrupt time of the last GP_GET/fence change */
    H2_REC Rec[H2_MAX_INFLIGHT];           /* submissions by fence % 64 */
} H2_CHANNEL;

static H2_CHANNEL *g_H2Chan[H1_SLOTS];

static ULONGLONG H2Pte(ULONG IovaUntagged, BOOLEAN Priv)
{
    return (((ULONGLONG)IovaUntagged | ACR_GM20B_IOMMU_TAG) >> 8) | 1ull | (Priv ? PTE_PRIV : 0ull);
}

static BOOLEAN H2Lost(PSWGPU_DEVICE Dev, const H2_CHANNEL *C)
{
    return C->Gen != Dev->ResidentGen || Dev->BlitReset || !Dev->ResidentActive;
}

static volatile ULONG *H2Fence(const H2_CHANNEL *C) { return (volatile ULONG *)(C->Block + H2_BLK_FENCE); }

/* F2Invalidate for this channel's page directory (TLB entries are tagged by
 * PDB, and a slot's block IOVA is reused by its next channel). */
static BOOLEAN H2InvalidatePdb(PSWGPU_DEVICE Dev, const H2_CHANNEL *C)
{
    volatile UCHAR *b = Dev->Bar0;
    ULONG writes[2][3], i;
    ULONG pdb = (ULONG)(((((ULONGLONG)C->BlockIova + H2_BLK_PGD) | ACR_GM20B_IOMMU_TAG) >> 12) << 4);
    for (i = 0; i < 200u && !(Rd(b, 0x100C80u) & 0x00FF0000u); ++i) Us(5u);
    if (i == 200u || !F2Write(Dev, b, 0x100CB8u, pdb, &writes[0]) ||
        !F2Write(Dev, b, 0x100CBCu, 0x80000001u, &writes[1])) return FALSE;
    for (i = 0; i < 200u && !(Rd(b, 0x100C80u) & 0x00008000u); ++i) Us(5u);
    return i != 200u;
}

/* H1 FREE with a live channel on the slot: clear the PTEs, flush L2 (the
 * MMU walk may have cached them) and drop this PDB's TLB before the pages
 * go back. Lock and BlitGate held. FALSE = keep the pages. */
static BOOLEAN H2BeforeFree(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, const H1_ALLOC *A, BOOLEAN Gate)
{
    volatile ULONGLONG *pt = H1GpuPt(F->Slot);
    volatile ULONG *spt = H1SmmuPt(F->Slot);
    ULONG i, writes[2][3], polls;
    if (F->Channel == NULL || H2Lost(Dev, F->Channel)) return TRUE;
    if (!Gate || InterlockedCompareExchange(&Dev->BlitOnline, 0, 0) == 0) return FALSE;
    for (i = 0u; i < A->Pages; ++i) { pt[A->FirstPage + i] = 0u; spt[A->FirstPage + i] = 0u; }
    KeMemoryBarrier();
    F2FlushOuter(Dev);
    return F2L2(Dev, writes, &polls) && H2InvalidatePdb(Dev, F->Channel);
}

static ULONG H2RunlistId(PSWGPU_DEVICE Dev)
{
    return (Rd(Dev->Bar0, 0x00022700u) >> 21) & 0xFu;
}

/* BlitGate + lock held. Runlist = resident chid 0 + every enabled user channel. */
static BOOLEAN H2UpdateRunlist(PSWGPU_DEVICE Dev, PULONG Count, PULONG Polls)
{
    volatile UCHAR *b = Dev->Bar0;
    PULONG runlist = (PULONG)((PUCHAR)g_GrVm.Arena + GR_VM_D2_RUNLIST_OFF);
    const ULONG id = H2RunlistId(Dev);
    ULONG n = 0u, s, i, base;
    for (i = 0u; i < 200u && (Rd(b, 0x00002284u + id * 8u) & 0x00100000u); ++i) Us(50u);
    runlist[0] = Dev->ResidentChannelId; runlist[1] = 0u; n = 1u;
    for (s = 0u; s < H1_SLOTS; ++s) {
        H2_CHANNEL *c = g_H2Chan[s];
        if (c != NULL && c->InRunlist) { runlist[n * 2u] = c->Chid; runlist[n * 2u + 1u] = 0u; ++n; }
    }
    KeMemoryBarrier();
    /* D2's choice: the resident instance target and the tagged runlist page. */
    base = (((Rd(b, 0x00800000u + Dev->ResidentChannelId * 8u) >> 28) & 3u) << 28) |
           (ULONG)((g_GrVm.Iova + GR_VM_D2_RUNLIST_OFF) >> 12);
    Wr(b, 0x00002270u, base);
    Wr(b, 0x00002274u, (id << 20) | n);
    for (i = 0u; i < 4000u; ++i) {
        if ((Rd(b, 0x00002284u + id * 8u) & 0x00100000u) == 0u) break;
        Us(50u);
    }
    *Count = n; *Polls = i;
    return i < 4000u && (Rd(b, 0x00120058u) | Rd(b, 0x0012005Cu)) == 0u;
}

static VOID H2SampleRegs(PSWGPU_DEVICE Dev, ULONG Chid, PULONG Ccsr, PULONG Engine, PULONG Fifo,
                         PULONG Pbdma, PULONG Mmu)
{
    volatile UCHAR *b = Dev->Bar0;
    *Ccsr = Rd(b, 0x00800004u + Chid * 8u);
    *Engine = Rd(b, 0x00002640u);
    *Fifo = Rd(b, FIFO_INTR_0);
    *Pbdma = Rd(b, 0x00040108u);
    *Mmu = Rd(b, 0x0000259Cu);
}

/* Fill the block for slot s / chid. Lock held; no hardware access. */
static VOID H2BuildBlock(H2_CHANNEL *C, BOOLEAN PrivPtes)
{
    PUCHAR blk = C->Block;
    volatile ULONGLONG *pgd = (volatile ULONGLONG *)(blk + H2_BLK_PGD);
    volatile ULONGLONG *pt = (volatile ULONGLONG *)(blk + H2_BLK_PT3);
    PULONG inst = (PULONG)(blk + H2_BLK_INST);
    PULONG header = (PULONG)(blk + H2_BLK_CTX);
    const ULONG iova = C->BlockIova;
    const ULONGLONG userdIova = g_GrVm.Iova + GR_VM_D2_USERD_OFF + C->Chid * 0x200u;
    const ULONG userdTarget = *(volatile ULONG *)((PUCHAR)g_GrVm.Arena + GR_VM_INST_OFF + 0x008u) & 3u;
    ULONG i, ptBase = (GR_VM_GPU_VA >> 12) & 0x7FFFu;

    pgd[GR_VM_GPU_VA >> 27] = ((((ULONGLONG)iova + H2_BLK_PT3) | ACR_GM20B_IOMMU_TAG) << 24) | (5ull << 32);
    pgd[H1_USER_VA >> 27] = H1GpuPde(C->Slot);
    for (i = 0u; i < 0x20000u / PAGE_SIZE; ++i)                   /* own GR context */
        pt[ptBase + (GR_VM_CTX_OFF >> 12) + i] = H2Pte(iova + H2_BLK_CTX + i * PAGE_SIZE, PrivPtes);
    for (i = GR_VM_BUNDLE_OFF; i < GR_VM_ZCULL_OFF; i += PAGE_SIZE)  /* shared global buffers */
        pt[ptBase + (i >> 12)] = (((g_GrVm.Iova + i) >> 8) | 1ull | (PrivPtes ? PTE_PRIV : 0ull));
    pt[ptBase + (GR_VM_PATCH_OFF >> 12)] = H2Pte(iova + H2_BLK_PATCH, PrivPtes);
    for (i = GR_VM_PRIV_OFF; i < GR_VM_GPU_PGD_OFF; i += PAGE_SIZE)  /* priv access map */
        pt[ptBase + (i >> 12)] = (((g_GrVm.Iova + i) >> 8) | 1ull | (PrivPtes ? PTE_PRIV : 0ull));
    pt[(H2_CTL_VA >> 12) & 0x7FFFu] = H2Pte(iova + H2_BLK_GPFIFO, FALSE);
    pt[((H2_CTL_VA >> 12) + 1u) & 0x7FFFu] = H2Pte(iova + H2_BLK_TRAILER, FALSE);
    pt[((H2_CTL_VA >> 12) + 2u) & 0x7FFFu] = H2Pte(iova + H2_BLK_FENCE, FALSE);

    /* Context: golden image, then D4's header edits with this VA space's pointers. */
    RtlCopyMemory(header, (PUCHAR)g_GrVm.Arena + GR_VM_GLOBAL_OFF, g_GrVm.GoldenBytes);
    header[0x10u / 4u] = 0u;
    header[0x14u / 4u] = GR_VM_GPU_VA + GR_VM_PATCH_OFF;
    header[0x18u / 4u] = 0u;
    header[0x1Cu / 4u] = 1u;
    header[0x20u / 4u] = 0u;
    header[0x28u / 4u] &= ~0x7u;
    header[0x2Cu / 4u] = 0u;
    header[0x3Cu / 4u] &= ~0x8u;
    header[0xA0u / 4u] = 0u;
    header[0xA4u / 4u] = GR_VM_GPU_VA + GR_VM_PRIV_OFF;
    header[0xA8u / 4u] = 0u;
    header[0xF4u / 4u] = 0u;
    header[0xF8u / 4u] = 0u;

    /* RAMFC: the D2 values with this channel's USERD, GPFIFO and chid. */
    RtlZeroMemory(inst, PAGE_SIZE);
    inst[0x008u / 4u] = ((ULONG)userdIova & ~0x1FFu) | userdTarget;
    inst[0x00Cu / 4u] = (ULONG)(userdIova >> 32);
    inst[0x010u / 4u] = 0x0000FACEu;
    inst[0x030u / 4u] = 0x7FFFF902u;
    inst[0x048u / 4u] = H2_CTL_VA;
    inst[0x04Cu / 4u] = 9u << 16;
    inst[0x084u / 4u] = 0x20400000u;
    inst[0x094u / 4u] = 0x30000001u;
    inst[0x09Cu / 4u] = 0x00000100u;
    inst[0x0ACu / 4u] = 0x0000001Fu;
    inst[0x0E4u / 4u] = 0x00000020u;
    inst[0x0E8u / 4u] = C->Chid;
    inst[0x0B8u / 4u] = 0xF8000000u;
    inst[0x0F8u / 4u] = 0x10003080u;
    inst[0x0FCu / 4u] = 0x10000010u;
    /* PDB (vol, target as the GR instance) and VA limit; context pointer. */
    *(volatile ULONGLONG *)((PUCHAR)inst + 0x200u) = ((((ULONGLONG)iova + H2_BLK_PGD) | ACR_GM20B_IOMMU_TAG) & ~0xFFFull) | 4u;
    *(volatile ULONGLONG *)((PUCHAR)inst + 0x208u) = 0xFFFFFFFFFFull;
    inst[132] = (GR_VM_GPU_VA + GR_VM_CTX_OFF) | 4u;
    inst[133] = 0u;
    RtlZeroMemory((PUCHAR)g_GrVm.Arena + GR_VM_D2_USERD_OFF + C->Chid * 0x200u, 0x200u);
    KeMemoryBarrier();
}

static VOID H2SetMetaMap(ULONG Slot, PMDL Mdl)
{
    volatile ULONG *msmmu = (volatile ULONG *)(g_H1.Meta + H1_META_MSMMU_OFF);
    ULONG i, first = H2_META_FIRST + Slot * H2_BLOCK_PAGES;
    PPFN_NUMBER pfns = Mdl != NULL ? MmGetMdlPfnArray(Mdl) : NULL;
    for (i = 0u; i < H2_BLOCK_PAGES; ++i)
        msmmu[first + i] = pfns != NULL ? ((ULONG)pfns[i] | SMMU_ATTR_RWNS) : 0u;
    KeMemoryBarrier();
}

static VOID H2FreeChannelMemory(H2_CHANNEL *C, BOOLEAN Quarantine)
{
    if (C->Mdl != NULL) {
        if (C->Block != NULL) MmUnmapLockedPages(C->Block, C->Mdl);
        if (!Quarantine) { MmFreePagesFromMdl(C->Mdl); ExFreePool(C->Mdl); }
    }
    ExFreePoolWithTag(C, 'h2wS');
}

/* BlitGate + lock held, GPU online. Tear the channel off the hardware. */
static BOOLEAN H2Teardown(PSWGPU_DEVICE Dev, H2_CHANNEL *C, SWGPU_H2_CHANNEL_OUT *Out)
{
    volatile UCHAR *b = Dev->Bar0;
    ULONG i, n = 0u, polls = 0u;
    BOOLEAN ok = TRUE;
    if (C->Enabled) {
        Wr(b, 0x00800004u + C->Chid * 8u, Rd(b, 0x00800004u + C->Chid * 8u) | 0x00000800u);
        Wr(b, 0x00002634u, C->Chid);
        for (i = 0u; i < 2000u && (Rd(b, 0x00002634u) & 0x00100000u); ++i) Us(50u);
        if (Out) Out->PreemptPolls = i;
        if (i == 2000u) ok = FALSE;
        C->Enabled = FALSE;
    }
    if (C->InRunlist) {
        C->InRunlist = FALSE;
        if (!H2UpdateRunlist(Dev, &n, &polls)) ok = FALSE;
        if (Out) { Out->RunlistCount = n; Out->RunlistPolls = polls; }
    }
    if (C->Bound) { Wr(b, 0x00800000u + C->Chid * 8u, 0u); C->Bound = FALSE; }
    if (ok && !H2InvalidatePdb(Dev, C)) ok = FALSE;
    return ok;
}

static ULONG H2InstPage(const H2_CHANNEL *C);
static VOID H2CheckRecover(PSWGPU_DEVICE Dev, ULONG HintChid);

/* BlitGate + lock held, GPU online. Build the block and put the channel on
 * the hardware (instance bind, runlist id, enable, runlist). On failure the
 * channel is taken off again (GPU reset if that cannot be confirmed). */
static NTSTATUS H2Attach(PSWGPU_DEVICE Dev, H2_CHANNEL *C, SWGPU_H2_CHANNEL_OUT *Out)
{
    volatile UCHAR *b = Dev->Bar0;
    ULONG n = 0u, polls = 0u, id, l2[4] = { 0u };
    NTSTATUS status = STATUS_SUCCESS;
    H2BuildBlock(C, C->Priv);
    H2SetMetaMap(C->Slot, C->Mdl);
    if (Out) Out->CtxMagic = *(volatile ULONG *)(C->Block + H2_BLK_CTX + 0xFCu);
    (VOID)H1PublishOuter();
    F2FlushOuter(Dev);
    if (!B3cC2eL2Flush(b, l2) || !H2InvalidatePdb(Dev, C)) return STATUS_DEVICE_HARDWARE_ERROR;
    id = H2RunlistId(Dev);
    {
        ULONG target = (Rd(b, 0x00800000u + Dev->ResidentChannelId * 8u) >> 28) & 3u;
        Wr(b, FIFO_INTR_0, Rd(b, FIFO_INTR_0) & 1u);          /* clear a stale bind error */
        Wr(b, 0x00800000u + C->Chid * 8u, H2InstPage(C) | (target << 28) | 0x80000000u);
        C->Bound = TRUE;
        if (Rd(b, FIFO_INTR_0) & 1u) { status = STATUS_DEVICE_HARDWARE_ERROR; goto teardown; }
    }
    Wr(b, 0x00800004u + C->Chid * 8u, (Rd(b, 0x00800004u + C->Chid * 8u) & ~0x000F0000u) | (id << 16));
    Wr(b, 0x00800004u + C->Chid * 8u, Rd(b, 0x00800004u + C->Chid * 8u) | 0x00000400u);
    C->Enabled = TRUE;
    C->InRunlist = TRUE;
    g_H2Chan[C->Slot] = C;
    if (!H2UpdateRunlist(Dev, &n, &polls)) { status = STATUS_DEVICE_HARDWARE_ERROR; goto teardown; }
    if (Out) { Out->RunlistCount = n; Out->RunlistPolls = polls; }
    return STATUS_SUCCESS;
teardown:
    if (!H2Teardown(Dev, C, Out)) {
        F2_RUN run;
        RtlZeroMemory(&run, sizeof(run));
        (VOID)F2Abort(Dev, &run);               /* cannot confirm: reset the GPU */
    }
    return status;
}

/* Queue dispatch, BlitGate held. */
static NTSTATUS H2Create(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, SWGPU_H2_CHANNEL_OUT *Out)
{
    volatile UCHAR *b = Dev->Bar0;
    PHYSICAL_ADDRESS low, high, skip;
    H2_CHANNEL *c = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG i;
    const BOOLEAN priv = ReadParam(L"H2PrivPtes", 1u) != 0u;
    Out->PrivPtes = priv ? 1u : 0u;

    H1Lock();
    if (!F->HasSlot || g_H1.Meta == NULL) { status = STATUS_INVALID_DEVICE_STATE; goto out; }  /* ALLOC first */
    Out->Slot = F->Slot;
    if (F->Channel != NULL) { status = STATUS_OBJECT_NAME_COLLISION; goto out; }
    H2CheckRecover(Dev, 0u);
    if (InterlockedCompareExchange(&Dev->BlitOnline, 0, 0) == 0) {
        (VOID)G1RequestWake(Dev);
        status = STATUS_DEVICE_NOT_READY;
        goto out;
    }
    if (!F2CanRun(Dev) || g_GrVm.GoldenBytes == 0u || g_GrVm.GoldenBytes > 0x20000u) {
        status = STATUS_DEVICE_NOT_READY; goto out;
    }
    c = (H2_CHANNEL *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), 'h2wS');
    if (c == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    c->Slot = F->Slot;
    c->Chid = F->Slot + 1u;
    c->Gen = Dev->ResidentGen;
    c->Priv = priv;
    low.QuadPart = 0; high.QuadPart = 0x3FFFFFFFFll; skip.QuadPart = 0;
    c->Mdl = MmAllocatePagesForMdlEx(low, high, skip, (SIZE_T)H2_BLOCK_PAGES * PAGE_SIZE, MmNonCached,
                                     MM_ALLOCATE_FULLY_REQUIRED);
    if (c->Mdl == NULL || MmGetMdlByteCount(c->Mdl) != H2_BLOCK_PAGES * PAGE_SIZE) {
        if (c->Mdl != NULL) { MmFreePagesFromMdl(c->Mdl); ExFreePool(c->Mdl); c->Mdl = NULL; }
        status = STATUS_INSUFFICIENT_RESOURCES; goto out;
    }
    {
        PPFN_NUMBER pfns = MmGetMdlPfnArray(c->Mdl);
        for (i = 0u; i < H2_BLOCK_PAGES; ++i)
            if (pfns[i] >= (1ull << 22)) { status = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    }
    c->Block = (PUCHAR)MmMapLockedPagesSpecifyCache(c->Mdl, KernelMode, MmNonCached, NULL, FALSE,
                                                     NormalPagePriority | MdlMappingNoExecute);
    if (c->Block == NULL) { status = STATUS_INSUFFICIENT_RESOURCES; goto out; }
    RtlZeroMemory(c->Block, H2_BLOCK_PAGES * PAGE_SIZE);
    c->BlockIova = H1_META_IOVA + (H2_META_FIRST + c->Slot * H2_BLOCK_PAGES) * PAGE_SIZE;
    status = H2Attach(Dev, c, Out);
    if (!NT_SUCCESS(status)) { g_H2Chan[c->Slot] = NULL; goto sample; }
    c->LastProgress = KeQueryInterruptTime();
    Out->Chid = c->Chid;
    Out->GpfifoEntries = H2_GPFIFO_ENTRIES;
    F->Channel = c;
    InterlockedIncrement(&Dev->H2Channels);
    InterlockedExchange64(&Dev->LastBlitTime, (LONG64)KeQueryInterruptTime());
    c = NULL;
sample:
    if (!Dev->BlitReset && Dev->ResidentActive) {   /* BAR0 is unusable after an abort */
        H2SampleRegs(Dev, F->Slot + 1u, &Out->CcsrChannel, &Out->EngineStatus,
                     &Out->FifoIntr, &Out->PbdmaIntr, &Out->MmuFault);
        Out->CcsrInst = Rd(b, 0x00800000u + (F->Slot + 1u) * 8u);
    }
out:
    if (c != NULL) {
        if (c->Mdl != NULL && c->Block != NULL) H2SetMetaMap(c->Slot, NULL);
        H2FreeChannelMemory(c, Dev->BlitUnsafe);
    }
    Out->Result = (ULONG)status;
    H1Unlock();
    Trace(L"H2LastCreateResult", (ULONG)status);
    return status;
}

/* ---- Claude 0.18.0 (H2c): fault and hang recovery ----------------------
 * A fault or hang of one user channel stops the whole GR runlist on GM20B
 * (GR + GRCE share it, and an engine reset needs the ctxsw firmware again).
 * Recovery therefore restarts the GPU through the same path G1 uses
 * (resident shutdown + resident start), blames one channel and rebuilds the
 * others:
 *   - culprit: the channel whose instance the MMU fault names, else the
 *     channel loaded on GR (engine status), else the channel that hung.
 *     Its later SUBMIT/WAIT return H2_FAULTED; the process destroys it and
 *     may create a new one.
 *   - every other channel gets a fresh context and its unfinished
 *     submissions are put on its GPFIFO again (a submit that was cut off in
 *     the middle runs again from its start).
 *   - the kernel channel restarts empty (display presents fall back to the
 *     CPU meanwhile, as before).
 * Triggers: an MMU fault seen by SUBMIT/WAIT, a channel with pending work
 * and no progress for H2_HANG_MS seen by WAIT, and a G1 tick that finds the
 * GPU reset after an aborted kernel copy (formerly G1 just switched off).
 * At most H2_RECOVER_BURST recoveries per minute; then the GPU stays off as
 * before (ResidentStop / Resident to try again). */

static struct {
    ULONG Count, Refused, LastCulprit, LastMmu, LastEngine, LastInst, LastPbdmaChan;
    ULONG LastReason, LastUs, LastResult, LastRebuilt;
    ULONGLONG Times[H2_RECOVER_BURST];
    ULONG TimeIdx;
} g_H2Rec;

static ULONG H2InstPage(const H2_CHANNEL *C)
{
    return (ULONG)(((((ULONGLONG)C->BlockIova + H2_BLK_INST) | ACR_GM20B_IOMMU_TAG) >> 12) & 0x0FFFFFFFu);
}

static BOOLEAN H2FaultPending(PSWGPU_DEVICE Dev)
{
    if (Dev->BlitReset || !Dev->ResidentActive || Dev->Bar0 == NULL) return FALSE;
    return Rd(Dev->Bar0, 0x0000259Cu) != 0u || (Rd(Dev->Bar0, FIFO_INTR_0) & 0x10000000u) != 0u;
}

static ULONG H2Culprit(PSWGPU_DEVICE Dev, ULONG Hint)
{
    ULONG mmu = 0u, eng = 0u, inst = 0u, pbch = 0u, s, e, id;
    if (!Dev->BlitReset && Dev->ResidentActive && Dev->Bar0 != NULL) {
        mmu = Rd(Dev->Bar0, 0x0000259Cu);
        eng = Rd(Dev->Bar0, 0x00002640u);
        pbch = Rd(Dev->Bar0, 0x00040120u);
        for (e = 0u; e < 16u && mmu != 0u; ++e)
            if (mmu & (1u << e)) { inst = Rd(Dev->Bar0, 0x00002800u + e * 16u); break; }
    } else if (Dev->AbortValid) {             /* sampled by F2Abort before its reset */
        mmu = Dev->AbortMmu; eng = Dev->AbortEngine; inst = Dev->AbortInst; pbch = Dev->AbortPbdmaChan;
    }
    g_H2Rec.LastMmu = mmu; g_H2Rec.LastEngine = eng; g_H2Rec.LastInst = inst; g_H2Rec.LastPbdmaChan = pbch;
    if (mmu != 0u && inst != 0u) {
        for (s = 0u; s < H1_SLOTS; ++s)
            if (g_H2Chan[s] != NULL && (inst & 0x0FFFFFFFu) == H2InstPage(g_H2Chan[s])) return g_H2Chan[s]->Chid;
        return 0u;                             /* the kernel channel itself */
    }
    if (eng & 0x80000000u) {                   /* engine busy: id, or next id while loading */
        id = ((eng >> 13) & 7u) == 5u ? (eng >> 16) & 0xFFFu : eng & 0xFFFu;
        if (id >= 1u && id <= H1_SLOTS && g_H2Chan[id - 1u] != NULL) return id;
    }
    /* The channel loaded on the PBDMA (e.g. stuck in a semaphore acquire), if it
     * still has unfinished work: a waiter that notices the hang first may be
     * an innocent one. */
    id = pbch & 0xFFFu;
    if (id >= 1u && id <= H1_SLOTS && g_H2Chan[id - 1u] != NULL &&
        (LONG)(g_H2Chan[id - 1u]->Fence - H2Fence(g_H2Chan[id - 1u])[0]) > 0) return id;
    /* Else the channel that has been stuck the longest with unfinished work. */
    {
        ULONGLONG oldest = MAXULONGLONG;
        ULONG pick = Hint;
        for (s = 0u; s < H1_SLOTS; ++s) {
            H2_CHANNEL *c = g_H2Chan[s];
            if (c != NULL && !c->Faulted && (LONG)(c->Fence - H2Fence(c)[0]) > 0 && c->LastProgress < oldest) {
                oldest = c->LastProgress;
                pick = c->Chid;
            }
        }
        return pick;
    }
}

/* BlitGate + lock held, GPU online and this generation. Put the unfinished
 * submissions (fence > completed) on an empty GPFIFO again. */
static BOOLEAN H2Replay(PSWGPU_DEVICE Dev, H2_CHANNEL *C)
{
    PULONG gp = (PULONG)(C->Block + H2_BLK_GPFIFO);
    ULONG completed = H2Fence(C)[0], f, put = 0u, writes[2][3], polls;
    for (f = completed + 1u; (LONG)(C->Fence - f) >= 0; ++f) {
        const H2_REC *r = &C->Rec[f % H2_MAX_INFLIGHT];
        PULONG tr = (PULONG)(C->Block + H2_BLK_TRAILER + (f % H2_TRAILERS) * 32u);
        tr[0] = F1Method(0u, 0x0010u, 4u);
        tr[1] = 0u; tr[2] = H2_CTL_VA + 2u * PAGE_SIZE; tr[3] = f;
        tr[4] = 0x01000002u;
        gp[put * 2u] = r->PushVa; gp[put * 2u + 1u] = r->PushDwords << 10; ++put;
        gp[put * 2u] = H2_CTL_VA + PAGE_SIZE + (f % H2_TRAILERS) * 32u;
        gp[put * 2u + 1u] = H2_TRAILER_DWORDS << 10; ++put;
    }
    C->Put = put & (H2_GPFIFO_ENTRIES - 1u);
    C->LastProgress = KeQueryInterruptTime();
    C->LastCompleted = completed;
    KeMemoryBarrier();
    if (!F2L2(Dev, writes, &polls)) return FALSE;
    if (put != 0u) Wr(Dev->Bar1Cpu, BAR1_D3_LOW_VA + C->Chid * 0x200u + 0x8Cu, C->Put);
    return TRUE;
}

/* BlitGate + lock held. A channel from an older generation (not faulted)
 * goes back on the hardware with its pending work. */
static NTSTATUS H2Reattach(PSWGPU_DEVICE Dev, H2_CHANNEL *C)
{
    NTSTATUS status;
    if (C->Faulted) return H2_FAULTED;
    if (!Dev->ResidentActive || Dev->BlitReset || InterlockedCompareExchange(&Dev->BlitOnline, 0, 0) == 0)
        return STATUS_DEVICE_NOT_READY;
    C->Enabled = C->InRunlist = C->Bound = FALSE;
    C->Gen = Dev->ResidentGen;
    C->Put = 0u;
    status = H2Attach(Dev, C, NULL);
    if (NT_SUCCESS(status) && !H2Replay(Dev, C)) status = STATUS_DEVICE_HARDWARE_ERROR;
    if (NT_SUCCESS(status)) ++C->Reattached;
    return status;
}

/* BlitGate + lock held. Restart the GPU, blame one channel, rebuild the rest. */
static BOOLEAN H2RecoverGpu(PSWGPU_DEVICE Dev, ULONG HintChid, ULONG Reason)
{
    LARGE_INTEGER start, freq;
    ULONGLONG now = KeQueryInterruptTime();
    ULONG culprit, s, rebuilt = 0u, slot = g_H2Rec.TimeIdx % H2_RECOVER_BURST;
    NTSTATUS status;
    if (Dev->BlitUnsafe) return FALSE;
    if (g_H2Rec.Times[slot] != 0u && now - g_H2Rec.Times[slot] < 60ull * 10000000ull) {
        ++g_H2Rec.Refused;
        Trace(L"H2RecoverRefused", g_H2Rec.Refused);
        return FALSE;
    }
    g_H2Rec.Times[slot] = now;
    ++g_H2Rec.TimeIdx;
    start = KeQueryPerformanceCounter(&freq);
    culprit = H2Culprit(Dev, HintChid);
    if (culprit >= 1u && culprit <= H1_SLOTS && g_H2Chan[culprit - 1u] != NULL)
        g_H2Chan[culprit - 1u]->Faulted = TRUE;
    if (Dev->ResidentActive) (VOID)B3eResidentShutdown(Dev, TRUE);
    Dev->AbortValid = FALSE;
    status = Dev->BlitUnsafe ? SWGPU_BLIT_DMA_UNSAFE : B3eStartOnline(Dev, FALSE, FALSE);
    if (NT_SUCCESS(status)) {
        for (s = 0u; s < H1_SLOTS; ++s) {
            H2_CHANNEL *c = g_H2Chan[s];
            if (c != NULL && !c->Faulted && NT_SUCCESS(H2Reattach(Dev, c))) ++rebuilt;
        }
    }
    ++g_H2Rec.Count;
    g_H2Rec.LastCulprit = culprit;
    g_H2Rec.LastReason = Reason;
    g_H2Rec.LastResult = (ULONG)status;
    g_H2Rec.LastRebuilt = rebuilt;
    g_H2Rec.LastUs = (ULONG)((KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart) * 1000000ll / freq.QuadPart);
    Trace(L"H2Recoveries", g_H2Rec.Count);
    Trace(L"H2LastCulprit", culprit);
    Trace(L"H2LastReason", Reason);
    Trace(L"H2LastMmuFault", g_H2Rec.LastMmu);
    Trace(L"H2LastFaultInst", g_H2Rec.LastInst);
    Trace(L"H2LastEngineStatus", g_H2Rec.LastEngine);
    Trace(L"H2LastPbdmaChannel", g_H2Rec.LastPbdmaChan);
    Trace(L"H2LastRecoverResult", (ULONG)status);
    Trace(L"H2LastRecoverUs", g_H2Rec.LastUs);
    Trace(L"H2LastRebuilt", rebuilt);
    return NT_SUCCESS(status);
}

/* G1 tick (BlitGate held): the GPU was reset by an aborted kernel copy. */
static BOOLEAN H2RecoverFromG1(PSWGPU_DEVICE Dev)
{
    BOOLEAN ok;
    H1Lock();
    ok = H2RecoverGpu(Dev, 0u, H2_REASON_G1);
    H1Unlock();
    return ok;
}

/* BlitGate + lock held: recover first if the hardware says so. */
static VOID H2CheckRecover(PSWGPU_DEVICE Dev, ULONG HintChid)
{
    if (Dev->BlitReset && !Dev->BlitUnsafe && Dev->ResidentActive)
        (VOID)H2RecoverGpu(Dev, 0u, H2_REASON_RESET);
    else if (H2FaultPending(Dev))
        (VOID)H2RecoverGpu(Dev, HintChid, H2_REASON_FAULT);
}

/* Lock held, BlitGate held (or the channel is lost). */
static VOID H2Destroy(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, SWGPU_H2_CHANNEL_OUT *Out, BOOLEAN HaveGate)
{
    H2_CHANNEL *c = F->Channel;
    BOOLEAN quarantine = FALSE;
    if (c == NULL) return;
    if (!H2Lost(Dev, c)) {
        if (!HaveGate || !H2Teardown(Dev, c, Out)) {
            /* Could not stop it cleanly: reset the GPU before freeing anything. */
            if (HaveGate) {
                F2_RUN run;
                RtlZeroMemory(&run, sizeof(run));
                quarantine = !F2Abort(Dev, &run);
            } else {
                quarantine = TRUE;          /* still live: keep its memory and mappings */
            }
        }
    }
    g_H2Chan[c->Slot] = NULL;
    if (quarantine) g_H1.Quarantined = TRUE;
    else H2SetMetaMap(c->Slot, NULL);
    H2FreeChannelMemory(c, quarantine);
    F->Channel = NULL;
    InterlockedDecrement(&Dev->H2Channels);
}

/* Queue dispatch, BlitGate held. */
static NTSTATUS H2Submit(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, const SWGPU_H2_SUBMIT_IN *In,
                         SWGPU_H2_SUBMIT_OUT *Out)
{
    volatile UCHAR *b = Dev->Bar0;
    H2_CHANNEL *c;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG get, put, next, fence, t, writes[2][3], l2Polls = 0u;
    PULONG gp, tr;
    ULONG userd;
    H1Lock();
    c = F->Channel;
    if (c == NULL) { status = STATUS_INVALID_DEVICE_STATE; goto out; }
    if (In->PushDwords == 0u || In->PushDwords > 0x100000u ||
        H1FindRange(F, In->PushVa, In->PushDwords * 4u) == NULL || (In->PushVa & 3u)) {
        ++F->Rejected;
        status = STATUS_ACCESS_VIOLATION; goto out;
    }
    H2CheckRecover(Dev, 0u);
    if (c->Faulted) { Out->Lost = 2u; status = H2_FAULTED; goto out; }
    if (H2Lost(Dev, c)) {
        /* After a recovery or a resident restart: put it back on the GPU. */
        status = H2Reattach(Dev, c);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_DEVICE_NOT_READY) (VOID)G1RequestWake(Dev);
            Out->Lost = 1u;
            goto out;
        }
    }
    Out->Completed = H2Fence(c)[0];
    if (c->Fence - Out->Completed >= H2_MAX_INFLIGHT) { status = STATUS_DEVICE_BUSY; goto out; }
    userd = BAR1_D3_LOW_VA + c->Chid * 0x200u;
    get = Rd(Dev->Bar1Cpu, userd + 0x88u);
    put = c->Put;
    Out->Get = get;
    if (((get - put - 1u) & (H2_GPFIFO_ENTRIES - 1u)) < 2u) { status = STATUS_DEVICE_BUSY; goto out; }
    if (H1PublishOuter()) F2FlushOuter(Dev);
    fence = c->Fence + 1u;
    c->Rec[fence % H2_MAX_INFLIGHT].PushVa = In->PushVa;
    c->Rec[fence % H2_MAX_INFLIGHT].PushDwords = In->PushDwords;
    t = fence % H2_TRAILERS;
    tr = (PULONG)(c->Block + H2_BLK_TRAILER + t * 32u);
    tr[0] = F1Method(0u, 0x0010u, 4u);
    tr[1] = 0u; tr[2] = H2_CTL_VA + 2u * PAGE_SIZE; tr[3] = fence;
    tr[4] = 0x01000002u;                    /* release, WFI enabled, 4 bytes */
    gp = (PULONG)(c->Block + H2_BLK_GPFIFO);
    gp[put * 2u] = In->PushVa;
    gp[put * 2u + 1u] = In->PushDwords << 10;
    next = (put + 1u) & (H2_GPFIFO_ENTRIES - 1u);
    gp[next * 2u] = H2_CTL_VA + PAGE_SIZE + t * 32u;
    gp[next * 2u + 1u] = H2_TRAILER_DWORDS << 10;
    next = (next + 1u) & (H2_GPFIFO_ENTRIES - 1u);
    KeMemoryBarrier();
    if (!F2L2(Dev, writes, &l2Polls)) { status = STATUS_DEVICE_HARDWARE_ERROR; goto out; }
    if (c->Fence == Out->Completed) c->LastProgress = KeQueryInterruptTime();  /* was idle */
    Wr(Dev->Bar1Cpu, userd + 0x8Cu, next);
    c->Put = next;
    c->Fence = fence;
    Out->Fence = fence;
    Out->Put = next;
    InterlockedExchange64(&Dev->LastBlitTime, (LONG64)KeQueryInterruptTime());
    Out->Completed = H2Fence(c)[0];
out:
    if (c != NULL && !Dev->BlitReset && Dev->ResidentActive) {
        H2SampleRegs(Dev, c->Chid, &Out->CcsrChannel, &Out->EngineStatus, &Out->FifoIntr,
                     &Out->PbdmaIntr, &Out->MmuFault);
        Out->GrIntr = Rd(b, 0x00400100u);
        Out->FecsCtx = Rd(b, 0x00409B00u);
        Out->CeIntr = Rd(b, F1_CE_INTR_STATUS);
    }
    Out->Result = (ULONG)status;
    H1Unlock();
    Trace(L"H2LastSubmitResult", (ULONG)status);
    return status;
}

/* Caller context: does not hold the queue. Each round takes BlitGate only if
 * it is free (never blocks a present); with it and the GPU online it flushes
 * L2, notices faults and hangs and recovers. Lock order: BlitGate, then H1. */
static NTSTATUS H2Wait(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, const SWGPU_H2_WAIT_IN *In, SWGPU_H2_WAIT_OUT *Out)
{
    LARGE_INTEGER start, freq;
    ULONG writes[2][3], polls, ms = In->TimeoutMs > 10000u ? 10000u : In->TimeoutMs;
    NTSTATUS status = STATUS_TIMEOUT;
    start = KeQueryPerformanceCounter(&freq);
    for (;;) {
        H2_CHANNEL *c;
        BOOLEAN done = FALSE, gate = InterlockedCompareExchange(&Dev->BlitGate, 1, 0) == 0;
        H1Lock();
        c = F->Channel;
        if (c == NULL) status = STATUS_INVALID_DEVICE_STATE;
        else {
            if (gate) {
                H2CheckRecover(Dev, c->Chid);
                if (!c->Faulted && H2Lost(Dev, c)) (VOID)H2Reattach(Dev, c);
                if (InterlockedCompareExchange(&Dev->BlitOnline, 0, 0) && !Dev->BlitReset && !H2Lost(Dev, c)) {
                    ULONG get;
                    (VOID)F2L2(Dev, writes, &polls);
                    ++Out->Flushes;
                    KeMemoryBarrier();
                    get = Rd(Dev->Bar1Cpu, BAR1_D3_LOW_VA + c->Chid * 0x200u + 0x88u);
                    if (get != c->LastGet || H2Fence(c)[0] != c->LastCompleted) {
                        c->LastGet = get;
                        c->LastCompleted = H2Fence(c)[0];
                        c->LastProgress = KeQueryInterruptTime();
                    } else if ((LONG)(c->Fence - H2Fence(c)[0]) > 0 &&
                               KeQueryInterruptTime() - c->LastProgress >= H2_HANG_MS * 10000ull) {
                        ++Out->Recovered;
                        (VOID)H2RecoverGpu(Dev, c->Chid, H2_REASON_HANG);
                    }
                }
            }
            KeMemoryBarrier();
            Out->Completed = H2Fence(c)[0];
            done = (LONG)(Out->Completed - In->Fence) >= 0;
            if (done) status = STATUS_SUCCESS;
            else if (c->Faulted) { Out->Lost = 2u; status = H2_FAULTED; }
        }
        H1Unlock();
        if (gate) InterlockedExchange(&Dev->BlitGate, 0);
        if (status != STATUS_TIMEOUT) break;
        if ((KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart) * 1000ll / freq.QuadPart >= ms) break;
        Ms(1u);
    }
    Out->WaitUs = (ULONG)((KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart) * 1000000ll / freq.QuadPart);
    Out->Result = (ULONG)status;
    return status;
}

/* Cleanup path (caller context): take BlitGate for up to 3 s to tear down. */
static VOID H2CleanupFile(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F)
{
    ULONG i;
    BOOLEAN gate = FALSE;
    if (F->Channel == NULL) return;
    for (i = 0u; i < 3000u && !gate; ++i) {
        gate = InterlockedCompareExchange(&Dev->BlitGate, 1, 0) == 0;
        if (!gate) Ms(1u);
    }
    H1Lock();
    H2Destroy(Dev, F, NULL, gate && InterlockedCompareExchange(&Dev->BlitOnline, 0, 0) != 0);
    H1Unlock();
    if (gate) InterlockedExchange(&Dev->BlitGate, 0);
    Trace(L"H2Cleanups", ReadParam(L"H2Cleanups", 0u) + 1u);
}

/* (moved from swgpu-h1.inc in 0.17.0) ALLOC / FREE / INFO / H2 WAIT run in the
 * caller's process (user mappings, long waits); COPY goes
 * through the power-managed sequential queue like the other GPU IOCTLs. */
static VOID H1EvtIoInCallerContext(WDFDEVICE Device, WDFREQUEST Request)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice(Device);
    WDF_REQUEST_PARAMETERS params;
    PSWGPU_H1_FILE f;
    PVOID in = NULL, out = NULL;
    NTSTATUS status;
    ULONG code;
    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    code = params.Type == WdfRequestTypeDeviceControl ? params.Parameters.DeviceIoControl.IoControlCode : 0u;
    if (code != IOCTL_SWGPU_H1_ALLOC && code != IOCTL_SWGPU_H1_FREE && code != IOCTL_SWGPU_H1_INFO &&
        code != IOCTL_SWGPU_H2_WAIT) {
        status = WdfDeviceEnqueueRequest(Device, Request);
        if (!NT_SUCCESS(status)) WdfRequestComplete(Request, status);
        return;
    }
    if (WdfRequestGetRequestorMode(Request) != UserMode || WdfRequestGetFileObject(Request) == NULL ||
        KeGetCurrentIrql() != PASSIVE_LEVEL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
    f = SwGpuH1File(WdfRequestGetFileObject(Request));
    if (code == IOCTL_SWGPU_H1_ALLOC) {
        SWGPU_H1_ALLOC_IN copyIn;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SWGPU_H1_ALLOC_IN), &in, NULL);
        if (NT_SUCCESS(status))
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SWGPU_H1_ALLOC_OUT), &out, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        copyIn = *(SWGPU_H1_ALLOC_IN *)in;        /* in and out share the system buffer */
        status = H1Alloc(f, &copyIn, (SWGPU_H1_ALLOC_OUT *)out);
        WdfRequestCompleteWithInformation(Request, status,
            NT_SUCCESS(status) ? sizeof(SWGPU_H1_ALLOC_OUT) : 0u);
    } else if (code == IOCTL_SWGPU_H2_WAIT) {
        SWGPU_H2_WAIT_IN waitIn;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SWGPU_H2_WAIT_IN), &in, NULL);
        if (NT_SUCCESS(status))
            status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SWGPU_H2_WAIT_OUT), &out, NULL);
        if (!NT_SUCCESS(status)) { WdfRequestComplete(Request, status); return; }
        waitIn = *(SWGPU_H2_WAIT_IN *)in;
        RtlZeroMemory(out, sizeof(SWGPU_H2_WAIT_OUT));
        ((SWGPU_H2_WAIT_OUT *)out)->Version = SWGPU_VERSION;
        (VOID)H2Wait(dev, f, &waitIn, (SWGPU_H2_WAIT_OUT *)out);
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, sizeof(SWGPU_H2_WAIT_OUT));
    } else if (code == IOCTL_SWGPU_H1_FREE) {
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SWGPU_H1_FREE_IN), &in, NULL);
        if (NT_SUCCESS(status)) status = H1Free(dev, f, (const SWGPU_H1_FREE_IN *)in);
        WdfRequestComplete(Request, status);
    } else {
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SWGPU_H1_INFO_OUT), &out, NULL);
        if (NT_SUCCESS(status)) H1Info(dev, f, (SWGPU_H1_INFO_OUT *)out);
        WdfRequestCompleteWithInformation(Request, status,
            NT_SUCCESS(status) ? sizeof(SWGPU_H1_INFO_OUT) : 0u);
    }
}
