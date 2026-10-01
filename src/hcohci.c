/*
 * hcohci.c - OHCI host controller driver (low and full speed).
 *
 * Each pipe owns an endpoint descriptor on the control, bulk or
 * interrupt list. Completion is detected by comparing the ED head with
 * its tail; the done queue is only acknowledged.
 */

#include "usbnt.h"

#define OR_REVISION     0x00
#define OR_CONTROL      0x04
#define OR_CMDSTATUS    0x08
#define OR_INTSTATUS    0x0C
#define OR_INTENABLE    0x10
#define OR_INTDISABLE   0x14
#define OR_HCCA         0x18
#define OR_CTRLHEAD     0x20
#define OR_CTRLCUR      0x24
#define OR_BULKHEAD     0x28
#define OR_BULKCUR      0x2C
#define OR_FMINTERVAL   0x34
#define OR_PERIODICSTART 0x40
#define OR_LSTHRESHOLD  0x44
#define OR_RHDESCA      0x48
#define OR_RHDESCB      0x4C
#define OR_RHSTATUS     0x50
#define OR_RHPORT(n)    (0x54 + 4 * ((n) - 1))

#define CTL_CBSR3       0x00000003
#define CTL_PLE         0x00000004
#define CTL_CLE         0x00000010
#define CTL_BLE         0x00000020
#define CTL_HCFS_MASK   0x000000C0
#define CTL_HCFS_RESET  0x00000000
#define CTL_HCFS_OPER   0x00000080
#define CTL_IR          0x00000100

#define CS_HCR          0x00000001
#define CS_CLF          0x00000002
#define CS_BLF          0x00000004
#define CS_OCR          0x00000008

#define INT_SO          0x00000001
#define INT_WDH         0x00000002
#define INT_SF          0x00000004
#define INT_UE          0x00000010
#define INT_RHSC        0x00000040
#define INT_MIE         0x80000000

#define RH_CCS          0x00000001
#define RH_PES          0x00000002
#define RH_PRS          0x00000010
#define RH_PPS          0x00000100
#define RH_LSDA         0x00000200
#define RH_CSC          0x00010000
#define RH_PRSC         0x00100000
#define RH_CHANGES      0x001F0000

#define ED_SKIP         0x00004000
#define ED_LOWSPEED     0x00002000
#define ED_HALTED       0x00000001
#define ED_CARRY        0x00000002

#define TD_ROUNDING     0x00040000
#define TD_DP_SETUP     0x00000000
#define TD_DP_OUT       0x00080000
#define TD_DP_IN        0x00100000
#define TD_NOINT        0x00E00000
#define TD_T_DATA0      0x02000000
#define TD_T_DATA1      0x03000000
#define TD_CC_NOTACC    0xF0000000

#define CC_NOERROR      0
#define CC_CRC          1
#define CC_STALL        4
#define CC_NORESPONSE   5
#define CC_DATAOVERRUN  8
#define CC_DATAUNDERRUN 9

#define OHCI_MAX_TDS    12
#define TD_SIZE         32

typedef struct _OED {
    ULONG      *Ed;
    ULONG       EdPhys;
    ULONG      *Td[OHCI_MAX_TDS];
    ULONG       TdPhys[OHCI_MAX_TDS];
    ULONG       TdStart[OHCI_MAX_TDS];
    ULONG       TdLen[OHCI_MAX_TDS];
    UCHAR       Order[OHCI_MAX_TDS];
    UCHAR       Used;
    UCHAR       Dummy;
    UCHAR       Kind;           /* 0 int, 1 control, 2 bulk */
    UCHAR       Linked;
    UCHAR       DataIdx;        /* control: position of data TD or 0xFF */
    UCHAR       Pad[3];
    USB_PIPE   *Pipe;
    struct _OED *Next;
} OED;

typedef struct _OHC {
    USB_HC     *Hc;
    UCHAR      *Regs;
    ULONG      *Hcca;
    ULONG       HccaPhys;
    OED         Head[3];
    BOOLEAN     UseIrq;
} OHC;

#define OHCP(hc)    ((OHC *)(hc)->Priv)

static ULONG Rd(OHC *o, ULONG r) { return MmioRd32(o->Regs + r); }
static void  Wr(OHC *o, ULONG r, ULONG v) { MmioWr32(o->Regs + r, v); }

