/*
 * i2chid.c - HID over I2C: touch pads, touch screens, pens and keyboards
 * on the I2C buses of notebooks and tablets.
 *
 * Controllers: the Synopsys DesignWare I2C master found in Intel LPSS
 * (Haswell .. current, Bay Trail / Cherry Trail / Atom) as a PCI function,
 * Intel and AMD controllers that the firmware only shows in ACPI
 * (Memory32Fixed resource of INT33C2, INT3432, 80860F41, 808622C1, AMD0010,
 * AMDI0010 ...), and the fixed AMD FCH addresses.
 *
 * Devices: the ACPI definition blocks are searched for devices with the
 * HID over I2C compatible id (PNP0C50 / ACPI0C50) and their I2cSerialBus
 * resource (address, speed, controller); the HID descriptor register comes
 * from their _DSM when it is a constant. Firmware that patches the address
 * at run time is covered by trying the usual addresses of touch pads and
 * touch screens (registry I2cHid = 2 turns that off, 0 turns HID over I2C
 * off). A device counts as found only when its 30 byte HID descriptor is
 * valid.
 *
 * Windows NT cannot run the ACPI methods and has no GPIO interrupt for the
 * device, so the input register is polled (non blocking: the read is put
 * into the controller FIFO and collected on the next pass), every 8 ms
 * while the device reports and every 30 ms when it is idle.
 *
 * Reports go through the USB HID code (usbhid.c): touch pads in their
 * default mouse mode report like a mouse, touch screens and pens become an
 * absolute pointer (tip switch = left button), keyboards are turned into
 * boot reports.
 */

#include "usbinput.h"
#include "acpi.h"
#include "aml.h"
#include "gpio.h"
#include "ptp.h"

/* DesignWare registers */
#define DW_CON              0x00
#define DW_TAR              0x04
#define DW_DATA_CMD         0x10
#define DW_SS_HCNT          0x14
#define DW_SS_LCNT          0x18
#define DW_FS_HCNT          0x1C
#define DW_FS_LCNT          0x20
#define DW_INTR_MASK        0x30
#define DW_RAW_INTR         0x34
#define DW_CLR_INTR         0x40
#define DW_CLR_TX_ABRT      0x54
#define DW_CLR_STOP_DET     0x60
#define DW_ENABLE           0x6C
#define DW_STATUS           0x70
#define DW_TXFLR            0x74
#define DW_RXFLR            0x78
#define DW_SDA_HOLD         0x7C
#define DW_ABRT_SOURCE      0x80
#define DW_ENABLE_STATUS    0x9C
#define DW_COMP_PARAM1      0xF4
#define DW_COMP_VERSION     0xF8
#define DW_COMP_TYPE        0xFC
#define DW_TYPE_VALUE       0x44570140UL

#define CON_MASTER          0x0001
#define CON_STD             0x0002
#define CON_FAST            0x0004
#define CON_RESTART_EN      0x0020
#define CON_SLAVE_DISABLE   0x0040

#define CMD_READ            0x0100
#define CMD_STOP            0x0200
#define CMD_RESTART         0x0400

#define INT_TX_ABRT         0x0040
#define INT_STOP_DET        0x0200

#define ST_ACTIVITY         0x0001
#define ST_TFE              0x0004
#define ST_RFNE             0x0008

/* HID over I2C */
#define OP_RESET            0x01
#define OP_SET_POWER        0x08
#define PWR_ON              0x00
#define PWR_SLEEP           0x01

#define I2C_OK              0
#define I2C_NACK            (-20)
#define I2C_ERR             (-21)
#define I2C_TIMEOUT         (-22)
#define I2C_PENDING         1

#define MAX_CTRL            12
#define MAX_HID             8
#define MAX_CAND            24
#define MAX_READ            512

typedef struct _DW_CTRL {
    UCHAR      *Regs;
    ULONG       Phys;
    UCHAR       Bus, Dev, Fn;
    UCHAR       Pci;
    USHORT      Vendor, Device;
    ULONG       ClkMhz;
    ULONG       TxDepth, RxDepth;
    UCHAR       BiosFast;
    UCHAR       Busy;               /* an asynchronous read is in the FIFO */
    UCHAR       BusyLen;
    void       *Owner;
    UCHAR       Errors;
    ULONG       BusySince;
    char        AcpiName[5];
    char        Name[40];
} DW_CTRL;

typedef struct _I2C_HID {
    DW_CTRL    *C;
    UCHAR       Addr;
    UCHAR       Fast;
    USHORT      DescReg;
    USHORT      RdLen, RdReg, InReg, MaxIn, OutReg, MaxOut, CmdReg, DataReg, Vid, Pid, Ver;
    void       *Mouse;
    void       *Kbd;
    HID_MOUSE_LAYOUT ML;
    HID_KBD_LAYOUT   KL;
    UCHAR       HasMouse, HasKbd;
    UCHAR       PtpOn;              /* multi-touch mode: gestures (ptp.c) */
    PTP_STATE  *Ptp;
    UCHAR       Repeats;            /* the device answers polls with its last report again */
    UCHAR       Dead;
    char        AcpiPath[48];       /* ACPI device path for _PS0 / _PS3 (empty: none) */
    char        CtrlPath[48];       /* the I2C controller's ACPI path for its _PS0 */
    GPIO_PIN    IntPin;             /* GpioInt "data ready" line */
    UCHAR       HasInt;
    UCHAR       IntActiveLow;
    GPIO_PIN    RstPin;             /* GpioIo reset / power line */
    UCHAR       HasRst;
    USHORT      ReadLen;
    ULONG       NextPoll;
    ULONG       LastActive;
    ULONG       Errors;
    ULONG       Reports;
    ULONG       RetryAt;
    ULONG       LastLen;
    ULONG       SameCount;
    UCHAR       Last[64];
    UCHAR       Buf[MAX_READ];
    char        Name[48];
} I2C_HID;

typedef struct _I2C_CAND {
    UCHAR       Addr;
    UCHAR       Fast;
    UCHAR       NRegs;
    UCHAR       FromAcpi;
    USHORT      Regs[4];
    char        Ctrl[5];
    char        Hid[10];
    /* from the AML interpreter (conditional _CRS / _DSM / _PS0 / GPIO) */
    UCHAR       FromAml;
    char        Path[48];           /* device path, for _PS0 / _PS3 */
    char        CtrlPath[48];       /* I2C controller path, for its _PS0 */
    UCHAR       HasInt, IntActiveLow, IntLevel;
    char        IntCtrl[5];
    USHORT      IntPin;
    UCHAR       HasRst, RstActiveLow;
    char        RstCtrl[5];
    USHORT      RstPin;
} I2C_CAND;

int I2cHidCount;
static DW_CTRL *Ctrls[MAX_CTRL];
static int NCtrl;
static I2C_HID *Hids[MAX_HID];
static int NHid;
static I2C_CAND Cands[MAX_CAND];
static int NCand;
static ULONG Mode;
static BOOLEAN ModeRead;

static ULONG GetMode(void)
{
    if (!ModeRead) {
        ModeRead = TRUE;
        Mode = OsGetConfig("I2cHid", 1);
    }
    return Mode;
}

ULONG UsbPollWantMs;

#define RD(c, r)        MmioRd32((c)->Regs + (r))
#define WR(c, r, v)     MmioWr32((c)->Regs + (r), (v))

/* ------------------------------------------------------------------ */
/* Controller tables                                                    */
/* ------------------------------------------------------------------ */

/* Intel LPSS I2C functions (Sunrise Point and later) and their input clock
   in MHz, from the Linux intel-lpss-pci driver */
static const struct { USHORT Id; USHORT Mhz; } LpssIds[] = {
#include "lpssids.h"
};

/* Bay Trail, Cherry Trail / Braswell, Haswell / Broadwell LPSS: private
   registers at 0x800, fixed SCL counts */
static const USHORT OldLpss[] = {
    0x0F41, 0x0F42, 0x0F43, 0x0F44, 0x0F45, 0x0F46, 0x0F47,
    0x22C1, 0x22C2, 0x22C3, 0x22C4, 0x22C5, 0x22C6, 0x22C7,
    0x9C61, 0x9C62, 0x9CE1, 0x9CE2
};

static ULONG LpssClock(USHORT Id)
{
    ULONG i;

    for (i = 0; i < sizeof(LpssIds) / sizeof(LpssIds[0]); i++) {
        if (LpssIds[i].Id == Id) {
            return LpssIds[i].Mhz;
        }
    }
    return 0;
}

