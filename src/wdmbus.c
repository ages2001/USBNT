/*
 * wdmbus.c - the USBNT device bus (Windows 2000 / XP): a root enumerated
 * device (hardware id USBNT\Bus, made by usbinst) - or, where there is no
 * such device (text-mode setup installed USBNT from TXTSETUP.OEM, which can
 * make no root device), the function device of the first host controller
 * that starts - whose children are what the USB devices behind the
 * controllers are to Windows:
 *
 *   USBNT\Keyboard, USBNT\Mouse   kbdclass / mouclass sit on a USBNT function
 *                                  device on them (all keyboards and mice in
 *                                  one, as on NT 4.0)
 *   USBNT\Disk..., GenDisk / GenCdRom
 *                                  one per disk slot with a device in it
 *                                  (USB disk or LUN, card reader slot, NVMe
 *                                  drive); disk.sys / cdrom.sys send their
 *                                  SRBs here, like to usbstor's PDOs
 *
 * Disks come and go with the devices; "Safely Remove Hardware" removes one
 * (the slot is shut down like usbeject does).  disk.sys shows only the first
 * partition of a removable medium, so a medium with several partitions is
 * reported as fixed (RemovableAsFixed, usbmsc.c); disk.sys never looks at a
 * fixed disk's medium again, so such a disk is a device of its own for each
 * medium: when the medium goes or changes, the PDO goes and a new one comes.
 * Two devices with one instance id would stop Windows: while Plug and Play
 * has not removed the PDO of the previous medium (a program still has a file
 * of it open), the new one gets the second instance id of the slot's device;
 * if that one is taken too, it waits for a removal.
 */

#include "wdm.h"
#include <ntddstor.h>

#define USBNT_TAG       0x544E5355UL           /* "USNT" */

static PDEVICE_OBJECT BusFdo;                   /* the one USBNT device bus */
static PDEVICE_OBJECT BusPdo;
static PDEVICE_OBJECT BusLower;
static WDM_EXT *Children;                       /* PDOs, list */
static KSPIN_LOCK ChildLock;
static BOOLEAN ChildLockInit;
static ULONG WantIdentity[SCSI_MAX_SLOTS];      /* worker: what each slot should be (0 = no PDO) */
static ULONG SeenIdentity[SCSI_MAX_SLOTS];
static UCHAR WantFixed[SCSI_MAX_SLOTS];         /* ... reported as a fixed disk */
static ULONG WantGen[SCSI_MAX_SLOTS];           /* ... and then with this medium */
static BOOLEAN WaitOld[SCSI_MAX_SLOTS];         /* a new PDO waits for the removal of the old one */
static volatile LONG PdoFreed;
static BOOLEAN BusStarted;
static BOOLEAN RootBus;                         /* a USBNT\Bus device is installed */
static BOOLEAN OnController;                    /* BusFdo is a host controller's function device */
static volatile LONG PendingRelations;          /* slots changed before the bus was there to tell */

/* ------------------------------------------------------------------ */
/* Strings                                                              */
/* ------------------------------------------------------------------ */

/* text-mode setup loads disk.sys / cdrom.sys after the boot drivers; a disk device node that
   comes before them keeps no driver (Plug and Play does not try again when setup loads them):
   there the disks are reported once disk.sys is there */
static BOOLEAN TsWaitClass;
static ULONG TsLastCheck;

static BOOLEAN NtRefDriverLoaded(const WCHAR *Name)
{
    PDRIVER_OBJECT d = NtRefDriver(Name);

    if (d == NULL) {
        return FALSE;
    }
    ObDereferenceObject(d);
    return TRUE;
}

static BOOLEAN TsInput;                 /* text-mode setup: child device nodes made beforehand (TsNode) */
static void TsDisk(struct _WDM_EXT *x);
static WCHAR *BuildChildId(struct _WDM_EXT *x, BUS_QUERY_ID_TYPE Type, WCHAR *buf);

static WCHAR *WAppend(WCHAR *D, const WCHAR *S)
{
    while (*S) {
        *D++ = *S++;
    }
    *D = 0;
    return D;
}

/* SCSI inquiry text: trailing blanks off, characters a device id may hold */
static WCHAR *AAppendId(WCHAR *D, const UCHAR *S, int N)
{
    int i, last = -1;

    for (i = 0; i < N; i++) {
        if (S[i] > ' ' && S[i] < 0x7F) {
            last = i;
        }
    }
    for (i = 0; i <= last; i++) {
        UCHAR c = S[i];
        *D++ = (WCHAR)((c <= ' ' || c >= 0x7F || c == ',') ? L'_' : c);
    }
    *D = 0;
    return D;
}

static WCHAR *AAppendText(WCHAR *D, const UCHAR *S, int N)
{
    int i, last = -1;

    for (i = 0; i < N; i++) {
        if (S[i] > ' ' && S[i] < 0x7F) {
            last = i;
        }
    }
    for (i = 0; i <= last; i++) {
        *D++ = (WCHAR)((S[i] < ' ' || S[i] >= 0x7F) ? L' ' : S[i]);
    }
    *D = 0;
    return D;
}

static WCHAR *HexAppend(WCHAR *D, ULONG V, int Digits)
{
    int i;

    for (i = Digits - 1; i >= 0; i--) {
        ULONG n = (V >> (4 * i)) & 15;
        *D++ = (WCHAR)(n < 10 ? L'0' + n : L'A' + n - 10);
    }
    *D = 0;
    return D;
}

