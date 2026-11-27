/* Claude 1.9.0 (G2a, test only): block-linear scanout with the display
 * controller's transpose (WIN_OPTIONS SCAN_COLUMN), the way FreeBSD's
 * tegra_dc rotates by 90/270 degrees (block-linear surfaces only).
 *
 * Runs once on the guard thread when Parameters\G2aTest = 1 and the GPU
 * endpoint is connected:
 *   1. A 1280x720 test picture (pitch) is copied by the GPU copy engine into a
 *      block-linear buffer (GOB 64 B x 8, 16 GOBs per block), first with PTE
 *      kind 0 (pitch), then with kind 0xFE (generic 16Bx2) if kind 0 did not
 *      match. The CPU compares every byte with its own swizzle.
 *   2. The CPU writes the picture block-linear itself and window A of the
 *      panel shows it transposed, in 4 variants (H/V direction), 6 s each.
 *      Variant N has N yellow squares at the top of the picture.
 *   3. The original window A registers are restored.
 * Results: G2a* values under Parameters. G2aTest is cleared first, so a hang
 * cannot repeat at every boot. */
#define G2A_W           1280u
#define G2A_H           720u
#define G2A_PITCH       (G2A_W * 4u)        /* 5120 = 80 GOBs */
#define G2A_BH_LOG2     4u                  /* 16 GOBs = 128 rows per block */
#define G2A_ROWS        768u                /* 720 rounded up to 128 */
#define G2A_BL_BYTES    (G2A_PITCH * G2A_ROWS)
#define G2A_SHOW_MS     6000u
#define DC_WIN_H_DIRECTION (1u << 0)
#define DC_WIN_V_DIRECTION (1u << 2)
#define DC_WIN_SCAN_COLUMN (1u << 4)
#define DC_KIND_BLOCK      2u

static volatile LONG gG2aState;             /* 0 = not checked, 1 = done or not asked */

/* Maxwell/Tegra GOB (64 B x 8 rows, 512 B) "16Bx2" layout, blocks of
 * 1 << G2A_BH_LOG2 GOBs stacked vertically, blocks row-major. */
static ULONG G2aBlOffset(ULONG Xb, ULONG Y)
{
    const ULONG blockRows = 8u << G2A_BH_LOG2, gobsPerRow = G2A_PITCH / 64u;
    ULONG block = (Y / blockRows) * gobsPerRow + Xb / 64u;
    ULONG gob = (Y % blockRows) / 8u;
    ULONG in = ((Xb % 64u) / 32u) * 256u + ((Y % 8u) / 2u) * 64u +
               ((Xb % 32u) / 16u) * 32u + (Y % 2u) * 16u + (Xb % 16u);
    return block * (512u << G2A_BH_LOG2) + gob * 512u + in;
}

/* B8G8R8A8 in memory = 0xAARRGGBB. White "L" top-left, red top-right, blue
 * bottom-left, Variant yellow squares along the top, gradient and grid. */
static ULONG G2aPixel(ULONG X, ULONG Y, ULONG Variant)
{
    ULONG k;
    if ((X < 240u && Y < 48u) || (X < 48u && Y < 240u)) return 0xFFFFFFFFu;
    if (X >= 1160u && Y < 120u) return 0xFFFF0000u;
    if (X < 120u && Y >= 600u) return 0xFF0000FFu;
    for (k = 0u; k < Variant; ++k)
        if (Y >= 8u && Y < 48u && X >= 300u + k * 60u && X < 340u + k * 60u) return 0xFFFFFF00u;
    if ((X % 160u) == 0u || (Y % 160u) == 0u) return 0xFF808080u;
    return 0xFF000000u | ((X * 255u / (G2A_W - 1u)) << 16) | ((Y * 255u / (G2A_H - 1u)) << 8) | 0x40u;
}

