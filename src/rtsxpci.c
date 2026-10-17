/*
 * rtsxpci.c - Realtek PCI Express card readers (RTS5209, RTS5227, RTS522A,
 * RTS5229, RTS5249, RTS524A, RTS525A, RTS5260, RTS5261, RTS5228, RTS5264,
 * RTL8411, RTL8411B, RTL8402) as SD hosts for sdcard.c.
 *
 * The chip is programmed through a command buffer in host memory (register
 * write / read / check entries the reader executes on its own, read results
 * come back in the same buffer) and moves card data by ADMA.  Interrupts are
 * not used: the reader's interrupt line stays disabled (PCI command bit 10)
 * and the pending register is polled.  Register map, values and the per chip
 * set-up / power / clock sequences follow the chips' programming model as
 * used by the Linux rtsx_pci driver; SD 3.3 V default and high speed only
 * (no UHS, no MemoryStick, no SD Express).
 */

#include "usbnt.h"
#include "sdcard.h"
#include "rtsxpreg.h"

#define CMD_BUF         1024                /* host command buffer */
#define SG_OFF          CMD_BUF             /* ADMA descriptor table after it */
#define RESV_BUF        4096
#define XFER_MAX        0x8000              /* bounce buffer: 64 blocks */
#define MAX_READERS     4

typedef struct _RTSX RTSX;

typedef struct _CHIP {
    USHORT  Pid;
    const char *Name;
    void  (*Params)(RTSX *r);
    void  (*Vendor)(RTSX *r);
    int   (*OptimizePhy)(RTSX *r);
    int   (*ExtraInit)(RTSX *r);
    int   (*PowerOn)(RTSX *r);
    int   (*PowerOff)(RTSX *r);
    int   (*Out3V3)(RTSX *r);
    void  (*InitOcp)(RTSX *r);              /* NULL: the generic one */
    void  (*EnableOcp)(RTSX *r);
    void  (*DisableOcp)(RTSX *r);
    void  (*StopCmd)(RTSX *r);
    int   (*AutoBlink)(RTSX *r);
    UCHAR   Clock;                          /* 0 generic, 1 RTS5261/5228, 2 RTS5264 */
    UCHAR   Bar;                            /* register BAR: 0, or 1 on RTS525A/5264 */
    UCHAR   SgLong;                         /* RTS5261/5228 descriptor length layout */
    UCHAR   AspmReg;                        /* ASPM switched by register, not config space */
    UCHAR   Conv8411;                       /* RTL8411 clock / divider relation */
    UCHAR   PhyAlias;                       /* RTS524A: PHY addresses 0x80+ map to 0x40+ */
} CHIP;

struct _RTSX {
    UCHAR   Bus, Dev, Fn;
    USHORT  Pid;
    const CHIP *Chip;
    UCHAR  *Regs;
    ULONG   RegLen;
    UCHAR  *Resv;                           /* command buffer + descriptor table */
    ULONG   ResvPhys;
    UCHAR  *Buf;                            /* data bounce buffer */
    ULONG   BufPhys;
    ULONG   Ci;
    UCHAR   IcVer;
    UCHAR   CardDrive;
    UCHAR   Sd30Drive3V3, Sd30Drive1V8;
    UCHAR   MsPmos, ReverseSocket, Rtd3, Qfn48;
    UCHAR   OcpEn, OcpGlitch, OcpThd;
    USHORT  PmCtrl3;
    ULONG   Bier;
    const ULONG *PullOn, *PullOff;
    ULONG   CurClock;                       /* MHz of the SSC, 0 = not set */
    UCHAR   Power, Initial, CardExist;
    UCHAR   PcieCap;
    SD_CARD *Card;
    char    Name[24];
};

static RTSX *Readers[MAX_READERS];

/* ------------------------------------------------------------------ */
/* register access                                                     */
/* ------------------------------------------------------------------ */
#define RD32(r, o)      MmioRd32((r)->Regs + (o))
#define WR32(r, o, v)   MmioWr32((r)->Regs + (o), (v))

static int WriteReg(RTSX *r, USHORT addr, UCHAR mask, UCHAR data)
{
    ULONG v = HAIMR_WRITE_START | ((ULONG)(addr & 0x3FFF) << 16) | ((ULONG)mask << 8) | data;
    int i;
    WR32(r, RTSX_HAIMR, v);
    for (i = 0; i < MAX_RW_REG_CNT; i++) {
        v = RD32(r, RTSX_HAIMR);
        if (!(v & HAIMR_TRANS_END)) return (UCHAR)v == data || mask != 0xFF ? SD_OK : SD_EIO;
    }
    return SD_ETIMEOUT;
}

static int ReadReg(RTSX *r, USHORT addr, UCHAR *data)
{
    ULONG v = HAIMR_READ_START | ((ULONG)(addr & 0x3FFF) << 16);
    int i;
    WR32(r, RTSX_HAIMR, v);
    for (i = 0; i < MAX_RW_REG_CNT; i++) {
        v = RD32(r, RTSX_HAIMR);
        if (!(v & HAIMR_TRANS_END)) {
            if (data) *data = (UCHAR)v;
            return SD_OK;
        }
    }
    return SD_ETIMEOUT;
}

static UCHAR Rd8(RTSX *r, USHORT addr)
{
    UCHAR v = 0;
    ReadReg(r, addr, &v);
    return v;
}

static int WritePhy(RTSX *r, UCHAR addr, USHORT val)
{
    int i;
    UCHAR t;
    if (r->Chip->PhyAlias && (addr & 0x80)) addr = (UCHAR)((addr & 0x7F) | 0x40);
    WriteReg(r, PHYDATA0, 0xFF, (UCHAR)val);
    WriteReg(r, PHYDATA1, 0xFF, (UCHAR)(val >> 8));
    WriteReg(r, PHYADDR, 0xFF, addr);
    WriteReg(r, PHYRWCTL, 0xFF, 0x81);
    for (i = 0; i < 100000; i++) {
        if (ReadReg(r, PHYRWCTL, &t) != SD_OK) return SD_EIO;
        if (!(t & 0x80)) return SD_OK;
    }
    return SD_ETIMEOUT;
}

static int ReadPhy(RTSX *r, UCHAR addr, USHORT *val)
{
    int i;
    UCHAR t, lo, hi;
    if (r->Chip->PhyAlias && (addr & 0x80)) addr = (UCHAR)((addr & 0x7F) | 0x40);
    WriteReg(r, PHYADDR, 0xFF, addr);
    WriteReg(r, PHYRWCTL, 0xFF, 0x80);
    for (i = 0; i < 100000; i++) {
        if (ReadReg(r, PHYRWCTL, &t) != SD_OK) return SD_EIO;
        if (!(t & 0x80)) break;
    }
    if (i == 100000) return SD_ETIMEOUT;
    lo = Rd8(r, PHYDATA0);
    hi = Rd8(r, PHYDATA1);
    *val = (USHORT)(lo | (hi << 8));
    return SD_OK;
}

static int UpdatePhy(RTSX *r, UCHAR addr, USHORT mask, USHORT append)
{
    USHORT v = 0;
    int e = ReadPhy(r, addr, &v);
    if (e != SD_OK) return e;
    return WritePhy(r, addr, (USHORT)((v & ~mask) | append));
}

static ULONG Cfg32(RTSX *r, ULONG off)
{
    ULONG v = 0xFFFFFFFFUL;
    if (off < 0x100) return OsPciRead(r->Bus, r->Dev, r->Fn, off, 4);
    if (!UsbPciExt(r->Bus, r->Dev, r->Fn, off & ~3UL, &v, FALSE)) return 0xFFFFFFFFUL;
    return v >> (8 * (off & 3));
}

static void CfgW32(RTSX *r, ULONG off, ULONG v)
{
    if (off < 0x100) OsPciWrite(r->Bus, r->Dev, r->Fn, off, v, 4);
    else UsbPciExt(r->Bus, r->Dev, r->Fn, off, &v, TRUE);
}

/* ------------------------------------------------------------------ */
/* command buffer                                                      */
/* ------------------------------------------------------------------ */
static void InitCmd(RTSX *r) { r->Ci = 0; }

static void AddCmd(RTSX *r, UCHAR type, USHORT reg, UCHAR mask, UCHAR data)
{
    ULONG v = ((ULONG)(type & 3) << 30) | ((ULONG)(reg & 0x3FFF) << 16) | ((ULONG)mask << 8) | data;
    UCHAR *p;
    if (r->Ci >= CMD_BUF / 4) return;
    p = r->Resv + r->Ci * 4;
    p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24);
    r->Ci++;
}

static UCHAR *CmdData(RTSX *r) { return r->Resv; }

static void StopCmd(RTSX *r)
{
    if (r->Chip->StopCmd) {
        r->Chip->StopCmd(r);
        return;
    }
    WR32(r, RTSX_HCBCTLR, STOP_CMD);
    WR32(r, RTSX_HDBCTLR, STOP_DMA);
    WriteReg(r, DMACTL, 0x80, 0x80);
    WriteReg(r, RBCTL, 0x80, 0x80);
}

/* waits for the end of a command buffer run or a DMA transfer by polling the
   pending register (the interrupt line is off) */
static int WaitDone(RTSX *r, ULONG ms)
{
    ULONG t0 = OsTimeMs(), v, n = 0;
    for (;;) {
        v = RD32(r, RTSX_BIPR);
        if (v == 0xFFFFFFFFUL) return SD_ENODEV;
        if (v & (TRANS_FAIL_INT | DELINK_INT | TRANS_OK_INT)) {
            WR32(r, RTSX_BIPR, v & (NEED_COMPLETE_INT | DELINK_INT));
            return (v & (TRANS_FAIL_INT | DELINK_INT)) ? SD_EIO : SD_OK;
        }
        if (OsTimeMs() - t0 >= ms) return SD_ETIMEOUT;
        if (++n < 200) OsStallUs(5);
        else OsStallUs(50);
    }
}

static void ClearDone(RTSX *r)
{
    WR32(r, RTSX_BIPR, NEED_COMPLETE_INT | DELINK_INT);
}

static void StartCmd(RTSX *r)
{
    ClearDone(r);
    WR32(r, RTSX_HCBAR, r->ResvPhys);
    /* hardware auto response: read results are written back to the buffer */
    WR32(r, RTSX_HCBCTLR, 0x80000000UL | 0x40000000UL | ((r->Ci * 4) & 0x00FFFFFFUL));
}

static int SendCmd(RTSX *r, ULONG ms)
{
    int e;
    StartCmd(r);
    e = WaitDone(r, ms);
    if (e != SD_OK && e != SD_ENODEV) StopCmd(r);
    return e;
}

static int DmaXfer(RTSX *r, ULONG len, BOOLEAN read, ULONG ms)
{
    UCHAR *d = r->Resv + SG_OFF;
    ULONG lo, hi;
    int e;
    /* one descriptor: address in the high dword, length and flags in the low one */
    if (r->Chip->SgLong) lo = (len << 16) | RTSX_SG_VALID | RTSX_SG_TRANS_DATA | RTSX_SG_END;
    else lo = (len << 12) | RTSX_SG_VALID | RTSX_SG_TRANS_DATA | RTSX_SG_END;
    hi = r->BufPhys;
    d[0] = (UCHAR)lo; d[1] = (UCHAR)(lo >> 8); d[2] = (UCHAR)(lo >> 16); d[3] = (UCHAR)(lo >> 24);
    d[4] = (UCHAR)hi; d[5] = (UCHAR)(hi >> 8); d[6] = (UCHAR)(hi >> 16); d[7] = (UCHAR)(hi >> 24);
    WR32(r, RTSX_HDBAR, r->ResvPhys + SG_OFF);
    WR32(r, RTSX_HDBCTLR, ((ULONG)(read ? 1 : 0) << 29) | TRIG_DMA | ADMA_MODE);
    e = WaitDone(r, ms);
    if (e != SD_OK && e != SD_ENODEV) StopCmd(r);
    return e;
}

static int SetPull(RTSX *r, const ULONG *t)
{
    InitCmd(r);
    while (t && (*t & 0xFFFF0000UL)) {
        AddCmd(r, WRITE_REG_CMD, (USHORT)(*t >> 16), 0xFF, (UCHAR)*t);
        t++;
    }
    return SendCmd(r, 100);
}

/* ------------------------------------------------------------------ */
/* over current protection (generic parts)                             */
/* ------------------------------------------------------------------ */
static void EnableOcp(RTSX *r)
{
    if (r->Chip->EnableOcp) {
        r->Chip->EnableOcp(r);
        return;
    }
    WriteReg(r, FPDCTL, OC_POWER_DOWN, 0);
    WriteReg(r, REG_OCPCTL, 0xFF, SD_OCP_INT_EN | SD_DETECT_EN);
}

static void DisableOcp(RTSX *r)
{
    if (r->Chip->DisableOcp) {
        r->Chip->DisableOcp(r);
        return;
    }
    WriteReg(r, REG_OCPCTL, SD_OCP_INT_EN | SD_DETECT_EN, 0);
    WriteReg(r, FPDCTL, OC_POWER_DOWN, OC_POWER_DOWN);
}

static void InitOcp(RTSX *r)
{
    if (r->Chip->InitOcp) {
        r->Chip->InitOcp(r);
        return;
    }
    if (!r->OcpEn) return;
    WriteReg(r, FPDCTL, OC_POWER_DOWN, 0);
    WriteReg(r, REG_OCPPARA1, SD_OCP_TIME_MASK, SD_OCP_TIME_800);
    WriteReg(r, REG_OCPPARA2, SD_OCP_THD_MASK, r->OcpThd);
    WriteReg(r, REG_OCPGLITCH, SD_OCP_GLITCH_MASK, r->OcpGlitch);
    EnableOcp(r);
}

/* ------------------------------------------------------------------ */
/* vendor settings from configuration space                            */
/* ------------------------------------------------------------------ */
static UCHAR MapSdDrive(ULONG idx)
{
    static const UCHAR t[4] = { 0x01, 0x02, 0x05, 0x03 };      /* type D, C, A, B */
    return t[idx & 3];
}

#define VS_VALID(reg)           (!((reg) & 0x1000000UL))
#define VS_DRIVE_1V8(reg)       (((reg) >> 26) & 0x03)
#define VS_DRIVE_3V3(reg)       (((reg) >> 5) & 0x03)
#define VS_CARD_DRIVE(reg)      ((UCHAR)((((reg) >> 25) & 0x01) << 6))
#define VS_REVERSE(reg)         ((reg) & 0x4000)

static void VendorBase(RTSX *r)                 /* RTS5227/5249/5260 families */
{
    ULONG reg = Cfg32(r, PCR_SETTING_REG1);
    if (reg == 0xFFFFFFFFUL || !VS_VALID(reg)) return;
    r->Sd30Drive1V8 = (UCHAR)VS_DRIVE_1V8(reg);
    r->CardDrive = (UCHAR)((r->CardDrive & 0x3F) | VS_CARD_DRIVE(reg));
    reg = Cfg32(r, PCR_SETTING_REG2);
    if (reg == 0xFFFFFFFFUL) return;
    if (r->Pid == 0x522A) r->Rtd3 = (UCHAR)((reg & 0x02) != 0);
    if (r->Pid == 0x524A || r->Pid == 0x525A) r->Rtd3 = (UCHAR)((reg & 0x04) != 0);
    r->Sd30Drive3V3 = (UCHAR)VS_DRIVE_3V3(reg);
    if (VS_REVERSE(reg)) r->ReverseSocket = 1;
}