/* a copy in paged pool, what PnP frees */
static PWCHAR PoolString(const WCHAR *S, ULONG Chars)
{
    PWCHAR p = (PWCHAR)ExAllocatePoolWithTag(PagedPool, (Chars + 1) * sizeof(WCHAR), USBNT_TAG);

    if (p != NULL) {
        RtlCopyMemory(p, S, Chars * sizeof(WCHAR));
        p[Chars] = 0;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* Children                                                             */
/* ------------------------------------------------------------------ */

/* the disk PDO stands for what its slot holds now */
static BOOLEAN DiskWanted(WDM_EXT *x)
{
    int i = x->Slot;

    return (BOOLEAN)(x->Child == PDO_DISK && i >= 0 && i < SCSI_MAX_SLOTS && WantIdentity[i] == x->Identity &&
                     WantFixed[i] == x->Fixed && (!x->Fixed || WantGen[i] == x->Gen));
}

BOOLEAN WdmIsOurPdo(PDEVICE_OBJECT Pdo, ULONG *Child)
{
    WDM_EXT *x;

    if (Pdo->DriverObject != WdmDriver) {
        return FALSE;
    }
    x = (WDM_EXT *)Pdo->DeviceExtension;
    if (x->Nt.Kind != EXT_PDO) {
        return FALSE;
    }
    *Child = x->Child;
    return TRUE;
}

static WDM_EXT *NewPdo(ULONG Child, int Slot)
{
    PDEVICE_OBJECT pdo;
    WDM_EXT *x;
    DEVICE_TYPE type = Child == PDO_KBD ? FILE_DEVICE_KEYBOARD : Child == PDO_MOUSE ? FILE_DEVICE_MOUSE :
                       Child == PDO_AUDIO ? FILE_DEVICE_SOUND : Child == PDO_USB ? FILE_DEVICE_UNKNOWN :
                       FILE_DEVICE_MASS_STORAGE;
    KIRQL irql;

    if (!NT_SUCCESS(IoCreateDevice(WdmDriver, sizeof(WDM_EXT), NULL, type, FILE_AUTOGENERATED_DEVICE_NAME |
                                   FILE_DEVICE_SECURE_OPEN, FALSE, &pdo))) {
        return NULL;
    }
    x = (WDM_EXT *)pdo->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Nt.Kind = EXT_PDO;
    x->Nt.Self = pdo;
    x->Child = Child;
    x->Slot = Slot;
    if (Child == PDO_DISK) {
        SCSI_SLOT_INFO si;
        UsbScsiGetSlot(Slot, &si);
        x->Identity = si.Identity ? si.Identity : 0x5A5A0000UL + (ULONG)Slot;
        x->DevType = (UCHAR)(si.Inquiry[0] & 0x1F);
        RtlCopyMemory(x->Inquiry, si.Inquiry, 36);
        x->Proto = si.Protocol;
        x->Fixed = si.Fixed;
        x->Gen = si.Fixed ? si.MediaGen : 0;
        pdo->Flags |= DO_DIRECT_IO;
        if (!x->Fixed) {
            pdo->Characteristics |= FILE_REMOVABLE_MEDIA;
        }
        pdo->AlignmentRequirement = FILE_WORD_ALIGNMENT;
    }
    pdo->Flags |= DO_POWER_PAGABLE;
    pdo->Flags &= ~DO_DEVICE_INITIALIZING;
    KeAcquireSpinLock(&ChildLock, &irql);
    x->NextChild = Children;
    Children = x;
    KeReleaseSpinLock(&ChildLock, irql);
    return x;
}

static void FreePdo(WDM_EXT *x)
{
    WDM_EXT **pp;
    KIRQL irql;

    KeAcquireSpinLock(&ChildLock, &irql);
    for (pp = &Children; *pp != NULL; pp = &(*pp)->NextChild) {
        if (*pp == x) {
            *pp = x->NextChild;
            break;
        }
    }
    KeReleaseSpinLock(&ChildLock, irql);
    IoDeleteDevice(x->Nt.Self);
    InterlockedExchange(&PdoFreed, 1);
}

/* worker: which slot holds which device; asks PnP to look again when that changed */
static BOOLEAN WantAudio, SeenAudio;

/* a USB audio or MIDI device is connected (AUDIONT serves it) */
static BOOLEAN AudioPresent(void)
{
    int i;

    if (OsGetConfig("DisableAudio", 0)) {
        return FALSE;
    }
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (UsbDevs[i].InUse && !UsbDevs[i].Gone && (UsbDevs[i].Function & (USB_FUNC_AUDIO | USB_FUNC_MIDI))) {
            return TRUE;
        }
    }
    return FALSE;
}

/* which slot holds which device; TRUE when that changed */
static BOOLEAN SlotsChanged(void)
{
    BOOLEAN change = FALSE;
    ULONG i;

    for (i = 0; i < SCSI_MAX_SLOTS; i++) {
        ULONG want = 0, gen = 0;
        UCHAR fixed = 0;
        if (i < UsbScsiSlotCount && UsbScsiSlotPresent((int)i)) {
            SCSI_SLOT_INFO si;
            UsbScsiGetSlot((int)i, &si);
            want = si.Identity ? si.Identity : 0x5A5A0000UL + i;
            fixed = si.Fixed;
            gen = si.Fixed ? si.MediaGen : 0;
        }
        if (want != SeenIdentity[i] || fixed != WantFixed[i] || gen != WantGen[i]) {
            if (want != 0 && want == SeenIdentity[i]) {
                UsbLog(LOG_INFO, "PnP: disk slot %u: %s medium, a new disk for Windows\n", i,
                       fixed ? "fixed reported" : "removable");
            }
            SeenIdentity[i] = want;
            WantIdentity[i] = want;
            WantFixed[i] = fixed;
            WantGen[i] = gen;
            change = TRUE;
        }
    }
    return change;
}

/* worker, while a controller starts: the disks there are now go into the next bus relations */
void WdmBusRefresh(void)
{
    if (SlotsChanged()) {
        PendingRelations = 1;
    }
}

void WdmBusService(void)
{
    BOOLEAN change = FALSE;

    if (!BusStarted || BusPdo == NULL) {
        return;
    }
    if (AudioPresent() != SeenAudio) {
        SeenAudio = WantAudio = (BOOLEAN)!SeenAudio;
        change = TRUE;
    }
    if (UsbdiService()) {
        change = TRUE;
    }
    if (InterlockedExchange(&PdoFreed, 0)) {
        ULONG i;
        for (i = 0; i < SCSI_MAX_SLOTS; i++) {
            change |= WaitOld[i];               /* the old PDO may be the one that went */
        }
    }
    if (SlotsChanged()) {
        change = TRUE;
    }
    if (InterlockedExchange(&PendingRelations, 0) != 0) {
        change = TRUE;
    }
    if (TsWaitClass && OsTimeMs() - TsLastCheck > 500) {
        TsLastCheck = OsTimeMs();
        if (NtRefDriverLoaded(L"\\Driver\\Disk")) {
            TsWaitClass = FALSE;
            UsbLog(LOG_INFO, "PnP: text-mode setup: the disk class driver is there, the disks come\n");
            change = TRUE;
        }
    }
    if (change) {
        IoInvalidateDeviceRelations(BusPdo, BusRelations);
    }
}

/* usbeject / usbtree / usbmon: the disk goes the way of Safely Remove Hardware, so Windows
   closes its volumes first (and refuses while files are open) */
BOOLEAN WdmEjectSlot(ULONG Slot)
{
    WDM_EXT *x;
    PDEVICE_OBJECT pdo = NULL;
    KIRQL irql;

    if (!ChildLockInit) {
        return FALSE;
    }
    KeAcquireSpinLock(&ChildLock, &irql);
    for (x = Children; x != NULL; x = x->NextChild) {
        if (x->Child == PDO_DISK && (ULONG)x->Slot == Slot && x->Reported && x->Started && DiskWanted(x)) {
            pdo = x->Nt.Self;
            ObReferenceObject(pdo);
            break;
        }
    }
    KeReleaseSpinLock(&ChildLock, irql);
    if (pdo == NULL) {
        return FALSE;
    }
    UsbLog(LOG_INFO, "PnP: disk slot %u: eject requested\n", Slot);
    IoRequestDeviceEject(pdo);
    ObDereferenceObject(pdo);
    return TRUE;
}

static NTSTATUS BusRelationsQuery(PIRP Irp)
{
    PDEVICE_RELATIONS old = (PDEVICE_RELATIONS)Irp->IoStatus.Information, rel;
    WDM_EXT *x, *next;
    ULONG n = 0, keep = old ? old->Count : 0, i;
    BOOLEAN haveAudio = FALSE, haveKbd = FALSE, haveMou = FALSE, kbd = !OsGetConfig("DisableKeyboard", 0);
    BOOLEAN mou = !OsGetConfig("DisableMouse", 0), disks = !OsGetConfig("DisableStorage", 0);
    BOOLEAN have[SCSI_MAX_SLOTS];

    RtlZeroMemory(have, sizeof(have));
    /* who stays, who goes */
    for (x = Children; x != NULL; x = x->NextChild) {
        x->Reported = FALSE;
        if (x->Child == PDO_KBD && kbd && !haveKbd) {
            x->Reported = haveKbd = TRUE;
        } else if (x->Child == PDO_AUDIO && WantAudio && !haveAudio) {
            x->Reported = haveAudio = TRUE;
        } else if (x->Child == PDO_USB && UsbdiWanted(x->Func)) {
            x->Reported = TRUE;
        } else if (x->Child == PDO_MOUSE && mou && !haveMou) {
            x->Reported = haveMou = TRUE;
        } else if (x->Child == PDO_DISK && disks && DiskWanted(x) && !x->Gone && !have[x->Slot]) {
            x->Reported = have[x->Slot] = TRUE;
        }
    }
    /* who comes */
    if (kbd && !haveKbd && (x = NewPdo(PDO_KBD, -1)) != NULL) {
        x->Reported = TRUE;
    }
    if (mou && !haveMou && (x = NewPdo(PDO_MOUSE, -1)) != NULL) {
        x->Reported = TRUE;
    }
    if (WantAudio && !haveAudio && (x = NewPdo(PDO_AUDIO, -1)) != NULL) {
        x->Reported = TRUE;
        UsbLog(LOG_INFO, "PnP: USB audio to Windows (AUDIONT)\n");
    }
    {
        /* USB devices and functions for Windows' drivers */
        struct _UD_FUNC *f;
        while ((f = UsbdiNewFunc()) != NULL) {
            if ((x = NewPdo(PDO_USB, -1)) == NULL) {
                UsbdiFreeFunc(f);
                break;
            }
            x->Func = f;
            UsbdiBind(f, x->Nt.Self);
            x->Reported = TRUE;
        }
    }
    for (i = 0; disks && i < SCSI_MAX_SLOTS; i++) {
        WaitOld[i] = FALSE;
        if (WantIdentity[i] == 0 || have[i]) {
            continue;
        }
        {
            /* the PDOs of the slot's previous media (same instance ids) that are not removed yet */
            BOOLEAN used[2];
            used[0] = used[1] = FALSE;
            for (x = Children; x != NULL; x = x->NextChild) {
                if (x->Child == PDO_DISK && x->Slot == (int)i && x->Identity == WantIdentity[i]) {
                    used[x->Alt & 1] = TRUE;
                }
            }
            if (used[0] && used[1]) {
                WaitOld[i] = TRUE;
                UsbLog(LOG_INFO, "PnP: disk slot %u waits for the removal of its previous disk\n", i);
                continue;
            }
            if (TsInput && !NtRefDriverLoaded(L"\\Driver\\Disk")) {
                TsWaitClass = TRUE;                 /* text-mode setup: reported once disk.sys is there */
                continue;
            }
            if ((x = NewPdo(PDO_DISK, (int)i)) != NULL) {
                x->Alt = (UCHAR)(used[0] ? 1 : 0);
                x->Reported = TRUE;
                if (TsInput) {
                    TsDisk(x);
                }
                UsbLog(LOG_INFO, "PnP: disk slot %u to Windows%s%s\n", i, x->Fixed ? " (fixed medium)" : "",
                       x->Alt ? " (second instance)" : "");
            }
        }
    }
    for (x = Children; x != NULL; x = x->NextChild) {
        n += x->Reported;
    }
    rel = (PDEVICE_RELATIONS)ExAllocatePoolWithTag(PagedPool, sizeof(DEVICE_RELATIONS) +
                                                   (n + keep) * sizeof(PDEVICE_OBJECT), USBNT_TAG);
    if (rel == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    rel->Count = 0;
    for (i = 0; i < keep; i++) {
        rel->Objects[rel->Count++] = old->Objects[i];
    }
    for (x = Children; x != NULL; x = next) {
        next = x->NextChild;
        if (x->Reported) {
            ObReferenceObject(x->Nt.Self);
            rel->Objects[rel->Count++] = x->Nt.Self;
        }
        /* a PDO no longer reported is deleted at its (next) IRP_MN_REMOVE_DEVICE: Plug and Play still
           holds it while it looks at these relations */
    }
    if (old != NULL) {
        ExFreePool(old);
    }
    Irp->IoStatus.Information = (ULONG_PTR)rel;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* The bus                                                              */
/* ------------------------------------------------------------------ */

static BOOLEAN WEqNoCase(const WCHAR *A, ULONG ALen, const WCHAR *B)
{
    ULONG i;

    for (i = 0; i < ALen && B[i] != 0; i++) {
        WCHAR a = A[i], b = B[i];
        if (a >= L'a' && a <= L'z') a = (WCHAR)(a - 32);
        if (b >= L'a' && b <= L'z') b = (WCHAR)(b - 32);
        if (a != b) {
            return FALSE;
        }
    }
    return (BOOLEAN)(B[i] == 0 && (i == ALen || A[i] == 0));
}

static HANDLE OpenSub(HANDLE Parent, PUNICODE_STRING Name)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;

    InitializeObjectAttributes(&oa, Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Parent, NULL);
    return NT_SUCCESS(ZwOpenKey(&h, KEY_READ, &oa)) ? h : NULL;
}

/* an instance key whose first hardware id is USBNT\Bus */
static BOOLEAN IsBusInstance(HANDLE Key)
{
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 128];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    UNICODE_STRING name;
    ULONG len = 0;

    RtlInitUnicodeString(&name, L"HardwareID");
    if (!NT_SUCCESS(ZwQueryValueKey(Key, &name, KeyValuePartialInformation, buf, sizeof(buf) - 2, &len)) ||
        (v->Type != REG_MULTI_SZ && v->Type != REG_SZ)) {
        return FALSE;
    }
    return WEqNoCase((const WCHAR *)v->Data, v->DataLength / sizeof(WCHAR), L"USBNT\\Bus");
}

/* ------------------------------------------------------------------ */
/* The device classes of the keyboard and the mouse                     */
/*                                                                      */
/* Text-mode setup (F6 driver disk, or a CD that nLite made) writes a   */
/* CriticalDeviceDatabase entry for every [HardwareIds.scsi] id of      */
/* TXTSETUP.OEM - all of them with the SCSI adapter class.  USBNT\Key-  */
/* board and USBNT\Mouse then start without kbdclass / mouclass (the    */
/* class upper filters come with the class): no USB keyboard and mouse  */
/* in GUI-mode setup and at the logon screen until an INF is installed, */
/* on a machine without a PS/2 keyboard whose class driver USBNT could  */
/* borrow.  So at every start, before the bus reports them: the         */
/* CriticalDeviceDatabase entries and the device nodes that are not in  */
/* their class get the keyboard / mouse class.                          */
/* ------------------------------------------------------------------ */
static const struct {
    const WCHAR *Cddb, *Enum, *Guid, *Class;
    const char *Name;
} InputClass[] = {
    { L"usbnt#keyboard", L"Keyboard", L"{4D36E96B-E325-11CE-BFC1-08002BE10318}", L"Keyboard", "Keyboard" },
    { L"usbnt#mouse", L"Mouse", L"{4D36E96F-E325-11CE-BFC1-08002BE10318}", L"Mouse", "Mouse" },
    /* the root hubs, hubs and devices of Device Manager (wdmtree.c): usbnt is their function driver */
    { L"usbnt#hub", NULL, L"{36FC9E60-C465-11CF-8056-444553540000}", L"USB", "HUB" },
    { L"usbnt#device", NULL, L"{36FC9E60-C465-11CF-8056-444553540000}", L"USB", "DEVICE" },
};

static HANDLE OpenRw(HANDLE Parent, PUNICODE_STRING Name, BOOLEAN Create)
{
    OBJECT_ATTRIBUTES oa;
    HANDLE h = NULL;
    ULONG disp;

    InitializeObjectAttributes(&oa, Name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, Parent, NULL);
    if (Create) {
        return NT_SUCCESS(ZwCreateKey(&h, KEY_ALL_ACCESS, &oa, 0, NULL, REG_OPTION_NON_VOLATILE, &disp)) ? h : NULL;
    }
    return NT_SUCCESS(ZwOpenKey(&h, KEY_ALL_ACCESS, &oa)) ? h : NULL;
}

/* REG_SZ value Name equals Want (no case)? */
static BOOLEAN SzIs(HANDLE Key, const WCHAR *Name, const WCHAR *Want, ULONG Chars)
{
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 160];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    UNICODE_STRING name;
    ULONG len = 0, n;
    const WCHAR *d;

    RtlInitUnicodeString(&name, Name);
    RtlZeroMemory(buf, sizeof(buf));
    if (!NT_SUCCESS(ZwQueryValueKey(Key, &name, KeyValuePartialInformation, buf, sizeof(buf) - 4, &len)) ||
        v->Type != REG_SZ) {
        return FALSE;
    }
    d = (const WCHAR *)v->Data;
    n = v->DataLength / sizeof(WCHAR);
    while (n > 0 && d[n - 1] == 0) n--;
    if (Chars != 0 && n > Chars) n = Chars;         /* Driver = "{guid}\\0003": compare the class part */
    return WEqNoCase(d, n, Want);
}

