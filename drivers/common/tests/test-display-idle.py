#!/usr/bin/env python3
"""Check real screen-driven WDF idle-reference lifetime and asynchronous wake."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'display_idle.h').read_text();parts=[]
for name in ['SwDisplayIdleWorker','SwDisplayIdleChanged','SwDisplayIdleStart','SwDisplayIdleStop']:
 m=re.search(r'^static (?:VOID|NTSTATUS) '+name+r'\(',s,re.M);assert m
 b=s.index('{',m.start());e=b+1;n=1
 while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
 parts.append(s[m.start():e])
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef void VOID,*PVOID;typedef int32_t LONG,NTSTATUS;typedef uint32_t ULONG,*PULONG;
typedef bool BOOLEAN;typedef void *WDFDEVICE;typedef const int *LPCGUID;
#define FALSE false
#define TRUE true
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define IsEqualGUID(a,b) ((a)==(b))
typedef struct Work *WDFWORKITEM;
typedef struct {WDFDEVICE Device;WDFWORKITEM Work;PVOID Callback;volatile LONG DisplayOn,Closing;BOOLEAN Held;NTSTATUS LastStatus;} SW_DISPLAY_IDLE;
struct Work {SW_DISPLAY_IDLE *Idle;bool queued;};
static const int SwDisplayIdleGuid=1;
static unsigned references,stops,resumes,unregistered;static NTSTATUS stop_result,register_result;
static NTSTATUS (*callback)(LPCGUID,PVOID,ULONG,PVOID);static PVOID cb_context;
static void run(WDFWORKITEM w);
static struct Work *SwDisplayIdleWork(WDFWORKITEM w){return w;}
static LONG InterlockedCompareExchange(volatile LONG *p,LONG v,LONG c){LONG old=*p;if(old==c)*p=v;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG v){LONG old=*p;*p=v;return old;}
static NTSTATUS WdfDeviceStopIdle(WDFDEVICE d,BOOLEAN wait){(void)d;assert(!wait);stops++;if(NT_SUCCESS(stop_result))references++;return stop_result;}
static void WdfDeviceResumeIdle(WDFDEVICE d){(void)d;assert(references);references--;resumes++;}
static void WdfWorkItemEnqueue(WDFWORKITEM w){w->queued=true;}
static void WdfWorkItemFlush(WDFWORKITEM w){run(w);}
static WDFDEVICE WdfDeviceWdmGetDeviceObject(WDFDEVICE d){return d;}
static NTSTATUS PoRegisterPowerSettingCallback(WDFDEVICE d,LPCGUID g,NTSTATUS (*fn)(LPCGUID,PVOID,ULONG,PVOID),PVOID c,PVOID *h){
 (void)d;if(register_result<0)return register_result;callback=fn;cb_context=c;*h=c;ULONG on=1;fn(g,&on,4,c);return 0;}
static NTSTATUS PoUnregisterPowerSettingCallback(PVOID h){assert(h==cb_context);callback=NULL;unregistered++;return 0;}
'''
tests=r'''
static void run(WDFWORKITEM w){while(w->queued){w->queued=false;SwDisplayIdleWorker(w);}}
static void state(WDFWORKITEM w,ULONG n){assert(callback);callback(&SwDisplayIdleGuid,&n,4,cb_context);run(w);}
int main(void){SW_DISPLAY_IDLE d={0};struct Work w={&d,false};d.Work=&w;d.Device=&d;d.DisplayOn=1;
 assert(SwDisplayIdleStart(&d)==0);run(&w);assert(references==1&&d.Held&&stops==1);
 for(unsigned n=0;n<50;n++){
  state(&w,0);assert(references==0&&!d.Held);state(&w,0);assert(references==0);
  stop_result=0x103;state(&w,1);assert(references==1&&d.Held);state(&w,2);assert(references==1);
 }
 /* A failed wake obtains no power reference and cleanup must not release one. */
 state(&w,0);stop_result=-1;state(&w,1);assert(!d.Held&&references==0);
 stop_result=0;state(&w,1);assert(d.Held&&references==1);
 /* A queued notification racing removal cannot re-acquire a released reference. */
 ULONG off=0;callback(&SwDisplayIdleGuid,&off,4,cb_context);SwDisplayIdleStop(&d);
 assert(!references&&!d.Held&&!d.Callback&&d.Closing&&unregistered==1&&!w.queued);
 /* Registration failure balances the one initial reference. */
 register_result=-7;assert(SwDisplayIdleStart(&d)==-7&&!references&&!d.Held);
 assert(resumes+1==stops); /* one failed StopIdle never owned a reference */
 puts("PASS: display transitions, pending/failed asynchronous wake, reference balance and teardown race");}
'''
with tempfile.TemporaryDirectory(prefix='display-idle-') as d:
 p=Path(d);(p/'test.c').write_text(prefix+'\n'.join(parts)+tests)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
