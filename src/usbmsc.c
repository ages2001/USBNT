/*
 * usbmsc.c - USB mass storage (Bulk-Only, CBI and UAS transports) and the
 * removable disk slots presented to the operating system as SCSI targets.
 *
 * Bulk-Only and CBI commands run synchronously in the worker. On
 * controllers that queue transfers the CBW, data and CSW stages of a
 * Bulk-Only command are queued together. UAS devices on SuperSpeed ports
 * use bulk streams and run several read/write commands at the same time;
 * everything else is issued one command at a time.
 */

#include "usbscsi.h"
#include "usbblk.h"

#define MSC_BUF_SIZE        0x10000

#define MSC_PROTO_CBI_INT   0x00
#define MSC_PROTO_CBI       0x01
#define MSC_PROTO_BOT       0x50
#define MSC_PROTO_UAS       0x62
#define MSC_PROTO_VIRT      0xFE    /* not USB: a BLK_TARGET (SD card readers) */

#define MSC_OK              0
#define MSC_FAILED          1
#define MSC_ERROR           2
#define MSC_NODEV           3

#define CBW_SIGNATURE       0x43425355
#define CSW_SIGNATURE       0x53425355

#define UAS_MAX_TAGS        8
#define UAS_IU_SIZE         128         /* command IU at 0, status IU at 32 */
#define UAS_STAT_OFF        32
#define UAS_STAT_LEN        96
#define UAS_TIMEOUT_MS      30000

#define IU_COMMAND          0x01
#define IU_SENSE            0x03
#define IU_RESPONSE         0x04
#define IU_READ_READY       0x06
#define IU_WRITE_READY      0x07

#define USB_DT_PIPE_USAGE   0x24
#define UAS_PIPE_CMD        1
#define UAS_PIPE_STAT       2
#define UAS_PIPE_IN         3
#define UAS_PIPE_OUT        4

#define SCSIOP_TEST_UNIT_READY  0x00
#define SCSIOP_REQUEST_SENSE    0x03
#define SCSIOP_FORMAT_UNIT      0x04
#define SCSIOP_READ6            0x08
#define SCSIOP_WRITE6           0x0A
#define SCSIOP_INQUIRY          0x12
#define SCSIOP_MODE_SELECT6     0x15
#define SCSIOP_MODE_SENSE6      0x1A
#define SCSIOP_START_STOP       0x1B
#define SCSIOP_MEDIUM_REMOVAL   0x1E
#define SCSIOP_READ_CAPACITY    0x25
#define SCSIOP_READ10           0x28
#define SCSIOP_WRITE10          0x2A
#define SCSIOP_VERIFY10         0x2F
#define SCSIOP_SYNC_CACHE       0x35
#define SCSIOP_MODE_SELECT10    0x55
#define SCSIOP_MODE_SENSE10     0x5A
#define SCSIOP_READ12           0xA8
#define SCSIOP_WRITE12          0xAA

#define SENSE_NOT_READY         0x02
#define SENSE_ILLEGAL_REQUEST   0x05
#define SENSE_UNIT_ATTENTION    0x06
#define SENSE_DATA_PROTECT      0x07

/* Where the data of a command lives */
typedef struct _MSC_DATA {
    UCHAR      *Va;             /* system virtual address, may be NULL with Pfn */
    ULONG      *Pfn;            /* page frames, NULL = use Va */
    ULONG       PageOff;
} MSC_DATA;

typedef struct _MSC_DEV MSC_DEV;

typedef struct _SCSI_SLOT {
    MSC_DEV    *Msc;
    UCHAR       Lun;
    UCHAR       Present;
    UCHAR       UnitAttention;
    UCHAR       Ejected;
    UCHAR       Type;           /* type reported to the OS */
    UCHAR       TypeFixed;
    UCHAR       WriteProtect;
    UCHAR       Used;           /* has held a device since boot */
    UCHAR       MaybeMounted;   /* media was accessed since the last eject */
    UCHAR       MediaReady;
    UCHAR       SenseValid;
    UCHAR       Ph;             /* NT 3.x / 4.0: placeholder medium until the disk driver read its table */
    UCHAR       Parts;          /* partitions on the medium (MBR primary and logical, GPT), 0 = none */
    UCHAR       Fixed;          /* reported to the OS as a fixed medium (RMB 0) */
    UCHAR       DevRmb;         /* the device's own RMB bit */
    UCHAR       Rescan;         /* the medium may be new: count its partitions again */
    UCHAR       PollGone;       /* media poll: no medium seen since the slot was ejected */
    ULONG       MediaGen;       /* changes with every new medium */
    ULONG       PollMs;         /* last media poll */
    ULONG       Identity;
    ULONG       LastLba;
    ULONG       BlockSize;
    UCHAR       Inquiry[36];
    UCHAR       Sense[18];
} SCSI_SLOT;

typedef struct _UAS_TAG {
    MSC_DEV    *Msc;
    UCHAR       Busy;
    UCHAR       Sync;           /* issued by the worker, which reads the result */
    UCHAR       Left;           /* transfers still running */
    UCHAR       Failed;         /* a transfer ended with an error */
    USHORT      Tag;
    UCHAR       Lun;
    UCHAR       Dir;
    ULONG       Len;
    ULONG       Start;
    SCSI_REQ   *Req;
    SCSI_SLOT  *Slot;
    UCHAR      *Iu;
    ULONG       IuPhys;
    USB_XFER    Cmd;
    USB_XFER    Data;
    USB_XFER    Stat;
    OS_EVENT    Done;
    int         Result;
    ULONG       Moved;
} UAS_TAG;

struct _MSC_DEV {
    USB_DEV    *Dev;
    UCHAR       Iface;
    UCHAR       Alt;
    UCHAR       SubClass;
    UCHAR       Proto;
    UCHAR       MaxLun;
    UCHAR       Gone;
    UCHAR       CdbPad12;
    UCHAR       NumTags;        /* UAS: tags in use at most */
    UCHAR       Streams;        /* UAS: bulk streams in use */
    UCHAR       Recover;        /* UAS: pipes need an abort and halt clear */
    UCHAR       SenseValid;
    UCHAR       SenseLun;
    ULONG       Tag;
    ULONG       Identity;
    ULONG       MaxXfer;        /* bytes per device command */
    USB_PIPE    In;             /* BOT/CBI bulk pipes, UAS data pipes */
    USB_PIPE    Out;
    USB_PIPE    Intr;
    USB_PIPE    Cmd;            /* UAS command and status pipes */
    USB_PIPE    Stat;
    USB_PIPE    StatS[UAS_MAX_TAGS];
    USB_PIPE    InS[UAS_MAX_TAGS];
    USB_PIPE    OutS[UAS_MAX_TAGS];
    UCHAR      *Cbw;            /* CBW at 0, CSW at 32, CBI status at 48 */
    ULONG       CbwPhys;
    UCHAR      *Buf;
    ULONG       BufPhys;
    UCHAR       Sense[18];      /* UAS: sense of the last failed command */
    UAS_TAG     Tags[UAS_MAX_TAGS];
    SCSI_REQ   *WaitHead;       /* UAS: requests waiting for a free tag */
    SCSI_REQ   *WaitTail;
    /* MSC_PROTO_VIRT */
    UCHAR       VKind;                  /* BLK_KIND_* */
    const BLK_TARGET_OPS *VOps;
    void       *VCtx;
    UCHAR       VSense[18];
    UCHAR       VSenseValid;
    USB_DEV     VDev;           /* stand-in for the USB device the slot code refers to */
    USB_HC      VHc;
    char        VName[24];
};

ULONG UsbScsiSlotCount = 4;
ULONG UsbScsiFixedMode;         /* Windows 2000 / XP: RemovableAsFixed */
static ULONG PhParts;           /* NT 3.x / 4.0: partition devices prepared per slot, 0 = none */
static ULONG FreezeMs;
static SCSI_SLOT Slots[SCSI_MAX_SLOTS];
static SCSI_REQ *QueueHead;
static SCSI_REQ *QueueTail;
static UCHAR Frozen;
static OS_EVENT MscKick;        /* new request or finished UAS command */
static volatile ULONG UasBusy;  /* asynchronous UAS commands running */
static BOOLEAN UasFromWait;     /* restarting a request from a waiting list */

static void UasRecover(MSC_DEV *m);

/* ------------------------------------------------------------------ */
/* Data movement                                                        */
/* ------------------------------------------------------------------ */

static void DataAdvance(MSC_DATA *d, ULONG n)
{
    if (d->Va != NULL) {
        d->Va += n;
    }
    if (d->Pfn != NULL) {
        ULONG o = d->PageOff + n;
        d->Pfn += o / USB_PAGE_SIZE;
        d->PageOff = o % USB_PAGE_SIZE;
    }
}

static BOOLEAN DataSg(MSC_DEV *m, const MSC_DATA *d)
{
    return (BOOLEAN)(d->Pfn != NULL && m->Dev->Hc->SgMax != 0);
}

/* Points a transfer at data that needs no copy; FALSE if a bounce copy is needed */
static BOOLEAN DataDirect(MSC_DEV *m, const MSC_DATA *d, USB_XFER *X)
{
    if (DataSg(m, d)) {
        X->Pfn = d->Pfn;
        X->PageOff = d->PageOff;
        X->Buf = NULL;
        return TRUE;
    }
    if (d->Va == m->Buf) {
        X->Pfn = NULL;
        X->Buf = m->Buf;
        X->Phys = m->BufPhys;
        return TRUE;
    }
    return FALSE;
}

/* Moves Len bytes through a bulk pipe, copying through the bounce buffer when needed */
static int MscMove(MSC_DEV *m, USB_PIPE *p, const MSC_DATA *d, ULONG Len, ULONG *Moved)
{
    ULONG done = 0;
    int r = USB_OK;

    *Moved = 0;
    if (Len == 0) {
        return USB_OK;
    }
    if (DataSg(m, d)) {
        return UsbTransferPages(p, d->Pfn, d->PageOff, Len, 20000, Moved);
    }
    if (d->Va == m->Buf) {
        return UsbTransfer(p, m->Buf, m->BufPhys, Len, 20000, Moved);
    }
    if (d->Va == NULL) {
        return USB_ERR_PARAM;
    }
    while (done < Len) {
        ULONG chunk = Len - done;
        ULONG got = 0;
        if (chunk > MSC_BUF_SIZE) {
            chunk = MSC_BUF_SIZE;
        }
        if (!p->DirIn) {
            UsbMemCpy(m->Buf, d->Va + done, chunk);
        }
        r = UsbTransfer(p, m->Buf, m->BufPhys, chunk, 20000, &got);
        if (p->DirIn && got != 0) {
            UsbMemCpy(d->Va + done, m->Buf, got);
        }
        done += got;
        if (r != USB_OK || got < chunk) {
            break;
        }
    }
    *Moved = done;
    return r;
}

/* ------------------------------------------------------------------ */
/* Bulk-Only transport                                                  */
/* ------------------------------------------------------------------ */

static void BotResetRecovery(MSC_DEV *m)
{
    int r;

    UsbLog(LOG_DBG, "msc %u: reset recovery\n", m->Dev->Index);
    UsbCancelPipe(&m->In);
    UsbCancelPipe(&m->Out);
    r = UsbControl(m->Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE, 0xFF, 0, m->Iface, 0, NULL, 2000, NULL);
    if (r == USB_ERR_NODEV) {
        return;
    }
    UsbClearHalt(&m->In);
    UsbClearHalt(&m->Out);
}

static void BuildCbw(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len)
{
    UCHAR *cbw = m->Cbw;

    m->Tag++;
    UsbMemSet(cbw, 0, 31);
    PUT32(cbw, CBW_SIGNATURE);
    PUT32(cbw + 4, m->Tag);
    PUT32(cbw + 8, Len);
    cbw[12] = (UCHAR)((Dir == SCSI_DIR_IN) ? 0x80 : 0x00);
    cbw[13] = Lun;
    cbw[14] = CdbLen;
    UsbMemCpy(cbw + 15, Cdb, CdbLen);
    UsbMemSet(m->Cbw + 32, 0, 13);
}

/* Reads the CSW after the data stage; retries once after a stall */
static int BotReadCsw(MSC_DEV *m, ULONG *Got)
{
    int r = USB_ERR_IO;
    int tries;

    for (tries = 0; tries < 2; tries++) {
        r = UsbTransfer(&m->In, m->Cbw + 32, m->CbwPhys + 32, 13, 20000, Got);
        if (r == USB_ERR_STALL) {
            UsbClearHalt(&m->In);
            continue;
        }
        break;
    }
    return r;
}

static int BotCheckCsw(MSC_DEV *m, int r, ULONG Got, ULONG Len, ULONG Moved, ULONG *Done)
{
    UCHAR *csw = m->Cbw + 32;
    ULONG residue;

    if (r != USB_OK || Got < 13 || GET32(csw) != CSW_SIGNATURE || GET32(csw + 4) != m->Tag) {
        if (r == USB_ERR_NODEV || m->Dev->Gone) {
            return MSC_NODEV;
        }
        UsbLog(LOG_DBG, "msc %u: bad CSW (r=%d got=%u)\n", m->Dev->Index, r, Got);
        BotResetRecovery(m);
        return MSC_ERROR;
    }
    residue = GET32(csw + 8);
    if (csw[12] == 2) {
        BotResetRecovery(m);
        return MSC_ERROR;
    }
    if (residue <= Len && Len - residue < Moved) {
        Moved = Len - residue;
    }
    *Done = Moved;
    return (csw[12] == 0) ? MSC_OK : MSC_FAILED;
}