static BOOLEAN IsOldLpss(USHORT Id)
{
    ULONG i;

    for (i = 0; i < sizeof(OldLpss) / sizeof(OldLpss[0]); i++) {
        if (OldLpss[i] == Id) {
            return TRUE;
        }
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* DesignWare master                                                    */
/* ------------------------------------------------------------------ */

static int DwDisable(DW_CTRL *c)
{
    int t;

    for (t = 0; t < 200; t++) {
        WR(c, DW_ENABLE, t < 100 ? 0 : 2);
        if (!(RD(c, DW_ENABLE_STATUS) & 1)) {
            return I2C_OK;
        }
        OsStallUs(25);
    }
    return I2C_TIMEOUT;
}

static int DwSetup(DW_CTRL *c, UCHAR Addr, UCHAR Fast)
{
    if (DwDisable(c) != I2C_OK) {
        return I2C_TIMEOUT;
    }
    WR(c, DW_CON, CON_MASTER | (Fast ? CON_FAST : CON_STD) | CON_RESTART_EN | CON_SLAVE_DISABLE);
    WR(c, DW_TAR, Addr);
    WR(c, DW_INTR_MASK, 0);
    (void)RD(c, DW_CLR_INTR);
    WR(c, DW_ENABLE, 1);
    return I2C_OK;
}

static int DwAbort(DW_CTRL *c)
{
    ULONG src = RD(c, DW_ABRT_SOURCE);

    (void)RD(c, DW_CLR_TX_ABRT);
    (void)RD(c, DW_CLR_INTR);
    return (src & 0x0F) ? I2C_NACK : I2C_ERR;
}

/* finishes a read that was put into the FIFO */
static int DwCollect(DW_CTRL *c, UCHAR *Buf, ULONG Len, BOOLEAN Wait)
{
    ULONG start = OsTimeMs();

    for (;;) {
        ULONG raw = RD(c, DW_RAW_INTR);
        if (raw & INT_TX_ABRT) {
            c->Busy = 0;
            return DwAbort(c);
        }
        if (RD(c, DW_RXFLR) >= Len) {
            ULONG i;
            for (i = 0; i < Len; i++) {
                Buf[i] = (UCHAR)RD(c, DW_DATA_CMD);
            }
            c->Busy = 0;
            (void)RD(c, DW_CLR_STOP_DET);
            return I2C_OK;
        }
        if (OsTimeMs() - start > 100 || OsTimeMs() - c->BusySince > 100) {
            c->Busy = 0;
            DwDisable(c);
            return I2C_TIMEOUT;
        }
        if (!Wait) {
            return I2C_PENDING;
        }
        OsStallUs(20);
    }
}

/* any write and / or read, waiting for the end; the FIFO is kept filled so
   that the controller does not end the transfer early */
static int DwXfer(DW_CTRL *c, UCHAR Addr, UCHAR Fast, const UCHAR *W, ULONG Wl, UCHAR *R, ULONG Rl)
{
    ULONG total = Wl + Rl;
    ULONG pushed = 0, got = 0;
    ULONG budget = 20000 + total * (Fast ? 60 : 200);
    ULONG waited = 0;
    OS_IRQL irql;
    int r = I2C_OK;

    if (c->Busy) {
        UCHAR tmp[64];
        DwCollect(c, tmp, c->BusyLen, TRUE);
    }
    if (total == 0) {
        return I2C_OK;
    }
    if (DwSetup(c, Addr, Fast) != I2C_OK) {
        return I2C_TIMEOUT;
    }
    irql = OsRaise();
    for (;;) {
        ULONG raw = RD(c, DW_RAW_INTR);
        if (raw & INT_TX_ABRT) {
            r = DwAbort(c);
            break;
        }
        while (pushed < total && RD(c, DW_TXFLR) < c->TxDepth &&
               (pushed < Wl || pushed - Wl - got < c->RxDepth - 1)) {
            ULONG cmd;
            if (pushed < Wl) {
                cmd = W[pushed];
            } else {
                cmd = CMD_READ;
                if (pushed == Wl && Wl != 0) {
                    cmd |= CMD_RESTART;
                }
            }
            if (pushed == total - 1) {
                cmd |= CMD_STOP;
            }
            WR(c, DW_DATA_CMD, cmd);
            pushed++;
        }
        while (got < Rl && (RD(c, DW_STATUS) & ST_RFNE)) {
            R[got++] = (UCHAR)RD(c, DW_DATA_CMD);
        }
        if (pushed == total && got == Rl && (raw & INT_STOP_DET)) {
            break;
        }
        if (pushed == total && got == Rl && (RD(c, DW_STATUS) & (ST_ACTIVITY | ST_TFE)) == ST_TFE) {
            if (RD(c, DW_RAW_INTR) & INT_TX_ABRT) {
                r = DwAbort(c);
            }
            break;
        }
        if (waited > budget) {
            r = I2C_TIMEOUT;
            break;
        }
        OsStallUs(10);
        waited += 10;
    }
    OsLower(irql);
    (void)RD(c, DW_CLR_STOP_DET);
    if (r == I2C_TIMEOUT) {
        DwDisable(c);
    }
    return r;
}

/* puts a plain read into the FIFO; DwCollect picks the data up later */
static int DwStartRead(DW_CTRL *c, UCHAR Addr, UCHAR Fast, ULONG Len, void *Owner)
{
    ULONG i;

    if (Len == 0 || Len > c->TxDepth || Len > c->RxDepth || Len > 64) {
        return I2C_ERR;
    }
    if (DwSetup(c, Addr, Fast) != I2C_OK) {
        return I2C_TIMEOUT;
    }
    for (i = 0; i < Len; i++) {
        WR(c, DW_DATA_CMD, CMD_READ | (i == Len - 1 ? CMD_STOP : 0));
    }
    c->Busy = 1;
    c->Owner = Owner;
    c->BusyLen = (UCHAR)Len;
    c->BusySince = OsTimeMs();
    return I2C_OK;
}

static ULONG Count(ULONG Mhz, ULONG Ns, ULONG Sub)
{
    ULONG v = (Mhz * Ns + 500) / 1000;
    return v > Sub + 8 ? v - Sub : 8;
}

static void DwInit(DW_CTRL *c, int Kind)
{
    ULONG p = RD(c, DW_COMP_PARAM1);
    ULONG ver = RD(c, DW_COMP_VERSION);

    c->TxDepth = ((p >> 16) & 0xFF) + 1;
    c->RxDepth = ((p >> 8) & 0xFF) + 1;
    if (p == 0 || c->TxDepth < 2 || c->RxDepth < 2) {
        c->TxDepth = c->RxDepth = 16;
    }
    c->BiosFast = (UCHAR)(((RD(c, DW_CON) >> 1) & 3) == 2);    /* 3 is the reset value */
    DwDisable(c);
    /* SCL counts: the firmware's when it set them, else from the clock */
    if (RD(c, DW_SS_HCNT) == 0 || RD(c, DW_SS_LCNT) == 0 || RD(c, DW_FS_HCNT) == 0 || RD(c, DW_FS_LCNT) == 0) {
        if (Kind == 1) {                            /* Bay Trail / Cherry Trail */
            WR(c, DW_SS_HCNT, 0x200); WR(c, DW_SS_LCNT, 0x200);
            WR(c, DW_FS_HCNT, 0x55); WR(c, DW_FS_LCNT, 0x99);
        } else if (Kind == 2) {                     /* Haswell / Broadwell */
            WR(c, DW_SS_HCNT, 0x1B0); WR(c, DW_SS_LCNT, 0x1FB);
            WR(c, DW_FS_HCNT, 0x48); WR(c, DW_FS_LCNT, 0xA0);
        } else {
            ULONG m = c->ClkMhz ? c->ClkMhz : 100;
            WR(c, DW_SS_HCNT, Count(m, 4000 + 300, 3));
            WR(c, DW_SS_LCNT, Count(m, 4700 + 300, 1));
            WR(c, DW_FS_HCNT, Count(m, 600 + 300, 3));
            WR(c, DW_FS_LCNT, Count(m, 1300 + 300, 1));
        }
        if (ver >= 0x3131312AUL && (RD(c, DW_SDA_HOLD) & 0xFFFF) == 0) {
            WR(c, DW_SDA_HOLD, Kind == 1 ? 6 : Kind == 2 ? 9 : Count(c->ClkMhz ? c->ClkMhz : 100, 300, 0));
        }
    }
    WR(c, DW_INTR_MASK, 0);
    (void)RD(c, DW_CLR_INTR);
}

static DW_CTRL *AddCtrl(ULONG Phys, UCHAR *Regs, const char *What)
{
    DW_CTRL *c;
    int i;

    for (i = 0; i < NCtrl; i++) {
        if (Ctrls[i]->Phys == Phys) {
            return NULL;
        }
    }
    if (NCtrl >= MAX_CTRL) {
        return NULL;
    }
    c = (DW_CTRL *)OsAlloc(sizeof(DW_CTRL));
    if (c == NULL) {
        return NULL;
    }
    c->Phys = Phys;
    c->Regs = Regs;
    UsbFormat(c->Name, sizeof(c->Name), "%s", What);
    Ctrls[NCtrl++] = c;
    return c;
}

static UCHAR FindCap(UCHAR Bus, UCHAR Dev, UCHAR Fn, UCHAR Id)
{
    UCHAR p;
    int n = 0;

    if (!(OsPciRead(Bus, Dev, Fn, 0x06, 2) & 0x10)) {
        return 0;
    }
    p = (UCHAR)(OsPciRead(Bus, Dev, Fn, 0x34, 1) & 0xFC);
    while (p && n++ < 48) {
        if (OsPciRead(Bus, Dev, Fn, p, 1) == Id) {
            return p;
        }
        p = (UCHAR)(OsPciRead(Bus, Dev, Fn, p + 1, 1) & 0xFC);
    }
    return 0;
}

/* called by the PCI scan for every function */
void I2cPciProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, ULONG Class)
{
    USHORT vid = (USHORT)Id, did = (USHORT)(Id >> 16);
    ULONG clk = 0, bar, priv = 0x200, rst = 7;
    int kind = 0;
    UCHAR *regs;
    UCHAR pm;
    DW_CTRL *c;

    if (GetMode() == 0 || vid != 0x8086) {
        return;
    }
    clk = LpssClock(did);
    if (IsOldLpss(did)) {
        kind = (did >= 0x9C00) ? 2 : 1;
        priv = 0x800;
        rst = 3;
        clk = (kind == 1) ? 133 : 100;
    } else if (clk == 0 && (Class >> 8) != 0x0C80) {
        return;
    }
    bar = OsPciRead(Bus, Dev, Fn, 0x10, 4);
    if ((bar & 1) || (bar & ~0xFFFUL) == 0) {
        return;
    }
    if (((bar >> 1) & 3) == 2 && OsPciRead(Bus, Dev, Fn, 0x14, 4) != 0) {
        bar = UsbPciRelocateBar(Bus, Dev, Fn, 0x10);
        if (bar == 0) {
            return;
        }
    }
    bar &= ~0xFFFUL;
    pm = FindCap(Bus, Dev, Fn, 0x01);
    if (pm && (OsPciRead(Bus, Dev, Fn, pm + 4, 2) & 3) != 0) {
        OsPciWrite(Bus, Dev, Fn, pm + 4, OsPciRead(Bus, Dev, Fn, pm + 4, 2) & ~3UL, 2);
        OsSleepMs(10);
    }
    OsPciWrite(Bus, Dev, Fn, 4, OsPciRead(Bus, Dev, Fn, 4, 2) | 0x0002, 2);
    regs = (UCHAR *)OsMapMmio(Bus, bar, 0x1000);
    if (regs == NULL) {
        return;
    }
    /* take the function out of reset (only the known LPSS functions) */
    if ((clk != 0 || kind != 0) && (MmioRd32(regs + priv + 4) & rst) != rst) {
        MmioWr32(regs + priv + 4, 0);
        OsStallUs(10);
        MmioWr32(regs + priv + 4, rst);
        OsStallUs(100);
    }
    if (MmioRd32(regs + DW_COMP_TYPE) != DW_TYPE_VALUE) {
        if (clk != 0 || kind != 0) {
            UsbLog(LOG_INFO, "i2c %02x:%02x.%u: %04x:%04x is not a DesignWare I2C controller (%08x)\n", Bus, Dev, Fn,
                   vid, did, MmioRd32(regs + DW_COMP_TYPE));
        }
        OsUnmapMmio(regs, 0x1000);
        return;
    }
    c = AddCtrl(bar, regs, "");
    if (c == NULL) {
        OsUnmapMmio(regs, 0x1000);
        return;
    }
    c->Pci = 1;
    c->Bus = Bus;
    c->Dev = Dev;
    c->Fn = Fn;
    c->Vendor = vid;
    c->Device = did;
    c->ClkMhz = clk;
    UsbFormat(c->Name, sizeof(c->Name), "%02x:%02x.%u", Bus, Dev, Fn);
    DwInit(c, kind);
}

/* ------------------------------------------------------------------ */
/* ACPI                                                                 */
/* ------------------------------------------------------------------ */

static BOOLEAN NameChar(UCHAR ch, BOOLEAN First)
{
    if ((ch >= 'A' && ch <= 'Z') || ch == '_') {
        return TRUE;
    }
    return (BOOLEAN)(!First && ch >= '0' && ch <= '9');
}

static BOOLEAN SegOk(const UCHAR *p)
{
    return (BOOLEAN)(NameChar(p[0], TRUE) && NameChar(p[1], FALSE) && NameChar(p[2], FALSE) && NameChar(p[3], FALSE));
}

static ULONG PkgLen(const UCHAR *p, ULONG Avail, ULONG *Hdr)
{
    ULONG n = p[0] >> 6, len, i;

    if (Avail < n + 1) {
        return 0;
    }
    if (n == 0) {
        *Hdr = 1;
        return p[0] & 0x3F;
    }
    len = p[0] & 0x0F;
    for (i = 0; i < n; i++) {
        len |= (ULONG)p[1 + i] << (4 + 8 * i);
    }
    *Hdr = n + 1;
    return len;
}

/* decodes a NameString; Last gets its last segment; returns its length */
static ULONG NameStr(const UCHAR *p, ULONG Avail, char *Last)
{
    ULONG o = 0, segs = 1;

    while (o < Avail && (p[o] == '\\' || p[o] == '^')) {
        o++;
    }
    if (o >= Avail) {
        return 0;
    }
    if (p[o] == 0x2E) {
        segs = 2;
        o++;
    } else if (p[o] == 0x2F) {
        if (o + 1 >= Avail) {
            return 0;
        }
        segs = p[o + 1];
        o += 2;
    } else if (p[o] == 0x00) {
        return 0;
    }
    if (segs == 0 || o + segs * 4 > Avail) {
        return 0;
    }
    while (segs--) {
        if (!SegOk(p + o)) {
            return 0;
        }
        if (segs == 0 && Last != NULL) {
            UsbMemCpy(Last, p + o, 4);
            Last[4] = 0;
        }
        o += 4;
    }
    return o;
}

typedef struct _AML_DEV {
    ULONG Body;     /* first byte after the name */
    ULONG End;
    char  Name[5];
} AML_DEV;

/* the innermost Device () that contains Pos */
static BOOLEAN DeviceAt(const UCHAR *T, ULONG Len, ULONG Pos, AML_DEV *D)
{
    ULONG i;
    BOOLEAN found = FALSE;

    for (i = 36; i + 8 < Len && i < Pos; i++) {
        ULONG hdr, plen, nl;
        char name[5];
        if (T[i] != 0x5B || T[i + 1] != 0x82) {
            continue;
        }
        plen = PkgLen(T + i + 2, Len - i - 2, &hdr);
        if (plen < hdr + 4 || i + 2 + plen > Len || i + 2 + plen <= Pos) {
            continue;
        }
        nl = NameStr(T + i + 2 + hdr, plen - hdr, name);
        if (nl == 0) {
            continue;
        }
        if (!found || i + 2 + hdr + nl > D->Body) {
            D->Body = i + 2 + hdr + nl;
            D->End = i + 2 + plen;
            UsbMemCpy(D->Name, name, 5);
            found = TRUE;
        }
    }
    return found;
}

static BOOLEAN Find(const UCHAR *T, ULONG From, ULONG To, const UCHAR *Pat, ULONG N, ULONG *At)
{
    ULONG i;

    for (i = From; i + N <= To; i++) {
        if (T[i] == Pat[0] && UsbMemCmp(T + i, Pat, N) == 0) {
            if (At != NULL) {
                *At = i;
            }
            return TRUE;
        }
    }
    return FALSE;
}

static void EisaStr(const UCHAR *b, char *Out)
{
    USHORT v = (USHORT)((b[0] << 8) | b[1]);
    static const char hex[] = "0123456789ABCDEF";

    Out[0] = (char)('@' + ((v >> 10) & 0x1F));
    Out[1] = (char)('@' + ((v >> 5) & 0x1F));
    Out[2] = (char)('@' + (v & 0x1F));
    Out[3] = hex[b[2] >> 4];
    Out[4] = hex[b[2] & 15];
    Out[5] = hex[b[3] >> 4];
    Out[6] = hex[b[3] & 15];
    Out[7] = 0;
}

/* the _HID of a device as text */
static void DevHid(const UCHAR *T, const AML_DEV *D, char *Out, ULONG Size)
{
    static const UCHAR pat[] = { 0x08, '_', 'H', 'I', 'D' };
    ULONG at, i;

    Out[0] = 0;
    if (!Find(T, D->Body, D->End, pat, 5, &at) || at + 6 >= D->End) {
        return;
    }
    at += 5;
    if (T[at] == 0x0D) {
        for (i = 0; i + 1 < Size && at + 1 + i < D->End && T[at + 1 + i] >= 0x20 && T[at + 1 + i] < 0x7F; i++) {
            Out[i] = (char)T[at + 1 + i];
        }
        Out[i] = 0;
    } else if (T[at] == 0x0C && at + 5 <= D->End && Size >= 8) {
        EisaStr(T + at + 1, Out);
    }
}

static BOOLEAN IsHidI2c(const UCHAR *T, const AML_DEV *D)
{
    static const UCHAR eisa[] = { 0x0C, 0x41, 0xD0, 0x0C, 0x50 };
    static const UCHAR s1[] = { 0x0D, 'P', 'N', 'P', '0', 'C', '5', '0', 0 };
    static const UCHAR s2[] = { 0x0D, 'A', 'C', 'P', 'I', '0', 'C', '5', '0', 0 };

    return (BOOLEAN)(Find(T, D->Body, D->End, eisa, sizeof(eisa), NULL) ||
                     Find(T, D->Body, D->End, s1, sizeof(s1), NULL) ||
                     Find(T, D->Body, D->End, s2, sizeof(s2), NULL));
}

/* constants returned by the _DSM of a HID over I2C device: the register of
   the HID descriptor */
static void DsmRegs(const UCHAR *T, const AML_DEV *D, I2C_CAND *Cd)
{
    static const UCHAR dsm[] = { '_', 'D', 'S', 'M' };
    ULONG at, i, end;

    ULONG from = D->Body, hs;

    for (;;) {
        if (!Find(T, from, D->End, dsm, 4, &at) || at < 2) {
            return;
        }
        /* Method (_DSM, 4, ...): 14 PkgLength _DSM */
        end = 0;
        for (hs = 1; hs <= 4 && at > hs; hs++) {
            ULONG hdr, plen;
            if (T[at - hs - 1] == 0x14 && (plen = PkgLen(T + at - hs, hs, &hdr)) != 0 && hdr == hs &&
                at - hs + plen <= D->End) {
                end = at - hs + plen;
                break;
            }
        }
        if (end != 0) {
            break;
        }
        from = at + 4;
    }
    for (i = at + 4; i + 2 < end && Cd->NRegs < 4; i++) {
        ULONG v;
        if (T[i] != 0xA4) {
            continue;
        }
        if (T[i + 1] == 0x0A) {
            v = T[i + 2];
        } else if (T[i + 1] == 0x0B && i + 3 < end) {
            v = GET16(T + i + 2);
        } else if (T[i + 1] == 0x01) {
            v = 1;
        } else {
            continue;
        }
        if (v != 0 && (Cd->NRegs == 0 || Cd->Regs[Cd->NRegs - 1] != v)) {
            Cd->Regs[Cd->NRegs++] = (USHORT)v;
        }
    }
}

static void AddCand(UCHAR Addr, UCHAR Fast, const char *Ctrl, const char *Hid, const USHORT *Regs, UCHAR NRegs,
                    UCHAR FromAcpi)
{
    int i;
    I2C_CAND *c;

    for (i = 0; i < NCand; i++) {
        if (Cands[i].Addr == Addr && (Ctrl[0] == 0 || UsbMemCmp(Cands[i].Ctrl, Ctrl, 4) == 0)) {
            return;
        }
    }
    if (NCand >= MAX_CAND) {
        return;
    }
    c = &Cands[NCand++];
    UsbMemSet(c, 0, sizeof(*c));
    c->Addr = Addr;
    c->Fast = Fast;
    c->FromAcpi = FromAcpi;
    UsbMemCpy(c->Ctrl, Ctrl, 5);
    UsbFormat(c->Hid, sizeof(c->Hid), "%s", Hid);
    for (i = 0; i < NRegs && i < 4; i++) {
        c->Regs[i] = Regs[i];
    }
    c->NRegs = NRegs;
}

/* DesignWare controllers that the firmware describes only in ACPI */
static const struct { const char *Hid; UCHAR Kind; UCHAR Mhz; } DwHids[] = {
    { "INT33C2", 2, 100 }, { "INT33C3", 2, 100 }, { "INT3432", 2, 100 }, { "INT3433", 2, 100 },
    { "80860F41", 1, 133 }, { "808622C1", 1, 133 }, { "AMD0010", 0, 133 }, { "AMDI0010", 0, 150 },
    { "AMDI0510", 0, 150 }, { "HYGO0010", 0, 150 }, { "INTC10EF", 0, 100 },
    { "AMDI0019", 9, 0 }                            /* shared with the PSP: never touched */
};
static ULONG Forbid[4];
static int NForbid;

static void AcpiScan(void)
{
    int ti;
    const ACPI_TABLE *t;

    for (ti = 0; (t = AcpiTable(ti)) != NULL; ti++) {
        const UCHAR *T = t->Va;
        ULONG len = t->Len, i;

        if (t->Sig != ACPI_SIG('D', 'S', 'D', 'T') && t->Sig != ACPI_SIG('S', 'S', 'D', 'T')) {
            continue;
        }
        for (i = 36; i + 20 < len; i++) {
            AML_DEV d;
            char hid[16];

            /* I2cSerialBus (V2) resource */
            if (T[i] == 0x8E && (T[i + 3] == 1 || T[i + 3] == 2) && T[i + 5] == 1 && GET16(T + i + 10) >= 6 &&
                GET16(T + i + 1) >= 15 && i + 3 + GET16(T + i + 1) <= len) {
                ULONG dl = GET16(T + i + 1);
                ULONG tl = GET16(T + i + 10);
                UCHAR addr = (UCHAR)(GET16(T + i + 16) & 0x7F);
                ULONG speed = GET32(T + i + 12);
                char ctrl[5];
                ULONG s = i + 12 + tl;
                ULONG k;
                I2C_CAND tmp;

                ctrl[0] = 0;
                /* resource source: "\_SB.PCI0.I2C1" -> I2C1, "\_SB.PCI0.S28" -> S28_ */
                {
                    ULONG seg = s, n;
                    for (k = s; k < i + 3 + dl && T[k] != 0; k++) {
                        if (T[k] == '.' || T[k] == '\\' || T[k] == '^') {
                            seg = k + 1;
                        }
                    }
                    n = k - seg;
                    if (k < i + 3 + dl && n >= 1 && n <= 4) {
                        ULONG m;
                        for (m = 0; m < 4; m++) {
                            ctrl[m] = (char)(m < n ? T[seg + m] : '_');
                        }
                        ctrl[4] = 0;
                        if (!SegOk((const UCHAR *)ctrl)) {
                            ctrl[0] = 0;
                        }
                    }
                }
                if (!DeviceAt(T, len, i, &d) || !IsHidI2c(T, &d)) {
                    i += 3 + dl - 1;
                    continue;
                }
                DevHid(T, &d, hid, sizeof(hid));
                UsbMemSet(&tmp, 0, sizeof(tmp));
                DsmRegs(T, &d, &tmp);
                if (addr >= 0x08 && addr <= 0x77 && !(GET16(T + i + 7) & 1)) {
                    UsbLog(LOG_INFO, "acpi: %s (%s) HID over I2C at %02x on %s, %u kHz%s\n", d.Name, hid, addr,
                           ctrl[0] ? ctrl : "?", speed / 1000, tmp.NRegs ? "" : ", descriptor register unknown");
                    AddCand(addr, (UCHAR)(speed >= 400000), ctrl, hid, tmp.Regs, tmp.NRegs, 1);
                } else {
                    UsbLog(LOG_DBG, "acpi: %s (%s) HID over I2C, address set at run time\n", d.Name, hid);
                }
                i += 3 + dl - 1;
                continue;
            }
            /* Memory32Fixed of a DesignWare controller device */
            if (T[i] == 0x86 && T[i + 1] == 9 && T[i + 2] == 0 && GET32(T + i + 4) >= 0x10000000UL &&
                GET32(T + i + 8) >= 0x100 && GET32(T + i + 8) <= 0x10000 && DeviceAt(T, len, i, &d)) {
                ULONG base = GET32(T + i + 4) & ~0xFFFUL;
                ULONG k;
                DevHid(T, &d, hid, sizeof(hid));
                for (k = 0; k < sizeof(DwHids) / sizeof(DwHids[0]); k++) {
                    const char *h = DwHids[k].Hid;
                    ULONG n = 0;
                    while (h[n] && h[n] == hid[n]) n++;
                    if (h[n] == 0 && hid[n] == 0) {
                        UCHAR *regs;
                        DW_CTRL *c;
                        BOOLEAN old = (BOOLEAN)(DwHids[k].Kind == 1 || DwHids[k].Kind == 2);
                        if (DwHids[k].Kind == 9) {
                            if (NForbid < 4) Forbid[NForbid++] = base;
                            break;
                        }
                        regs = (UCHAR *)OsMapMmio(0, base, 0x1000);
                        if (regs == NULL) {
                            break;
                        }
                        if (old && (MmioRd32(regs + 0x804) & 3) != 3 && MmioRd32(regs + 0x804) != 0xFFFFFFFFUL) {
                            MmioWr32(regs + 0x804, 3);
                            OsStallUs(100);
                        }
                        if (MmioRd32(regs + DW_COMP_TYPE) != DW_TYPE_VALUE ||
                            (c = AddCtrl(base, regs, "")) == NULL) {
                            OsUnmapMmio(regs, 0x1000);
                            break;
                        }
                        UsbFormat(c->Name, sizeof(c->Name), "%s %s", d.Name, hid);
                        UsbMemCpy(c->AcpiName, d.Name, 5);
                        c->ClkMhz = DwHids[k].Mhz;
                        DwInit(c, DwHids[k].Kind);
                        break;
                    }
                }
            }
        }
        /* names of the PCI controllers: Device (I2C1) { Name (_ADR, 0x00150001) } */
        for (i = 36; i + 12 < len; i++) {
            static const UCHAR adr[] = { 0x08, '_', 'A', 'D', 'R', 0x0C };
            ULONG v;
            int k;
            AML_DEV d;
            if (T[i] != 0x08 || UsbMemCmp(T + i, adr, 6) != 0) {
                continue;
            }
            v = GET32(T + i + 6);
            for (k = 0; k < NCtrl; k++) {
                DW_CTRL *c = Ctrls[k];
                if (c->Pci && c->AcpiName[0] == 0 && c->Bus == 0 && (v >> 16) == c->Dev && (v & 0xFFFF) == c->Fn &&
                    DeviceAt(T, len, i, &d)) {
                    UsbMemCpy(c->AcpiName, d.Name, 5);
                }
            }
        }
    }
}

/* AMD FCH I2C controllers at their fixed addresses (when ACPI did not show them) */
static void AmdFixed(void)
{
    static const ULONG bases[] = { 0xFEDC2000UL, 0xFEDC3000UL, 0xFEDC4000UL, 0xFEDC5000UL, 0xFEDC6000UL, 0xFEDCB000UL };
    ULONG i;

    if ((OsPciRead(0, 0, 0, 0, 4) & 0xFFFF) != 0x1022 || NCtrl != 0) {
        return;
    }
    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        UCHAR *regs;
        DW_CTRL *c;
        int f;
        for (f = 0; f < NForbid && Forbid[f] != bases[i]; f++) {
        }
        if (f < NForbid) {
            continue;
        }
        regs = (UCHAR *)OsMapMmio(0, bases[i], 0x1000);
        if (regs == NULL) {
            continue;
        }
        if (MmioRd32(regs + DW_COMP_TYPE) != DW_TYPE_VALUE || (c = AddCtrl(bases[i], regs, "")) == NULL) {
            OsUnmapMmio(regs, 0x1000);
            continue;
        }
        UsbFormat(c->Name, sizeof(c->Name), "AMD FCH I2C%u", i < 5 ? i : 5);
        c->ClkMhz = 150;
        DwInit(c, 0);
    }
}

