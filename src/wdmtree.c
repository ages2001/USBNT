/*
 * wdmtree.c - Windows 2000 / XP: what Device Manager shows below USBNT's
 * host controllers, the way Windows' own USB stack shows it:
 *
 *   USB host controller (USBNT's function device; its friendly name says
 *   which controller it is: "Renesas uPD720202 USB 3.0 Host Controller
 *   (USBNT)")
 *     USB 3.x Root Hub (USBNT)                 PDO_HUB, USBNT\ROOT_HUB
 *       USB 2.0 Hub (USBNT)                    PDO_HUB, USBNT\VID_v&PID_p
 *         USB Keyboard (product name)          PDO_UDEV, USBNT\VID_v&PID_p
 *       USB Mass Storage Device (product name) PDO_UDEV
 *
 * The nodes stand for the USB devices USBNT drives itself (hubs, keyboards,
 * mice, disks, audio, cameras) and for devices nobody drives; a device on a
 * PDO for Windows' own USB drivers (usbdi.c) is shown by that PDO instead.
 * usbnt.sys is the function driver of the nodes (usbnt.inf: USBNT\HUB,
 * USBNT\DEVICE, USB class; after text-mode setup the CriticalDeviceDatabase
 * entries wdmbus.c writes): a hub node's function device reports the hubs and
 * devices on its ports.  What the devices do stays where it was: the keyboards
 * and mice behind "USBNT USB Keyboard" / "USBNT USB Mouse", the disks under
 * Disk drives (wdmbus.c).  The nodes need no resources, may go at any time
 * (SurpriseRemovalOK: no "unsafe removal" message) and carry the names the
 * driver gives them as their FriendlyName.
 */

#include "wdm.h"

#define TREE_TAG        0x54425355UL            /* "USBT" */


static WDM_EXT *Nodes;
static KSPIN_LOCK TreeLock;
static BOOLEAN TreeInit;
static ULONG TreeSig;

