/*
 * usbdi.c - Windows 2000 / XP (x86 and x64): the USB devices (and the
 * functions of composite devices) USBNT has no driver of its own for are
 * reported on the USBNT device bus with the ids Windows' hub driver gives
 * them (USB\VID_v&PID_p&REV_r, USB\Class_c&SubClass_s&Prot_p, &MI_nn), and
 * their drivers - Windows' own (printers, serial adapters, HID devices,
 * modems, Bluetooth, ...) and vendors' - talk to them through the USB
 * driver interface: URBs (IOCTL_INTERNAL_USB_SUBMIT_URB), the port
 * requests and the USBDI bus interface.  UsbPdo=2 gives every device but
 * the hubs to Windows' drivers, UsbPdo=0 none.
 *
 * Transfers run on the core's pipes asynchronously; data goes through
 * contiguous buffers below 4 GB (a cache refilled by the worker), in 64 KB
 * pieces for bulk and interrupt pipes.  Configuration and pipe changes run
 * in the worker.
 */

#include "wdm.h"
#include <usb.h>
#include <usbioctl.h>
#include <usbbusif.h>

#define UD_TAG          0x44555355      /* 'USUD' */
#define UD_PIPES        32
#define UD_ISO_INFLIGHT 8
#define UD_CHUNK        0x10000

#ifndef USBD_STATUS_DEVICE_GONE
#define USBD_STATUS_DEVICE_GONE ((USBD_STATUS)0xC0007000L)
#endif
#ifndef USBD_STATUS_CANCELED
#define USBD_STATUS_CANCELED ((USBD_STATUS)0xC0010000L)
#endif

/* ------------------------------------------------------------------ */
/* Objects                                                              */
/* ------------------------------------------------------------------ */

typedef struct _UD_FUNC UD_FUNC;

typedef struct _UD_PIPE {
    USB_PIPE        P;
    UD_FUNC        *F;
    UCHAR           Addr;
    UCHAR           Type;
    UCHAR           If;
    UCHAR           Used;
    UCHAR           Abort;              /* worker: cancel the transfers on the wire */
    UCHAR           Pad[3];
    ULONG           MaxXfer;
    LIST_ENTRY      Wait;               /* requests not on the wire yet */
    ULONG           Active;             /* requests on the wire */
} UD_PIPE;

typedef struct _UD_IFACE {              /* what SELECT_* hands out as the interface handle */
    UD_FUNC        *F;
    UCHAR           Num;
    UCHAR           Alt;
} UD_IFACE;

struct _UD_FUNC {
    UD_FUNC        *Next;
    USB_DEV        *Dev;                /* NULL once the device is gone */
    ULONG           Gen;
    UCHAR           DevIndex;
    UCHAR           First, Count;       /* interfaces of a function of a composite device */
    BOOLEAN         Whole;              /* the whole device */
    BOOLEAN         Dead;               /* unplugged (or the port cycled) */
    BOOLEAN         Started;
    BOOLEAN         GoneReported;       /* the bus relations no longer have it */
    BOOLEAN         NameDone;           /* the device node's name was looked at */
    UCHAR           ChildNames;         /* composite device: times the names of usbccgp's functions were set */
    ULONG           StartedAt;
    UCHAR           Config;             /* configuration value selected, 0 = none */
    UCHAR           Speed;
    PDEVICE_OBJECT  Pdo;
    LONG            Pending;            /* requests not completed */
    UCHAR           DevDesc[18];
    UCHAR          *Cfg;                /* configuration descriptor as the function sees it */
    ULONG           CfgLen;
    char            Product[48];
    char            Serial[32];
    char            Mfg[32];
    UCHAR           Port, RootPort, HcIndex, Depth;
    UD_IFACE        Iface[32];
    UD_PIPE         Pipe[UD_PIPES];
};

typedef struct _UD_BUF {
    struct _UD_BUF *Next;
    UCHAR          *Va;
    ULONG           Pa;
    ULONG           Size;
    int             Class;
} UD_BUF;

typedef struct _UD_REQ {
    USB_XFER        X;
    LIST_ENTRY      Link;
    PIRP            Irp;
    PURB            Urb;
    UD_FUNC        *F;
    UD_PIPE        *Pipe;               /* NULL: default control pipe */
    UCHAR          *Client;             /* the caller's buffer (system address) */
    ULONG           Len, Done, Seg;
    UCHAR           In;
    UCHAR           OnWire;
    UCHAR           Ctl;                /* control transfer */
    UCHAR           Synth;              /* 2: cancelled while on the default pipe */
    UCHAR           CancelSeen;         /* the cancel routine ran and left the IRP to the completion */
    UD_BUF         *Buf;
    UCHAR           Setup[8];
    USB_ISO_PACKET *Iso;
    ULONG           IsoCount;
} UD_REQ;

/* the default control pipe is the device's: one queue per device for all its functions */
typedef struct _UD_EP0 {
    LIST_ENTRY      Wait;
    UD_REQ         *Active;
    BOOLEAN         Retry;
    BOOLEAN         Reset;              /* a control transfer failed (stall): the worker resets the pipe first */
} UD_EP0;

static KSPIN_LOCK UdLock;
static BOOLEAN UdInit;
static LIST_ENTRY Cancelled;            /* IRPs the cancel routine finished: the worker completes them */
static UD_FUNC *Funcs;
static UD_EP0 Ep0[USB_MAX_DEVICES];
static ULONG WinBuild;
static volatile LONG Kick;              /* the worker has queued work to look at */

/* functions to create on the next BusRelations */
typedef struct _UD_WANT {
    UCHAR   Dev, First, Count, Whole;
    ULONG   Gen;
} UD_WANT;
#define UD_MAX_WANT 32
static UD_WANT Want[UD_MAX_WANT];
static int NWant;

/* ------------------------------------------------------------------ */
/* Buffers (contiguous, below 4 GB, never across 64 KB for bulk)        */
/* ------------------------------------------------------------------ */

static const ULONG BufSize[3] = { 0x1000, 0x10000, 0x40000 };
static const int BufMin[3] = { 16, 4, 0 };
static const int BufKeep[3] = { 64, 16, 4 };
static UD_BUF *BufFree[3];
static int BufCount[3], BufNeed[3];
static UD_BUF *BufDrop;

static int BufClass(ULONG Size)
{
    int c;

    for (c = 0; c < 3; c++) {
        if (Size <= BufSize[c]) {
            return c;
        }
    }
    return -1;
}

/* UdLock held */
static UD_BUF *BufGet(ULONG Size)
{
    int c = BufClass(Size);
    UD_BUF *b;

    if (c < 0) {
        return NULL;
    }
    b = BufFree[c];
    if (b == NULL) {
        BufNeed[c]++;
        Kick = 1;
        return NULL;
    }
    BufFree[c] = b->Next;
    BufCount[c]--;
    return b;
}

static void BufPut(UD_BUF *b)
{
    if (BufCount[b->Class] >= BufKeep[b->Class]) {
        b->Next = BufDrop;
        BufDrop = b;
        Kick = 1;
        return;
    }
    b->Next = BufFree[b->Class];
    BufFree[b->Class] = b;
    BufCount[b->Class]++;
}

/* worker (PASSIVE_LEVEL) */
static void BufRefill(void)
{
    KIRQL irql;
    int c;

    for (c = 0; c < 3; c++) {
        for (;;) {
            PHYSICAL_ADDRESS lo, hi, bd;
            UD_BUF *b;
            BOOLEAN more;
            KeAcquireSpinLock(&UdLock, &irql);
            more = BufCount[c] < BufMin[c] + BufNeed[c] && BufCount[c] < BufKeep[c];
            KeReleaseSpinLock(&UdLock, irql);
            if (!more) {
                break;
            }
            b = (UD_BUF *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*b), UD_TAG);
            if (b == NULL) {
                break;
            }
            lo.QuadPart = 0;
            hi.QuadPart = 0xFFFFFFFFULL;
            bd.QuadPart = c < 2 ? 0x10000 : 0;
            b->Va = (UCHAR *)MmAllocateContiguousMemorySpecifyCache(BufSize[c], lo, hi, bd, MmCached);
            if (b->Va == NULL) {
                ExFreePool(b);
                UsbLog(LOG_ERR, "usbdi: no contiguous memory for a %u byte buffer\n", BufSize[c]);
                break;
            }
            b->Pa = MmGetPhysicalAddress(b->Va).LowPart;
            b->Size = BufSize[c];
            b->Class = c;
            KeAcquireSpinLock(&UdLock, &irql);
            b->Next = BufFree[c];
            BufFree[c] = b;
            BufCount[c]++;
            KeReleaseSpinLock(&UdLock, irql);
        }
        KeAcquireSpinLock(&UdLock, &irql);
        BufNeed[c] = 0;
        KeReleaseSpinLock(&UdLock, irql);
    }
    for (;;) {
        UD_BUF *b;
        KeAcquireSpinLock(&UdLock, &irql);
        b = BufDrop;
        if (b != NULL) {
            BufDrop = b->Next;
        }
        KeReleaseSpinLock(&UdLock, irql);
        if (b == NULL) {
            break;
        }
        MmFreeContiguousMemory(b->Va);
        ExFreePool(b);
    }
}

/* ------------------------------------------------------------------ */
/* Completion                                                           */
/* ------------------------------------------------------------------ */

static USBD_STATUS UsbdStatus(LONG St, UD_FUNC *F)
{
    switch (St) {
    case USB_OK:        return USBD_STATUS_SUCCESS;
    case USB_ERR_STALL: return USBD_STATUS_STALL_PID;
    case USB_ERR_BABBLE: return USBD_STATUS_BABBLE_DETECTED;
    case USB_ERR_TIMEOUT: return USBD_STATUS_DEV_NOT_RESPONDING;
    case USB_ERR_NODEV: return USBD_STATUS_DEVICE_GONE;
    case USB_ERR_ABORTED: return (F->Dead || F->Dev == NULL) ? USBD_STATUS_DEVICE_GONE : USBD_STATUS_CANCELED;
    case USB_ERR_NOMEM: return USBD_STATUS_INSUFFICIENT_RESOURCES;
    case USB_ERR_PARAM: return USBD_STATUS_INVALID_PARAMETER;
    default:            return USBD_STATUS_XACT_ERROR;
    }
}

static NTSTATUS NtStatusOf(USBD_STATUS U)
{
    if (USBD_SUCCESS(U)) {
        return STATUS_SUCCESS;
    }
    if (U == USBD_STATUS_DEVICE_GONE) {
        return STATUS_NO_SUCH_DEVICE;
    }
    if (U == USBD_STATUS_CANCELED) {
        return STATUS_CANCELLED;
    }
    if (U == USBD_STATUS_INSUFFICIENT_RESOURCES) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (U == USBD_STATUS_INVALID_PARAMETER || U == USBD_STATUS_INVALID_URB_FUNCTION ||
        U == USBD_STATUS_INVALID_PIPE_HANDLE) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_UNSUCCESSFUL;
}

/* IRPs are completed after UdLock is released */
static void Finish(UD_REQ *r, USBD_STATUS U, PLIST_ENTRY Done)
{
    PURB u = r->Urb;
    PIRP irp = r->Irp;

    u->UrbHeader.Status = U;
    if (UsbDebugLevel >= LOG_TRACE) {
        UsbLog(LOG_TRACE, "usbdi: irp %p done %x%s\n", irp, U, r->CancelSeen ? " (cancelled)" : "");
    }
    if (r->IsoCount) {
        u->UrbIsochronousTransfer.TransferBufferLength = r->Done;
    } else if (u->UrbHeader.Function == URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER) {
        u->UrbBulkOrInterruptTransfer.TransferBufferLength = r->Done;
    } else {
        u->UrbControlTransfer.TransferBufferLength = r->Done;
    }
    if (r->Buf != NULL) {
        BufPut(r->Buf);
        r->Buf = NULL;
    }
    irp->IoStatus.Status = NtStatusOf(U);
    irp->IoStatus.Information = 0;
    if (!r->CancelSeen && IoSetCancelRoutine(irp, NULL) == NULL && irp->Cancel) {
        /* the cancel routine is about to run: it completes the IRP (UdLock orders the two) */
        irp->Tail.Overlay.DriverContext[0] = (PVOID)(ULONG_PTR)-1;
    } else {
        irp->Tail.Overlay.DriverContext[0] = NULL;
        InsertTailList(Done, &irp->Tail.Overlay.ListEntry);
    }
    InterlockedDecrement(&r->F->Pending);
    ExFreePool(r);
}

