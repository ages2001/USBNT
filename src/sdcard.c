/*
 * sdcard.c - SD (and plain MMC) memory cards behind a card reader host.
 *
 * The card is brought up the standard way (CMD0, CMD8, ACMD41 / CMD1, CMD2,
 * CMD3, CMD9, CMD7, 4-bit bus, high speed when the card can switch) and then
 * answers SCSI block commands as a removable disk (usbblk.h).  Card insertion
 * and removal show up as a media change on the disk slot, so mounted volumes
 * are re-verified.  SDSC (byte addressed), SDHC and SDXC (block addressed)
 * cards are handled; eMMC is not targeted.
 */

#include "usbnt.h"
#include "usbscsi.h"
#include "usbblk.h"
#include "sdcard.h"

#define CARD_NONE   0
#define CARD_SD1    1
#define CARD_SD2    2
#define CARD_MMC    3

struct _SD_CARD {
    const SD_HOST_OPS *Ops;
    void       *Ctx;
    BLK_TARGET *Target;
    char        Name[24];
    ULONG       MaxBlocks;
    ULONG       MaxClock;
    BOOLEAN     Present;            /* card detect switch */
    BOOLEAN     Ready;              /* initialised */
    BOOLEAN     Failed;             /* initialisation failed for this insertion */
    BOOLEAN     Wp;
    UCHAR       Type;               /* CARD_* */
    BOOLEAN     HighCap;            /* block addressing */
    BOOLEAN     HighSpeed;
    UCHAR       Width;
    USHORT      Rca;
    ULONG       Blocks;             /* 512 byte blocks */
    ULONG       Cid[4];
    ULONG       Csd[4];
    ULONG       LastPoll;
    ULONG       Errors;
};

/* bits [hi:lo] of a 128 bit register held MSB first in r[0..3] */
static ULONG Bits(const ULONG *r, int hi, int lo)
{
    ULONG v = 0;
    int b;
    for (b = hi; b >= lo; b--) {
        int w = 3 - b / 32, s = b % 32;
        v = (v << 1) | ((r[w] >> s) & 1);
    }
    return v;
}

static int Cmd(SD_CARD *c, UCHAR idx, ULONG arg, UCHAR rsp, ULONG *resp)
{
    SD_REQ r;
    int st;
    UsbMemSet(&r, 0, sizeof(r));
    r.Cmd = idx;
    r.Arg = arg;
    r.Rsp = rsp;
    st = c->Ops->Request(c->Ctx, &r);
    if (resp != NULL) {
        resp[0] = r.Resp[0]; resp[1] = r.Resp[1]; resp[2] = r.Resp[2]; resp[3] = r.Resp[3];
    }
    return st;
}

static int AppCmd(SD_CARD *c, UCHAR idx, ULONG arg, UCHAR rsp, ULONG *resp)
{
    ULONG r[4];
    int st = Cmd(c, 55, (ULONG)c->Rca << 16, SD_RSP_R1, r);
    if (st != SD_OK) {
        return st;
    }
    if (!(r[0] & (1UL << 5))) {
        return SD_EIO;                              /* APP_CMD not accepted */
    }
    return Cmd(c, idx, arg, rsp, resp);
}

static int DataCmd(SD_CARD *c, UCHAR idx, ULONG arg, UCHAR *buf, ULONG blksz, ULONG blocks, BOOLEAN write, BOOLEAN stop)
{
    SD_REQ r;
    UsbMemSet(&r, 0, sizeof(r));
    r.Cmd = idx;
    r.Arg = arg;
    r.Rsp = SD_RSP_R1;
    r.Data = buf;
    r.BlkSize = blksz;
    r.Blocks = blocks;
    r.Write = (UCHAR)write;
    r.Stop = (UCHAR)stop;
    return c->Ops->Request(c->Ctx, &r);
}

/* waits until the card is back in the transfer state (after a write) */
static int WaitReady(SD_CARD *c, ULONG ms)
{
    ULONG t0 = OsTimeMs();
    ULONG r[4];
    for (;;) {
        int st = Cmd(c, 13, (ULONG)c->Rca << 16, SD_RSP_R1, r);
        if (st == SD_OK && (r[0] & (1UL << 8)) && ((r[0] >> 9) & 0xF) == 4) {
            return SD_OK;
        }
        if (st == SD_ENOMEDIUM || st == SD_ENODEV) {
            return st;
        }
        if (OsTimeMs() - t0 > ms) {
            return SD_ETIMEOUT;
        }
        OsSleepMs(1);
    }
}

