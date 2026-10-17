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
#define IOCTL_USBNT_GET_LOG     CTL_CODE(FILE_DEVICE_UNKNOWN, 0x905, METHOD_BUFFERED, FILE_ANY_ACCESS)  /* out: log text (latest 32 KB) */
/* 0x906, 0x907: mouse buttons for usbmon, below */

/* Mouse buttons for usbmon (NT 3.x / 4.0, which have no X buttons and, before
   NT 4.0 SP3, no low level mouse hook): button presses of the mice usbnt.sys
   drives (USB, PS/2, I2C) that usbmon asks for go to it instead of to the
   system.  Every release goes where its press went.  The capture ends when
   the handle that set it is closed (usbmon ends or crashes) and when usbmon
   does not fetch the events for a second. */
#define IOCTL_USBNT_MOUSE_WAIT    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x906, METHOD_BUFFERED, FILE_ANY_ACCESS)  /* out: USBNT_MOUSE_EVENTS */
#define IOCTL_USBNT_MOUSE_CAPTURE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x907, METHOD_BUFFERED, FILE_ANY_ACCESS)  /* in: ULONG USBNT_CAP_*, out (optional): ULONG flags now */

#define USBNT_CAP_XBUTTONS      0x01    /* buttons 4 and 5 */
#define USBNT_CAP_MIDDLE        0x02    /* the middle button; its press also turns USBNT_CAP_GRAB on */
#define USBNT_CAP_GRAB          0x04    /* every button (autoscroll runs); usbmon turns it off */

/* button flags of an event, the MOUSE_INPUT_DATA ButtonFlags values */
#define USBNT_MB_LEFT_DOWN      0x0001
#define USBNT_MB_LEFT_UP        0x0002
#define USBNT_MB_RIGHT_DOWN     0x0004
#define USBNT_MB_RIGHT_UP       0x0008
#define USBNT_MB_MIDDLE_DOWN    0x0010
#define USBNT_MB_MIDDLE_UP      0x0020
#define USBNT_MB_4_DOWN         0x0040
#define USBNT_MB_4_UP           0x0080
#define USBNT_MB_5_DOWN         0x0100
#define USBNT_MB_5_UP           0x0200

#define USBNT_MOUSE_MAX_EVENTS  16

typedef struct _USBNT_MOUSE_EVENT {
    USHORT  Buttons;            /* USBNT_MB_* taken from the system */
    USHORT  Grab;               /* 1: taken because USBNT_CAP_GRAB was on */
    ULONG   TimeMs;
} USBNT_MOUSE_EVENT;

typedef struct _USBNT_MOUSE_EVENTS {
    ULONG   Count;
    ULONG   Flags;              /* USBNT_CAP_* now */
    USBNT_MOUSE_EVENT Ev[USBNT_MOUSE_MAX_EVENTS];
} USBNT_MOUSE_EVENTS;

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
#define USBNT_FUNC_AUDIO        0x20    /* USB Audio 1.0 / 2.0 streaming (AUDIONT) */
#define USBNT_FUNC_MIDI         0x40    /* USB MIDI output */
#define USBNT_FUNC_VIDEO        0x80    /* USB Video Class camera (usbcam.exe) */

typedef struct _USBNT_HC_INFO {
    UCHAR   Type;               /* USBNT_HC_* */
    UCHAR   Bus;
    UCHAR   Device;
    UCHAR   Function;
    USHORT  VendorId;
    USHORT  DeviceId;
    UCHAR   NumPorts;
    UCHAR   Running;            /* 0 no, 1 yes, 2 unplugged, 3 yes, hot plugged (Thunderbolt / USB4 dock) */
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
    UCHAR   Partitions;         /* partitions on the medium (bit 7: reported as a fixed disk, 2000 / XP) */
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

/* with an output buffer of sizeof (USBNT_TREE2) the driver also lists what is
   not on a USB port: card reader slots (Slot is the disk slot) and HID over
   I2C controllers and devices; Version is then USBNT_TREE_VERSION2 */
#define USBNT_EXTRA_I2C_CTRL    1
#define USBNT_EXTRA_I2C_HID     2
#define USBNT_EXTRA_READER      3
#define USBNT_EXTRA_NVME        4   /* NVMe drive (PCIe hot plug, Thunderbolt / USB4): Slot */
#define USBNT_EXTRA_TBT         5   /* Thunderbolt / USB4 host router or connected device */
#define USBNT_MAX_EXTRA         24

typedef struct _USBNT_EXTRA_INFO {
    UCHAR   Kind;               /* USBNT_EXTRA_* */
    UCHAR   Slot;               /* USBNT_EXTRA_READER: disk slot */
    UCHAR   Pad[2];
    char    Name[48];
    char    Detail[160];        /* lines separated by '\n' */
} USBNT_EXTRA_INFO;

typedef struct _USBNT_TREE2 {
    USBNT_TREE       Base;
    ULONG            ExtraCount;
    USBNT_EXTRA_INFO Extra[USBNT_MAX_EXTRA];
} USBNT_TREE2;

#define USBNT_TREE_VERSION2     2

/* ------------------------------------------------------------------ */
/* USB Video Class cameras (usbvideo.c), used by usbcam.exe            */
/* ------------------------------------------------------------------ */
#define IOCTL_USBNT_VIDEO_LIST    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x910, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBNT_VIDEO_START   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x911, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBNT_VIDEO_STOP    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x912, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_USBNT_VIDEO_FRAME   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x913, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_USBNT_VIDEO_CONTROL CTL_CODE(FILE_DEVICE_UNKNOWN, 0x914, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define USBNT_MAX_CAMS          4
#define USBNT_VID_FORMATS       8
#define USBNT_VID_FRAMES        12
#define USBNT_VID_INTERVALS     8

/* four character codes (little endian, as in AVI / BITMAPINFOHEADER) */
#define USBNT_FOURCC(a, b, c, d) ((ULONG)(UCHAR)(a) | ((ULONG)(UCHAR)(b) << 8) | ((ULONG)(UCHAR)(c) << 16) | \
                                  ((ULONG)(UCHAR)(d) << 24))
