/*
 * rtsxusb.c - Realtek USB card readers with the vendor specific protocol
 * (RTS5129 "USB2.0-CRW" 0BDA:0129, RTS5139 0BDA:0139, RTS5179 0BDA:0140).
 * Most other Realtek USB readers are plain mass storage devices and need
 * nothing extra.
 *
 * The reader is driven through register command packets on the bulk pipes
 * (header "RTCR", read/write/check register commands, optional data stage);
 * the SD card logic is sdcard.c.  Register map and sequences follow the
 * reader's published programming model as used by the Linux rtsx_usb and
 * rtsx_usb_sdmmc drivers.
 */

#include "usbnt.h"
#include "sdcard.h"

/* command packet */
#define READ_REG_CMD        0
#define WRITE_REG_CMD       1
#define CHECK_REG_CMD       2
#define PACKET_TYPE         4
#define CNT_H               5
#define CNT_L               6
#define STAGE_FLAG          7
#define CMD_OFFSET          8
#define SEQ_WRITE_DATA_OFFSET 12
#define BATCH_CMD           0
#define SEQ_READ            1
#define SEQ_WRITE           2
#define STAGE_R             0x01
#define STAGE_DI            0x02
#define STAGE_DO            0x04
#define MODE_C              0x00
#define MODE_CR             STAGE_R
#define MODE_CDIR           (STAGE_R | STAGE_DI)
#define MODE_CDOR           (STAGE_R | STAGE_DO)
#define IOBUF_SIZE          1024

/* registers */
#define FPDCTL              0xFC00
#define HW_VERSION          0xFC01
#define CLK_DIV             0xFC03
#define SFSM_ED             0xFC04
#define SSC_DIV_N_0         0xFC07
#define SSC_CTL1            0xFC09
#define SSC_CTL2            0xFC0A
#define CFG_MODE            0xFC0E
#define CFG_MODE_1          0xFC0F
#define CD_DEGLITCH_WIDTH   0xFC20
#define CD_DEGLITCH_EN      0xFC21
#define SD_VPCLK0_CTL       0xFC2A
#define CARD_CLK_SOURCE     0xFC2E
#define SYS_DUMMY0          0xFC30
#define CARD_SHARE_MODE     0xFD51
#define CARD_DRIVE_SEL      0xFD52
#define CARD_STOP           0xFD53
#define CARD_OE             0xFD54
#define CARD_GPIO           0xFD56
#define SD30_DRIVE_SEL      0xFD57
#define CARD_DMA1_CTL       0xFD5C
#define CARD_DATA_SOURCE    0xFD5D
#define CARD_SELECT         0xFD5E
#define CARD_PULL_CTL1      0xFD60
#define CARD_PULL_CTL2      0xFD61
#define CARD_PULL_CTL3      0xFD62
#define CARD_PULL_CTL4      0xFD63
#define CARD_PULL_CTL5      0xFD64
#define CARD_PULL_CTL6      0xFD65
#define CARD_EXIST          0xFD6F
#define CARD_INT_PEND       0xFD71
#define CARD_CLK_EN         0xFD79
#define CARD_PWR_CTL        0xFD7A
#define LDO_POWER_CFG       0xFD7B
#define OCPSTAT             0xFD83
#define SD_CFG1             0xFDA0
#define SD_CFG2             0xFDA1
#define SD_STAT1            0xFDA3
#define SD_BUS_STAT         0xFDA5
#define SD_PAD_CTL          0xFDA6
#define SD_SAMPLE_POINT_CTL 0xFDA7
#define SD_PUSH_POINT_CTL   0xFDA8
#define SD_CMD0             0xFDA9
#define SD_CMD1             0xFDAA
#define SD_CMD2             0xFDAB
#define SD_CMD3             0xFDAC
#define SD_CMD4             0xFDAD
#define SD_BYTE_CNT_L       0xFDAF
#define SD_BYTE_CNT_H       0xFDB0
#define SD_BLOCK_CNT_L      0xFDB1
#define SD_BLOCK_CNT_H      0xFDB2
#define SD_TRANSFER         0xFDB3
#define SD_DATA_STATE       0xFDB6
#define HS_VCONTROL         0xFE26
#define HS_VSTAIN           0xFE27
#define HS_VLOADM           0xFE28
#define MC_FIFO_CTL         0xFF02
#define MC_DMA_CTL          0xFF10
#define MC_DMA_TC0          0xFF11
#define MC_DMA_TC1          0xFF12
#define MC_DMA_TC2          0xFF13
#define MC_DMA_TC3          0xFF14
#define MC_DMA_RST          0xFF15
#define PPBUF_BASE2         0xFA00