static int BotCommand(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len,
                      const MSC_DATA *d, ULONG *Done)
{
    ULONG got = 0;
    ULONG moved = 0;
    int r;

    *Done = 0;
    BuildCbw(m, Lun, Cdb, CdbLen, Dir, Len);
    r = UsbTransfer(&m->Out, m->Cbw, m->CbwPhys, 31, 5000, &got);
    if (r != USB_OK || got != 31) {
        if (r == USB_ERR_NODEV) {
            return MSC_NODEV;
        }
        BotResetRecovery(m);
        return MSC_ERROR;
    }
    if (Len != 0 && Dir != SCSI_DIR_NONE) {
        USB_PIPE *p = (Dir == SCSI_DIR_IN) ? &m->In : &m->Out;
        r = MscMove(m, p, d, Len, &moved);
        if (r == USB_ERR_STALL) {
            UsbClearHalt(p);
        } else if (r != USB_OK) {
            if (r == USB_ERR_NODEV) {
                return MSC_NODEV;
            }
            BotResetRecovery(m);
            return MSC_ERROR;
        }
    }
    r = BotReadCsw(m, &got);
    return BotCheckCsw(m, r, got, Len, moved, Done);
}

static void QueuedDone(USB_XFER *X)
{
    OsEventSet((OS_EVENT *)X->Context);
}

typedef struct _BOT_Q {
    OS_EVENT    Ev;
    USB_XFER    Cbw;
    USB_XFER    Data;
    USB_XFER    Csw;
    volatile LONG CswQueued;    /* 1 queued, -1 could not be queued */
} BOT_Q;

/*
 * Completion of the last stage before the CSW: the CSW is queued right
 * away instead of after a round trip through the worker.
 */
static void BotStageDone(USB_XFER *X)
{
    BOT_Q *q = (BOT_Q *)X->Context;

    if (X->Status == USB_OK && q->CswQueued == 0) {
        q->CswQueued = (UsbSubmit(&q->Csw) == USB_OK) ? 1 : -1;
    }
    OsEventSet(&q->Ev);
}

static void BotCswDone(USB_XFER *X)
{
    BOT_Q *q = (BOT_Q *)X->Context;

    OsEventSet(&q->Ev);
}

/* Bulk-Only command with CBW and data queued together and the CSW queued from their completion */
static int BotCommandQueued(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len,
                            const MSC_DATA *d, ULONG *Done)
{
    BOT_Q q;
    BOOLEAN data = (BOOLEAN)(Len != 0 && Dir != SCSI_DIR_NONE);
    USB_PIPE *dp = (Dir == SCSI_DIR_IN) ? &m->In : &m->Out;
    ULONG start = OsTimeMs();
    ULONG moved = 0;
    ULONG got = 0;
    int r;

    *Done = 0;
    BuildCbw(m, Lun, Cdb, CdbLen, Dir, Len);
    UsbMemSet(&q, 0, sizeof(q));
    OsEventInit(&q.Ev);
    q.Cbw.Pipe = &m->Out;
    q.Cbw.Buf = m->Cbw;
    q.Cbw.Phys = m->CbwPhys;
    q.Cbw.Length = 31;
    q.Data.Pipe = dp;
    q.Data.DirIn = dp->DirIn;
    q.Data.Length = Len;
    DataDirect(m, d, &q.Data);
    q.Csw.Pipe = &m->In;
    q.Csw.DirIn = 1;
    q.Csw.Buf = m->Cbw + 32;
    q.Csw.Phys = m->CbwPhys + 32;
    q.Csw.Length = 13;
    q.Cbw.Context = q.Data.Context = q.Csw.Context = &q;
    q.Cbw.Complete = data ? BotCswDone : BotStageDone;
    q.Data.Complete = BotStageDone;
    q.Csw.Complete = BotCswDone;

    r = UsbSubmit(&q.Cbw);
    if (r == USB_OK && data) {
        r = UsbSubmit(&q.Data);
    }
    if (r != USB_OK) {
        UsbCancelPipe(&m->Out);
        if (r == USB_ERR_NODEV || m->Dev->Gone) {
            return MSC_NODEV;
        }
        BotResetRecovery(m);
        return MSC_ERROR;
    }

    for (;;) {
        OsEventReset(&q.Ev);
        if (q.CswQueued == 1 && !q.Csw.Busy) {
            break;
        }
        if (q.CswQueued < 0 || (!q.Cbw.Busy && q.Cbw.Status != USB_OK) ||
            (data && !q.Data.Busy && q.Data.Status != USB_OK)) {
            break;
        }
        if (m->Dev->Gone || OsTimeMs() - start > 20000) {
            break;
        }
        OsEventWait(&q.Ev, 1000);
    }
    {
        int tries = 0;
        while ((q.Cbw.Busy || q.Data.Busy || q.Csw.Busy) && tries++ < 5) {
            UsbCancelPipe(&m->Out);
            UsbCancelPipe(&m->In);
        }
    }
    if (m->Dev->Gone) {
        return MSC_NODEV;
    }
    if (q.Cbw.Status != USB_OK || q.Cbw.Actual != 31) {
        UsbLog(LOG_DBG, "msc %u: CBW failed (%d)\n", m->Dev->Index, q.Cbw.Status);
        BotResetRecovery(m);
        return MSC_ERROR;
    }
    if (data) {
        moved = q.Data.Actual;
        if (q.Data.Status == USB_ERR_STALL) {
            UsbClearHalt(dp);
        } else if (q.Data.Status != USB_OK) {
            UsbLog(LOG_DBG, "msc %u: data stage failed (%d)\n", m->Dev->Index, q.Data.Status);
            BotResetRecovery(m);
            return MSC_ERROR;
        }
    }
    if (q.CswQueued == 1 && q.Csw.Status == USB_OK) {
        r = USB_OK;
        got = q.Csw.Actual;
    } else {
        if (q.CswQueued == 1 && q.Csw.Status == USB_ERR_STALL) {
            UsbClearHalt(&m->In);
        }
        r = BotReadCsw(m, &got);
    }
    return BotCheckCsw(m, r, got, Len, moved, Done);
}

/* ------------------------------------------------------------------ */
/* CBI transport                                                        */
/* ------------------------------------------------------------------ */

static int CbiCommand(MSC_DEV *m, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len,
                      const MSC_DATA *d, ULONG *Done)
{
    UCHAR cmd[12];
    ULONG moved = 0;
    ULONG got = 0;
    int r;

    *Done = 0;
    UsbMemSet(cmd, 0, sizeof(cmd));
    UsbMemCpy(cmd, Cdb, CdbLen > 12 ? 12 : CdbLen);
    r = UsbControl(m->Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE, 0, 0, m->Iface, 12, cmd, 5000, NULL);
    if (r == USB_ERR_STALL) {
        return MSC_FAILED;
    }
    if (r != USB_OK) {
        return (r == USB_ERR_NODEV) ? MSC_NODEV : MSC_ERROR;
    }
    if (Len != 0 && Dir != SCSI_DIR_NONE) {
        USB_PIPE *p = (Dir == SCSI_DIR_IN) ? &m->In : &m->Out;
        r = MscMove(m, p, d, Len, &moved);
        if (r == USB_ERR_STALL) {
            UsbClearHalt(p);
            *Done = moved;
            return MSC_FAILED;
        }
        if (r != USB_OK) {
            return (r == USB_ERR_NODEV) ? MSC_NODEV : MSC_ERROR;
        }
    }
    *Done = moved;
    if (m->Proto == MSC_PROTO_CBI_INT && m->Intr.Opened) {
        UCHAR *st = m->Cbw + 48;
        r = UsbTransfer(&m->Intr, st, m->CbwPhys + 48, 2, 5000, &got);
        if (r != USB_OK || got < 2) {
            return MSC_ERROR;
        }
        if (m->SubClass == 0x04) {
            return (st[0] == 0 && st[1] == 0) ? MSC_OK : MSC_FAILED;
        }
        return ((st[1] & 3) == 0) ? MSC_OK : MSC_FAILED;
    }
    return MSC_OK;
}

/* ------------------------------------------------------------------ */
/* UAS transport                                                        */
/* ------------------------------------------------------------------ */

static void UasBuildCommand(UAS_TAG *t, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen)
{
    UCHAR *iu = t->Iu;

    UsbMemSet(iu, 0, UAS_IU_SIZE);
    iu[0] = IU_COMMAND;
    PUTBE16(iu + 2, t->Tag);
    iu[4] = 0;                  /* simple task attribute */
    iu[9] = Lun;
    UsbMemCpy(iu + 16, Cdb, CdbLen > 16 ? 16 : CdbLen);
}

/* Interprets the status IU of a finished tag */
static void UasResult(UAS_TAG *t)
{
    MSC_DEV *m = t->Msc;
    UCHAR *iu = t->Iu + UAS_STAT_OFF;
    ULONG got = t->Stat.Actual;

    t->Moved = (t->Len != 0) ? t->Data.Actual : 0;
    if (m->Gone || m->Dev->Gone) {
        t->Result = MSC_NODEV;
        return;
    }
    if (t->Stat.Status != USB_OK || got < 4 || GETBE16(iu + 2) != t->Tag) {
        t->Result = MSC_ERROR;
        return;
    }
    if (iu[0] == IU_SENSE && got >= 16) {
        ULONG n = GETBE16(iu + 14);
        if (iu[6] == 0) {
            t->Result = MSC_OK;
            return;
        }
        if (n > got - 16) {
            n = got - 16;
        }
        if (n > 18) {
            n = 18;
        }
        UsbMemSet(m->Sense, 0, sizeof(m->Sense));
        UsbMemCpy(m->Sense, iu + 16, n);
        m->SenseValid = (UCHAR)(n >= 8);
        m->SenseLun = t->Lun;
        t->Result = MSC_FAILED;
        return;
    }
    t->Result = MSC_ERROR;
}

static void UasFreeTag(UAS_TAG *t)
{
    OS_IRQL irql = OsLock();

    if (!t->Sync && UasBusy != 0) {
        UasBusy--;
    }
    t->Busy = 0;
    t->Req = NULL;
    OsUnlock(irql);
}

static void CompleteFromDevice(SCSI_REQ *r, SCSI_SLOT *s, int st, ULONG Done, BOOLEAN ShortIsError);

static void UasFinish(UAS_TAG *t)
{
    MSC_DEV *m = t->Msc;
    SCSI_REQ *r = t->Req;

    UasResult(t);
    if (t->Failed) {
        m->Recover = 1;
    }
    if (t->Sync) {
        OsEventSet(&t->Done);
        return;
    }
    CompleteFromDevice(r, t->Slot, t->Result, t->Moved, TRUE);
    if (r->SrbStatus == SRBST_SUCCESS) {
        t->Slot->MaybeMounted = 1;
    }
    UasFreeTag(t);
    OsEventSet(&MscKick);
    OsWakeWorker();
    if (r->Complete != NULL) {
        r->Complete(r);
    }
}

static void UasXferDone(USB_XFER *X)
{
    UAS_TAG *t = (UAS_TAG *)X->Context;
    OS_IRQL irql;
    BOOLEAN last;

    irql = OsLock();
    if (X->Status != USB_OK) {
        t->Failed = 1;
    }
    t->Left--;
    last = (BOOLEAN)(t->Left == 0);
    OsUnlock(irql);
    if (last) {
        UasFinish(t);
    } else if (X->Status != USB_OK) {
        t->Msc->Recover = 1;
        OsEventSet(&MscKick);
        OsWakeWorker();
    }
}

static UAS_TAG *UasAllocTag(MSC_DEV *m, BOOLEAN Sync)
{
    OS_IRQL irql = OsLock();
    int i;

    for (i = 0; i < m->NumTags; i++) {
        UAS_TAG *t = &m->Tags[i];
        if (!t->Busy) {
            t->Busy = 1;
            t->Sync = (UCHAR)Sync;
            t->Failed = 0;
            if (!Sync) {
                UasBusy++;
            }
            OsUnlock(irql);
            return t;
        }
    }
    OsUnlock(irql);
    return NULL;
}

/* Takes N transfers that never ran off a tag; finishes it when nothing is left */
static void UasDrop(UAS_TAG *t, UCHAR N)
{
    OS_IRQL irql = OsLock();
    BOOLEAN last;

    t->Failed = 1;
    t->Left = (UCHAR)(t->Left - N);
    last = (BOOLEAN)(t->Left == 0);
    OsUnlock(irql);
    t->Msc->Recover = 1;
    if (last) {
        UasFinish(t);
    }
}

/*
 * Queues the status, data and command transfers of a tag (streams mode).
 * The tag always finishes through UasFinish, also when queueing fails.
 */
