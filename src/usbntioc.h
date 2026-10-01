/*
 * usbntioc.h - private IOCTL interface of \\.\UsbNt, shared by the driver
 * and the user mode tools (usbwheel.exe, usbeject.exe).
 */

#ifndef USBNTIOC_H
#define USBNTIOC_H

#define USBNT_DEVICE_NAME       L"\\Device\\UsbNt"
#define USBNT_DOS_NAME          L"\\DosDevices\\UsbNt"
#define USBNT_WIN32_NAME        "\\\\.\\UsbNt"

#define IOCTL_USBNT_WHEEL_WAIT  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBNT_EJECT       CTL_CODE(FILE_DEVICE_UNKNOWN, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBNT_QUERY_SLOTS CTL_CODE(FILE_DEVICE_UNKNOWN, 0x902, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_USBNT_QUERY_TREE  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x903, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBNT_WAIT_CHANGE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x904, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define USBNT_MAX_SLOTS         8
#define USBNT_MAX_HCS           16
#define USBNT_MAX_DEVS          64
#define USBNT_MAX_PORTS         32

typedef struct _USBNT_SLOT {
    UCHAR   Present;
    UCHAR   DeviceType;
    UCHAR   Reserved[2];
    UCHAR   Vendor[8];
    UCHAR   Product[16];
} USBNT_SLOT;

typedef struct _USBNT_SLOTS {
    ULONG       PortNumber;
    ULONG       SlotCount;
    USBNT_SLOT  Slot[USBNT_MAX_SLOTS];
} USBNT_SLOTS;

/* IOCTL_USBNT_QUERY_TREE output */

#define USBNT_HC_UHCI           0
#define USBNT_HC_OHCI           1
#define USBNT_HC_EHCI           2
#define USBNT_HC_XHCI           3

#define USBNT_SPEED_FULL        0
#define USBNT_SPEED_LOW         1
#define USBNT_SPEED_HIGH        2
#define USBNT_SPEED_SUPER       3

#define USBNT_FUNC_HUB          0x01
#define USBNT_FUNC_KEYBOARD     0x02
#define USBNT_FUNC_MOUSE        0x04
#define USBNT_FUNC_TABLET       0x08
#define USBNT_FUNC_STORAGE      0x10

typedef struct _USBNT_HC_INFO {
    UCHAR   Type;               /* USBNT_HC_* */
    UCHAR   Bus;
    UCHAR   Device;
    UCHAR   Function;
    USHORT  VendorId;
    USHORT  DeviceId;
    UCHAR   NumPorts;
    UCHAR   Running;
    UCHAR   Irq;
    UCHAR   Companion;          /* EHCI index this UHCI/OHCI serves, 0xFF = none */
    UCHAR   PortProto[USBNT_MAX_PORTS];  /* xHCI: 2 or 3 per port, 0 otherwise */
} USBNT_HC_INFO;

typedef struct _USBNT_DEV_INFO {
    UCHAR   Index;
    UCHAR   Hc;
    UCHAR   Parent;             /* device index of the hub, 0xFF = root port */
    UCHAR   Port;               /* port on the parent hub or root hub */
    UCHAR   RootPort;
    UCHAR   Depth;
    UCHAR   Speed;              /* USBNT_SPEED_* */
    UCHAR   Address;
    USHORT  UsbVersion;         /* bcdUSB */
    USHORT  VendorId;
    USHORT  ProductId;
    USHORT  DeviceVersion;      /* bcdDevice */
    UCHAR   Class;
    UCHAR   SubClass;
    UCHAR   Protocol;
    UCHAR   Functions;          /* USBNT_FUNC_* */
    UCHAR   HubPorts;
    UCHAR   MaxPower;           /* units of 2 mA (8 mA for SuperSpeed) */
    UCHAR   Interfaces;
    UCHAR   LinkGbps;           /* SuperSpeed link rate in Gbit/s (5, 10, 20) */
    char    Manufacturer[32];
    char    Product[48];
    char    Serial[32];
} USBNT_DEV_INFO;

typedef struct _USBNT_SLOT_INFO {
    UCHAR   Present;
    UCHAR   DeviceType;         /* SCSI peripheral type */
    UCHAR   DevIndex;           /* USBNT_DEV_INFO index, 0xFF = empty */
    UCHAR   Lun;
    UCHAR   Protocol;           /* 0x50 Bulk-Only, 0x62 UAS, 0x00/0x01 CBI */
    UCHAR   MediaReady;
    UCHAR   WriteProtect;
    UCHAR   Pad;
    ULONG   LastLba;            /* 0 if unknown */
    ULONG   BlockSize;
    ULONG   MaxTransfer;        /* bytes per device command */
    UCHAR   Vendor[8];
    UCHAR   Product[16];
    UCHAR   Revision[4];
    ULONG   Identity;           /* same value for the same device and LUN */
} USBNT_SLOT_INFO;

typedef struct _USBNT_TREE {
    ULONG           Version;    /* USBNT_TREE_VERSION */
    ULONG           ChangeCount;
    ULONG           ScsiPort;
    ULONG           HcCount;
    ULONG           SlotCount;
    USBNT_HC_INFO   Hc[USBNT_MAX_HCS];
    USBNT_DEV_INFO  Dev[USBNT_MAX_DEVS];        /* Dev[i].Index == 0xFF: unused */
    USBNT_SLOT_INFO Slot[USBNT_MAX_SLOTS];
} USBNT_TREE;

#define USBNT_TREE_VERSION      1

#endif