/* values */
#define SD_CD               0x01
#define SD_WP               0x08
#define POWER_OFF           0x03
#define PARTIAL_POWER_ON    0x02
#define POWER_ON            0x00
#define POWER_MASK          0x03
#define LDO3318_PWR_MASK    0x0C
#define LDO_ON              0x00
#define LDO_SUSPEND         0x08
#define FORCE_LDO_POWERB    0x60
#define CLK_CHANGE          0x80
#define SSC_POWER_MASK      0x01
#define SSC_POWER_ON        0x00
#define SSC_RSTB            0x80
#define SSC_8X_EN           0x40
#define SSC_SEL_4M          0x10
#define SSC_DEPTH_MASK      0x03
#define SSC_DEPTH_2M        0x01
#define SSC_DEPTH_512K      0x03
#define XTAL_FREE           0x80
#define CLK_MODE_MASK       0x03
#define CLK_MODE_NON_XTAL   0x01
#define RTS5179             0x02
#define NYET_EN             0x01
#define NYET_MSAK           0x01
#define SD30_DRIVE_MASK     0x07
#define SD20_DRIVE_MASK     0x03
#define DRIVER_TYPE_D       0x01
#define XD_CD_DEGLITCH_EN   0x04
#define CARD_SHARE_LQFP_SEL 0x04
#define CARD_SHARE_MASK     0x03
#define CARD_SHARE_SD       0x01
#define SD_CLK_EN           0x04
#define SD_MOD_SEL          2
#define SD_OUTPUT_EN        0x04
#define SD_STOP             0x04
#define SD_CLR_ERR          0x40
#define PINGPONG_BUFFER     0x01
#define RING_BUFFER         0x00
#define EXTEND_DMA1_ASYNC_SIGNAL 0x02
#define XD_INT              0x10
#define MS_INT              0x08
#define SD_INT              0x04
#define SD_TRANSFER_START   0x80
#define SD_TRANSFER_END     0x40
#define SD_STAT_IDLE        0x20
#define SD_TRANSFER_ERR     0x10
#define SD_TM_AUTO_WRITE_3  0x01
#define SD_TM_AUTO_WRITE_2  0x0A
#define SD_TM_AUTO_READ_3   0x05
#define SD_TM_CMD_RSP       0x08
#define SD_TM_NORMAL_READ   0x0C
#define SD_CLK_DIVIDE_0     0x00
#define SD_CLK_DIVIDE_128   0x80
#define SD_CLK_DIVIDE_MASK  0xC0
#define SD_BUS_WIDTH_1BIT   0x00
#define SD_BUS_WIDTH_4BIT   0x01
#define SD_20_MODE          0x00
#define SD_NO_CALCULATE_CRC7 0x80
#define SD_NO_CHECK_CRC7    0x04
#define SD_RSP_LEN_0        0x00
#define SD_RSP_LEN_6        0x01
#define SD_RSP_TYPE_R0      0x04
#define SD_RSP_TYPE_R1      0x01
#define SD_RSP_TYPE_R1b     0x09
#define SD_RSP_TYPE_R2      0x02
#define SD_RSP_TYPE_R3      0x05
#define SD_CRC7_ERR         0x80
#define CRC_FIX_CLK         0x00
#define SD30_VAR_CLK0       0x04
#define SAMPLE_VAR_CLK1     0x20
#define SD20_TX_14_AHEAD    0x10
#define SD20_TX_SEL_MASK    0x10
#define SD20_RX_14_DELAY    0x08
#define SD20_RX_SEL_MASK    0x08
#define SD20_RX_POS_EDGE    0x00
#define FIFO_FLUSH          0x01
#define DMA_RESET           0x01
#define DMA_DIR_TO_CARD     0x00
#define DMA_DIR_FROM_CARD   0x02
#define DMA_EN              0x01
#define DMA_512             (2 << 2)
#define DMA_PACK_SIZE_MASK  0x0C
#define MIN_DIV_N           60
#define MAX_DIV_N           120
#define CLK_DIV_1           0x00
#define CLK_DIV_4           0x02
#define LQFP48              1

#define XFER_MAX            0x10000     /* data bounce buffer: 128 blocks */

typedef struct _RTSX_USB {
    USB_DEV    *Dev;
    USB_PIPE    Out, In;
    UCHAR      *Cmd;
    ULONG       CmdPhys;
    UCHAR      *Rsp;
    ULONG       RspPhys;
    UCHAR      *Data;
    ULONG       DataPhys;
    ULONG       CmdIdx;
    UCHAR       Package;
    UCHAR       Is5179;
    UCHAR       Power;
    ULONG       CurClk;             /* MHz of the SSC */
    SD_CARD    *Card;
    char        Name[24];
} RTSX_USB;

#define MAX_READERS 4
static RTSX_USB *Readers[MAX_READERS];

/* ------------------------------------------------------------------ */
/* transport                                                           */
/* ------------------------------------------------------------------ */
static int Err(int r)
{
    return r == USB_ERR_NODEV ? SD_ENODEV : r == USB_ERR_TIMEOUT ? SD_ETIMEOUT : SD_EIO;
}

static void InitCmd(RTSX_USB *u)
{
    u->Cmd[0] = 'R'; u->Cmd[1] = 'T'; u->Cmd[2] = 'C'; u->Cmd[3] = 'R';
    u->Cmd[PACKET_TYPE] = BATCH_CMD;
    u->CmdIdx = 0;
}

static void AddCmd(RTSX_USB *u, UCHAR type, USHORT reg, UCHAR mask, UCHAR data)
{
    ULONG i;
    if (u->CmdIdx >= (IOBUF_SIZE - CMD_OFFSET) / 4) return;
    i = CMD_OFFSET + u->CmdIdx * 4;
    u->Cmd[i++] = (UCHAR)(((type & 3) << 6) | ((reg >> 8) & 0x3F));
    u->Cmd[i++] = (UCHAR)reg;
    u->Cmd[i++] = mask;
    u->Cmd[i] = data;
    u->CmdIdx++;
}

static void ClearFsmErr(RTSX_USB *u);

