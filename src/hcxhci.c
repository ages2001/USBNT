/*
 * hcxhci.c - xHCI host controller driver.
 *
 * Supports control, bulk and interrupt transfers on USB 2.0 and USB 3.x
 * root ports and behind USB 2.0 hubs (split transactions are done by the
 * controller). One transfer is outstanding per endpoint; every transfer
 * buffer is physically contiguous and never crosses a 64 KB boundary, so
 * a bulk or interrupt TD is always a single Normal TRB.
 */

#include "usbnt.h"

/* Capability registers */
#define XCAP_CAPLENGTH      0x00
#define XCAP_HCSPARAMS1     0x04
#define XCAP_HCSPARAMS2     0x08
#define XCAP_HCCPARAMS1     0x10
#define XCAP_DBOFF          0x14
#define XCAP_RTSOFF         0x18

/* Operational registers */
#define XOP_USBCMD          0x00
#define XOP_USBSTS          0x04
#define XOP_PAGESIZE        0x08
#define XOP_DNCTRL          0x14
#define XOP_CRCR            0x18
#define XOP_DCBAAP          0x30
#define XOP_CONFIG          0x38
#define XOP_PORTSC(n)       (0x400 + 0x10 * ((n) - 1))

#define CMD_RS              0x00000001
#define CMD_HCRST           0x00000002
#define CMD_INTE            0x00000004

#define STS_HCH             0x00000001
#define STS_HSE             0x00000004
#define STS_EINT            0x00000008
#define STS_PCD             0x00000010
#define STS_CNR             0x00000800
#define STS_HCE             0x00001000

/* PORTSC */
#define PORT_CCS            0x00000001
#define PORT_PED            0x00000002
#define PORT_OCA            0x00000008
#define PORT_PR             0x00000010
#define PORT_PLS_MASK       0x000001E0
#define PORT_PP             0x00000200
#define PORT_SPEED_SHIFT    10
#define PORT_CSC            0x00020000
#define PORT_PEC            0x00040000
#define PORT_WRC            0x00080000
#define PORT_OCC            0x00100000
#define PORT_PRC            0x00200000
#define PORT_PLC            0x00400000
#define PORT_CEC            0x00800000
#define PORT_WPR            0x80000000
#define PORT_CHANGE_BITS    0x00FE0000
#define PORT_PRESERVE       0x4F00FFE9  /* RO and RW-preserve bits */

/* Runtime registers, interrupter 0 */
#define XRT_IMAN            0x20
#define XRT_IMOD            0x24
#define XRT_ERSTSZ          0x28
#define XRT_ERSTBA          0x30
#define XRT_ERDP            0x38

/* TRB types */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_DISABLE_SLOT    10
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_EVALUATE_CTX    13
#define TRB_RESET_EP        14
#define TRB_STOP_EP         15
#define TRB_SET_TR_DEQ      16
#define TRB_NOOP_CMD        23
#define TRB_TRANSFER_EVENT  32
#define TRB_CMD_COMPLETION  33
#define TRB_PORT_STATUS     34
#define TRB_HC_EVENT        37

#define TRB_C               0x00000001
#define TRB_TC              0x00000002
#define TRB_CH              0x00000010
#define TRB_ISP             0x00000004
#define TRB_IOC             0x00000020
#define TRB_IDT             0x00000040
#define TRB_BSR             0x00000200
#define TRB_DIR_IN          0x00010000
#define TRB_TYPE(t)         ((ULONG)(t) << 10)
#define TRB_GET_TYPE(d3)    (((d3) >> 10) & 0x3F)

/* Completion codes */
#define CC_SUCCESS          1
#define CC_DATA_BUFFER      2
#define CC_BABBLE           3
#define CC_TRANSACTION      4
#define CC_TRB_ERROR        5
#define CC_STALL            6
#define CC_SHORT_PACKET     13
#define CC_CONTEXT_STATE    19
#define CC_STOPPED          26
#define CC_STOPPED_LEN      27

#define SEG_TRBS            256         /* TRBs per ring segment, the last is a link */
#define RING_MAX_SEGS       4
#define BULK_SEGS           4
#define EVENT_TRBS          256
#define XHCI_MAX_SLOTS      64
#define XHCI_MAX_STREAMS    32
#define CMD_TIMEOUT_MS      2000

/* Endpoint context states */
#define EP_STATE_DISABLED   0
#define EP_STATE_RUNNING    1
#define EP_STATE_HALTED     2
#define EP_STATE_STOPPED    3
#define EP_STATE_ERROR      4

typedef struct _XRING {
    ULONG      *Seg[RING_MAX_SEGS];
    ULONG       SegPhys[RING_MAX_SEGS];
    USB_XFER  **Owner;          /* transfer owning each TRB */
    ULONG      *Off;            /* byte offset of each TRB inside its transfer */
    ULONG       NSeg;
    ULONG       EnqSeg;
    ULONG       Enq;
    ULONG       Cycle;
    ULONG       Used;           /* TRBs held by queued transfers */
    ULONG      *Last;           /* last TRB written */
    ULONG       LastIdx;
} XRING;

typedef struct _XSTREAM {
    XRING       Ring;
    USB_PIPE   *Pipe;
} XSTREAM;

typedef struct _XEP {
    XRING       Ring;
    UCHAR       Dci;
    UCHAR       Pad[3];
    USB_PIPE   *Pipe;
    ULONG       NumStreams;     /* streams in use, 0 = plain ring */
    ULONG       PsaSize;        /* primary stream array entries */
    ULONG      *Psa;
    ULONG       PsaPhys;
    XSTREAM    *Str[XHCI_MAX_STREAMS + 1];
} XEP;

typedef struct _XSLOT {
    ULONG       SlotId;
    UCHAR      *Out;
    ULONG       OutPhys;
    ULONG       OutSize;
    UCHAR      *In;
    ULONG       InPhys;
    ULONG       InSize;
    ULONG       MaxDci;
    XEP        *Ep[32];
} XSLOT;

typedef struct _XHC {
    USB_HC     *Hc;
    UCHAR      *Cap;
    UCHAR      *Op;
    UCHAR      *Rt;
    UCHAR      *Db;
    ULONG       MaxSlots;
    ULONG       CtxSize;
    ULONG      *Dcbaa;
    ULONG       DcbaaPhys;
    XRING       Cmd;
    ULONG      *Evt;
    ULONG       EvtPhys;
    ULONG       EvtDeq;
    ULONG       EvtCycle;
    ULONG      *Erst;
    ULONG       ErstPhys;
    ULONG      *ScratchArr;
    ULONG       ScratchPhys;
    ULONG       ScratchCount;
    volatile ULONG CmdBusy;
    ULONG       CmdTrbPhys;
    ULONG       CmdCode;
    ULONG       CmdSlot;
    OS_EVENT    CmdEvent;
    UCHAR       PortProto[USB_MAX_PORTS + 1];
    UCHAR       SsGbps[16];     /* link rate per protocol speed id */
    XSLOT      *Slots[XHCI_MAX_SLOTS + 1];
    BOOLEAN     UseIrq;
} XHC;

#define XHCP(hc)    ((XHC *)(hc)->Priv)

static ULONG OpRd(XHC *x, ULONG r) { return MmioRd32(x->Op + r); }
static void  OpWr(XHC *x, ULONG r, ULONG v) { MmioWr32(x->Op + r, v); }
static ULONG RtRd(XHC *x, ULONG r) { return MmioRd32(x->Rt + r); }
static void  RtWr(XHC *x, ULONG r, ULONG v) { MmioWr32(x->Rt + r, v); }