static void UasStart(UAS_TAG *t, const MSC_DATA *d)
{
    MSC_DEV *m = t->Msc;
    int idx = t->Tag - 1;
    UCHAR total;

    UsbMemSet(&t->Cmd, 0, sizeof(t->Cmd));
    UsbMemSet(&t->Data, 0, sizeof(t->Data));
    UsbMemSet(&t->Stat, 0, sizeof(t->Stat));
    t->Stat.Pipe = &m->StatS[idx];
    t->Stat.DirIn = 1;
    t->Stat.Buf = t->Iu + UAS_STAT_OFF;
    t->Stat.Phys = t->IuPhys + UAS_STAT_OFF;
    t->Stat.Length = UAS_STAT_LEN;
    t->Cmd.Pipe = &m->Cmd;
    t->Cmd.Buf = t->Iu;
    t->Cmd.Phys = t->IuPhys;
    t->Cmd.Length = 32;
    t->Stat.Complete = t->Cmd.Complete = t->Data.Complete = UasXferDone;
    t->Stat.Context = t->Cmd.Context = t->Data.Context = t;
    total = 2;
    if (t->Len != 0) {
        t->Data.Pipe = (t->Dir == SCSI_DIR_IN) ? &m->InS[idx] : &m->OutS[idx];
        t->Data.DirIn = (UCHAR)(t->Dir == SCSI_DIR_IN);
        t->Data.Length = t->Len;
        total = 3;
    }
    t->Left = total;
    t->Start = OsTimeMs();
    UsbLog(LOG_TRACE, "uas: tag %u op %02x len %u, %u running\n", t->Tag, t->Iu[16], t->Len, UasBusy);

    if (t->Len != 0 && !DataDirect(m, d, &t->Data)) {
        UasDrop(t, total);
        return;
    }
    if (UsbSubmit(&t->Stat) != USB_OK) {
        UasDrop(t, total);
        return;
    }
    if (t->Len != 0 && UsbSubmit(&t->Data) != USB_OK) {
        UasDrop(t, 2);
        UsbCancelPipe(t->Stat.Pipe);
        return;
    }
    if (UsbSubmit(&t->Cmd) != USB_OK) {
        UasDrop(t, 1);
        UsbCancelPipe(t->Stat.Pipe);
        if (t->Len != 0) {
            UsbCancelPipe(t->Data.Pipe);
        }
    }
}

/* One command without streams (high speed): status IUs tell when to move data */
static int UasCommandNoStreams(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len,
                               const MSC_DATA *d, ULONG *Done)
{
    UAS_TAG *t = &m->Tags[0];
    OS_EVENT ev;
    ULONG got = 0;
    ULONG moved = 0;
    BOOLEAN dataDone = FALSE;
    int round;
    int r;

    *Done = 0;
    t->Len = Len;
    t->Lun = Lun;
    t->Dir = Dir;
    UasBuildCommand(t, Lun, Cdb, CdbLen);
    OsEventInit(&ev);

    for (round = 0; round < 2; round++) {
        UsbMemSet(&t->Stat, 0, sizeof(t->Stat));
        t->Stat.Pipe = &m->Stat;
        t->Stat.DirIn = 1;
        t->Stat.Buf = t->Iu + UAS_STAT_OFF;
        t->Stat.Phys = t->IuPhys + UAS_STAT_OFF;
        t->Stat.Length = UAS_STAT_LEN;
        t->Stat.Complete = QueuedDone;
        t->Stat.Context = &ev;
        OsEventReset(&ev);
        r = UsbSubmit(&t->Stat);
        if (r != USB_OK) {
            return (r == USB_ERR_NODEV) ? MSC_NODEV : MSC_ERROR;
        }
        if (round == 0) {
            r = UsbTransfer(&m->Cmd, t->Iu, t->IuPhys, 32, 5000, &got);
            if (r != USB_OK) {
                UsbCancelPipe(&m->Stat);
                m->Recover = 1;
                return (r == USB_ERR_NODEV) ? MSC_NODEV : MSC_ERROR;
            }
        }
        if (!OsEventWait(&ev, UAS_TIMEOUT_MS) || t->Stat.Busy) {
            UsbCancelPipe(&m->Stat);
            m->Recover = 1;
            return MSC_ERROR;
        }
        if (t->Stat.Status != USB_OK || t->Stat.Actual < 4) {
            m->Recover = 1;
            return (t->Stat.Status == USB_ERR_NODEV || m->Dev->Gone) ? MSC_NODEV : MSC_ERROR;
        }
        if ((t->Iu[UAS_STAT_OFF] == IU_READ_READY || t->Iu[UAS_STAT_OFF] == IU_WRITE_READY) && !dataDone &&
            Len != 0) {
            USB_PIPE *p = (Dir == SCSI_DIR_IN) ? &m->In : &m->Out;
            r = MscMove(m, p, d, Len, &moved);
            dataDone = TRUE;
            if (r != USB_OK && r != USB_ERR_STALL) {
                m->Recover = 1;
                return (r == USB_ERR_NODEV) ? MSC_NODEV : MSC_ERROR;
            }
            if (r == USB_ERR_STALL) {
                UsbClearHalt(p);
            }
            continue;
        }
        break;
    }
    t->Data.Actual = moved;
    t->Data.Status = USB_OK;
    UasResult(t);
    *Done = t->Moved;
    return t->Result;
}

/* Synchronous UAS command, worker context */
static int UasCommand(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len,
                      const MSC_DATA *d, ULONG *Done)
{
    MSC_DATA bd;
    UAS_TAG *t;
    ULONG start = OsTimeMs();
    int r;

    *Done = 0;
    if (m->Recover) {
        UasRecover(m);
    }
    if (m->Streams == 0) {
        return UasCommandNoStreams(m, Lun, Cdb, CdbLen, Dir, Len, d, Done);
    }
    bd = *d;
    if (Len != 0 && !DataSg(m, d) && d->Va != m->Buf) {
        if (Len > MSC_BUF_SIZE || d->Va == NULL) {
            return MSC_ERROR;
        }
        if (Dir == SCSI_DIR_OUT) {
            UsbMemCpy(m->Buf, d->Va, Len);
        }
        bd.Va = m->Buf;
        bd.Pfn = NULL;
    }
    while ((t = UasAllocTag(m, TRUE)) == NULL) {
        OsEventReset(&MscKick);
        if (m->Gone || m->Dev->Gone) {
            return MSC_NODEV;
        }
        if (OsTimeMs() - start > UAS_TIMEOUT_MS) {
            return MSC_ERROR;
        }
        OsEventWait(&MscKick, 100);
    }
    t->Req = NULL;
    t->Len = (Dir == SCSI_DIR_NONE) ? 0 : Len;
    t->Lun = Lun;
    t->Dir = Dir;
    UasBuildCommand(t, Lun, Cdb, CdbLen);
    OsEventReset(&t->Done);
    UasStart(t, &bd);
    if (!OsEventWait(&t->Done, UAS_TIMEOUT_MS)) {
        UsbLog(LOG_ERR, "msc %u: UAS command %02x timed out\n", m->Dev->Index, Cdb[0]);
        UasRecover(m);
        OsEventWait(&t->Done, 1000);
    }
    r = (t->Left == 0) ? t->Result : MSC_ERROR;
    *Done = t->Moved;
    if (r == MSC_OK && Dir == SCSI_DIR_IN && bd.Va == m->Buf && d->Va != m->Buf && t->Moved != 0) {
        UsbMemCpy(d->Va, m->Buf, t->Moved);
    }
    UasFreeTag(t);
    if (m->Recover) {
        UasRecover(m);
    }
    return r;
}

/* Aborts every running UAS command and clears the pipes, worker context */
static void UasRecover(MSC_DEV *m)
{
    int i;

    UsbLog(LOG_DBG, "msc %u: UAS recovery\n", m->Dev->Index);
    m->Recover = 0;
    for (i = 0; i < m->Streams; i++) {
        UsbCancelPipe(&m->StatS[i]);
        UsbCancelPipe(&m->InS[i]);
        UsbCancelPipe(&m->OutS[i]);
    }
    UsbCancelPipe(&m->Stat);
    UsbCancelPipe(&m->In);
    UsbCancelPipe(&m->Out);
    UsbCancelPipe(&m->Cmd);
    if (m->Gone || m->Dev->Gone) {
        return;
    }
    UsbClearHalt(&m->Cmd);
    UsbClearHalt(&m->Stat);
    UsbClearHalt(&m->In);
    UsbClearHalt(&m->Out);
    m->Recover = 0;
}

/* ------------------------------------------------------------------ */
/* Transport selection                                                  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Virtual targets (usbblk.h)                                           */
/* ------------------------------------------------------------------ */

void BlkSense(UCHAR *Sense, UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    UsbMemSet(Sense, 0, 18);
    Sense[0] = 0x70;
    Sense[2] = Key;
    Sense[7] = 10;
    Sense[12] = Asc;
    Sense[13] = Ascq;
}

static int VirtCommand(MSC_DEV *m, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len, const MSC_DATA *d,
                       ULONG *Done)
{
    int r;

    *Done = 0;
    if (Cdb[0] == 0x03) {                                   /* REQUEST SENSE: the stored sense */
        ULONG n = Len < 18 ? Len : 18;
        if (!m->VSenseValid) {
            BlkSense(m->VSense, 0, 0, 0);
        }
        if (d->Va != NULL && n != 0) {
            UsbMemCpy(d->Va, m->VSense, n);
        }
        m->VSenseValid = 0;
        *Done = n;
        return MSC_OK;
    }
    if (Len != 0 && d->Va == NULL) {
        return MSC_ERROR;
    }
    r = m->VOps->Command(m->VCtx, Cdb, CdbLen, Dir, Len, d->Va, Done, m->VSense);
    switch (r) {
    case BLK_OK:
        return MSC_OK;
    case BLK_FAILED:
        m->VSenseValid = 1;
        return MSC_FAILED;
    case BLK_NODEV:
        return MSC_NODEV;
    default:
        return MSC_ERROR;
    }
}

static int MscCommandD(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len,
                       const MSC_DATA *d, ULONG *Done)
{
    UCHAR cdb[16];

    *Done = 0;
    if (m->Gone || m->Dev->Gone) {
        return MSC_NODEV;
    }
    if (Dir == SCSI_DIR_NONE) {
        Len = 0;
    }
    UsbMemSet(cdb, 0, sizeof(cdb));
    UsbMemCpy(cdb, Cdb, CdbLen > 16 ? 16 : CdbLen);
    if (m->CdbPad12 && CdbLen < 12) {
        CdbLen = 12;
    }
    switch (m->Proto) {
    case MSC_PROTO_VIRT:
        return VirtCommand(m, cdb, CdbLen, Dir, Len, d, Done);
    case MSC_PROTO_BOT:
        if (m->Dev->Hc->QueueOk && (Len == 0 || DataSg(m, d) || d->Va == m->Buf)) {
            return BotCommandQueued(m, Lun, cdb, CdbLen, Dir, Len, d, Done);
        }
        return BotCommand(m, Lun, cdb, CdbLen, Dir, Len, d, Done);
    case MSC_PROTO_UAS:
        return UasCommand(m, Lun, cdb, CdbLen, Dir, Len, d, Done);
    default:
        return CbiCommand(m, cdb, CdbLen, Dir, Len, d, Done);
    }
}

/* Command whose data lives in the bounce buffer */
static int MscCommand(MSC_DEV *m, UCHAR Lun, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len, ULONG *Done)
{
    MSC_DATA d;

    if (Len > MSC_BUF_SIZE) {
        return MSC_ERROR;
    }
    d.Va = m->Buf;
    d.Pfn = NULL;
    d.PageOff = 0;
    return MscCommandD(m, Lun, Cdb, CdbLen, Dir, Len, &d, Done);
}

static int MscRequestSense(MSC_DEV *m, UCHAR Lun, UCHAR *Sense)
{
    UCHAR cdb[6];
    ULONG got = 0;
    int r;

    if (m->Proto == MSC_PROTO_UAS && m->SenseValid && m->SenseLun == Lun) {
        UsbMemCpy(Sense, m->Sense, 18);
        m->SenseValid = 0;
        return MSC_OK;
    }
    UsbMemSet(cdb, 0, sizeof(cdb));
    cdb[0] = SCSIOP_REQUEST_SENSE;
    cdb[4] = 18;
    r = MscCommand(m, Lun, cdb, 6, SCSI_DIR_IN, 18, &got);
    if (r != MSC_OK || got < 8) {
        return r == MSC_OK ? MSC_ERROR : r;
    }
    UsbMemSet(Sense, 0, 18);
    UsbMemCpy(Sense, m->Buf, got > 18 ? 18 : got);
    return MSC_OK;
}

/* ------------------------------------------------------------------ */
/* Slots                                                                */
/* ------------------------------------------------------------------ */

static void SetSense(UCHAR *Sense, UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    UsbMemSet(Sense, 0, 18);
    Sense[0] = 0x70;
    Sense[2] = Key;
    Sense[7] = 10;
    Sense[12] = Asc;
    Sense[13] = Ascq;
}

static void ReqCheck(SCSI_REQ *r, SCSI_SLOT *s, UCHAR Key, UCHAR Asc, UCHAR Ascq)
{
    SetSense(r->Sense, Key, Asc, Ascq);
    r->SenseLength = 18;
    r->ScsiStatus = SCSIST_CHECK_CONDITION;
    r->SrbStatus = SRBST_ERROR | SRBST_AUTOSENSE_VALID;
    r->Transferred = 0;
    if (s != NULL) {
        UsbMemCpy(s->Sense, r->Sense, 18);
        s->SenseValid = 1;
    }
}

static void ReqData(SCSI_REQ *r, const void *Data, ULONG Len)
{
    if (Len > r->DataLength) {
        Len = r->DataLength;
    }
    if (Len != 0 && r->Data != NULL) {
        UsbMemCpy(r->Data, Data, Len);
    }
    r->Transferred = Len;
    r->ScsiStatus = SCSIST_GOOD;
    r->SrbStatus = SRBST_SUCCESS;
}

static BOOLEAN IsDiskType(UCHAR t)
{
    return (BOOLEAN)(t == 0x00 || t == 0x07 || t == 0x0E);
}