static void CapacityFromCsd(SD_CARD *c)
{
    ULONG s = Bits(c->Csd, 127, 126);
    if (c->Type != CARD_MMC && s == 1) {
        c->Blocks = (Bits(c->Csd, 69, 48) + 1) * 1024;                 /* CSD 2.0: SDHC / SDXC */
    } else if (c->Type != CARD_MMC && s == 2) {
        c->Blocks = (Bits(c->Csd, 75, 48) + 1) * 1024;                 /* CSD 3.0: SDUC (> 2 TB: clipped) */
        if (Bits(c->Csd, 75, 70)) c->Blocks = 0xFFFFFFFFUL;
    } else {
        ULONG csize = Bits(c->Csd, 73, 62), mult = Bits(c->Csd, 49, 47), bl = Bits(c->Csd, 83, 80);
        ULONG blocks = (csize + 1) << (mult + 2);                       /* in READ_BL_LEN units */
        c->Blocks = bl >= 9 ? blocks << (bl - 9) : blocks >> (9 - bl);
    }
}

/* CMD6: high speed (function 1 of group 1) when the card offers it */
static void TryHighSpeed(SD_CARD *c)
{
    UCHAR st[64];
    if (c->Type == CARD_MMC || c->MaxClock < 50000000) {
        return;
    }
    if (DataCmd(c, 6, 0x00FFFFF1UL, st, 64, 1, FALSE, FALSE) != SD_OK || !(st[13] & 0x02)) {
        return;
    }
    if (DataCmd(c, 6, 0x80FFFFF1UL, st, 64, 1, FALSE, FALSE) != SD_OK || (st[16] & 0x0F) != 1) {
        return;
    }
    c->HighSpeed = TRUE;
}

