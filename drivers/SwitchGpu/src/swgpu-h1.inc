/* Claude 0.16.0 (H1): per-process GPU VA spaces, memory and CPU mappings.
 *
 * Included after swgpu-blit.inc and swgpu-idle.inc (F2 helpers, G1 wake).
 *
 * Every file handle that allocates gets one of H1_SLOTS "VA spaces". A VA
 * space is its own 4 KiB GPU page table for GPU VA H1_USER_VA (one 128 MiB
 * GMMU PDE, index 8) plus its own outer SMMU page tables for a private IOVA
 * range. Two handles therefore get the same GPU VA numbers for different
 * pages. The resident GR channel has one page directory, so a VA space is
 * bound per copy: under BlitGate the copy writes GR PDE 8 and the outer SMMU
 * PDEs of that one slot, invalidates, runs CE2, then clears them again. While
 * slot A is bound no PDE of any other slot is present, so the GPU cannot
 * reach another slot's pages even by IOVA.
 *
 * Layout (none of it overlaps the display's GPU present windows):
 *   GPU VA   0x18000000  GR arena (PDE 3)
 *   GPU VA   0x20000000  display present windows 0/1, 32 MiB (PDE 4)
 *   GPU VA   0x40000000  H1 user window, 32 MiB used of PDE 8
 *   IOVA     0x20000000  display windows (outer PDEs 0x80-0x87)
 *   IOVA     0x40000000  H1 slot s user pages: + s * 32 MiB (PDEs 0x100-0x11F)
 *   IOVA     0x48000000  H1 GPU page tables (PDE 0x120)
 *   IOVA     >= 0x80000000 identity arenas (ACR/GR/WPR)
 *
 * User pages come from MmAllocatePagesForMdlEx (write-combined, zeroed) and
 * are mapped write-combined into the calling process from the in-caller
 * context callback. Cleanup unmaps them in the owning process. Admin-only
 * device (INF security); a handle duplicated into another process that
 * outlives its creator is not supported in H1. */

#define SWGPU_H1_VERSION      1u
#define IOCTL_SWGPU_H1_ALLOC  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x808, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_H1_FREE   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x809, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_H1_COPY   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80A, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_SWGPU_H1_INFO   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x80B, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define H1_SLOTS          4u
#define H1_USER_VA        0x40000000u
#define H1_USER_BYTES     0x02000000u               /* 32 MiB per VA space */
#define H1_USER_PAGES     (H1_USER_BYTES / PAGE_SIZE)
#define H1_IOVA_BASE      0x40000000u
#define H1_META_IOVA      0x48000000u
#define H1_MAX_ALLOC      0x01000000u               /* 16 MiB per allocation */
#define H1_MAX_ALLOCS     32u
#define H1_PT_BYTES       0x40000u                  /* full 128 MiB small-page table */
#define H1_SMMU_PTS       (H1_USER_BYTES >> 22)     /* 8 outer tables per slot */
#define H1_META_PT_OFF    0u
#define H1_META_SMMU_OFF  (H1_SLOTS * H1_PT_BYTES)
#define H1_META_MSMMU_OFF (H1_META_SMMU_OFF + H1_SLOTS * H1_SMMU_PTS * PAGE_SIZE)
#define H1_META_BYTES     (H1_META_MSMMU_OFF + PAGE_SIZE)
#define H1_META_MAPPED    (H1_META_SMMU_OFF / PAGE_SIZE) /* pages the GPU walks */
#define H1_ERROR          0xE00000B1u

C_ASSERT((H1_USER_VA >> 27) != (GR_VM_BLIT_BASE >> 27));
C_ASSERT((H1_USER_VA >> 27) != (GR_VM_GPU_VA >> 27));
C_ASSERT((H1_USER_VA >> 27) != (BAR1_D3_LOW_VA >> 27));
C_ASSERT(H1_USER_BYTES <= (1u << 27));
C_ASSERT(H1_IOVA_BASE >= GR_VM_BLIT_BASE + GR_VM_BLIT_PDES * 0x400000u);
C_ASSERT(H1_IOVA_BASE + H1_SLOTS * H1_USER_BYTES <= H1_META_IOVA);
C_ASSERT(H1_META_IOVA + 0x400000u <= 0x80000000u);
C_ASSERT(H1_META_MAPPED <= 1024u);