static void Vendor5209(RTSX *r)
{
    ULONG reg = Cfg32(r, PCR_SETTING_REG1);
    if (reg != 0xFFFFFFFFUL && !(reg & 0x80)) {
        if (!(reg & 0x08)) r->MsPmos = 1;
    }
    reg = Cfg32(r, PCR_SETTING_REG2);
    if (reg != 0xFFFFFFFFUL && (reg & 0x80)) {
        r->Sd30Drive1V8 = (UCHAR)((reg >> 3) & 0x07);
        r->Sd30Drive3V3 = (UCHAR)(reg & 0x07);
        r->CardDrive = (UCHAR)(reg >> 8);
    }
}

static void Vendor5229(RTSX *r)                 /* also RTL8411 (setting 3 byte) */
{
    ULONG reg = Cfg32(r, PCR_SETTING_REG1);
    if (reg == 0xFFFFFFFFUL || !VS_VALID(reg)) return;
    r->Sd30Drive1V8 = MapSdDrive(VS_DRIVE_1V8(reg));
    r->CardDrive = (UCHAR)((r->CardDrive & 0x3F) | VS_CARD_DRIVE(reg));
    if (r->Pid == 0x5229) {
        reg = Cfg32(r, PCR_SETTING_REG2);
        if (reg != 0xFFFFFFFFUL) r->Sd30Drive3V3 = MapSdDrive(VS_DRIVE_3V3(reg));
    } else {
        reg = Cfg32(r, PCR_SETTING_REG3 & ~3UL) >> (8 * (PCR_SETTING_REG3 & 3));
        if (reg != 0xFFFFFFFFUL) r->Sd30Drive3V3 = (UCHAR)(((reg & 0xFF) >> 5) & 0x07);
    }
}

static void Vendor8411b(RTSX *r)
{
    ULONG reg = Cfg32(r, PCR_SETTING_REG1);
    if (reg == 0xFFFFFFFFUL || !VS_VALID(reg)) return;
    r->Sd30Drive1V8 = MapSdDrive(VS_DRIVE_1V8(reg));
    r->Sd30Drive3V3 = MapSdDrive(reg & 0x03);
}

static void Vendor5228(RTSX *r)
{
    ULONG reg = Cfg32(r, PCR_SETTING_REG1);
    if (reg == 0xFFFFFFFFUL || !VS_VALID(reg)) return;
    r->Sd30Drive1V8 = (UCHAR)VS_DRIVE_1V8(reg);
    reg = Cfg32(r, PCR_SETTING_REG2);
    if (reg == 0xFFFFFFFFUL) return;
    r->Rtd3 = (UCHAR)((reg & 0x02) != 0);
    r->Sd30Drive3V3 = (UCHAR)VS_DRIVE_3V3(reg);
    if (VS_REVERSE(reg)) r->ReverseSocket = 1;
}

/* RTS5261 / RTS5264: the settings live in the efuse or in configuration space */
static void Vendor5261(RTSX *r, USHORT pmeForce, USHORT efAddr, USHORT efCtl, USHORT efData)
{
    ULONG l1, l2, i, s1, s2;
    UCHAR t = 0, efuse, valid;
    WriteReg(r, pmeForce, REG_EFUSE_POR | REG_EFUSE_POWER_MASK, REG_EFUSE_POR | REG_EFUSE_POWERON);
    OsStallUs(1);
    WriteReg(r, efAddr, RTS5261_EFUSE_ADDR_MASK, 0x00);
    WriteReg(r, efCtl, RTS5261_EFUSE_ENABLE | RTS5261_EFUSE_MODE_MASK, RTS5261_EFUSE_ENABLE);
    for (i = 0; i < MAX_RW_REG_CNT; i++) {
        ReadReg(r, efCtl, &t);
        if (!(t & 0x80)) break;
    }
    ReadReg(r, efData, &t);
    efuse = (UCHAR)((t & 0x0C) >> 2);
    l2 = Cfg32(r, PCR_SETTING_REG2);
    valid = (UCHAR)((l2 >> 16) & 0x03);
    WriteReg(r, pmeForce, REG_EFUSE_POR, 0);
    if (efuse == 2 || efuse == 3) {
        if (valid == 3) { s1 = PCR_SETTING_REG1; s2 = PCR_SETTING_REG2; }
        else { s1 = PCR_SETTING_REG4; s2 = PCR_SETTING_REG5; }
    } else if (efuse == 0) {
        s1 = PCR_SETTING_REG1; s2 = PCR_SETTING_REG2;
    } else {
        return;
    }
    l2 = Cfg32(r, s2);
    if (l2 == 0xFFFFFFFFUL || !(l2 & 0x010000)) return;
    r->Rtd3 = (UCHAR)((l2 & 0x08) != 0);
    if (l2 & 0x04) r->ReverseSocket = 1;
    l1 = Cfg32(r, s1);
    if (l1 == 0xFFFFFFFFUL) return;
    r->Sd30Drive1V8 = (UCHAR)((l1 >> 22) & 0x03);
    r->Sd30Drive3V3 = (UCHAR)((l1 >> 16) & 0x03);
    if (s1 == PCR_SETTING_REG1) {
        WriteReg(r, 0xFF0C, 0xFF, (UCHAR)l1);
        WriteReg(r, 0xFF0D, 0xFF, (UCHAR)(l1 >> 8));
        WriteReg(r, 0xFF0E, 0xFF, (UCHAR)(l1 >> 16));
        WriteReg(r, 0xFF0F, 0xFF, (UCHAR)(l1 >> 24));
        WriteReg(r, 0xFF10, 0xFF, (UCHAR)l2);
        WriteReg(r, 0xFF11, 0xFF, (UCHAR)(l2 >> 8));
        WriteReg(r, 0xFF12, 0xFF, (UCHAR)(l2 >> 16));
        CfgW32(r, PCR_SETTING_REG4, l1);
        CfgW32(r, PCR_SETTING_REG5, l2 & 0x00FFFFFFUL);
    }
}

/* ------------------------------------------------------------------ */
/* drive strength tables (3.3 V rows: clk, cmd, dat)                   */
/* ------------------------------------------------------------------ */
static void FillDriving(RTSX *r, const UCHAR (*t)[3], BOOLEAN queue)
{
    const UCHAR *d = t[r->Sd30Drive3V3 & 3];
    if (queue) {
        AddCmd(r, WRITE_REG_CMD, SD30_CLK_DRIVE_SEL, 0xFF, d[0]);
        AddCmd(r, WRITE_REG_CMD, SD30_CMD_DRIVE_SEL, 0xFF, d[1]);
        AddCmd(r, WRITE_REG_CMD, SD30_DAT_DRIVE_SEL, 0xFF, d[2]);
    } else {
        WriteReg(r, SD30_CLK_DRIVE_SEL, 0xFF, d[0]);
        WriteReg(r, SD30_CMD_DRIVE_SEL, 0xFF, d[1]);
        WriteReg(r, SD30_DAT_DRIVE_SEL, 0xFF, d[2]);
    }
}

static const UCHAR Drv5227[4][3] = { { 0x13, 0x13, 0x13 }, { 0x96, 0x96, 0x96 }, { 0x7F, 0x7F, 0x7F }, { 0x96, 0x96, 0x96 } };
static const UCHAR Drv5249[4][3] = { { 0x11, 0x11, 0x18 }, { 0x55, 0x55, 0x5C }, { 0xFF, 0xFF, 0xFF }, { 0x96, 0x96, 0x96 } };
static const UCHAR Drv5260[4][3] = { { 0x11, 0x11, 0x11 }, { 0x22, 0x22, 0x22 }, { 0x55, 0x55, 0x55 }, { 0x33, 0x33, 0x33 } };
static const UCHAR Drv5261[4][3] = { { 0x96, 0x96, 0x96 }, { 0x96, 0x96, 0x96 }, { 0x7F, 0x7F, 0x7F }, { 0x13, 0x13, 0x13 } };
static const UCHAR Drv5228[4][3] = { { 0x13, 0x13, 0x13 }, { 0x96, 0x96, 0x96 }, { 0x7F, 0x7F, 0x7F }, { 0x96, 0x96, 0x96 } };
static const UCHAR Drv5264[4][3] = { { 0x88, 0x88, 0x88 }, { 0x77, 0x77, 0x77 }, { 0x99, 0x99, 0x99 }, { 0x66, 0x66, 0x66 } };

/* card pull up / down tables: (register << 16) | value */
#define PAIR(reg, v)    (((ULONG)(reg) << 16) | (v))
static const ULONG PullOn5209[] = { PAIR(CARD_PULL_CTL1, 0xAA), PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xE9), 0 };
static const ULONG PullOff5209[] = { PAIR(CARD_PULL_CTL1, 0x55), PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xD5), 0 };
static const ULONG PullOn5229a[] = { PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xE9), 0 };
static const ULONG PullOn5229c[] = { PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xD9), 0 };
static const ULONG PullOff5229a[] = { PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xD5), 0 };
static const ULONG PullOff5229c[] = { PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xE5), 0 };
static const ULONG PullOn5249[] = { PAIR(CARD_PULL_CTL1, 0x66), PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xE9),
                                    PAIR(CARD_PULL_CTL4, 0xAA), 0 };
static const ULONG PullOff5249[] = { PAIR(CARD_PULL_CTL1, 0x66), PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xD5),
                                     PAIR(CARD_PULL_CTL4, 0x55), 0 };
static const ULONG PullOn8411[] = { PAIR(CARD_PULL_CTL1, 0xAA), PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xA9),
                                    PAIR(CARD_PULL_CTL4, 0x09), PAIR(CARD_PULL_CTL5, 0x09), PAIR(CARD_PULL_CTL6, 0x04), 0 };
static const ULONG PullOff8411[] = { PAIR(CARD_PULL_CTL1, 0x65), PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0x95),
                                     PAIR(CARD_PULL_CTL4, 0x09), PAIR(CARD_PULL_CTL5, 0x05), PAIR(CARD_PULL_CTL6, 0x04), 0 };
static const ULONG PullOn8411b64[] = { PAIR(CARD_PULL_CTL1, 0xAA), PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xD9),
                                       PAIR(CARD_PULL_CTL4, 0x59), PAIR(CARD_PULL_CTL5, 0x55), PAIR(CARD_PULL_CTL6, 0x15), 0 };
static const ULONG PullOff8411b64[] = { PAIR(CARD_PULL_CTL1, 0x65), PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xD5),
                                        PAIR(CARD_PULL_CTL4, 0x59), PAIR(CARD_PULL_CTL5, 0x55), PAIR(CARD_PULL_CTL6, 0x15), 0 };
static const ULONG PullOn8411b48[] = { PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xF9), PAIR(CARD_PULL_CTL6, 0x19), 0 };
static const ULONG PullOff8411b48[] = { PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xF5), PAIR(CARD_PULL_CTL6, 0x15), 0 };
static const ULONG PullOn5261[] = { PAIR(CARD_PULL_CTL2, 0xAA), PAIR(CARD_PULL_CTL3, 0xE9), 0 };
static const ULONG PullOff5261[] = { PAIR(CARD_PULL_CTL2, 0x55), PAIR(CARD_PULL_CTL3, 0xD5), 0 };

/* ------------------------------------------------------------------ */
/* RTS5209                                                             */
/* ------------------------------------------------------------------ */
static void Params5209(RTSX *r)
{
    r->CardDrive = RTS5209_CARD_DRIVE_DEFAULT;
    r->Sd30Drive1V8 = DRIVER_TYPE_B;
    r->Sd30Drive3V3 = DRIVER_TYPE_D;
    r->IcVer = (UCHAR)(MmioRd8(r->Regs + 0x1C) & 0x0F);
    r->PullOn = PullOn5209;
    r->PullOff = PullOff5209;
}

static int Extra5209(RTSX *r)
{
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_GPIO, 0xFF, 0x03);            /* LED off */
    AddCmd(r, WRITE_REG_CMD, ASPM_FORCE_CTL, 0x3F, 0);
    AddCmd(r, WRITE_REG_CMD, PETXCFG, 0x08, 0x08);              /* CLKREQ# driven low */
    AddCmd(r, WRITE_REG_CMD, CARD_GPIO_DIR, 0xFF, 0x03);
    AddCmd(r, WRITE_REG_CMD, SD30_DRIVE_SEL, 0xFF, r->Sd30Drive3V3);
    return SendCmd(r, 100);
}

static int Phy5209(RTSX *r) { return WritePhy(r, 0x00, 0xB966); }
static int Phy5229(RTSX *r) { return WritePhy(r, 0x00, 0xBA42); }

static int On5209(RTSX *r)
{
    int e;
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_PARTIAL_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x04);
    e = SendCmd(r, 100);
    if (e != SD_OK) return e;
    OsStallUs(150);                                             /* in-rush current */
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x00);
    return SendCmd(r, 100);
}

static int Off5209(RTSX *r)
{
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK | PMOS_STRG_MASK, SD_POWER_OFF | PMOS_STRG_400mA);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x06);
    return SendCmd(r, 100);
}

static int V5209(RTSX *r)                       /* also RTS5229 */
{
    int e = WriteReg(r, SD30_DRIVE_SEL, 0x07, r->Sd30Drive3V3);
    if (e != SD_OK) return e;
    return WritePhy(r, 0x08, 0x4FC0 | 0x24);
}

static int Blink5209(RTSX *r) { return WriteReg(r, CARD_AUTO_BLINK, 0xFF, 0x0D); }
static int BlinkBase(RTSX *r) { return WriteReg(r, OLT_LED_CTL, 0x08, 0x08); }
static int BlinkShine(RTSX *r) { return WriteReg(r, OLT_LED_CTL, LED_SHINE_MASK, LED_SHINE_EN); }

/* ------------------------------------------------------------------ */
/* RTS5229                                                             */
/* ------------------------------------------------------------------ */
static void Params5229(RTSX *r)
{
    r->CardDrive = RTSX_CARD_DRIVE_DEFAULT;
    r->Sd30Drive1V8 = DRIVER_TYPE_B;
    r->Sd30Drive3V3 = DRIVER_TYPE_D;
    r->IcVer = (UCHAR)(Rd8(r, DUMMY_REG_RESET_0) & 0x0F);
    r->PullOn = r->IcVer == IC_VER_C ? PullOn5229c : PullOn5229a;
    r->PullOff = r->IcVer == IC_VER_C ? PullOff5229c : PullOff5229a;
}

static int Extra5229(RTSX *r)
{
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, GPIO_CTL, 0x02, 0x02);
    AddCmd(r, WRITE_REG_CMD, ASPM_FORCE_CTL, 0x3F, 0);
    AddCmd(r, WRITE_REG_CMD, PETXCFG, 0x08, 0x08);
    AddCmd(r, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x00);         /* LDO3318 from card_3v3 */
    AddCmd(r, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x01);
    AddCmd(r, WRITE_REG_CMD, OLT_LED_CTL, 0x0F, 0x02);
    AddCmd(r, WRITE_REG_CMD, SD30_DRIVE_SEL, 0xFF, r->Sd30Drive3V3);
    return SendCmd(r, 100);
}

static int On5229(RTSX *r)
{
    int e;
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_PARTIAL_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x02);
    e = SendCmd(r, 100);
    if (e != SD_OK) return e;
    OsStallUs(150);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x06);
    return SendCmd(r, 100);
}

static int Off5229(RTSX *r)
{
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK | PMOS_STRG_MASK, SD_POWER_OFF | PMOS_STRG_400mA);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x00);
    return SendCmd(r, 100);
}