static BOOLEAN SetSz(HANDLE Key, const WCHAR *Name, const WCHAR *Val)
{
    UNICODE_STRING name;
    ULONG n = 0;

    while (Val[n]) n++;
    RtlInitUnicodeString(&name, Name);
    return NT_SUCCESS(ZwSetValueKey(Key, &name, 0, REG_SZ, (PVOID)Val, (n + 1) * sizeof(WCHAR)));
}

static void InputClassFix(void)
{
    UNICODE_STRING path, sub, nm;
    HANDLE base, h, cls, inst;
    ULONG c, i, len;

    for (c = 0; c < sizeof(InputClass) / sizeof(InputClass[0]); c++) {
        RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Control\\CriticalDeviceDatabase");
        if ((base = OpenRw(NULL, &path, FALSE)) != NULL) {
            RtlInitUnicodeString(&sub, InputClass[c].Cddb);
            if ((h = OpenRw(base, &sub, TRUE)) != NULL) {
                if (!SzIs(h, L"ClassGUID", InputClass[c].Guid, 0) || !SzIs(h, L"Service", L"usbnt", 0)) {
                    SetSz(h, L"Service", L"usbnt");
                    SetSz(h, L"ClassGUID", InputClass[c].Guid);
                    UsbLog(LOG_INFO, "PnP: CriticalDeviceDatabase USBNT\\%s: class %s\n", InputClass[c].Name,
                           InputClass[c].Name);
                }
                ZwClose(h);
            }
            ZwClose(base);
        }
        /* device nodes there already (Enum\USBNT\Keyboard\<instance>) */
        if (InputClass[c].Enum == NULL) {
            continue;
        }
        RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\USBNT");
        if ((base = OpenSub(NULL, &path)) == NULL) {
            continue;
        }
        RtlInitUnicodeString(&sub, InputClass[c].Enum);
        cls = OpenSub(base, &sub);
        ZwClose(base);
        for (i = 0; cls != NULL && i < 64; i++) {
            UCHAR ib[sizeof(KEY_BASIC_INFORMATION) + 160];
            PKEY_BASIC_INFORMATION k = (PKEY_BASIC_INFORMATION)ib;
            len = 0;
            if (!NT_SUCCESS(ZwEnumerateKey(cls, i, KeyBasicInformation, ib, sizeof(ib), &len))) {
                break;
            }
            nm.Buffer = k->Name;
            nm.Length = nm.MaximumLength = (USHORT)k->NameLength;
            if ((inst = OpenRw(cls, &nm, FALSE)) == NULL) {
                continue;
            }
            if (!SzIs(inst, L"ClassGUID", InputClass[c].Guid, 0)) {
                UNICODE_STRING drv;
                /* a driver key of another class (an INF of that class put it there) goes too */
                RtlInitUnicodeString(&drv, L"Driver");
                if (!SzIs(inst, L"Driver", InputClass[c].Guid, 38)) {
                    ZwDeleteValueKey(inst, &drv);
                }
                SetSz(inst, L"ClassGUID", InputClass[c].Guid);
                SetSz(inst, L"Class", InputClass[c].Class);
                UsbLog(LOG_INFO, "PnP: a USBNT\\%s device node of another class: now class %s\n",
                       InputClass[c].Name, InputClass[c].Name);
            }
            ZwClose(inst);
        }
        if (cls != NULL) {
            ZwClose(cls);
        }
    }
}