/* ------------------------------------------------------------------ */
/* AML: conditional _CRS / _DSM / _STA / _PS0 / GPIO                     */
/* ------------------------------------------------------------------ */
static ULONG GpioMode(void)
{
    return OsGetConfig("I2cHidGpio", 1);
}

static BOOLEAN IdIs(const char *s, const char *want)
{
    while (*s && *want) {
        char a = *s++, b = *want++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return FALSE;
    }
    return (BOOLEAN)(*s == 0 && *want == 0);
}

static BOOLEAN IsHidI2cId(const char *hid, const char *cid)
{
    return (BOOLEAN)(IdIs(hid, "PNP0C50") || IdIs(cid, "PNP0C50") ||
                     IdIs(hid, "ACPI0C50") || IdIs(cid, "ACPI0C50"));
}

/* run a power method (_PS0 / _PS3) if the device has it */
static void RunPs(const char *path, const char *method)
{
    AML_VAL out;
    if (path == NULL || path[0] == 0 || !AmlHasObject(path, method)) {
        return;
    }
    if (AmlEvalMethod(path, method, NULL, 0, &out) == AML_OK) {
        UsbLog(LOG_DBG, "aml: %s.%s ran\n", path, method);
    }
    AmlFree(&out);
}

/* _DSM function 1 of a HID over I2C device: the HID descriptor register.
   Accepts an integer result or a Package whose first element is the integer. */