static VOID G2aFillPitch(PUCHAR Va, ULONG Variant)
{
    ULONG x, y;
    for (y = 0u; y < G2A_H; ++y)
        for (x = 0u; x < G2A_W; ++x)
            ((PULONG)(Va + (SIZE_T)y * G2A_PITCH))[x] = G2aPixel(x, y, Variant);
}

static VOID G2aFillBlock(PUCHAR Va, ULONG Variant)
{
    ULONG x, y;
    for (y = 0u; y < G2A_H; ++y)
        for (x = 0u; x < G2A_W; ++x)
            *(PULONG)(Va + G2aBlOffset(x * 4u, y)) = G2aPixel(x, y, Variant);
}

/* Bytes of the first 720 rows that differ from the CPU swizzle; first bad offset. */
static ULONG G2aCompare(PUCHAR Bl, ULONG Variant, PULONG FirstBad)
{
    ULONG x, y, bad = 0u;
    *FirstBad = 0xFFFFFFFFu;
    for (y = 0u; y < G2A_H; ++y)
        for (x = 0u; x < G2A_W; ++x) {
            ULONG off = G2aBlOffset(x * 4u, y);
            if (*(volatile ULONG *)(Bl + off) != G2aPixel(x, y, Variant)) {
                if (bad == 0u) *FirstBad = off;
                ++bad;
            }
        }
    return bad;
}

static NTSTATUS G2aGpuCopy(PSWD_CONTEXT Ctx, PMDL Src, PMDL Dst, ULONG Kind)
{
    SWGPU_BLIT_REQUEST req = {0};
    req.Version = SWGPU_BLIT_VERSION;
    req.Src = Src; req.Dst = Dst;
    req.SrcPitch = G2A_PITCH; req.DstPitch = G2A_PITCH;
    req.Count = 1u;
    req.Rect[0].SrcOffset = 0u; req.Rect[0].DstOffset = 0u;
    req.Rect[0].RowBytes = G2A_PITCH; req.Rect[0].Rows = G2A_H;
    req.DstLayout = SWGPU_BLIT_DST_BLOCK;
    req.DstBlockHeightLog2 = G2A_BH_LOG2;
    req.DstHeight = G2A_ROWS;
    req.DstKind = Kind;
    return SwdGpuSend(Ctx, &req);
}

/* Window A registers changed by the test, saved from the active bank. */
static const ULONG G2aRegs[] = {
    DC_WIN_POSITION, DC_WIN_SIZE, DC_WIN_PRESCALED, DC_WIN_H_INIT_DDA, DC_WIN_V_INIT_DDA,
    DC_WIN_DDA_INC, DC_WIN_LINE_STRIDE, DC_WINBUF_START, DC_WINBUF_START_HI,
    DC_WINBUF_H_OFF, DC_WINBUF_V_OFF, DC_WINBUF_KIND, DC_WIN_OPTIONS,
};

static VOID G2aSave(PUCHAR Dc, ULONG *Out)
{
    ULONG hdr = READ_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER));
    ULONG access = READ_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG));
    ULONG i;
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER), DC_WIN_A_SELECT);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG), SWD_DC_READ_ACTIVE);
    for (i = 0u; i < RTL_NUMBER_OF(G2aRegs); ++i) Out[i] = READ_REGISTER_ULONG((PULONG)(Dc + G2aRegs[i]));
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG), access);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER), hdr);
}

/* Writes window A (assembly), latches it at the next frame and waits for the
 * active start address. TRUE when latched without a window A error. */
