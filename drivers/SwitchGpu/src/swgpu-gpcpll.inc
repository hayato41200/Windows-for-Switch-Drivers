/* GM20B legacy PLL. Included by swgpu-blit.inc after the checked MMIO helper. */
#define F2C_CFG        0x137000u
#define F2C_COEFF      0x137004u
#define F2C_CFG2       0x13700Cu
#define F2C_DVFS0      0x137010u
#define F2C_DVFS1      0x137014u
#define F2C_CFG3       0x137018u
#define F2C_SLOW       0x13701Cu
#define F2C_RAMP       0x1328A0u
#define F2C_SEL        0x137100u
#define F2C_OUT        0x137250u
#define F2C_BYPASS     0x137340u
#define F2C_COUNT_CFG  0x134124u
#define F2C_COUNT_VAL  0x134128u
#define F2C_ENABLE     0x00000001u
#define F2C_IDDQ       0x00000002u
#define F2C_SYNC       0x00000004u
#define F2C_LOCK_OFF   0x00000010u
#define F2C_LOCK       0x00020000u
#define F2C_RAMP_EN    0x80000000u
#define F2C_SLOW_PLL   0x00400000u
#define F2C_VCO_DIV    0x00003F00u
#define F2C_REF_KHZ    38400u
#define F2C_CAP_KHZ    614400u
#define F2C_STEPS      5u

typedef struct _F2C_STEP {
    ULONG TargetKhz, M, N, Pl, CvbMv, LockPolls, RampPolls, MeasuredKhz;
    ULONG Result, Verified, CachedUs, WcUs, FailOffset, Stage;
} F2C_STEP;

static BOOLEAN F2cUpdate(PSWGPU_DEVICE Dev, ULONG Offset, ULONG Mask, ULONG Value)
{
    ULONG writes[1][3] = { { 0u } };
    ULONG previous = Rd(Dev->Bar0, Offset);
    BOOLEAN written = F2Write(Dev, Dev->Bar0, Offset, (previous & ~Mask) | (Value & Mask), &writes[0]);
    if (written && ((writes[0][2] ^ Value) & Mask) == 0u) return TRUE;
    /* Claude 0.12.1: record which checked write failed (0.12.0 stage 5 gave
     * no detail). Readback mismatch vs priv-ring rejection: F2cFailWritten. */
    Trace(L"F2cFailOffset", Offset);
    Trace(L"F2cFailPrevious", previous);
    Trace(L"F2cFailMask", Mask);
    Trace(L"F2cFailWanted", Value);
    Trace(L"F2cFailReadback", writes[0][2]);
    TraceDurable(L"F2cFailWritten", written ? 1u : 0u);
    return FALSE;
}

/* Read the T210 CPU_SPEEDO_2 fuse shadow only. Resident has enabled FUSE. */
static ULONG F2cReadSpeedo(VOID)
{
    PHYSICAL_ADDRESS pa;
    volatile UCHAR *fuse;
    ULONG speedo = 0u;
    pa.QuadPart = 0x7000F800ll;
    fuse = (volatile UCHAR *)MmMapIoSpaceEx(pa, 0x400u, PAGE_READONLY | PAGE_NOCACHE);
    if (fuse != NULL) {
        speedo = Rd(fuse, 0x130u);
        MmUnmapIoSpace((PVOID)fuse, 0x400u);
    }
    Trace(L"F2cRawSpeedo", speedo);
    return speedo;
}

static LONG F2cRound100(LONG Value)
{
    return Value < 0 ? -((-Value + 50) / 100) : (Value + 50) / 100;
}

/* Apply the speedo-ID-0 legacy polynomial and its 950 mV floor.
 * Take the highest voltage across the three T210 fuse-revision decodings,
 * so an unknown revision cannot admit an OPP via a faster interpretation. */
