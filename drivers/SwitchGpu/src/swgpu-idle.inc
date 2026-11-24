/* G1 owns one scheduler and one PASSIVE_LEVEL worker per WDF device.
 * PowerLock serializes enqueue / cancellation (no MMIO under the lock).
 * BlitGate serializes the worker with F3, test IOCTLs, Stop and PnP. */
#define G1_DEFAULT_IDLE_MS 10000u
#define G1_TICK_MS 1000u
#define G1_D0_WAIT_MS 5000u
#define G1_DRAIN_MS 20000u

static BOOLEAN G1AllowedLocked(PSWGPU_DEVICE Dev)
{
    return Dev->PowerPolicy && !Dev->PowerPaused && !Dev->PowerFinal &&
           !Dev->PowerDisabled && !Dev->PowerManualStop;
}

static VOID G1QueueLocked(PSWGPU_DEVICE Dev)
{
    if (!G1AllowedLocked(Dev) || Dev->PowerQueued) return;
    Dev->PowerQueued = TRUE;
    KeClearEvent(&Dev->PowerWorkDone);
    WdfWorkItemEnqueue(Dev->PowerWork);
}

static BOOLEAN G1RequestWake(PSWGPU_DEVICE Dev)
{
    BOOLEAN policy;
    KIRQL oldIrql;
    KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
    policy = Dev->PowerPolicy;
    /* Remember demand arriving during a nonfinal D0 transition. The
     * scheduler will enqueue it after the pause fence opens. */
    if (policy && !Dev->PowerFinal && !Dev->PowerDisabled && !Dev->PowerManualStop) {
        Dev->PowerWakeWanted = TRUE;
        G1QueueLocked(Dev);
    }
    KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
    return policy;
}

static VOID G1TraceState(PSWGPU_DEVICE Dev)
{
    Trace(L"G1Policy", Dev->PowerPolicy);
    Trace(L"G1State", Dev->ResidentActive && !Dev->BlitReset ? 1u : 0u);
    Trace(L"G1IdlePowerOffMs", Dev->IdlePowerOffMs);
    Trace(L"G1PowerOffs", Dev->PowerOffs);
    Trace(L"G1PowerOns", Dev->PowerOns);
    Trace(L"G1LastOffMs", Dev->LastOffMs);
    Trace(L"G1LastOnUs", Dev->LastOnUs);
    Trace(L"G1OnFailures", Dev->OnFailures);
    Trace(L"G1Disabled", Dev->PowerDisabled);
    Trace(L"G1ManualStop", Dev->PowerManualStop);
}

static VOID G1ReleaseIdleIfOff(PSWGPU_DEVICE Dev)
{
    if (!Dev->ResidentActive && Dev->ResidentIdleRefHeld) {
        Dev->ResidentIdleRefHeld = FALSE;
        WdfDeviceResumeIdle(Dev->Device);
    }
}

static NTSTATUS G1TakeIdle(PSWGPU_DEVICE Dev)
{
    NTSTATUS status = WdfDeviceStopIdle(Dev->Device, FALSE);
    TraceDurable(L"B3eIdleRefStatus", (ULONG)status);
    if (NT_SUCCESS(status)) Dev->ResidentIdleRefHeld = TRUE;
    return status;
}

/* Boot retains the one firmware-read retry. Runtime takes its D0 reference
 * before touching the GPU and uses precisely the same resident bring-up. */
static NTSTATUS B3eStartOnline(PSWGPU_DEVICE Dev, BOOLEAN BootRetry, BOOLEAN Runtime)
{
    NTSTATUS status = StartResident(Dev);
    KIRQL oldIrql;
    BOOLEAN publish;
    if (BootRetry && !Dev->ResidentActive && ReadParam(L"E1Result", 0u) == 0xE0000041u) {
        TraceDurable(L"AutoResidentRetry", 1u);
        Ms(3000u);
        status = StartResident(Dev);
        TraceDurable(L"AutoResidentRetryResult", Dev->ResidentActive ? 0u : ReadParam(L"E1Result", 0u));
    }
    if (!Dev->ResidentActive) {
        G1ReleaseIdleIfOff(Dev);
        return NT_SUCCESS(status) ? STATUS_DEVICE_HARDWARE_ERROR : status;
    }
    if (!Dev->ResidentIdleRefHeld) status = G1TakeIdle(Dev);
    KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
    publish = NT_SUCCESS(status) && !Dev->PowerFinal &&
              (!Runtime || (G1AllowedLocked(Dev) && Dev->PowerD0));
    if (publish) {
        Dev->PowerWakeWanted = FALSE;
        InterlockedExchange64(&Dev->LastBlitTime, (LONG64)KeQueryInterruptTime());
        InterlockedExchange(&Dev->BlitOnline, 1);
    }
    KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
    if (publish) return STATUS_SUCCESS;
    if (NT_SUCCESS(status)) status = STATUS_CANCELLED;
    TraceDurable(L"E1Result", (ULONG)status);
    (VOID)B3eResidentShutdown(Dev, TRUE);
    return status;
}

