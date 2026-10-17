/*
 * nvmehp.c - NVMe drives that come and go: in Thunderbolt / USB4 enclosures
 * and docks (PCIe tunnel), ExpressCard and other PCIe hot plug slots.
 *
 * Windows NT has no PCI hot plug, so a SCSI miniport (nvme2k) only sees the
 * NVMe controllers that are there at boot.  The ones that arrive later are
 * found by the hot plug code of usbcore.c, which gives them bus numbers and
 * memory, and are driven here: one admin and one I/O queue pair, polled (no
 * interrupt line can be added at run time), every namespace a disk slot like
 * a USB disk.  Data goes through a 128 KB bounce buffer, so any page list of
 * the request is fine.  A controller that is pulled out reads as all ones:
 * its requests fail at once and its slots go away.
 *
 * Controllers present at boot are left to nvme2k unless NvmeAtBoot = 1.
 */

#include "usbnt.h"
#include "usbscsi.h"
#include "usbblk.h"
#include "nvmehp.h"

#define NVME_MAX        4               /* controllers */
#define NVME_MAX_NS     4               /* namespaces (disk slots) per controller */
#define NVME_XFER       0x20000         /* bounce buffer, also the largest command */
#define NVME_QSIZE      16              /* entries of each queue (only one command runs at a time) */

/* registers */
#define REG_CAP         0x00
#define REG_VS          0x08
#define REG_INTMS       0x0C
#define REG_CC          0x14
#define REG_CSTS        0x1C
#define REG_AQA         0x24
#define REG_ASQ         0x28
#define REG_ACQ         0x30
#define CC_EN           0x00000001UL
#define CC_SHN_NORMAL   0x00004000UL
#define CC_IOSQES_CQES  ((6UL << 16) | (4UL << 20))
#define CSTS_RDY        0x01
#define CSTS_CFS        0x02
#define CSTS_SHST_MASK  0x0C
#define CSTS_SHST_DONE  0x08

/* admin commands */
#define ADM_DELETE_SQ   0x00
#define ADM_CREATE_SQ   0x01
#define ADM_DELETE_CQ   0x04
#define ADM_CREATE_CQ   0x05
#define ADM_IDENTIFY    0x06
#define ADM_SET_FEAT    0x09
/* NVM commands */
#define NVM_FLUSH       0x00
#define NVM_WRITE       0x01
#define NVM_READ        0x02

#define SENSE_NOT_READY         0x02
#define SENSE_MEDIUM_ERROR      0x03
#define SENSE_HARDWARE_ERROR    0x04
#define SENSE_ILLEGAL_REQUEST   0x05

typedef struct _NVME_CTRL NVME_CTRL;

typedef struct _NVME_NS {
    NVME_CTRL  *C;
    ULONG       Nsid;
    ULONGLONG   Blocks;
    ULONG       BlockSize;
    BLK_TARGET *Target;
} NVME_NS;

typedef struct _NVME_Q {
    UCHAR      *Sq, *Cq;
    ULONG       SqPhys, CqPhys;
    USHORT      Tail, Head;
    UCHAR       Phase;
    USHORT      Cid;
} NVME_Q;

struct _NVME_CTRL {
    BOOLEAN     Used, Gone;
    BOOLEAN     Down;                   /* shut down for removal (eject) */
    UCHAR       Bus, Dev, Fn;
    ULONG       Phys, Len;
    volatile UCHAR *Regs;
    ULONG       Stride;                 /* doorbell stride in bytes */
    ULONG       TimeoutMs;              /* CAP.TO */
    ULONG       MaxXfer;                /* MDTS, at most NVME_XFER */
    NVME_Q      Adm, Io;
    UCHAR      *Buf;                    /* bounce buffer */
    ULONG       BufPhys;
    ULONG      *Prp;                    /* PRP list for the bounce buffer */
    ULONG       PrpPhys;
    UCHAR      *Id;                     /* 4 KB for Identify */
    ULONG       IdPhys;
    char        Model[41];
    char        Serial[21];
    ULONG       Identity;
    int         NNs;
    NVME_NS     Ns[NVME_MAX_NS];
};

static NVME_CTRL Ctrls[NVME_MAX];

