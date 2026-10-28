/*
 * ntinput.c - keyboard and pointer port devices for kbdclass/mouclass and
 * the \\.\UsbNt control device (wheel channel for NT 3.x, slot control).
 */

#include "ntdrv.h"
#include "usbvideo.h"
#include "usbblk.h"
#include "tbt.h"
#include "smbtp.h"

typedef VOID (NTAPI *CLASS_SERVICE)(PVOID ClassDeviceObject, PVOID Start, PVOID End, PVOID Consumed);

typedef struct _PORT_STATE {
    PDEVICE_OBJECT  Device;
    PDEVICE_OBJECT  ClassDevice;
    CLASS_SERVICE   Service;
    LONG            Enabled;
    ULONG           Number;
    PDEVICE_OBJECT  Owner;              /* Windows 2000 / XP: our device the class driver connected on */
} PORT_STATE;

static PORT_STATE Kbd;
static PORT_STATE Mou;
#define LENT_TRIES      40              /* searches for it, one per key at most once a second */
static PORT_STATE Lent;                 /* the class driver of another keyboard port (below) */
static volatile LONG LentWanted;
static ULONG LentTries;
static ULONG LentLastTry;
static BOOLEAN SetupChecked;
static BOOLEAN InTextSetup;
static PDRIVER_OBJECT InputDriver;
static KSPIN_LOCK InputLock;
static KEYBOARD_TYPEMATIC_PARAMETERS Typematic = { 0, 30, 500 };
static KEYBOARD_INDICATOR_PARAMETERS Indicators = { 0, 0 };

static PDEVICE_OBJECT CtlDevice;
static PIRP WheelIrp;
static LONG WheelAccum;
static LONG PanAccum;               /* horizontal wheel: usbmon turns it into WM_HSCROLL */
static LONG WheelOpen;
static ULONG WheelMode;

#define USB_WHEEL_DELTA 120
#define WHEEL_AUTO      0
#define WHEEL_FLAG_ONLY 1
#define WHEEL_CHANNEL   2

/* button channel to usbmon (IOCTL_USBNT_MOUSE_*), under the cancel spin lock */
static ULONG MouCap;                    /* USBNT_CAP_* */
static PFILE_OBJECT MouCapOwner;        /* the handle that set it */
static USHORT MouRouted;                /* *_DOWN flags of the buttons whose press went to usbmon */
static PIRP MouIrp;
static USBNT_MOUSE_EVENT MouQ[USBNT_MOUSE_MAX_EVENTS];
static ULONG MouQn;
static ULONG MouQTime;                  /* when the oldest queued event came */

static void MakeName(WCHAR *Buf, const WCHAR *Base, ULONG N)
{
    WCHAR digits[12];
    int n = 0;
    int i = 0;

    while (*Base) {
        Buf[i++] = *Base++;
    }
    do {
        digits[n++] = (WCHAR)(L'0' + N % 10);
        N /= 10;
    } while (N != 0);
    while (n > 0) {
        Buf[i++] = digits[--n];
    }
    Buf[i] = 0;
}

#ifndef USB_WDM
static NTSTATUS CreatePort(PDRIVER_OBJECT Drv, PORT_STATE *P, ULONG Kind, const WCHAR *Base,
                           const WCHAR *MapKey, DEVICE_TYPE Type)
{
    WCHAR name[64];
    UNICODE_STRING uname;
    NT_EXT *ext;
    NTSTATUS st = STATUS_UNSUCCESSFUL;
    ULONG len;
    ULONG n;

    /* like the port drivers of NT, take the first free \Device\xxxPortN */
    for (n = 0; n < 8; n++) {
        MakeName(name, Base, n);
        RtlInitUnicodeString(&uname, name);
        st = IoCreateDevice(Drv, sizeof(NT_EXT), &uname, Type, 0, FALSE, &P->Device);
        if (NT_SUCCESS(st)) {
            break;
        }
    }
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "cannot create input port device (%x)\n", st);
        return st;
    }
    P->Number = n;
    ext = (NT_EXT *)P->Device->DeviceExtension;
    ext->Kind = Kind;
    ext->Number = P->Number;
    ext->Self = P->Device;
    P->Device->Flags |= DO_BUFFERED_IO;
    P->Device->Flags &= ~DO_DEVICE_INITIALIZING;

    len = NtRegistryPath.Length + sizeof(WCHAR);
    RtlWriteRegistryValue(RTL_REGISTRY_DEVICEMAP, (PWSTR)MapKey, name, REG_SZ, NtRegistryPath.Buffer, len);
    return STATUS_SUCCESS;
}

/* kbdclass and mouclass must serve more than one port (the PS/2 one and ours) into
   KeyboardClass0 / PointerClass0, the devices the window system reads.  usbinst sets
   it; a system that text-mode setup installed USBNT on (TXTSETUP.OEM) has never seen
   usbinst, so the driver sets it itself.  Both class drivers start after usbnt.sys
   (groups Keyboard Class / Pointer Class), so it counts from this boot on. */
static void ClassParams(void)
{
    static const WCHAR *keys[] = { L"Kbdclass\\Parameters", L"Mouclass\\Parameters" };
    static const WCHAR *full[] = { L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Kbdclass\\Parameters",
                                   L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\Mouclass\\Parameters" };
    ULONG one = 1, v;
    int i;

    for (i = 0; i < 2; i++) {
        v = 0;
        if (NtQueryDword(full[i], L"ConnectMultiplePorts", &v) && v == 1) {
            continue;
        }
        RtlCreateRegistryKey(RTL_REGISTRY_SERVICES, (PWSTR)keys[i]);
        if (NT_SUCCESS(RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, (PWSTR)keys[i], L"ConnectMultiplePorts",
                                             REG_DWORD, &one, sizeof(one)))) {
            UsbLog(LOG_INFO, "%s: ConnectMultiplePorts set to 1 (was %u)\n", i ? "mouclass" : "kbdclass", v);
        }
    }
}
#endif

#ifdef USB_WDM
/* Windows 2000 / XP: kbdclass / mouclass leave (the keyboard / mouse device Dev of the bus goes
   away).  Only the device the class driver is connected on counts: when Plug and Play rebuilds
   the device node's stack (the INF is installed, the class changes), the new stack's class
   driver may connect before the old stack is removed - that removal must not cut it off. */
void NtInputDisconnect(ULONG Kind, PDEVICE_OBJECT Dev)
{
    PORT_STATE *p = (Kind == EXT_KBD) ? &Kbd : &Mou;
    KIRQL old;
    BOOLEAN done = FALSE;

    KeAcquireSpinLock(&InputLock, &old);
    if (p->Owner == Dev || p->Owner == NULL) {
        p->Service = NULL;
        p->ClassDevice = NULL;
        p->Enabled = 0;
        p->Owner = NULL;
        done = TRUE;
    }
    KeReleaseSpinLock(&InputLock, old);
    UsbLog(LOG_DBG, "%s class driver %s\n", (Kind == EXT_KBD) ? "keyboard" : "pointer",
           done ? "disconnected" : "of an old device stack gone");
}