static BOOLEAN WaitOp(XHC *x, ULONG Reg, ULONG Mask, ULONG Val, ULONG TimeoutMs)
{
    ULONG start = OsTimeMs();

    for (;;) {
        ULONG v = OpRd(x, Reg);
        if (v == 0xFFFFFFFF) {
            return FALSE;
        }
        if ((v & Mask) == Val) {
            return TRUE;
        }
        if (OsTimeMs() - start > TimeoutMs) {
            return FALSE;
        }
        OsStallUs(200);
    }
}

/* ------------------------------------------------------------------ */
/* Rings                                                                */
/* ------------------------------------------------------------------ */

static void RingFree(XRING *r)
{
    ULONG i;

    for (i = 0; i < RING_MAX_SEGS; i++) {
        if (r->Seg[i] != NULL) {
            UsbDmaFree(r->Seg[i], SEG_TRBS * 16);
            r->Seg[i] = NULL;
        }
    }
    if (r->Owner != NULL) {
        OsFree(r->Owner);
        r->Owner = NULL;
    }
    if (r->Off != NULL) {
        OsFree(r->Off);
        r->Off = NULL;
    }
    r->NSeg = 0;
}

/* Allocates (first call) or clears a ring of NSeg segments */
static int RingInit(XRING *r, ULONG NSeg)
{
    ULONG i;

    if (r->NSeg != 0 && r->NSeg != NSeg) {
        RingFree(r);
    }
    if (r->NSeg == 0) {
        for (i = 0; i < NSeg; i++) {
            r->Seg[i] = (ULONG *)UsbDmaAlloc(SEG_TRBS * 16, &r->SegPhys[i]);
            if (r->Seg[i] == NULL) {
                RingFree(r);
                return USB_ERR_NOMEM;
            }
        }
        r->Owner = (USB_XFER **)OsAlloc(NSeg * SEG_TRBS * sizeof(USB_XFER *));
        r->Off = (ULONG *)OsAlloc(NSeg * SEG_TRBS * sizeof(ULONG));
        r->NSeg = NSeg;
        if (r->Owner == NULL || r->Off == NULL) {
            RingFree(r);
            return USB_ERR_NOMEM;
        }
    }
    for (i = 0; i < NSeg; i++) {
        ULONG *link = r->Seg[i] + (SEG_TRBS - 1) * 4;
        UsbMemSet(r->Seg[i], 0, SEG_TRBS * 16);
        link[0] = r->SegPhys[(i + 1) % NSeg];
        link[1] = 0;
        link[2] = 0;
        link[3] = TRB_TYPE(TRB_LINK) | ((i == NSeg - 1) ? TRB_TC : 0);
    }
    UsbMemSet(r->Owner, 0, NSeg * SEG_TRBS * sizeof(USB_XFER *));
    r->EnqSeg = 0;
    r->Enq = 0;
    r->Cycle = 1;
    r->Used = 0;
    return USB_OK;
}

static ULONG RingCapacity(XRING *r)
{
    return r->NSeg * (SEG_TRBS - 1) - 1;
}

/*
 * Writes one TRB and returns its physical address. With Hold the cycle
 * bit is left inverted so the controller does not start a transfer whose
 * later TRBs are still being written; RingRelease hands it over.
 */
static ULONG RingPutEx(XRING *r, ULONG d0, ULONG d1, ULONG d2, ULONG d3, BOOLEAN Hold)
{
    ULONG *t = r->Seg[r->EnqSeg] + r->Enq * 4;
    ULONG phys = r->SegPhys[r->EnqSeg] + r->Enq * 16;
    ULONG c = r->Cycle ? TRB_C : 0;

    if (Hold) {
        c ^= TRB_C;
    }
    t[0] = d0;
    t[1] = d1;
    t[2] = d2;
    ((volatile ULONG *)t)[3] = (d3 & ~TRB_C) | c;
    r->Last = t;
    r->LastIdx = r->EnqSeg * SEG_TRBS + r->Enq;
    r->Enq++;
    if (r->Enq == SEG_TRBS - 1) {
        ULONG *link = r->Seg[r->EnqSeg] + (SEG_TRBS - 1) * 4;
        BOOLEAN last = (BOOLEAN)(r->EnqSeg == r->NSeg - 1);
        ((volatile ULONG *)link)[3] = TRB_TYPE(TRB_LINK) | (last ? TRB_TC : 0) | (d3 & TRB_CH) |
                                      (r->Cycle ? TRB_C : 0);
        if (last) {
            r->Cycle ^= 1;
        }
        r->EnqSeg = (r->EnqSeg + 1) % r->NSeg;
        r->Enq = 0;
    }
    return phys;
}

static ULONG RingPut(XRING *r, ULONG d0, ULONG d1, ULONG d2, ULONG d3)
{
    return RingPutEx(r, d0, d1, d2, d3, FALSE);
}

static void RingRelease(ULONG *Trb)
{
    ((volatile ULONG *)Trb)[3] ^= TRB_C;
}

static ULONG RingEnqPhys(XRING *r)
{
    return r->SegPhys[r->EnqSeg] + r->Enq * 16;
}

/* Ring index of a TRB address, or -1 */
static LONG RingIndex(XRING *r, ULONG Ptr)
{
    ULONG i;

    for (i = 0; i < r->NSeg; i++) {
        if (Ptr >= r->SegPhys[i] && Ptr < r->SegPhys[i] + SEG_TRBS * 16) {
            return (LONG)(i * SEG_TRBS + (Ptr - r->SegPhys[i]) / 16);
        }
    }
    return -1;
}

static ULONG *RingTrb(XRING *r, ULONG Idx)
{
    return r->Seg[Idx / SEG_TRBS] + (Idx % SEG_TRBS) * 4;
}

/* ------------------------------------------------------------------ */
/* Commands                                                             */
/* ------------------------------------------------------------------ */

static int XhciCommand(XHC *x, ULONG d0, ULONG d1, ULONG d2, ULONG d3, ULONG *SlotOut)
{
    OS_IRQL irql;
    ULONG code;

    OsEventReset(&x->CmdEvent);
    irql = OsLock();
    x->CmdBusy = 1;
    x->CmdCode = 0;
    x->CmdTrbPhys = RingPut(&x->Cmd, d0, d1, d2, d3);
    MmioWr32(x->Db, 0);
    OsUnlock(irql);

    if (!OsEventWait(&x->CmdEvent, CMD_TIMEOUT_MS)) {
        UsbLog(LOG_ERR, "xhci: command %u timed out\n", TRB_GET_TYPE(d3));
        irql = OsLock();
        x->CmdBusy = 0;
        OsUnlock(irql);
        return -1;
    }
    code = x->CmdCode;
    if (SlotOut != NULL) {
        *SlotOut = x->CmdSlot;
    }
    if (code != CC_SUCCESS) {
        UsbLog(LOG_DBG, "xhci: command %u completion code %u\n", TRB_GET_TYPE(d3), code);
    }
    return (int)code;
}

/* ------------------------------------------------------------------ */
/* Contexts                                                             */
/* ------------------------------------------------------------------ */