/* ------------------------------------------------------------------ */
/* Lists                                                                */
/* ------------------------------------------------------------------ */

static void EdLink(OHC *o, OED *q)
{
    OED *head = &o->Head[q->Kind];
    OS_IRQL irql;

    if (q->Linked) {
        return;
    }
    irql = OsLock();
    q->Ed[3] = head->Ed[3];
    q->Next = head->Next;
    head->Ed[3] = q->EdPhys;
    head->Next = q;
    q->Linked = 1;
    OsUnlock(irql);
}

static void EdUnlink(OHC *o, OED *q)
{
    OED *prev = &o->Head[q->Kind];
    OS_IRQL irql;

    ULONG ctl = 0;

    if (!q->Linked) {
        return;
    }
    q->Ed[0] |= ED_SKIP;
    if (q->Kind != 0) {
        ctl = Rd(o, OR_CONTROL);
        Wr(o, OR_CONTROL, ctl & ~(CTL_CLE | CTL_BLE));
    }
    OsSleepMs(2);
    irql = OsLock();
    while (prev != NULL && prev->Next != q) {
        prev = prev->Next;
    }
    if (prev != NULL) {
        prev->Ed[3] = q->Ed[3];
        prev->Next = q->Next;
    }
    q->Linked = 0;
    OsUnlock(irql);
    if (q->Kind != 0) {
        Wr(o, OR_CTRLCUR, 0);
        Wr(o, OR_BULKCUR, 0);
        Wr(o, OR_CONTROL, ctl);
        Wr(o, OR_CMDSTATUS, CS_CLF | CS_BLF);
    } else {
        OsSleepMs(2);
    }
}

/* ------------------------------------------------------------------ */
/* Controller                                                           */
/* ------------------------------------------------------------------ */

