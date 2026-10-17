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
#define OR_FMNUMBER     0x3C
#define OR_PERIODICSTART 0x40
#define OR_LSTHRESHOLD  0x44
#define OR_RHDESCA      0x48
#define OR_RHDESCB      0x4C
#define OR_RHSTATUS     0x50
#define OR_RHPORT(n)    (0x54 + 4 * ((n) - 1))

#define CTL_CBSR3       0x00000003
#define CTL_PLE         0x00000004
#define CTL_IE          0x00000008
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
#define ED_ISO          0x00008000
#define ED_DIR_OUT      0x00000800
#define ED_DIR_IN       0x00001000
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
    OED         Head[4];        /* interrupt, control, bulk, isochronous */
    BOOLEAN     UseIrq;
    ULONG       FrameExt;       /* 32 bit frame counter */
    ULONG       FrameRaw;
    struct _OISO *IsoList;
} OHC;

/*
 * Isochronous pipe: an ED on the isochronous list (behind the interrupt
 * EDs of the periodic list) with a ring of isochronous TDs. The ring entry
 * at Head is the ED's dummy tail; ITDs are linked in ring order.
 */
#define OISO_TDS        64

typedef struct _OISO {
    OED         Q;              /* Ed, EdPhys, Kind 3, Linked, Next */
    ULONG      *Td[OISO_TDS];
    ULONG       TdPhys[OISO_TDS];
    UCHAR       First[OISO_TDS];    /* first packet of the ITD */
    UCHAR       Count[OISO_TDS];    /* packets in the ITD */
    ULONG       Head;
    ULONG       Used;
    BOOLEAN     Started;
    USB_PIPE   *Pipe;
    struct _OISO *IsoNext;
} OISO;

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
    if (q->Kind == 1 || q->Kind == 2) {
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
    if (q->Kind == 1 || q->Kind == 2) {
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
    for (i = 0; i < 4; i++) {
        o->Head[i].Ed = (ULONG *)UsbDmaAlloc(32, &o->Head[i].EdPhys);
        if (o->Head[i].Ed == NULL) {
            return USB_ERR_NOMEM;
        }
        o->Head[i].Ed[0] = ED_SKIP;
        o->Head[i].Kind = (UCHAR)i;
    }
    /* periodic list: interrupt EDs, then the isochronous EDs */
    o->Head[0].Ed[3] = o->Head[3].EdPhys;
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
    Wr(o, OR_CONTROL, CTL_HCFS_OPER | CTL_PLE | CTL_IE | CTL_CLE | CTL_BLE | CTL_CBSR3);
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
    Hc->IsoOk = 1;
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
/* Isochronous                                                          */
/* ------------------------------------------------------------------ */

#define ITD_CC_NOTACC   0xF0000000

static int CcStatus(ULONG cc);
#define PSW_NOTACC      0xE000

/* lock held */
static void FrameUpdate(OHC *o)
{
    ULONG raw = Rd(o, OR_FMNUMBER) & 0xFFFF;

    o->FrameExt += (raw - o->FrameRaw) & 0xFFFF;
    o->FrameRaw = raw;
}

static void IsoFree(OISO *iso)
{
    ULONG i;

    for (i = 0; i < OISO_TDS; i++) {
        if (iso->Td[i] != NULL) {
            UsbDmaFree(iso->Td[i], TD_SIZE);
        }
    }
    if (iso->Q.Ed != NULL) {
        UsbDmaFree(iso->Q.Ed, 32);
    }
    OsFree(iso);
}

static int IsoOpen(OHC *o, USB_PIPE *Pipe)
{
    OISO *iso = (OISO *)OsAlloc(sizeof(OISO));
    ULONG i;
    OS_IRQL irql;

    if (iso == NULL) {
        return USB_ERR_NOMEM;
    }
    iso->Q.Ed = (ULONG *)UsbDmaAlloc(32, &iso->Q.EdPhys);
    if (iso->Q.Ed == NULL) {
        IsoFree(iso);
        return USB_ERR_NOMEM;
    }
    for (i = 0; i < OISO_TDS; i++) {
        iso->Td[i] = (ULONG *)UsbDmaAlloc(TD_SIZE, &iso->TdPhys[i]);
        if (iso->Td[i] == NULL) {
            IsoFree(iso);
            return USB_ERR_NOMEM;
        }
    }
    for (i = 0; i < OISO_TDS; i++) {
        iso->Td[i][2] = iso->TdPhys[(i + 1) % OISO_TDS];
    }
    iso->Pipe = Pipe;
    iso->Q.Pipe = Pipe;
    iso->Q.Kind = 3;
    iso->Q.Ed[0] = Pipe->Dev->Address | ((ULONG)Pipe->Endpoint << 7) | (Pipe->DirIn ? ED_DIR_IN : ED_DIR_OUT) |
                   ED_ISO | ((ULONG)(Pipe->MaxPacket & 0x7FF) << 16);
    iso->Q.Ed[1] = iso->TdPhys[0];
    iso->Q.Ed[2] = iso->TdPhys[0];
    Pipe->HcPriv = iso;
    irql = OsLock();
    iso->IsoNext = o->IsoList;
    o->IsoList = iso;
    OsUnlock(irql);
    EdLink(o, &iso->Q);
    return USB_OK;
}

static void IsoCancel(OISO *iso)
{
    OS_IRQL irql;

    iso->Q.Ed[0] |= ED_SKIP;
    OsSleepMs(2);
    irql = OsLock();
    iso->Q.Ed[2] = iso->TdPhys[iso->Head];
    iso->Q.Ed[1] = iso->TdPhys[iso->Head];
    iso->Used = 0;
    iso->Started = FALSE;
    iso->Q.Ed[0] &= ~ED_SKIP;
    OsUnlock(irql);
}

static void IsoClose(OHC *o, USB_PIPE *Pipe)
{
    OISO *iso = (OISO *)Pipe->HcPriv;
    OISO **pp;
    OS_IRQL irql;

    EdUnlink(o, &iso->Q);
    irql = OsLock();
    for (pp = &o->IsoList; *pp != NULL; pp = &(*pp)->IsoNext) {
        if (*pp == iso) {
            *pp = iso->IsoNext;
            break;
        }
    }
    OsUnlock(irql);
    Pipe->HcPriv = NULL;
    IsoFree(iso);
}

/*
 * Lock held. Packets go into ITDs of up to 8 frames whose data is
 * contiguous and touches at most two pages.
 */
static int IsoSubmit(OHC *o, USB_XFER *X)
{
    USB_PIPE *p = X->Pipe;
    OISO *iso = (OISO *)p->HcPriv;
    ULONG per = p->IsoSlots / 8;
    ULONG n = X->IsoCount;
    ULONG start;
    ULONG i, j, k, ntd = 0;
    ULONG idx[OISO_TDS];

    if (per == 0) {
        per = 1;
    }
    FrameUpdate(o);
    start = p->IsoNext;
    if (!iso->Started || (LONG)(start - (o->FrameExt + 2)) < 0) {
        if (iso->Started) {
            p->IsoLate = 1;
        }
        start = o->FrameExt + USB_ISO_RESTART_FRAMES;
    }
    if ((LONG)(start + n * per - o->FrameExt) > 30000) {
        return USB_ERR_BUSY;
    }
    for (i = 0; i < n; i = j) {
        ULONG first = X->Phys + X->Iso[i].Offset;
        ULONG last = first + X->Iso[i].Length - 1;
        ULONG *t;
        ULONG bp0;
        if (X->Iso[i].Length == 0 || X->Iso[i].Length > p->MaxPacket) {
            return USB_ERR_PARAM;
        }
        if (iso->Used + ntd + 2 > OISO_TDS) {
            return USB_ERR_BUSY;
        }
        j = i + 1;
        while (per == 1 && j < n && j - i < 8 && X->Iso[j].Length != 0 && X->Iso[j].Length <= p->MaxPacket &&
               X->Iso[j].Offset == X->Iso[j - 1].Offset + X->Iso[j - 1].Length &&
               ((first + (X->Iso[j].Offset - X->Iso[i].Offset) + X->Iso[j].Length - 1) >> 12) - (first >> 12) <= 1) {
            last = first + (X->Iso[j].Offset - X->Iso[i].Offset) + X->Iso[j].Length - 1;
            j++;
        }
        if ((last >> 12) - (first >> 12) > 1) {
            return USB_ERR_PARAM;
        }
        k = (iso->Head + ntd) % OISO_TDS;
        idx[ntd++] = k;
        t = iso->Td[k];
        bp0 = first & ~0xFFFUL;
        t[0] = ((start + i * per) & 0xFFFF) | ((ULONG)(j - i - 1) << 24) | ITD_CC_NOTACC |
               ((o->UseIrq && j == n) ? 0 : TD_NOINT);
        t[1] = bp0;
        t[2] = iso->TdPhys[(k + 1) % OISO_TDS];
        t[3] = last;
        for (k = 0; k < 4; k++) {
            ULONG a = i + 2 * k, b = i + 2 * k + 1;
            ULONG lo = (a < j) ? (PSW_NOTACC | ((X->Phys + X->Iso[a].Offset - bp0) & 0x1FFF)) : 0;
            ULONG hi = (b < j) ? (PSW_NOTACC | ((X->Phys + X->Iso[b].Offset - bp0) & 0x1FFF)) : 0;
            t[4 + k] = lo | (hi << 16);
        }
        iso->First[idx[ntd - 1]] = (UCHAR)i;
        iso->Count[idx[ntd - 1]] = (UCHAR)(j - i);
    }
    for (i = 0; i < n; i++) {
        X->Iso[i].Actual = 0;
        X->Iso[i].Status = USB_ERR_BUSY;
    }
    X->Hc[0] = start;
    X->Hc[1] = iso->Head;
    X->Hc[2] = per;
    X->Hc[3] = ntd;
    X->IsoFrame = start;
    iso->Head = (iso->Head + ntd) % OISO_TDS;
    iso->Used += ntd;
    iso->Started = TRUE;
    p->IsoNext = start + n * per;
    /* the new dummy is the ED tail: the HC may now run the new ITDs */
    iso->Q.Ed[1] = iso->TdPhys[iso->Head];
    return USB_OK;
}

/* lock held: completes transfers whose last frame has passed and whose ITDs the HC retired */
static void IsoPoll(OHC *o)
{
    OISO *iso;

    if (o->IsoList == NULL) {
        return;
    }
    FrameUpdate(o);
    for (iso = o->IsoList; iso != NULL; iso = iso->IsoNext) {
        USB_PIPE *p = iso->Pipe;
        USB_XFER *X;
        while ((X = p->Cur) != NULL && X->Busy) {
            ULONG n = X->IsoCount;
            ULONG last = X->Hc[0] + (n - 1) * X->Hc[2];
            ULONG ntd = X->Hc[3];
            ULONG head = iso->Q.Ed[2] & ~0xFUL;
            ULONG total = 0;
            ULONG i, k, hidx = OISO_TDS;
            if ((LONG)(o->FrameExt - (last + 1)) < 0) {
                break;
            }
            for (k = 0; k < ntd; k++) {
                if (iso->TdPhys[(X->Hc[1] + k) % OISO_TDS] == head) {
                    hidx = k;
                }
            }
            if (hidx != OISO_TDS) {
                /* not retired yet: expired ITDs leave on the next visit */
                if ((LONG)(o->FrameExt - (last + 8)) < 0) {
                    break;
                }
                /* the ED is not being serviced: take the ITDs back */
                iso->Q.Ed[0] |= ED_SKIP;
                iso->Q.Ed[2] = iso->TdPhys[(X->Hc[1] + ntd) % OISO_TDS] | (iso->Q.Ed[2] & 2);
                iso->Q.Ed[0] &= ~ED_SKIP;
            }
            for (k = 0; k < ntd; k++) {
                ULONG *t = iso->Td[(X->Hc[1] + k) % OISO_TDS];
                ULONG f = iso->First[(X->Hc[1] + k) % OISO_TDS];
                ULONG c = iso->Count[(X->Hc[1] + k) % OISO_TDS];
                for (i = 0; i < c; i++) {
                    USB_ISO_PACKET *pk = &X->Iso[f + i];
                    ULONG psw = (t[4 + i / 2] >> ((i & 1) * 16)) & 0xFFFF;
                    ULONG cc = psw >> 12;
                    if (cc >= 0xE) {
                        pk->Status = USB_ERR_IO;
                        pk->Actual = 0;
                    } else if (cc == CC_NOERROR || cc == CC_DATAUNDERRUN) {
                        pk->Status = USB_OK;
                        pk->Actual = (USHORT)(p->DirIn ? (psw & 0x7FF) : pk->Length);
                    } else {
                        pk->Status = CcStatus(cc);
                        pk->Actual = (USHORT)(p->DirIn ? (psw & 0x7FF) : 0);
                    }
                    total += pk->Actual;
                }
            }
            iso->Used -= (iso->Used >= ntd) ? ntd : iso->Used;
            UsbXferDone(X, USB_OK, total);
        }
    }
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
        return IsoOpen(o, Pipe);
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
    if (Pipe->Type == USB_EP_ISOCH) {
        IsoClose(o, Pipe);
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
    if (p->Type == USB_EP_ISOCH) {
        return IsoSubmit(o, X);
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
    IsoPoll(o);
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
    if (Pipe->Type == USB_EP_ISOCH) {
        IsoCancel((OISO *)q);
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
    if (Pipe->Type == USB_EP_ISOCH) {
        IsoCancel((OISO *)q);
        return USB_OK;
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