static int CardInit(SD_CARD *c)
{
    ULONG r[4], ocr = 0, t0;
    int st, i;

    c->Ready = FALSE;
    c->Type = CARD_NONE;
    c->HighCap = FALSE;
    c->HighSpeed = FALSE;
    c->Width = SD_WIDTH_1;
    c->Rca = 0;
    c->Ops->SetIos(c->Ctx, FALSE, 0, SD_WIDTH_1, FALSE);
    OsSleepMs(20);
    st = c->Ops->SetIos(c->Ctx, TRUE, 400000, SD_WIDTH_1, FALSE);
    if (st != SD_OK) {
        return st;
    }
    OsSleepMs(10);                                  /* power ramp, 74 clocks */
    Cmd(c, 0, 0, SD_RSP_NONE, NULL);
    OsSleepMs(2);
    st = Cmd(c, 8, 0x1AA, SD_RSP_R1, r);
    if (st == SD_OK && (r[0] & 0xFFF) == 0x1AA) {
        c->Type = CARD_SD2;
    } else if (st == SD_ENOMEDIUM || st == SD_ENODEV) {
        return st;
    } else {
        c->Type = CARD_SD1;
        Cmd(c, 0, 0, SD_RSP_NONE, NULL);            /* some cards need a reset after an unknown command */
    }
    /* ACMD41 until the card leaves the busy state (spec: within 1 s) */
    t0 = OsTimeMs();
    for (i = 0;; i++) {
        st = AppCmd(c, 41, (c->Type == CARD_SD2 ? 0x40000000UL : 0) | 0x00FF8000UL, SD_RSP_R3, r);
        if (st != SD_OK) {
            break;
        }
        ocr = r[0];
        if (ocr & 0x80000000UL) {
            break;
        }
        if (OsTimeMs() - t0 > 1500) {
            st = SD_ETIMEOUT;
            break;
        }
        OsSleepMs(10);
    }
    if (st != SD_OK && c->Type == CARD_SD1) {
        /* no answer to ACMD41: an MMC card */
        Cmd(c, 0, 0, SD_RSP_NONE, NULL);
        c->Type = CARD_MMC;
        t0 = OsTimeMs();
        for (;;) {
            st = Cmd(c, 1, 0x40FF8000UL, SD_RSP_R3, r);
            if (st != SD_OK) {
                break;
            }
            ocr = r[0];
            if (ocr & 0x80000000UL) {
                break;
            }
            if (OsTimeMs() - t0 > 1500) {
                st = SD_ETIMEOUT;
                break;
            }
            OsSleepMs(10);
        }
    }
    if (st != SD_OK) {
        UsbLog(LOG_ERR, "%s: card does not power up (%d, ocr %08x)\n", c->Name, st, ocr);
        return st;
    }
    c->HighCap = (ocr & 0x40000000UL) != 0;
    st = Cmd(c, 2, 0, SD_RSP_R2, c->Cid);
    if (st != SD_OK) {
        return st;
    }
    if (c->Type == CARD_MMC) {
        c->Rca = 1;
        st = Cmd(c, 3, 1UL << 16, SD_RSP_R1, r);
    } else {
        st = Cmd(c, 3, 0, SD_RSP_R1, r);
        c->Rca = (USHORT)(r[0] >> 16);
    }
    if (st != SD_OK) {
        return st;
    }
    st = Cmd(c, 9, (ULONG)c->Rca << 16, SD_RSP_R2, c->Csd);
    if (st != SD_OK) {
        return st;
    }
    CapacityFromCsd(c);
    /* data transfer mode */
    c->Ops->SetIos(c->Ctx, TRUE, c->Type == CARD_MMC ? 20000000 : 25000000, SD_WIDTH_1, FALSE);
    st = Cmd(c, 7, (ULONG)c->Rca << 16, SD_RSP_R1B, r);
    if (st != SD_OK) {
        return st;
    }
    if (!c->HighCap) {
        Cmd(c, 16, 512, SD_RSP_R1, r);
    }
    if (c->Type != CARD_MMC) {
        AppCmd(c, 42, 0, SD_RSP_R1, r);             /* card detect pull-up off (ACMD42) */
        if (AppCmd(c, 6, 2, SD_RSP_R1, r) == SD_OK) {
            c->Width = SD_WIDTH_4;
        }
        c->Ops->SetIos(c->Ctx, TRUE, 25000000, c->Width, FALSE);
        TryHighSpeed(c);
        if (c->HighSpeed) {
            c->Ops->SetIos(c->Ctx, TRUE, 50000000, c->Width, TRUE);
        }
    }
    c->Ready = TRUE;
    c->Errors = 0;
    UsbLog(LOG_INFO, "%s: %s card %c%c%c%c%c, %u MB, %u-bit%s%s\n", c->Name,
           c->Type == CARD_MMC ? "MMC" : c->HighCap ? (c->Blocks > 67108864UL ? "SDXC" : "SDHC") : "SDSC",
           (char)(c->Cid[0] & 0xFF), (char)(c->Cid[1] >> 24), (char)(c->Cid[1] >> 16), (char)(c->Cid[1] >> 8),
           (char)(c->Cid[1]), c->Blocks / 2048, c->Width, c->HighSpeed ? ", high speed" : "",
           c->Wp ? ", write protected" : "");
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* block transfers                                                     */
/* ------------------------------------------------------------------ */
static int Rw(SD_CARD *c, ULONG lba, ULONG count, UCHAR *buf, BOOLEAN write)
{
    while (count != 0) {
        ULONG n = count > c->MaxBlocks ? c->MaxBlocks : count;
        ULONG addr = c->HighCap ? lba : lba * 512;
        int st, tries;
        for (tries = 0; tries < 3; tries++) {
            if (n == 1) {
                st = DataCmd(c, (UCHAR)(write ? 24 : 17), addr, buf, 512, 1, write, FALSE);
            } else {
                st = DataCmd(c, (UCHAR)(write ? 25 : 18), addr, buf, 512, n, write, TRUE);
            }
            if (st == SD_OK && write) {
                st = WaitReady(c, 2000);
            }
            if (st == SD_OK || st == SD_ENOMEDIUM || st == SD_ENODEV) {
                break;
            }
            c->Errors++;
            UsbLog(LOG_ERR, "%s: %s of %u blocks at %u failed (%d)\n", c->Name, write ? "write" : "read", n, lba, st);
            /* back to the transfer state before retrying */
            Cmd(c, 12, 0, SD_RSP_R1B, NULL);
            WaitReady(c, 500);
        }
        if (st != SD_OK) {
            return st;
        }
        lba += n;
        count -= n;
        buf += n * 512;
    }
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* SCSI target                                                         */
/* ------------------------------------------------------------------ */
#define SENSE_NOT_READY         0x02
#define SENSE_MEDIUM_ERROR      0x03
#define SENSE_ILLEGAL_REQUEST   0x05
#define SENSE_DATA_PROTECT      0x07

static void Put32(UCHAR *p, ULONG v) { p[0] = (UCHAR)(v >> 24); p[1] = (UCHAR)(v >> 16); p[2] = (UCHAR)(v >> 8); p[3] = (UCHAR)v; }
static ULONG Get32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

static void Copy(UCHAR *dst, ULONG len, const UCHAR *src, ULONG n, ULONG *done)
{
    ULONG k = n < len ? n : len;
    if (dst != NULL && k != 0) UsbMemCpy(dst, src, k);
    *done = k;
}

/* card present and working; FALSE with the sense filled in otherwise */
static BOOLEAN Usable(SD_CARD *c, UCHAR *Sense)
{
    if (!c->Present) {
        BlkSense(Sense, SENSE_NOT_READY, 0x3A, 0x00);
        return FALSE;
    }
    if (!c->Ready && !c->Failed) {
        if (CardInit(c) != SD_OK) {
            c->Failed = TRUE;
            c->Ops->SetIos(c->Ctx, FALSE, 0, SD_WIDTH_1, FALSE);
        }
    }
    if (!c->Ready) {
        BlkSense(Sense, SENSE_NOT_READY, 0x3A, 0x00);   /* unreadable card: like no medium */
        return FALSE;
    }
    return TRUE;
}

static int SdScsi(void *Ctx, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len, UCHAR *Data, ULONG *Done,
                  UCHAR *Sense)
{
    SD_CARD *c = (SD_CARD *)Ctx;
    UCHAR b[36];
    ULONG lba, cnt;
    int st;
    (void)CdbLen; (void)Dir;

    *Done = 0;
    switch (Cdb[0]) {
    case 0x12: {                                    /* INQUIRY */
        int i;
        UsbMemSet(b, ' ', sizeof(b));
        b[0] = 0x00; b[1] = 0x80; b[2] = 0x02; b[3] = 0x02; b[4] = 31;
        b[5] = 0; b[6] = 0; b[7] = 0;
        for (i = 0; i < 8 && "SD/MMC  "[i]; i++) b[8 + i] = (UCHAR)"SD/MMC  "[i];
        for (i = 0; i < 16 && c->Name[i]; i++) b[16 + i] = (UCHAR)c->Name[i];
        b[32] = '1'; b[33] = '.'; b[34] = '0'; b[35] = ' ';
        Copy(Data, Len, b, 36, Done);
        return BLK_OK;
    }
    case 0x00:                                      /* TEST UNIT READY */
        return Usable(c, Sense) ? BLK_OK : BLK_FAILED;
    case 0x25:                                      /* READ CAPACITY (10) */
        if (!Usable(c, Sense)) return BLK_FAILED;
        Put32(b, c->Blocks - 1);
        Put32(b + 4, 512);
        Copy(Data, Len, b, 8, Done);
        return BLK_OK;
    case 0x9E:                                      /* READ CAPACITY (16) */
        if (!Usable(c, Sense)) return BLK_FAILED;
        UsbMemSet(b, 0, 32);
        Put32(b + 4, c->Blocks - 1);
        Put32(b + 8, 512);
        Copy(Data, Len, b, 32, Done);
        return BLK_OK;
    case 0x1A: case 0x5A: {                         /* MODE SENSE (6) / (10) */
        UsbMemSet(b, 0, 8);
        if (Cdb[0] == 0x1A) { b[0] = 3; b[2] = c->Wp ? 0x80 : 0; Copy(Data, Len, b, 4, Done); }
        else { b[1] = 6; b[3] = c->Wp ? 0x80 : 0; Copy(Data, Len, b, 8, Done); }
        return BLK_OK;
    }
    case 0x08: case 0x0A:                           /* READ (6) / WRITE (6) */
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
        if (Get32(Cdb + 2)) { BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x21, 0); return BLK_FAILED; }
        lba = Get32(Cdb + 6);
        cnt = Get32(Cdb + 10);
    rw: {
        BOOLEAN write = (Cdb[0] & 0x02) != 0;
        if (!Usable(c, Sense)) return BLK_FAILED;
        if (cnt == 0) return BLK_OK;
        if (lba >= c->Blocks || cnt > c->Blocks - lba) {
            BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x21, 0);
            return BLK_FAILED;
        }
        if (Len < cnt * 512 || Data == NULL) {
            BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x24, 0);
            return BLK_FAILED;
        }
        if (write && c->Wp) {
            BlkSense(Sense, SENSE_DATA_PROTECT, 0x27, 0);
            return BLK_FAILED;
        }
        st = Rw(c, lba, cnt, Data, write);
        if (st == SD_OK) {
            *Done = cnt * 512;
            return BLK_OK;
        }
        if (st == SD_ENODEV) return BLK_NODEV;
        if (st == SD_ENOMEDIUM) {
            c->Present = FALSE;
            c->Ready = FALSE;
            BlkSense(Sense, SENSE_NOT_READY, 0x3A, 0);
        } else {
            /* the card may need a fresh start after errors */
            if (c->Errors > 6) c->Ready = FALSE;
            BlkSense(Sense, SENSE_MEDIUM_ERROR, write ? 0x0C : 0x11, 0);
        }
        return BLK_FAILED;
    }
    case 0x1B: case 0x1E: case 0x35: case 0x91: case 0x2F: /* START STOP, PREVENT, SYNC CACHE (10/16), VERIFY */
        if (Cdb[0] == 0x1B && !(Cdb[4] & 1) && (Cdb[4] & 2) && c->Ready) {
            /* eject: power the card down */
            c->Ready = FALSE;
            c->Ops->SetIos(c->Ctx, FALSE, 0, SD_WIDTH_1, FALSE);
        }
        return BLK_OK;
    default:
        BlkSense(Sense, SENSE_ILLEGAL_REQUEST, 0x20, 0);
        return BLK_FAILED;
    }
}