typedef struct _SWGPU_H1_ALLOC_IN { ULONG Version, Bytes; } SWGPU_H1_ALLOC_IN;
typedef struct _SWGPU_H1_ALLOC_OUT {
    ULONG Version, Handle, GpuVa, Bytes, Slot, Pages;
    ULONGLONG CpuVa;
} SWGPU_H1_ALLOC_OUT;
typedef struct _SWGPU_H1_FREE_IN { ULONG Version, Handle; } SWGPU_H1_FREE_IN;
typedef struct _SWGPU_H1_COPY_IN { ULONG Version, SrcVa, DstVa, Bytes; } SWGPU_H1_COPY_IN;
typedef struct _SWGPU_H1_COPY_OUT {
    ULONG Version, Result, Slot, WaitUs, Polls, IrqWakes, MapPolls, UnmapPolls;
    ULONG PtesChecked, OtherPdesClear, Lines, LineBytes, FaultStatus, PbdmaIntr, CeIntr, Reserved;
} SWGPU_H1_COPY_OUT;
typedef struct _SWGPU_H1_INFO_OUT {
    ULONG Version, Slot, SlotsInUse, Allocs, Pages, ValidGpuPtes, ValidSmmuPtes;
    ULONG ForeignPtes, OrphanPtes, IsolationOk, Copies, CopyFailures, Rejected;
    ULONG ProcessId, Quarantined, MetaPa, Resident, BlitOnline;
} SWGPU_H1_INFO_OUT;

typedef struct _H1_ALLOC {
    BOOLEAN Used;
    ULONG FirstPage, Pages;     /* in the slot's VA space; one guard page after */
    PMDL Mdl;
    PVOID UserVa;
} H1_ALLOC;

struct _H2_CHANNEL;
typedef struct _SWGPU_H1_FILE {
    struct _H2_CHANNEL *Channel;   /* H2: at most one channel per handle */
    BOOLEAN HasSlot;
    ULONG Slot;
    PEPROCESS Process;          /* referenced while the handle has a slot */
    ULONG Allocs, Pages, Copies, CopyFailures, Rejected;
    H1_ALLOC Alloc[H1_MAX_ALLOCS];
} SWGPU_H1_FILE, *PSWGPU_H1_FILE;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SWGPU_H1_FILE, SwGpuH1File)
static VOID H2CleanupFile(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F);   /* swgpu-h2.inc */
struct _H1_ALLOC;

typedef struct _H1_SLOT {
    BOOLEAN InUse;
    RTL_BITMAP Map;
    ULONG Bits[H1_USER_PAGES / 32u];
} H1_SLOT;

static struct {
    BOOLEAN LockReady;
    KMUTEX Lock;                /* slot tables, allocations; held across a copy. A KMUTEX
                                 * keeps PASSIVE_LEVEL: F2Abort may do synchronous I/O. */
    PUCHAR Meta;                /* GPU PTs + outer SMMU PTs, NonCached */
    PHYSICAL_ADDRESS MetaPa;
    BOOLEAN Quarantined;        /* a copy could not be stopped: never free pages */
    H1_SLOT Slot[H1_SLOTS];
} g_H1;

static VOID H1Lock(VOID)
{
    (VOID)KeWaitForSingleObject(&g_H1.Lock, Executive, KernelMode, FALSE, NULL);
}

static VOID H1Unlock(VOID)
{
    (VOID)KeReleaseMutex(&g_H1.Lock, FALSE);
}

/* DeviceAdd: one GPU device. */
static VOID H1Init(VOID)
{
    ULONG s;
    if (g_H1.LockReady) return;
    KeInitializeMutex(&g_H1.Lock, 0u);
    for (s = 0u; s < H1_SLOTS; ++s)
        RtlInitializeBitMap(&g_H1.Slot[s].Map, g_H1.Slot[s].Bits, H1_USER_PAGES);
    g_H1.LockReady = TRUE;
}

static volatile ULONGLONG *H1GpuPt(ULONG Slot)
{
    return (volatile ULONGLONG *)(g_H1.Meta + H1_META_PT_OFF + Slot * H1_PT_BYTES);
}

static volatile ULONG *H1SmmuPt(ULONG Slot)
{
    return (volatile ULONG *)(g_H1.Meta + H1_META_SMMU_OFF + Slot * H1_SMMU_PTS * PAGE_SIZE);
}

static ULONG H1SlotIova(ULONG Slot, ULONG Page)
{
    return H1_IOVA_BASE + Slot * H1_USER_BYTES + Page * PAGE_SIZE;
}

static ULONGLONG H1GpuPte(ULONG Slot, ULONG Page)
{
    return (((ULONGLONG)H1SlotIova(Slot, Page) | ACR_GM20B_IOMMU_TAG) >> 8) | 1ull;
}