static ULONG F2cCvbMv(ULONG Khz, ULONG RawSpeedo)
{
    static const LONG cvb[][2] = {
        {1786666, -85625}, {1846729, -87525}, {1910480, -89425}, {1977920, -91325},
        {2049049, -93215}, {2122872, -95095}, {2201331, -96985}, {2283479, -98885}
    };
    LONG speeds[3], uv = 950000;
    ULONG i, row;
    if (!Khz || Khz > F2C_CAP_KHZ || Khz % 76800u || RawSpeedo < 76u || RawSpeedo > 3000u)
        return 0u;
    row = Khz / 76800u - 1u;
    speeds[0] = (LONG)RawSpeedo;
    speeds[1] = (LONG)RawSpeedo - 75;
    speeds[2] = (1082 * (LONG)RawSpeedo / 100 - 1662) / 10;
    for (i = 0; i < RTL_NUMBER_OF(speeds); ++i) {
        LONG inner = F2cRound100(1632 * speeds[i]) + cvb[row][1];
        LONG required = cvb[row][0] + F2cRound100(inner * speeds[i]);
        if (required > uv) uv = required;
    }
    return (ULONG)(uv + 999) / 1000u;
}

/* Exact targets only. Search ascending postdividers, then input dividers,
 * enforcing GM20B non-NA PFD 12..38.4 MHz and VCO 1.3..2.6 GHz. */
static BOOLEAN F2cFindMnp(F2C_STEP *Clock)
{
    ULONG pl, m;
    for (pl = 1u; pl <= 31u; ++pl) {
        ULONG vco = 2u * Clock->TargetKhz * pl;
        if (vco < 1300000u || vco > 2600000u) continue;
        for (m = 1u; F2C_REF_KHZ >= 12000u * m; ++m) {
            ULONG numerator = vco * m;
            ULONG n = numerator / F2C_REF_KHZ;
            if (numerator % F2C_REF_KHZ || n < 8u || n > 255u) continue;
            Clock->M = m;
            Clock->N = n;
            Clock->Pl = pl;
            return TRUE;
        }
    }
    return FALSE;
}

/* Claude 0.12.3: 0.12.1/0.12.2 hardware: SEL_VCO bit0 writes are accepted
 * but read back 0, even with BYPASSCTRL cleared. Try, in one run: (0) the
 * write as is with SYNC_MODE, (1) SYNC_MODE cleared (nouveau gk20a switches
 * without it), (2) after a further 100 us settle. Poll the readback up to
 * 100 us each, and trace what happened. */
static BOOLEAN F2cSelVco(PSWGPU_DEVICE Dev)
{
    ULONG writes[1][3] = { { 0u } }, i = 0u, variant;
    for (variant = 0u; variant < 3u; ++variant) {
        if (variant == 1u) (VOID)F2cUpdate(Dev, F2C_CFG, F2C_SYNC, 0u);
        if (variant == 2u) Us(100u);
        (VOID)F2Write(Dev, Dev->Bar0, F2C_SEL, Rd(Dev->Bar0, F2C_SEL) | 1u, &writes[0]);
        for (i = 0u; i < 100u; ++i) {
            if (Rd(Dev->Bar0, F2C_SEL) & 1u) break;
            Us(1u);
        }
        Trace(variant == 0u ? L"F2cSelV0Read" : variant == 1u ? L"F2cSelV1Read" : L"F2cSelV2Read",
              Rd(Dev->Bar0, F2C_SEL));
        if (i < 100u) {
            Trace(L"F2cSelVariant", variant);
            Trace(L"F2cSelPolls", i);
            return TRUE;
        }
    }
    Trace(L"F2cSelVariant", 0xFFFFFFFFu);
    Trace(L"F2cCfgAtSelFail", Rd(Dev->Bar0, F2C_CFG));
    Trace(L"F2cCoeffAtSelFail", Rd(Dev->Bar0, F2C_COEFF));
    Trace(L"F2cOutAtSelFail", Rd(Dev->Bar0, F2C_OUT));
    Trace(L"F2cBypassAtSelFail", Rd(Dev->Bar0, F2C_BYPASS));
    return FALSE;
}