static void CompleteList(PLIST_ENTRY Done)
{
    while (!IsListEmpty(Done)) {
        PIRP irp = CONTAINING_RECORD(RemoveHeadList(Done), IRP, Tail.Overlay.ListEntry);
        if (UsbDebugLevel >= LOG_TRACE) {
            UsbLog(LOG_TRACE, "usbdi: irp %p completed\n", irp);
        }
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

static BOOLEAN StartReq(UD_REQ *r);

/* the next requests of a pipe (or of a device's control pipe) go on the wire; UdLock held */
static void Pump(UD_FUNC *F, UD_PIPE *P, PLIST_ENTRY Done)
{
    PLIST_ENTRY wait = P ? &P->Wait : &Ep0[F->DevIndex].Wait;

    while (!IsListEmpty(wait)) {
        UD_REQ *r = CONTAINING_RECORD(wait->Flink, UD_REQ, Link);
        if (P ? (P->Type == USB_EP_ISOCH ? P->Active >= UD_ISO_INFLIGHT : P->Active != 0)
              : (Ep0[r->F->DevIndex].Active != NULL || Ep0[r->F->DevIndex].Reset)) {
            return;
        }
        if (r->F->Dead || r->F->Dev == NULL) {
            RemoveEntryList(&r->Link);
            Finish(r, USBD_STATUS_DEVICE_GONE, Done);
            continue;
        }
        if (!StartReq(r)) {
            return;                         /* no buffer yet, or the core uses the control pipe */
        }
        RemoveEntryList(&r->Link);
        if (r->OnWire == 2) {               /* failed at once */
            Finish(r, UsbdStatus(r->X.Status, r->F), Done);
        }
    }
}

static void XferDone(USB_XFER *X)
{
    UD_REQ *r = (UD_REQ *)X->Context;
    UD_FUNC *f = r->F;
    UD_PIPE *p = r->Pipe;
    LIST_ENTRY done;
    KIRQL irql;
    ULONG got = X->Actual;
    USBD_STATUS u;

    InitializeListHead(&done);
    KeAcquireSpinLock(&UdLock, &irql);
    r->OnWire = 0;
    if (p != NULL) {
        p->Active--;
    } else if (Ep0[f->DevIndex].Active == r) {
        Ep0[f->DevIndex].Active = NULL;
    }
    /* a stall leaves the default pipe halted on xHCI: it is reset (in the worker) before the next request,
       as UsbControl does for the core's own requests */
    if (r->Ctl && (X->Status == USB_ERR_STALL || X->Status == USB_ERR_IO || X->Status == USB_ERR_BABBLE)) {
        Ep0[f->DevIndex].Reset = TRUE;
        Kick = 1;
    }
    u = UsbdStatus(X->Status, f);
    if (r->IsoCount) {
        PURB urb = r->Urb;
        ULONG i;
        r->Done = 0;
        for (i = 0; i < r->IsoCount; i++) {
            USBD_ISO_PACKET_DESCRIPTOR *d = &urb->UrbIsochronousTransfer.IsoPacket[i];
            d->Status = r->Iso[i].Status == USB_OK ? USBD_STATUS_SUCCESS : USBD_STATUS_XACT_ERROR;
            if (r->In) {
                d->Length = r->Iso[i].Actual;
                if (r->Iso[i].Actual) {
                    RtlCopyMemory(r->Client + d->Offset, r->Buf->Va + r->Iso[i].Offset, r->Iso[i].Actual);
                }
            }
            r->Done += r->In ? r->Iso[i].Actual : r->Iso[i].Length;
            if (r->Iso[i].Status != USB_OK) {
                urb->UrbIsochronousTransfer.ErrorCount++;
            }
        }
        urb->UrbIsochronousTransfer.StartFrame = X->IsoFrame;
        if (u == USBD_STATUS_XACT_ERROR) {
            u = USBD_STATUS_ISOCH_REQUEST_FAILED;
        }
        Finish(r, u, &done);
    } else {
        if (r->In && got != 0) {
            RtlCopyMemory(r->Client + r->Done, r->Buf->Va + (r->Ctl ? 64 : 0), got);
        }
        r->Done += got;
        if (u == USBD_STATUS_SUCCESS && !r->Ctl && got == r->Seg && r->Done < r->Len) {
            /* the next piece of a long bulk / interrupt transfer */
            if (StartReq(r) && r->OnWire == 2) {
                Finish(r, UsbdStatus(r->X.Status, f), &done);
            } else if (!r->OnWire) {
                InsertHeadList(&p->Wait, &r->Link);         /* waits for its turn again */
            }
        } else {
            if (u == USBD_STATUS_STALL_PID && p != NULL) {
                u = USBD_STATUS_STALL_PID;
            }
            Finish(r, u, &done);
        }
    }
    Pump(f, p, &done);
    KeReleaseSpinLock(&UdLock, irql);
    CompleteList(&done);
    if (Kick) {
        OsWakeWorker();
    }
}

/*
 * Puts a request (its next piece) on the wire; UdLock held.  FALSE: it has to wait
 * (no buffer yet, the control pipe is busy).  OnWire = 1 on the wire, 2 failed at once.
 */
static BOOLEAN StartReq(UD_REQ *r)
{
    UD_FUNC *f = r->F;
    USB_DEV *dev = f->Dev;
    USB_XFER *x = &r->X;
    int st;

    if (dev == NULL) {
        r->X.Status = USB_ERR_NODEV;
        r->OnWire = 2;
        return TRUE;
    }
    RtlZeroMemory(x, sizeof(*x));
    x->Complete = XferDone;
    x->Context = r;
    if (r->Ctl) {
        if (Ep0[f->DevIndex].Active != NULL) {
            return FALSE;
        }
        if (r->Buf == NULL && (r->Buf = BufGet(64 + r->Len)) == NULL) {
            return FALSE;
        }
        RtlCopyMemory(r->Buf->Va, r->Setup, 8);
        if (!r->In && r->Len) {
            RtlCopyMemory(r->Buf->Va + 64, r->Client, r->Len);
        }
        x->Pipe = &dev->Ep0;
        x->SetupPhys = r->Buf->Pa;
        x->Setup = r->Buf->Va;
        x->Buf = r->Buf->Va + 64;
        x->Phys = r->Buf->Pa + 64;
        x->Length = r->Len;
        x->DirIn = r->In;
        st = UsbSubmit(x);
        if (st == USB_ERR_BUSY) {
            Ep0[f->DevIndex].Retry = TRUE;      /* the core's own control transfer: the worker tries again */
            Kick = 1;
            return FALSE;
        }
        if (st == USB_OK) {
            Ep0[f->DevIndex].Active = r;
            r->OnWire = 1;
        } else {
            x->Status = st;
            r->OnWire = 2;
        }
        return TRUE;
    }
    if (r->IsoCount) {
        if (r->Buf == NULL && (r->Buf = BufGet(r->Len)) == NULL) {
            if (BufClass(r->Len) < 0) {
                x->Status = USB_ERR_PARAM;
                r->OnWire = 2;
                return TRUE;
            }
            return FALSE;
        }
        if (!r->In) {
            RtlCopyMemory(r->Buf->Va, r->Client, r->Len);
        }
        x->Pipe = &r->Pipe->P;
        x->Buf = r->Buf->Va;
        x->Phys = r->Buf->Pa;
        x->Length = r->Len;
        x->DirIn = r->In;
        x->Iso = r->Iso;
        x->IsoCount = r->IsoCount;
    } else {
        ULONG maxp = r->Pipe->P.MaxPacket ? r->Pipe->P.MaxPacket : 8;
        ULONG hcmax = dev->Hc->MaxXfer ? dev->Hc->MaxXfer : UD_CHUNK;
        ULONG seg = r->Len - r->Done;
        if (r->Buf == NULL && (r->Buf = BufGet(seg > UD_CHUNK ? UD_CHUNK : (seg ? seg : 1))) == NULL) {
            return FALSE;
        }
        if (seg > r->Buf->Size) {
            seg = r->Buf->Size;
        }
        if (seg > hcmax) {
            seg = hcmax;
        }
        if (seg < r->Len - r->Done) {
            seg -= seg % maxp;              /* every piece but the last is whole packets */
        }
        r->Seg = seg;
        if (!r->In && seg) {
            RtlCopyMemory(r->Buf->Va, r->Client + r->Done, seg);
        }
        x->Pipe = &r->Pipe->P;
        x->Buf = r->Buf->Va;
        x->Phys = r->Buf->Pa;
        x->Length = seg;
        x->DirIn = r->In;
    }
    st = UsbSubmit(x);
    if (st == USB_OK) {
        r->Pipe->Active++;
        r->OnWire = 1;
    } else {
        x->Status = st;
        r->OnWire = 2;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Cancel                                                               */
/* ------------------------------------------------------------------ */

static VOID NTAPI CancelIrp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    UD_REQ *r;
    LIST_ENTRY done;
    KIRQL irql;
    BOOLEAN complete = FALSE;

    (void)Dev;
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    InitializeListHead(&done);
    KeAcquireSpinLock(&UdLock, &irql);
    r = (UD_REQ *)Irp->Tail.Overlay.DriverContext[0];
    if (UsbDebugLevel >= LOG_TRACE) {
        UsbLog(LOG_TRACE, "usbdi: cancel irp %p req %p wire %d\n", Irp, r,
               (r != NULL && r != (UD_REQ *)(ULONG_PTR)-1) ? r->OnWire : -1);
    }
    if (r == (UD_REQ *)(ULONG_PTR)-1) {
        complete = TRUE;                    /* finished meanwhile, left to us */
    } else if (r != NULL) {
        if (!r->OnWire) {
            RemoveEntryList(&r->Link);
            Finish(r, USBD_STATUS_CANCELED, &done);
            complete = Irp->Tail.Overlay.DriverContext[0] == (PVOID)(ULONG_PTR)-1;
        } else {
            /* on the wire: the worker takes it off, the completion completes the IRP */
            r->CancelSeen = 1;
            if (r->Pipe != NULL) {
                r->Pipe->Abort = 1;
            } else {
                Ep0[r->F->DevIndex].Retry = TRUE;
                r->Synth = 2;
            }
            Kick = 1;
        }
    }
    /* never completed here, inside IoCancelIrp: usbccgp's timeout (and usbport's behaviour) expect the
       completion to come afterwards - completed in IoCancelIrp, usbccgp completes the IRP a second time */
    while (!IsListEmpty(&done)) {
        InsertTailList(&Cancelled, RemoveHeadList(&done));
    }
    if (complete) {
        InsertTailList(&Cancelled, &Irp->Tail.Overlay.ListEntry);
    }
    if (!IsListEmpty(&Cancelled)) {
        Kick = 1;
    }
    KeReleaseSpinLock(&UdLock, irql);
    if (Kick) {
        OsWakeWorker();
    }
}

/* ------------------------------------------------------------------ */
/* Submitting                                                           */
/* ------------------------------------------------------------------ */

static UCHAR *ClientBuffer(PVOID Buf, PMDL Mdl)
{
    if (Mdl != NULL) {
        return (UCHAR *)MmGetSystemAddressForMdlSafe(Mdl, HighPagePriority);
    }
    return (UCHAR *)Buf;
}

static NTSTATUS Queue(UD_REQ *r)
{
    UD_FUNC *f = r->F;
    LIST_ENTRY done;
    KIRQL irql;
    PIRP irp = r->Irp;
    PLIST_ENTRY wait;

    InitializeListHead(&done);
    InterlockedIncrement(&f->Pending);
    IoMarkIrpPending(irp);
    irp->Tail.Overlay.DriverContext[0] = r;
    IoSetCancelRoutine(irp, CancelIrp);
    KeAcquireSpinLock(&UdLock, &irql);
    if (irp->Cancel && IoSetCancelRoutine(irp, NULL) != NULL) {
        irp->Tail.Overlay.DriverContext[0] = NULL;
        KeReleaseSpinLock(&UdLock, irql);
        r->Urb->UrbHeader.Status = USBD_STATUS_CANCELED;
        InterlockedDecrement(&f->Pending);
        ExFreePool(r);
        irp->IoStatus.Status = STATUS_CANCELLED;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_PENDING;
    }
    if (UsbDebugLevel >= LOG_TRACE) {
        UsbLog(LOG_TRACE, "usbdi: dev %u irp %p urb %x ep %02x len %u setup %02x %02x %02x%02x %02x%02x\n", f->DevIndex,
               irp, r->Urb->UrbHeader.Function, r->Pipe ? r->Pipe->Addr : 0, r->Len, r->Setup[0], r->Setup[1],
               r->Setup[3], r->Setup[2], r->Setup[7], r->Setup[6]);
    }
    wait = r->Pipe ? &r->Pipe->Wait : &Ep0[f->DevIndex].Wait;
    InsertTailList(wait, &r->Link);
    Pump(f, r->Pipe, &done);
    KeReleaseSpinLock(&UdLock, irql);
    CompleteList(&done);
    if (Kick) {
        OsWakeWorker();
    }
    return STATUS_PENDING;
}

static NTSTATUS Fail(PIRP Irp, PURB Urb, USBD_STATUS U)
{
    if (UsbDebugLevel >= LOG_TRACE) {
        UsbLog(LOG_TRACE, "usbdi: irp %p urb %x failed %x\n", Irp, Urb->UrbHeader.Function, U);
    }
    Urb->UrbHeader.Status = U;
    Irp->IoStatus.Status = NtStatusOf(U);
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return NtStatusOf(U);
}

static NTSTATUS Succeed(PIRP Irp, PURB Urb)
{
    if (UsbDebugLevel >= LOG_TRACE) {
        UsbLog(LOG_TRACE, "usbdi: irp %p urb %x done at once\n", Irp, Urb->UrbHeader.Function);
    }
    Urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

static UD_REQ *NewReq(UD_FUNC *F, PIRP Irp, PURB Urb, ULONG Iso)
{
    UD_REQ *r = (UD_REQ *)ExAllocatePoolWithTag(NonPagedPool, sizeof(UD_REQ) + Iso * sizeof(USB_ISO_PACKET), UD_TAG);

    if (r != NULL) {
        RtlZeroMemory(r, sizeof(*r));
        r->F = F;
        r->Irp = Irp;
        r->Urb = Urb;
        if (Iso) {
            r->Iso = (USB_ISO_PACKET *)(r + 1);
            r->IsoCount = Iso;
        }
    }
    return r;
}

/* a control transfer on the device's default pipe */
static NTSTATUS Control(UD_FUNC *F, PIRP Irp, PURB Urb, UCHAR Type, UCHAR Req, USHORT Value, USHORT Index,
                        PVOID Buf, PMDL Mdl, ULONG Len, BOOLEAN In)
{
    UD_REQ *r;

    if (Len > 0x10000 - 64) {
        return Fail(Irp, Urb, USBD_STATUS_INVALID_PARAMETER);
    }
    /* a function of a composite device sees only its own interfaces in the configuration */
    if (!F->Whole && In && Type == 0x80 && Req == USB_REQ_GET_DESCRIPTOR && (Value >> 8) == USB_DT_CONFIG &&
        F->Cfg != NULL) {
        UCHAR *dst = ClientBuffer(Buf, Mdl);
        ULONG n = Len < F->CfgLen ? Len : F->CfgLen;
        if (dst == NULL) {
            return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
        }
        RtlCopyMemory(dst, F->Cfg, n);
        Urb->UrbControlTransfer.TransferBufferLength = n;
        return Succeed(Irp, Urb);
    }
    r = NewReq(F, Irp, Urb, 0);
    if (r == NULL) {
        return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
    r->Ctl = 1;
    r->In = In;
    r->Len = Len;
    r->Client = Len ? ClientBuffer(Buf, Mdl) : NULL;
    if (Len && r->Client == NULL) {
        ExFreePool(r);
        return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
    r->Setup[0] = (UCHAR)(Type | (In ? 0x80 : 0));
    r->Setup[1] = Req;
    r->Setup[2] = (UCHAR)Value;
    r->Setup[3] = (UCHAR)(Value >> 8);
    r->Setup[4] = (UCHAR)Index;
    r->Setup[5] = (UCHAR)(Index >> 8);
    r->Setup[6] = (UCHAR)Len;
    r->Setup[7] = (UCHAR)(Len >> 8);
    return Queue(r);
}

static UD_PIPE *PipeOf(UD_FUNC *F, USBD_PIPE_HANDLE H)
{
    UD_PIPE *p = (UD_PIPE *)H;

    if (p < &F->Pipe[0] || p >= &F->Pipe[UD_PIPES] || !p->Used || !p->P.Opened) {
        return NULL;
    }
    return p;
}

static NTSTATUS Transfer(UD_FUNC *F, PIRP Irp, PURB Urb)
{
    struct _URB_BULK_OR_INTERRUPT_TRANSFER *t = &Urb->UrbBulkOrInterruptTransfer;
    UD_PIPE *p = PipeOf(F, t->PipeHandle);
    UD_REQ *r;

    if (p == NULL) {
        return Fail(Irp, Urb, USBD_STATUS_INVALID_PIPE_HANDLE);
    }
    r = NewReq(F, Irp, Urb, 0);
    if (r == NULL) {
        return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
    r->Pipe = p;
    r->In = p->P.DirIn;
    r->Len = t->TransferBufferLength;
    r->Client = r->Len ? ClientBuffer(t->TransferBuffer, t->TransferBufferMDL) : NULL;
    if (r->Len && r->Client == NULL) {
        ExFreePool(r);
        return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
    return Queue(r);
}

static NTSTATUS Isoch(UD_FUNC *F, PIRP Irp, PURB Urb)
{
    struct _URB_ISOCH_TRANSFER *t = &Urb->UrbIsochronousTransfer;
    UD_PIPE *p = PipeOf(F, t->PipeHandle);
    UD_REQ *r;
    ULONG i, n = t->NumberOfPackets;

    if (p == NULL || p->Type != USB_EP_ISOCH) {
        return Fail(Irp, Urb, USBD_STATUS_INVALID_PIPE_HANDLE);
    }
    if (n == 0 || n > 1024 || t->TransferBufferLength == 0) {
        return Fail(Irp, Urb, USBD_STATUS_INVALID_PARAMETER);
    }
    r = NewReq(F, Irp, Urb, n);
    if (r == NULL) {
        return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
    r->Pipe = p;
    r->In = p->P.DirIn;
    r->Len = t->TransferBufferLength;
    r->Client = ClientBuffer(t->TransferBuffer, t->TransferBufferMDL);
    if (r->Client == NULL) {
        ExFreePool(r);
        return Fail(Irp, Urb, USBD_STATUS_INSUFFICIENT_RESOURCES);
    }
    t->ErrorCount = 0;
    for (i = 0; i < n; i++) {
        ULONG off = t->IsoPacket[i].Offset;
        ULONG end = i + 1 < n ? t->IsoPacket[i + 1].Offset : t->TransferBufferLength;
        if (end < off || end > t->TransferBufferLength) {
            ExFreePool(r);
            return Fail(Irp, Urb, USBD_STATUS_INVALID_PARAMETER);
        }
        r->Iso[i].Offset = off;
        r->Iso[i].Length = (USHORT)(end - off);
        t->IsoPacket[i].Length = 0;
        t->IsoPacket[i].Status = USBD_STATUS_NOT_ACCESSED;
    }
    return Queue(r);
}

/* ------------------------------------------------------------------ */
/* Configuration (worker)                                               */
/* ------------------------------------------------------------------ */

static const UCHAR *FindIface(USB_DEV *Dev, UCHAR Num, UCHAR Alt)
{
    const UCHAR *end = Dev->Config + Dev->ConfigLen, *d = Dev->Config;

    while ((d = UsbNextDesc(d, end)) != NULL) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[2] == Num && d[3] == Alt) {
            return d;
        }
    }
    return NULL;
}

static BOOLEAN Owns(UD_FUNC *F, UCHAR If)
{
    return F->Whole || (If >= F->First && If < F->First + F->Count);
}

/* every pipe of interface If (0xFF: all) closes; worker */
static void ClosePipes(UD_FUNC *F, UCHAR If)
{
    LIST_ENTRY done;
    KIRQL irql;
    int i;

    InitializeListHead(&done);
    for (i = 0; i < UD_PIPES; i++) {
        UD_PIPE *p = &F->Pipe[i];
        if (!p->Used || (If != 0xFF && p->If != If)) {
            continue;
        }
        if (p->P.Opened) {
            UsbClosePipe(&p->P);            /* what is on the wire completes (aborted) */
        }
        KeAcquireSpinLock(&UdLock, &irql);
        while (!IsListEmpty(&p->Wait)) {
            UD_REQ *r = CONTAINING_RECORD(RemoveHeadList(&p->Wait), UD_REQ, Link);
            Finish(r, F->Dev ? USBD_STATUS_CANCELED : USBD_STATUS_DEVICE_GONE, &done);
        }
        p->Used = 0;
        KeReleaseSpinLock(&UdLock, irql);
    }
    CompleteList(&done);
}

static NTSTATUS OpenIface(UD_FUNC *F, PUSBD_INTERFACE_INFORMATION Info)
{
    USB_DEV *dev = F->Dev;
    const UCHAR *d, *end = dev->Config + dev->ConfigLen;
    ULONG i, n = 0, want;
    int slot = 0;

    d = FindIface(dev, Info->InterfaceNumber, Info->AlternateSetting);
    if (d == NULL || !Owns(F, Info->InterfaceNumber)) {
        return STATUS_INVALID_PARAMETER;
    }
    want = d[4];
    /* GET_USBD_INTERFACE_SIZE(n): an interface without endpoints has no pipe entry at all */
    if (Info->Length < FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) + want * sizeof(USBD_PIPE_INFORMATION)) {
        UsbLog(LOG_ERR, "usbdi: device %u: interface %u.%u: %u bytes for %u pipe(s)\n", dev->Index,
               Info->InterfaceNumber, Info->AlternateSetting, Info->Length, want);
        return STATUS_BUFFER_TOO_SMALL;
    }
    Info->Class = d[5];
    Info->SubClass = d[6];
    Info->Protocol = d[7];
    Info->Reserved = 0;
    F->Iface[Info->InterfaceNumber & 31].F = F;
    F->Iface[Info->InterfaceNumber & 31].Num = Info->InterfaceNumber;
    F->Iface[Info->InterfaceNumber & 31].Alt = Info->AlternateSetting;
    Info->InterfaceHandle = &F->Iface[Info->InterfaceNumber & 31];
    for (d = UsbNextDesc(d, end); d != NULL && n < want; d = UsbNextDesc(d, end)) {
        UD_PIPE *p;
        USHORT mps;
        if (d[1] == USB_DT_INTERFACE) {
            break;
        }
        if (d[1] != USB_DT_ENDPOINT || d[0] < 7) {
            continue;
        }
        while (slot < UD_PIPES && F->Pipe[slot].Used) {
            slot++;
        }
        if (slot >= UD_PIPES) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        p = &F->Pipe[slot];
        RtlZeroMemory(p, sizeof(*p));
        InitializeListHead(&p->Wait);
        p->F = F;
        p->If = Info->InterfaceNumber;
        p->Addr = d[2];
        p->Type = (UCHAR)(d[3] & 3);
        if (UsbOpenPipe(dev, &p->P, d) != USB_OK) {
            UsbLog(LOG_ERR, "usbdi: device %u: endpoint %02x does not open\n", dev->Index, d[2]);
            return STATUS_UNSUCCESSFUL;
        }
        p->Used = 1;
        mps = GET16(d + 4);
        i = n++;
        Info->Pipes[i].MaximumPacketSize = (USHORT)((mps & 0x7FF) * (dev->Speed == USB_SPEED_HIGH &&
                                           (p->Type == USB_EP_ISOCH || p->Type == USB_EP_INTERRUPT) ? ((mps >> 11) & 3) + 1 : 1));
        Info->Pipes[i].EndpointAddress = d[2];
        Info->Pipes[i].Interval = d[6];
        Info->Pipes[i].PipeType = (USBD_PIPE_TYPE)p->Type;
        Info->Pipes[i].PipeHandle = (USBD_PIPE_HANDLE)p;
        if (Info->Pipes[i].MaximumTransferSize == 0 || Info->Pipes[i].MaximumTransferSize > 0x400000) {
            Info->Pipes[i].MaximumTransferSize = 0x10000;
        }
        p->MaxXfer = Info->Pipes[i].MaximumTransferSize;
    }
    Info->NumberOfPipes = n;
    return STATUS_SUCCESS;
}

typedef struct _UD_JOB {
    UD_FUNC        *F;
    PIRP            Irp;
    PURB            Urb;
    ULONG           Code;               /* IOCTL; 0 = URB */
    NTSTATUS        St;
    PIO_WORKITEM    Wi;
} UD_JOB;

static void SelectConfig(UD_JOB *J)
{
    UD_FUNC *f = J->F;
    PURB u = J->Urb;
    struct _URB_SELECT_CONFIGURATION *s = &u->UrbSelectConfiguration;
    PUSBD_INTERFACE_INFORMATION info;
    UCHAR *end = (UCHAR *)u + u->UrbHeader.Length;
    NTSTATUS st = STATUS_SUCCESS;

    ClosePipes(f, 0xFF);
    if (s->ConfigurationDescriptor == NULL) {
        if (f->Whole) {
            UsbControl(f->Dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEVICE, USB_REQ_SET_CONFIGURATION, 0, 0, 0, NULL,
                       1000, NULL);
        }
        f->Config = 0;
        u->UrbHeader.Status = USBD_STATUS_SUCCESS;
        J->St = STATUS_SUCCESS;
        return;
    }
    if (f->Whole && UsbControl(f->Dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEVICE, USB_REQ_SET_CONFIGURATION,
                               f->Dev->ConfigValue, 0, 0, NULL, 1000, NULL) != USB_OK) {
        u->UrbHeader.Status = USBD_STATUS_SET_CONFIG_FAILED;
        J->St = STATUS_UNSUCCESSFUL;
        return;
    }
    f->Config = f->Dev->ConfigValue;
    for (info = &s->Interface; (UCHAR *)info + FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes) <= end && info->Length;
         info = (PUSBD_INTERFACE_INFORMATION)((UCHAR *)info + info->Length)) {
        if (info->AlternateSetting != 0 &&
            UsbSetInterface(f->Dev, info->InterfaceNumber, info->AlternateSetting) != USB_OK) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        st = OpenIface(f, info);
        if (!NT_SUCCESS(st)) {
            break;
        }
    }
    s->ConfigurationHandle = (USBD_CONFIGURATION_HANDLE)f;
    u->UrbHeader.Status = NT_SUCCESS(st) ? USBD_STATUS_SUCCESS : USBD_STATUS_INAVLID_CONFIGURATION_DESCRIPTOR;
    J->St = st;
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "usbdi: device %u: configuration failed (%x)\n", f->DevIndex, st);
    }
}

static void SelectIface(UD_JOB *J)
{
    UD_FUNC *f = J->F;
    PUSBD_INTERFACE_INFORMATION info = &J->Urb->UrbSelectInterface.Interface;
    NTSTATUS st;

    if (!Owns(f, info->InterfaceNumber)) {
        J->Urb->UrbHeader.Status = USBD_STATUS_INVALID_PARAMETER;
        J->St = STATUS_INVALID_PARAMETER;
        return;
    }
    ClosePipes(f, info->InterfaceNumber);
    if (UsbSetInterface(f->Dev, info->InterfaceNumber, info->AlternateSetting) != USB_OK &&
        info->AlternateSetting != 0) {
        J->Urb->UrbHeader.Status = USBD_STATUS_STALL_PID;
        J->St = STATUS_UNSUCCESSFUL;
        return;
    }
    st = OpenIface(f, info);
    J->Urb->UrbHeader.Status = NT_SUCCESS(st) ? USBD_STATUS_SUCCESS : USBD_STATUS_INVALID_PARAMETER;
    J->St = st;
}

/* the requests of a pipe end (aborted); worker */
static void AbortPipe(UD_PIPE *P)
{
    LIST_ENTRY done;
    KIRQL irql;

    InitializeListHead(&done);
    KeAcquireSpinLock(&UdLock, &irql);
    while (!IsListEmpty(&P->Wait)) {
        UD_REQ *r = CONTAINING_RECORD(RemoveHeadList(&P->Wait), UD_REQ, Link);
        Finish(r, USBD_STATUS_CANCELED, &done);
    }
    P->Abort = 0;
    KeReleaseSpinLock(&UdLock, irql);
    CompleteList(&done);
    if (P->P.Opened && P->P.Cur != NULL) {
        UsbCancelPipe(&P->P);
    }
}

static void PipeRequest(UD_JOB *J)
{
    UD_PIPE *p = PipeOf(J->F, J->Urb->UrbPipeRequest.PipeHandle);
    USHORT fn = J->Urb->UrbHeader.Function;

    if (p == NULL) {
        J->Urb->UrbHeader.Status = USBD_STATUS_INVALID_PIPE_HANDLE;
        J->St = STATUS_INVALID_PARAMETER;
        return;
    }
    if (fn == URB_FUNCTION_ABORT_PIPE) {
        AbortPipe(p);
    } else {
        AbortPipe(p);
        if (fn != URB_FUNCTION_SYNC_RESET_PIPE && p->Type != USB_EP_ISOCH) {
            UsbClearHalt(&p->P);            /* clears the halt on the device and the data toggle */
        } else {
            UsbResetPipeToggle(&p->P);
        }
    }
    J->Urb->UrbHeader.Status = USBD_STATUS_SUCCESS;
    J->St = STATUS_SUCCESS;
}

/* IOCTL_INTERNAL_USB_RESET_PORT: the device is configured again as it was */
static void ResetPort(UD_JOB *J)
{
    UD_FUNC *f = J->F;
    int i;

    for (i = 0; i < UD_PIPES; i++) {
        if (f->Pipe[i].Used) {
            AbortPipe(&f->Pipe[i]);
            UsbResetPipeToggle(&f->Pipe[i].P);
        }
    }
    if (f->Whole && f->Config) {
        UsbControl(f->Dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEVICE, USB_REQ_SET_CONFIGURATION, f->Config, 0, 0, NULL,
                   1000, NULL);
        for (i = 0; i < 32; i++) {
            if (f->Iface[i].F != NULL && f->Iface[i].Alt != 0) {
                UsbSetInterface(f->Dev, f->Iface[i].Num, f->Iface[i].Alt);
            }
        }
    }
    J->St = STATUS_SUCCESS;
}

static void JobW(void *A)
{
    UD_JOB *j = (UD_JOB *)A;

    if (j->F->Dead || j->F->Dev == NULL) {
        if (j->Urb != NULL) {
            j->Urb->UrbHeader.Status = USBD_STATUS_DEVICE_GONE;
        }
        j->St = STATUS_NO_SUCH_DEVICE;
        return;
    }
    if (j->Code == IOCTL_INTERNAL_USB_RESET_PORT) {
        ResetPort(j);
        return;
    }
    switch (j->Urb->UrbHeader.Function) {
    case URB_FUNCTION_SELECT_CONFIGURATION:
        SelectConfig(j);
        break;
    case URB_FUNCTION_SELECT_INTERFACE:
        SelectIface(j);
        break;
    default:
        PipeRequest(j);
        break;
    }
}

static VOID NTAPI JobWork(PDEVICE_OBJECT Dev, PVOID Ctx)
{
    UD_JOB *j = (UD_JOB *)Ctx;
    PIO_WORKITEM wi = j->Wi;

    (void)Dev;
    WdmRunInWorker(JobW, j);
    j->Irp->IoStatus.Status = j->St;
    IoCompleteRequest(j->Irp, IO_NO_INCREMENT);
    InterlockedDecrement(&j->F->Pending);
    ExFreePool(j);
    IoFreeWorkItem(wi);
}

/* runs in the worker: now (PASSIVE_LEVEL) or through a work item */
static NTSTATUS Job(UD_FUNC *F, PIRP Irp, PURB Urb, ULONG Code)
{
    UD_JOB *j;

    if (KeGetCurrentIrql() == PASSIVE_LEVEL) {
        UD_JOB jj;
        RtlZeroMemory(&jj, sizeof(jj));
        jj.F = F;
        jj.Irp = Irp;
        jj.Urb = Urb;
        jj.Code = Code;
        if (UsbDebugLevel >= LOG_TRACE) {
            UsbLog(LOG_TRACE, "usbdi: irp %p job %x\n", Irp, Urb ? Urb->UrbHeader.Function : Code);
        }
        WdmRunInWorker(JobW, &jj);
        Irp->IoStatus.Status = jj.St;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return jj.St;
    }
    j = (UD_JOB *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*j), UD_TAG);
    if (j == NULL || (j->Wi = IoAllocateWorkItem(F->Pdo)) == NULL) {
        if (j != NULL) {
            ExFreePool(j);
        }
        Irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    j->F = F;
    j->Irp = Irp;
    j->Urb = Urb;
    j->Code = Code;
    InterlockedIncrement(&F->Pending);
    IoMarkIrpPending(Irp);
    IoQueueWorkItem(j->Wi, JobWork, DelayedWorkQueue, j);
    return STATUS_PENDING;
}

/* ------------------------------------------------------------------ */
/* URBs                                                                 */
/* ------------------------------------------------------------------ */

static NTSTATUS Urb(UD_FUNC *F, PIRP Irp, PURB U)
{
    USHORT fn = U->UrbHeader.Function;
    struct _URB_CONTROL_DESCRIPTOR_REQUEST *dr = &U->UrbControlDescriptorRequest;
    struct _URB_CONTROL_FEATURE_REQUEST *fr = &U->UrbControlFeatureRequest;
    struct _URB_CONTROL_GET_STATUS_REQUEST *gs = &U->UrbControlGetStatusRequest;
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST *vc = &U->UrbControlVendorClassRequest;

    if (F->Dead || F->Dev == NULL) {
        return Fail(Irp, U, USBD_STATUS_DEVICE_GONE);
    }
    U->UrbHeader.Status = USBD_STATUS_PENDING;
    switch (fn) {
    case URB_FUNCTION_SELECT_CONFIGURATION:
    case URB_FUNCTION_SELECT_INTERFACE:
    case URB_FUNCTION_ABORT_PIPE:
    case URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL:
    case URB_FUNCTION_SYNC_RESET_PIPE:
    case URB_FUNCTION_SYNC_CLEAR_STALL:
        return Job(F, Irp, U, 0);
    case URB_FUNCTION_BULK_OR_INTERRUPT_TRANSFER:
        return Transfer(F, Irp, U);
    case URB_FUNCTION_ISOCH_TRANSFER:
        return Isoch(F, Irp, U);
    case URB_FUNCTION_CONTROL_TRANSFER: {
        struct _URB_CONTROL_TRANSFER *c = &U->UrbControlTransfer;
        UCHAR *s = c->SetupPacket;
        BOOLEAN in = (c->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0 || (s[0] & 0x80) != 0;
        if (c->PipeHandle != NULL && PipeOf(F, c->PipeHandle) == NULL) {
            return Fail(Irp, U, USBD_STATUS_INVALID_PIPE_HANDLE);
        }
        return Control(F, Irp, U, (UCHAR)(s[0] & 0x7F), s[1], (USHORT)(s[2] | (s[3] << 8)),
                       (USHORT)(s[4] | (s[5] << 8)), c->TransferBuffer, c->TransferBufferMDL,
                       c->TransferBufferLength, in);
    }
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE:
    case URB_FUNCTION_GET_DESCRIPTOR_FROM_ENDPOINT: {
        UCHAR rcp = fn == URB_FUNCTION_GET_DESCRIPTOR_FROM_DEVICE ? 0 :
                    fn == URB_FUNCTION_GET_DESCRIPTOR_FROM_INTERFACE ? 1 : 2;
        return Control(F, Irp, U, rcp, USB_REQ_GET_DESCRIPTOR, (USHORT)((dr->DescriptorType << 8) | dr->Index),
                       dr->LanguageId, dr->TransferBuffer, dr->TransferBufferMDL, dr->TransferBufferLength, TRUE);
    }
    case URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE:
    case URB_FUNCTION_SET_DESCRIPTOR_TO_ENDPOINT: {
        UCHAR rcp = fn == URB_FUNCTION_SET_DESCRIPTOR_TO_DEVICE ? 0 :
                    fn == URB_FUNCTION_SET_DESCRIPTOR_TO_INTERFACE ? 1 : 2;
        return Control(F, Irp, U, rcp, 7, (USHORT)((dr->DescriptorType << 8) | dr->Index), dr->LanguageId,
                       dr->TransferBuffer, dr->TransferBufferMDL, dr->TransferBufferLength, FALSE);
    }
    case URB_FUNCTION_SET_FEATURE_TO_DEVICE:
    case URB_FUNCTION_SET_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_SET_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_SET_FEATURE_TO_OTHER:
    case URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE:
    case URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT:
    case URB_FUNCTION_CLEAR_FEATURE_TO_OTHER: {
        BOOLEAN set = fn == URB_FUNCTION_SET_FEATURE_TO_DEVICE || fn == URB_FUNCTION_SET_FEATURE_TO_INTERFACE ||
                      fn == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT || fn == URB_FUNCTION_SET_FEATURE_TO_OTHER;
        UCHAR rcp = (fn == URB_FUNCTION_SET_FEATURE_TO_DEVICE || fn == URB_FUNCTION_CLEAR_FEATURE_TO_DEVICE) ? 0 :
                    (fn == URB_FUNCTION_SET_FEATURE_TO_INTERFACE || fn == URB_FUNCTION_CLEAR_FEATURE_TO_INTERFACE) ? 1 :
                    (fn == URB_FUNCTION_SET_FEATURE_TO_ENDPOINT || fn == URB_FUNCTION_CLEAR_FEATURE_TO_ENDPOINT) ? 2 : 3;
        return Control(F, Irp, U, rcp, set ? USB_REQ_SET_FEATURE : USB_REQ_CLEAR_FEATURE, fr->FeatureSelector,
                       fr->Index, NULL, NULL, 0, FALSE);
    }
    case URB_FUNCTION_GET_STATUS_FROM_DEVICE:
    case URB_FUNCTION_GET_STATUS_FROM_INTERFACE:
    case URB_FUNCTION_GET_STATUS_FROM_ENDPOINT:
    case URB_FUNCTION_GET_STATUS_FROM_OTHER: {
        UCHAR rcp = fn == URB_FUNCTION_GET_STATUS_FROM_DEVICE ? 0 : fn == URB_FUNCTION_GET_STATUS_FROM_INTERFACE ? 1 :
                    fn == URB_FUNCTION_GET_STATUS_FROM_ENDPOINT ? 2 : 3;
        return Control(F, Irp, U, rcp, USB_REQ_GET_STATUS, 0, gs->Index, gs->TransferBuffer, gs->TransferBufferMDL,
                       gs->TransferBufferLength < 2 ? gs->TransferBufferLength : 2, TRUE);
    }
    case URB_FUNCTION_VENDOR_DEVICE:
    case URB_FUNCTION_VENDOR_INTERFACE:
    case URB_FUNCTION_VENDOR_ENDPOINT:
    case URB_FUNCTION_VENDOR_OTHER:
    case URB_FUNCTION_CLASS_DEVICE:
    case URB_FUNCTION_CLASS_INTERFACE:
    case URB_FUNCTION_CLASS_ENDPOINT:
    case URB_FUNCTION_CLASS_OTHER: {
        BOOLEAN vendor = fn == URB_FUNCTION_VENDOR_DEVICE || fn == URB_FUNCTION_VENDOR_INTERFACE ||
                         fn == URB_FUNCTION_VENDOR_ENDPOINT || fn == URB_FUNCTION_VENDOR_OTHER;
        UCHAR rcp = (fn == URB_FUNCTION_VENDOR_DEVICE || fn == URB_FUNCTION_CLASS_DEVICE) ? 0 :
                    (fn == URB_FUNCTION_VENDOR_INTERFACE || fn == URB_FUNCTION_CLASS_INTERFACE) ? 1 :
                    (fn == URB_FUNCTION_VENDOR_ENDPOINT || fn == URB_FUNCTION_CLASS_ENDPOINT) ? 2 : 3;
        return Control(F, Irp, U, (UCHAR)((vendor ? USB_RT_VENDOR : USB_RT_CLASS) | rcp | vc->RequestTypeReservedBits),
                       vc->Request, vc->Value, vc->Index, vc->TransferBuffer, vc->TransferBufferMDL,
                       vc->TransferBufferLength, (vc->TransferFlags & USBD_TRANSFER_DIRECTION_IN) != 0);
    }
    case URB_FUNCTION_GET_CONFIGURATION: {
        struct _URB_CONTROL_GET_CONFIGURATION_REQUEST *g = &U->UrbControlGetConfigurationRequest;
        return Control(F, Irp, U, 0, USB_REQ_GET_CONFIGURATION, 0, 0, g->TransferBuffer, g->TransferBufferMDL,
                       g->TransferBufferLength ? 1 : 0, TRUE);
    }
    case URB_FUNCTION_GET_INTERFACE: {
        struct _URB_CONTROL_GET_INTERFACE_REQUEST *g = &U->UrbControlGetInterfaceRequest;
        return Control(F, Irp, U, 1, 10, 0, g->Interface, g->TransferBuffer, g->TransferBufferMDL,
                       g->TransferBufferLength ? 1 : 0, TRUE);
    }
    case URB_FUNCTION_GET_MS_FEATURE_DESCRIPTOR: {
        /* the vendor code is in the MS OS string descriptor (0xEE); the usual devices use the request the
           descriptor names - a vendor request with wIndex = the feature */
        struct _URB_OS_FEATURE_DESCRIPTOR_REQUEST *m = &U->UrbOSFeatureDescriptorRequest;
        return Control(F, Irp, U, (UCHAR)(USB_RT_VENDOR | (m->Recipient & 3)), 0x01 /* common vendor code */,
                       (USHORT)(m->InterfaceNumber | (m->MS_PageIndex << 8)), m->MS_FeatureDescriptorIndex,
                       m->TransferBuffer, m->TransferBufferMDL, m->TransferBufferLength, TRUE);
    }
    case URB_FUNCTION_GET_CURRENT_FRAME_NUMBER:
        U->UrbGetCurrentFrameNumber.FrameNumber = OsTimeMs();
        return Succeed(Irp, U);
    case URB_FUNCTION_GET_FRAME_LENGTH:
        U->UrbGetFrameLength.FrameLength = 12000 - 1;
        U->UrbGetFrameLength.FrameNumber = OsTimeMs();
        return Succeed(Irp, U);
    case URB_FUNCTION_TAKE_FRAME_LENGTH_CONTROL:
    case URB_FUNCTION_RELEASE_FRAME_LENGTH_CONTROL:
    case URB_FUNCTION_SET_FRAME_LENGTH:
        return Succeed(Irp, U);
    default:
        UsbLog(LOG_DBG, "usbdi: device %u: URB function %x not supported\n", F->DevIndex, fn);
        return Fail(Irp, U, USBD_STATUS_INVALID_URB_FUNCTION);
    }
}

/* IRP_MJ_INTERNAL_DEVICE_CONTROL of a USB PDO */
NTSTATUS UsbdiInternal(UD_FUNC *F, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS st;

    if (code != IOCTL_INTERNAL_USB_SUBMIT_URB) {
        UsbLog(LOG_DBG, "usbdi: device %u: irp %p ioctl %x\n", F->DevIndex, Irp, code);
    }
    switch (code) {
    case IOCTL_INTERNAL_USB_SUBMIT_URB:
        return Urb(F, Irp, (PURB)sp->Parameters.Others.Argument1);
    case IOCTL_INTERNAL_USB_GET_PORT_STATUS:
        if (sp->Parameters.Others.Argument1 != NULL) {
            *(PULONG)sp->Parameters.Others.Argument1 = (F->Dead || F->Dev == NULL) ? 0 :
                                                       (USBD_PORT_ENABLED | USBD_PORT_CONNECTED);
        }
        st = STATUS_SUCCESS;
        break;
    case IOCTL_INTERNAL_USB_RESET_PORT:
        if (F->Dead || F->Dev == NULL) {
            st = STATUS_NO_SUCH_DEVICE;
            break;
        }
        return Job(F, Irp, NULL, code);
    case IOCTL_INTERNAL_USB_CYCLE_PORT:
        /* the device leaves and comes back: Windows loads its driver anew */
        F->Dead = TRUE;
        Kick = 1;
        OsWakeWorker();
        st = STATUS_SUCCESS;
        break;
    case IOCTL_INTERNAL_USB_ENABLE_PORT:
        st = STATUS_SUCCESS;
        break;
    case IOCTL_INTERNAL_USB_GET_HUB_COUNT:
        if (sp->Parameters.Others.Argument1 != NULL) {
            (*(PULONG)sp->Parameters.Others.Argument1)++;
        }
        st = STATUS_SUCCESS;
        break;
    case IOCTL_INTERNAL_USB_GET_BUS_INFO:
        if (Irp->AssociatedIrp.SystemBuffer != NULL &&
            sp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(USB_BUS_NOTIFICATION)) {
            PUSB_BUS_NOTIFICATION b = (PUSB_BUS_NOTIFICATION)Irp->AssociatedIrp.SystemBuffer;
            RtlZeroMemory(b, sizeof(*b));
            b->NotificationType = 0;
            b->TotalBandwidth = F->Speed == USB_SPEED_HIGH ? 480000 : 12000;
            b->ConsumedBandwidth = 0;
            b->ControllerNameLength = 0;
            Irp->IoStatus.Information = sizeof(*b);
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    default:
        st = STATUS_NOT_SUPPORTED;      /* idle notification (selective suspend), hub and controller names */
        break;
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

/* ------------------------------------------------------------------ */
/* USBDI bus interface                                                  */
/* ------------------------------------------------------------------ */

static VOID NTAPI IfRef(PVOID Ctx) { (void)Ctx; }

static VOID NTAPI IfGetVersion(PVOID Ctx, PUSBD_VERSION_INFORMATION Info, PULONG Speed)
{
    UD_FUNC *f = (UD_FUNC *)Ctx;

    UsbLog(LOG_DBG, "usbdi: device %u: USBDI version asked\n", f->DevIndex);
    if (Info != NULL) {
        Info->USBDI_Version = 0x500;        /* Windows XP */
        Info->Supported_USB_Version = 0x200;
    }
    if (Speed != NULL) {
        *Speed = f->Speed == USB_SPEED_HIGH || f->Speed == USB_SPEED_SUPER ? 2 /* USB 2.0 */ : 1;
    }
}

static NTSTATUS NTAPI IfBusTime(PVOID Ctx, PULONG Frame)
{
    (void)Ctx;
    if (Frame != NULL) {
        *Frame = OsTimeMs();
    }
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI IfIsoOut(PVOID Ctx, PURB U)
{
    (void)Ctx;
    (void)U;
    return STATUS_NOT_SUPPORTED;
}

/* level 0: bandwidth; level 1: bandwidth and the controller's name (usbvideo.sys asks for it) */
static NTSTATUS NTAPI IfBusInfo(PVOID Ctx, ULONG Level, PVOID Buf, PULONG Len, PULONG Actual)
{
    static const WCHAR name[] = L"USBNT";
    UD_FUNC *f = (UD_FUNC *)Ctx;
    ULONG fixed = Level == 0 ? sizeof(USB_BUS_INFORMATION_LEVEL_0) :
                  FIELD_OFFSET(USB_BUS_INFORMATION_LEVEL_1, ControllerNameUnicodeString);
    ULONG need = fixed + (Level == 1 ? sizeof(name) - sizeof(WCHAR) : 0);
    ULONG bw = (f->Speed == USB_SPEED_HIGH || f->Speed == USB_SPEED_SUPER) ? 480000 : 12000;

    UsbLog(LOG_DBG, "usbdi: device %u: bus information level %u, %u bytes\n", f->DevIndex, Level, Len ? *Len : 0);
    if (Actual != NULL) {
        *Actual = need;
    }
    if (Level > 1) {
        return STATUS_NOT_SUPPORTED;
    }
    if (Len == NULL || Buf == NULL || *Len < fixed) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (Level == 0) {
        PUSB_BUS_INFORMATION_LEVEL_0 b = (PUSB_BUS_INFORMATION_LEVEL_0)Buf;
        b->TotalBandwidth = bw;
        b->ConsumedBandwidth = 0;
    } else {
        PUSB_BUS_INFORMATION_LEVEL_1 b = (PUSB_BUS_INFORMATION_LEVEL_1)Buf;
        ULONG room = *Len - fixed, n = sizeof(name) - sizeof(WCHAR);
        b->TotalBandwidth = bw;
        b->ConsumedBandwidth = 0;
        b->ControllerNameLength = n;
        RtlCopyMemory(b->ControllerNameUnicodeString, name, room < n ? room : n);
        if (room < n) {
            return STATUS_BUFFER_TOO_SMALL;
        }
    }
    return STATUS_SUCCESS;
}

static BOOLEAN NTAPI IfHighSpeed(PVOID Ctx)
{
    UD_FUNC *f = (UD_FUNC *)Ctx;

    return f->Speed == USB_SPEED_HIGH || f->Speed == USB_SPEED_SUPER;
}

static NTSTATUS QueryInterface(UD_FUNC *F, PIO_STACK_LOCATION Sp, NTSTATUS Cur)
{
    PUSB_BUS_INTERFACE_USBDI_V1 i = (PUSB_BUS_INTERFACE_USBDI_V1)Sp->Parameters.QueryInterface.Interface;
    static const GUID usbdi = { 0xb1a96a13, 0x3de0, 0x4574, { 0x9b, 0x01, 0xc0, 0x8f, 0xea, 0xb3, 0x18, 0xd6 } };
    USHORT ver = Sp->Parameters.QueryInterface.Version;

    if (Sp->Parameters.QueryInterface.InterfaceType != NULL) {
        const GUID *g = Sp->Parameters.QueryInterface.InterfaceType;
        UsbLog(LOG_DBG, "usbdi: device %u: interface %08x-%04x-%04x-%02x%02x version %u size %u\n", F->DevIndex,
               g->Data1, g->Data2, g->Data3, g->Data4[0], g->Data4[1], Sp->Parameters.QueryInterface.Version,
               Sp->Parameters.QueryInterface.Size);
    }
    if (Sp->Parameters.QueryInterface.InterfaceType == NULL ||
        RtlCompareMemory(Sp->Parameters.QueryInterface.InterfaceType, &usbdi, sizeof(GUID)) != sizeof(GUID)) {
        return Cur;
    }
    if (ver > 1) {
        ver = 1;
    }
    if (Sp->Parameters.QueryInterface.Size < (ver == 0 ? sizeof(USB_BUS_INTERFACE_USBDI_V0) :
                                              sizeof(USB_BUS_INTERFACE_USBDI_V1))) {
        return STATUS_INVALID_PARAMETER;
    }
    i->Size = (USHORT)(ver == 0 ? sizeof(USB_BUS_INTERFACE_USBDI_V0) : sizeof(USB_BUS_INTERFACE_USBDI_V1));
    i->Version = ver;
    i->BusContext = F;
    i->InterfaceReference = IfRef;
    i->InterfaceDereference = IfRef;
    i->GetUSBDIVersion = IfGetVersion;
    i->QueryBusTime = IfBusTime;
    i->SubmitIsoOutUrb = IfIsoOut;
    i->QueryBusInformation = IfBusInfo;
    if (ver >= 1) {
        i->IsDeviceHighSpeed = IfHighSpeed;
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Plug and Play, power                                                 */
/* ------------------------------------------------------------------ */

static WCHAR *WStr(WCHAR *D, const char *S)
{
    while (*S) {
        *D++ = (WCHAR)(UCHAR)*S++;
    }
    *D = 0;
    return D;
}

static WCHAR *WHex(WCHAR *D, ULONG V, int Digits)
{
    int i;

    for (i = Digits - 1; i >= 0; i--) {
        ULONG n = (V >> (4 * i)) & 15;
        *D++ = (WCHAR)(n < 10 ? L'0' + n : L'A' + n - 10);
    }
    *D = 0;
    return D;
}

static WCHAR *WDec(WCHAR *D, ULONG V, int Digits)
{
    int i;

    for (i = Digits - 1; i >= 0; i--) {
        ULONG p = 1, k;
        for (k = 0; k < (ULONG)i; k++) {
            p *= 10;
        }
        *D++ = (WCHAR)(L'0' + (V / p) % 10);
    }
    *D = 0;
    return D;
}

static WCHAR *VidPid(UD_FUNC *F, WCHAR *P, BOOLEAN Rev)
{
    P = WStr(P, "USB\\VID_");
    P = WHex(P, GET16(F->DevDesc + 8), 4);
    P = WStr(P, "&PID_");
    P = WHex(P, GET16(F->DevDesc + 10), 4);
    if (Rev) {
        P = WStr(P, "&REV_");
        P = WHex(P, GET16(F->DevDesc + 12), 4);
    }
    if (!F->Whole) {
        P = WStr(P, "&MI_");
        P = WHex(P, F->First, 2);
    }
    return P;
}

/* the class of the device, of the function (its interface association, as usbccgp has it) or of its
   (first) interface */
static void ClassOf(UD_FUNC *F, UCHAR *C, UCHAR *S, UCHAR *Pr)
{
    const UCHAR *end = F->Cfg + F->CfgLen, *d = F->Cfg;

    *C = F->DevDesc[4];
    *S = F->DevDesc[5];
    *Pr = F->DevDesc[6];
    if (F->Whole && *C != 0 && *C != 0xEF) {
        return;
    }
    while ((d = UsbNextDesc(d, end)) != NULL) {
        if (!F->Whole && d[1] == 0x0B && d[0] >= 8 && d[2] == F->First) {
            *C = d[4];
            *S = d[5];
            *Pr = d[6];
            return;
        }
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && (F->Whole || d[2] == F->First)) {
            *C = d[5];
            *S = d[6];
            *Pr = d[7];
            return;
        }
    }
}

static int CountIfaces(UD_FUNC *F)
{
    const UCHAR *end = F->Cfg + F->CfgLen, *d = F->Cfg;
    int n = 0;

    while ((d = UsbNextDesc(d, end)) != NULL) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[3] == 0) {
            n++;
        }
    }
    return n;
}

/* a composite device for usbccgp (Windows XP): more than one interface, device class 0 or
   "interface association" (EF / 02 / 01) */
static BOOLEAN Composite(UD_FUNC *F)
{
    return F->Whole && CountIfaces(F) > 1 &&
           (F->DevDesc[4] == 0 || (F->DevDesc[4] == 0xEF && F->DevDesc[5] == 0x02 && F->DevDesc[6] == 0x01));
}

static WCHAR *ClassIds(WCHAR *P, const char *Kind, UCHAR C, UCHAR S, UCHAR Pr)
{
    P = WStr(P, Kind);
    P = WHex(P, C, 2);
    P = WStr(P, "&SubClass_");
    P = WHex(P, S, 2);
    P = WStr(P, "&Prot_");
    P = WHex(P, Pr, 2) + 1;
    P = WStr(P, Kind);
    P = WHex(P, C, 2);
    P = WStr(P, "&SubClass_");
    P = WHex(P, S, 2) + 1;
    P = WStr(P, Kind);
    P = WHex(P, C, 2) + 1;
    return P;
}

static BOOLEAN SerialOk(const char *S)
{
    int n = 0;

    for (; *S; S++, n++) {
        if (*S <= ' ' || *S > '~' || *S == ',' || *S == '\\' || *S == '&') {
            return FALSE;
        }
    }
    return n > 0;
}

NTSTATUS UsbdiQueryId(UD_FUNC *F, PIRP Irp, BUS_QUERY_ID_TYPE Type)
{
    WCHAR buf[600], *p = buf;
    UCHAR c, s, pr;
    ULONG n;
    PWCHAR out;

    RtlZeroMemory(buf, sizeof(buf));
    ClassOf(F, &c, &s, &pr);
    switch (Type) {
    case BusQueryDeviceID:
        p = VidPid(F, p, FALSE) + 1;
        break;
    case BusQueryHardwareIDs:
        p = VidPid(F, p, TRUE) + 1;
        p = VidPid(F, p, FALSE) + 1;
        break;
    case BusQueryCompatibleIDs:
        if (Composite(F)) {
            /* what Windows' hub gives a composite device: the device class, then usbccgp splits it
               (Windows XP; a class driver for the first interface must not take the whole device) */
            p = ClassIds(p, "USB\\DevClass_", F->DevDesc[4], F->DevDesc[5], F->DevDesc[6]);
            p = WStr(p, "USB\\COMPOSITE") + 1;
        } else {
            p = ClassIds(p, "USB\\Class_", c, s, pr);
        }
        break;
    case BusQueryInstanceID:
        if (F->Whole && SerialOk(F->Serial)) {
            p = WStr(p, F->Serial);
        } else {
            /* where it is: controller, root port, port, interface */
            p = WHex(p, F->HcIndex, 1);
            *p++ = L'&';
            p = WHex(p, F->RootPort, 2);
            *p++ = L'&';
            p = WHex(p, F->Port, 2);
            *p++ = L'&';
            p = WHex(p, F->Depth, 1);
            if (!F->Whole) {
                *p++ = L'&';
                p = WHex(p, F->First, 2);
            }
        }
        p++;
        break;
    default:
        return Irp->IoStatus.Status;
    }
    *p++ = 0;
    n = (ULONG)(p - buf);
    out = (PWCHAR)ExAllocatePoolWithTag(PagedPool, n * sizeof(WCHAR), UD_TAG);
    if (out == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(out, buf, n * sizeof(WCHAR));
    Irp->IoStatus.Information = (ULONG_PTR)out;
    return STATUS_SUCCESS;
}

/* what a USB class, subclass and protocol are (Device Manager names); NULL: nothing known */
const char *UsbClassName(UCHAR C, UCHAR S, UCHAR P)
{
    switch (C) {
    case 0x01: return S == 1 ? "Audio Control" : S == 2 ? "Audio Streaming" : S == 3 ? "MIDI" : "Audio";
    case 0x02: return S == 2 && P == 0xFF ? "RNDIS Network" : S == 2 ? "Modem" : S == 6 ? "Ethernet (ECM)" : S == 0x0D ? "Ethernet (NCM)" :
                      S == 0x0E ? "Mobile Broadband (MBIM)" : "Communications";
    case 0x03: return S == 1 && P == 1 ? "Keyboard" : S == 1 && P == 2 ? "Mouse" : "Human Interface Device";
    case 0x05: return "Physical Interface Device";
    case 0x06: return "Still Image Device";
    case 0x07: return "Printer";
    case 0x08: return P == 0x62 ? "Mass Storage (UAS)" : "Mass Storage";
    case 0x09: return "Hub";
    case 0x0A: return "CDC Data";
    case 0x0B: return "Smart Card Reader";
    case 0x0D: return "Content Security";
    case 0x0E: return S == 1 ? "Video Control" : S == 2 ? "Video Streaming" : "Video";
    case 0x0F: return "Personal Healthcare";
    case 0x10: return "Audio/Video";
    case 0x11: return "Billboard";
    case 0x12: return "Type-C Bridge";
    case 0xDC: return "Diagnostic";
    case 0xE0: return S == 1 && P == 1 ? "Bluetooth Radio" : S == 1 && P == 3 ? "RNDIS Network" : "Wireless Controller";
    case 0xEF: return S == 4 && P == 1 ? "RNDIS Network" : S == 2 && P == 1 ? "Composite" : "Miscellaneous";
    case 0xFE: return S == 1 ? "Firmware Update (DFU)" : S == 2 ? "IrDA Bridge" : S == 3 ? "Test and Measurement" :
                      "Application Specific";
    case 0xFF: return "Vendor Specific";
    default: return NULL;
    }
}

/* "USB Mass Storage Device", "USB Keyboard": the device a class name stands for */
WCHAR *UsbClassDevice(WCHAR *P, const char *Name)
{
    static const char *noun[] = { "Keyboard", "Mouse", "Printer", "Hub", "Radio", "Reader", "Modem", "Network",
                                  "Device", "Bridge", "(ECM)", "(NCM)", "(MBIM)", "(UAS)", "(DFU)" };
    const char *last = Name, *q;
    ULONG i;

    for (q = Name; *q; q++) {
        if (*q == ' ') {
            last = q + 1;
        }
    }
    P = WStr(P, "USB ");
    P = WStr(P, Name);
    for (i = 0; i < sizeof(noun) / sizeof(noun[0]); i++) {
        const char *a = last, *b = noun[i];
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            return P;
        }
    }
    return WStr(P, " Device");
}

/* "<product> - <class> (Interface n)" / "(Interfaces n-m)"; the device: its product string, else the
   manufacturer's, else its ids */
static void PartName(UD_FUNC *F, WCHAR *P, const char *Cls, UCHAR First, UCHAR Count)
{
    if (F->Product[0]) {
        P = WStr(P, F->Product);
    } else if (F->Mfg[0]) {
        P = WStr(P, F->Mfg);
        P = WStr(P, " USB Device");
    } else {
        P = WStr(P, "USB Device VID_");
        P = WHex(P, GET16(F->DevDesc + 8), 4);
        P = WStr(P, " PID_");
        P = WHex(P, GET16(F->DevDesc + 10), 4);
    }
    P = WStr(P, " - ");
    P = WStr(P, Cls != NULL ? Cls : "Function");
    P = WStr(P, Count > 1 ? " (Interfaces " : " (Interface ");
    P = WDec(P, First, First >= 10 ? 2 : 1);
    if (Count > 1) {
        UCHAR last = (UCHAR)(First + Count - 1);
        P = WStr(P, "-");
        P = WDec(P, last, last >= 10 ? 2 : 1);
    }
    WStr(P, ")");
}

/* the name a device or function of one gets: its product string, else the manufacturer's and what it is,
   else what it is and its ids; a function says which interface(s): "UD-3900 - Audio Control (Interface 0)" */
static void FuncName(UD_FUNC *F, WCHAR *P)
{
    UCHAR c, s, pr;
    const char *cls;

    ClassOf(F, &c, &s, &pr);
    cls = UsbClassName(c, s, pr);
    if (F->Whole) {
        if (F->Product[0]) {
            WStr(P, F->Product);
            return;
        }
        if (F->Mfg[0]) {
            P = WStr(P, F->Mfg);
            P = WStr(P, " ");
        }
        if (Composite(F) || cls == NULL || (c == 0xEF && s == 2 && pr == 1)) {
            P = WStr(P, Composite(F) ? "USB Composite Device" : "USB Device");
        } else {
            P = UsbClassDevice(P, cls);
        }
        if (!F->Mfg[0]) {
            P = WStr(P, " (VID_");
            P = WHex(P, GET16(F->DevDesc + 8), 4);
            P = WStr(P, " PID_");
            P = WHex(P, GET16(F->DevDesc + 10), 4);
            WStr(P, ")");
        }
        return;
    }
    PartName(F, P, cls, F->First, F->Count);
}

/*
 * Windows XP's composite driver (usbccgp) names its functions after the device's product string, or
 * "USB Device" when there is none - several functions alike.  A function that has no driver (Device
 * Manager shows its DeviceDesc) gets the name of its interfaces: "USB Device VID_0414 PID_A00B - Audio
 * Control (Interface 0)" (Enum\USB\VID_v&PID_p&MI_nn, every instance without a service; a few seconds
 * after usbccgp started, and once more later).  UsbInterfaceNames = 0: off.
 */
static void ChildNames(UD_FUNC *F)
{
    const UCHAR *end = F->Cfg + F->CfgLen, *d;
    UCHAR covered[32];
    WCHAR name[160];
    int n;

    RtlZeroMemory(covered, sizeof(covered));
    for (d = F->Cfg; (d = UsbNextDesc(d, end)) != NULL;) {
        if (d[1] == 0x0B && d[0] >= 8 && d[2] < 32) {
            PartName(F, name, UsbClassName(d[4], d[5], d[6]), d[2], d[3]);
            WdmNameUsbFunction(GET16(F->DevDesc + 8), GET16(F->DevDesc + 10), d[2], name);
            for (n = d[2]; n < d[2] + d[3] && n < 32; n++) {
                covered[n] = 1;
            }
        }
    }
    for (d = F->Cfg; (d = UsbNextDesc(d, end)) != NULL;) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[3] == 0 && d[2] < 32 && !covered[d[2]]) {
            PartName(F, name, UsbClassName(d[5], d[6], d[7]), d[2], 1);
            WdmNameUsbFunction(GET16(F->DevDesc + 8), GET16(F->DevDesc + 10), d[2], name);
        }
    }
}

NTSTATUS UsbdiQueryText(UD_FUNC *F, PIRP Irp, DEVICE_TEXT_TYPE Type)
{
    WCHAR buf[160], *p = buf;
    ULONG n;
    PWCHAR out;

    if (Type == DeviceTextDescription) {
        FuncName(F, p);
        while (*p) p++;
    } else if (Type == DeviceTextLocationInformation) {
        p = WStr(p, "Port_#");
        p = WDec(p, F->Port, 4);
        p = WStr(p, ".Hub_#");
        p = WDec(p, F->Depth + 1, 4);
    } else {
        return Irp->IoStatus.Status;
    }
    n = (ULONG)(p - buf) + 1;
    out = (PWCHAR)ExAllocatePoolWithTag(PagedPool, n * sizeof(WCHAR), UD_TAG);
    if (out == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(out, buf, n * sizeof(WCHAR));
    Irp->IoStatus.Information = (ULONG_PTR)out;
    return STATUS_SUCCESS;
}

static void CloseAllW(void *A)
{
    ClosePipes((UD_FUNC *)A, 0xFF);
}

/* IRP_MJ_PNP of a USB PDO; *Free: the PDO is to be deleted (REMOVE of a device no longer reported) */
NTSTATUS UsbdiPnp(UD_FUNC *F, PIRP Irp, BOOLEAN Reported, BOOLEAN *Free)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = Irp->IoStatus.Status;
    ULONG_PTR info = Irp->IoStatus.Information;

    *Free = FALSE;
    if (!F->NameDone && sp->MinorFunction != IRP_MN_QUERY_ID && sp->MinorFunction != IRP_MN_QUERY_CAPABILITIES &&
        sp->MinorFunction != IRP_MN_QUERY_DEVICE_TEXT && KeGetCurrentIrql() == PASSIVE_LEVEL) {
        /* a node without a driver keeps the name it got first ("USB Device" from older versions) */
        WCHAR name[160];
        FuncName(F, name);
        F->NameDone = WdmNameDriverless(F->Pdo, name);
    }
    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        F->Started = TRUE;
        F->StartedAt = OsTimeMs();
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_SURPRISE_REMOVAL:
    case IRP_MN_REMOVE_DEVICE:
        /* the driver is gone: its pipes close (the configuration stays) */
        F->Started = FALSE;
        WdmRunInWorker(CloseAllW, F);
        if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE && !Reported) {
            *Free = TRUE;
        }
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES c = sp->Parameters.DeviceCapabilities.Capabilities;
        int i;
        if (c->Version != 1 || c->Size < sizeof(DEVICE_CAPABILITIES)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++) {
            c->DeviceState[i] = PowerDeviceD3;
        }
        c->DeviceWake = PowerDeviceUnspecified;
        c->SystemWake = PowerSystemUnspecified;
        c->DeviceD1 = c->DeviceD2 = FALSE;
        c->LockSupported = FALSE;
        c->EjectSupported = FALSE;
        c->Removable = TRUE;
        c->DockDevice = FALSE;
        c->UniqueID = F->Whole && SerialOk(F->Serial);
        c->SilentInstall = FALSE;
        c->RawDeviceOK = FALSE;
        c->SurpriseRemovalOK = TRUE;
        c->Address = F->Port;
        c->UINumber = F->Port;
        st = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_ID:
        st = UsbdiQueryId(F, Irp, sp->Parameters.QueryId.IdType);
        info = Irp->IoStatus.Information;
        break;
    case IRP_MN_QUERY_DEVICE_TEXT:
        st = UsbdiQueryText(F, Irp, sp->Parameters.QueryDeviceText.DeviceTextType);
        info = Irp->IoStatus.Information;
        break;
    case IRP_MN_QUERY_INTERFACE:
        st = QueryInterface(F, sp, st);
        break;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS), UD_TAG);
            if (rel == NULL) {
                st = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                rel->Count = 1;
                rel->Objects[0] = F->Pdo;
                ObReferenceObject(F->Pdo);
                info = (ULONG_PTR)rel;
                st = STATUS_SUCCESS;
            }
        }
        break;
    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION b = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(PagedPool, sizeof(*b), UD_TAG);
        static const GUID usb = { 0x9d7debbc, 0xc85d, 0x11d1, { 0x9e, 0xb4, 0x00, 0x60, 0x08, 0xc3, 0xa1, 0x9a } };
        if (b == NULL) {
            st = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            b->BusTypeGuid = usb;
            b->LegacyBusType = PNPBus;
            b->BusNumber = 0;
            info = (ULONG_PTR)b;
            st = STATUS_SUCCESS;
        }
        break;
    }
    default:
        break;
    }
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