/* ------------------------------------------------------------------ */
/* registers and queues                                                 */
/* ------------------------------------------------------------------ */

static ULONG Rd(NVME_CTRL *c, ULONG off)
{
    return MmioRd32(c->Regs + off);
}

static void Wr(NVME_CTRL *c, ULONG off, ULONG v)
{
    MmioWr32(c->Regs + off, v);
}

static BOOLEAN Gone(NVME_CTRL *c)
{
    if (!c->Gone && Rd(c, REG_CSTS) == 0xFFFFFFFFUL) {
        c->Gone = TRUE;
        UsbLog(LOG_INFO, "nvme %02x:%02x.%u: the controller is gone\n", c->Bus, c->Dev, c->Fn);
    }
    return c->Gone;
}

static BOOLEAN QueueAlloc(NVME_Q *q)
{
    q->Sq = (UCHAR *)OsDmaAlloc(4096, &q->SqPhys);
    q->Cq = (UCHAR *)OsDmaAlloc(4096, &q->CqPhys);
    q->Tail = q->Head = 0;
    q->Phase = 1;
    return q->Sq != NULL && q->Cq != NULL;
}

static void QueueFree(NVME_Q *q)
{
    if (q->Sq) OsDmaFree(q->Sq, 4096);
    if (q->Cq) OsDmaFree(q->Cq, 4096);
    q->Sq = q->Cq = NULL;
}

/* one command, waited for (polled); Qid 0 = admin; returns the NVMe status
   (0 = success), 0xFFFF on time-out, 0xFFFE when the controller is gone */
static ULONG Exec(NVME_CTRL *c, int Qid, ULONG *Cmd, ULONG TimeoutMs, ULONG *Result)
{
    NVME_Q *q = Qid ? &c->Io : &c->Adm;
    ULONG *sqe = (ULONG *)(q->Sq + (ULONG)q->Tail * 64);
    ULONG t0 = OsTimeMs(), spins = 0, cid;
    int i;

    if (Gone(c)) return 0xFFFE;
    cid = q->Cid++;
    Cmd[0] = (Cmd[0] & 0xFFFF) | (cid << 16);
    for (i = 0; i < 16; i++) sqe[i] = Cmd[i];
    q->Tail = (USHORT)((q->Tail + 1) % NVME_QSIZE);
    Wr(c, 0x1000 + (2 * Qid) * c->Stride, q->Tail);
    for (;;) {
        volatile ULONG *cqe = (volatile ULONG *)(q->Cq + (ULONG)q->Head * 16);
        ULONG dw3 = cqe[3];
        if (((dw3 >> 16) & 1) == q->Phase) {
            ULONG st = (dw3 >> 17) & 0x7FFF;
            if (Result) *Result = cqe[0];
            q->Head = (USHORT)((q->Head + 1) % NVME_QSIZE);
            if (q->Head == 0) q->Phase ^= 1;
            Wr(c, 0x1000 + (2 * Qid + 1) * c->Stride, q->Head);
            if ((dw3 & 0xFFFF) != cid) {
                UsbLog(LOG_ERR, "nvme %02x:%02x.%u: completion for command %u, %u expected\n", c->Bus, c->Dev,
                       c->Fn, dw3 & 0xFFFF, cid);
                continue;
            }
            return st;
        }
        if (++spins > 200) {
            if (Gone(c)) return 0xFFFE;
            if (Rd(c, REG_CSTS) & CSTS_CFS) {
                UsbLog(LOG_ERR, "nvme %02x:%02x.%u: controller fatal status\n", c->Bus, c->Dev, c->Fn);
                return 0xFFFF;
            }
            if (OsTimeMs() - t0 > TimeoutMs) {
                UsbLog(LOG_ERR, "nvme %02x:%02x.%u: command %02x timed out\n", c->Bus, c->Dev, c->Fn,
                       Cmd[0] & 0xFF);
                return 0xFFFF;
            }
            if (OsInWorker()) OsSleepMs(1); else OsStallUs(50);
        } else {
            OsStallUs(5);
        }
    }
}