static void Init(void)
{
    if (!TreeInit) {
        KeInitializeSpinLock(&TreeLock);
        TreeInit = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Strings                                                              */
/* ------------------------------------------------------------------ */

static WCHAR *WCat(WCHAR *D, const WCHAR *S)
{
    while (*S) {
        *D++ = *S++;
    }
    *D = 0;
    return D;
}

static WCHAR *ACat(WCHAR *D, const char *S, int Max)
{
    int i, last = -1;

    for (i = 0; i < Max && S[i]; i++) {
        if ((UCHAR)S[i] > ' ') {
            last = i;
        }
    }
    for (i = 0; i <= last; i++) {
        UCHAR c = (UCHAR)S[i];
        *D++ = (WCHAR)((c < ' ' || c >= 0x7F) ? L' ' : c);
    }
    *D = 0;
    return D;
}

static WCHAR *HexCat(WCHAR *D, ULONG V, int Digits)
{
    int i;

    for (i = Digits - 1; i >= 0; i--) {
        ULONG n = (V >> (4 * i)) & 15;
        *D++ = (WCHAR)(n < 10 ? L'0' + n : L'A' + n - 10);
    }
    *D = 0;
    return D;
}

static WCHAR *DecCat(WCHAR *D, ULONG V)
{
    WCHAR t[12];
    int n = 0;

    do {
        t[n++] = (WCHAR)(L'0' + V % 10);
        V /= 10;
    } while (V != 0);
    while (n > 0) {
        *D++ = t[--n];
    }
    *D = 0;
    return D;
}

static PWCHAR Pool(const WCHAR *S, ULONG Chars)
{
    PWCHAR p = (PWCHAR)ExAllocatePoolWithTag(PagedPool, (Chars + 1) * sizeof(WCHAR), TREE_TAG);

    if (p != NULL) {
        RtlCopyMemory(p, S, Chars * sizeof(WCHAR));
        p[Chars] = 0;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Names                                                                */
/* ------------------------------------------------------------------ */

static const struct {
    USHORT Ven, Dev;
    UCHAR MinRev, MaxRev;
    const WCHAR *Name;
} HcNames[] = {
    /* the add-in card / onboard USB 3 controllers (PCI ids as Linux' pci.ids and drivers have them) */
    { 0x1033, 0x0194, 0x00, 0x03, L"Renesas (NEC) uPD720200 USB 3.0 Host Controller" },
    { 0x1033, 0x0194, 0x04, 0xFF, L"Renesas (NEC) uPD720200A USB 3.0 Host Controller" },
    { 0x1912, 0x0014, 0x00, 0xFF, L"Renesas uPD720201 USB 3.0 Host Controller" },
    { 0x1912, 0x0015, 0x00, 0xFF, L"Renesas uPD720202 USB 3.0 Host Controller" },
    { 0x1B21, 0x1042, 0x00, 0xFF, L"ASMedia ASM1042 USB 3.0 Host Controller" },
    { 0x1B21, 0x1142, 0x00, 0xFF, L"ASMedia ASM1042A USB 3.0 Host Controller" },
    { 0x1B21, 0x1242, 0x00, 0xFF, L"ASMedia ASM1142 USB 3.1 Host Controller" },
    { 0x1B21, 0x2142, 0x00, 0xFF, L"ASMedia ASM2142 USB 3.1 Host Controller" },
    { 0x1B21, 0x3242, 0x00, 0xFF, L"ASMedia ASM3242 USB 3.2 Host Controller" },
    { 0x1106, 0x3432, 0x00, 0xFF, L"VIA VL800 / VL801 USB 3.0 Host Controller" },
    { 0x1106, 0x3483, 0x00, 0xFF, L"VIA VL805 / VL806 USB 3.0 Host Controller" },
    { 0x1B6F, 0x7023, 0x00, 0xFF, L"Etron EJ168 USB 3.0 Host Controller" },
    { 0x1B6F, 0x7052, 0x00, 0xFF, L"Etron EJ188 / EJ198 USB 3.0 Host Controller" },
    { 0x1B73, 0x1000, 0x00, 0xFF, L"Fresco Logic FL1000 USB 3.0 Host Controller" },
    { 0x1B73, 0x1009, 0x00, 0xFF, L"Fresco Logic FL1009 USB 3.0 Host Controller" },
    { 0x1B73, 0x1100, 0x00, 0xFF, L"Fresco Logic FL1100 USB 3.0 Host Controller" },
    { 0x104C, 0x8241, 0x00, 0xFF, L"Texas Instruments TUSB7340 USB 3.0 Host Controller" },
    { 0x1B36, 0x000D, 0x00, 0xFF, L"QEMU USB 3.0 Host Controller" },
};

static const struct {
    USHORT Ven;
    const WCHAR *Name;
} Vendors[] = {
    { 0x8086, L"Intel" }, { 0x1022, L"AMD" }, { 0x1002, L"ATI" }, { 0x10DE, L"NVIDIA" }, { 0x1106, L"VIA" },
    { 0x1039, L"SiS" }, { 0x10B9, L"ALi" }, { 0x1033, L"NEC" }, { 0x1912, L"Renesas" }, { 0x1B21, L"ASMedia" },
    { 0x1B6F, L"Etron" }, { 0x1B73, L"Fresco Logic" }, { 0x104C, L"Texas Instruments" }, { 0x1180, L"Ricoh" },
    { 0x14E4, L"Broadcom" }, { 0x1B36, L"QEMU" }, { 0x1D94, L"Hygon" }, { 0x1283, L"ITE" }, { 0x197B, L"JMicron" },
};

/* "Renesas uPD720202 USB 3.0 Host Controller (USBNT)", "Intel USB 2.0 Enhanced Host Controller (USBNT)" */
static void HcName(USB_HC *H, WCHAR *Out)
{
    UCHAR rev = (UCHAR)OsPciRead(H->Bus, H->Dev, H->Fn, 8, 1);
    WCHAR *p = Out;
    ULONG i;

    *p = 0;
    for (i = 0; i < sizeof(HcNames) / sizeof(HcNames[0]); i++) {
        if (HcNames[i].Ven == H->VendorId && HcNames[i].Dev == H->DeviceId && rev >= HcNames[i].MinRev &&
            rev <= HcNames[i].MaxRev) {
            p = WCat(p, HcNames[i].Name);
            break;
        }
    }
    if (Out[0] == 0) {
        for (i = 0; i < sizeof(Vendors) / sizeof(Vendors[0]); i++) {
            if (Vendors[i].Ven == H->VendorId) {
                p = WCat(p, Vendors[i].Name);
                p = WCat(p, L" ");
                break;
            }
        }
        p = WCat(p, H->Type == HC_XHCI ? L"USB 3.x eXtensible Host Controller" :
                    H->Type == HC_EHCI ? L"USB 2.0 Enhanced Host Controller" :
                    H->Type == HC_OHCI ? L"USB Open Host Controller" : L"USB Universal Host Controller");
    }
    WCat(p, L" (USBNT)");
}

static const WCHAR *DevKind(USB_DEV *D)
{
    ULONG f = D->Function & (USB_FUNC_KEYBOARD | USB_FUNC_MOUSE | USB_FUNC_TABLET | USB_FUNC_STORAGE |
                             USB_FUNC_AUDIO | USB_FUNC_MIDI | USB_FUNC_VIDEO);

    if (f == 0) {
        return D->NativeIf ? L"USB Device" : L"USB Device (no driver)";
    }
    if ((f & ~(USB_FUNC_KEYBOARD | USB_FUNC_MOUSE | USB_FUNC_TABLET)) == 0) {
        if (f == USB_FUNC_KEYBOARD) return L"USB Keyboard";
        if (f == USB_FUNC_MOUSE) return L"USB Mouse";
        if (f == USB_FUNC_TABLET) return L"USB Tablet";
        return L"USB Input Device";
    }
    if (f == USB_FUNC_STORAGE) return L"USB Mass Storage Device";
    if ((f & ~(USB_FUNC_AUDIO | USB_FUNC_MIDI)) == 0) return L"USB Audio Device";
    if (f == USB_FUNC_VIDEO || f == (USB_FUNC_VIDEO | USB_FUNC_AUDIO)) return L"USB Video Device";
    return L"USB Composite Device";
}

static USB_HC *HcOf(PDEVICE_OBJECT Fdo)
{
    WDM_EXT *h = (WDM_EXT *)Fdo->DeviceExtension;
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *c = UsbHcs[i];
        if (c != NULL && !c->Removed && c->Bus == h->Bus && c->Dev == h->Dev && c->Fn == h->Fn) {
            return c;
        }
    }
    return NULL;
}

/* the controller a node's devices hang off */
static USB_HC *NodeHc(WDM_EXT *x)
{
    if (x->TDev != NULL) {
        return x->TDev->Hc;
    }
    return x->TOwner != NULL ? HcOf(x->TOwner) : NULL;
}

static void NodeName(WDM_EXT *x, WCHAR *Out)
{
    WCHAR *p = Out;
    USB_DEV *d = x->TDev;

    *p = 0;
    if (x->Child == PDO_HUB && d == NULL) {
        USB_HC *h = NodeHc(x);
        p = WCat(p, h == NULL ? L"USB Root Hub" : h->Type == HC_XHCI ? L"USB 3.x Root Hub" :
                    h->Type == HC_EHCI ? L"USB 2.0 Root Hub" : L"USB Root Hub");
        WCat(p, L" (USBNT)");
        return;
    }
    if (d == NULL) {
        WCat(p, L"USB Device");
        return;
    }
    if (x->Child == PDO_HUB) {
        p = WCat(p, d->Speed == USB_SPEED_SUPER ? L"USB 3.x Hub" : d->Speed == USB_SPEED_HIGH ? L"USB 2.0 Hub" :
                    L"USB Hub");
        WCat(p, L" (USBNT)");
        return;
    }
    p = WCat(p, DevKind(d));
    if (d->Product[0] || d->Mfg[0]) {
        p = WCat(p, L" (");
        if (d->Product[0]) {
            p = ACat(p, d->Product, sizeof(d->Product));
        } else {
            p = ACat(p, d->Mfg, sizeof(d->Mfg));
        }
        WCat(p, L")");
    }
}

/* the instance key of Pdo's device node ("...\Enum\PCI\VEN_..\3&..."): the parent of the key
   IoOpenDeviceRegistryKey(PLUGPLAY_REGKEY_DEVICE) opens ("...\<instance>\Device Parameters") */
/* the number of path parts after "\\Enum\\" in the key name N (C characters): an instance key has 3
   (enumerator, device, instance); 0 when there is no "\\Enum\\" */
static ULONG EnumDepth(const WCHAR *N, ULONG C)
{
    static const WCHAR e[] = L"\\ENUM\\";
    ULONG i, j, parts = 0;

    for (i = 0; i + 6 <= C; i++) {
        for (j = 0; j < 6; j++) {
            WCHAR a = N[i + j];
            if (a >= L'a' && a <= L'z') a = (WCHAR)(a - 32);
            if (a != e[j]) break;
        }
        if (j == 6) {
            parts = 1;
            for (j = i + 6; j < C; j++) {
                if (N[j] == L'\\') parts++;
            }
            return (i + 6 < C) ? parts : 0;
        }
    }
    return 0;
}

/* before Plug and Play has made a device node's instance key, IoOpenDeviceRegistryKey opens a
   key that is not below one ("Enum\\Device Parameters"): only an instance key's is taken */
static HANDLE OpenInstance(PDEVICE_OBJECT Pdo)
{
    HANDLE h = NULL, inst = NULL;
    PVOID obj = NULL;
    ULONG len = 0;
    POBJECT_NAME_INFORMATION ni;
    OBJECT_ATTRIBUTES oa;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !NT_SUCCESS(IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &h))) {
        return NULL;
    }
    ni = (POBJECT_NAME_INFORMATION)ExAllocatePoolWithTag(PagedPool, 1024, TREE_TAG);
    if (ni != NULL && NT_SUCCESS(ObReferenceObjectByHandle(h, KEY_READ, NULL, KernelMode, &obj, NULL))) {
        if (NT_SUCCESS(ObQueryNameString(obj, ni, 1024 - sizeof(WCHAR), &len)) && ni->Name.Buffer != NULL) {
            USHORT c = (USHORT)(ni->Name.Length / sizeof(WCHAR));
            while (c > 0 && ni->Name.Buffer[c - 1] != L'\\') {
                c--;
            }
            if (c > 1 && EnumDepth(ni->Name.Buffer, c - 1) == 3) {
                ni->Name.Length = (USHORT)((c - 1) * sizeof(WCHAR));
                InitializeObjectAttributes(&oa, &ni->Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
                if (!NT_SUCCESS(ZwOpenKey(&inst, KEY_ALL_ACCESS, &oa))) {
                    inst = NULL;
                }
            }
        }
        ObDereferenceObject(obj);
    }
    if (ni != NULL) {
        ExFreePool(ni);
    }
    ZwClose(h);
    return inst;
}

static void SetSzValue(HANDLE K, const WCHAR *Name, const WCHAR *Val)
{
    UNICODE_STRING vn;
    ULONG n = 0;

    while (Val[n]) n++;
    RtlInitUnicodeString(&vn, Name);
    ZwSetValueKey(K, &vn, 0, REG_SZ, (PVOID)Val, (n + 1) * sizeof(WCHAR));
}

/* FriendlyName of the device node of Pdo (what Device Manager shows) */
void WdmSetFriendlyName(PDEVICE_OBJECT Pdo, const WCHAR *Name)
{
    HANDLE k = OpenInstance(Pdo);

    if (k != NULL) {
        SetSzValue(k, L"FriendlyName", Name);
        ZwClose(k);
    }
}

/* a USB host controller that text-mode setup installed (CriticalDeviceDatabase, no INF) is in
   the SCSI adapter class: Device Manager lists it with the USB controllers again */
static void UsbClassIfScsi(PDEVICE_OBJECT Pdo)
{
    static const WCHAR scsi[] = L"{4D36E97B-E325-11CE-BFC1-08002BE10318}";
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    UNICODE_STRING vn;
    ULONG len = 0, i;
    HANDLE k = OpenInstance(Pdo);
    BOOLEAN same = TRUE;

    if (k == NULL) {
        return;
    }
    RtlZeroMemory(buf, sizeof(buf));
    RtlInitUnicodeString(&vn, L"ClassGUID");
    if (NT_SUCCESS(ZwQueryValueKey(k, &vn, KeyValuePartialInformation, buf, sizeof(buf) - 4, &len)) &&
        v->Type == REG_SZ && v->DataLength >= 38 * sizeof(WCHAR)) {
        for (i = 0; i < 38 && same; i++) {
            WCHAR a = ((const WCHAR *)v->Data)[i], b = scsi[i];
            if (a >= L'a' && a <= L'z') a = (WCHAR)(a - 32);
            same = (BOOLEAN)(a == b);
        }
        RtlInitUnicodeString(&vn, L"Driver");
        if (same && !NT_SUCCESS(ZwQueryValueKey(k, &vn, KeyValuePartialInformation, buf, sizeof(buf) - 4, &len))) {
            SetSzValue(k, L"ClassGUID", L"{36FC9E60-C465-11CF-8056-444553540000}");
            SetSzValue(k, L"Class", L"USB");
            UsbLog(LOG_INFO, "PnP: the host controller is a USB controller for Device Manager again (was SCSI)\n");
        }
    }
    ZwClose(k);
}

/* ------------------------------------------------------------------ */
/* Nodes                                                                */
/* ------------------------------------------------------------------ */

/* a device to show: USBNT drives it (or a part of it), or nobody does */
static BOOLEAN Visible(USB_DEV *D)
{
    if (!D->InUse || D->Gone || !D->Ready) {
        return FALSE;
    }
    return (BOOLEAN)(D->IsHub || D->NativeIf != 0 || !UsbdiHasDevice(D));
}

static WDM_EXT *NewNode(ULONG Child, PDEVICE_OBJECT Owner, PDEVICE_OBJECT Parent, USB_DEV *D)
{
    PDEVICE_OBJECT pdo;
    WDM_EXT *x;
    KIRQL irql;

    if (!NT_SUCCESS(IoCreateDevice(WdmDriver, sizeof(WDM_EXT), NULL,
                                   Child == PDO_HUB ? FILE_DEVICE_BUS_EXTENDER : FILE_DEVICE_UNKNOWN,
                                   FILE_AUTOGENERATED_DEVICE_NAME | FILE_DEVICE_SECURE_OPEN, FALSE, &pdo))) {
        return NULL;
    }
    x = (WDM_EXT *)pdo->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Nt.Kind = EXT_PDO;
    x->Nt.Self = pdo;
    x->Child = Child;
    x->Slot = -1;
    x->TOwner = Owner;
    x->Pdo = Parent;                            /* the hub node that reports it (NULL: a root hub) */
    x->TDev = D;
    x->TGen = D != NULL ? D->Gen : 0;
    x->TPort = D != NULL ? D->Port : 0;
    pdo->Flags |= DO_POWER_PAGABLE;
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    KeAcquireSpinLock(&TreeLock, &irql);
    x->TNext = Nodes;
    Nodes = x;
    KeReleaseSpinLock(&TreeLock, irql);
    return x;
}

static void FreeNode(WDM_EXT *x);
static void Preinstall(WDM_EXT *x);

/* the nodes x reported that Plug and Play has removed already go with it */
static void FreeChildren(PDEVICE_OBJECT Parent)
{
    WDM_EXT *c;
    KIRQL irql;
    BOOLEAN again = TRUE;

    while (again) {
        again = FALSE;
        KeAcquireSpinLock(&TreeLock, &irql);
        for (c = Nodes; c != NULL; c = c->TNext) {
            if (c->Pdo == Parent) {
                c->Reported = FALSE;
                if (c->Removed) {
                    break;
                }
            }
        }
        KeReleaseSpinLock(&TreeLock, irql);
        if (c != NULL) {
            FreeNode(c);
            again = TRUE;
        }
    }
}

static void FreeNode(WDM_EXT *x)
{
    WDM_EXT **pp;
    KIRQL irql;

    KeAcquireSpinLock(&TreeLock, &irql);
    for (pp = &Nodes; *pp != NULL; pp = &(*pp)->TNext) {
        if (*pp == x) {
            *pp = x->TNext;
            break;
        }
    }
    KeReleaseSpinLock(&TreeLock, irql);
    FreeChildren(x->Nt.Self);
    IoDeleteDevice(x->Nt.Self);
}

/* the relations in Irp (from a lower driver, or none) plus Add[] */
static NTSTATUS Merge(PIRP Irp, WDM_EXT **Add, ULONG N)
{
    PDEVICE_RELATIONS old = (PDEVICE_RELATIONS)Irp->IoStatus.Information, rel;
    ULONG keep = old != NULL ? old->Count : 0, i;

    rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) +
                                                   (keep + N) * sizeof(PDEVICE_OBJECT), TREE_TAG);
    if (rel == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    rel->Count = 0;
    for (i = 0; i < keep; i++) {
        rel->Objects[rel->Count++] = old->Objects[i];
    }
    for (i = 0; i < N; i++) {
        ObReferenceObject(Add[i]->Nt.Self);
        rel->Objects[rel->Count++] = Add[i]->Nt.Self;
    }
    if (old != NULL) {
        ExFreePool(old);
    }
    Irp->IoStatus.Information = (ULONG_PTR)rel;
    return STATUS_SUCCESS;
}

/* a host controller's bus relations: its root hub */
NTSTATUS WdmTreeHcRelations(WDM_EXT *Hc, PIRP Irp)
{
    USB_HC *h;
    WDM_EXT *x, *root = NULL;
    KIRQL irql;

    Init();
    h = Hc->Bus == 0xFF ? NULL : HcOf(Hc->Nt.Self);
    KeAcquireSpinLock(&TreeLock, &irql);
    for (x = Nodes; x != NULL; x = x->TNext) {
        if (x->TOwner == Hc->Nt.Self && x->TDev == NULL) {
            x->Reported = FALSE;
            if (root == NULL && h != NULL && h->Running && Hc->Started && !x->Gone) {
                root = x;
                x->Reported = TRUE;
            }
        }
    }
    KeReleaseSpinLock(&TreeLock, irql);
    if (h == NULL || !h->Running || !Hc->Started) {
        return STATUS_SUCCESS;                  /* no USB host controller (card reader, I2C ...) or not running */
    }
    if (root == NULL && (root = NewNode(PDO_HUB, Hc->Nt.Self, NULL, NULL)) != NULL) {
        root->Reported = TRUE;
        Preinstall(root);
    }
    return root != NULL ? Merge(Irp, &root, 1) : STATUS_SUCCESS;
}

/* a hub node's bus relations: the hubs and devices on its ports */
static NTSTATUS HubRelations(WDM_EXT *Hub, PIRP Irp)
{
    WDM_EXT *x, *add[USB_MAX_DEVICES];
    USB_HC *hc = NodeHc(Hub);
    ULONG n = 0, i;
    KIRQL irql;

    KeAcquireSpinLock(&TreeLock, &irql);
    for (x = Nodes; x != NULL; x = x->TNext) {
        if (x->Pdo == Hub->Nt.Self) {
            x->Reported = FALSE;
        }
    }
    KeReleaseSpinLock(&TreeLock, irql);
    if (hc == NULL || (Hub->TDev != NULL && (Hub->TDev->Gen != Hub->TGen || !Hub->TDev->InUse ||
                                             Hub->TDev->Gone))) {
        return STATUS_SUCCESS;
    }
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        if (d->Hc != hc || d->Parent != Hub->TDev || !Visible(d)) {
            continue;
        }
        KeAcquireSpinLock(&TreeLock, &irql);
        for (x = Nodes; x != NULL; x = x->TNext) {
            if (x->Pdo == Hub->Nt.Self && x->TDev == d && x->TGen == d->Gen && !x->Gone) {
                break;
            }
        }
        if (x != NULL) {
            x->Reported = TRUE;
        }
        KeReleaseSpinLock(&TreeLock, irql);
        if (x == NULL && (x = NewNode(d->IsHub ? PDO_HUB : PDO_UDEV, NULL, Hub->Nt.Self, d)) != NULL) {
            x->Reported = TRUE;
            Preinstall(x);
        }
        if (x != NULL && n < USB_MAX_DEVICES) {
            add[n++] = x;
        }
    }
    return Merge(Irp, add, n);
}