/* ------------------------------------------------------------------ */
/* RTS5227 / RTS522A                                                   */
/* ------------------------------------------------------------------ */
static void Params5227(RTSX *r)
{
    r->CardDrive = RTSX_CARD_DRIVE_DEFAULT;
    r->Sd30Drive1V8 = CFG_DRIVER_TYPE_B;
    r->Sd30Drive3V3 = CFG_DRIVER_TYPE_B;
    r->IcVer = (UCHAR)(Rd8(r, DUMMY_REG_RESET_0) & 0x0F);
    r->PullOn = PullOn5229a;
    r->PullOff = PullOff5229a;
    r->PmCtrl3 = PM_CTRL3;
    if (r->Pid == 0x522A) {
        r->PmCtrl3 = RTS522A_PM_CTRL3;
        r->OcpEn = 1;
        r->OcpGlitch = SD_OCP_GLITCH_10M;
        r->OcpThd = RTS522A_OCP_THD_800;
    }
}

/* PCI Express capability: LTR enable bit of Device Control 2 */
static BOOLEAN LtrEnabled(RTSX *r)
{
    return r->PcieCap && (OsPciRead(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x28, 2) & 0x0400) != 0;
}

static void OobsPolling(RTSX *r, BOOLEAN on)
{
    USHORT v = 0;
    if (r->Pid != 0x525A && r->Pid != 0x5260 && r->Pid != 0x5264) {
        ReadPhy(r, 0x01, &v);
        if (on) v |= 1 << 9;
        else v &= ~(1 << 9);
        WritePhy(r, 0x01, v);
    }
    WriteReg(r, REG_CFG_VCM_ON_TIMER, 0xFF, on ? 0x40 : 0x03);
    WriteReg(r, REG_CFG_OOBS_POLLING, 0xFF, on ? 0xDE : 0x00);
}

static int Extra5227(RTSX *r)
{
    if (r->Pid == 0x522A) OobsPolling(r, TRUE);     /* no L1 substates here */
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, GPIO_CTL, 0x02, 0x02);
    AddCmd(r, WRITE_REG_CMD, ASPM_FORCE_CTL, 0x3F, 0);
    AddCmd(r, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x00);
    AddCmd(r, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x01);
    AddCmd(r, WRITE_REG_CMD, OLT_LED_CTL, 0x0F, 0x02);
    if (LtrEnabled(r)) AddCmd(r, WRITE_REG_CMD, LTR_CTL, 0xFF, 0xA3);
    AddCmd(r, WRITE_REG_CMD, OBFF_CFG, 0x03, 0x03);
    FillDriving(r, Drv5227, TRUE);
    AddCmd(r, WRITE_REG_CMD, PETXCFG, 0x30, r->ReverseSocket ? 0x30 : 0x00);
    if (r->Pid == 0x522A) AddCmd(r, WRITE_REG_CMD, RTS522A_AUTOLOAD_CFG1, CD_RESUME_EN_MASK, CD_RESUME_EN_MASK);
    if (r->Pid == 0x522A) {
        AddCmd(r, WRITE_REG_CMD, RTS522A_PM_CTRL3, 0x01, r->Rtd3 ? 0x01 : 0x00);
        AddCmd(r, WRITE_REG_CMD, RTS522A_PME_FORCE_CTL, 0x30, r->Rtd3 ? 0x30 : 0x20);
    } else if (r->Rtd3) {
        AddCmd(r, WRITE_REG_CMD, PM_CTRL3, 0x01, 0x01);
        AddCmd(r, WRITE_REG_CMD, PME_FORCE_CTL, 0xFF, 0x33);
    } else {
        AddCmd(r, WRITE_REG_CMD, PME_FORCE_CTL, 0xFF, 0x30);
        AddCmd(r, WRITE_REG_CMD, PM_CTRL3, 0x01, 0x00);
    }
    AddCmd(r, WRITE_REG_CMD, PETXCFG, FORCE_CLKREQ_DELINK_MASK, FORCE_CLKREQ_LOW);
    AddCmd(r, WRITE_REG_CMD, r->PmCtrl3, 0x10, 0x00);
    if (SendCmd(r, 100) != SD_OK) return SD_EIO;
    if (r->Pid == 0x522A) {
        if (!r->CardExist) WriteReg(r, FPDCTL, OC_POWER_DOWN, OC_POWER_DOWN);
        WriteReg(r, FUNC_FORCE_CTL, FUNC_FORCE_UPME_XMT_DBG, FUNC_FORCE_UPME_XMT_DBG);
        WriteReg(r, PCLK_CTL, 0x04, 0x04);
        WriteReg(r, PM_EVENT_DEBUG, PME_DEBUG_0, PME_DEBUG_0);
        WriteReg(r, PM_CLK_FORCE_CTL, 0xFF, 0x11);
    }
    return SD_OK;
}

static int Phy5227(RTSX *r)
{
    int e = WriteReg(r, PM_CTRL3, D3_DELINK_MODE_EN, 0x00);
    if (e != SD_OK) return e;
    return WritePhy(r, 0x00, 0xBA42);
}

static int Phy522a(RTSX *r)
{
    int e = WriteReg(r, RTS522A_PM_CTRL3, D3_DELINK_MODE_EN, 0x00);
    if (e != SD_OK) return e;
    if (r->IcVer == IC_VER_A) {
        e = WritePhy(r, PHY_RCR2, PHY_RCR2_INIT_27S);
        if (e != SD_OK) return e;
        WritePhy(r, PHY_RCR1, PHY_RCR1_INIT_27S);
        WritePhy(r, PHY_FLD0, PHY_FLD0_INIT_27S);
        WritePhy(r, PHY_FLD3, PHY_FLD3_INIT_27S);
        WritePhy(r, PHY_FLD4, PHY_FLD4_INIT_27S);
    }
    return SD_OK;
}

static int On5227(RTSX *r)
{
    int e;
    if (r->OcpEn) EnableOcp(r);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_PARTIAL_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x02);
    e = SendCmd(r, 100);
    if (e != SD_OK) return e;
    OsSleepMs(20);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x06);
    AddCmd(r, WRITE_REG_CMD, CARD_OE, SD_OUTPUT_EN, SD_OUTPUT_EN);
    AddCmd(r, WRITE_REG_CMD, CARD_OE, MS_OUTPUT_EN, MS_OUTPUT_EN);
    return SendCmd(r, 100);
}

static int Off5227(RTSX *r)
{
    if (r->OcpEn) DisableOcp(r);
    WriteReg(r, CARD_PWR_CTL, SD_POWER_MASK | PMOS_STRG_MASK, SD_POWER_OFF | PMOS_STRG_400mA);
    WriteReg(r, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x00);
    return SD_OK;
}

static int V5227(RTSX *r)
{
    int e = WritePhy(r, 0x08, r->Pid == 0x522A ? 0x57E4 : (0x4FC0 | 0x24));
    if (e != SD_OK) return e;
    InitCmd(r);
    FillDriving(r, Drv5227, TRUE);
    return SendCmd(r, 100);
}

/* ------------------------------------------------------------------ */
/* RTS5249 / RTS524A / RTS525A                                         */
/* ------------------------------------------------------------------ */
static void Params5249(RTSX *r)
{
    r->CardDrive = RTSX_CARD_DRIVE_DEFAULT;
    r->Sd30Drive1V8 = CFG_DRIVER_TYPE_B;
    r->Sd30Drive3V3 = CFG_DRIVER_TYPE_B;
    r->IcVer = (UCHAR)(Rd8(r, DUMMY_REG_RESET_0) & 0x0F);
    r->PullOn = PullOn5249;
    r->PullOff = PullOff5249;
    r->PmCtrl3 = PM_CTRL3;
    if (r->Pid != 0x5249) {
        r->PmCtrl3 = RTS524A_PM_CTRL3;
        r->OcpEn = 1;
        r->OcpGlitch = SD_OCP_GLITCH_10M;
        r->OcpThd = r->Pid == 0x524A ? RTS524A_OCP_THD_800 : RTS525A_OCP_THD_800;
    }
}

static UCHAR EfuseRead(RTSX *r, UCHAR addr)
{
    UCHAR t = 0, v = 0;
    int j;
    WriteReg(r, RTS525A_EFUSE_ADD, REG_EFUSE_ADD_MASK, addr);
    WriteReg(r, RTS525A_EFUSE_CTL, REG_EFUSE_ENABLE | REG_EFUSE_MODE, REG_EFUSE_ENABLE);
    for (j = 0; j < 1024; j++) {
        ReadReg(r, RTS525A_EFUSE_CTL, &t);
        if (!(t & 0x80)) break;
    }
    ReadReg(r, RTS525A_EFUSE_DATA, &v);
    return v;
}

static void Ids(RTSX *r, USHORT at)
{
    WriteReg(r, at, 0xFF, 0xEC);                                /* vendor 10EC */
    WriteReg(r, (USHORT)(at + 1), 0xFF, 0x10);
    WriteReg(r, (USHORT)(at + 2), 0xFF, (UCHAR)r->Pid);
    WriteReg(r, (USHORT)(at + 3), 0xFF, (UCHAR)(r->Pid >> 8));
}

/* RTS524A / RTS525A keep a copy of their efuse content in the autoload space */
static void EfuseToAutoload(RTSX *r)
{
    UCHAR v, cnt, sv;
    int i;
    WriteReg(r, RTS524A_PME_FORCE_CTL, REG_EFUSE_BYPASS | REG_EFUSE_POR, REG_EFUSE_POR);
    OsStallUs(1);
    v = EfuseRead(r, 0x00);
    cnt = (UCHAR)(v & 0x0F);
    sv = (UCHAR)(v & 0x10);
    if (sv) {
        for (i = 0; i < 4; i++) WriteReg(r, (USHORT)(0xFF04 + i), 0xFF, EfuseRead(r, (UCHAR)(0x04 + i)));
    } else {
        Ids(r, 0xFF04);
    }
    for (i = 0; i < cnt * 4; i++)
        WriteReg(r, (USHORT)(0xFF08 + i), 0xFF, EfuseRead(r, (UCHAR)((sv ? 0x08 : 0x04) + i)));
    WriteReg(r, 0xFF00, 0xFF, (UCHAR)((cnt & 0x7F) | 0x80));
    WriteReg(r, RTS524A_PME_FORCE_CTL, REG_EFUSE_BYPASS | REG_EFUSE_POR, REG_EFUSE_BYPASS);
}

static void SaveAutoload(RTSX *r)
{
    UCHAR v = Rd8(r, RESET_LOAD_REG);
    if (v & 0x02) {
        v = Rd8(r, RTS525A_BIOS_CFG);
        if (v & RTS525A_LOAD_BIOS_FLAG) {
            WriteReg(r, RTS525A_BIOS_CFG, RTS525A_LOAD_BIOS_FLAG, RTS525A_CLEAR_BIOS_FLAG);
            WriteReg(r, RTS524A_PME_FORCE_CTL, REG_EFUSE_POWER_MASK, REG_EFUSE_POWERON);
            OsStallUs(1000);
            EfuseToAutoload(r);
        } else if (!(Rd8(r, RTS524A_PME_FORCE_CTL) & 0x08)) {
            EfuseToAutoload(r);
        }
    } else {
        WriteReg(r, 0xFF00, 0xFF, 0x80);
        Ids(r, 0xFF04);
    }
}

static int Extra5249(RTSX *r)
{
    BOOLEAN a = r->Pid == 0x524A || r->Pid == 0x525A;
    if (a) OobsPolling(r, TRUE);
    if (a) SaveAutoload(r);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, L1SUB_CONFIG3, 0xFF, 0x00);
    AddCmd(r, WRITE_REG_CMD, GPIO_CTL, 0x02, 0x02);
    AddCmd(r, WRITE_REG_CMD, ASPM_FORCE_CTL, 0x3F, 0);
    AddCmd(r, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x00);
    AddCmd(r, WRITE_REG_CMD, LDO_PWR_SEL, 0x03, 0x01);
    AddCmd(r, WRITE_REG_CMD, OLT_LED_CTL, 0x0F, 0x02);
    FillDriving(r, Drv5249, TRUE);
    AddCmd(r, WRITE_REG_CMD, PETXCFG, 0xB0, r->ReverseSocket ? 0xB0 : 0x80);
    SendCmd(r, 100);
    if (a) {
        WriteReg(r, REG_VREF, PWD_SUSPND_EN, PWD_SUSPND_EN);
        WriteReg(r, RTS524A_AUTOLOAD_CFG1, CD_RESUME_EN_MASK, CD_RESUME_EN_MASK);
        WriteReg(r, RTS524A_PM_CTRL3, 0x01, r->Rtd3 ? 0x01 : 0x00);
        WriteReg(r, RTS524A_PME_FORCE_CTL, 0x30, r->Rtd3 ? 0x30 : 0x20);
    } else if (r->Rtd3) {
        WriteReg(r, PM_CTRL3, 0x01, 0x01);
        WriteReg(r, PME_FORCE_CTL, 0xFF, 0x33);
    } else {
        WriteReg(r, PME_FORCE_CTL, 0xFF, 0x30);
        WriteReg(r, PM_CTRL3, 0x01, 0x00);
    }
    WriteReg(r, PETXCFG, FORCE_CLKREQ_DELINK_MASK, FORCE_CLKREQ_LOW);
    WriteReg(r, r->PmCtrl3, 0x10, 0x00);
    if (a) WriteReg(r, RTS524A_PME_FORCE_CTL, REG_EFUSE_POWER_MASK, REG_EFUSE_POWEROFF);
    if (r->Pid == 0x524A) {
        WriteReg(r, FUNC_FORCE_CTL, FORCE_ASPM_L1_EN, FORCE_ASPM_L1_EN);
        WriteReg(r, PM_EVENT_DEBUG, PME_DEBUG_0, PME_DEBUG_0);
        WriteReg(r, LDO_VCC_CFG1, LDO_VCC_LMT_EN, LDO_VCC_LMT_EN);
        WriteReg(r, PCLK_CTL, PCLK_MODE_SEL, PCLK_MODE_SEL);
        if (r->IcVer == IC_VER_A) {
            WriteReg(r, LDO_DV18_CFG, LDO_DV18_SR_MASK, LDO_DV18_SR_DF);
            WriteReg(r, LDO_VCC_CFG1, LDO_VCC_REF_TUNE_MASK, LDO_VCC_REF_1V2);
            WriteReg(r, LDO_VIO_CFG, LDO_VIO_REF_TUNE_MASK, LDO_VIO_REF_1V2);
            WriteReg(r, LDO_VIO_CFG, LDO_VIO_SR_MASK, LDO_VIO_SR_DF);
            WriteReg(r, LDO_DV12S_CFG, LDO_REF12_TUNE_MASK, LDO_REF12_TUNE_DF);
            WriteReg(r, SD40_LDO_CTL1, SD40_VIO_TUNE_MASK, SD40_VIO_TUNE_1V7);
        }
    } else if (r->Pid == 0x525A) {
        WriteReg(r, RTS5250_CLK_CFG3, RTS525A_CFG_MEM_PD, RTS525A_CFG_MEM_PD);
        WriteReg(r, PCLK_CTL, PCLK_MODE_SEL, PCLK_MODE_SEL);
        if (r->IcVer == IC_VER_A) {
            WriteReg(r, L1SUB_CONFIG2, L1SUB_AUTO_CFG, L1SUB_AUTO_CFG);
            WriteReg(r, RREF_CFG, RREF_VBGSEL_MASK, RREF_VBGSEL_1V25);
            WriteReg(r, LDO_VIO_CFG, LDO_VIO_TUNE_MASK, LDO_VIO_1V7);
            WriteReg(r, LDO_DV12S_CFG, LDO_D12_TUNE_MASK, LDO_D12_TUNE_DF);
            WriteReg(r, LDO_AV12S_CFG, LDO_AV12S_TUNE_MASK, LDO_AV12S_TUNE_DF);
            WriteReg(r, LDO_VCC_CFG0, LDO_VCC_LMTVTH_MASK, LDO_VCC_LMTVTH_2A);
            WriteReg(r, OOBS_CONFIG, OOBS_AUTOK_DIS | OOBS_VAL_MASK, 0x89);
        }
    }
    return SD_OK;
}

