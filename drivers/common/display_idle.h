/* SPDX-License-Identifier: MIT
 * KMDF S0 idle policy for input devices that are only needed while the display
 * is on. The power button has a separate wake-capable interrupt and is never
 * governed by this helper. Power notifications only enqueue work; no I/O or
 * synchronous D0 wait is allowed in them.
 */
#ifndef SWITCH_DISPLAY_IDLE_H
#define SWITCH_DISPLAY_IDLE_H

typedef struct {
    WDFDEVICE Device;
    WDFWORKITEM Work;
    PVOID Callback;
    volatile LONG DisplayOn, Closing;
    BOOLEAN Held;
    NTSTATUS LastStatus;
} SW_DISPLAY_IDLE;
typedef struct { SW_DISPLAY_IDLE *Idle; } SW_DISPLAY_IDLE_WORK;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SW_DISPLAY_IDLE_WORK, SwDisplayIdleWork)
static const GUID SwDisplayIdleGuid={0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};

static VOID SwDisplayIdleWorker(WDFWORKITEM Work)
{
    SW_DISPLAY_IDLE *idle=SwDisplayIdleWork(Work)->Idle;
    if (InterlockedCompareExchange(&idle->Closing,0,0)) return;
    if (InterlockedCompareExchange(&idle->DisplayOn,0,0)) {
        if (!idle->Held) {
            /* Never block a D0 transition whose callback may need this worker. */
            idle->LastStatus=WdfDeviceStopIdle(idle->Device,FALSE);
            if (NT_SUCCESS(idle->LastStatus)) idle->Held=TRUE;
        }
    } else if (idle->Held) {
        idle->Held=FALSE;
        WdfDeviceResumeIdle(idle->Device);
    }
}
static NTSTATUS SwDisplayIdleChanged(LPCGUID Setting,PVOID Value,ULONG Length,PVOID Context)
{
    SW_DISPLAY_IDLE *idle=Context;
    if (Length==sizeof(ULONG) && IsEqualGUID(Setting,&SwDisplayIdleGuid) &&
        !InterlockedCompareExchange(&idle->Closing,0,0)) {
        InterlockedExchange(&idle->DisplayOn,*(PULONG)Value!=0);
        WdfWorkItemEnqueue(idle->Work);
    }
    return STATUS_SUCCESS;
}
static NTSTATUS SwDisplayIdleConfigure(WDFDEVICE Device,SW_DISPLAY_IDLE *Idle)
{
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS settings;
    WDF_WORKITEM_CONFIG work;WDF_OBJECT_ATTRIBUTES attributes;
    Idle->Device=Device;Idle->DisplayOn=1;
    WDF_DEVICE_POWER_POLICY_IDLE_SETTINGS_INIT(&settings,IdleCannotWakeFromS0);
    settings.DxState=PowerDeviceD3;settings.IdleTimeout=1000;
    settings.IdleTimeoutType=SystemManagedIdleTimeoutWithHint;
    settings.ExcludeD3Cold=WdfTrue;settings.PowerUpIdleDeviceOnSystemWake=WdfTrue;
    settings.UserControlOfIdleSettings=IdleDoNotAllowUserControl;
    settings.Enabled=WdfTrue;
    NTSTATUS status=WdfDeviceAssignS0IdleSettings(Device,&settings);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG_INIT(&work,SwDisplayIdleWorker);work.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,SW_DISPLAY_IDLE_WORK);attributes.ParentObject=Device;
    status=WdfWorkItemCreate(&work,&attributes,&Idle->Work);
    if (NT_SUCCESS(status)) SwDisplayIdleWork(Idle->Work)->Idle=Idle;
    return status;
}
/* Called after the first D0 entry, not from DeviceAdd. Later D0 entries leave
 * the existing screen-off decision and reference count intact. */
static NTSTATUS SwDisplayIdleStart(SW_DISPLAY_IDLE *Idle)
{
    if (Idle->Callback) return STATUS_SUCCESS;
    InterlockedExchange(&Idle->Closing,0);
    NTSTATUS status=WdfDeviceStopIdle(Idle->Device,FALSE);
    if (!NT_SUCCESS(status)) return status;
    Idle->Held=TRUE;
    status=PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Idle->Device),
        &SwDisplayIdleGuid,SwDisplayIdleChanged,Idle,&Idle->Callback);
    if (!NT_SUCCESS(status)) {
        Idle->Callback=NULL;
        WdfWorkItemFlush(Idle->Work);
        if (Idle->Held) {Idle->Held=FALSE;WdfDeviceResumeIdle(Idle->Device);}
    }
    return status;
}
static VOID SwDisplayIdleStop(SW_DISPLAY_IDLE *Idle)
{
    InterlockedExchange(&Idle->Closing,1);
    if (Idle->Callback) {PoUnregisterPowerSettingCallback(Idle->Callback);Idle->Callback=NULL;}
    if (Idle->Work) WdfWorkItemFlush(Idle->Work);
    if (Idle->Held) {Idle->Held=FALSE;WdfDeviceResumeIdle(Idle->Device);}
}
#endif