static BOOLEAN DsmReg(const char *path, USHORT *reg)
{
    static const UCHAR uuid[16] = { 0xF7, 0xF6, 0xDF, 0x3C, 0x67, 0x42, 0x55, 0x45,
                                    0xAD, 0x05, 0xB3, 0x0A, 0x3D, 0x89, 0x38, 0xDE };
    AML_VAL args[4], out;
    ULONGLONG v = 0;
    int r;
    if (!AmlHasObject(path, "_DSM")) {
        return FALSE;
    }
    UsbMemSet(args, 0, sizeof(args));
    args[0].Kind = AMLV_BUF;
    args[0].Buf = (UCHAR *)OsAlloc(16);
    if (args[0].Buf == NULL) {
        return FALSE;
    }
    UsbMemCpy(args[0].Buf, uuid, 16);
    args[0].Len = 16;
    AmlSetInt(&args[1], 1);                 /* revision */
    AmlSetInt(&args[2], 1);                 /* function index 1 */
    args[3].Kind = AMLV_PKG;                /* empty argument package */
    args[3].Len = 0;
    args[3].Pkg = (AML_VAL *)OsAlloc(sizeof(AML_VAL));
    r = AmlEvalMethod(path, "_DSM", args, 4, &out);
    AmlFree(&args[0]);
    AmlFree(&args[3]);
    if (r != AML_OK) {
        return FALSE;
    }
    if (out.Kind == AMLV_INT) {
        v = out.Int;
    } else if (out.Kind == AMLV_PKG) {
        const AML_VAL *e = AmlPkgElem(&out, 0);
        if (e) {
            AmlAsInt(e, &v);
        }
    }
    AmlFree(&out);
    if (v == 0 || v > 0xFFFF) {
        return FALSE;
    }
    *reg = (USHORT)v;
    return TRUE;
}