NTSTATUS UsbdiPower(UD_FUNC *F, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = Irp->IoStatus.Status;

    (void)F;
    switch (sp->MinorFunction) {
    case IRP_MN_SET_POWER:
    case IRP_MN_QUERY_POWER:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_WAIT_WAKE:
        st = STATUS_NOT_SUPPORTED;          /* no remote wake */
        break;
    default:
        break;
    }
    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

/* ------------------------------------------------------------------ */
/* Which devices and functions go to Windows (worker)                   */
/* ------------------------------------------------------------------ */

static void Detach(USB_DEV *Dev)
{
    UD_FUNC *f;
    LIST_ENTRY done;
    KIRQL irql;

    InitializeListHead(&done);
    for (f = Funcs; f != NULL; f = f->Next) {
        if (f->Dev == Dev && f->Gen == Dev->Gen) {
            f->Dead = TRUE;
            ClosePipes(f, 0xFF);
        }
    }
    KeAcquireSpinLock(&UdLock, &irql);
    while (!IsListEmpty(&Ep0[Dev->Index].Wait)) {
        UD_REQ *r = CONTAINING_RECORD(RemoveHeadList(&Ep0[Dev->Index].Wait), UD_REQ, Link);
        Finish(r, USBD_STATUS_DEVICE_GONE, &done);
    }
    KeReleaseSpinLock(&UdLock, irql);
    CompleteList(&done);
    /* the control transfer on the wire ends when the core cancels the default pipe; then the device is ours no more */
    if (Ep0[Dev->Index].Active != NULL) {
        UsbCancelPipe(&Dev->Ep0);
    }
    for (f = Funcs; f != NULL; f = f->Next) {
        if (f->Dev == Dev && f->Dead) {
            f->Dev = NULL;
        }
    }
}

void UsbdiInit(void)
{
    ULONG maj = 5, min = 0, build = 0;
    int i;

    if (UdInit) {
        return;
    }
    KeInitializeSpinLock(&UdLock);
    InitializeListHead(&Cancelled);
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        InitializeListHead(&Ep0[i].Wait);
    }
    PsGetVersion(&maj, &min, &build, NULL);
    WinBuild = build;
    UsbPdoMode = OsGetConfig("UsbPdo", 1);
    if (UsbTextSetupHid && UsbPdoMode == 0) {
        UsbPdoMode = 1;                         /* text-mode setup: the HID devices are setup's */
    }
    UsbWinVideo = OsGetConfig("WindowsCamera", 0);
    if (UsbWinVideo && UsbPdoMode) {
        UsbLog(LOG_INFO, "usbdi: cameras go to Windows' camera driver (usbvideo.sys)\n");
    }
    UsbDetachHook = Detach;
    UdInit = TRUE;
}