static int Phy5249(RTSX *r)
{
    int e = WriteReg(r, PM_CTRL3, D3_DELINK_MODE_EN, 0x00);
    if (e != SD_OK) return e;
    e = WritePhy(r, PHY_REV, PHY_REV_RESV | PHY_REV_RXIDLE_LATCHED | PHY_REV_P1_EN | PHY_REV_RXIDLE_EN |
                 PHY_REV_CLKREQ_TX_EN | PHY_REV_RX_PWST | PHY_REV_CLKREQ_DT_1_0 | PHY_REV_STOP_CLKRD |
                 PHY_REV_STOP_CLKWR);
    if (e != SD_OK) return e;
    OsSleepMs(1);
    e = WritePhy(r, PHY_BPCR, PHY_BPCR_IBRXSEL | PHY_BPCR_IBTXSEL | PHY_BPCR_IB_FILTER | PHY_BPCR_CMIRROR_EN);
    if (e == SD_OK)
        e = WritePhy(r, PHY_PCR, PHY_PCR_FORCE_CODE | PHY_PCR_OOBS_CALI_50 | PHY_PCR_OOBS_VCM_08 |
                     PHY_PCR_OOBS_SEN_90 | PHY_PCR_RSSI_EN | PHY_PCR_RX10K);
    if (e == SD_OK)
        e = WritePhy(r, PHY_RCR2, PHY_RCR2_EMPHASE_EN | PHY_RCR2_NADJR | PHY_RCR2_CDR_SR_2 | PHY_RCR2_FREQSEL_12 |
                     PHY_RCR2_CDR_SC_12P | PHY_RCR2_CALIB_LATE);
    if (e == SD_OK)
        e = WritePhy(r, PHY_FLD4, PHY_FLD4_FLDEN_SEL | PHY_FLD4_REQ_REF | PHY_FLD4_RXAMP_OFF | PHY_FLD4_REQ_ADDA |
                     PHY_FLD4_BER_COUNT | PHY_FLD4_BER_TIMER | PHY_FLD4_BER_CHK_EN);
    if (e == SD_OK) e = WritePhy(r, PHY_RDR, PHY_RDR_RXDSEL_1_9 | PHY_SSC_AUTO_PWD);
    if (e == SD_OK) e = WritePhy(r, PHY_RCR1, PHY_RCR1_ADP_TIME_4 | PHY_RCR1_VCO_COARSE);
    if (e == SD_OK) e = WritePhy(r, PHY_FLD3, PHY_FLD3_TIMER_4 | PHY_FLD3_TIMER_6 | PHY_FLD3_RXDELINK);
    if (e == SD_OK)
        e = WritePhy(r, PHY_TUNE, PHY_TUNE_TUNEREF_1_0 | PHY_TUNE_VBGSEL_1252 | PHY_TUNE_SDBUS_33 |
                     PHY_TUNE_TUNED18 | PHY_TUNE_TUNED12 | PHY_TUNE_TUNEA12);
    return e;
}

static int Phy524a(RTSX *r)
{
    int e = WriteReg(r, RTS524A_PM_CTRL3, D3_DELINK_MODE_EN, 0x00);
    if (e != SD_OK) return e;
    WritePhy(r, PHY_PCR, PHY_PCR_FORCE_CODE | PHY_PCR_OOBS_CALI_50 | PHY_PCR_OOBS_VCM_08 | PHY_PCR_OOBS_SEN_90 |
             PHY_PCR_RSSI_EN);
    WritePhy(r, PHY_SSCCR3, PHY_SSCCR3_STEP_IN | PHY_SSCCR3_CHECK_DELAY);
    if (r->IcVer == IC_VER_A) {
        WritePhy(r, PHY_SSCCR3, PHY_SSCCR3_STEP_IN | PHY_SSCCR3_CHECK_DELAY);
        WritePhy(r, PHY_SSCCR2, PHY_SSCCR2_PLL_NCODE | PHY_SSCCR2_TIME0 | PHY_SSCCR2_TIME2_WIDTH);
        WritePhy(r, PHY_ANA1A, PHY_ANA1A_TXR_LOOPBACK | PHY_ANA1A_RXT_BIST | PHY_ANA1A_TXR_BIST | PHY_ANA1A_REV);
        WritePhy(r, PHY_ANA1D, PHY_ANA1D_DEBUG_ADDR);
        WritePhy(r, PHY_DIG1E, PHY_DIG1E_REV | PHY_DIG1E_D0_X_D1 | PHY_DIG1E_RX_ON_HOST | PHY_DIG1E_RCLK_REF_HOST |
                 PHY_DIG1E_RCLK_TX_EN_KEEP | PHY_DIG1E_RCLK_TX_TERM_KEEP | PHY_DIG1E_RCLK_RX_EIDLE_ON |
                 PHY_DIG1E_TX_TERM_KEEP | PHY_DIG1E_RX_TERM_KEEP | PHY_DIG1E_TX_EN_KEEP | PHY_DIG1E_RX_EN_KEEP);
    }
    WritePhy(r, PHY_ANA08, PHY_ANA08_RX_EQ_DCGAIN | PHY_ANA08_SEL_RX_EN | PHY_ANA08_RX_EQ_VAL | PHY_ANA08_SCP |
             PHY_ANA08_SEL_IPI);
    return SD_OK;
}

static int Phy525a(RTSX *r)
{
    int e = WriteReg(r, RTS524A_PM_CTRL3, D3_DELINK_MODE_EN, 0x00);
    if (e != SD_OK) return e;
    WritePhy(r, _PHY_FLD0, _PHY_FLD0_CLK_REQ_20C | _PHY_FLD0_RX_IDLE_EN | _PHY_FLD0_BIT_ERR_RSTN |
             _PHY_FLD0_BER_COUNT | _PHY_FLD0_BER_TIMER | _PHY_FLD0_CHECK_EN);
    WritePhy(r, _PHY_ANA03, _PHY_ANA03_TIMER_MAX | _PHY_ANA03_OOBS_DEB_EN | _PHY_CMU_DEBUG_EN);
    if (r->IcVer == IC_VER_A)
        WritePhy(r, _PHY_REV0, _PHY_REV0_FILTER_OUT | _PHY_REV0_CDR_BYPASS_PFD | _PHY_REV0_CDR_RX_IDLE_BYPASS);
    return SD_OK;
}

static int OnBase(RTSX *r)
{
    int e;
    if (r->Pid == 0x525A) WriteReg(r, LDO_VCC_CFG1, LDO_VCC_TUNE_MASK, LDO_VCC_3V3);
    if (r->OcpEn) EnableOcp(r);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_VCC_PARTIAL_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x02);
    e = SendCmd(r, 100);
    if (e != SD_OK) return e;
    OsSleepMs(5);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, SD_POWER_MASK, SD_VCC_POWER_ON);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x06);
    return SendCmd(r, 100);
}

static int OffBase(RTSX *r)
{
    if (r->OcpEn) DisableOcp(r);
    WriteReg(r, CARD_PWR_CTL, SD_POWER_MASK, SD_POWER_OFF);
    WriteReg(r, PWR_GATE_CTRL, LDO3318_PWR_MASK, 0x00);
    return SD_OK;
}

static int V5249(RTSX *r)
{
    int e;
    if (r->Pid == 0x525A) {
        WriteReg(r, LDO_CONFIG2, LDO_D3318_MASK, LDO_D3318_33V);
        WriteReg(r, SD_PAD_CTL, SD_IO_USING_1V8, 0);
    } else {
        e = UpdatePhy(r, PHY_TUNE, PHY_TUNE_VOLTAGE_MASK, PHY_TUNE_VOLTAGE_3V3);
        if (e != SD_OK) return e;
    }
    InitCmd(r);
    FillDriving(r, Drv5249, TRUE);
    return SendCmd(r, 100);
}

/* ------------------------------------------------------------------ */
/* RTL8411 / RTL8411B / RTL8402                                        */
/* ------------------------------------------------------------------ */
static void Params8411(RTSX *r)
{
    r->CardDrive = RTL8411_CARD_DRIVE_DEFAULT;
    r->Sd30Drive1V8 = DRIVER_TYPE_B;
    r->Sd30Drive3V3 = DRIVER_TYPE_D;
    r->IcVer = (UCHAR)(Rd8(r, SYS_VER) & 0x0F);
    r->PullOn = PullOn8411;
    r->PullOff = PullOff8411;
    if (r->Pid == 0x5287) {
        r->Qfn48 = (UCHAR)((Rd8(r, RTL8411B_PACKAGE_MODE) & 0x02) != 0);
        r->PullOn = r->Qfn48 ? PullOn8411b48 : PullOn8411b64;
        r->PullOff = r->Qfn48 ? PullOff8411b48 : PullOff8411b64;
    }
}

static int Extra8411(RTSX *r)
{
    InitCmd(r);
    if (r->Pid == 0x5287 && r->Qfn48) AddCmd(r, WRITE_REG_CMD, CARD_PULL_CTL3, 0xFF, 0xF5);
    AddCmd(r, WRITE_REG_CMD, SD30_DRIVE_SEL, 0xFF, r->Sd30Drive3V3);
    AddCmd(r, WRITE_REG_CMD, CD_PAD_CTL, CD_DISABLE_MASK | CD_AUTO_DISABLE, CD_ENABLE);
    if (r->Pid == 0x5287) AddCmd(r, WRITE_REG_CMD, FUNC_FORCE_CTL, 0x06, 0x00);
    return SendCmd(r, 100);
}

static int On8411(RTSX *r)
{
    int e;
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_5_PERCENT_ON);
    AddCmd(r, WRITE_REG_CMD, LDO_CTL, BPP_LDO_POWB, BPP_LDO_SUSPEND);
    e = SendCmd(r, 100);
    if (e != SD_OK) return e;
    OsStallUs(150);
    if ((e = WriteReg(r, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_10_PERCENT_ON)) != SD_OK) return e;
    OsStallUs(150);
    if ((e = WriteReg(r, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_15_PERCENT_ON)) != SD_OK) return e;
    OsStallUs(150);
    if ((e = WriteReg(r, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_ON)) != SD_OK) return e;
    return WriteReg(r, LDO_CTL, BPP_LDO_POWB, BPP_LDO_ON);
}

static int Off8411(RTSX *r)
{
    int e = WriteReg(r, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_OFF);
    if (e != SD_OK) return e;
    return WriteReg(r, LDO_CTL, BPP_LDO_POWB, BPP_LDO_SUSPEND);
}

static int V8411(RTSX *r)
{
    int shift = r->Pid == 0x5286 ? BPP_TUNED18_SHIFT_8402 : BPP_TUNED18_SHIFT_8411;
    int e = WriteReg(r, SD30_DRIVE_SEL, 0x07, r->Sd30Drive3V3);
    if (e != SD_OK) return e;
    return WriteReg(r, LDO_CTL, (UCHAR)((BPP_REG_TUNED18 << shift) | BPP_PAD_MASK),
                    (UCHAR)((BPP_ASIC_3V3 << shift) | BPP_PAD_3V3));
}

/* RTL8411: SD and MemoryStick share the card detect pins */
static ULONG Deglitch8411(RTSX *r)
{
    ULONG ce = RD32(r, RTSX_BIPR) & CARD_EXIST;
    if (!ce) {
        WriteReg(r, CD_PAD_CTL, CD_DISABLE_MASK, CD_ENABLE);
        WriteReg(r, EFUSE_CONTENT, 0xE0, 0x00);
        return 0;
    }
    if ((ce & (ce - 1)) != 0) {                                 /* more than one bit */
        WriteReg(r, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_5_PERCENT_ON);
        OsSleepMs(100);
        ce = RD32(r, RTSX_BIPR);
        ce = (ce & MS_EXIST) ? MS_EXIST : (ce & SD_EXIST) ? SD_EXIST : 0;
        WriteReg(r, CARD_PWR_CTL, BPP_POWER_MASK, BPP_POWER_OFF);
    }
    if (ce & MS_EXIST) {
        WriteReg(r, EFUSE_CONTENT, 0xE0, 0x40);
        WriteReg(r, CD_PAD_CTL, CD_DISABLE_MASK, MS_CD_EN_ONLY);
    } else if (ce & SD_EXIST) {
        WriteReg(r, EFUSE_CONTENT, 0xE0, 0x80);
        WriteReg(r, CD_PAD_CTL, CD_DISABLE_MASK, SD_CD_EN_ONLY);
    }
    return ce;
}

/* ------------------------------------------------------------------ */
/* RTS5260                                                             */
/* ------------------------------------------------------------------ */
static void Params5260(RTSX *r)
{
    r->CardDrive = RTSX_CARD_DRIVE_DEFAULT;
    r->Sd30Drive1V8 = CFG_DRIVER_TYPE_B;
    r->Sd30Drive3V3 = CFG_DRIVER_TYPE_B;
    r->IcVer = (UCHAR)(Rd8(r, DUMMY_REG_RESET_0) & IC_VERSION_MASK);
    r->PullOn = PullOn5249;
    r->PullOff = PullOff5249;
    r->PmCtrl3 = RTS524A_PM_CTRL3;
    r->OcpEn = 1;
    r->OcpGlitch = SD_OCP_GLITCH_100U | SDVIO_OCP_GLITCH_800U;
    r->OcpThd = RTS5260_DVCC_OCP_THD_970;
}

static void SampleSd30(RTSX *r)
{
    WriteReg(r, SD_CFG1, SD_MODE_SELECT_MASK | SD_ASYNC_FIFO_NOT_RST, SD_30_MODE | SD_ASYNC_FIFO_NOT_RST);
    WriteReg(r, CLK_CTL, CLK_LOW_FREQ, CLK_LOW_FREQ);
    WriteReg(r, CARD_CLK_SOURCE, 0xFF, CRC_VAR_CLK0 | SD30_FIX_CLK | SAMPLE_VAR_CLK1);
    WriteReg(r, CLK_CTL, CLK_LOW_FREQ, 0);
}

static int On5260(RTSX *r)
{
    if (r->OcpEn) EnableOcp(r);
    WriteReg(r, LDO_CONFIG2, DV331812_VDD1, DV331812_VDD1);
    WriteReg(r, LDO_VCC_CFG0, RTS5260_DVCC_TUNE_MASK, RTS5260_DVCC_33);
    WriteReg(r, LDO_VCC_CFG1, LDO_POW_SDVDD1_MASK, LDO_POW_SDVDD1_ON);
    WriteReg(r, LDO_CONFIG2, DV331812_POWERON, DV331812_POWERON);
    OsSleepMs(20);
    SampleSd30(r);
    WriteReg(r, SD_CFG1, 0xFF, SD_CLK_DIVIDE_128 | SD_20_MODE);
    WriteReg(r, SD_SAMPLE_POINT_CTL, 0xFF, SD20_RX_POS_EDGE);
    WriteReg(r, SD_PUSH_POINT_CTL, 0xFF, 0);
    WriteReg(r, CARD_STOP, SD_STOP | SD_CLR_ERR, SD_STOP | SD_CLR_ERR);
    WriteReg(r, SD_CFG3, SD30_CLK_END_EN, 0);
    WriteReg(r, REG_SD_STOP_SDCLK_CFG, SD30_CLK_STOP_CFG_EN | SD30_CLK_STOP_CFG1 | SD30_CLK_STOP_CFG0, 0);
    WriteReg(r, REG_PRE_RW_MODE, EN_INFINITE_MODE, 0);
    return SD_OK;
}

static int V5260(RTSX *r)
{
    WriteReg(r, LDO_CONFIG2, DV331812_VDD1, DV331812_VDD1);
    WriteReg(r, LDO_DV18_CFG, DV331812_MASK, DV331812_33);
    WriteReg(r, SD_PAD_CTL, SD_IO_USING_1V8, 0);
    FillDriving(r, Drv5260, FALSE);
    return SD_OK;
}

