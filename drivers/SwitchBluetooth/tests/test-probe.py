#!/usr/bin/env python3
"""Run the actual polled probe against firmware/baud failures and retained D0 state."""
from pathlib import Path
import os, re, subprocess, tempfile
source = (Path(__file__).resolve().parents[1] / 'src/switchbt.c').read_text()
names = ('SwBtReset', 'SwBtReadVersion', 'SwBtPatchram', 'SwBtHighSpeed', 'SwBtProbe')
functions = []
for name in names:
    m = re.search(r'^static (?:BOOLEAN|ULONG) ' + name + r'\(', source, re.M)
    assert m, name
    a=source.index('{',m.start()); b=a+1; depth=1
    while depth:
        depth += (source[b]=='{')-(source[b]=='}'); b+=1
    functions.append(source[m.start():b])
constants='\n'.join(line for line in source.splitlines() if re.match(r'#define (?:UART_MCR_|UART_DIV_|CAR_UART_|BCM_|HCI_(?:RESET|READ_))',line))
preamble=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>
typedef void VOID; typedef uint8_t UCHAR,*PUCHAR; typedef uint16_t USHORT;
typedef uint32_t ULONG,*PULONG; typedef wchar_t WCHAR; typedef bool BOOLEAN;
#define TRUE true
#define FALSE false
#define MAXULONG UINT32_MAX
#define RTL_NUMBER_OF(x) (sizeof(x)/sizeof((x)[0]))
#define POOL_TAG 0
#define UART_MCR 4
struct DEVICE { volatile ULONG *CarUartd; BOOLEAN HighSpeedNow,FirmwareVerified; ULONG FirmwareVersion; UCHAR BdAddr[6]; };
typedef struct DEVICE SWBT_DEVICE,*PSWBT_DEVICE;
static ULONG car,host_baud,chip_baud,version,patch_record,downloads,attempts,commands;
static ULONG failed_record,failed_opcode,fw_result,high_result;
static bool dead,reset_recovers,fail_once,bad_status,baud_reject,baud_lost_reply;
static bool reject_address,no_version; static int fail_record; static unsigned fw_loads;
static UCHAR firmware[3*7];static ULONG firmware_size;static ULONG sleep_ms;
static void SwBtTrace(const WCHAR *name,ULONG v) {
 if(!wcscmp(name,L"FirmwareFailedRecord"))failed_record=v;
 if(!wcscmp(name,L"FirmwareFailedOpcode"))failed_opcode=v;
 if(!wcscmp(name,L"FirmwareResult"))fw_result=v;
 if(!wcscmp(name,L"HighSpeedResult"))high_result=v;
}
static void SwBtTraceString(const WCHAR *n,const WCHAR *v){(void)n;(void)v;}
static void SwBtSleepMs(ULONG n){sleep_ms+=n;}
static void KeStallExecutionProcessor(ULONG n){(void)n;}
static void SwBtFlushRx(PSWBT_DEVICE d){(void)d;}
static void SwBtWrite(PSWBT_DEVICE d,ULONG r,ULONG v){(void)d;(void)r;(void)v;}
static ULONG READ_REGISTER_ULONG(PULONG p){return *p;}
static void WRITE_REGISTER_ULONG(PULONG p,ULONG v){*p=v;}
static void SwBtUartInit(PSWBT_DEVICE d,ULONG div,ULONG mcr){(void)d;(void)mcr;host_baud=div==221?115200:3000000;}
static void SwBtTraceVersion(PSWBT_DEVICE d,UCHAR *e,ULONG max){(void)d;(void)e;(void)max;}
static PUCHAR SwBtLoadFirmware(PULONG n){UCHAR *b=malloc(firmware_size);assert(b);memcpy(b,firmware,firmware_size);*n=firmware_size;fw_loads++;return b;}
static void ExFreePoolWithTag(void *p,ULONG tag){(void)tag;free(p);}
static void SwBtChooseAddress(PSWBT_DEVICE d){memset(d->BdAddr,1,6);}
static BOOLEAN SwBtReadParam(const WCHAR *n,PULONG v){(void)n;(void)v;return FALSE;}
static void RtlStringCchPrintfW(WCHAR *out,size_t n,const WCHAR *fmt,...){(void)fmt;assert(n);out[0]=0;}
'''
model=r'''
static ULONG SwBtPollCommand(PSWBT_DEVICE d,USHORT opcode,const UCHAR *params,UCHAR plen,UCHAR *event,ULONG max) {
 (void)d;(void)plen;assert(max>=15);commands++;
 if(host_baud!=chip_baud)return 0;
 memset(event,0,max);
 if(opcode==HCI_RESET) { if(dead&&!reset_recovers)return 0;dead=false;return 7; }
 if(dead)return 0;
 if(opcode==BCM_DOWNLOAD_MINI){downloads++;patch_record=0;return 7;}
 if(opcode==0xfc4c||opcode==0xfc4e) {
   if((int)patch_record==fail_record&&(!fail_once||attempts++==0)) {
     if(bad_status){event[6]=0x12;return 7;}dead=true;return 0;
   }
   patch_record++;if(opcode==0xfc4e)version=0x43211234;return 7;
 }
 if(opcode==BCM_UPDATE_BAUD) {
   if(baud_reject){event[6]=0x12;return 7;}
   chip_baud=(ULONG)params[2]|((ULONG)params[3]<<8)|((ULONG)params[4]<<16);
   return baud_lost_reply?0:7;
 }
 if(opcode==HCI_READ_VERSION) {
   if(no_version)return 0;
   event[8]=(UCHAR)version;event[9]=(UCHAR)(version>>8);
   event[13]=(UCHAR)(version>>16);event[14]=(UCHAR)(version>>24);return 15;
 }
 if(opcode==BCM_WRITE_BD_ADDR){event[6]=reject_address?0x12:0;return 7;}
 if(opcode==HCI_READ_BD_ADDR)return 13;
 if(opcode==HCI_READ_NAME)return 8;
 assert(false);return 0;
}
'''
tests=r'''
static void init(SWBT_DEVICE *d) {
 memset(d,0,sizeof(*d));d->CarUartd=&car;car=0;
 host_baud=chip_baud=115200;version=0x24092000;
 patch_record=downloads=attempts=commands=fw_loads=sleep_ms=0;
 failed_record=failed_opcode=fw_result=high_result=0;
 dead=reset_recovers=fail_once=bad_status=baud_reject=baud_lost_reply=false;
 reject_address=no_version=false;fail_record=-1;
 UCHAR f[]={0x4c,0xfc,4,1,2,3,4,0x4c,0xfc,4,5,6,7,8,0x4e,0xfc,4,255,255,255,255};
 memcpy(firmware,f,sizeof(f));firmware_size=sizeof(f);
}
int main(void) {
 SWBT_DEVICE d;UCHAR event[260];init(&d);
 assert(SwBtProbe(&d)&&d.FirmwareVerified&&d.HighSpeedNow&&downloads==1&&patch_record==3);
 /* D0 exit moves the UART back to ROM speed, but the patch remains resident. */
 chip_baud=115200;host_baud=115200;
 assert(SwBtProbe(&d)&&downloads==1&&fw_loads==1&&fw_result==2);
 /* Actual hardware power loss restores the ROM revision: load again. */
 chip_baud=115200;version=0x24092000;
 assert(SwBtProbe(&d)&&downloads==2&&fw_loads==2);
 /* Mid-download timeout must not publish an apparently healthy radio. */
 init(&d);fail_record=1;assert(!SwBtProbe(&d)&&!d.FirmwareVerified);
 assert(fw_result==0xf0000003&&failed_record==1&&failed_opcode==0xfc4c&&downloads==1);
 /* One complete retry is allowed only after an acknowledged reset. */
 init(&d);fail_record=1;fail_once=true;reset_recovers=true;
 assert(SwBtProbe(&d)&&downloads==2&&d.FirmwareVerified);
 /* A negative HCI status is not successful firmware download. */
 init(&d);fail_record=1;bad_status=true;assert(!SwBtProbe(&d));
 assert(downloads==2&&fw_result==0xf0000005&&!d.FirmwareVerified);
 /* Truncated headers/payloads are rejected before entering the minidriver. */
 for(ULONG n=1;n<7;n++) {
   init(&d);assert(SwBtPatchram(&d,firmware,n,event,sizeof(event))==0&&commands==0);
 }
 init(&d);firmware_size--;assert(!SwBtProbe(&d)&&downloads==0);
 /* Baud-change ACK loss: verify the new rate, do not assume 115200. */
 init(&d);baud_lost_reply=true;assert(SwBtProbe(&d)&&d.HighSpeedNow&&high_result==2);
 init(&d);baud_reject=true;assert(SwBtProbe(&d)&&!d.HighSpeedNow&&high_result==1);
 init(&d);dead=true;assert(!SwBtHighSpeed(&d,event,sizeof(event))&&high_result==4);
 init(&d);no_version=true;assert(!SwBtProbe(&d)&&downloads==0);
 init(&d);reject_address=true;assert(!SwBtProbe(&d));
 puts("PASS: patch failure/status/truncation, bounded reset recovery, retained D0 patch and verified baud fallback");
}
'''
with tempfile.TemporaryDirectory(prefix='switchbt-probe-') as tmp:
 c=Path(tmp)/'test.c';exe=Path(tmp)/'test'
 c.write_text(preamble+constants+'\n'+model+'\n'.join(functions)+tests)
 subprocess.run([os.environ.get('CC','clang'),'-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-g',str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
