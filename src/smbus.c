/*
 * smbus.c - SMBus host controllers, polled (no interrupt): only what the
 * touch pads on SMBus need (byte, byte data, block read / write).
 *
 * Intel ICH / PCH "i801" (ICH2 .. current PCH, the PCI ids of Linux's
 * i2c-i801.c): I/O registers at BAR 4, host configuration register 40h
 * (host enable on, I2C mode off); block transfers through the 32 byte block
 * buffer (ICH4 and later, E32B) or byte by byte (ICH2 / ICH3).  The INUSE_STS
 * semaphore is taken by the status read that starts each transfer and given
 * back afterwards; while the firmware holds it (or the host is busy) the
 * transfer waits.  SMBus hidden behind the P2SB is not handled.
 *
 * AMD SB800, Hudson-2, FCH "KERNCZ" (Linux's i2c-piix4.c): PIIX4 style
 * registers; the I/O base and the multiplexed port come from the power
 * management registers, indexed I/O at 0CD6h or, on KERNCZ revision 51h and
 * later, memory at FED80300h.  Each transfer takes the SMBus semaphore
 * (SMBSLVCNT bit 4 / 5), selects the port and restores the previous port
 * afterwards; block transfers tell the IMC (when it runs) to keep off the bus.
 *
 * Nothing but the touch pad addresses is ever addressed (smbtp.c).
 */

#include "smbtp.h"

#define MAX_HOST            4

/* i801 */
#define I_HSTSTS            0
#define I_HSTCNT            2
#define I_HSTCMD            3
#define I_HSTADD            4
#define I_HSTDAT0           5
#define I_HSTDAT1           6
#define I_BLKDAT            7
#define I_AUXSTS            12
#define I_AUXCTL            13

#define I_HSTCFG            0x40
#define HSTCFG_HST_EN       0x01
#define HSTCFG_SMI_EN       0x02
#define HSTCFG_I2C_EN       0x04

#define ST_BYTE_DONE        0x80
#define ST_INUSE            0x40
#define ST_SMBALERT         0x20
#define ST_FAILED           0x10
#define ST_BUS_ERR          0x08
#define ST_DEV_ERR          0x04
#define ST_INTR             0x02
#define ST_HOST_BUSY        0x01
#define ST_ERRORS           (ST_FAILED | ST_BUS_ERR | ST_DEV_ERR)
#define ST_FLAGS            (ST_BYTE_DONE | ST_INTR | ST_ERRORS)

#define CNT_KILL            0x02
#define CNT_LAST_BYTE       0x20
#define CNT_START           0x40

#define AUX_CRC             0x01
#define AUX_E32B            0x02

#define X_BYTE              0x04
#define X_BYTE_DATA         0x08
#define X_BLOCK             0x14

/* PIIX4 / SB800 */
#define P_SLVCNT            8
#define SB800_PM_IDX        0xCD6
#define FCH_PM_MMIO         0xFED80300UL
#define KERNCZ_IMC_IDX      0x3E
#define KERNCZ_IMC_DATA     0x3F

#define HT_I801             1
#define HT_FCH              2

#define OP_READ_BYTE_DATA   1
#define OP_WRITE_BYTE       2
#define OP_READ_BLOCK       3
#define OP_WRITE_BLOCK      4

struct _SMB_HOST {
    UCHAR       Type;
    UCHAR       Bus, Dev, Fn;
    USHORT      Vendor, Device;
    UCHAR       Rev;
    UCHAR       BlockBuffer;            /* i801: E32B */
    USHORT      Base;                   /* I/O registers */
    UCHAR       Ports;                  /* FCH: multiplexed main ports */
    UCHAR       PortReg, PortMask, PortShift;
    UCHAR       NotifyImc;
    UCHAR       UseMmio;
    UCHAR       *PmMmio;
    char        Name[48];
    /* the transfer in progress */
    UCHAR       Busy;
    UCHAR       Op;
    UCHAR       SavedPort;
    UCHAR       SlvCnt;
    UCHAR       WLen;
    UCHAR       WBuf[SMB_BLOCK_MAX];
    ULONG       Started;
    ULONG       InUseSince;
    ULONG       Transfers, Errors, Waits;
};

int SmbHostCount;
static SMB_HOST *Hosts[MAX_HOST];

