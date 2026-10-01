/*
 * usbcore.c - controller discovery, transfer management and device
 * enumeration.
 *
 * Threading model: all enumeration, configuration and synchronous
 * transfers run in one worker context. Host controller schedules are
 * protected by the global OS lock; completions are collected under the
 * lock and their callbacks run after it is released.
 */

#include "usbnt.h"

USB_HC  *UsbHcs[USB_MAX_HC];
int      UsbHcCount;
USB_DEV  UsbDevs[USB_MAX_DEVICES];

volatile ULONG UsbChangeCount;

static USB_XFER *DoneHead;
static USB_XFER *DoneTail;

/* ------------------------------------------------------------------ */
/* Descriptor helpers                                                   */
/* ------------------------------------------------------------------ */

const UCHAR *UsbNextDesc(const UCHAR *Cur, const UCHAR *End)
{
    if (Cur == NULL || Cur >= End || Cur[0] < 2) {
        return NULL;
    }
    Cur += Cur[0];
    if (Cur + 2 > End || Cur[0] < 2 || Cur + Cur[0] > End) {
        return NULL;
    }
    return Cur;
}

const UCHAR *UsbFindDesc(const UCHAR *Start, const UCHAR *End, UCHAR Type)
{
    const UCHAR *d = Start;

    while (d != NULL) {
        d = UsbNextDesc(d, End);
        if (d == NULL) {
            return NULL;
        }
        if (d[1] == Type) {
            return d;
        }
        if (d[1] == USB_DT_INTERFACE && Type != USB_DT_INTERFACE) {
            return NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Transfers                                                            */
/* ------------------------------------------------------------------ */

/* Called by host controller drivers with the lock held */
void UsbXferDone(USB_XFER *Xfer, LONG Status, ULONG Actual)
{
    USB_PIPE *p = Xfer->Pipe;

    if (!Xfer->Busy) {
        return;
    }
    Xfer->Busy = 0;
    Xfer->Status = Status;
    Xfer->Actual = Actual;
    if (p != NULL) {
        USB_XFER *prev = NULL;
        USB_XFER *q = p->Cur;
        while (q != NULL && q != Xfer) {
            prev = q;
            q = q->QNext;
        }
        if (q == Xfer) {
            if (prev == NULL) {
                p->Cur = Xfer->QNext;
            } else {
                prev->QNext = Xfer->QNext;
            }
            if (p->Tail == Xfer) {
                p->Tail = prev;
            }
        }
        Xfer->QNext = NULL;
        if (Status == USB_ERR_STALL) {
            p->Halted = 1;
        }
    }
    Xfer->DoneNext = NULL;
    if (DoneTail != NULL) {
        DoneTail->DoneNext = Xfer;
    } else {
        DoneHead = Xfer;
    }
    DoneTail = Xfer;
}

/* Physical address of byte Off of a transfer and the contiguous run from there */
ULONG UsbXferPhys(USB_XFER *X, ULONG Off, ULONG *Run)
{
    ULONG left = X->Length - Off;

    if (X->Pfn != NULL) {
        ULONG o = X->PageOff + Off;
        ULONG idx = o / USB_PAGE_SIZE;
        ULONG in = o % USB_PAGE_SIZE;
        ULONG run = USB_PAGE_SIZE - in;
        while (run < left && X->Pfn[idx + 1] == X->Pfn[idx] + 1) {
            run += USB_PAGE_SIZE;
            idx++;
        }
        *Run = (run < left) ? run : left;
        return X->Pfn[o / USB_PAGE_SIZE] * USB_PAGE_SIZE + in;
    }
    *Run = left;
    return X->Phys + Off;
}

static void RunCompletions(USB_XFER *List)
{
    while (List != NULL) {
        USB_XFER *next = List->DoneNext;
        if (List->Complete != NULL) {
            List->Complete(List);
        }
        List = next;
    }
}

void UsbPollAll(void)
{
    OS_IRQL irql;
    USB_XFER *list;
    int i;

    irql = OsLock();
    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Running) {
            hc->Ops->Poll(hc);
        }
    }
    list = DoneHead;
    DoneHead = NULL;
    DoneTail = NULL;
    OsUnlock(irql);
    RunCompletions(list);
}

BOOLEAN UsbInterruptAll(void)
{
    BOOLEAN ours = FALSE;
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Running && hc->Ops->Interrupt != NULL && hc->Ops->Interrupt(hc)) {
            ours = TRUE;
        }
    }
    return ours;
}

void UsbTick(void)
{
    UsbPollAll();
    HidTick();
}

