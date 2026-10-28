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
#include "nvmehp.h"
#include "tbt.h"
#include "sdcard.h"
#include "usbvideo.h"
#include "usbscsi.h"
#include "smbtp.h"

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
    AudioTick();
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
    if (p->Type == USB_EP_ISOCH && (Xfer->Iso == NULL || Xfer->IsoCount == 0 || !p->Dev->Hc->IsoOk)) {
        OsUnlock(irql);
        return USB_ERR_PARAM;
    }
    if (p->Cur != NULL && p->Type != USB_EP_ISOCH && (!p->Dev->Hc->QueueOk || p->Type == USB_EP_CONTROL)) {
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
    if (r == USB_ERR_BUSY && X->Pipe->Type == USB_EP_CONTROL) {
        /* a control transfer of Windows' driver of the device is on the way */
        ULONG t0 = OsTimeMs();
        while (r == USB_ERR_BUSY && OsTimeMs() - t0 < TimeoutMs) {
            OsSleepMs(1);
            r = UsbSubmit(X);
        }
    }
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
    Pipe->Mult = 1;
    if (Dev->Speed == USB_SPEED_HIGH && (Pipe->Type == USB_EP_ISOCH || Pipe->Type == USB_EP_INTERRUPT)) {
        Pipe->Mult = (UCHAR)(((mps >> 11) & 3) + 1);
        if (Pipe->Mult > 3) {
            Pipe->Mult = 3;
        }
    }
    if (Pipe->Type == USB_EP_ISOCH) {
        Pipe->IsoSlots = UsbIsoSlotUframes(Pipe);
    }
    if (Dev->Speed == USB_SPEED_SUPER && Dev->Config != NULL) {
        const UCHAR *comp = UsbNextDesc(Ep, Dev->Config + Dev->ConfigLen);
        if (comp != NULL && comp[1] == USB_DT_SS_EP_COMP && comp[0] >= 6) {
            Pipe->MaxBurst = comp[2];
            if (Pipe->Type == USB_EP_ISOCH) {
                Pipe->Mult = (UCHAR)((comp[3] & 3) + 1);
            }
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

/*
 * Service interval of an isochronous endpoint in microframes: 2^(bInterval-1)
 * microframes at high/super speed, 2^(bInterval-1) frames at full speed.
 */
ULONG UsbIsoSlotUframes(USB_PIPE *Pipe)
{
    ULONG b = Pipe->Interval;

    if (b < 1) {
        b = 1;
    }
    if (b > 16) {
        b = 16;
    }
    if (Pipe->Dev->Speed == USB_SPEED_HIGH || Pipe->Dev->Speed == USB_SPEED_SUPER) {
        return 1UL << (b - 1);
    }
    return 8UL << (b - 1);
}

int UsbSetInterface(USB_DEV *Dev, UCHAR Interface, UCHAR Alt)
{
    return UsbControl(Dev, USB_RT_OUT | USB_RT_STD | USB_RT_INTERFACE, USB_REQ_SET_INTERFACE,
                      Alt, Interface, 0, NULL, 1000, NULL);
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
        if (ch == 0) {
            break;                          /* some devices pad with NULs */
        }
        if (n == 0 && (ch == ' ' || ch == '\t')) {
            continue;                       /* leading blanks */
        }
        /* Latin-1 maps 1:1 to the ANSI code page; a few common punctuation marks */
        if (ch == 0x2122) {
            ch = 0x99;                      /* trade mark */
        } else if (ch == 0x2013 || ch == 0x2014) {
            ch = '-';
        } else if (ch == 0x2018 || ch == 0x2019) {
            ch = '\'';
        }
        Out[n++] = (char)((ch >= 32 && ch < 127) || (ch >= 0xA0 && ch <= 0xFF) || ch == 0x99 ? ch : '?');
    }
    while (n > 0 && Out[n - 1] == ' ') {
        n--;
    }
    Out[n] = 0;
}

static void ReadStrings(USB_DEV *Dev)
{
    UCHAR buf[32];
    ULONG got = 0;
    USHORT lang = 0x0409;

    if (Dev->DevDesc[14] == 0 && Dev->DevDesc[15] == 0 && Dev->DevDesc[16] == 0) {
        return;
    }
    if (UsbGetDescriptor(Dev, USB_DT_STRING, 0, 0, buf, sizeof(buf), &got) == USB_OK && got >= 4) {
        ULONG i;
        lang = GET16(buf + 2);
        for (i = 2; i + 1 < got && i + 1 < buf[0]; i += 2) {
            if (GET16(buf + i) == 0x0409) {
                lang = 0x0409;              /* English when the device has it */
            }
        }
    }
    ReadString(Dev, Dev->DevDesc[14], lang, Dev->Mfg, sizeof(Dev->Mfg));
    ReadString(Dev, Dev->DevDesc[15], lang, Dev->Product, sizeof(Dev->Product));
    ReadString(Dev, Dev->DevDesc[16], lang, Dev->Serial, sizeof(Dev->Serial));
}

ULONG UsbPdoMode;
ULONG UsbTextSetupHid;
ULONG UsbWinVideo;
void (*UsbDetachHook)(USB_DEV *Dev);
static ULONG DevGen;

/* every interface of class Class serves the class driver that took the device */
static void MarkClass(USB_DEV *Dev, UCHAR Class)
{
    const UCHAR *end = Dev->Config + Dev->ConfigLen;
    const UCHAR *d = Dev->Config;

    while ((d = UsbNextDesc(d, end)) != NULL) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[5] == Class && d[2] < 32) {
            Dev->NativeIf |= 1UL << d[2];
        }
    }
}

