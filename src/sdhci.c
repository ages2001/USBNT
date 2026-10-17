/*
 * sdhci.c - SD Host Controller Interface (PCI class 08 05) card readers:
 * the readers built into most notebooks and chipsets (Intel, AMD, Ricoh,
 * O2 Micro, JMicron, Texas Instruments, Toshiba, VIA, ...).
 *
 * Programmed I/O through the buffer data port, polled status; every slot
 * of the function becomes an SD card (sdcard.c).
 */

#include "usbnt.h"
#include "sdcard.h"

/* registers */
#define R_SDMA          0x00
#define R_BLKSIZE       0x04
#define R_BLKCNT        0x06
#define R_ARG           0x08
#define R_XFER          0x0C
#define R_CMD           0x0E
#define R_RESP          0x10
#define R_BUFFER        0x20
#define R_PRESENT       0x24
#define R_HOSTCTL       0x28
#define R_POWER         0x29
#define R_CLOCK         0x2C
#define R_TIMEOUT       0x2E
#define R_RESET         0x2F
#define R_INT           0x30
#define R_ERR           0x32
#define R_INT_EN        0x34
#define R_ERR_EN        0x36
#define R_INT_SIG       0x38
#define R_ERR_SIG       0x3A
#define R_HOSTCTL2      0x3E
#define R_CAPS          0x40
#define R_CAPS1         0x44
#define R_VERSION       0xFE

#define PS_CMD_INHIBIT  0x00000001UL
#define PS_DAT_INHIBIT  0x00000002UL
#define PS_CARD         0x00010000UL
#define PS_STABLE       0x00020000UL
#define PS_WRITABLE     0x00080000UL

#define INT_CMD         0x0001
#define INT_XFER        0x0002
#define INT_WRITE_RDY   0x0010
#define INT_READ_RDY    0x0020
#define INT_INSERT      0x0040
#define INT_REMOVE      0x0080
#define INT_ERROR       0x8000

#define ERR_CMD_TIMEOUT 0x0001
#define ERR_CMD_CRC     0x0002
#define ERR_DATA_TIMEOUT 0x0010
#define ERR_DATA_CRC    0x0020

#define MAX_HOSTS       8

typedef struct _SDHCI {
    UCHAR      *Regs;
    UCHAR       Bus, Dev, Fn, Slot;
    USHORT      Vendor, Device;
    UCHAR       Version;
    ULONG       Caps;
    ULONG       BaseClock;          /* Hz */
    BOOLEAN     BrokenCd;           /* card detect does not work: assume a card */
    BOOLEAN     Gone;
    SD_CARD    *Card;
    char        Name[24];
} SDHCI;

static SDHCI *Hosts[MAX_HOSTS];
static int NumHosts;
int SdHostCount;

#define RD8(h, r)       MmioRd8((h)->Regs + (r))
#define RD16(h, r)      MmioRd16((h)->Regs + (r))
#define RD32(h, r)      MmioRd32((h)->Regs + (r))
#define WR8(h, r, v)    MmioWr8((h)->Regs + (r), v)
#define WR16(h, r, v)   MmioWr16((h)->Regs + (r), v)
#define WR32(h, r, v)   MmioWr32((h)->Regs + (r), v)

static BOOLEAN Reset(SDHCI *h, UCHAR mask)
{
    ULONG t0 = OsTimeMs();
    WR8(h, R_RESET, mask);
    while (RD8(h, R_RESET) & mask) {
        if (OsTimeMs() - t0 > 100) {
            return FALSE;
        }
        OsStallUs(10);
    }
    return TRUE;
}

static void SetClock(SDHCI *h, ULONG hz)
{
    ULONG div, t0;
    USHORT clk;
    WR16(h, R_CLOCK, 0);
    if (!hz) {
        return;
    }
    if (h->Version >= 2) {
        /* 10-bit divided clock: base / (2 N), N = 0 means base */
        div = 0;
        if (hz < h->BaseClock) {
            div = (h->BaseClock + 2 * hz - 1) / (2 * hz);
            if (div > 0x3FF) div = 0x3FF;
        }
        clk = (USHORT)(((div & 0xFF) << 8) | ((div & 0x300) >> 2));
    } else {
        /* 8-bit power of two divider: base / (2 N) */
        div = 1;
        while (div < 0x80 && h->BaseClock / (2 * div) > hz) div <<= 1;
        if (h->BaseClock <= hz) div = 0;
        clk = (USHORT)(div << 8);
    }
    clk |= 0x0001;                                      /* internal clock enable */
    WR16(h, R_CLOCK, clk);
    t0 = OsTimeMs();
    while (!(RD16(h, R_CLOCK) & 0x0002)) {             /* internal clock stable */
        if (OsTimeMs() - t0 > 20) {
            UsbLog(LOG_ERR, "%s: clock does not become stable\n", h->Name);
            break;
        }
        OsStallUs(10);
    }
    WR16(h, R_CLOCK, clk | 0x0004);                     /* SD clock on */
}