static void Stop5260(RTSX *r)               /* also RTS5261 / RTS5228 */
{
    WR32(r, RTSX_HCBCTLR, STOP_CMD);
    WR32(r, RTSX_HDBCTLR, STOP_DMA);
    WriteReg(r, RTS5260_DMA_RST_CTL_0, RTS5260_DMA_RST | RTS5260_ADMA3_RST, RTS5260_DMA_RST | RTS5260_ADMA3_RST);
    WriteReg(r, RBCTL, RB_FLUSH, RB_FLUSH);
}

static int Off5260(RTSX *r)
{
    Stop5260(r);
    V5260(r);
    WriteReg(r, LDO_VCC_CFG1, LDO_POW_SDVDD1_MASK, LDO_POW_SDVDD1_OFF);
    WriteReg(r, LDO_CONFIG2, DV331812_POWERON, DV331812_POWEROFF);
    if (r->OcpEn) DisableOcp(r);
    return SD_OK;
}

static void EnOcp5260(RTSX *r) { WriteReg(r, REG_OCPCTL, 0xFF, SD_OCP_INT_EN | SD_DETECT_EN); }
static void DisOcp5260(RTSX *r) { WriteReg(r, REG_OCPCTL, SD_OCP_INT_EN | SD_DETECT_EN, 0); }

static void InitOcp5260(RTSX *r)
{
    if (r->OcpEn) {
        WriteReg(r, RTS5260_DVCC_CTRL, RTS5260_DVCC_OCP_THD_MASK, r->OcpThd);
        WriteReg(r, RTS5260_DV331812_CFG, RTS5260_DV331812_OCP_THD_MASK, RTS5260_DV331812_OCP_THD_270);
        WriteReg(r, REG_OCPGLITCH, SD_OCP_GLITCH_MASK, r->OcpGlitch);
        WriteReg(r, RTS5260_DVCC_CTRL, RTS5260_DVCC_OCP_EN | RTS5260_DVCC_OCP_CL_EN,
                 RTS5260_DVCC_OCP_EN | RTS5260_DVCC_OCP_CL_EN);
        EnOcp5260(r);
    } else {
        WriteReg(r, RTS5260_DVCC_CTRL, RTS5260_DVCC_OCP_EN | RTS5260_DVCC_OCP_CL_EN, 0);
    }
}

static int Extra5260(RTSX *r)
{
    WriteReg(r, 0xFC03, 0x7F, 0x07);
    WriteReg(r, SSC_DIV_N_0, 0xFF, 0x5D);
    /* power saving set-up for L1 without substates */
    WriteReg(r, ASPM_FORCE_CTL, 0xFF, 0);
    WriteReg(r, PWR_GLOBAL_CTRL, 0xFF, PCIE_L1_0_EN);
    WriteReg(r, PWR_FE_CTL, 0xFF, PCIE_L1_0_PD_FE_EN);
    WriteReg(r, CFG_L1_0_PCIE_DPHY_RET_VALUE, 0xFF, CFG_L1_0_RET_VALUE_DEFAULT);
    WriteReg(r, CFG_L1_0_PCIE_MAC_RET_VALUE, 0xFF, CFG_L1_0_RET_VALUE_DEFAULT);
    WriteReg(r, CFG_L1_0_CRC_SD30_RET_VALUE, 0xFF, CFG_L1_0_RET_VALUE_DEFAULT);
    WriteReg(r, CFG_L1_0_CRC_SD40_RET_VALUE, 0xFF, CFG_L1_0_RET_VALUE_DEFAULT);
    WriteReg(r, CFG_L1_0_SYS_RET_VALUE, 0xFF, CFG_L1_0_RET_VALUE_DEFAULT);
    WriteReg(r, CFG_PCIE_APHY_OFF_0, 0xFF, CFG_PCIE_APHY_OFF_0_DEFAULT);
    WriteReg(r, CFG_PCIE_APHY_OFF_1, 0xFF, CFG_PCIE_APHY_OFF_1_DEFAULT);
    WriteReg(r, CFG_PCIE_APHY_OFF_2, 0xFF, CFG_PCIE_APHY_OFF_2_DEFAULT);
    WriteReg(r, CFG_PCIE_APHY_OFF_3, 0xFF, CFG_PCIE_APHY_OFF_3_DEFAULT);
    WriteReg(r, PWC_CDR, 0xFF, PWC_CDR_DEFAULT);
    WriteReg(r, CFG_LP_FPWM_VALUE, 0xFF, CFG_LP_FPWM_VALUE_DEFAULT);
    WriteReg(r, CFG_L1_0_CRC_MISC_RET_VALUE, 0xFF, CFG_L1_0_CRC_MISC_RET_VALUE_DEFAULT);
    WriteReg(r, RTS5260_AUTOLOAD_CFG4, 0xFF, RTS5260_MIMO_DISABLE);
    WriteReg(r, LDO_VCC_CFG0, RTS5260_DVCC_TUNE_MASK, RTS5260_DVCC_33);
    WriteReg(r, PCLK_CTL, PCLK_MODE_SEL, PCLK_MODE_SEL);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, L1SUB_CONFIG1, AUX_CLK_ACTIVE_SEL_MASK, MAC_CKSW_DONE);
    AddCmd(r, WRITE_REG_CMD, L1SUB_CONFIG3, 0xFF, 0x00);
    AddCmd(r, WRITE_REG_CMD, PM_CLK_FORCE_CTL, CLK_PM_EN, CLK_PM_EN);
    AddCmd(r, WRITE_REG_CMD, PWD_SUSPEND_EN, 0xFF, 0xFF);
    AddCmd(r, WRITE_REG_CMD, PWR_GATE_CTRL, PWR_GATE_EN, PWR_GATE_EN);
    AddCmd(r, WRITE_REG_CMD, REG_VREF, PWD_SUSPND_EN, PWD_SUSPND_EN);
    AddCmd(r, WRITE_REG_CMD, RBCTL, U_AUTO_DMA_EN_MASK, U_AUTO_DMA_DISABLE);
    AddCmd(r, WRITE_REG_CMD, PETXCFG, 0xB0, r->ReverseSocket ? 0xB0 : 0x80);
    AddCmd(r, WRITE_REG_CMD, OBFF_CFG, OBFF_EN_MASK, OBFF_DISABLE);
    if (SendCmd(r, 100) != SD_OK) return SD_EIO;
    InitOcp5260(r);
    WriteReg(r, PETXCFG, FORCE_CLKREQ_DELINK_MASK, FORCE_CLKREQ_LOW);
    WriteReg(r, r->PmCtrl3, 0x10, 0x00);
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* RTS5261 / RTS5228 / RTS5264                                         */
/* ------------------------------------------------------------------ */
static void Params5261(RTSX *r)
{
    r->CardDrive = RTSX_CARD_DRIVE_DEFAULT;
    r->IcVer = (UCHAR)(Rd8(r, DUMMY_REG_RESET_0) & (r->Pid == 0x5264 ? 0x0F : IC_VERSION_MASK));
    r->PullOn = PullOn5261;
    r->PullOff = PullOff5261;
    r->OcpEn = 1;
    if (r->Pid == 0x5261) {
        r->Sd30Drive1V8 = r->Sd30Drive3V3 = 0;
        r->PmCtrl3 = RTS5261_AUTOLOAD_CFG3;
        r->OcpGlitch = SD_OCP_GLITCH_800U;
        r->OcpThd = RTS5261_LDO1_OCP_THD_1040;
    } else if (r->Pid == 0x5228) {
        r->Sd30Drive1V8 = r->Sd30Drive3V3 = CFG_DRIVER_TYPE_B;
        r->PmCtrl3 = RTS5228_AUTOLOAD_CFG3;
        r->OcpGlitch = SD_OCP_GLITCH_800U;
        r->OcpThd = RTS5228_LDO1_OCP_THD_930;
    } else {
        r->Sd30Drive1V8 = r->Sd30Drive3V3 = 0;
        r->PmCtrl3 = RTS5264_AUTOLOAD_CFG3;
        r->OcpGlitch = SD_OCP_GLITCH_800U | SDVIO_OCP_GLITCH_800U;
        r->OcpThd = RTS5264_LDO1_OCP_THD_1150;
    }
}

static void Vendor5261x(RTSX *r)
{
    /* done inside the extra init (needs the chip awake) */
    (void)r;
}

static const UCHAR (*Drive5261x(RTSX *r))[3]
{
    return r->Pid == 0x5261 ? Drv5261 : r->Pid == 0x5228 ? Drv5228 : Drv5264;
}

static int On5261(RTSX *r)
{
    if (r->OcpEn) EnableOcp(r);
    WriteReg(r, REG_CRC_DUMMY_0, CFG_SD_POW_AUTO_PD, CFG_SD_POW_AUTO_PD);
    if (r->Pid == 0x5228) {
        WriteReg(r, RTS5228_LDO1_CFG1, RTS5228_LDO1_TUNE_MASK, RTS5228_LDO1_33);
        WriteReg(r, RTS5228_LDO1233318_POW_CTL, RTS5228_LDO1_POWERON_MASK, RTS5228_LDO1_SOFTSTART);
        OsStallUs(2000);
        WriteReg(r, RTS5228_LDO1233318_POW_CTL, RTS5228_LDO1_POWERON_MASK, RTS5228_LDO1_FULLON);
        WriteReg(r, RTS5228_LDO1233318_POW_CTL, RTS5228_LDO3318_POWERON, RTS5228_LDO3318_POWERON);
    } else if (r->Pid == 0x5261) {
        WriteReg(r, RTS5261_LDO1_CFG1, RTS5261_LDO1_TUNE_MASK, RTS5261_LDO1_33);
        WriteReg(r, RTS5261_LDO1233318_POW_CTL, RTS5261_LDO1_POWERON, RTS5261_LDO1_POWERON);
        WriteReg(r, RTS5261_LDO1233318_POW_CTL, RTS5261_LDO3318_POWERON, RTS5261_LDO3318_POWERON);
    } else {
        WriteReg(r, RTS5264_LDO1_CFG1, RTS5264_LDO1_TUNE_MASK, RTS5264_LDO1_33);
        WriteReg(r, RTS5264_LDO1233318_POW_CTL, RTS5264_LDO1_POWERON, RTS5264_LDO1_POWERON);
        WriteReg(r, RTS5264_LDO1233318_POW_CTL, RTS5264_LDO3318_POWERON, RTS5264_LDO3318_POWERON);
    }
    OsSleepMs(20);
    WriteReg(r, CARD_OE, SD_OUTPUT_EN, SD_OUTPUT_EN);
    WriteReg(r, SD_CFG1, 0xFF, SD_CLK_DIVIDE_128 | SD_20_MODE | SD_BUS_WIDTH_1BIT);
    WriteReg(r, SD_SAMPLE_POINT_CTL, 0xFF, SD20_RX_POS_EDGE);
    WriteReg(r, SD_PUSH_POINT_CTL, 0xFF, 0);
    WriteReg(r, CARD_STOP, SD_STOP | SD_CLR_ERR, SD_STOP | SD_CLR_ERR);
    WriteReg(r, SD_CFG3, SD30_CLK_END_EN, 0);
    WriteReg(r, REG_SD_STOP_SDCLK_CFG, SD30_CLK_STOP_CFG_EN | SD30_CLK_STOP_CFG1 | SD30_CLK_STOP_CFG0, 0);
    SampleSd30(r);
    return SD_OK;
}

static int V5261(RTSX *r)
{
    USHORT v = 0;
    int e;
    if (r->Pid == 0x5264) {
        WriteReg(r, RTS5264_CARD_PWR_CTL, RTS5264_PUPDC, RTS5264_PUPDC);
        WriteReg(r, RTS5264_LDO1233318_POW_CTL, RTS5264_TUNE_REF_LDO3318, RTS5264_TUNE_REF_LDO3318);
        WriteReg(r, RTS5264_DV3318_CFG, RTS5264_DV3318_TUNE_MASK, RTS5264_DV3318_33);
    } else {
        WriteReg(r, r->Pid == 0x5261 ? RTS5261_CARD_PWR_CTL : RTS5228_CARD_PWR_CTL, RTS5261_PUPDC, RTS5261_PUPDC);
        ReadPhy(r, PHY_TUNE, &v);
        e = WritePhy(r, PHY_TUNE, (USHORT)(v | PHY_TUNE_SDBUS_33));
        if (e != SD_OK) return e;
        WriteReg(r, r->Pid == 0x5261 ? RTS5261_DV3318_CFG : RTS5228_DV3318_CFG, RTS5261_DV3318_TUNE_MASK,
                 RTS5261_DV3318_33);
    }
    WriteReg(r, SD_PAD_CTL, SD_IO_USING_1V8, 0);
    FillDriving(r, Drive5261x(r), FALSE);
    return SD_OK;
}

static void Stop5264(RTSX *r)
{
    WR32(r, RTSX_HCBCTLR, STOP_CMD);
    WR32(r, RTSX_HDBCTLR, STOP_DMA);
    WriteReg(r, DMACTL, DMA_RST, DMA_RST);
    WriteReg(r, RBCTL, RB_FLUSH, RB_FLUSH);
}

static void EnOcp5261(RTSX *r)
{
    if (r->Pid == 0x5264) {
        WriteReg(r, RTS5264_LDO1_CFG0, RTS5264_LDO1_OCP_EN | RTS5264_LDO1_OCP_LMT_EN,
                 RTS5264_LDO1_OCP_EN | RTS5264_LDO1_OCP_LMT_EN);
        WriteReg(r, RTS5264_LDO2_CFG0, RTS5264_LDO2_OCP_EN | RTS5264_LDO2_OCP_LMT_EN,
                 RTS5264_LDO2_OCP_EN | RTS5264_LDO2_OCP_LMT_EN);
        WriteReg(r, RTS5264_LDO3_CFG0, RTS5264_LDO3_OCP_EN | RTS5264_LDO3_OCP_LMT_EN,
                 RTS5264_LDO3_OCP_EN | RTS5264_LDO3_OCP_LMT_EN);
        WriteReg(r, RTS5264_OVP_DET, RTS5264_POW_VDET, RTS5264_POW_VDET);
        WriteReg(r, REG_OCPCTL, SD_OCP_INT_EN | SD_DETECT_EN | SDVIO_OCP_INT_EN | SDVIO_DETECT_EN,
                 SD_OCP_INT_EN | SD_DETECT_EN | SDVIO_OCP_INT_EN | SDVIO_DETECT_EN);
        WriteReg(r, RTS5264_OCP_VDD3_CTL, SD_VDD3_OCP_INT_EN | SD_VDD3_DETECT_EN,
                 SD_VDD3_OCP_INT_EN | SD_VDD3_DETECT_EN);
        WriteReg(r, RTS5264_OVP_CTL, RTS5264_OVP_INT_EN | RTS5264_OVP_DETECT_EN,
                 RTS5264_OVP_INT_EN | RTS5264_OVP_DETECT_EN);
        return;
    }
    if (r->Pid == 0x5261) {
        WriteReg(r, RTS5261_LDO1_CFG0, RTS5261_LDO1_OCP_EN | RTS5261_LDO1_OCP_LMT_EN,
                 RTS5261_LDO1_OCP_EN | RTS5261_LDO1_OCP_LMT_EN);
        WriteReg(r, REG_OCPCTL, 0xFF, SD_OCP_INT_EN | SD_DETECT_EN);
    } else {
        WriteReg(r, REG_OCPCTL, 0xFF, SD_OCP_INT_EN | SD_DETECT_EN);
        WriteReg(r, RTS5228_LDO1_CFG0, RTS5228_LDO1_OCP_EN | RTS5228_LDO1_OCP_LMT_EN,
                 RTS5228_LDO1_OCP_EN | RTS5228_LDO1_OCP_LMT_EN);
    }
}