static void BindInterfaces(USB_DEV *Dev)
{
    const UCHAR *end = Dev->Config + Dev->ConfigLen;
    const UCHAR *d = Dev->Config;
    int bound = 0;

    while ((d = UsbNextDesc(d, end)) != NULL) {
        int before = bound;
        if (d[1] != USB_DT_INTERFACE || d[0] < 9 || d[3] != 0) {
            continue;
        }
        if (UsbPdoMode == 2 && d[5] != USB_CLASS_HUB) {
            continue;                       /* everything to Windows' drivers */
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
            if (UsbTextSetupHid) {
                break;                      /* text-mode setup: setup's own HID drivers (usbdi.c) */
            }
            if (HidAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        case USB_CLASS_AUDIO:
            /* the control interface claims its streaming / MIDI interfaces */
            if (AudioAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        case 0x0E:
            /* video: the control interface claims its streaming interface; Windows 2000 / XP with
               WindowsCamera = 1: the camera goes to Windows' usbvideo.sys */
            if (UsbWinVideo && UsbPdoMode) {
                break;
            }
            if (d[6] == 0x01 && UvcAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        case 0xFF:
            /* vendor specific: Realtek USB2.0-CRW card readers */
            if (RtsxUsbAttach(Dev, d, end) == USB_OK) {
                bound++;
            }
            break;
        default:
            break;
        }
        if (bound != before && d[2] < 32) {
            Dev->NativeIf |= 1UL << d[2];
            if (d[5] == USB_CLASS_AUDIO || d[5] == 0x0E) {
                MarkClass(Dev, d[5]);       /* the control interface claimed its streaming interfaces */
            }
        }
    }
    if (!bound) {
        UsbLog(LOG_INFO, "device %u: no driver for this device%s\n", Dev->Index,
               UsbPdoMode ? " in USBNT, it goes to Windows" : "");
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
    Dev->Gen = ++DevGen;
    Dev->Ready = 1;
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
        AudioDetach(d);
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
    Dev->Ready = 0;
    if (UsbDetachHook != NULL) {
        UsbDetachHook(Dev);                 /* Windows' driver of the device lets go of it */
    }
    HubRemoveChildren(Dev);
    AudioDetach(Dev);
    HidDetach(Dev);
    MscDetach(Dev);
    RtsxUsbDetach(Dev);
    UvcDetach(Dev);
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

static void HpService(void);
static int NHp;

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
    AudioService();
    SdHostService();
    RtsxUsbService();
    I2cHidService();
    SmbusService();
    UvcService();
    if (TbtHostCount != 0) {
        TbtService();
    }
    if (NHp != 0 && OsGetConfig("PciHotPlug", 1)) {
        HpService();
    }
}

/* ------------------------------------------------------------------ */
/* Controller discovery                                                 */
/* ------------------------------------------------------------------ */

/*
 * Intel chipsets with EHCI and xHCI (7 series and later): which controller the
 * ports belong to.  IntelRouteToXhci 1 = SuperSpeed on (USB3_PSSEN from
 * USB3PRM) and the USB 2.0 side of every switchable port to xHCI (XUSB2PR from
 * XUSB2PRM), 2 = only SuperSpeed to xHCI, the USB 2.0 side stays with EHCI
 * (Windows 2000 / XP when Windows' own EHCI driver drives the EHCI: USB 2.0
 * devices must reach it), 0 = leave what the firmware set.
 *
 * Windows 2000 / XP default 2 (usbinst writes the value it chose; text-mode setup writes
 * none): there the USB 2.0 side of the ports always has a driver - Windows' EHCI driver, or
 * USBNT's own - so moving it to xHCI only took the devices away from Windows' EHCI.  In
 * text-mode setup with the "USB 3 xHCI" choice that was the USB keyboard setup was reading
 * (XP's text-mode setup has its own USB keyboard support, on EHCI) and the USB install media:
 * no keyboard in text-mode setup on Intel 7 series and later, while the installed system
 * (where usbinst writes 2) worked.
 */
static void IntelRouteToXhci(UCHAR Bus, UCHAR Dev, UCHAR Fn)
{
#ifdef USB_WDM
    ULONG mode = OsGetConfig("IntelRouteToXhci", 2);
#else
    ULONG mode = OsGetConfig("IntelRouteToXhci", 1);
#endif
    ULONG mask;

    if (mode == 0) {
        return;
    }
    mask = OsPciRead(Bus, Dev, Fn, 0xDC, 4);
    OsPciWrite(Bus, Dev, Fn, 0xD8, mask, 4);
    if (mode == 2) {
        OsPciWrite(Bus, Dev, Fn, 0xD0, 0, 4);
        UsbLog(LOG_INFO, "intel: SuperSpeed to xHCI (%08x), USB 2.0 ports stay with EHCI\n", mask);
        return;
    }
    mask = OsPciRead(Bus, Dev, Fn, 0xD4, 4);
    OsPciWrite(Bus, Dev, Fn, 0xD0, mask, 4);
    UsbLog(LOG_INFO, "intel: USB 2.0 ports routed to xHCI (%08x)\n", mask);
}

/* 64-bit BARs that the firmware put above 4 GB go below 4 GB (pcireloc.c) */
#define RL_CFG32(b, d, f, o)     OsPciRead((UCHAR)(b), (UCHAR)(d), (UCHAR)(f), o, 4)
#define RL_CFGW32(b, d, f, o, v) OsPciWrite((UCHAR)(b), (UCHAR)(d), (UCHAR)(f), o, v, 4)
#define RL_MAP(pa, size)         OsMapMmio(0, pa, size)
#define RL_UNMAP(va, size)       OsUnmapMmio(va, size)
#define RL_LOG(...)              UsbLog(LOG_INFO, __VA_ARGS__), UsbLog(LOG_INFO, "\n")
#include "pcireloc.c"

/* can the kernel map physical addresses above 4 GB?  x64 always; 32-bit Windows
   2000 / XP when it runs the PAE kernel; NT 3.x / 4.0 never */
static BOOLEAN CanMapHigh(void)
{
#if defined(_WIN64)
    return TRUE;
#elif defined(USB_WDM)
    ULONG cr4;
    __asm__ __volatile__("movl %%cr4, %0" : "=r"(cr4));
    return (cr4 & 0x20) != 0;
#else
    return FALSE;
#endif
}

/* RelocateBars: 0 never, 1 only the controllers USBNT drives, 2 every device in the
   system when the kernel cannot reach them above 4 GB (default), 3 every device always */
void UsbPciRelocateAll(void)
{
    ULONG mode = OsGetConfig("RelocateBars", 2);
    if (mode < 2) {
        return;
    }
    if (mode == 2 && CanMapHigh()) {
        UsbLog(LOG_INFO, "pcireloc: the kernel maps above 4 GB (PAE / x64), devices stay where they are\n");
        return;
    }
    PciRelocateAll();
}

ULONG UsbPciRelocateBar(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off)
{
    if (!OsGetConfig("RelocateBars", 1)) {
        return 0;
    }
    return PciRelocateBar(Bus, Dev, Fn, Off);
}

/* PCI Express extended configuration space (0x100-0xFFF) through the MMCONFIG
   window of the chipset (Intel PCIEXBAR, AMD MMIO_CFG_BASE); FALSE when unknown */
BOOLEAN UsbPciExt(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, ULONG *Val, BOOLEAN Write)
{
    static ULONG lo, hi;
    static BOOLEAN done;
    ULONG pa;
    UCHAR *va;

    if (!done) {
        done = TRUE;
        RlHoleBottom(&lo, &hi);
        if (lo) {
            UsbLog(LOG_INFO, "PCI Express configuration window %08x-%08x\n", lo, hi - 1);
        }
    }
    if (Off < 0x100) {
        if (Write) {
            OsPciWrite(Bus, Dev, Fn, Off, *Val, 4);
        } else {
            *Val = OsPciRead(Bus, Dev, Fn, Off, 4);
        }
        return TRUE;
    }
    pa = lo + ((ULONG)Bus << 20) + ((ULONG)Dev << 15) + ((ULONG)Fn << 12);
    if (lo == 0 || Off > 0xFFC || pa + 0x1000 > hi || pa < lo) {
        return FALSE;
    }
    va = (UCHAR *)OsMapMmio(Bus, pa, 0x1000);
    if (va == NULL) {
        return FALSE;
    }
    if (Write) {
        MmioWr32(va + (Off & ~3UL), *Val);
    } else {
        *Val = MmioRd32(va + (Off & ~3UL));
    }
    OsUnmapMmio(va, 0x1000);
    return TRUE;
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
            bar = UsbPciRelocateBar(Bus, Dev, Fn, 0x10);
            if (bar == 0) {
                UsbLog(LOG_ERR, "%02x:%02x.%u: register BAR above 4 GB could not be moved, controller skipped\n",
                       Bus, Dev, Fn);
                OsFree(hc);
                return NULL;
            }
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

/* ------------------------------------------------------------------ */
/* PCIe hot plug: Thunderbolt 3 / 4 and USB4 docks, ExpressCard          */
/*                                                                      */
/* Windows NT does not know PCI hot plug. The ports that can hot plug   */
/* are noted at the start; every 2 s their slot status is read. When a  */
/* dock comes up (the Thunderbolt firmware connection manager has made  */
/* the PCIe tunnel) the new functions get bus numbers, bridge windows   */
/* and BARs inside the window the firmware reserved for the port, and   */
/* the USB host controllers among them are started (polled, the legacy  */
/* interrupt stays off). When it goes away its controllers stop and     */
/* their devices are removed.                                           */
/* ------------------------------------------------------------------ */

#define HP_MAX      16

typedef struct _HP_PORT {
    UCHAR   Bus, Dev, Fn, Cap;
    UCHAR   Sec, Sub;
    UCHAR   Present;
    UCHAR   Pad;
    ULONG   Since;          /* arrival time */
    ULONG   StopAt;         /* attention button pressed: the slot stops at this time (PCIe: 5 s to cancel) */
} HP_PORT;

static HP_PORT HpPorts[HP_MAX];
static int NHp;
static ULONG HpNext;

static UCHAR PciCapFind(UCHAR Bus, UCHAR Dev, UCHAR Fn, UCHAR Id)
{
    UCHAR p;
    int n = 0;

    if (!(OsPciRead(Bus, Dev, Fn, 0x06, 2) & 0x10)) {
        return 0;
    }
    p = (UCHAR)(OsPciRead(Bus, Dev, Fn, 0x34, 1) & 0xFC);
    while (p && n++ < 48) {
        if (OsPciRead(Bus, Dev, Fn, p, 1) == Id) {
            return p;
        }
        p = (UCHAR)(OsPciRead(Bus, Dev, Fn, p + 1, 1) & 0xFC);
    }
    return 0;
}

static BOOLEAN HpSlotPresent(const HP_PORT *p)
{
    ULONG ss = OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x1A, 2);
    ULONG ls = OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x12, 2);
    ULONG id;

    if (ss == 0xFFFF) {
        return FALSE;
    }
    if ((ss & 0x40) || (ls & 0x2000)) {                 /* presence detect, data link layer active */
        return TRUE;
    }
    id = OsPciRead(p->Sec, 0, 0, 0, 4);
    return (BOOLEAN)(p->Sec != 0 && id != 0xFFFFFFFFUL && id != 0);
}

/* a PCI-PCI bridge seen by the start scan: note it when its slot can hot plug */
static void HpNote(UCHAR Bus, UCHAR Dev, UCHAR Fn)
{
    UCHAR cap = PciCapFind(Bus, Dev, Fn, 0x10);
    ULONG flags, slot, bn;
    HP_PORT *p;

    if (cap == 0 || NHp >= HP_MAX) {
        return;
    }
    flags = OsPciRead(Bus, Dev, Fn, cap + 2, 2);
    slot = OsPciRead(Bus, Dev, Fn, cap + 0x14, 4);
    if (!(flags & 0x100) || !(slot & 0x40)) {
        return;
    }
    bn = OsPciRead(Bus, Dev, Fn, 0x18, 4);
    p = &HpPorts[NHp++];
    p->Bus = Bus;
    p->Dev = Dev;
    p->Fn = Fn;
    p->Cap = cap;
    p->Sec = (UCHAR)(bn >> 8);
    p->Sub = (UCHAR)(bn >> 16);
    p->Present = HpSlotPresent(p);
    if (p->Present && OsGetConfig("NvmeAtBoot", 0)) {
        p->Present = FALSE;                 /* taken as an arrival: NVMe drives there are driven here */
    }
}

typedef struct _HP_ALLOC {
    ULONG   Lo;
    ULONG   Cur;
    ULONG   End;
    UCHAR   NextBus;
    UCHAR   MaxBus;
    UCHAR   Failed;
    UCHAR   Found;
    UCHAR   NewBus[8], NewDev[8], NewFn[8], NewIf[8];
} HP_ALLOC;

static ULONG AlignUp(ULONG v, ULONG a)
{
    return (v + a - 1) & ~(a - 1);
}

/* gives the memory BARs of a USB host controller an address in the window */
static void HpBars(UCHAR Bus, UCHAR Dev, UCHAR Fn, HP_ALLOC *A)
{
    ULONG off;

    OsPciWrite(Bus, Dev, Fn, 4, 0x0400, 2);                     /* decoding off, INTx off */
    for (off = 0x10; off <= 0x24; off += 4) {
        ULONG lo = OsPciRead(Bus, Dev, Fn, off, 4), mask, size;
        BOOLEAN is64 = (BOOLEAN)((lo & 7) == 4);
        if (lo & 1) {
            continue;                                           /* I/O: not used by xHCI / EHCI */
        }
        if ((lo & ~0xFUL) >= A->Lo && (lo & ~0xFUL) < A->End && !(is64 && OsPciRead(Bus, Dev, Fn, off + 4, 4))) {
            if (is64) off += 4;
            continue;                                           /* already set */
        }
        OsPciWrite(Bus, Dev, Fn, off, 0xFFFFFFFFUL, 4);
        mask = OsPciRead(Bus, Dev, Fn, off, 4) & ~0xFUL;
        if (mask == 0) {
            OsPciWrite(Bus, Dev, Fn, off, lo, 4);
            if (is64) off += 4;
            continue;                                           /* not implemented */
        }
        size = ~mask + 1;
        if (size < 0x1000) {
            size = 0x1000;
        }
        A->Cur = AlignUp(A->Cur, size);
        if (A->Cur + size > A->End || A->Cur + size < A->Cur) {
            A->Failed = 1;
            return;
        }
        OsPciWrite(Bus, Dev, Fn, off, A->Cur, 4);
        if (is64) {
            OsPciWrite(Bus, Dev, Fn, off + 4, 0, 4);
            off += 4;
        }
        UsbLog(LOG_DBG, "hotplug: %02x:%02x.%u BAR %x at %08x (%x bytes)\n", Bus, Dev, Fn, off, A->Cur, size);
        A->Cur += size;
    }
}

/* bus numbers, windows and BARs below a hot plug port (depth first) */
static void HpBus(UCHAR Bus, HP_ALLOC *A, int Depth)
{
    ULONG dev, fn;

    for (dev = 0; dev < 32 && !A->Failed; dev++) {
        for (fn = 0; fn < 8 && !A->Failed; fn++) {
            ULONG id = OsPciRead(Bus, (UCHAR)dev, (UCHAR)fn, 0, 4);
            ULONG hdr, cls;
            if (id == 0xFFFFFFFFUL || id == 0) {
                if (fn == 0) break;
                continue;
            }
            hdr = OsPciRead(Bus, (UCHAR)dev, (UCHAR)fn, 0x0E, 1);
            cls = OsPciRead(Bus, (UCHAR)dev, (UCHAR)fn, 8, 4) >> 8;
            if ((hdr & 0x7F) == 1 && Depth < 6) {
                ULONG bn = OsPciRead(Bus, (UCHAR)dev, (UCHAR)fn, 0x18, 4);
                UCHAR sec = (UCHAR)(bn >> 8);
                ULONG start;
                if (sec == 0 || sec <= Bus) {
                    if (A->NextBus > A->MaxBus) {
                        A->Failed = 1;
                        break;
                    }
                    sec = A->NextBus++;
                    OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x18, (bn & 0xFF000000UL) | ((ULONG)A->MaxBus << 16) |
                               ((ULONG)sec << 8) | Bus, 4);
                } else if (sec >= A->NextBus) {
                    A->NextBus = (UCHAR)(((bn >> 16) & 0xFF) + 1);
                }
                A->Cur = AlignUp(A->Cur, 0x100000);
                start = A->Cur;
                HpBus(sec, A, Depth + 1);
                /* subordinate = the last bus used below */
                OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x1A, (UCHAR)(A->NextBus - 1), 1);
                A->Cur = AlignUp(A->Cur, 0x100000);
                if (A->Cur > start) {
                    OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x20, ((A->Cur - 1) & 0xFFF00000UL) | (start >> 16), 4);
                } else {
                    OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x20, 0x0000FFF0UL, 4);   /* no window */
                }
                OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x24, 0x0000FFF0UL, 4);       /* no prefetchable window */
                OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x28, 0, 4);
                OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 0x2C, 0, 4);
                OsPciWrite(Bus, (UCHAR)dev, (UCHAR)fn, 4, OsPciRead(Bus, (UCHAR)dev, (UCHAR)fn, 4, 2) | 0x0006, 2);
            } else if ((cls >> 8) == 0x0C03 && ((cls & 0xFF) == 0x30 || (cls & 0xFF) == 0x20)) {
                int k;
                BOOLEAN known = FALSE;
                for (k = 0; k < UsbHcCount; k++) {
                    if (UsbHcs[k]->Bus == Bus && UsbHcs[k]->Dev == dev && UsbHcs[k]->Fn == fn && !UsbHcs[k]->Removed) {
                        known = TRUE;
                    }
                }
                if (!known && A->Found < 8) {
                    HpBars(Bus, (UCHAR)dev, (UCHAR)fn, A);
                    A->NewBus[A->Found] = Bus;
                    A->NewDev[A->Found] = (UCHAR)dev;
                    A->NewFn[A->Found] = (UCHAR)fn;
                    A->NewIf[A->Found] = (UCHAR)(cls & 0xFF);
                    A->Found++;
                }
            } else if (cls == 0x010802 && A->Found < 8) {
                /* NVMe: started after the scan (nvmehp.c) */
                HpBars(Bus, (UCHAR)dev, (UCHAR)fn, A);
                A->NewBus[A->Found] = Bus;
                A->NewDev[A->Found] = (UCHAR)dev;
                A->NewFn[A->Found] = (UCHAR)fn;
                A->NewIf[A->Found] = 0xFF;
                A->Found++;
            } else if (cls == 0x0C0340) {
                UsbLog(LOG_INFO, "hotplug: USB4 router %04x:%04x in the dock at %02x:%02x.%u\n", id & 0xFFFF, id >> 16,
                       Bus, dev, fn);
            }
            if (fn == 0 && !(hdr & 0x80)) {
                break;
            }
        }
    }
}