static void CmdInit(ULONG *Cmd, UCHAR Op, ULONG Nsid)
{
    int i;
    for (i = 0; i < 16; i++) Cmd[i] = 0;
    Cmd[0] = Op;
    Cmd[1] = Nsid;
}

/* PRP entries for Len bytes of the bounce buffer */
static void SetPrp(NVME_CTRL *c, ULONG *Cmd, ULONG BufPhys, ULONG Len)
{
    Cmd[6] = BufPhys;
    Cmd[7] = 0;
    if (Len <= 4096) {
        Cmd[8] = 0;
    } else if (Len <= 8192) {
        Cmd[8] = BufPhys + 4096;
    } else {
        ULONG n = (Len + 4095) / 4096, i;
        for (i = 1; i < n; i++) {
            c->Prp[2 * (i - 1)] = BufPhys + i * 4096;
            c->Prp[2 * (i - 1) + 1] = 0;
        }
        Cmd[8] = c->PrpPhys;
    }
    Cmd[9] = 0;
}

static BOOLEAN WaitReady(NVME_CTRL *c, BOOLEAN Want)
{
    ULONG t0 = OsTimeMs();
    for (;;) {
        ULONG st = Rd(c, REG_CSTS);
        if (st == 0xFFFFFFFFUL) return FALSE;
        if (((st & CSTS_RDY) != 0) == Want) return TRUE;
        if (Want && (st & CSTS_CFS)) return FALSE;
        if (OsTimeMs() - t0 > c->TimeoutMs) return FALSE;
        OsSleepMs(10);
    }
}

static void Trim(char *Out, const UCHAR *In, int N)
{
    int i;
    for (i = 0; i < N; i++) Out[i] = (char)(In[i] >= 32 && In[i] < 127 ? In[i] : ' ');
    Out[N] = 0;
    for (i = N - 1; i >= 0 && Out[i] == ' '; i--) Out[i] = 0;
}

/* ------------------------------------------------------------------ */
/* SCSI                                                                 */
/* ------------------------------------------------------------------ */

static void Put32(UCHAR *p, ULONG v) { p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16); p[2] = (UCHAR)(v >> 8); p[3] = (UCHAR)v; }
static ULONG Get32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

static void Copy(UCHAR *dst, ULONG len, const UCHAR *src, ULONG n, ULONG *done)
{
    ULONG k = n < len ? n : len;
    if (dst != NULL && k != 0) UsbMemCpy(dst, src, k);
    *done = k;
}

/* NVMe status -> SCSI sense */
static int StatusSense(NVME_CTRL *c, ULONG St, BOOLEAN Write, UCHAR *Sense)
{
    if (St == 0xFFFE) return BLK_NODEV;
    if (St == 0xFFFF) {
        BlkSense(Sense, SENSE_HARDWARE_ERROR, 0x44, 0);
    } else if ((St & 0x7FF) == 0x80) {                 /* LBA out of range */
        BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x21, 0);
    } else if (((St >> 8) & 7) == 2) {                  /* media and data integrity errors */
        BlkSense(Sense, SENSE_MEDIUM_ERROR, Write ? 0x0C : 0x11, 0);
    } else {
        BlkSense(Sense, SENSE_HARDWARE_ERROR, 0x44, 0);
    }
    UsbLog(LOG_ERR, "nvme %02x:%02x.%u: status %04x\n", c->Bus, c->Dev, c->Fn, St);
    return BLK_FAILED;
}

static int Rw(NVME_NS *ns, ULONGLONG Lba, ULONG Count, UCHAR *Data, BOOLEAN Write, UCHAR *Sense)
{
    NVME_CTRL *c = ns->C;
    ULONG per = c->MaxXfer / ns->BlockSize;
    ULONG cmd[16];

    if (per == 0) per = 1;
    while (Count != 0) {
        ULONG n = Count > per ? per : Count, len = n * ns->BlockSize, st;
        CmdInit(cmd, (UCHAR)(Write ? NVM_WRITE : NVM_READ), ns->Nsid);
        SetPrp(c, cmd, c->BufPhys, len);
        cmd[10] = (ULONG)Lba;
        cmd[11] = (ULONG)(Lba >> 32);
        cmd[12] = n - 1;
        if (Write) UsbMemCpy(c->Buf, Data, len);
        st = Exec(c, 1, cmd, 30000, NULL);
        if (st != 0) return StatusSense(c, st, Write, Sense);
        if (!Write) UsbMemCpy(Data, c->Buf, len);
        Lba += n;
        Count -= n;
        Data += len;
    }
    return BLK_OK;
}