static BOOLEAN HasClass(HANDLE Inst, const WCHAR *Cc)
{
    UCHAR vb[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 800];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)vb;
    UNICODE_STRING vn;
    ULONG len = 0, c, k, n;
    const WCHAR *w;

    RtlInitUnicodeString(&vn, L"CompatibleIDs");
    RtlZeroMemory(vb, sizeof(vb));
    if (!NT_SUCCESS(ZwQueryValueKey(Inst, &vn, KeyValuePartialInformation, vb, sizeof(vb) - 4, &len))) {
        return FALSE;
    }
    w = (const WCHAR *)v->Data;
    n = v->DataLength / sizeof(WCHAR);
    for (k = 0; Cc[k]; k++) {
    }
    for (c = 0; c + k <= n; c++) {
        ULONG i;
        for (i = 0; i < k && w[c + i] == Cc[i]; i++) {
        }
        if (i == k && (c + k == n || w[c + k] == 0)) {
            return TRUE;
        }
    }
    return FALSE;
}

static ULONG TsBus[8], TsSlot[8], TsN;

/*
 * Text-mode setup: a device that comes after setup has started (setup gives the service of its
 * [HardwareIdsDatabase] only to the devices there at the start - a USB disk comes when its
 * controller has found it) or that the database does not name (Windows 2000 with an F6 disk:
 * USBNT\Keyboard) gets no driver, and Plug and Play takes the service of a new device node before
 * usbnt.sys could write it.  So the keyboard, mouse and disk device nodes are made before the bus
 * reports them, with the service and the class, under instance ids unique in the system
 * (Enum\USBNT\Keyboard\TextSetup, a disk's own).  Setup's registry is thrown away afterwards.
 */
/* Enum\USBNT\<Dev>\<Inst> with the service and the class */
static void TsNode(const WCHAR *Dev, const WCHAR *Inst, const WCHAR *Service, const WCHAR *Guid, const WCHAR *Class)
{
    UNICODE_STRING path, sub;
    HANDLE e, d, i;
    ULONG zero = 0;

    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\USBNT");
    if ((e = OpenRw(NULL, &path, TRUE)) == NULL) {
        return;
    }
    RtlInitUnicodeString(&sub, Dev);
    d = OpenRw(e, &sub, TRUE);
    ZwClose(e);
    if (d == NULL) {
        return;
    }
    RtlInitUnicodeString(&sub, Inst);
    i = OpenRw(d, &sub, TRUE);
    ZwClose(d);
    if (i == NULL) {
        return;
    }
    SetSz(i, L"Service", Service);
    SetSz(i, L"ClassGUID", Guid);
    SetSz(i, L"Class", Class);
    RtlInitUnicodeString(&sub, L"ConfigFlags");
    ZwSetValueKey(i, &sub, 0, REG_DWORD, &zero, sizeof(zero));
    ZwClose(i);
}

static void WdmTextSetupInput(void)
{
    TsNode(L"Keyboard", L"TextSetup", L"usbnt", L"{4D36E96B-E325-11CE-BFC1-08002BE10318}", L"Keyboard");
    TsNode(L"Mouse", L"TextSetup", L"usbnt", L"{4D36E96F-E325-11CE-BFC1-08002BE10318}", L"Mouse");
    TsInput = TRUE;
}

/* a disk / CD-ROM child, before it is reported (text-mode setup) */
static void TsDisk(WDM_EXT *x)
{
    WCHAR dev[512], inst[512];
    BOOLEAN cd = (BOOLEAN)(x->DevType == 5);

    if (BuildChildId(x, BusQueryDeviceID, dev) == NULL || BuildChildId(x, BusQueryInstanceID, inst) == NULL ||
        dev[0] == 0 || inst[0] == 0) {
        return;
    }
    TsNode(dev + 6, inst, cd ? L"cdrom" : L"disk",
           cd ? L"{4D36E965-E325-11CE-BFC1-08002BE10318}" : L"{4D36E967-E325-11CE-BFC1-08002BE10318}",
           cd ? L"CDROM" : L"DiskDrive");                       /* dev + 6: after "USBNT\\" */
    UsbLog(LOG_DBG, "PnP: text-mode setup: disk device node made\n");
}

