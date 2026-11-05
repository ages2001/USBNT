/*
 * usbntcli.h - generic USB device interface of USBNT for other kernel
 * drivers.  USBNT exposes EVERY USB device it enumerates through
 * \Device\UsbNtDev; a client driver (RLTKLANT for Realtek USB LAN, and
 * any other) registers, is told when a device arrives or leaves, picks the
 * ones it supports by vendor/product/class, and drives them with control
 * and bulk/interrupt transfers.  It is the network/vendor counterpart of
 * the audio interface in usbntaud.h and follows the same shape.
 *
 * Self contained: needs only the NT basic types (ULONG, USHORT, UCHAR,
 * BOOLEAN).
 *
 * A client opens \Device\UsbNtDev with IoGetDeviceObjectPointer and either
 * sends IOCTL_USBNT_DEV_INTERFACE as an internal device control request
 * (METHOD_NEITHER, output buffer = USBNT_DEV_IF) or reads the table from
 * the device extension (USBNT_DEV_EXT, check Magic and If->Version).  The
 * functions are then called directly:
 *
 *   Register / Unregister / GetInfo / GetDescriptor / Control / Transfer /
 *   Claim                                   PASSIVE_LEVEL (worker or client)
 *   UC_CLIENT Arrival / Removal callbacks   USBNT worker thread (PASSIVE)
 *
 * Copyright (C) 2026 ages2001.
 */
#ifndef USBNTCLI_H
#define USBNTCLI_H

#define USBNT_DEV_DEVICE_NAME       L"\\Device\\UsbNtDev"
/* CTL_CODE(FILE_DEVICE_UNKNOWN, 0x930, METHOD_NEITHER, FILE_ANY_ACCESS) */
#define IOCTL_USBNT_DEV_INTERFACE   0x002224C3
#define USBNT_DEV_IF_VERSION        2 /* attachment tokens and callback rundown */

/* every function and callback is __cdecl, whatever the compiler default */
#ifndef UC_CALL
#if defined(_MSC_VER) || defined(_WIN32) || defined(__MINGW32__)
#define UC_CALL __cdecl
#elif defined(__GNUC__) && defined(__i386__)
#define UC_CALL __attribute__((cdecl))
#else
#define UC_CALL
#endif
#endif

#define UC_MAX_CLIENTS      4

/* speed, same values as usbntioc.h USBNT_SPEED_* */
#define UC_SPEED_FULL       0
#define UC_SPEED_LOW        1
#define UC_SPEED_HIGH       2
#define UC_SPEED_SUPER      3

typedef struct _UC_DEVINFO {
    ULONG   Id;                     /* handle, never 0, stable while present */
    USHORT  Vid;
    USHORT  Pid;
    USHORT  BcdDevice;
    UCHAR   Class;                  /* device descriptor bDeviceClass */
    UCHAR   SubClass;
    UCHAR   Protocol;
    UCHAR   Speed;                  /* UC_SPEED_* */
    UCHAR   NumInterfaces;
    UCHAR   Pad;
    char    Manufacturer[32];
    char    Product[48];
} UC_DEVINFO;

/* Callbacks run at PASSIVE_LEVEL (worker or Register replay). Keep them
 * nonblocking. Never call Unregister from this client's own callback.
 * Unregister waits for callbacks already in progress before returning. */
typedef struct _UC_CLIENT {
    void   *Context;
    void  (UC_CALL *Arrival)(void *Ctx, const UC_DEVINFO *Info);
    void  (UC_CALL *Removal)(void *Ctx, ULONG Id);
} UC_CLIENT;

typedef struct _USBNT_DEV_IF {
    ULONG   Version;                /* USBNT_DEV_IF_VERSION */
    ULONG   Size;                   /* sizeof(USBNT_DEV_IF) */
    int   (UC_CALL *Register)(const UC_CLIENT *Client);
    void  (UC_CALL *Unregister)(const UC_CLIENT *Client);
    int   (UC_CALL *GetInfo)(ULONG Id, UC_DEVINFO *Info);
    /* a standard descriptor (device = 1, config = 2, string = 3, ...) */
    int   (UC_CALL *GetDescriptor)(ULONG Id, UCHAR Type, UCHAR Index,
                  void *Buf, ULONG Len, ULONG *Actual);
    /* a control transfer on endpoint 0 (standard, class or vendor) */
    int   (UC_CALL *Control)(ULONG Id, UCHAR ReqType, UCHAR Req, USHORT Value,
                  USHORT Index, void *Buf, USHORT Len, ULONG TimeoutMs, ULONG *Actual);
    /* a bulk or interrupt transfer on the endpoint address (0x81, 0x02, ...).
       The pipe is opened on first use and kept, so the data toggle is
       preserved across transfers. */
    int   (UC_CALL *Transfer)(ULONG Id, UCHAR Endpoint, void *Buf, ULONG Len,
                  ULONG TimeoutMs, ULONG *Actual);
    /* Claim/release is advisory; it does not identify an owner. */
    int   (UC_CALL *Claim)(ULONG Id, int Claim);
} USBNT_DEV_IF;

/* device extension of \Device\UsbNtDev */
#define USBNT_DEV_EXT_MAGIC         0x56444E55      /* "UNDV" */
typedef struct _USBNT_DEV_EXT {
    ULONG   Kind;                   /* USBNT private */
    ULONG   Number;
    void   *Self;
    ULONG   Magic;                  /* USBNT_DEV_EXT_MAGIC */
    const USBNT_DEV_IF *If;
} USBNT_DEV_EXT;

/* return values */
#define UC_OK               0
#define UC_ERR_NODEV        (-5)
#define UC_ERR_NOMEM        (-6)
#define UC_ERR_PARAM        (-7)
#define UC_ERR_IO           (-8)
#define UC_ERR_BUSY         (-9)

#endif