static int SendCmd(RTSX_USB *u, UCHAR flag, ULONG ms)
{
    ULONG got = 0;
    int r;
    u->Cmd[CNT_H] = (UCHAR)(u->CmdIdx >> 8);
    u->Cmd[CNT_L] = (UCHAR)u->CmdIdx;
    u->Cmd[STAGE_FLAG] = flag;
    r = UsbTransfer(&u->Out, u->Cmd, u->CmdPhys, u->CmdIdx * 4 + CMD_OFFSET, ms < 600 ? 600 : ms, &got);
    if (r != USB_OK) {
        if (r != USB_ERR_NODEV) ClearFsmErr(u);
        return Err(r);
    }
    return SD_OK;
}

static int GetRsp(RTSX_USB *u, ULONG len, ULONG ms)
{
    ULONG got = 0;
    int r = UsbTransfer(&u->In, u->Rsp, u->RspPhys, (len + 3) & ~3UL, ms < 600 ? 600 : ms, &got);
    return r == USB_OK ? SD_OK : Err(r);
}

static int Ep0Write(RTSX_USB *u, USHORT reg, UCHAR mask, UCHAR data)
{
    USHORT a = (USHORT)(reg | (3 << 14));
    int r = UsbControl(u->Dev, USB_RT_OUT | USB_RT_VENDOR | USB_RT_DEVICE, 0x00, (USHORT)((a >> 8) | (a << 8)),
                       (USHORT)(mask | (data << 8)), 0, NULL, 200, NULL);
    return r == USB_OK ? SD_OK : Err(r);
}

static void ClearFsmErr(RTSX_USB *u)
{
    Ep0Write(u, SFSM_ED, 0xF8, 0xF8);
}

static int WriteReg(RTSX_USB *u, USHORT reg, UCHAR mask, UCHAR data)
{
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, reg, mask, data);
    return SendCmd(u, MODE_C, 100);
}

static int ReadReg(RTSX_USB *u, USHORT reg, UCHAR *data)
{
    int r;
    *data = 0;
    InitCmd(u);
    AddCmd(u, READ_REG_CMD, reg, 0, 0);
    r = SendCmd(u, MODE_CR, 100);
    if (r == SD_OK) r = GetRsp(u, 1, 100);
    if (r == SD_OK) *data = u->Rsp[0];
    return r;
}

/* sequential register write / read (the ping-pong buffer) */
static int SeqHdr(RTSX_USB *u, USHORT addr, USHORT len, UCHAR type)
{
    u->Cmd[0] = 'R'; u->Cmd[1] = 'T'; u->Cmd[2] = 'C'; u->Cmd[3] = 'R';
    u->Cmd[PACKET_TYPE] = type;
    u->Cmd[5] = (UCHAR)(len >> 8);
    u->Cmd[6] = (UCHAR)len;
    u->Cmd[STAGE_FLAG] = (UCHAR)(type == SEQ_WRITE ? 0 : STAGE_R);
    u->Cmd[8] = (UCHAR)(addr >> 8);
    u->Cmd[9] = (UCHAR)addr;
    u->Cmd[10] = u->Cmd[11] = 0;
    return 0;
}

static int __attribute__((unused)) WritePpbuf(RTSX_USB *u, const UCHAR *buf, USHORT len)
{
    ULONG got = 0, n = (SEQ_WRITE_DATA_OFFSET + len + 3) & ~3UL;
    int r;
    if (n > IOBUF_SIZE) return SD_EIO;
    SeqHdr(u, PPBUF_BASE2, len, SEQ_WRITE);
    UsbMemCpy(u->Cmd + SEQ_WRITE_DATA_OFFSET, buf, len);
    r = UsbTransfer(&u->Out, u->Cmd, u->CmdPhys, n, 600, &got);
    return r == USB_OK ? SD_OK : Err(r);
}

static int ReadPpbuf(RTSX_USB *u, UCHAR *buf, USHORT len)
{
    ULONG got = 0, al = len & ~3UL, i;
    int r;
    if (al) {
        SeqHdr(u, PPBUF_BASE2, len, SEQ_READ);
        r = UsbTransfer(&u->Out, u->Cmd, u->CmdPhys, 12, 600, &got);
        if (r != USB_OK) return Err(r);
        r = UsbTransfer(&u->In, u->Rsp, u->RspPhys, al, 600, &got);
        if (r != USB_OK) return Err(r);
        UsbMemCpy(buf, u->Rsp, al);
    }
    for (i = al; i < len; i++) {
        r = ReadReg(u, (USHORT)(PPBUF_BASE2 + i), buf + i);
        if (r != SD_OK) return r;
    }
    return SD_OK;
}

static int CardStatus(RTSX_USB *u, USHORT *status)
{
    int r;
    InitCmd(u);
    AddCmd(u, READ_REG_CMD, CARD_EXIST, 0, 0);
    AddCmd(u, READ_REG_CMD, OCPSTAT, 0, 0);
    r = SendCmd(u, MODE_CR, 100);
    if (r == SD_OK) r = GetRsp(u, 2, 100);
    if (r != SD_OK) return r;
    *status = (USHORT)(((u->Rsp[0] >> 2) & 0x0F) | ((u->Rsp[1] & 0x03) << 4));
    return SD_OK;
}