ULONG WdmTextSetupClaims(ULONG *Bus, ULONG *Slot, ULONG Max)
{
    ULONG i;
    for (i = 0; i < TsN && i < Max; i++) {
        Bus[i] = TsBus[i];
        Slot[i] = TsSlot[i];
    }
    return i;
}

/* "PCI bus B, device D, function F" */
static BOOLEAN PciLocation(HANDLE Inst, ULONG *Bus, ULONG *Slot)
{
    UCHAR vb[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 200];
    PKEY_VALUE_PARTIAL_INFORMATION v = (PKEY_VALUE_PARTIAL_INFORMATION)vb;
    UNICODE_STRING vn;
    ULONG len = 0, n, i, num[3], k = 0;
    const WCHAR *w;

    RtlInitUnicodeString(&vn, L"LocationInformation");
    RtlZeroMemory(vb, sizeof(vb));
    if (!NT_SUCCESS(ZwQueryValueKey(Inst, &vn, KeyValuePartialInformation, vb, sizeof(vb) - 4, &len))) {
        return FALSE;
    }
    w = (const WCHAR *)v->Data;
    n = v->DataLength / sizeof(WCHAR);
    for (i = 0; i < n && k < 3; i++) {
        if (w[i] >= L'0' && w[i] <= L'9') {
            num[k] = 0;
            while (i < n && w[i] >= L'0' && w[i] <= L'9') {
                num[k] = num[k] * 10 + (w[i++] - L'0');
            }
            k++;
        }
    }
    if (k != 3) {
        return FALSE;
    }
    *Bus = num[0];
    *Slot = (num[1] & 31) | ((num[2] & 7) << 5);
    return TRUE;
}

/* Windows 2000 text-mode setup with an F6 driver disk: the USB host controllers (and SD hosts)
   setup gave no driver - usbnt.sys takes them itself (wdmmain.c TextSetupHcs).  Text-mode setup:
   the setup driver's service key is in its registry. */
static BOOLEAN TextSetupClaim(void)
{
    UNICODE_STRING path, sub, vn;
    HANDLE pci, dev, inst, h;
    ULONG i, j, len;

    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\setupdd");
    if ((h = OpenSub(NULL, &path)) == NULL) {
        return FALSE;
    }
    ZwClose(h);
    UsbLog(LOG_INFO, "PnP: text-mode setup\n");
    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\PCI");
    if ((pci = OpenSub(NULL, &path)) == NULL) {
        return TRUE;
    }
    for (i = 0; i < 256; i++) {
        UCHAR kb[sizeof(KEY_BASIC_INFORMATION) + 256];
        PKEY_BASIC_INFORMATION k = (PKEY_BASIC_INFORMATION)kb;
        if (!NT_SUCCESS(ZwEnumerateKey(pci, i, KeyBasicInformation, kb, sizeof(kb), &len))) {
            break;
        }
        sub.Buffer = k->Name;
        sub.Length = sub.MaximumLength = (USHORT)k->NameLength;
        if ((dev = OpenSub(pci, &sub)) == NULL) {
            continue;
        }
        for (j = 0; j < 8; j++) {
            UCHAR ib[sizeof(KEY_BASIC_INFORMATION) + 128];
            PKEY_BASIC_INFORMATION n = (PKEY_BASIC_INFORMATION)ib;
            UCHAR vb[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 64];
            if (!NT_SUCCESS(ZwEnumerateKey(dev, j, KeyBasicInformation, ib, sizeof(ib), &len))) {
                break;
            }
            sub.Buffer = n->Name;
            sub.Length = sub.MaximumLength = (USHORT)n->NameLength;
            if ((inst = OpenRw(dev, &sub, FALSE)) == NULL) {
                continue;
            }
            RtlInitUnicodeString(&vn, L"Service");
            if ((HasClass(inst, L"PCI\\CC_0C03") || HasClass(inst, L"PCI\\CC_0805")) &&
                !NT_SUCCESS(ZwQueryValueKey(inst, &vn, KeyValuePartialInformation, vb, sizeof(vb), &len))) {
                ULONG b, sl;
                if (TsN < 8 && PciLocation(inst, &b, &sl)) {
                    TsBus[TsN] = b;
                    TsSlot[TsN++] = sl;
                    UsbLog(LOG_INFO, "PnP: text-mode setup: host controller %02x:%02x.%u without a driver\n", b, sl & 31, sl >> 5);
                }
            }
            ZwClose(inst);
        }
        ZwClose(dev);
    }
    ZwClose(pci);
    return TRUE;
}

/* DriverEntry: usbinst made a root enumerated USBNT\Bus device (Enum\Root\<class>\NNNN)? */
void WdmBusInit(void)
{
    UNICODE_STRING path;
    HANDLE root;
    ULONG i, j;

    if (!ChildLockInit) {
        KeInitializeSpinLock(&ChildLock);
        ChildLockInit = TRUE;
    }
    InputClassFix();
    if (TextSetupClaim()) {
        WdmTextSetupInput();
    }
    RtlInitUnicodeString(&path, L"\\Registry\\Machine\\System\\CurrentControlSet\\Enum\\Root");
    root = OpenSub(NULL, &path);
    for (i = 0; root != NULL && !RootBus && i < 4096; i++) {
        UCHAR kb[sizeof(KEY_BASIC_INFORMATION) + 256];
        PKEY_BASIC_INFORMATION k = (PKEY_BASIC_INFORMATION)kb;
        UNICODE_STRING sub;
        HANDLE cls;
        ULONG len = 0;
        if (!NT_SUCCESS(ZwEnumerateKey(root, i, KeyBasicInformation, kb, sizeof(kb), &len))) {
            break;
        }
        sub.Buffer = k->Name;
        sub.Length = sub.MaximumLength = (USHORT)k->NameLength;
        if (k->NameLength >= 7 * sizeof(WCHAR) && WEqNoCase(k->Name, 7, L"LEGACY_")) {
            continue;                           /* the legacy drivers' made-up devices */
        }
        if ((cls = OpenSub(root, &sub)) == NULL) {
            continue;
        }
        for (j = 0; !RootBus && j < 256; j++) {
            UCHAR ib[sizeof(KEY_BASIC_INFORMATION) + 128];
            PKEY_BASIC_INFORMATION n = (PKEY_BASIC_INFORMATION)ib;
            UNICODE_STRING inst;
            HANDLE h;
            if (!NT_SUCCESS(ZwEnumerateKey(cls, j, KeyBasicInformation, ib, sizeof(ib), &len))) {
                break;
            }
            inst.Buffer = n->Name;
            inst.Length = inst.MaximumLength = (USHORT)n->NameLength;
            if ((h = OpenSub(cls, &inst)) != NULL) {
                RootBus = IsBusInstance(h);
                ZwClose(h);
            }
        }
        ZwClose(cls);
    }
    if (root != NULL) {
        ZwClose(root);
    }
    UsbLog(LOG_INFO, "PnP: %s\n", RootBus ? "USBNT\\Bus device installed" :
           "no USBNT\\Bus device: the first host controller will be the device bus");
}

/* host controller Hc has started; without a USBNT\Bus device it becomes the bus */
void WdmBusOnController(WDM_EXT *Hc)
{
    if (RootBus || BusFdo != NULL) {
        return;
    }
    BusFdo = Hc->Nt.Self;
    BusPdo = Hc->Pdo;
    BusLower = Hc->Lower;
    OnController = TRUE;
    BusStarted = TRUE;
    UsbLog(LOG_INFO, "PnP: USBNT device bus on the host controller %02x:%02x.%u\n", Hc->Bus, Hc->Dev, Hc->Fn);
}

