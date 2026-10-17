/*
 * usbblk.h - block devices that are not USB mass storage (SD cards in PCI
 * and vendor specific USB card readers) appear in the same disk slots as the
 * USB disks.  The device side answers SCSI commands; usbmsc.c does the rest
 * (slot assignment, disk.sys interface, media change handling).
 */

#ifndef USBBLK_H
#define USBBLK_H

#include "usbnt.h"

#define BLK_OK          0       /* command done */
#define BLK_FAILED      1       /* check condition, Sense holds the sense data */
#define BLK_ERROR       2       /* transport failure */
#define BLK_NODEV       3       /* the device is gone */

typedef struct _BLK_TARGET_OPS {
    /* Data is a system virtual address (Len bytes); Dir is SCSI_DIR_* */
    int (*Command)(void *Ctx, const UCHAR *Cdb, UCHAR CdbLen, UCHAR Dir, ULONG Len, UCHAR *Data,
                   ULONG *Done, UCHAR *Sense);
} BLK_TARGET_OPS;

typedef struct _MSC_DEV BLK_TARGET;

/* worker context: the target gets a disk slot (or waits for one) */
BLK_TARGET *MscVirtAttach(const char *Name, ULONG Identity, ULONG MaxXfer, const BLK_TARGET_OPS *Ops, void *Ctx);
void        MscVirtDetach(BLK_TARGET *T);
/* what the target is, for usbtree (default: a card reader) */
#define BLK_KIND_READER 0
#define BLK_KIND_NVME   1
void        MscVirtSetKind(BLK_TARGET *T, UCHAR Kind);
/* medium inserted or removed: mounted volumes re-verify, next command sees UNIT ATTENTION */
void        MscVirtMediaChange(BLK_TARGET *T);

/* SCSI sense helpers for targets */
void        BlkSense(UCHAR *Sense, UCHAR Key, UCHAR Asc, UCHAR Ascq);

#endif