#define HIN(h, r)        OsIn8((ULONG)(h)->Base + (r))
#define HOUT(h, r, v)    OsOut8((ULONG)(h)->Base + (r), (UCHAR)(v))

/* PCI ids of Linux's i2c-i801.c (the IDF functions of server chipsets left out) */
static const USHORT I801Ids[] = {
    0x2413, 0x2423, 0x2443, 0x2483, 0x24C3, 0x24D3, 0x25A4, 0x266A, 0x27DA, 0x269B, 0x283E, 0x2930, 0x5032,
    0x3A30, 0x3A60, 0x3B30, 0x1C22, 0x1D22, 0x2330, 0x1E22, 0x8C22, 0x9C22, 0x1F3C, 0x8D22, 0x23B0, 0x31D4,
    0x8CA2, 0x9CA2, 0x0F12, 0x2292, 0xA123, 0x9D23, 0x18DF, 0x19DF, 0x1BC9, 0x5AD4, 0xA1A3, 0xA223, 0xA2A3,
    0xA323, 0x9DA3, 0x34A3, 0x38A3, 0x02A3, 0x06A3, 0xA3A3, 0x4B23, 0xA0A3, 0x43A3, 0x4DA3, 0x7AA3, 0x51A3,
    0x54A3, 0x7A23, 0x7E22, 0xAE22, 0x7F23, 0x5796, 0x5827, 0x7722, 0xE322, 0xE422, 0x4D22, 0x6E23
};
/* ICH, ICH0, ICH2, ICH3: no block buffer */
static const USHORT I801NoBuf[] = { 0x2413, 0x2423, 0x2443, 0x2483 };

#define AMD_HUDSON2         0x780B
#define AMD_KERNCZ          0x790B
#define ATI_SBX00           0x4385

/* ------------------------------------------------------------------ */
/* AMD power management registers                                       */
/* ------------------------------------------------------------------ */

static UCHAR PmRead(SMB_HOST *h, UCHAR Reg)
{
    if (h->UseMmio && h->PmMmio != NULL) {
        return MmioRd8(h->PmMmio + Reg);
    }
    OsOut8(SB800_PM_IDX, Reg);
    return OsIn8(SB800_PM_IDX + 1);
}

static void PmWrite(SMB_HOST *h, UCHAR Reg, UCHAR Val)
{
    if (h->UseMmio && h->PmMmio != NULL) {
        MmioWr8(h->PmMmio + Reg, Val);
        return;
    }
    OsOut8(SB800_PM_IDX, Reg);
    OsOut8(SB800_PM_IDX + 1, Val);
}

/* port select; returns the previous selection (shifted, as in the register) */
static UCHAR PortSel(SMB_HOST *h, UCHAR Port)
{
    UCHAR v = PmRead(h, h->PortReg), n = (UCHAR)((v & ~h->PortMask) | (Port & h->PortMask));

    if (n != v) {
        PmWrite(h, h->PortReg, n);
    }
    return (UCHAR)(v & h->PortMask);
}

static void ImcWrite(UCHAR Idx, UCHAR Val)
{
    OsOut8(KERNCZ_IMC_IDX, Idx);
    OsOut8(KERNCZ_IMC_DATA, Val);
}

static UCHAR ImcRead(UCHAR Idx)
{
    OsOut8(KERNCZ_IMC_IDX, Idx);
    return OsIn8(KERNCZ_IMC_DATA);
}

