#!/usr/bin/env python3
"""Exercise the real S4 identity-read and write-guard code with polled SDHC I/O."""
from pathlib import Path
import ast
import re
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
tree = ast.parse((root / 'tests/test-t214-data.py').read_text())
prefix = ast.literal_eval(next(n.value for n in tree.body if isinstance(n, ast.Assign)
                              and any(isinstance(t, ast.Name) and t.id == 'prefix' for t in n.targets)))
extract = next(n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'function')
exec(compile(ast.Module(body=[extract], type_ignores=[]), '<extract-c>', 'exec'))
source = (root / 'src/nvtsdhc.c').read_text()
board = (root / 'src/t214_sdmmc.h').read_text()
target = Path(sys.argv[1]).read_text()
prefix = prefix.replace('#define NV_T214_HIBERNATE 0', '#define NV_T214_HIBERNATE 1')
prefix = prefix.replace('static bool clock_fail;', '''static bool clock_fail;
static unsigned mode, fifo, sector_reads, active_lba, error_resets;
static UCHAR mbr[512],ntfs[512];
''')
old = 'static ULONG SdhcReadRegisterUlong(PSDHC_EXTENSION s,ULONG off){ULONG v;memcpy(&v,s->regs+off,4);return v;}'
new = '''static ULONG SdhcReadRegisterUlong(PSDHC_EXTENSION s,ULONG off){
 ULONG v;
 if(off==0x20){assert(fifo<128);memcpy(&v,(active_lba?ntfs:mbr)+fifo++*4,4);
  if(fifo==128){ULONG done=2;memcpy(s->regs+0x30,&done,4);}return v;}
 memcpy(&v,s->regs+off,4);return v;
}'''
assert old in prefix
prefix = prefix.replace(old, new)
prefix = prefix.replace('static void T214Write(PUCHAR b,ULONG off,ULONG v){memcpy(b+off,&v,4);}',
                        'static void T214Write(PUCHAR b,ULONG off,ULONG v){if(off==0x30){ULONG old;memcpy(&old,b+off,4);v=old&~v;}memcpy(b+off,&v,4);}')
prefix = prefix.replace('static void SdhcWriteRegisterUchar(PSDHC_EXTENSION s,ULONG off,UCHAR v){s->regs[off]=v;}',
                        'static void SdhcWriteRegisterUchar(PSDHC_EXTENSION s,ULONG off,UCHAR v){if(off==0x2f){error_resets++;v=0;}s->regs[off]=v;}')
old = 'static void SdhcWriteRegisterUshort(PSDHC_EXTENSION s,ULONG off,USHORT v){if(off==SDHC_CLOCK_CONTROL&&(v&1)&&!clock_fail)v|=2;memcpy(s->regs+off,&v,2);}'
new = '''static void SdhcWriteRegisterUshort(PSDHC_EXTENSION s,ULONG off,USHORT v){
 if(off==SDHC_CLOCK_CONTROL&&(v&1)&&!clock_fail)v|=2;
 if(off==0x30){USHORT old;memcpy(&old,s->regs+off,2);v=old&~v;}
 memcpy(s->regs+off,&v,2);
 if(off==0x0e){assert(v==0x113a);fifo=0;sector_reads++;
  active_lba=SdhcReadRegisterUlong(s,8);ULONG events=mode==1?0x108000:mode==2?0:0x21;
  memcpy(s->regs+0x30,&events,4);}
}'''
assert old in prefix
prefix = prefix.replace(old, new)
prefix += r'''
#define SDHC_INTERRUPT_STATUS 0x30
#define SDHC_BLOCK_SIZE 4
#define SDHC_BLOCK_COUNT 6
#define SDHC_ARGUMENT 8
#define SDHC_COMMAND 0x0e
#define SDHC_DATA_PORT 0x20
#define SDHC_TC_MAX_DATA_TIMEOUT 14
#define SDHC_TM_TRANSFER_READ 16
#define SDHC_TM_BLKCNT_ENABLE 2
#define SDHC_IS_CMD_COMPLETE 1
#define SDHC_IS_BUFFER_READ_READY 32
#define SDHC_IS_TRANSFER_COMPLETE 2
#define SDHC_RESET 0x2f
#define SDHC_RESET_CMD 2
#define SDHC_RESET_DAT 4
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_MEDIA_WRITE_PROTECTED -5
static BOOLEAN T214WaitClear(PUCHAR b,ULONG o,ULONG mask,ULONG timeout){
 (void)timeout;return !(T214Read(b,o)&mask);
}
'''
code = prefix + target
for name, result in [('LiteObserveRead','void'),('LiteBlocksInExtent','BOOLEAN'),('LiteAllowRequest','BOOLEAN')]:
    code += function(source, name, result)
for name, result in [('T214ReadIdentitySector','NTSTATUS'),('T214ValidateHibernateTarget','NTSTATUS'),('T214BuildDma','BOOLEAN')]:
    code += function(board, name, result)
