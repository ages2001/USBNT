/*
 * wdmps2.c - Windows 2000 / XP: usbnt.sys as an upper filter of i8042prt's
 * PS/2 mouse (the PNP0F.. device of the Mouse class), the way the touch pad
 * vendors' drivers sit there: touch pads get their native protocols
 * (ps2pad.c: Synaptics, ALPS, Elantech) and the gestures of the touch pad
 * engine (ptp.c), every other mouse stays i8042prt's.
 *
 * i8042prt sends IOCTL_INTERNAL_I8042_HOOK_MOUSE up its stack: the filter's
 * ISR hook then sees every byte from the mouse before i8042prt does.  When
 * i8042prt has reset and set up the mouse (at start and after resume), the
 * filter talks to the mouse itself - polled, at the controller's interrupt
 * level (KeSynchronizeExecution with the interrupt object of
 * IOCTL_INTERNAL_I8042_MOUSE_START_INFORMATION, Windows XP; on Windows 2000,
 * which does not send it, inside the ISR hook: the filter has the mouse ACK
 * a command and works in that interrupt): it asks for a touch pad, and when
 * there is one, switches it to its absolute mode.  From then on the hook
 * takes every byte (i8042prt's packet decoding never sees them), a DPC decodes
 * them and hands MOUSE_INPUT_DATA (MOUSE_WHEEL for scrolling) to mouclass
 * through the service callback of IOCTL_INTERNAL_MOUSE_CONNECT, which the
 * filter replaced.  A plain mouse is set up again the way i8042prt had it
 * (device id 0 / 3 / 4, rate, resolution) and the filter stays passive.
 * Keyboard bytes that come during the 0.1 - 0.5 s of the detection are lost.
 *
 * usbinst /install puts usbnt into the UpperFilters of the i8042prt mouse
 * (not with /nops2; /remove takes it out).  Registry: Ps2Touchpad = 0: the
 * filter stays passive.
 */

#include "wdm.h"
#include "ps2pad.h"

#define IOCTL_INTERNAL_I8042_HOOK_MOUSE \
    CTL_CODE(FILE_DEVICE_MOUSE, 0x0FF0, METHOD_NEITHER, FILE_ANY_ACCESS)
#define IOCTL_INTERNAL_I8042_MOUSE_START_INFORMATION \
    CTL_CODE(FILE_DEVICE_MOUSE, 0x0FF3, METHOD_NEITHER, FILE_ANY_ACCESS)

#define MOUSE_RESETTING     5               /* MOUSE_STATE: MouseResetting (ntdd8042.h) */
#define RING                512

typedef BOOLEAN (NTAPI *I8042_MOUSE_ISR)(PVOID IsrContext, PMOUSE_INPUT_DATA CurrentInput, PVOID CurrentOutput,
                                         UCHAR StatusByte, PUCHAR Byte, PBOOLEAN ContinueProcessing,
                                         PLONG MouseState, PLONG ResetSubState);
typedef VOID (NTAPI *I8042_ISR_WRITE_PORT)(PVOID Context, UCHAR Value);
typedef VOID (NTAPI *I8042_QUEUE_PACKET)(PVOID Context);
typedef VOID (NTAPI *CLASS_SERVICE)(PVOID ClassDeviceObject, PVOID Start, PVOID End, PVOID Consumed);

typedef struct _I8042_HOOK_MOUSE {
    PVOID               Context;
    I8042_MOUSE_ISR     IsrRoutine;
    I8042_ISR_WRITE_PORT IsrWritePort;
    I8042_QUEUE_PACKET  QueueMousePacket;
    PVOID               CallContext;
} I8042_HOOK_MOUSE;

typedef struct _I8042_START_INFO {
    ULONG               Size;
    PKINTERRUPT         InterruptObject;
    ULONG               Reserved[8];
} I8042_START_INFO;

