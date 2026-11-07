#!/usr/bin/env python3
"""Exercise real button callbacks across stopped I2C, wake presses and rapid transitions."""
from pathlib import Path
import os,re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'src/switchpwrbtn.c').read_text()
functions=[]
for name in ['SwPwrEvtTimer','SwPwrEvtSuspend','SwPwrEvtRestart']:
 m=re.search(r'^(?:VOID|NTSTATUS) '+name+r'\(',s,re.M);assert m
 b=s.index('{',m.start());n=1;e=b+1
 while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
 functions.append(s[m.start():e])
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <wchar.h>
#include <string.h>
#include <stdio.h>
typedef void VOID;typedef uint8_t UCHAR;typedef bool BOOLEAN;
typedef uint32_t ULONG;typedef int32_t LONG,NTSTATUS;typedef int KIRQL;
#define TRUE true
#define FALSE false
#define STATUS_SUCCESS 0
#define NT_SUCCESS(x) ((x)>=0)
#define MAX77620_ONOFFIRQ 0x0b
#define MAX77620_ONOFFSTAT 0x15
#define ONOFFSTAT_EN0 4
#define ONOFFIRQ_EN0_R 8
#define POLL_MS 50
#define WDF_REL_TIMEOUT_IN_MS(x) (x)
typedef struct Device *WDFDEVICE,*PSWPWR_DEVICE;
typedef struct Timer {WDFDEVICE Device;bool armed;} *WDFTIMER;
struct Device {bool Stopping,Pressed,SuppressHeld;int TimerLock;WDFTIMER Timer;void *I2c,*Vhf,*Interrupt;
 ULONG Failures;LONG Polls,Presses,Reports;};
static bool locked,fail;static UCHAR irq,stat;
static unsigned reads,count;static bool reports[64];
static PSWPWR_DEVICE SwPwrGetDevice(WDFDEVICE d){return d;}
static WDFTIMER SwPwrGetTimer(WDFTIMER t){return t;}
static void SwPwrTrace(const wchar_t *n,ULONG v){(void)n;(void)v;}
static void SwPwrReport(PSWPWR_DEVICE d,bool p){(void)d;assert(!locked&&count<64);reports[count++]=p;}
static LONG InterlockedIncrement(LONG *p){return ++*p;}
static void KeAcquireSpinLock(int *l,KIRQL *i){(void)l;*i=0;assert(!locked);locked=true;}
static void KeReleaseSpinLock(int *l,KIRQL i){(void)l;(void)i;assert(locked);locked=false;}
static void WdfTimerStart(WDFTIMER t,int ms){assert(locked&&ms==50&&!t->Device->Stopping);t->armed=true;}
static void WdfTimerStop(WDFTIMER t,bool wait){assert(!locked&&wait&&t->Device->Stopping);t->armed=false;}
static NTSTATUS SwPwrReadReg(void *i,UCHAR r,UCHAR *v){(void)i;assert(!locked);reads++;if(fail)return -1;
 if(r==MAX77620_ONOFFIRQ){*v=irq;irq=0;}else {assert(r==MAX77620_ONOFFSTAT);*v=stat;}return 0;}
'''
tests=r'''
int main(void){
 struct Device d={0};struct Timer t={&d,false};d.Timer=&t;d.Vhf=&d;d.Stopping=true;
 assert(SwPwrEvtRestart(&d)==0&&t.armed&&!d.Stopping);
 stat=4;SwPwrEvtTimer(&t);assert(count==1&&reports[0]&&d.Pressed);
 SwPwrEvtSuspend(&d);assert(!t.armed&&d.Stopping&&!d.Pressed&&count==2&&!reports[1]);
 unsigned saved=reads;SwPwrEvtTimer(&t);assert(reads==saved&&!t.armed);
 /* Waking while the physical key is held emits no second power-down press. */
 irq=8;stat=4;assert(SwPwrEvtRestart(&d)==0&&d.SuppressHeld);
 SwPwrEvtTimer(&t);assert(count==2);
 stat=0;SwPwrEvtTimer(&t);assert(!d.SuppressHeld&&count==2);
 stat=4;SwPwrEvtTimer(&t);assert(count==3&&reports[2]);
 stat=0;SwPwrEvtTimer(&t);assert(count==4&&!reports[3]);
 /* A short press still generates exactly one pair after resume. */
 irq=8;SwPwrEvtTimer(&t);assert(count==6&&reports[4]&&!reports[5]);
 /* Failed resume cannot restart a poll against a sleeping I2C controller. */
 SwPwrEvtSuspend(&d);fail=true;assert(SwPwrEvtRestart(&d)<0&&!t.armed&&d.Stopping);fail=false;
 for(int n=0;n<20;n++){assert(SwPwrEvtRestart(&d)==0);SwPwrEvtTimer(&t);SwPwrEvtSuspend(&d);}
 assert(count==6&&!t.armed&&d.Stopping&&!locked);
 puts("PASS: timer drain, stopped I2C, wake-press consumption, short presses and repeated suspend/resume");
}
'''
with tempfile.TemporaryDirectory(prefix='switchpwrbtn-sleep-') as tmp:
 c=Path(tmp)/'test.c';e=Path(tmp)/'test';c.write_text(prefix+'\n'.join(functions)+tests)
 subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(e)],check=True)
 subprocess.run([str(e)],check=True)
