/*
 * hcuhci.c - UHCI host controller driver (low and full speed).
 *
 * Every pipe owns a queue head linked into a fixed skeleton:
 * frame list -> interrupt QHs -> control QHs -> bulk QHs. Transfer
 * descriptors are processed depth first; data toggles are kept in
 * software.
 */

#include "usbnt.h"

#define UR_USBCMD       0x00
#define UR_USBSTS       0x02
#define UR_USBINTR      0x04
#define UR_FRNUM        0x06
#define UR_FRBASE       0x08
#define UR_SOFMOD       0x0C
#define UR_PORTSC(n)    (0x10 + 2 * ((n) - 1))

#define CMD_RS          0x0001
#define CMD_HCRESET     0x0002
#define CMD_GRESET      0x0004
#define CMD_CF          0x0040
#define CMD_MAXP        0x0080

#define STS_USBINT      0x0001
#define STS_ERR         0x0002
#define STS_RD          0x0004
#define STS_HSE         0x0008
#define STS_HCPE        0x0010
#define STS_HALTED      0x0020

#define PORT_CCS        0x0001
#define PORT_CSC        0x0002
#define PORT_PED        0x0004
#define PORT_PEDC       0x0008
#define PORT_LSDA       0x0100
#define PORT_PR         0x0200
#define PORT_SUSP       0x1000
#define PORT_RWC        (PORT_CSC | PORT_PEDC)

#define LINK_T          0x00000001
#define LINK_QH         0x00000002
#define LINK_VF         0x00000004

#define TD_ACTIVE       0x00800000
#define TD_STALLED      0x00400000
#define TD_DBERR        0x00200000
#define TD_BABBLE       0x00100000
#define TD_NAK          0x00080000
#define TD_CRCTO        0x00040000
#define TD_BITSTUFF     0x00020000
#define TD_ERRMASK      (TD_STALLED | TD_DBERR | TD_BABBLE | TD_CRCTO | TD_BITSTUFF)
#define TD_IOC          0x01000000
#define TD_LS           0x04000000
#define TD_CERR3        0x18000000
#define TD_SPD          0x20000000

#define PID_SETUP       0x2D
#define PID_IN          0x69
#define PID_OUT         0xE1

#define UHCI_MAX_XFER   4096
#define TD_SIZE         32

typedef struct _UQH {
    ULONG      *Qh;
    ULONG       QhPhys;
    ULONG     **Td;
    ULONG      *TdPhys;
    USHORT     *TdLen;
    ULONG       NTd;
    ULONG       Used;
    ULONG       StatusTd;       /* control: index of status TD */
    UCHAR       Linked;
    UCHAR       Kind;           /* 0 int, 1 control, 2 bulk */
    UCHAR       Pad[2];
    USB_PIPE   *Pipe;
    struct _UQH *Next;
} UQH;

typedef struct _UHC {
    USB_HC     *Hc;
    ULONG       Io;
    ULONG      *Frames;
    ULONG       FramesPhys;
    UQH         Skel[3];        /* interrupt, control, bulk */
    BOOLEAN     UseIrq;
} UHC;

#define UHCP(hc)    ((UHC *)(hc)->Priv)

static USHORT Rd16(UHC *u, ULONG r) { return OsIn16(u->Io + r); }
static void Wr16(UHC *u, ULONG r, USHORT v) { OsOut16(u->Io + r, v); }

/* ------------------------------------------------------------------ */
/* Skeleton                                                             */
/* ------------------------------------------------------------------ */

static void QhLink(UHC *u, UQH *q)
{
    UQH *skel = &u->Skel[q->Kind];
    OS_IRQL irql;

    if (q->Linked) {
        return;
    }
    irql = OsLock();
    q->Qh[0] = skel->Qh[0];
    q->Next = skel->Next;
    skel->Qh[0] = q->QhPhys | LINK_QH;
    skel->Next = q;
    q->Linked = 1;
    OsUnlock(irql);
}

static void QhUnlink(UHC *u, UQH *q)
{
    UQH *prev = &u->Skel[q->Kind];
    OS_IRQL irql;

    if (!q->Linked) {
        return;
    }
    irql = OsLock();
    while (prev != NULL && prev->Next != q) {
        prev = prev->Next;
    }
    if (prev != NULL) {
        prev->Qh[0] = q->Qh[0];
        prev->Next = q->Next;
    }
    q->Linked = 0;
    OsUnlock(irql);
    OsSleepMs(2);
}

