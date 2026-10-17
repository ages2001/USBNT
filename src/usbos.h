/*
 * usbos.h - operating system abstraction used by the portable USB code.
 *
 * The portable part of the driver (host controllers, USB core, class
 * drivers) talks to the outside world only through the functions below.
 * ntos.c implements them for Windows NT 3.1 - 4.0, harness/os.c implements
 * them for the bare metal QEMU test kernel.
 */

#ifndef USBOS_H
#define USBOS_H

#include <stdarg.h>

#ifdef USB_HARNESS

typedef unsigned char   UCHAR;
typedef unsigned short  USHORT;
typedef unsigned long   ULONG;
typedef unsigned long long ULONGLONG;
typedef char            CHAR;
typedef short           SHORT;
typedef long            LONG;
typedef unsigned char   BOOLEAN;
typedef void           *PVOID;
typedef ULONG           OS_IRQL;
typedef ULONG           USB_UPTR;       /* integer as wide as a pointer */

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#ifndef NULL
#define NULL  ((void *)0)
#endif

typedef struct _OS_EVENT {
    volatile ULONG Signaled;
} OS_EVENT;

#else

#include <ntddk.h>

typedef KIRQL OS_IRQL;
typedef ULONG_PTR USB_UPTR;             /* integer as wide as a pointer */

typedef struct _OS_EVENT {
    KEVENT Event;
    volatile LONG Signaled;
} OS_EVENT;

#endif

/* Memory */
void   *OsAlloc(ULONG Size);                 /* zeroed, non paged */
void    OsFree(void *Ptr);
void   *OsDmaAlloc(ULONG Size, ULONG *Phys); /* zeroed, contiguous, page aligned, below 4 GB */
void    OsDmaFree(void *Ptr, ULONG Size);

/* Time */
void    OsStallUs(ULONG Us);
ULONG   OsTimeMs(void);
void    OsSleepMs(ULONG Ms);                 /* worker context only */

/* PCI configuration space; Size is 1, 2 or 4 */
ULONG   OsPciRead(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, int Size);
void    OsPciWrite(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, ULONG Val, int Size);

/* Register access */
void   *OsMapMmio(UCHAR Bus, ULONG Phys, ULONG Len);
void    OsUnmapMmio(void *Va, ULONG Len);
UCHAR   OsIn8(ULONG Port);
USHORT  OsIn16(ULONG Port);
ULONG   OsIn32(ULONG Port);
void    OsOut8(ULONG Port, UCHAR Val);
void    OsOut16(ULONG Port, USHORT Val);
void    OsOut32(ULONG Port, ULONG Val);

#define MmioRd8(a)      (*(volatile UCHAR  *)(a))
#define MmioRd16(a)     (*(volatile USHORT *)(a))
#define MmioRd32(a)     (*(volatile ULONG  *)(a))
#define MmioWr8(a, v)   (*(volatile UCHAR  *)(a) = (UCHAR)(v))
#define MmioWr16(a, v)  (*(volatile USHORT *)(a) = (USHORT)(v))
#define MmioWr32(a, v)  (*(volatile ULONG  *)(a) = (ULONG)(v))

/* Global lock protecting host controller schedules and transfer state */
OS_IRQL OsLock(void);
void    OsUnlock(OS_IRQL Irql);

/* No thread switches until OsLower (short timed register sequences) */
OS_IRQL OsRaise(void);
void    OsLower(OS_IRQL Irql);

/* Events used by the worker to wait for completions */
void    OsEventInit(OS_EVENT *Ev);
void    OsEventSet(OS_EVENT *Ev);
void    OsEventReset(OS_EVENT *Ev);
BOOLEAN OsEventWait(OS_EVENT *Ev, ULONG TimeoutMs);  /* worker context only */

/* Wakes the worker (hub change, pending requests) */
void    OsWakeWorker(void);
BOOLEAN OsInWorker(void);                   /* running in the worker (or a single threaded host) */

/* Diagnostics */
void    OsLogStr(const char *Str);
ULONG   OsLogRead(char *Out, ULONG Max);       /* NT: the latest log text */
extern  ULONG UsbDebugLevel;

/* Registry style configuration, returns Default when not present */
ULONG   OsGetConfig(const char *Name, ULONG Default);

/* Small binary values kept across boots (Thunderbolt device keys), worker context */
BOOLEAN OsStoreGet(const char *Name, void *Data, ULONG Len);  /* FALSE when not there */
BOOLEAN OsStoreSet(const char *Name, const void *Data, ULONG Len);

/* Timer noise for random numbers */
ULONGLONG OsEntropy(void);

#endif
