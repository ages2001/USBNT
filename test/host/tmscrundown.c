/* Production MSC callbacks and teardown; transport and NT ABI are mocked. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_HARNESS 1
#ifndef MSC_SOURCE
#define MSC_SOURCE "../../src/usbmsc.c"
#endif
#include MSC_SOURCE
static pthread_mutex_t core=PTHREAD_MUTEX_INITIALIZER, sync_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;
static unsigned blocks, entered, released, waiting, stopped, completions, pause_client, pause_submit;
static unsigned pause_unlock;
static _Thread_local unsigned callback_thread;
static USB_DEV dev;
static MSC_DEV *m;
static SCSI_REQ req;
static USB_XFER *pending;
#ifndef MSC_LEGACY_SOURCE
static BOT_Q q;
#endif
static void barrier(void) {
    pthread_mutex_lock(&sync_lock); entered=1; pthread_cond_broadcast(&cond);
    while(!released) pthread_cond_wait(&cond,&sync_lock);
    pthread_mutex_unlock(&sync_lock);
}
void *OsAlloc(ULONG n){void*p=calloc(1,n);assert(p);blocks++;return p;}
void OsFree(void*p){if(p){assert(blocks);blocks--;free(p);}}
void *OsDmaAlloc(ULONG n,ULONG*p){*p=0;return OsAlloc(n);}
void OsDmaFree(void*p,ULONG n){(void)n;OsFree(p);}
void *UsbDmaAlloc(ULONG n,ULONG*p){return OsDmaAlloc(n,p);}
void UsbDmaFree(void*p,ULONG n){OsDmaFree(p,n);}
void PUT32(void*p,ULONG v){UCHAR*b=p;unsigned i;for(i=0;i<4;i++)b[i]=(UCHAR)(v>>(i*8));}
void PUTBE16(void*p,USHORT v){UCHAR*b=p;b[0]=(UCHAR)(v>>8);b[1]=(UCHAR)v;}
void UsbMemSet(void*p,int c,ULONG n){memset(p,c,n);}
void UsbMemCpy(void*a,const void*b,ULONG n){memcpy(a,b,n);}
void UsbLog(ULONG l,const char*f,...){(void)l;(void)f;}
OS_IRQL OsLock(void){assert(!pthread_mutex_lock(&core));return 0;}
void OsUnlock(OS_IRQL i){(void)i;assert(!pthread_mutex_unlock(&core));if(callback_thread&&pause_unlock){pause_unlock=0;barrier();}}
void OsSleepMs(ULONG n){(void)n;pthread_mutex_lock(&sync_lock);waiting=1;pthread_cond_broadcast(&cond);while(!released)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);}
ULONG OsTimeMs(void){return 0;}
ULONG OsGetConfig(const char*n,ULONG d){(void)n;return d;}
void OsWakeWorker(void){}
void UsbPollAll(void){}
void OsEventInit(OS_EVENT*e){memset(e,0,sizeof(*e));}
void OsEventSet(OS_EVENT*e){(void)e;}
void OsEventReset(OS_EVENT*e){(void)e;}
BOOLEAN OsEventWait(OS_EVENT*e,ULONG t){(void)e;(void)t;return TRUE;}
void OsScsiMediaChange(int slot){(void)slot;}
int UsbSubmit(USB_XFER*x){if(pause_submit)barrier();assert(!x->Pipe->Cur);x->Pipe->Cur=x;return USB_OK;}
void UsbCancelPipe(USB_PIPE*p){USB_XFER*x=p->Cur;p->Cur=NULL;if(x){x->Status=USB_ERR_ABORTED;x->Complete(x);}}
#ifndef MSC_RESET_TEST
void UsbClosePipe(USB_PIPE*p){UsbCancelPipe(p);p->Opened=0;}
#endif
int UsbClearHalt(USB_PIPE*p){(void)p;return USB_OK;}
int UsbTransfer(USB_PIPE*p,void*b,ULONG ph,ULONG n,ULONG t,ULONG*a){(void)p;(void)b;(void)ph;(void)n;(void)t;*a=0;return USB_ERR_IO;}
int UsbTransferPages(USB_PIPE*p,ULONG*f,ULONG o,ULONG n,ULONG t,ULONG*a){(void)f;(void)o;return UsbTransfer(p,NULL,0,n,t,a);}
#ifndef MSC_RESET_TEST
int UsbControl(USB_DEV*d,UCHAR rt,UCHAR r,USHORT v,USHORT i,USHORT n,void*b,ULONG t,ULONG*a){(void)d;(void)rt;(void)r;(void)v;(void)i;(void)n;(void)b;(void)t;if(a)*a=0;return USB_ERR_IO;}
#endif
static void client_done(SCSI_REQ*r){(void)r;completions++;if(pause_client)barrier();}
static void setup(void){
    memset(&dev,0,sizeof(dev));memset(Slots,0,sizeof(Slots));memset(&req,0,sizeof(req));
    entered=released=waiting=stopped=completions=pause_client=pause_submit=pause_unlock=0;
    m=OsAlloc(sizeof(*m));dev.Msc=m;m->Dev=&dev;m->Proto=MSC_PROTO_UAS;m->NumTags=1;
    m->Cbw=UsbDmaAlloc(64,&m->CbwPhys);m->Buf=OsDmaAlloc(MSC_BUF_SIZE,&m->BufPhys);
    Slots[0].Msc=m;Slots[0].Present=1;
    m->Tags[0].Msc=m;m->Tags[0].Busy=1;m->Tags[0].Left=1;m->Tags[0].Tag=1;
    m->Tags[0].Req=&req;m->Tags[0].Slot=&Slots[0];UasBusy=1;
    m->Tags[0].Iu=UsbDmaAlloc(UAS_IU_SIZE,&m->Tags[0].IuPhys);
    m->Tags[0].Iu[UAS_STAT_OFF]=IU_SENSE;PUTBE16(m->Tags[0].Iu+UAS_STAT_OFF+2,1);
    pending=&m->Tags[0].Stat;pending->Context=&m->Tags[0];pending->Status=USB_OK;pending->Actual=16;
    pending->Complete=UasXferDone;req.Complete=client_done;
}
static void *finish(void*delay){callback_thread=1;if(delay)barrier();pending->Complete(pending);return NULL;}
static void *remove_dev(void*unused){(void)unused;MscDetach(&dev);pthread_mutex_lock(&sync_lock);stopped=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);return NULL;}
static void await_entry(void){pthread_mutex_lock(&sync_lock);while(!entered)pthread_cond_wait(&cond,&sync_lock);pthread_mutex_unlock(&sync_lock);}
static void release_after_wait(void){pthread_mutex_lock(&sync_lock);while(!waiting&&!stopped)pthread_cond_wait(&cond,&sync_lock);assert(waiting&&!stopped&&blocks==4);released=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);}
#ifndef MSC_LEGACY_SOURCE
static void *bot_done(void*delay){if(delay)barrier();BotStageDone(&q.Data);return NULL;}
static void *bot_drain(void*unused){(void)unused;BotQDrain(&q,m);pthread_mutex_lock(&sync_lock);stopped=1;pthread_cond_broadcast(&cond);pthread_mutex_unlock(&sync_lock);return NULL;}
#endif
int main(void){unsigned n,mode;pthread_t a,b;
    for(n=0;n<1000;n++)for(mode=0;mode<3;mode++){
        setup();pause_client=mode==1;
        if(mode==2){m->Tags[0].Left=2;pause_unlock=1;pending->Status=USB_ERR_IO;}
        assert(!pthread_create(&a,NULL,finish,mode==0?&dev:NULL));await_entry();
        if(mode==2){USB_XFER last;memset(&last,0,sizeof(last));last.Context=&m->Tags[0];last.Status=USB_ERR_ABORTED;UasXferDone(&last);}
#ifndef MSC_LEGACY_SOURCE
        if(mode) assert(UasAllocTag(m,FALSE)==NULL);
#endif
        assert(!pthread_create(&b,NULL,remove_dev,NULL));release_after_wait();
        assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
        assert(!blocks&&!dev.Msc&&!UasBusy&&completions==1);
    }
#ifndef MSC_LEGACY_SOURCE
    for(n=0;n<1000;n++)for(mode=0;mode<2;mode++){
        setup();m->Tags[0].Busy=m->Tags[0].Left=0;UasBusy=0;
        memset(&q,0,sizeof(q));q.Pending=1;q.Data.Context=&q;q.Data.Status=USB_OK;
        q.Csw.Context=&q;q.Csw.Pipe=&m->In;q.Csw.Complete=BotCswDone;
        pause_submit=mode==1;
        assert(!pthread_create(&a,NULL,bot_done,mode==0?&dev:NULL));await_entry();
        assert(!pthread_create(&b,NULL,bot_drain,NULL));release_after_wait();
        assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));
        assert(!q.Pending&&!q.Submitting&&!m->In.Cur);
        MscDetach(&dev);assert(!blocks);
    }
#endif
    puts("RESULT msc-rundown PASS: 3000 UAS collected/active/overlapping callbacks; 2000 BOT collected/submitting callbacks; no premature free or tag reuse");
    return 0;
}