static const BLK_TARGET_OPS SdTargetOps = { SdScsi };

/* ------------------------------------------------------------------ */
SD_CARD *SdAttach(const char *Name, ULONG Identity, ULONG MaxBlocks, ULONG MaxClock, const SD_HOST_OPS *Ops,
                  void *Ctx)
{
    SD_CARD *c = (SD_CARD *)OsAlloc(sizeof(SD_CARD));
    int i;
    if (c == NULL) {
        return NULL;
    }
    for (i = 0; i < (int)sizeof(c->Name) - 1 && Name[i]; i++) {
        c->Name[i] = Name[i];
    }
    c->Ops = Ops;
    c->Ctx = Ctx;
    c->MaxBlocks = MaxBlocks ? MaxBlocks : 128;
    c->MaxClock = MaxClock ? MaxClock : 25000000;
    Ops->Status(Ctx, &c->Present, &c->Wp);
    c->LastPoll = OsTimeMs();
    if (c->Present && CardInit(c) != SD_OK) {
        c->Failed = TRUE;
        Ops->SetIos(Ctx, FALSE, 0, SD_WIDTH_1, FALSE);
    }
    c->Target = MscVirtAttach(c->Name, Identity, c->MaxBlocks * 512, &SdTargetOps, c);
    return c;
}