static void SlotInquiry(SCSI_SLOT *s, UCHAR *Out)
{
    static const char vendor[8] = { 'U', 'S', 'B', ' ', ' ', ' ', ' ', ' ' };
    static const char prod[16] = { 'R', 'e', 'm', 'o', 'v', 'a', 'b', 'l', 'e', ' ', 'D', 'i', 's', 'k', ' ', ' ' };

    UsbMemSet(Out, 0, 36);
    if (s->Present) {
        UsbMemCpy(Out, s->Inquiry, 36);
    } else {
        UsbMemCpy(Out + 8, vendor, 8);
        UsbMemCpy(Out + 16, prod, 16);
        Out[32] = '1';
        Out[33] = '.';
        Out[34] = '0';
        Out[35] = ' ';
    }
    Out[0] = s->Type;
    Out[1] = (UCHAR)(s->Fixed ? 0x00 : 0x80);
    Out[2] = 2;
    Out[3] = 2;
    Out[4] = 31;
}

/* Status of a request from a transport result; data was already moved */
static void CompleteFromDevice(SCSI_REQ *r, SCSI_SLOT *s, int st, ULONG Done, BOOLEAN ShortIsError)
{
    MSC_DEV *m = s->Msc;

    if (st == MSC_OK) {
        r->Transferred = Done;
        r->ScsiStatus = SCSIST_GOOD;
        if (Done < r->DataLength && ShortIsError) {
            r->SrbStatus = SRBST_ERROR;
        } else {
            r->SrbStatus = SRBST_SUCCESS;
        }
        switch (r->Cdb[0]) {
        case SCSIOP_TEST_UNIT_READY:
        case SCSIOP_READ_CAPACITY:
        case SCSIOP_READ6:
        case SCSIOP_READ10:
        case SCSIOP_READ12:
        case SCSIOP_WRITE6:
        case SCSIOP_WRITE10:
        case SCSIOP_WRITE12:
            /* (not every command needs the medium: PREVENT ALLOW MEDIUM REMOVAL does not) */
            if (!s->MediaReady) {
                s->Rescan = 1;                  /* a medium came (card reader) */
            }
            s->MediaReady = 1;
            break;
        default:
            break;
        }
        return;
    }
    if (st == MSC_FAILED) {
        r->Transferred = 0;
        r->ScsiStatus = SCSIST_CHECK_CONDITION;
        if (m != NULL && m->Proto == MSC_PROTO_UAS && m->SenseValid && m->SenseLun == s->Lun) {
            UsbMemCpy(r->Sense, m->Sense, 18);
            m->SenseValid = 0;
            r->SenseLength = 18;
            r->SrbStatus = SRBST_ERROR | SRBST_AUTOSENSE_VALID;
        } else if (m != NULL && m->Proto != MSC_PROTO_UAS && MscRequestSense(m, s->Lun, r->Sense) == MSC_OK) {
            r->SenseLength = 18;
            r->SrbStatus = SRBST_ERROR | SRBST_AUTOSENSE_VALID;
        } else {
            r->SrbStatus = SRBST_ERROR;
            return;
        }
        UsbMemCpy(s->Sense, r->Sense, 18);
        s->SenseValid = 1;
        if ((r->Sense[2] & 0x0F) == SENSE_DATA_PROTECT) {
            s->WriteProtect = 1;
        }
        if ((r->Sense[2] & 0x0F) == SENSE_NOT_READY && r->Sense[12] == 0x3A) {
            if (s->MediaReady || s->Fixed) {
                s->Rescan = 1;                  /* the medium went away */
            }
            s->MediaReady = 0;
        }
        if ((r->Sense[2] & 0x0F) == SENSE_UNIT_ATTENTION && r->Sense[12] == 0x28) {
            s->Rescan = 1;                      /* another medium */
        }
        return;
    }
    r->Transferred = 0;
    r->ScsiStatus = SCSIST_GOOD;
    r->SrbStatus = (st == MSC_NODEV) ? SRBST_SELECTION_TIMEOUT : SRBST_ERROR;
}

static void ReqDataDesc(SCSI_REQ *r, MSC_DATA *d)
{
    d->Va = (UCHAR *)r->Data;
    d->Pfn = r->Pfn;
    d->PageOff = r->PageOff;
}

/* Sends a command whose data lives in the request buffer */
static int SlotPassThrough(SCSI_REQ *r, SCSI_SLOT *s, const UCHAR *Cdb, UCHAR CdbLen, BOOLEAN ShortIsError)
{
    MSC_DEV *m = s->Msc;
    MSC_DATA d;
    ULONG done = 0;
    int st;

    ReqDataDesc(r, &d);
    if (!DataSg(m, &d)) {
        d.Pfn = NULL;
        if (r->DataLength > MSC_BUF_SIZE && m->Proto == MSC_PROTO_UAS) {
            r->SrbStatus = SRBST_INVALID_REQUEST;
            return MSC_ERROR;
        }
    }
    st = MscCommandD(m, s->Lun, Cdb, CdbLen, r->Direction, r->DataLength, &d, &done);
    CompleteFromDevice(r, s, st, done, ShortIsError);
    return st;
}

static ULONG RwCount(const UCHAR *Cdb)
{
    return (Cdb[0] == SCSIOP_READ12 || Cdb[0] == SCSIOP_WRITE12) ? GETBE32(Cdb + 6) : GETBE16(Cdb + 7);
}

static void RwSetCount(UCHAR *Cdb, ULONG N)
{
    if (Cdb[0] == SCSIOP_READ12 || Cdb[0] == SCSIOP_WRITE12) {
        PUTBE32(Cdb + 6, N);
    } else {
        PUTBE16(Cdb + 7, (USHORT)N);
    }
}

/*
 * READ / WRITE (10 or 12). Requests larger than what the device takes in
 * one command are split; on UAS devices with streams they run in the
 * background. Returns FALSE when the request completes later.
 */
static BOOLEAN SlotReadWrite(SCSI_REQ *r, SCSI_SLOT *s, UCHAR *Cdb, UCHAR CdbLen)
{
    MSC_DEV *m = s->Msc;
    MSC_DATA d;
    ULONG limit = m->MaxXfer;
    ULONG blocks = RwCount(Cdb);
    ULONG lba = GETBE32(Cdb + 2);
    ULONG bs;
    ULONG doneBlocks = 0;
    ULONG doneBytes = 0;
    int st = MSC_OK;

    ReqDataDesc(r, &d);
    if (!DataSg(m, &d)) {
        d.Pfn = NULL;
        if (limit > MSC_BUF_SIZE) {
            limit = MSC_BUF_SIZE;
        }
        if (d.Va == NULL && r->DataLength != 0) {
            r->SrbStatus = SRBST_ERROR;
            return TRUE;
        }
    }
    if (m->Proto == MSC_PROTO_UAS && m->Streams != 0 && d.Pfn != NULL && r->DataLength <= limit &&
        r->Direction != SCSI_DIR_NONE && !m->Recover) {
        UAS_TAG *t;
        if ((m->WaitHead == NULL || UasFromWait) && (t = UasAllocTag(m, FALSE)) != NULL) {
            t->Req = r;
            t->Slot = s;
            t->Len = r->DataLength;
            t->Lun = s->Lun;
            t->Dir = r->Direction;
            UasBuildCommand(t, s->Lun, Cdb, CdbLen);
            UasStart(t, &d);
            return FALSE;
        }
        if (UasFromWait) {
            /* goes back to the front of the waiting list */
            r->Next = m->WaitHead;
            m->WaitHead = r;
            if (m->WaitTail == NULL) {
                m->WaitTail = r;
            }
            return FALSE;
        }
        r->Next = NULL;
        if (m->WaitTail != NULL) {
            m->WaitTail->Next = r;
        } else {
            m->WaitHead = r;
        }
        m->WaitTail = r;
        return FALSE;
    }

    if (blocks == 0 || r->DataLength == 0 || r->DataLength % blocks != 0) {
        SlotPassThrough(r, s, Cdb, CdbLen, TRUE);
        return TRUE;
    }
    bs = r->DataLength / blocks;
    while (doneBlocks < blocks) {
        UCHAR cdb[16];
        MSC_DATA dd = d;
        ULONG per = limit / bs;
        ULONG n;
        ULONG moved = 0;
        if (per == 0) {
            per = 1;
        }
        n = blocks - doneBlocks;
        if (n > per) {
            n = per;
        }
        UsbMemCpy(cdb, Cdb, 16);
        PUTBE32(cdb + 2, lba + doneBlocks);
        RwSetCount(cdb, n);
        DataAdvance(&dd, doneBlocks * bs);
        st = MscCommandD(m, s->Lun, cdb, CdbLen, r->Direction, n * bs, &dd, &moved);
        if (st == MSC_ERROR && n * bs > 0x10000 && !m->Gone && !m->Dev->Gone) {
            UsbLog(LOG_INFO, "msc %u: %u byte commands fail, using 64 KB\n", m->Dev->Index, n * bs);
            m->MaxXfer = 0x10000;
            limit = 0x10000;
            continue;
        }
        doneBytes += moved;
        if (st != MSC_OK || moved < n * bs) {
            break;
        }
        doneBlocks += n;
    }
    CompleteFromDevice(r, s, st, doneBytes, TRUE);
    if (r->SrbStatus == SRBST_SUCCESS) {
        s->MaybeMounted = 1;
    }
    return TRUE;
}

static void ModeSenseDisk(SCSI_REQ *r, SCSI_SLOT *s, BOOLEAN Ten)
{
    UCHAR hdr[8];

    UsbMemSet(hdr, 0, sizeof(hdr));
    if (Ten) {
        hdr[1] = 6;
        hdr[3] = (UCHAR)(s->WriteProtect ? 0x80 : 0);
        ReqData(r, hdr, 8);
    } else {
        hdr[0] = 3;
        hdr[2] = (UCHAR)(s->WriteProtect ? 0x80 : 0);
        ReqData(r, hdr, 4);
    }
}

/* MODE SENSE(6) for CD style devices, sent as MODE SENSE(10) */
static void ModeSense6As10(SCSI_REQ *r, SCSI_SLOT *s)
{
    MSC_DEV *m = s->Msc;
    UCHAR cdb[10];
    ULONG want = r->DataLength + 4;
    ULONG done = 0;
    int st;

    if (want > 255 + 4) {
        want = 255 + 4;
    }
    UsbMemSet(cdb, 0, sizeof(cdb));
    cdb[0] = SCSIOP_MODE_SENSE10;
    cdb[1] = r->Cdb[1] & 0x08;
    cdb[2] = r->Cdb[2];
    PUTBE16(cdb + 7, (USHORT)want);
    st = MscCommand(m, s->Lun, cdb, 10, SCSI_DIR_IN, want, &done);
    if (st == MSC_OK && done >= 8) {
        UCHAR out[260];
        ULONG bdl = GETBE16(m->Buf + 6);
        ULONG len = GETBE16(m->Buf) + 2;
        ULONG n;
        if (len > done) {
            len = done;
        }
        n = len - 8;
        if (n > sizeof(out) - 4) {
            n = sizeof(out) - 4;
        }
        out[0] = (UCHAR)(n + 3);
        out[1] = m->Buf[2];
        out[2] = m->Buf[3];
        out[3] = (UCHAR)(bdl > 255 ? 0 : bdl);
        UsbMemCpy(out + 4, m->Buf + 8, n);
        ReqData(r, out, n + 4);
        return;
    }
    CompleteFromDevice(r, s, st, 0, FALSE);
}

/* ------------------------------------------------------------------ */
/* Partitions                                                           */
/*                                                                      */
/* The disk drivers of NT treat a removable medium as one volume:       */
/*                                                                      */
/* - Windows 2000 / XP show only its first partition.  A medium with    */
/*   several partitions (RemovableAsFixed 1, also a device that is a    */
/*   fixed disk itself) or every medium (2) is reported as fixed; the   */
/*   bus side (wdmbus.c) makes it a new Plug and Play disk for every    */
/*   medium, and a fixed reported slot of a device with removable media */
/*   (card reader) is polled, so that a medium that goes away or is     */
/*   swapped is noticed.                                                */
/* - NT 3.x / 4.0 create the partition devices of a disk once, when the */
/*   disk driver starts: as many as the medium in the slot has then,    */
/*   one for an empty slot.  A removable disk keeps them and maps the   */
/*   n-th one to the n-th partition of whatever medium is in the slot.  */
/*   So until the disk driver has read the partition table of a slot,   */
/*   the slot holds a placeholder medium with MaxPartitions partitions  */
/*   (the real medium, if any, appears right after that); usbmon gives  */
/*   the partitions drive letters.  Reporting a slot as fixed would not */
/*   do on these versions: a fixed disk's partitions and size are read  */
/*   only once (never again for another medium, and NT 3.1 cannot make  */
/*   partition devices later at all).                                   */
/* ------------------------------------------------------------------ */

#define PH_STEP     2048        /* placeholder: partition k at block PH_STEP * k, 1024 blocks */
#define PH_TIMEOUT  60000       /* placeholder at most this long after the disk driver started */

static BOOLEAN IsContainer(UCHAR t)
{
    return (BOOLEAN)(t == 0x05 || t == 0x0F || t == 0x85);
}

/* Count blocks from Lba into m->Buf */
static BOOLEAN ReadBlocks(MSC_DEV *m, UCHAR Lun, ULONG Lba, ULONG Count, ULONG Bs)
{
    UCHAR cdb[10];
    ULONG done = 0;
    UCHAR sense[18];
    int st;

    UsbMemSet(cdb, 0, sizeof(cdb));
    cdb[0] = SCSIOP_READ10;
    PUTBE32(cdb + 2, Lba);
    PUTBE16(cdb + 7, (USHORT)Count);
    st = MscCommand(m, Lun, cdb, 10, SCSI_DIR_IN, Count * Bs, &done);
    if (st == MSC_FAILED) {
        MscRequestSense(m, Lun, sense);
    }
    return (BOOLEAN)(st == MSC_OK && done == Count * Bs);
}

