/* F4 shares TvWriteWindow's assembly -> update -> activate sequence.
 * READ_MUX selects the live bank (Tegra tegra_dc_finish_page_flip). */
#define SWD_DC_READ_ACTIVE 1u
#define SWD_FLIP_REQ (GENERAL_ACT_REQ | WIN_A_ACT_REQ)
#define SWD_WIN_A_ERRORS ((1u << 8) | (1u << 14))

static BOOLEAN SwdFlipLock(PKEVENT Event)
{
    LARGE_INTEGER t;
    t.QuadPart = -2500000ll;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS;
}

static VOID SwdFlipUnlock(PKEVENT Event)
{
    KeSetEvent(Event, IO_NO_INCREMENT, FALSE);
}

static PUCHAR SwdFlipVa(PSWD_CONTEXT Ctx, BOOLEAN Tv, LONG Index)
{
    if (Tv) return Index ? Ctx->TvFlip.Second : Ctx->TvVa;
    return Index ? Ctx->FbFlip.Second : Ctx->FbVa;
}

static VOID SwdCopyRows(PUCHAR To, PUCHAR From, ULONG Pitch, LONG L, LONG T, LONG R, LONG B)
{
    LONG y;
    if (L >= R || T >= B) return;
    for (y = T; y < B; ++y) {
        SIZE_T off = (SIZE_T)y * Pitch + (SIZE_T)L * 4u;
        RtlCopyMemory(To + off, From + off, (SIZE_T)(R - L) * 4u);
    }
}

static ULONGLONG SwdFlipActive(PUCHAR Dc)
{
    ULONG hdr = READ_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER));
    ULONG access = READ_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG));
    ULONG lo, hi;
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER), DC_WIN_A_SELECT);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG), SWD_DC_READ_ACTIVE);
    lo = READ_REGISTER_ULONG((PULONG)(Dc + DC_WINBUF_START));
    hi = READ_REGISTER_ULONG((PULONG)(Dc + DC_WINBUF_START_HI));
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG), access);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER), hdr);
    return ((ULONGLONG)hi << 32) | lo;
}

static BOOLEAN SwdFlipWait(PUCHAR Dc, ULONGLONG Phys, SWD_FLIP *Flip)
{
    LARGE_INTEGER start, freq;
    ULONG i, us;
    BOOLEAN done = FALSE;
    start = KeQueryPerformanceCounter(&freq);
    for (i = 1u; i <= 50u; ++i) {
        LARGE_INTEGER delay;
        ULONG pending = READ_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_CONTROL_REG));
        if (!(pending & SWD_FLIP_REQ) && SwdFlipActive(Dc) == Phys) { done = TRUE; break; }
        if (i == 50u || SwdGpuUs(start, freq) >= 49000u) break;
        delay.QuadPart = -10000ll;
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
    us = SwdGpuUs(start, freq);
    Flip->Polls = i;
    if (us > Flip->MaxUs) Flip->MaxUs = us;
    if (!done) {
        ++Flip->Timeouts;
        Flip->Status = STATUS_IO_TIMEOUT;
    }
    return done;
}

static VOID SwdFlipFallback(SWD_FLIP *Flip, ULONG Status)
{
    Flip->Enabled = FALSE;
    ++Flip->Fallbacks;
    Flip->Status = Status;
}

static BOOLEAN SwdFlipRequest(PSWD_CONTEXT Ctx, BOOLEAN Tv, LONG Index)
{
    SWD_FLIP *flip = Tv ? &Ctx->TvFlip : &Ctx->FbFlip;
    PUCHAR dc = Tv ? Ctx->TvDc : Ctx->DcVa;
    ULONGLONG phys = Index ? (ULONGLONG)flip->Phys.QuadPart :
                             (ULONGLONG)(Tv ? Ctx->TvPhys.QuadPart : Ctx->FbPhys.QuadPart);
    ULONG hdr, access;
    InterlockedExchange(&flip->Pending, 1);
    KeMemoryBarrier();
    hdr = READ_REGISTER_ULONG((PULONG)(dc + DC_WINDOW_HEADER));
    access = READ_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_ACCESS_REG));
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_ACCESS_REG), 0u);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_WINDOW_HEADER), DC_WIN_A_SELECT);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_WINBUF_START), (ULONG)phys);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_WINBUF_START_HI), (ULONG)(phys >> 32));
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG), GENERAL_UPDATE | WIN_A_UPDATE);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG), SWD_FLIP_REQ);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_WINDOW_HEADER), hdr);
    WRITE_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_ACCESS_REG), access);
    if (!SwdFlipWait(dc, phys, flip)) return FALSE;
    InterlockedExchange(&flip->Front, Index);
    InterlockedExchange(&flip->Pending, 0);
    return TRUE;
}

/* Contiguous buffers allocated here are ordinary nonpaged storage with a WC
 * system mapping. The boot framebuffer remains an IO-space destination only. */