static int NvmeScsi(void *Ctx, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len, UCHAR *Data, ULONG *Done,
                    UCHAR *Sense)
{
    NVME_NS *ns = (NVME_NS *)Ctx;
    NVME_CTRL *c = ns->C;
    UCHAR b[36];
    ULONGLONG lba;
    ULONG cnt, cmd[16], st;
    (void)CdbLen; (void)Dir;

    *Done = 0;
    if (c->Gone || Gone(c)) return BLK_NODEV;
    switch (Cdb[0]) {
    case 0x12: {                                    /* INQUIRY */
        int i;
        UsbMemSet(b, ' ', sizeof(b));
        b[0] = 0x00; b[1] = 0x80; b[2] = 0x05; b[3] = 0x02; b[4] = 31;
        b[5] = 0; b[6] = 0; b[7] = 0x02;
        for (i = 0; i < 8; i++) b[8 + i] = (UCHAR)"NVMe    "[i];
        for (i = 0; i < 16 && c->Model[i]; i++) b[16 + i] = (UCHAR)c->Model[i];
        b[32] = '1'; b[33] = '.'; b[34] = '0'; b[35] = ' ';
        Copy(Data, Len, b, 36, Done);
        return BLK_OK;
    }
    case 0x1B:                                      /* START STOP UNIT */
        if ((Cdb[4] & 3) == 2 && !c->Down) {
            /* eject (safe removal): the drive saves its caches and tables and
               may be pulled out */
            ULONG t0 = OsTimeMs();
            CmdInit(cmd, NVM_FLUSH, ns->Nsid);
            Exec(c, 1, cmd, 30000, NULL);
            Wr(c, REG_CC, (Rd(c, REG_CC) & ~0xC000UL) | CC_SHN_NORMAL);
            while ((Rd(c, REG_CSTS) & CSTS_SHST_MASK) != CSTS_SHST_DONE && OsTimeMs() - t0 < 5000) {
                OsSleepMs(10);
            }
            c->Down = TRUE;
            UsbLog(LOG_INFO, "nvme %02x:%02x.%u: shut down for removal\n", c->Bus, c->Dev, c->Fn);
        }
        return BLK_OK;
    case 0x00:                                      /* TEST UNIT READY */
        if (c->Down) {
            BlkSense(Sense, SENSE_NOT_READY, 0x3A, 0);
            return BLK_FAILED;
        }
        return BLK_OK;
    case 0x1E: case 0x2F:                           /* PREVENT, VERIFY */
        return BLK_OK;
    case 0x35: case 0x91:                           /* SYNCHRONIZE CACHE (10 / 16) */
        CmdInit(cmd, NVM_FLUSH, ns->Nsid);
        st = Exec(c, 1, cmd, 30000, NULL);
        return st ? StatusSense(c, st, TRUE, Sense) : BLK_OK;
    case 0x25:                                      /* READ CAPACITY (10) */
        Put32(b, ns->Blocks > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (ULONG)(ns->Blocks - 1));
        Put32(b + 4, ns->BlockSize);
        Copy(Data, Len, b, 8, Done);
        return BLK_OK;
    case 0x9E:                                      /* READ CAPACITY (16) */
        UsbMemSet(b, 0, 32);
        Put32(b, (ULONG)((ns->Blocks - 1) >> 32));
        Put32(b + 4, (ULONG)(ns->Blocks - 1));
        Put32(b + 8, ns->BlockSize);
        Copy(Data, Len, b, 32, Done);
        return BLK_OK;
    case 0x1A: case 0x5A:                           /* MODE SENSE (6) / (10) */
        UsbMemSet(b, 0, 8);
        if (Cdb[0] == 0x1A) { b[0] = 3; Copy(Data, Len, b, 4, Done); }
        else { b[1] = 6; Copy(Data, Len, b, 8, Done); }
        return BLK_OK;
    case 0x08: case 0x0A:                           /* READ (6) / WRITE (6) */
        if (c->Down) goto down;
        lba = ((ULONG)(Cdb[1] & 0x1F) << 16) | ((ULONG)Cdb[2] << 8) | Cdb[3];
        cnt = Cdb[4] ? Cdb[4] : 256;
        goto rw;
    case 0x28: case 0x2A:                           /* READ / WRITE (10) */
        lba = Get32(Cdb + 2);
        cnt = ((ULONG)Cdb[7] << 8) | Cdb[8];
        goto rw;
    case 0xA8: case 0xAA:                           /* READ / WRITE (12) */
        lba = Get32(Cdb + 2);
        cnt = Get32(Cdb + 6);
        goto rw;
    case 0x88: case 0x8A:                           /* READ / WRITE (16) */
        lba = ((ULONGLONG)Get32(Cdb + 2) << 32) | Get32(Cdb + 6);
        cnt = Get32(Cdb + 10);
    rw: {
        BOOLEAN write = (Cdb[0] & 0x02) != 0;
        if (c->Down) {
        down:
            BlkSense(Sense, SENSE_NOT_READY, 0x3A, 0);
            return BLK_FAILED;
        }
        if (cnt == 0) return BLK_OK;
        if (lba >= ns->Blocks || cnt > ns->Blocks - lba) {
            BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x21, 0);
            return BLK_FAILED;
        }
        if (Data == NULL || Len < cnt * ns->BlockSize) {
            BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x24, 0);
            return BLK_FAILED;
        }
        {
            int r = Rw(ns, lba, cnt, Data, write, Sense);
            if (r == BLK_OK) *Done = cnt * ns->BlockSize;
            return r;
        }
    }
    default:
        BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x20, 0);
        return BLK_FAILED;
    }
}

