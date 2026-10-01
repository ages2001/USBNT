/*
 * hcehci.c - EHCI host controller driver.
 *
 * High speed control, bulk and interrupt transfers. Low and full speed
 * devices behind a high speed hub (including the rate matching hub of
 * Intel 5..8 series chipsets) use split transactions. Low and full speed
 * devices on root ports are handed over to the companion controller.
 */

#include "usbnt.h"

#define EOP_USBCMD          0x00
#define EOP_USBSTS          0x04
#define EOP_USBINTR         0x08
#define EOP_FRINDEX         0x0C
#define EOP_CTRLDSSEGMENT   0x10
#define EOP_PERIODICBASE    0x14
#define EOP_ASYNCLISTADDR   0x18
#define EOP_CONFIGFLAG      0x40
#define EOP_PORTSC(n)       (0x44 + 4 * ((n) - 1))

#define CMD_RS              0x00000001
#define CMD_HCRESET         0x00000002
#define CMD_PSE             0x00000010
#define CMD_ASE             0x00000020
#define CMD_IAAD            0x00000040
#define CMD_ITC_1UF         0x00010000

#define STS_USBINT          0x00000001
#define STS_ERR             0x00000002
#define STS_PCD             0x00000004
#define STS_FLR             0x00000008
#define STS_HSE             0x00000010
#define STS_IAA             0x00000020
#define STS_HALTED          0x00001000
#define STS_PSS             0x00004000
#define STS_ASS             0x00008000
#define STS_ACK_MASK        0x0000003F

#define PORT_CCS            0x00000001
#define PORT_CSC            0x00000002
#define PORT_PED            0x00000004
#define PORT_PEDC           0x00000008
#define PORT_OCA            0x00000010
#define PORT_OCC            0x00000020
#define PORT_PR             0x00000100
#define PORT_LS_MASK        0x00000C00
#define PORT_LS_K           0x00000400
#define PORT_PP             0x00001000
#define PORT_OWNER          0x00002000
#define PORT_RWC            (PORT_CSC | PORT_PEDC | PORT_OCC)

#define LINK_T              0x00000001
#define LINK_QH             0x00000002

#define TOK_ACTIVE          0x00000080
#define TOK_HALTED          0x00000040
#define TOK_DBERR           0x00000020
#define TOK_BABBLE          0x00000010
#define TOK_XACT            0x00000008
#define TOK_MMF             0x00000004
#define TOK_PID_OUT         (0UL << 8)
#define TOK_PID_IN          (1UL << 8)
#define TOK_PID_SETUP       (2UL << 8)
#define TOK_CERR3           (3UL << 10)
#define TOK_IOC             0x00008000
#define TOK_TOGGLE          0x80000000

#define EHCI_MAX_TDS        80          /* bulk: at least 1 MB per transfer */
#define EHCI_CTL_TDS        4
#define QH_SIZE             128
#define TD_SIZE             64

typedef struct _EQH {
    ULONG      *Qh;
    ULONG       QhPhys;
    ULONG      *Td[EHCI_MAX_TDS];
    ULONG       TdPhys[EHCI_MAX_TDS];
    ULONG       TdLen[EHCI_MAX_TDS];
    UCHAR       NTd;
    UCHAR       DataTd;         /* control: index of data qTD, 0xFF if none */
    UCHAR       Periodic;
    UCHAR       Linked;
    USB_PIPE   *Pipe;
    struct _EQH *Next;          /* schedule order */
} EQH;

typedef struct _EHC {
    USB_HC     *Hc;
    UCHAR      *Cap;
    UCHAR      *Op;
    ULONG       HcsParams;
    ULONG       HccParams;
    ULONG      *Frames;
    ULONG       FramesPhys;
    EQH         AsyncHead;
    EQH         IntHead;
    BOOLEAN     UseIrq;
    BOOLEAN     HasCompanions;
} EHC;

#define EHCP(hc)    ((EHC *)(hc)->Priv)

static ULONG OpRd(EHC *e, ULONG r) { return MmioRd32(e->Op + r); }
static void  OpWr(EHC *e, ULONG r, ULONG v) { MmioWr32(e->Op + r, v); }