typedef struct _PFLT {
    WDM_EXT        *X;
    BOOLEAN         Hooked;
    BOOLEAN         Want;               /* Ps2Touchpad */
    UCHAR           Prev;               /* buttons of the last event */
    UCHAR           Id;                 /* i8042prt's mouse set-up, to restore a plain mouse */
    UCHAR           Rate, Res;
    UCHAR           Pad1[2];
    CONNECT_DATA    Upper;              /* mouclass */
    I8042_MOUSE_ISR UpperIsr;
    PVOID           UpperCtx;
    PKINTERRUPT     Int;
    volatile LONG   Native;             /* the touch pad runs in its own protocol */
    volatile LONG   Pending;            /* i8042prt (re)started the mouse: detect when it is done */
    volatile LONG   Kick;               /* Windows 2000: the ACK that starts the detection is due */
    volatile ULONG  ResetBytes;         /* bytes i8042prt took while resetting the mouse */
    ULONG           SeenBytes, QuietSince, KickMs;
    volatile ULONG  Head, Tail;
    UCHAR           Ring[RING];
    KSPIN_LOCK      Lock;
    KDPC            Dpc;
    KTIMER          Timer;
    ULONG           Events, Detections;
    int             Result;
    volatile LONG   LogDue;             /* a detection ran: the worker logs it */
    ULONG           Hist[16];           /* the last bytes of the hook: state << 8 | byte (log) */
    ULONG           NHist;
    PS2PAD          Pad;
} PFLT;

static PFLT *F;