#define USBNT_FCC_YUY2          USBNT_FOURCC('Y', 'U', 'Y', '2')
#define USBNT_FCC_UYVY          USBNT_FOURCC('U', 'Y', 'V', 'Y')
#define USBNT_FCC_NV12          USBNT_FOURCC('N', 'V', '1', '2')
#define USBNT_FCC_I420          USBNT_FOURCC('I', '4', '2', '0')
#define USBNT_FCC_MJPG          USBNT_FOURCC('M', 'J', 'P', 'G')
#define USBNT_FCC_RGB24         USBNT_FOURCC('R', 'G', 'B', '3')   /* bottom-up BGR, as a DIB */
#define USBNT_FCC_GREY          USBNT_FOURCC('Y', '8', '0', '0')

typedef struct _USBNT_VID_FRAME {
    USHORT  Width;
    USHORT  Height;
    ULONG   MaxFrameSize;           /* bytes */
    ULONG   NumIntervals;
    ULONG   Interval[USBNT_VID_INTERVALS];  /* frame intervals in 100 ns units, shortest first */
} USBNT_VID_FRAME;

typedef struct _USBNT_VID_FORMAT {
    ULONG   FourCC;                 /* USBNT_FCC_* */
    UCHAR   BitsPerPixel;           /* 0 for compressed formats */
    UCHAR   NumFrames;
    UCHAR   Pad[2];
    USBNT_VID_FRAME Frame[USBNT_VID_FRAMES];
} USBNT_VID_FORMAT;

#define USBNT_VCTL_BRIGHTNESS   0x02    /* UVC processing unit selectors */
#define USBNT_VCTL_CONTRAST     0x03
#define USBNT_VCTL_HUE          0x06
#define USBNT_VCTL_SATURATION   0x07
#define USBNT_VCTL_SHARPNESS    0x08
#define USBNT_VCTL_GAMMA        0x09
#define USBNT_VCTL_WB_TEMP      0x0A
#define USBNT_VCTL_GAIN         0x04
#define USBNT_VCTL_POWERLINE    0x05

typedef struct _USBNT_CAMERA {
    ULONG   Id;                     /* 0 = unused entry */
    UCHAR   DevIndex;               /* USBNT_DEV_INFO index */
    UCHAR   NumFormats;
    UCHAR   Streaming;
    UCHAR   UvcMinor;               /* bcdUVC: 0x10 = 1.0, 0x11 = 1.1, 0x15 = 1.5 */
    ULONG   Controls;               /* bit n = processing unit selector n available */
    char    Name[48];
    USBNT_VID_FORMAT Format[USBNT_VID_FORMATS];
} USBNT_CAMERA;

typedef struct _USBNT_CAMERAS {
    ULONG   Version;                /* USBNT_VIDEO_VERSION */
    ULONG   Count;
    USBNT_CAMERA Cam[USBNT_MAX_CAMS];
} USBNT_CAMERAS;

#define USBNT_VIDEO_VERSION     1

typedef struct _USBNT_VID_START {   /* in */
    ULONG   Id;
    ULONG   Format;                 /* index into USBNT_CAMERA.Format */
    ULONG   Frame;                  /* index into .Frame */
    ULONG   Interval;               /* 100 ns, 0 = fastest */
} USBNT_VID_START;

typedef struct _USBNT_VID_STATUS {  /* out of START */
    LONG    Result;                 /* 0 = streaming */
    ULONG   FourCC;
    USHORT  Width;
    USHORT  Height;
    ULONG   MaxFrameSize;
    ULONG   Interval;
} USBNT_VID_STATUS;

typedef struct _USBNT_VID_GET {     /* in of FRAME: wait for a frame newer than LastSequence */
    ULONG   Id;
    ULONG   LastSequence;
    ULONG   TimeoutMs;
} USBNT_VID_GET;

typedef struct _USBNT_VID_FRAME_HDR { /* start of the FRAME output buffer, image data follows */
    ULONG   Sequence;
    ULONG   Bytes;                  /* image bytes after the header */
    ULONG   FourCC;
    USHORT  Width;
    USHORT  Height;
    ULONG   TimeMs;
    ULONG   Flags;                  /* bit 0: the frame had errors */
} USBNT_VID_FRAME_HDR;

typedef struct _USBNT_VID_CONTROL { /* in and out */
    ULONG   Id;
    ULONG   Selector;               /* USBNT_VCTL_* */
    ULONG   Set;                    /* 1 = set Value */
    LONG    Value;
    LONG    Min, Max, Def, Res;     /* out */
} USBNT_VID_CONTROL;

#endif
