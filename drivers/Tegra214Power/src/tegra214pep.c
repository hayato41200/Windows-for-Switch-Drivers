/* SPDX-License-Identifier: MIT
 * T214 processor power backend for Windows PoFx.
 *
 * Follows Microsoft's i.MX PEP notification contract and Switchroot's pinned
 * T210/B01 PSCI state IDs. PoFx owns the CPU context and resume address. Never
 * issue a context-losing CPU_SUSPEND directly from an arbitrary driver thread.
 *
 * This first backend implements CPU C7 and coordinated cluster CC6 only.
 * Peripheral/DRAM state is retained. SC7 is deliberately not advertised: its
 * display, storage and wake-controller restore paths must be implemented first.
 */
#include <ntddk.h>
#include <ntstrsafe.h>
#include <wdf.h>
#define PEP_KERNEL_INFORMATION_VERSION PEP_KERNEL_INFORMATION_V3
#include <pep_x.h>
#include <intrin.h>

#define CPU_COUNT 4
#define IDLE_WFI 0
#define IDLE_C7 1
#define IDLE_CC6 2
#define IDLE_COUNT 3
#define PSCI_CORE_POWERDOWN 0x00010007u
#define PSCI_CLUSTER_IDLE  0x01010010u

typedef struct {
    POHANDLE Kernel;
    BOOLEAN Enumerated;
    volatile LONG Halted;
    volatile LONG Attempts;
    volatile LONG Returns;
    volatile LONG LastStatus;
} T214_CPU;

static T214_CPU Cpus[CPU_COUNT];
static PEP_KERNEL_INFORMATION Kernel;
static volatile LONG DisplayOffState;
static PVOID DisplayCallback;
static WDFWORKITEM SnapshotWork;
static PUCHAR Car;
static WCHAR Parameters[320];
static const GUID DisplayGuid = {0x6fe69556,0x704a,0x47a0,{0x8f,0x24,0xc2,0x8d,0x93,0x6f,0xda,0x47}};
static UNICODE_STRING CpuNames[CPU_COUNT];
static const PCWSTR CpuPaths[] = {L"\\_SB.CPU0",L"\\_SB.CPU1",L"\\_SB.CPU2",L"\\_SB.CPU3"};

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD AddDevice;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT StartNotifications;
EVT_WDF_DEVICE_QUERY_REMOVE RefuseRemoval;
EVT_WDF_WORKITEM Snapshot;

static VOID Trace(PCWSTR Name, ULONG Value)
{
    if (Parameters[0] && KeGetCurrentIrql() == PASSIVE_LEVEL)
        (VOID)RtlWriteRegistryValue(RTL_REGISTRY_ABSOLUTE,Parameters,Name,REG_DWORD,&Value,sizeof(Value));
}
static T214_CPU *FindCpu(PCUNICODE_STRING Id)
{
    if (!Id) return NULL;
    for (ULONG i=0;i<CPU_COUNT;i++) if (RtlEqualUnicodeString(Id,&CpuNames[i],TRUE)) return &Cpus[i];
    return NULL;
}
static T214_CPU *CpuHandle(PEPHANDLE Handle)
{
    for (ULONG i=0;i<CPU_COUNT;i++) if (Handle==(PEPHANDLE)&Cpus[i]) return &Cpus[i];
    return NULL;
}
static ULONG OnlineCount(VOID)
{
    ULONG count=0;
    for (ULONG i=0;i<CPU_COUNT;i++) if (Cpus[i].Enumerated && Cpus[i].Kernel) count++;
    return count;
}
static BOOLEAN CanPowerDown(VOID)
{
    /* The fan driver restores the inherited CPU clock before screen-off
       cooling. Do not race an in-progress PLL relock or enter CC6 at 2 GHz. */
    if (!InterlockedCompareExchange(&DisplayOffState,0,0) || !Car) return FALSE;
    ULONG policy=READ_REGISTER_ULONG((PULONG)(Car+0x20));
    ULONG pll=READ_REGISTER_ULONG((PULONG)(Car+0xe0));
    ULONG m=pll&0xff,n=(pll>>8)&0xff,p=(pll>>20)&0x1f;
    if (policy!=0x20008888u || (pll&0xc8000000u)!=0x48000000u || !m || p!=0) return FALSE;
    return (38400u*n)/m <= 998400u;
}