static int HostStatus(void *Ctx, BOOLEAN *Present, BOOLEAN *Wp)
{
    SDHCI *h = (SDHCI *)Ctx;
    ULONG ps = RD32(h, R_PRESENT);
    if (ps == 0xFFFFFFFFUL) {
        h->Gone = TRUE;
        return SD_ENODEV;
    }
    *Present = h->BrokenCd || (ps & PS_CARD) != 0;
    *Wp = !(ps & PS_WRITABLE);
    {
        /* acknowledge card insertion / removal (bits 6 and 7): some hosts hold
           back a new insertion until the removal was seen */
        USHORT st = RD16(h, R_INT);
        if (st != 0xFFFF && (st & 0x00C0)) WR16(h, R_INT, (USHORT)(st & 0x00C0));
    }
    if (h->BrokenCd) *Wp = FALSE;
    return SD_OK;
}

static int HostSetIos(void *Ctx, BOOLEAN Power, ULONG ClockHz, int Width, BOOLEAN HighSpeed)
{
    SDHCI *h = (SDHCI *)Ctx;
    UCHAR ctl;
    if (!Power) {
        SetClock(h, 0);
        WR8(h, R_POWER, 0);
        return SD_OK;
    }
    if (!(RD8(h, R_POWER) & 1)) {
        UCHAR v = (h->Caps & (1UL << 24)) ? 0x0E : (h->Caps & (1UL << 25)) ? 0x0C : 0x0E;
        WR8(h, R_POWER, v);
        WR8(h, R_POWER, (UCHAR)(v | 1));
        OsSleepMs(10);
    }
    ctl = RD8(h, R_HOSTCTL) & ~(0x02 | 0x04 | 0x18 | 0x20);
    if (Width == SD_WIDTH_4) ctl |= 0x02;
    if (HighSpeed && (h->Caps & (1UL << 21))) ctl |= 0x04;
    WR8(h, R_HOSTCTL, ctl);
    if (HighSpeed && !(h->Caps & (1UL << 21)) && ClockHz > 25000000) ClockHz = 25000000;
    SetClock(h, ClockHz);
    return SD_OK;
}

/* waits for any of the interrupt status bits in 'mask' (or an error) */
static int WaitInt(SDHCI *h, USHORT mask, ULONG ms, USHORT *errOut)
{
    ULONG t0 = OsTimeMs();
    for (;;) {
        USHORT st = RD16(h, R_INT);
        if (st == 0xFFFF) {
            h->Gone = TRUE;
            return SD_ENODEV;
        }
        if (st & INT_ERROR) {
            USHORT e = RD16(h, R_ERR);
            WR16(h, R_ERR, e);
            WR16(h, R_INT, st & (mask | INT_ERROR));
            if (errOut) *errOut = e;
            if (e & (ERR_CMD_TIMEOUT | ERR_DATA_TIMEOUT)) return SD_ETIMEOUT;
            if (e & (ERR_CMD_CRC | ERR_DATA_CRC)) return SD_ECRC;
            return SD_EIO;
        }
        if (st & mask) {
            WR16(h, R_INT, st & mask);
            return SD_OK;
        }
        if (OsTimeMs() - t0 > ms) {
            return SD_ETIMEOUT;
        }
        OsStallUs(5);
    }
}

