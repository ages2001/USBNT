/*
 * ntdrv.h - Windows NT 3.1 - 4.0 side of the driver.
 */

#ifndef NTDRV_H
#define NTDRV_H

#include "usbscsi.h"
#include "usbinput.h"
#include <ntddscsi.h>
#include <srb.h>
#include <ntddkbd.h>
#include <ntddmou.h>
#include <kbdmou.h>
#include "usbntioc.h"

/*
 * One binary for NT 3.1 to 4.0: only kernel and HAL entry points that every
 * one of these versions exports. The DDK headers route several calls to the
 * fastcall variants that appeared in NT 3.5 and 3.51.
 */
#undef KeAcquireSpinLock
#undef KeReleaseSpinLock
#undef KeRaiseIrql
#undef KeLowerIrql
#undef IoCompleteRequest
#undef KeQueryTickCount
#undef KeClearEvent
#undef IoSetCancelRoutine
#undef RtlMoveMemory
#undef RtlCopyMemory
#undef RtlZeroMemory
#undef InterlockedIncrement
#undef InterlockedDecrement
#undef InterlockedExchange

__declspec(dllimport) VOID NTAPI KeAcquireSpinLock(PKSPIN_LOCK SpinLock, PKIRQL OldIrql);
__declspec(dllimport) VOID NTAPI KeReleaseSpinLock(PKSPIN_LOCK SpinLock, KIRQL NewIrql);
__declspec(dllimport) VOID NTAPI KeRaiseIrql(KIRQL NewIrql, PKIRQL OldIrql);
__declspec(dllimport) VOID NTAPI KeLowerIrql(KIRQL NewIrql);
__declspec(dllimport) VOID NTAPI IoCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
__declspec(dllimport) VOID NTAPI KeQueryTickCount(PLARGE_INTEGER TickCount);

LONG    NtAtomicAdd(LONG volatile *Value, LONG Delta);
LONG    NtAtomicSwap(LONG volatile *Value, LONG New);
PDRIVER_CANCEL NtSetCancelRoutine(PIRP Irp, PDRIVER_CANCEL Routine);

#define KeClearEvent(e)             KeResetEvent(e)
#define RtlMoveMemory(d, s, n)      UsbMemCpy((d), (s), (ULONG)(n))
#define RtlCopyMemory(d, s, n)      UsbMemCpy((d), (s), (ULONG)(n))
#define RtlZeroMemory(d, n)         UsbMemSet((d), 0, (ULONG)(n))
#define InterlockedIncrement(p)     NtAtomicAdd((LONG volatile *)(p), 1)
#define InterlockedDecrement(p)     NtAtomicAdd((LONG volatile *)(p), -1)
#define InterlockedExchange(p, v)   NtAtomicSwap((LONG volatile *)(p), (LONG)(v))
#define IoSetCancelRoutine(Irp, R)  NtSetCancelRoutine((Irp), (R))

/* values missing from older DDK headers */
#ifndef SRB_FUNCTION_REMOVE_DEVICE
#define SRB_FUNCTION_REMOVE_DEVICE      0x16
#endif
#ifndef SRB_FUNCTION_FLUSH_QUEUE
#define SRB_FUNCTION_FLUSH_QUEUE        0x15
#endif
#ifndef SRB_FUNCTION_TERMINATE_IO
#define SRB_FUNCTION_TERMINATE_IO       0x14
#endif
#ifndef SRB_FUNCTION_RELEASE_RECOVERY
#define SRB_FUNCTION_RELEASE_RECOVERY   0x11
#endif
#ifndef MOUSE_WHEEL
#define MOUSE_WHEEL                     0x0400
#endif
#ifndef WHEELMOUSE_I8042_HARDWARE
#define WHEELMOUSE_I8042_HARDWARE       0x0020
#endif
#ifndef FILE_DEVICE_8042_PORT
#define FILE_DEVICE_8042_PORT           0x00000027
#endif

#define EXT_SCSI        1
#define EXT_KBD         2
#define EXT_MOUSE       3
#define EXT_CTL         4

typedef struct _NT_EXT {
    ULONG           Kind;
    ULONG           Number;
    PDEVICE_OBJECT  Self;
} NT_EXT;

/* ntos.c */
void    NtOsInit(PUNICODE_STRING RegistryPath);
void    NtKernelInit(PDRIVER_OBJECT Drv);
PVOID   NtKernelExport(const char *Name);
PVOID   NtHalExport(const char *Name);
extern  UNICODE_STRING NtRegistryPath;
extern  KEVENT NtWorkerEvent;

/* ntscsi.c */
NTSTATUS NtScsiCreate(PDRIVER_OBJECT Drv);
NTSTATUS NtScsiDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS NtScsiSrb(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS NtCtlEject(ULONG Slot);
extern ULONG NtScsiPortNumber;

/* ntinput.c */
NTSTATUS NtInputCreate(PDRIVER_OBJECT Drv);
NTSTATUS NtInputIoctl(PDEVICE_OBJECT Dev, PIRP Irp);
NTSTATUS NtCtlIoctl(PDEVICE_OBJECT Dev, PIRP Irp);
void     NtCtlCreateClose(PDEVICE_OBJECT Dev, PIRP Irp, BOOLEAN Create);
void     NtCtlCleanup(PDEVICE_OBJECT Dev, PIRP Irp);

#endif
