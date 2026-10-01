/*
 * usbnt.h - USB core definitions shared by host controller, core and
 * class driver code.
 */

#ifndef USBNT_H
#define USBNT_H

#include "usbos.h"

#define USBNT_VERSION_STR "1.0"

/* ------------------------------------------------------------------ */
/* Utilities (usbutil.c)                                                */
/* ------------------------------------------------------------------ */

void    UsbMemCpy(void *Dst, const void *Src, ULONG Len);
void    UsbMemSet(void *Dst, int Val, ULONG Len);
int     UsbMemCmp(const void *A, const void *B, ULONG Len);
int     UsbVFormat(char *Buf, int Size, const char *Fmt, va_list Ap);
int     UsbFormat(char *Buf, int Size, const char *Fmt, ...);
void    UsbLog(ULONG Level, const char *Fmt, ...);
void    UsbHexDump(ULONG Level, const char *Title, const void *Data, ULONG Len);

#define LOG_ERR     1
#define LOG_INFO    2
#define LOG_DBG     3
#define LOG_TRACE   4

#define GET16(p)    ((USHORT)(((const UCHAR *)(p))[0] | (((const UCHAR *)(p))[1] << 8)))
#define GET32(p)    ((ULONG)GET16(p) | ((ULONG)GET16((const UCHAR *)(p) + 2) << 16))
#define GETBE16(p)  ((USHORT)((((const UCHAR *)(p))[0] << 8) | ((const UCHAR *)(p))[1]))
#define GETBE32(p)  (((ULONG)GETBE16(p) << 16) | GETBE16((const UCHAR *)(p) + 2))
void    PUT16(void *P, USHORT V);
void    PUT32(void *P, ULONG V);
void    PUTBE16(void *P, USHORT V);
void    PUTBE32(void *P, ULONG V);

/* ------------------------------------------------------------------ */
/* USB protocol constants                                               */
/* ------------------------------------------------------------------ */

#define USB_SPEED_FULL      0
#define USB_SPEED_LOW       1
#define USB_SPEED_HIGH      2
#define USB_SPEED_SUPER     3

#define USB_EP_CONTROL      0
#define USB_EP_ISOCH        1
#define USB_EP_BULK         2
#define USB_EP_INTERRUPT    3

#define USB_DT_DEVICE       1
#define USB_DT_CONFIG       2
#define USB_DT_STRING       3
#define USB_DT_INTERFACE    4
#define USB_DT_ENDPOINT     5
#define USB_DT_HID          0x21
#define USB_DT_REPORT       0x22
#define USB_DT_HUB          0x29
#define USB_DT_SS_HUB       0x2A
#define USB_DT_SS_EP_COMP   0x30

#define USB_REQ_GET_STATUS          0
#define USB_REQ_CLEAR_FEATURE       1
#define USB_REQ_SET_FEATURE         3
#define USB_REQ_SET_ADDRESS         5
#define USB_REQ_GET_DESCRIPTOR      6
#define USB_REQ_GET_CONFIGURATION   8
#define USB_REQ_SET_CONFIGURATION   9
#define USB_REQ_SET_INTERFACE       11

#define USB_RT_IN           0x80
#define USB_RT_OUT          0x00
#define USB_RT_STD          0x00
#define USB_RT_CLASS        0x20
#define USB_RT_VENDOR       0x40
#define USB_RT_DEVICE       0x00
#define USB_RT_INTERFACE    0x01
#define USB_RT_ENDPOINT     0x02
#define USB_RT_OTHER        0x03

#define USB_FEATURE_ENDPOINT_HALT   0

#define USB_CLASS_HID       0x03
#define USB_CLASS_MSC       0x08
#define USB_CLASS_HUB       0x09

/* Transfer status */
#define USB_OK              0
#define USB_ERR_STALL       (-1)
#define USB_ERR_IO          (-2)
#define USB_ERR_TIMEOUT     (-3)
#define USB_ERR_ABORTED     (-4)
#define USB_ERR_NODEV       (-5)
#define USB_ERR_NOMEM       (-6)
#define USB_ERR_PARAM       (-7)
#define USB_ERR_BABBLE      (-8)
#define USB_ERR_BUSY        (-9)

/* ------------------------------------------------------------------ */
/* Core objects                                                         */
/* ------------------------------------------------------------------ */

#define USB_MAX_HC          16
#define USB_MAX_DEVICES     64
#define USB_MAX_PORTS       32
#define USB_MAX_DEPTH       5

#define USB_PAGE_SIZE       4096
#define USB_DEV_DATA_OFF    64          /* control data offset in device page */
#define USB_DEV_DATA_MAX    2048        /* control data area size */
#define USB_DEV_SCRATCH_OFF 2112        /* free area for class drivers */
#define USB_DEV_SCRATCH_MAX (USB_PAGE_SIZE - USB_DEV_SCRATCH_OFF)

typedef struct _USB_HC   USB_HC;
typedef struct _USB_DEV  USB_DEV;
typedef struct _USB_PIPE USB_PIPE;
typedef struct _USB_XFER USB_XFER;