static void AddAmlCand(const I2C_CAND *src)
{
    int i;
    for (i = 0; i < NCand; i++) {
        if (Cands[i].Addr == src->Addr &&
            (src->Ctrl[0] == 0 || Cands[i].Ctrl[0] == 0 || UsbMemCmp(Cands[i].Ctrl, src->Ctrl, 4) == 0)) {
            /* enrich an address-only candidate found by the static scan */
            if (!Cands[i].FromAml) {
                UCHAR keepRegs = Cands[i].NRegs;
                USHORT r0 = Cands[i].Regs[0];
                Cands[i] = *src;
                if (src->NRegs == 0 && keepRegs) {
                    Cands[i].NRegs = keepRegs;
                    Cands[i].Regs[0] = r0;
                }
            }
            return;
        }
    }
    if (NCand >= MAX_CAND) {
        return;
    }
    Cands[NCand++] = *src;
}

static int AmlDevCb(const char *path, const char *hid, const char *cid, void *ctx)
{
    AML_VAL sta;
    AML_CRS crs;
    I2C_CAND cd;
    USHORT reg = 0;
    int i;
    (void)ctx;

    if (!IsHidI2cId(hid, cid)) {
        return TRUE;
    }
    /* _STA: skip a device the firmware says is not present */
    if (AmlHasObject(path, "_STA")) {
        ULONGLONG s = 0xF;
        if (AmlEvalMethod(path, "_STA", NULL, 0, &sta) == AML_OK) {
            AmlAsInt(&sta, &s);
        }
        AmlFree(&sta);
        if ((s & 1) == 0) {
            UsbLog(LOG_INFO, "aml: %s (%s) _STA=%x: absent, skipped\n", path, hid[0] ? hid : cid, (ULONG)s);
            return TRUE;
        }
    }
    /* _CRS: the I2C address / controller and any GpioInt / GpioIo lines */
    if (AmlEvalMethod(path, "_CRS", NULL, 0, &sta) != AML_OK) {
        UsbLog(LOG_DBG, "aml: %s: _CRS did not evaluate\n", path);
        return TRUE;
    }
    UsbMemSet(&crs, 0, sizeof(crs));
    AmlParseCrs(&sta, &crs);
    AmlFree(&sta);
    if (crs.NI2c < 1 || crs.I2c[0].Addr < 0x08 || crs.I2c[0].Addr > 0x77) {
        return TRUE;
    }
    UsbMemSet(&cd, 0, sizeof(cd));
    cd.FromAml = 1;
    cd.FromAcpi = 1;
    cd.Addr = (UCHAR)crs.I2c[0].Addr;
    cd.Fast = (UCHAR)(crs.I2c[0].Speed >= 400000);
    UsbMemCpy(cd.Ctrl, crs.I2c[0].Ctrl4, 5);
    {
        ULONG n = 0;
        while (path[n] && n + 1 < (int)sizeof(cd.Path)) { cd.Path[n] = path[n]; n++; }
        cd.Path[n] = 0;
    }
    /* the controller path (resource source of the I2C descriptor) for its _PS0 */
    {
        const char *src = crs.I2c[0].Ctrl;
        ULONG n = 0;
        while (src[n] && n + 1 < (int)sizeof(cd.CtrlPath)) { cd.CtrlPath[n] = src[n]; n++; }
        cd.CtrlPath[n] = 0;
    }
    {
        ULONG n = 0;
        while (hid[n] && n + 1 < (int)sizeof(cd.Hid)) { cd.Hid[n] = hid[n]; n++; }
        cd.Hid[n] = 0;
    }
    if (DsmReg(path, &reg)) {
        cd.Regs[cd.NRegs++] = reg;
        UsbLog(LOG_INFO, "aml: %s (%s) at %02x on %s, descriptor register %04x (_DSM)\n", path,
               cd.Hid[0] ? cd.Hid : "?", cd.Addr, cd.Ctrl[0] ? cd.Ctrl : "?", reg);
    } else {
        UsbLog(LOG_INFO, "aml: %s (%s) at %02x on %s, descriptor register from _DSM unknown\n", path,
               cd.Hid[0] ? cd.Hid : "?", cd.Addr, cd.Ctrl[0] ? cd.Ctrl : "?");
    }
    /* GpioInt (data ready) and the first GpioIo (reset / power) */
    for (i = 0; i < crs.NGpio; i++) {
        AML_GPIO_RES *g = &crs.Gpio[i];
        if (g->IsInt && !cd.HasInt) {
            cd.HasInt = 1;
            cd.IntPin = g->Pin;
            cd.IntActiveLow = g->ActiveLow;
            cd.IntLevel = g->Level;
            UsbMemCpy(cd.IntCtrl, g->Ctrl4, 5);
        } else if (!g->IsInt && !cd.HasRst) {
            cd.HasRst = 1;
            cd.RstPin = g->Pin;
            cd.RstActiveLow = g->ActiveLow;
            UsbMemCpy(cd.RstCtrl, g->Ctrl4, 5);
        }
    }
    AddAmlCand(&cd);
    return TRUE;
}