static BOOLEAN Covered(USB_DEV *Dev, UCHAR First, BOOLEAN Whole)
{
    UD_FUNC *f;
    int i;

    for (f = Funcs; f != NULL; f = f->Next) {
        if (!f->Dead && f->Dev == Dev && f->Gen == Dev->Gen && f->Whole == Whole && (Whole || f->First == First)) {
            return TRUE;
        }
    }
    for (i = 0; i < NWant; i++) {
        if (Want[i].Dev == Dev->Index && Want[i].Gen == Dev->Gen && Want[i].Whole == Whole &&
            (Whole || Want[i].First == First)) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN AddWant(USB_DEV *Dev, UCHAR First, UCHAR Count, BOOLEAN Whole)
{
    KIRQL irql;

    if (Covered(Dev, First, Whole) || NWant >= UD_MAX_WANT) {
        return FALSE;
    }
    KeAcquireSpinLock(&UdLock, &irql);
    Want[NWant].Dev = Dev->Index;
    Want[NWant].Gen = Dev->Gen;
    Want[NWant].First = First;
    Want[NWant].Count = Count;
    Want[NWant].Whole = Whole;
    NWant++;
    KeReleaseSpinLock(&UdLock, irql);
    return TRUE;
}

/* the functions of a device for Windows: interfaces (with their association) USBNT does not serve */
static BOOLEAN Plan(USB_DEV *Dev)
{
    const UCHAR *end = Dev->Config + Dev->ConfigLen, *d = Dev->Config;
    int total = 0;
    BOOLEAN change = FALSE;
    UCHAR iadFirst = 0xFF, iadCount = 0;

    while ((d = UsbNextDesc(d, end)) != NULL) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[3] == 0) {
            total++;
        }
    }
    if (total == 0 || (Dev->NativeIf & ((total >= 32) ? 0xFFFFFFFFUL : ((1UL << total) - 1))) ==
                      ((total >= 32) ? 0xFFFFFFFFUL : ((1UL << total) - 1))) {
        return FALSE;                       /* USBNT serves all of it */
    }
    /* a hub is USBNT's even when it could not set it up: Windows' usbhub.sys cannot run on the USBNT
       bus (it needs the root hub and the hub bus interface of usbport) and shows a failed hub
       instead, with nothing behind it */
    for (d = Dev->Config; Dev->DevDesc[4] != USB_CLASS_HUB && (d = UsbNextDesc(d, end)) != NULL;) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[5] == USB_CLASS_HUB) {
            break;
        }
    }
    if (Dev->DevDesc[4] == USB_CLASS_HUB || d != NULL) {
        UsbLog(LOG_ERR, "device %u: hub that USBNT could not set up, it is not given to Windows\n", Dev->Index);
        Dev->NativeIf = 0xFFFFFFFFUL;       /* asked once */
        return FALSE;
    }
    /* the whole device (Windows XP splits composites); in text-mode setup the functions, as Windows 2000
       has them: setup binds them by their class ids, and nothing copies usbccgp for the installed system */
    if (Dev->NativeIf == 0 && (total == 1 || (WinBuild >= 2600 && !UsbTextSetupHid))) {
        return AddWant(Dev, 0, (UCHAR)total, TRUE);
    }
    for (d = Dev->Config; (d = UsbNextDesc(d, end)) != NULL;) {
        if (d[1] == 0x0B && d[0] >= 8) {    /* interface association */
            iadFirst = d[2];
            iadCount = d[3];
            continue;
        }
        if (d[1] != USB_DT_INTERFACE || d[0] < 9 || d[3] != 0 || d[2] >= 32) {
            continue;
        }
        if (iadFirst != 0xFF && d[2] > iadFirst && d[2] < iadFirst + iadCount) {
            continue;                       /* part of the function started at iadFirst */
        }
        if (Dev->NativeIf & (1UL << d[2])) {
            continue;
        }
        if (iadFirst != 0xFF && d[2] == iadFirst) {
            change |= AddWant(Dev, d[2], iadCount, FALSE);
        } else {
            change |= AddWant(Dev, d[2], 1, FALSE);
        }
    }
    return change;
}