int UsbSubmit(USB_XFER *Xfer)
{
    USB_PIPE *p = Xfer->Pipe;
    OS_IRQL irql;
    int r;

    irql = OsLock();
    if (p->Dev->Gone || !p->Opened || !p->Dev->Hc->Running) {
        OsUnlock(irql);
        return USB_ERR_NODEV;
    }
    if (p->Cur != NULL && (!p->Dev->Hc->QueueOk || p->Type == USB_EP_CONTROL)) {
        OsUnlock(irql);
        return USB_ERR_BUSY;
    }
    Xfer->Busy = 1;
    Xfer->Status = USB_ERR_BUSY;
    Xfer->Actual = 0;
    Xfer->QNext = NULL;
    if (p->Cur == NULL) {
        p->Cur = Xfer;
    } else {
        p->Tail->QNext = Xfer;
    }
    p->Tail = Xfer;
    r = p->Dev->Hc->Ops->Submit(Xfer);
    if (r != USB_OK) {
        USB_XFER *prev = NULL;
        USB_XFER *q = p->Cur;
        while (q != NULL && q != Xfer) {
            prev = q;
            q = q->QNext;
        }
        if (prev == NULL) {
            p->Cur = NULL;
        } else {
            prev->QNext = NULL;
        }
        p->Tail = prev;
        Xfer->Busy = 0;
        Xfer->Status = r;
    }
    OsUnlock(irql);
    return r;
}

void UsbCancelPipe(USB_PIPE *Pipe)
{
    OS_IRQL irql;

    if (Pipe->Cur == NULL) {
        return;
    }
    if (Pipe->Dev->Hc->Running && Pipe->Dev->Hc->Ops->Cancel != NULL && Pipe->HcPriv != NULL) {
        Pipe->Dev->Hc->Ops->Cancel(Pipe);
    }
    irql = OsLock();
    while (Pipe->Cur != NULL) {
        UsbXferDone(Pipe->Cur, USB_ERR_ABORTED, 0);
    }
    OsUnlock(irql);
    UsbPollAll();
}

static void SyncDone(USB_XFER *Xfer)
{
    OsEventSet((OS_EVENT *)Xfer->Context);
}

static int XferSync(USB_XFER *X, ULONG TimeoutMs)
{
    OS_EVENT *ev = (OS_EVENT *)X->Context;
    int r;

    OsEventReset(ev);
    r = UsbSubmit(X);
    if (r != USB_OK) {
        return r;
    }
    if (!OsEventWait(ev, TimeoutMs)) {
        UsbCancelPipe(X->Pipe);
        OsEventWait(ev, 100);
        if (X->Busy) {
            return USB_ERR_TIMEOUT;
        }
        if (X->Status == USB_ERR_ABORTED) {
            return USB_ERR_TIMEOUT;
        }
    }
    return X->Status;
}

/*
 * Synchronous bulk or interrupt transfer on a physically contiguous
 * buffer. The buffer is split so that no segment crosses a 64 KB
 * boundary or exceeds the controller limit; every segment except the
 * last is a multiple of the packet size, so the wire traffic is the same
 * as for one large transfer.
 */
int UsbTransfer(USB_PIPE *Pipe, void *Buf, ULONG Phys, ULONG Len, ULONG TimeoutMs, ULONG *Actual)
{
    USB_XFER x;
    OS_EVENT ev;
    ULONG done = 0;
    ULONG maxp = Pipe->MaxPacket ? Pipe->MaxPacket : 8;
    ULONG hcmax = Pipe->Dev->Hc->MaxXfer;
    int r = USB_OK;

    OsEventInit(&ev);
    UsbMemSet(&x, 0, sizeof(x));
    x.Pipe = Pipe;
    x.DirIn = Pipe->DirIn;
    x.Complete = SyncDone;
    x.Context = &ev;

    do {
        ULONG seg = Len - done;
        ULONG bound = 0x10000 - ((Phys + done) & 0xFFFF);
        if (seg > bound) {
            seg = bound;
        }
        if (seg > hcmax) {
            seg = hcmax;
        }
        if (seg < Len - done) {
            seg -= seg % maxp;
            if (seg == 0) {
                r = USB_ERR_PARAM;
                break;
            }
        }
        x.Buf = (UCHAR *)Buf + done;
        x.Phys = Phys + done;
        x.Length = seg;
        r = XferSync(&x, TimeoutMs);
        if (r != USB_OK) {
            done += x.Actual;
            break;
        }
        done += x.Actual;
        if (x.Actual < seg) {
            break;
        }
    } while (done < Len);

    if (Actual != NULL) {
        *Actual = done;
    }
    return r;
}

/*
 * Synchronous bulk transfer on a list of page frame numbers. The first
 * page holds the data from byte PageOff on, every further page from its
 * start.
 */