static void AmlScan(void)
{
    if (OsGetConfig("I2cHidAml", 1) == 0) {
        return;
    }
    if (AmlInit() == 0) {
        return;
    }
    AmlForEachDevice(AmlDevCb, NULL);
}

/* current candidate being probed, so Probe() can copy its AML / GPIO fields */
static I2C_CAND *g_CurCand;

/* bring a device out of reset and power it on through ACPI / GPIO before probing */
static void PrepCand(I2C_CAND *cd)
{
    if (!cd->FromAml) {
        return;
    }
    if (cd->HasRst && GpioMode() != 0) {
        GPIO_PIN rp;
        if (GpioResolvePin(cd->RstCtrl, cd->RstPin, &rp)) {
            /* assert reset, then release (deasserted level = NOT active) */
            GpioSet(&rp, (BOOLEAN)(cd->RstActiveLow ? 0 : 1));
            OsSleepMs(5);
            GpioSet(&rp, (BOOLEAN)(cd->RstActiveLow ? 1 : 0));
            OsSleepMs(10);
            UsbLog(LOG_INFO, "i2c: %s reset line %s/%u driven\n", cd->Path, cd->RstCtrl, cd->RstPin);
        }
    }
    RunPs(cd->CtrlPath, "_PS0");
    if (AmlHasObject(cd->Path, "_INI")) {
        AML_VAL out;
        if (AmlEvalMethod(cd->Path, "_INI", NULL, 0, &out) == AML_OK) {
            UsbLog(LOG_DBG, "aml: %s._INI ran\n", cd->Path);
        }
        AmlFree(&out);
    }
    RunPs(cd->Path, "_PS0");
}

/* ------------------------------------------------------------------ */
/* HID over I2C                                                         */
/* ------------------------------------------------------------------ */

static int RegRead(DW_CTRL *c, UCHAR Addr, UCHAR Fast, USHORT Reg, UCHAR *Buf, ULONG Len)
{
    UCHAR w[2];

    w[0] = (UCHAR)Reg;
    w[1] = (UCHAR)(Reg >> 8);
    return DwXfer(c, Addr, Fast, w, 2, Buf, Len);
}

static int Command(I2C_HID *h, UCHAR Op, UCHAR Low)
{
    UCHAR w[4];

    w[0] = (UCHAR)h->CmdReg;
    w[1] = (UCHAR)(h->CmdReg >> 8);
    w[2] = Low;
    w[3] = Op;
    return DwXfer(h->C, h->Addr, h->Fast, w, 4, NULL, 0);
}

static BOOLEAN DescOk(const UCHAR *d)
{
    return (BOOLEAN)(GET16(d) == 30 && GET16(d + 2) == 0x0100 && GET16(d + 4) != 0 && GET16(d + 4) <= 8192 &&
                     GET16(d + 10) >= 2);
}

/* SET_REPORT of a feature report (Rep: report id, then the data) */
static int SetFeature(I2C_HID *h, const UCHAR *Rep, ULONG Len)
{
    UCHAR w[80];
    ULONG n = 0, i;
    UCHAR id = Rep[0];

    if (Len + 10 > sizeof(w) || h->DataReg == 0) {
        return I2C_ERR;
    }
    w[n++] = (UCHAR)h->CmdReg;
    w[n++] = (UCHAR)(h->CmdReg >> 8);
    w[n++] = (UCHAR)(0x30 | (id < 15 ? id : 15));       /* feature report */
    if (id >= 15) {
        w[n++] = 0x03;                                  /* opcode, then the third byte holds the id */
        w[n++] = id;
    } else {
        w[n++] = 0x03;                                  /* SET_REPORT */
    }
    w[n++] = (UCHAR)h->DataReg;
    w[n++] = (UCHAR)(h->DataReg >> 8);
    w[n++] = (UCHAR)((Len + 2) & 0xFF);                 /* length field counts itself */
    w[n++] = (UCHAR)((Len + 2) >> 8);
    for (i = 0; i < Len; i++) {
        w[n++] = Rep[i];
    }
    return DwXfer(h->C, h->Addr, h->Fast, w, n, NULL, 0);
}

static void LedOut(void *Ctx, UCHAR HidLeds)
{
    I2C_HID *h = (I2C_HID *)Ctx;
    UCHAR rep[16];
    UCHAR w[24];
    ULONG n, i;

    if (h->Dead || h->OutReg == 0) {
        return;
    }
    n = HidKbdLedReport(&h->KL, HidLeds, rep, sizeof(rep));
    if (n == 0) {
        return;
    }
    /* output register: length (including itself), report */
    w[0] = (UCHAR)h->OutReg;
    w[1] = (UCHAR)(h->OutReg >> 8);
    w[2] = (UCHAR)(n + 2);
    w[3] = 0;
    for (i = 0; i < n; i++) {
        w[4 + i] = rep[i];
    }
    DwXfer(h->C, h->Addr, h->Fast, w, 4 + n, NULL, 0);
}

static ULONG ReportBytes(ULONG Bits, UCHAR Ids)
{
    return (Bits + 7) / 8 + (Ids ? 1 : 0);
}

/* power on, reset, report descriptor; FALSE when the device is of no use */
static BOOLEAN HidStart(I2C_HID *h)
{
    UCHAR *rd;
    UCHAR tmp[4];
    ULONG i;
    int r;

    Command(h, OP_SET_POWER, PWR_ON);
    OsSleepMs(10);
    r = Command(h, OP_RESET, 0);
    if (r != I2C_OK) {
        UsbLog(LOG_INFO, "i2c: %s: reset failed (%d)\n", h->Name, r);
    }
    /* the reset ends with a zero length input report */
    OsSleepMs(50);
    for (i = 0; i < 20; i++) {
        if (DwXfer(h->C, h->Addr, h->Fast, NULL, 0, tmp, 2) == I2C_OK && GET16(tmp) == 0) {
            break;
        }
        OsSleepMs(10);
    }
    rd = (UCHAR *)OsAlloc(h->RdLen);
    if (rd == NULL) {
        return FALSE;
    }
    r = RegRead(h->C, h->Addr, h->Fast, h->RdReg, rd, h->RdLen);
    if (r != I2C_OK) {
        UsbLog(LOG_ERR, "i2c: %s: report descriptor read failed (%d)\n", h->Name, r);
        OsFree(rd);
        return FALSE;
    }
    if (HidParseMouse(rd, h->RdLen, &h->ML) == USB_OK) {
        h->HasMouse = 1;
    }
    if (HidParseKeyboard(rd, h->RdLen, &h->KL) == USB_OK) {
        h->HasKbd = 1;
    }
    {
        /* precision touch pad: switch it to multi-touch mode for scrolling and taps */
        HID_PTP_LAYOUT pl;
        if (HidParsePtp(rd, h->RdLen, &pl) == USB_OK && pl.ModeValid && OsGetConfig("TouchpadGestures", 1)) {
            UCHAR fr[16];
            ULONG fl = PtpModeReport(&pl, fr, sizeof(fr));
            h->Ptp = (PTP_STATE *)OsAlloc(sizeof(PTP_STATE));
            if (h->Ptp != NULL && fl != 0 && SetFeature(h, fr, fl) == I2C_OK) {
                PtpInit(h->Ptp, &pl);
                h->PtpOn = 1;
                h->HasMouse = 1;
                UsbLog(LOG_INFO, "i2c: %s: precision touch pad (%u contact(s) per report): two finger scrolling, taps\n", h->Name,
                       pl.NFingers);
            } else if (h->Ptp != NULL) {
                OsFree(h->Ptp);
                h->Ptp = NULL;
            }
        }
    }
    OsFree(rd);
    if (!h->HasMouse && !h->HasKbd) {
        UsbLog(LOG_INFO, "i2c: %s: no pointer or keyboard collection, not used\n", h->Name);
        Command(h, OP_SET_POWER, PWR_SLEEP);
        return FALSE;
    }
    h->ReadLen = h->MaxIn;
    if (h->ReadLen > MAX_READ) {
        h->ReadLen = MAX_READ;
    }
    UsbLog(LOG_INFO, "i2c: %s: %s%s%s, reports up to %u bytes\n", h->Name,
           !h->HasMouse ? "" : h->PtpOn ? "precision touch pad (multi-touch mode)" :
           h->ML.Kind == HID_PTR_TOUCHPAD ? "touch pad" :
           h->ML.Kind == HID_PTR_DIGITIZER ? "touch screen / pen" : h->ML.X.Relative ? "pointer (touch pad in mouse mode)" :
           "absolute pointer", h->HasMouse && h->HasKbd ? " + " : "", h->HasKbd ? "keyboard" : "", h->MaxIn - 2);
    (void)ReportBytes;
    return TRUE;
}