/* worker, every iteration: TRUE when the bus relations changed */
BOOLEAN UsbdiService(void)
{
    BOOLEAN change = FALSE;
    UD_FUNC *f;
    int i;
    KIRQL irql;
    LIST_ENTRY done;

    if (!UdInit) {
        return FALSE;
    }
    /* cancelled requests */
    InitializeListHead(&done);
    KeAcquireSpinLock(&UdLock, &irql);
    while (!IsListEmpty(&Cancelled)) {
        InsertTailList(&done, RemoveHeadList(&Cancelled));
    }
    KeReleaseSpinLock(&UdLock, irql);
    CompleteList(&done);
    if (Kick) {
        Kick = 0;
        BufRefill();
        /* requests that waited for a buffer or for the control pipe, transfers to take off the wire */
        for (f = Funcs; f != NULL; f = f->Next) {
            for (i = 0; i < UD_PIPES; i++) {
                UD_PIPE *p = &f->Pipe[i];
                if (!p->Used) {
                    continue;
                }
                if (p->Abort) {
                    p->Abort = 0;
                    if (p->P.Opened && p->P.Cur != NULL) {
                        UsbCancelPipe(&p->P);
                    }
                }
                InitializeListHead(&done);
                KeAcquireSpinLock(&UdLock, &irql);
                Pump(f, p, &done);
                KeReleaseSpinLock(&UdLock, irql);
                CompleteList(&done);
            }
        }
        for (i = 0; i < USB_MAX_DEVICES; i++) {
            UD_REQ *a = Ep0[i].Active;
            /* the core used the control pipe, a transfer was cancelled - or requests wait for a buffer */
            if (!Ep0[i].Retry && !Ep0[i].Reset && (a != NULL || IsListEmpty(&Ep0[i].Wait))) {
                continue;
            }
            Ep0[i].Retry = FALSE;
            if (Ep0[i].Reset && a == NULL) {
                USB_DEV *d = &UsbDevs[i];
                if (d->InUse && !d->Gone && d->Hc->Ops->PipeReset != NULL) {
                    d->Hc->Ops->PipeReset(&d->Ep0);
                }
                d->Ep0.Halted = 0;
                Ep0[i].Reset = FALSE;
            }
            if (a != NULL && a->Synth == 2 && UsbDevs[i].InUse) {
                UsbCancelPipe(&UsbDevs[i].Ep0);     /* a cancelled control transfer */
            }
            for (f = Funcs; f != NULL; f = f->Next) {
                if (f->DevIndex == i && f->Dev != NULL) {
                    InitializeListHead(&done);
                    KeAcquireSpinLock(&UdLock, &irql);
                    Pump(f, NULL, &done);
                    KeReleaseSpinLock(&UdLock, irql);
                    CompleteList(&done);
                    break;
                }
            }
        }
    }
    /* the functions usbccgp made of a composite device: names (ChildNames) */
    for (f = Funcs; f != NULL; f = f->Next) {
        if (f->Whole && f->Started && f->Cfg != NULL && f->ChildNames < 2 && Composite(f) &&
            OsTimeMs() - f->StartedAt > (f->ChildNames == 0 ? 3000UL : 20000UL) &&
            KeGetCurrentIrql() == PASSIVE_LEVEL) {
            f->ChildNames++;
            if (OsGetConfig("UsbInterfaceNames", 1)) {
                ChildNames(f);
            }
        }
    }
    if (UsbPdoMode == 0) {
        return FALSE;
    }
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        if (d->InUse && !d->Gone && d->Ready && !d->IsHub && d->Config != NULL) {
            change |= Plan(d);
        }
    }
    /* functions whose device left (or whose port cycled) leave the bus */
    for (f = Funcs; f != NULL; f = f->Next) {
        if (f->Dead && !f->GoneReported) {
            if (f->Dev != NULL && f->Dev->Gen == f->Gen && !f->Dev->Gone) {
                /* a cycled port: the device stays, a new function takes its place */
                ClosePipes(f, 0xFF);
                f->Dev = NULL;
            }
            change = TRUE;
            f->GoneReported = TRUE;
        }
    }
    return change;
}