static void HpRemoved(USB_HC *hc)
{
    OS_IRQL irql;
    int port;

    UsbLog(LOG_INFO, "hotplug: %s at %02x:%02x.%u is gone\n", hc->Ops->Name, hc->Bus, hc->Dev, hc->Fn);
    irql = OsLock();
    hc->Running = 0;
    hc->Removed = 1;
    OsUnlock(irql);
    for (port = 1; port <= hc->NumPorts; port++) {
        if (hc->RootDev[port] != NULL) {
            UsbDevDetach(hc->RootDev[port]);
            hc->RootDev[port] = NULL;
        }
    }
    UsbNotifyChange();
}

static void HpArrived(HP_PORT *p)
{
    HP_ALLOC a;
    ULONG w = OsPciRead(p->Bus, p->Dev, p->Fn, 0x20, 4);
    ULONG wlo = (w & 0xFFF0) << 16, whi = (w & 0xFFF00000UL) | 0xFFFFF;
    int i;

    UsbMemSet(&a, 0, sizeof(a));
    if (whi <= wlo) {
        UsbLog(LOG_ERR, "hotplug: port %02x:%02x.%u has no memory window, the firmware reserved nothing\n",
               p->Bus, p->Dev, p->Fn);
        return;
    }
    a.Lo = wlo;
    a.Cur = wlo;
    a.End = whi + 1;
    a.NextBus = (UCHAR)(p->Sec + 1);
    a.MaxBus = p->Sub;
    OsPciWrite(p->Bus, p->Dev, p->Fn, 4, OsPciRead(p->Bus, p->Dev, p->Fn, 4, 2) | 0x0006, 2);
    HpBus(p->Sec, &a, 0);
    if (a.Failed) {
        UsbLog(LOG_ERR, "hotplug: not enough bus numbers or memory behind %02x:%02x.%u\n", p->Bus, p->Dev, p->Fn);
    }
    for (i = 0; i < a.Found; i++) {
        USB_HC *hc;
        int r;
        if (a.Failed) {
            break;
        }
        if (a.NewIf[i] == 0xFF) {
            NvmeHotAttach(a.NewBus[i], a.NewDev[i], a.NewFn[i]);
            continue;
        }
        hc = AddController(a.NewBus[i], a.NewDev[i], a.NewFn[i], a.NewIf[i]);
        if (hc == NULL) {
            continue;
        }
        hc->Hot = 1;
        OsPciWrite(hc->Bus, hc->Dev, hc->Fn, 4, OsPciRead(hc->Bus, hc->Dev, hc->Fn, 4, 2) | 0x0400, 2);  /* polled */
        r = hc->Ops->Start(hc);
        UsbLog(LOG_INFO, "hotplug: %s at %02x:%02x.%u %s\n", hc->Ops->Name, hc->Bus, hc->Dev, hc->Fn,
               r == USB_OK ? "started (polled)" : "does not start");
        if (r != USB_OK) {
            hc->Running = 0;
        }
    }
    UsbNotifyChange();
}