static BOOLEAN WaitOp(EHC *e, ULONG Reg, ULONG Mask, ULONG Val, ULONG TimeoutMs)
{
    ULONG start = OsTimeMs();

    for (;;) {
        ULONG v = OpRd(e, Reg);
        if (v == 0xFFFFFFFF) {
            return FALSE;
        }
        if ((v & Mask) == Val) {
            return TRUE;
        }
        if (OsTimeMs() - start > TimeoutMs) {
            return FALSE;
        }
        OsStallUs(100);
    }
}

/* ------------------------------------------------------------------ */
/* Schedule management                                                  */
/* ------------------------------------------------------------------ */

static void AsyncWaitAdvance(EHC *e)
{
    ULONG start;

    if (!(OpRd(e, EOP_USBSTS) & STS_ASS)) {
        return;
    }
    OpWr(e, EOP_USBSTS, STS_IAA);
    OpWr(e, EOP_USBCMD, OpRd(e, EOP_USBCMD) | CMD_IAAD);
    start = OsTimeMs();
    while (!(OpRd(e, EOP_USBSTS) & STS_IAA)) {
        if (OsTimeMs() - start > 20) {
            break;
        }
        OsStallUs(50);
    }
    OpWr(e, EOP_USBSTS, STS_IAA);
}

static void QhLink(EHC *e, EQH *q)
{
    EQH *head = q->Periodic ? &e->IntHead : &e->AsyncHead;
    OS_IRQL irql;

    if (q->Linked) {
        return;
    }
    irql = OsLock();
    q->Qh[0] = head->Qh[0];
    q->Next = head->Next;
    head->Qh[0] = q->QhPhys | LINK_QH;
    head->Next = q;
    q->Linked = 1;
    OsUnlock(irql);
}

static void QhUnlink(EHC *e, EQH *q)
{
    EQH *prev = q->Periodic ? &e->IntHead : &e->AsyncHead;
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
    if (q->Periodic) {
        OsSleepMs(2);
    } else {
        AsyncWaitAdvance(e);
    }
}

static void QhIdle(EQH *q, BOOLEAN ResetToggle)
{
    ULONG tog = ResetToggle ? 0 : (q->Qh[6] & TOK_TOGGLE);
    int i;

    q->Qh[3] = 0;
    q->Qh[4] = LINK_T;
    q->Qh[5] = LINK_T;
    q->Qh[6] = tog;
    for (i = 7; i < 17; i++) {
        q->Qh[i] = 0;
    }
    for (i = 0; i < q->NTd; i++) {
        q->Td[i][2] = 0;
    }
}

static void QhSetEndpoint(EQH *q, USB_PIPE *p)
{
    USB_DEV *d = p->Dev;
    ULONG eps;
    ULONG ch;
    ULONG caps;

    switch (d->Speed) {
    case USB_SPEED_LOW:  eps = 1; break;
    case USB_SPEED_HIGH: eps = 2; break;
    default:             eps = 0; break;
    }
    ch = d->Address | ((ULONG)p->Endpoint << 8) | (eps << 12) | ((ULONG)p->MaxPacket << 16);
    if (p->Type == USB_EP_CONTROL) {
        ch |= 1UL << 14;
        if (eps != 2) {
            ch |= 1UL << 27;
        }
    }
    if (!q->Periodic && eps == 2) {
        ch |= 4UL << 28;
    }
    caps = 1UL << 30;
    if (eps != 2 && d->TtHub != NULL) {
        caps |= ((ULONG)d->TtHub->Address << 16) | ((ULONG)d->TtPort << 23);
    }
    if (q->Periodic) {
        caps |= 0x01;
        if (eps != 2) {
            caps |= 0x1CUL << 8;
        }
    }
    q->Qh[1] = ch;
    q->Qh[2] = caps;
}

/* ------------------------------------------------------------------ */
/* Controller                                                           */
/* ------------------------------------------------------------------ */