static BOOLEAN G2aWrite(PUCHAR Dc, const ULONG *Values, PULONG Errors)
{
    ULONG hdr = READ_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER));
    ULONG access = READ_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG));
    ULONG before = READ_REGISTER_ULONG((PULONG)(Dc + DC_CMD_INT_STATUS_REG)), i;
    ULONGLONG want = ((ULONGLONG)Values[8] << 32) | Values[7];
    SWD_FLIP probe = {0};
    BOOLEAN ok;
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG), 0u);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER), DC_WIN_A_SELECT);
    for (i = 0u; i < RTL_NUMBER_OF(G2aRegs); ++i) WRITE_REGISTER_ULONG((PULONG)(Dc + G2aRegs[i]), Values[i]);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_CONTROL_REG), GENERAL_UPDATE | WIN_A_UPDATE);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_CONTROL_REG), SWD_FLIP_REQ);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_WINDOW_HEADER), hdr);
    WRITE_REGISTER_ULONG((PULONG)(Dc + DC_CMD_STATE_ACCESS_REG), access);
    ok = SwdFlipWait(Dc, want, &probe);
    *Errors = READ_REGISTER_ULONG((PULONG)(Dc + DC_CMD_INT_STATUS_REG)) & ~before & SWD_WIN_A_ERRORS;
    return ok;
}

