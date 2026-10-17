/*
 * wdm.h - Windows 2000 / XP (x86 and x64) side of the driver: Plug and Play
 * function devices for the PCI functions (USB host controllers, card readers,
 * Thunderbolt / USB4 host interfaces, I2C controllers) and the USBNT device
 * bus with its children (keyboard, mouse, disks).
 */

#ifndef WDM_H
#define WDM_H

#include "ntdrv.h"

#define EXT_HC          10      /* function device of a PCI function */
#define EXT_BUS         11      /* USBNT device bus (root enumerated USBNT\Bus, made by usbinst) */
#define EXT_PDO         12      /* child of the bus */
#define EXT_DFLT        13      /* lower filter of disk.sys on a USBSTOR disk (wdmflt.c) */
#define EXT_PFLT        14      /* upper filter of i8042prt's PS/2 mouse (wdmps2.c) */
#define EXT_NODE        16      /* function device on a root hub / hub / device node (wdmtree.c) */
/* EXT_KBD / EXT_MOUSE (ntdrv.h): function devices on the keyboard / mouse PDOs */

#define PDO_KBD         1
#define PDO_MOUSE       2
#define PDO_DISK        3
#define PDO_AUDIO       4       /* "USBNT\Audio": AUDIONT serves the USB audio devices */
#define PDO_USB         5       /* a USB device or function for Windows' USB drivers (usbdi.c) */
#define PDO_HUB         6       /* Device Manager: a root hub (of a host controller) or a hub (wdmtree.c) */
#define PDO_UDEV        7       /* Device Manager: a USB device USBNT drives (wdmtree.c) */

typedef struct _WDM_EXT {
    NT_EXT          Nt;                 /* Kind, Number, Self */
    PDEVICE_OBJECT  Lower;              /* function devices: the next device down */
    PDEVICE_OBJECT  Pdo;                /* function devices: their PDO */
    UCHAR           Bus, Dev, Fn;
    BOOLEAN         Started;
    BOOLEAN         Removed;            /* REMOVE_DEVICE seen */
    BOOLEAN         Reported;           /* PDO: in the last bus relations */
    BOOLEAN         Gone;               /* surprise removal */
    BOOLEAN         Sleeping;           /* HC: stopped for a sleep state */
    ULONG           Child;              /* PDO_* */
    int             Slot;               /* PDO_DISK: disk slot */
    ULONG           Identity;           /* PDO_DISK: device in the slot */
    UCHAR           DevType;            /* PDO_DISK: SCSI peripheral type */
    UCHAR           Inquiry[36];
    UCHAR           Proto;              /* PDO_DISK: 0x50 BOT, 0x62 UAS, else virtual (reader, NVMe) */
    UCHAR           Fixed;              /* PDO_DISK: the medium is reported as fixed (RemovableAsFixed) */
    UCHAR           Alt;                /* PDO_DISK: the second instance id (the first one is not removed yet) */
    ULONG           Gen;                /* PDO_DISK, fixed: the medium (SCSI_SLOT_INFO.MediaGen) */
    LONG            Paging;
    LONG            Checking;           /* EXT_DFLT: a look at a new medium is queued */
    struct _WDM_EXT *NextChild;
    struct _UD_FUNC *Func;              /* PDO_USB */
    /* PDO_HUB / PDO_UDEV (wdmtree.c) */
    PDEVICE_OBJECT  TOwner;             /* root hub: the host controller's function device */
    struct _USB_DEV *TDev;              /* the hub / device (NULL: root hub) */
    ULONG           TGen;               /* ... its USB_DEV.Gen when the node was made */
    UCHAR           TPort;              /* port on the parent hub */
    UCHAR           TNamed;             /* the friendly name was written */
    UCHAR           ClassSet;
    UCHAR           Claimed;            /* host controller taken in text-mode setup without a driver */           /* keyboard / mouse PDO: the device node's class was checked */
    UCHAR           TPad[2];
    struct _WDM_EXT *TNext;             /* the tree's list */
} WDM_EXT;

/* wdmmain.c */
void    WdmRunInWorker(void (*Fn)(void *), void *Arg);
extern  PDRIVER_OBJECT WdmDriver;
NTSTATUS WdmForwardSync(PDEVICE_OBJECT Lower, PIRP Irp);
NTSTATUS WdmComplete(PIRP Irp, NTSTATUS St, ULONG_PTR Info);