static void DisOcp5261(RTSX *r)
{
    if (r->Pid == 0x5264) {
        WriteReg(r, REG_OCPCTL, SD_OCP_INT_EN | SD_DETECT_EN | SDVIO_OCP_INT_EN | SDVIO_DETECT_EN, 0);
        WriteReg(r, RTS5264_OCP_VDD3_CTL, SD_VDD3_OCP_INT_EN | SD_VDD3_DETECT_EN, 0);
        WriteReg(r, RTS5264_OVP_CTL, RTS5264_OVP_INT_EN | RTS5264_OVP_DETECT_EN, 0);
        WriteReg(r, RTS5264_LDO1_CFG0, RTS5264_LDO1_OCP_EN | RTS5264_LDO1_OCP_LMT_EN, 0);
        WriteReg(r, RTS5264_LDO2_CFG0, RTS5264_LDO2_OCP_EN | RTS5264_LDO2_OCP_LMT_EN, 0);
        WriteReg(r, RTS5264_LDO3_CFG0, RTS5264_LDO3_OCP_EN | RTS5264_LDO3_OCP_LMT_EN, 0);
        WriteReg(r, RTS5264_OVP_DET, RTS5264_POW_VDET, 0);
        return;
    }
    WriteReg(r, REG_OCPCTL, SD_OCP_INT_EN | SD_DETECT_EN, 0);
    WriteReg(r, r->Pid == 0x5261 ? RTS5261_LDO1_CFG0 : RTS5228_LDO1_CFG0,
             RTS5261_LDO1_OCP_EN | RTS5261_LDO1_OCP_LMT_EN, 0);
}

static void InitOcp5261(RTSX *r)
{
    if (r->Pid == 0x5264) {
        if (!r->OcpEn) {
            DisOcp5261(r);
            return;
        }
        WriteReg(r, RTS5264_LDO1_CFG0, RTS5264_LDO1_OCP_THD_MASK, r->OcpThd);
        WriteReg(r, RTS5264_LDO1_CFG0, RTS5264_LDO1_OCP_LMT_THD_MASK, RTS5264_LDO1_LMT_THD_2000);
        WriteReg(r, RTS5264_LDO2_CFG0, RTS5264_LDO2_OCP_THD_MASK, RTS5264_LDO2_OCP_THD_950);
        WriteReg(r, RTS5264_LDO2_CFG0, RTS5264_LDO2_OCP_LMT_THD_MASK, RTS5264_LDO2_LMT_THD_2000);
        WriteReg(r, RTS5264_LDO3_CFG0, RTS5264_LDO3_OCP_THD_MASK, RTS5264_LDO3_OCP_THD_710);
        WriteReg(r, RTS5264_LDO3_CFG0, RTS5264_LDO3_OCP_LMT_THD_MASK, RTS5264_LDO3_LMT_THD_1500);
        WriteReg(r, RTS5264_OVP_DET, RTS5264_TUNE_VROV_MASK, RTS5264_TUNE_VROV_1V6);
        WriteReg(r, REG_OCPGLITCH, SD_OCP_GLITCH_MASK | SDVIO_OCP_GLITCH_MASK, r->OcpGlitch);
        return;
    }
    {
        USHORT cfg0 = r->Pid == 0x5261 ? RTS5261_LDO1_CFG0 : RTS5228_LDO1_CFG0;
        if (!r->OcpEn) {
            WriteReg(r, cfg0, RTS5261_LDO1_OCP_EN | RTS5261_LDO1_OCP_LMT_EN, 0);
            return;
        }
        WriteReg(r, cfg0, RTS5261_LDO1_OCP_EN | RTS5261_LDO1_OCP_LMT_EN, RTS5261_LDO1_OCP_EN | RTS5261_LDO1_OCP_LMT_EN);
        WriteReg(r, cfg0, RTS5261_LDO1_OCP_THD_MASK, r->OcpThd);
        WriteReg(r, cfg0, RTS5261_LDO1_OCP_LMT_THD_MASK,
                 r->Pid == 0x5261 ? RTS5261_LDO1_LMT_THD_2000 : RTS5228_LDO1_LMT_THD_1500);
        WriteReg(r, REG_OCPGLITCH, SD_OCP_GLITCH_MASK, r->OcpGlitch);
        EnOcp5261(r);
    }
}

static int Off5261(RTSX *r)
{
    if (r->Pid == 0x5264) Stop5264(r);
    else Stop5260(r);
    V5261(r);
    WriteReg(r, r->Pid == 0x5261 ? RTS5261_LDO1233318_POW_CTL :
             r->Pid == 0x5228 ? RTS5228_LDO1233318_POW_CTL : RTS5264_LDO1233318_POW_CTL,
             r->Pid == 0x5228 ? RTS5228_LDO_POWERON_MASK : RTS5261_LDO_POWERON_MASK, 0);
    WriteReg(r, REG_CRC_DUMMY_0, CFG_SD_POW_AUTO_PD, 0);
    if (r->OcpEn) DisOcp5261(r);
    return SD_OK;
}

static int Phy5228(RTSX *r) { return WritePhy(r, 0x07, 0x8F40); }

static int Extra5261(RTSX *r)
{
    USHORT autoload1 = r->Pid == 0x5261 ? RTS5261_AUTOLOAD_CFG1 : r->Pid == 0x5228 ? RTS5228_AUTOLOAD_CFG1 :
                       RTS5264_AUTOLOAD_CFG1;
    USHORT pme = r->Pid == 0x5261 ? RTS5261_REG_PME_FORCE_CTL : r->Pid == 0x5228 ? RTS5228_REG_PME_FORCE_CTL :
                 RTS5264_REG_PME_FORCE_CTL;
    WriteReg(r, autoload1, CD_RESUME_EN_MASK, CD_RESUME_EN_MASK);
    if (r->Pid == 0x5264) WriteReg(r, REG_VREF, PWD_SUSPND_EN, PWD_SUSPND_EN);
    if (r->Pid != 0x5261) {
        OobsPolling(r, TRUE);
        WriteReg(r, ASPM_FORCE_CTL, 0xFF, 0);
    }
    if (r->Pid == 0x5261) Vendor5261(r, pme, RTS5261_EFUSE_ADDR, RTS5261_EFUSE_CTL, RTS5261_EFUSE_READ_DATA);
    else if (r->Pid == 0x5264) Vendor5261(r, pme, RTS5264_EFUSE_ADDR, RTS5264_EFUSE_CTL, RTS5264_EFUSE_READ_DATA);
    if (r->Pid != 0x5228) WriteReg(r, pme, REG_EFUSE_POWER_MASK, REG_EFUSE_POWEROFF);
    if (r->Pid == 0x5264) {
        WriteReg(r, RTS5264_AUTOLOAD_CFG2, RTS5264_CHIP_RST_N_SEL, 0);
        WriteReg(r, RTS5264_REG_LDO12_CFG, RTS5264_LDO12_SR_MASK, RTS5264_LDO12_SR_0_0_MS);
        WriteReg(r, CDGW, 0xFF, 0x01);
        WriteReg(r, RTS5264_CKMUX_MBIAS_PWR, RTS5264_POW_CKMUX, RTS5264_POW_CKMUX);
        WriteReg(r, RTS5264_CMD_OE_START_EARLY, RTS5264_CMD_OE_EARLY_EN | RTS5264_CMD_OE_EARLY_CYCLE_MASK,
                 RTS5264_CMD_OE_EARLY_EN);
        WriteReg(r, RTS5264_DAT_OE_START_EARLY, RTS5264_DAT_OE_EARLY_EN | RTS5264_DAT_OE_EARLY_CYCLE_MASK,
                 RTS5264_DAT_OE_EARLY_EN);
        WriteReg(r, SSC_DIV_N_0, 0xFF, 0x5D);
        WriteReg(r, RTS5264_PWR_CUT, RTS5264_CFG_MEM_PD, RTS5264_CFG_MEM_PD);
    }
    WriteReg(r, L1SUB_CONFIG1, AUX_CLK_ACTIVE_SEL_MASK, MAC_CKSW_DONE);
    WriteReg(r, L1SUB_CONFIG3, 0xFF, 0);
    if (r->Pid == 0x5261 && r->IcVer > IC_VER_B) WR32(r, RTSX_DUM_REG, RD32(r, RTSX_DUM_REG) | 0x1);
    if (r->Pid == 0x5261) {
        WriteReg(r, RTS5261_AUTOLOAD_CFG4, RTS5261_AUX_CLK_16M_EN, 0);
        WriteReg(r, RTS5261_AUTOLOAD_CFG4, RTS5261_FORCE_PRSNT_LOW, 0);
    } else if (r->Pid == 0x5264) {
        WriteReg(r, RTS5264_AUTOLOAD_CFG4, RTS5264_AUX_CLK_16M_EN, 0);
        WriteReg(r, RTS5264_AUTOLOAD_CFG4, RTS5264_FORCE_PRSNT_LOW, 0);
    }
    if (r->Pid != 0x5264) WriteReg(r, FUNC_FORCE_CTL, FUNC_FORCE_UPME_XMT_DBG, FUNC_FORCE_UPME_XMT_DBG);
    WriteReg(r, PCLK_CTL, PCLK_MODE_SEL, PCLK_MODE_SEL);
    if (r->Pid != 0x5264) {
        WriteReg(r, PM_EVENT_DEBUG, PME_DEBUG_0, PME_DEBUG_0);
        WriteReg(r, PM_CLK_FORCE_CTL, CLK_PM_EN, CLK_PM_EN);
    }
    WriteReg(r, OLT_LED_CTL, 0x0F, 0x02);
    FillDriving(r, Drive5261x(r), FALSE);
    WriteReg(r, PETXCFG, 0x30, r->ReverseSocket ? 0x30 : 0x00);
    WriteReg(r, PETXCFG, FORCE_CLKREQ_DELINK_MASK, FORCE_CLKREQ_LOW);
    if (r->Pid == 0x5264) {
        WriteReg(r, PWD_SUSPEND_EN, 0xFF, 0xFF);
        WriteReg(r, RBCTL, U_AUTO_DMA_EN_MASK, 0);
        WriteReg(r, RTS5264_AUTOLOAD_CFG4, RTS5264_F_HIGH_RC_MASK, RTS5264_F_HIGH_RC_400K);
        WriteReg(r, r->PmCtrl3, 0x01, 0x00);
        WriteReg(r, pme, FORCE_PM_CONTROL | FORCE_PM_VALUE, r->Rtd3 ? 0 : FORCE_PM_CONTROL);
    } else {
        WriteReg(r, PWD_SUSPEND_EN, 0xFF, 0xFB);
        WriteReg(r, r->PmCtrl3, 0x01, r->Rtd3 ? 0x01 : 0x00);
        WriteReg(r, pme, FORCE_PM_CONTROL | FORCE_PM_VALUE,
                 r->Rtd3 ? FORCE_PM_CONTROL | FORCE_PM_VALUE : FORCE_PM_CONTROL);
    }
    WriteReg(r, r->PmCtrl3, D3_DELINK_MODE_EN, 0x00);
    if (r->Pid == 0x5261) WriteReg(r, RTS5261_FW_CTL, RTS5261_INFORM_RTD3_COLD, 0);
    if (r->Pid == 0x5264) WriteReg(r, RTS5264_FW_CTL, RTS5264_INFORM_RTD3_COLD, 0);
    return SD_OK;
}

/* ------------------------------------------------------------------ */
/* chip table                                                          */
/* ------------------------------------------------------------------ */
/*  pid    name        params      vendor       phy       extra      on      off      3v3    init ocp     en ocp
    dis ocp     stop      blink    clk bar sg aspm conv alias */
static const CHIP Chips[] = {
    { 0x5209, "RTS5209", Params5209, Vendor5209, Phy5209, Extra5209, On5209, Off5209, V5209, NULL, NULL,
      NULL, NULL, Blink5209, 0, 0, 0, 0, 0, 0 },
    { 0x5229, "RTS5229", Params5229, Vendor5229, Phy5229, Extra5229, On5229, Off5229, V5209, NULL, NULL,
      NULL, NULL, BlinkBase, 0, 0, 0, 0, 0, 0 },
    { 0x5289, "RTL8411", Params8411, Vendor5229, NULL, Extra8411, On8411, Off8411, V8411, NULL, NULL,
      NULL, NULL, Blink5209, 0, 0, 0, 0, 1, 0 },
    { 0x5227, "RTS5227", Params5227, VendorBase, Phy5227, Extra5227, On5227, Off5227, V5227, NULL, NULL,
      NULL, NULL, BlinkBase, 0, 0, 0, 0, 0, 0 },
    { 0x522A, "RTS522A", Params5227, VendorBase, Phy522a, Extra5227, On5227, Off5227, V5227, NULL, NULL,
      NULL, NULL, BlinkBase, 0, 0, 0, 1, 0, 0 },
    { 0x5249, "RTS5249", Params5249, VendorBase, Phy5249, Extra5249, OnBase, OffBase, V5249, NULL, NULL,
      NULL, NULL, BlinkBase, 0, 0, 0, 0, 0, 0 },
    { 0x5287, "RTL8411B", Params8411, Vendor8411b, NULL, Extra8411, On8411, Off8411, V8411, NULL, NULL,
      NULL, NULL, Blink5209, 0, 0, 0, 0, 1, 0 },
    { 0x5286, "RTL8402", Params8411, Vendor5229, NULL, Extra8411, On8411, Off8411, V8411, NULL, NULL,
      NULL, NULL, Blink5209, 0, 0, 0, 0, 1, 0 },
    { 0x524A, "RTS524A", Params5249, VendorBase, Phy524a, Extra5249, OnBase, OffBase, V5249, NULL, NULL,
      NULL, NULL, BlinkBase, 0, 0, 0, 1, 0, 1 },
    { 0x525A, "RTS525A", Params5249, VendorBase, Phy525a, Extra5249, OnBase, OffBase, V5249, NULL, NULL,
      NULL, NULL, BlinkBase, 0, 1, 0, 1, 0, 0 },
    { 0x5260, "RTS5260", Params5260, VendorBase, NULL, Extra5260, On5260, Off5260, V5260, InitOcp5260, EnOcp5260,
      DisOcp5260, Stop5260, BlinkShine, 0, 0, 0, 1, 0, 0 },
    { 0x5261, "RTS5261", Params5261, Vendor5261x, NULL, Extra5261, On5261, Off5261, V5261, InitOcp5261, EnOcp5261,
      DisOcp5261, Stop5260, BlinkShine, 1, 0, 1, 1, 0, 0 },
    { 0x5228, "RTS5228", Params5261, Vendor5228, Phy5228, Extra5261, On5261, Off5261, V5261, InitOcp5261, EnOcp5261,
      DisOcp5261, Stop5260, BlinkShine, 1, 0, 1, 1, 0, 0 },
    { 0x5264, "RTS5264", Params5261, Vendor5261x, NULL, Extra5261, On5261, Off5261, V5261, InitOcp5261, EnOcp5261,
      DisOcp5261, Stop5264, BlinkShine, 2, 1, 0, 1, 0, 0 },
};

/* ------------------------------------------------------------------ */
/* clock                                                               */
/* ------------------------------------------------------------------ */
static int Conv8411(int in, BOOLEAN toDivN) { return toDivN ? in * 4 / 5 - 2 : (in + 2) * 5 / 4; }