static ULONG G1IdleMs(PSWGPU_DEVICE Dev)
{
    ULONGLONG last = (ULONGLONG)InterlockedCompareExchange64(&Dev->LastBlitTime, 0, 0);
    ULONGLONG now = KeQueryInterruptTime();
    ULONGLONG ms = now >= last ? (now - last) / 10000ull : 0ull;
    return ms > MAXULONG ? MAXULONG : (ULONG)ms;
}

static EVT_WDF_TIMER G1Timer;
static VOID G1Timer(WDFTIMER Timer)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice((WDFDEVICE)WdfTimerGetParentObject(Timer));
    KIRQL oldIrql;
    KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
    if (G1AllowedLocked(dev)) {
        if (dev->PowerWakeWanted ||
            (InterlockedCompareExchange(&dev->BlitOnline, 0, 0) &&
             G1IdleMs(dev) >= dev->IdlePowerOffMs))
            G1QueueLocked(dev);
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(G1_TICK_MS));
    }
    KeReleaseSpinLock(&dev->PowerLock, oldIrql);
}

/* A volatile registry child retains the lockout across a device/driver
 * restart in this boot. The diagnostic DWORD G1Disabled is only a trace. */
static BOOLEAN G1BootLatch(BOOLEAN Disable)
{
    WCHAR buffer[RTL_NUMBER_OF(SwGpuParamsPath) + 8u];
    UNICODE_STRING path, name;
    OBJECT_ATTRIBUTES attributes;
    HANDLE key;
    ULONG value = 0u, length;
    NTSTATUS status;
    struct {
        KEY_VALUE_PARTIAL_INFORMATION Info;
        ULONG Tail;
    } result;
    if (SwGpuParamsPath[0] == L'\0') return FALSE;
    RtlInitEmptyUnicodeString(&path, buffer, sizeof(buffer));
    status = RtlUnicodeStringCopyString(&path, SwGpuParamsPath);
    if (NT_SUCCESS(status)) status = RtlUnicodeStringCatString(&path, L"\\G1Boot");
    if (!NT_SUCCESS(status)) return FALSE;
    InitializeObjectAttributes(&attributes, &path, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    status = Disable ? ZwCreateKey(&key, KEY_SET_VALUE, &attributes, 0u, NULL, REG_OPTION_VOLATILE, NULL) :
                       ZwOpenKey(&key, KEY_QUERY_VALUE, &attributes);
    if (!NT_SUCCESS(status)) {
        if (Disable) Trace(L"G1LockoutStoreStatus", (ULONG)status);
        return FALSE;
    }
    RtlInitUnicodeString(&name, L"Disabled");
    if (Disable) {
        value = 1u;
        status = ZwSetValueKey(key, &name, 0u, REG_DWORD, &value, sizeof(value));
        Trace(L"G1LockoutStoreStatus", (ULONG)status);
    } else {
        status = ZwQueryValueKey(key, &name, KeyValuePartialInformation, &result, sizeof(result), &length);
        if (NT_SUCCESS(status) && result.Info.Type == REG_DWORD && result.Info.DataLength == sizeof(value))
            RtlCopyMemory(&value, result.Info.Data, sizeof(value));
    }
    ZwClose(key);
    return value != 0u;
}

static VOID G1AccountWake(PSWGPU_DEVICE Dev, NTSTATUS Status)
{
    LARGE_INTEGER freq;
    LARGE_INTEGER end = KeQueryPerformanceCounter(&freq);
    ULONGLONG us = (ULONGLONG)(end.QuadPart - Dev->PowerWakeQpc.QuadPart) *
                   1000000ull / (ULONGLONG)freq.QuadPart;
    KIRQL oldIrql;
    Dev->LastOnUs = us > MAXULONG ? MAXULONG : (ULONG)us;
    if (NT_SUCCESS(Status)) {
        ++Dev->PowerOns;
        Dev->OnStreak = 0u;
    } else if (Status != STATUS_CANCELLED) {
        ++Dev->OnFailures;
        ++Dev->OnStreak;
        if (Dev->OnStreak >= 2u || Dev->BlitUnsafe) {
            KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
            Dev->PowerDisabled = TRUE;
            Dev->PowerWakeWanted = FALSE;
            KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
            if (Dev->OnStreak >= 2u) (VOID)G1BootLatch(TRUE);
        }
    }
    Trace(L"G1LastOnStatus", (ULONG)Status);
    G1TraceState(Dev);
}

static EVT_WDF_WORKITEM G1Worker;
static VOID G1Worker(WDFWORKITEM WorkItem)
{
    PSWGPU_DEVICE dev = SwGpuGetDevice((WDFDEVICE)WdfWorkItemGetParentObject(WorkItem));
    KIRQL oldIrql;
    BOOLEAN allowed, wake, d0;
    if (InterlockedCompareExchange(&dev->BlitGate, 1, 0) != 0) goto finished;
    KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
    allowed = G1AllowedLocked(dev);
    d0 = dev->PowerD0;
    wake = dev->PowerWakeWanted;
    KeReleaseSpinLock(&dev->PowerLock, oldIrql);

    if (allowed && (dev->BlitUnsafe || (dev->ResidentActive && dev->BlitReset))) {
        KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
        dev->PowerDisabled = TRUE;
        dev->PowerWakeWanted = FALSE;
        KeReleaseSpinLock(&dev->PowerLock, oldIrql);
        G1TraceState(dev);
        goto unlock;
    }
    if (allowed && dev->ResidentActive && d0 && !dev->CopyInFlight && !dev->BlitReset &&
        InterlockedCompareExchange(&dev->BlitOnline, 0, 0) &&
        G1IdleMs(dev) >= dev->IdlePowerOffMs) {
        ULONG result;
        dev->LastOffMs = G1IdleMs(dev);
        result = B3eResidentShutdown(dev, TRUE);
        if (result == 0u) ++dev->PowerOffs;
        Trace(L"G1LastOffStatus", result);
        if (result != 0u) {
            KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
            dev->PowerDisabled = TRUE;
            dev->PowerWakeWanted = FALSE;
            KeReleaseSpinLock(&dev->PowerLock, oldIrql);
        }
        G1TraceState(dev);
    } else if (allowed && wake && !dev->ResidentActive && !dev->BlitUnsafe) {
        NTSTATUS status = STATUS_SUCCESS;
        if (!dev->ResidentIdleRefHeld) {
            dev->PowerWakeStart = KeQueryInterruptTime();
            dev->PowerWakeQpc = KeQueryPerformanceCounter(NULL);
            status = G1TakeIdle(dev);
        }
        if (NT_SUCCESS(status) && (!d0 || status == STATUS_PENDING)) {
            if (KeQueryInterruptTime() - dev->PowerWakeStart < G1_D0_WAIT_MS * 10000ull)
                goto unlock; /* D0Ready or the next timer tick continues. */
            status = STATUS_IO_TIMEOUT;
        }
        KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
        dev->PowerWakeWanted = FALSE;
        allowed = G1AllowedLocked(dev);
        KeReleaseSpinLock(&dev->PowerLock, oldIrql);
        if (!allowed) status = STATUS_CANCELLED;
        if (NT_SUCCESS(status)) status = B3eStartOnline(dev, FALSE, TRUE);
        else {
            if (status != STATUS_CANCELLED) TraceDurable(L"E1Result", (ULONG)status);
            G1ReleaseIdleIfOff(dev);
        }
        G1AccountWake(dev, status);
    }
unlock:
    InterlockedExchange(&dev->BlitGate, 0);
finished:
    /* Close the scheduling token and publish the drain event together. A
     * pending request is retried on the next tick if the gate was busy. */
    KeAcquireSpinLock(&dev->PowerLock, &oldIrql);
    dev->PowerQueued = FALSE;
    KeSetEvent(&dev->PowerWorkDone, IO_NO_INCREMENT, FALSE);
    KeReleaseSpinLock(&dev->PowerLock, oldIrql);
}

static VOID G1ManualStop(PSWGPU_DEVICE Dev)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
    if (Dev->PowerPolicy) {
        Dev->PowerManualStop = TRUE;
        Dev->PowerWakeWanted = FALSE;
    }
    KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
    G1ReleaseIdleIfOff(Dev);
}

