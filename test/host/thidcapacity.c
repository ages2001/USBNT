/* Production HID attach/detach with mock DMA and endpoint ownership.
 * This checks software lifetime, not real USB scheduling or NT ABI layout. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_HARNESS 1
#include "../../src/usbhid.c"

static unsigned blocks, pipes, controls, attempts, fail_alloc, fail_open;
void *OsAlloc(ULONG n) {
    void *p;
    if (++attempts == fail_alloc) return NULL;
    p = calloc(1, n); assert(p); blocks++; return p;
}
void OsFree(void *p) { if (p) { assert(blocks); blocks--; free(p); } }
void *UsbDmaAlloc(ULONG n, ULONG *phys) { *phys = 0; return OsAlloc(n); }
void UsbDmaFree(void *p, ULONG n) { (void)n; OsFree(p); }
void UsbLog(ULONG level, const char *fmt, ...) { (void)level; (void)fmt; }
void UsbMemCpy(void *a, const void *b, ULONG n) { memcpy(a, b, n); }
void UsbMemSet(void *p, int c, ULONG n) { memset(p, c, n); }
OS_IRQL OsLock(void) { return 0; }
void OsUnlock(OS_IRQL irql) { (void)irql; }
ULONG OsTimeMs(void) { return 0; }
void OsSleepMs(ULONG n) { (void)n; }
void OsWakeWorker(void) {}
void UsbPollAll(void) {}
void OsKbdInput(const USB_KEY_EVENT *e, ULONG n) { (void)e; (void)n; }
void OsMouseInput(const USB_MOUSE_EVENT *e) { (void)e; }
int UsbClearHalt(USB_PIPE *p) { p->Halted = 0; return USB_OK; }
const UCHAR *UsbFindDesc(const UCHAR *p, const UCHAR *end, UCHAR type) {
    for (p += p[0]; p + 2 <= end && p[0] >= 2 && p + p[0] <= end; p += p[0]) {
        if (p[1] == USB_DT_INTERFACE) break;
        if (p[1] == type) return p;
    }
    return NULL;
}
int UsbControl(USB_DEV *dev, UCHAR type, UCHAR req, USHORT value, USHORT index,
               USHORT len, void *data, ULONG timeout, ULONG *actual) {
    (void)dev; (void)type; (void)req; (void)value; (void)index;
    (void)len; (void)data; (void)timeout; (void)actual;
    controls++; return USB_OK;
}
int UsbOpenPipe(USB_DEV *dev, USB_PIPE *p, const UCHAR *ep) {
    if (fail_open) return USB_ERR_IO;
    assert(!p->Opened); p->Opened = 1; p->Dev = dev;
    p->MaxPacket = ep[4]; pipes++; return USB_OK;
}
int UsbSubmit(USB_XFER *x) {
    assert(x->Pipe->Opened && !x->Pipe->Cur);
    x->Pipe->Cur = x; return USB_OK;
}
void UsbClosePipe(USB_PIPE *p) {
    USB_XFER *x = p->Cur;
    p->Cur = NULL;
    if (p->Opened) { assert(pipes); pipes--; p->Opened = 0; }
    if (x) { x->Status = USB_ERR_ABORTED; x->Complete(x); }
}
static int attach(USB_DEV *d, unsigned iface) {
    UCHAR desc[] = {9, USB_DT_INTERFACE, 0, 0, 1, USB_CLASS_HID, 1, 1, 0,
                    7, USB_DT_ENDPOINT, 0x81, USB_EP_INTERRUPT, 8, 0, 10};
    desc[2] = (UCHAR)iface;
    desc[11] = (UCHAR)(0x81 + iface);
    return HidAttach(d, desc, desc + sizeof(desc));
}
static void empty(void) {
    unsigned i;
    assert(!blocks && !pipes);
    for (i = 0; i < HID_MAX; i++) assert(!HidDevs[i]);
}
int main(void) {
    USB_DEV dev[5]; unsigned n, i, j, before, c;
    for (n = 0; n < 1000; n++) {
        memset(dev, 0, sizeof(dev));
        for (i = 0; i < 4; i++) assert(attach(&dev[0], i) == USB_OK);
        before = blocks; c = controls;
        /* A fifth interface must never become an untracked live HID_DEV. */
        assert(attach(&dev[0], 4) == USB_ERR_NOMEM);
        assert(blocks == before && controls == c && pipes == 4);
        HidDetach(&dev[0]); empty();
        /* Reach the global limit through four independent physical devices. */
        for (i = 0; i < 4; i++) for (j = 0; j < 4; j++)
            assert(attach(&dev[i], j) == USB_OK);
        before = blocks; c = controls;
        assert(attach(&dev[4], 0) == USB_ERR_NOMEM);
        assert(blocks == before && controls == c && pipes == 16);
        for (i = 0; i < 4; i++) HidDetach(&dev[(i + n) % 4]);
        empty();
    }
    memset(dev, 0, sizeof(dev));
    for (i = 1; i <= 2; i++) {
        attempts = 0; fail_alloc = i;
        assert(attach(dev, 0) == USB_ERR_NOMEM); empty();
    }
    fail_alloc = 0; fail_open = 1;
    assert(attach(dev, 0) == USB_ERR_IO); empty();
    fail_open = 0;
    assert(attach(dev, 0) == USB_OK); HidDetach(dev); empty();
    puts("RESULT hid-capacity PASS: 1000 composite/global capacity cycles; active-transfer detach; allocation/pipe failure cleanup");
    return 0;
}