static int HostRequest(void *Ctx, SD_REQ *r)
{
    SDHCI *h = (SDHCI *)Ctx;
    ULONG t0, mask = PS_CMD_INHIBIT, k, b;
    USHORT cmd, xfer = 0, err = 0;
    int st;

    if (h->Gone) return SD_ENODEV;
    if (!h->BrokenCd && !(RD32(h, R_PRESENT) & PS_CARD)) return SD_ENOMEDIUM;
    if (r->Data != NULL || r->Rsp == SD_RSP_R1B) mask |= PS_DAT_INHIBIT;
    if (r->Cmd == 12) mask = PS_CMD_INHIBIT;
    t0 = OsTimeMs();
    while (RD32(h, R_PRESENT) & mask) {
        if (OsTimeMs() - t0 > 500) {
            Reset(h, 0x06);
            break;
        }
        OsStallUs(10);
    }
    WR16(h, R_INT, 0xFFFF);
    WR16(h, R_ERR, 0xFFFF);
    switch (r->Rsp) {
    case SD_RSP_NONE: cmd = 0x00; break;
    case SD_RSP_R2:   cmd = 0x01 | 0x08; break;
    case SD_RSP_R3:   cmd = 0x02; break;
    case SD_RSP_R1B:  cmd = 0x03 | 0x08 | 0x10; break;
    default:          cmd = 0x02 | 0x08 | 0x10; break;
    }
    if (r->Data != NULL) {
        cmd |= 0x20;
        WR8(h, R_TIMEOUT, 0x0E);
        WR16(h, R_BLKSIZE, (USHORT)(r->BlkSize & 0xFFF));
        WR16(h, R_BLKCNT, (USHORT)r->Blocks);
        xfer = (USHORT)((r->Write ? 0 : 0x10) | (r->Blocks > 1 ? (0x20 | 0x02) : 0) | (r->Stop ? 0x04 : 0));
    }
    if (r->Cmd == 12) cmd |= 0xC0;                      /* abort command */
    WR32(h, R_ARG, r->Arg);
    WR16(h, R_XFER, xfer);
    WR16(h, R_CMD, (USHORT)(((USHORT)r->Cmd << 8) | cmd));
    st = WaitInt(h, INT_CMD, 1000, &err);
    if (st != SD_OK) {
        Reset(h, 0x06);                                 /* CMD and DAT lines */
        return st;
    }
    if (r->Rsp == SD_RSP_R2) {
        for (k = 0; k < 4; k++) {
            ULONG reg = R_RESP + (3 - k) * 4;
            r->Resp[k] = RD32(h, reg) << 8;
            if (k != 3) r->Resp[k] |= RD8(h, reg - 1);
        }
    } else if (r->Rsp != SD_RSP_NONE) {
        r->Resp[0] = RD32(h, R_RESP);
    }
    if (r->Data != NULL) {
        for (b = 0; b < r->Blocks; b++) {
            ULONG *p = (ULONG *)(r->Data + b * r->BlkSize), n = r->BlkSize / 4;
            st = WaitInt(h, (USHORT)(r->Write ? INT_WRITE_RDY : INT_READ_RDY), 1000, &err);
            if (st != SD_OK) break;
            if (r->Write) {
                for (k = 0; k < n; k++) WR32(h, R_BUFFER, p[k]);
            } else {
                for (k = 0; k < n; k++) p[k] = RD32(h, R_BUFFER);
            }
        }
        if (st == SD_OK) st = WaitInt(h, INT_XFER, r->Write ? 3000 : 1000, &err);
        if (st != SD_OK) {
            UsbLog(LOG_ERR, "%s: CMD%u data %d (err %04x, present %08x)\n", h->Name, r->Cmd, st, err,
                   RD32(h, R_PRESENT));
            Reset(h, 0x06);
            return st;
        }
    } else if (r->Rsp == SD_RSP_R1B) {
        st = WaitInt(h, INT_XFER, 3000, &err);          /* busy end */
        if (st != SD_OK && st != SD_ETIMEOUT) return st;
    }
    return SD_OK;
}

static const SD_HOST_OPS SdhciOps = { HostStatus, HostSetIos, HostRequest };