int UsbTransferPages(USB_PIPE *Pipe, ULONG *Pfn, ULONG PageOff, ULONG Len, ULONG TimeoutMs, ULONG *Actual)
{
    USB_XFER x;
    OS_EVENT ev;
    ULONG done = 0;
    ULONG maxp = Pipe->MaxPacket ? Pipe->MaxPacket : 8;
    ULONG hcmax = Pipe->Dev->Hc->SgMax;
    int r = USB_OK;

    if (hcmax < maxp) {
        return USB_ERR_PARAM;
    }
    OsEventInit(&ev);
    UsbMemSet(&x, 0, sizeof(x));
    x.Pipe = Pipe;
    x.DirIn = Pipe->DirIn;
    x.Complete = SyncDone;
    x.Context = &ev;

    do {
        ULONG seg = Len - done;
        ULONG o = PageOff + done;
        if (seg > hcmax) {
            seg = hcmax - hcmax % maxp;
        }
        x.Buf = NULL;
        x.Pfn = Pfn + o / USB_PAGE_SIZE;
        x.PageOff = o % USB_PAGE_SIZE;
        x.Length = seg;
        r = XferSync(&x, TimeoutMs);
        done += x.Actual;
        if (r != USB_OK || x.Actual < seg) {
            break;
        }
    } while (done < Len);

    if (Actual != NULL) {
        *Actual = done;
    }
    return r;
}

int UsbControl(USB_DEV *Dev, UCHAR ReqType, UCHAR Req, USHORT Value, USHORT Index,
               USHORT Len, void *Data, ULONG TimeoutMs, ULONG *Actual)
{
    USB_XFER *x = &Dev->CtlXfer;
    UCHAR *setup = Dev->Page;
    int r;

    if (Actual != NULL) {
        *Actual = 0;
    }
    if (Dev->Gone) {
        return USB_ERR_NODEV;
    }
    if (Len > USB_DEV_DATA_MAX) {
        return USB_ERR_PARAM;
    }
    setup[0] = ReqType;
    setup[1] = Req;
    PUT16(setup + 2, Value);
    PUT16(setup + 4, Index);
    PUT16(setup + 6, Len);
    if (!(ReqType & USB_RT_IN) && Len != 0) {
        UsbMemCpy(Dev->Page + USB_DEV_DATA_OFF, Data, Len);
    }
    UsbMemSet(x, 0, sizeof(*x));
    x->Pipe = &Dev->Ep0;
    x->SetupPhys = Dev->PagePhys;
    x->Buf = Dev->Page + USB_DEV_DATA_OFF;
    x->Phys = Dev->PagePhys + USB_DEV_DATA_OFF;
    x->Length = Len;
    x->DirIn = (UCHAR)((ReqType & USB_RT_IN) ? 1 : 0);
    x->Complete = SyncDone;
    x->Context = &Dev->CtlEvent;

    r = XferSync(x, TimeoutMs ? TimeoutMs : 5000);
    if (r == USB_OK) {
        if ((ReqType & USB_RT_IN) && x->Actual != 0 && Data != NULL) {
            UsbMemCpy(Data, Dev->Page + USB_DEV_DATA_OFF, x->Actual);
        }
        if (Actual != NULL) {
            *Actual = x->Actual;
        }
    } else if (!Dev->Gone && (r == USB_ERR_STALL || r == USB_ERR_IO || r == USB_ERR_BABBLE || r == USB_ERR_TIMEOUT)) {
        if (Dev->Hc->Ops->PipeReset != NULL) {
            Dev->Hc->Ops->PipeReset(&Dev->Ep0);
        }
        Dev->Ep0.Halted = 0;
    }
    return r;
}

int UsbGetDescriptor(USB_DEV *Dev, UCHAR Type, UCHAR Index, USHORT LangId, void *Buf, USHORT Len, ULONG *Actual)
{
    return UsbControl(Dev, USB_RT_IN | USB_RT_STD | USB_RT_DEVICE, USB_REQ_GET_DESCRIPTOR,
                      (USHORT)((Type << 8) | Index), LangId, Len, Buf, 1000, Actual);
}

int UsbOpenPipe(USB_DEV *Dev, USB_PIPE *Pipe, const UCHAR *Ep)
{
    USHORT mps = GET16(Ep + 4);
    int r;

    UsbMemSet(Pipe, 0, sizeof(*Pipe));
    Pipe->Dev = Dev;
    Pipe->Endpoint = (UCHAR)(Ep[2] & 0x0F);
    Pipe->DirIn = (UCHAR)((Ep[2] & 0x80) ? 1 : 0);
    Pipe->Type = (UCHAR)(Ep[3] & 3);
    Pipe->Interval = Ep[6];
    Pipe->MaxPacket = (USHORT)(mps & 0x7FF);
    if (Dev->Speed == USB_SPEED_SUPER && Dev->Config != NULL) {
        const UCHAR *comp = UsbNextDesc(Ep, Dev->Config + Dev->ConfigLen);
        if (comp != NULL && comp[1] == USB_DT_SS_EP_COMP && comp[0] >= 6) {
            Pipe->MaxBurst = comp[2];
        }
    }
    if (Pipe->MaxPacket == 0) {
        return USB_ERR_PARAM;
    }
    r = Dev->Hc->Ops->PipeOpen(Pipe);
    if (r == USB_OK) {
        Pipe->Opened = 1;
    }
    return r;
}