/* the configuration descriptor a function of a composite device sees: its interfaces only */
static void BuildCfg(UD_FUNC *F, USB_DEV *Dev)
{
    const UCHAR *end = Dev->Config + Dev->ConfigLen, *d;
    UCHAR *o;
    ULONG n = 9;
    UCHAR ifs = 0;
    BOOLEAN keep = FALSE;

    F->Cfg = (UCHAR *)ExAllocatePoolWithTag(NonPagedPool, Dev->ConfigLen, UD_TAG);
    if (F->Cfg == NULL) {
        return;
    }
    if (F->Whole) {
        RtlCopyMemory(F->Cfg, Dev->Config, Dev->ConfigLen);
        F->CfgLen = Dev->ConfigLen;
        return;
    }
    RtlCopyMemory(F->Cfg, Dev->Config, 9);
    o = F->Cfg + 9;
    for (d = Dev->Config; (d = UsbNextDesc(d, end)) != NULL;) {
        if (d[1] == 0x0B) {
            keep = d[2] == F->First;
        } else if (d[1] == USB_DT_INTERFACE && d[0] >= 9) {
            keep = Owns(F, d[2]);
            if (keep && d[3] == 0) {
                ifs++;
            }
        }
        if (keep && n + d[0] <= Dev->ConfigLen) {
            RtlCopyMemory(o, d, d[0]);
            o += d[0];
            n += d[0];
        }
    }
    F->Cfg[2] = (UCHAR)n;
    F->Cfg[3] = (UCHAR)(n >> 8);
    F->Cfg[4] = ifs;
    F->CfgLen = n;
}