static VOID G2aRun(PSWD_CONTEXT Ctx)
{
    PHYSICAL_ADDRESS lo = {0}, hi, skip = {0};
    PUCHAR src = NULL, bl = NULL;
    PMDL srcMdl = NULL, blMdl = NULL;
    ULONG saved[RTL_NUMBER_OF(G2aRegs)], vals[RTL_NUMBER_OF(G2aRegs)];
    ULONG v, first, bad, errors, shown = 0u, kind;
    NTSTATUS st;
    BOOLEAN safe = TRUE, locked = FALSE;
    LARGE_INTEGER wait;

    RegPutDword(L"G2aTest", 0u);
    RegPutDword(L"G2aStep", 1u);
    hi.QuadPart = 0xFFFFFFFFll;
    src = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(G2A_PITCH * G2A_H, lo, hi, skip, MmWriteCombined);
    bl = (PUCHAR)MmAllocateContiguousMemorySpecifyCache(G2A_BL_BYTES, lo, hi, skip, MmWriteCombined);
    if (src) srcMdl = SwdOwnedScanoutMdl(src, G2A_PITCH * G2A_H);
    if (bl) blMdl = SwdOwnedScanoutMdl(bl, G2A_BL_BYTES);
    if (!srcMdl || !blMdl || !Ctx->DcVa) { RegPutDword(L"G2aStep", 0xE1u); goto out; }
    RegPutDword(L"G2aBlPhysLo", MmGetPhysicalAddress(bl).LowPart);

    /* 1. GPU copy engine into the block-linear buffer, kind 0 then 0xFE. */
    G2aFillPitch(src, 0u);
    for (kind = 0u; kind <= 0xFEu; kind += 0xFEu) {
        RtlZeroMemory(bl, G2A_BL_BYTES);
        KeMemoryBarrier();
        /* G1 may have powered the GPU off: a NOT_READY answer starts it (~2 s). */
        for (v = 0u; v < 10u; ++v) {
            st = G2aGpuCopy(Ctx, srcMdl, blMdl, kind);
            if (st != STATUS_DEVICE_NOT_READY && st != STATUS_DEVICE_BUSY) break;
            wait.QuadPart = -30000000ll;
            KeDelayExecutionThread(KernelMode, FALSE, &wait);
        }
        RegPutDword(L"G2aGpuTries", v + 1u);
        RegPutDword(kind ? L"G2aKindFEStatus" : L"G2aKind0Status", (ULONG)st);
        if (st == SWGPU_BLIT_DMA_UNSAFE) { safe = FALSE; RegPutDword(L"G2aStep", 0xE2u); goto out; }
        bad = NT_SUCCESS(st) ? G2aCompare(bl, 0u, &first) : 0xFFFFFFFFu;
        RegPutDword(kind ? L"G2aKindFEBad" : L"G2aKind0Bad", bad);
        RegPutDword(kind ? L"G2aKindFEFirstBad" : L"G2aKind0FirstBad", NT_SUCCESS(st) ? first : 0u);
        if (bad == 0u) break;
    }
    RegPutDword(L"G2aStep", 2u);

    /* 2. Show the CPU-swizzled picture transposed, 4 direction variants. */
    if (!SwdFlipLock(&Ctx->FbLock)) { RegPutDword(L"G2aStep", 0xE3u); goto out; }
    locked = TRUE;
    G2aSave(Ctx->DcVa, saved);
    RegPutDword(L"G2aSavedOptions", saved[12]);
    RegPutDword(L"G2aSavedSize", saved[1]);
    RegPutDword(L"G2aSavedPrescaled", saved[2]);
    RegPutDword(L"G2aSavedDda", saved[5]);
    for (v = 1u; v <= 4u; ++v) {
        /* FreeBSD tegra_dc: 90 = transpose + flip_y, 270 = transpose + flip_x. */
        BOOLEAN fx = v == 2u || v == 4u, fy = v == 1u || v == 4u;
        static const PCWSTR names[4] = { L"G2aVariant1", L"G2aVariant2", L"G2aVariant3", L"G2aVariant4" };
        G2aFillBlock(bl, v);
        KeMemoryBarrier();
        RtlCopyMemory(vals, saved, sizeof(vals));
        vals[0] = 0u;                                        /* position 0,0 */
        vals[1] = (G2A_H & 0x1FFFu) | ((G2A_W & 0x1FFFu) << 16);   /* 720 wide, 1280 high on the panel */
        vals[2] = ((G2A_H * 4u) & 0x7FFFu) | ((G2A_W & 0x1FFFu) << 16); /* fetch: 720 px x 1280 lines */
        vals[6] = G2A_PITCH;
        vals[7] = MmGetPhysicalAddress(bl).LowPart;
        vals[8] = 0u;
        vals[9] = fx ? (G2A_W - 1u) * 4u : 0u;
        vals[10] = fy ? G2A_H - 1u : 0u;
        vals[11] = DC_KIND_BLOCK | (G2A_BH_LOG2 << 4);
        vals[12] = (saved[12] & ~(DC_WIN_H_DIRECTION | DC_WIN_V_DIRECTION | DC_WIN_SCAN_COLUMN |
                                  DC_WIN_H_FILTER | DC_WIN_V_FILTER)) | DC_WIN_SCAN_COLUMN |
                   (fx ? DC_WIN_H_DIRECTION : 0u) | (fy ? DC_WIN_V_DIRECTION : 0u);
        RegPutDword(names[v - 1u], G2aWrite(Ctx->DcVa, vals, &errors) ? errors : 0xFFFFFFFFu);
        ++shown;
        wait.QuadPart = -(LONGLONG)G2A_SHOW_MS * 10000ll;
        KeDelayExecutionThread(KernelMode, FALSE, &wait);
    }
    RegPutDword(L"G2aStep", 3u);

    /* 3. Restore. Free the buffer only when the old start address is live. */
    safe = G2aWrite(Ctx->DcVa, saved, &errors);
    RegPutDword(L"G2aRestored", safe ? 1u : 0u);
    RegPutDword(L"G2aStep", safe ? 4u : 0xE4u);

out:
    if (locked) SwdFlipUnlock(&Ctx->FbLock);
    RegPutDword(L"G2aShown", shown);
    if (safe) {
        if (srcMdl) IoFreeMdl(srcMdl);
        if (blMdl) IoFreeMdl(blMdl);
        if (src) MmFreeContiguousMemorySpecifyCache(src, G2A_PITCH * G2A_H, MmWriteCombined);
        if (bl) MmFreeContiguousMemorySpecifyCache(bl, G2A_BL_BYTES, MmWriteCombined);
    }
    RegPutDword(L"G2aDone", 1u);
}

/* Guard thread: once the GPU endpoint is connected. */
static VOID G2aMaybeRun(PSWD_CONTEXT Ctx)
{
    if (gG2aState != 0 || Ctx->GpuTarget == NULL || Ctx->DcStopped) return;
    InterlockedExchange(&gG2aState, 1);
    if (RegGetDword(L"G2aTest", 0u) == 1u) G2aRun(Ctx);
}
