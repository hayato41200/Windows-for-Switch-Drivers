/* Optional identity-only path. Discovery and registry publication run on
 * the existing guard thread. Present only calls an already-open endpoint. */
static volatile LONG gGpuDisabled;

static VOID SwdGpuConnect(PSWD_CONTEXT Ctx)
{
    PWSTR links = NULL;
    UNICODE_STRING name;
    PFILE_OBJECT file = NULL;
    PDEVICE_OBJECT device = NULL;
    NTSTATUS status;
    if (!Ctx->GpuPresent || gGpuDisabled || Ctx->GpuTarget != NULL) return;
    status = IoGetDeviceInterfaces(&SwGpuBlitDeviceInterface, NULL, 0, &links);
    if (!NT_SUCCESS(status)) return;
    if (links[0] != L'\0') {
        RtlInitUnicodeString(&name, links);
        status = IoGetDeviceObjectPointer(&name, FILE_READ_DATA | FILE_WRITE_DATA, &file, &device);
        if (NT_SUCCESS(status)) {
            Ctx->GpuFile = file;
            InterlockedExchangePointer((PVOID volatile *)&Ctx->GpuTarget, device);
        }
    }
    ExFreePool(links);
}

static VOID SwdGpuDisconnect(PSWD_CONTEXT Ctx)
{
    if (Ctx->GpuFile != NULL) {
        InterlockedExchangePointer((PVOID volatile *)&Ctx->GpuTarget, NULL);
        ObDereferenceObject(Ctx->GpuFile);
        Ctx->GpuFile = NULL;
    }
}

/* These scanout buffers are physically contiguous, write-combined and
 * owned for their entire scanout lifetime. Describe the IO mapping without
 * asking MmBuildMdlForNonPagedPool to look up an MmMapIoSpace address. */
static PMDL SwdGpuScanoutMdl(PUCHAR Va, PHYSICAL_ADDRESS Pa, SIZE_T Bytes)
{
    PMDL mdl;
    PPFN_NUMBER pfns;
    ULONG page, count;
    if (Bytes == 0 || Bytes > SWGPU_BLIT_WINDOW_BYTES ||
        Pa.QuadPart < 0 || (Pa.QuadPart & (PAGE_SIZE - 1u)) != 0u ||
        BYTE_OFFSET(Va) != 0u || (ULONGLONG)Pa.QuadPart + Bytes > 0x100000000ull)
        return NULL;
    mdl = IoAllocateMdl(Va, (ULONG)Bytes, FALSE, FALSE, NULL);
    if (mdl == NULL) return NULL;
    pfns = MmGetMdlPfnArray(mdl);
    count = ((ULONG)Bytes + PAGE_SIZE - 1u) / PAGE_SIZE;
    for (page = 0u; page < count; ++page)
        pfns[page] = (PFN_NUMBER)(Pa.QuadPart >> PAGE_SHIFT) + page;
    mdl->MdlFlags |= MDL_IO_SPACE;
    return mdl;
}

typedef struct _SWD_GPU_CALL {
    KEVENT Done;
    IO_STATUS_BLOCK IoStatus;
} SWD_GPU_CALL;

