/*
 * wdmmain.c - driver entry, Plug and Play, power, dispatch, worker thread
 * and poll timer for Windows 2000 and XP (x86 and x64).
 *
 * USBNT is the function driver of every PCI function its INF matches: USB
 * host controllers (xHCI, EHCI, UHCI, OHCI), SD card readers, Thunderbolt /
 * USB4 host interfaces, Intel LPSS I2C controllers.  START_DEVICE hands the
 * function to the portable core, STOP / REMOVE takes it away again.  The
 * core runs in one worker thread, so the Plug and Play requests are carried
 * out there.  What USB devices are behind the controllers appears on the
 * USBNT device bus (wdmbus.c): keyboard, mouse, disks.
 */

#include "wdm.h"
#include "usbaudio.h"
#include "smbtp.h"

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING RegistryPath);

PDRIVER_OBJECT WdmDriver;
static KTIMER PollTimer;
static KDPC PollDpc;
static volatile LONG Stopping;
static PDEVICE_OBJECT HcList[USB_MAX_HC];

/* requests for the worker */
typedef struct _WREQ {
    void      (*Fn)(void *);
    void       *Arg;
    KEVENT      Done;
} WREQ;

#define WREQ_MAX 16
static WREQ *WQueue[WREQ_MAX];
static KSPIN_LOCK WLock;
static BOOLEAN I2cStarted;
static ULONG FirstStart;

/* PS/2 stays with i8042prt on Windows 2000 / XP */
void    NtPs2SetLeds(ULONG NtIndicators) { (void)NtIndicators; }
void    NtPs2SetTypematic(ULONG DelayMs, ULONG RateCps) { (void)DelayMs; (void)RateCps; }
BOOLEAN NtPs2MouseWheel(void) { return FALSE; }
ULONG   NtPs2MouseButtons(void) { return 0; }

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

NTSTATUS WdmComplete(PIRP Irp, NTSTATUS St, ULONG_PTR Info)
{
    Irp->IoStatus.Status = St;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return St;
}

static NTSTATUS NTAPI SyncDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    (void)Dev;
    (void)Irp;
    KeSetEvent((PKEVENT)Ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* passes the IRP down and waits; the caller completes it */
NTSTATUS WdmForwardSync(PDEVICE_OBJECT Lower, PIRP Irp)
{
    KEVENT ev;
    NTSTATUS st;

    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SyncDone, &ev, TRUE, TRUE, TRUE);
    st = IoCallDriver(Lower, Irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
        st = Irp->IoStatus.Status;
    }
    return st;
}

static NTSTATUS PassDown(WDM_EXT *x, PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(x->Lower, Irp);
}

void WdmRunInWorker(void (*Fn)(void *), void *Arg)
{
    WREQ r;
    KIRQL irql;
    int i;

    while (NtWorkerThread == NULL) {
        OsSleepMs(10);                          /* DriverEntry has just started it */
    }
    if (OsInWorker()) {
        Fn(Arg);
        return;
    }
    r.Fn = Fn;
    r.Arg = Arg;
    KeInitializeEvent(&r.Done, NotificationEvent, FALSE);
    for (;;) {
        KeAcquireSpinLock(&WLock, &irql);
        for (i = 0; i < WREQ_MAX && WQueue[i] != NULL; i++) {
        }
        if (i < WREQ_MAX) {
            WQueue[i] = &r;
        }
        KeReleaseSpinLock(&WLock, irql);
        if (i < WREQ_MAX) {
            break;
        }
        OsSleepMs(10);
    }
    OsWakeWorker();
    KeWaitForSingleObject(&r.Done, Executive, KernelMode, FALSE, NULL);
}

static void RunQueued(void)
{
    WREQ *r;
    KIRQL irql;
    int i;

    for (;;) {
        r = NULL;
        KeAcquireSpinLock(&WLock, &irql);
        for (i = 0; i < WREQ_MAX; i++) {
            if (WQueue[i] != NULL) {
                r = WQueue[i];
                WQueue[i] = NULL;
                break;
            }
        }
        KeReleaseSpinLock(&WLock, irql);
        if (r == NULL) {
            return;
        }
        r->Fn(r->Arg);
        KeSetEvent(&r->Done, IO_NO_INCREMENT, FALSE);
    }
}

