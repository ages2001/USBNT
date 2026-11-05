"""Exercise production synchronous-transfer callback lifetime under ASan/UBSan."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = Path(os.environ.get('USB_SYNC_SOURCE', root / 'src/usbcore.c')).read_text()
def function(name):
    m = re.search(r'static (?:void|int) '+name+r'\([^;{]*\)\s*\{', source)
    assert m
    end = m.end(); depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[m.start():end]
prefix = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
typedef unsigned long ULONG;
typedef int OS_IRQL;
typedef struct { int Signaled; } OS_EVENT;
typedef struct { int Type; } USB_PIPE;
typedef struct { void *Context; USB_PIPE *Pipe; int Busy, Status; } USB_XFER;
#define USB_EP_CONTROL 0
#define USB_OK 0
#define USB_ERR_BUSY -1
#define USB_ERR_ABORTED -2
#define USB_ERR_TIMEOUT -3
#define USB_ERR_IO -4
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER, gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int mode, reached, release_callback, returned, wait_calls, result, submit_error;
static USB_XFER x; static USB_PIPE pipe_; static OS_EVENT event;
static pthread_t callback_thread;
static OS_IRQL OsLock(void) { assert(!pthread_mutex_lock(&lock)); return 0; }
static void OsUnlock(OS_IRQL i) { (void)i; assert(!pthread_mutex_unlock(&lock)); }
static void OsSleepMs(ULONG ms) { struct timespec t={0,(long)ms*1000000}; nanosleep(&t,0); }
static ULONG OsTimeMs(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000; }
static void pause_callback(void) {
    pthread_mutex_lock(&gate);reached=1;pthread_cond_broadcast(&changed);
    while(!release_callback)pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
}
static void OsEventReset(OS_EVENT *e) { pthread_mutex_lock(&gate);e->Signaled=0;pthread_mutex_unlock(&gate); }
static void OsEventSet(OS_EVENT *e) {
    pthread_mutex_lock(&gate);e->Signaled=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
    if(mode==0)pause_callback(); /* event signaled, final event access still pending */
    pthread_mutex_lock(&gate);assert(e->Signaled);pthread_mutex_unlock(&gate);
}
static int OsEventWait(OS_EVENT *e,ULONG ms) {
    int ready;(void)ms;
    pthread_mutex_lock(&gate);wait_calls++;
    if(mode==1){pthread_mutex_unlock(&gate);return 0;}
    while(!e->Signaled)pthread_cond_wait(&changed,&gate);
    ready=e->Signaled;pthread_mutex_unlock(&gate);return ready;
}
static void UsbPollAll(void) {}
static void SyncDone(USB_XFER *);
static void *callback(void *unused) {
    OS_IRQL i;(void)unused;
    i=OsLock();x.Busy=0;x.Status=mode==1?USB_ERR_ABORTED:USB_OK;OsUnlock(i);
    if(mode==1)pause_callback(); /* collected on another CPU after cancellation */
    SyncDone(&x);return 0;
}
static int UsbSubmit(USB_XFER *p) {
    if(submit_error)return USB_ERR_IO;
    p->Busy=1;assert(!pthread_create(&callback_thread,0,callback,0));return USB_OK;
}
static void UsbCancelPipe(USB_PIPE *p) {
    (void)p;
    pthread_mutex_lock(&gate);while(!reached)pthread_cond_wait(&changed,&gate);pthread_mutex_unlock(&gate);
}
'''
context = ''
m = re.search(r'typedef struct _SYNC_CONTEXT\s*\{.*?\}\s*SYNC_CONTEXT;', source, re.S)
if m: context = m.group()
suffix = r'''
static void *caller(void *unused) {
    (void)unused;result=XferSync(&x,1);
    pthread_mutex_lock(&gate);returned=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
    return 0;
}
int main(void) {
    unsigned cycle;pthread_t thread;
    for(mode=0;mode<2;mode++)for(cycle=0;cycle<100;cycle++){
        struct timespec deadline;int early;
        reached=release_callback=returned=wait_calls=submit_error=0;
        event.Signaled=0;x.Context=&event;x.Pipe=&pipe_;x.Busy=0;
        assert(!pthread_create(&thread,0,caller,0));
        pthread_mutex_lock(&gate);while(!reached)pthread_cond_wait(&changed,&gate);
        clock_gettime(CLOCK_REALTIME,&deadline);deadline.tv_nsec+=10000000;
        if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
        while(!returned){int rc=pthread_cond_timedwait(&changed,&gate,&deadline);if(rc==ETIMEDOUT)break;assert(!rc);}
        early=returned;release_callback=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
        pthread_join(callback_thread,0);pthread_join(thread,0);
        if(early){fprintf(stderr,"FAIL: synchronous caller returned while callback still owns event/transfer (mode %d)\n",mode);return 1;}
        assert(x.Context==&event && result==(mode?USB_ERR_TIMEOUT:USB_OK));
    }
    for(cycle=0;cycle<100;cycle++){
        submit_error=1;x.Context=&event;x.Pipe=&pipe_;
        assert(XferSync(&x,1)==USB_ERR_IO && x.Context==&event);
    }
    puts("PASS: 200 delayed callback/event/timeout schedules and 100 submission failures; caller context restored; no early return");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='usbnt-sync-rundown-') as td:
    p = Path(td)
    (p/'test.c').write_text(prefix+context+'\n'+function('SyncDone')+'\n'+function('XferSync')+suffix)
    subprocess.run(['gcc','-std=c11','-O1','-g','-pthread','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
