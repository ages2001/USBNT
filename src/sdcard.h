/*
 * sdcard.h - SD memory card protocol on top of a card reader host
 * (SDHCI, Realtek PCIe, Realtek USB).  The card shows up as a removable
 * disk through usbblk.h.
 */

#ifndef SDCARD_H
#define SDCARD_H

#include "usbnt.h"

/* response types */
#define SD_RSP_NONE     0
#define SD_RSP_R1       1       /* also R5, R6, R7 */
#define SD_RSP_R1B      2
#define SD_RSP_R2       3       /* 136 bit: CID, CSD */
#define SD_RSP_R3       4       /* OCR, no CRC */

/* results */
#define SD_OK           0
#define SD_ETIMEOUT     -1
#define SD_ECRC         -2
#define SD_EIO          -3
#define SD_ENOMEDIUM    -4
#define SD_ENODEV       -5      /* reader gone */

typedef struct _SD_REQ {
    UCHAR   Cmd;
    UCHAR   Rsp;                /* SD_RSP_* */
    UCHAR   Write;              /* data direction */
    UCHAR   Stop;               /* multi-block: send CMD12 after the data */
    ULONG   Arg;
    ULONG   Resp[4];            /* R2: Resp[0] = bits 127..96 (CID/CSD MSB first) */
    UCHAR  *Data;               /* system virtual address, NULL = no data */
    ULONG   BlkSize;
    ULONG   Blocks;
} SD_REQ;

#define SD_WIDTH_1      1
#define SD_WIDTH_4      4

typedef struct _SD_HOST_OPS {
    /* card present / write protect switch */
    int  (*Status)(void *Ctx, BOOLEAN *Present, BOOLEAN *WriteProtect);
    /* power off/on, clock in Hz (0 = off), bus width, high speed timing */
    int  (*SetIos)(void *Ctx, BOOLEAN Power, ULONG ClockHz, int Width, BOOLEAN HighSpeed);
    int  (*Request)(void *Ctx, SD_REQ *Req);
} SD_HOST_OPS;

typedef struct _SD_CARD SD_CARD;

/* worker context */
SD_CARD *SdAttach(const char *Name, ULONG Identity, ULONG MaxBlocks, ULONG MaxClock, const SD_HOST_OPS *Ops,
                  void *Ctx);
void     SdDetach(SD_CARD *Card);
/* poll the card detect switch (worker), returns TRUE when a card is present */
BOOLEAN  SdPoll(SD_CARD *Card);
/* the host saw an insert/remove interrupt */
void     SdCardChanged(SD_CARD *Card);

/* SD card readers on PCI (sdhci.c, rtsxpci.c) */
void     SdhciProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG ClassCode);
void     RtsxPciProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, USHORT DeviceId);
void     SdHostService(void);       /* worker: card detect polling of every PCI reader */
void     RtsxPciService(void);
extern int SdHostCount;             /* PCI card readers found */
ULONG    UsbPciRelocateBar(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off);   /* usbcore.c */
BOOLEAN  UsbPciExt(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, ULONG *Val, BOOLEAN Write);

/* Realtek USB card readers (rtsxusb.c) */
int      RtsxUsbAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End);
void     RtsxUsbDetach(USB_DEV *Dev);
void     RtsxUsbService(void);

#endif