/* ------------------------------------------------------------------ */
/* PCI functions (host controllers and the rest)                        */
/* ------------------------------------------------------------------ */

typedef struct _HC_ARG {
    WDM_EXT    *X;
    BOOLEAN     Gone;
    BOOLEAN     Settle;
    int         Result;
} HC_ARG;

static void HcStartW(void *A)
{
    HC_ARG *a = (HC_ARG *)A;

    NtReloadConfig();
    a->Result = a->X->Bus == 0xFF ? 0 : UsbPnpStart(a->X->Bus, a->X->Dev, a->X->Fn);
    if (FirstStart == 0) {
        FirstStart = OsTimeMs() | 1;
    }
    if (a->Settle && a->Result == USB_OK && a->X->Bus != 0xFF) {
        /* the devices are there before Plug and Play asks for the bus relations (boot disk) */
        UsbPnpSettle(a->X->Bus, a->X->Dev, a->X->Fn, OsGetConfig("BootWaitMs", 1500));
        WdmBusRefresh();
    }
}

static void HcStopW(void *A)
{
    HC_ARG *a = (HC_ARG *)A;

    if (a->X->Bus == 0xFF) {
        return;                                 /* ACPI I2C controller */
    }
    UsbPnpStop(a->X->Bus, a->X->Dev, a->X->Fn, a->Gone);
}

static BOOLEAN PciAddress(PDEVICE_OBJECT Pdo, UCHAR *Bus, UCHAR *Dev, UCHAR *Fn)
{
    ULONG bus = 0, addr = 0, len;

    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo, DevicePropertyBusNumber, sizeof(bus), &bus, &len)) ||
        !NT_SUCCESS(IoGetDeviceProperty(Pdo, DevicePropertyAddress, sizeof(addr), &addr, &len))) {
        return FALSE;
    }
    *Bus = (UCHAR)bus;
    *Dev = (UCHAR)(addr >> 16);
    *Fn = (UCHAR)(addr & 0xFFFF);
    return TRUE;
}

static NTSTATUS HcPnp(WDM_EXT *x, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    HC_ARG a;
    NTSTATUS st;
    int i;

    a.X = x;
    a.Gone = FALSE;
    a.Settle = FALSE;
    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = WdmForwardSync(x->Lower, Irp);
        if (NT_SUCCESS(st) && x->Claimed && x->Started) {
            return WdmComplete(Irp, st, 0);     /* taken in text-mode setup (TextSetupHcs): running */
        }
        if (NT_SUCCESS(st)) {
            if (!PciAddress(x->Pdo, &x->Bus, &x->Dev, &x->Fn)) {
                /* an ACPI I2C controller: the HID over I2C part finds it in the ACPI namespace */
                UsbLog(LOG_INFO, "PnP: I2C controller (ACPI) started\n");
                x->Bus = 0xFF;
                if (FirstStart == 0) {
                    FirstStart = OsTimeMs() | 1;
                }
                st = STATUS_SUCCESS;
            } else {
                a.Settle = TRUE;
                WdmRunInWorker(HcStartW, &a);
                UsbLog(LOG_INFO, "PnP: %02x:%02x.%u started (%d)\n", x->Bus, x->Dev, x->Fn, a.Result);
                x->Started = TRUE;
                st = STATUS_SUCCESS;
                /* no USBNT\Bus device (text-mode setup made none): this controller is the device bus */
                WdmBusOnController(x);
                WdmTreeHcStarted(x);                /* Device Manager: which controller it is */
            }
        }
        return WdmComplete(Irp, st, 0);
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == BusRelations) {
            if (WdmIsBus(x->Nt.Self)) {
                st = WdmBusRelations(Irp);
                if (!NT_SUCCESS(st)) {
                    return WdmComplete(Irp, st, 0);
                }
            }
            st = WdmTreeHcRelations(x, Irp);       /* the root hub (Device Manager) */
            if (!NT_SUCCESS(st)) {
                return WdmComplete(Irp, st, 0);
            }
            if (Irp->IoStatus.Information != 0) {
                Irp->IoStatus.Status = STATUS_SUCCESS;
            }
        }
        return PassDown(x, Irp);
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(x, Irp);
    case IRP_MN_STOP_DEVICE:
        if (x->Started) {
            WdmRunInWorker(HcStopW, &a);
            x->Started = FALSE;
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(x, Irp);
    case IRP_MN_SURPRISE_REMOVAL:
        x->Gone = TRUE;
        if (x->Started) {
            a.Gone = TRUE;
            WdmRunInWorker(HcStopW, &a);
            x->Started = FALSE;
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        return PassDown(x, Irp);
    case IRP_MN_REMOVE_DEVICE:
        if (x->Started) {
            a.Gone = x->Gone;
            WdmRunInWorker(HcStopW, &a);
            x->Started = FALSE;
        }
        Irp->IoStatus.Status = STATUS_SUCCESS;
        st = PassDown(x, Irp);
        for (i = 0; i < USB_MAX_HC; i++) {
            if (HcList[i] == x->Nt.Self) {
                HcList[i] = NULL;
            }
        }
        WdmBusOff(x->Nt.Self);                  /* it was the device bus: the children go with it */
        WdmTreeHcGone(x);
        IoDetachDevice(x->Lower);
        IoDeleteDevice(x->Nt.Self);
        return st;
    default:
        return PassDown(x, Irp);
    }
}