/* IMC mailbox command 96h: B4h = the host takes the bus, B5h = gives it back */
static BOOLEAN ImcMailbox(UCHAR Flag)
{
    ULONG i;

    ImcWrite(0x82, 0x00);
    ImcWrite(0x83, Flag);
    ImcWrite(0x80, 0x96);
    for (i = 0; i < 200; i++) {
        if (ImcRead(0x82) == 0xFA) {
            return TRUE;
        }
        OsStallUs(100);
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* probing                                                              */
/* ------------------------------------------------------------------ */

static BOOLEAN InList(const USHORT *L, ULONG N, USHORT Id)
{
    ULONG i;

    for (i = 0; i < N; i++) {
        if (L[i] == Id) {
            return TRUE;
        }
    }
    return FALSE;
}

static SMB_HOST *NewHost(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, UCHAR Type)
{
    SMB_HOST *h;
    int i;

    for (i = 0; i < SmbHostCount; i++) {
        if (Hosts[i]->Bus == Bus && Hosts[i]->Dev == Dev && Hosts[i]->Fn == Fn) {
            return NULL;
        }
    }
    if (SmbHostCount >= MAX_HOST) {
        return NULL;
    }
    h = (SMB_HOST *)OsAlloc(sizeof(SMB_HOST));
    if (h == NULL) {
        return NULL;
    }
    h->Type = Type;
    h->Bus = Bus;
    h->Dev = Dev;
    h->Fn = Fn;
    h->Vendor = (USHORT)Id;
    h->Device = (USHORT)(Id >> 16);
    h->Rev = (UCHAR)OsPciRead(Bus, Dev, Fn, 8, 1);
    h->Ports = 1;
    return h;
}

static void I801Probe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id)
{
    ULONG bar = OsPciRead(Bus, Dev, Fn, 0x20, 4), cmd;
    UCHAR cfg;
    SMB_HOST *h;

    if (!(bar & 1) || (bar & 0xFFE0) == 0) {
        UsbLog(LOG_INFO, "smbus: Intel SMBus %04x at %02x:%02x.%u has no I/O base (hidden or off)\n",
               (USHORT)(Id >> 16), Bus, Dev, Fn);
        return;
    }
    h = NewHost(Bus, Dev, Fn, Id, HT_I801);
    if (h == NULL) {
        return;
    }
    h->Base = (USHORT)(bar & 0xFFE0);
    h->BlockBuffer = (UCHAR)!InList(I801NoBuf, sizeof(I801NoBuf) / sizeof(I801NoBuf[0]), h->Device);
    cmd = OsPciRead(Bus, Dev, Fn, 4, 2);
    if (!(cmd & 1)) {
        OsPciWrite(Bus, Dev, Fn, 4, cmd | 1, 2);           /* I/O decoding */
    }
    /* host enabled, SMBus (not I2C) timing, as Linux sets it */
    cfg = (UCHAR)OsPciRead(Bus, Dev, Fn, I_HSTCFG, 1);
    if ((cfg & (HSTCFG_HST_EN | HSTCFG_I2C_EN)) != HSTCFG_HST_EN) {
        OsPciWrite(Bus, Dev, Fn, I_HSTCFG, (cfg & ~HSTCFG_I2C_EN) | HSTCFG_HST_EN, 1);
    }
    if (h->BlockBuffer) {
        HOUT(h, I_AUXCTL, HIN(h, I_AUXCTL) & ~(AUX_CRC | AUX_E32B));
    }
    UsbFormat(h->Name, sizeof(h->Name), "Intel SMBus %04X at %02x:%02x.%u", h->Device, Bus, Dev, Fn);
    UsbLog(LOG_INFO, "smbus: %s, I/O %04x, host config %02x%s%s\n", h->Name, h->Base, cfg,
           h->BlockBuffer ? ", block buffer" : "", (cfg & HSTCFG_SMI_EN) ? ", SMI#" : "");
    Hosts[SmbHostCount++] = h;
}

static void FchProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id)
{
    SMB_HOST *h = NewHost(Bus, Dev, Fn, Id, HT_FCH);
    UCHAR smbEn, lo, hi, enabled;
    BOOLEAN kerncz;

    if (h == NULL) {
        return;
    }
    kerncz = (BOOLEAN)(h->Device == AMD_KERNCZ);
    /* PM registers through memory on KERNCZ 51h and later (0CD6h may be off there) */
    if (h->Vendor == 0x1022 && kerncz && h->Rev >= 0x51) {
        h->PmMmio = (UCHAR *)OsMapMmio(0, FCH_PM_MMIO, 8);
        h->UseMmio = (UCHAR)(h->PmMmio != NULL);
    }
    if ((h->Vendor == 0x1022 && h->Device == AMD_HUDSON2 && h->Rev >= 0x41) ||
        (h->Vendor == 0x1022 && kerncz && h->Rev >= 0x49) || (h->Vendor == 0x1D94 && kerncz)) {
        smbEn = 0x00;
    } else {
        smbEn = 0x2C;
    }
    lo = PmRead(h, smbEn);
    hi = PmRead(h, (UCHAR)(smbEn + 1));
    if (smbEn == 0) {
        enabled = (UCHAR)(lo & 0x10);
        h->Base = (USHORT)(hi << 8);
    } else {
        enabled = (UCHAR)(lo & 0x01);
        h->Base = (USHORT)(((hi << 8) | lo) & 0xFFE0);
    }
    if (!enabled || h->Base == 0) {
        UsbLog(LOG_INFO, "smbus: AMD SMBus %04x at %02x:%02x.%u not enabled\n", h->Device, Bus, Dev, Fn);
        if (h->PmMmio != NULL) {
            OsUnmapMmio(h->PmMmio, 8);
        }
        OsFree(h);
        return;
    }
    /* the port select register */
    if (h->Vendor == 0x1022 || h->Vendor == 0x1D94) {
        if (kerncz || (h->Device == AMD_HUDSON2 && h->Rev >= 0x1F)) {
            h->PortReg = 0x02;                              /* DecodeEn bits 20:19 */
            h->PortMask = 0x18;
            h->PortShift = 3;
            h->Ports = 2;
        } else {
            h->PortReg = 0x2E;
            h->PortMask = 0x06;
            h->PortShift = 1;
            h->Ports = 4;
        }
    } else {
        h->PortReg = (UCHAR)((PmRead(h, 0x2F) & 0x01) ? 0x2E : 0x2C);
        h->PortMask = 0x06;
        h->PortShift = 1;
        h->Ports = 4;
    }
    /* KERNCZ: is the IMC running (LPC bridge 00:14.3, register 40h bit 7)? */
    if (kerncz) {
        ULONG lpc = OsPciRead(0, 0x14, 3, 0, 4);
        if (((lpc & 0xFFFF) == 0x1022 || (lpc & 0xFFFF) == 0x1D94) && (OsPciRead(0, 0x14, 3, 0x40, 1) & 0x80)) {
            h->NotifyImc = 1;
        }
    }
    UsbFormat(h->Name, sizeof(h->Name), "AMD SMBus %04X rev %02X at %02x:%02x.%u", h->Device, h->Rev, Bus, Dev, Fn);
    UsbLog(LOG_INFO, "smbus: %s, I/O %04x, %u ports (select %02x), PM %s%s\n", h->Name, h->Base, h->Ports,
           h->PortReg, h->UseMmio ? "memory" : "0CD6h", h->NotifyImc ? ", IMC" : "");
    Hosts[SmbHostCount++] = h;
}

void SmbHostProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, ULONG Class)
{
    USHORT ven = (USHORT)Id, dev = (USHORT)(Id >> 16);

    (void)Class;
    if (ven == 0x8086 && InList(I801Ids, sizeof(I801Ids) / sizeof(I801Ids[0]), dev)) {
        I801Probe(Bus, Dev, Fn, Id);
    } else if ((ven == 0x1022 && (dev == AMD_HUDSON2 || dev == AMD_KERNCZ)) || (ven == 0x1D94 && dev == AMD_KERNCZ) ||
               (ven == 0x1002 && dev == ATI_SBX00 && OsPciRead(Bus, Dev, Fn, 8, 1) >= 0x40)) {
        FchProbe(Bus, Dev, Fn, Id);
    }
}

SMB_HOST *SmbHostGet(int Index)
{
    return (Index >= 0 && Index < SmbHostCount) ? Hosts[Index] : NULL;
}

int SmbHostPorts(SMB_HOST *H)
{
    return H->Ports;
}

const char *SmbHostName(SMB_HOST *H)
{
    return H->Name;
}

int SmbHostDescribe(SMB_HOST *H, char *Buf, int Size)
{
    return UsbFormat(Buf, Size, "%s io=%04x ports=%u transfers=%u errors=%u waits=%u", H->Type == HT_I801 ? "i801" : "fch",
                     H->Base, H->Ports, H->Transfers, H->Errors, H->Waits);
}

/* ------------------------------------------------------------------ */
/* transfers                                                            */
/* ------------------------------------------------------------------ */