void WdmTreeHcStarted(WDM_EXT *Hc)
{
    USB_HC *h = HcOf(Hc->Nt.Self);
    WCHAR name[128];

    if (h == NULL) {
        return;
    }
    HcName(h, name);
    WdmSetFriendlyName(Hc->Pdo, name);
    UsbClassIfScsi(Hc->Pdo);
}

void WdmTreeHcGone(WDM_EXT *Hc)
{
    WDM_EXT *x;
    KIRQL irql;
    BOOLEAN again = TRUE;

    if (!TreeInit) {
        return;
    }
    while (again) {
        again = FALSE;
        KeAcquireSpinLock(&TreeLock, &irql);
        for (x = Nodes; x != NULL; x = x->TNext) {
            if (x->TOwner == Hc->Nt.Self) {
                x->Reported = FALSE;
                if (x->Removed) {
                    break;
                }
            }
        }
        KeReleaseSpinLock(&TreeLock, irql);
        if (x != NULL) {
            FreeNode(x);
            again = TRUE;
        }
    }
}

/* worker: when the hubs and devices changed, Plug and Play asks every hub node (and controller) again */
void WdmTreeService(void)
{
    ULONG sig = 0x811C9DC5UL, i;
    WDM_EXT *x;
    PDEVICE_OBJECT list[USB_MAX_DEVICES + 8];
    ULONG n = 0;
    KIRQL irql;

    Init();
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        if (Visible(d)) {
            sig = (sig ^ (i + 1)) * 16777619UL;
            sig = (sig ^ d->Gen) * 16777619UL;
            sig = (sig ^ (ULONG)(ULONG_PTR)d->Parent) * 16777619UL;
            sig = (sig ^ (ULONG)(ULONG_PTR)d->Hc) * 16777619UL;
        }
    }
    for (i = 0; i < (ULONG)UsbHcCount; i++) {
        if (UsbHcs[i] != NULL) {
            sig = (sig ^ (UsbHcs[i]->Running + 2 * UsbHcs[i]->Removed + 7 * i)) * 16777619UL;
        }
    }
    if (sig == TreeSig) {
        return;
    }
    TreeSig = sig;
    KeAcquireSpinLock(&TreeLock, &irql);
    for (x = Nodes; x != NULL && n < USB_MAX_DEVICES + 8; x = x->TNext) {
        if (x->Child == PDO_HUB && x->Started && x->Reported) {
            ObReferenceObject(x->Nt.Self);
            list[n++] = x->Nt.Self;
        }
    }
    KeReleaseSpinLock(&TreeLock, irql);
    for (i = 0; i < n; i++) {
        IoInvalidateDeviceRelations(list[i], BusRelations);
        ObDereferenceObject(list[i]);
    }
    WdmInvalidateHcs();
}

