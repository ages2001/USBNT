/* Production UVC teardown against collected callbacks, submitters and readers.
 * The HCD is mocked; rendezvous points enforce each interleaving. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_HARNESS 1
#ifndef UVC_SOURCE
#define UVC_SOURCE "../../src/usbvideo.c"
#endif
#include UVC_SOURCE
static pthread_mutex_t core=PTHREAD_MUTEX_INITIALIZER, sync_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static unsigned blocks,entered,released,waiting,stopped,mode,submit_calls;
static _Thread_local unsigned actor,paused;
static USB_DEV dev;
static UVC_DEV *cam;
static USB_XFER *pending;
static void barrier(void){pthread_mutex_lock(&sync_lock);entered=1;pthread_cond_broadcast(&cond);while(!released)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);}
void *OsAlloc(ULONG n){void*p=calloc(1,n);assert(p);blocks++;return p;}
void OsFree(void*p){if(p){assert(blocks);blocks--;free(p);}}
void *OsDmaAlloc(ULONG n,ULONG*p){*p=0;return OsAlloc(n);}
void OsDmaFree(void*p,ULONG n){(void)n;OsFree(p);}
void UsbMemSet(void*p,int c,ULONG n){memset(p,c,n);}
void UsbMemCpy(void*a,const void*b,ULONG n){if(actor&&mode>=4&&!paused){paused=1;barrier();}memcpy(a,b,n);}
void UsbLog(ULONG l,const char*f,...){(void)l;(void)f;}
OS_IRQL OsLock(void){assert(!pthread_mutex_lock(&core));return 0;}
void OsUnlock(OS_IRQL i){unsigned delay=actor&&(mode==1||mode==7)&&!paused&&cam->Pending==0;(void)i;if(delay)paused=1;assert(!pthread_mutex_unlock(&core));if(delay)barrier();}
void OsSleepMs(ULONG n){(void)n;pthread_mutex_lock(&sync_lock);waiting=1;pthread_cond_broadcast(&cond);while(!released)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);}
ULONG OsTimeMs(void){return 1;}
void OsWakeWorker(void){}
void UsbPollAll(void){}
void OsEventSet(OS_EVENT*e){(void)e;}
void OsEventReset(OS_EVENT*e){(void)e;}
BOOLEAN OsEventWait(OS_EVENT*e,ULONG t){(void)e;(void)t;return TRUE;}
int UsbSetInterface(USB_DEV*d,UCHAR i,UCHAR a){(void)d;(void)i;(void)a;return USB_OK;}
int UsbSubmit(USB_XFER*x){submit_calls++;if(actor&&(mode==2||mode==3))barrier();assert(x->Pipe->Opened&&!x->Pipe->Cur);if(mode==3)return USB_ERR_IO;x->Pipe->Cur=x;return USB_OK;}
void UsbCancelPipe(USB_PIPE*p){USB_XFER*x=p->Cur;p->Cur=NULL;if(x){x->Status=USB_ERR_ABORTED;x->Complete(x);}}
void UsbClosePipe(USB_PIPE*p){UsbCancelPipe(p);p->Opened=0;}
static void setup(void){unsigned i;memset(&dev,0,sizeof(dev));memset(Cams,0,sizeof(Cams));entered=released=waiting=stopped=submit_calls=0;cam=OsAlloc(sizeof(*cam));Cams[0]=cam;cam->Dev=&dev;cam->Id=1;cam->Running=1;cam->Pipe.Opened=1;cam->X[0].U=cam;cam->X[0].Size=64;cam->X[0].Buf=OsAlloc(64);cam->FrameAlloc=64;for(i=0;i<3;i++)cam->Frame[i]=OsAlloc(64);pending=&cam->X[0].X;pending->Context=&cam->X[0];pending->Status=USB_ERR_ABORTED;pending->Complete=XferDone;pending->Pipe=&cam->Pipe;cam->Fill=0;cam->Ready=1;cam->Read=2;cam->ReadySeq=1;cam->ReadyLen=16;memset(cam->Frame[1],0xA5,16);if(mode<2||mode>=6){cam->Pending=1;cam->X[0].Busy=1;}}
static void *operate(void*unused){(void)unused;actor=1;if(mode==0||mode==6)barrier();if(mode<2||mode>=6)XferDone(pending);else if(mode<4)Submit(&cam->X[0]);else{USBNT_VID_GET get;UCHAR out[128];memset(&get,0,sizeof(get));get.Id=1;assert(UvcGetFrame(&get,out,sizeof(out))==(LONG)(sizeof(USBNT_VID_FRAME_HDR)+16));assert(out[sizeof(USBNT_VID_FRAME_HDR)]==0xA5);}return NULL;}
static void *retire(void*unused){(void)unused;if(mode>=6)UvcBeginDeviceReset(&dev);else if(mode==5)StopNow(cam);else UvcDetach(&dev);pthread_mutex_lock(&sync_lock);stopped=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);return NULL;}
int main(void){unsigned n;pthread_t a,b;for(n=0;n<500;n++)for(mode=0;mode<8;mode++){setup();assert(!pthread_create(&a,NULL,operate,NULL));pthread_mutex_lock(&sync_lock);while(!entered)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);assert(!pthread_create(&b,NULL,retire,NULL));pthread_mutex_lock(&sync_lock);while(!waiting&&!stopped)pthread_cond_wait(&cond,&sync_lock);assert(waiting&&!stopped&&blocks==5);released=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);pthread_join(a,NULL);pthread_join(b,NULL);if(mode>=5){assert(blocks==1&&cam->Stopping&&!cam->Running);if(mode>=6)assert(cam->Resetting&&cam->Resume&&Cams[0]==cam);UvcDetach(&dev);}assert(blocks==0&&Cams[0]==NULL);assert(submit_calls==(mode==2||mode==3));}puts("PASS: 4000 UVC callback/submit/reader stop, removal and physical-reset rundown schedules; no premature free or late rearm");return 0;}