static void EhciHandoff(EHC *e)
{
    USB_HC *hc = e->Hc;
    ULONG off = (e->HccParams >> 8) & 0xFF;
    int guard = 0;

    while (off >= 0x40 && guard++ < 16) {
        ULONG cap = OsPciRead(hc->Bus, hc->Dev, hc->Fn, off, 4);
        if ((cap & 0xFF) == 1) {
            if (cap & (1UL << 16)) {
                ULONG start = OsTimeMs();
                OsPciWrite(hc->Bus, hc->Dev, hc->Fn, off + 3, 1, 1);
                while ((OsPciRead(hc->Bus, hc->Dev, hc->Fn, off, 4) & (1UL << 16)) && OsTimeMs() - start < 1000) {
                    OsSleepMs(10);
                }
                if (OsPciRead(hc->Bus, hc->Dev, hc->Fn, off, 4) & (1UL << 16)) {
                    UsbLog(LOG_ERR, "ehci: BIOS did not release controller, forcing\n");
                    OsPciWrite(hc->Bus, hc->Dev, hc->Fn, off + 2, 0, 1);
                } else {
                    UsbLog(LOG_INFO, "ehci: BIOS handoff done\n");
                }
            } else {
                OsPciWrite(hc->Bus, hc->Dev, hc->Fn, off + 3, 1, 1);
            }
            OsPciWrite(hc->Bus, hc->Dev, hc->Fn, off + 4, 0, 4);
        }
        off = (cap >> 8) & 0xFF;
    }
}

static int QhAlloc(EQH *q, int NTd)
{
    int i;

    q->Qh = (ULONG *)UsbDmaAlloc(QH_SIZE, &q->QhPhys);
    if (q->Qh == NULL) {
        return USB_ERR_NOMEM;
    }
    for (i = 0; i < NTd; i++) {
        q->Td[i] = (ULONG *)UsbDmaAlloc(TD_SIZE, &q->TdPhys[i]);
        if (q->Td[i] == NULL) {
            return USB_ERR_NOMEM;
        }
    }
    q->NTd = (UCHAR)NTd;
    q->Qh[4] = LINK_T;
    q->Qh[5] = LINK_T;
    return USB_OK;
}

static void QhFree(EQH *q)
{
    int i;

    for (i = 0; i < q->NTd; i++) {
        UsbDmaFree(q->Td[i], TD_SIZE);
    }
    UsbDmaFree(q->Qh, QH_SIZE);
}

static int EhciStart(USB_HC *Hc)
{
    EHC *e;
    ULONG i;
    ULONG ports;

    e = (EHC *)OsAlloc(sizeof(EHC));
    if (e == NULL) {
        return USB_ERR_NOMEM;
    }
    Hc->Priv = e;
    e->Hc = Hc;
    e->Cap = Hc->Mmio;
    e->Op = e->Cap + MmioRd8(e->Cap);
    e->HcsParams = MmioRd32(e->Cap + 4);
    e->HccParams = MmioRd32(e->Cap + 8);
    e->UseIrq = (BOOLEAN)OsGetConfig("UseInterrupts", 0);
    if (e->HcsParams == 0xFFFFFFFF) {
        return USB_ERR_NODEV;
    }
    e->HasCompanions = (BOOLEAN)(((e->HcsParams >> 12) & 0xF) != 0);

    EhciHandoff(e);

    OpWr(e, EOP_USBINTR, 0);
    if (!(OpRd(e, EOP_USBSTS) & STS_HALTED)) {
        OpWr(e, EOP_USBCMD, OpRd(e, EOP_USBCMD) & ~(CMD_RS | CMD_PSE | CMD_ASE));
        WaitOp(e, EOP_USBSTS, STS_HALTED, STS_HALTED, 50);
    }
    OpWr(e, EOP_USBCMD, CMD_HCRESET);
    if (!WaitOp(e, EOP_USBCMD, CMD_HCRESET, 0, 250)) {
        UsbLog(LOG_ERR, "ehci: reset failed\n");
        return USB_ERR_IO;
    }

    e->Frames = (ULONG *)UsbDmaAlloc(4096, &e->FramesPhys);
    if (e->Frames == NULL || QhAlloc(&e->AsyncHead, 0) != USB_OK || QhAlloc(&e->IntHead, 0) != USB_OK) {
        return USB_ERR_NOMEM;
    }
    e->AsyncHead.Qh[0] = e->AsyncHead.QhPhys | LINK_QH;
    e->AsyncHead.Qh[1] = (1UL << 15) | (2UL << 12);
    e->AsyncHead.Qh[2] = 1UL << 30;
    e->AsyncHead.Qh[6] = TOK_HALTED;
    e->IntHead.Qh[0] = LINK_T;
    e->IntHead.Qh[1] = (2UL << 12);
    e->IntHead.Qh[2] = 1UL << 30;
    e->IntHead.Qh[6] = TOK_HALTED;
    e->IntHead.Periodic = 1;
    for (i = 0; i < 1024; i++) {
        e->Frames[i] = e->IntHead.QhPhys | LINK_QH;
    }

    if (e->HccParams & 1) {
        OpWr(e, EOP_CTRLDSSEGMENT, 0);
    }
    OpWr(e, EOP_PERIODICBASE, e->FramesPhys);
    OpWr(e, EOP_ASYNCLISTADDR, e->AsyncHead.QhPhys);
    OpWr(e, EOP_USBSTS, STS_ACK_MASK);
    OpWr(e, EOP_USBINTR, e->UseIrq ? (STS_USBINT | STS_ERR | STS_HSE) : 0);
    OpWr(e, EOP_USBCMD, CMD_RS | CMD_PSE | CMD_ASE | CMD_ITC_1UF);
    if (!WaitOp(e, EOP_USBSTS, STS_HALTED, 0, 100)) {
        UsbLog(LOG_ERR, "ehci: controller did not start\n");
        return USB_ERR_IO;
    }
    OpWr(e, EOP_CONFIGFLAG, 1);
    OsSleepMs(5);

    ports = e->HcsParams & 0xF;
    if (ports > USB_MAX_PORTS) {
        ports = USB_MAX_PORTS;
    }
    Hc->NumPorts = (UCHAR)ports;
    Hc->MaxXfer = 0x10000;
    Hc->SgMax = 0x100000;
    if (e->HcsParams & 0x10) {
        for (i = 1; i <= ports; i++) {
            ULONG sc = OpRd(e, EOP_PORTSC(i));
            if (!(sc & PORT_PP)) {
                OpWr(e, EOP_PORTSC(i), (sc & ~PORT_RWC) | PORT_PP);
            }
        }
        OsSleepMs(20);
    }
    Hc->Running = 1;
    UsbLog(LOG_INFO, "ehci: %u ports, %u companion controller(s)\n", ports, (e->HcsParams >> 12) & 0xF);
    return USB_OK;
}

