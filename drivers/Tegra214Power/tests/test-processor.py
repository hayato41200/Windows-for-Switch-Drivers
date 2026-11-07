#!/usr/bin/env python3
"""Exercise the actual PoFx processor dispatch with a mock Windows halt callback."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'src/tegra214pep.c').read_text();parts=[]
for name in ['OnlineCount','CanPowerDown','ProcessorNotify']:
 m=re.search(r'^static (?:ULONG|BOOLEAN) '+name+r'\(',s,re.M);assert m
 b=s.index('{',m.start());e=b+1;n=1
 while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
 parts.append(s[m.start():e])
constants='\n'.join(x for x in s.splitlines() if x.startswith('#define ') and not 'PEP_KERNEL' in x)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
typedef int32_t LONG,NTSTATUS;typedef uint32_t ULONG,*PULONG;typedef uint16_t USHORT;
typedef bool BOOLEAN;typedef void VOID,*PVOID,*PEPHANDLE,*POHANDLE;typedef wchar_t WCHAR;
#define TRUE true
#define FALSE false
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define PEP_IDLE_VETO_NONE 0
#define PEP_PLATFORM_IDLE_STATE_NONE UINT32_MAX
#define PROCESSOR_HALT_VIA_PSCI_CPU_SUSPEND 16
#define PROCESSOR_HALT_CACHE_FLUSH_OVERRIDE 1
#define _ARM64_BARRIER_SY 15
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory memcpy
enum {PEP_NOTIFY_PPM_QUERY_CAPABILITIES=1,PEP_NOTIFY_PPM_QUERY_IDLE_STATES_V2,PEP_NOTIFY_PPM_QUERY_PLATFORM_STATES,PEP_NOTIFY_PPM_QUERY_COORDINATED_STATES,PEP_NOTIFY_PPM_QUERY_COORDINATED_DEPENDENCY,PEP_NOTIFY_PPM_TEST_IDLE_STATE,PEP_NOTIFY_PPM_IDLE_EXECUTE,PEP_NOTIFY_PPM_IDLE_COMPLETE,PEP_NOTIFY_PPM_IS_PROCESSOR_HALTED,PEP_NOTIFY_PPM_INITIATE_WAKE,PEP_NOTIFY_PPM_QUERY_VETO_REASONS,PEP_NOTIFY_PPM_QUERY_VETO_REASON};
#define TYPE(name,...) typedef struct {__VA_ARGS__} name,*P##name
TYPE(PEP_PPM_QUERY_CAPABILITIES, ULONG FeedbackCounterCount,IdleStateCount;BOOLEAN PerformanceStatesSupported,ParkingSupported;unsigned char DiscretePerformanceStateCount;);
TYPE(PEP_PROCESSOR_IDLE_STATE_V2, BOOLEAN Interruptible,WakesSpuriously,CacheCoherent,ThreadContextRetained,PlatformOnly;ULONG Latency,BreakEvenDuration;);
TYPE(PEP_PPM_QUERY_IDLE_STATES_V2, ULONG Count;PEP_PROCESSOR_IDLE_STATE_V2 IdleStates[3];);
TYPE(PEP_PPM_QUERY_PLATFORM_STATES, ULONG PlatformStateCount;);
TYPE(PEP_PPM_QUERY_COORDINATED_STATES, ULONG Count;struct {ULONG Latency,BreakEvenDuration,DependencyCount,MaximumDependencySize;} States[1];);
TYPE(PEP_PPM_QUERY_COORDINATED_DEPENDENCY, ULONG StateIndex,DependencyIndex,DependencySize,DependencySizeUsed;POHANDLE TargetProcessor;struct {unsigned char ExpectedStateIndex;BOOLEAN LooseDependency,InitiatingState,DependentState;} Options[1];);
TYPE(PEP_PPM_TEST_IDLE_STATE, ULONG ProcessorState,PlatformState,VetoReason;);
TYPE(PEP_PPM_IDLE_EXECUTE_V2, NTSTATUS Status;ULONG ProcessorState,PlatformState,CoordinatedStateCount;PULONG CoordinatedStates;);
TYPE(PEP_PPM_IS_PROCESSOR_HALTED, BOOLEAN Halted;);
TYPE(PEP_PPM_INITIATE_WAKE, BOOLEAN NeedInterruptForCompletion;);
TYPE(PEP_PPM_QUERY_VETO_REASONS, ULONG VetoReasonCount;);
TYPE(PEP_PPM_QUERY_VETO_REASON, ULONG VetoReason;USHORT NameSize;WCHAR *Name;);
typedef struct {POHANDLE Kernel;BOOLEAN Enumerated;volatile LONG Halted,Attempts,Returns,LastStatus;} T214_CPU;
static T214_CPU Cpus[4];static volatile LONG DisplayOffState;static uint8_t registers[256],*Car=registers;
static unsigned halts,wfis;static ULONG last_state;static NTSTATUS halt_result;
static NTSTATUS Halt(ULONG flags,PVOID context,PVOID callback){assert(flags==17&&callback==NULL);last_state=*(PULONG)context;halts++;return halt_result;}
static struct {NTSTATUS (*ProcessorHalt)(ULONG,PVOID,PVOID);} Kernel={Halt};
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG compare){LONG old=*p;if(old==compare)*p=value;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG value){LONG old=*p;*p=value;return old;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static ULONG READ_REGISTER_ULONG(PULONG p){return *p;}
static void __dmb(int barrier){assert(barrier==15);}
static void __wfi(void){wfis++;}
static T214_CPU *CpuHandle(PEPHANDLE h){for(int i=0;i<4;i++)if(h==&Cpus[i])return &Cpus[i];return NULL;}
'''
tests=r'''
int main(void){
 for(unsigned i=0;i<4;i++){
  Cpus[i].Kernel=&Cpus[i];PEP_PPM_QUERY_CAPABILITIES cap={0};
  assert(ProcessorNotify(&Cpus[i],PEP_NOTIFY_PPM_QUERY_CAPABILITIES,&cap)&&cap.IdleStateCount==3&&!cap.PerformanceStatesSupported);
 }
 PEP_PPM_QUERY_IDLE_STATES_V2 states={.Count=3};
 assert(ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_QUERY_IDLE_STATES_V2,&states));
 assert(states.IdleStates[0].ThreadContextRetained&&!states.IdleStates[1].ThreadContextRetained&&states.IdleStates[2].PlatformOnly);
 PEP_PPM_QUERY_COORDINATED_STATES coordinated={.Count=1};
 assert(ProcessorNotify(NULL,PEP_NOTIFY_PPM_QUERY_COORDINATED_STATES,&coordinated)&&coordinated.States[0].DependencyCount==4);
 for(unsigned i=0;i<4;i++){
  PEP_PPM_QUERY_COORDINATED_DEPENDENCY d={.DependencyIndex=i,.DependencySize=1};
  assert(ProcessorNotify(NULL,PEP_NOTIFY_PPM_QUERY_COORDINATED_DEPENDENCY,&d)&&d.TargetProcessor==&Cpus[i]);
  assert(d.Options[0].ExpectedStateIndex==2&&d.Options[0].LooseDependency);
 }
 *(PULONG)(Car+0x20)=0x20008888;*(PULONG)(Car+0xe0)=0x48003402;
 PEP_PPM_TEST_IDLE_STATE test={.ProcessorState=1};
 ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_TEST_IDLE_STATE,&test);assert(test.VetoReason==1);
 PEP_PPM_IDLE_EXECUTE_V2 run={.ProcessorState=1,.PlatformState=UINT32_MAX};
 assert(ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_IDLE_EXECUTE,&run)&&wfis==1&&halts==0);
 DisplayOffState=1;assert(CanPowerDown());
 ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_TEST_IDLE_STATE,&test);assert(test.VetoReason==0);
 assert(ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_IDLE_EXECUTE,&run)&&halts==1&&last_state==0x10007);
 run.PlatformState=0;run.ProcessorState=2;halt_result=-5;
 assert(ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_IDLE_EXECUTE,&run)&&halts==2&&last_state==0x1010010&&run.Status==-5);
 assert(Cpus[0].Attempts==2&&Cpus[0].Returns==2&&!Cpus[0].Halted&&Cpus[0].LastStatus==-5);
 /* Active display, unlocked PLL, overclock or a policy transition veto deep idle. */
 *(PULONG)(Car+0xe0)=0x48006802;assert(!CanPowerDown());
 run.Status=0;ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_IDLE_EXECUTE,&run);assert(wfis==2&&halts==2);
 *(PULONG)(Car+0xe0)=0x40003402;assert(!CanPowerDown());
 *(PULONG)(Car+0xe0)=0x48003402;*(PULONG)(Car+0x20)=0x20000000;assert(!CanPowerDown());
 run.ProcessorState=99;ProcessorNotify(&Cpus[0],PEP_NOTIFY_PPM_IDLE_EXECUTE,&run);assert(run.Status==STATUS_INVALID_PARAMETER&&halts==2);
 puts("PASS: PoFx CPU dependencies, verified clock gating, context-saving halt dispatch, failures and wake bookkeeping");
}
'''
with tempfile.TemporaryDirectory(prefix='t214-pep-') as d:
 p=Path(d);(p/'test.c').write_text(prefix+constants+'\n'+'\n'.join(parts)+tests)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