/* wdmbus.c */
NTSTATUS WdmBusAdd(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo);
NTSTATUS WdmBusPnp(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmPdoPnp(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmPdoPower(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmPdoDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmPdoInternal(PDEVICE_OBJECT Dev, PIRP Irp);
void    WdmBusService(void);            /* worker: children that came or went */
void    WdmBusInit(void);               /* DriverEntry: looks for a USBNT\Bus device */
void    WdmBusRefresh(void);            /* worker: which disk slot holds what, now */
void    WdmBusOnController(WDM_EXT *Hc); /* no USBNT\Bus device: a host controller is the bus */
BOOLEAN WdmIsBus(PDEVICE_OBJECT Fdo);
NTSTATUS WdmBusRelations(PIRP Irp);
void    WdmBusOff(PDEVICE_OBJECT Fdo);  /* that host controller goes */
BOOLEAN WdmIsOurPdo(PDEVICE_OBJECT Pdo, ULONG *Child);
BOOLEAN WdmEjectSlot(ULONG Slot);           /* IOCTL_USBNT_EJECT: through Plug and Play */

/* wdmtree.c: root hubs, hubs and devices for Device Manager */
NTSTATUS WdmTreeHcRelations(WDM_EXT *Hc, PIRP Irp);  /* adds the root hub to the relations in Irp */
void    WdmTreeHcStarted(WDM_EXT *Hc);  /* worker: the controller's name */
void    WdmTreeHcGone(WDM_EXT *Hc);
NTSTATUS WdmTreePdoPnp(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmTreeNodePnp(PDEVICE_OBJECT Dev, PIRP Irp);   /* EXT_NODE */
void    WdmTreeService(void);           /* worker: hubs / devices that came or went */
void    WdmSetFriendlyName(PDEVICE_OBJECT Pdo, const WCHAR *Name);
BOOLEAN UsbdiHasDevice(struct _USB_DEV *Dev);   /* the whole device is Windows' (usbdi.c) */
void    WdmInvalidateHcs(void);         /* wdmmain.c: the controllers' relations again */

/* wdmflt.c: lower filter of disk.sys on USBSTOR disks */
BOOLEAN WdmFltWanted(PDEVICE_OBJECT Pdo);
NTSTATUS WdmFltAdd(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo);
NTSTATUS WdmFltPass(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmFltPnp(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmFltPower(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmFltDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS WdmFltInternal(PDEVICE_OBJECT Dev, PIRP Irp);

/* wdmps2.c: upper filter of i8042prt's PS/2 mouse (touch pads) */
BOOLEAN WdmPs2Wanted(PDEVICE_OBJECT Pdo);
NTSTATUS WdmPs2Add(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo);
NTSTATUS WdmPs2Dispatch(PDEVICE_OBJECT Dev, PIRP Irp);
void    WdmPs2Service(void);            /* worker: touch pad detection when i8042prt is done */

/* usbdi.c: USB devices for Windows' USB drivers */
struct _UD_FUNC;
void    UsbdiInit(void);
BOOLEAN UsbdiService(void);
struct _UD_FUNC *UsbdiNewFunc(void);
void    UsbdiBind(struct _UD_FUNC *F, PDEVICE_OBJECT Pdo);
BOOLEAN UsbdiWanted(struct _UD_FUNC *F);
void    UsbdiFreeFunc(struct _UD_FUNC *F);
NTSTATUS UsbdiQueryId(struct _UD_FUNC *F, PIRP Irp, BUS_QUERY_ID_TYPE Type);
NTSTATUS UsbdiQueryText(struct _UD_FUNC *F, PIRP Irp, DEVICE_TEXT_TYPE Type);
NTSTATUS UsbdiPnp(struct _UD_FUNC *F, PIRP Irp, BOOLEAN Reported, BOOLEAN *Free);
NTSTATUS UsbdiPower(struct _UD_FUNC *F, PIRP Irp);
NTSTATUS UsbdiInternal(struct _UD_FUNC *F, PIRP Irp);

/* ntscsi.c */
NTSTATUS NtScsiSrbSlot(PDEVICE_OBJECT Dev, PIRP Irp, ULONG Slot);
NTSTATUS NtScsiPassThroughSlot(PIRP Irp, ULONG InLen, ULONG OutLen, ULONG Slot);

/* ntinput.c */
void    NtInputDisconnect(ULONG Kind, PDEVICE_OBJECT Dev);
void    NtInputEnable(ULONG Kind, BOOLEAN Enable, PDEVICE_OBJECT Dev);

/* wdmtree.c */
void    WdmInputClassNode(WDM_EXT *Pdo);
ULONG   WdmTextSetupClaims(ULONG *Bus, ULONG *Slot, ULONG Max);
BOOLEAN WdmBusPresent(void);
void    WdmBusReportedStarted(void);
void    WdmDumpInstance(PDEVICE_OBJECT Pdo, const char *Tag);

NTKERNELAPI NTSTATUS NTAPI ObQueryNameString(PVOID Object, POBJECT_NAME_INFORMATION ObjectNameInfo, ULONG Length,
                                             PULONG ReturnLength);

#endif