typedef void (*USB_XFER_DONE)(USB_XFER *Xfer);

struct _USB_XFER {
    USB_PIPE       *Pipe;
    UCHAR          *Buf;            /* virtual address of data */
    ULONG           Phys;           /* physical address, contiguous */
    ULONG          *Pfn;            /* or page frame list (Hc->SgMax != 0) */
    ULONG           PageOff;        /* offset of the data in the first page */
    ULONG           Length;
    ULONG           SetupPhys;      /* control transfers only */
    UCHAR           DirIn;          /* direction of data stage */
    UCHAR           Busy;
    UCHAR           Pad[2];
    LONG            Status;
    ULONG           Actual;
    USB_XFER_DONE   Complete;
    void           *Context;
    ULONG           Hc[8];          /* host controller private */
    USB_XFER       *DoneNext;
    USB_XFER       *QNext;          /* next transfer queued on the pipe */
};

struct _USB_PIPE {
    USB_DEV        *Dev;
    UCHAR           Endpoint;       /* 0..15 */
    UCHAR           DirIn;
    UCHAR           Type;           /* USB_EP_* */
    UCHAR           Interval;       /* raw bInterval */
    UCHAR           Toggle;
    UCHAR           MaxBurst;
    UCHAR           Opened;
    UCHAR           Halted;
    USHORT          MaxPacket;
    USHORT          Pad;
    USB_XFER       *Cur;            /* oldest queued transfer */
    USB_XFER       *Tail;
    void           *HcPriv;
    USHORT          Stream;         /* stream id, 0 = none */
    USHORT          NumStreams;     /* streams opened on this endpoint */
    USB_PIPE       *Parent;         /* stream pipes: the endpoint pipe */
};

struct _USB_DEV {
    UCHAR           InUse;
    UCHAR           Gone;
    UCHAR           Index;
    UCHAR           Speed;
    UCHAR           Address;
    UCHAR           Port;           /* port on parent hub or root hub, 1 based */
    UCHAR           RootPort;       /* root port the tree hangs off, 1 based */
    UCHAR           Depth;          /* 0 = on root port */
    UCHAR           TtPort;         /* port on TtHub */
    UCHAR           IsHub;
    UCHAR           HubPorts;
    UCHAR           HubTtt;         /* think time, 0..3 */
    UCHAR           HubMtt;
    UCHAR           ConfigValue;
    UCHAR           Function;       /* USB_FUNC_* bits of the bound class drivers */
    UCHAR           LinkGbps;       /* SuperSpeed link rate, Gbit/s */
    ULONG           Route;          /* xHCI route string */
    USB_HC         *Hc;
    USB_DEV        *Parent;
    USB_DEV        *TtHub;          /* nearest high speed hub for LS/FS devices */
    USB_PIPE        Ep0;
    USB_XFER        CtlXfer;
    OS_EVENT        CtlEvent;
    UCHAR          *Page;           /* one DMA page per device */
    ULONG           PagePhys;
    UCHAR           DevDesc[18];
    USHORT          ConfigLen;
    UCHAR          *Config;
    void           *HcPriv;
    void           *Hub;
    void           *Msc;
    void           *Hid[4];
    char            Mfg[32];        /* string descriptors, ASCII */
    char            Product[48];
    char            Serial[32];
};

#define USB_FUNC_HUB        0x01
#define USB_FUNC_KEYBOARD   0x02
#define USB_FUNC_MOUSE      0x04
#define USB_FUNC_TABLET     0x08
#define USB_FUNC_STORAGE    0x10

/* Normalized root port status */
#define PS_CONNECT          0x0001
#define PS_ENABLE           0x0002
#define PS_CHANGE           0x0004
#define PS_OTHER_OWNER      0x0008
#define PS_OVERCURRENT      0x0010
#define PS_POWER            0x0020
#define PS_SS_PORT          0x0040
#define PS_SPEED_SHIFT      8
#define PS_SPEED(s)         (((s) >> PS_SPEED_SHIFT) & 0xF)

/* PortReset result besides USB_OK / errors */
#define USB_PORT_HANDOFF    1

typedef struct _HCD_OPS {
    const char *Name;
    int     (*Start)(USB_HC *Hc);
    void    (*Stop)(USB_HC *Hc);
    ULONG   (*PortStatus)(USB_HC *Hc, int Port);
    void    (*PortClearChange)(USB_HC *Hc, int Port);
    int     (*PortReset)(USB_HC *Hc, int Port, UCHAR *Speed);
    void    (*PortDisable)(USB_HC *Hc, int Port);
    int     (*DevInit)(USB_DEV *Dev);
    int     (*DevSetAddress)(USB_DEV *Dev);
    int     (*DevUpdate)(USB_DEV *Dev);
    void    (*DevFree)(USB_DEV *Dev);
    int     (*PipeOpen)(USB_PIPE *Pipe);
    void    (*PipeClose)(USB_PIPE *Pipe);
    int     (*Submit)(USB_XFER *Xfer);
    void    (*Cancel)(USB_PIPE *Pipe);
    int     (*PipeReset)(USB_PIPE *Pipe);
    void    (*Poll)(USB_HC *Hc);
    BOOLEAN (*Interrupt)(USB_HC *Hc);
    int     (*StreamsOpen)(USB_PIPE *Pipe, USB_PIPE *Child, int Count);
} HCD_OPS;