/* ------------------------------------------------------------------ */
static void AddSlot(UCHAR Bus, UCHAR Dev, UCHAR Fn, UCHAR Slot, ULONG Phys)
{
    SDHCI *h;
    ULONG maxClk;
    if (NumHosts >= MAX_HOSTS) return;
    h = (SDHCI *)OsAlloc(sizeof(SDHCI));
    if (h == NULL) return;
    h->Bus = Bus; h->Dev = Dev; h->Fn = Fn; h->Slot = Slot;
    h->Vendor = (USHORT)OsPciRead(Bus, Dev, Fn, 0, 2);
    h->Device = (USHORT)OsPciRead(Bus, Dev, Fn, 2, 2);
    h->Regs = (UCHAR *)OsMapMmio(Bus, Phys, 0x100);
    if (h->Regs == NULL) { OsFree(h); return; }
    if (RD32(h, R_PRESENT) == 0xFFFFFFFFUL) {
        UsbLog(LOG_ERR, "sdhci %02x:%02x.%u: registers not readable\n", Bus, Dev, Fn);
        OsFree(h);
        return;
    }
    if (!Reset(h, 0x01)) {
        UsbLog(LOG_ERR, "sdhci %02x:%02x.%u: reset failed\n", Bus, Dev, Fn);
    }
    h->Version = (UCHAR)(RD16(h, R_VERSION) & 0xFF);
    h->Caps = RD32(h, R_CAPS);
    h->BaseClock = ((h->Version >= 2) ? ((h->Caps >> 8) & 0xFF) : ((h->Caps >> 8) & 0x3F)) * 1000000UL;
    if (h->BaseClock == 0) h->BaseClock = 50000000;     /* not reported: the common value */
    /* every status visible, nothing signalled (polled) */
    WR16(h, R_INT_EN, 0x00FB | INT_ERROR);
    WR16(h, R_ERR_EN, 0x03FF);
    WR16(h, R_INT_SIG, 0);
    WR16(h, R_ERR_SIG, 0);
    WR8(h, R_TIMEOUT, 0x0E);
    if (OsGetConfig("SdBrokenCd", 0) & (1UL << NumHosts)) h->BrokenCd = TRUE;
    {
        static const struct { USHORT Vid; const char *Name; } vendors[] = {
            { 0x1180, "Ricoh " }, { 0x1217, "O2Micro " }, { 0x104C, "TI " }, { 0x197B, "JMicron " },
            { 0x8086, "Intel " }, { 0x1022, "AMD " }, { 0x17A0, "Genesys " }, { 0x10EC, "Realtek " },
            { 0x1524, "ENE " }, { 0x14E4, "Broadcom " }, { 0x1679, "Tokyo Electron " }, { 0x1106, "VIA " },
            { 0x1039, "SiS " }, { 0x10B9, "ALi " }, { 0x1A3B, "AzureWave " }
        };
        static const char base[] = "SD Card Reader";
        const char *v = "";
        int k = 0, j;
        for (j = 0; j < (int)(sizeof(vendors) / sizeof(vendors[0])); j++) {
            if (vendors[j].Vid == h->Vendor) v = vendors[j].Name;
        }
        for (j = 0; v[j] && k < 8; j++) h->Name[k++] = v[j];
        for (j = 0; base[j]; j++) h->Name[k++] = base[j];
        if (NumHosts) { h->Name[k++] = ' '; h->Name[k++] = (char)('1' + NumHosts); }
        h->Name[k] = 0;
    }
    maxClk = (h->Caps & (1UL << 21)) ? 50000000 : 25000000;
    UsbLog(LOG_INFO, "sdhci %02x:%02x.%u slot %u: %04x:%04x, spec %u.00, base clock %u MHz, caps %08x%s\n", Bus, Dev,
           Fn, Slot, h->Vendor, h->Device, h->Version + 1, h->BaseClock / 1000000, h->Caps,
           h->BrokenCd ? ", no card detect" : "");
    Hosts[NumHosts++] = h;
    SdHostCount++;
    h->Card = SdAttach(h->Name, ((ULONG)h->Vendor << 16) ^ h->Device ^ ((ULONG)Slot << 24) ^ 0x5D00000UL,
                       128, maxClk, &SdhciOps, h);
}

void SdhciProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG ClassCode)
{
    ULONG info = OsPciRead(Bus, Dev, Fn, 0x40, 1), first = info & 7, slots = ((info >> 4) & 7) + 1, s, cmd;
    (void)ClassCode;
    if (OsGetConfig("DisableSd", 0)) return;
    if (first > 5) first = 0;
    cmd = OsPciRead(Bus, Dev, Fn, 4, 2);
    OsPciWrite(Bus, Dev, Fn, 4, (cmd | 0x0006) & ~0x0400UL, 2);   /* memory + bus master, INTx on */
    for (s = 0; s < slots && first + s < 6; s++) {
        ULONG off = 0x10 + (first + s) * 4, bar = OsPciRead(Bus, Dev, Fn, off, 4);
        if (bar & 1) continue;
        if ((bar & 6) == 4 && OsPciRead(Bus, Dev, Fn, off + 4, 4) != 0) {
            ULONG nb = UsbPciRelocateBar(Bus, Dev, Fn, off);
            if (!nb) {
                UsbLog(LOG_ERR, "sdhci %02x:%02x.%u: BAR above 4 GB\n", Bus, Dev, Fn);
                continue;
            }
            bar = nb;
        }
        if (!(bar & ~0xFFUL)) continue;
        AddSlot(Bus, Dev, Fn, (UCHAR)s, bar & ~0xFFUL);
    }
}

void SdHostService(void)
{
    int i;
    for (i = 0; i < NumHosts; i++) {
        if (Hosts[i]->Card != NULL && !Hosts[i]->Gone) SdPoll(Hosts[i]->Card);
    }
    RtsxPciService();
}