static int WritePhy(RTSX_USB *u, UCHAR addr, UCHAR val)
{
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, HS_VSTAIN, 0xFF, val);
    AddCmd(u, WRITE_REG_CMD, HS_VCONTROL, 0xFF, (UCHAR)(addr & 0x0F));
    AddCmd(u, WRITE_REG_CMD, HS_VLOADM, 0xFF, 0x00);
    AddCmd(u, WRITE_REG_CMD, HS_VLOADM, 0xFF, 0x00);
    AddCmd(u, WRITE_REG_CMD, HS_VLOADM, 0xFF, 0x01);
    AddCmd(u, WRITE_REG_CMD, HS_VCONTROL, 0xFF, (UCHAR)((addr >> 4) & 0x0F));
    AddCmd(u, WRITE_REG_CMD, HS_VLOADM, 0xFF, 0x00);
    AddCmd(u, WRITE_REG_CMD, HS_VLOADM, 0xFF, 0x00);
    AddCmd(u, WRITE_REG_CMD, HS_VLOADM, 0xFF, 0x01);
    return SendCmd(u, MODE_C, 100);
}

static void ClearError(RTSX_USB *u)
{
    Ep0Write(u, CARD_STOP, SD_STOP | SD_CLR_ERR, SD_STOP | SD_CLR_ERR);
    Ep0Write(u, MC_FIFO_CTL, FIFO_FLUSH, FIFO_FLUSH);
    Ep0Write(u, MC_DMA_RST, DMA_RESET, DMA_RESET);
    ClearFsmErr(u);
}

/* ------------------------------------------------------------------ */
/* chip set-up                                                         */
/* ------------------------------------------------------------------ */
static int ResetChip(RTSX_USB *u)
{
    UCHAR val = 0;
    int r;
    InitCmd(u);
    if (u->Package == LQFP48) {
        AddCmd(u, WRITE_REG_CMD, CARD_PWR_CTL, LDO3318_PWR_MASK, LDO_SUSPEND);
        AddCmd(u, WRITE_REG_CMD, CARD_PWR_CTL, FORCE_LDO_POWERB, FORCE_LDO_POWERB);
        AddCmd(u, WRITE_REG_CMD, CARD_PULL_CTL1, 0x30, 0x10);
        AddCmd(u, WRITE_REG_CMD, CARD_PULL_CTL5, 0x03, 0x01);
        AddCmd(u, WRITE_REG_CMD, CARD_PULL_CTL6, 0x0C, 0x04);
    }
    AddCmd(u, WRITE_REG_CMD, SYS_DUMMY0, NYET_MSAK, NYET_EN);
    AddCmd(u, WRITE_REG_CMD, CD_DEGLITCH_WIDTH, 0xFF, 0x08);
    AddCmd(u, WRITE_REG_CMD, CD_DEGLITCH_EN, XD_CD_DEGLITCH_EN, 0x00);
    AddCmd(u, WRITE_REG_CMD, SD30_DRIVE_SEL, SD30_DRIVE_MASK, DRIVER_TYPE_D);
    AddCmd(u, WRITE_REG_CMD, CARD_DRIVE_SEL, SD20_DRIVE_MASK, 0x00);
    AddCmd(u, WRITE_REG_CMD, LDO_POWER_CFG, 0xE0, 0x00);
    if (u->Is5179) AddCmd(u, WRITE_REG_CMD, CARD_PULL_CTL5, 0x03, 0x01);
    AddCmd(u, WRITE_REG_CMD, CARD_DMA1_CTL, EXTEND_DMA1_ASYNC_SIGNAL, EXTEND_DMA1_ASYNC_SIGNAL);
    AddCmd(u, WRITE_REG_CMD, CARD_INT_PEND, XD_INT | MS_INT | SD_INT, XD_INT | MS_INT | SD_INT);
    r = SendCmd(u, MODE_C, 100);
    if (r != SD_OK) return r;
    ReadReg(u, CFG_MODE, &val);
    if ((val & XTAL_FREE) || (val & CLK_MODE_MASK) == CLK_MODE_NON_XTAL) {
        r = WritePhy(u, 0xC2, 0x7C);                    /* non-crystal mode */
    }
    return r;
}

static int InitChip(RTSX_USB *u)
{
    UCHAR val = 0;
    int r;
    ClearFsmErr(u);
    r = WriteReg(u, FPDCTL, SSC_POWER_MASK, SSC_POWER_ON);
    if (r != SD_OK) return r;
    OsStallUs(200);
    r = WriteReg(u, CLK_DIV, CLK_CHANGE, 0);
    if (r != SD_OK) return r;
    r = ReadReg(u, HW_VERSION, &val);
    if (r != SD_OK) return r;
    ReadReg(u, CARD_SHARE_MODE, &val);
    u->Package = (UCHAR)((val & CARD_SHARE_LQFP_SEL) ? LQFP48 : 0);
    ReadReg(u, CFG_MODE_1, &val);
    u->Is5179 = (UCHAR)((val & RTS5179) != 0);
    return ResetChip(u);
}