/* an MBR or EBR: the entries look like a partition table (an unpartitioned medium
   has the boot code of its file system there) */
static BOOLEAN TableOk(const UCHAR *b, ULONG Blocks)
{
    int i;

    if (b[510] != 0x55 || b[511] != 0xAA) {
        return FALSE;
    }
    for (i = 0; i < 4; i++) {
        const UCHAR *e = b + 446 + 16 * i;
        if (e[0] != 0x00 && e[0] != 0x80) {
            return FALSE;
        }
        if (e[4] != 0 && (GET32(e + 8) == 0 || GET32(e + 12) == 0 || GET32(e + 8) >= Blocks)) {
            return FALSE;
        }
    }
    return TRUE;
}

/* GPT: the used entries of the partition array (1 if the header is not valid) */
static ULONG GptParts(PART_READ Read, void *Ctx, UCHAR *B, ULONG BufSize, ULONG Bs, ULONG Blocks)
{
    static const UCHAR sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    ULONG lba, cnt, esz, per = BufSize / Bs, done = 0, n = 0;

    if (!Read(Ctx, 1, 1, B) || UsbMemCmp(B, sig, 8) != 0 || GET32(B + 76) != 0) {
        return 1;
    }
    lba = GET32(B + 72);
    cnt = GET32(B + 80);
    esz = GET32(B + 84);
    if (esz < 128 || esz > 1024 || (esz & (esz - 1)) != 0 || cnt == 0 || cnt > 1024 || lba < 2 || lba >= Blocks ||
        per == 0) {
        return 1;
    }
    while (done < cnt) {
        ULONG blocks = ((cnt - done) * esz + Bs - 1) / Bs, i, k;
        if (blocks > per) {
            blocks = per;
        }
        if (!Read(Ctx, lba, blocks, B)) {
            break;
        }
        for (i = 0; i < blocks * Bs / esz && done < cnt; i++, done++) {
            for (k = 0; k < 16 && B[i * esz + k] == 0; k++) {
            }
            n += (k < 16);
        }
        lba += blocks;
    }
    return n;
}

/* sector 0 is an MBR with a disk signature (or a GPT disk's protective MBR, which needs none):
   Windows 2000 / XP mount no volume of a fixed disk without one */
BOOLEAN UsbSignedDisk(PART_READ Read, void *Ctx, UCHAR *Buf)
{
    ULONG i;

    if (!Read(Ctx, 0, 1, Buf) || Buf[510] != 0x55 || Buf[511] != 0xAA) {
        return FALSE;
    }
    for (i = 0; i < 4; i++) {
        if (Buf[446 + 16 * i + 4] == 0xEE) {
            return TRUE;
        }
    }
    return (BOOLEAN)((Buf[440] | Buf[441] | Buf[442] | Buf[443]) != 0);
}

/* partitions of a medium: MBR primary and logical ones, or GPT; 0 for a medium without a
   partition table.  Buf: BufSize bytes (at least 4 blocks) for the reads. */
UCHAR UsbCountPartitions(PART_READ Read, void *Ctx, UCHAR *Buf, ULONG BufSize, ULONG Bs, ULONG Blocks)
{
    UCHAR *b = Buf;
    ULONG ext = 0, n = 0, cur, i, guard;

    if (Bs < 512 || Bs > 4096 || Blocks < 2 || BufSize < Bs || !Read(Ctx, 0, 1, b) || !TableOk(b, Blocks)) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        if (b[446 + 16 * i + 4] == 0xEE) {
            n = GptParts(Read, Ctx, Buf, BufSize, Bs, Blocks);
            return (UCHAR)(n > 255 ? 255 : n);
        }
    }
    for (i = 0; i < 4; i++) {
        const UCHAR *e = b + 446 + 16 * i;
        if (IsContainer(e[4])) {
            if (ext == 0) {
                ext = GET32(e + 8);
            }
        } else if (e[4] != 0) {
            n++;
        }
    }
    /* the chain of extended boot records: one logical partition and the link to the next each */
    for (cur = ext, guard = 0; cur != 0 && guard < 128; guard++) {
        ULONG next = 0;
        if (cur >= Blocks || !Read(Ctx, cur, 1, b) || !TableOk(b, Blocks)) {
            break;
        }
        for (i = 0; i < 4; i++) {
            const UCHAR *e = b + 446 + 16 * i;
            if (IsContainer(e[4])) {
                if (next == 0) {
                    next = ext + GET32(e + 8);
                }
            } else if (e[4] != 0) {
                n++;
            }
        }
        cur = (next == cur) ? 0 : next;
    }
    return (UCHAR)(n > 255 ? 255 : n);
}

typedef struct _SLOT_READ {
    MSC_DEV    *M;
    UCHAR       Lun;
    ULONG       Bs;
} SLOT_READ;

static BOOLEAN SlotRead(void *Ctx, ULONG Lba, ULONG Count, UCHAR *Buf)
{
    SLOT_READ *r = (SLOT_READ *)Ctx;

    (void)Buf;                                  /* = m->Buf, where the reads land */
    return ReadBlocks(r->M, r->Lun, Lba, Count, r->Bs);
}

/* worker: the medium of the slot is (or may be) new - its partitions, and whether the OS gets
   it as a fixed medium */
static void SlotScan(SCSI_SLOT *s)
{
    MSC_DEV *m = s->Msc;
    UCHAR fixed = 0;

    s->Rescan = 0;
    s->Parts = 0;
    if (m != NULL && !m->Gone && IsDiskType(s->Type) && s->MediaReady && s->BlockSize != 0) {
        SLOT_READ rd;
        rd.M = m;
        rd.Lun = s->Lun;
        rd.Bs = s->BlockSize;
        s->Parts = UsbCountPartitions(SlotRead, &rd, m->Buf, MSC_BUF_SIZE, s->BlockSize, s->LastLba + 1);
        fixed = (UCHAR)(UsbScsiFixedMode >= 2 || (UsbScsiFixedMode == 1 && (s->Parts > 1 || !s->DevRmb)));
        if (fixed && UsbScsiFixedMode == 1 && !UsbSignedDisk(SlotRead, &rd, m->Buf)) {
            /* Windows 2000 / XP mount no volume of a fixed disk without a disk signature (it waits
               for Disk Management to write one): such a disk stays removable (first partition) */
            UsbLog(LOG_INFO, "slot %d: no disk signature in the MBR, stays a removable medium\n", (int)(s - Slots));
            fixed = 0;
        }
    }
    if (fixed != s->Fixed || s->Parts > 1) {
        UsbLog(LOG_INFO, "slot %d: %u partition(s), %s medium\n", (int)(s - Slots), s->Parts,
               fixed ? "reported as a fixed" : "a removable");
    }
    s->Fixed = fixed;
}

/* NT 3.x / 4.0 (ntscsi.c), before the disk driver starts: every slot holds a placeholder medium
   with Parts partitions until the disk driver has read its partition table */
void UsbScsiPlaceholder(ULONG Parts)
{
    ULONG i;

    if (Parts < 2) {
        return;
    }
    PhParts = Parts > 24 ? 24 : Parts;
    for (i = 0; i < SCSI_MAX_SLOTS; i++) {
        Slots[i].Ph = 1;
    }
}

/* partitions 1 .. PhPrimary() are primary, the others logical (always at least one: the
   placeholder ends when the last extended boot record has been read) */
static ULONG PhPrimary(void)
{
    return PhParts - 1 < 3 ? PhParts - 1 : 3;
}

static ULONG PhEbr(ULONG k)
{
    return PH_STEP * k - 32;
}

static void PhEntry(UCHAR *e, UCHAR Type, ULONG Start, ULONG Count)
{
    ULONG end = Start + Count - 1;

    e[0] = 0;
    e[1] = (UCHAR)((Start / 32) % 64);          /* CHS with 64 heads, 32 sectors */
    e[2] = (UCHAR)(((Start / 2048) >> 2 & 0xC0) | (Start % 32 + 1));
    e[3] = (UCHAR)(Start / 2048);
    e[4] = Type;
    e[5] = (UCHAR)((end / 32) % 64);
    e[6] = (UCHAR)(((end / 2048) >> 2 & 0xC0) | (end % 32 + 1));
    e[7] = (UCHAR)(end / 2048);
    e[8] = (UCHAR)Start;
    e[9] = (UCHAR)(Start >> 8);
    e[10] = (UCHAR)(Start >> 16);
    e[11] = (UCHAR)(Start >> 24);
    e[12] = (UCHAR)Count;
    e[13] = (UCHAR)(Count >> 8);
    e[14] = (UCHAR)(Count >> 16);
    e[15] = (UCHAR)(Count >> 24);
}

/* one 512 byte block of the placeholder; returns 1 for a partition table, 2 for the last one */
static int PhBlock(ULONG Lba, UCHAR *B)
{
    ULONG p = PhPrimary(), ext = PhEbr(p + 1), k;

    UsbMemSet(B, 0, 512);
    if (Lba == 0) {
        for (k = 1; k <= p; k++) {
            PhEntry(B + 446 + 16 * (k - 1), 0x06, PH_STEP * k, 1024);
        }
        PhEntry(B + 446 + 16 * p, 0x05, ext, PH_STEP * (PhParts + 1) - ext);
        B[510] = 0x55;
        B[511] = 0xAA;
        return 1;
    }
    for (k = p + 1; k <= PhParts; k++) {
        if (Lba == PhEbr(k)) {
            PhEntry(B + 446, 0x06, 32, 1024);
            if (k < PhParts) {
                PhEntry(B + 462, 0x05, PhEbr(k + 1) - ext, PH_STEP);
            }
            B[510] = 0x55;
            B[511] = 0xAA;
            return k == PhParts ? 2 : 1;
        }
    }
    return 0;
}

static void PhEnd(SCSI_SLOT *s, const char *Why)
{
    s->Ph = 0;
    if (Why == NULL) {
        return;                                 /* not a disk slot */
    }
    if (s->Present && !s->Ejected) {
        s->UnitAttention = 1;                   /* the real medium comes in */
    }
    UsbLog(LOG_INFO, "slot %d: placeholder done (%s), %u partition devices\n", (int)(s - Slots), Why, PhParts);
}

static BOOLEAN PhExecute(SCSI_REQ *r, SCSI_SLOT *s)
{
    UCHAR op = r->Cdb[0];

    switch (op) {
    case SCSIOP_TEST_UNIT_READY:
    case SCSIOP_START_STOP:
    case SCSIOP_MEDIUM_REMOVAL:
    case SCSIOP_SYNC_CACHE:
    case SCSIOP_VERIFY10:
        r->ScsiStatus = SCSIST_GOOD;
        r->SrbStatus = SRBST_SUCCESS;
        return TRUE;
    case SCSIOP_READ_CAPACITY: {
        UCHAR cap[8];
        PUTBE32(cap, PH_STEP * (PhParts + 1) - 1);
        PUTBE32(cap + 4, 512);
        ReqData(r, cap, 8);
        return TRUE;
    }
    case SCSIOP_MODE_SENSE6:
    case SCSIOP_MODE_SENSE10: {
        UCHAR hdr[8];
        UsbMemSet(hdr, 0, sizeof(hdr));
        if (op == SCSIOP_MODE_SENSE10) {
            hdr[1] = 6;
            hdr[3] = 0x80;                      /* write protected */
            ReqData(r, hdr, 8);
        } else {
            hdr[0] = 3;
            hdr[2] = 0x80;
            ReqData(r, hdr, 4);
        }
        return TRUE;
    }
    case SCSIOP_READ6:
    case SCSIOP_READ10:
    case SCSIOP_READ12: {
        ULONG lba, cnt, i;
        int kind, last = 0, data = 0;
        if (op == SCSIOP_READ6) {
            lba = ((ULONG)(r->Cdb[1] & 0x1F) << 16) | ((ULONG)r->Cdb[2] << 8) | r->Cdb[3];
            cnt = r->Cdb[4] ? r->Cdb[4] : 256;
        } else {
            lba = GETBE32(r->Cdb + 2);
            cnt = RwCount(r->Cdb);
        }
        if (r->Data == NULL || r->DataLength < cnt * 512) {
            ReqCheck(r, s, SENSE_ILLEGAL_REQUEST, 0x24, 0);
            return TRUE;
        }
        for (i = 0; i < cnt; i++) {
            kind = PhBlock(lba + i, (UCHAR *)r->Data + 512 * i);
            last |= (kind == 2);
            data |= (kind == 0);
        }
        r->Transferred = cnt * 512;
        r->ScsiStatus = SCSIST_GOOD;
        r->SrbStatus = SRBST_SUCCESS;
        if (last || data) {
            PhEnd(s, last ? "partition table read" : "data read");
        }
        return TRUE;
    }
    case SCSIOP_WRITE6:
    case SCSIOP_WRITE10:
    case SCSIOP_WRITE12:
        PhEnd(s, "write");
        ReqCheck(r, s, SENSE_DATA_PROTECT, 0x27, 0);
        return TRUE;
    default:
        ReqCheck(r, s, SENSE_ILLEGAL_REQUEST, 0x20, 0);
        return TRUE;
    }
}