/* Windows 2000 / XP: kbdclass / mouclass open (enable) or close (disable) the port */
void NtInputEnable(ULONG Kind, BOOLEAN Enable, PDEVICE_OBJECT Dev)
{
    PORT_STATE *p = (Kind == EXT_KBD) ? &Kbd : &Mou;

    if (p->Owner != NULL && p->Owner != Dev) {
        return;                                 /* an old stack's device */
    }
    if (Enable) {
        InterlockedIncrement(&p->Enabled);
    } else if (p->Enabled > 0) {
        InterlockedDecrement(&p->Enabled);
    }
    UsbLog(LOG_DBG, "%s port %s\n", (Kind == EXT_KBD) ? "keyboard" : "pointer", Enable ? "opened" : "closed");
}
#endif

NTSTATUS NtInputCreate(PDRIVER_OBJECT Drv)
{
    UNICODE_STRING uname;
    UNICODE_STRING ulink;
    NT_EXT *ext;
    NTSTATUS st;

    KeInitializeSpinLock(&InputLock);
    WheelMode = OsGetConfig("WheelMode", WHEEL_AUTO);
    InputDriver = Drv;

#ifndef USB_WDM
    ClassParams();
    /* Windows 2000 / XP: the keyboard and the mouse are devices of the USBNT bus (wdmbus.c) */
    if (!OsGetConfig("DisableKeyboard", 0)) {
        CreatePort(Drv, &Kbd, EXT_KBD, L"\\Device\\KeyboardPort", L"KEYBOARDPORT", FILE_DEVICE_8042_PORT);
    }
    if (!OsGetConfig("DisableMouse", 0)) {
        CreatePort(Drv, &Mou, EXT_MOUSE, L"\\Device\\PointerPort", L"POINTERPORT", FILE_DEVICE_8042_PORT);
    }
#endif

    RtlInitUnicodeString(&uname, USBNT_DEVICE_NAME);
    st = IoCreateDevice(Drv, sizeof(NT_EXT), &uname, FILE_DEVICE_UNKNOWN, 0, FALSE, &CtlDevice);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    ext = (NT_EXT *)CtlDevice->DeviceExtension;
    ext->Kind = EXT_CTL;
    ext->Self = CtlDevice;
    CtlDevice->Flags |= DO_BUFFERED_IO;
    CtlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    RtlInitUnicodeString(&ulink, USBNT_DOS_NAME);
    IoCreateSymbolicLink(&ulink, &uname);
    UsbLog(LOG_INFO, "keyboard port %u, pointer port %u\n", Kbd.Number, Mou.Number);
    return STATUS_SUCCESS;
}

/* undoes NtInputCreate when the driver does not stay loaded: nothing may keep
   pointing at the driver's code once the system unloads it */