static PMDL SwdOwnedScanoutMdl(PUCHAR Va, SIZE_T Bytes)
{
    PMDL mdl = IoAllocateMdl(Va, (ULONG)Bytes, FALSE, FALSE, NULL);
    if (mdl) MmBuildMdlForNonPagedPool(mdl);
    return mdl;
}

static VOID SwdFlipFree(SWD_FLIP *Flip, BOOLEAN Safe);

static BOOLEAN SwdPanelMdlAllowed(PSWD_CONTEXT Ctx)
{
    ULONGLONG phys = (ULONGLONG)Ctx->FbPhys.QuadPart;
    return phys >= SWD_FB_PHYS && phys < SWD_FB_PHYS + SWD_FB_RESERVED &&
        Ctx->FbMapSize <= SWD_FB_PHYS + SWD_FB_RESERVED - phys;
}

static VOID SwdFlipAllocate(PSWD_CONTEXT Ctx, BOOLEAN Tv)
{
    SWD_FLIP *flip = Tv ? &Ctx->TvFlip : &Ctx->FbFlip;
    PUCHAR dc = Tv ? Ctx->TvDc : Ctx->DcVa;
    PHYSICAL_ADDRESS primary = Tv ? Ctx->TvPhys : Ctx->FbPhys;
    PHYSICAL_ADDRESS lo = {0}, hi, skip = {0};
    SIZE_T bytes = Tv ? Ctx->TvSize : Ctx->FbMapSize;
    ULONGLONG end = (ULONGLONG)primary.QuadPart + bytes;
    ULONGLONG next = (end + PAGE_SIZE - 1u) & ~((ULONGLONG)PAGE_SIZE - 1u);
    flip->Status = 0u;
    if ((Tv && !Ctx->TvMdl) || (!Tv && SwdPanelMdlAllowed(Ctx) && !Ctx->FbMdl)) {
        SwdFlipFallback(flip, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    if (!dc || primary.QuadPart < 0 || end > 0x100000000ull ||
        (primary.QuadPart & (PAGE_SIZE - 1u)) ||
        !SwdFlipWait(dc, (ULONGLONG)primary.QuadPart, flip)) {
        SwdFlipFallback(flip, flip->Status ? flip->Status : STATUS_NOT_SUPPORTED);
        return;
    }

    /* Only use unused tail space if UEFI's primary belongs to this reserve. */
    if (!Tv && (ULONGLONG)primary.QuadPart >= SWD_FB_PHYS &&
        next <= SWD_FB_PHYS + SWD_FB_RESERVED && bytes <= SWD_FB_PHYS + SWD_FB_RESERVED - next) {
        flip->Phys.QuadPart = (LONGLONG)next;
        flip->Second = (PUCHAR)MmMapIoSpaceEx(flip->Phys, bytes, PAGE_READWRITE | PAGE_WRITECOMBINE);
        flip->Mapped = flip->Second != NULL;
    }
    if (!flip->Second) {
        hi.QuadPart = 0xFFFFFFFFll;
        flip->Second = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(bytes, lo, hi, skip, MmWriteCombined);
        if (flip->Second) flip->Phys = MmGetPhysicalAddress(flip->Second);
    }
    if (!flip->Second) {
        SwdFlipFallback(flip, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    flip->Bytes = bytes;
    flip->Mdl = flip->Mapped ? SwdGpuScanoutMdl(flip->Second, flip->Phys, bytes) :
                               SwdOwnedScanoutMdl(flip->Second, bytes);
    if (!flip->Mdl) {
        SwdFlipFree(flip, TRUE);
        SwdFlipFallback(flip, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }
    RtlCopyMemory(flip->Second, SwdFlipVa(Ctx, Tv, 0), bytes);
    KeMemoryBarrier();
    flip->Front = 0;
    flip->Pending = 0;
    flip->Status = 0u;
    flip->Enabled = TRUE;
}

static VOID SwdFlipFree(SWD_FLIP *Flip, BOOLEAN Safe)
{
    if (Safe) {
        if (Flip->Mdl) IoFreeMdl(Flip->Mdl);
        if (Flip->Second) {
            if (Flip->Mapped) MmUnmapIoSpace(Flip->Second, Flip->Bytes);
            else MmFreeContiguousMemorySpecifyCache(Flip->Second, Flip->Bytes, MmWriteCombined);
        }
    }
    /* If scanout or DMA cannot be ruled out, retain the allocation and MDL. */
    if (!Safe && Flip->Second) ++Flip->Retained;
    Flip->Enabled = FALSE;
    Flip->Pending = 0;
    Flip->Front = 0;
    Flip->Second = NULL;
    Flip->Mdl = NULL;
    Flip->Mapped = FALSE;
}

/* After a timeout both copies get the latest image. A delayed latch then
 * cannot show the previous frame. Never free while ACT_REQ is unresolved. */
static BOOLEAN SwdFlipComplete(PSWD_CONTEXT Ctx, BOOLEAN Tv, LONG Latest)
{
    SWD_FLIP *flip = Tv ? &Ctx->TvFlip : &Ctx->FbFlip;
    PUCHAR dc = Tv ? Ctx->TvDc : Ctx->DcVa;
    ULONG before = READ_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_STATUS_REG));
    ULONG status = STATUS_IO_TIMEOUT;
    if (SwdFlipRequest(Ctx, Tv, Latest)) {
        ULONG after = READ_REGISTER_ULONG((PULONG)(dc + DC_CMD_INT_STATUS_REG));
        ++flip->Flips;
        if (!(after & ~before & SWD_WIN_A_ERRORS)) return TRUE;
        status = STATUS_DEVICE_HARDWARE_ERROR;
    }
    SwdFlipFallback(flip, status);
    RtlCopyMemory(SwdFlipVa(Ctx, Tv, 1 - Latest), SwdFlipVa(Ctx, Tv, Latest), flip->Bytes);
    KeMemoryBarrier();
    return FALSE;
}

static LONG SwdFlipDrawIndex(PSWD_CONTEXT Ctx, BOOLEAN Tv)
{
    SWD_FLIP *flip = Tv ? &Ctx->TvFlip : &Ctx->FbFlip;
    if (flip->Enabled) return 1 - flip->Front;
    if (flip->Pending) {
        PUCHAR dc = Tv ? Ctx->TvDc : Ctx->DcVa;
        ULONG pending = READ_REGISTER_ULONG((PULONG)(dc + DC_CMD_STATE_CONTROL_REG));
        ULONGLONG active = SwdFlipActive(dc);
        ULONGLONG primary = (ULONGLONG)(Tv ? Ctx->TvPhys.QuadPart : Ctx->FbPhys.QuadPart);
        if (active == (ULONGLONG)flip->Phys.QuadPart || active == primary) {
            InterlockedExchange(&flip->Front, active == primary ? 0 : 1);
            if (!(pending & SWD_FLIP_REQ))
                InterlockedExchange(&flip->Pending, 0);
        }
    }
    return flip->Front;
}

static VOID SwdFlipClear(PSWD_CONTEXT Ctx, BOOLEAN Tv)
{
    SWD_FLIP *flip = Tv ? &Ctx->TvFlip : &Ctx->FbFlip;
    LONG index = SwdFlipDrawIndex(Ctx, Tv);
    if ((Tv ? Ctx->TvDmaUnsafe : Ctx->FbDmaUnsafe) || Ctx->SystemDisplay) return;
    RtlZeroMemory(SwdFlipVa(Ctx, Tv, index), flip->Bytes);
    KeMemoryBarrier();
    if (flip->Enabled && (Tv || !Ctx->DcStopped) && !SwdFlipComplete(Ctx, Tv, index)) return;
    RtlZeroMemory(SwdFlipVa(Ctx, Tv, 1 - index), flip->Bytes);
    KeMemoryBarrier();
}

static BOOLEAN SwdPanelPrimary(PSWD_CONTEXT Ctx)
{
    SWD_FLIP *flip = &Ctx->FbFlip;
    LONG index;
    if (!flip->Second) return TRUE;
    if (!Ctx->DcVa || Ctx->FbDmaUnsafe) return FALSE;
    index = flip->Enabled ? flip->Front : SwdFlipDrawIndex(Ctx, FALSE);
    if (!index && !flip->Pending &&
        !(READ_REGISTER_ULONG((PULONG)(Ctx->DcVa + DC_CMD_STATE_CONTROL_REG)) & SWD_FLIP_REQ) &&
        SwdFlipActive(Ctx->DcVa) == (ULONGLONG)Ctx->FbPhys.QuadPart) return TRUE;
    /* Normal copy-forward has already synchronized the primary. */
    if (!flip->Enabled && index != 0)
        RtlCopyMemory(Ctx->FbVa, flip->Second, flip->Bytes);
    return SwdFlipRequest(Ctx, FALSE, 0);
}

static VOID SwdFlipFlush(PSWD_CONTEXT Ctx)
{
    SWD_FLIP *p = &Ctx->FbFlip, *t = &Ctx->TvFlip;
    RegPutDword(L"F4Active", Ctx->Started);
    RegPutDword(L"F4LockTimeouts", Ctx->F4LockTimeouts);
    RegPutDword(L"F4PanelPrimaryLo", Ctx->FbPhys.LowPart);
    RegPutDword(L"F4PanelPrimaryMdl", Ctx->FbMdl != NULL);
#define FLIP_STAT(Name, Field) \
    RegPutDword(L"F4Panel" L##Name, (ULONG)p->Field); \
    RegPutDword(L"F4Tv" L##Name, (ULONG)t->Field)
    FLIP_STAT("Enabled", Enabled); FLIP_STAT("Front", Front); FLIP_STAT("Flips", Flips);
    FLIP_STAT("Timeouts", Timeouts); FLIP_STAT("MaxUs", MaxUs); FLIP_STAT("Polls", Polls);
    FLIP_STAT("GpuForward", GpuForward); FLIP_STAT("CpuForward", CpuForward);
    FLIP_STAT("Fallbacks", Fallbacks); FLIP_STAT("Retained", Retained); FLIP_STAT("Status", Status);
#undef FLIP_STAT
}