/* the device bus usbnt.sys reported itself (IoReportDetectedDevice, text-mode setup): Plug and
   Play takes a reported device as started - no start request comes */
void WdmBusReportedStarted(void)
{
    if (BusFdo != NULL) {
        ((WDM_EXT *)BusFdo->DeviceExtension)->Started = TRUE;
        BusStarted = TRUE;
        UsbLog(LOG_INFO, "PnP: USBNT device bus started (reported)\n");
    }
}

BOOLEAN WdmBusPresent(void)
{
    return (BOOLEAN)(RootBus || BusFdo != NULL);
}

BOOLEAN WdmIsBus(PDEVICE_OBJECT Fdo)
{
    return (BOOLEAN)(Fdo != NULL && Fdo == BusFdo);
}

NTSTATUS WdmBusRelations(PIRP Irp)
{
    return BusRelationsQuery(Irp);
}

/* the bus device goes (IRP_MN_REMOVE_DEVICE): so do the children */
static void BusGone(void)
{
    WDM_EXT *c, *next;

    BusStarted = FALSE;
    for (c = Children; c != NULL; c = next) {
        next = c->NextChild;
        FreePdo(c);
    }
    BusFdo = BusPdo = BusLower = NULL;
    OnController = FALSE;
}

void WdmBusOff(PDEVICE_OBJECT Fdo)
{
    if (OnController && Fdo == BusFdo) {
        UsbLog(LOG_INFO, "PnP: the host controller with the device bus is removed\n");
        BusGone();
    }
}

NTSTATUS WdmBusAdd(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fdo;
    WDM_EXT *x;
    NTSTATUS st;

    if (!ChildLockInit) {
        KeInitializeSpinLock(&ChildLock);
        ChildLockInit = TRUE;
    }
    if (BusFdo != NULL) {
        if (OnController) {
            /* usbinst made one while a controller is the bus: from the next start on it is the bus */
            UsbLog(LOG_INFO, "PnP: USBNT\\Bus device added while a host controller is the bus: used after a restart\n");
        }
        return STATUS_DEVICE_ALREADY_ATTACHED;
    }
    st = IoCreateDevice(Drv, sizeof(WDM_EXT), NULL, FILE_DEVICE_BUS_EXTENDER, FILE_DEVICE_SECURE_OPEN, FALSE, &fdo);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    x = (WDM_EXT *)fdo->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Nt.Kind = EXT_BUS;
    x->Nt.Self = fdo;
    x->Pdo = Pdo;
    x->Lower = IoAttachDeviceToDeviceStack(fdo, Pdo);
    if (x->Lower == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_NO_SUCH_DEVICE;
    }
    BusFdo = fdo;
    BusPdo = Pdo;
    BusLower = x->Lower;
    fdo->Flags |= DO_POWER_PAGABLE;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS WdmBusPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = WdmForwardSync(x->Lower, Irp);
        if (NT_SUCCESS(st)) {
            x->Started = TRUE;
            BusStarted = TRUE;
            UsbLog(LOG_INFO, "PnP: USBNT device bus started\n");
            IoInvalidateDeviceRelations(BusPdo, BusRelations);
        } else {
            UsbLog(LOG_ERR, "PnP: USBNT device bus does not start (%x)\n", st);
        }
        return WdmComplete(Irp, st, 0);
    case IRP_MN_QUERY_DEVICE_RELATIONS:
        if (sp->Parameters.QueryDeviceRelations.Type == BusRelations) {
            st = BusRelationsQuery(Irp);
            if (!NT_SUCCESS(st)) {
                return WdmComplete(Irp, st, 0);
            }
            Irp->IoStatus.Status = STATUS_SUCCESS;
        }
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(x->Lower, Irp);
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_QUERY_REMOVE_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(x->Lower, Irp);
    case IRP_MN_REMOVE_DEVICE:
        BusGone();
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoSkipCurrentIrpStackLocation(Irp);
        st = IoCallDriver(x->Lower, Irp);
        IoDetachDevice(x->Lower);
        IoDeleteDevice(Dev);
        return st;
    default:
        IoSkipCurrentIrpStackLocation(Irp);
        return IoCallDriver(x->Lower, Irp);
    }
}

/* ------------------------------------------------------------------ */
/* Children: Plug and Play                                              */
/* ------------------------------------------------------------------ */

static const WCHAR *DiskKind(WDM_EXT *x)
{
    if (x->Proto == 0x62) {
        return L"USB 3.x UAS";
    }
    if (x->Proto == 0x50 || x->Proto == 0x00 || x->Proto == 0x01) {
        return L"USB";
    }
    return L"";
}

/* the id of Type of a keyboard / mouse / audio / disk child into Buf (512 characters); the end, NULL
   for an id it has none of */
static WCHAR *BuildChildId(WDM_EXT *x, BUS_QUERY_ID_TYPE Type, WCHAR *buf)
{
    WCHAR *p = buf;
    const WCHAR *gen = (x->DevType == 5) ? L"GenCdRom" : (x->DevType == 0x0E || x->DevType == 0) ? L"GenDisk" :
                       L"GenOptical";
    const WCHAR *cls = (x->DevType == 5) ? L"CdRom" : L"Disk";

    RtlZeroMemory(buf, 512 * sizeof(WCHAR));
    if (x->Child == PDO_AUDIO) {
        switch (Type) {
        case BusQueryDeviceID:
        case BusQueryHardwareIDs:
            p = WAppend(p, L"USBNT\\Audio") + 1;
            break;
        case BusQueryInstanceID:
            p = WAppend(p, L"0");
            break;
        default:
            return NULL;
        }
    } else if (x->Child == PDO_KBD || x->Child == PDO_MOUSE) {
        const WCHAR *id = (x->Child == PDO_KBD) ? L"USBNT\\Keyboard" : L"USBNT\\Mouse";
        switch (Type) {
        case BusQueryDeviceID:
            p = WAppend(p, id);
            break;
        case BusQueryHardwareIDs:
            p = WAppend(p, id) + 1;
            p = WAppend(p, x->Child == PDO_KBD ? L"USBNT\\HID_Keyboard" : L"USBNT\\HID_Mouse") + 1;
            break;
        case BusQueryInstanceID:
            p = WAppend(p, TsInput ? L"TextSetup" : L"0");
            break;
        default:
            return NULL;
        }
    } else {
        switch (Type) {
        case BusQueryDeviceID:
            /* USBNT\Disk&Ven_x&Prod_y&Rev_z, the form usbstor uses */
            p = WAppend(p, L"USBNT\\");
            p = WAppend(p, cls);
            p = WAppend(p, L"&Ven_");
            p = AAppendId(p, x->Inquiry + 8, 8);
            p = WAppend(p, L"&Prod_");
            p = AAppendId(p, x->Inquiry + 16, 16);
            p = WAppend(p, L"&Rev_");
            p = AAppendId(p, x->Inquiry + 32, 4);
            break;
        case BusQueryHardwareIDs:
            p = WAppend(p, L"USBNT\\");
            p = WAppend(p, cls);
            p = AAppendId(p, x->Inquiry + 8, 8);
            p = AAppendId(p, x->Inquiry + 16, 16);
            p = AAppendId(p, x->Inquiry + 32, 4) + 1;
            p = WAppend(p, L"USBNT\\");
            p = WAppend(p, cls);
            p = AAppendId(p, x->Inquiry + 8, 8);
            p = AAppendId(p, x->Inquiry + 16, 16) + 1;
            p = WAppend(p, L"USBNT\\");
            p = WAppend(p, cls);
            p = AAppendId(p, x->Inquiry + 8, 8) + 1;
            p = WAppend(p, L"USBNT\\");
            p = WAppend(p, cls) + 1;
            p = WAppend(p, gen) + 1;
            break;
        case BusQueryCompatibleIDs:
            p = WAppend(p, L"USBNT\\RAW") + 1;
            p = WAppend(p, gen) + 1;
            break;
        case BusQueryInstanceID:
            p = HexAppend(p, x->Identity, 8);
            p = WAppend(p, L"&");
            p = HexAppend(p, (ULONG)x->Slot, 1);
            if (x->Alt) {
                p = WAppend(p, L"&1");
            }
            break;
        default:
            return NULL;
        }
    }
    return p + 1;                               /* the final 0 of a list (or of a string) */
}