static const BLK_TARGET_OPS NvmeTargetOps = { NvmeScsi };

/* ------------------------------------------------------------------ */
/* bring-up                                                             */
/* ------------------------------------------------------------------ */

static void Release(NVME_CTRL *c)
{
    QueueFree(&c->Adm);
    QueueFree(&c->Io);
    if (c->Buf) OsDmaFree(c->Buf, NVME_XFER);
    if (c->Prp) OsDmaFree(c->Prp, 4096);
    if (c->Id) OsDmaFree(c->Id, 4096);
    if (c->Regs) OsUnmapMmio((void *)c->Regs, c->Len);
    UsbMemSet(c, 0, sizeof(*c));
}

static int Start(NVME_CTRL *c)
{
    ULONG cap0, cap1, cmd[16], st, v, mps, i, nn;
    ULONG *list;

    cap0 = Rd(c, REG_CAP);
    cap1 = Rd(c, REG_CAP + 4);
    v = Rd(c, REG_VS);
    if (cap0 == 0xFFFFFFFFUL) return USB_ERR_NODEV;
    c->Stride = 4UL << (cap1 & 0xF);
    c->TimeoutMs = ((cap0 >> 24) & 0xFF) * 500;
    if (c->TimeoutMs < 2000) c->TimeoutMs = 2000;
    if (c->TimeoutMs > 60000) c->TimeoutMs = 60000;
    mps = (cap1 >> 16) & 0xF;                           /* MPSMIN */
    if (mps != 0) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: smallest page %u bytes, 4 KB needed\n", c->Bus, c->Dev, c->Fn,
               4096U << mps);
        return USB_ERR_PARAM;
    }
    if ((cap0 & 0xFFFF) + 1 < NVME_QSIZE) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: queues too small\n", c->Bus, c->Dev, c->Fn);
        return USB_ERR_PARAM;
    }
    /* disable, set up the admin queue, enable */
    if (Rd(c, REG_CC) & CC_EN) {
        Wr(c, REG_CC, Rd(c, REG_CC) & ~CC_EN);
        if (!WaitReady(c, FALSE)) return USB_ERR_TIMEOUT;
    }
    if (!QueueAlloc(&c->Adm) || !QueueAlloc(&c->Io)) return USB_ERR_NOMEM;
    c->Buf = (UCHAR *)OsDmaAlloc(NVME_XFER, &c->BufPhys);
    c->Prp = (ULONG *)OsDmaAlloc(4096, &c->PrpPhys);
    c->Id = (UCHAR *)OsDmaAlloc(4096, &c->IdPhys);
    if (c->Buf == NULL || c->Prp == NULL || c->Id == NULL) return USB_ERR_NOMEM;
    Wr(c, REG_INTMS, 0xFFFFFFFFUL);                     /* polled */
    Wr(c, REG_AQA, ((NVME_QSIZE - 1UL) << 16) | (NVME_QSIZE - 1UL));
    Wr(c, REG_ASQ, c->Adm.SqPhys);
    Wr(c, REG_ASQ + 4, 0);
    Wr(c, REG_ACQ, c->Adm.CqPhys);
    Wr(c, REG_ACQ + 4, 0);
    Wr(c, REG_CC, CC_IOSQES_CQES | CC_EN);              /* NVM command set, 4 KB pages, round robin */
    if (!WaitReady(c, TRUE)) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: does not become ready (status %08x)\n", c->Bus, c->Dev, c->Fn,
               Rd(c, REG_CSTS));
        return USB_ERR_TIMEOUT;
    }
    /* Identify controller */
    CmdInit(cmd, ADM_IDENTIFY, 0);
    SetPrp(c, cmd, c->IdPhys, 4096);
    cmd[10] = 1;
    if ((st = Exec(c, 0, cmd, 5000, NULL)) != 0) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: Identify fails (%04x)\n", c->Bus, c->Dev, c->Fn, st);
        return USB_ERR_IO;
    }
    Trim(c->Serial, c->Id + 4, 20);
    Trim(c->Model, c->Id + 24, 40);
    nn = *(ULONG *)(c->Id + 516);
    c->MaxXfer = NVME_XFER;
    if (c->Id[77] != 0 && (4096UL << c->Id[77]) < c->MaxXfer) c->MaxXfer = 4096UL << c->Id[77];
    c->Identity = 0x4E56;
    for (i = 0; c->Serial[i]; i++) c->Identity = c->Identity * 31 + (UCHAR)c->Serial[i];
    UsbLog(LOG_INFO, "nvme %02x:%02x.%u: %s, serial %s, NVMe %u.%u, %u namespace(s), up to %u KB per command\n",
           c->Bus, c->Dev, c->Fn, c->Model, c->Serial, v >> 16, (v >> 8) & 0xFF, nn, c->MaxXfer / 1024);
    /* I/O queue pair (polled: no interrupt vector) */
    CmdInit(cmd, ADM_SET_FEAT, 0);
    cmd[10] = 7;                                        /* number of queues: 1 + 1 */
    cmd[11] = 0;
    Exec(c, 0, cmd, 5000, NULL);
    CmdInit(cmd, ADM_CREATE_CQ, 0);
    cmd[6] = c->Io.CqPhys;
    cmd[10] = 1 | ((NVME_QSIZE - 1UL) << 16);
    cmd[11] = 1;                                        /* physically contiguous, no interrupts */
    if ((st = Exec(c, 0, cmd, 5000, NULL)) != 0) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: Create I/O CQ fails (%04x)\n", c->Bus, c->Dev, c->Fn, st);
        return USB_ERR_IO;
    }
    CmdInit(cmd, ADM_CREATE_SQ, 0);
    cmd[6] = c->Io.SqPhys;
    cmd[10] = 1 | ((NVME_QSIZE - 1UL) << 16);
    cmd[11] = 1 | (1UL << 16);                          /* contiguous, CQ 1 */
    if ((st = Exec(c, 0, cmd, 5000, NULL)) != 0) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: Create I/O SQ fails (%04x)\n", c->Bus, c->Dev, c->Fn, st);
        return USB_ERR_IO;
    }
    /* namespaces: the active list (NVMe 1.1+), else 1..NN */
    list = (ULONG *)c->Buf;
    UsbMemSet(list, 0, 4096);
    CmdInit(cmd, ADM_IDENTIFY, 0);
    SetPrp(c, cmd, c->BufPhys, 4096);
    cmd[10] = 2;
    if (v < 0x10100 || Exec(c, 0, cmd, 5000, NULL) != 0) {
        for (i = 0; i < nn && i < NVME_MAX_NS; i++) list[i] = i + 1;
        list[i] = 0;
    }
    for (i = 0; list[i] != 0 && c->NNs < NVME_MAX_NS && i < 1024; i++) {
        ULONG nsid = list[i], lbaf, lbads;
        ULONGLONG nsze;
        NVME_NS *ns;
        CmdInit(cmd, ADM_IDENTIFY, nsid);
        SetPrp(c, cmd, c->IdPhys, 4096);
        cmd[10] = 0;
        if (Exec(c, 0, cmd, 5000, NULL) != 0) continue;
        nsze = *(ULONGLONG *)c->Id;
        lbaf = *(ULONG *)(c->Id + 128 + 4 * (c->Id[26] & 0xF));
        lbads = (lbaf >> 16) & 0xFF;
        if (nsze == 0) continue;                        /* inactive */
        if (lbads < 9 || lbads > 12 || (lbaf & 0xFFFF) != 0) {
            UsbLog(LOG_ERR, "nvme %02x:%02x.%u: namespace %u has %u byte blocks%s, not used\n", c->Bus, c->Dev,
                   c->Fn, nsid, 1U << lbads, (lbaf & 0xFFFF) ? " with metadata" : "");
            continue;
        }
        ns = &c->Ns[c->NNs++];
        ns->C = c;
        ns->Nsid = nsid;
        ns->Blocks = nsze;
        ns->BlockSize = 1UL << lbads;
        UsbLog(LOG_INFO, "nvme %02x:%02x.%u: namespace %u: %u MB, %u byte blocks\n", c->Bus, c->Dev, c->Fn, nsid,
               (ULONG)((nsze << lbads) >> 20), ns->BlockSize);
    }
    return USB_OK;
}