static int PullCtl(RTSX_USB *u, BOOLEAN on)
{
    static const UCHAR lqfpOn[6] = { 0xAA, 0xAA, 0xA9, 0x55, 0x55, 0xA5 };
    static const UCHAR lqfpOff[6] = { 0x55, 0x55, 0x95, 0x55, 0x55, 0xA5 };
    static const UCHAR qfnOn[6] = { 0xA5, 0x9A, 0xA5, 0x9A, 0x65, 0x5A };
    static const UCHAR qfnOff[6] = { 0x65, 0x55, 0x95, 0x55, 0x56, 0x59 };
    const UCHAR *t = u->Package == LQFP48 ? (on ? lqfpOn : lqfpOff) : (on ? qfnOn : qfnOff);
    int i;
    InitCmd(u);
    for (i = 0; i < 6; i++) AddCmd(u, WRITE_REG_CMD, (USHORT)(CARD_PULL_CTL1 + i), 0xFF, t[i]);
    return SendCmd(u, MODE_C, 100);
}

static int RxPowerOn(RTSX_USB *u)
{
    int r;
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, CARD_SELECT, 0x07, SD_MOD_SEL);
    AddCmd(u, WRITE_REG_CMD, CARD_SHARE_MODE, CARD_SHARE_MASK, CARD_SHARE_SD);
    AddCmd(u, WRITE_REG_CMD, CARD_CLK_EN, SD_CLK_EN, SD_CLK_EN);
    r = SendCmd(u, MODE_C, 100);
    if (r == SD_OK) r = PullCtl(u, TRUE);
    if (r == SD_OK) r = WriteReg(u, CARD_PWR_CTL, POWER_MASK, PARTIAL_POWER_ON);
    if (r != SD_OK) return r;
    OsSleepMs(1);
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, CARD_PWR_CTL, POWER_MASK | LDO3318_PWR_MASK, POWER_ON | LDO_ON);
    AddCmd(u, WRITE_REG_CMD, CARD_OE, SD_OUTPUT_EN, SD_OUTPUT_EN);
    return SendCmd(u, MODE_C, 100);
}

static int RxPowerOff(RTSX_USB *u)
{
    int r;
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, CARD_CLK_EN, SD_CLK_EN, 0);
    AddCmd(u, WRITE_REG_CMD, CARD_OE, SD_OUTPUT_EN, 0);
    AddCmd(u, WRITE_REG_CMD, CARD_PWR_CTL, POWER_MASK, POWER_OFF);
    AddCmd(u, WRITE_REG_CMD, CARD_PWR_CTL, POWER_MASK | LDO3318_PWR_MASK, POWER_OFF | LDO_SUSPEND);
    r = SendCmd(u, MODE_C, 100);
    if (r == SD_OK) r = PullCtl(u, FALSE);
    return r;
}

static int SwitchClock(RTSX_USB *u, ULONG hz, UCHAR ssc, BOOLEAN initial)
{
    UCHAR n, mcu, div = CLK_DIV_1;
    ULONG clk;
    int r = WriteReg(u, SD_CFG1, SD_CLK_DIVIDE_MASK, initial ? SD_CLK_DIVIDE_128 : SD_CLK_DIVIDE_0);
    if (r != SD_OK) return r;
    clk = (initial ? 30000000UL : hz) / 1000000;
    if (!initial) clk *= 2;                             /* double clock mode */
    if (clk == u->CurClk) return SD_OK;
    if (clk <= 2 || clk - 2 > MAX_DIV_N) return SD_EIO;
    n = (UCHAR)(clk - 2);
    mcu = (UCHAR)(60 / clk + 3);
    if (mcu > 15) mcu = 15;
    while (n < MIN_DIV_N && div < CLK_DIV_4) {
        n = (UCHAR)((n + 2) * 2 - 2);
        div++;
    }
    if (ssc > 1) ssc--;                                 /* double clock */
    if (div > CLK_DIV_1) ssc = ssc > div - 1 ? (UCHAR)(ssc - (div - 1)) : SSC_DEPTH_2M;
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, CLK_DIV, CLK_CHANGE, CLK_CHANGE);
    AddCmd(u, WRITE_REG_CMD, CLK_DIV, 0x3F, (UCHAR)((div << 4) | mcu));
    AddCmd(u, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, 0);
    AddCmd(u, WRITE_REG_CMD, SSC_CTL2, SSC_DEPTH_MASK, ssc);
    AddCmd(u, WRITE_REG_CMD, SSC_DIV_N_0, 0xFF, n);
    AddCmd(u, WRITE_REG_CMD, SSC_CTL1, SSC_RSTB, SSC_RSTB);
    r = SendCmd(u, MODE_C, 2000);
    if (r == SD_OK) r = WriteReg(u, SSC_CTL1, 0xFF, SSC_RSTB | SSC_8X_EN | SSC_SEL_4M);
    if (r != SD_OK) return r;
    OsStallUs(500);
    r = WriteReg(u, CLK_DIV, CLK_CHANGE, 0);
    if (r == SD_OK) u->CurClk = clk;
    return r;
}

/* ------------------------------------------------------------------ */
/* SD host operations                                                  */
/* ------------------------------------------------------------------ */
static int HStatus(void *Ctx, BOOLEAN *Present, BOOLEAN *Wp)
{
    RTSX_USB *u = (RTSX_USB *)Ctx;
    USHORT st = 0;
    int r = CardStatus(u, &st);
    if (r != SD_OK) return r;
    *Present = (st & SD_CD) != 0;
    *Wp = (st & SD_WP) != 0;
    return SD_OK;
}