static BOOLEAN G1Pause(PSWGPU_DEVICE Dev, BOOLEAN Final)
{
    KIRQL oldIrql;
    LARGE_INTEGER timeout, start, freq;
    NTSTATUS status;
    BOOLEAN queued;
    KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
    Dev->PowerPaused = TRUE;
    Dev->PowerD0 = FALSE;
    InterlockedExchange(&Dev->BlitOnline, 0);
    if (Final) {
        Dev->PowerFinal = TRUE;
        Dev->PowerWakeWanted = FALSE;
    }
    queued = Dev->PowerQueued;
    KeReleaseSpinLock(&Dev->PowerLock, oldIrql);

    if (Dev->PowerTimer != NULL) (VOID)WdfTimerStop(Dev->PowerTimer, FALSE);
    if (!queued) return TRUE;
    /* No producer can enqueue beyond the pause fence. Use a timed join
     * rather than WdfWorkItemFlush, which has no timeout. On timeout the
     * caller must retain the register mappings and DMA pool. */
    timeout.QuadPart = WDF_REL_TIMEOUT_IN_MS(G1_DRAIN_MS);
    start = KeQueryPerformanceCounter(&freq);
    status = KeWaitForSingleObject(&Dev->PowerWorkDone, Executive, KernelMode, FALSE, &timeout);
    Trace(L"G1DrainUs", (ULONG)((KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart) *
                                1000000ll / freq.QuadPart));
    Trace(L"G1DrainStatus", (ULONG)status);
    return status == STATUS_SUCCESS;
}