static ULONG *InCtl(XHC *x, XSLOT *s) { (void)x; return (ULONG *)s->In; }
static ULONG *InSlotCtx(XHC *x, XSLOT *s) { return (ULONG *)(s->In + x->CtxSize); }
static ULONG *InEpCtx(XHC *x, XSLOT *s, ULONG Dci) { return (ULONG *)(s->In + x->CtxSize * (Dci + 1)); }
static ULONG *OutSlotCtx(XHC *x, XSLOT *s) { (void)x; return (ULONG *)s->Out; }
static ULONG *OutEpCtx(XHC *x, XSLOT *s, ULONG Dci) { return (ULONG *)(s->Out + x->CtxSize * Dci); }

static ULONG XhciSpeedId(UCHAR Speed)
{
    switch (Speed) {
    case USB_SPEED_LOW:   return 2;
    case USB_SPEED_HIGH:  return 3;
    case USB_SPEED_SUPER: return 4;
    default:              return 1;
    }
}

static void FillSlotCtx(XHC *x, XSLOT *s, USB_DEV *Dev)
{
    ULONG *sc = InSlotCtx(x, s);
    ULONG dw0;
    ULONG dw1;
    ULONG dw2 = 0;

    dw0 = (Dev->Route & 0xFFFFF) | (XhciSpeedId(Dev->Speed) << 20) | (s->MaxDci << 27);
    dw1 = ((ULONG)Dev->RootPort << 16);
    if (Dev->IsHub) {
        dw0 |= 1UL << 26;
        dw1 |= (ULONG)Dev->HubPorts << 24;
        if (Dev->Speed == USB_SPEED_HIGH) {
            dw2 |= (ULONG)(Dev->HubTtt & 3) << 16;
            if (Dev->HubMtt) {
                dw0 |= 1UL << 25;
            }
        }
    }
    if ((Dev->Speed == USB_SPEED_LOW || Dev->Speed == USB_SPEED_FULL) && Dev->TtHub != NULL) {
        XSLOT *hub = (XSLOT *)Dev->TtHub->HcPriv;
        if (hub != NULL) {
            dw2 |= hub->SlotId | ((ULONG)Dev->TtPort << 8);
            if (Dev->TtHub->HubMtt) {
                dw0 |= 1UL << 25;
            }
        }
    }
    sc[0] = dw0;
    sc[1] = dw1;
    sc[2] = dw2;
    sc[3] = 0;
}

static ULONG EpInterval(USB_PIPE *p)
{
    ULONG b = p->Interval;

    if (p->Type != USB_EP_INTERRUPT && p->Type != USB_EP_ISOCH) {
        return 0;
    }
    if (p->Dev->Speed == USB_SPEED_HIGH || p->Dev->Speed == USB_SPEED_SUPER) {
        if (b == 0) {
            b = 1;
        }
        if (b > 16) {
            b = 16;
        }
        return b - 1;
    } else {
        ULONG frames = b ? b : 1;
        ULONG units = frames * 8;
        ULONG n = 3;
        while (n < 10 && (1UL << (n + 1)) <= units) {
            n++;
        }
        return n;
    }
}

static void FillEpCtx(XHC *x, XSLOT *s, XEP *ep, USB_PIPE *p)
{
    ULONG *ec = InEpCtx(x, s, ep->Dci);
    ULONG type;
    ULONG mps = p->MaxPacket;
    ULONG burst = p->MaxBurst;
    ULONG avg;
    ULONG esit = 0;

    switch (p->Type) {
    case USB_EP_CONTROL:   type = 4; avg = 8; break;
    case USB_EP_BULK:      type = p->DirIn ? 6 : 2; avg = 3072; break;
    case USB_EP_INTERRUPT: type = p->DirIn ? 7 : 3; avg = mps; break;
    default:               type = p->DirIn ? 5 : 1; avg = mps; break;
    }
    if (p->Type == USB_EP_INTERRUPT) {
        esit = mps * (burst + 1);
    }
    UsbMemSet(ec, 0, x->CtxSize);
    ec[0] = (EpInterval(p) << 16) | ((esit >> 16) << 24);
    ec[1] = (3UL << 1) | (type << 3) | (burst << 8) | (mps << 16);
    if (ep->NumStreams != 0) {
        ULONG n = 0;
        while ((2UL << n) < ep->PsaSize) {
            n++;
        }
        ec[0] |= (n << 10) | (1UL << 15);
        ec[2] = ep->PsaPhys;
    } else {
        ec[2] = ep->Ring.SegPhys[0] | 1;
    }
    ec[3] = 0;
    ec[4] = (avg & 0xFFFF) | ((esit & 0xFFFF) << 16);
}

static ULONG EpState(XHC *x, XSLOT *s, ULONG Dci)
{
    return OutEpCtx(x, s, Dci)[0] & 7;
}

/* ------------------------------------------------------------------ */
/* Controller start / stop                                              */
/* ------------------------------------------------------------------ */

static void XhciHandoff(XHC *x)
{
    ULONG hcc = MmioRd32(x->Cap + XCAP_HCCPARAMS1);
    ULONG off = (hcc >> 16) << 2;
    int guard = 0;

    while (off != 0 && guard++ < 64) {
        UCHAR *cap = x->Cap + off;
        ULONG v = MmioRd32(cap);
        ULONG id = v & 0xFF;
        ULONG next = (v >> 8) & 0xFF;

        if (id == 1) {
            if (v & (1UL << 16)) {
                ULONG start = OsTimeMs();
                MmioWr8(cap + 3, 1);
                while ((MmioRd32(cap) & (1UL << 16)) && OsTimeMs() - start < 1000) {
                    OsSleepMs(10);
                }
                if (MmioRd32(cap) & (1UL << 16)) {
                    UsbLog(LOG_ERR, "xhci: BIOS did not release controller, forcing\n");
                    MmioWr8(cap + 2, 0);
                } else {
                    UsbLog(LOG_INFO, "xhci: BIOS handoff done\n");
                }
            } else {
                MmioWr8(cap + 3, 1);
            }
            {
                ULONG ctl = MmioRd32(cap + 4);
                MmioWr32(cap + 4, (ctl & ~0x0000E011UL) | 0xE0000000UL);
            }
        } else if (id == 2) {
            ULONG major = v >> 24;
            ULONG dw2 = MmioRd32(cap + 8);
            ULONG first = dw2 & 0xFF;
            ULONG count = (dw2 >> 8) & 0xFF;
            ULONG p;
            ULONG psic = dw2 >> 28;
            ULONG i;
            for (p = first; p < first + count && p <= USB_MAX_PORTS; p++) {
                if (p >= 1) {
                    x->PortProto[p] = (UCHAR)major;
                }
            }
            for (i = 0; major == 3 && i < psic; i++) {
                ULONG psi = MmioRd32(cap + 16 + i * 4);
                ULONG m = psi >> 16;
                ULONG g = 0;
                if (((psi >> 4) & 3) == 3) {
                    g = m;
                } else if (((psi >> 4) & 3) == 2) {
                    g = (m + 500) / 1000;
                }
                if (g > 0 && g < 256 && g > x->SsGbps[psi & 0xF]) {
                    x->SsGbps[psi & 0xF] = (UCHAR)g;
                }
            }
        }
        if (next == 0) {
            break;
        }
        off += next << 2;
    }
}