static int SwitchClock(RTSX *r, ULONG hz, BOOLEAN initial)
{
    /* SD legacy / high speed: SSC depth 500 kHz, double clock, no phase reset */
    static const UCHAR depthGen = SSC_DEPTH_500K;
    int e, clk, n, div = CLK_DIV_1, mcu, depth;
    UCHAR divider = initial ? SD_CLK_DIVIDE_128 : SD_CLK_DIVIDE_0;
    if (initial) hz = 30000000;
    e = WriteReg(r, SD_CFG1, SD_CLK_DIVIDE_MASK, divider);
    if (e != SD_OK) return e;
    clk = (int)(hz / 1000000);
    if (!initial) clk *= 2;
    if ((ULONG)clk == r->CurClock) return SD_OK;
    if (r->Chip->Clock == 0) {
        n = r->Chip->Conv8411 ? Conv8411(clk, TRUE) : clk - 2;
        if (clk <= 2 || n > MAX_DIV_N_PCR) return SD_EIO;
        mcu = 125 / clk + 3;
        if (mcu > 15) mcu = 15;
        while (n < MIN_DIV_N_PCR && div < CLK_DIV_8) {
            if (r->Chip->Conv8411) n = Conv8411(Conv8411(n, FALSE) * 2, TRUE);
            else n = (n + 2) * 2 - 2;
            div++;
        }
        depth = depthGen;
        if (depth > 1) depth--;                                 /* double clock */
        if (div > CLK_DIV_1) depth = depth > div - 1 ? depth - (div - 1) : SSC_DEPTH_4M;
    } else {
        n = clk - 4;
        if (clk <= 4 || n > 396) return SD_EIO;
        mcu = 125 / clk + 3;
        if (mcu > 15) mcu = 15;
        while (n < MIN_DIV_N_PCR - 4 && div < CLK_DIV_8) {
            n = (n + 4) * 2 - 4;
            div++;
        }
        n = n / 2 - 1;
        depth = RTS5261_SSC_DEPTH_512K;                         /* the same value on 5228/5264 */
        if (depth > 1) depth--;
        if (depth) {
            int k = div == CLK_DIV_2 ? 1 : div == CLK_DIV_4 ? 2 : div == CLK_DIV_8 ? 3 : 0;
            if (k) depth = depth > k ? depth - k : RTS5261_SSC_DEPTH_8M;
        }
    }
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CLK_CTL, r->Chip->Clock == 2 ? CHANGE_CLK : CLK_LOW_FREQ,
           r->Chip->Clock == 2 ? CHANGE_CLK : CLK_LOW_FREQ);
    AddCmd(r, WRITE_REG_CMD, CLK_DIV, 0xFF, (UCHAR)((div << 4) | mcu));
    AddCmd(r, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, 0);
    AddCmd(r, WRITE_REG_CMD, SSC_CTL2, SSC_DEPTH_MASK, (UCHAR)depth);
    AddCmd(r, WRITE_REG_CMD, SSC_DIV_N_0, 0xFF, (UCHAR)n);
    if (r->Chip->Clock == 2 && r->IcVer == IC_VER_A) {
        AddCmd(r, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, 0);
        AddCmd(r, WRITE_REG_CMD, RTS5264_CARD_CLK_SRC2, RTS5264_REG_BIG_KVCO_A, 0);
    } else {
        AddCmd(r, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, SSC_RSTB);
        if (r->Chip->Clock == 2) AddCmd(r, WRITE_REG_CMD, RTS5264_SYS_DUMMY_1, RTS5264_REG_BIG_KVCO, 0);
    }
    e = SendCmd(r, 2000);
    if (e != SD_OK) return e;
    OsStallUs(SSC_CLOCK_STABLE_WAIT);
    e = WriteReg(r, CLK_CTL, r->Chip->Clock == 2 ? CHANGE_CLK : CLK_LOW_FREQ, 0);
    if (e == SD_OK) r->CurClock = (ULONG)clk;
    return e;
}

/* ------------------------------------------------------------------ */
/* SD host operations                                                  */
/* ------------------------------------------------------------------ */
static ULONG CardExist(RTSX *r)
{
    if (r->Chip->Conv8411) return Deglitch8411(r);
    return RD32(r, RTSX_BIPR);
}

static int HStatus(void *Ctx, BOOLEAN *Present, BOOLEAN *Wp)
{
    RTSX *r = (RTSX *)Ctx;
    ULONG v = RD32(r, RTSX_BIPR);
    if (v == 0xFFFFFFFFUL) return SD_ENODEV;
    if (r->Chip->Conv8411) v = (v & ~CARD_EXIST) | (CardExist(r) & CARD_EXIST);
    *Present = (v & SD_EXIST) != 0;
    *Wp = (v & SD_WRITE_PROTECT) != 0;
    if (v & SD_INT) WR32(r, RTSX_BIPR, SD_INT);
    return SD_OK;
}

static int SetTiming(RTSX *r, BOOLEAN hs)
{
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, SD_CFG1, 0x0C, SD_20_MODE);
    AddCmd(r, WRITE_REG_CMD, CLK_CTL, CLK_LOW_FREQ, CLK_LOW_FREQ);
    AddCmd(r, WRITE_REG_CMD, CARD_CLK_SOURCE, 0xFF, CRC_FIX_CLK | SD30_VAR_CLK0 | SAMPLE_VAR_CLK1);
    AddCmd(r, WRITE_REG_CMD, CLK_CTL, CLK_LOW_FREQ, 0);
    if (hs) {
        AddCmd(r, WRITE_REG_CMD, SD_PUSH_POINT_CTL, SD20_TX_SEL_MASK, SD20_TX_14_AHEAD);
        AddCmd(r, WRITE_REG_CMD, SD_SAMPLE_POINT_CTL, SD20_RX_SEL_MASK, SD20_RX_14_DELAY);
    } else {
        AddCmd(r, WRITE_REG_CMD, SD_PUSH_POINT_CTL, 0xFF, 0);
        AddCmd(r, WRITE_REG_CMD, SD_SAMPLE_POINT_CTL, SD20_RX_SEL_MASK, SD20_RX_POS_EDGE);
    }
    return SendCmd(r, 100);
}

static int CardOn(RTSX *r)
{
    int e;
    OsSleepMs(100);
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_SELECT, 0x07, SD_MOD_SEL);
    AddCmd(r, WRITE_REG_CMD, CARD_SHARE_MODE, CARD_SHARE_MASK, CARD_SHARE_48_SD);
    AddCmd(r, WRITE_REG_CMD, CARD_CLK_EN, SD_CLK_EN, SD_CLK_EN);
    e = SendCmd(r, 100);
    if (e == SD_OK) e = SetPull(r, r->PullOn);
    if (e == SD_OK) e = r->Chip->PowerOn(r);
    if (e != SD_OK) return e;
    OsStallUs(1000);
    e = WriteReg(r, CARD_OE, SD_OUTPUT_EN, SD_OUTPUT_EN);
    if (e != SD_OK) return e;
    /* at least 74 clocks before the first command */
    WriteReg(r, SD_BUS_STAT, SD_CLK_TOGGLE_EN, SD_CLK_TOGGLE_EN);
    /* 3.3 V signalling */
    r->Chip->Out3V3(r);
    WriteReg(r, SD_BUS_STAT, SD_CLK_TOGGLE_EN | SD_CLK_FORCE_STOP, 0);
    return SD_OK;
}

static int CardOff(RTSX *r)
{
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CARD_CLK_EN, SD_CLK_EN, 0);
    AddCmd(r, WRITE_REG_CMD, CARD_OE, SD_OUTPUT_EN, 0);
    SendCmd(r, 100);
    r->Chip->PowerOff(r);
    return SetPull(r, r->PullOff);
}

static int HSetIos(void *Ctx, BOOLEAN Power, ULONG Hz, int Width, BOOLEAN HighSpeed)
{
    RTSX *r = (RTSX *)Ctx;
    int e;
    if (!Power) {
        if (r->Power) CardOff(r);
        r->Power = 0;
        r->CurClock = 0;
        return SD_OK;
    }
    WriteReg(r, SD_CFG1, 0x03, Width == SD_WIDTH_4 ? SD_BUS_WIDTH_4BIT : SD_BUS_WIDTH_1BIT);
    if (!r->Power) {
        e = CardOn(r);
        if (e != SD_OK) return e;
        r->Power = 1;
    }
    e = SetTiming(r, HighSpeed);
    if (e != SD_OK) return e;
    r->Initial = (UCHAR)(Hz <= 1000000);
    return SwitchClock(r, Hz ? Hz : 400000, r->Initial);
}

static ULONG Be32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

static UCHAR RspType(const SD_REQ *q)
{
    switch (q->Rsp) {
    case SD_RSP_NONE: return SD_RSP_TYPE_R0;
    case SD_RSP_R1B: return SD_RSP_TYPE_R1b;
    case SD_RSP_R2: return SD_RSP_TYPE_R2;
    case SD_RSP_R3: return SD_RSP_TYPE_R3;
    default: return SD_RSP_TYPE_R1;
    }
}

static void PutCmd(RTSX *r, const SD_REQ *q)
{
    AddCmd(r, WRITE_REG_CMD, SD_CMD0, 0xFF, (UCHAR)(SD_CMD_START | q->Cmd));
    AddCmd(r, WRITE_REG_CMD, SD_CMD1, 0xFF, (UCHAR)(q->Arg >> 24));
    AddCmd(r, WRITE_REG_CMD, SD_CMD2, 0xFF, (UCHAR)(q->Arg >> 16));
    AddCmd(r, WRITE_REG_CMD, SD_CMD3, 0xFF, (UCHAR)(q->Arg >> 8));
    AddCmd(r, WRITE_REG_CMD, SD_CMD4, 0xFF, (UCHAR)q->Arg);
}

static void PutLen(RTSX *r, ULONG blocks, ULONG blksz)
{
    AddCmd(r, WRITE_REG_CMD, SD_BLOCK_CNT_L, 0xFF, (UCHAR)blocks);
    AddCmd(r, WRITE_REG_CMD, SD_BLOCK_CNT_H, 0xFF, (UCHAR)(blocks >> 8));
    AddCmd(r, WRITE_REG_CMD, SD_BYTE_CNT_L, 0xFF, (UCHAR)blksz);
    AddCmd(r, WRITE_REG_CMD, SD_BYTE_CNT_H, 0xFF, (UCHAR)(blksz >> 8));
}

static void ClearError(RTSX *r) { WriteReg(r, CARD_STOP, SD_STOP | SD_CLR_ERR, SD_STOP | SD_CLR_ERR); }

static int CmdGetRsp(RTSX *r, SD_REQ *q)
{
    UCHAR type = RspType(q), *p;
    ULONG timeout = type == SD_RSP_TYPE_R1b ? 3000 : 100, i, stat;
    int e;
    InitCmd(r);
    PutCmd(r, q);
    AddCmd(r, WRITE_REG_CMD, SD_CFG2, 0xFF, type);
    AddCmd(r, WRITE_REG_CMD, CARD_DATA_SOURCE, 0x01, PINGPONG_BUFFER);
    AddCmd(r, WRITE_REG_CMD, SD_TRANSFER, 0xFF, SD_TM_CMD_RSP | SD_TRANSFER_START);
    AddCmd(r, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END | SD_STAT_IDLE, SD_TRANSFER_END | SD_STAT_IDLE);
    if (type == SD_RSP_TYPE_R2) {
        for (i = PPBUF_BASE2; i < PPBUF_BASE2 + 16; i++) AddCmd(r, READ_REG_CMD, (USHORT)i, 0, 0);
        stat = 16;
    } else if (type != SD_RSP_TYPE_R0) {
        for (i = SD_CMD0; i <= SD_CMD4; i++) AddCmd(r, READ_REG_CMD, (USHORT)i, 0, 0);
        stat = 5;
    } else {
        stat = 0;
    }
    AddCmd(r, READ_REG_CMD, SD_STAT1, 0, 0);
    e = SendCmd(r, timeout);
    if (e != SD_OK) {
        if (e != SD_ENODEV) ClearError(r);
        return e == SD_EIO ? SD_ETIMEOUT : e;
    }
    if (type == SD_RSP_TYPE_R0) return SD_OK;
    p = CmdData(r) + 1;                                         /* past the CHECK_REG result */
    if (p[0] & 0xC0) return SD_ECRC;                            /* start / transmission bits */
    if (!(type & SD_NO_CHECK_CRC7) && (p[stat] & SD_CRC7_ERR)) return SD_ECRC;
    if (type == SD_RSP_TYPE_R2) {
        p[16] = 1;                                              /* CRC and end bit are not kept */
        for (i = 0; i < 4; i++) q->Resp[i] = Be32(p + 1 + i * 4);
    } else {
        q->Resp[0] = Be32(p + 1);
    }
    return SD_OK;
}

static int ReadPpbuf(RTSX *r, UCHAR *buf, ULONG len)
{
    ULONG done = 0, i, n;
    int e;
    while (done < len) {
        n = len - done > 256 ? 256 : len - done;
        InitCmd(r);
        for (i = 0; i < n; i++) AddCmd(r, READ_REG_CMD, (USHORT)(PPBUF_BASE2 + done + i), 0, 0);
        e = SendCmd(r, 250);
        if (e != SD_OK) return e;
        UsbMemCpy(buf + done, CmdData(r), n);
        done += n;
    }
    return SD_OK;
}

static int WritePpbuf(RTSX *r, const UCHAR *buf, ULONG len)
{
    ULONG done = 0, i, n;
    int e;
    while (done < len) {
        n = len - done > 256 ? 256 : len - done;
        InitCmd(r);
        for (i = 0; i < n; i++) AddCmd(r, WRITE_REG_CMD, (USHORT)(PPBUF_BASE2 + done + i), 0xFF, buf[done + i]);
        e = SendCmd(r, 250);
        if (e != SD_OK) return e;
        done += n;
    }
    return SD_OK;
}

static void InitialMode(RTSX *r, BOOLEAN on)
{
    if (r->Initial) WriteReg(r, SD_CFG1, SD_CLK_DIVIDE_MASK, on ? SD_CLK_DIVIDE_128 : SD_CLK_DIVIDE_0);
}

/* short reads: SCR, SD status, switch function status */
static int ReadShort(RTSX *r, SD_REQ *q)
{
    int e;
    InitialMode(r, FALSE);
    InitCmd(r);
    PutCmd(r, q);
    PutLen(r, 1, q->BlkSize);
    AddCmd(r, WRITE_REG_CMD, SD_CFG2, 0xFF,
           SD_CALCULATE_CRC7 | SD_CHECK_CRC16 | SD_NO_WAIT_BUSY_END | SD_CHECK_CRC7 | SD_RSP_LEN_6);
    AddCmd(r, WRITE_REG_CMD, CARD_DATA_SOURCE, 0x01, PINGPONG_BUFFER);
    AddCmd(r, WRITE_REG_CMD, SD_TRANSFER, 0xFF, SD_TM_NORMAL_READ | SD_TRANSFER_START);
    AddCmd(r, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END, SD_TRANSFER_END);
    AddCmd(r, READ_REG_CMD, SD_CMD1, 0, 0);
    AddCmd(r, READ_REG_CMD, SD_CMD2, 0, 0);
    AddCmd(r, READ_REG_CMD, SD_CMD3, 0, 0);
    AddCmd(r, READ_REG_CMD, SD_CMD4, 0, 0);
    e = SendCmd(r, 200);
    if (e == SD_OK) {
        q->Resp[0] = Be32(CmdData(r) + 1);
        e = ReadPpbuf(r, q->Data, q->BlkSize);
    } else if (e != SD_ENODEV) {
        ClearError(r);
    }
    InitialMode(r, TRUE);
    return e;
}