/* worker context: a controller found behind a hot plug port */
void NvmeHotAttach(UCHAR Bus, UCHAR Dev, UCHAR Fn)
{
    NVME_CTRL *c = NULL;
    ULONG bar, i;
    int r;

    for (i = 0; i < NVME_MAX; i++) {
        if (Ctrls[i].Used && !Ctrls[i].Gone && Ctrls[i].Bus == Bus && Ctrls[i].Dev == Dev && Ctrls[i].Fn == Fn) {
            return;                                     /* already ours */
        }
    }
    for (i = 0; i < NVME_MAX; i++) {
        if (!Ctrls[i].Used) {
            c = &Ctrls[i];
            break;
        }
    }
    if (c == NULL) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: more than %u NVMe controllers\n", Bus, Dev, Fn, NVME_MAX);
        return;
    }
    if (OsGetConfig("DisableNvme", 0)) {
        return;
    }
    UsbMemSet(c, 0, sizeof(*c));
    c->Used = TRUE;
    c->Bus = Bus;
    c->Dev = Dev;
    c->Fn = Fn;
    bar = OsPciRead(Bus, Dev, Fn, 0x10, 4);
    if ((bar & 1) || ((bar & 6) == 4 && OsPciRead(Bus, Dev, Fn, 0x14, 4) != 0) || (bar & ~0xFUL) == 0) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: BAR0 %08x not usable\n", Bus, Dev, Fn, bar);
        c->Used = FALSE;
        return;
    }
    c->Phys = bar & ~0xFUL;
    c->Len = 0x4000;
    c->Regs = (volatile UCHAR *)OsMapMmio(Bus, c->Phys, c->Len);
    if (c->Regs == NULL) {
        c->Used = FALSE;
        return;
    }
    /* memory decoding and bus mastering on, legacy interrupt off (polled) */
    OsPciWrite(Bus, Dev, Fn, 4, (OsPciRead(Bus, Dev, Fn, 4, 2) | 0x0006 | 0x0400), 2);
    r = Start(c);
    if (r != USB_OK || c->NNs == 0) {
        UsbLog(LOG_ERR, "nvme %02x:%02x.%u: not started (%d)\n", Bus, Dev, Fn, r);
        Wr(c, REG_CC, 0);
        Release(c);
        return;
    }
    for (i = 0; i < (ULONG)c->NNs; i++) {
        NVME_NS *ns = &c->Ns[i];
        char name[40];
        UsbFormat(name, sizeof(name), c->NNs > 1 ? "NVMe %s #%u" : "NVMe %s", c->Model, ns->Nsid);
        ns->Target = MscVirtAttach(name, c->Identity + ns->Nsid, c->MaxXfer, &NvmeTargetOps, ns);
        if (ns->Target != NULL) {
            MscVirtSetKind(ns->Target, BLK_KIND_NVME);
        }
    }
    UsbNotifyChange();
}