#define HC_UHCI     0
#define HC_OHCI     1
#define HC_EHCI     2
#define HC_XHCI     3

struct _USB_HC {
    const HCD_OPS  *Ops;
    UCHAR           Type;
    UCHAR           Index;
    UCHAR           Bus;
    UCHAR           Dev;
    UCHAR           Fn;
    UCHAR           IrqLine;
    UCHAR           NumPorts;
    UCHAR           Running;
    ULONG           IoBase;
    UCHAR          *Mmio;
    ULONG           MmioPhys;
    ULONG           MmioLen;
    ULONG           MaxXfer;        /* largest single transfer segment */
    ULONG           SgMax;          /* largest page list transfer, 0 = none */
    UCHAR           QueueOk;        /* several transfers may be queued per pipe */
    UCHAR           MaxStreams;     /* bulk streams supported, 0 = none */
    UCHAR           Pad2[2];
    USHORT          VendorId;
    USHORT          DeviceId;
    USB_HC         *Ehci;           /* EHCI owning this companion */
    USB_DEV        *RootDev[USB_MAX_PORTS + 1];
    UCHAR           PortGbps[USB_MAX_PORTS + 1];    /* SuperSpeed link rate after reset */
    ULONG           PortRetry[USB_MAX_PORTS + 1];
    UCHAR           AddrMap[16];
    void           *Priv;
};

/* ------------------------------------------------------------------ */
/* Core API (usbcore.c)                                                 */
/* ------------------------------------------------------------------ */

extern USB_HC  *UsbHcs[USB_MAX_HC];
extern int      UsbHcCount;
extern USB_DEV  UsbDevs[USB_MAX_DEVICES];

int     UsbInit(void);
void    UsbShutdown(void);
void    UsbPollAll(void);
BOOLEAN UsbInterruptAll(void);
void    UsbWorkerIteration(void);
void    UsbTick(void);

void    UsbXferDone(USB_XFER *Xfer, LONG Status, ULONG Actual);

int     UsbSubmit(USB_XFER *Xfer);
int     UsbTransfer(USB_PIPE *Pipe, void *Buf, ULONG Phys, ULONG Len, ULONG TimeoutMs, ULONG *Actual);
int     UsbTransferPages(USB_PIPE *Pipe, ULONG *Pfn, ULONG PageOff, ULONG Len, ULONG TimeoutMs, ULONG *Actual);
ULONG   UsbXferPhys(USB_XFER *X, ULONG Off, ULONG *Run);
int     UsbControl(USB_DEV *Dev, UCHAR ReqType, UCHAR Req, USHORT Value, USHORT Index,
                   USHORT Len, void *Data, ULONG TimeoutMs, ULONG *Actual);
int     UsbGetDescriptor(USB_DEV *Dev, UCHAR Type, UCHAR Index, USHORT LangId, void *Buf, USHORT Len, ULONG *Actual);
int     UsbOpenPipe(USB_DEV *Dev, USB_PIPE *Pipe, const UCHAR *EpDesc);
void    UsbClosePipe(USB_PIPE *Pipe);
void    UsbCancelPipe(USB_PIPE *Pipe);
int     UsbClearHalt(USB_PIPE *Pipe);
int     UsbResetPipeToggle(USB_PIPE *Pipe);
int     UsbOpenStreams(USB_PIPE *Pipe, USB_PIPE *Child, int Count);

USB_DEV *UsbDevAttach(USB_HC *Hc, USB_DEV *Parent, int Port, UCHAR Speed);
void    UsbDevDetach(USB_DEV *Dev);

const UCHAR *UsbFindDesc(const UCHAR *Start, const UCHAR *End, UCHAR Type);
const UCHAR *UsbNextDesc(const UCHAR *Cur, const UCHAR *End);

/* DMA pool (dmapool.c) */
void   *UsbDmaAlloc(ULONG Size, ULONG *Phys);    /* Size <= 4096, aligned to its size class */
void    UsbDmaFree(void *Ptr, ULONG Size);

/* Host controllers */
extern const HCD_OPS UhciOps;
extern const HCD_OPS OhciOps;
extern const HCD_OPS EhciOps;
extern const HCD_OPS XhciOps;

/* Class drivers */
int     HubAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End);
void    HubDetach(USB_DEV *Dev);
void    HubServiceAll(void);
void    HubRemoveChildren(USB_DEV *Dev);

int     MscAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End);
void    MscDetach(USB_DEV *Dev);
void    MscService(void);

int     HidAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End);
void    HidDetach(USB_DEV *Dev);
void    HidService(void);
void    HidTick(void);

void    UsbPortPoll(USB_HC *Hc);

/* Device or slot changes: counter for user mode and an OS notification */
extern volatile ULONG UsbChangeCount;
void    UsbNotifyChange(void);
void    OsNotifyChange(void);

#endif