/* Returns FALSE when the request completes later */
static BOOLEAN ExecuteOnSlot(SCSI_REQ *r, SCSI_SLOT *s)
{
    UCHAR op = r->Cdb[0];
    UCHAR cdb[16];
    int st;

    switch (op) {
    case SCSIOP_INQUIRY:
        if (r->Cdb[1] & 1) {
            ReqCheck(r, s, SENSE_ILLEGAL_REQUEST, 0x24, 0);
        } else {
            UCHAR inq[36];
            ULONG alloc = r->Cdb[4];
            SlotInquiry(s, inq);
            if (alloc == 0 || alloc > 36) {
                alloc = 36;
            }
            ReqData(r, inq, alloc);
        }
        return TRUE;
    case SCSIOP_REQUEST_SENSE:
        if (s->SenseValid) {
            ReqData(r, s->Sense, r->Cdb[4] ? r->Cdb[4] : 18);
            s->SenseValid = 0;
        } else {
            UCHAR sense[18];
            if (!s->Present || s->Ejected) {
                SetSense(sense, SENSE_NOT_READY, 0x3A, 0);
            } else {
                SetSense(sense, 0, 0, 0);
            }
            ReqData(r, sense, r->Cdb[4] ? r->Cdb[4] : 18);
        }
        return TRUE;
    default:
        break;
    }

    /* the placeholder is there from the moment the class drivers asked for the slots (UsbScsiFreeze) */
    if (s->Ph && Frozen) {
        if (PhParts == 0 || !IsDiskType(s->Type)) {
            PhEnd(s, NULL);
        } else if (Frozen && OsTimeMs() - FreezeMs > PH_TIMEOUT) {
            PhEnd(s, "time");
        } else {
            return PhExecute(r, s);
        }
    }
    if (!s->Present || s->Ejected) {
        ReqCheck(r, s, SENSE_NOT_READY, 0x3A, 0);
        return TRUE;
    }
    if (s->UnitAttention) {
        s->UnitAttention = 0;
        ReqCheck(r, s, SENSE_UNIT_ATTENTION, 0x28, 0);
        return TRUE;
    }

    UsbMemSet(cdb, 0, sizeof(cdb));
    UsbMemCpy(cdb, r->Cdb, r->CdbLength > 16 ? 16 : r->CdbLength);

    switch (op) {
    case SCSIOP_READ6:
    case SCSIOP_WRITE6: {
        ULONG lba = ((ULONG)(r->Cdb[1] & 0x1F) << 16) | ((ULONG)r->Cdb[2] << 8) | r->Cdb[3];
        USHORT cnt = r->Cdb[4] ? r->Cdb[4] : 256;
        UsbMemSet(cdb, 0, sizeof(cdb));
        cdb[0] = (UCHAR)((op == SCSIOP_READ6) ? SCSIOP_READ10 : SCSIOP_WRITE10);
        PUTBE32(cdb + 2, lba);
        PUTBE16(cdb + 7, cnt);
        return SlotReadWrite(r, s, cdb, 10);
    }
    case SCSIOP_READ10:
    case SCSIOP_WRITE10:
        return SlotReadWrite(r, s, cdb, 10);
    case SCSIOP_READ12:
    case SCSIOP_WRITE12:
        return SlotReadWrite(r, s, cdb, 12);
    case SCSIOP_READ_CAPACITY:
        SlotPassThrough(r, s, cdb, 10, FALSE);
        if (r->SrbStatus == SRBST_SUCCESS && r->Transferred >= 8 && r->Data != NULL) {
            s->LastLba = GETBE32((UCHAR *)r->Data);
            s->BlockSize = GETBE32((UCHAR *)r->Data + 4);
        }
        return TRUE;
    case SCSIOP_MODE_SENSE6:
        if (IsDiskType(s->Type)) {
            ModeSenseDisk(r, s, FALSE);
        } else {
            ModeSense6As10(r, s);
        }
        return TRUE;
    case SCSIOP_MODE_SENSE10:
        if (IsDiskType(s->Type)) {
            ModeSenseDisk(r, s, TRUE);
        } else {
            SlotPassThrough(r, s, cdb, 10, FALSE);
        }
        return TRUE;
    case SCSIOP_MODE_SELECT6:
        if (IsDiskType(s->Type)) {
            r->Transferred = r->DataLength;
            r->ScsiStatus = SCSIST_GOOD;
            r->SrbStatus = SRBST_SUCCESS;
        } else {
            SlotPassThrough(r, s, cdb, 6, FALSE);
        }
        return TRUE;
    case SCSIOP_START_STOP:
    case SCSIOP_MEDIUM_REMOVAL:
    case SCSIOP_SYNC_CACHE:
    case SCSIOP_VERIFY10:
        st = SlotPassThrough(r, s, cdb, r->CdbLength, FALSE);
        if (st == MSC_FAILED && (r->Sense[2] & 0x0F) == SENSE_ILLEGAL_REQUEST) {
            r->ScsiStatus = SCSIST_GOOD;
            r->SrbStatus = SRBST_SUCCESS;
            r->SenseLength = 0;
            s->SenseValid = 0;
        }
        return TRUE;
    default:
        SlotPassThrough(r, s, cdb, r->CdbLength, FALSE);
        return TRUE;
    }
}

BOOLEAN UsbScsiExecute(SCSI_REQ *r)
{
    SCSI_SLOT *s;
    BOOLEAN done;

    r->Transferred = 0;
    r->SenseLength = 0;
    r->ScsiStatus = SCSIST_GOOD;
    if (r->Target >= UsbScsiSlotCount || r->Lun != 0) {
        r->SrbStatus = SRBST_SELECTION_TIMEOUT;
        return TRUE;
    }
    s = &Slots[r->Target];
    if (r->Internal == 1) {
        UsbScsiEject(r->Target);
        r->SrbStatus = SRBST_SUCCESS;
        return TRUE;
    }
    done = ExecuteOnSlot(r, s);
    if (done) {
        UsbLog(LOG_TRACE, "scsi t%u op %02x len %u -> srb %02x scsi %02x sense %x/%02x xfer %u\n", r->Target,
               r->Cdb[0], r->DataLength, r->SrbStatus, r->ScsiStatus,
               (r->SrbStatus & SRBST_AUTOSENSE_VALID) ? (r->Sense[2] & 0xF) : 0,
               (r->SrbStatus & SRBST_AUTOSENSE_VALID) ? r->Sense[12] : 0, r->Transferred);
    }
    return done;
}

void UsbScsiQueue(SCSI_REQ *Req)
{
    OS_IRQL irql;

    Req->Next = NULL;
    irql = OsLock();
    if (QueueTail != NULL) {
        QueueTail->Next = Req;
    } else {
        QueueHead = Req;
    }
    QueueTail = Req;
    OsUnlock(irql);
    OsEventSet(&MscKick);
    OsWakeWorker();
}

/* Restarts waiting UAS requests, recovers stuck devices */
static void UasService(void)
{
    int i;
    int k;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        MSC_DEV *m = (MSC_DEV *)UsbDevs[i].Msc;
        if (!UsbDevs[i].InUse || m == NULL || m->Proto != MSC_PROTO_UAS || m->Gone) {
            continue;
        }
        for (k = 0; k < m->NumTags && !m->Recover; k++) {
            UAS_TAG *t = &m->Tags[k];
            if (t->Busy && !t->Sync && OsTimeMs() - t->Start > UAS_TIMEOUT_MS) {
                UsbLog(LOG_ERR, "msc %u: UAS tag %u timed out\n", m->Dev->Index, t->Tag);
                m->Recover = 1;
            }
        }
        if (m->Recover) {
            UasRecover(m);
        }
        while (m->WaitHead != NULL && !m->Recover) {
            SCSI_REQ *r = m->WaitHead;
            SCSI_SLOT *s = &Slots[r->Target];
            BOOLEAN done;
            m->WaitHead = r->Next;
            if (m->WaitHead == NULL) {
                m->WaitTail = NULL;
            }
            if (s->Msc != m) {
                r->SrbStatus = SRBST_SELECTION_TIMEOUT;
                if (r->Complete != NULL) {
                    r->Complete(r);
                }
                continue;
            }
            UasFromWait = TRUE;
            done = ExecuteOnSlot(r, s);
            UasFromWait = FALSE;
            if (done) {
                if (r->Complete != NULL) {
                    r->Complete(r);
                }
            } else if (m->WaitHead == r) {
                break;              /* no free tag yet */
            }
        }
    }
}

#define MEDIA_POLL_MS   1000

/* READ CAPACITY of a slot's medium (worker, nothing else running on the device) */
static void SlotCapacity(SCSI_SLOT *s)
{
    MSC_DEV *m = s->Msc;
    UCHAR cdb[10], sense[18];
    ULONG done = 0;
    int st;

    UsbMemSet(cdb, 0, sizeof(cdb));
    cdb[0] = SCSIOP_READ_CAPACITY;
    st = MscCommand(m, s->Lun, cdb, 10, SCSI_DIR_IN, 8, &done);
    if (st == MSC_OK && done >= 8) {
        s->LastLba = GETBE32(m->Buf);
        s->BlockSize = GETBE32(m->Buf + 4);
        s->MediaReady = 1;
        return;
    }
    if (st == MSC_FAILED && MscRequestSense(m, s->Lun, sense) == MSC_OK && (sense[2] & 0x0F) == SENSE_UNIT_ATTENTION) {
        st = MscCommand(m, s->Lun, cdb, 10, SCSI_DIR_IN, 8, &done);      /* once more after the attention */
        if (st == MSC_OK && done >= 8) {
            s->LastLba = GETBE32(m->Buf);
            s->BlockSize = GETBE32(m->Buf + 4);
            s->MediaReady = 1;
            return;
        }
        if (st == MSC_FAILED) {
            MscRequestSense(m, s->Lun, sense);
        }
    }
    s->MediaReady = 0;
    s->LastLba = 0;
    s->BlockSize = 0;
}

/* Windows 2000 / XP: TEST UNIT READY on a fixed reported slot of a device with removable media
   (nobody else asks a fixed disk whether its medium is still there), and on an ejected one, which
   comes back with the next medium put in */
static void SlotPoll(SCSI_SLOT *s)
{
    MSC_DEV *m = s->Msc;
    UCHAR cdb[6], sense[18];
    ULONG done = 0;
    int st;

    UsbMemSet(cdb, 0, sizeof(cdb));
    st = MscCommand(m, s->Lun, cdb, 6, SCSI_DIR_NONE, 0, &done);
    if (st == MSC_OK) {
        if (s->Ejected && s->PollGone) {
            UsbLog(LOG_INFO, "slot %d: new medium after the removal\n", (int)(s - Slots));
            s->Ejected = 0;
            s->PollGone = 0;
            s->MediaReady = 0;
            s->Rescan = 1;
        } else if (!s->Ejected && !s->MediaReady) {
            s->Rescan = 1;
        }
        return;
    }
    if (st != MSC_FAILED || MscRequestSense(m, s->Lun, sense) != MSC_OK) {
        return;
    }
    if ((sense[2] & 0x0F) == SENSE_NOT_READY && sense[12] == 0x3A) {
        if (s->Ejected) {
            s->PollGone = 1;
        } else if (s->MediaReady || s->Fixed) {
            s->MediaReady = 0;
            s->Rescan = 1;
        }
    } else if ((sense[2] & 0x0F) == SENSE_UNIT_ATTENTION && sense[12] == 0x28) {
        if (s->Ejected) {
            s->PollGone = 1;
        } else {
            s->Rescan = 1;
        }
    }
}

/* worker, nothing queued: slots whose medium may be new are looked at again (a fixed reported
   medium that changed is a new disk to Windows 2000 / XP) */
static void MscMediaService(void)
{
    ULONG i, now = OsTimeMs();

    for (i = 0; i < UsbScsiSlotCount; i++) {
        SCSI_SLOT *s = &Slots[i];
        MSC_DEV *m = s->Msc;
        UCHAR fixed;
        if (!s->Present || m == NULL || m->Gone || m->Dev->Gone || !IsDiskType(s->Type) || m->WaitHead != NULL ||
            (s->Ph && Frozen)) {
            continue;
        }
        if (UsbScsiFixedMode != 0 && s->DevRmb && m->Proto != MSC_PROTO_VIRT && (s->Fixed || s->Ejected) &&
            now - s->PollMs >= MEDIA_POLL_MS) {
            s->PollMs = now;
            SlotPoll(s);
        }
        if (!s->Rescan || s->Ejected) {
            continue;
        }
        fixed = s->Fixed;
        SlotCapacity(s);
        s->MediaGen++;
        s->SenseValid = 0;
        SlotScan(s);
        s->PollMs = now;
        if (s->Fixed != fixed) {
            OsScsiMediaChange((int)i);
        }
        UsbNotifyChange();
        UsbLog(LOG_INFO, "slot %u: medium %s\n", i, s->MediaReady ? "in" : "out");
    }
}

void MscService(void)
{
    int rounds = 0;

    for (;;) {
        UasService();
        for (;;) {
            OS_IRQL irql = OsLock();
            SCSI_REQ *r = QueueHead;
            if (r != NULL) {
                QueueHead = r->Next;
                if (QueueHead == NULL) {
                    QueueTail = NULL;
                }
            }
            OsUnlock(irql);
            if (r == NULL) {
                break;
            }
            if (UsbScsiExecute(r) && r->Complete != NULL) {
                r->Complete(r);
            }
        }
        /* poll while UAS commands run so that they finish without waiting for a timer tick */
        if (UasBusy == 0 || rounds++ > 200) {
            break;
        }
        OsEventReset(&MscKick);
        if (QueueHead != NULL) {
            continue;
        }
        if (!OsEventWait(&MscKick, 20)) {
            break;
        }
    }
    if (UasBusy == 0 && QueueHead == NULL) {
        MscMediaService();
    }
}