static ULONGLONG H1GpuPde(ULONG Slot)
{
    ULONGLONG ptIova = (ULONGLONG)(H1_META_IOVA + H1_META_PT_OFF + Slot * H1_PT_BYTES) | ACR_GM20B_IOMMU_TAG;
    return (ptIova << 24) | (5ull << 32);   /* same small-PT encoding as the GR arena */
}

static ULONG H1OuterPde(ULONG MetaOffset)
{
    return (ULONG)((g_H1.MetaPa.QuadPart + MetaOffset) >> 12) | SMMU_ATTR_RWNS | SMMU_PDE_NEXT;
}

/* Lock held. One contiguous block for all slots, kept until the device goes. */
static NTSTATUS H1EnsureMeta(VOID)
{
    PHYSICAL_ADDRESS low, high, boundary;
    volatile ULONG *msmmu;
    ULONG i;
    if (g_H1.Meta != NULL) return STATUS_SUCCESS;
    low.QuadPart = 0; high.QuadPart = 0xFFFFFFFFll; boundary.QuadPart = 0;
    g_H1.Meta = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(H1_META_BYTES, low, high, boundary, MmNonCached);
    if (g_H1.Meta == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    g_H1.MetaPa = MmGetPhysicalAddress(g_H1.Meta);
    RtlZeroMemory(g_H1.Meta, H1_META_BYTES);
    msmmu = (volatile ULONG *)(g_H1.Meta + H1_META_MSMMU_OFF);
    for (i = 0u; i < H1_META_MAPPED; ++i)
        msmmu[i] = (ULONG)((g_H1.MetaPa.QuadPart >> 12) + i) | SMMU_ATTR_RWNS;
    KeMemoryBarrier();
    Trace(L"H1MetaPa", g_H1.MetaPa.LowPart);
    return STATUS_SUCCESS;
}

static VOID H1FreeMeta(VOID);
static EVT_WDF_OBJECT_CONTEXT_CLEANUP H1EvtDeviceCleanup;
static VOID H1EvtDeviceCleanup(WDFOBJECT Object)
{
    UNREFERENCED_PARAMETER(Object);
    H1FreeMeta();
}

static VOID H1FreeMeta(VOID)
{
    /* Device teardown: all files are closed, nothing is bound. */
    if (g_H1.Meta != NULL && !g_H1.Quarantined)
        MmFreeContiguousMemorySpecifyCache(g_H1.Meta, H1_META_BYTES, MmNonCached);
    g_H1.Meta = NULL;
}

/* Lock held. */
static NTSTATUS H1TakeSlot(PSWGPU_H1_FILE F)
{
    ULONG s;
    if (F->HasSlot) return STATUS_SUCCESS;
    for (s = 0u; s < H1_SLOTS; ++s) {
        if (!g_H1.Slot[s].InUse) {
            g_H1.Slot[s].InUse = TRUE;
            RtlClearAllBits(&g_H1.Slot[s].Map);
            RtlZeroMemory((PVOID)H1GpuPt(s), H1_PT_BYTES);
            RtlZeroMemory((PVOID)H1SmmuPt(s), H1_SMMU_PTS * PAGE_SIZE);
            F->HasSlot = TRUE;
            F->Slot = s;
            F->Process = PsGetCurrentProcess();
            ObReferenceObject(F->Process);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

/* Lock held. Caller is in the owner's process context or attached to it. */
static VOID H1ReleaseAlloc(PSWGPU_H1_FILE F, H1_ALLOC *A)
{
    volatile ULONGLONG *pt = H1GpuPt(F->Slot);
    volatile ULONG *spt = H1SmmuPt(F->Slot);
    ULONG i;
    if (!A->Used) return;
    if (A->UserVa != NULL) MmUnmapLockedPages(A->UserVa, A->Mdl);
    for (i = 0u; i < A->Pages; ++i) { pt[A->FirstPage + i] = 0u; spt[A->FirstPage + i] = 0u; }
    KeMemoryBarrier();
    RtlClearBits(&g_H1.Slot[F->Slot].Map, A->FirstPage, A->Pages + 1u);
    if (!g_H1.Quarantined) {
        MmFreePagesFromMdl(A->Mdl);
        ExFreePool(A->Mdl);
    }
    F->Pages -= A->Pages;
    --F->Allocs;
    RtlZeroMemory(A, sizeof(*A));
}

static NTSTATUS H1Alloc(PSWGPU_H1_FILE F, const SWGPU_H1_ALLOC_IN *In, SWGPU_H1_ALLOC_OUT *Out)
{
    PHYSICAL_ADDRESS low, high, skip;
    PMDL mdl = NULL;
    PVOID userVa = NULL;
    PPFN_NUMBER pfns;
    ULONG pages, first, i, k;
    NTSTATUS status;
    H1_ALLOC *a = NULL;

    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    if (In->Version != SWGPU_H1_VERSION || In->Bytes == 0u || In->Bytes > H1_MAX_ALLOC)
        return STATUS_INVALID_PARAMETER;
    pages = (In->Bytes + PAGE_SIZE - 1u) / PAGE_SIZE;

    low.QuadPart = 0; high.QuadPart = 0x3FFFFFFFFll; skip.QuadPart = 0;  /* SMMU PFN field: 22 bits */
    mdl = MmAllocatePagesForMdlEx(low, high, skip, (SIZE_T)pages * PAGE_SIZE, MmWriteCombined,
                                  MM_ALLOCATE_FULLY_REQUIRED);
    if (mdl == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    if (MmGetMdlByteCount(mdl) != pages * PAGE_SIZE) {
        MmFreePagesFromMdl(mdl); ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pfns = MmGetMdlPfnArray(mdl);
    for (i = 0u; i < pages; ++i) {
        if (pfns[i] >= (1ull << 22)) {
            MmFreePagesFromMdl(mdl); ExFreePool(mdl);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    __try {
        userVa = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmWriteCombined, NULL, FALSE,
                                              NormalPagePriority | MdlMappingNoExecute);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        userVa = NULL;
    }
    if (userVa == NULL) {
        MmFreePagesFromMdl(mdl); ExFreePool(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    H1Lock();
    status = H1EnsureMeta();
    if (NT_SUCCESS(status) && g_H1.Quarantined) status = STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status) && F->HasSlot && F->Process != PsGetCurrentProcess())
        status = STATUS_ACCESS_DENIED;          /* one process per handle */
    if (NT_SUCCESS(status)) status = H1TakeSlot(F);
    if (NT_SUCCESS(status)) {
        for (k = 0u; k < H1_MAX_ALLOCS && F->Alloc[k].Used; ++k) { }
        if (k == H1_MAX_ALLOCS) status = STATUS_INSUFFICIENT_RESOURCES;
        else a = &F->Alloc[k];
    }
    if (NT_SUCCESS(status)) {
        /* One unmapped guard page after every allocation. */
        first = RtlFindClearBitsAndSet(&g_H1.Slot[F->Slot].Map, pages + 1u, 0u);
        if (first == 0xFFFFFFFFu) status = STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!NT_SUCCESS(status)) {
        H1Unlock();
        MmUnmapLockedPages(userVa, mdl);
        MmFreePagesFromMdl(mdl); ExFreePool(mdl);
        return status;
    }
    {
        volatile ULONGLONG *pt = H1GpuPt(F->Slot);
        volatile ULONG *spt = H1SmmuPt(F->Slot);
        for (i = 0u; i < pages; ++i) {
            spt[first + i] = (ULONG)pfns[i] | SMMU_ATTR_RWNS;
            pt[first + i] = H1GpuPte(F->Slot, first + i);
        }
        KeMemoryBarrier();
    }
    a->Used = TRUE; a->FirstPage = first; a->Pages = pages; a->Mdl = mdl; a->UserVa = userVa;
    ++F->Allocs;
    F->Pages += pages;
    Out->Handle = k + 1u;
    Out->GpuVa = H1_USER_VA + first * PAGE_SIZE;
    Out->Bytes = pages * PAGE_SIZE;
    Out->Slot = F->Slot;
    Out->Pages = pages;
    Out->CpuVa = (ULONGLONG)(ULONG_PTR)userVa;
    H1Unlock();
    Trace(L"H1LastAllocSlot", F->Slot);
    Trace(L"H1LastAllocVa", Out->GpuVa);
    return STATUS_SUCCESS;
}

static BOOLEAN H2BeforeFree(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, const H1_ALLOC *A, BOOLEAN Gate);

static NTSTATUS H1Free(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, const SWGPU_H1_FREE_IN *In)
{
    NTSTATUS status = STATUS_INVALID_HANDLE;
    BOOLEAN gate = FALSE;
    ULONG i;
    if (In->Version != SWGPU_H1_VERSION) return STATUS_INVALID_PARAMETER;
    /* 0.17.0: a live H2 channel may hold this slot's TLB entries; flushing
     * them needs the GPU, so take BlitGate first (same order as the queue). */
    for (i = 0u; F->Channel != NULL && i < 1000u && !gate; ++i) {
        gate = InterlockedCompareExchange(&Dev->BlitGate, 1, 0) == 0;
        if (!gate) Ms(1u);
    }
    H1Lock();
    if (F->HasSlot && F->Process == PsGetCurrentProcess() &&
        In->Handle >= 1u && In->Handle <= H1_MAX_ALLOCS && F->Alloc[In->Handle - 1u].Used) {
        if (H2BeforeFree(Dev, F, &F->Alloc[In->Handle - 1u], gate)) {
            H1ReleaseAlloc(F, &F->Alloc[In->Handle - 1u]);
            status = STATUS_SUCCESS;
        } else {
            status = STATUS_DEVICE_BUSY;
        }
    }
    H1Unlock();
    if (gate) InterlockedExchange(&Dev->BlitGate, 0);
    return status;
}

/* Lock held. A range is valid only inside one allocation of this handle. */
static H1_ALLOC *H1FindRange(PSWGPU_H1_FILE F, ULONG Va, ULONG Bytes)
{
    ULONG k;
    ULONGLONG off, end;
    if (Va < H1_USER_VA) return NULL;
    off = (ULONGLONG)Va - H1_USER_VA;
    end = off + Bytes;
    if (end > H1_USER_BYTES) return NULL;
    for (k = 0u; k < H1_MAX_ALLOCS; ++k) {
        const H1_ALLOC *a = &F->Alloc[k];
        ULONGLONG s = (ULONGLONG)a->FirstPage * PAGE_SIZE, e = s + (ULONGLONG)a->Pages * PAGE_SIZE;
        if (a->Used && off >= s && end <= e) return &F->Alloc[k];
    }
    return NULL;
}

/* Lock held. Every page of the range reads back as this slot needs it. */
static BOOLEAN H1CheckPtes(PSWGPU_H1_FILE F, const H1_ALLOC *A, ULONG Va, ULONG Bytes, PULONG Checked)
{
    volatile ULONGLONG *pt = H1GpuPt(F->Slot);
    volatile ULONG *spt = H1SmmuPt(F->Slot);
    PPFN_NUMBER pfns = MmGetMdlPfnArray(A->Mdl);
    ULONG first = (Va - H1_USER_VA) / PAGE_SIZE, last = (Va - H1_USER_VA + Bytes - 1u) / PAGE_SIZE, p;
    for (p = first; p <= last; ++p) {
        ++*Checked;
        if (pt[p] != H1GpuPte(F->Slot, p) ||
            spt[p] != ((ULONG)pfns[p - A->FirstPage] | SMMU_ATTR_RWNS)) return FALSE;
    }
    return pt[A->FirstPage + A->Pages] == 0u && spt[A->FirstPage + A->Pages] == 0u;  /* guard */
}

/* Publish the outer SMMU PDEs of every slot in use plus the meta window.
 * The GPU reaches an IOVA only through a GMMU table that maps it, so keeping
 * them present is safe: the GMMU tables (kernel PDE 8 per copy, one page
 * directory per H2 channel) are what separate the processes.
 * Lock held. Returns TRUE if anything changed (caller flushes). */
static BOOLEAN H1PublishOuter(VOID)
{
    volatile ULONG *outerPd = (volatile ULONG *)g_AcrVm.Arena;
    const ULONG base = H1_IOVA_BASE >> 22;
    BOOLEAN changed = FALSE;
    ULONG i, v;
    if (outerPd == NULL || g_H1.Meta == NULL) return FALSE;
    for (i = 0u; i < H1_SLOTS * H1_SMMU_PTS; ++i) {
        ULONG s = i / H1_SMMU_PTS, j = i % H1_SMMU_PTS;
        v = g_H1.Slot[s].InUse ? H1OuterPde(H1_META_SMMU_OFF + (s * H1_SMMU_PTS + j) * PAGE_SIZE) : 0u;
        if (outerPd[base + i] != v) { outerPd[base + i] = v; changed = TRUE; }
    }
    v = H1OuterPde(H1_META_MSMMU_OFF);
    if (outerPd[H1_META_IOVA >> 22] != v) { outerPd[H1_META_IOVA >> 22] = v; changed = TRUE; }
    KeMemoryBarrier();
    return changed;
}

/* BlitGate and lock held. Publish only this slot's PDEs. */
static BOOLEAN H1Bind(PSWGPU_DEVICE Dev, ULONG Slot, SWGPU_H1_COPY_OUT *Out)
{
    volatile ULONG *outerPd = (volatile ULONG *)g_AcrVm.Arena;
    volatile ULONGLONG *gpuPd = (volatile ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGD_OFF);
    const ULONG base = H1_IOVA_BASE >> 22;
    ULONG i, clear = 0u, unused = 0u;
    (VOID)H1PublishOuter();
    gpuPd[H1_USER_VA >> 27] = H1GpuPde(Slot);
    KeMemoryBarrier();
    /* Kernel channel: PDE 8 names only this slot's table. Slots not in use
     * have no outer PDE at all. */
    for (i = 0u; i < H1_SLOTS * H1_SMMU_PTS; ++i) {
        if (g_H1.Slot[i / H1_SMMU_PTS].InUse) continue;
        ++unused;
        if (outerPd[base + i] == 0u) ++clear;
    }
    Out->OtherPdesClear = clear;
    if (clear != unused || gpuPd[H1_USER_VA >> 27] != H1GpuPde(Slot))
        return FALSE;
    F2FlushOuter(Dev);
    return F2Invalidate(Dev, &Out->MapPolls);
}

static BOOLEAN H1Unbind(PSWGPU_DEVICE Dev, SWGPU_H1_COPY_OUT *Out)
{
    volatile ULONG *outerPd = (volatile ULONG *)g_AcrVm.Arena;
    volatile ULONGLONG *gpuPd = (volatile ULONGLONG *)((PUCHAR)g_GrVm.Arena + GR_VM_GPU_PGD_OFF);
    if (outerPd == NULL || g_GrVm.Arena == NULL) return TRUE;
    gpuPd[H1_USER_VA >> 27] = 0u;     /* outer PDEs stay: H2 channels may use them */
    KeMemoryBarrier();
    if (Dev->BlitReset) return TRUE;
    F2FlushOuter(Dev);
    return F2Invalidate(Dev, &Out->UnmapPolls);
}

/* Same CE2 / host semaphore sequence as F2SubmitPlan (single run), with
 * the source and destination in the bound H1 window. */
static NTSTATUS H1Submit(PSWGPU_DEVICE Dev, ULONG SrcVa, ULONG DstVa, ULONG LineBytes,
                         ULONG Lines, F2_RUN *Run)
{
    const ULONG pushOff = GR_VM_F1_PUSH_OFF, pushBytes = PAGE_SIZE, waitMs = 2000u;
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
    if (put >= ringEntries || Rd(Dev->Bar1Cpu, getOffset) != put) return STATUS_DEVICE_BUSY;
    if (Rd(Dev->Bar0, 0x0000259Cu) || (Rd(Dev->Bar0, FIFO_INTR_0) & 0x10000000u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    next = (put + 1u) % ringEntries;
    sem[0] = 0u; sem[4] = 0u;
    push[n++] = 0x60010000u | (F1_COPY_SUBCHANNEL << 13);
    push[n++] = 0xB0B5u;
    push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x0240u, 3u);
    push[n++] = 0u; push[n++] = ceSemVa; push[n++] = 0xB1000001u;
    push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x0400u, 8u);
    push[n++] = 0u; push[n++] = SrcVa; push[n++] = 0u; push[n++] = DstVa;
    push[n++] = LineBytes; push[n++] = LineBytes;     /* pitch in / out */
    push[n++] = LineBytes; push[n++] = Lines;
    push[n++] = F1Method(F1_COPY_SUBCHANNEL, 0x0300u, 1u);
    push[n++] = 0x38Eu;   /* non-pipelined, flush, release semaphore, pitch/pitch, multi-line */
    push[n++] = F1Method(0u, 0x0010u, 4u);
    push[n++] = 0u; push[n++] = ceSemVa; push[n++] = 0xB1000001u; push[n++] = 1u;
    push[n++] = F1Method(0u, 0x0010u, 4u);
    push[n++] = 0u; push[n++] = hostSemVa; push[n++] = 0xB1000002u; push[n++] = 0x01100002u;
    if (useIrq) {
        push[n++] = 0x20010008u; push[n++] = 0u;
        KeClearEvent(&Dev->E3Event);
    }
    ring[put * 2u] = pushVa;
    ring[put * 2u + 1u] = n << 10;
    KeMemoryBarrier();
    if (!F2L2(Dev, writes, &l2Polls)) return STATUS_DEVICE_HARDWARE_ERROR;
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
    for (i = 0u; i < 4000u; ++i) {
        if (!F2L2(Dev, writes, &l2Polls)) { polls += l2Polls; status = STATUS_DEVICE_HARDWARE_ERROR; break; }
        polls += l2Polls + 1u;
        KeMemoryBarrier();
        if (sem[0] == 0xB1000001u && sem[4] == 0xB1000002u && Rd(Dev->Bar1Cpu, getOffset) == next) {
            Run->Issued = FALSE;
            status = STATUS_SUCCESS;
            break;
        }
        if (Rd(Dev->Bar0, 0x0000259Cu) ||
            KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart >= freq.QuadPart * waitMs / 1000)
            break;
        if (i < 200u) Us(10u); else Ms(1u);
    }
    F2SampleWait(Run, start, freq.QuadPart, polls);
    return status;
}

/* Queue dispatch, BlitGate held. */
static NTSTATUS H1Copy(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, const SWGPU_H1_COPY_IN *In,
                       SWGPU_H1_COPY_OUT *Out)
{
    H1_ALLOC *src, *dst;
    F2_RUN run;
    NTSTATUS status;
    ULONG lineBytes, lines;
    RtlZeroMemory(Out, sizeof(*Out));
    RtlZeroMemory(&run, sizeof(run));
    Out->Version = SWGPU_VERSION;
    if (In->Version != SWGPU_H1_VERSION || In->Bytes == 0u || In->Bytes > H1_MAX_ALLOC)
        return STATUS_INVALID_PARAMETER;
    if ((In->Bytes & (PAGE_SIZE - 1u)) == 0u) { lineBytes = PAGE_SIZE; lines = In->Bytes / PAGE_SIZE; }
    else if (In->Bytes < 0x100000u) { lineBytes = In->Bytes; lines = 1u; }
    else return STATUS_INVALID_PARAMETER;
    Out->Lines = lines; Out->LineBytes = lineBytes;

    H1Lock();
    Out->Slot = F->HasSlot ? F->Slot : 0xFFFFFFFFu;
    src = F->HasSlot ? H1FindRange(F, In->SrcVa, In->Bytes) : NULL;
    dst = F->HasSlot ? H1FindRange(F, In->DstVa, In->Bytes) : NULL;
    if (src == NULL || dst == NULL ||
        ((ULONGLONG)In->SrcVa < (ULONGLONG)In->DstVa + In->Bytes &&
         (ULONGLONG)In->DstVa < (ULONGLONG)In->SrcVa + In->Bytes)) {
        ++F->Rejected;
        status = STATUS_ACCESS_VIOLATION;       /* not this handle's memory */
        goto out;
    }
    if (g_H1.Quarantined) { status = SWGPU_BLIT_DMA_UNSAFE; goto out; }
    if (InterlockedCompareExchange(&Dev->BlitOnline, 0, 0) == 0) {
        (VOID)G1RequestWake(Dev);
        status = STATUS_DEVICE_NOT_READY;
        goto out;
    }
    if (!F2CanRun(Dev)) { status = STATUS_DEVICE_NOT_READY; goto out; }
    if (!H1CheckPtes(F, src, In->SrcVa, In->Bytes, &Out->PtesChecked) ||
        !H1CheckPtes(F, dst, In->DstVa, In->Bytes, &Out->PtesChecked)) {
        status = STATUS_DEVICE_DATA_ERROR;
        goto out;
    }
    run.Mapped = TRUE;
    if (!H1Bind(Dev, F->Slot, Out)) {
        status = STATUS_DEVICE_HARDWARE_ERROR;
    } else {
        status = H1Submit(Dev, In->SrcVa, In->DstVa, lineBytes, lines, &run);
    }
    Out->FaultStatus = Rd(Dev->Bar0, 0x0000259Cu);
    Out->PbdmaIntr = Rd(Dev->Bar0, 0x00040108u);
    Out->CeIntr = Rd(Dev->Bar0, F1_CE_INTR_STATUS);
    if (run.Issued && !F2Abort(Dev, &run)) {
        /* Reset/drain not confirmed: the GPU may still touch these pages. */
        g_H1.Quarantined = TRUE;
        status = SWGPU_BLIT_DMA_UNSAFE;
    }
    if (!H1Unbind(Dev, Out) && NT_SUCCESS(status)) {
        (VOID)F2Abort(Dev, &run);
        status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    Out->WaitUs = run.MaxWaitUs; Out->Polls = run.MaxPolls; Out->IrqWakes = run.IrqWakes;
    InterlockedExchange64(&Dev->LastBlitTime, (LONG64)KeQueryInterruptTime());
out:
    if (NT_SUCCESS(status)) ++F->Copies; else if (status != STATUS_ACCESS_VIOLATION &&
        status != STATUS_DEVICE_NOT_READY) ++F->CopyFailures;
    Out->Result = (ULONG)status;
    H1Unlock();
    Trace(L"H1LastCopyResult", (ULONG)status);
    return status;
}

/* Caller context. Reads only memory: no GPU access. */
static VOID H1Info(PSWGPU_DEVICE Dev, PSWGPU_H1_FILE F, SWGPU_H1_INFO_OUT *Out)
{
    ULONG s, p, k;
    RtlZeroMemory(Out, sizeof(*Out));
    Out->Version = SWGPU_VERSION;
    Out->ProcessId = (ULONG)(ULONG_PTR)PsGetCurrentProcessId();
    Out->Resident = Dev->ResidentActive && !Dev->BlitReset ? 1u : 0u;
    Out->BlitOnline = (ULONG)InterlockedCompareExchange(&Dev->BlitOnline, 0, 0);
    H1Lock();
    for (s = 0u; s < H1_SLOTS; ++s) if (g_H1.Slot[s].InUse) ++Out->SlotsInUse;
    Out->Quarantined = g_H1.Quarantined ? 1u : 0u;
    Out->MetaPa = g_H1.MetaPa.LowPart;
    Out->Slot = F->HasSlot ? F->Slot : 0xFFFFFFFFu;
    Out->Allocs = F->Allocs; Out->Pages = F->Pages;
    Out->Copies = F->Copies; Out->CopyFailures = F->CopyFailures; Out->Rejected = F->Rejected;
    if (F->HasSlot && g_H1.Meta != NULL) {
        volatile ULONGLONG *pt = H1GpuPt(F->Slot);
        volatile ULONG *spt = H1SmmuPt(F->Slot);
        /* The whole 128 MiB table: entries past 32 MiB must stay empty too. */
        for (p = 0u; p < H1_PT_BYTES / sizeof(ULONGLONG); ++p) {
            ULONGLONG e = pt[p];
            BOOLEAN owned = FALSE;
            if (e == 0u) continue;
            ++Out->ValidGpuPtes;
            /* Anything but this slot's own IOVA for this page is foreign. */
            if (p >= H1_USER_PAGES || e != H1GpuPte(F->Slot, p)) ++Out->ForeignPtes;
            for (k = 0u; k < H1_MAX_ALLOCS && !owned; ++k)
                owned = F->Alloc[k].Used && p >= F->Alloc[k].FirstPage &&
                        p < F->Alloc[k].FirstPage + F->Alloc[k].Pages;
            if (!owned) ++Out->OrphanPtes;
        }
        for (p = 0u; p < H1_USER_PAGES; ++p) if (spt[p] != 0u) ++Out->ValidSmmuPtes;
        Out->IsolationOk = Out->ForeignPtes == 0u && Out->OrphanPtes == 0u &&
            Out->ValidGpuPtes == F->Pages && Out->ValidSmmuPtes == F->Pages ? 1u : 0u;
    }
    H1Unlock();
}

/* ntifs.h is not included here: KAPC_STATE (0x30 bytes on ARM64) is opaque. */
NTKERNELAPI VOID KeStackAttachProcess(PRKPROCESS Process, PVOID ApcState);
NTKERNELAPI VOID KeUnstackDetachProcess(PVOID ApcState);

static VOID H1EvtFileCleanup(WDFFILEOBJECT FileObject)
{
    PSWGPU_H1_FILE f = SwGpuH1File(FileObject);
    DECLSPEC_ALIGN(16) ULONGLONG apc[8];
    BOOLEAN attached = FALSE;
    ULONG k;
    if (!g_H1.LockReady || !f->HasSlot) return;
    /* H2: stop the channel before its memory goes. */
    H2CleanupFile(SwGpuGetDevice(WdfFileObjectGetDevice(FileObject)), f);
    if (f->Process != PsGetCurrentProcess()) {
        KeStackAttachProcess((PRKPROCESS)f->Process, apc);
        attached = TRUE;
    }
    /* A copy holds the lock until its DMA is finished or reset. */
    H1Lock();
    for (k = 0u; k < H1_MAX_ALLOCS; ++k) H1ReleaseAlloc(f, &f->Alloc[k]);
    RtlZeroMemory((PVOID)H1GpuPt(f->Slot), H1_PT_BYTES);
    RtlZeroMemory((PVOID)H1SmmuPt(f->Slot), H1_SMMU_PTS * PAGE_SIZE);
    g_H1.Slot[f->Slot].InUse = FALSE;
    f->HasSlot = FALSE;
    H1Unlock();
    if (attached) KeUnstackDetachProcess(apc);
    ObDereferenceObject(f->Process);
    f->Process = NULL;
    Trace(L"H1Cleanups", ReadParam(L"H1Cleanups", 0u) + 1u);
}