void UsbClosePipe(USB_PIPE *Pipe)
{
    if (!Pipe->Opened) {
        return;
    }
    UsbCancelPipe(Pipe);
    Pipe->Dev->Hc->Ops->PipeClose(Pipe);
    Pipe->Opened = 0;
}

int UsbResetPipeToggle(USB_PIPE *Pipe)
{
    int r = USB_OK;

    if (Pipe->Cur != NULL) {
        UsbCancelPipe(Pipe);
    }
    if (Pipe->Dev->Hc->Ops->PipeReset != NULL) {
        r = Pipe->Dev->Hc->Ops->PipeReset(Pipe);
    }
    Pipe->Toggle = 0;
    Pipe->Halted = 0;
    return r;
}

int UsbOpenStreams(USB_PIPE *Pipe, USB_PIPE *Child, int Count)
{
    const HCD_OPS *ops = Pipe->Dev->Hc->Ops;

    if (ops->StreamsOpen == NULL || Pipe->Dev->Hc->MaxStreams == 0) {
        return USB_ERR_PARAM;
    }
    return ops->StreamsOpen(Pipe, Child, Count);
}

int UsbClearHalt(USB_PIPE *Pipe)
{
    int r;

    UsbResetPipeToggle(Pipe);
    r = UsbControl(Pipe->Dev, USB_RT_OUT | USB_RT_STD | USB_RT_ENDPOINT, USB_REQ_CLEAR_FEATURE,
                   USB_FEATURE_ENDPOINT_HALT, (USHORT)(Pipe->Endpoint | (Pipe->DirIn ? 0x80 : 0)),
                   0, NULL, 1000, NULL);
    return r;
}

/* ------------------------------------------------------------------ */
/* Enumeration                                                          */
/* ------------------------------------------------------------------ */

static const char *SpeedName(UCHAR s)
{
    switch (s) {
    case USB_SPEED_LOW:   return "low";
    case USB_SPEED_HIGH:  return "high";
    case USB_SPEED_SUPER: return "super";
    default:              return "full";
    }
}

static UCHAR AllocAddress(USB_HC *Hc)
{
    int a;

    for (a = 1; a < 128; a++) {
        if (!(Hc->AddrMap[a >> 3] & (1 << (a & 7)))) {
            Hc->AddrMap[a >> 3] |= (UCHAR)(1 << (a & 7));
            return (UCHAR)a;
        }
    }
    return 0;
}

static void FreeAddress(USB_HC *Hc, UCHAR a)
{
    if (a != 0 && a < 128) {
        Hc->AddrMap[a >> 3] &= (UCHAR)~(1 << (a & 7));
    }
}

static USB_DEV *DevAlloc(void)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        if (!d->InUse) {
            UCHAR *page = d->Page;
            ULONG phys = d->PagePhys;
            UsbMemSet(d, 0, sizeof(*d));
            if (page == NULL) {
                page = (UCHAR *)OsDmaAlloc(USB_PAGE_SIZE, &phys);
                if (page == NULL) {
                    return NULL;
                }
            } else {
                UsbMemSet(page, 0, USB_PAGE_SIZE);
            }
            d->Page = page;
            d->PagePhys = phys;
            d->Index = (UCHAR)i;
            d->InUse = 1;
            OsEventInit(&d->CtlEvent);
            return d;
        }
    }
    return NULL;
}

void UsbNotifyChange(void)
{
    UsbChangeCount++;
    OsNotifyChange();
}

/* Reads string descriptor Index as ASCII */
static void ReadString(USB_DEV *Dev, UCHAR Index, USHORT Lang, char *Out, ULONG Size)
{
    UCHAR buf[128];
    ULONG got = 0;
    ULONG i;
    ULONG n = 0;

    Out[0] = 0;
    if (Index == 0 || UsbGetDescriptor(Dev, USB_DT_STRING, Index, Lang, buf, sizeof(buf), &got) != USB_OK ||
        got < 4) {
        return;
    }
    if (got > buf[0]) {
        got = buf[0];
    }
    for (i = 2; i + 1 < got && n + 1 < Size; i += 2) {
        USHORT ch = GET16(buf + i);
        Out[n++] = (char)((ch >= 32 && ch < 127) ? ch : '?');
    }
    while (n > 0 && Out[n - 1] == ' ') {
        n--;
    }
    Out[n] = 0;
}

