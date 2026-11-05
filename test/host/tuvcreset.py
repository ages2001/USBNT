"""Production camera reset/control/admission with injected restore failures.
Streaming and hardware are mocked here; native QEMU tests cover the real paths.
"""
from pathlib import Path
import re,subprocess,tempfile
src=Path(__file__).resolve().parents[2]/'src';s=(src/'usbvideo.c').read_text()
def function(name):
 m=re.search(r'(?:static )?(?:int|void|ULONG|LONG|UVC_DEV \*)\s*'+name+r'\([^;{]*\)\s*\{',s);assert m,name
 end=m.end();depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[m.start():end]+'\n'
def part(start,end):
 a=s.index(start);b=s.index(end,a)+len(end);return s[a:b]+'\n'
prefix=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usbnt.h"
#include "usbntioc.h"
#include "usbvideo.h"
'''
types=part('#define CC_VIDEO','} UVC_DEV;')+part('typedef struct _UVC_REQ','} UVC_REQ;')
mocks=r'''
static UVC_DEV *Cams[MAX_CAMS];
static unsigned step,fault,locked,stops,starts;
static LONG hw[32];
static int check(void){return ++step==fault?USB_ERR_IO:USB_OK;}
OS_IRQL OsLock(void){assert(!locked);locked=1;return 0;}
void OsUnlock(OS_IRQL i){(void)i;assert(locked);locked=0;}
void OsWakeWorker(void){assert(!locked);}
void UsbMemSet(void*p,int c,ULONG n){memset(p,c,n);}
static void StopNow(UVC_DEV*u){assert(u->Resetting||u->Gone);u->Stopping=1;u->Running=0;stops++;}
static int StartNow(UVC_DEV*u,const USBNT_VID_START*in,USBNT_VID_STATUS*out){int r=check();(void)out;assert(u->Resetting&&u->Resume&&in==&u->Request&&in->Format==1&&in->Frame==2&&in->Interval==666666);starts++;if(r==0){u->Running=1;u->Stopping=0;}return r;}
int UsbControl(USB_DEV*d,UCHAR type,UCHAR req,USHORT val,USHORT idx,USHORT len,void*buf,ULONG timeout,ULONG*actual){UCHAR*b=buf;unsigned cs=val>>8;(void)d;(void)idx;(void)timeout;(void)type;assert(cs<32);if(req==SET_CUR){int r=check();if(r)return r;hw[cs]=(SHORT)(b[0]|(b[1]<<8));}else{b[0]=(UCHAR)hw[cs];b[1]=(UCHAR)(hw[cs]>>8);}if(actual)*actual=len;return 0;}
static LONG GetFrame(UVC_DEV*u,const USBNT_VID_GET*g,UCHAR*out,ULONG size){(void)u;(void)g;(void)out;(void)size;assert(0);return -1;}
'''
body=''.join(function(n) for n in ['Find','ControlNow','Execute','UvcGetFrame','UvcResetInterfaces','UvcBeginDeviceReset','UvcRestoreDevice','UvcEndDeviceReset'])
main=r'''
static void blocked(UVC_DEV*u){UVC_REQ c;USBNT_VID_GET g;UCHAR out[128];memset(&c,0,sizeof(c));memset(&g,0,sizeof(g));c.Id=g.Id=u->Id;c.Op=1;assert(Execute(&c)==USB_ERR_BUSY);c.Op=3;assert(Execute(&c)==USB_ERR_BUSY);assert(UvcGetFrame(&g,out,sizeof(out))==USB_ERR_NODEV);assert(!u->Readers);}
int main(void){USB_DEV d,sibling;UVC_DEV cams[3];unsigned variant,n,f,i,max_steps,cases=0;int result;
 for(variant=0;variant<4;variant++)for(n=0;n<100;n++){
  memset(&d,0,sizeof(d));memset(&sibling,0,sizeof(sibling));memset(cams,0,sizeof(cams));memset(Cams,0,sizeof(Cams));
  for(i=0;i<3;i++){UVC_DEV*u=&cams[i];Cams[i]=u;u->Dev=i==2?&sibling:&d;u->Id=i+1;u->VcIf=i*2;u->VsIf=i*2+1;u->Running=i==2?1:(variant>>i)&1;u->PuId=1;u->Pub.Controls=u->ControlSet=(1<<2)|(1<<3);u->ControlValue[2]=40;u->ControlValue[3]=22;u->Seq=1000+i;u->Request.Format=1;u->Request.Frame=2;u->Request.Interval=666666;}
  step=fault=stops=starts=0;assert(UvcResetInterfaces(&d)==15);UvcBeginDeviceReset(&d);for(i=0;i<2;i++)blocked(&cams[i]);assert(UvcRestoreDevice(&d)==0);max_steps=step;UvcEndDeviceReset(&d,0);
  for(f=0;f<=max_steps;f++){
   UvcBeginDeviceReset(&d);for(i=0;i<2;i++){assert(cams[i].Resetting&&!cams[i].Running);blocked(&cams[i]);}
   memset(hw,0,sizeof(hw));step=0;fault=f;result=UvcRestoreDevice(&d);assert((result!=0)==(f!=0));UvcEndDeviceReset(&d,result);
   if(f){for(i=0;i<2;i++){assert(cams[i].Resetting&&cams[i].Stopping&&!cams[i].Running);blocked(&cams[i]);}UvcBeginDeviceReset(&d);fault=step=0;assert(UvcRestoreDevice(&d)==0);UvcEndDeviceReset(&d,0);}
   for(i=0;i<2;i++)assert(!cams[i].Resetting&&!cams[i].Resume&&cams[i].Running==((variant>>i)&1)&&Cams[i]==&cams[i]&&cams[i].Id==i+1&&cams[i].Seq==1000+i);
   assert(cams[2].Running&&!cams[2].Resetting&&cams[2].Seq==1002&&hw[2]==40&&hw[3]==22);cases++;
  }
  UvcBeginDeviceReset(&d);{UVC_REQ close;memset(&close,0,sizeof(close));close.Op=2;close.Id=1;assert(Execute(&close)==0&&!cams[0].Resume);}
  d.Gone=1;assert(UvcRestoreDevice(&d)==USB_ERR_NODEV);UvcEndDeviceReset(&d,USB_ERR_NODEV);
  {USBNT_VID_CONTROL bad;unsigned before=step;memset(&bad,0,sizeof(bad));bad.Selector=32;assert(ControlNow(&cams[0],&bad)==USB_ERR_PARAM);bad.Selector=(ULONG)-1;assert(ControlNow(&cams[0],&bad)==USB_ERR_PARAM);assert(step==before);}
 }
 printf("PASS: %u multi-camera control/stream restore fault and retry schedules; stable IDs/sequences; stopped-state admission; unrelated camera preserved\n",cases);return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='usbnt-uvc-reset-') as td:
 c=Path(td)/'test.c';exe=Path(td)/'test';c.write_text(prefix+types+mocks+body+main)
 subprocess.run(['gcc','-std=c11','-O1','-g','-DUSB_HARNESS=1','-fsanitize=address,undefined','-I'+str(src),str(c),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