void UsbScsiInit(void)
{
    ULONG n = OsGetConfig("MaxDisks", 4);

    if (n < 1) {
        n = 1;
    }
    if (n > SCSI_MAX_SLOTS) {
        n = SCSI_MAX_SLOTS;
    }
    UsbScsiSlotCount = n;
    OsEventInit(&MscKick);
}

void UsbScsiFreeze(void)
{
    if (!Frozen) {
        FreezeMs = OsTimeMs();
    }
    Frozen = 1;
}

void UsbScsiGetSlot(int Slot, SCSI_SLOT_INFO *Info)
{
    SCSI_SLOT *s = &Slots[Slot];
    MSC_DEV *m = s->Msc;

    UsbMemSet(Info, 0, sizeof(*Info));
    Info->Present = (UCHAR)(s->Present && !s->Ejected);
    Info->DeviceType = s->Type;
    SlotInquiry(s, Info->Inquiry);
    Info->DevIndex = 0xFF;
    if (s->Present && m != NULL) {
        Info->DevIndex = m->Dev->Index;
        Info->Lun = s->Lun;
        Info->Protocol = m->Proto;
        Info->MediaReady = s->MediaReady;
        Info->WriteProtect = s->WriteProtect;
        Info->LastLba = s->LastLba;
        Info->BlockSize = s->BlockSize;
        Info->MaxXfer = m->MaxXfer;
        Info->Identity = s->Identity;
        Info->Fixed = s->Fixed;
        Info->Parts = s->Parts;
        Info->MediaGen = s->MediaGen;
    }
}

int UsbScsiSlotPresent(int Slot)
{
    if (Slot < 0 || (ULONG)Slot >= UsbScsiSlotCount) {
        return 0;
    }
    return Slots[Slot].Present && !Slots[Slot].Ejected;
}

void UsbScsiEject(int Slot)
{
    SCSI_SLOT *s;
    UCHAR cdb[10];
    ULONG done;

    if (Slot < 0 || (ULONG)Slot >= UsbScsiSlotCount) {
        return;
    }
    s = &Slots[Slot];
    if (!s->Present || s->Ejected) {
        return;
    }
    UsbMemSet(cdb, 0, sizeof(cdb));
    cdb[0] = SCSIOP_SYNC_CACHE;
    MscCommand(s->Msc, s->Lun, cdb, 10, SCSI_DIR_NONE, 0, &done);
    if (s->Msc->Proto == MSC_PROTO_VIRT) {
        /* card readers power the card down, NVMe drives shut down */
        UsbMemSet(cdb, 0, sizeof(cdb));
        cdb[0] = 0x1B;
        cdb[4] = 0x02;
        MscCommand(s->Msc, s->Lun, cdb, 6, SCSI_DIR_NONE, 0, &done);
    }
    s->Ejected = 1;
    s->UnitAttention = 0;
    s->MaybeMounted = 0;
    OsScsiMediaChange(Slot);
    UsbNotifyChange();
    UsbLog(LOG_INFO, "slot %d: ready for removal\n", Slot);
}

/* ------------------------------------------------------------------ */
/* Attach / detach                                                      */
/* ------------------------------------------------------------------ */

/* Spin up a LUN and place it into a slot */
static void MscAttachLun(MSC_DEV *m, UCHAR Lun)
{
    UCHAR cdb[12];
    UCHAR inq[36];
    UCHAR sense[18];
    ULONG done = 0;
    int st;
    int i;
    ULONG start;
    SCSI_SLOT *s = NULL;
    ULONG id = m->Identity ^ ((ULONG)Lun << 28);
    UCHAR ready = 0;

    UsbMemSet(cdb, 0, sizeof(cdb));
    cdb[0] = SCSIOP_INQUIRY;
    cdb[4] = 36;
    st = MSC_ERROR;
    for (i = 0; i < 3 && st != MSC_OK; i++) {
        st = MscCommand(m, Lun, cdb, 6, SCSI_DIR_IN, 36, &done);
        if (st == MSC_NODEV) {
            return;
        }
        if (st == MSC_FAILED) {
            MscRequestSense(m, Lun, sense);
        }
    }
    if (st != MSC_OK || done < 5) {
        UsbLog(LOG_ERR, "msc %u lun %u: inquiry failed\n", m->Dev->Index, Lun);
        return;
    }
    UsbMemSet(inq, ' ', sizeof(inq));
    UsbMemCpy(inq, m->Buf, done > 36 ? 36 : done);
    if ((inq[0] & 0xE0) != 0 || (inq[0] & 0x1F) == 0x1F) {
        return;
    }

    /* wait up to 5 s for the unit to become ready; card readers without media stay not ready */
    start = OsTimeMs();
    for (;;) {
        UsbMemSet(cdb, 0, sizeof(cdb));
        st = MscCommand(m, Lun, cdb, 6, SCSI_DIR_NONE, 0, &done);
        if (st == MSC_OK) {
            ready = 1;
            break;
        }
        if (st == MSC_NODEV) {
            break;
        }
        if (st == MSC_FAILED) {
            if (MscRequestSense(m, Lun, sense) == MSC_OK && (sense[2] & 0x0F) == SENSE_NOT_READY &&
                (sense[12] == 0x3A)) {
                break;
            }
        }
        if (OsTimeMs() - start > 5000) {
            break;
        }
        OsSleepMs(100);
    }
    if (st == MSC_NODEV) {
        return;
    }

    for (i = 0; i < (int)UsbScsiSlotCount; i++) {
        if (!Slots[i].Present && Slots[i].Used && Slots[i].Identity == id) {
            s = &Slots[i];
            break;
        }
    }
    if (s == NULL) {
        UCHAR want = (UCHAR)(inq[0] & 0x1F);
        for (i = 0; i < (int)UsbScsiSlotCount; i++) {
            SCSI_SLOT *c = &Slots[i];
            if (c->Present) {
                continue;
            }
            if (Frozen && c->TypeFixed && (IsDiskType(c->Type) != IsDiskType(want) || (!IsDiskType(want) && c->Type != want))) {
                continue;
            }
            if (Frozen && !c->TypeFixed && !IsDiskType(want)) {
                continue;
            }
            s = c;
            break;
        }
    }
    if (s == NULL) {
        UsbLog(LOG_ERR, "msc %u lun %u: no free disk slot (MaxDisks=%u)\n", m->Dev->Index, Lun, UsbScsiSlotCount);
        return;
    }

    s->Msc = m;
    s->Lun = Lun;
    s->Identity = id;
    s->Used = 1;
    s->Ejected = 0;
    s->WriteProtect = 0;
    s->SenseValid = 0;
    s->MediaReady = ready;
    s->LastLba = 0;
    s->BlockSize = 0;
    UsbMemCpy(s->Inquiry, inq, 36);
    s->DevRmb = (UCHAR)(inq[1] & 0x80);
    s->Inquiry[1] |= 0x80;
    if (!s->TypeFixed) {
        s->Type = IsDiskType((UCHAR)(inq[0] & 0x1F)) ? 0 : (UCHAR)(inq[0] & 0x1F);
        if (Frozen) {
            s->TypeFixed = 1;
        }
    }
    s->Inquiry[0] = s->Type;

    if (IsDiskType(s->Type)) {
        UsbMemSet(cdb, 0, sizeof(cdb));
        cdb[0] = SCSIOP_MODE_SENSE10;
        cdb[2] = 0x3F;
        PUTBE16(cdb + 7, 192);
        st = MscCommand(m, Lun, cdb, 10, SCSI_DIR_IN, 192, &done);
        if (st == MSC_OK && done >= 4) {
            s->WriteProtect = (UCHAR)((m->Buf[3] & 0x80) ? 1 : 0);
        } else if (st == MSC_FAILED) {
            MscRequestSense(m, Lun, sense);
        }
    }
    if (ready) {
        UsbMemSet(cdb, 0, sizeof(cdb));
        cdb[0] = SCSIOP_READ_CAPACITY;
        st = MscCommand(m, Lun, cdb, 10, SCSI_DIR_IN, 8, &done);
        if (st == MSC_OK && done >= 8) {
            s->LastLba = GETBE32(m->Buf);
            s->BlockSize = GETBE32(m->Buf + 4);
        } else if (st == MSC_FAILED) {
            MscRequestSense(m, Lun, sense);
        }
    }

    s->MediaGen++;
    s->PollGone = 0;
    s->Fixed = 0;
    SlotScan(s);
    s->PollMs = OsTimeMs();

    /* a media change only matters if a file system may still have the old media mounted */
    s->UnitAttention = (UCHAR)(Frozen && s->MaybeMounted);
    s->MaybeMounted = 0;
    s->Present = 1;
    if (Frozen) {
        OsScsiMediaChange((int)(s - Slots));
    }
    UsbNotifyChange();
    UsbLog(LOG_INFO, "msc %u lun %u: slot %u, type %u, '%c%c%c%c%c%c%c%c %c%c%c%c%c%c%c%c%c%c%c%c%c%c%c%c'%s\n",
           m->Dev->Index, Lun, (ULONG)(s - Slots), s->Type,
           inq[8], inq[9], inq[10], inq[11], inq[12], inq[13], inq[14], inq[15],
           inq[16], inq[17], inq[18], inq[19], inq[20], inq[21], inq[22], inq[23],
           inq[24], inq[25], inq[26], inq[27], inq[28], inq[29], inq[30], inq[31],
           s->WriteProtect ? " write protected" : "");
}

static ULONG ReadIdentity(USB_DEV *Dev)
{
    ULONG h = ((ULONG)GET16(Dev->DevDesc + 8) << 16) ^ GET16(Dev->DevDesc + 10) ^ ((ULONG)GET16(Dev->DevDesc + 12) << 8);
    UCHAR buf[64];
    ULONG got = 0;
    ULONG i;

    if (Dev->DevDesc[16] != 0 &&
        UsbGetDescriptor(Dev, USB_DT_STRING, Dev->DevDesc[16], 0x0409, buf, sizeof(buf), &got) == USB_OK) {
        for (i = 2; i < got; i++) {
            h = (h << 5) ^ (h >> 27) ^ buf[i];
        }
    }
    return h & 0x0FFFFFFF;
}

/* Finds an alternate setting of the same interface using the UAS protocol */
static const UCHAR *FindUasAlt(const UCHAR *If, const UCHAR *End)
{
    const UCHAR *d = If;

    if (If[7] == MSC_PROTO_UAS) {
        return If;
    }
    while ((d = UsbNextDesc(d, End)) != NULL) {
        if (d[1] != USB_DT_INTERFACE || d[0] < 9) {
            continue;
        }
        if (d[2] != If[2]) {
            break;
        }
        if (d[5] == USB_CLASS_MSC && d[7] == MSC_PROTO_UAS) {
            return d;
        }
    }
    return NULL;
}

/* Endpoints of a UAS interface by their pipe usage descriptors */
static int UasParse(const UCHAR *If, const UCHAR *End, const UCHAR **Ep, UCHAR *StreamsExp)
{
    const UCHAR *d = If;
    const UCHAR *cur = NULL;

    *StreamsExp = 0;
    while ((d = UsbNextDesc(d, End)) != NULL) {
        if (d[1] == USB_DT_INTERFACE) {
            break;
        }
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7) {
            cur = d;
        } else if (d[1] == USB_DT_SS_EP_COMP && d[0] >= 6 && cur != NULL && (cur[3] & 3) == USB_EP_BULK) {
            UCHAR e = (UCHAR)(d[3] & 0x1F);
            if (e != 0 && (*StreamsExp == 0 || e < *StreamsExp)) {
                *StreamsExp = e;
            }
        } else if (d[1] == USB_DT_PIPE_USAGE && d[0] >= 4 && cur != NULL && d[2] >= 1 && d[2] <= 4) {
            Ep[d[2]] = cur;
        }
    }
    return (Ep[1] && Ep[2] && Ep[3] && Ep[4]) ? USB_OK : USB_ERR_PARAM;
}

static int UasSetup(MSC_DEV *m, const UCHAR *If, const UCHAR *End)
{
    USB_DEV *Dev = m->Dev;
    const UCHAR *ep[5] = { NULL, NULL, NULL, NULL, NULL };
    UCHAR exp = 0;
    int r;
    int i;
    int n;

    if (UasParse(If, End, ep, &exp) != USB_OK) {
        return USB_ERR_PARAM;
    }
    if (If[3] != 0) {
        r = UsbControl(Dev, USB_RT_OUT | USB_RT_STD | USB_RT_INTERFACE, USB_REQ_SET_INTERFACE, If[3], If[2],
                       0, NULL, 1000, NULL);
        if (r != USB_OK) {
            return r;
        }
    }
    m->Alt = If[3];
    r = UsbOpenPipe(Dev, &m->Cmd, ep[UAS_PIPE_CMD]);
    if (r == USB_OK) r = UsbOpenPipe(Dev, &m->Stat, ep[UAS_PIPE_STAT]);
    if (r == USB_OK) r = UsbOpenPipe(Dev, &m->In, ep[UAS_PIPE_IN]);
    if (r == USB_OK) r = UsbOpenPipe(Dev, &m->Out, ep[UAS_PIPE_OUT]);
    if (r != USB_OK) {
        return r;
    }
    if (Dev->Speed == USB_SPEED_SUPER) {
        n = (exp != 0) ? (1 << (exp > 4 ? 4 : exp)) : 0;
        if (n > UAS_MAX_TAGS) {
            n = UAS_MAX_TAGS;
        }
        if (n > Dev->Hc->MaxStreams) {
            n = Dev->Hc->MaxStreams;
        }
        if (n < 1 || !Dev->Hc->QueueOk) {
            UsbLog(LOG_INFO, "device %u: UAS needs bulk streams, not available\n", Dev->Index);
            return USB_ERR_PARAM;
        }
        r = UsbOpenStreams(&m->Stat, m->StatS, n);
        if (r == USB_OK) r = UsbOpenStreams(&m->In, m->InS, n);
        if (r == USB_OK) r = UsbOpenStreams(&m->Out, m->OutS, n);
        if (r != USB_OK) {
            return r;
        }
        m->Streams = (UCHAR)n;
        m->NumTags = (UCHAR)n;
    } else {
        m->Streams = 0;
        m->NumTags = 1;
    }
    for (i = 0; i < m->NumTags; i++) {
        UAS_TAG *t = &m->Tags[i];
        t->Msc = m;
        t->Tag = (USHORT)(i + 1);
        OsEventInit(&t->Done);
        t->Iu = (UCHAR *)UsbDmaAlloc(UAS_IU_SIZE, &t->IuPhys);
        if (t->Iu == NULL) {
            return USB_ERR_NOMEM;
        }
    }
    m->Proto = MSC_PROTO_UAS;
    return USB_OK;
}