/* ------------------------------------------------------------------ */
/* polled access to the mouse (at the controller's interrupt level)    */
/* ------------------------------------------------------------------ */
static BOOLEAN WaitIn(void)
{
    ULONG i;
    for (i = 0; i < 20000; i++) {
        if (!(READ_PORT_UCHAR((PUCHAR)0x64) & 0x02)) return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static int AuxRead(void *Ctx, ULONG Ms)
{
    ULONG i;
    (void)Ctx;
    for (i = 0; i < Ms * 100; i++) {
        UCHAR st = READ_PORT_UCHAR((PUCHAR)0x64);
        if (st & 0x01) {
            UCHAR b = READ_PORT_UCHAR((PUCHAR)0x60);
            if (st & 0x20) return b;
            continue;                                   /* a keyboard byte: lost */
        }
        KeStallExecutionProcessor(10);
    }
    return -1;
}

static BOOLEAN AuxSend(void *Ctx, UCHAR B)
{
    int r = -1, t, n;
    for (t = 0; t < 3; t++) {
        if (!WaitIn()) return FALSE;
        WRITE_PORT_UCHAR((PUCHAR)0x64, 0xD4);
        if (!WaitIn()) return FALSE;
        WRITE_PORT_UCHAR((PUCHAR)0x60, B);
        for (n = 0; n < 8; n++) {                       /* stray bytes (a late AA 00, a packet) skipped */
            r = AuxRead(Ctx, 100);
            if (r < 0 || r == 0xFA || r == 0xFE || r == 0xFC) break;
        }
        if (r == 0xFA) return TRUE;
        if (r != 0xFE) return FALSE;
    }
    return FALSE;
}

static const PS2PAD_IO Io = { NULL, AuxSend, AuxRead };

static BOOLEAN Rate(UCHAR R) { return (BOOLEAN)(AuxSend(NULL, 0xF3) && AuxSend(NULL, R)); }

static int GetId(void)
{
    if (!AuxSend(NULL, 0xF2)) return -1;
    return AuxRead(NULL, 100);
}

static void Out(void *Ctx, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan);

/* the detection, at the controller's interrupt level */
static BOOLEAN NTAPI Detect(PVOID Ctx)
{
    UCHAR st[3];
    int id, r;
    (void)Ctx;

    F->Detections++;
    F->LogDue = 1;
    /* what i8042prt set up: device id, rate, resolution */
    id = GetId();
    F->Id = (UCHAR)(id == 3 || id == 4 ? id : 0);
    F->Rate = 100;
    F->Res = 3;
    if (AuxSend(NULL, 0xE9) && (r = AuxRead(NULL, 100)) >= 0) {
        st[0] = (UCHAR)r;
        st[1] = (UCHAR)AuxRead(NULL, 100);
        st[2] = (UCHAR)AuxRead(NULL, 100);
        if (st[1] <= 3) F->Res = st[1];
        if (st[2] >= 10 && st[2] <= 200) F->Rate = st[2];
    }
    F->Result = PS2PAD_NONE;
    if (AuxSend(NULL, 0xFF)) {
        r = AuxRead(NULL, 2500);                      /* self test: up to 2.5 s (slow trackpoints, pads) */
        if (r == 0xAA) (void)AuxRead(NULL, 200);
        F->Result = Ps2PadDetect(&F->Pad, &Io, Out, NULL);
    }
    if (F->Result != PS2PAD_NONE && AuxSend(NULL, 0xF4)) {
        Ps2PadStarted(&F->Pad, &Io);
        F->Prev = 0;
        F->Head = F->Tail = 0;
        InterlockedExchange((PLONG)&F->Native, 1);
        return TRUE;
    }
    /* a mouse (or a pad we leave relative): back to i8042prt's set-up */
    F->Result = PS2PAD_NONE;
    AuxSend(NULL, 0xF6);
    if (AuxSend(NULL, 0xFF)) {
        r = AuxRead(NULL, 2500);                      /* self test: up to 2.5 s (slow trackpoints, pads) */
        if (r == 0xAA) (void)AuxRead(NULL, 200);
    }
    if (F->Id >= 3) {
        if (Rate(200) && Rate(100) && Rate(80) && GetId() == 3 && F->Id == 4) {
            Rate(200); Rate(200); Rate(80); GetId();
        }
    }
    Rate(F->Rate);
    AuxSend(NULL, 0xE8); AuxSend(NULL, F->Res);
    AuxSend(NULL, 0xF4);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* data path                                                           */
/* ------------------------------------------------------------------ */
static void Out(void *Ctx, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan)
{
    USB_MOUSE_EVENT e;
    MOUSE_INPUT_DATA d;
    ULONG used = 0;
    (void)Ctx;

    if (!Ps2PadEvent(&F->Prev, Buttons, Dx, Dy, Wheel, Pan, &e) || F->Upper.ClassService == NULL) {
        return;
    }
    if (Pan != 0) {
        OsMousePan(Pan);                                /* 2000 / XP have no horizontal wheel: usbmon, WM_HSCROLL */
    }
    if (e.ButtonFlags == 0 && Dx == 0 && Dy == 0) {
        return;
    }
    RtlZeroMemory(&d, sizeof(d));
    d.Flags = MOUSE_MOVE_RELATIVE;
    d.ButtonFlags = e.ButtonFlags;                      /* UMOU_* are the MOUSE_* button flags */
    if (e.ButtonFlags & UMOU_WHEEL) {
        d.ButtonData = (USHORT)(SHORT)(e.Wheel * 120);
    }
    d.LastX = Dx;
    d.LastY = Dy;
    F->Events++;
    ((CLASS_SERVICE)F->Upper.ClassService)(F->Upper.ClassDeviceObject, &d, &d + 1, &used);
}

static VOID NTAPI DataDpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    LARGE_INTEGER due;
    (void)Dpc; (void)Ctx; (void)A1; (void)A2;

    KeAcquireSpinLockAtDpcLevel(&F->Lock);
    while (F->Tail != F->Head) {
        UCHAR b = F->Ring[F->Tail];
        F->Tail = (F->Tail + 1) % RING;
        if (F->Native) Ps2PadByte(&F->Pad, b);
    }
    if (F->Native && Ps2PadBusy(&F->Pad)) {
        Ps2PadTick(&F->Pad);
        due.QuadPart = -150000;                         /* 15 ms: the end of a tap click */
        KeSetTimer(&F->Timer, due, &F->Dpc);
    }
    KeReleaseSpinLockFromDpcLevel(&F->Lock);
}

static BOOLEAN NTAPI IsrHook(PVOID IsrContext, PMOUSE_INPUT_DATA CurrentInput, PVOID CurrentOutput, UCHAR StatusByte,
                             PUCHAR Byte, PBOOLEAN ContinueProcessing, PLONG MouseState, PLONG ResetSubState)
{
    BOOLEAN r = TRUE;
    (void)IsrContext;

    if (F->UpperIsr != NULL) {
        r = F->UpperIsr(F->UpperCtx, CurrentInput, CurrentOutput, StatusByte, Byte, ContinueProcessing, MouseState,
                        ResetSubState);
        if (!r || !*ContinueProcessing) {
            return r;
        }
    }
    *ContinueProcessing = TRUE;                     /* i8042prt goes on with the byte unless we take it */
    F->Hist[F->NHist++ % 16] = (ULONG)((MouseState ? *MouseState : 0xFF) << 16 |
                                       (ResetSubState ? (*ResetSubState & 0xFF) : 0xFF) << 8 | *Byte);
    if (MouseState != NULL && *MouseState == MOUSE_RESETTING) {
        /* i8042prt (re)starts the mouse: it is a relative mouse again until the detection */
        F->Native = 0;
        F->Kick = 0;
        F->ResetBytes++;
        F->Pending = 1;
        return r;
    }
    if (F->Kick && *Byte == 0xFA) {
        F->Kick = 0;                                    /* Windows 2000: the ACK we asked for */
        Detect(NULL);
        *ContinueProcessing = FALSE;
        return TRUE;
    }
    if (F->Native) {
        ULONG n = (F->Head + 1) % RING;
        if (n != F->Tail) {
            F->Ring[F->Head] = *Byte;
            F->Head = n;
        }
        KeInsertQueueDpc(&F->Dpc, NULL, NULL);
        *ContinueProcessing = FALSE;
        return TRUE;
    }
    return r;
}

/* worker (PASSIVE_LEVEL): the detection once i8042prt is done with the mouse */
void WdmPs2Service(void)
{
    ULONG now;

    if (F == NULL) {
        return;
    }
    if (F->LogDue) {
        F->LogDue = 0;
        if (F->Pad.Info[0]) UsbLog(LOG_INFO, "ps2 filter: %s\n", F->Pad.Info);
        UsbLog(LOG_INFO, "ps2 filter: %s%s (i8042prt had id %u, rate %u, resolution %u)\n",
               F->Native ? "touch pad in its native mode: " : "plain mouse, left to i8042prt",
               F->Native ? F->Pad.Name : "", F->Id, F->Rate, F->Res);
        if (UsbDebugLevel >= LOG_DBG) {
            ULONG k, n = F->NHist;
            char line[160];
            int m = 0;
            for (k = n > 16 ? n - 16 : 0; k < n && m < 150; k++) {
                m += UsbFormat(line + m, sizeof(line) - m, " %x", F->Hist[k % 16]);
            }
            UsbLog(LOG_DBG, "ps2 filter: %u hook bytes, last (state.substate.byte):%s\n", n, line);
        }
    }
    if (F->X == NULL || !F->Hooked || !F->Want || !F->Pending) {
        return;
    }
    now = OsTimeMs();
    if (F->ResetBytes != F->SeenBytes) {
        F->SeenBytes = F->ResetBytes;
        F->QuietSince = now;
        return;
    }
    if (now - F->QuietSince < 500) {
        return;
    }
    if (F->Kick) {
        if (now - F->KickMs > 1000) {
            F->Kick = 0;
            F->Pending = 0;
            UsbLog(LOG_ERR, "ps2 filter: the mouse does not answer, no touch pad detection\n");
        }
        return;
    }
    F->Pending = 0;
    if (F->Int != NULL) {
        KeSynchronizeExecution(F->Int, Detect, NULL);
    } else {
        /* no interrupt object (Windows 2000): the mouse ACKs an enable, the hook detects in that interrupt */
        F->KickMs = now;
        F->Kick = 1;
        if (WaitIn()) WRITE_PORT_UCHAR((PUCHAR)0x64, 0xD4);
        if (WaitIn()) WRITE_PORT_UCHAR((PUCHAR)0x60, 0xF4);
    }
}

/* ------------------------------------------------------------------ */
/* the filter device                                                   */
/* ------------------------------------------------------------------ */
static VOID NTAPI FltService(PVOID ClassDevice, PVOID Start, PVOID End, PVOID Consumed)
{
    (void)ClassDevice;
    if (F != NULL && F->Upper.ClassService != NULL) {
        ((CLASS_SERVICE)F->Upper.ClassService)(F->Upper.ClassDeviceObject, Start, End, Consumed);
    } else {
        *(PULONG)Consumed = (ULONG)(((PMOUSE_INPUT_DATA)End) - ((PMOUSE_INPUT_DATA)Start));
    }
}

/* the PDO is i8042prt's PS/2 mouse: a device of the Mouse class that is not one of ours */
BOOLEAN WdmPs2Wanted(PDEVICE_OBJECT Pdo)
{
    WCHAR cls[16];
    ULONG len = 0, child = 0;

    if (WdmIsOurPdo(Pdo, &child)) {
        return FALSE;
    }
    RtlZeroMemory(cls, sizeof(cls));
    if (!NT_SUCCESS(IoGetDeviceProperty(Pdo, DevicePropertyClassName, sizeof(cls) - sizeof(WCHAR), cls, &len))) {
        return FALSE;
    }
    return (BOOLEAN)((cls[0] == L'M' || cls[0] == L'm') && (cls[1] == L'o' || cls[1] == L'O') &&
                     (cls[2] == L'u' || cls[2] == L'U') && (cls[3] == L's' || cls[3] == L'S') &&
                     (cls[4] == L'e' || cls[4] == L'E') && cls[5] == 0);
}

NTSTATUS WdmPs2Add(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fdo;
    WDM_EXT *x;
    NTSTATUS st;

    if (F != NULL && F->X != NULL) {
        return STATUS_SUCCESS;                          /* one PS/2 mouse port */
    }
    if (F == NULL) {
        F = (PFLT *)OsAlloc(sizeof(PFLT));              /* kept for good: the ISR hook may still be called */
        if (F == NULL) {
            return STATUS_SUCCESS;
        }
        KeInitializeSpinLock(&F->Lock);
        KeInitializeDpc(&F->Dpc, DataDpc, NULL);
        KeInitializeTimer(&F->Timer);
    }
    st = IoCreateDevice(Drv, sizeof(WDM_EXT), NULL, FILE_DEVICE_MOUSE, 0, FALSE, &fdo);
    if (!NT_SUCCESS(st)) {
        return STATUS_SUCCESS;                          /* not filtered: the mouse works without us */
    }
    x = (WDM_EXT *)fdo->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Nt.Kind = EXT_PFLT;
    x->Nt.Self = fdo;
    x->Pdo = Pdo;
    x->Lower = IoAttachDeviceToDeviceStack(fdo, Pdo);
    if (x->Lower == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_SUCCESS;
    }
    F->X = x;
    F->Hooked = FALSE;
    F->Native = 0;
    F->Pending = 0;
    F->Upper.ClassService = NULL;
    F->Want = (BOOLEAN)(OsGetConfig("Ps2Touchpad", 1) != 0);
    fdo->Flags |= x->Lower->Flags & (DO_BUFFERED_IO | DO_DIRECT_IO | DO_POWER_PAGABLE);
    fdo->DeviceType = x->Lower->DeviceType;
    fdo->Characteristics = x->Lower->Characteristics;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    UsbLog(LOG_INFO, "ps2 filter: on the PS/2 mouse%s\n", F->Want ? "" : " (Ps2Touchpad=0: passive)");
    return STATUS_SUCCESS;
}

static NTSTATUS NTAPI AttrDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    (void)Dev; (void)Ctx;
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    if (NT_SUCCESS(Irp->IoStatus.Status) && F != NULL && F->Native &&
        Irp->IoStatus.Information >= sizeof(MOUSE_ATTRIBUTES)) {
        PMOUSE_ATTRIBUTES a = (PMOUSE_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer;
        a->MouseIdentifier = WHEELMOUSE_I8042_HARDWARE;
        if (a->NumberOfButtons < 3) a->NumberOfButtons = 3;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS Pass(WDM_EXT *x, PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(x->Lower, Irp);
}

NTSTATUS WdmPs2Dispatch(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    switch (sp->MajorFunction) {
    case IRP_MJ_POWER:
        PoStartNextPowerIrp(Irp);
        IoSkipCurrentIrpStackLocation(Irp);
        return PoCallDriver(x->Lower, Irp);
    case IRP_MJ_PNP:
        if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE) {
            if (F != NULL) {
                F->Native = 0;
                F->Hooked = FALSE;
                F->Upper.ClassService = NULL;
                KeCancelTimer(&F->Timer);
                F->X = NULL;
            }
            st = Pass(x, Irp);
            IoDetachDevice(x->Lower);
            IoDeleteDevice(Dev);
            return st;
        }
        return Pass(x, Irp);
    case IRP_MJ_INTERNAL_DEVICE_CONTROL:
        switch (sp->Parameters.DeviceIoControl.IoControlCode) {
        case IOCTL_INTERNAL_MOUSE_CONNECT: {
            PCONNECT_DATA cd = (PCONNECT_DATA)sp->Parameters.DeviceIoControl.Type3InputBuffer;
            if (F == NULL || cd == NULL || sp->Parameters.DeviceIoControl.InputBufferLength < sizeof(CONNECT_DATA)) {
                break;
            }
            /* mouclass' service callback: ours in between */
            F->Upper = *cd;
            cd->ClassDeviceObject = Dev;
            cd->ClassService = (PVOID)FltService;
            break;
        }
        case IOCTL_INTERNAL_MOUSE_DISCONNECT:
            if (F != NULL) {
                F->Native = 0;
                F->Upper.ClassService = NULL;
            }
            break;
        case IOCTL_INTERNAL_I8042_HOOK_MOUSE: {
            I8042_HOOK_MOUSE *h = (I8042_HOOK_MOUSE *)sp->Parameters.DeviceIoControl.Type3InputBuffer;
            if (F == NULL || h == NULL || !F->Want) {
                break;                                  /* passive: i8042prt alone */
            }
            F->UpperCtx = h->Context;
            F->UpperIsr = h->IsrRoutine;
            h->Context = Dev;
            h->IsrRoutine = IsrHook;
            F->Hooked = TRUE;
            F->Pending = 1;                             /* also when the reset went by before the hook */
            F->ResetBytes++;
            break;
        }
        case IOCTL_INTERNAL_I8042_MOUSE_START_INFORMATION: {
            I8042_START_INFO *si = (I8042_START_INFO *)sp->Parameters.DeviceIoControl.Type3InputBuffer;
            if (F != NULL && si != NULL) {
                F->Int = si->InterruptObject;
            }
            break;
        }
        case IOCTL_MOUSE_QUERY_ATTRIBUTES:
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, AttrDone, NULL, TRUE, FALSE, FALSE);
            return IoCallDriver(x->Lower, Irp);
        }
        return Pass(x, Irp);
    case IRP_MJ_DEVICE_CONTROL:
        if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_MOUSE_QUERY_ATTRIBUTES) {
            IoCopyCurrentIrpStackLocationToNext(Irp);
            IoSetCompletionRoutine(Irp, AttrDone, NULL, TRUE, FALSE, FALSE);
            return IoCallDriver(x->Lower, Irp);
        }
        return Pass(x, Irp);
    default:
        return Pass(x, Irp);
    }
}