static void EhciStop(USB_HC *Hc)
{
    EHC *e = EHCP(Hc);

    if (e == NULL) {
        return;
    }
    OpWr(e, EOP_USBCMD, OpRd(e, EOP_USBCMD) & ~(CMD_RS | CMD_PSE | CMD_ASE));
    WaitOp(e, EOP_USBSTS, STS_HALTED, STS_HALTED, 50);
    OpWr(e, EOP_CONFIGFLAG, 0);
    Hc->Running = 0;
}

/* ------------------------------------------------------------------ */
/* Root ports                                                           */
/* ------------------------------------------------------------------ */

static ULONG EhciPortStatus(USB_HC *Hc, int Port)
{
    EHC *e = EHCP(Hc);
    ULONG sc = OpRd(e, EOP_PORTSC(Port));
    ULONG ps = 0;

    if (sc == 0xFFFFFFFF) {
        return 0;
    }
    if (sc & PORT_OWNER) {
        return PS_OTHER_OWNER | ((sc & PORT_CCS) ? PS_CONNECT : 0);
    }
    if (sc & PORT_CCS) ps |= PS_CONNECT;
    if (sc & PORT_PED) ps |= PS_ENABLE;
    if (sc & PORT_CSC) ps |= PS_CHANGE;
    if (sc & PORT_OCA) ps |= PS_OVERCURRENT;
    if (sc & PORT_PP)  ps |= PS_POWER;
    ps |= (ULONG)USB_SPEED_HIGH << PS_SPEED_SHIFT;
    return ps;
}

static void EhciPortClearChange(USB_HC *Hc, int Port)
{
    EHC *e = EHCP(Hc);
    ULONG sc = OpRd(e, EOP_PORTSC(Port));

    OpWr(e, EOP_PORTSC(Port), sc);
}