/* ------------------------------------------------------------------ */
/* Controller                                                           */
/* ------------------------------------------------------------------ */

static int UhciStart(USB_HC *Hc)
{
    UHC *u;
    ULONG i;
    int port;
    ULONG start;

    u = (UHC *)OsAlloc(sizeof(UHC));
    if (u == NULL) {
        return USB_ERR_NOMEM;
    }
    Hc->Priv = u;
    u->Hc = Hc;
    u->Io = Hc->IoBase;
    u->UseIrq = (BOOLEAN)OsGetConfig("UseInterrupts", 0);

    /* legacy support: stop SMI and keyboard emulation, clear status */
    OsPciWrite(Hc->Bus, Hc->Dev, Hc->Fn, 0xC0, 0x8F00, 2);

    Wr16(u, UR_USBCMD, 0);
    Wr16(u, UR_USBINTR, 0);
    OsSleepMs(2);
    Wr16(u, UR_USBCMD, CMD_HCRESET);
    start = OsTimeMs();
    while (Rd16(u, UR_USBCMD) & CMD_HCRESET) {
        if (OsTimeMs() - start > 100) {
            UsbLog(LOG_ERR, "uhci: reset failed\n");
            return USB_ERR_IO;
        }
        OsStallUs(100);
    }

    u->Frames = (ULONG *)UsbDmaAlloc(4096, &u->FramesPhys);
    if (u->Frames == NULL) {
        return USB_ERR_NOMEM;
    }
    for (i = 0; i < 3; i++) {
        u->Skel[i].Qh = (ULONG *)UsbDmaAlloc(32, &u->Skel[i].QhPhys);
        if (u->Skel[i].Qh == NULL) {
            return USB_ERR_NOMEM;
        }
        u->Skel[i].Qh[1] = LINK_T;
        u->Skel[i].Kind = (UCHAR)i;
    }
    u->Skel[2].Qh[0] = LINK_T;
    u->Skel[1].Qh[0] = u->Skel[2].QhPhys | LINK_QH;
    u->Skel[0].Qh[0] = u->Skel[1].QhPhys | LINK_QH;
    for (i = 0; i < 1024; i++) {
        u->Frames[i] = u->Skel[0].QhPhys | LINK_QH;
    }

    Wr16(u, UR_USBINTR, 0);
    Wr16(u, UR_FRNUM, 0);
    OsOut32(u->Io + UR_FRBASE, u->FramesPhys);
    OsOut8(u->Io + UR_SOFMOD, 64);
    Wr16(u, UR_USBSTS, 0x3F);
    Wr16(u, UR_USBCMD, CMD_RS | CMD_CF | CMD_MAXP);
    OsSleepMs(2);
    if (Rd16(u, UR_USBSTS) & STS_HALTED) {
        UsbLog(LOG_ERR, "uhci: controller did not start\n");
        return USB_ERR_IO;
    }
    if (u->UseIrq) {
        Wr16(u, UR_USBINTR, 0x000F);
        OsPciWrite(Hc->Bus, Hc->Dev, Hc->Fn, 0xC0, 0x2000, 2);
    }

    /* UHCI has at least two ports; bit 7 always reads 1 on a real port */
    Hc->NumPorts = 0;
    for (port = 1; port <= 8; port++) {
        USHORT v = Rd16(u, UR_PORTSC(port));
        if (v == 0xFFFF || !(v & 0x0080)) {
            break;
        }
        Hc->NumPorts = (UCHAR)port;
    }
    if (Hc->NumPorts < 2) {
        Hc->NumPorts = 2;
    }
    Hc->MaxXfer = UHCI_MAX_XFER;
    Hc->Running = 1;
    UsbLog(LOG_INFO, "uhci: %u ports at io %x\n", Hc->NumPorts, u->Io);
    return USB_OK;
}

static void UhciStop(USB_HC *Hc)
{
    UHC *u = UHCP(Hc);

    if (u == NULL) {
        return;
    }
    Wr16(u, UR_USBCMD, 0);
    Wr16(u, UR_USBINTR, 0);
    Hc->Running = 0;
}

/* ------------------------------------------------------------------ */
/* Root ports                                                           */
/* ------------------------------------------------------------------ */