static BOOLEAN Probe(DW_CTRL *c, UCHAR Addr, UCHAR Fast, USHORT Reg, I2C_HID **Out)
{
    UCHAR d[30];
    I2C_HID *h;
    int i;

    for (i = 0; i < NHid; i++) {
        if (Hids[i]->C == c && Hids[i]->Addr == Addr) {
            return TRUE;
        }
    }
    if (RegRead(c, Addr, 0, Reg, d, 30) != I2C_OK || !DescOk(d)) {
        return FALSE;
    }
    if (NHid >= MAX_HID) {
        return FALSE;
    }
    h = (I2C_HID *)OsAlloc(sizeof(I2C_HID));
    if (h == NULL) {
        return FALSE;
    }
    h->C = c;
    h->Addr = Addr;
    h->Fast = Fast;
    h->DescReg = Reg;
    h->RdLen = GET16(d + 4);
    h->RdReg = GET16(d + 6);
    h->InReg = GET16(d + 8);
    h->MaxIn = GET16(d + 10);
    h->OutReg = GET16(d + 12);
    h->MaxOut = GET16(d + 14);
    h->CmdReg = GET16(d + 16);
    h->DataReg = GET16(d + 18);
    h->Vid = GET16(d + 20);
    h->Pid = GET16(d + 22);
    h->Ver = GET16(d + 24);
    if (g_CurCand != NULL && g_CurCand->FromAml) {
        UsbMemCpy(h->AcpiPath, g_CurCand->Path, sizeof(h->AcpiPath));
        UsbMemCpy(h->CtrlPath, g_CurCand->CtrlPath, sizeof(h->CtrlPath));
        if (g_CurCand->HasInt && GpioMode() != 0 &&
            GpioResolvePin(g_CurCand->IntCtrl, g_CurCand->IntPin, &h->IntPin)) {
            h->HasInt = 1;
            h->IntActiveLow = g_CurCand->IntActiveLow;
            UsbLog(LOG_INFO, "i2c: %s uses GpioInt %s/%u (%s) for data ready\n", h->AcpiPath,
                   g_CurCand->IntCtrl, g_CurCand->IntPin, g_CurCand->IntActiveLow ? "active low" : "active high");
        }
        if (g_CurCand->HasRst && GpioMode() != 0) {
            if (GpioResolvePin(g_CurCand->RstCtrl, g_CurCand->RstPin, &h->RstPin)) {
                h->HasRst = 1;
            }
        }
    }
    UsbFormat(h->Name, sizeof(h->Name), "%s/%02x %04x:%04x", c->AcpiName[0] ? c->AcpiName : c->Name, Addr, h->Vid,
              h->Pid);
    UsbLog(LOG_INFO, "i2c: HID over I2C device %04x:%04x v%04x at %02x on %s (descriptor register %04x)\n", h->Vid,
           h->Pid, h->Ver, Addr, c->Name, Reg);
    if (!HidStart(h)) {
        OsFree(h);
        return TRUE;                                /* found, but not used: no further probing */
    }
    Hids[NHid++] = h;
    *Out = h;
    return TRUE;
}

static void Attach(I2C_HID *h)
{
    if (h->HasMouse) {
        h->Mouse = HidExtAdd(HID_EXT_MOUSE, &h->ML, NULL, NULL);
    }
    if (h->HasKbd) {
        h->Kbd = HidExtAdd(HID_EXT_KBD, NULL, LedOut, h);
    }
    h->NextPoll = OsTimeMs();
    h->LastActive = OsTimeMs();
    I2cHidCount++;
}

/* addresses of HID over I2C touch pads and touch screens */
static const UCHAR KnownAddr[] = { 0x2C, 0x15, 0x2A, 0x10, 0x14, 0x5D, 0x38, 0x24, 0x4B };

static BOOLEAN Present(DW_CTRL *c, UCHAR Addr)
{
    UCHAR b;
    return (BOOLEAN)(DwXfer(c, Addr, 0, NULL, 0, &b, 1) == I2C_OK);
}

void I2cHidInit(void)
{
    int i, k;
    ULONG r;

    if (GetMode() == 0) {
        return;
    }
    if (AcpiInit() != 0) {
        AcpiScan();
        AmlScan();                                  /* conditional _CRS / _DSM / _STA / GPIO */
    }
    AmdFixed();
    for (i = 0; i < NCtrl; i++) {
        DW_CTRL *c = Ctrls[i];
        UsbLog(LOG_INFO, "i2c: DesignWare I2C controller %s%s%s at %08x, FIFO %u/%u, %u MHz%s\n", c->Name,
               c->AcpiName[0] ? " = " : "", c->AcpiName, c->Phys, c->TxDepth, c->RxDepth, c->ClkMhz,
               c->BiosFast ? ", fast mode set by the firmware" : "");
    }
    if (NCtrl == 0) {
        return;
    }
    /* devices described in ACPI, on their controller (all when unknown) */
    for (k = 0; k < NCand; k++) {
        I2C_CAND *cd = &Cands[k];
        static const USHORT defRegs[] = { 0x20, 0x01, 0x00 };
        for (i = 0; i < NCtrl; i++) {
            DW_CTRL *c = Ctrls[i];
            I2C_HID *h = NULL;
            BOOLEAN done = FALSE;
            if (cd->Ctrl[0] && c->AcpiName[0] && UsbMemCmp(cd->Ctrl, c->AcpiName, 4) != 0) {
                continue;
            }
            if (!Present(c, cd->Addr)) {
                continue;
            }
            PrepCand(cd);                           /* _PS0 / reset GPIO before probing */
            g_CurCand = cd;
            for (r = 0; r < cd->NRegs && !done; r++) {
                done = Probe(c, cd->Addr, cd->Fast, cd->Regs[r], &h);
            }
            for (r = 0; r < 3 && !done; r++) {
                done = Probe(c, cd->Addr, cd->Fast, defRegs[r], &h);
            }
            g_CurCand = NULL;
            if (h != NULL) {
                Attach(h);
            }
            if (done) {
                break;
            }
        }
    }
    /* the usual addresses, when the firmware sets them at run time */
    if (Mode == 1) {
        for (i = 0; i < NCtrl; i++) {
            DW_CTRL *c = Ctrls[i];
            for (k = 0; k < (int)sizeof(KnownAddr); k++) {
                I2C_HID *h = NULL;
                int j;
                BOOLEAN known = FALSE;
                for (j = 0; j < NHid; j++) {
                    if (Hids[j]->C == c && Hids[j]->Addr == KnownAddr[k]) known = TRUE;
                }
                if (known || !Present(c, KnownAddr[k])) {
                    continue;
                }
                if (!Probe(c, KnownAddr[k], c->BiosFast, 0x20, &h)) {
                    Probe(c, KnownAddr[k], c->BiosFast, 0x01, &h);
                }
                if (h != NULL) {
                    Attach(h);
                }
            }
        }
    }
    if (I2cHidCount != 0) {
        UsbPollWantMs = 8;
    }
}