static int XhciStart(USB_HC *Hc)
{
    XHC *x;
    ULONG hcs1, hcs2, hcc;
    ULONG i;
    ULONG ports;

    x = (XHC *)OsAlloc(sizeof(XHC));
    if (x == NULL) {
        return USB_ERR_NOMEM;
    }
    Hc->Priv = x;
    x->Hc = Hc;
    x->Cap = Hc->Mmio;
    x->Op = x->Cap + MmioRd8(x->Cap + XCAP_CAPLENGTH);
    x->Rt = x->Cap + (MmioRd32(x->Cap + XCAP_RTSOFF) & ~0x1FUL);
    x->Db = x->Cap + (MmioRd32(x->Cap + XCAP_DBOFF) & ~0x3UL);
    OsEventInit(&x->CmdEvent);
    x->UseIrq = (BOOLEAN)OsGetConfig("UseInterrupts", 0);

    hcs1 = MmioRd32(x->Cap + XCAP_HCSPARAMS1);
    hcs2 = MmioRd32(x->Cap + XCAP_HCSPARAMS2);
    hcc = MmioRd32(x->Cap + XCAP_HCCPARAMS1);
    if (hcs1 == 0xFFFFFFFF) {
        return USB_ERR_NODEV;
    }

    XhciHandoff(x);

    if (!WaitOp(x, XOP_USBSTS, STS_CNR, 0, 1000)) {
        UsbLog(LOG_ERR, "xhci: controller not ready\n");
        return USB_ERR_IO;
    }
    if (!(OpRd(x, XOP_USBSTS) & STS_HCH)) {
        OpWr(x, XOP_USBCMD, OpRd(x, XOP_USBCMD) & ~CMD_RS);
        WaitOp(x, XOP_USBSTS, STS_HCH, STS_HCH, 100);
    }
    OpWr(x, XOP_USBCMD, CMD_HCRST);
    OsSleepMs(2);
    if (!WaitOp(x, XOP_USBCMD, CMD_HCRST, 0, 1000) || !WaitOp(x, XOP_USBSTS, STS_CNR, 0, 1000)) {
        UsbLog(LOG_ERR, "xhci: reset failed\n");
        return USB_ERR_IO;
    }

    x->MaxSlots = hcs1 & 0xFF;
    if (x->MaxSlots > XHCI_MAX_SLOTS) {
        x->MaxSlots = XHCI_MAX_SLOTS;
    }
    x->CtxSize = (hcc & 4) ? 64 : 32;
    ports = hcs1 >> 24;
    if (ports > USB_MAX_PORTS) {
        ports = USB_MAX_PORTS;
    }
    Hc->NumPorts = (UCHAR)ports;
    Hc->MaxXfer = 0x10000;
    Hc->SgMax = 0x200000;
    Hc->QueueOk = 1;
    if (((hcc >> 12) & 0xF) != 0) {
        ULONG psa = 2UL << ((hcc >> 12) & 0xF);
        Hc->MaxStreams = (UCHAR)((psa - 1 > XHCI_MAX_STREAMS) ? XHCI_MAX_STREAMS : psa - 1);
    }

    x->Dcbaa = (ULONG *)UsbDmaAlloc(2048, &x->DcbaaPhys);
    if (x->Dcbaa == NULL) {
        return USB_ERR_NOMEM;
    }

    x->ScratchCount = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (x->ScratchCount != 0) {
        ULONG size = x->ScratchCount * 8;
        if (size <= USB_PAGE_SIZE) {
            x->ScratchArr = (ULONG *)UsbDmaAlloc(size < 64 ? 64 : size, &x->ScratchPhys);
        } else {
            x->ScratchArr = (ULONG *)OsDmaAlloc(size, &x->ScratchPhys);
        }
        if (x->ScratchArr == NULL) {
            return USB_ERR_NOMEM;
        }
        for (i = 0; i < x->ScratchCount; i++) {
            ULONG pp;
            if (OsDmaAlloc(USB_PAGE_SIZE, &pp) == NULL) {
                return USB_ERR_NOMEM;
            }
            x->ScratchArr[i * 2] = pp;
            x->ScratchArr[i * 2 + 1] = 0;
        }
        x->Dcbaa[0] = x->ScratchPhys;
        x->Dcbaa[1] = 0;
    }

    if (RingInit(&x->Cmd, 1) != USB_OK) {
        return USB_ERR_NOMEM;
    }
    x->Evt = (ULONG *)UsbDmaAlloc(EVENT_TRBS * 16, &x->EvtPhys);
    x->Erst = (ULONG *)UsbDmaAlloc(64, &x->ErstPhys);
    if (x->Evt == NULL || x->Erst == NULL) {
        return USB_ERR_NOMEM;
    }
    x->EvtDeq = 0;
    x->EvtCycle = 1;
    x->Erst[0] = x->EvtPhys;
    x->Erst[1] = 0;
    x->Erst[2] = EVENT_TRBS;
    x->Erst[3] = 0;

    OpWr(x, XOP_CONFIG, (OpRd(x, XOP_CONFIG) & ~0xFFUL) | x->MaxSlots);
    OpWr(x, XOP_DCBAAP, x->DcbaaPhys);
    OpWr(x, XOP_DCBAAP + 4, 0);
    OpWr(x, XOP_CRCR, x->Cmd.SegPhys[0] | 1);
    OpWr(x, XOP_CRCR + 4, 0);
    OpWr(x, XOP_DNCTRL, 0);

    RtWr(x, XRT_ERSTSZ, 1);
    RtWr(x, XRT_ERDP, x->EvtPhys);
    RtWr(x, XRT_ERDP + 4, 0);
    RtWr(x, XRT_ERSTBA, x->ErstPhys);
    RtWr(x, XRT_ERSTBA + 4, 0);
    RtWr(x, XRT_IMOD, 160);
    RtWr(x, XRT_IMAN, x->UseIrq ? 3 : 1);

    OpWr(x, XOP_USBSTS, STS_EINT | STS_PCD | STS_HSE);
    OpWr(x, XOP_USBCMD, CMD_RS | (x->UseIrq ? CMD_INTE : 0));
    if (!WaitOp(x, XOP_USBSTS, STS_HCH, 0, 100)) {
        UsbLog(LOG_ERR, "xhci: controller did not start\n");
        return USB_ERR_IO;
    }

    if (hcc & 8) {
        for (i = 1; i <= ports; i++) {
            ULONG sc = OpRd(x, XOP_PORTSC(i));
            if (!(sc & PORT_PP)) {
                OpWr(x, XOP_PORTSC(i), (sc & PORT_PRESERVE) | PORT_PP);
            }
        }
        OsSleepMs(20);
    }

    Hc->Running = 1;
    UsbLog(LOG_INFO, "xhci: %u ports, %u slots, ctx %u, scratch %u, version %x\n",
           ports, x->MaxSlots, x->CtxSize, x->ScratchCount, MmioRd32(x->Cap) >> 16);
    return USB_OK;
}

static void XhciStop(USB_HC *Hc)
{
    XHC *x = XHCP(Hc);

    if (x == NULL) {
        return;
    }
    OpWr(x, XOP_USBCMD, OpRd(x, XOP_USBCMD) & ~(CMD_RS | CMD_INTE));
    WaitOp(x, XOP_USBSTS, STS_HCH, STS_HCH, 50);
    Hc->Running = 0;
}

/* ------------------------------------------------------------------ */
/* Root ports                                                           */
/* ------------------------------------------------------------------ */

static UCHAR XhciPortSpeed(ULONG sc)
{
    switch ((sc >> PORT_SPEED_SHIFT) & 0xF) {
    case 2:  return USB_SPEED_LOW;
    case 3:  return USB_SPEED_HIGH;
    case 4:
    case 5:
    case 6:
    case 7:  return USB_SPEED_SUPER;
    default: return USB_SPEED_FULL;
    }
}