/* Restore the known running bypass source. If either mux write fails, use
 * the other mux to establish bypass before disabling the PLL. Verify the
 * original bypass routing and divider before allowing more submissions. */
static BOOLEAN F2cReturnToBypass(PSWGPU_DEVICE Dev)
{
    ULONG i;
    BOOLEAN ok = FALSE;
    if (!Dev->GpcManaged) return TRUE;
    for (i = 0u; i < 2u; ++i) {
        (VOID)F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, 0x200u);
        (VOID)F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, 0x200u);
        Us(2u);
        (VOID)F2cUpdate(Dev, F2C_SEL, 1u, 0u);
        if (Rd(Dev->Bar0, F2C_SEL) & 1u)
            continue;
        (VOID)F2cUpdate(Dev, F2C_CFG, F2C_ENABLE | F2C_SYNC, 0u);
        (VOID)F2cUpdate(Dev, F2C_SLOW, F2C_RAMP_EN | F2C_SLOW_PLL, 0u);
        /* Claude 0.12.2: SEL_VCO is 0 here, so restoring the boot global
         * bypass value cannot glitch the running clock. */
        (VOID)F2cUpdate(Dev, F2C_BYPASS, 1u, Dev->GpcBootBypass);
        (VOID)F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, Dev->GpcBootOut);
        (VOID)F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, Dev->GpcBootOut);
        if (!(Rd(Dev->Bar0, F2C_SEL) & 1u) &&
            !(Rd(Dev->Bar0, F2C_CFG) & (F2C_ENABLE | F2C_SYNC)) &&
            !(Rd(Dev->Bar0, F2C_SLOW) & (F2C_RAMP_EN | F2C_SLOW_PLL)) &&
            ((Rd(Dev->Bar0, F2C_BYPASS) ^ Dev->GpcBootBypass) & 1u) == 0u &&
            ((Rd(Dev->Bar0, F2C_OUT) ^ Dev->GpcBootOut) & F2C_VCO_DIV) == 0u) {
            ok = TRUE;
            break;
        }
    }
    Dev->GpcManaged = !ok;
    Trace(L"F2cBypassAttempts", i < 2u ? i + 1u : 2u);
    TraceDurable(L"F2cBypassResult", ok ? 0u : (ULONG)STATUS_DEVICE_HARDWARE_ERROR);
    return ok;
}