/* wdmtree.c: the hubs and devices changed - the root hubs may have come or gone too */
void WdmInvalidateHcs(void)
{
    int i;

    for (i = 0; i < USB_MAX_HC; i++) {
        PDEVICE_OBJECT f = HcList[i];
        if (f != NULL && ((WDM_EXT *)f->DeviceExtension)->Started) {
            IoInvalidateDeviceRelations(((WDM_EXT *)f->DeviceExtension)->Pdo, BusRelations);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Power                                                                */
/* ------------------------------------------------------------------ */

static VOID NTAPI ResumeWork(PDEVICE_OBJECT Dev, PVOID Ctx)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    HC_ARG a;

    a.X = x;
    a.Gone = FALSE;
    a.Settle = FALSE;
    if (x->Sleeping && !x->Removed) {
        x->Sleeping = FALSE;
        WdmRunInWorker(HcStartW, &a);
        UsbLog(LOG_INFO, "PnP: %02x:%02x.%u running again after sleep (%d)\n", x->Bus, x->Dev, x->Fn, a.Result);
    }
    IoFreeWorkItem((PIO_WORKITEM)Ctx);
}

static NTSTATUS NTAPI PowerUpDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_WORKITEM wi;

    (void)Ctx;
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    if (x->Sleeping) {
        wi = IoAllocateWorkItem(Dev);
        if (wi != NULL) {
            IoQueueWorkItem(wi, ResumeWork, DelayedWorkQueue, wi);
        }
    }
    PoStartNextPowerIrp(Irp);
    return STATUS_SUCCESS;
}

static NTSTATUS FdoPower(WDM_EXT *x, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    if (x->Nt.Kind == EXT_HC && x->Started && sp->MinorFunction == IRP_MN_SET_POWER &&
        sp->Parameters.Power.Type == SystemPowerState) {
        SYSTEM_POWER_STATE s = sp->Parameters.Power.State.SystemState;
        if (s > PowerSystemWorking && !x->Sleeping) {
            HC_ARG a;
            a.X = x;
            a.Gone = FALSE;
            a.Settle = FALSE;
            x->Sleeping = TRUE;
            WdmRunInWorker(HcStopW, &a);
        } else if (s == PowerSystemWorking && x->Sleeping) {
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, PowerUpDone, NULL, TRUE, TRUE, TRUE);
            return PoCallDriver(x->Lower, Irp);
        }
    }
    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(x->Lower, Irp);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

static void InputStackLog(WDM_EXT *X, PDEVICE_OBJECT Dev);

static NTSTATUS NTAPI DispatchPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    switch (x->Nt.Kind) {
    case EXT_DFLT:
        return WdmFltPnp(Dev, Irp);
    case EXT_HC:
        return HcPnp(x, Irp);
    case EXT_BUS:
        return WdmBusPnp(Dev, Irp);
    case EXT_PDO: {
        UCHAR mn = sp->MinorFunction;
        st = WdmPdoPnp(Dev, Irp);
        UsbLog(LOG_DBG, "PnP: child %u pnp %x -> %x\n", x->Child, mn, st);
        return st;
    }
    case EXT_NODE:
        return WdmTreeNodePnp(Dev, Irp);
    case EXT_KBD:
    case EXT_MOUSE:
        /* function devices on our keyboard / mouse PDOs */
        if (sp->MinorFunction == IRP_MN_START_DEVICE) {
            st = WdmForwardSync(x->Lower, Irp);
            x->Started = NT_SUCCESS(st);
            InputStackLog(x, Dev);
            return WdmComplete(Irp, st, 0);
        }
        if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE) {
            NtInputDisconnect(x->Nt.Kind, Dev);
            Irp->IoStatus.Status = STATUS_SUCCESS;
            st = PassDown(x, Irp);
            IoDetachDevice(x->Lower);
            IoDeleteDevice(Dev);
            return st;
        }
        if (sp->MinorFunction == IRP_MN_SURPRISE_REMOVAL || sp->MinorFunction == IRP_MN_STOP_DEVICE ||
            sp->MinorFunction == IRP_MN_QUERY_REMOVE_DEVICE || sp->MinorFunction == IRP_MN_QUERY_STOP_DEVICE ||
            sp->MinorFunction == IRP_MN_CANCEL_REMOVE_DEVICE || sp->MinorFunction == IRP_MN_CANCEL_STOP_DEVICE) {
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        return PassDown(x, Irp);
    default:
        return WdmComplete(Irp, Irp->IoStatus.Status, Irp->IoStatus.Information);
    }
}