static void Detach(NVME_CTRL *c, BOOLEAN Orderly)
{
    int i;

    for (i = 0; i < c->NNs; i++) {
        if (c->Ns[i].Target != NULL) {
            MscVirtDetach(c->Ns[i].Target);
            c->Ns[i].Target = NULL;
        }
    }
    if (Orderly && !c->Down && !Gone(c)) {
        /* normal shutdown: the drive saves its caches and mapping tables */
        ULONG t0 = OsTimeMs();
        Wr(c, REG_CC, (Rd(c, REG_CC) & ~0xC000UL) | CC_SHN_NORMAL);
        while ((Rd(c, REG_CSTS) & CSTS_SHST_MASK) != CSTS_SHST_DONE && OsTimeMs() - t0 < 5000) {
            OsSleepMs(10);
        }
        Wr(c, REG_CC, 0);
        OsPciWrite(c->Bus, c->Dev, c->Fn, 4, 0x0400, 2);
    }
    UsbLog(LOG_INFO, "nvme %02x:%02x.%u: %s\n", c->Bus, c->Dev, c->Fn, Orderly ? "shut down" : "removed");
    Release(c);
    UsbNotifyChange();
}

/* worker context: every 2 s with the hot plug check; controllers that read as
   all ones are gone (pulled out, tunnel torn down) */