static BOOLEAN DeviceNotify(ULONG Notification, PVOID Data)
{
    switch (Notification) {
    case PEP_DPM_PREPARE_DEVICE: {
        PPEP_PREPARE_DEVICE a=Data; a->DeviceAccepted=FindCpu(a->DeviceId)!=NULL;return TRUE;
    }
    case PEP_DPM_ABANDON_DEVICE: {
        PPEP_ABANDON_DEVICE a=Data;a->DeviceAccepted=FindCpu(a->DeviceId)!=NULL;return TRUE;
    }
    case PEP_DPM_REGISTER_DEVICE: {
        PPEP_REGISTER_DEVICE a=Data;T214_CPU *c=FindCpu(a->DeviceId);
        a->DeviceAccepted=PepDeviceNotAccepted;
        if (c) {c->Kernel=a->KernelHandle;a->DeviceHandle=(PEPHANDLE)c;a->DeviceAccepted=PepDeviceAccepted;}
        return TRUE;
    }
    case PEP_DPM_UNREGISTER_DEVICE: {
        T214_CPU *c=CpuHandle(((PPEP_UNREGISTER_DEVICE)Data)->DeviceHandle);
        if (c) {c->Kernel=NULL;c->Enumerated=FALSE;}return c!=NULL;
    }
    case PEP_DPM_DEVICE_POWER_STATE:
        return CpuHandle(((PPEP_DEVICE_POWER_STATE)Data)->DeviceHandle)!=NULL;
    default:return FALSE;
    }
}

