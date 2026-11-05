/* Production audio/MIDI lifecycle, with deterministic cross-CPU rendezvous. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_HARNESS 1
#ifndef UAC_SOURCE
#define UAC_SOURCE "../../src/usbaudio.c"
#endif
#include UAC_SOURCE
static pthread_mutex_t core=PTHREAD_MUTEX_INITIALIZER, sync_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static unsigned blocks,entered,released,waiting,stopped,mode,submit_calls;
static _Thread_local unsigned actor,paused;
static unsigned reset_variant;
#ifdef AUDIO_OPEN_TEST
static unsigned alloc_step,alloc_fault;
#endif
static USB_DEV dev;
static UA_DEV *audio_dev;
static UA_STREAM *stream;
static USB_XFER *pending;
ULONG UsbDebugLevel;
static void barrier(void){pthread_mutex_lock(&sync_lock);entered=1;pthread_cond_broadcast(&cond);while(!released)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);}
void *OsAlloc(ULONG n){void*p;
#ifdef AUDIO_OPEN_TEST
if(++alloc_step==alloc_fault)return NULL;
#endif
p=calloc(1,n);assert(p);blocks++;return p;}
void OsFree(void*p){if(p){assert(blocks);blocks--;free(p);}}
void *OsDmaAlloc(ULONG n,ULONG*p){*p=0;return OsAlloc(n);}
void OsDmaFree(void*p,ULONG n){(void)n;OsFree(p);}
void *UsbDmaAlloc(ULONG n,ULONG*p){return OsDmaAlloc(n,p);}
void UsbDmaFree(void*p,ULONG n){OsDmaFree(p,n);}
void UsbMemSet(void*p,int c,ULONG n){memset(p,c,n);}
void UsbMemCpy(void*a,const void*b,ULONG n){memcpy(a,b,n);}
void UsbLog(ULONG l,const char*f,...){(void)l;(void)f;}
void PUT32(void*p,ULONG v){UCHAR*b=p;unsigned i;for(i=0;i<4;i++)b[i]=(UCHAR)(v>>(i*8));}
OS_IRQL OsLock(void){assert(!pthread_mutex_lock(&core));return 0;}
void OsUnlock(OS_IRQL i){unsigned delay=actor&&!paused&&((mode==1&&stream->Pending==0)||(mode==7&&!audio_dev->Midi->Busy));(void)i;if(delay)paused=1;assert(!pthread_mutex_unlock(&core));if(delay)barrier();}
void OsSleepMs(ULONG n){(void)n;pthread_mutex_lock(&sync_lock);waiting=1;pthread_cond_broadcast(&cond);while(!released)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);}
ULONG OsTimeMs(void){return 1;}
void OsWakeWorker(void){}
void UsbPollAll(void){}
#ifndef AUDIO_OPEN_TEST
int UsbSetInterface(USB_DEV*d,UCHAR i,UCHAR a){(void)d;(void)i;(void)a;return USB_OK;}
#endif
#ifndef AUDIO_OPEN_TEST
int UsbSubmit(USB_XFER*x){submit_calls++;if(actor&&(mode==2||mode==3||mode==5||mode==8||mode==9))barrier();assert(x->Pipe->Opened&&!x->Pipe->Cur);if(mode==3||mode==9)return USB_ERR_IO;x->Pipe->Cur=x;return USB_OK;}
void UsbCancelPipe(USB_PIPE*p){USB_XFER*x=p->Cur;p->Cur=NULL;if(x){x->Status=USB_ERR_ABORTED;x->Complete(x);}}
#endif
void UsbClosePipe(USB_PIPE*p){UsbCancelPipe(p);p->Opened=0;}
static void UA_CALL fill(void*c,UCHAR*b,ULONG n){(void)c;(void)b;(void)n;if(actor&&mode==4)barrier();}
static void UA_CALL removed(void*c,ULONG id){(void)c;assert(id==1&&released);assert(!stream->Pending);}
static void setup(void){UA_XFER*x;memset(&dev,0,sizeof(dev));memset(UaDevs,0,sizeof(UaDevs));entered=released=waiting=stopped=submit_calls=0;audio_dev=OsAlloc(sizeof(*audio_dev));UaDevs[0]=audio_dev;dev.Audio=audio_dev;audio_dev->Dev=&dev;audio_dev->Id=1;audio_dev->Notified=UaClientSet=1;UaClient.Removal=removed;stream=&audio_dev->S[0];stream->U=audio_dev;stream->Open=stream->Running=1;stream->Pipe.Opened=1;stream->Alt=&audio_dev->Alts[0][0];stream->NX=stream->NXAlloc=1;stream->FrameBytes=2;stream->MaxFrames=1;stream->Slots=8;stream->Current=65536;stream->Ops.Fill=fill;x=&stream->Xf[0];x->S=stream;x->Size=64;x->Buf=OsAlloc(64);x->NPkt=1;x->Pkt=OsAlloc(sizeof(*x->Pkt));pending=&x->X;pending->Context=x;pending->Status=USB_ERR_ABORTED;pending->Complete=XferDone;pending->Pipe=&stream->Pipe;if(mode<2){stream->Pending=1;x->Busy=1;}if(mode>=6){UA_MIDI*m=OsAlloc(sizeof(*m));audio_dev->Midi=m;m->Buf=OsAlloc(512);m->Opened=m->Pipe.Opened=1;m->Head=1;m->Ring[0]=0x007F3C09;m->Pipe.MaxPacket=64;pending=&m->X;pending->Context=audio_dev;pending->Status=USB_ERR_ABORTED;pending->Complete=MidiDone;pending->Pipe=&m->Pipe;if(mode<8)m->Busy=1;}}
static void *operate(void*unused){(void)unused;actor=1;if(mode==0||mode==6)barrier();if(mode<2)XferDone(pending);else if(mode==5)UaStart(1,0);else if(mode<6)Submit(&stream->Xf[0]);else if(mode<8)MidiDone(pending);else{UCHAR note[]={0x90,60,127};UaMidiOut(1,0,note,sizeof(note));}return NULL;}
static void *retire(void*unused){(void)unused;if(reset_variant)AudioBeginDeviceReset(&dev);else AudioDetach(&dev);pthread_mutex_lock(&sync_lock);stopped=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);return NULL;}
int main(void){unsigned n;pthread_t a,b;for(reset_variant=0;reset_variant<2;reset_variant++)for(n=0;n<500;n++)for(mode=0;mode<10;mode++){setup();assert(!pthread_create(&a,NULL,operate,NULL));pthread_mutex_lock(&sync_lock);while(!entered)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);assert(!pthread_create(&b,NULL,retire,NULL));pthread_mutex_lock(&sync_lock);while(!waiting&&!stopped)pthread_cond_wait(&cond,&sync_lock);assert(waiting&&!stopped);assert(blocks==3);/* MIDI teardown has already freed the idle stream buffers. */released=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);pthread_join(a,NULL);pthread_join(b,NULL);if(reset_variant){assert(audio_dev->Resetting&&dev.Audio==audio_dev&&UaDevs[0]==audio_dev&&!stream->Open);AudioDetach(&dev);}assert(blocks==0&&dev.Audio==NULL&&UaDevs[0]==NULL);assert(submit_calls==((mode>=2&&mode<=5)||mode>=8));}puts("PASS: 10000 audio/MIDI callback, fill, submit, start and writer removal/reset schedules; no premature free or late rearm");return 0;}