static VOID G1ResumeScheduler(PSWGPU_DEVICE Dev)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
    Dev->PowerPaused = FALSE;
    if (G1AllowedLocked(Dev)) {
        if (Dev->PowerWakeWanted) G1QueueLocked(Dev);
        WdfTimerStart(Dev->PowerTimer, WDF_REL_TIMEOUT_IN_MS(G1_TICK_MS));
    }
    KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
}

static VOID G1StartPolicy(PSWGPU_DEVICE Dev, BOOLEAN AutoResident, NTSTATUS BootStatus, ULONG BootUs)
{
    KIRQL oldIrql;
    ULONG idleMs = AutoResident ? ReadParam(L"IdlePowerOffMs", G1_DEFAULT_IDLE_MS) : 0u;
    KeAcquireSpinLock(&Dev->PowerLock, &oldIrql);
    Dev->IdlePowerOffMs = idleMs;
    Dev->PowerPolicy = idleMs != 0u && Dev->PowerTimer != NULL && Dev->PowerWork != NULL;
    if (Dev->PowerPolicy && NT_SUCCESS(BootStatus) && Dev->ResidentActive) {
        ++Dev->PowerOns;
        Dev->LastOnUs = BootUs;
    }
    KeReleaseSpinLock(&Dev->PowerLock, oldIrql);
    G1TraceState(Dev);
    G1ResumeScheduler(Dev);
}

static VOID G1CreateObjects(PSWGPU_DEVICE Dev)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_WORKITEM_CONFIG workConfig;
    WDF_TIMER_CONFIG timerConfig;
    NTSTATUS status;
    KeInitializeSpinLock(&Dev->PowerLock);
    KeInitializeEvent(&Dev->PowerWorkDone, NotificationEvent, TRUE);
    WDF_WORKITEM_CONFIG_INIT(&workConfig, G1Worker);
    workConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Dev->Device;
    status = WdfWorkItemCreate(&workConfig, &attributes, &Dev->PowerWork);
    if (NT_SUCCESS(status)) {
        /* KMDF passive timers are one-shot; G1Timer rearms after each tick. */
        WDF_TIMER_CONFIG_INIT(&timerConfig, G1Timer);
        timerConfig.AutomaticSerialization = FALSE;
        timerConfig.TolerableDelay = 100u;
        attributes.ExecutionLevel = WdfExecutionLevelPassive;
        status = WdfTimerCreate(&timerConfig, &attributes, &Dev->PowerTimer);
    }
    Trace(L"G1SetupStatus", (ULONG)status);
}