static void XhciLinkRate(USB_HC *Hc, int Port, ULONG sc)
{
    XHC *x = XHCP(Hc);
    ULONG id = (sc >> PORT_SPEED_SHIFT) & 0xF;
    UCHAR g = 0;

    if (XhciPortSpeed(sc) == USB_SPEED_SUPER) {
        g = x->SsGbps[id];
        if (g == 0) {
            g = (UCHAR)((id == 5) ? 10 : 5);
        }
    }
    Hc->PortGbps[Port] = g;
}

static ULONG XhciPortStatus(USB_HC *Hc, int Port)
{
    XHC *x = XHCP(Hc);
    ULONG sc = OpRd(x, XOP_PORTSC(Port));
    ULONG ps = 0;

    if (sc == 0xFFFFFFFF) {
        return 0;
    }
    if (sc & PORT_CCS) ps |= PS_CONNECT;
    if (sc & PORT_PED) ps |= PS_ENABLE;
    if (sc & PORT_CSC) ps |= PS_CHANGE;
    if (sc & PORT_OCA) ps |= PS_OVERCURRENT;
    if (sc & PORT_PP)  ps |= PS_POWER;
    if (x->PortProto[Port] == 3) ps |= PS_SS_PORT;
    ps |= (ULONG)XhciPortSpeed(sc) << PS_SPEED_SHIFT;
    return ps;
}

static void XhciPortClearChange(USB_HC *Hc, int Port)
{
    XHC *x = XHCP(Hc);
    ULONG sc = OpRd(x, XOP_PORTSC(Port));

    OpWr(x, XOP_PORTSC(Port), (sc & PORT_PRESERVE) | (sc & PORT_CHANGE_BITS));
}

static int XhciPortReset(USB_HC *Hc, int Port, UCHAR *Speed)
{
    XHC *x = XHCP(Hc);
    ULONG sc = OpRd(x, XOP_PORTSC(Port));
    ULONG start;
    BOOLEAN ss = (BOOLEAN)(x->PortProto[Port] == 3);

    if (!(sc & PORT_CCS)) {
        return USB_ERR_NODEV;
    }
    if (ss && (sc & PORT_PED)) {
        *Speed = XhciPortSpeed(sc);
        XhciLinkRate(Hc, Port, sc);
        return USB_OK;
    }
    OpWr(x, XOP_PORTSC(Port), (sc & PORT_PRESERVE) | (ss ? PORT_WPR : PORT_PR));
    start = OsTimeMs();
    for (;;) {
        OsSleepMs(10);
        sc = OpRd(x, XOP_PORTSC(Port));
        if (!(sc & PORT_PR) && (sc & (PORT_PRC | PORT_WRC))) {
            break;
        }
        if (OsTimeMs() - start > 500) {
            UsbLog(LOG_ERR, "xhci: port %d reset timeout (%08x)\n", Port, sc);
            return USB_ERR_TIMEOUT;
        }
    }
    OpWr(x, XOP_PORTSC(Port), (sc & PORT_PRESERVE) | PORT_PRC | PORT_WRC | PORT_CSC | PORT_PEC);
    OsSleepMs(10);
    sc = OpRd(x, XOP_PORTSC(Port));
    if (ss && !(sc & PORT_PED)) {
        start = OsTimeMs();
        while (!(sc & PORT_PED) && OsTimeMs() - start < 200) {
            OsSleepMs(10);
            sc = OpRd(x, XOP_PORTSC(Port));
        }
    }
    if (!(sc & PORT_CCS)) {
        return USB_ERR_NODEV;
    }
    if (!(sc & PORT_PED)) {
        UsbLog(LOG_ERR, "xhci: port %d not enabled after reset (%08x)\n", Port, sc);
        return USB_ERR_IO;
    }
    *Speed = XhciPortSpeed(sc);
    XhciLinkRate(Hc, Port, sc);
    return USB_OK;
}

static void XhciPortDisable(USB_HC *Hc, int Port)
{
    XHC *x = XHCP(Hc);
    ULONG sc = OpRd(x, XOP_PORTSC(Port));

    if (x->PortProto[Port] != 3) {
        OpWr(x, XOP_PORTSC(Port), (sc & PORT_PRESERVE) | PORT_PED);
    }
}

/* ------------------------------------------------------------------ */
/* Devices                                                              */
/* ------------------------------------------------------------------ */

static void StreamsFree(XEP *ep)
{
    ULONG i;

    for (i = 1; i <= XHCI_MAX_STREAMS; i++) {
        if (ep->Str[i] != NULL) {
            if (ep->Str[i]->Pipe != NULL) {
                ep->Str[i]->Pipe->HcPriv = NULL;
            }
            RingFree(&ep->Str[i]->Ring);
            OsFree(ep->Str[i]);
            ep->Str[i] = NULL;
        }
    }
    if (ep->Psa != NULL) {
        UsbDmaFree(ep->Psa, ep->PsaSize * 16);
        ep->Psa = NULL;
    }
    ep->NumStreams = 0;
    ep->PsaSize = 0;
}

static void EpFree(XEP *ep)
{
    StreamsFree(ep);
    RingFree(&ep->Ring);
    OsFree(ep);
}

static XEP *EpAlloc(XSLOT *s, ULONG Dci, USB_PIPE *Pipe)
{
    XEP *ep = s->Ep[Dci];

    if (ep == NULL) {
        ep = (XEP *)OsAlloc(sizeof(XEP));
        if (ep == NULL) {
            return NULL;
        }
        s->Ep[Dci] = ep;
    }
    if (RingInit(&ep->Ring, (Pipe->Type == USB_EP_BULK) ? BULK_SEGS : 1) != USB_OK) {
        return NULL;
    }
    ep->Dci = (UCHAR)Dci;
    ep->Pipe = Pipe;
    return ep;
}

static void SlotFree(XHC *x, XSLOT *s)
{
    ULONG i;

    for (i = 0; i < 32; i++) {
        if (s->Ep[i] != NULL) {
            EpFree(s->Ep[i]);
            s->Ep[i] = NULL;
        }
    }
    if (s->SlotId != 0 && s->SlotId <= x->MaxSlots) {
        x->Dcbaa[s->SlotId * 2] = 0;
        x->Dcbaa[s->SlotId * 2 + 1] = 0;
        x->Slots[s->SlotId] = NULL;
    }
    UsbDmaFree(s->Out, s->OutSize);
    UsbDmaFree(s->In, s->InSize);
    OsFree(s);
}

static int XhciDevInit(USB_DEV *Dev)
{
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s;
    ULONG slot = 0;
    int cc;
    XEP *ep;

    cc = XhciCommand(x, 0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &slot);
    if (cc != CC_SUCCESS || slot == 0 || slot > x->MaxSlots) {
        UsbLog(LOG_ERR, "xhci: enable slot failed (%d)\n", cc);
        return USB_ERR_IO;
    }
    s = (XSLOT *)OsAlloc(sizeof(XSLOT));
    if (s == NULL) {
        XhciCommand(x, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (slot << 24), NULL);
        return USB_ERR_NOMEM;
    }
    s->SlotId = slot;
    s->OutSize = 32 * x->CtxSize;
    s->InSize = 33 * x->CtxSize;
    s->Out = (UCHAR *)UsbDmaAlloc(s->OutSize, &s->OutPhys);
    s->In = (UCHAR *)UsbDmaAlloc(s->InSize, &s->InPhys);
    if (s->Out == NULL || s->In == NULL) {
        XhciCommand(x, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (slot << 24), NULL);
        s->SlotId = 0;
        SlotFree(x, s);
        return USB_ERR_NOMEM;
    }
    x->Slots[slot] = s;
    x->Dcbaa[slot * 2] = s->OutPhys;
    x->Dcbaa[slot * 2 + 1] = 0;
    Dev->HcPriv = s;

    ep = EpAlloc(s, 1, &Dev->Ep0);
    if (ep == NULL) {
        return USB_ERR_NOMEM;
    }
    Dev->Ep0.HcPriv = ep;
    s->MaxDci = 1;
    return USB_OK;
}