static void ReadStrings(USB_DEV *Dev)
{
    UCHAR buf[8];
    ULONG got = 0;
    USHORT lang = 0x0409;

    if (Dev->DevDesc[14] == 0 && Dev->DevDesc[15] == 0 && Dev->DevDesc[16] == 0) {
        return;
    }
    if (UsbGetDescriptor(Dev, USB_DT_STRING, 0, 0, buf, sizeof(buf), &got) == USB_OK && got >= 4) {
        lang = GET16(buf + 2);
    }
    ReadString(Dev, Dev->DevDesc[14], lang, Dev->Mfg, sizeof(Dev->Mfg));
    ReadString(Dev, Dev->DevDesc[15], lang, Dev->Product, sizeof(Dev->Product));
    ReadString(Dev, Dev->DevDesc[16], lang, Dev->Serial, sizeof(Dev->Serial));
}

static void BindInterfaces(USB_DEV *Dev)
{
    const UCHAR *end = Dev->Config + Dev->ConfigLen;
    const UCHAR *d = Dev->Config;
    int bound = 0;

    while ((d = UsbNextDesc(d, end)) != NULL) {
        if (d[1] != USB_DT_INTERFACE || d[0] < 9 || d[3] != 0) {
            continue;
        }
        switch (d[5]) {
        case USB_CLASS_HUB:
            if (HubAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        case USB_CLASS_MSC:
            if (MscAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        case USB_CLASS_HID:
            if (HidAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        default:
            break;
        }
    }
    if (!bound) {
        UsbLog(LOG_INFO, "device %u: no driver for this device\n", Dev->Index);
    }
}

static int Enumerate(USB_DEV *Dev)
{
    const HCD_OPS *ops = Dev->Hc->Ops;
    UCHAR buf[18];
    ULONG got = 0;
    int r;
    int tries;
    USHORT total;
    UCHAR mps0;

    switch (Dev->Speed) {
    case USB_SPEED_HIGH:  Dev->Ep0.MaxPacket = 64; break;
    case USB_SPEED_SUPER: Dev->Ep0.MaxPacket = 512; break;
    default:              Dev->Ep0.MaxPacket = 8; break;
    }
    Dev->Ep0.Dev = Dev;
    Dev->Ep0.Type = USB_EP_CONTROL;

    r = ops->DevInit(Dev);
    if (r != USB_OK) {
        return r;
    }
    r = ops->PipeOpen(&Dev->Ep0);
    if (r != USB_OK) {
        return r;
    }
    Dev->Ep0.Opened = 1;

    if (ops->DevSetAddress != NULL) {
        r = ops->DevSetAddress(Dev);
        if (r != USB_OK) {
            return r;
        }
        OsSleepMs(2);
    }

    r = USB_ERR_IO;
    for (tries = 0; tries < 3 && r != USB_OK; tries++) {
        r = UsbGetDescriptor(Dev, USB_DT_DEVICE, 0, 0, buf, 8, &got);
        if (r == USB_OK && got < 8) {
            r = USB_ERR_IO;
        }
        if (r != USB_OK) {
            OsSleepMs(50);
        }
    }
    if (r != USB_OK) {
        UsbLog(LOG_ERR, "device %u: cannot read device descriptor (%d)\n", Dev->Index, r);
        return r;
    }

    mps0 = buf[7];
    if (Dev->Speed == USB_SPEED_SUPER) {
        Dev->Ep0.MaxPacket = (USHORT)((mps0 >= 5 && mps0 <= 12) ? (1 << mps0) : 512);
    } else if (mps0 == 8 || mps0 == 16 || mps0 == 32 || mps0 == 64) {
        Dev->Ep0.MaxPacket = mps0;
    }

    if (ops->DevSetAddress == NULL) {
        UCHAR addr = AllocAddress(Dev->Hc);
        if (addr == 0) {
            return USB_ERR_NOMEM;
        }
        r = UsbControl(Dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEVICE, USB_REQ_SET_ADDRESS,
                       addr, 0, 0, NULL, 1000, NULL);
        if (r != USB_OK) {
            FreeAddress(Dev->Hc, addr);
            UsbLog(LOG_ERR, "device %u: set address failed (%d)\n", Dev->Index, r);
            return r;
        }
        OsSleepMs(10);
        Dev->Address = addr;
    }
    if (ops->DevUpdate != NULL) {
        r = ops->DevUpdate(Dev);
        if (r != USB_OK) {
            return r;
        }
    }

    r = UsbGetDescriptor(Dev, USB_DT_DEVICE, 0, 0, Dev->DevDesc, 18, &got);
    if (r != USB_OK || got < 18) {
        UsbLog(LOG_ERR, "device %u: device descriptor failed (%d)\n", Dev->Index, r);
        return (r != USB_OK) ? r : USB_ERR_IO;
    }

    r = UsbGetDescriptor(Dev, USB_DT_CONFIG, 0, 0, buf, 9, &got);
    if (r != USB_OK || got < 9) {
        UsbLog(LOG_ERR, "device %u: config descriptor failed (%d)\n", Dev->Index, r);
        return (r != USB_OK) ? r : USB_ERR_IO;
    }
    total = GET16(buf + 2);
    if (total < 9) {
        return USB_ERR_IO;
    }
    if (total > USB_DEV_DATA_MAX) {
        total = USB_DEV_DATA_MAX;
    }
    Dev->Config = (UCHAR *)OsAlloc(total);
    if (Dev->Config == NULL) {
        return USB_ERR_NOMEM;
    }
    r = UsbGetDescriptor(Dev, USB_DT_CONFIG, 0, 0, Dev->Config, total, &got);
    if (r != USB_OK || got < 9) {
        return (r != USB_OK) ? r : USB_ERR_IO;
    }
    Dev->ConfigLen = (USHORT)got;
    Dev->ConfigValue = Dev->Config[5];

    r = UsbControl(Dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEVICE, USB_REQ_SET_CONFIGURATION,
                   Dev->ConfigValue, 0, 0, NULL, 1000, NULL);
    if (r != USB_OK) {
        UsbLog(LOG_ERR, "device %u: set configuration failed (%d)\n", Dev->Index, r);
        return r;
    }

    ReadStrings(Dev);
    UsbLog(LOG_INFO, "device %u: %04x:%04x class %02x, %s speed%s, %s port %u%s%u, addr %u\n",
           Dev->Index, GET16(Dev->DevDesc + 8), GET16(Dev->DevDesc + 10), Dev->DevDesc[4],
           SpeedName(Dev->Speed), Dev->LinkGbps == 10 ? " 10 Gbit/s" : (Dev->LinkGbps == 20 ? " 20 Gbit/s" : ""),
           Dev->Hc->Ops->Name, Dev->RootPort,
           Dev->Parent ? " hub port " : " depth ", Dev->Parent ? Dev->Port : Dev->Depth, Dev->Address);
    if (Dev->Product[0] != 0 || Dev->Mfg[0] != 0) {
        UsbLog(LOG_INFO, "device %u: %s %s\n", Dev->Index, Dev->Mfg, Dev->Product);
    }
    BindInterfaces(Dev);
    UsbNotifyChange();
    return USB_OK;
}

static void DevRelease(USB_DEV *Dev)
{
    if (Dev->Ep0.Opened) {
        UsbCancelPipe(&Dev->Ep0);
        if (Dev->Hc->Ops->PipeClose != NULL) {
            Dev->Hc->Ops->PipeClose(&Dev->Ep0);
        }
        Dev->Ep0.Opened = 0;
    }
    if (Dev->Hc->Ops->DevFree != NULL) {
        Dev->Hc->Ops->DevFree(Dev);
    }
    if (Dev->Hc->Ops->DevSetAddress == NULL) {
        FreeAddress(Dev->Hc, Dev->Address);
    }
    if (Dev->Config != NULL) {
        OsFree(Dev->Config);
        Dev->Config = NULL;
    }
    Dev->InUse = 0;
}

USB_DEV *UsbDevAttach(USB_HC *Hc, USB_DEV *Parent, int Port, UCHAR Speed)
{
    USB_DEV *d = DevAlloc();
    int r;

    if (d == NULL) {
        UsbLog(LOG_ERR, "too many devices\n");
        return NULL;
    }
    d->Hc = Hc;
    d->Parent = Parent;
    d->Port = (UCHAR)Port;
    d->Speed = Speed;
    if (Parent != NULL) {
        d->Depth = (UCHAR)(Parent->Depth + 1);
        d->RootPort = Parent->RootPort;
        d->Route = Parent->Route | ((ULONG)(Port > 15 ? 15 : Port) << (4 * Parent->Depth));
        if (Speed == USB_SPEED_LOW || Speed == USB_SPEED_FULL) {
            if (Parent->Speed == USB_SPEED_HIGH) {
                d->TtHub = Parent;
                d->TtPort = (UCHAR)Port;
            } else {
                d->TtHub = Parent->TtHub;
                d->TtPort = Parent->TtPort;
            }
        }
    } else {
        d->RootPort = (UCHAR)Port;
    }
    if (Speed == USB_SPEED_SUPER) {
        d->LinkGbps = (Parent == NULL && Hc->PortGbps[Port] != 0) ? Hc->PortGbps[Port] : 5;
    }

    r = Enumerate(d);
    if (r != USB_OK) {
        d->Gone = 1;
        HubDetach(d);
        MscDetach(d);
        HidDetach(d);
        DevRelease(d);
        return NULL;
    }
    return d;
}

void UsbDevDetach(USB_DEV *Dev)
{
    if (Dev == NULL || !Dev->InUse || Dev->Gone) {
        return;
    }
    UsbLog(LOG_INFO, "device %u: removed\n", Dev->Index);
    Dev->Gone = 1;
    HubRemoveChildren(Dev);
    HidDetach(Dev);
    MscDetach(Dev);
    HubDetach(Dev);
    DevRelease(Dev);
    UsbNotifyChange();
}

/* Root port polling, worker context */
void UsbPortPoll(USB_HC *Hc)
{
    int port;

    for (port = 1; port <= Hc->NumPorts; port++) {
        ULONG st = Hc->Ops->PortStatus(Hc, port);
        USB_DEV *dev = Hc->RootDev[port];
        UCHAR speed = USB_SPEED_FULL;
        int r;

        if (st & PS_CHANGE) {
            Hc->Ops->PortClearChange(Hc, port);
        }
        if (dev != NULL && (!(st & PS_CONNECT) || (st & PS_CHANGE) || (st & PS_OTHER_OWNER) ||
                            (!(st & PS_ENABLE) && !(st & PS_SS_PORT)))) {
            UsbDevDetach(dev);
            Hc->RootDev[port] = NULL;
            dev = NULL;
        }
        if (!(st & PS_CONNECT) || (st & PS_OTHER_OWNER)) {
            Hc->PortRetry[port] = 0;
            continue;
        }
        if (dev != NULL || Hc->PortRetry[port] >= 3) {
            continue;
        }

        UsbLog(LOG_TRACE, "%s port %d connected (%x)\n", Hc->Ops->Name, port, st);
        OsSleepMs(100);
        st = Hc->Ops->PortStatus(Hc, port);
        if (!(st & PS_CONNECT)) {
            continue;
        }
        r = Hc->Ops->PortReset(Hc, port, &speed);
        UsbLog(LOG_TRACE, "%s port %d reset: %d speed %u\n", Hc->Ops->Name, port, r, speed);
        if (r == USB_PORT_HANDOFF) {
            continue;
        }
        Hc->Ops->PortClearChange(Hc, port);
        if (r != USB_OK) {
            Hc->PortRetry[port]++;
            continue;
        }
        OsSleepMs(20);
        dev = UsbDevAttach(Hc, NULL, port, speed);
        if (dev == NULL) {
            Hc->PortRetry[port]++;
            if (Hc->Ops->PortDisable != NULL) {
                Hc->Ops->PortDisable(Hc, port);
            }
            Hc->Ops->PortClearChange(Hc, port);
            continue;
        }
        Hc->RootDev[port] = dev;
        Hc->PortRetry[port] = 0;
    }
}

void UsbWorkerIteration(void)
{
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Running) {
            UsbPortPoll(hc);
        }
    }
    HubServiceAll();
    HidService();
    MscService();
}

/* ------------------------------------------------------------------ */
/* Controller discovery                                                 */
/* ------------------------------------------------------------------ */

static void IntelRouteToXhci(UCHAR Bus, UCHAR Dev, UCHAR Fn)
{
    ULONG mask;

    if (!OsGetConfig("IntelRouteToXhci", 1)) {
        return;
    }
    mask = OsPciRead(Bus, Dev, Fn, 0xDC, 4);
    OsPciWrite(Bus, Dev, Fn, 0xD8, mask, 4);
    mask = OsPciRead(Bus, Dev, Fn, 0xD4, 4);
    OsPciWrite(Bus, Dev, Fn, 0xD0, mask, 4);
    UsbLog(LOG_INFO, "intel: USB 2.0 ports routed to xHCI (%08x)\n", mask);
}

static USB_HC *AddController(UCHAR Bus, UCHAR Dev, UCHAR Fn, UCHAR ProgIf)
{
    USB_HC *hc;
    ULONG bar;
    ULONG cmd;

    if (UsbHcCount >= USB_MAX_HC) {
        return NULL;
    }
    hc = (USB_HC *)OsAlloc(sizeof(USB_HC));
    if (hc == NULL) {
        return NULL;
    }
    hc->Bus = Bus;
    hc->Dev = Dev;
    hc->Fn = Fn;
    hc->Index = (UCHAR)UsbHcCount;
    hc->VendorId = (USHORT)OsPciRead(Bus, Dev, Fn, 0, 2);
    hc->DeviceId = (USHORT)OsPciRead(Bus, Dev, Fn, 2, 2);
    hc->IrqLine = (UCHAR)OsPciRead(Bus, Dev, Fn, 0x3C, 1);

    switch (ProgIf) {
    case 0x00:
        hc->Type = HC_UHCI;
        hc->Ops = &UhciOps;
        bar = OsPciRead(Bus, Dev, Fn, 0x20, 4);
        if (!(bar & 1) || (bar & ~3UL) == 0) {
            OsFree(hc);
            return NULL;
        }
        hc->IoBase = bar & 0xFFE0;
        break;
    case 0x10:
    case 0x20:
    case 0x30:
        hc->Type = (ProgIf == 0x10) ? HC_OHCI : (ProgIf == 0x20) ? HC_EHCI : HC_XHCI;
        hc->Ops = (ProgIf == 0x10) ? &OhciOps : (ProgIf == 0x20) ? &EhciOps : &XhciOps;
        bar = OsPciRead(Bus, Dev, Fn, 0x10, 4);
        if ((bar & 1) || (bar & ~0xFUL) == 0) {
            OsFree(hc);
            return NULL;
        }
        if (((bar >> 1) & 3) == 2 && OsPciRead(Bus, Dev, Fn, 0x14, 4) != 0) {
            UsbLog(LOG_ERR, "%02x:%02x.%u: register BAR above 4 GB, controller skipped\n", Bus, Dev, Fn);
            OsFree(hc);
            return NULL;
        }
        hc->MmioPhys = bar & ~0xFUL;
        hc->MmioLen = (ProgIf == 0x30) ? 0x10000 : 0x1000;
        hc->Mmio = (UCHAR *)OsMapMmio(Bus, hc->MmioPhys, hc->MmioLen);
        if (hc->Mmio == NULL) {
            OsFree(hc);
            return NULL;
        }
        break;
    default:
        OsFree(hc);
        return NULL;
    }

    cmd = OsPciRead(Bus, Dev, Fn, 4, 2);
    cmd |= (hc->Type == HC_UHCI) ? 0x0005 : 0x0006;
    cmd &= ~0x0400UL;
    OsPciWrite(Bus, Dev, Fn, 4, cmd, 2);

    UsbHcs[UsbHcCount++] = hc;
    UsbLog(LOG_INFO, "found %s at %02x:%02x.%u (%04x:%04x) irq %u %s %08x\n",
           hc->Ops->Name, Bus, Dev, Fn, hc->VendorId, hc->DeviceId, hc->IrqLine,
           hc->Type == HC_UHCI ? "io" : "mem", hc->Type == HC_UHCI ? hc->IoBase : hc->MmioPhys);
    return hc;
}

static void StartType(UCHAR Type)
{
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Type == Type && !hc->Running) {
            if (hc->Ops->Start(hc) != USB_OK) {
                UsbLog(LOG_ERR, "%s at %02x:%02x.%u failed to start\n", hc->Ops->Name, hc->Bus, hc->Dev, hc->Fn);
                hc->Running = 0;
            }
        }
    }
}

