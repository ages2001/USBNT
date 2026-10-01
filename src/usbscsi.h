/*
 * usbscsi.h - SCSI request interface between the USB mass storage code
 * and the operating system side (virtual SCSI adapter).
 */

#ifndef USBSCSI_H
#define USBSCSI_H

#include "usbnt.h"

#define SCSI_MAX_SLOTS          8

/* SRB status values, identical to the NT definitions */
#define SRBST_SUCCESS           0x01
#define SRBST_ABORTED           0x02
#define SRBST_ERROR             0x04
#define SRBST_INVALID_REQUEST   0x06
#define SRBST_NO_DEVICE         0x08
#define SRBST_TIMEOUT           0x09
#define SRBST_SELECTION_TIMEOUT 0x0A
#define SRBST_BUS_RESET         0x0E
#define SRBST_DATA_OVERRUN      0x12
#define SRBST_INVALID_LUN       0x20
#define SRBST_INVALID_TARGET    0x21
#define SRBST_AUTOSENSE_VALID   0x80

#define SCSIST_GOOD             0x00
#define SCSIST_CHECK_CONDITION  0x02

#define SCSI_DIR_NONE           0
#define SCSI_DIR_IN             1
#define SCSI_DIR_OUT            2

typedef struct _SCSI_REQ SCSI_REQ;

struct _SCSI_REQ {
    UCHAR       Target;
    UCHAR       Lun;
    UCHAR       CdbLength;
    UCHAR       Direction;
    UCHAR       Cdb[16];
    void       *Data;           /* system virtual address */
    ULONG      *Pfn;            /* page frames of Data, or NULL */
    ULONG       PageOff;        /* offset of Data in its first page */
    ULONG       DataLength;
    ULONG       Transferred;
    UCHAR       SrbStatus;
    UCHAR       ScsiStatus;
    UCHAR       SenseLength;
    UCHAR       Internal;       /* 1 = prepare slot for removal */
    UCHAR       Sense[18];
    USHORT      Pad2;
    void      (*Complete)(SCSI_REQ *Req);
    void       *Context;
    SCSI_REQ   *Next;
};

typedef struct _SCSI_SLOT_INFO {
    UCHAR       Present;
    UCHAR       DeviceType;
    UCHAR       Inquiry[36];
    UCHAR       DevIndex;       /* USB device, 0xFF if empty */
    UCHAR       Lun;
    UCHAR       Protocol;       /* interface protocol: 0x50 BOT, 0x62 UAS, 0/1 CBI */
    UCHAR       MediaReady;
    UCHAR       WriteProtect;
    UCHAR       Pad[3];
    ULONG       LastLba;        /* from the last READ CAPACITY, 0 if unknown */
    ULONG       BlockSize;
    ULONG       MaxXfer;        /* bytes per device command */
    ULONG       Identity;
} SCSI_SLOT_INFO;

extern ULONG UsbScsiSlotCount;

void    UsbScsiInit(void);
void    UsbScsiQueue(SCSI_REQ *Req);
BOOLEAN UsbScsiExecute(SCSI_REQ *Req);          /* worker context, FALSE = completes later */
void    UsbScsiGetSlot(int Slot, SCSI_SLOT_INFO *Info);
void    UsbScsiFreeze(void);
void    UsbScsiEject(int Slot);
int     UsbScsiSlotPresent(int Slot);

#endif