static int OhciStart(USB_HC *Hc)
{
    OHC *o;
    ULONG fi;
    ULONG ctl;
    ULONG desc;
    ULONG i;
    ULONG start;
    ULONG ports;

    o = (OHC *)OsAlloc(sizeof(OHC));
    if (o == NULL) {
        return USB_ERR_NOMEM;
    }
    Hc->Priv = o;
    o->Hc = Hc;
    o->Regs = Hc->Mmio;
    o->UseIrq = (BOOLEAN)OsGetConfig("UseInterrupts", 0);
    if (Rd(o, OR_REVISION) == 0xFFFFFFFF) {
        return USB_ERR_NODEV;
    }

    Wr(o, OR_INTDISABLE, INT_MIE);
    ctl = Rd(o, OR_CONTROL);
    if (ctl & CTL_IR) {
        start = OsTimeMs();
        Wr(o, OR_CMDSTATUS, CS_OCR);
        while ((Rd(o, OR_CONTROL) & CTL_IR) && OsTimeMs() - start < 1000) {
            OsSleepMs(10);
        }
        if (Rd(o, OR_CONTROL) & CTL_IR) {
            UsbLog(LOG_ERR, "ohci: BIOS did not release controller, forcing\n");
            Wr(o, OR_CONTROL, Rd(o, OR_CONTROL) & ~CTL_IR);
        } else {
            UsbLog(LOG_INFO, "ohci: BIOS handoff done\n");
        }
    } else if ((ctl & CTL_HCFS_MASK) != CTL_HCFS_RESET) {
        Wr(o, OR_CONTROL, CTL_HCFS_RESET);
        OsSleepMs(50);
    }

    fi = Rd(o, OR_FMINTERVAL) & 0x3FFF;
    if (fi < 11000 || fi > 13000) {
        fi = 11999;
    }
    Wr(o, OR_CMDSTATUS, CS_HCR);
    start = OsTimeMs();
    while (Rd(o, OR_CMDSTATUS) & CS_HCR) {
        if (OsTimeMs() - start > 50) {
            UsbLog(LOG_ERR, "ohci: reset failed\n");
            return USB_ERR_IO;
        }
        OsStallUs(10);
    }

    o->Hcca = (ULONG *)UsbDmaAlloc(256, &o->HccaPhys);
    if (o->Hcca == NULL) {
        return USB_ERR_NOMEM;
    }
    for (i = 0; i < 3; i++) {
        o->Head[i].Ed = (ULONG *)UsbDmaAlloc(32, &o->Head[i].EdPhys);
        if (o->Head[i].Ed == NULL) {
            return USB_ERR_NOMEM;
        }
        o->Head[i].Ed[0] = ED_SKIP;
        o->Head[i].Kind = (UCHAR)i;
    }
    for (i = 0; i < 32; i++) {
        o->Hcca[i] = o->Head[0].EdPhys;
    }

    Wr(o, OR_HCCA, o->HccaPhys);
    Wr(o, OR_CTRLHEAD, o->Head[1].EdPhys);
    Wr(o, OR_BULKHEAD, o->Head[2].EdPhys);
    Wr(o, OR_CTRLCUR, 0);
    Wr(o, OR_BULKCUR, 0);
    Wr(o, OR_INTDISABLE, 0xC000007F);
    Wr(o, OR_INTSTATUS, 0xC000007F);
    Wr(o, OR_FMINTERVAL, ((((fi - 210) * 6) / 7) << 16) | fi | ((Rd(o, OR_FMINTERVAL) & 0x80000000) ^ 0x80000000));
    Wr(o, OR_PERIODICSTART, (fi * 9) / 10);
    Wr(o, OR_LSTHRESHOLD, 0x628);
    Wr(o, OR_CONTROL, CTL_HCFS_OPER | CTL_PLE | CTL_CLE | CTL_BLE | CTL_CBSR3);
    OsSleepMs(2);
    if ((Rd(o, OR_CONTROL) & CTL_HCFS_MASK) != CTL_HCFS_OPER) {
        UsbLog(LOG_ERR, "ohci: controller did not become operational\n");
        return USB_ERR_IO;
    }
    if (o->UseIrq) {
        Wr(o, OR_INTENABLE, INT_MIE | INT_WDH | INT_UE);
    }

    desc = Rd(o, OR_RHDESCA);
    ports = desc & 0xFF;
    if (ports > USB_MAX_PORTS) {
        ports = USB_MAX_PORTS;
    }
    if (ports > 15) {
        ports = 15;
    }
    if (!(desc & 0x200)) {
        Wr(o, OR_RHSTATUS, 0x10000);
        if (desc & 0x100) {
            for (i = 1; i <= ports; i++) {
                Wr(o, OR_RHPORT(i), RH_PPS);
            }
        }
        i = ((desc >> 24) & 0xFF) * 2;
        OsSleepMs(i < 20 ? 20 : i);
    }
    Hc->NumPorts = (UCHAR)ports;
    Hc->MaxXfer = 0x10000;
    Hc->Running = 1;
    UsbLog(LOG_INFO, "ohci: %u ports, revision %x\n", ports, Rd(o, OR_REVISION) & 0xFF);
    return USB_OK;
}

static void OhciStop(USB_HC *Hc)
{
    OHC *o = OHCP(Hc);

    if (o == NULL) {
        return;
    }
    Wr(o, OR_INTDISABLE, INT_MIE);
    Wr(o, OR_CONTROL, CTL_HCFS_RESET);
    Hc->Running = 0;
}

/* ------------------------------------------------------------------ */
/* Root ports                                                           */
/* ------------------------------------------------------------------ */