static void ReleasePort(EHC *e, int Port, const char *Why)
{
    ULONG sc = OpRd(e, EOP_PORTSC(Port));

    if (!e->HasCompanions) {
        UsbLog(LOG_ERR, "ehci: %s device on port %d, no companion controller\n", Why, Port);
        return;
    }
    UsbLog(LOG_DBG, "ehci: port %d %s device handed to companion\n", Port, Why);
    OpWr(e, EOP_PORTSC(Port), (sc & ~PORT_RWC) | PORT_OWNER);
    OsSleepMs(10);
}

static int EhciPortReset(USB_HC *Hc, int Port, UCHAR *Speed)
{
    EHC *e = EHCP(Hc);
    ULONG sc = OpRd(e, EOP_PORTSC(Port));
    ULONG start;

    if (!(sc & PORT_CCS)) {
        return USB_ERR_NODEV;
    }
    if ((sc & PORT_LS_MASK) == PORT_LS_K) {
        ReleasePort(e, Port, "low speed");
        return e->HasCompanions ? USB_PORT_HANDOFF : USB_ERR_IO;
    }
    OpWr(e, EOP_PORTSC(Port), ((sc & ~PORT_RWC) & ~PORT_PED) | PORT_PR);
    OsSleepMs(50);
    sc = OpRd(e, EOP_PORTSC(Port));
    OpWr(e, EOP_PORTSC(Port), (sc & ~PORT_RWC) & ~PORT_PR);
    start = OsTimeMs();
    while (OpRd(e, EOP_PORTSC(Port)) & PORT_PR) {
        if (OsTimeMs() - start > 20) {
            UsbLog(LOG_ERR, "ehci: port %d reset did not complete\n", Port);
            return USB_ERR_TIMEOUT;
        }
        OsStallUs(100);
    }
    OsSleepMs(5);
    sc = OpRd(e, EOP_PORTSC(Port));
    if (!(sc & PORT_CCS)) {
        return USB_ERR_NODEV;
    }
    if (!(sc & PORT_PED)) {
        ReleasePort(e, Port, "full speed");
        return e->HasCompanions ? USB_PORT_HANDOFF : USB_ERR_IO;
    }
    *Speed = USB_SPEED_HIGH;
    return USB_OK;
}

static void EhciPortDisable(USB_HC *Hc, int Port)
{
    EHC *e = EHCP(Hc);
    ULONG sc = OpRd(e, EOP_PORTSC(Port));

    OpWr(e, EOP_PORTSC(Port), (sc & ~PORT_RWC) & ~PORT_PED);
}

/* ------------------------------------------------------------------ */
/* Devices and pipes                                                    */
/* ------------------------------------------------------------------ */

static int EhciDevUpdate(USB_DEV *Dev)
{
    EQH *q = (EQH *)Dev->Ep0.HcPriv;

    if (q != NULL) {
        QhSetEndpoint(q, &Dev->Ep0);
    }
    return USB_OK;
}

static int EhciPipeOpen(USB_PIPE *Pipe)
{
    EHC *e = EHCP(Pipe->Dev->Hc);
    EQH *q;

    if (Pipe->Type == USB_EP_ISOCH) {
        return USB_ERR_PARAM;
    }
    q = (EQH *)OsAlloc(sizeof(EQH));
    if (q == NULL) {
        return USB_ERR_NOMEM;
    }
    q->Pipe = Pipe;
    q->Periodic = (UCHAR)(Pipe->Type == USB_EP_INTERRUPT);
    if (QhAlloc(q, (Pipe->Type == USB_EP_INTERRUPT) ? 2 :
                   (Pipe->Type == USB_EP_CONTROL) ? EHCI_CTL_TDS : EHCI_MAX_TDS) != USB_OK) {
        QhFree(q);
        OsFree(q);
        return USB_ERR_NOMEM;
    }
    QhSetEndpoint(q, Pipe);
    QhIdle(q, TRUE);
    Pipe->HcPriv = q;
    QhLink(e, q);
    return USB_OK;
}

static void EhciPipeClose(USB_PIPE *Pipe)
{
    EHC *e = EHCP(Pipe->Dev->Hc);
    EQH *q = (EQH *)Pipe->HcPriv;

    if (q == NULL) {
        return;
    }
    QhUnlink(e, q);
    Pipe->HcPriv = NULL;
    QhFree(q);
    OsFree(q);
}