/* ------------------------------------------------------------------ */
/* The nodes: Plug and Play (PDO)                                       */
/* ------------------------------------------------------------------ */

/* the node's ids into Buf (a string, or a list ending in two zeros); the end, or NULL */
static WCHAR *BuildId(WDM_EXT *x, BUS_QUERY_ID_TYPE Type, WCHAR *Buf)
{
    WCHAR *p = Buf;
    USB_DEV *d = x->TDev;
    USHORT vid = 0, pid = 0, rev = 0;
    UCHAR cls = 0;

    if (d != NULL) {
        vid = (USHORT)(d->DevDesc[8] | (d->DevDesc[9] << 8));
        pid = (USHORT)(d->DevDesc[10] | (d->DevDesc[11] << 8));
        rev = (USHORT)(d->DevDesc[12] | (d->DevDesc[13] << 8));
        cls = d->DevDesc[4];
        if (cls == 0 && d->Config != NULL && d->ConfigLen > 9 + 5) {
            cls = d->Config[9 + 5];             /* the first interface's class */
        }
    }
    switch (Type) {
    case BusQueryDeviceID:
        if (d == NULL) {
            p = WCat(p, L"USBNT\\ROOT_HUB");
        } else {
            p = WCat(p, L"USBNT\\VID_");
            p = HexCat(p, vid, 4);
            p = WCat(p, L"&PID_");
            p = HexCat(p, pid, 4);
        }
        break;
    case BusQueryHardwareIDs:
        if (d == NULL) {
            USB_HC *h = NodeHc(x);
            p = WCat(p, L"USBNT\\ROOT_HUB&VID");
            p = HexCat(p, h != NULL ? h->VendorId : 0, 4);
            p = WCat(p, L"&PID");
            p = HexCat(p, h != NULL ? h->DeviceId : 0, 4) + 1;
            p = WCat(p, L"USBNT\\ROOT_HUB") + 1;
        } else {
            p = WCat(p, L"USBNT\\VID_");
            p = HexCat(p, vid, 4);
            p = WCat(p, L"&PID_");
            p = HexCat(p, pid, 4);
            p = WCat(p, L"&REV_");
            p = HexCat(p, rev, 4) + 1;
            p = WCat(p, L"USBNT\\VID_");
            p = HexCat(p, vid, 4);
            p = WCat(p, L"&PID_");
            p = HexCat(p, pid, 4) + 1;
        }
        break;
    case BusQueryCompatibleIDs:
        if (x->Child == PDO_HUB) {
            p = WCat(p, L"USBNT\\HUB") + 1;
        } else {
            p = WCat(p, L"USBNT\\Class_");
            p = HexCat(p, cls, 2) + 1;
            p = WCat(p, L"USBNT\\DEVICE") + 1;
        }
        break;
    case BusQueryInstanceID: {
        /* unique in the system (UniqueID): the controller's PCI address and the port path,
           so usbnt.sys can make the device node itself (Preinstall) */
        UCHAR ports[8];
        int n = 0;
        USB_DEV *e;
        USB_HC *h = NodeHc(x);
        p = HexCat(p, h != NULL ? h->Bus : 0, 2);
        p = HexCat(p, h != NULL ? h->Dev : 0, 2);
        p = HexCat(p, h != NULL ? h->Fn : 0, 1);
        for (e = d; e != NULL && n < 8; e = e->Parent) {
            ports[n++] = e->Port;
        }
        while (n > 0) {
            p = WCat(p, L"_");
            p = DecCat(p, ports[--n]);
        }
        break;
    }
    default:
        return NULL;
    }
    return p + 1;                               /* the final 0 of a list (or of a string) */
}