/* takes the host for a transfer: semaphore, port */
static int Acquire(SMB_HOST *h, UCHAR Port, BOOLEAN Block)
{
    UCHAR st;

    if (h->Type == HT_I801) {
        st = HIN(h, I_HSTSTS);                               /* sets INUSE_STS when it was clear */
        if (st & ST_HOST_BUSY) {
            h->Waits++;
            return SMB_BUSY;
        }
        if (st & ST_INUSE) {
            /* held by the firmware; a semaphore nobody gives back for a second is a stale one */
            if (h->InUseSince == 0) {
                h->InUseSince = OsTimeMs() | 1;
            }
            if (OsTimeMs() - h->InUseSince < 1000) {
                h->Waits++;
                return SMB_BUSY;
            }
            UsbLog(LOG_INFO, "smbus: %s: INUSE_STS stays set, taken over\n", h->Name);
        }
        h->InUseSince = 0;
        if (st & ST_FLAGS) {
            HOUT(h, I_HSTSTS, st & ST_FLAGS);
        }
        if (h->BlockBuffer) {
            UCHAR a = HIN(h, I_AUXCTL);
            if (HIN(h, I_AUXSTS) & 1) {
                HOUT(h, I_AUXSTS, 1);
            }
            HOUT(h, I_AUXCTL, (a & ~(AUX_CRC | AUX_E32B)) | (Block ? AUX_E32B : 0));
        }
        return SMB_OK;
    }
    /* AMD: the SMBus semaphore (the IMC and the firmware share the controller) */
    h->SlvCnt = HIN(h, P_SLVCNT);
    HOUT(h, P_SLVCNT, h->SlvCnt | 0x10);
    h->SlvCnt = HIN(h, P_SLVCNT);
    if (!(h->SlvCnt & 0x10)) {
        h->Waits++;
        return SMB_BUSY;
    }
    if (Block && h->NotifyImc && !ImcMailbox(0xB4)) {
        UsbLog(LOG_INFO, "smbus: %s: the IMC does not answer, continuing without it\n", h->Name);
        h->NotifyImc = 0;
    }
    h->SavedPort = PortSel(h, (UCHAR)(Port << h->PortShift));
    st = HIN(h, I_HSTSTS);
    if (st != 0) {
        HOUT(h, I_HSTSTS, st);
        if (HIN(h, I_HSTSTS) & ST_HOST_BUSY) {
            PortSel(h, h->SavedPort);
            HOUT(h, P_SLVCNT, h->SlvCnt | 0x20);
            h->Waits++;
            return SMB_BUSY;
        }
    }
    return SMB_OK;
}

static void Release(SMB_HOST *h)
{
    if (h->Type == HT_I801) {
        if (h->BlockBuffer) {
            HOUT(h, I_AUXCTL, HIN(h, I_AUXCTL) & ~(AUX_CRC | AUX_E32B));
        }
        HOUT(h, I_HSTSTS, ST_INUSE | ST_FLAGS);              /* the semaphore goes back */
        return;
    }
    PortSel(h, h->SavedPort);
    HOUT(h, P_SLVCNT, h->SlvCnt | 0x20);
    if (h->NotifyImc && (h->Op == OP_READ_BLOCK || h->Op == OP_WRITE_BLOCK)) {
        ImcMailbox(0xB5);
    }
}

static int Start(SMB_HOST *h, UCHAR Port, UCHAR Op, UCHAR Addr, UCHAR Cmd, const UCHAR *W, ULONG Wl)
{
    BOOLEAN block = (BOOLEAN)(Op == OP_READ_BLOCK || Op == OP_WRITE_BLOCK);
    BOOLEAN read = (BOOLEAN)(Op == OP_READ_BYTE_DATA || Op == OP_READ_BLOCK);
    UCHAR x = Op == OP_WRITE_BYTE ? X_BYTE : Op == OP_READ_BYTE_DATA ? X_BYTE_DATA : X_BLOCK;
    ULONG i;
    int r;

    if (h->Busy) {
        return SMB_BUSY;
    }
    if (Port >= h->Ports || (block && Wl > SMB_BLOCK_MAX) || (Op == OP_WRITE_BLOCK && Wl == 0)) {
        return SMB_ERR;
    }
    r = Acquire(h, Port, block);
    if (r != SMB_OK) {
        return r;
    }
    h->Op = Op;
    h->WLen = (UCHAR)Wl;
    for (i = 0; i < Wl && i < SMB_BLOCK_MAX; i++) {
        h->WBuf[i] = W[i];
    }
    HOUT(h, I_HSTADD, (Addr << 1) | (read ? 1 : 0));
    HOUT(h, I_HSTCMD, Cmd);
    if (Op == OP_WRITE_BLOCK) {
        HOUT(h, I_HSTDAT0, Wl);
        (void)HIN(h, I_HSTCNT);                              /* block buffer index to 0 */
        if (h->Type == HT_FCH || h->BlockBuffer) {
            for (i = 0; i < Wl; i++) {
                HOUT(h, I_BLKDAT, W[i]);
            }
        } else {
            HOUT(h, I_BLKDAT, W[0]);
        }
    }
    h->Busy = 1;
    h->Started = OsTimeMs();
    h->Transfers++;
    if (h->Type == HT_FCH) {
        HOUT(h, I_HSTCNT, x);
        HOUT(h, I_HSTCNT, HIN(h, I_HSTCNT) | CNT_START);
        OsStallUs(250);                                     /* PIIX4 errata: wait before the first look */
    } else {
        HOUT(h, I_HSTCNT, x | CNT_START);
    }
    return SMB_OK;
}