static void FillTd(EQH *q, int i, ULONG Phys, ULONG Len, ULONG Token)
{
    ULONG *t = q->Td[i];
    ULONG page = Phys & ~0xFFFUL;
    int k;

    t[0] = LINK_T;
    t[1] = LINK_T;
    t[3] = Phys;
    for (k = 1; k < 5; k++) {
        t[3 + k] = page + 0x1000UL * k;
    }
    for (k = 8; k < 13; k++) {
        t[k] = 0;
    }
    q->TdLen[i] = Len;
    t[2] = Token | (Len << 16) | TOK_CERR3 | TOK_ACTIVE;
}

/* Bulk / interrupt qTD for bytes Off .. Off + Len of a transfer */
static void FillTdX(EQH *q, int i, USB_XFER *X, ULONG Off, ULONG Len, ULONG Token)
{
    ULONG *t = q->Td[i];
    ULONG run;
    ULONG phys = (X->Length != 0) ? UsbXferPhys(X, Off, &run) : X->Phys;
    ULONG next = Off + 0x1000 - (phys & 0xFFF);
    int k;

    t[0] = LINK_T;
    t[1] = LINK_T;
    t[3] = phys;
    for (k = 1; k < 5; k++) {
        if (next < Off + Len) {
            t[3 + k] = UsbXferPhys(X, next, &run) & ~0xFFFUL;
        } else {
            t[3 + k] = 0;
        }
        next += 0x1000;
    }
    for (k = 8; k < 13; k++) {
        t[k] = 0;
    }
    q->TdLen[i] = Len;
    t[2] = Token | (Len << 16) | TOK_CERR3 | TOK_ACTIVE;
}

static int EhciSubmit(USB_XFER *X)
{
    USB_PIPE *p = X->Pipe;
    EQH *q = (EQH *)p->HcPriv;
    int n = 0;
    int i;
    ULONG first;

    if (q == NULL) {
        return USB_ERR_NODEV;
    }
    if (q->Qh[6] & TOK_ACTIVE) {
        return USB_ERR_BUSY;
    }
    q->DataTd = 0xFF;

    if (p->Type == USB_EP_CONTROL) {
        FillTd(q, n++, X->SetupPhys, 8, TOK_PID_SETUP);
        if (X->Length != 0) {
            q->DataTd = (UCHAR)n;
            FillTd(q, n++, X->Phys, X->Length, (X->DirIn ? TOK_PID_IN : TOK_PID_OUT) | TOK_TOGGLE);
        }
        FillTd(q, n++, 0, 0, ((X->Length == 0 || !X->DirIn) ? TOK_PID_IN : TOK_PID_OUT) | TOK_TOGGLE | TOK_IOC);
        if (q->DataTd != 0xFF) {
            q->Td[q->DataTd][1] = q->TdPhys[n - 1];
        }
    } else {
        ULONG done = 0;
        ULONG pid = p->DirIn ? TOK_PID_IN : TOK_PID_OUT;
        do {
            ULONG run;
            ULONG phys = (X->Length != 0) ? UsbXferPhys(X, done, &run) : X->Phys;
            ULONG len = X->Length - done;
            ULONG room = 0x5000 - (phys & 0xFFF);
            if (len > room) {
                len = room - room % p->MaxPacket;
            }
            if (n == q->NTd) {
                return USB_ERR_PARAM;
            }
            FillTdX(q, n++, X, done, len, pid);
            done += len;
        } while (done < X->Length);
        q->Td[n - 1][2] |= TOK_IOC;
    }
    for (i = 0; i + 1 < n; i++) {
        q->Td[i][0] = q->TdPhys[i + 1];
    }
    for (i = n; i < q->NTd; i++) {
        q->Td[i][2] = 0;
    }
    X->Hc[0] = (ULONG)n;

    first = q->TdPhys[0];
    q->Qh[5] = LINK_T;
    q->Qh[6] &= TOK_TOGGLE;
    q->Qh[4] = first;
    return USB_OK;
}

static int TokenStatus(ULONG tok)
{
    if (tok & TOK_BABBLE) {
        return USB_ERR_BABBLE;
    }
    if (tok & (TOK_DBERR | TOK_XACT | TOK_MMF)) {
        return ((tok >> 10) & 3) ? USB_ERR_STALL : USB_ERR_IO;
    }
    return ((tok >> 10) & 3) ? USB_ERR_STALL : USB_ERR_IO;
}