void SdDetach(SD_CARD *c)
{
    if (c == NULL) {
        return;
    }
    MscVirtDetach(c->Target);
    OsFree(c);
}

void SdCardChanged(SD_CARD *c)
{
    c->LastPoll = 0;
}

BOOLEAN SdPoll(SD_CARD *c)
{
    BOOLEAN present = FALSE, wp = FALSE;
    if (c == NULL) {
        return FALSE;
    }
    if (c->LastPoll && OsTimeMs() - c->LastPoll < 500) {
        return c->Present;
    }
    c->LastPoll = OsTimeMs();
    if (c->Ops->Status(c->Ctx, &present, &wp) != SD_OK) {
        return c->Present;
    }
    if (present != c->Present) {
        c->Present = present;
        c->Wp = wp;
        c->Ready = FALSE;
        c->Failed = FALSE;
        if (present) {
            UsbLog(LOG_INFO, "%s: card inserted\n", c->Name);
            OsSleepMs(50);                          /* contact bounce */
            if (CardInit(c) != SD_OK) {
                c->Failed = TRUE;
                c->Ops->SetIos(c->Ctx, FALSE, 0, SD_WIDTH_1, FALSE);
            }
        } else {
            UsbLog(LOG_INFO, "%s: card removed\n", c->Name);
            c->Ops->SetIos(c->Ctx, FALSE, 0, SD_WIDTH_1, FALSE);
        }
        MscVirtMediaChange(c->Target);
    }
    return c->Present;
}