static ULONG OhciPortStatus(USB_HC *Hc, int Port)
{
    OHC *o = OHCP(Hc);
    ULONG sc = Rd(o, OR_RHPORT(Port));
    ULONG ps = 0;

    if (sc == 0xFFFFFFFF) {
        return 0;
    }
    if (sc & RH_CCS) ps |= PS_CONNECT;
    if (sc & RH_PES) ps |= PS_ENABLE;
    if (sc & RH_CSC) ps |= PS_CHANGE;
    if (sc & RH_PPS) ps |= PS_POWER;
    ps |= (ULONG)((sc & RH_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL) << PS_SPEED_SHIFT;
    return ps;
}

static void OhciPortClearChange(USB_HC *Hc, int Port)
{
    OHC *o = OHCP(Hc);

    Wr(o, OR_RHPORT(Port), RH_CHANGES & ~RH_PRSC);
}

static int OhciPortReset(USB_HC *Hc, int Port, UCHAR *Speed)
{
    OHC *o = OHCP(Hc);
    ULONG sc;
    ULONG start;

    Wr(o, OR_RHPORT(Port), RH_PRS);
    start = OsTimeMs();
    for (;;) {
        OsSleepMs(10);
        sc = Rd(o, OR_RHPORT(Port));
        if (sc & RH_PRSC) {
            break;
        }
        if (OsTimeMs() - start > 200) {
            UsbLog(LOG_ERR, "ohci: port %d reset timeout\n", Port);
            return USB_ERR_TIMEOUT;
        }
    }
    Wr(o, OR_RHPORT(Port), RH_PRSC);
    OsSleepMs(10);
    sc = Rd(o, OR_RHPORT(Port));
    if (!(sc & RH_CCS)) {
        return USB_ERR_NODEV;
    }
    if (!(sc & RH_PES)) {
        return USB_ERR_IO;
    }
    *Speed = (UCHAR)((sc & RH_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL);
    return USB_OK;
}

static void OhciPortDisable(USB_HC *Hc, int Port)
{
    OHC *o = OHCP(Hc);

    Wr(o, OR_RHPORT(Port), RH_CCS);
}

/* ------------------------------------------------------------------ */
/* Pipes                                                                */
/* ------------------------------------------------------------------ */

static void EdSetEndpoint(OED *q, USB_PIPE *p)
{
    ULONG v = p->Dev->Address | ((ULONG)p->Endpoint << 7) | ((ULONG)p->MaxPacket << 16);

    if (p->Dev->Speed == USB_SPEED_LOW) {
        v |= ED_LOWSPEED;
    }
    q->Ed[0] = (q->Ed[0] & ED_SKIP) | v;
}

static void EdFree(OED *q)
{
    int i;

    for (i = 0; i < OHCI_MAX_TDS; i++) {
        UsbDmaFree(q->Td[i], TD_SIZE);
    }
    UsbDmaFree(q->Ed, 32);
    OsFree(q);
}

static int OhciPipeOpen(USB_PIPE *Pipe)
{
    OHC *o = OHCP(Pipe->Dev->Hc);
    OED *q;
    int i;

    if (Pipe->Type == USB_EP_ISOCH) {
        return USB_ERR_PARAM;
    }
    q = (OED *)OsAlloc(sizeof(OED));
    if (q == NULL) {
        return USB_ERR_NOMEM;
    }
    q->Pipe = Pipe;
    q->Kind = (UCHAR)((Pipe->Type == USB_EP_INTERRUPT) ? 0 : (Pipe->Type == USB_EP_CONTROL) ? 1 : 2);
    q->Ed = (ULONG *)UsbDmaAlloc(32, &q->EdPhys);
    if (q->Ed == NULL) {
        OsFree(q);
        return USB_ERR_NOMEM;
    }
    for (i = 0; i < OHCI_MAX_TDS; i++) {
        q->Td[i] = (ULONG *)UsbDmaAlloc(TD_SIZE, &q->TdPhys[i]);
        if (q->Td[i] == NULL) {
            EdFree(q);
            return USB_ERR_NOMEM;
        }
    }
    q->Dummy = 0;
    EdSetEndpoint(q, Pipe);
    q->Ed[1] = q->TdPhys[0];
    q->Ed[2] = q->TdPhys[0];
    Pipe->HcPriv = q;
    EdLink(o, q);
    return USB_OK;
}

static void OhciPipeClose(USB_PIPE *Pipe)
{
    OHC *o = OHCP(Pipe->Dev->Hc);
    OED *q = (OED *)Pipe->HcPriv;

    if (q == NULL) {
        return;
    }
    EdUnlink(o, q);
    Pipe->HcPriv = NULL;
    EdFree(q);
}

static int OhciDevUpdate(USB_DEV *Dev)
{
    OED *q = (OED *)Dev->Ep0.HcPriv;

    if (q != NULL) {
        EdSetEndpoint(q, &Dev->Ep0);
    }
    return USB_OK;
}

static void FillTd(OED *q, int k, ULONG Flags, ULONG Phys, ULONG Len)
{
    ULONG *t = q->Td[q->Order[k]];

    t[0] = Flags | TD_CC_NOTACC;
    t[1] = Len ? Phys : 0;
    t[2] = 0;
    t[3] = Len ? Phys + Len - 1 : 0;
    q->TdStart[k] = Phys;
    q->TdLen[k] = Len;
}

static int OhciSubmit(USB_XFER *X)
{
    USB_PIPE *p = X->Pipe;
    OED *q = (OED *)p->HcPriv;
    OHC *o = OHCP(p->Dev->Hc);
    int n = 0;
    int k;
    int idx;
    UCHAR newDummy;

    if (q == NULL) {
        return USB_ERR_NODEV;
    }
    /* first TD is the current dummy, the rest are the other free TDs */
    q->Order[0] = q->Dummy;
    idx = 1;
    for (k = 0; k < OHCI_MAX_TDS; k++) {
        if (k != q->Dummy) {
            q->Order[idx++] = (UCHAR)k;
        }
    }
    q->DataIdx = 0xFF;

    if (p->Type == USB_EP_CONTROL) {
        FillTd(q, n++, TD_DP_SETUP | TD_T_DATA0 | TD_NOINT, X->SetupPhys, 8);
        if (X->Length != 0) {
            q->DataIdx = (UCHAR)n;
            FillTd(q, n++, (X->DirIn ? TD_DP_IN : TD_DP_OUT) | TD_T_DATA1 | TD_NOINT | TD_ROUNDING, X->Phys, X->Length);
        }
        FillTd(q, n++, ((X->Length == 0 || !X->DirIn) ? TD_DP_IN : TD_DP_OUT) | TD_T_DATA1 |
               (o->UseIrq ? 0 : TD_NOINT), 0, 0);
    } else {
        ULONG done = 0;
        ULONG dp = p->DirIn ? TD_DP_IN : TD_DP_OUT;
        do {
            ULONG phys = X->Phys + done;
            ULONG len = X->Length - done;
            ULONG room = 0x2000 - (phys & 0xFFF);
            if (len > room) {
                len = room - room % p->MaxPacket;
            }
            if (n + 1 >= OHCI_MAX_TDS) {
                return USB_ERR_PARAM;
            }
            FillTd(q, n++, dp | TD_NOINT, phys, len);
            done += len;
        } while (done < X->Length);
        q->Td[q->Order[n - 1]][0] |= TD_ROUNDING;
        if (o->UseIrq) {
            q->Td[q->Order[n - 1]][0] &= ~TD_NOINT;
        }
    }
    newDummy = q->Order[n];
    for (k = 0; k < n; k++) {
        q->Td[q->Order[k]][2] = q->TdPhys[q->Order[k + 1]];
    }
    UsbMemSet(q->Td[newDummy], 0, 16);
    q->Used = (UCHAR)n;
    q->Dummy = newDummy;
    X->Hc[0] = q->TdPhys[newDummy];
    q->Ed[1] = q->TdPhys[newDummy];
    if (q->Kind == 1) {
        Wr(o, OR_CMDSTATUS, CS_CLF);
    } else if (q->Kind == 2) {
        Wr(o, OR_CMDSTATUS, CS_BLF);
    }
    return USB_OK;
}

static int CcStatus(ULONG cc)
{
    switch (cc) {
    case CC_STALL:        return USB_ERR_STALL;
    case CC_DATAOVERRUN:  return USB_ERR_BABBLE;
    default:              return USB_ERR_IO;
    }
}

static void EdSkipRest(OED *q)
{
    q->Ed[2] = (q->Ed[1] & ~0xFUL) | (q->Ed[2] & ED_CARRY);
}

static void CheckEd(OED *q)
{
    USB_PIPE *p = q->Pipe;
    USB_XFER *X = p->Cur;
    ULONG head;
    ULONG actual = 0;
    int k;
    BOOLEAN control = (BOOLEAN)(p->Type == USB_EP_CONTROL);

    if (X == NULL || q->Used == 0) {
        return;
    }
    head = q->Ed[2];
    if ((head & ~0xFUL) != X->Hc[0] && !(head & ED_HALTED)) {
        return;
    }
    for (k = 0; k < q->Used; k++) {
        ULONG *t = q->Td[q->Order[k]];
        ULONG cc = t[0] >> 28;
        ULONG got;
        if (cc >= 0xE) {
            break;
        }
        got = (t[1] == 0) ? q->TdLen[k] : t[1] - q->TdStart[k];
        if (!control || k == q->DataIdx) {
            actual += got;
        }
        if (cc == CC_NOERROR) {
            continue;
        }
        if (cc == CC_DATAUNDERRUN) {
            EdSkipRest(q);
            q->Used = 0;
            UsbXferDone(X, USB_OK, actual);
            return;
        }
        EdSkipRest(q);
        q->Used = 0;
        UsbXferDone(X, CcStatus(cc), actual);
        return;
    }
    if (head & ED_HALTED) {
        EdSkipRest(q);
        q->Used = 0;
        UsbXferDone(X, USB_ERR_IO, actual);
        return;
    }
    q->Used = 0;
    UsbXferDone(X, USB_OK, actual);
}

static void AckDone(OHC *o)
{
    if (Rd(o, OR_INTSTATUS) & INT_WDH) {
        o->Hcca[33] = 0;
        Wr(o, OR_INTSTATUS, INT_WDH);
    }
}

static void OhciPoll(USB_HC *Hc)
{
    OHC *o = OHCP(Hc);
    int k;
    OED *q;

    if (o == NULL) {
        return;
    }
    if (!o->UseIrq) {
        AckDone(o);
        if (Rd(o, OR_INTSTATUS) & INT_UE) {
            UsbLog(LOG_ERR, "ohci: unrecoverable error\n");
            Wr(o, OR_INTSTATUS, INT_UE);
        }
    }
    for (k = 0; k < 3; k++) {
        for (q = o->Head[k].Next; q != NULL; q = q->Next) {
            CheckEd(q);
        }
    }
}

static BOOLEAN OhciInterrupt(USB_HC *Hc)
{
    OHC *o = OHCP(Hc);
    ULONG sts;

    if (o == NULL || !Hc->Running) {
        return FALSE;
    }
    sts = Rd(o, OR_INTSTATUS);
    if (sts == 0xFFFFFFFF) {
        return FALSE;
    }
    sts &= Rd(o, OR_INTENABLE);
    if (!(sts & 0x7F)) {
        return FALSE;
    }
    if (sts & INT_WDH) {
        o->Hcca[33] = 0;
    }
    Wr(o, OR_INTSTATUS, sts & 0x7F);
    return TRUE;
}

static void OhciCancel(USB_PIPE *Pipe)
{
    OED *q = (OED *)Pipe->HcPriv;
    OS_IRQL irql;

    if (q == NULL) {
        return;
    }
    q->Ed[0] |= ED_SKIP;
    OsSleepMs(2);
    irql = OsLock();
    CheckEd(q);
    EdSkipRest(q);
    q->Ed[2] &= ~ED_HALTED;
    q->Used = 0;
    q->Ed[0] &= ~ED_SKIP;
    OsUnlock(irql);
}

static int OhciPipeReset(USB_PIPE *Pipe)
{
    OED *q = (OED *)Pipe->HcPriv;
    OS_IRQL irql;

    if (q == NULL) {
        return USB_ERR_NODEV;
    }
    q->Ed[0] |= ED_SKIP;
    OsSleepMs(2);
    irql = OsLock();
    q->Ed[2] = q->Ed[1] & ~0xFUL;
    q->Ed[0] &= ~ED_SKIP;
    OsUnlock(irql);
    Pipe->Toggle = 0;
    return USB_OK;
}

static int OhciDevInit(USB_DEV *Dev)
{
    (void)Dev;
    return USB_OK;
}

static void OhciDevFree(USB_DEV *Dev)
{
    (void)Dev;
}

const HCD_OPS OhciOps = {
    "OHCI",
    OhciStart,
    OhciStop,
    OhciPortStatus,
    OhciPortClearChange,
    OhciPortReset,
    OhciPortDisable,
    OhciDevInit,
    NULL,
    OhciDevUpdate,
    OhciDevFree,
    OhciPipeOpen,
    OhciPipeClose,
    OhciSubmit,
    OhciCancel,
    OhciPipeReset,
    OhciPoll,
    OhciInterrupt
};
