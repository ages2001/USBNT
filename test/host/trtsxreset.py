"""Production card-reader/SD reset coordination, with fault-injected hardware.
Register/SD protocol behavior is tested separately by trtsxreset.py in QEMU.
"""
from pathlib import Path
import re,subprocess,tempfile
src=Path(__file__).resolve().parents[2]/'src'
sd=(src/'sdcard.c').read_text();crw=(src/'rtsxusb.c').read_text()
def function(s,name):
    m=re.search(r'(?:static )?(?:int|void|ULONG|BOOLEAN) '+name+r'\([^;{]*\)\s*\{',s);assert m,name
    end=m.end();depth=1
    while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[m.start():end]+'\n'
def structure(s,start,end):
    a=s.index(start);b=s.index(end,a)+len(end);return s[a:b]+'\n'
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usbnt.h"
#include "usbscsi.h"
#include "usbblk.h"
#include "sdcard.h"
'''
structs='\n'.join(line for line in sd.splitlines() if line.startswith('#define SENSE_'))+'\n'+structure(sd,'struct _SD_CARD {','\n};')+structure(crw,'typedef struct _RTSX_USB {','} RTSX_USB;')
mocks=r'''
#define MAX_READERS 4
static RTSX_USB *Readers[MAX_READERS];
static unsigned step,fault,notifications,present,ios_calls,rw_calls;
static int check(void){return ++step==fault?SD_EIO:SD_OK;}
static int InitChip(RTSX_USB*u){assert(u->Resetting&&u->Out.Opened&&u->In.Opened);return check();}
static int RxPowerOff(RTSX_USB*u){(void)u;return check();}
static int CardInit(SD_CARD*c){int r=check();assert(c->Resetting);c->Ready=r==SD_OK;return r;}
static int Rw(SD_CARD*c,ULONG l,ULONG n,UCHAR*b,BOOLEAN w){(void)c;(void)l;(void)n;(void)b;(void)w;rw_calls++;return SD_OK;}
void UsbMemSet(void*p,int c,ULONG n){memset(p,c,n);}
void UsbMemCpy(void*a,const void*b,ULONG n){memcpy(a,b,n);}
void UsbLog(ULONG l,const char*f,...){(void)l;(void)f;}
ULONG OsTimeMs(void){return 1000;}
void OsSleepMs(ULONG n){(void)n;}
void BlkSense(UCHAR*s,UCHAR k,UCHAR a,UCHAR q){s[0]=k;s[1]=a;s[2]=q;}
void MscVirtMediaChange(BLK_TARGET*t){assert(t==(void*)0x1234);notifications++;}
void UsbClosePipe(USB_PIPE*p){p->Opened=0;}
int UsbOpenPipe(USB_DEV*d,USB_PIPE*p,const UCHAR*ep){(void)d;assert(ep&& !p->Opened);p->Opened=1;return check()==SD_OK?USB_OK:USB_ERR_IO;}
int UsbSetInterface(USB_DEV*d,UCHAR i,UCHAR a){(void)d;assert(i==2&&a==1);return check()==SD_OK?USB_OK:USB_ERR_IO;}
static int status(void*ctx,BOOLEAN*p,BOOLEAN*w){(void)ctx;*p=present;*w=0;return check();}
static int ios(void*ctx,BOOLEAN p,ULONG hz,int w,BOOLEAN hs){(void)ctx;(void)hz;(void)w;(void)hs;assert(!p);ios_calls++;return 0;}
'''
body=''.join(function(sd,n) for n in ['Put32','Get32','Copy','Usable','SdScsi','SdPoll','SdBeginReset','SdRestoreReset','SdEndReset'])+''.join(function(crw,n) for n in ['RtsxUsbResetInterfaces','RtsxUsbBeginDeviceReset','RtsxUsbRestoreDevice','RtsxUsbEndDeviceReset'])
main=r'''
static void blocked(SD_CARD*c){UCHAR cdb[16]={0},sense[18],data[512];ULONG done;unsigned op;for(op=0;op<256;op++){cdb[0]=(UCHAR)op;done=99;memset(sense,0,sizeof(sense));if(op==0x12){assert(SdScsi(c,cdb,16,0,sizeof(data),data,&done,sense)==BLK_OK);continue;}assert(SdScsi(c,cdb,16,0,sizeof(data),data,&done,sense)==BLK_FAILED);assert(done==0&&sense[0]==SENSE_NOT_READY&&sense[1]==4&&sense[2]==2);}assert(!rw_calls);{unsigned before=step;SdPoll(c);assert(step==before);}}
int main(void){USB_DEV d;RTSX_USB u;SD_CARD c;SD_HOST_OPS ops={status,ios,NULL};UCHAR ep[7]={7,5};unsigned a,p,n,f,max_steps,cases=0;int result;
 for(a=0;a<2;a++)for(p=0;p<2;p++)for(n=0;n<100;n++){
  memset(&d,0,sizeof(d));memset(&u,0,sizeof(u));memset(&c,0,sizeof(c));memset(Readers,0,sizeof(Readers));u.Dev=&d;u.Iface=2;u.Alt=a;u.EpOut=u.EpIn=ep;u.Card=&c;Readers[0]=&u;c.Ops=&ops;c.Ctx=&u;c.Target=(void*)0x1234;c.Present=present=p;c.Ready=p;c.Blocks=777;fault=step=notifications=rw_calls=ios_calls=0;
  assert(RtsxUsbResetInterfaces(&d)==4);RtsxUsbBeginDeviceReset(&d);blocked(&c);assert(RtsxUsbRestoreDevice(&d)==0);max_steps=step;RtsxUsbEndDeviceReset(&d,0);assert(!u.Resetting&&!c.Resetting&&c.Ready==p);
  for(f=0;f<=max_steps;f++){
   unsigned notices=notifications;RtsxUsbBeginDeviceReset(&d);assert(u.Resetting&&c.Resetting&&!c.Ready&&!u.In.Opened&&!u.Out.Opened);blocked(&c);fault=f;step=0;result=RtsxUsbRestoreDevice(&d);assert((result!=0)==(f!=0));RtsxUsbEndDeviceReset(&d,result);
   if(f){assert(u.Resetting&&c.Resetting&&!c.Ready&&!u.In.Opened&&!u.Out.Opened&&notifications==notices);blocked(&c);RtsxUsbBeginDeviceReset(&d);fault=step=0;assert(RtsxUsbRestoreDevice(&d)==0);RtsxUsbEndDeviceReset(&d,0);}
   assert(!u.Resetting&&!c.Resetting&&c.Ready==p&&u.Card==&c&&c.Target==(void*)0x1234&&c.Blocks==777&&notifications==notices+1);cases++;
  }
  RtsxUsbBeginDeviceReset(&d);d.Gone=1;assert(RtsxUsbRestoreDevice(&d)==USB_ERR_NODEV);RtsxUsbEndDeviceReset(&d,USB_ERR_NODEV);blocked(&c);
 }
 printf("PASS: %u reset/failure/retry schedules; 256-opcode stopped-state admission; no hardware polling after failure; stable target/identity; absent card and removal\n",cases);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='usbnt-rtsx-reset-') as td:
    c=Path(td)/'test.c';exe=Path(td)/'test';c.write_text(prefix+structs+mocks+body+main)
    subprocess.run(['gcc','-std=c11','-O1','-g','-DUSB_HARNESS=1','-fsanitize=address,undefined','-I'+str(src),str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