static NTSTATUS QueryId(WDM_EXT *x, PIRP Irp, BUS_QUERY_ID_TYPE Type)
{
    WCHAR buf[256], *e;

    RtlZeroMemory(buf, sizeof(buf));
    if ((e = BuildId(x, Type, buf)) == NULL) {
        return Irp->IoStatus.Status;
    }
    Irp->IoStatus.Information = (ULONG_PTR)Pool(buf, (ULONG)(e - buf));
    return Irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

/* The device node of a new node, made before Plug and Play hears of it: usbnt is its function
   driver, USB class, installed - so Windows starts it at once, also where no INF was installed
   (text-mode setup) and without a "Found New Hardware" for every hub and device, which an
   unsigned driver would make ask each time. */
static HANDLE KeyCreate(HANDLE Parent, const WCHAR *Name, ULONG Chars, BOOLEAN *New)
{
    OBJECT_ATTRIBUTES oa;
    UNICODE_STRING u;
    HANDLE h = NULL;
    ULONG disp = 0;

    u.Buffer = (PWCHAR)Name;
    u.Length = u.MaximumLength = (USHORT)(Chars * sizeof(WCHAR));
    InitializeObjectAttributes(&oa, &u, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Parent, NULL);
    if (!NT_SUCCESS(ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp))) {
        return NULL;
    }
    if (New != NULL) {
        *New = (BOOLEAN)(disp == REG_CREATED_NEW_KEY);
    }
    return h;
}