static ULONG UhciPortStatus(USB_HC *Hc, int Port)
{
    UHC *u = UHCP(Hc);
    USHORT sc = Rd16(u, UR_PORTSC(Port));
    ULONG ps = PS_POWER;

    if (sc == 0xFFFF) {
        return 0;
    }
    if (sc & PORT_CCS) ps |= PS_CONNECT;
    if (sc & PORT_PED) ps |= PS_ENABLE;
    if (sc & PORT_CSC) ps |= PS_CHANGE;
    ps |= (ULONG)((sc & PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL) << PS_SPEED_SHIFT;
    return ps;
}

static void UhciPortClearChange(USB_HC *Hc, int Port)
{
    UHC *u = UHCP(Hc);
    USHORT sc = Rd16(u, UR_PORTSC(Port));

    Wr16(u, UR_PORTSC(Port), (USHORT)(sc & (PORT_PED | PORT_RWC)));
}

static int UhciPortReset(USB_HC *Hc, int Port, UCHAR *Speed)
{
    UHC *u = UHCP(Hc);
    USHORT sc;
    int i;

    Wr16(u, UR_PORTSC(Port), PORT_PR);
    OsSleepMs(50);
    Wr16(u, UR_PORTSC(Port), 0);
    OsStallUs(50);
    for (i = 0; i < 10; i++) {
        sc = Rd16(u, UR_PORTSC(Port));
        if (!(sc & PORT_CCS)) {
            return USB_ERR_NODEV;
        }
        if (sc & PORT_RWC) {
            Wr16(u, UR_PORTSC(Port), (USHORT)(PORT_RWC | (sc & PORT_PED)));
            continue;
        }
        if (sc & PORT_PED) {
            *Speed = (UCHAR)((sc & PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL);
            OsSleepMs(10);
            return USB_OK;
        }
        Wr16(u, UR_PORTSC(Port), PORT_PED);
        OsSleepMs(10);
    }
    UsbLog(LOG_ERR, "uhci: port %d not enabled after reset\n", Port);
    return USB_ERR_IO;
}

static void UhciPortDisable(USB_HC *Hc, int Port)
{
    UHC *u = UHCP(Hc);

    Wr16(u, UR_PORTSC(Port), 0);
}

/* ------------------------------------------------------------------ */
/* Pipes                                                                */
/* ------------------------------------------------------------------ */

static void QhFree(UQH *q)
{
    ULONG i;

    if (q->Td != NULL) {
        for (i = 0; i < q->NTd; i++) {
            UsbDmaFree(q->Td[i], TD_SIZE);
        }
        OsFree(q->Td);
    }
    if (q->TdPhys != NULL) {
        OsFree(q->TdPhys);
    }
    if (q->TdLen != NULL) {
        OsFree(q->TdLen);
    }
    UsbDmaFree(q->Qh, 32);
    OsFree(q);
}

static int UhciPipeOpen(USB_PIPE *Pipe)
{
    UHC *u = UHCP(Pipe->Dev->Hc);
    UQH *q;
    ULONG mp = Pipe->MaxPacket ? Pipe->MaxPacket : 8;
    ULONG n;
    ULONG i;

    if (Pipe->Type == USB_EP_ISOCH) {
        return USB_ERR_PARAM;
    }
    if (mp > 64) {
        mp = 64;
    }
    switch (Pipe->Type) {
    case USB_EP_CONTROL: n = (USB_DEV_DATA_MAX + 7) / 8 + 2; break;
    case USB_EP_BULK:    n = UHCI_MAX_XFER / mp + 1; break;
    default:             n = 2; break;
    }
    q = (UQH *)OsAlloc(sizeof(UQH));
    if (q == NULL) {
        return USB_ERR_NOMEM;
    }
    q->Pipe = Pipe;
    q->Kind = (UCHAR)((Pipe->Type == USB_EP_INTERRUPT) ? 0 : (Pipe->Type == USB_EP_CONTROL) ? 1 : 2);
    q->Qh = (ULONG *)UsbDmaAlloc(32, &q->QhPhys);
    q->Td = (ULONG **)OsAlloc(n * sizeof(ULONG *));
    q->TdPhys = (ULONG *)OsAlloc(n * sizeof(ULONG));
    q->TdLen = (USHORT *)OsAlloc(n * sizeof(USHORT));
    if (q->Qh == NULL || q->Td == NULL || q->TdPhys == NULL || q->TdLen == NULL) {
        QhFree(q);
        return USB_ERR_NOMEM;
    }
    for (i = 0; i < n; i++) {
        q->Td[i] = (ULONG *)UsbDmaAlloc(TD_SIZE, &q->TdPhys[i]);
        if (q->Td[i] == NULL) {
            q->NTd = i;
            QhFree(q);
            return USB_ERR_NOMEM;
        }
    }
    q->NTd = n;
    q->Qh[1] = LINK_T;
    Pipe->HcPriv = q;
    QhLink(u, q);
    return USB_OK;
}

static void UhciPipeClose(USB_PIPE *Pipe)
{
    UHC *u = UHCP(Pipe->Dev->Hc);
    UQH *q = (UQH *)Pipe->HcPriv;

    if (q == NULL) {
        return;
    }
    QhUnlink(u, q);
    Pipe->HcPriv = NULL;
    QhFree(q);
}

static void FillTd(UQH *q, USB_PIPE *p, ULONG i, UCHAR Pid, ULONG Phys, ULONG Len, ULONG Toggle, BOOLEAN Spd)
{
    ULONG *t = q->Td[i];
    USB_DEV *d = p->Dev;
    ULONG ctl = TD_ACTIVE | TD_CERR3;

    if (d->Speed == USB_SPEED_LOW) {
        ctl |= TD_LS;
    }
    if (Spd) {
        ctl |= TD_SPD;
    }
    t[0] = LINK_T;
    t[1] = ctl | 0x7FF;
    t[2] = Pid | ((ULONG)d->Address << 8) | ((ULONG)p->Endpoint << 15) | ((Toggle & 1) << 19) |
           (((Len - 1) & 0x7FF) << 21);
    t[3] = Len ? Phys : 0;
    q->TdLen[i] = (USHORT)Len;
}

static int UhciSubmit(USB_XFER *X)
{
    USB_PIPE *p = X->Pipe;
    UQH *q = (UQH *)p->HcPriv;
    UHC *u = UHCP(p->Dev->Hc);
    ULONG mp = p->MaxPacket ? p->MaxPacket : 8;
    ULONG n = 0;
    ULONG i;
    ULONG toggle;

    if (q == NULL) {
        return USB_ERR_NODEV;
    }
    if (p->Type == USB_EP_CONTROL) {
        ULONG done = 0;
        UCHAR pid = X->DirIn ? PID_IN : PID_OUT;
        FillTd(q, p, n++, PID_SETUP, X->SetupPhys, 8, 0, FALSE);
        toggle = 1;
        while (done < X->Length) {
            ULONG len = X->Length - done;
            if (len > mp) {
                len = mp;
            }
            if (n + 1 >= q->NTd) {
                return USB_ERR_PARAM;
            }
            FillTd(q, p, n++, pid, X->Phys + done, len, toggle, (BOOLEAN)X->DirIn);
            toggle ^= 1;
            done += len;
        }
        q->StatusTd = n;
        FillTd(q, p, n++, (UCHAR)((X->Length == 0 || !X->DirIn) ? PID_IN : PID_OUT), 0, 0, 1, FALSE);
    } else {
        ULONG done = 0;
        UCHAR pid = p->DirIn ? PID_IN : PID_OUT;
        toggle = p->Toggle;
        do {
            ULONG len = X->Length - done;
            if (len > mp) {
                len = mp;
            }
            if (n >= q->NTd) {
                return USB_ERR_PARAM;
            }
            FillTd(q, p, n++, pid, X->Phys + done, len, toggle, (BOOLEAN)p->DirIn);
            toggle ^= 1;
            done += len;
        } while (done < X->Length);
    }
    for (i = 0; i + 1 < n; i++) {
        q->Td[i][0] = q->TdPhys[i + 1] | LINK_VF;
    }
    if (u->UseIrq) {
        q->Td[n - 1][1] |= TD_IOC;
    }
    q->Used = n;
    q->Qh[1] = q->TdPhys[0];
    UsbLog(LOG_TRACE, "uhci submit ep%u n=%u tog=%u frnum=%u\n", p->Endpoint, n, p->Toggle, Rd16(u, UR_FRNUM));
    return USB_OK;
}

static int TdStatus(ULONG ctl)
{
    if (ctl & TD_BABBLE) {
        return USB_ERR_BABBLE;
    }
    if (ctl & TD_STALLED) {
        if (ctl & (TD_CRCTO | TD_BITSTUFF | TD_DBERR)) {
            return USB_ERR_IO;
        }
        return USB_ERR_STALL;
    }
    return USB_ERR_IO;
}

static void CheckQh(UQH *q)
{
    USB_PIPE *p = q->Pipe;
    USB_XFER *X = p->Cur;
    ULONG actual = 0;
    ULONG i;
    BOOLEAN control = (BOOLEAN)(p->Type == USB_EP_CONTROL);

    if (X == NULL || q->Used == 0) {
        return;
    }
    for (i = 0; i < q->Used; i++) {
        ULONG ctl = q->Td[i][1];
        ULONG got;
        if (ctl & TD_ACTIVE) {
            return;
        }
        if (ctl & TD_ERRMASK) {
            q->Qh[1] = LINK_T;
            if (!control) {
                p->Toggle = (UCHAR)(((q->Td[i][2] >> 19) & 1));
            }
            UsbXferDone(X, TdStatus(ctl), actual);
            return;
        }
        got = ((ctl & 0x7FF) + 1) & 0x7FF;
        if (!control || (i != 0 && i != q->StatusTd)) {
            actual += got;
        }
        if (!control) {
            p->Toggle = (UCHAR)(((q->Td[i][2] >> 19) & 1) ^ 1);
        }
        if (got < q->TdLen[i] && i + 1 < q->Used) {
            if (control && i != q->StatusTd) {
                if (q->Td[q->StatusTd][1] & TD_ACTIVE) {
                    if ((q->Qh[1] & ~0xFUL) != q->TdPhys[q->StatusTd]) {
                        q->Qh[1] = q->TdPhys[q->StatusTd];
                    }
                    return;
                }
                i = q->StatusTd - 1;
                continue;
            }
            q->Qh[1] = LINK_T;
            UsbXferDone(X, USB_OK, actual);
            return;
        }
    }
    q->Qh[1] = LINK_T;
    UsbLog(LOG_TRACE, "uhci done ep%u actual=%u ctl=%08x\n", p->Endpoint, actual, q->Td[0][1]);
    UsbXferDone(X, USB_OK, actual);
}

static void UhciPoll(USB_HC *Hc)
{
    UHC *u = UHCP(Hc);
    int k;
    UQH *q;

    if (u == NULL) {
        return;
    }
    for (k = 0; k < 3; k++) {
        for (q = u->Skel[k].Next; q != NULL; q = q->Next) {
            CheckQh(q);
        }
    }
    if (!u->UseIrq) {
        USHORT sts = Rd16(u, UR_USBSTS);
        if (sts & 0x1F) {
            Wr16(u, UR_USBSTS, (USHORT)(sts & 0x1F));
            if (sts & (STS_HSE | STS_HCPE)) {
                UsbLog(LOG_ERR, "uhci: controller error %04x\n", sts);
            }
        }
    }
}

static BOOLEAN UhciInterrupt(USB_HC *Hc)
{
    UHC *u = UHCP(Hc);
    USHORT sts;

    if (u == NULL || !Hc->Running) {
        return FALSE;
    }
    sts = Rd16(u, UR_USBSTS);
    if (sts == 0xFFFF || !(sts & 0x1F)) {
        return FALSE;
    }
    Wr16(u, UR_USBSTS, (USHORT)(sts & 0x1F));
    return TRUE;
}

static void UhciCancel(USB_PIPE *Pipe)
{
    UQH *q = (UQH *)Pipe->HcPriv;
    OS_IRQL irql;
    ULONG i;

    if (q == NULL) {
        return;
    }
    irql = OsLock();
    q->Qh[1] = LINK_T;
    OsUnlock(irql);
    OsSleepMs(2);
    irql = OsLock();
    CheckQh(q);
    for (i = 0; i < q->Used; i++) {
        q->Td[i][1] &= ~TD_ACTIVE;
    }
    q->Used = 0;
    OsUnlock(irql);
}

static int UhciPipeReset(USB_PIPE *Pipe)
{
    UQH *q = (UQH *)Pipe->HcPriv;

    if (q != NULL) {
        q->Qh[1] = LINK_T;
    }
    Pipe->Toggle = 0;
    return USB_OK;
}

static int UhciDevInit(USB_DEV *Dev)
{
    (void)Dev;
    return USB_OK;
}

static int UhciDevUpdate(USB_DEV *Dev)
{
    (void)Dev;
    return USB_OK;
}

static void UhciDevFree(USB_DEV *Dev)
{
    (void)Dev;
}

const HCD_OPS UhciOps = {
    "UHCI",
    UhciStart,
    UhciStop,
    UhciPortStatus,
    UhciPortClearChange,
    UhciPortReset,
    UhciPortDisable,
    UhciDevInit,
    NULL,
    UhciDevUpdate,
    UhciDevFree,
    UhciPipeOpen,
    UhciPipeClose,
    UhciSubmit,
    UhciCancel,
    UhciPipeReset,
    UhciPoll,
    UhciInterrupt
};