void NtInputDestroy(void)
{
    UNICODE_STRING ulink;
    WCHAR name[64];
    if (Kbd.Device != NULL) {
        MakeName(name, L"\\Device\\KeyboardPort", Kbd.Number);
        RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP, L"KEYBOARDPORT", name);
        IoDeleteDevice(Kbd.Device);
        Kbd.Device = NULL;
    }
    if (Mou.Device != NULL) {
        MakeName(name, L"\\Device\\PointerPort", Mou.Number);
        RtlDeleteRegistryValue(RTL_REGISTRY_DEVICEMAP, L"POINTERPORT", name);
        IoDeleteDevice(Mou.Device);
        Mou.Device = NULL;
    }
    if (CtlDevice != NULL) {
        RtlInitUnicodeString(&ulink, USBNT_DOS_NAME);
        IoDeleteSymbolicLink(&ulink);
        IoDeleteDevice(CtlDevice);
        CtlDevice = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Data delivery (DISPATCH_LEVEL or below)                              */
/* ------------------------------------------------------------------ */

static void Deliver(PORT_STATE *P, PVOID Start, PVOID End)
{
    KIRQL old = 0;
    BOOLEAN raised = FALSE;
    ULONG consumed = 0;
    CLASS_SERVICE svc;
    PDEVICE_OBJECT cls;

    svc = P->Service;
    cls = P->ClassDevice;
    if (svc == NULL || cls == NULL || P->Enabled <= 0) {
        return;
    }
    if (KeGetCurrentIrql() < DISPATCH_LEVEL) {
        KeRaiseIrql(DISPATCH_LEVEL, &old);
        raised = TRUE;
    }
    svc(cls, Start, End, &consumed);
    if (raised) {
        KeLowerIrql(old);
    }
}

void OsKbdInput(const USB_KEY_EVENT *Events, ULONG Count)
{
    KEYBOARD_INPUT_DATA data[48];
    ULONG i;

    if (Count > 48) {
        Count = 48;
    }
    for (i = 0; i < Count; i++) {
        data[i].UnitId = 0;
        data[i].MakeCode = Events[i].MakeCode;
        data[i].Flags = Events[i].Flags;
        data[i].Reserved = 0;
        data[i].ExtraInformation = 0;
    }
    /* text-mode setup reads the keyboard it started with: the lent class device goes first there */
    if (Lent.Service != NULL && (InTextSetup || Kbd.Service == NULL)) {
        Deliver(&Lent, &data[0], &data[Count]);
        return;
    }
    /* (text-mode setup: also when kbdclass is on our port - setup reads the keyboard it
       started with; a search that came too early is tried again with the next key) */
    if (Kbd.Service == NULL || !SetupChecked || InTextSetup) {
        if (Lent.Service == NULL && !LentWanted && LentTries < LENT_TRIES) {
            LentWanted = 1;                     /* the worker looks for a class driver (NtInputService) */
            OsWakeWorker();
        }
    }
    Deliver(&Kbd, &data[0], &data[Count]);
}

/* ------------------------------------------------------------------ */
/* USB keys for the class driver of another keyboard                    */
/* ------------------------------------------------------------------ */
/*
 * Text-mode setup of NT 3.x / 4.0 loads usbnt.sys from the driver disk
 * (TXTSETUP.OEM) long after kbdclass, which connected to the ports there
 * were then and to no later one.  Taking the USB controllers from the BIOS
 * also ends its PS/2 emulation, the keyboard setup was using.  So when a USB
 * key comes and no class driver listens to our keyboard port, the keys go to
 * the class device of the PS/2 keyboard (i8042prt) - setup reads that one:
 * its connect data (class device object, class service callback, as kbdclass
 * gave them in IOCTL_INTERNAL_KEYBOARD_CONNECT) is in the port's device
 * extension.  It is recognised by what it points to: a keyboard class device
 * of another driver, and a callback inside that driver's image.  Windows 2000
 * / XP text-mode setup: the same when kbdclass does not come onto USBNT's
 * keyboard device (and there kbdhid's keyboards are looked at too).
 */
typedef NTSTATUS (NTAPI *OB_REF_NAME)(PUNICODE_STRING Name, ULONG Attributes, PACCESS_STATE Access,
                                      ACCESS_MASK Desired, POBJECT_TYPE Type, KPROCESSOR_MODE Mode,
                                      PVOID Context, PVOID *Object);

/* \Driver\name, referenced (NT 4.0 and later: NT 3.x has no ObReferenceObjectByName) */
PDRIVER_OBJECT NtRefDriver(const WCHAR *Name)
{
    OB_REF_NAME ref = (OB_REF_NAME)NtKernelExport("ObReferenceObjectByName");
    POBJECT_TYPE *type = (POBJECT_TYPE *)NtKernelExport("IoDriverObjectType");
    UNICODE_STRING u;
    PVOID obj = NULL;

    if (ref == NULL || type == NULL || *type == NULL) {
        return NULL;
    }
    RtlInitUnicodeString(&u, Name);
    if (!NT_SUCCESS(ref(&u, OBJ_CASE_INSENSITIVE, NULL, 0, *type, KernelMode, NULL, &obj))) {
        return NULL;
    }
    return (PDRIVER_OBJECT)obj;
}

static BOOLEAN KernelPtr(const void *P, ULONG Size)
{
    return (BOOLEAN)(P != NULL && (LONG_PTR)P < 0 && ((ULONG_PTR)P & (sizeof(PVOID) - 1)) == 0 &&
                     MmIsAddressValid((PVOID)P) && MmIsAddressValid((PUCHAR)P + Size - 1));
}

/* the loaded image (MZ / PE header) that contains A, and its size */
static PUCHAR ImageOf(const void *A, ULONG *Size)
{
    PUCHAR p = (PUCHAR)((ULONG_PTR)A & ~(ULONG_PTR)0xFFF);
    int i;

    for (i = 0; i < 512 && (LONG_PTR)p < 0 && MmIsAddressValid(p); i++, p -= 0x1000) {
        ULONG e = *(ULONG *)(p + 0x3C);
        if (p[0] == 'M' && p[1] == 'Z' && e >= 0x40 && e < 0x1000 - 0x58 && *(ULONG *)(p + e) == 0x00004550) {
            *Size = *(ULONG *)(p + e + 0x50);   /* OptionalHeader.SizeOfImage, PE32 and PE32+ */
            return p;
        }
    }
    return NULL;
}

/* Class, Service: a CONNECT_DATA a keyboard class driver gave port device Port */
static BOOLEAN ClassConnect(PDEVICE_OBJECT Port, PDEVICE_OBJECT Class, PVOID Service)
{
    PDRIVER_OBJECT drv;
    PUCHAR base, rd;
    ULONG size = 0;

    if (Class == Port || !KernelPtr(Class, sizeof(DEVICE_OBJECT)) || Class->Type != IO_TYPE_DEVICE ||
        Class->DeviceType != FILE_DEVICE_KEYBOARD) {
        return FALSE;
    }
    drv = Class->DriverObject;
    if (!KernelPtr(drv, sizeof(DRIVER_OBJECT)) || drv->Type != IO_TYPE_DRIVER || drv == Port->DriverObject ||
        drv == InputDriver || (LONG_PTR)Service >= 0 || !MmIsAddressValid(Service)) {
        return FALSE;
    }
    base = ImageOf(Service, &size);
    rd = (PUCHAR)drv->MajorFunction[IRP_MJ_READ];
    return (BOOLEAN)(base != NULL && rd >= base && rd < base + size && (PUCHAR)Service < base + size);
}

static BOOLEAN ScanPort(PDEVICE_OBJECT D, const char *What)
{
    PUCHAR ext, end;
    PVOID *p;

    if (D == NULL || D->DriverObject == InputDriver || D->DeviceExtension == NULL ||
        D->Size <= sizeof(DEVICE_OBJECT)) {
        return FALSE;
    }
    ext = (PUCHAR)D->DeviceExtension;
    end = (PUCHAR)D + D->Size;                  /* Size: the device object and its extension */
    if (ext + (D->Size - sizeof(DEVICE_OBJECT)) < end) {
        end = ext + (D->Size - sizeof(DEVICE_OBJECT));
    }
    if (ext < (PUCHAR)D || end > ext + 0x2000) {
        return FALSE;
    }
    for (p = (PVOID *)ext; (PUCHAR)(p + 2) <= end; p++) {
        if (ClassConnect(D, (PDEVICE_OBJECT)p[0], p[1])) {
            KIRQL old;
            KeAcquireSpinLock(&InputLock, &old);
            Lent.ClassDevice = (PDEVICE_OBJECT)p[0];
            Lent.Service = (CLASS_SERVICE)p[1];
            Lent.Enabled = 1;
            KeReleaseSpinLock(&InputLock, old);
            UsbLog(LOG_INFO, "keyboard: no class driver on our port, USB keys go to the class driver of %s%s\n",
                   What, InTextSetup ? " (text-mode setup)" : "");
            return TRUE;
        }
    }
    return FALSE;
}

static BOOLEAN FindLent(void)
{
    static const WCHAR *drivers[] = { L"\\Driver\\i8042prt", L"\\Driver\\kbdhid", NULL };
    static const char *labels[] = { "i8042prt", "kbdhid" };
    static char port[] = "\\Device\\KeyboardPort0";
    PDEVICE_OBJECT d;
    WCHAR name[40];
    int i;

    for (i = 0; drivers[i] != NULL; i++) {
        PDRIVER_OBJECT drv = NtRefDriver(drivers[i]);
        BOOLEAN found = FALSE;
        if (drv == NULL) {
            continue;
        }
        for (d = drv->DeviceObject; d != NULL && !found; d = d->NextDevice) {
            found = ScanPort(d, labels[i]);
        }
        ObDereferenceObject(drv);
        if (found) {
            return TRUE;
        }
    }
    /* NT 3.x: the keyboard ports by name */
    for (i = 0; i < 8; i++) {
        UNICODE_STRING u;
        PFILE_OBJECT fo = NULL;
        BOOLEAN found = FALSE;
        MakeName(name, L"\\Device\\KeyboardPort", (ULONG)i);
        RtlInitUnicodeString(&u, name);
        if (!NT_SUCCESS(IoGetDeviceObjectPointer(&u, FILE_READ_ATTRIBUTES, &fo, &d))) {
            continue;
        }
        port[sizeof(port) - 2] = (char)('0' + i);
        if (d->DriverObject != InputDriver) {
            PDEVICE_OBJECT e;
            for (e = d->DriverObject->DeviceObject; e != NULL && !found; e = e->NextDevice) {
                found = ScanPort(e, port);
            }
        }
        ObDereferenceObject(fo);
        if (found) {
            return TRUE;
        }
    }
    return FALSE;
}

/* worker (PASSIVE_LEVEL): a USB key came and no class driver took it */
void NtInputService(void)
{
    ULONG now = OsTimeMs();

    if (!LentWanted || Lent.Service != NULL) {
        return;
    }
    if (!SetupChecked) {
        PDRIVER_OBJECT s = NtRefDriver(L"\\Driver\\setupdd");
        SetupChecked = TRUE;
        if (s != NULL) {
            InTextSetup = TRUE;
            ObDereferenceObject(s);
        }
    }
    if (Kbd.Service != NULL && !InTextSetup) {
        LentWanted = 0;                         /* kbdclass is on our port: the normal case */
        return;
    }
    if (LentTries != 0 && now - LentLastTry < 1000) {
        return;
    }
    LentTries++;
    LentLastTry = now;
    if (!FindLent() && LentTries == LENT_TRIES) {
        UsbLog(LOG_INFO, "keyboard: no class driver for the USB keys found\n");
    }
    LentWanted = 0;
}

/* IOCTL_USBNT_WHEEL_WAIT: a LONG (vertical detents) or, with room for two, vertical and
   horizontal (positive = right).  Called with the cancel spin lock held; FALSE: nothing
   for this caller yet */
static BOOLEAN WheelTake(PIRP Irp)
{
    LONG *out = (LONG *)Irp->AssociatedIrp.SystemBuffer;
    BOOLEAN two = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength >= 2 * sizeof(LONG);
    if (WheelAccum == 0 && (!two || PanAccum == 0)) {
        return FALSE;
    }
    out[0] = WheelAccum;
    WheelAccum = 0;
    if (two) {
        out[1] = PanAccum;
        PanAccum = 0;
    }
    Irp->IoStatus.Information = two ? 2 * sizeof(LONG) : sizeof(LONG);
    return TRUE;
}

static void WheelChannel(LONG Delta, LONG Pan)
{
    KIRQL irql;
    PIRP irp = NULL;

    IoAcquireCancelSpinLock(&irql);
    if (WheelOpen <= 0) {
        IoReleaseCancelSpinLock(irql);
        return;
    }
    WheelAccum += Delta;
    PanAccum += Pan;
    if (PanAccum > 50) PanAccum = 50;               /* nobody reads it (usbmon not running): stays small */
    if (PanAccum < -50) PanAccum = -50;
    if (WheelIrp != NULL && WheelTake(WheelIrp)) {
        irp = WheelIrp;
        WheelIrp = NULL;
        IoSetCancelRoutine(irp, NULL);
    }
    IoReleaseCancelSpinLock(irql);
    if (irp != NULL) {
        irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(irp, IO_MOUSE_INCREMENT);
    }
}

static void MouCapReset(void)
{
    MouCap = 0;
    MouCapOwner = NULL;
    MouRouted = 0;
    MouQn = 0;
}

/* completes the waiting IOCTL_USBNT_MOUSE_WAIT with the queued events; called
   with the cancel spin lock held, returns the IRP to complete after releasing it */
static PIRP MouTake(void)
{
    PIRP irp = MouIrp;
    USBNT_MOUSE_EVENTS *o;

    if (irp == NULL || MouQn == 0) {
        return NULL;
    }
    MouIrp = NULL;
    IoSetCancelRoutine(irp, NULL);
    o = (USBNT_MOUSE_EVENTS *)irp->AssociatedIrp.SystemBuffer;
    RtlZeroMemory(o, sizeof(*o));
    o->Count = MouQn;
    o->Flags = MouCap;
    RtlMoveMemory(o->Ev, MouQ, MouQn * sizeof(USBNT_MOUSE_EVENT));
    MouQn = 0;
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = sizeof(USBNT_MOUSE_EVENTS);
    return irp;
}

/* takes the button flags usbmon asked for out of an event; returns them */
static USHORT MouRoute(USHORT Flags)
{
    static const USHORT down[5] = { UMOU_LEFT_DOWN, UMOU_RIGHT_DOWN, UMOU_MIDDLE_DOWN, UMOU_B4_DOWN, UMOU_B5_DOWN };
    KIRQL irql;
    PIRP irp;
    USHORT taken = 0;
    USHORT grab = 0;
    ULONG now;
    int i;

    if (!(Flags & 0x03FF) || (MouCap == 0 && MouRouted == 0)) {
        return 0;
    }
    now = OsTimeMs();
    IoAcquireCancelSpinLock(&irql);
    if (MouQn != 0 && MouIrp == NULL && (now - MouQTime > 1000 || MouQn == USBNT_MOUSE_MAX_EVENTS)) {
        /* usbmon does not fetch its events (hangs): the buttons are the system's again */
        UsbLog(LOG_ERR, "mouse: usbmon does not answer, button capture ended\n");
        MouCapReset();
    }
    for (i = 0; i < 5; i++) {
        USHORT d = down[i];
        USHORT u = (USHORT)(d << 1);
        if (Flags & d) {
            BOOLEAN take = (BOOLEAN)((MouCap & USBNT_CAP_GRAB) || (i == 2 && (MouCap & USBNT_CAP_MIDDLE)) ||
                                     (i >= 3 && (MouCap & USBNT_CAP_XBUTTONS)));
            if (take) {
                if (MouCap & USBNT_CAP_GRAB) {
                    grab = 1;
                }
                if (i == 2 && (MouCap & USBNT_CAP_MIDDLE)) {
                    MouCap |= USBNT_CAP_GRAB;           /* autoscroll starts: all buttons until usbmon ends it */
                }
                MouRouted |= d;
                taken |= d;
            } else {
                MouRouted &= ~d;
            }
        }
        if ((Flags & u) && (MouRouted & d)) {
            MouRouted &= ~d;
            taken |= u;
        }
    }
    if (taken != 0) {
        if (MouQn == 0) {
            MouQTime = now;
        }
        MouQ[MouQn].Buttons = taken;
        MouQ[MouQn].Grab = grab;
        MouQ[MouQn].TimeMs = now;
        MouQn++;
    }
    irp = MouTake();
    IoReleaseCancelSpinLock(irql);
    if (irp != NULL) {
        IoCompleteRequest(irp, IO_MOUSE_INCREMENT);
    }
    return taken;
}

/* a horizontal wheel movement only (the Windows 2000 / XP PS/2 filter): to usbmon */
void OsMousePan(LONG Pan)
{
    if (Pan != 0) {
        WheelChannel(0, Pan);
    }
}

void OsMouseInput(const USB_MOUSE_EVENT *E)
{
    MOUSE_INPUT_DATA d;
    USHORT flags = E->ButtonFlags;
    BOOLEAN channel = FALSE;

    flags &= ~MouRoute(flags);
    RtlZeroMemory(&d, sizeof(d));
    if (flags & UMOU_WHEEL) {
        if (WheelMode == WHEEL_CHANNEL) {
            flags &= ~UMOU_WHEEL;
            channel = TRUE;
        } else if (WheelMode == WHEEL_AUTO) {
            channel = TRUE;
        }
        if (flags & UMOU_WHEEL) {
            d.ButtonData = (USHORT)(SHORT)(E->Wheel * USB_WHEEL_DELTA);
        }
    }
    d.UnitId = 0;
    d.Flags = E->Absolute ? MOUSE_MOVE_ABSOLUTE : MOUSE_MOVE_RELATIVE;
    d.ButtonFlags = flags;
    d.LastX = E->X;
    d.LastY = E->Y;
    if (flags != 0 || E->X != 0 || E->Y != 0 || E->Absolute) {
        Deliver(&Mou, &d, &d + 1);
    }
    if ((channel && E->Wheel != 0) || E->Pan != 0) {
        WheelChannel(channel ? E->Wheel : 0, E->Pan);
    }
}

/* ------------------------------------------------------------------ */
/* Keyboard / pointer IOCTLs (internal and regular)                     */
/* ------------------------------------------------------------------ */

static NTSTATUS Connect(PORT_STATE *P, PIO_STACK_LOCATION sp)
{
    PCONNECT_DATA cd = (PCONNECT_DATA)sp->Parameters.DeviceIoControl.Type3InputBuffer;
    KIRQL old;

    if (sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA) || cd == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
#ifdef USB_WDM
    /* a new stack of the device node (Plug and Play rebuilt it): its class driver takes over */
    if (P->Service != NULL && P->Owner == sp->DeviceObject) {
        return STATUS_SHARING_VIOLATION;
    }
#else
    if (P->Service != NULL) {
        return STATUS_SHARING_VIOLATION;
    }
#endif
    KeAcquireSpinLock(&InputLock, &old);
    P->ClassDevice = cd->ClassDeviceObject;
    P->Service = (CLASS_SERVICE)cd->ClassService;
#ifdef USB_WDM
    if (P->Owner != sp->DeviceObject) {
        P->Enabled = 0;                         /* opens of the old stack's class device do not count */
    }
    P->Owner = sp->DeviceObject;
#endif
    KeReleaseSpinLock(&InputLock, old);
    UsbLog(LOG_INFO, "%s class driver connected\n", (P == &Kbd) ? "keyboard" : "pointer");
    return STATUS_SUCCESS;
}

static NTSTATUS KbdIoctl(PIRP Irp, PIO_STACK_LOCATION sp)
{
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;

    switch (code) {
    case IOCTL_INTERNAL_KEYBOARD_CONNECT:
        return Connect(&Kbd, sp);
    case IOCTL_INTERNAL_KEYBOARD_DISCONNECT:
#ifdef USB_WDM
        NtInputDisconnect(EXT_KBD, sp->DeviceObject);
        return STATUS_SUCCESS;
#else
        return STATUS_NOT_IMPLEMENTED;
#endif
    case IOCTL_INTERNAL_KEYBOARD_ENABLE:
        InterlockedIncrement(&Kbd.Enabled);
        return STATUS_SUCCESS;
    case IOCTL_INTERNAL_KEYBOARD_DISABLE:
        if (Kbd.Enabled > 0) {
            InterlockedDecrement(&Kbd.Enabled);
        }
        return STATUS_SUCCESS;
    case IOCTL_KEYBOARD_QUERY_ATTRIBUTES: {
        PKEYBOARD_ATTRIBUTES a = (PKEYBOARD_ATTRIBUTES)buf;
        if (outLen < sizeof(KEYBOARD_ATTRIBUTES)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        RtlZeroMemory(a, sizeof(*a));
        a->KeyboardIdentifier.Type = 4;
        a->KeyboardIdentifier.Subtype = 0;
        a->KeyboardMode = 1;
        a->NumberOfFunctionKeys = 12;
        a->NumberOfIndicators = 3;
        a->NumberOfKeysTotal = 101;
        a->InputDataQueueLength = 100;
        a->KeyRepeatMinimum.UnitId = 0;
        a->KeyRepeatMinimum.Rate = 2;
        a->KeyRepeatMinimum.Delay = 250;
        a->KeyRepeatMaximum.UnitId = 0;
        a->KeyRepeatMaximum.Rate = 30;
        a->KeyRepeatMaximum.Delay = 1000;
        Irp->IoStatus.Information = sizeof(KEYBOARD_ATTRIBUTES);
        return STATUS_SUCCESS;
    }
    case IOCTL_KEYBOARD_QUERY_TYPEMATIC:
        if (outLen < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PKEYBOARD_TYPEMATIC_PARAMETERS)buf = Typematic;
        Irp->IoStatus.Information = sizeof(KEYBOARD_TYPEMATIC_PARAMETERS);
        return STATUS_SUCCESS;
    case IOCTL_KEYBOARD_SET_TYPEMATIC:
        if (inLen < sizeof(KEYBOARD_TYPEMATIC_PARAMETERS)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        Typematic = *(PKEYBOARD_TYPEMATIC_PARAMETERS)buf;
        HidSetTypematic(Typematic.Delay, Typematic.Rate);
        NtPs2SetTypematic(Typematic.Delay, Typematic.Rate);
        return STATUS_SUCCESS;
    case IOCTL_KEYBOARD_QUERY_INDICATORS:
        if (outLen < sizeof(KEYBOARD_INDICATOR_PARAMETERS)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PKEYBOARD_INDICATOR_PARAMETERS)buf = Indicators;
        Irp->IoStatus.Information = sizeof(KEYBOARD_INDICATOR_PARAMETERS);
        return STATUS_SUCCESS;
    case IOCTL_KEYBOARD_SET_INDICATORS:
        if (inLen < sizeof(KEYBOARD_INDICATOR_PARAMETERS)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        Indicators = *(PKEYBOARD_INDICATOR_PARAMETERS)buf;
        HidSetLeds(Indicators.LedFlags);
        NtPs2SetLeds(Indicators.LedFlags);
        return STATUS_SUCCESS;
    case IOCTL_KEYBOARD_QUERY_INDICATOR_TRANSLATION: {
        ULONG need = FIELD_OFFSET(KEYBOARD_INDICATOR_TRANSLATION, IndicatorList) + 3 * sizeof(INDICATOR_LIST);
        PKEYBOARD_INDICATOR_TRANSLATION t = (PKEYBOARD_INDICATOR_TRANSLATION)buf;
        if (outLen < need) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        t->NumberOfIndicatorKeys = 3;
        t->IndicatorList[0].MakeCode = 0x3A;
        t->IndicatorList[0].IndicatorFlags = KEYBOARD_CAPS_LOCK_ON;
        t->IndicatorList[1].MakeCode = 0x45;
        t->IndicatorList[1].IndicatorFlags = KEYBOARD_NUM_LOCK_ON;
        t->IndicatorList[2].MakeCode = 0x46;
        t->IndicatorList[2].IndicatorFlags = KEYBOARD_SCROLL_LOCK_ON;
        Irp->IoStatus.Information = need;
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

static NTSTATUS MouIoctl(PIRP Irp, PIO_STACK_LOCATION sp)
{
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;

    switch (code) {
    case IOCTL_INTERNAL_MOUSE_CONNECT:
        return Connect(&Mou, sp);
    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
#ifdef USB_WDM
        NtInputDisconnect(EXT_MOUSE, sp->DeviceObject);
        return STATUS_SUCCESS;
#else
        return STATUS_NOT_IMPLEMENTED;
#endif
    case IOCTL_INTERNAL_MOUSE_ENABLE:
        InterlockedIncrement(&Mou.Enabled);
        return STATUS_SUCCESS;
    case IOCTL_INTERNAL_MOUSE_DISABLE:
        if (Mou.Enabled > 0) {
            InterlockedDecrement(&Mou.Enabled);
        }
        return STATUS_SUCCESS;
    case IOCTL_MOUSE_QUERY_ATTRIBUTES: {
        PMOUSE_ATTRIBUTES a = (PMOUSE_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer;
        if (outLen < sizeof(MOUSE_ATTRIBUTES)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        a->MouseIdentifier = (USHORT)(((HidMouseHasWheel() || NtPs2MouseWheel()) && WheelMode != WHEEL_CHANNEL) ?
                                      WHEELMOUSE_I8042_HARDWARE : MOUSE_I8042_HARDWARE);
        a->NumberOfButtons = (USHORT)(NtPs2MouseButtons() > 3 || HidMouseButtons() > 3 ? 5 : 3);
        a->SampleRate = 100;
        a->InputDataQueueLength = 100 * sizeof(MOUSE_INPUT_DATA);
        Irp->IoStatus.Information = sizeof(MOUSE_ATTRIBUTES);
        return STATUS_SUCCESS;
    }
    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

NTSTATUS NtInputIoctl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NT_EXT *ext = (NT_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    Irp->IoStatus.Information = 0;
    UsbLog(LOG_DBG, "%s port ioctl %x\n", ext->Kind == EXT_KBD ? "keyboard" : "pointer",
           sp->Parameters.DeviceIoControl.IoControlCode);
    st = (ext->Kind == EXT_KBD) ? KbdIoctl(Irp, sp) : MouIoctl(Irp, sp);
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

/* ------------------------------------------------------------------ */
/* Control device                                                       */
/* ------------------------------------------------------------------ */

static VOID NTAPI WheelCancel(PDEVICE_OBJECT Dev, PIRP Irp)
{
    (void)Dev;
    if (WheelIrp == Irp) {
        WheelIrp = NULL;
    }
    if (MouIrp == Irp) {
        MouIrp = NULL;
    }
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* Pending IOCTL_USBNT_WAIT_CHANGE requests */
#define MAX_CHANGE_WAITERS 8
static PIRP ChangeIrp[MAX_CHANGE_WAITERS];

static VOID NTAPI ChangeCancel(PDEVICE_OBJECT Dev, PIRP Irp)
{
    int i;

    (void)Dev;
    for (i = 0; i < MAX_CHANGE_WAITERS; i++) {
        if (ChangeIrp[i] == Irp) {
            ChangeIrp[i] = NULL;
        }
    }
    IoReleaseCancelSpinLock(Irp->CancelIrql);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

void OsNotifyChange(void)
{
    PIRP done[MAX_CHANGE_WAITERS];
    KIRQL irql;
    int n = 0;
    int i;

    IoAcquireCancelSpinLock(&irql);
    for (i = 0; i < MAX_CHANGE_WAITERS; i++) {
        if (ChangeIrp[i] != NULL) {
            IoSetCancelRoutine(ChangeIrp[i], NULL);
            done[n++] = ChangeIrp[i];
            ChangeIrp[i] = NULL;
        }
    }
    IoReleaseCancelSpinLock(irql);
    for (i = 0; i < n; i++) {
        *(ULONG *)done[i]->AssociatedIrp.SystemBuffer = UsbChangeCount;
        done[i]->IoStatus.Information = sizeof(ULONG);
        done[i]->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(done[i], IO_NO_INCREMENT);
    }
}

static void CopyStr(char *Dst, const char *Src, ULONG Size)
{
    ULONG i;

    for (i = 0; i + 1 < Size && Src[i] != 0; i++) {
        Dst[i] = Src[i];
    }
    Dst[i] = 0;
}

static void FillTree(USBNT_TREE *t)
{
    ULONG i;
    int p;

    RtlZeroMemory(t, sizeof(*t));
    t->Version = USBNT_TREE_VERSION;
    t->ChangeCount = UsbChangeCount;
    t->ScsiPort = NtScsiPortNumber;
    t->HcCount = (ULONG)UsbHcCount;
    for (i = 0; i < (ULONG)UsbHcCount && i < USBNT_MAX_HCS; i++) {
        USB_HC *hc = UsbHcs[i];
        USBNT_HC_INFO *h = &t->Hc[i];
        h->Type = hc->Type;
        h->Bus = hc->Bus;
        h->Device = hc->Dev;
        h->Function = hc->Fn;
        h->VendorId = hc->VendorId;
        h->DeviceId = hc->DeviceId;
        h->NumPorts = hc->NumPorts;
        h->Running = (UCHAR)(hc->Removed ? 2 : (hc->Running && hc->Hot) ? 3 : hc->Running);
        h->Irq = hc->IrqLine;
        h->Companion = (UCHAR)(hc->Ehci != NULL ? hc->Ehci->Index : 0xFF);
        if (hc->Type == HC_XHCI && hc->Running) {
            for (p = 1; p <= hc->NumPorts && p <= USBNT_MAX_PORTS; p++) {
                h->PortProto[p - 1] = (UCHAR)((hc->Ops->PortStatus(hc, p) & PS_SS_PORT) ? 3 : 2);
            }
        }
    }
    for (i = 0; i < USBNT_MAX_DEVS && i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        USBNT_DEV_INFO *o = &t->Dev[i];
        if (!d->InUse || d->Gone) {
            o->Index = 0xFF;
            continue;
        }
        o->Index = (UCHAR)i;
        o->Hc = d->Hc->Index;
        o->Parent = (UCHAR)(d->Parent != NULL ? d->Parent->Index : 0xFF);
        o->Port = d->Port;
        o->RootPort = d->RootPort;
        o->Depth = d->Depth;
        o->Speed = d->Speed;
        o->Address = d->Address;
        o->UsbVersion = GET16(d->DevDesc + 2);
        o->VendorId = GET16(d->DevDesc + 8);
        o->ProductId = GET16(d->DevDesc + 10);
        o->DeviceVersion = GET16(d->DevDesc + 12);
        o->Class = d->DevDesc[4];
        o->SubClass = d->DevDesc[5];
        o->Protocol = d->DevDesc[6];
        o->Functions = d->Function;
        o->LinkGbps = d->LinkGbps;
        o->HubPorts = d->HubPorts;
        if (d->Config != NULL && d->ConfigLen >= 9) {
            o->Interfaces = d->Config[4];
            o->MaxPower = d->Config[8];
        }
        CopyStr(o->Manufacturer, d->Mfg, sizeof(o->Manufacturer));
        CopyStr(o->Product, d->Product, sizeof(o->Product));
        CopyStr(o->Serial, d->Serial, sizeof(o->Serial));
    }
    t->SlotCount = UsbScsiSlotCount;
    for (i = 0; i < UsbScsiSlotCount && i < USBNT_MAX_SLOTS; i++) {
        SCSI_SLOT_INFO si;
        USBNT_SLOT_INFO *o = &t->Slot[i];
        UsbScsiGetSlot((int)i, &si);
        o->Present = si.Present;
        o->DeviceType = si.DeviceType;
        o->DevIndex = si.Present ? si.DevIndex : 0xFF;
        o->Lun = si.Lun;
        o->Protocol = si.Protocol;
        o->MediaReady = si.MediaReady;
        o->WriteProtect = si.WriteProtect;
        o->Partitions = (UCHAR)((si.Parts > 127 ? 127 : si.Parts) | (si.Fixed ? 0x80 : 0));
        o->LastLba = si.LastLba;
        o->BlockSize = si.BlockSize;
        o->MaxTransfer = si.MaxXfer;
        RtlMoveMemory(o->Vendor, si.Inquiry + 8, 8);
        RtlMoveMemory(o->Product, si.Inquiry + 16, 16);
        RtlMoveMemory(o->Revision, si.Inquiry + 32, 4);
        o->Identity = si.Identity;
    }
}

static void FillExtra(USBNT_TREE2 *t)
{
    ULONG n = 0;
    int i;

    RtlZeroMemory(&t->ExtraCount, sizeof(*t) - sizeof(t->Base));
    t->Base.Version = USBNT_TREE_VERSION2;
    for (i = 0; i < (int)UsbScsiSlotCount && n < USBNT_MAX_EXTRA; i++) {
        USBNT_EXTRA_INFO *e = &t->Extra[n];
        UCHAR kind = 0;
        if (UsbScsiSlotHost(i, e->Name, sizeof(e->Name), &kind)) {
            e->Kind = kind == BLK_KIND_NVME ? USBNT_EXTRA_NVME : USBNT_EXTRA_READER;
            e->Slot = (UCHAR)i;
            n++;
        }
    }
    for (i = 0; n < USBNT_MAX_EXTRA; i++) {
        USBNT_EXTRA_INFO *e = &t->Extra[n];
        if (!TbtInfo(i, e->Name, sizeof(e->Name), e->Detail, sizeof(e->Detail))) {
            break;
        }
        e->Kind = USBNT_EXTRA_TBT;
        n++;
    }
    for (i = 0; n < USBNT_MAX_EXTRA; i++) {
        USBNT_EXTRA_INFO *e = &t->Extra[n];
        if (!I2cHidInfo(i, &e->Kind, e->Name, sizeof(e->Name), e->Detail, sizeof(e->Detail))) {
            break;
        }
        n++;
    }
    /* SMBus host controllers and touch pads: listed with the I2C ones */
    for (i = 0; n < USBNT_MAX_EXTRA; i++) {
        USBNT_EXTRA_INFO *e = &t->Extra[n];
        if (!SmbusInfo(i, &e->Kind, e->Name, sizeof(e->Name), e->Detail, sizeof(e->Detail))) {
            break;
        }
        n++;
    }
    t->ExtraCount = n;
}

void NtCtlCreateClose(PDEVICE_OBJECT Dev, PIRP Irp, BOOLEAN Create)
{
    KIRQL irql;

    (void)Dev;
    (void)Irp;
    IoAcquireCancelSpinLock(&irql);
    if (Create) {
        WheelOpen++;
    } else if (WheelOpen > 0) {
        WheelOpen--;
        if (WheelOpen == 0) {
            WheelAccum = 0;
            PanAccum = 0;
        }
    }
    IoReleaseCancelSpinLock(irql);
}

void NtCtlCleanup(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    KIRQL irql;
    PIRP pend = NULL;

    PIRP chg[MAX_CHANGE_WAITERS + 1];
    int n = 0;
    int i;

    (void)Dev;
    IoAcquireCancelSpinLock(&irql);
    if (WheelIrp != NULL && IoGetCurrentIrpStackLocation(WheelIrp)->FileObject == sp->FileObject) {
        pend = WheelIrp;
        WheelIrp = NULL;
        IoSetCancelRoutine(pend, NULL);
    }
    if (MouIrp != NULL && IoGetCurrentIrpStackLocation(MouIrp)->FileObject == sp->FileObject) {
        IoSetCancelRoutine(MouIrp, NULL);
        chg[n++] = MouIrp;
        MouIrp = NULL;
    }
    if (MouCapOwner != NULL && MouCapOwner == sp->FileObject) {
        MouCapReset();                      /* usbmon ended (or crashed): the buttons are the system's */
    }
    for (i = 0; i < MAX_CHANGE_WAITERS; i++) {
        if (ChangeIrp[i] != NULL && IoGetCurrentIrpStackLocation(ChangeIrp[i])->FileObject == sp->FileObject) {
            IoSetCancelRoutine(ChangeIrp[i], NULL);
            chg[n++] = ChangeIrp[i];
            ChangeIrp[i] = NULL;
        }
    }
    IoReleaseCancelSpinLock(irql);
    if (pend != NULL) {
        pend->IoStatus.Status = STATUS_CANCELLED;
        pend->IoStatus.Information = 0;
        IoCompleteRequest(pend, IO_NO_INCREMENT);
    }
    for (i = 0; i < n; i++) {
        chg[i]->IoStatus.Status = STATUS_CANCELLED;
        chg[i]->IoStatus.Information = 0;
        IoCompleteRequest(chg[i], IO_NO_INCREMENT);
    }
}

NTSTATUS NtCtlIoctl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st;
    KIRQL irql;

    (void)Dev;
    Irp->IoStatus.Information = 0;
    switch (code) {
    case IOCTL_USBNT_WHEEL_WAIT:
        if (outLen < sizeof(LONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        IoAcquireCancelSpinLock(&irql);
        if (WheelTake(Irp)) {
            IoReleaseCancelSpinLock(irql);
            st = STATUS_SUCCESS;
            break;
        }
        if (WheelIrp != NULL || Irp->Cancel) {
            IoReleaseCancelSpinLock(irql);
            st = Irp->Cancel ? STATUS_CANCELLED : STATUS_DEVICE_BUSY;
            break;
        }
        IoMarkIrpPending(Irp);
        IoSetCancelRoutine(Irp, WheelCancel);
        WheelIrp = Irp;
        IoReleaseCancelSpinLock(irql);
        return STATUS_PENDING;
    case IOCTL_USBNT_MOUSE_WAIT: {
        PIRP irp;
        if (outLen < sizeof(USBNT_MOUSE_EVENTS)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        IoAcquireCancelSpinLock(&irql);
        if (MouIrp != NULL || Irp->Cancel) {
            IoReleaseCancelSpinLock(irql);
            st = Irp->Cancel ? STATUS_CANCELLED : STATUS_DEVICE_BUSY;
            break;
        }
        IoMarkIrpPending(Irp);
        IoSetCancelRoutine(Irp, WheelCancel);
        MouIrp = Irp;
        irp = MouTake();                    /* events already queued: at once */
        IoReleaseCancelSpinLock(irql);
        if (irp != NULL) {
            IoCompleteRequest(irp, IO_MOUSE_INCREMENT);
        }
        return STATUS_PENDING;
    }
    case IOCTL_USBNT_MOUSE_CAPTURE: {
        ULONG want;
        if (inLen < sizeof(ULONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        want = *(ULONG *)Irp->AssociatedIrp.SystemBuffer & (USBNT_CAP_XBUTTONS | USBNT_CAP_MIDDLE | USBNT_CAP_GRAB);
        IoAcquireCancelSpinLock(&irql);
        if (MouCap != want) {
            UsbLog(LOG_DBG, "mouse: usbmon takes buttons %x\n", want);
        }
        MouCap = want;
        MouCapOwner = want != 0 ? sp->FileObject : NULL;
        if (want == 0) {
            MouQn = 0;
        }
        IoReleaseCancelSpinLock(irql);
        if (outLen >= sizeof(ULONG)) {
            *(ULONG *)Irp->AssociatedIrp.SystemBuffer = want;
            Irp->IoStatus.Information = sizeof(ULONG);
        }
        st = STATUS_SUCCESS;
        break;
    }
    case IOCTL_USBNT_QUERY_TREE:
        if (outLen < sizeof(USBNT_TREE)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        FillTree((USBNT_TREE *)Irp->AssociatedIrp.SystemBuffer);
        Irp->IoStatus.Information = sizeof(USBNT_TREE);
        if (outLen >= sizeof(USBNT_TREE2)) {
            FillExtra((USBNT_TREE2 *)Irp->AssociatedIrp.SystemBuffer);
            Irp->IoStatus.Information = sizeof(USBNT_TREE2);
        }
        st = STATUS_SUCCESS;
        break;
    case IOCTL_USBNT_VIDEO_LIST:
        if (outLen < sizeof(USBNT_CAMERAS)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        UvcList((USBNT_CAMERAS *)Irp->AssociatedIrp.SystemBuffer);
        Irp->IoStatus.Information = sizeof(USBNT_CAMERAS);
        st = STATUS_SUCCESS;
        break;
    case IOCTL_USBNT_VIDEO_START: {
        USBNT_VID_START in;
        if (inLen < sizeof(in) || outLen < sizeof(USBNT_VID_STATUS)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        in = *(USBNT_VID_START *)Irp->AssociatedIrp.SystemBuffer;
        {
            /* a failure is reported in Result (USB_ERR_NOMEM: the picture size
               needs more memory than there is) */
            USBNT_VID_STATUS *o = (USBNT_VID_STATUS *)Irp->AssociatedIrp.SystemBuffer;
            int r = UvcStart(&in, o);
            if (r != USB_OK) {
                UsbMemSet(o, 0, sizeof(*o));
                o->Result = r;
            }
        }
        st = STATUS_SUCCESS;
        Irp->IoStatus.Information = sizeof(USBNT_VID_STATUS);
        break;
    }
    case IOCTL_USBNT_VIDEO_STOP:
        if (inLen < sizeof(ULONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        UvcStop(*(ULONG *)Irp->AssociatedIrp.SystemBuffer);
        st = STATUS_SUCCESS;
        break;
    case IOCTL_USBNT_VIDEO_CONTROL:
        if (inLen < sizeof(USBNT_VID_CONTROL) || outLen < sizeof(USBNT_VID_CONTROL)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        st = UvcControl((USBNT_VID_CONTROL *)Irp->AssociatedIrp.SystemBuffer) == USB_OK ? STATUS_SUCCESS
                                                                                        : STATUS_NOT_SUPPORTED;
        Irp->IoStatus.Information = sizeof(USBNT_VID_CONTROL);
        break;
    case IOCTL_USBNT_VIDEO_FRAME: {
        USBNT_VID_GET get;
        UCHAR *out;
        LONG n;
        if (inLen < sizeof(get) || Irp->MdlAddress == NULL || outLen < sizeof(USBNT_VID_FRAME_HDR)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        get = *(USBNT_VID_GET *)Irp->AssociatedIrp.SystemBuffer;
        if (get.TimeoutMs > 10000) {
            get.TimeoutMs = 10000;
        }
        out = (UCHAR *)MmGetSystemAddressForMdl(Irp->MdlAddress);
        if (out == NULL) {
            st = STATUS_INSUFFICIENT_RESOURCES;
            break;
        }
        n = UvcGetFrame(&get, out, outLen);
        if (n > 0) {
            Irp->IoStatus.Information = (ULONG)n;
            st = STATUS_SUCCESS;
        } else if (n == 0) {
            st = STATUS_TIMEOUT;
        } else {
            st = n == USB_ERR_NODEV ? STATUS_DEVICE_DOES_NOT_EXIST : STATUS_INVALID_DEVICE_STATE;
        }
        break;
    }
    case IOCTL_USBNT_GET_LOG:
        Irp->IoStatus.Information = OsLogRead((char *)Irp->AssociatedIrp.SystemBuffer, outLen);
        st = STATUS_SUCCESS;
        break;
    case IOCTL_USBNT_WAIT_CHANGE: {
        ULONG last;
        int i;
        if (inLen < sizeof(ULONG) || outLen < sizeof(ULONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        last = *(ULONG *)Irp->AssociatedIrp.SystemBuffer;
        IoAcquireCancelSpinLock(&irql);
        if (last != UsbChangeCount || Irp->Cancel) {
            IoReleaseCancelSpinLock(irql);
            *(ULONG *)Irp->AssociatedIrp.SystemBuffer = UsbChangeCount;
            Irp->IoStatus.Information = sizeof(ULONG);
            st = Irp->Cancel ? STATUS_CANCELLED : STATUS_SUCCESS;
            break;
        }
        for (i = 0; i < MAX_CHANGE_WAITERS && ChangeIrp[i] != NULL; i++) {
        }
        if (i == MAX_CHANGE_WAITERS) {
            IoReleaseCancelSpinLock(irql);
            st = STATUS_DEVICE_BUSY;
            break;
        }
        IoMarkIrpPending(Irp);
        IoSetCancelRoutine(Irp, ChangeCancel);
        ChangeIrp[i] = Irp;
        IoReleaseCancelSpinLock(irql);
        return STATUS_PENDING;
    }
    case IOCTL_USBNT_EJECT:
        if (inLen < sizeof(ULONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        st = NtCtlEject(*(ULONG *)Irp->AssociatedIrp.SystemBuffer);
        break;
    case IOCTL_USBNT_QUERY_SLOTS: {
        USBNT_SLOTS *s = (USBNT_SLOTS *)Irp->AssociatedIrp.SystemBuffer;
        ULONG i;
        if (outLen < sizeof(USBNT_SLOTS)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        RtlZeroMemory(s, sizeof(*s));
        s->PortNumber = NtScsiPortNumber;
        s->SlotCount = UsbScsiSlotCount;
        for (i = 0; i < UsbScsiSlotCount && i < USBNT_MAX_SLOTS; i++) {
            SCSI_SLOT_INFO si;
            UsbScsiGetSlot((int)i, &si);
            s->Slot[i].Present = si.Present;
            s->Slot[i].DeviceType = si.DeviceType;
            RtlMoveMemory(s->Slot[i].Vendor, si.Inquiry + 8, 8);
            RtlMoveMemory(s->Slot[i].Product, si.Inquiry + 16, 16);
        }
        Irp->IoStatus.Information = sizeof(USBNT_SLOTS);
        st = STATUS_SUCCESS;
        break;
    }
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