/* BusRelations: a new function to report, NULL when there is none */
UD_FUNC *UsbdiNewFunc(void)
{
    UD_WANT w;
    UD_FUNC *f;
    USB_DEV *dev;
    KIRQL irql;

    for (;;) {
        KeAcquireSpinLock(&UdLock, &irql);
        if (NWant == 0) {
            KeReleaseSpinLock(&UdLock, irql);
            return NULL;
        }
        w = Want[0];
        RtlMoveMemory(&Want[0], &Want[1], (NWant - 1) * sizeof(UD_WANT));
        NWant--;
        KeReleaseSpinLock(&UdLock, irql);
        dev = &UsbDevs[w.Dev];
        if (dev->InUse && !dev->Gone && dev->Ready && dev->Gen == w.Gen) {
            break;
        }
    }
    f = (UD_FUNC *)ExAllocatePoolWithTag(NonPagedPool, sizeof(*f), UD_TAG);
    if (f == NULL) {
        return NULL;
    }
    RtlZeroMemory(f, sizeof(*f));
    f->Dev = dev;
    f->Gen = w.Gen;
    f->DevIndex = w.Dev;
    f->First = w.First;
    f->Count = w.Count;
    f->Whole = w.Whole;
    f->Speed = dev->Speed;
    f->Config = dev->ConfigValue;
    f->Port = dev->Port;
    f->RootPort = dev->RootPort;
    f->HcIndex = dev->Hc->Index;
    f->Depth = dev->Depth;
    RtlCopyMemory(f->DevDesc, dev->DevDesc, 18);
    RtlCopyMemory(f->Product, dev->Product, sizeof(f->Product));
    RtlCopyMemory(f->Serial, dev->Serial, sizeof(f->Serial));
    RtlCopyMemory(f->Mfg, dev->Mfg, sizeof(f->Mfg));
    BuildCfg(f, dev);
    KeAcquireSpinLock(&UdLock, &irql);
    f->Next = Funcs;
    Funcs = f;
    KeReleaseSpinLock(&UdLock, irql);
    UsbLog(LOG_INFO, "PnP: device %u (%04x:%04x)%s to Windows' USB drivers\n", f->DevIndex, GET16(f->DevDesc + 8),
           GET16(f->DevDesc + 10), f->Whole ? "" : " function");
    if (!f->Whole) {
        UsbLog(LOG_INFO, "PnP:   interfaces %u..%u\n", f->First, f->First + f->Count - 1);
    }
    return f;
}