/* debug: the class and the drivers above our keyboard / mouse function device */
static void InputStackLog(WDM_EXT *X, PDEVICE_OBJECT Dev)
{
    WCHAR guid[40];
    ULONG len = 0;
    PDEVICE_OBJECT a;
    UCHAR buf[160];
    POBJECT_NAME_INFORMATION ni = (POBJECT_NAME_INFORMATION)buf;
    char name[64];
    int n = 0;

    if (UsbDebugLevel < LOG_DBG || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlZeroMemory(guid, sizeof(guid));
    IoGetDeviceProperty(X->Pdo, DevicePropertyClassGuid, sizeof(guid) - sizeof(WCHAR), guid, &len);
    for (len = 0; guid[len] != 0 && len < 39; len++) {
        name[len] = (char)guid[len];
    }
    name[len] = 0;
    UsbLog(LOG_DBG, "PnP: %s device started, class %s\n", X->Nt.Kind == EXT_KBD ? "keyboard" : "mouse", name);
    WdmDumpInstance(X->Pdo, "PnP: input node at start");
    for (a = Dev->AttachedDevice; a != NULL && n < 6; a = a->AttachedDevice, n++) {
        ULONG i = 0;
        name[0] = 0;
        if (NT_SUCCESS(ObQueryNameString(a->DriverObject, ni, sizeof(buf) - sizeof(WCHAR), &len)) &&
            ni->Name.Buffer != NULL) {
            for (i = 0; i < ni->Name.Length / sizeof(WCHAR) && i < 63; i++) {
                name[i] = (char)ni->Name.Buffer[i];
            }
        }
        name[i] = 0;
        UsbLog(LOG_DBG, "PnP:   above it: %s (type %x)\n", name, a->DeviceType);
    }
    if (n == 0) {
        UsbLog(LOG_DBG, "PnP:   nothing above it\n");
    }
}

static NTSTATUS NTAPI DispatchPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;

    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    switch (x->Nt.Kind) {
    case EXT_DFLT:
        return WdmFltPower(Dev, Irp);
    case EXT_HC:
    case EXT_BUS:
    case EXT_KBD:
    case EXT_MOUSE:
    case EXT_NODE:
        return FdoPower(x, Irp);
    case EXT_PDO:
        return WdmPdoPower(Dev, Irp);
    default:
        PoStartNextPowerIrp(Irp);
        return WdmComplete(Irp, STATUS_SUCCESS, 0);
    }
}