static void HpService(void)
{
    int i;
    ULONG now = OsTimeMs();

    if ((LONG)(now - HpNext) < 0) {
        return;
    }
    HpNext = now + 2000;
    NvmeHotService();
    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Hot && !hc->Removed && OsPciRead(hc->Bus, hc->Dev, hc->Fn, 0, 4) == 0xFFFFFFFFUL) {
            HpRemoved(hc);
        }
    }
    for (i = 0; i < NHp; i++) {
        HP_PORT *p = &HpPorts[i];
        ULONG ss = OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x1A, 2);
        BOOLEAN present;
        if (ss != 0xFFFF && (ss & 0x0001)) {
            /* attention button (slots with a button, also QEMU): power on, or stop and power off */
            ULONG sc = OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, 2);
            OsPciWrite(p->Bus, p->Dev, p->Fn, p->Cap + 0x1A, ss & 0x0019, 2);   /* clear ABP, PDC, CC (only those set: QEMU drops a clear of more) */
            if (p->Present && p->StopAt != 0) {
                /* a second press within 5 s cancels (PCIe hot plug rules) */
                p->StopAt = 0;
                sc = (sc & ~0x0300UL) | 0x0100;                                  /* power indicator on */
                OsPciWrite(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, sc, 2);
                UsbLog(LOG_INFO, "hotplug: %02x:%02x.%u button pressed again, the slot keeps running\n", p->Bus,
                       p->Dev, p->Fn);
                continue;
            }
            if (p->Present) {
                p->StopAt = now + 5000;
                if (p->StopAt == 0) p->StopAt = 1;
                sc = (sc & ~0x0300UL) | 0x0200;                                  /* power indicator blinks */
                OsPciWrite(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, sc, 2);
                UsbLog(LOG_INFO, "hotplug: %02x:%02x.%u button pressed, the slot stops in 5 s\n", p->Bus, p->Dev,
                       p->Fn);
                continue;
            }
            sc = (sc & ~0x07C0UL) | 0x00C0 | 0x0100;                             /* power LED on, power on */
            OsPciWrite(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, sc, 2);
            UsbLog(LOG_INFO, "hotplug: %02x:%02x.%u button pressed, slot powered\n", p->Bus, p->Dev, p->Fn);
            OsSleepMs(100);
        }
        if (p->Present && p->StopAt != 0 && (LONG)(now - p->StopAt) >= 0) {
            ULONG sc = OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, 2);
            p->StopAt = 0;
            {
                int k;
                UsbLog(LOG_INFO, "hotplug: %02x:%02x.%u stopping the slot\n", p->Bus, p->Dev, p->Fn);
                NvmeHotStop(p->Sec, p->Sub);
                for (k = 0; k < UsbHcCount; k++) {
                    USB_HC *hc = UsbHcs[k];
                    if (!hc->Removed && hc->Bus >= p->Sec && hc->Bus <= p->Sub) {
                        if (hc->Running && hc->Ops->Stop != NULL) {
                            int port;
                            for (port = 1; port <= hc->NumPorts; port++) {
                                if (hc->RootDev[port] != NULL) {
                                    UsbDevDetach(hc->RootDev[port]);
                                    hc->RootDev[port] = NULL;
                                }
                            }
                            hc->Ops->Stop(hc);
                        }
                        OsPciWrite(hc->Bus, hc->Dev, hc->Fn, 4, 0x0400, 2);           /* no DMA, no decoding */
                        HpRemoved(hc);
                    }
                }
                sc = (sc & ~0x07C0UL) | 0x00C0 | 0x0300 | 0x0400;               /* indicators off, power off */
                OsPciWrite(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, sc, 2);
                p->Present = FALSE;
                continue;
            }
        }
        present = HpSlotPresent(p);
        if (present && !p->Present) {
            UsbLog(LOG_INFO, "hotplug: something arrived at %02x:%02x.%u (bus %u-%u)\n", p->Bus, p->Dev, p->Fn,
                   p->Sec, p->Sub);
            OsSleepMs(200);                                     /* link training, then the config space */
            HpArrived(p);
            p->Since = OsTimeMs();
            if (OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x14, 4) & 0x10) {
                /* power indicator on: the slot is in use (QEMU and some ports blink it while a card is
                   added and refuse a removal request until the system says it is done, like pciehp) */
                ULONG sc = OsPciRead(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, 2);
                if ((sc & 0x0300) != 0x0100) {
                    OsPciWrite(p->Bus, p->Dev, p->Fn, p->Cap + 0x18, (sc & ~0x0300UL) | 0x0100, 2);
                }
            }
        } else if (!present && p->Present) {
            int k;
            UsbLog(LOG_INFO, "hotplug: %02x:%02x.%u emptied\n", p->Bus, p->Dev, p->Fn);
            for (k = 0; k < UsbHcCount; k++) {
                USB_HC *hc = UsbHcs[k];
                if (!hc->Removed && hc->Bus >= p->Sec && hc->Bus <= p->Sub &&
                    OsPciRead(hc->Bus, hc->Dev, hc->Fn, 0, 4) == 0xFFFFFFFFUL) {
                    HpRemoved(hc);
                }
            }
        }
        if (!present) {
            p->StopAt = 0;
        }
        p->Present = present;
    }
}

