/*
 * ntinput.c - keyboard and pointer port devices for kbdclass/mouclass and
 * the \\.\UsbNt control device (wheel channel for NT 3.x, slot control).
 */

#include "ntdrv.h"

typedef VOID (NTAPI *CLASS_SERVICE)(PVOID ClassDeviceObject, PVOID Start, PVOID End, PVOID Consumed);

typedef struct _PORT_STATE {
    PDEVICE_OBJECT  Device;
    PDEVICE_OBJECT  ClassDevice;
    CLASS_SERVICE   Service;
    LONG            Enabled;
    ULONG           Number;
} PORT_STATE;

static PORT_STATE Kbd;
static PORT_STATE Mou;
static KSPIN_LOCK InputLock;
static KEYBOARD_TYPEMATIC_PARAMETERS Typematic = { 0, 30, 500 };
static KEYBOARD_INDICATOR_PARAMETERS Indicators = { 0, 0 };

static PDEVICE_OBJECT CtlDevice;
static PIRP WheelIrp;
static LONG WheelAccum;
static LONG WheelOpen;
static ULONG WheelMode;

#define USB_WHEEL_DELTA 120
#define WHEEL_AUTO      0
#define WHEEL_FLAG_ONLY 1
#define WHEEL_CHANNEL   2

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

NTSTATUS NtInputCreate(PDRIVER_OBJECT Drv)
{
    UNICODE_STRING uname;
    UNICODE_STRING ulink;
    NT_EXT *ext;
    NTSTATUS st;

    KeInitializeSpinLock(&InputLock);
    WheelMode = OsGetConfig("WheelMode", WHEEL_AUTO);

    if (!OsGetConfig("DisableKeyboard", 0)) {
        CreatePort(Drv, &Kbd, EXT_KBD, L"\\Device\\KeyboardPort", L"KEYBOARDPORT", FILE_DEVICE_8042_PORT);
    }
    if (!OsGetConfig("DisableMouse", 0)) {
        CreatePort(Drv, &Mou, EXT_MOUSE, L"\\Device\\PointerPort", L"POINTERPORT", FILE_DEVICE_8042_PORT);
    }

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
    Deliver(&Kbd, &data[0], &data[Count]);
}

static void WheelChannel(LONG Delta)
{
    KIRQL irql;
    PIRP irp = NULL;
    LONG value = 0;

    IoAcquireCancelSpinLock(&irql);
    if (WheelOpen <= 0) {
        IoReleaseCancelSpinLock(irql);
        return;
    }
    WheelAccum += Delta;
    if (WheelIrp != NULL) {
        irp = WheelIrp;
        WheelIrp = NULL;
        IoSetCancelRoutine(irp, NULL);
        value = WheelAccum;
        WheelAccum = 0;
    }
    IoReleaseCancelSpinLock(irql);
    if (irp != NULL) {
        *(LONG *)irp->AssociatedIrp.SystemBuffer = value;
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = sizeof(LONG);
        IoCompleteRequest(irp, IO_MOUSE_INCREMENT);
    }
}

void OsMouseInput(const USB_MOUSE_EVENT *E)
{
    MOUSE_INPUT_DATA d;
    USHORT flags = E->ButtonFlags;
    BOOLEAN channel = FALSE;

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
    if (channel && E->Wheel != 0) {
        WheelChannel(E->Wheel);
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
    if (P->Service != NULL) {
        return STATUS_SHARING_VIOLATION;
    }
    KeAcquireSpinLock(&InputLock, &old);
    P->ClassDevice = cd->ClassDeviceObject;
    P->Service = (CLASS_SERVICE)cd->ClassService;
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
        return STATUS_NOT_IMPLEMENTED;
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
        return STATUS_NOT_IMPLEMENTED;
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
        a->MouseIdentifier = (USHORT)((HidMouseHasWheel() && WheelMode != WHEEL_CHANNEL) ?
                                      WHEELMOUSE_I8042_HARDWARE : MOUSE_I8042_HARDWARE);
        a->NumberOfButtons = 3;
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
        h->Running = hc->Running;
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
        o->LastLba = si.LastLba;
        o->BlockSize = si.BlockSize;
        o->MaxTransfer = si.MaxXfer;
        RtlMoveMemory(o->Vendor, si.Inquiry + 8, 8);
        RtlMoveMemory(o->Product, si.Inquiry + 16, 16);
        RtlMoveMemory(o->Revision, si.Inquiry + 32, 4);
        o->Identity = si.Identity;
    }
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
        }
    }
    IoReleaseCancelSpinLock(irql);
}

void NtCtlCleanup(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    KIRQL irql;
    PIRP pend = NULL;

    PIRP chg[MAX_CHANGE_WAITERS];
    int n = 0;
    int i;

    (void)Dev;
    IoAcquireCancelSpinLock(&irql);
    if (WheelIrp != NULL && IoGetCurrentIrpStackLocation(WheelIrp)->FileObject == sp->FileObject) {
        pend = WheelIrp;
        WheelIrp = NULL;
        IoSetCancelRoutine(pend, NULL);
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
        if (WheelAccum != 0) {
            *(LONG *)Irp->AssociatedIrp.SystemBuffer = WheelAccum;
            WheelAccum = 0;
            IoReleaseCancelSpinLock(irql);
            Irp->IoStatus.Information = sizeof(LONG);
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
    case IOCTL_USBNT_QUERY_TREE:
        if (outLen < sizeof(USBNT_TREE)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        FillTree((USBNT_TREE *)Irp->AssociatedIrp.SystemBuffer);
        Irp->IoStatus.Information = sizeof(USBNT_TREE);
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