static int HSetIos(void *Ctx, BOOLEAN Power, ULONG Hz, int Width, BOOLEAN HighSpeed)
{
    RTSX_USB *u = (RTSX_USB *)Ctx;
    int r = SD_OK;
    if (!Power) {
        if (u->Power) r = RxPowerOff(u);
        u->Power = 0;
        u->CurClk = 0;
        return r;
    }
    if (!u->Power) {
        r = RxPowerOn(u);
        if (r != SD_OK) return r;
        u->Power = 1;
    }
    WriteReg(u, SD_CFG1, 0x03, Width == SD_WIDTH_4 ? SD_BUS_WIDTH_4BIT : SD_BUS_WIDTH_1BIT);
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, SD_CFG1, 0x0C, SD_20_MODE);
    AddCmd(u, WRITE_REG_CMD, CARD_CLK_SOURCE, 0xFF, CRC_FIX_CLK | SD30_VAR_CLK0 | SAMPLE_VAR_CLK1);
    if (HighSpeed) {
        AddCmd(u, WRITE_REG_CMD, SD_PUSH_POINT_CTL, SD20_TX_SEL_MASK, SD20_TX_14_AHEAD);
        AddCmd(u, WRITE_REG_CMD, SD_SAMPLE_POINT_CTL, SD20_RX_SEL_MASK, SD20_RX_14_DELAY);
    } else {
        AddCmd(u, WRITE_REG_CMD, SD_PUSH_POINT_CTL, 0xFF, 0);
        AddCmd(u, WRITE_REG_CMD, SD_SAMPLE_POINT_CTL, SD20_RX_SEL_MASK, SD20_RX_POS_EDGE);
    }
    r = SendCmd(u, MODE_C, 100);
    if (r != SD_OK) return r;
    return SwitchClock(u, Hz, SSC_DEPTH_512K, Hz <= 1000000);
}

static ULONG Be32(const UCHAR *p) { return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3]; }

static int SendCmdGetRsp(RTSX_USB *u, SD_REQ *q)
{
    UCHAR type, *p;
    ULONG timeout = 100, stat = 0, len = 2, i;
    int r;
    switch (q->Rsp) {
    case SD_RSP_NONE: type = SD_RSP_TYPE_R0; break;
    case SD_RSP_R1B:  type = SD_RSP_TYPE_R1b; timeout = 3000; break;
    case SD_RSP_R2:   type = SD_RSP_TYPE_R2; break;
    case SD_RSP_R3:   type = SD_RSP_TYPE_R3; break;
    default:          type = SD_RSP_TYPE_R1; break;
    }
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, SD_CMD0, 0xFF, (UCHAR)(0x40 | q->Cmd));
    AddCmd(u, WRITE_REG_CMD, SD_CMD1, 0xFF, (UCHAR)(q->Arg >> 24));
    AddCmd(u, WRITE_REG_CMD, SD_CMD2, 0xFF, (UCHAR)(q->Arg >> 16));
    AddCmd(u, WRITE_REG_CMD, SD_CMD3, 0xFF, (UCHAR)(q->Arg >> 8));
    AddCmd(u, WRITE_REG_CMD, SD_CMD4, 0xFF, (UCHAR)q->Arg);
    AddCmd(u, WRITE_REG_CMD, SD_CFG2, 0xFF, type);
    AddCmd(u, WRITE_REG_CMD, CARD_DATA_SOURCE, 0x01, PINGPONG_BUFFER);
    AddCmd(u, WRITE_REG_CMD, SD_TRANSFER, 0xFF, SD_TM_CMD_RSP | SD_TRANSFER_START);
    AddCmd(u, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END | SD_STAT_IDLE, SD_TRANSFER_END | SD_STAT_IDLE);
    if (type == SD_RSP_TYPE_R2) {
        for (i = PPBUF_BASE2; i < PPBUF_BASE2 + 16; i++) AddCmd(u, READ_REG_CMD, (USHORT)i, 0, 0);
        stat = 16;
    } else if (type != SD_RSP_TYPE_R0) {
        for (i = SD_CMD0; i <= SD_CMD4; i++) AddCmd(u, READ_REG_CMD, (USHORT)i, 0, 0);
        stat = 5;
    }
    len += stat;
    AddCmd(u, READ_REG_CMD, SD_STAT1, 0, 0);
    r = SendCmd(u, MODE_CR, 100);
    if (r == SD_OK) r = GetRsp(u, len, timeout);
    if (r != SD_OK || (u->Rsp[0] & SD_TRANSFER_ERR)) {
        ClearError(u);
        return r != SD_OK ? r : SD_ETIMEOUT;
    }
    if (type == SD_RSP_TYPE_R0) return SD_OK;
    p = u->Rsp + 1;
    if ((p[0] & 0xC0) != 0) return SD_ECRC;
    if (type != SD_RSP_TYPE_R3 && (p[stat] & SD_CRC7_ERR)) return SD_ECRC;
    if (type == SD_RSP_TYPE_R2) {
        p[16] = 1;
        for (i = 0; i < 4; i++) q->Resp[i] = Be32(p + 1 + i * 4);
    } else {
        q->Resp[0] = Be32(p + 1);
    }
    return SD_OK;
}