int UsbInit(void)
{
    ULONG bus;
    ULONG maxBus = OsGetConfig("MaxPciBus", 255);
    ULONG dev, fn;
    int i, j;
    ULONG disable = OsGetConfig("DisableControllers", 0);

    UsbDebugLevel = OsGetConfig("DebugLevel", UsbDebugLevel);

    for (bus = 0; bus <= maxBus; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                ULONG id = OsPciRead((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, 0, 4);
                ULONG cls;
                if (id == 0xFFFFFFFF || id == 0) {
                    if (fn == 0) {
                        break;
                    }
                    continue;
                }
                cls = OsPciRead((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, 8, 4) >> 8;
                if ((cls >> 8) == 0x0C03) {
                    UCHAR pi = (UCHAR)(cls & 0xFF);
                    if ((pi == 0x00 && !(disable & 1)) || (pi == 0x10 && !(disable & 2)) ||
                        (pi == 0x20 && !(disable & 4)) || (pi == 0x30 && !(disable & 8))) {
                        AddController((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, pi);
                    }
                    if (pi == 0x30 && (id & 0xFFFF) == 0x8086) {
                        IntelRouteToXhci((UCHAR)bus, (UCHAR)dev, (UCHAR)fn);
                    }
                }
                if (fn == 0 && !(OsPciRead((UCHAR)bus, (UCHAR)dev, 0, 0x0E, 1) & 0x80)) {
                    break;
                }
            }
        }
    }

    /* link companion controllers to the EHCI in the same PCI device */
    for (i = 0; i < UsbHcCount; i++) {
        if (UsbHcs[i]->Type != HC_EHCI) {
            continue;
        }
        for (j = 0; j < UsbHcCount; j++) {
            USB_HC *c = UsbHcs[j];
            if ((c->Type == HC_UHCI || c->Type == HC_OHCI) && c->Bus == UsbHcs[i]->Bus &&
                c->Dev == UsbHcs[i]->Dev) {
                c->Ehci = UsbHcs[i];
            }
        }
    }

    StartType(HC_XHCI);
    StartType(HC_EHCI);
    StartType(HC_UHCI);
    StartType(HC_OHCI);

    for (i = 0; i < UsbHcCount; i++) {
        if (UsbHcs[i]->Running) {
            return USB_OK;
        }
    }
    return USB_ERR_NODEV;
}

void UsbShutdown(void)
{
    int i;
    int port;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        for (port = 1; port <= hc->NumPorts; port++) {
            if (hc->RootDev[port] != NULL) {
                UsbDevDetach(hc->RootDev[port]);
                hc->RootDev[port] = NULL;
            }
        }
    }
    for (i = UsbHcCount - 1; i >= 0; i--) {
        if (UsbHcs[i]->Running) {
            UsbHcs[i]->Ops->Stop(UsbHcs[i]);
        }
    }
}