static NTSTATUS QueryId(WDM_EXT *x, PIRP Irp, BUS_QUERY_ID_TYPE Type)
{
    WCHAR buf[512];
    WCHAR *p;

    if (x->Child == PDO_USB) {
        return UsbdiQueryId(x->Func, Irp, Type);
    }
    if ((p = BuildChildId(x, Type, buf)) == NULL) {
        return Irp->IoStatus.Status;
    }
    Irp->IoStatus.Information = (ULONG_PTR)PoolString(buf, (ULONG)(p - buf));
    return Irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static NTSTATUS QueryText(WDM_EXT *x, PIRP Irp, DEVICE_TEXT_TYPE Type)
{
    WCHAR buf[128];
    WCHAR *p = buf;

    if (x->Child == PDO_USB) {
        return UsbdiQueryText(x->Func, Irp, Type);
    }
    if (Type == DeviceTextLocationInformation) {
        if (x->Child != PDO_DISK) {
            return Irp->IoStatus.Status;
        }
        p = WAppend(p, L"USBNT disk slot ");
        p = HexAppend(p, (ULONG)x->Slot, 1);
    } else if (Type == DeviceTextDescription) {
        if (x->Child == PDO_KBD) {
            p = WAppend(p, L"USBNT USB Keyboard");
        } else if (x->Child == PDO_MOUSE) {
            p = WAppend(p, L"USBNT USB Mouse");
        } else if (x->Child == PDO_AUDIO) {
            p = WAppend(p, L"USBNT USB Audio");
        } else {
            p = AAppendText(p, x->Inquiry + 8, 8);
            *p++ = L' ';
            p = AAppendText(p, x->Inquiry + 16, 16);
            if (DiskKind(x)[0]) {
                p = WAppend(p, L" (");
                p = WAppend(p, DiskKind(x));
                p = WAppend(p, L")");
            }
        }
    } else {
        return Irp->IoStatus.Status;
    }
    Irp->IoStatus.Information = (ULONG_PTR)PoolString(buf, (ULONG)(p - buf));
    return Irp->IoStatus.Information ? STATUS_SUCCESS : STATUS_INSUFFICIENT_RESOURCES;
}

static void EjectW(void *A)
{
    WDM_EXT *x = (WDM_EXT *)A;

    UsbScsiEject(x->Slot);
}

NTSTATUS WdmPdoPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = Irp->IoStatus.Status;
    ULONG_PTR info = Irp->IoStatus.Information;

    if (x->Child == PDO_HUB || x->Child == PDO_UDEV) {
        return WdmTreePdoPnp(Dev, Irp);         /* Device Manager nodes (wdmtree.c) */
    }
    if (x->Child == PDO_USB) {
        BOOLEAN freeIt = FALSE;
        st = UsbdiPnp(x->Func, Irp, x->Reported, &freeIt);
        if (freeIt) {
            UsbdiFreeFunc(x->Func);
            x->Func = NULL;
            FreePdo(x);
        }
        return st;
    }
    if ((x->Child == PDO_KBD || x->Child == PDO_MOUSE) && !x->ClassSet && sp->MinorFunction != IRP_MN_QUERY_ID &&
        sp->MinorFunction != IRP_MN_QUERY_CAPABILITIES) {
        WdmInputClassNode(x);                   /* before the stack is built (text-mode setup, wdmtree.c) */
    }
    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        x->Started = TRUE;
        x->Removed = FALSE;
        x->Gone = FALSE;
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_STOP_DEVICE:
    case IRP_MN_CANCEL_STOP_DEVICE:
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_CANCEL_REMOVE_DEVICE:
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_REMOVE_DEVICE:
        st = x->Paging > 0 ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
        break;
    case IRP_MN_SURPRISE_REMOVAL:
        x->Started = FALSE;
        x->Gone = TRUE;                         /* not reported again: a new PDO comes after its removal */
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_REMOVE_DEVICE:
        /* the device stays on the bus while it is there: Windows may start the PDO again (a new driver),
           or keep it "prepared for safe removal" until IRP_MN_EJECT */
        x->Started = FALSE;
        x->Removed = TRUE;
        Irp->IoStatus.Status = STATUS_SUCCESS;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        if (!x->Reported) {
            FreePdo(x);                         /* gone from the bus already */
        }
        return STATUS_SUCCESS;
    case IRP_MN_EJECT:
        /* Safely Remove Hardware: the device is shut down and not reported again until it is plugged in anew */
        if (x->Child == PDO_DISK && UsbScsiSlotPresent(x->Slot) && DiskWanted(x)) {
            WdmRunInWorker(EjectW, x);
            UsbLog(LOG_INFO, "PnP: disk slot %d ejected by Windows\n", x->Slot);
        }
        st = STATUS_SUCCESS;
        break;
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
        c->EjectSupported = (x->Child == PDO_DISK);
        c->Removable = (x->Child == PDO_DISK);
        c->DockDevice = FALSE;
        c->UniqueID = (BOOLEAN)(TsInput && (x->Child == PDO_KBD || x->Child == PDO_MOUSE || x->Child == PDO_DISK));
        c->SilentInstall = TRUE;
        c->RawDeviceOK = FALSE;
        c->SurpriseRemovalOK = FALSE;
        c->Address = (ULONG)(x->Slot >= 0 ? x->Slot : 0);
        c->UINumber = c->Address;
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
                                                                             USBNT_TAG);
            if (rel == NULL) {
                st = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                rel->Count = 1;
                rel->Objects[0] = Dev;
                ObReferenceObject(Dev);
                info = (ULONG_PTR)rel;
                st = STATUS_SUCCESS;
            }
        }
        break;
    case IRP_MN_QUERY_BUS_INFORMATION: {
        PPNP_BUS_INFORMATION b = (PPNP_BUS_INFORMATION)ExAllocatePoolWithTag(PagedPool, sizeof(PNP_BUS_INFORMATION),
                                                                             USBNT_TAG);
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
    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        if (sp->Parameters.UsageNotification.Type == DeviceUsageTypePaging) {
            if (sp->Parameters.UsageNotification.InPath) {
                InterlockedIncrement(&x->Paging);
            } else if (x->Paging > 0) {
                InterlockedDecrement(&x->Paging);
            }
        }
        st = STATUS_SUCCESS;
        break;
    case IRP_MN_QUERY_RESOURCES:
    case IRP_MN_QUERY_RESOURCE_REQUIREMENTS:
    case IRP_MN_FILTER_RESOURCE_REQUIREMENTS:
    default:
        break;
    }
    Irp->IoStatus.Status = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

NTSTATUS WdmPdoPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = Irp->IoStatus.Status;

    if (((WDM_EXT *)Dev->DeviceExtension)->Child == PDO_USB) {
        return UsbdiPower(((WDM_EXT *)Dev->DeviceExtension)->Func, Irp);
    }

    if (sp->MinorFunction == IRP_MN_SET_POWER || sp->MinorFunction == IRP_MN_QUERY_POWER) {
        if (sp->MinorFunction == IRP_MN_SET_POWER && sp->Parameters.Power.Type == DevicePowerState) {
            PoSetPowerState(Dev, DevicePowerState, sp->Parameters.Power.State);
        }
        st = STATUS_SUCCESS;
    } else if (sp->MinorFunction == IRP_MN_WAIT_WAKE) {
        st = STATUS_NOT_SUPPORTED;
    }
    PoStartNextPowerIrp(Irp);
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

