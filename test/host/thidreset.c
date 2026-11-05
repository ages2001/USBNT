/* Production HID attach/detach with mock DMA and endpoint ownership.
 * This checks software lifetime, not real USB scheduling or NT ABI layout. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define USB_HARNESS 1
#include "../../src/usbhid.c"

static unsigned blocks, pipes, controls, attempts, fail_alloc, fail_open;
static unsigned restore_step, fail_step, protocol, idle, breaks;
static int check(void) { return ++restore_step == fail_step ? USB_ERR_IO : USB_OK; }
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
void OsKbdInput(const USB_KEY_EVENT *e, ULONG n) { ULONG i; for (i=0;i<n;i++) if(e[i].Flags & UKEY_BREAK) breaks++; }
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
    controls++;
    if (req == HID_REQ_SET_PROTOCOL) { assert(value == 0); protocol++; }
    if (req == HID_REQ_SET_IDLE) { assert(value == 0); idle++; }
    return check();
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
    if (p->Opened) { assert(pipes); pipes--; p->Opened = 0; p->HcPriv = NULL; }
    if (x) { x->Status = USB_ERR_ABORTED; x->Complete(x); }
}
static int reopen(USB_PIPE *p) {
    assert(!p->Opened && !p->HcPriv && !p->Cur);
    assert(!p->Toggle && !p->Halted && !p->IsoNext);
    p->HcPriv = (void *)1; pipes++; return check();
}
static void close_context(USB_PIPE *p) {
    if (p->HcPriv) { assert(pipes); pipes--; p->HcPriv = NULL; }
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
    USB_DEV d, sibling; USB_HC hc; HCD_OPS ops;
    HID_DEV *saved[4]; unsigned cycle, fault, i, before;
    memset(&hc, 0, sizeof(hc)); memset(&ops, 0, sizeof(ops));
    ops.PipeOpen = reopen; ops.PipeClose = close_context; hc.Ops = &ops;
    for (cycle=0; cycle<100; cycle++) for (fault=0; fault<=12; fault++) {
        memset(&d, 0, sizeof(d)); memset(&sibling, 0, sizeof(sibling));
        d.Hc = sibling.Hc = &hc; fail_step=restore_step=0;
        for(i=0;i<4;i++) {
            assert(attach(&d,i)==USB_OK); saved[i]=d.Hid[i];
            saved[i]->In.Toggle=saved[i]->In.Halted=1;
            saved[i]->In.IsoNext=23;
            saved[i]->Prev[4]=4; /* held key is not necessarily in slot 2 */
        }
        assert(attach(&sibling,0)==USB_OK);
        assert(HidResetInterfaces(&d)==15);
        before=breaks;
        HidBeginDeviceReset(&d);
        assert(pipes==1 && breaks==before+4);
        for(i=0;i<4;i++) assert(saved[i]->Paused && !saved[i]->Active);
        before=controls; HidService();
        /* Only the unaffected sibling can issue its LED request. */
        assert(controls <= before+1);
        fail_step=fault; restore_step=protocol=idle=0;
        {
            int r=HidRestoreDevice(&d);
            assert(r==(fault ? USB_ERR_IO : USB_OK));
            for(i=0;i<4;i++) assert(!saved[i]->Active);
            HidEndDeviceReset(&d,r);
        }
        assert(pipes==(fault?1:5));
        for(i=0;i<4;i++) {
            assert(d.Hid[i]==saved[i]);
            assert(saved[i]->Paused==(fault!=0) && saved[i]->Active==(fault==0));
        }
        if(fault) {
            before=controls; HidService(); assert(controls==before);
            HidBeginDeviceReset(&d); fail_step=restore_step=0;
            assert(HidRestoreDevice(&d)==USB_OK);
            HidEndDeviceReset(&d,USB_OK); assert(pipes==5);
        } else assert(protocol==4 && idle==4);
        assert(((HID_DEV*)sibling.Hid[0])->Active);
        /* Removal after a failed restore also must not retain an owner. */
        if(cycle&1) {
            HidBeginDeviceReset(&d); fail_step=1; restore_step=0;
            assert(HidRestoreDevice(&d)==USB_ERR_IO);
            HidEndDeviceReset(&d,USB_ERR_IO);
        }
        HidDetach(&d); HidDetach(&sibling); empty();
    }
    puts("RESULT hid-reset PASS: 1300 four-interface resets; each protocol/idle/partial-open failure; retry and failed-reset removal; stable handles; held-key release; sibling continuity");
    return 0;
}