/* a complete input report (length field first) */
static void Report(I2C_HID *h, const UCHAR *B, ULONG Len)
{
    ULONG n = GET16(B);

    if (n == 0 || n == 0xFFFF) {
        return;                                     /* nothing new, or a reset */
    }
    if (n < 3 || n > Len) {
        h->Errors++;
        return;
    }
    /* firmware that answers every poll with its last report again */
    if (n == h->LastLen && n <= sizeof(h->Last) && UsbMemCmp(B, h->Last, n) == 0) {
        if (++h->SameCount >= 100 && !h->Repeats) {
            h->Repeats = 1;
            UsbLog(LOG_INFO, "i2c: %s repeats its last report, duplicates are dropped\n", h->Name);
        }
        if (h->Repeats) {
            return;
        }
    } else {
        h->SameCount = 0;
        if (n <= sizeof(h->Last)) {
            UsbMemCpy(h->Last, B, n);
            h->LastLen = n;
        }
    }
    h->Reports++;
    h->LastActive = OsTimeMs();
    B += 2;
    n -= 2;
    UsbLog(LOG_TRACE, "i2c: %s report %02x %02x %02x %02x (%u)\n", h->Name, B[0], n > 1 ? B[1] : 0, n > 2 ? B[2] : 0,
           n > 3 ? B[3] : 0, n);
    if (h->PtpOn && h->Mouse != NULL && (!h->Ptp->L.UsesIds || B[0] == h->Ptp->L.ReportId)) {
        PtpReport(h->Ptp, B, n, h->Mouse);
    } else if (h->Mouse != NULL && (!h->ML.UsesIds || B[0] == h->ML.ReportId)) {
        HidExtReport(h->Mouse, B, n);
    }
    if (h->Kbd != NULL) {
        UCHAR boot[8];
        if (HidKbdToBoot(&h->KL, B, n, boot)) {
            HidExtReport(h->Kbd, boot, 8);
        }
    }
}

void I2cHidService(void)
{
    int i;
    ULONG now = OsTimeMs();
    ULONG want = 0;

    for (i = 0; i < NHid; i++) {
        I2C_HID *h = Hids[i];
        DW_CTRL *c = h->C;
        int r;

        if (h->Dead) {
            continue;
        }
        if (h->PtpOn && h->Mouse != NULL) {
            PtpTick(h->Ptp, h->Mouse);
        }
        if (h->RetryAt != 0) {
            /* lost (power off, reset): start it again */
            if ((LONG)(now - h->RetryAt) < 0) {
                continue;
            }
            h->RetryAt = 0;
            h->Errors = 0;
            Command(h, OP_SET_POWER, PWR_ON);
        }
        if (c->Busy && c->Owner == h) {
            r = DwCollect(c, h->Buf, h->ReadLen, FALSE);
            if (r == I2C_PENDING) {
                want = 2;
                continue;
            }
            if (r == I2C_OK) {
                h->Errors = 0;
                Report(h, h->Buf, h->ReadLen);
            } else {
                h->Errors++;
            }
        }
        if ((LONG)(now - h->NextPoll) < 0) {
            continue;
        }
        h->NextPoll = now + ((now - h->LastActive < 1000) ? 8 : 30);
        if (c->Busy) {
            h->NextPoll = now;                      /* the other device on this bus first */
            want = 2;
            continue;
        }
        /* GpioInt "data ready": only read the device when its line is asserted
           (the cheap MMIO pad read instead of a blind I2C transfer). NT has no
           GPIO interrupt, so this still runs on the poll timer; it falls back to
           polling when the pad level cannot be read (GpioGet returns -1). */
        if (h->HasInt) {
            int lv = GpioGet(&h->IntPin);
            if (lv >= 0) {
                BOOLEAN asserted = (BOOLEAN)(h->IntActiveLow ? (lv == 0) : (lv != 0));
                if (!asserted) {
                    continue;
                }
            }
        }
        if (h->ReadLen <= 64 && h->ReadLen <= c->RxDepth && h->ReadLen <= c->TxDepth) {
            r = DwStartRead(c, h->Addr, h->Fast, h->ReadLen, h);
            if (r != I2C_OK) {
                h->Errors++;
            } else {
                want = 2;
            }
        } else {
            r = DwXfer(c, h->Addr, h->Fast, NULL, 0, h->Buf, h->ReadLen);
            if (r == I2C_OK) {
                h->Errors = 0;
                Report(h, h->Buf, h->ReadLen);
            } else {
                h->Errors++;
            }
        }
        if (h->Errors > 50) {
            UsbLog(LOG_ERR, "i2c: %s does not answer, retrying in 2 s\n", h->Name);
            h->RetryAt = now + 2000;
            h->Errors = 0;
        }
    }
    if (I2cHidCount != 0) {
        UsbPollWantMs = want ? want : 8;
    }
}

/* harness / diagnostics: one line per controller and device */
int I2cHidDescribe(int Index, char *Buf, int Size)
{
    if (Index < NCtrl) {
        DW_CTRL *c = Ctrls[Index];
        return UsbFormat(Buf, Size, "I2CCTRL %d %s acpi=%s phys=%08x fifo=%u/%u clk=%u", Index, c->Name,
                         c->AcpiName[0] ? c->AcpiName : "-", c->Phys, c->TxDepth, c->RxDepth, c->ClkMhz);
    }
    Index -= NCtrl;
    if (Index < NHid) {
        I2C_HID *h = Hids[Index];
        return UsbFormat(Buf, Size, "I2CHID %d %s addr=%02x reg=%04x vid=%04x pid=%04x mouse=%u kind=%u kbd=%u "
                         "maxin=%u reports=%u fast=%u ptp=%u", Index, h->Name, h->Addr, h->DescReg, h->Vid, h->Pid,
                         h->HasMouse, h->ML.Kind, h->HasKbd, h->MaxIn, h->Reports, h->Fast, h->PtpOn);
    }
    return 0;
}

/* user mode listing (usbtree): Kind 1 controller, 2 device; FALSE past the end */
BOOLEAN I2cHidInfo(int Index, UCHAR *Kind, char *Name, int NameSize, char *Detail, int DetailSize)
{
    if (Index < NCtrl) {
        DW_CTRL *c = Ctrls[Index];
        *Kind = 1;
        if (c->Pci) {
            UsbFormat(Name, NameSize, "I2C controller %02x:%02x.%u", c->Bus, c->Dev, c->Fn);
            UsbFormat(Detail, DetailSize, "DesignWare I2C, PCI %04X:%04X%s%s\nRegisters at %08X, FIFO %u/%u, "
                      "clock %u MHz", c->Vendor, c->Device, c->AcpiName[0] ? ", ACPI " : "", c->AcpiName, c->Phys,
                      c->TxDepth, c->RxDepth, c->ClkMhz);
        } else {
            UsbFormat(Name, NameSize, "I2C controller %s", c->Name);
            UsbFormat(Detail, DetailSize, "DesignWare I2C (ACPI / fixed address)\nRegisters at %08X, FIFO %u/%u, "
                      "clock %u MHz", c->Phys, c->TxDepth, c->RxDepth, c->ClkMhz);
        }
        return TRUE;
    }
    Index -= NCtrl;
    if (Index < NHid) {
        I2C_HID *h = Hids[Index];
        const char *what = !h->HasMouse ? "Keyboard" : h->PtpOn ? "Precision touch pad" :
                           h->ML.Kind == HID_PTR_TOUCHPAD ? "Touch pad" :
                           h->ML.Kind == HID_PTR_DIGITIZER ? "Touch screen / pen" :
                           h->ML.X.Relative ? "Touch pad (mouse mode)" : "Absolute pointer";
        *Kind = 2;
        UsbFormat(Name, NameSize, "%s%s %04X:%04X", what, h->HasMouse && h->HasKbd ? " + keyboard" : "", h->Vid,
                  h->Pid);
        UsbFormat(Detail, DetailSize, "HID over I2C, address %02X on %s, %s mode\nHID descriptor register %04X, "
                  "version %04X, reports up to %u bytes, %u reports%s", h->Addr,
                  h->C->AcpiName[0] ? h->C->AcpiName : h->C->Name, h->Fast ? "fast (400 kHz)" : "standard (100 kHz)",
                  h->DescReg, h->Ver, h->MaxIn - 2, h->Reports, h->Dead ? ", stopped" : "");
        return TRUE;
    }
    return FALSE;
}

void I2cHidShutdown(void)
{
    int i;

    for (i = 0; i < NHid; i++) {
        I2C_HID *h = Hids[i];
        if (h->C->Busy) {
            UCHAR tmp[64];
            DwCollect(h->C, tmp, h->C->BusyLen, TRUE);
        }
        if (h->Mouse) HidExtRemove(h->Mouse);
        if (h->Kbd) HidExtRemove(h->Kbd);
        h->Mouse = h->Kbd = NULL;
        h->Dead = 1;
        /* power the device down the way USBNT already stops devices */
        if (h->AcpiPath[0]) {
            RunPs(h->AcpiPath, "_PS3");
        }
    }
    AmlShutdown();
}