static void PutSz(HANDLE K, const WCHAR *Name, const WCHAR *Val)
{
    UNICODE_STRING n;
    ULONG c = 0;

    while (Val[c]) c++;
    RtlInitUnicodeString(&n, Name);
    ZwSetValueKey(K, &n, 0, REG_SZ, (PVOID)Val, (c + 1) * sizeof(WCHAR));
}

static void Preinstall(WDM_EXT *x)
{
    WCHAR dev[96], inst[64], name[160];
    WCHAR *e;
    HANDLE enm, k1, k2, k3;
    UNICODE_STRING path, vn;
    OBJECT_ATTRIBUTES oa;
    ULONG zero = 0, len = 0, i, sep;
    BOOLEAN isNew = FALSE;
    UCHAR vb[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 32];

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    RtlZeroMemory(dev, sizeof(dev));
    RtlZeroMemory(inst, sizeof(inst));
    if (BuildId(x, BusQueryDeviceID, dev) == NULL || (e = BuildId(x, BusQueryInstanceID, inst)) == NULL) {
        return;
    }
    for (sep = 0; dev[sep] && dev[sep] != L'\\'; sep++) {
    }
    if (dev[sep] != L'\\') {
        return;
    }
    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum");
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&enm, KEY_ALL_ACCESS, &oa))) {
        return;
    }
    for (i = sep + 1; dev[i]; i++) {
    }
    k1 = KeyCreate(enm, dev, sep, NULL);                                    /* USBNT */
    k2 = k1 != NULL ? KeyCreate(k1, dev + sep + 1, i - sep - 1, NULL) : NULL;   /* VID_v&PID_p */
    for (i = 0; inst[i]; i++) {
    }
    k3 = k2 != NULL ? KeyCreate(k2, inst, i, &isNew) : NULL;
    if (k3 != NULL) {
        RtlInitUnicodeString(&vn, L"Service");
        if (isNew || !NT_SUCCESS(ZwQueryValueKey(k3, &vn, KeyValuePartialInformation, vb, sizeof(vb), &len))) {
            NodeName(x, name);
            PutSz(k3, L"Service", L"usbnt");
            PutSz(k3, L"ClassGUID", L"{36FC9E60-C465-11CF-8056-444553540000}");
            PutSz(k3, L"Class", L"USB");
            PutSz(k3, L"Mfg", L"USBNT");
            PutSz(k3, L"DeviceDesc", name);
            RtlInitUnicodeString(&vn, L"ConfigFlags");
            ZwSetValueKey(k3, &vn, 0, REG_DWORD, &zero, sizeof(zero));
        }
        ZwClose(k3);
    }
    if (k2 != NULL) ZwClose(k2);
    if (k1 != NULL) ZwClose(k1);
    ZwClose(enm);
}