static NTSTATUS F2cSlide(PSWGPU_DEVICE Dev, ULONG N, F2C_STEP *Clock)
{
    ULONG i;
    if (((Rd(Dev->Bar0, F2C_COEFF) >> 8) & 255u) == N) return STATUS_SUCCESS;
    if (!F2cUpdate(Dev, F2C_SLOW, F2C_SLOW_PLL, F2C_SLOW_PLL))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Us(1u);
    if (!F2cUpdate(Dev, F2C_COEFF, 0xFF00u, N << 8))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Us(1u);
    if (!F2cUpdate(Dev, F2C_SLOW, F2C_RAMP_EN, F2C_RAMP_EN))
        return STATUS_DEVICE_HARDWARE_ERROR;
    for (i = 0u; i < 500u; ++i) {
        Us(1u);
        if (Rd(Dev->Bar0, F2C_RAMP) & 0x01000000u) break;
    }
    Clock->RampPolls += i < 500u ? i + 1u : 500u;
    if (!F2cUpdate(Dev, F2C_SLOW, F2C_SLOW_PLL | F2C_RAMP_EN, 0u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    if (i == 500u) return STATUS_IO_TIMEOUT;
    return (Rd(Dev->Bar0, F2C_CFG) & F2C_LOCK) ? STATUS_SUCCESS : STATUS_DEVICE_HARDWARE_ERROR;
}

/* Normal exit is idle: lower the running VCO first as in gk20a_clk_fini.
 * Bypass/reset recovery still proceeds if the ramp fails. */
static BOOLEAN F2cPark(PSWGPU_DEVICE Dev)
{
    F2C_STEP clock = {0};
    NTSTATUS status = STATUS_SUCCESS;
    if (!Dev->GpcManaged) return TRUE;
    if ((Rd(Dev->Bar0, F2C_CFG) & (F2C_ENABLE | F2C_LOCK)) == (F2C_ENABLE | F2C_LOCK) &&
        (Rd(Dev->Bar0, F2C_SEL) & 1u)) {
        ULONG m = Rd(Dev->Bar0, F2C_COEFF) & 255u;
        if (m >= 1u && m <= 3u)
            status = F2cSlide(Dev, (m * 1300000u + F2C_REF_KHZ - 1u) / F2C_REF_KHZ, &clock);
    }
    Trace(L"F2cParkRampPolls", clock.RampPolls);
    Trace(L"F2cParkRampStatus", (ULONG)status);
    return F2cReturnToBypass(Dev);
}

/* Nouveau gk20a_clk_setup_slide, gk20a_pllg_enable, gk20a_pllg_slide and
 * gk20a_pllg_program_mnp_slide with GM20B limits. The caller owns the
 * resident submission gate and runs bypass recovery on any failure. */
static NTSTATUS F2cProgram(PSWGPU_DEVICE Dev, ULONG Speedo, F2C_STEP *Clock)
{
    ULONG old, lowN, i;
    NTSTATUS status;
    Clock->Stage = 1u;
    Clock->CvbMv = F2cCvbMv(Clock->TargetKhz, Speedo);
    if (Clock->CvbMv == 0u || Clock->CvbMv > 1000u || Dev->VoutOnRaw != VDD_GPU_1000MV ||
        (Rd(Dev->Car, 0x050u) & 0xFC000000u) != 0x50000000u || !F2cFindMnp(Clock))
        return STATUS_NOT_SUPPORTED;
    if ((Rd(Dev->Bar0, F2C_DVFS0) & 0x10000000u) ||
        (Rd(Dev->Bar0, F2C_DVFS1) & 0x30000000u))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    if (Rd(Dev->Bar0, 0x120058u) || Rd(Dev->Bar0, 0x12005Cu))
        return STATUS_DEVICE_HARDWARE_ERROR;
    if (!Dev->GpcManaged &&
        ((Rd(Dev->Bar0, F2C_CFG) & F2C_ENABLE) || (Rd(Dev->Bar0, F2C_SEL) & 1u) ||
         !(Rd(Dev->Bar0, F2C_OUT) & 0x80000000u)))
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    Clock->Stage = 2u;
    if (Dev->GpcManaged) {
        old = Rd(Dev->Bar0, F2C_COEFF);
        if ((old & 255u) == Clock->M && ((old >> 16) & 63u) == Clock->Pl)
            return F2cSlide(Dev, Clock->N, Clock);
        lowN = ((old & 255u) * 1300000u + F2C_REF_KHZ - 1u) / F2C_REF_KHZ;
        status = F2cSlide(Dev, lowN, Clock);
        if (!NT_SUCCESS(status)) return status;
        if (!F2cReturnToBypass(Dev)) return STATUS_DEVICE_HARDWARE_ERROR;
    }

    Dev->GpcBootOut = Rd(Dev->Bar0, F2C_OUT);
    Dev->GpcBootBypass = Rd(Dev->Bar0, F2C_BYPASS);
    Dev->GpcManaged = TRUE;
    Clock->Stage = 3u;
    if (!F2cUpdate(Dev, F2C_CFG, F2C_IDDQ | F2C_SYNC, 0u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Us(5u);
    /* Claude 0.12.2: 0.12.1 hardware showed SEL_VCO (0x137100 bit0) ignores
     * writes while the global BYPASSCTRL GPCPLL bit is 1. nvgpu's gm20b clock
     * init clears that global bypass first; the GPU stays in bypass because
     * SEL_VCO is still 0 (facts only, no code copied). Restored on return. */
    if (!F2cUpdate(Dev, F2C_BYPASS, 1u, 0u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    lowN = (Clock->M * 1300000u + F2C_REF_KHZ - 1u) / F2C_REF_KHZ;
    if (!F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, 0x200u) ||
        !F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, 0x200u) ||
        !F2cUpdate(Dev, F2C_CFG2, 0xFF000000u, 0x04000000u) ||
        !F2cUpdate(Dev, F2C_CFG3, 0x00FF0000u, 0x00050000u) ||
        !F2cUpdate(Dev, F2C_COEFF, 0x003FFFFFu, Clock->M | (lowN << 8) | (Clock->Pl << 16)))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Trace(L"F2cCfgBeforeEnable", Rd(Dev->Bar0, F2C_CFG));   /* Claude 0.12.3: stale LOCK? */
    if (!F2cUpdate(Dev, F2C_CFG, F2C_ENABLE, F2C_ENABLE))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Trace(L"F2cCfgAfterEnable", Rd(Dev->Bar0, F2C_CFG));
    if (!F2cUpdate(Dev, F2C_CFG, F2C_LOCK_OFF, 0u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Clock->Stage = 4u;
    for (i = 0u; i < 300u; ++i) {
        Us(1u);
        if (Rd(Dev->Bar0, F2C_CFG) & F2C_LOCK) break;
    }
    Clock->LockPolls = i < 300u ? i + 1u : 300u;
    if (i == 300u) return STATUS_IO_TIMEOUT;
    Clock->Stage = 5u;
    /* Global BYPASSCTRL was cleared at stage 3; SYNC_MODE + SEL_VCO switch
     * GPC2CLK from bypass to the VCO. */
    Trace(L"F2cCfgAtLock", Rd(Dev->Bar0, F2C_CFG));
    /* Claude 0.12.4: LOCK reads set after 1 us, too early to trust. 0.12.3
     * only switched because the registry traces above added delay; settle
     * explicitly before leaving bypass. */
    Us(100u);
    if (!F2cUpdate(Dev, F2C_CFG, F2C_SYNC, F2C_SYNC) || !F2cSelVco(Dev))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Us(2u);
    if (!F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, 0u) ||
        !F2cUpdate(Dev, F2C_OUT, F2C_VCO_DIV, 0u))
        return STATUS_DEVICE_HARDWARE_ERROR;
    Clock->Stage = 6u;
    return F2cSlide(Dev, Clock->N, Clock);
}

/* Window semantics from gm20b_clk_get_gpcclk_clock_counter. This reads
 * the idle clock and leaves thermal/idle slowdown programming alone. */
static NTSTATUS F2cMeasure(PSWGPU_DEVICE Dev, F2C_STEP *Clock)
{
    ULONG writes[3][3], first = 0u, second = 0u;
    ULONG saved = Rd(Dev->Bar0, F2C_COUNT_CFG);
    BOOLEAN ok;
    if ((saved & 0x01100000u) != 0u || (saved & 0xFFFFu) > 0x3FFFu)
        return STATUS_SUCCESS;
    ok = F2Write(Dev, Dev->Bar0, F2C_COUNT_CFG, 0x01000000u, &writes[0]) &&
         F2Write(Dev, Dev->Bar0, F2C_COUNT_CFG, 0x00110320u, &writes[1]);
    if (ok) {
        Us(200u);
        first = Rd(Dev->Bar0, F2C_COUNT_VAL) & 0xFFFFFu;
        Us(100u);
        second = Rd(Dev->Bar0, F2C_COUNT_VAL) & 0xFFFFFu;
    }
    if (!F2Write(Dev, Dev->Bar0, F2C_COUNT_CFG, saved | 0x10000u, &writes[2]) ||
        ((writes[2][2] ^ saved) & 0x01103FFFu) != 0u || !ok)
        return STATUS_DEVICE_HARDWARE_ERROR;
    if (first == second && second != 0u && second != 0xFFFFFu)
        Clock->MeasuredKhz = second * 48u; /* 38400 / 800 */
    return STATUS_SUCCESS;
}