static int XhciDevSetAddress(USB_DEV *Dev)
{
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    XEP *ep = s->Ep[1];
    int cc;

    UsbMemSet(s->In, 0, s->InSize);
    InCtl(x, s)[1] = 3;
    FillSlotCtx(x, s, Dev);
    FillEpCtx(x, s, ep, &Dev->Ep0);
    cc = XhciCommand(x, s->InPhys, 0, 0, TRB_TYPE(TRB_ADDRESS_DEVICE) | (s->SlotId << 24), NULL);
    if (cc != CC_SUCCESS) {
        UsbLog(LOG_ERR, "xhci: address device failed (%d)\n", cc);
        return USB_ERR_IO;
    }
    Dev->Address = (UCHAR)(OutSlotCtx(x, s)[3] & 0xFF);
    return USB_OK;
}

static int XhciDevUpdate(USB_DEV *Dev)
{
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    int cc;

    if (s == NULL) {
        return USB_ERR_NODEV;
    }
    UsbMemSet(s->In, 0, s->InSize);
    InCtl(x, s)[1] = 2;
    FillSlotCtx(x, s, Dev);
    FillEpCtx(x, s, s->Ep[1], &Dev->Ep0);
    /* keep the current dequeue pointer of EP0 */
    InEpCtx(x, s, 1)[2] = OutEpCtx(x, s, 1)[2];
    InEpCtx(x, s, 1)[3] = OutEpCtx(x, s, 1)[3];
    cc = XhciCommand(x, s->InPhys, 0, 0, TRB_TYPE(TRB_EVALUATE_CTX) | (s->SlotId << 24), NULL);
    return (cc == CC_SUCCESS) ? USB_OK : USB_ERR_IO;
}

static void XhciDevFree(USB_DEV *Dev)
{
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;

    if (s == NULL) {
        return;
    }
    if (Dev->Hc->Running) {
        XhciCommand(x, 0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (s->SlotId << 24), NULL);
    }
    SlotFree(x, s);
    Dev->HcPriv = NULL;
    Dev->Ep0.HcPriv = NULL;
}

/* ------------------------------------------------------------------ */
/* Pipes                                                                */
/* ------------------------------------------------------------------ */

static ULONG PipeDci(USB_PIPE *p)
{
    if (p->Endpoint == 0) {
        return 1;
    }
    return (ULONG)p->Endpoint * 2 + (p->DirIn ? 1 : 0);
}

static int ConfigureEp(XHC *x, XSLOT *s, USB_DEV *Dev, ULONG Dci, BOOLEAN Drop, BOOLEAN Add)
{
    int cc;

    UsbMemSet(s->In, 0, s->InSize);
    InCtl(x, s)[0] = Drop ? (1UL << Dci) : 0;
    InCtl(x, s)[1] = 1 | (Add ? (1UL << Dci) : 0);
    FillSlotCtx(x, s, Dev);
    if (Add) {
        FillEpCtx(x, s, s->Ep[Dci], s->Ep[Dci]->Pipe);
    }
    cc = XhciCommand(x, s->InPhys, 0, 0, TRB_TYPE(TRB_CONFIGURE_EP) | (s->SlotId << 24), NULL);
    return cc;
}

static int XhciPipeOpen(USB_PIPE *Pipe)
{
    USB_DEV *Dev = Pipe->Dev;
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    ULONG dci = PipeDci(Pipe);
    XEP *ep;
    int cc;

    if (s == NULL) {
        return USB_ERR_NODEV;
    }
    if (dci == 1) {
        Pipe->HcPriv = s->Ep[1];
        return USB_OK;
    }
    ep = EpAlloc(s, dci, Pipe);
    if (ep == NULL) {
        return USB_ERR_NOMEM;
    }
    Pipe->HcPriv = ep;
    if (dci > s->MaxDci) {
        s->MaxDci = dci;
    }
    cc = ConfigureEp(x, s, Dev, dci, FALSE, TRUE);
    if (cc != CC_SUCCESS) {
        UsbLog(LOG_ERR, "xhci: configure endpoint %u failed (%d)\n", dci, cc);
        return USB_ERR_IO;
    }
    return USB_OK;
}

static void XhciPipeClose(USB_PIPE *Pipe)
{
    USB_DEV *Dev = Pipe->Dev;
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    ULONG dci = PipeDci(Pipe);

    Pipe->HcPriv = NULL;
    if (s == NULL || dci == 1 || s->Ep[dci] == NULL) {
        return;
    }
    if (Dev->Hc->Running && !Dev->Gone) {
        ConfigureEp(x, s, Dev, dci, TRUE, FALSE);
    }
    EpFree(s->Ep[dci]);
    s->Ep[dci] = NULL;
}

/* Ring and doorbell target of a pipe (plain endpoint or one stream of it) */
static XRING *PipeRing(USB_PIPE *p, XEP **EpOut)
{
    if (p->Stream != 0) {
        XSTREAM *st = (XSTREAM *)p->HcPriv;
        if (st == NULL || p->Parent == NULL || p->Parent->HcPriv == NULL) {
            return NULL;
        }
        *EpOut = (XEP *)p->Parent->HcPriv;
        return &st->Ring;
    }
    *EpOut = (XEP *)p->HcPriv;
    return (*EpOut != NULL) ? &(*EpOut)->Ring : NULL;
}

static ULONG TdTrbCount(USB_XFER *X)
{
    ULONG off = 0;
    ULONG n = 0;

    if (X->Length == 0) {
        return 1;
    }
    while (off < X->Length) {
        ULONG run;
        ULONG phys = UsbXferPhys(X, off, &run);
        ULONG chunk = 0x10000 - (phys & 0xFFFF);
        if (chunk > run) {
            chunk = run;
        }
        off += chunk;
        n++;
    }
    return n;
}