/*
 * The keyboard / mouse device node gets its class (Keyboard / Mouse) before Plug and Play
 * builds its driver stack, when it has none: the class's UpperFilters (kbdclass / mouclass)
 * come onto the device only through it.  Text-mode setup of Windows 2000 / XP gives a device
 * the service its [HardwareIdsDatabase] names, and the class only when the line has one -
 * nLite and the like write USBNT\Keyboard = "usbnt" from TXTSETUP.OEM, without a class: then
 * no kbdclass would come onto the USB keyboard, and on a machine without a PS/2 keyboard
 * text-mode setup would have no keyboard.  A class an INF gave (GUI setup) is left alone.
 * Called with the device node's first requests after its instance key exists.
 */
/* the instance key's full name ends with "\\<Want>\\<instance>" (Want: "USBNT\\Keyboard"):
   before Plug and Play has made the device node's instance key, IoOpenDeviceRegistryKey
   opens a key of another node (the parent of what it opens is then not an instance key) */
static BOOLEAN IsInstanceOf(HANDLE K, const WCHAR *Want)
{
    UCHAR b[600];
    POBJECT_NAME_INFORMATION ni = (POBJECT_NAME_INFORMATION)b;
    PVOID obj = NULL;
    ULONG len = 0, n, w, i;
    BOOLEAN ok = FALSE;

    for (w = 0; Want[w]; w++) {
    }
    if (!NT_SUCCESS(ObReferenceObjectByHandle(K, KEY_READ, NULL, KernelMode, &obj, NULL))) {
        return FALSE;
    }
    if (NT_SUCCESS(ObQueryNameString(obj, ni, sizeof(b) - sizeof(WCHAR), &len)) && ni->Name.Buffer != NULL) {
        n = ni->Name.Length / sizeof(WCHAR);
        while (n > 0 && ni->Name.Buffer[n - 1] != L'\\') {
            n--;                                /* drops "\<instance>" */
        }
        if (n > w + 1 && ni->Name.Buffer[n - w - 2] == L'\\') {
            ok = TRUE;
            for (i = 0; i < w && ok; i++) {
                WCHAR a = ni->Name.Buffer[n - w - 1 + i], c = Want[i];
                if (a >= L'a' && a <= L'z') a = (WCHAR)(a - 32);
                if (c >= L'a' && c <= L'z') c = (WCHAR)(c - 32);
                ok = (BOOLEAN)(a == c);
            }
        }
    }
    ObDereferenceObject(obj);
    return ok;
}

/* debug: the values of Pdo's instance key */
void WdmDumpInstance(PDEVICE_OBJECT Pdo, const char *Tag)
{
    UCHAR b[512];
    PKEY_VALUE_FULL_INFORMATION v = (PKEY_VALUE_FULL_INFORMATION)b;
    ULONG i, j, len;
    char line[200];
    HANDLE inst;

    if (UsbDebugLevel < LOG_DBG || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if ((inst = OpenInstance(Pdo)) == NULL) {
        UsbLog(LOG_DBG, "%s: no instance key\n", Tag);
        return;
    }
    for (i = 0; i < 40; i++) {
        ULONG n = 0;
        RtlZeroMemory(b, sizeof(b));
        if (!NT_SUCCESS(ZwEnumerateValueKey(inst, i, KeyValueFullInformation, b, sizeof(b) - 4, &len))) {
            break;
        }
        for (j = 0; j < v->NameLength / 2 && n < 40; j++) {
            line[n++] = (char)v->Name[j];
        }
        line[n++] = '=';
        if (v->Type == REG_SZ || v->Type == REG_MULTI_SZ) {
            WCHAR *d = (WCHAR *)(b + v->DataOffset);
            for (j = 0; j < v->DataLength / 2 && n < 190; j++) {
                line[n++] = d[j] ? (char)d[j] : '|';
            }
        } else if (v->Type == REG_DWORD) {
            ULONG d = *(ULONG *)(b + v->DataOffset), k;
            for (k = 0; k < 8; k++) {
                line[n++] = "0123456789abcdef"[(d >> (28 - 4 * k)) & 15];
            }
        } else {
            line[n++] = '?';
        }
        line[n] = 0;
        UsbLog(LOG_DBG, "%s: %s\n", Tag, line);
    }
    ZwClose(inst);
}

void WdmInputClassNode(WDM_EXT *x)
{
    static const WCHAR *guids[] = { L"{4D36E96B-E325-11CE-BFC1-08002BE10318}", L"{4D36E96F-E325-11CE-BFC1-08002BE10318}" };
    static const WCHAR *names[] = { L"Keyboard", L"Mouse" };
    static const WCHAR *ids[] = { L"USBNT\\Keyboard", L"USBNT\\Mouse" };
    int k = (x->Child == PDO_KBD) ? 0 : 1;
    UCHAR vb[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 96];
    UNICODE_STRING vn;
    ULONG len = 0;
    HANDLE inst;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL || (inst = OpenInstance(x->Nt.Self)) == NULL) {
        return;                                 /* no instance key yet: the next request */
    }
    if (!IsInstanceOf(inst, ids[k])) {
        ZwClose(inst);
        return;
    }
    x->ClassSet = 1;
    WdmDumpInstance(x->Nt.Self, "PnP: input node before");
    RtlInitUnicodeString(&vn, L"ClassGUID");
    if (!NT_SUCCESS(ZwQueryValueKey(inst, &vn, KeyValuePartialInformation, vb, sizeof(vb), &len)) ||
        ((PKEY_VALUE_PARTIAL_INFORMATION)vb)->DataLength < 38 * sizeof(WCHAR)) {
        PutSz(inst, L"ClassGUID", guids[k]);
        PutSz(inst, L"Class", names[k]);
        UsbLog(LOG_INFO, "PnP: the %s device node had no class: now %s\n", k ? "mouse" : "keyboard", k ? "Mouse" : "Keyboard");
    }
    /* Windows 2000 text-mode setup with an F6 disk: setup knows no service for it either */
    RtlInitUnicodeString(&vn, L"Service");
    if (!NT_SUCCESS(ZwQueryValueKey(inst, &vn, KeyValuePartialInformation, vb, sizeof(vb), &len))) {
        PutSz(inst, L"Service", L"usbnt");
        UsbLog(LOG_INFO, "PnP: the %s device node had no service: now usbnt\n", k ? "mouse" : "keyboard");
    }
    ZwClose(inst);
}