static void CheckQh(EQH *q)
{
    USB_XFER *X = q->Pipe->Cur;
    int n;
    int i;
    ULONG actual = 0;
    BOOLEAN control = (BOOLEAN)(q->Pipe->Type == USB_EP_CONTROL);

    if (X == NULL) {
        return;
    }
    n = (int)X->Hc[0];
    for (i = 0; i < n; i++) {
        ULONG tok = q->Td[i][2];
        ULONG left;
        if (tok & TOK_ACTIVE) {
            return;
        }
        if (tok & TOK_HALTED) {
            UsbXferDone(X, TokenStatus(tok), actual);
            QhIdle(q, FALSE);
            return;
        }
        left = (tok >> 16) & 0x7FFF;
        if (!control || i == q->DataTd) {
            actual += q->TdLen[i] - left;
        }
        if (left != 0 && i + 1 < n) {
            if (control && i == q->DataTd) {
                continue;
            }
            UsbXferDone(X, USB_OK, actual);
            QhIdle(q, FALSE);
            return;
        }
    }
    UsbXferDone(X, USB_OK, actual);
}

static void EhciPoll(USB_HC *Hc)
{
    EHC *e = EHCP(Hc);
    EQH *q;

    if (e == NULL) {
        return;
    }
    for (q = e->AsyncHead.Next; q != NULL; q = q->Next) {
        CheckQh(q);
    }
    for (q = e->IntHead.Next; q != NULL; q = q->Next) {
        CheckQh(q);
    }
    if (!e->UseIrq) {
        ULONG sts = OpRd(e, EOP_USBSTS);
        if (sts & (STS_USBINT | STS_ERR | STS_PCD)) {
            OpWr(e, EOP_USBSTS, sts & (STS_USBINT | STS_ERR | STS_PCD));
        }
        if (sts & STS_HSE) {
            UsbLog(LOG_ERR, "ehci: host system error\n");
            OpWr(e, EOP_USBSTS, STS_HSE);
        }
    }
}

static BOOLEAN EhciInterrupt(USB_HC *Hc)
{
    EHC *e = EHCP(Hc);
    ULONG sts;

    if (e == NULL || !Hc->Running) {
        return FALSE;
    }
    sts = OpRd(e, EOP_USBSTS);
    if (sts == 0xFFFFFFFF || !(sts & (STS_USBINT | STS_ERR | STS_HSE | STS_PCD | STS_IAA | STS_FLR))) {
        return FALSE;
    }
    OpWr(e, EOP_USBSTS, sts & (STS_USBINT | STS_ERR | STS_HSE | STS_PCD | STS_FLR));
    return TRUE;
}

static void EhciCancel(USB_PIPE *Pipe)
{
    EHC *e = EHCP(Pipe->Dev->Hc);
    EQH *q = (EQH *)Pipe->HcPriv;
    OS_IRQL irql;

    if (q == NULL) {
        return;
    }
    QhUnlink(e, q);
    irql = OsLock();
    CheckQh(q);
    QhIdle(q, FALSE);
    OsUnlock(irql);
    QhLink(e, q);
}

static int EhciPipeReset(USB_PIPE *Pipe)
{
    EHC *e = EHCP(Pipe->Dev->Hc);
    EQH *q = (EQH *)Pipe->HcPriv;

    if (q == NULL) {
        return USB_ERR_NODEV;
    }
    QhUnlink(e, q);
    QhIdle(q, TRUE);
    QhLink(e, q);
    return USB_OK;
}

static int EhciDevInit(USB_DEV *Dev)
{
    (void)Dev;
    return USB_OK;
}

static void EhciDevFree(USB_DEV *Dev)
{
    (void)Dev;
}

const HCD_OPS EhciOps = {
    "EHCI",
    EhciStart,
    EhciStop,
    EhciPortStatus,
    EhciPortClearChange,
    EhciPortReset,
    EhciPortDisable,
    EhciDevInit,
    NULL,
    EhciDevUpdate,
    EhciDevFree,
    EhciPipeOpen,
    EhciPipeClose,
    EhciSubmit,
    EhciCancel,
    EhciPipeReset,
    EhciPoll,
    EhciInterrupt
};