static NTSTATUS SwdGpuSend(PSWD_CONTEXT Ctx, const SWGPU_BLIT_REQUEST *Req)
{
    PDEVICE_OBJECT device = (PDEVICE_OBJECT)InterlockedCompareExchangePointer(
        (PVOID volatile *)&Ctx->GpuTarget, NULL, NULL);
    SWD_GPU_CALL *call;
    PIRP irp;
    NTSTATUS status;
    LARGE_INTEGER timeout;
    if (device == NULL || KeGetCurrentIrql() != PASSIVE_LEVEL) return STATUS_DEVICE_NOT_READY;
    call = (SWD_GPU_CALL *)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*call), SWD_TAG);
    if (call == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    KeInitializeEvent(&call->Done, NotificationEvent, FALSE);
    irp = IoBuildDeviceIoControlRequest(IOCTL_SWGPU_PRESENT_BLIT, device,
        (PVOID)Req, sizeof(*Req), NULL, 0u, TRUE, &call->Done, &call->IoStatus);
    if (irp == NULL) { ExFreePoolWithTag(call, SWD_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
    IoGetNextIrpStackLocation(irp)->FileObject = Ctx->GpuFile;
    status = IoCallDriver(device, irp);
    if (status == STATUS_PENDING) {
        /* The SwitchGpu endpoint completes inline. A filter may instead
         * pend it. On timeout the I/O manager still owns the IRP and needs
         * the event/status block, so quarantine them and both DMA buffers. */
        timeout.QuadPart = -500000ll;
        if (KeWaitForSingleObject(&call->Done, Executive, KernelMode, FALSE, &timeout) != STATUS_SUCCESS) {
            ObReferenceObject(Ctx->GpuFile);
            return SWGPU_BLIT_DMA_UNSAFE;
        }
        status = call->IoStatus.Status;
    }
    ExFreePoolWithTag(call, SWD_TAG);
    return status;
}

static ULONG SwdGpuUs(LARGE_INTEGER Start, LARGE_INTEGER Frequency)
{
    return (ULONG)((KeQueryPerformanceCounter(NULL).QuadPart - Start.QuadPart) *
        1000000ll / Frequency.QuadPart);
}

static NTSTATUS SwdGpuRects(PSWD_CONTEXT Ctx, const DXGKARG_PRESENT_DISPLAYONLY *Present,
                           PMDL Src, PMDL Dst, LONG SrcTop, LONG Bottom,
                           LONG Width, ULONG DstPitch)
{
    SWGPU_BLIT_REQUEST req = {0};
    ULONG pass, i;
    if (Dst == NULL) return STATUS_DEVICE_NOT_READY;
    req.Version = SWGPU_BLIT_VERSION;
    req.Src = Src; req.Dst = Dst;
    req.SrcPitch = (ULONG)Present->Pitch; req.DstPitch = DstPitch;
    if ((ULONGLONG)Width * 4u > req.SrcPitch || (ULONGLONG)Width * 4u > DstPitch)
        return STATUS_INVALID_PARAMETER;
    for (pass = 0u; pass < 2u; ++pass) {
        ULONG count = pass == 0u ? Present->NumMoves : Present->NumDirtyRects;
        if (count > SWGPU_BLIT_MAX_RECTS) return STATUS_BUFFER_OVERFLOW;
        for (i = 0u; i < count; ++i) {
            RECT r = pass == 0u ? Present->pMoves[i].DestRect : Present->pDirtyRect[i];
            SWGPU_BLIT_RECT *out;
            ULONG x;
            if (r.left < 0) r.left = 0;
            if (r.right > Width) r.right = Width;
            if (r.top < SrcTop) r.top = SrcTop;
            if (r.bottom > Bottom) r.bottom = Bottom;
            if (r.left >= r.right || r.top >= r.bottom) continue;
            if (req.Count == SWGPU_BLIT_MAX_RECTS) return STATUS_BUFFER_OVERFLOW;
            out = &req.Rect[req.Count++];
            x = (ULONG)r.left * 4u;
            out->SrcOffset = (ULONG)(r.top - SrcTop) * req.SrcPitch + x;
            out->DstOffset = (ULONG)r.top * DstPitch + x;
            out->Rows = (ULONG)(r.bottom - r.top);
            out->RowBytes = (ULONG)(r.right - r.left) * 4u;
        }
    }
    return req.Count == 0u ? STATUS_SUCCESS : SwdGpuSend(Ctx, &req);
}

static BOOLEAN SwdGpuTry(PSWD_CONTEXT Ctx, const DXGKARG_PRESENT_DISPLAYONLY *Present,
                          PMDL Src, PMDL Dst, LONG Top, LONG Bottom, LONG Width,
                          ULONG Pitch, BOOLEAN Tv, PBOOLEAN KeepMdl,
                          PBOOLEAN GpuSuccess, PULONG GpuUs)
{
    LARGE_INTEGER freq, start;
    NTSTATUS status;
    ULONG us;
    if (!Ctx->GpuPresent || gGpuDisabled) return FALSE;
    /* The guard thread may still be discovering the GPU after boot. */
    if (Ctx->GpuTarget == NULL) return FALSE;
    start = KeQueryPerformanceCounter(&freq);
    status = SwdGpuRects(Ctx, Present, Src, Dst, Top, Bottom, Width, Pitch);
    us = SwdGpuUs(start, freq);
    InterlockedExchange(&Ctx->GpuLastStatus, (LONG)status);
    if (NT_SUCCESS(status)) {
        *GpuSuccess = TRUE;
        *GpuUs += us;
        InterlockedExchange(&Ctx->GpuStreak, 0);
        return TRUE;
    }
    InterlockedIncrement(&Ctx->GpuFailures);
    if (status == SWGPU_BLIT_DMA_UNSAFE) {
        /* The copy may still be live. Withhold these pages and skip the CPU
         * write to this target. Only this exceptional failure is quarantined. */
        if (Tv) Ctx->TvDmaUnsafe = TRUE;
        else Ctx->FbDmaUnsafe = TRUE;
        *KeepMdl = TRUE;
        InterlockedExchange(&gGpuDisabled, 1);
        return TRUE;
    }
    if (InterlockedIncrement(&Ctx->GpuStreak) >= 3) InterlockedExchange(&gGpuDisabled, 1);
    return FALSE;
}

static VOID SwdGpuFlushCounters(PSWD_CONTEXT Ctx)
{
    LONG gpu = InterlockedCompareExchange(&Ctx->GpuPresents, 0, 0);
    LONG cpu = InterlockedCompareExchange(&Ctx->GpuFallbacks, 0, 0);
    LONGLONG gpuUs = InterlockedCompareExchange64(&Ctx->GpuPresentUs, 0, 0);
    LONGLONG cpuUs = InterlockedCompareExchange64(&Ctx->GpuFallbackUs, 0, 0);
    RegPutDword(L"GpuPresents", (ULONG)gpu);
    RegPutDword(L"GpuFallbacks", (ULONG)cpu);
    RegPutDword(L"GpuFailures", (ULONG)Ctx->GpuFailures);
    RegPutDword(L"GpuAverageUs", gpu > 0 ? (ULONG)(gpuUs / gpu) : 0u);
    RegPutDword(L"GpuCpuAverageUs", cpu > 0 ? (ULONG)(cpuUs / cpu) : 0u);
    RegPutDword(L"GpuLastStatus", (ULONG)Ctx->GpuLastStatus);
    RegPutDword(L"GpuDisabled", (ULONG)gGpuDisabled);
    RegPutDword(L"GpuConnected", Ctx->GpuTarget != NULL);
}