/* Thunderbolt / USB4 host routers (tbt.c): the driver takes their control
   channel; their USB ports use an xHCI that is driven like any other */
static void NoteRouter(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, ULONG Cls)
{
    TbtProbe(Bus, Dev, Fn, Id, Cls);
}

static void StartType(UCHAR Type)
{
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Type == Type && !hc->Running) {
            int r = hc->Ops->Start(hc);
            if (r != USB_OK) {
                UsbLog(LOG_ERR, "%s at %02x:%02x.%u failed to start (%d)\n", hc->Ops->Name, hc->Bus, hc->Dev, hc->Fn, r);
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
                /* SD card readers: SDHCI, Realtek PCIe */
                if ((cls >> 8) == 0x0805) {
                    SdhciProbe((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, cls);
                } else if ((id & 0xFFFF) == 0x10EC && ((cls >> 16) == 0xFF || (cls >> 8) == 0x0805 || (cls >> 16) == 0x08)) {
                    RtsxPciProbe((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, (USHORT)(id >> 16));
                }
                if ((OsPciRead((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, 0x0E, 1) & 0x7F) == 1) {
                    HpNote((UCHAR)bus, (UCHAR)dev, (UCHAR)fn);
                }
                NoteRouter((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, id, cls);
                /* DesignWare I2C controllers (HID over I2C) */
                if ((id & 0xFFFF) == 0x8086 && (cls >> 8) == 0x0C80) {
                    I2cPciProbe((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, id, cls);
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

    /* HID over I2C (touchpads, touch screens) */
    I2cHidInit();
    /* SMBus host controllers (touch pads on SMBus: set up by the PS/2 driver's identification) */
    SmbusInit();

    if (NHp != 0) {
        UsbLog(LOG_INFO, "%d PCIe hot plug port(s) watched (Thunderbolt / USB4 docks)\n", NHp);
    }

    for (i = 0; i < UsbHcCount; i++) {
        if (UsbHcs[i]->Running) {
            return USB_OK;
        }
    }
    return (SdHostCount || I2cHidCount) ? USB_OK : USB_ERR_NODEV;
}

/* ------------------------------------------------------------------ */
/* Windows 2000 / XP: Plug and Play starts and stops one PCI function at */
/* a time (worker context)                                              */
/* ------------------------------------------------------------------ */

/* once: settings, the PCIe hot plug ports (nothing is started here) */
void UsbPnpInit(void)
{
    ULONG bus, dev, fn;
    ULONG maxBus = OsGetConfig("MaxPciBus", 255);

    UsbDebugLevel = OsGetConfig("DebugLevel", UsbDebugLevel);
    for (bus = 0; bus <= maxBus; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                ULONG id = OsPciRead((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, 0, 4);
                if (id == 0xFFFFFFFF || id == 0) {
                    if (fn == 0) {
                        break;
                    }
                    continue;
                }
                if ((OsPciRead((UCHAR)bus, (UCHAR)dev, (UCHAR)fn, 0x0E, 1) & 0x7F) == 1) {
                    HpNote((UCHAR)bus, (UCHAR)dev, (UCHAR)fn);
                }
                if (fn == 0 && !(OsPciRead((UCHAR)bus, (UCHAR)dev, 0, 0x0E, 1) & 0x80)) {
                    break;
                }
            }
        }
    }
    if (NHp != 0) {
        UsbLog(LOG_INFO, "%d PCIe hot plug port(s) watched (Thunderbolt / USB4 docks)\n", NHp);
    }
}

static USB_HC *FindHc(UCHAR Bus, UCHAR Dev, UCHAR Fn, BOOLEAN Live)
{
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        if (hc->Bus == Bus && hc->Dev == Dev && hc->Fn == Fn && (hc->Removed == 0) == Live) {
            return hc;
        }
    }
    return NULL;
}

/* the PCI function Plug and Play gave us: USB host controller, card reader, I2C controller, Thunderbolt /
   USB4 host interface */
int UsbPnpStart(UCHAR Bus, UCHAR Dev, UCHAR Fn)
{
    ULONG id = OsPciRead(Bus, Dev, Fn, 0, 4);
    ULONG cls = OsPciRead(Bus, Dev, Fn, 8, 4) >> 8;
    ULONG disable = OsGetConfig("DisableControllers", 0);
    int i, r;

    if (id == 0xFFFFFFFF || id == 0) {
        return USB_ERR_NODEV;
    }
    if ((cls >> 8) == 0x0C03) {
        UCHAR pi = (UCHAR)(cls & 0xFF);
        USB_HC *hc, *old;
        if ((pi == 0x00 && (disable & 1)) || (pi == 0x10 && (disable & 2)) || (pi == 0x20 && (disable & 4)) ||
            (pi == 0x30 && (disable & 8)) || pi > 0x30) {
            return USB_ERR_NODEV;
        }
        if (FindHc(Bus, Dev, Fn, TRUE) != NULL) {
            return USB_OK;
        }
        if (pi == 0x30 && (id & 0xFFFF) == 0x8086) {
            IntelRouteToXhci(Bus, Dev, Fn);
        }
        old = FindHc(Bus, Dev, Fn, FALSE);
        hc = AddController(Bus, Dev, Fn, pi);
        if (hc == NULL) {
            return USB_ERR_NOMEM;
        }
        if (old != NULL) {
            /* started again (resume, rebalance): the new one takes the old place in the list */
            UsbHcs[old->Index] = hc;
            hc->Index = old->Index;
            UsbHcCount--;
            OsFree(old);
        }
        /* companions and their EHCI find each other whatever comes first */
        for (i = 0; i < UsbHcCount; i++) {
            USB_HC *c = UsbHcs[i];
            if (c == hc || c->Removed || c->Bus != Bus || c->Dev != Dev) {
                continue;
            }
            if (hc->Type == HC_EHCI && (c->Type == HC_UHCI || c->Type == HC_OHCI)) {
                c->Ehci = hc;
            } else if (c->Type == HC_EHCI && (hc->Type == HC_UHCI || hc->Type == HC_OHCI)) {
                hc->Ehci = c;
            }
        }
        r = hc->Ops->Start(hc);
        if (r != USB_OK) {
            UsbLog(LOG_ERR, "%s at %02x:%02x.%u failed to start (%d)\n", hc->Ops->Name, Bus, Dev, Fn, r);
            hc->Running = 0;
            hc->Removed = 1;
            return r;
        }
        UsbNotifyChange();
        return USB_OK;
    }
    if ((cls >> 8) == 0x0805) {
        SdhciProbe(Bus, Dev, Fn, cls);
        return USB_OK;
    }
    if ((id & 0xFFFF) == 0x10EC && ((cls >> 16) == 0xFF || (cls >> 16) == 0x08)) {
        RtsxPciProbe(Bus, Dev, Fn, (USHORT)(id >> 16));
        return USB_OK;
    }
    if (cls == 0x0C0340 || (cls >> 8) == 0x0880) {
        TbtProbe(Bus, Dev, Fn, id, cls);
        return USB_OK;
    }
    /* DesignWare I2C (HID over I2C): found with the ACPI devices by I2cHidInit */
    return USB_OK;
}

/* Windows 2000 / XP: the devices at a just started controller come up (hubs, disks, keyboard) before
   Plug and Play asks the device bus for them, so that a disk the system boots from is there when the
   kernel looks for its boot device.  Ends when nothing has changed for half a second and every
   connected root port has its device (or gave up), at most MaxMs. */
void UsbPnpSettle(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG MaxMs)
{
    USB_HC *hc = FindHc(Bus, Dev, Fn, TRUE);
    ULONG start = OsTimeMs(), stable = start;
    int last = -1;

    if (hc == NULL || !hc->Running || hc->Ops->PortStatus == NULL) {
        return;
    }
    while (OsTimeMs() - start < MaxMs) {
        int n = 0, i, p;
        BOOLEAN waiting = FALSE;
        for (i = 0; i < USB_MAX_DEVICES; i++) {
            n += (UsbDevs[i].InUse && !UsbDevs[i].Gone);
        }
        for (i = 0; i < (int)UsbScsiSlotCount; i++) {
            n += UsbScsiSlotPresent(i) ? 1 : 0;
        }
        for (p = 1; p <= hc->NumPorts; p++) {
            ULONG st = hc->Ops->PortStatus(hc, p);
            if ((st & PS_CONNECT) && !(st & PS_OTHER_OWNER) && hc->RootDev[p] == NULL && hc->PortRetry[p] < 3) {
                waiting = TRUE;
            }
        }
        if (n != last) {
            last = n;
            stable = OsTimeMs();
        } else if (!waiting && OsTimeMs() - stable >= 500) {
            break;
        }
        UsbWorkerIteration();
        OsSleepMs(20);
    }
    UsbLog(LOG_INFO, "%s %02x:%02x.%u: devices settled after %u ms\n", hc->Ops->Name, Bus, Dev, Fn,
           OsTimeMs() - start);
}

/* Plug and Play takes the function away again: Gone = it is not there any more (surprise removal) */
void UsbPnpStop(UCHAR Bus, UCHAR Dev, UCHAR Fn, BOOLEAN Gone)
{
    USB_HC *hc = FindHc(Bus, Dev, Fn, TRUE);
    OS_IRQL irql;
    int port;

    if (hc == NULL) {
        return;
    }
    UsbLog(LOG_INFO, "%s at %02x:%02x.%u %s\n", hc->Ops->Name, Bus, Dev, Fn, Gone ? "is gone" : "stops");
    if (Gone) {
        irql = OsLock();
        hc->Running = 0;
        OsUnlock(irql);
    }
    for (port = 1; port <= hc->NumPorts; port++) {
        if (hc->RootDev[port] != NULL) {
            UsbDevDetach(hc->RootDev[port]);
            hc->RootDev[port] = NULL;
        }
    }
    if (!Gone && hc->Running) {
        hc->Ops->Stop(hc);
    }
    irql = OsLock();
    hc->Running = 0;
    hc->Removed = 1;
    OsUnlock(irql);
    UsbNotifyChange();
}

void UsbShutdown(void)
{
    int i;
    int port;

    I2cHidShutdown();
    SmbusShutdown();
    NvmeShutdown();
    TbtShutdown();
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
