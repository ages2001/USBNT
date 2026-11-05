/* Production HID lifetime with callbacks/submissions paused on another CPU.
 * USB scheduling is mocked. The NT kernel and hardware ABI are not modeled. */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_HARNESS 1
#ifndef HID_SOURCE
#define HID_SOURCE "../../src/usbhid.c"
#endif
#include HID_SOURCE

static pthread_mutex_t core = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_cond = PTHREAD_COND_INITIALIZER;
static unsigned blocks, pipes, wait_seen, detached, release_op, entered;
static unsigned pause_input, pause_submit, fail_submit, submissions, reset_mode;
static USB_DEV dev;
static USB_XFER *pending;

static void barrier(void) {
    pthread_mutex_lock(&sync_lock);
    entered = 1; pthread_cond_broadcast(&sync_cond);
    while (!release_op) pthread_cond_wait(&sync_cond, &sync_lock);
    pthread_mutex_unlock(&sync_lock);
}
static void await_entry(void) {
    pthread_mutex_lock(&sync_lock);
    while (!entered) pthread_cond_wait(&sync_cond, &sync_lock);
    pthread_mutex_unlock(&sync_lock);
}
void *OsAlloc(ULONG n) { void *p = calloc(1, n); assert(p); blocks++; return p; }
void OsFree(void *p) { if (p) { assert(blocks); blocks--; free(p); } }
void *UsbDmaAlloc(ULONG n, ULONG *phys) { *phys = 0; return OsAlloc(n); }
void UsbDmaFree(void *p, ULONG n) { (void)n; OsFree(p); }
void UsbLog(ULONG level, const char *fmt, ...) { (void)level; (void)fmt; }
void UsbMemCpy(void *a, const void *b, ULONG n) { memcpy(a, b, n); }
void UsbMemSet(void *p, int c, ULONG n) { memset(p, c, n); }
OS_IRQL OsLock(void) { assert(!pthread_mutex_lock(&core)); return 0; }
void OsUnlock(OS_IRQL old) { (void)old; assert(!pthread_mutex_unlock(&core)); }
ULONG OsTimeMs(void) { return 0; }
void OsSleepMs(ULONG n) {
    (void)n;
    pthread_mutex_lock(&sync_lock);
    wait_seen = 1; pthread_cond_broadcast(&sync_cond);
    while (!release_op) pthread_cond_wait(&sync_cond, &sync_lock);
    pthread_mutex_unlock(&sync_lock);
}
void OsWakeWorker(void) {}
void UsbPollAll(void) {}
void OsKbdInput(const USB_KEY_EVENT *e, ULONG n) {
    (void)e; (void)n;
    if (pause_input) barrier();
}
void OsMouseInput(const USB_MOUSE_EVENT *e) { (void)e; }
int UsbClearHalt(USB_PIPE *p) { p->Halted = 0; return USB_OK; }
const UCHAR *UsbFindDesc(const UCHAR *p, const UCHAR *end, UCHAR type) {
    for (p += p[0]; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
        if (p[1] == USB_DT_INTERFACE) break;
        if (p[1] == type) return p;
    }
    return NULL;
}
int UsbControl(USB_DEV *d, UCHAR type, UCHAR req, USHORT value, USHORT index,
               USHORT len, void *data, ULONG timeout, ULONG *actual) {
    (void)d; (void)type; (void)req; (void)value; (void)index;
    (void)len; (void)data; (void)timeout; (void)actual; return USB_OK;
}
int UsbOpenPipe(USB_DEV *d, USB_PIPE *p, const UCHAR *ep) {
    assert(!p->Opened); p->Opened = 1; p->Dev = d;
    p->MaxPacket = ep[4]; pipes++; return USB_OK;
}
int UsbSubmit(USB_XFER *x) {
    OS_IRQL old;
    if (pause_submit) barrier();
    old = OsLock();
    assert(x->Pipe->Opened && !x->Pipe->Cur);
    submissions++;
    if (fail_submit) { OsUnlock(old); return USB_ERR_IO; }
    x->Pipe->Cur = x;
    OsUnlock(old); return USB_OK;
}
void UsbClosePipe(USB_PIPE *p) {
    USB_XFER *x;
    OS_IRQL old = OsLock();
    x = p->Cur; p->Cur = NULL;
    if (p->Opened) { assert(pipes); pipes--; p->Opened = 0; }
    OsUnlock(old);
    if (x) { x->Status = USB_ERR_ABORTED; x->Complete(x); }
}
static void setup(void) {
    UCHAR desc[] = {9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, 1, 1, 0,
                   7, USB_DT_ENDPOINT, 0x81, USB_EP_INTERRUPT, 8, 0, 10};
    memset(&dev, 0, sizeof(dev));
    wait_seen = detached = release_op = entered = 0;
    pause_input = pause_submit = fail_submit = submissions = 0;
    assert(HidAttach(&dev, desc, desc + sizeof(desc)) == USB_OK);
    pending = ((HID_DEV *)dev.Hid[0])->In.Cur;
    assert(pending);
    /* The poller owns a completed transfer, absent from the hardware queue. */
    pending->Pipe->Cur = NULL;
    pending->Status = USB_OK; pending->Actual = 8;
    memset(pending->Buf, 0, 8); ((UCHAR *)pending->Buf)[2] = 4;
}
static void *finish(void *arg) {
    if (arg) barrier(); /* collected but callback not yet entered */
    pending->Complete(pending); return NULL;
}
static void *detach(void *arg) {
    (void)arg;
#ifndef HID_LEGACY_SOURCE
    if (reset_mode) HidBeginDeviceReset(&dev);
    else
#endif
    HidDetach(&dev);
    pthread_mutex_lock(&sync_lock);
    detached = 1; pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock); return NULL;
}
static void release_when_waiting(unsigned must_keep_pipe) {
    pthread_mutex_lock(&sync_lock);
    while (!wait_seen && !detached) pthread_cond_wait(&sync_cond, &sync_lock);
    /* Old code frees here while the other CPU still owns h / Xfer / Buf. */
    assert(wait_seen && !detached && blocks == 2);
    if (must_keep_pipe) assert(pipes == 1);
    release_op = 1; pthread_cond_broadcast(&sync_cond);
    pthread_mutex_unlock(&sync_lock);
}
int main(void) {
    unsigned n, mode, i;
#ifdef HID_LEGACY_SOURCE
    const unsigned modes = 4;
#else
    const unsigned modes = 8;
#endif
    pthread_t completion, removal;
    for (n = 0; n < 1000; n++) for (mode = 0; mode < modes; mode++) {
        setup(); reset_mode = mode >= 4;
        pause_input = (mode % 4) == 1; pause_submit = (mode % 4) >= 2;
        fail_submit = (mode % 4) == 3;
        assert(!pthread_create(&completion, NULL, finish, (mode % 4) == 0 ? &dev : NULL));
        await_entry();
        if ((mode % 4) == 1) {
            /* Worker service must not reuse Xfer while its callback reads it. */
            HidService(); assert(submissions == 1);
        }
        assert(!pthread_create(&removal, NULL, detach, NULL));
        release_when_waiting((mode % 4) >= 2);
        assert(!pthread_join(completion, NULL));
        assert(!pthread_join(removal, NULL));
#ifndef HID_LEGACY_SOURCE
        if (reset_mode) {
            HID_DEV *h = dev.Hid[0];
            assert(h && h->Paused && !h->Gone && !h->Active && !pipes && blocks==2);
            HidDetach(&dev);
        }
#endif
        assert(!blocks && !pipes && !dev.Hid[0]);
        for (i = 0; i < HID_MAX; i++) assert(!HidDevs[i]);
    }
    puts("RESULT hid-rundown PASS: 8000 removal/reset delayed-completion, active-callback, concurrent-submit and rejected-submit schedules; worker rearm exclusion; no leaks");
    return 0;
}