static int WriteShort(RTSX *r, SD_REQ *q)
{
    int e = CmdGetRsp(r, q);
    if (e != SD_OK) return e;
    e = WritePpbuf(r, q->Data, q->BlkSize);
    if (e != SD_OK) return e;
    InitCmd(r);
    PutLen(r, 1, q->BlkSize);
    AddCmd(r, WRITE_REG_CMD, SD_CFG2, 0xFF,
           SD_CALCULATE_CRC7 | SD_CHECK_CRC16 | SD_NO_WAIT_BUSY_END | SD_CHECK_CRC7 | SD_RSP_LEN_0);
    AddCmd(r, WRITE_REG_CMD, SD_TRANSFER, 0xFF, SD_TRANSFER_START | SD_TM_AUTO_WRITE_3);
    AddCmd(r, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END, SD_TRANSFER_END);
    return SendCmd(r, 200);
}

/* 512 byte blocks by DMA; a read sends its command in the same run */
static int RwLong(RTSX *r, SD_REQ *q)
{
    ULONG len = q->Blocks * 512;
    UCHAR cfg2;
    int e;
    if (len > XFER_MAX || len == 0) return SD_EIO;
    if (q->Write) {
        e = CmdGetRsp(r, q);
        if (e != SD_OK) return e;
        UsbMemCpy(r->Buf, q->Data, len);
        cfg2 = SD_NO_CALCULATE_CRC7 | SD_CHECK_CRC16 | SD_NO_WAIT_BUSY_END | SD_NO_CHECK_CRC7 | SD_RSP_LEN_0 |
               SD_NO_CHECK_WAIT_CRC_TO;
    } else {
        InitialMode(r, FALSE);
        cfg2 = (UCHAR)(SD_NO_CHECK_WAIT_CRC_TO | RspType(q));
    }
    InitCmd(r);
    if (!q->Write) PutCmd(r, q);
    PutLen(r, q->Blocks, 512);
    AddCmd(r, WRITE_REG_CMD, IRQSTAT0, DMA_DONE_INT, DMA_DONE_INT);
    AddCmd(r, WRITE_REG_CMD, DMATC3, 0xFF, (UCHAR)(len >> 24));
    AddCmd(r, WRITE_REG_CMD, DMATC2, 0xFF, (UCHAR)(len >> 16));
    AddCmd(r, WRITE_REG_CMD, DMATC1, 0xFF, (UCHAR)(len >> 8));
    AddCmd(r, WRITE_REG_CMD, DMATC0, 0xFF, (UCHAR)len);
    AddCmd(r, WRITE_REG_CMD, DMACTL, 0x03 | DMA_PACK_SIZE_MASK,
           (UCHAR)((q->Write ? DMA_DIR_TO_CARD : DMA_DIR_FROM_CARD) | DMA_EN | DMA_512));
    AddCmd(r, WRITE_REG_CMD, CARD_DATA_SOURCE, 0x01, RING_BUFFER);
    AddCmd(r, WRITE_REG_CMD, SD_CFG2, 0xFF, cfg2);
    AddCmd(r, WRITE_REG_CMD, SD_TRANSFER, 0xFF,
           (UCHAR)(SD_TRANSFER_START | (q->Write ? SD_TM_AUTO_WRITE_3 : SD_TM_AUTO_READ_2)));
    AddCmd(r, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END, SD_TRANSFER_END);
    StartCmd(r);                                                /* no wait: the DMA run completes it */
    e = DmaXfer(r, len, (BOOLEAN)!q->Write, 10000);
    if (e != SD_OK && e != SD_ENODEV) ClearError(r);
    if (!q->Write) {
        InitialMode(r, TRUE);
        if (e == SD_OK) UsbMemCpy(q->Data, r->Buf, len);
    }
    return e;
}

static int HRequest(void *Ctx, SD_REQ *q)
{
    RTSX *r = (RTSX *)Ctx;
    int e;
    if (RD32(r, RTSX_BIPR) == 0xFFFFFFFFUL) return SD_ENODEV;
    if (!(CardExist(r) & SD_EXIST)) return SD_ENOMEDIUM;
    WriteReg(r, CARD_SELECT, 0x07, SD_MOD_SEL);
    WriteReg(r, CARD_SHARE_MODE, CARD_SHARE_MASK, CARD_SHARE_48_SD);
    if (q->Data == NULL) return CmdGetRsp(r, q);
    if (q->BlkSize != 512) return q->Write ? WriteShort(r, q) : ReadShort(r, q);
    e = RwLong(r, q);
    if (q->Stop) {
        SD_REQ s;
        UsbMemSet(&s, 0, sizeof(s));
        s.Cmd = 12;
        s.Rsp = SD_RSP_R1B;
        CmdGetRsp(r, &s);
    }
    return e;
}

static const SD_HOST_OPS RtsxPciOps = { HStatus, HSetIos, HRequest };

/* ------------------------------------------------------------------ */
/* set-up                                                              */
/* ------------------------------------------------------------------ */
static UCHAR FindCap(RTSX *r, UCHAR id)
{
    UCHAR p;
    int n = 0;
    if (!(OsPciRead(r->Bus, r->Dev, r->Fn, 0x06, 2) & 0x10)) return 0;
    p = (UCHAR)(OsPciRead(r->Bus, r->Dev, r->Fn, 0x34, 1) & 0xFC);
    while (p && n++ < 48) {
        if (OsPciRead(r->Bus, r->Dev, r->Fn, p, 1) == id) return p;
        p = (UCHAR)(OsPciRead(r->Bus, r->Dev, r->Fn, p + 1, 1) & 0xFC);
    }
    return 0;
}

static void AspmOff(RTSX *r)
{
    if (r->Chip->AspmReg) {
        if (r->Pid == 0x5261 || r->Pid == 0x5228 || r->Pid == 0x5264) {
            if (r->PcieCap) {
                ULONG v = OsPciRead(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x10, 2);
                OsPciWrite(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x10, v & ~3UL, 2);
            }
            WriteReg(r, ASPM_FORCE_CTL, FORCE_ASPM_VAL_MASK | FORCE_ASPM_CTL0 | FORCE_ASPM_CTL1,
                     FORCE_ASPM_CTL0 | FORCE_ASPM_CTL1);
            WriteReg(r, SD_CFG1, SD_ASYNC_FIFO_NOT_RST, 0);
        } else {
            WriteReg(r, ASPM_FORCE_CTL, FORCE_ASPM_CTL0 | FORCE_ASPM_CTL1, FORCE_ASPM_CTL0 | FORCE_ASPM_CTL1);
        }
    } else if (r->PcieCap) {
        ULONG v = OsPciRead(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x10, 2);
        OsPciWrite(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x10, v & ~3UL, 2);
    }
    OsSleepMs(10);
}

static int InitHw(RTSX *r)
{
    int e;
    if (r->Pid == 0x5228) WriteReg(r, RTS5228_LDO1_CFG1, RTS5228_LDO1_SR_TIME_MASK, RTS5228_LDO1_SR_0_5);
    WR32(r, RTSX_HCBAR, r->ResvPhys);
    /* the pending bits latch through the enable mask; the interrupt line itself stays off */
    r->Bier = TRANS_OK_INT_EN | TRANS_FAIL_INT_EN | SD_INT_EN | (r->OcpEn ? SD_OC_INT_EN : 0);
    WR32(r, RTSX_BIER, r->Bier);
    if (r->Pid == 0x5261 || r->Pid == 0x5264) {
        WriteReg(r, RTS5261_FW_CFG1, RTS5261_MCU_CLOCK_GATING, 0);
        e = WriteReg(r, RTS5261_REG_FPDCTL, SSC_POWER_DOWN, 0);
    } else {
        e = WriteReg(r, FPDCTL, SSC_POWER_DOWN, 0);
    }
    if (e != SD_OK) return e;
    OsStallUs(200);
    AspmOff(r);
    OsSleepMs(1);
    if (r->Chip->OptimizePhy) {
        e = r->Chip->OptimizePhy(r);
        if (e != SD_OK) return e;
    }
    InitCmd(r);
    AddCmd(r, WRITE_REG_CMD, CLK_DIV, 0x07, 0x07);
    AddCmd(r, WRITE_REG_CMD, HOST_SLEEP_STATE, 0x03, 0x00);
    AddCmd(r, WRITE_REG_CMD, CARD_CLK_EN, 0x1E, 0);
    AddCmd(r, WRITE_REG_CMD, CHANGE_LINK_STATE, 0x0A, 0);
    AddCmd(r, WRITE_REG_CMD, CARD_DRIVE_SEL, 0xFF, r->CardDrive);
    AddCmd(r, WRITE_REG_CMD, SSC_CTL1, 0xFF, SSC_8X_EN | SSC_SEL_4M);
    if (r->Pid == 0x5261) AddCmd(r, WRITE_REG_CMD, SSC_CTL2, 0xFF, RTS5261_SSC_DEPTH_2M);
    else if (r->Pid == 0x5228) AddCmd(r, WRITE_REG_CMD, SSC_CTL2, 0xFF, RTS5228_SSC_DEPTH_2M);
    else if (r->Pid == 0x5264 && r->IcVer == IC_VER_A) AddCmd(r, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, 0);
    else if (r->Pid == 0x5264) AddCmd(r, WRITE_REG_CMD, SSC_CTL2, 0xFF, RTS5264_SSC_DEPTH_2M);
    else AddCmd(r, WRITE_REG_CMD, SSC_CTL2, 0xFF, 0x12);
    AddCmd(r, WRITE_REG_CMD, CHANGE_LINK_STATE, 0x16, 0x10);
    AddCmd(r, WRITE_REG_CMD, IRQSTAT0, LINK_RDY_INT, LINK_RDY_INT);
    AddCmd(r, WRITE_REG_CMD, PERST_GLITCH_WIDTH, 0xFF, 0x80);
    AddCmd(r, WRITE_REG_CMD, RCCTL, 0x01, 0x00);
    AddCmd(r, WRITE_REG_CMD, NFTS_TX_CTRL, 0x02, 0);
    e = SendCmd(r, 100);
    if (e != SD_OK) return e;
    if (r->Pid == 0x524A || r->Pid == 0x525A || r->Pid == 0x5260 || r->Pid == 0x5261 || r->Pid == 0x5228 ||
        r->Pid == 0x5264)
        WriteReg(r, PM_CLK_FORCE_CTL, 1, 1);
    InitOcp(r);
    /* clock power management request, L1 entry when the host link is idle */
    if (r->PcieCap) {
        ULONG v = OsPciRead(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x10, 2);
        OsPciWrite(r->Bus, r->Dev, r->Fn, r->PcieCap + 0x10, v | 0x0100, 2);
    }
    {
        ULONG v = 0;
        if (UsbPciExt(r->Bus, r->Dev, r->Fn, 0x70C, &v, FALSE)) {
            v = (v & 0x00FFFFFFUL) | (0x5BUL << 24);
            UsbPciExt(r->Bus, r->Dev, r->Fn, 0x70C, &v, TRUE);
        }
    }
    r->CardExist = (UCHAR)((RD32(r, RTSX_BIPR) & SD_EXIST) != 0);
    if (r->Chip->ExtraInit) {
        e = r->Chip->ExtraInit(r);
        if (e != SD_OK) return e;
    }
    if (r->Chip->AspmReg) WriteReg(r, ASPM_FORCE_CTL, 0x30, 0x30);
    AspmOff(r);                                                 /* the extra init may have reset it */
    if (r->Chip->AutoBlink) r->Chip->AutoBlink(r);
    return SD_OK;
}

void RtsxPciProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, USHORT DeviceId)
{
    const CHIP *chip = NULL;
    RTSX *r;
    ULONG bar, off, cmd, i;
    int slot = -1;

    for (i = 0; i < sizeof(Chips) / sizeof(Chips[0]); i++) if (Chips[i].Pid == DeviceId) chip = &Chips[i];
    if (chip == NULL || OsGetConfig("DisableSd", 0)) return;
    for (i = 0; i < MAX_READERS; i++) if (Readers[i] == NULL) { slot = (int)i; break; }
    if (slot < 0) return;
    off = 0x10 + 4UL * chip->Bar;
    bar = OsPciRead(Bus, Dev, Fn, off, 4);
    if ((bar & 1) || (bar & ~0xFUL) == 0) return;
    if (((bar >> 1) & 3) == 2 && OsPciRead(Bus, Dev, Fn, off + 4, 4) != 0) {
        bar = UsbPciRelocateBar(Bus, Dev, Fn, off);
        if (bar == 0) {
            UsbLog(LOG_ERR, "rtsx %02x:%02x.%u: register BAR above 4 GB, reader skipped\n", Bus, Dev, Fn);
            return;
        }
    }
    r = (RTSX *)OsAlloc(sizeof(RTSX));
    if (r == NULL) return;
    r->Bus = Bus;
    r->Dev = Dev;
    r->Fn = Fn;
    r->Pid = DeviceId;
    r->Chip = chip;
    /* D0, memory space, bus master, legacy interrupt line off */
    {
        UCHAR pm;
        r->PcieCap = FindCap(r, 0x10);
        pm = FindCap(r, 0x01);
        if (pm && (OsPciRead(Bus, Dev, Fn, pm + 4, 2) & 3) != 0) {
            OsPciWrite(Bus, Dev, Fn, pm + 4, OsPciRead(Bus, Dev, Fn, pm + 4, 2) & ~3UL, 2);
            OsSleepMs(10);
        }
    }
    cmd = OsPciRead(Bus, Dev, Fn, 4, 2);
    OsPciWrite(Bus, Dev, Fn, 4, cmd | 0x0006 | 0x0400, 2);
    r->RegLen = 0x1000;
    r->Regs = (UCHAR *)OsMapMmio(Bus, bar & ~0xFUL, r->RegLen);
    r->Resv = (UCHAR *)OsDmaAlloc(RESV_BUF, &r->ResvPhys);
    r->Buf = (UCHAR *)OsDmaAlloc(XFER_MAX, &r->BufPhys);
    if (r->Regs == NULL || r->Resv == NULL || r->Buf == NULL || RD32(r, RTSX_BIPR) == 0xFFFFFFFFUL) {
        UsbLog(LOG_ERR, "rtsx %02x:%02x.%u: no access to the %s\n", Bus, Dev, Fn, chip->Name);
        goto fail;
    }
    WR32(r, RTSX_BIER, 0);
    chip->Params(r);
    if (chip->Vendor) chip->Vendor(r);
    if (InitHw(r) != SD_OK) {
        UsbLog(LOG_ERR, "rtsx %02x:%02x.%u: %s does not start\n", Bus, Dev, Fn, chip->Name);
        goto fail;
    }
    UsbFormat(r->Name, sizeof(r->Name), "Realtek %s", chip->Name);
    UsbLog(LOG_INFO, "rtsx %02x:%02x.%u: %s card reader rev %u at %08x, card %s\n", Bus, Dev, Fn, r->Name,
           r->IcVer, bar & ~0xFUL, (RD32(r, RTSX_BIPR) & SD_EXIST) ? "present" : "absent");
    Readers[slot] = r;
    SdHostCount++;
    r->Card = SdAttach(r->Name, ((ULONG)DeviceId << 8) ^ ((ULONG)Bus << 3) ^ Dev ^ 0x10EC0000UL, XFER_MAX / 512,
                       50000000, &RtsxPciOps, r);
    return;
fail:
    if (r->Regs) OsUnmapMmio(r->Regs, r->RegLen);
    if (r->Resv) OsDmaFree(r->Resv, RESV_BUF);
    if (r->Buf) OsDmaFree(r->Buf, XFER_MAX);
    OsFree(r);
}

void RtsxPciService(void)
{
    int i;
    for (i = 0; i < MAX_READERS; i++) {
        if (Readers[i] != NULL) SdPoll(Readers[i]->Card);
    }
}
