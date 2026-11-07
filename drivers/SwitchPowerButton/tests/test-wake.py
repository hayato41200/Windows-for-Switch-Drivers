#!/usr/bin/env python3
"""Exercise the actual PMIC wake IRQ path without changing unrelated PMIC bits."""
from pathlib import Path
import re,subprocess,tempfile
s=(Path(__file__).resolve().parents[1]/'src/switchpwrbtn.c').read_text();parts=[]
for name in ['SwPwrSetIrq','SwPwrEvtD0Entry','SwPwrEvtD0Exit','SwPwrEvtIsr']:
 m=re.search(r'^(?:static )?(?:NTSTATUS|BOOLEAN) '+name+r'\(',s,re.M);assert m
 b=s.index('{',m.start());e=b+1;n=1
 while n:n+=(s[e]=='{')-(s[e]=='}');e+=1
 parts.append(s[m.start():e])
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
typedef bool BOOLEAN;typedef int32_t NTSTATUS;typedef uint32_t ULONG;typedef uint8_t UCHAR;
#define VOID void
#define TRUE true
#define FALSE false
#define STATUS_SUCCESS 0
#define NT_SUCCESS(s) ((s)>=0)
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define MAX77620_IRQTOPM 0x0d
#define MAX77620_INTENLBT 0x0e
#define MAX77620_ONOFFIRQM 0x12
#define MAX77620_ONOFFIRQ 0x0b
#define MAX77620_ONOFFSTAT 0x15
#define ONOFFSTAT_EN0 4
#define ONOFFIRQ_EN0_R 8
#define WdfPowerDeviceD3Final 5
typedef int WDF_POWER_DEVICE_STATE,POWER_ACTION;
enum {PowerActionNone,PowerActionSleep,PowerActionShutdown,PowerActionShutdownReset,PowerActionShutdownOff};
struct Device{void *I2c,*Vhf,*Interrupt;int I2cId,Presses;bool IrqConfigured;UCHAR OriginalTopMask,OriginalGlobalMask,OriginalOnOffMask;};
typedef struct Device *WDFDEVICE,*PSWPWR_DEVICE,*WDFINTERRUPT;
static UCHAR regs[256];static int fail=-1;static POWER_ACTION action;static bool reports[16];static unsigned count;
static NTSTATUS SwPwrReadReg(void *t,UCHAR r,UCHAR *v){(void)t;if(r==fail)return -1;*v=regs[r];if(r==0x0b)regs[r]=0;return 0;}
static NTSTATUS SwPwrWriteReg(void *t,UCHAR r,UCHAR v){(void)t;if(r==fail)return -1;regs[r]=v;return 0;}
static PSWPWR_DEVICE SwPwrGetDevice(WDFDEVICE d){return d;}
static WDFDEVICE WdfInterruptGetDevice(WDFINTERRUPT i){return i;}
static POWER_ACTION WdfDeviceGetSystemPowerAction(WDFDEVICE d){(void)d;return action;}
static NTSTATUS SwPwrOpenTarget(WDFDEVICE d,int id,void **p){(void)id;*p=d;return 0;}
static void SwPwrTrace(const wchar_t *n,ULONG v){(void)n;(void)v;}
static void SwPwrReport(PSWPWR_DEVICE d,bool p){(void)d;assert(count<16);reports[count++]=p;}
static int InterlockedIncrement(int *p){return ++*p;}
'''
tests=r'''
int main(void){struct Device d={0};d.Interrupt=&d;d.Vhf=&d;
 regs[0x0d]=0xb7;regs[0x0e]=0xa5;regs[0x12]=0xff;regs[0x0b]=8;
 assert(SwPwrEvtD0Entry(&d,0)==0&&d.IrqConfigured&&regs[0x0b]==0);
 assert(regs[0x0d]==0xb5&&regs[0x0e]==0xa4&&regs[0x12]==0xf3);
 /* Sleep keeps the wake route and D0 resume does not discard the wake edge. */
 action=PowerActionSleep;assert(SwPwrEvtD0Exit(&d,3)==0&&d.IrqConfigured);
 regs[0x0b]=8;regs[0x15]=4;assert(SwPwrEvtD0Entry(&d,3)==0&&regs[0x0b]==8);
 assert(SwPwrEvtIsr(&d,0)&&count==2&&reports[0]&&!reports[1]&&regs[0x0b]==0);
 regs[0x0b]=4;regs[0x15]=0;assert(SwPwrEvtIsr(&d,0)&&count==2);
 assert(!SwPwrEvtIsr(&d,0)&&count==2);
 /* A different PMIC user changes other bits: shutdown preserves those edits. */
 regs[0x0d]^=0x80;regs[0x0e]^=0x20;regs[0x12]^=0x80;
 action=PowerActionShutdown;assert(SwPwrEvtD0Exit(&d,3)==0&&!d.IrqConfigured);
 assert(regs[0x0d]==0x37&&regs[0x0e]==0x85&&regs[0x12]==0x7f);
 assert(SwPwrEvtD0Entry(&d,3)==0);fail=0x0b;
 assert(SwPwrEvtIsr(&d,0)&&!d.IrqConfigured);fail=-1;
 puts("PASS: PMIC wake mask ownership, wake-edge retention, HID pulse and final shutdown restoration");}
'''
with tempfile.TemporaryDirectory(prefix='switchpwrbtn-wake-') as d:
 p=Path(d);(p/'test.c').write_text(prefix+'\n'.join(parts)+tests)
 subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