/* single short block (CMD6 status, SCR) through the ping-pong buffer */
static int ReadShort(RTSX_USB *u, SD_REQ *q)
{
    ULONG cnt = q->BlkSize;
    int r;
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, SD_CMD0, 0xFF, (UCHAR)(0x40 | q->Cmd));
    AddCmd(u, WRITE_REG_CMD, SD_CMD1, 0xFF, (UCHAR)(q->Arg >> 24));
    AddCmd(u, WRITE_REG_CMD, SD_CMD2, 0xFF, (UCHAR)(q->Arg >> 16));
    AddCmd(u, WRITE_REG_CMD, SD_CMD3, 0xFF, (UCHAR)(q->Arg >> 8));
    AddCmd(u, WRITE_REG_CMD, SD_CMD4, 0xFF, (UCHAR)q->Arg);
    AddCmd(u, WRITE_REG_CMD, SD_BYTE_CNT_L, 0xFF, (UCHAR)cnt);
    AddCmd(u, WRITE_REG_CMD, SD_BYTE_CNT_H, 0xFF, (UCHAR)(cnt >> 8));
    AddCmd(u, WRITE_REG_CMD, SD_BLOCK_CNT_L, 0xFF, 1);
    AddCmd(u, WRITE_REG_CMD, SD_BLOCK_CNT_H, 0xFF, 0);
    AddCmd(u, WRITE_REG_CMD, SD_CFG2, 0xFF, SD_RSP_LEN_6);
    AddCmd(u, WRITE_REG_CMD, CARD_DATA_SOURCE, 0x01, PINGPONG_BUFFER);
    AddCmd(u, WRITE_REG_CMD, SD_TRANSFER, 0xFF, SD_TM_NORMAL_READ | SD_TRANSFER_START);
    AddCmd(u, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END, SD_TRANSFER_END);
    AddCmd(u, READ_REG_CMD, SD_CMD1, 0, 0);
    AddCmd(u, READ_REG_CMD, SD_CMD2, 0, 0);
    AddCmd(u, READ_REG_CMD, SD_CMD3, 0, 0);
    AddCmd(u, READ_REG_CMD, SD_CMD4, 0, 0);
    r = SendCmd(u, MODE_CR, 200);
    if (r == SD_OK) r = GetRsp(u, 5, 200);
    if (r != SD_OK || (u->Rsp[0] & SD_TRANSFER_ERR)) {
        ClearError(u);
        return r != SD_OK ? r : SD_EIO;
    }
    q->Resp[0] = Be32(u->Rsp + 1);
    return ReadPpbuf(u, q->Data, (USHORT)cnt);
}

/* 512 byte blocks through the ring buffer and the bulk pipes */
static int RwMulti(RTSX_USB *u, SD_REQ *q)
{
    ULONG len = q->Blocks * 512, got = 0;
    UCHAR cfg2, mode, flag;
    int r;
    if (len > XFER_MAX) return SD_EIO;
    if (q->Write) {
        cfg2 = SD_NO_CALCULATE_CRC7 | SD_NO_CHECK_CRC7 | SD_RSP_LEN_0;
        mode = SD_TM_AUTO_WRITE_3;
        flag = MODE_CDOR;
        UsbMemCpy(u->Data, q->Data, len);
    } else {
        cfg2 = SD_RSP_LEN_0;
        mode = SD_TM_AUTO_READ_3;
        flag = MODE_CDIR;
    }
    InitCmd(u);
    AddCmd(u, WRITE_REG_CMD, SD_BYTE_CNT_L, 0xFF, 0x00);
    AddCmd(u, WRITE_REG_CMD, SD_BYTE_CNT_H, 0xFF, 0x02);
    AddCmd(u, WRITE_REG_CMD, SD_BLOCK_CNT_L, 0xFF, (UCHAR)q->Blocks);
    AddCmd(u, WRITE_REG_CMD, SD_BLOCK_CNT_H, 0xFF, (UCHAR)(q->Blocks >> 8));
    AddCmd(u, WRITE_REG_CMD, CARD_DATA_SOURCE, 0x01, RING_BUFFER);
    AddCmd(u, WRITE_REG_CMD, MC_DMA_TC3, 0xFF, (UCHAR)(len >> 24));
    AddCmd(u, WRITE_REG_CMD, MC_DMA_TC2, 0xFF, (UCHAR)(len >> 16));
    AddCmd(u, WRITE_REG_CMD, MC_DMA_TC1, 0xFF, (UCHAR)(len >> 8));
    AddCmd(u, WRITE_REG_CMD, MC_DMA_TC0, 0xFF, (UCHAR)len);
    AddCmd(u, WRITE_REG_CMD, MC_DMA_CTL, 0x03 | DMA_PACK_SIZE_MASK,
           (UCHAR)((q->Write ? DMA_DIR_TO_CARD : DMA_DIR_FROM_CARD) | DMA_EN | DMA_512));
    AddCmd(u, WRITE_REG_CMD, SD_CFG2, 0xFF, cfg2);
    AddCmd(u, WRITE_REG_CMD, SD_TRANSFER, 0xFF, (UCHAR)(mode | SD_TRANSFER_START));
    AddCmd(u, CHECK_REG_CMD, SD_TRANSFER, SD_TRANSFER_END, SD_TRANSFER_END);
    r = SendCmd(u, flag, 100);
    if (r != SD_OK) return r;
    r = UsbTransfer(q->Write ? &u->Out : &u->In, u->Data, u->DataPhys, len, 10000, &got);
    if (r != USB_OK || got != len) {
        ClearError(u);
        return r != USB_OK ? Err(r) : SD_EIO;
    }
    r = GetRsp(u, 1, 2000);
    if (r != SD_OK) return r;
    if (u->Rsp[0] & SD_TRANSFER_ERR) {
        ClearError(u);
        return SD_EIO;
    }
    if (!q->Write) UsbMemCpy(q->Data, u->Data, len);
    return SD_OK;
}