static NTSTATUS NTAPI DispatchSystemControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;

    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    if (x->Nt.Kind == EXT_HC || x->Nt.Kind == EXT_BUS || x->Nt.Kind == EXT_KBD || x->Nt.Kind == EXT_MOUSE ||
        x->Nt.Kind == EXT_DFLT || x->Nt.Kind == EXT_NODE) {
        return PassDown(x, Irp);
    }
    return WdmComplete(Irp, Irp->IoStatus.Status, Irp->IoStatus.Information);
}

/* the requests nothing else here handles: a disk filter passes them on */
static NTSTATUS NTAPI DispatchOther(PDEVICE_OBJECT Dev, PIRP Irp)
{
    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_DFLT) {
        return WdmFltPass(Dev, Irp);
    }
    return WdmComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS NTAPI DispatchCreateClose(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NT_EXT *ext = (NT_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    if (ext->Kind == EXT_DFLT) {
        return WdmFltPass(Dev, Irp);
    }
    if (ext->Kind == EXT_CTL) {
        if (sp->MajorFunction == IRP_MJ_CLEANUP) {
            NtCtlCleanup(Dev, Irp);
        } else {
            NtCtlCreateClose(Dev, Irp, (BOOLEAN)(sp->MajorFunction == IRP_MJ_CREATE));
        }
    } else if ((ext->Kind == EXT_KBD || ext->Kind == EXT_MOUSE) && sp->MajorFunction != IRP_MJ_CLEANUP) {
        /* the keyboard / mouse device of the bus: kbdclass / mouclass enable a Plug and
           Play port with a create and disable it with a close (IOCTL_INTERNAL_*_ENABLE
           is only sent to the old ports) */
        NtInputEnable(ext->Kind, (BOOLEAN)(sp->MajorFunction == IRP_MJ_CREATE), Dev);
    }
    return WdmComplete(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS NTAPI DispatchDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;

    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    switch (x->Nt.Kind) {
    case EXT_DFLT:
        return WdmFltDeviceControl(Dev, Irp);
    case EXT_CTL:
        return NtCtlIoctl(Dev, Irp);
    case EXT_SCSI:
        return NtScsiDeviceControl(Dev, Irp);
    case EXT_KBD:
    case EXT_MOUSE:
        return NtInputIoctl(Dev, Irp);
    case EXT_PDO: {
        ULONG code = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.IoControlCode;
        NTSTATUS st = WdmPdoDeviceControl(Dev, Irp);
        UsbLog(LOG_DBG, "PnP: child %u ioctl %x -> %x\n", x->Child, code, st);
        return st;
    }
    case EXT_HC:
    case EXT_BUS:
        return PassDown(x, Irp);
    default:
        return WdmComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NTAPI DispatchInternal(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;

    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    switch (x->Nt.Kind) {
    case EXT_DFLT:
        return WdmFltInternal(Dev, Irp);
    case EXT_KBD:
    case EXT_MOUSE:
        return NtInputIoctl(Dev, Irp);
    case EXT_PDO: {
        PSCSI_REQUEST_BLOCK srb = IoGetCurrentIrpStackLocation(Irp)->Parameters.Scsi.Srb;
        UCHAR fn = srb ? srb->Function : 0xFF, op = srb ? srb->Cdb[0] : 0;
        NTSTATUS st = WdmPdoInternal(Dev, Irp);
        if (x->Child == PDO_DISK && (UsbDebugLevel >= LOG_TRACE || (fn != 0 && UsbDebugLevel >= LOG_DBG))) {
            UsbLog(LOG_DBG, "PnP: disk srb %x op %x -> %x\n", fn, op, st);
        }
        return st;
    }
    case EXT_AUDIO:
        return NtAudioInternal(Dev, Irp);
    case EXT_SCSI:
        return NtScsiSrb(Dev, Irp);
    case EXT_HC:
    case EXT_BUS:
        return PassDown(x, Irp);
    default:
        return WdmComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

static NTSTATUS NTAPI DispatchShutdown(PDEVICE_OBJECT Dev, PIRP Irp)
{
    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_PFLT) {
        return WdmPs2Dispatch(Dev, Irp);            /* i8042prt mouse filter */
    }
    if (((NT_EXT *)Dev->DeviceExtension)->Kind == EXT_DFLT) {
        return WdmFltPass(Dev, Irp);
    }
    return WdmComplete(Irp, STATUS_SUCCESS, 0);
}

/* ------------------------------------------------------------------ */
/* AddDevice                                                            */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI AddDevice(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fdo;
    WDM_EXT *x;
    NTSTATUS st;
    ULONG child = 0, kind = EXT_HC;
    WCHAR hwid[64];
    ULONG len = 0;
    int i;

    if (WdmFltWanted(Pdo)) {
        return WdmFltAdd(Drv, Pdo);             /* LowerFilters of a USBSTOR disk */
    }
    if (WdmPs2Wanted(Pdo)) {
        return WdmPs2Add(Drv, Pdo);             /* UpperFilters of i8042prt's PS/2 mouse */
    }
    if (WdmIsOurPdo(Pdo, &child)) {
        if (child == PDO_KBD) {
            kind = EXT_KBD;
        } else if (child == PDO_MOUSE) {
            kind = EXT_MOUSE;
        } else if (child == PDO_HUB || child == PDO_UDEV) {
            kind = EXT_NODE;                    /* Device Manager: root hub / hub / device (wdmtree.c) */
        } else {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
    } else {
        RtlZeroMemory(hwid, sizeof(hwid));
        IoGetDeviceProperty(Pdo, DevicePropertyHardwareID, sizeof(hwid) - sizeof(WCHAR), hwid, &len);
        if (hwid[0] == L'U' && hwid[1] == L'S' && hwid[2] == L'B' && hwid[3] == L'N' && hwid[4] == L'T' &&
            hwid[5] == L'\\' && hwid[6] == L'B') {
            return WdmBusAdd(Drv, Pdo);             /* USBNT\Bus: the device bus */
        }
    }
    UsbLog(LOG_DBG, "PnP: AddDevice kind %u child %u\n", kind, child);
    st = IoCreateDevice(Drv, sizeof(WDM_EXT), NULL, kind == EXT_HC ? FILE_DEVICE_CONTROLLER :
                        kind == EXT_NODE ? FILE_DEVICE_UNKNOWN :
                        (kind == EXT_KBD ? FILE_DEVICE_KEYBOARD : FILE_DEVICE_MOUSE), FILE_DEVICE_SECURE_OPEN,
                        FALSE, &fdo);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    x = (WDM_EXT *)fdo->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Nt.Kind = kind;
    x->Nt.Self = fdo;
    x->Pdo = Pdo;
    x->Lower = IoAttachDeviceToDeviceStack(fdo, Pdo);
    if (x->Lower == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }
    if (kind == EXT_HC) {
        for (i = 0; i < USB_MAX_HC; i++) {
            if (HcList[i] == NULL) {
                HcList[i] = fdo;
                break;
            }
        }
    }
    fdo->Flags |= (kind == EXT_HC) ? DO_POWER_PAGABLE : (DO_BUFFERED_IO | DO_POWER_PAGABLE);
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Worker thread, poll timer                                            */
/* ------------------------------------------------------------------ */

static void ArmTimer(void)
{
    LARGE_INTEGER due;

    due.QuadPart = -10000;
    KeSetTimer(&PollTimer, due, &PollDpc);
}

static VOID NTAPI PollDpcRoutine(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    (void)Dpc;
    (void)Ctx;
    (void)A1;
    (void)A2;
    UsbTick();
    if (!Stopping) {
        ArmTimer();
    }
}

static void TextSetupStart(void);
static WDM_EXT *ClaimX[8];                     /* controllers taken in text-mode setup (TextSetupHcs) */
static volatile LONG ClaimN;

static VOID NTAPI WorkerThread(PVOID Ctx)
{
    LARGE_INTEGER t;

    (void)Ctx;
    NtWorkerThread = KeGetCurrentThread();
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    UsbPnpInit();
    while (!Stopping) {
        ULONG ms = (UsbPollWantMs != 0 && UsbPollWantMs < 100) ? UsbPollWantMs : 100;
        t.QuadPart = -(LONGLONG)ms * 10000;
        KeWaitForSingleObject(&NtWorkerEvent, Executive, KernelMode, FALSE, &t);
        RunQueued();
        if (ClaimN > 0) {
            TextSetupStart();
        }
        UsbWorkerIteration();
        /* HID over I2C: the controllers are found with the ACPI devices once the PCI ones have started */
        if (!I2cStarted && FirstStart != 0 && OsTimeMs() - FirstStart > 2000) {
            I2cStarted = TRUE;
            I2cHidInit();
            SmbusInit();            /* touch pads that already answer on SMBus (smbtp.c) */
        }
        WdmBusService();
        WdmTreeService();
        WdmPs2Service();
        NtInputService();
    }
    RunQueued();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static PVOID WorkerObject;

static VOID NTAPI Unload(PDRIVER_OBJECT Drv)
{
    UNICODE_STRING ulink;
    PDEVICE_OBJECT d;

    Stopping = 1;
    KeCancelTimer(&PollTimer);
    OsWakeWorker();
    if (WorkerObject != NULL) {
        KeWaitForSingleObject(WorkerObject, Executive, KernelMode, FALSE, NULL);
        ObDereferenceObject(WorkerObject);
    }
    UsbShutdown();
    RtlInitUnicodeString(&ulink, USBNT_DOS_NAME);
    IoDeleteSymbolicLink(&ulink);
    while ((d = Drv->DeviceObject) != NULL) {
        IoDeleteDevice(d);
    }
}

/*
 * Windows 2000 text-mode setup with the driver disk (F6): setup loads usbnt.sys but does not use
 * the disk's [HardwareIds.scsi.*] (Windows XP's does; also an exact controller id does not help),
 * so the host controllers get no driver - no USB keyboard, no USB disk in setup.  WdmBusInit
 * found them (the controllers without a service in setup's registry); here usbnt.sys takes them
 * itself: IoReportDetectedDevice finds the PCI device's PDO (PCI's legacy device detection), our
 * function device goes onto it and the controller starts the way a start request starts it (it
 * needs nothing from the request).  Plug and Play does not start a device node that had no driver,
 * so the device bus (keyboard, mouse, disks) is a device usbnt.sys reports too - setup's registry
 * is thrown away at the end of text-mode setup, so it is not left behind.
 */
static void TextSetupHcs(PDRIVER_OBJECT Drv)
{
    ULONG bus[8], slot[8], n = WdmTextSetupClaims(bus, slot, 8), i;
    PDEVICE_OBJECT pdo;
    NTSTATUS st;

    if (n == 0) {
        return;
    }
    if (!WdmBusPresent()) {
        pdo = NULL;
        st = IoReportDetectedDevice(Drv, InterfaceTypeUndefined, (ULONG)-1, (ULONG)-1, NULL, NULL, FALSE, &pdo);
        if (NT_SUCCESS(st) && pdo != NULL) {
            st = WdmBusAdd(Drv, pdo);
            if (NT_SUCCESS(st)) {
                WdmBusReportedStarted();
            }
        }
        UsbLog(LOG_INFO, "PnP: text-mode setup: device bus reported (%x)\n", st);
    }
    for (i = 0; i < n; i++) {
        UCHAR nb[160];
        POBJECT_NAME_INFORMATION ni = (POBJECT_NAME_INFORMATION)nb;
        ULONG nl = 0;
        UNICODE_STRING pci;
        WDM_EXT *x;
        PDEVICE_OBJECT top;

        pdo = NULL;
        st = IoReportDetectedDevice(Drv, PCIBus, bus[i], slot[i], NULL, NULL, FALSE, &pdo);
        RtlInitUnicodeString(&pci, L"\\Driver\\Pci");
        if (!NT_SUCCESS(st) || pdo == NULL ||
            !NT_SUCCESS(ObQueryNameString(pdo->DriverObject, ni, sizeof(nb) - 2, &nl)) ||
            !RtlEqualUnicodeString(&ni->Name, &pci, TRUE)) {
            UsbLog(LOG_ERR, "PnP: text-mode setup: no PCI device %02x:%02x.%u (%x)\n", bus[i], slot[i] & 31,
                   slot[i] >> 5, st);
            continue;
        }
        if (!NT_SUCCESS(AddDevice(Drv, pdo))) {
            continue;
        }
        top = IoGetAttachedDeviceReference(pdo);   /* our function device (ACPI may filter the PDO) */
        ObDereferenceObject(top);
        if (top->DriverObject != Drv) {
            continue;
        }
        x = (WDM_EXT *)top->DeviceExtension;
        x->Bus = (UCHAR)bus[i];
        x->Dev = (UCHAR)(slot[i] & 31);
        x->Fn = (UCHAR)(slot[i] >> 5);
        x->Claimed = TRUE;
        if (ClaimN < 8) {
            ClaimX[ClaimN++] = x;                /* the worker starts it (DriverEntry must not wait for it) */
        }
    }
    OsWakeWorker();
}

/* worker: the controllers TextSetupHcs took */
static void TextSetupStart(void)
{
    HC_ARG a;
    WDM_EXT *x;

    while (ClaimN > 0) {
        x = ClaimX[--ClaimN];
        a.X = x;
        a.Gone = FALSE;
        a.Settle = TRUE;
        HcStartW(&a);
        x->Started = TRUE;
        UsbLog(LOG_INFO, "PnP: text-mode setup: %02x:%02x.%u taken without a driver from setup (%d)\n", x->Bus,
               x->Dev, x->Fn, a.Result);
        WdmBusOnController(x);
    }
}

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING RegistryPath)
{
    HANDLE thread;
    NTSTATUS st;

    WdmDriver = Drv;
    NtOsInit(RegistryPath);
    NtKernelInit(Drv);
    KeInitializeSpinLock(&WLock);
    UsbDebugLevel = OsGetConfig("DebugLevel", LOG_INFO);
    UsbLog(LOG_INFO, "USB driver %s for Windows 2000 / XP (%s), build %s\n", USBNT_VERSION_STR,
           sizeof(void *) == 8 ? "x64" : "x86", __DATE__);

    {
        int i;
        for (i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; i++) {
            Drv->MajorFunction[i] = DispatchOther;
        }
    }
    Drv->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_CLEANUP] = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    Drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternal;
    Drv->MajorFunction[IRP_MJ_SHUTDOWN] = DispatchShutdown;
    Drv->MajorFunction[IRP_MJ_PNP] = DispatchPnp;
    Drv->MajorFunction[IRP_MJ_POWER] = DispatchPower;
    Drv->MajorFunction[IRP_MJ_SYSTEM_CONTROL] = DispatchSystemControl;
    Drv->DriverExtension->AddDevice = AddDevice;
    Drv->DriverUnload = Unload;

    UsbPciRelocateAll();                        /* every BAR above 4 GB goes below 4 GB */
    UsbScsiFixedMode = OsGetConfig("RemovableAsFixed", 1);
    UsbScsiInit();
    WdmBusInit();                               /* is there a USBNT\Bus device? */
    UsbdiInit();                                /* USB devices for Windows' own USB drivers */
    NtScsiCreate(Drv);                          /* \\.\ScsiN: for SCSI pass through and the tools' drive map */
    NtInputCreate(Drv);                         /* the \\.\UsbNt control device */
    if (!OsGetConfig("DisableAudio", 0)) {
        NtAudioCreate(Drv);
    }
    KeInitializeDpc(&PollDpc, PollDpcRoutine, NULL);
    KeInitializeTimer(&PollTimer);
    if (OsGetConfig("TimerResolution", 1)) {
        ULONG now = ExSetTimerResolution(10000, TRUE);
        UsbLog(LOG_INFO, "timer resolution %u us\n", now / 10);
    }
    ArmTimer();
    st = PsCreateSystemThread(&thread, THREAD_ALL_ACCESS, NULL, NULL, NULL, WorkerThread, NULL);
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "cannot create worker thread (%x)\n", st);
        KeCancelTimer(&PollTimer);
        return st;
    }
    ObReferenceObjectByHandle(thread, THREAD_ALL_ACCESS, NULL, KernelMode, &WorkerObject, NULL);
    ZwClose(thread);
    TextSetupHcs(Drv);
    return STATUS_SUCCESS;
}