void UsbdiBind(UD_FUNC *F, PDEVICE_OBJECT Pdo)
{
    F->Pdo = Pdo;
}

BOOLEAN UsbdiWanted(UD_FUNC *F)
{
    return F != NULL && !F->Dead;
}

static void FreeW(void *A)
{
    UD_FUNC *F = (UD_FUNC *)A, **pp;
    KIRQL irql;

    if (F->Dev != NULL) {
        ClosePipes(F, 0xFF);
    }
    KeAcquireSpinLock(&UdLock, &irql);
    for (pp = &Funcs; *pp != NULL; pp = &(*pp)->Next) {
        if (*pp == F) {
            *pp = F->Next;
            break;
        }
    }
    KeReleaseSpinLock(&UdLock, irql);
    if (F->Cfg != NULL) {
        ExFreePool(F->Cfg);
    }
    ExFreePool(F);
}

/* the PDO is deleted: the function goes (in the worker, which walks the list) */
void UsbdiFreeFunc(UD_FUNC *F)
{
    int t;

    for (t = 0; t < 500 && F->Pending > 0; t++) {
        OsSleepMs(10);                      /* requests still finishing */
    }
    WdmRunInWorker(FreeW, F);
}

/* Device Manager (wdmtree.c): is the device, or a function of it, on a PDO for Windows' drivers? */
BOOLEAN UsbdiHasDevice(USB_DEV *Dev)
{
    UD_FUNC *f;
    KIRQL irql;
    BOOLEAN r = FALSE;

    KeAcquireSpinLock(&UdLock, &irql);
    for (f = Funcs; f != NULL && !r; f = f->Next) {
        r = (BOOLEAN)(f->Dev == Dev && f->Gen == Dev->Gen && !f->Dead);
    }
    KeReleaseSpinLock(&UdLock, irql);
    return r;
}