original = re.search(r'original\[70\]\s*=\s*\{([^}]+)', target).group(1)
code += '\nstatic const UCHAR original[70]={' + original + '};\n'
code += r'''
static void u32(UCHAR*p,ULONG v){memcpy(p,&v,4);}
static void u64(UCHAR*p,uint64_t v){memcpy(p,&v,8);}
int main(void){
 SDHC_EXTENSION s={0};s.BaseAddress=s.regs;s.CrashdumpMode=1;s.TargetFacts=LiteSdxc;s.NextReadLba=99;
 memcpy(mbr+440,original,70);mbr[510]=0x55;mbr[511]=0xaa;
 memcpy(ntfs+3,"NTFS    ",8);ntfs[12]=2;ntfs[13]=8;ntfs[510]=0x55;ntfs[511]=0xaa;
 u32(ntfs+28,WINARM_FIRST);u64(ntfs+40,WINARM_COUNT-1);u64(ntfs+72,WINARM_SERIAL);
 T214Write(s.regs,0x34,0x1111);T214Write(s.regs,0x38,0x2222);
 assert(T214ValidateHibernateTarget(&s)==0 && sector_reads==2);
 assert(s.TargetFacts==LiteWriteFacts && s.NextReadLba==99);
 assert(T214Read(s.regs,0x34)==0x1111 && T214Read(s.regs,0x38)==0x2222);
 SDPORT_REQUEST r={0};r.Type=SdRequestTypeCommandWithTransfer;
 UCHAR payload[16384]={0};r.Command.DataBuffer=payload;r.Command.Length=sizeof(payload);
 r.Command.Index=25;r.Command.BlockSize=512;r.Command.BlockCount=32;r.Command.Argument=WINARM_FIRST+32;
 r.Command.TransferDirection=SdTransferDirectionWrite;r.Command.TransferMethod=SdTransferMethodPio;
 r.Command.TransferType=SdTransferTypeMultiBlock;
 assert(LiteAllowRequest(&s,&r));
 _Alignas(16) UCHAR bounce[20480];memset(bounce,0xcc,sizeof(bounce));
 s.T214Bounce=bounce;s.T214DmaCapacity=16384;s.T214BouncePa.LowPart=0x8a000000;
 assert(T214BuildDma(&s,&r));assert(!memcmp(payload,bounce,sizeof(payload)));
 ULONG *desc=(ULONG*)(bounce+16384);assert(desc[0]==0x40000023 && desc[1]==0x8a000000);
 s.T214DmaActive=0;r.Command.BlockCount=33;assert(!T214BuildDma(&s,&r));r.Command.BlockCount=32;
 r.Command.Argument=WINARM_FIRST+WINARM_COUNT-31;assert(!LiteAllowRequest(&s,&r));
#ifdef FAT32_FIRST
 s.TargetFacts|=LiteFatBoot;r.Command.Argument=FAT32_FIRST+32;assert(!LiteAllowRequest(&s,&r));
#endif
 r.Command.Argument=0;assert(!LiteAllowRequest(&s,&r));
 s.TargetFacts=LiteSdxc;ntfs[72]^=1;
 assert(T214ValidateHibernateTarget(&s)==STATUS_MEDIA_WRITE_PROTECTED && !(s.TargetFacts&LiteWriteFacts));ntfs[72]^=1;
 s.TargetFacts=LiteSdxc;mbr[510]=0;assert(T214ValidateHibernateTarget(&s)==STATUS_MEDIA_WRITE_PROTECTED);mbr[510]=0x55;
 for(mode=1;mode<=2;mode++){
  s.TargetFacts=LiteSdxc;assert(T214ValidateHibernateTarget(&s)==(mode==1?STATUS_IO_DEVICE_ERROR:STATUS_IO_TIMEOUT));
  assert(!(s.TargetFacts&LiteWriteFacts) && T214Read(s.regs,0x38)==0x2222);
 }
 mode=0;s.TargetFacts=LiteSdxc;s.AppPrefixAccepted=1;
 unsigned reads=sector_reads;assert(T214ValidateHibernateTarget(&s)==STATUS_MEDIA_WRITE_PROTECTED && sector_reads==reads);
 s.AppPrefixAccepted=0;T214Write(s.regs,0x24,3);assert(T214ValidateHibernateTarget(&s)==STATUS_DEVICE_BUSY);
 assert(error_resets==5);
 puts("PASS: S4 polled identity, mismatched card/volume, timeout/CRC/busy paths, WINARM-only dump writes and 16 KiB DMA");
}
'''
with tempfile.TemporaryDirectory(prefix='lite-s4-') as temp:
    p = Path(temp)
    (p / 'test.c').write_text(code)
    subprocess.run(['clang', '-std=c11', '-fsanitize=address,undefined', '-g',
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