static int XhciSubmit(USB_XFER *X)
{
    USB_PIPE *p = X->Pipe;
    USB_DEV *Dev = p->Dev;
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    XEP *ep = NULL;
    XRING *r;

    r = PipeRing(p, &ep);
    if (s == NULL || ep == NULL || r == NULL) {
        return USB_ERR_NODEV;
    }
    UsbMemSet(X->Hc, 0, sizeof(X->Hc));

    if (p->Type == USB_EP_CONTROL) {
        const UCHAR *su = Dev->Page;
        ULONG trt = 0;
        if (X->Length != 0) {
            trt = X->DirIn ? 3 : 2;
        }
        X->Hc[0] = RingPut(r, GET32(su), GET32(su + 4), 8,
                           TRB_TYPE(TRB_SETUP) | TRB_IDT | (trt << 16));
        if (X->Length != 0) {
            X->Hc[1] = RingPut(r, X->Phys, 0, X->Length,
                               TRB_TYPE(TRB_DATA) | TRB_ISP | (X->DirIn ? TRB_DIR_IN : 0));
        }
        X->Hc[2] = RingPut(r, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                           ((X->Length == 0 || !X->DirIn) ? TRB_DIR_IN : 0));
    } else {
        ULONG maxp = p->MaxPacket ? p->MaxPacket : 8;
        ULONG n = TdTrbCount(X);
        ULONG off = 0;
        ULONG i;
        ULONG *first = NULL;

        if (r->Used + n > RingCapacity(r)) {
            return USB_ERR_BUSY;
        }
        for (i = 0; i < n; i++) {
            ULONG run = 0;
            ULONG phys = (X->Length != 0) ? UsbXferPhys(X, off, &run) : X->Phys;
            ULONG chunk = 0x10000 - (phys & 0xFFFF);
            ULONG after;
            ULONG tds;
            ULONG trb;
            if (chunk > run) {
                chunk = run;
            }
            after = X->Length - off - chunk;
            tds = (after + maxp - 1) / maxp;
            if (tds > 31) {
                tds = 31;
            }
            trb = RingPutEx(r, phys, 0, chunk | (tds << 17),
                            TRB_TYPE(TRB_NORMAL) | TRB_ISP | ((i + 1 == n) ? TRB_IOC : TRB_CH), (BOOLEAN)(i == 0));
            r->Owner[r->LastIdx] = X;
            r->Off[r->LastIdx] = off;
            if (i == 0) {
                X->Hc[0] = trb;
                first = r->Last;
            }
            X->Hc[2] = trb;
            off += chunk;
        }
        X->Hc[6] = n;
        r->Used += n;
        RingRelease(first);
    }
    MmioWr32(x->Db + 4 * s->SlotId, ep->Dci | ((ULONG)p->Stream << 16));
    return USB_OK;
}

static int CodeToStatus(ULONG cc)
{
    switch (cc) {
    case CC_SUCCESS:
    case CC_SHORT_PACKET: return USB_OK;
    case CC_STALL:        return USB_ERR_STALL;
    case CC_BABBLE:       return USB_ERR_BABBLE;
    default:              return USB_ERR_IO;
    }
}

static void HandleTransferEvent(XHC *x, ULONG *ev)
{
    ULONG ptr = ev[0];
    ULONG cc = ev[2] >> 24;
    ULONG resid = ev[2] & 0xFFFFFF;
    ULONG slot = ev[3] >> 24;
    ULONG dci = (ev[3] >> 16) & 0x1F;
    XSLOT *s;
    XEP *ep;
    XRING *r = NULL;
    USB_PIPE *pipe = NULL;
    USB_XFER *X;
    LONG idx = -1;

    if (slot == 0 || slot > x->MaxSlots || (ev[3] & 4)) {
        return;
    }
    s = x->Slots[slot];
    if (s == NULL) {
        return;
    }
    ep = s->Ep[dci];
    if (ep == NULL || ep->Pipe == NULL) {
        return;
    }
    if (ep->NumStreams != 0) {
        ULONG i;
        for (i = 1; i <= ep->NumStreams && idx < 0; i++) {
            if (ep->Str[i] != NULL) {
                idx = RingIndex(&ep->Str[i]->Ring, ptr);
                if (idx >= 0) {
                    r = &ep->Str[i]->Ring;
                    pipe = ep->Str[i]->Pipe;
                }
            }
        }
    } else {
        r = &ep->Ring;
        pipe = ep->Pipe;
        idx = RingIndex(r, ptr);
    }
    if (idx < 0 || pipe == NULL) {
        return;
    }
    X = pipe->Cur;
    UsbLog(LOG_TRACE, "xhci: event slot %u dci %u ptr %08x cc %u resid %u idx %d cur %p owner %p\n",
           slot, dci, ptr, cc, resid, idx, X, r->Owner[idx]);
    if (X == NULL || cc == CC_STOPPED || cc == CC_STOPPED_LEN) {
        return;
    }

    if (pipe->Type == USB_EP_CONTROL) {
        if (ptr == X->Hc[1] && X->Hc[1] != 0) {
            if (cc == CC_SHORT_PACKET) {
                X->Hc[3] = resid;
                return;
            }
            if (cc == CC_SUCCESS) {
                return;
            }
            UsbXferDone(X, CodeToStatus(cc), 0);
            return;
        }
        if (ptr == X->Hc[2]) {
            if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
                ULONG rr = X->Hc[3];
                UsbXferDone(X, USB_OK, (rr > X->Length) ? 0 : X->Length - rr);
            } else {
                UsbXferDone(X, CodeToStatus(cc), 0);
            }
            return;
        }
        if (ptr == X->Hc[0] && cc != CC_SUCCESS) {
            UsbXferDone(X, CodeToStatus(cc), 0);
        }
        return;
    }

    /* events for TRBs of transfers already completed are left over after a short packet */
    if (r->Owner[idx] != X || (cc == CC_SUCCESS && ptr != X->Hc[2])) {
        return;
    }
    if (r->Used >= X->Hc[6]) {
        r->Used -= X->Hc[6];
    } else {
        r->Used = 0;
    }
    if (cc == CC_SUCCESS || cc == CC_SHORT_PACKET) {
        ULONG len = RingTrb(r, (ULONG)idx)[2] & 0x1FFFF;
        ULONG done = r->Off[idx] + len - (resid > len ? len : resid);
        UsbXferDone(X, USB_OK, done > X->Length ? X->Length : done);
        /* some controllers only look at the next queued transfer after a doorbell */
        if (pipe->Cur != NULL) {
            MmioWr32(x->Db + 4 * s->SlotId, dci | ((ULONG)pipe->Stream << 16));
        }
    } else {
        UsbXferDone(X, CodeToStatus(cc), r->Off[idx]);
    }
}

static void XhciPoll(USB_HC *Hc)
{
    XHC *x = XHCP(Hc);
    ULONG handled = 0;

    if (x == NULL || x->Evt == NULL) {
        return;
    }
    for (;;) {
        ULONG *ev = x->Evt + x->EvtDeq * 4;
        ULONG d3 = ev[3];
        ULONG type;

        if ((d3 & TRB_C) != x->EvtCycle) {
            break;
        }
        type = TRB_GET_TYPE(d3);
        switch (type) {
        case TRB_TRANSFER_EVENT:
            HandleTransferEvent(x, ev);
            break;
        case TRB_CMD_COMPLETION:
            if (x->CmdBusy && ev[0] == x->CmdTrbPhys) {
                x->CmdCode = ev[2] >> 24;
                x->CmdSlot = d3 >> 24;
                x->CmdBusy = 0;
                OsEventSet(&x->CmdEvent);
            }
            break;
        case TRB_HC_EVENT:
            UsbLog(LOG_ERR, "xhci: host controller event %u\n", ev[2] >> 24);
            break;
        default:
            break;
        }
        handled++;
        x->EvtDeq++;
        if (x->EvtDeq == EVENT_TRBS) {
            x->EvtDeq = 0;
            x->EvtCycle ^= 1;
        }
    }
    if (handled) {
        RtWr(x, XRT_ERDP, (x->EvtPhys + x->EvtDeq * 16) | 8);
        RtWr(x, XRT_ERDP + 4, 0);
    }
}