static int ErrCode(UCHAR St)
{
    if (St & ST_DEV_ERR) return SMB_NACK;
    if (St & (ST_BUS_ERR | ST_FAILED)) return SMB_ERR;
    return SMB_OK;
}


/* i801 without block buffer (ICH2, ICH3): the bytes of a block transfer one by one, polled */
static int ByteByByte(SMB_HOST *h, UCHAR *Buf)
{
    BOOLEAN read = (BOOLEAN)(h->Op == OP_READ_BLOCK);
    ULONG len = read ? 0 : h->WLen, i, t;
    UCHAR st = 0;

    for (i = 1; read ? (len == 0 || i <= len) : i <= len; i++) {
        for (t = 0; t < 2000; t++) {                       /* ~20 ms per byte */
            st = HIN(h, I_HSTSTS);
            if (st & (ST_ERRORS | ST_BYTE_DONE)) {
                break;
            }
            OsStallUs(10);
        }
        if (!(st & ST_BYTE_DONE) || (st & ST_ERRORS)) {
            return (st & ST_ERRORS) ? ErrCode(st) : SMB_TIMEOUT;
        }
        if (read && len == 0) {
            len = HIN(h, I_HSTDAT0);
            if (len < 1 || len > SMB_BLOCK_MAX) {
                while (HIN(h, I_HSTSTS) & ST_HOST_BUSY) {
                    HOUT(h, I_HSTSTS, ST_BYTE_DONE);
                }
                HOUT(h, I_HSTSTS, ST_INTR);
                return SMB_PROTO;
            }
        }
        if (read) {
            Buf[i - 1] = HIN(h, I_BLKDAT);
            if (i == len - 1) {
                HOUT(h, I_HSTCNT, X_BLOCK | CNT_LAST_BYTE);
            }
        } else if (i < len) {
            HOUT(h, I_BLKDAT, h->WBuf[i]);
        }
        HOUT(h, I_HSTSTS, ST_BYTE_DONE);
    }
    return read ? (int)len : SMB_OK;
}

/* end of a transfer: results, status, semaphore; Buf gets the data of a read */
static int Finish(SMB_HOST *h, UCHAR St, UCHAR *Buf)
{
    int r = ErrCode(St), i, n;

    if (h->Type == HT_FCH && HIN(h, I_HSTSTS) != 0) {
        HOUT(h, I_HSTSTS, HIN(h, I_HSTSTS));
    } else if (h->Type == HT_I801) {
        HOUT(h, I_HSTSTS, St & ST_FLAGS);
    }
    if (r == SMB_OK) {
        if (h->Op == OP_READ_BYTE_DATA) {
            r = HIN(h, I_HSTDAT0);
        } else if (h->Op == OP_READ_BLOCK) {
            n = HIN(h, I_HSTDAT0);
            if (n < 1 || n > SMB_BLOCK_MAX) {
                r = SMB_PROTO;
            } else {
                (void)HIN(h, I_HSTCNT);                      /* block buffer index to 0 */
                for (i = 0; i < n; i++) {
                    Buf[i] = HIN(h, I_BLKDAT);
                }
                r = n;
            }
        }
    }
    Release(h);
    h->Busy = 0;
    if (r < 0) {
        h->Errors++;
        UsbLog(LOG_DBG, "smbus: %s: address %02x command %02x (%u) failed %d, status %02x\n", h->Name,
               HIN(h, I_HSTADD) >> 1, HIN(h, I_HSTCMD), h->Op, r, St);
    }
    return r;
}