static void MscClosePipes(MSC_DEV *m)
{
    int i;

    for (i = 0; i < m->Streams; i++) {
        UsbCancelPipe(&m->StatS[i]);
        UsbCancelPipe(&m->InS[i]);
        UsbCancelPipe(&m->OutS[i]);
    }
    UsbClosePipe(&m->Cmd);
    UsbClosePipe(&m->Stat);
    UsbClosePipe(&m->In);
    UsbClosePipe(&m->Out);
    UsbClosePipe(&m->Intr);
    m->Streams = 0;
}

static int BotCbiSetup(MSC_DEV *m, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *ep = If;
    const UCHAR *epIn = NULL;
    const UCHAR *epOut = NULL;
    const UCHAR *epInt = NULL;
    int r;

    while ((ep = UsbFindDesc(ep, End, USB_DT_ENDPOINT)) != NULL) {
        UCHAR type = (UCHAR)(ep[3] & 3);
        if (type == USB_EP_BULK && (ep[2] & 0x80) && epIn == NULL) {
            epIn = ep;
        } else if (type == USB_EP_BULK && !(ep[2] & 0x80) && epOut == NULL) {
            epOut = ep;
        } else if (type == USB_EP_INTERRUPT && (ep[2] & 0x80) && epInt == NULL) {
            epInt = ep;
        }
    }
    if (epIn == NULL || epOut == NULL) {
        return USB_ERR_PARAM;
    }
    r = UsbOpenPipe(m->Dev, &m->In, epIn);
    if (r == USB_OK) {
        r = UsbOpenPipe(m->Dev, &m->Out, epOut);
    }
    if (r == USB_OK && m->Proto == MSC_PROTO_CBI_INT && epInt != NULL) {
        r = UsbOpenPipe(m->Dev, &m->Intr, epInt);
    }
    return r;
}

int MscAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End)
{
    MSC_DEV *m;
    const UCHAR *uas = OsGetConfig("UseUas", 1) ? FindUasAlt(If, End) : NULL;
    UCHAR proto = If[7];
    ULONG kb;
    UCHAR lun;
    int r = USB_ERR_PARAM;

    if (proto != MSC_PROTO_BOT && proto != MSC_PROTO_CBI && proto != MSC_PROTO_CBI_INT && uas == NULL) {
        UsbLog(LOG_INFO, "device %u: mass storage protocol %02x not supported\n", Dev->Index, proto);
        return USB_ERR_PARAM;
    }
    if (Dev->Msc != NULL) {
        return USB_ERR_BUSY;
    }
    m = (MSC_DEV *)OsAlloc(sizeof(MSC_DEV));
    if (m == NULL) {
        return USB_ERR_NOMEM;
    }
    m->Dev = Dev;
    m->Iface = If[2];
    m->SubClass = If[6];
    m->Proto = proto;
    m->Cbw = (UCHAR *)UsbDmaAlloc(64, &m->CbwPhys);
    m->Buf = (UCHAR *)OsDmaAlloc(MSC_BUF_SIZE, &m->BufPhys);
    if (m->Cbw == NULL || m->Buf == NULL) {
        UsbDmaFree(m->Cbw, 64);
        if (m->Buf != NULL) {
            OsDmaFree(m->Buf, MSC_BUF_SIZE);
        }
        OsFree(m);
        return USB_ERR_NOMEM;
    }
    Dev->Msc = m;

    if (uas != NULL) {
        r = UasSetup(m, uas, End);
        if (r != USB_OK) {
            MscClosePipes(m);
            m->Proto = proto;
            if (uas != If && (proto == MSC_PROTO_BOT || proto == MSC_PROTO_CBI || proto == MSC_PROTO_CBI_INT)) {
                UsbControl(Dev, USB_RT_OUT | USB_RT_STD | USB_RT_INTERFACE, USB_REQ_SET_INTERFACE, If[3], If[2],
                           0, NULL, 1000, NULL);
            }
        }
    }
    if (m->Proto != MSC_PROTO_UAS) {
        if (proto != MSC_PROTO_BOT && proto != MSC_PROTO_CBI && proto != MSC_PROTO_CBI_INT) {
            MscDetach(Dev);
            return USB_ERR_PARAM;
        }
        r = BotCbiSetup(m, If, End);
    }
    if (r != USB_OK) {
        MscDetach(Dev);
        return r;
    }
    m->CdbPad12 = (UCHAR)(m->SubClass == 0x02 || m->SubClass == 0x04 || m->SubClass == 0x05 ||
                          (m->Proto != MSC_PROTO_BOT && m->Proto != MSC_PROTO_UAS));

    kb = OsGetConfig("MaxTransferKB", 0);
    if (kb != 0) {
        m->MaxXfer = kb * 1024;
    } else if (Dev->Speed == USB_SPEED_SUPER || m->Proto == MSC_PROTO_UAS) {
        m->MaxXfer = 0x100000;
    } else if (Dev->Speed == USB_SPEED_HIGH) {
        m->MaxXfer = 240 * 512;
    } else {
        m->MaxXfer = 0x10000;
    }
    if (m->MaxXfer < 0x1000) {
        m->MaxXfer = 0x1000;
    }

    m->MaxLun = 0;
    if (m->Proto == MSC_PROTO_BOT) {
        UCHAR maxlun = 0;
        ULONG got = 0;
        r = UsbControl(Dev, USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE, 0xFE, 0, m->Iface, 1, &maxlun, 1000, &got);
        if (r == USB_OK && got == 1 && maxlun < 16) {
            m->MaxLun = maxlun;
        }
    }
    m->Identity = ReadIdentity(Dev);
    Dev->Function |= USB_FUNC_STORAGE;
    if (m->Proto == MSC_PROTO_UAS) {
        UsbLog(LOG_INFO, "device %u: mass storage, UAS, %s, %u KB per command\n", Dev->Index,
               m->Streams ? "streams" : "no streams", m->MaxXfer / 1024);
    } else {
        UsbLog(LOG_INFO, "device %u: mass storage, subclass %02x protocol %02x, %u LUN(s), %u KB per command\n",
               Dev->Index, m->SubClass, m->Proto, m->MaxLun + 1, m->MaxXfer / 1024);
    }

    for (lun = 0; lun <= m->MaxLun && !Dev->Gone; lun++) {
        MscAttachLun(m, lun);
    }
    return USB_OK;
}

void MscDetach(USB_DEV *Dev)
{
    MSC_DEV *m = (MSC_DEV *)Dev->Msc;
    int i;

    if (m == NULL) {
        return;
    }
    m->Gone = 1;
    for (i = 0; i < SCSI_MAX_SLOTS; i++) {
        if (Slots[i].Msc == m) {
            Slots[i].Present = 0;
            Slots[i].Msc = NULL;
            Slots[i].Ejected = 0;
            Slots[i].UnitAttention = 0;
            Slots[i].MediaReady = 0;
            UsbLog(LOG_INFO, "slot %d: device removed\n", i);
            OsScsiMediaChange(i);
        }
    }
    MscClosePipes(m);
    while (m->WaitHead != NULL) {
        SCSI_REQ *r = m->WaitHead;
        m->WaitHead = r->Next;
        r->SrbStatus = SRBST_SELECTION_TIMEOUT;
        if (r->Complete != NULL) {
            r->Complete(r);
        }
    }
    for (i = 0; i < UAS_MAX_TAGS; i++) {
        if (m->Tags[i].Iu != NULL) {
            UsbDmaFree(m->Tags[i].Iu, UAS_IU_SIZE);
        }
    }
    UsbDmaFree(m->Cbw, 64);
    OsDmaFree(m->Buf, MSC_BUF_SIZE);
    Dev->Msc = NULL;
    OsFree(m);
}

/* ------------------------------------------------------------------ */
/* Virtual target attach / detach                                       */
/* ------------------------------------------------------------------ */

BLK_TARGET *MscVirtAttach(const char *Name, ULONG Identity, ULONG MaxXfer, const BLK_TARGET_OPS *Ops, void *Ctx)
{
    static UCHAR next = 0xF0;
    MSC_DEV *m = (MSC_DEV *)OsAlloc(sizeof(MSC_DEV));
    int i;

    if (m == NULL) {
        return NULL;
    }
    m->Buf = (UCHAR *)OsDmaAlloc(MSC_BUF_SIZE, &m->BufPhys);
    if (m->Buf == NULL) {
        OsFree(m);
        return NULL;
    }
    m->Proto = MSC_PROTO_VIRT;
    m->VOps = Ops;
    m->VCtx = Ctx;
    m->VDev.InUse = 1;
    m->VDev.Index = next;
    m->VDev.Hc = &m->VHc;
    m->VDev.Msc = m;
    next = (UCHAR)(next == 0xFD ? 0xF0 : next + 1);
    m->Dev = &m->VDev;
    m->Identity = Identity & 0x0FFFFFFF;
    m->MaxXfer = MaxXfer ? MaxXfer : 0x10000;
    if (m->MaxXfer > MSC_BUF_SIZE * 16) {
        m->MaxXfer = MSC_BUF_SIZE * 16;
    }
    for (i = 0; i < (int)sizeof(m->VName) - 1 && Name[i]; i++) {
        m->VName[i] = Name[i];
    }
    UsbLog(LOG_INFO, "%s: block device %u\n", m->VName, m->VDev.Index);
    MscAttachLun(m, 0);
    return m;
}

void MscVirtSetKind(BLK_TARGET *m, UCHAR Kind)
{
    if (m != NULL) {
        m->VKind = Kind;
        UsbNotifyChange();
    }
}

void MscVirtDetach(BLK_TARGET *m)
{
    int i;

    if (m == NULL) {
        return;
    }
    m->Gone = 1;
    m->VDev.Gone = 1;
    for (i = 0; i < SCSI_MAX_SLOTS; i++) {
        if (Slots[i].Msc == m) {
            Slots[i].Present = 0;
            Slots[i].Msc = NULL;
            Slots[i].Ejected = 0;
            Slots[i].UnitAttention = 0;
            Slots[i].MediaReady = 0;
            UsbLog(LOG_INFO, "slot %d: %s removed\n", i, m->VName);
            OsScsiMediaChange(i);
        }
    }
    UsbNotifyChange();
    OsDmaFree(m->Buf, MSC_BUF_SIZE);
    OsFree(m);
}

void MscVirtMediaChange(BLK_TARGET *m)
{
    int i;

    for (i = 0; i < SCSI_MAX_SLOTS; i++) {
        SCSI_SLOT *s = &Slots[i];
        if (s->Msc != m || !s->Present) {
            continue;
        }
        s->MediaReady = 0;
        s->SenseValid = 0;
        s->Ejected = 0;
        s->LastLba = 0;
        s->BlockSize = 0;
        if (s->MaybeMounted && Frozen) {
            s->UnitAttention = 1;
        }
        s->MaybeMounted = 0;
        {
            /* the new card's size at once, for usbtree and usbmon (the disk
               driver asks only when something opens the drive) */
            static const UCHAR rc[10] = { 0x25 };
            UCHAR cap[8], sense[18];
            ULONG done = 0;
            if (m->VOps->Command(m->VCtx, rc, 10, SCSI_DIR_IN, 8, cap, &done, sense) == BLK_OK && done == 8) {
                s->LastLba = GETBE32(cap);
                s->BlockSize = GETBE32(cap + 4);
                s->MediaReady = 1;
            }
        }
        s->MediaGen++;
        SlotScan(s);
        OsScsiMediaChange(i);
        UsbLog(LOG_INFO, "slot %d: medium changed (%s)\n", i, m->VName);
    }
    UsbNotifyChange();
}

/* name of the card reader behind a disk slot that is not a USB device */
BOOLEAN UsbScsiSlotHost(int Slot, char *Buf, int Size, UCHAR *Kind)
{
    MSC_DEV *m;
    int i;

    if (Slot < 0 || (ULONG)Slot >= UsbScsiSlotCount || Size < 1) {
        return FALSE;
    }
    m = Slots[Slot].Msc;
    if (!Slots[Slot].Present || m == NULL || m->Proto != MSC_PROTO_VIRT) {
        return FALSE;
    }
    for (i = 0; i < Size - 1 && m->VName[i]; i++) {
        Buf[i] = m->VName[i];
    }
    Buf[i] = 0;
    if (Kind != NULL) {
        *Kind = m->VKind;
    }
    return TRUE;
}