static BOOLEAN XhciInterrupt(USB_HC *Hc)
{
    XHC *x = XHCP(Hc);
    ULONG sts;

    if (x == NULL || !Hc->Running) {
        return FALSE;
    }
    sts = OpRd(x, XOP_USBSTS);
    if (sts == 0xFFFFFFFF || !(sts & (STS_EINT | STS_HSE))) {
        return FALSE;
    }
    OpWr(x, XOP_USBSTS, sts & (STS_EINT | STS_HSE | STS_PCD));
    RtWr(x, XRT_IMAN, RtRd(x, XRT_IMAN) | 1);
    return TRUE;
}

static void StopEp(XHC *x, XSLOT *s, XEP *ep)
{
    ULONG st = EpState(x, s, ep->Dci);

    if (st == EP_STATE_RUNNING) {
        XhciCommand(x, 0, 0, 0, TRB_TYPE(TRB_STOP_EP) | (s->SlotId << 24) | ((ULONG)ep->Dci << 16), NULL);
    } else if (st == EP_STATE_HALTED) {
        XhciCommand(x, 0, 0, 0, TRB_TYPE(TRB_RESET_EP) | (s->SlotId << 24) | ((ULONG)ep->Dci << 16), NULL);
    }
    UsbPollAll();
}

/* Moves a stopped ring's dequeue pointer to its enqueue pointer */
static void SkipRing(XHC *x, XSLOT *s, XEP *ep, XRING *r, ULONG Stream)
{
    XhciCommand(x, RingEnqPhys(r) | r->Cycle | (Stream ? 2 : 0), 0, Stream << 16,
                TRB_TYPE(TRB_SET_TR_DEQ) | (s->SlotId << 24) | ((ULONG)ep->Dci << 16), NULL);
    UsbMemSet(r->Owner, 0, r->NSeg * SEG_TRBS * sizeof(USB_XFER *));
    r->Used = 0;
}

/* Restarts the streams that still have transfers queued */
static void RingStreams(XHC *x, XSLOT *s, XEP *ep)
{
    ULONG i;

    for (i = 1; i <= ep->NumStreams; i++) {
        if (ep->Str[i] != NULL && ep->Str[i]->Pipe != NULL && ep->Str[i]->Pipe->Cur != NULL) {
            MmioWr32(x->Db + 4 * s->SlotId, ep->Dci | (i << 16));
        }
    }
}

/* Stops the endpoint and moves the dequeue pointer past everything queued */
static void XhciCancel(USB_PIPE *Pipe)
{
    USB_DEV *Dev = Pipe->Dev;
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    XEP *ep = NULL;
    XRING *r = PipeRing(Pipe, &ep);

    if (s == NULL || ep == NULL || r == NULL || !Dev->Hc->Running) {
        return;
    }
    StopEp(x, s, ep);
    if (EpState(x, s, ep->Dci) == EP_STATE_STOPPED) {
        SkipRing(x, s, ep, r, Pipe->Stream);
    }
    if (Pipe->Stream != 0) {
        RingStreams(x, s, ep);
    }
}

/* Clears a halt condition and resets the data toggle / sequence number */
static int XhciPipeReset(USB_PIPE *Pipe)
{
    USB_DEV *Dev = Pipe->Dev;
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    XEP *ep = NULL;
    int cc;
    ULONG i;

    if (Pipe->Stream != 0) {
        Pipe = Pipe->Parent;
    }
    ep = (XEP *)Pipe->HcPriv;
    if (s == NULL || ep == NULL) {
        return USB_ERR_NODEV;
    }
    StopEp(x, s, ep);
    if (ep->Dci == 1) {
        if (EpState(x, s, 1) == EP_STATE_STOPPED) {
            SkipRing(x, s, ep, &ep->Ring, 0);
        }
        return USB_OK;
    }
    /* drop and re-add the endpoint: fresh rings, sequence number 0 */
    if (ep->NumStreams != 0) {
        for (i = 1; i <= ep->NumStreams; i++) {
            if (ep->Str[i] != NULL) {
                RingInit(&ep->Str[i]->Ring, BULK_SEGS);
                ep->Psa[i * 4] = ep->Str[i]->Ring.SegPhys[0] | 1 | (1UL << 1);
                ep->Psa[i * 4 + 1] = 0;
            }
        }
    } else {
        RingInit(&ep->Ring, ep->Ring.NSeg);
    }
    cc = ConfigureEp(x, s, Dev, ep->Dci, TRUE, TRUE);
    return (cc == CC_SUCCESS) ? USB_OK : USB_ERR_IO;
}

/*
 * Turns a bulk endpoint into Count streams. Child[i] becomes the pipe of
 * stream i + 1; it shares the endpoint of Pipe.
 */
static int XhciStreamsOpen(USB_PIPE *Pipe, USB_PIPE *Child, int Count)
{
    USB_DEV *Dev = Pipe->Dev;
    XHC *x = XHCP(Dev->Hc);
    XSLOT *s = (XSLOT *)Dev->HcPriv;
    XEP *ep = (XEP *)Pipe->HcPriv;
    ULONG size = 4;
    int i;
    int cc;

    if (s == NULL || ep == NULL || Count < 1 || Count > XHCI_MAX_STREAMS || Count > Dev->Hc->MaxStreams) {
        return USB_ERR_PARAM;
    }
    while (size < (ULONG)Count + 1) {
        size <<= 1;
    }
    StreamsFree(ep);
    ep->Psa = (ULONG *)UsbDmaAlloc(size * 16, &ep->PsaPhys);
    if (ep->Psa == NULL) {
        return USB_ERR_NOMEM;
    }
    UsbMemSet(ep->Psa, 0, size * 16);
    ep->PsaSize = size;
    for (i = 1; i <= Count; i++) {
        XSTREAM *st = (XSTREAM *)OsAlloc(sizeof(XSTREAM));
        USB_PIPE *c = &Child[i - 1];
        if (st == NULL || RingInit(&st->Ring, BULK_SEGS) != USB_OK) {
            if (st != NULL) {
                OsFree(st);
            }
            StreamsFree(ep);
            return USB_ERR_NOMEM;
        }
        ep->Str[i] = st;
        ep->Psa[i * 4] = st->Ring.SegPhys[0] | 1 | (1UL << 1);
        ep->Psa[i * 4 + 1] = 0;
        UsbMemCpy(c, Pipe, sizeof(*c));
        c->Cur = NULL;
        c->Tail = NULL;
        c->Stream = (USHORT)i;
        c->Parent = Pipe;
        c->HcPriv = st;
        c->Opened = 1;
        st->Pipe = c;
    }
    ep->NumStreams = (ULONG)Count;
    cc = ConfigureEp(x, s, Dev, ep->Dci, TRUE, TRUE);
    if (cc != CC_SUCCESS) {
        UsbLog(LOG_ERR, "xhci: configuring %d streams failed (%d)\n", Count, cc);
        StreamsFree(ep);
        ConfigureEp(x, s, Dev, ep->Dci, TRUE, TRUE);
        return USB_ERR_IO;
    }
    Pipe->NumStreams = (USHORT)Count;
    return USB_OK;
}

const HCD_OPS XhciOps = {
    "xHCI",
    XhciStart,
    XhciStop,
    XhciPortStatus,
    XhciPortClearChange,
    XhciPortReset,
    XhciPortDisable,
    XhciDevInit,
    XhciDevSetAddress,
    XhciDevUpdate,
    XhciDevFree,
    XhciPipeOpen,
    XhciPipeClose,
    XhciSubmit,
    XhciCancel,
    XhciPipeReset,
    XhciPoll,
    XhciInterrupt,
    XhciStreamsOpen
};