/* one look at the transfer in progress */
static int Poll(SMB_HOST *h, UCHAR *Buf)
{
    UCHAR st;
    int r;

    if (!h->Busy) {
        return SMB_ERR;
    }
    if (h->Type == HT_I801 && !h->BlockBuffer && (h->Op == OP_READ_BLOCK || h->Op == OP_WRITE_BLOCK)) {
        r = ByteByByte(h, Buf);
        if (r < 0) {
            HOUT(h, I_HSTCNT, CNT_KILL);
            OsStallUs(100);
            HOUT(h, I_HSTCNT, 0);
            HOUT(h, I_HSTSTS, ST_FLAGS);
            Release(h);
            h->Busy = 0;
            h->Errors++;
            return r;
        }
        {
            ULONG t;
            for (t = 0; t < 2000; t++) {                   /* the end of the transfer */
                st = HIN(h, I_HSTSTS);
                if (!(st & ST_HOST_BUSY) && (st & (ST_INTR | ST_ERRORS))) {
                    break;
                }
                OsStallUs(10);
            }
        }
        HOUT(h, I_HSTSTS, st & ST_FLAGS);
        Release(h);
        h->Busy = 0;
        if (st & ST_ERRORS) {
            h->Errors++;
            return ErrCode(st);
        }
        return r;
    }
    st = HIN(h, I_HSTSTS);
    if (h->Type == HT_I801) {
        if (!(st & ST_HOST_BUSY) && (st & (ST_INTR | ST_ERRORS))) {
            return Finish(h, st, Buf);
        }
    } else if (!(st & ST_HOST_BUSY)) {
        return Finish(h, st, Buf);
    }
    if (OsTimeMs() - h->Started > 100) {
        /* stuck: stop it (i801 KILL), give the bus back */
        if (h->Type == HT_I801) {
            HOUT(h, I_HSTCNT, CNT_KILL);
            OsStallUs(500);
            HOUT(h, I_HSTCNT, 0);
        }
        HOUT(h, I_HSTSTS, h->Type == HT_I801 ? ST_FLAGS : HIN(h, I_HSTSTS));
        Release(h);
        h->Busy = 0;
        h->Errors++;
        UsbLog(LOG_ERR, "smbus: %s: transfer timed out (status %02x)\n", h->Name, st);
        return SMB_TIMEOUT;
    }
    return SMB_PENDING;
}

static int Sync(SMB_HOST *h, UCHAR Port, UCHAR Op, UCHAR Addr, UCHAR Cmd, const UCHAR *W, ULONG Wl, UCHAR *Buf)
{
    ULONG t0 = OsTimeMs();
    UCHAR tmp[SMB_BLOCK_MAX];
    int r;

    for (;;) {
        r = Start(h, Port, Op, Addr, Cmd, W, Wl);
        if (r != SMB_BUSY || OsTimeMs() - t0 > 200) {
            break;
        }
        OsStallUs(500);
    }
    if (r != SMB_OK) {
        return r;
    }
    for (;;) {
        r = Poll(h, Buf != NULL ? Buf : tmp);
        if (r != SMB_PENDING) {
            return r;
        }
        OsStallUs(50);
    }
}

int SmbReadByteData(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd)
{
    return Sync(H, Port, OP_READ_BYTE_DATA, Addr, Cmd, NULL, 0, NULL);
}

int SmbWriteByte(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd)
{
    return Sync(H, Port, OP_WRITE_BYTE, Addr, Cmd, NULL, 0, NULL);
}

int SmbReadBlock(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd, UCHAR *Buf)
{
    return Sync(H, Port, OP_READ_BLOCK, Addr, Cmd, NULL, 0, Buf);
}

int SmbWriteBlock(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd, const UCHAR *Buf, ULONG Len)
{
    return Sync(H, Port, OP_WRITE_BLOCK, Addr, Cmd, Buf, Len, NULL);
}

int SmbStartReadBlock(SMB_HOST *H, UCHAR Port, UCHAR Addr, UCHAR Cmd)
{
    return Start(H, Port, OP_READ_BLOCK, Addr, Cmd, NULL, 0);
}

int SmbCollect(SMB_HOST *H, UCHAR *Buf)
{
    return Poll(H, Buf);
}

BOOLEAN SmbHostIdle(SMB_HOST *H)
{
    return (BOOLEAN)!H->Busy;
}