/* ------------------------------------------------------------------ */
/* Children: I/O                                                        */
/* ------------------------------------------------------------------ */

static NTSTATUS StorageProperty(WDM_EXT *x, PIRP Irp, ULONG InLen, ULONG OutLen)
{
    PSTORAGE_PROPERTY_QUERY q = (PSTORAGE_PROPERTY_QUERY)Irp->AssociatedIrp.SystemBuffer;

    if (InLen < sizeof(STORAGE_PROPERTY_QUERY) - 1) {
        return WdmComplete(Irp, STATUS_INVALID_PARAMETER, 0);
    }
    if (q->PropertyId == StorageAdapterProperty) {
        STORAGE_ADAPTER_DESCRIPTOR a;
        RtlZeroMemory(&a, sizeof(a));
        a.Version = sizeof(a);
        a.Size = sizeof(a);
        a.MaximumTransferLength = 0x10000;
        a.MaximumPhysicalPages = 0x10000 / PAGE_SIZE + 1;
        a.AlignmentMask = 1;
        a.AdapterUsesPio = FALSE;
        a.AdapterScansDown = FALSE;
        a.CommandQueueing = FALSE;
        a.AcceleratedTransfer = FALSE;
        a.BusType = BusTypeUsb;
        a.BusMajorVersion = 2;
        if (q->QueryType == PropertyExistsQuery) {
            return WdmComplete(Irp, STATUS_SUCCESS, 0);
        }
        if (OutLen < sizeof(STORAGE_DESCRIPTOR_HEADER)) {
            return WdmComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        if (OutLen > sizeof(a)) {
            OutLen = sizeof(a);
        }
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, &a, OutLen);
        return WdmComplete(Irp, STATUS_SUCCESS, OutLen);
    }
    if (q->PropertyId == StorageDeviceProperty) {
        UCHAR buf[sizeof(STORAGE_DEVICE_DESCRIPTOR) + 64];
        PSTORAGE_DEVICE_DESCRIPTOR d = (PSTORAGE_DEVICE_DESCRIPTOR)buf;
        ULONG off = sizeof(STORAGE_DEVICE_DESCRIPTOR), i;
        RtlZeroMemory(buf, sizeof(buf));
        d->Version = sizeof(STORAGE_DEVICE_DESCRIPTOR);
        d->DeviceType = (UCHAR)(x->Inquiry[0] & 0x1F);
        d->DeviceTypeModifier = (UCHAR)(x->Inquiry[1] & 0x7F);
        d->RemovableMedia = (BOOLEAN)((x->Inquiry[1] & 0x80) != 0);
        d->CommandQueueing = FALSE;
        d->BusType = BusTypeUsb;
        d->VendorIdOffset = off;
        for (i = 0; i < 8; i++) buf[off++] = x->Inquiry[8 + i];
        buf[off++] = 0;
        d->ProductIdOffset = off;
        for (i = 0; i < 16; i++) buf[off++] = x->Inquiry[16 + i];
        buf[off++] = 0;
        d->ProductRevisionOffset = off;
        for (i = 0; i < 4; i++) buf[off++] = x->Inquiry[32 + i];
        buf[off++] = 0;
        d->SerialNumberOffset = 0;
        d->Size = off;
        if (q->QueryType == PropertyExistsQuery) {
            return WdmComplete(Irp, STATUS_SUCCESS, 0);
        }
        if (OutLen < sizeof(STORAGE_DESCRIPTOR_HEADER)) {
            return WdmComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        }
        if (OutLen > off) {
            OutLen = off;
        }
        RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, buf, OutLen);
        return WdmComplete(Irp, STATUS_SUCCESS, OutLen);
    }
    return WdmComplete(Irp, STATUS_NOT_SUPPORTED, 0);
}

NTSTATUS WdmPdoDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st;

    if (x->Child == PDO_USB || x->Child == PDO_AUDIO || x->Child == PDO_HUB || x->Child == PDO_UDEV) {
        return WdmComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (x->Child == PDO_KBD || x->Child == PDO_MOUSE) {
        x->Nt.Kind = (x->Child == PDO_KBD) ? EXT_KBD : EXT_MOUSE;
        st = NtInputIoctl(Dev, Irp);
        x->Nt.Kind = EXT_PDO;
        return st;
    }
    /* a removed PDO gets requests again once a driver is added to it anew */
    if (!UsbScsiSlotPresent(x->Slot) || !DiskWanted(x)) {
        return WdmComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
    }
    switch (code) {
    case IOCTL_STORAGE_QUERY_PROPERTY:
        return StorageProperty(x, Irp, inLen, outLen);
    case IOCTL_SCSI_GET_ADDRESS:
        if (outLen < sizeof(SCSI_ADDRESS)) {
            return WdmComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        } else {
            PSCSI_ADDRESS a = (PSCSI_ADDRESS)Irp->AssociatedIrp.SystemBuffer;
            a->Length = sizeof(SCSI_ADDRESS);
            a->PortNumber = (UCHAR)NtScsiPortNumber;
            a->PathId = 0;
            a->TargetId = (UCHAR)x->Slot;
            a->Lun = 0;
            return WdmComplete(Irp, STATUS_SUCCESS, sizeof(SCSI_ADDRESS));
        }
    case IOCTL_SCSI_PASS_THROUGH:
        st = NtScsiPassThroughSlot(Irp, inLen, outLen, (ULONG)x->Slot);
        Irp->IoStatus.Status = st;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return st;
    case IOCTL_STORAGE_GET_HOTPLUG_INFO:
        if (outLen < sizeof(STORAGE_HOTPLUG_INFO)) {
            return WdmComplete(Irp, STATUS_BUFFER_TOO_SMALL, 0);
        } else {
            PSTORAGE_HOTPLUG_INFO h = (PSTORAGE_HOTPLUG_INFO)Irp->AssociatedIrp.SystemBuffer;
            h->Size = sizeof(*h);
            h->MediaRemovable = (BOOLEAN)((x->Inquiry[1] & 0x80) != 0);
            h->MediaHotplug = TRUE;
            h->DeviceHotplug = TRUE;
            h->WriteCacheEnableOverride = FALSE;
            return WdmComplete(Irp, STATUS_SUCCESS, sizeof(*h));
        }
    default:
        return WdmComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
}

NTSTATUS WdmPdoInternal(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    NTSTATUS st;

    if (x->Child == PDO_USB) {
        return UsbdiInternal(x->Func, Irp);
    }
    if (x->Child == PDO_AUDIO || x->Child == PDO_HUB || x->Child == PDO_UDEV) {
        return WdmComplete(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    if (x->Child == PDO_KBD || x->Child == PDO_MOUSE) {
        x->Nt.Kind = (x->Child == PDO_KBD) ? EXT_KBD : EXT_MOUSE;
        st = NtInputIoctl(Dev, Irp);
        x->Nt.Kind = EXT_PDO;
        return st;
    }
    /* IRP_MJ_SCSI: disk.sys / cdrom.sys */
    if (!DiskWanted(x)) {
        PSCSI_REQUEST_BLOCK srb = IoGetCurrentIrpStackLocation(Irp)->Parameters.Scsi.Srb;
        if (srb != NULL) {
            srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        }
        return WdmComplete(Irp, STATUS_DEVICE_DOES_NOT_EXIST, 0);
    }
    return NtScsiSrbSlot(Dev, Irp, (ULONG)x->Slot);
}