static NTSTATUS QueryText(WDM_EXT *x, PIRP Irp, DEVICE_TEXT_TYPE Type)
{
    WCHAR buf[160], *p = buf;

    if (Type == DeviceTextDescription) {
        NodeName(x, buf);
        while (*p) p++;
    } else if (Type == DeviceTextLocationInformation && x->TDev != NULL) {
        p = WCat(p, L"Port_#");
        p = DecCat(p, x->TPort);
    } else {
        return Irp->IoStatus.Status;
    }
    Irp->IoStatus.Information = (ULONG_PTR)Pool(buf, (ULONG)(p - buf));
    return Irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

NTSTATUS WdmTreePdoPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = Irp->IoStatus.Status;
    ULONG_PTR info = Irp->IoStatus.Information;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE: {
        WCHAR name[160];
        x->Started = TRUE;
        x->Removed = FALSE;
        NodeName(x, name);
        WdmSetFriendlyName(Dev, name);          /* the INF's name is a generic one */
        st = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_SURPRISE_REMOVAL:
        x->Started = FALSE;
        x->Gone = TRUE;
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        x->Started = FALSE;
        x->Removed = TRUE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        if (!x->Reported) {
            FreeNode(x);
        }
        return STATUS_SUCCESS;
    case IRP_MN_QUERY_CAPABILITIES: {
        PDEVICE_CAPABILITIES c = sp->Parameters.DeviceCapabilities.Capabilities;
        int i;
        if (c->Version != 1 || c->Size < sizeof(DEVICE_CAPABILITIES)) {
            st = STATUS_UNSUCCESSFUL;
            break;
        }
        c->DeviceState[PowerSystemWorking] = PowerDeviceD0;
        for (i = PowerSystemSleeping1; i < PowerSystemMaximum; i++) {
            c->DeviceState[i] = PowerDeviceD3;
        }
        c->DeviceWake = PowerDeviceUnspecified;
        c->SystemWake = PowerSystemUnspecified;
        c->DeviceD1 = c->DeviceD2 = FALSE;
        c->LockSupported = FALSE;
        c->EjectSupported = FALSE;
        c->Removable = (BOOLEAN)(x->TDev != NULL);
        c->DockDevice = FALSE;
        c->UniqueID = TRUE;                     /* controller address + port path (BuildId) */
        c->SilentInstall = TRUE;
        c->RawDeviceOK = TRUE;                  /* starts without a function driver too */
        c->SurpriseRemovalOK = TRUE;            /* unplugging needs no Safely Remove Hardware */
        c->Address = x->TPort;
        c->UINumber = x->TPort;
        st = STATUS_SUCCESS;
        break;
    }
    case IRP_MN_QUERY_ID:
        st = QueryId(x, Irp, sp->Parameters.QueryId.IdType);
        info = Irp->IoStatus.Information;
        break;
    case IRP_MN_QUERY_DEVICE_TEXT:
        st = QueryText(x, Irp, sp->Parameters.QueryDeviceText.DeviceTextType);
        info = Irp->IoStatus.Information;
        break;
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == TargetDeviceRelation) {
            PDEVICE_RELATIONS rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS),
                                                                             TREE_TAG);
            if (rel == NULL) {
                st = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                rel->Count = 1;
                rel->Objects[0] = Dev;
                ObReferenceObject(Dev);
                info = (ULONG_PTR)rel;
                st = STATUS_SUCCESS;
            }
        } else if (sp->Parameters.QueryDeviceRelations.Type == BusRelations && x->Child == PDO_HUB) {
            /* the hub's ports (the function device on it passes the request down) */
            st = HubRelations(x, Irp);
            info = Irp->IoStatus.Information;
        }
        break;
    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION b = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(PagedPool, sizeof(PNP_BUS_INFORMATION),
                                                                             TREE_TAG);
        static const GUID usbnt = { 0x6d8a4f3e, 0x2b1c, 0x4e57, { 0x9a, 0x0d, 0x55, 0x53, 0x42, 0x4e, 0x54, 0x01 } };
        if (b == NULL) {
            st = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            b->BusTypeGuid = usbnt;
            b->LegacyBusType = PNPBus;
            b->BusNumber = 0;
            info = (ULONG_PTR)b;
            st = STATUS_SUCCESS;
        }
        break;
    }
    default:
        break;
    }
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

/* ------------------------------------------------------------------ */
/* The nodes: function device (EXT_NODE, usbnt.inf USBNT\HUB / DEVICE)   */
/* ------------------------------------------------------------------ */

NTSTATUS WdmTreeNodePnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = WdmForwardSync(x->Lower, Irp);
        x->Started = NT_SUCCESS(st);
        return WdmComplete(Irp, st, 0);
    /* IRP_MN_QUERY_DEVICE_RELATIONS (BusRelations of a hub): the PDO below answers, as for a raw hub */
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        FreeChildren(x->Pdo);                   /* (the PDO's children were removed before it) */
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        st = IoCallDriver(x->Lower, Irp);
        IoDetachDevice(x->Lower);
        IoDeleteDevice(Dev);
        return st;
    default:
        break;
    }
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(x->Lower, Irp);
}