void NvmeHotService(void)
{
    int i;
    for (i = 0; i < NVME_MAX; i++) {
        NVME_CTRL *c = &Ctrls[i];
        if (c->Used && (c->Gone || Gone(c) || OsPciRead(c->Bus, c->Dev, c->Fn, 0, 4) == 0xFFFFFFFFUL)) {
            c->Gone = TRUE;
            Detach(c, FALSE);
        }
    }
}

/* worker context: the slot of a hot plug port is stopped (button, eject):
   shut the drives behind buses First..Last down */
void NvmeHotStop(UCHAR First, UCHAR Last)
{
    int i;
    for (i = 0; i < NVME_MAX; i++) {
        NVME_CTRL *c = &Ctrls[i];
        if (c->Used && c->Bus >= First && c->Bus <= Last) {
            Detach(c, TRUE);
        }
    }
}

/* system shutdown: every drive gets a normal shutdown */
void NvmeShutdown(void)
{
    int i;
    for (i = 0; i < NVME_MAX; i++) {
        NVME_CTRL *c = &Ctrls[i];
        if (c->Used && !c->Gone && !c->Down && Rd(c, REG_CSTS) != 0xFFFFFFFFUL) {
            ULONG t0 = OsTimeMs();
            Wr(c, REG_CC, (Rd(c, REG_CC) & ~0xC000UL) | CC_SHN_NORMAL);
            while ((Rd(c, REG_CSTS) & CSTS_SHST_MASK) != CSTS_SHST_DONE && OsTimeMs() - t0 < 3000) {
                OsStallUs(1000);
            }
        }
    }
}

/* for usbtree: controller of slot target T (name, location) */
BOOLEAN NvmeHotInfo(int Index, char *Name, int Size, char *Detail, int DSize)
{
    NVME_CTRL *c;
    if (Index < 0 || Index >= NVME_MAX || !Ctrls[Index].Used) return FALSE;
    c = &Ctrls[Index];
    UsbFormat(Name, Size, "%s", c->Model);
    UsbFormat(Detail, DSize, "NVMe controller at PCI %02x:%02x.%u (hot plugged, polled)\nModel: %s\nSerial: %s\n"
              "Namespaces: %u\nLargest command: %u KB\n", c->Bus, c->Dev, c->Fn, c->Model, c->Serial, c->NNs,
              c->MaxXfer / 1024);
    return TRUE;
}