static int HRequest(void *Ctx, SD_REQ *q)
{
    RTSX_USB *u = (RTSX_USB *)Ctx;
    int r;
    if (u->Dev->Gone) return SD_ENODEV;
    if (q->Data == NULL) return SendCmdGetRsp(u, q);
    if (q->BlkSize != 512 && !q->Write) {
        /* the controller needs the full clock for data even in identification mode */
        return ReadShort(u, q);
    }
    r = SendCmdGetRsp(u, q);
    if (r != SD_OK) return r;
    r = RwMulti(u, q);
    if (q->Stop) {
        SD_REQ s;
        UsbMemSet(&s, 0, sizeof(s));
        s.Cmd = 12;
        s.Rsp = SD_RSP_R1B;
        SendCmdGetRsp(u, &s);
        WriteReg(u, MC_FIFO_CTL, FIFO_FLUSH, FIFO_FLUSH);
    }
    return r;
}

static const SD_HOST_OPS RtsxUsbOps = { HStatus, HSetIos, HRequest };

/* ------------------------------------------------------------------ */
int RtsxUsbAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End)
{
    USHORT vid = GET16(Dev->DevDesc + 8), pid = GET16(Dev->DevDesc + 10);
    const UCHAR *d = If, *epOut = NULL, *epIn = NULL;
    RTSX_USB *u;
    int i, slot = -1;

    if (vid != 0x0BDA || (pid != 0x0129 && pid != 0x0139 && pid != 0x0140)) return USB_ERR_PARAM;
    if (OsGetConfig("DisableSd", 0)) return USB_ERR_PARAM;
    for (i = 0; i < MAX_READERS; i++) if (Readers[i] == NULL) { slot = i; break; }
    if (slot < 0) return USB_ERR_BUSY;
    while ((d = UsbNextDesc(d, End)) != NULL && d[1] != USB_DT_INTERFACE) {
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7 && (d[3] & 3) == USB_EP_BULK) {
            if (d[2] & 0x80) { if (!epIn) epIn = d; }
            else if (!epOut) epOut = d;
        }
    }
    if (!epIn || !epOut) return USB_ERR_PARAM;
    u = (RTSX_USB *)OsAlloc(sizeof(RTSX_USB));
    if (u == NULL) return USB_ERR_NOMEM;
    u->Dev = Dev;
    u->Cmd = (UCHAR *)OsDmaAlloc(IOBUF_SIZE, &u->CmdPhys);
    u->Rsp = (UCHAR *)OsDmaAlloc(IOBUF_SIZE, &u->RspPhys);
    u->Data = (UCHAR *)OsDmaAlloc(XFER_MAX, &u->DataPhys);
    if (!u->Cmd || !u->Rsp || !u->Data || UsbOpenPipe(Dev, &u->Out, epOut) != USB_OK ||
        UsbOpenPipe(Dev, &u->In, epIn) != USB_OK) {
        goto fail;
    }
    if (InitChip(u) != SD_OK) {
        UsbLog(LOG_ERR, "device %u: Realtek card reader does not answer\n", Dev->Index);
        goto fail;
    }
    UsbFormat(u->Name, sizeof(u->Name), "Realtek RTS51%02u", pid == 0x0129 ? 29 : pid == 0x0139 ? 39 : 79);
    UsbLog(LOG_INFO, "device %u: %s card reader (%s package)\n", Dev->Index, u->Name,
           u->Package == LQFP48 ? "LQFP48" : "QFN24");
    Readers[slot] = u;
    Dev->Function |= USB_FUNC_STORAGE;
    u->Card = SdAttach(u->Name, ((ULONG)pid << 12) ^ (Dev->Serial[0] ? (ULONG)Dev->Serial[0] << 20 : 0) ^ 0x0BDA,
                       XFER_MAX / 512, 50000000, &RtsxUsbOps, u);
    return USB_OK;
fail:
    UsbClosePipe(&u->Out);
    UsbClosePipe(&u->In);
    if (u->Cmd) OsDmaFree(u->Cmd, IOBUF_SIZE);
    if (u->Rsp) OsDmaFree(u->Rsp, IOBUF_SIZE);
    if (u->Data) OsDmaFree(u->Data, XFER_MAX);
    OsFree(u);
    return USB_ERR_IO;
}

void RtsxUsbDetach(USB_DEV *Dev)
{
    int i;
    for (i = 0; i < MAX_READERS; i++) {
        RTSX_USB *u = Readers[i];
        if (u == NULL || u->Dev != Dev) continue;
        Readers[i] = NULL;
        SdDetach(u->Card);
        UsbClosePipe(&u->Out);
        UsbClosePipe(&u->In);
        OsDmaFree(u->Cmd, IOBUF_SIZE);
        OsDmaFree(u->Rsp, IOBUF_SIZE);
        OsDmaFree(u->Data, XFER_MAX);
        OsFree(u);
    }
}

void RtsxUsbService(void)
{
    int i;
    for (i = 0; i < MAX_READERS; i++) {
        if (Readers[i] != NULL && !Readers[i]->Dev->Gone) SdPoll(Readers[i]->Card);
    }
}