static BOOLEAN ProcessorNotify(PEPHANDLE Handle,ULONG Notification,PVOID Data)
{
    T214_CPU *cpu=CpuHandle(Handle);
    switch (Notification) {
    case PEP_NOTIFY_PPM_QUERY_CAPABILITIES: {
        PPEP_PPM_QUERY_CAPABILITIES a=Data;if (!cpu) return FALSE;
        a->FeedbackCounterCount=0;a->IdleStateCount=IDLE_COUNT;
        a->PerformanceStatesSupported=FALSE;a->ParkingSupported=FALSE;
        a->DiscretePerformanceStateCount=0;cpu->Enumerated=TRUE;return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_IDLE_STATES_V2: {
        PPEP_PPM_QUERY_IDLE_STATES_V2 a=Data;if (!cpu || a->Count!=IDLE_COUNT) return FALSE;
        RtlZeroMemory(a->IdleStates,sizeof(a->IdleStates[0])*IDLE_COUNT);
        for (ULONG i=0;i<IDLE_COUNT;i++) {
            PPEP_PROCESSOR_IDLE_STATE_V2 s=&a->IdleStates[i];
            s->Interruptible=TRUE;s->WakesSpuriously=TRUE;
            s->CacheCoherent=(i==IDLE_WFI);s->ThreadContextRetained=(i==IDLE_WFI);
            s->PlatformOnly=(i==IDLE_CC6);
            /* 100 ns units: conservative 1 ms exit, 10 ms break-even. */
            s->Latency=i?10000:0;s->BreakEvenDuration=i?100000:0;
        }
        return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_PLATFORM_STATES:
        ((PPEP_PPM_QUERY_PLATFORM_STATES)Data)->PlatformStateCount=1;return TRUE;
    case PEP_NOTIFY_PPM_QUERY_COORDINATED_STATES: {
        PPEP_PPM_QUERY_COORDINATED_STATES a=Data;if (a->Count!=1) return FALSE;
        a->States[0].Latency=20000;a->States[0].BreakEvenDuration=200000;
        a->States[0].DependencyCount=OnlineCount();a->States[0].MaximumDependencySize=1;return TRUE;
    }
    case PEP_NOTIFY_PPM_QUERY_COORDINATED_DEPENDENCY: {
        PPEP_PPM_QUERY_COORDINATED_DEPENDENCY a=Data;ULONG index=0;
        if (a->StateIndex!=0 || a->DependencySize<1) return FALSE;
        for (ULONG i=0;i<CPU_COUNT;i++) if (Cpus[i].Enumerated && Cpus[i].Kernel) {
            if (index++!=a->DependencyIndex) continue;
            a->TargetProcessor=Cpus[i].Kernel;a->DependencySizeUsed=1;
            a->Options[0].ExpectedStateIndex=IDLE_CC6;a->Options[0].LooseDependency=TRUE;
            a->Options[0].InitiatingState=TRUE;a->Options[0].DependentState=TRUE;return TRUE;
        }
        return FALSE;
    }
    case PEP_NOTIFY_PPM_TEST_IDLE_STATE: {
        PPEP_PPM_TEST_IDLE_STATE a=Data;
        a->VetoReason=(a->ProcessorState!=IDLE_WFI && !CanPowerDown())?1:PEP_IDLE_VETO_NONE;return TRUE;
    }
    case PEP_NOTIFY_PPM_IDLE_EXECUTE: {
        PPEP_PPM_IDLE_EXECUTE_V2 a=Data;if (!cpu) return FALSE;
        if (a->ProcessorState>=IDLE_COUNT || (a->PlatformState!=0 && a->PlatformState!=PEP_PLATFORM_IDLE_STATE_NONE)) {
            a->Status=STATUS_INVALID_PARAMETER;return TRUE;
        }
        if (a->ProcessorState==IDLE_WFI || !CanPowerDown()) {
            __dmb(_ARM64_BARRIER_SY);__wfi();a->Status=STATUS_SUCCESS;return TRUE;
        }
        ULONG state=(a->PlatformState==0)?PSCI_CLUSTER_IDLE:PSCI_CORE_POWERDOWN;
        InterlockedIncrement(&cpu->Attempts);InterlockedExchange(&cpu->Halted,1);
        a->Status=Kernel.ProcessorHalt(PROCESSOR_HALT_VIA_PSCI_CPU_SUSPEND|PROCESSOR_HALT_CACHE_FLUSH_OVERRIDE,&state,NULL);
        InterlockedExchange(&cpu->Halted,0);InterlockedExchange(&cpu->LastStatus,a->Status);
        InterlockedIncrement(&cpu->Returns);return TRUE;
    }
    case PEP_NOTIFY_PPM_IDLE_COMPLETE:return TRUE;
    case PEP_NOTIFY_PPM_IS_PROCESSOR_HALTED:
        if (!cpu) return FALSE;
        ((PPEP_PPM_IS_PROCESSOR_HALTED)Data)->Halted=(BOOLEAN)InterlockedCompareExchange(&cpu->Halted,0,0);return TRUE;
    case PEP_NOTIFY_PPM_INITIATE_WAKE:
        ((PPEP_PPM_INITIATE_WAKE)Data)->NeedInterruptForCompletion=TRUE;return TRUE;
    case PEP_NOTIFY_PPM_QUERY_VETO_REASONS:
        ((PPEP_PPM_QUERY_VETO_REASONS)Data)->VetoReasonCount=1;return TRUE;
    case PEP_NOTIFY_PPM_QUERY_VETO_REASON: {
        PPEP_PPM_QUERY_VETO_REASON a=Data;static const WCHAR name[]=L"Display active or CPU clock transition";
        if (a->VetoReason!=1) return FALSE;
        if (a->Name && a->NameSize>=sizeof(name)) RtlCopyMemory(a->Name,name,sizeof(name));
        a->NameSize=sizeof(name);return TRUE;
    }
    default:return FALSE;
    }
}

static NTSTATUS DisplayChanged(LPCGUID Setting,PVOID Value,ULONG Length,PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    if (IsEqualGUID(Setting,&DisplayGuid) && Length==sizeof(ULONG)) {
        InterlockedExchange(&DisplayOffState,*(PULONG)Value==0);
        if (SnapshotWork) WdfWorkItemEnqueue(SnapshotWork);
    }
    return STATUS_SUCCESS;
}
VOID Snapshot(WDFWORKITEM Work)
{
    ULONG attempts=0,returns=0;
    UNREFERENCED_PARAMETER(Work);
    for (ULONG i=0;i<CPU_COUNT;i++) {
        attempts+=(ULONG)InterlockedCompareExchange(&Cpus[i].Attempts,0,0);
        returns+=(ULONG)InterlockedCompareExchange(&Cpus[i].Returns,0,0);
        if (Cpus[i].Enumerated) Trace(L"LastHaltStatus",(ULONG)InterlockedCompareExchange(&Cpus[i].LastStatus,0,0));
    }
    Trace(L"HaltAttempts",attempts);Trace(L"HaltReturns",returns);Trace(L"ProcessorCount",OnlineCount());
    Trace(L"DisplayOff",(ULONG)InterlockedCompareExchange(&DisplayOffState,0,0));
}
NTSTATUS StartNotifications(WDFDEVICE Device)
{
    NTSTATUS s=PoRegisterPowerSettingCallback(WdfDeviceWdmGetDeviceObject(Device),&DisplayGuid,
                                             DisplayChanged,NULL,&DisplayCallback);
    Trace(L"DisplayCallbackStatus",(ULONG)s);return s;
}
NTSTATUS RefuseRemoval(WDFDEVICE Device)
{
    UNREFERENCED_PARAMETER(Device);
    /* PoFx has no unregister operation. Servicing this boot driver requires restart. */
    return STATUS_UNSUCCESSFUL;
}
NTSTATUS AddDevice(WDFDRIVER Driver,PWDFDEVICE_INIT Init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS cb;WDFDEVICE device;NTSTATUS status;
    WDF_WORKITEM_CONFIG work;WDF_OBJECT_ATTRIBUTES attributes;
    UNREFERENCED_PARAMETER(Driver);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);cb.EvtDeviceSelfManagedIoInit=StartNotifications;
    cb.EvtDeviceQueryRemove=RefuseRemoval;cb.EvtDeviceQueryStop=RefuseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(Init,&cb);
    status=WdfDeviceCreate(&Init,WDF_NO_OBJECT_ATTRIBUTES,&device);
    if (!NT_SUCCESS(status)) return status;
    WDF_WORKITEM_CONFIG_INIT(&work,Snapshot);work.AutomaticSerialization=FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);attributes.ParentObject=device;
    return WdfWorkItemCreate(&work,&attributes,&SnapshotWork);
}
NTSTATUS DriverEntry(PDRIVER_OBJECT Object,PUNICODE_STRING Path)
{
    WDF_DRIVER_CONFIG cfg;PEP_INFORMATION info={0};PHYSICAL_ADDRESS pa;NTSTATUS s;
    if (Path->Length+sizeof(L"\\Parameters")>sizeof(Parameters)) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(Parameters,Path->Buffer,Path->Length);
    RtlCopyMemory((PUCHAR)Parameters+Path->Length,L"\\Parameters",sizeof(L"\\Parameters"));
    (VOID)RtlCreateRegistryKey(RTL_REGISTRY_ABSOLUTE,Parameters);
    pa.QuadPart=0x70000804;PULONG hid=MmMapIoSpaceEx(pa,sizeof(ULONG),PAGE_READWRITE|PAGE_NOCACHE);
    if (!hid) return STATUS_INSUFFICIENT_RESOURCES;
    ULONG id=READ_REGISTER_ULONG(hid);MmUnmapIoSpace(hid,sizeof(ULONG));
    if ((id&0xfff0u)!=0x2120u) return STATUS_NOT_SUPPORTED;
    pa.QuadPart=0x60006000;Car=MmMapIoSpaceEx(pa,0x100,PAGE_READWRITE|PAGE_NOCACHE);
    if (!Car) return STATUS_INSUFFICIENT_RESOURCES;
    WDF_DRIVER_CONFIG_INIT(&cfg,AddDevice);
    s=WdfDriverCreate(Object,Path,WDF_NO_OBJECT_ATTRIBUTES,&cfg,WDF_NO_HANDLE);
    if (!NT_SUCCESS(s)) {MmUnmapIoSpace(Car,0x100);Car=NULL;return s;}
    for (ULONG i=0;i<CPU_COUNT;i++) RtlInitUnicodeString(&CpuNames[i],CpuPaths[i]);
    info.Version=PEP_INFORMATION_VERSION;info.Size=sizeof(info);
    info.AcceptDeviceNotification=DeviceNotify;info.AcceptProcessorNotification=ProcessorNotify;
    Kernel.Version=PEP_KERNEL_INFORMATION_V3;Kernel.Size=sizeof(Kernel);
    s=PoFxRegisterPlugin(&info,&Kernel);Trace(L"RegisterStatus",(ULONG)s);Trace(L"DriverVersion",1);
    if (!NT_SUCCESS(s)) {MmUnmapIoSpace(Car,0x100);Car=NULL;}
    return s;
}
