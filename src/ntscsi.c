/*
 * ntscsi.c - virtual SCSI port (\Device\ScsiPortN) through which disk.sys
 * and cdrom.sys see the USB mass storage slots.
 */

#include "ntdrv.h"
#ifdef USB_WDM
#include "wdm.h"
#endif

ULONG NtScsiPortNumber;

static PDEVICE_OBJECT ScsiDevice;
static BOOLEAN Claimed[SCSI_MAX_SLOTS];
static IO_SCSI_CAPABILITIES Caps;
static KSPIN_LOCK ClassLock;
static BOOLEAN ClassLockInit;

#define NT_SREQ_PAGES   (0x10000 / PAGE_SIZE + 2)

typedef struct _NT_SREQ {
    SCSI_REQ                Req;
    PIRP                    Irp;
    PSCSI_REQUEST_BLOCK     Srb;
    PKEVENT                 Event;
#ifdef USB_WDM
    ULONG                   Pfn[NT_SREQ_PAGES]; /* page frames as 32 bit numbers (x64, PAE) */
#endif
} NT_SREQ;

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

NTSTATUS NtScsiCreate(PDRIVER_OBJECT Drv)
{
    PCONFIGURATION_INFORMATION ci = IoGetConfigurationInformation();
    WCHAR name[64];
    WCHAR link[64];
    UNICODE_STRING uname;
    UNICODE_STRING ulink;
    NT_EXT *ext;
    NTSTATUS st;
    ULONG len;

    KeInitializeSpinLock(&ClassLock);
    ClassLockInit = TRUE;
    NtScsiPortNumber = ci->ScsiPortCount;
    MakeName(name, L"\\Device\\ScsiPort", NtScsiPortNumber);
    RtlInitUnicodeString(&uname, name);
    st = IoCreateDevice(Drv, sizeof(NT_EXT), &uname, FILE_DEVICE_CONTROLLER, 0, FALSE, &ScsiDevice);
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "cannot create SCSI port device (%x)\n", st);
        return st;
    }
    ext = (NT_EXT *)ScsiDevice->DeviceExtension;
    ext->Kind = EXT_SCSI;
    ext->Number = NtScsiPortNumber;
    ext->Self = ScsiDevice;
    ScsiDevice->Flags |= DO_DIRECT_IO;
    ScsiDevice->AlignmentRequirement = FILE_WORD_ALIGNMENT;

    MakeName(link, L"\\DosDevices\\Scsi", NtScsiPortNumber);
    len = 0;
    while (link[len]) {
        len++;
    }
    link[len++] = L':';
    link[len] = 0;
    RtlInitUnicodeString(&ulink, link);
    IoCreateSymbolicLink(&ulink, &uname);

    ci->ScsiPortCount++;

    Caps.Length = sizeof(Caps);
    Caps.MaximumTransferLength = 0x100000;
    Caps.MaximumPhysicalPages = 0x100000 / PAGE_SIZE + 1;
    Caps.SupportedAsynchronousEvents = 0;
    Caps.AlignmentMask = 1;
    Caps.TaggedQueuing = FALSE;
    Caps.AdapterScansDown = FALSE;
    Caps.AdapterUsesPio = TRUE;

#ifndef USB_WDM
    /* the disk driver makes the partition devices of a slot when it starts, and only then */
    UsbScsiPlaceholder(OsGetConfig("MaxPartitions", 8));
#endif
    ScsiDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    UsbLog(LOG_INFO, "SCSI port %u created, %u slot(s)\n", NtScsiPortNumber, UsbScsiSlotCount);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Media change: the class driver device objects (disk partitions) that  */
/* talked to a slot are remembered; when the medium goes away or comes   */
/* back every mounted one gets DO_VERIFY_VOLUME, so the file system      */
/* re-verifies the volume (same stick: carries on; another stick or no   */
/* stick: the old volume is dismounted) instead of serving its cache or  */
/* reading the new medium through stale FAT/MFT data.                    */
/* ------------------------------------------------------------------ */
#define NT_CLASS_DEVS   26              /* partition 0 and up to 24 partitions (MaxPartitions) */

typedef struct _CLASS_DEV {
    PDEVICE_OBJECT Dev;
    PDRIVER_OBJECT Drv;
} CLASS_DEV;

static CLASS_DEV ClassDevs[SCSI_MAX_SLOTS][NT_CLASS_DEVS];

static void NoteClassDevice(ULONG Slot, PIRP Irp)
{
    PIO_STACK_LOCATION up;
    PDEVICE_OBJECT d;
    KIRQL irql;
    int i, free = -1;

    if (Slot >= SCSI_MAX_SLOTS || Irp->CurrentLocation >= Irp->StackCount) {
        return;
    }
    up = IoGetCurrentIrpStackLocation(Irp) + 1;
    d = up->DeviceObject;
    if (d == NULL || d == ScsiDevice || d->DeviceType != FILE_DEVICE_DISK || d->DriverObject == NULL) {
        return;
    }
    if (!ClassLockInit) {
        KeInitializeSpinLock(&ClassLock);
        ClassLockInit = TRUE;
    }
    KeAcquireSpinLock(&ClassLock, &irql);
    for (i = 0; i < NT_CLASS_DEVS; i++) {
        if (ClassDevs[Slot][i].Dev == d) {
            free = -2;
            break;
        }
        if (ClassDevs[Slot][i].Dev == NULL && free == -1) {
            free = i;
        }
    }
    if (free >= 0) {
        ClassDevs[Slot][free].Dev = d;
        ClassDevs[Slot][free].Drv = d->DriverObject;
    }
    KeReleaseSpinLock(&ClassLock, irql);
}

/* the device object still exists: it is on its driver's list */
static BOOLEAN ClassDevAlive(CLASS_DEV *c)
{
    PDEVICE_OBJECT d;

    for (d = c->Drv->DeviceObject; d != NULL; d = d->NextDevice) {
        if (d == c->Dev) {
            return TRUE;
        }
    }
    return FALSE;
}

void OsScsiMediaChange(int Slot)
{
    KIRQL irql;
    int i, n = 0;

    if (Slot < 0 || Slot >= SCSI_MAX_SLOTS || !ClassLockInit) {
        return;
    }
    KeAcquireSpinLock(&ClassLock, &irql);
    for (i = 0; i < NT_CLASS_DEVS; i++) {
        CLASS_DEV *c = &ClassDevs[Slot][i];
        if (c->Dev == NULL) {
            continue;
        }
        if (!ClassDevAlive(c)) {
            c->Dev = NULL;
            continue;
        }
        if (c->Dev->Vpb != NULL && (c->Dev->Vpb->Flags & VPB_MOUNTED)) {
            c->Dev->Flags |= DO_VERIFY_VOLUME;
            n++;
        }
    }
    KeReleaseSpinLock(&ClassLock, irql);
    if (n != 0) {
        UsbLog(LOG_INFO, "slot %d: medium changed, %d mounted volume(s) marked for verification\n", Slot, n);
    }
}

static NTSTATUS SrbToStatus(UCHAR SrbStatus)
{
    switch (SrbStatus & 0x3F) {
    case SRB_STATUS_SUCCESS:
    case SRB_STATUS_DATA_OVERRUN:
        return STATUS_SUCCESS;
    case SRB_STATUS_SELECTION_TIMEOUT:
    case SRB_STATUS_NO_DEVICE:
        return STATUS_DEVICE_DOES_NOT_EXIST;
    case SRB_STATUS_INVALID_REQUEST:
        return STATUS_INVALID_DEVICE_REQUEST;
    default:
        return STATUS_IO_DEVICE_ERROR;
    }
}

/* failed or short commands of the disk driver and file systems go to the log
   (it is in usbtree /dump): what the disk saw is the first thing to know when
   a medium cannot be read.  No medium (02/3A) and the media change note
   (06/28) are normal and left out. */
static ULONG LoggedFailures;

static void LogFailure(PSCSI_REQUEST_BLOCK Srb, SCSI_REQ *Req)
{
    UCHAR op = Srb->Cdb[0], key = 0, asc = 0, ascq = 0;
    ULONG lba = 0, cnt = 0;
    BOOLEAN failed = (Req->SrbStatus & 0x3F) != SRB_STATUS_SUCCESS;
    BOOLEAN shortRw = !failed && (op == 0x28 || op == 0x2A || op == 0x88 || op == 0x8A || op == 0xA8 || op == 0xAA) &&
                      Req->Transferred < Srb->DataTransferLength;

    if ((!failed && !shortRw) || LoggedFailures >= 200) {
        return;
    }
    if ((Req->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) && Req->SenseLength >= 14) {
        key = (UCHAR)(Req->Sense[2] & 0x0F);
        asc = Req->Sense[12];
        ascq = Req->Sense[13];
    }
    if ((key == 0x02 && asc == 0x3A) || (key == 0x06 && asc == 0x28) || op == 0x00) {
        return;
    }
    if (op == 0x28 || op == 0x2A) {
        lba = ((ULONG)Srb->Cdb[2] << 24) | ((ULONG)Srb->Cdb[3] << 16) | ((ULONG)Srb->Cdb[4] << 8) | Srb->Cdb[5];
        cnt = ((ULONG)Srb->Cdb[7] << 8) | Srb->Cdb[8];
    } else if (op == 0x88 || op == 0x8A) {
        lba = ((ULONG)Srb->Cdb[6] << 24) | ((ULONG)Srb->Cdb[7] << 16) | ((ULONG)Srb->Cdb[8] << 8) | Srb->Cdb[9];
        cnt = ((ULONG)Srb->Cdb[12] << 8) | Srb->Cdb[13];
    }
    LoggedFailures++;
    UsbLog(LOG_INFO, "disk %u: command %02x lba %u count %u (%u bytes) %s: srb %02x scsi %02x sense %x/%02x/%02x, %u bytes done\n",
           Srb->TargetId, op, lba, cnt, Srb->DataTransferLength, failed ? "failed" : "short", Req->SrbStatus,
           Req->ScsiStatus, key, asc, ascq, Req->Transferred);
}

/* Worker context */
static void SrbDone(SCSI_REQ *Req)
{
    NT_SREQ *r = (NT_SREQ *)Req;
    PSCSI_REQUEST_BLOCK srb = r->Srb;
    PIRP irp = r->Irp;

    if (r->Event != NULL) {
        KeSetEvent(r->Event, 0, FALSE);
        return;
    }
    srb->SrbStatus = Req->SrbStatus & ~SRB_STATUS_AUTOSENSE_VALID;
    srb->ScsiStatus = Req->ScsiStatus;
    LogFailure(srb, Req);
    if (Req->Direction != SCSI_DIR_NONE) {
        srb->DataTransferLength = Req->Transferred;
    }
    if ((Req->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) && Req->SenseLength != 0 &&
        srb->SenseInfoBuffer != NULL && srb->SenseInfoBufferLength != 0 &&
        !(srb->SrbFlags & SRB_FLAGS_DISABLE_AUTOSENSE)) {
        ULONG n = Req->SenseLength;
        if (n > srb->SenseInfoBufferLength) {
            n = srb->SenseInfoBufferLength;
        }
        RtlMoveMemory(srb->SenseInfoBuffer, Req->Sense, n);
        srb->SenseInfoBufferLength = (UCHAR)n;
        srb->SrbStatus |= SRB_STATUS_AUTOSENSE_VALID;
    }
    irp->IoStatus.Status = SrbToStatus(srb->SrbStatus);
    irp->IoStatus.Information = (Req->Direction != SCSI_DIR_NONE) ? Req->Transferred : 0;
    OsFree(r);
    IoCompleteRequest(irp, IO_DISK_INCREMENT);
}

static NTSTATUS CompleteSrb(PIRP Irp, PSCSI_REQUEST_BLOCK Srb, UCHAR SrbStatus, NTSTATUS Status)
{
    Srb->SrbStatus = SrbStatus;
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

/* Page frames of the SRB data from the IRP's MDL, for transfers without copying */
static void SrbPages(PIRP Irp, PSCSI_REQUEST_BLOCK Srb, SCSI_REQ *Req)
{
    PMDL mdl = Irp->MdlAddress;
    ULONG ofs;

    Req->Pfn = NULL;
    Req->PageOff = 0;
    if (mdl == NULL || Srb->DataTransferLength == 0) {
        return;
    }
    if ((UCHAR *)Srb->DataBuffer < (UCHAR *)MmGetMdlVirtualAddress(mdl)) {
        return;
    }
    ofs = (ULONG)((UCHAR *)Srb->DataBuffer - (UCHAR *)MmGetMdlVirtualAddress(mdl));
    if (ofs + Srb->DataTransferLength > MmGetMdlByteCount(mdl)) {
        return;
    }
    ofs += MmGetMdlByteOffset(mdl);
#ifdef USB_WDM
    {
        /* MDL page frames are PFN_NUMBERs (64 bit on x64); pages above 4 GB go through the bounce buffer */
        PPFN_NUMBER pf = (PPFN_NUMBER)(mdl + 1) + ofs / PAGE_SIZE;
        ULONG n = ((ofs % PAGE_SIZE) + Srb->DataTransferLength + PAGE_SIZE - 1) / PAGE_SIZE, i;
        NT_SREQ *r = (NT_SREQ *)Req;
        if (n > NT_SREQ_PAGES) {
            return;
        }
        for (i = 0; i < n; i++) {
            if ((ULONGLONG)pf[i] >= 0x100000ULL) {
                return;
            }
            r->Pfn[i] = (ULONG)pf[i];
        }
        Req->Pfn = r->Pfn;
    }
#else
    Req->Pfn = (ULONG *)(mdl + 1) + ofs / PAGE_SIZE;
#endif
    Req->PageOff = ofs % PAGE_SIZE;
}

static void *SrbData(PIRP Irp, PSCSI_REQUEST_BLOCK Srb)
{
    PMDL mdl = Irp->MdlAddress;

    if (Srb->DataTransferLength == 0) {
        return NULL;
    }
    if (mdl != NULL) {
        UCHAR *sys = (UCHAR *)MmGetSystemAddressForMdl(mdl);
        UCHAR *base = (UCHAR *)MmGetMdlVirtualAddress(mdl);
        if (sys == NULL) {
            return NULL;
        }
        return sys + ((UCHAR *)Srb->DataBuffer - base);
    }
    return Srb->DataBuffer;
}

static NTSTATUS SrbForSlot(PDEVICE_OBJECT Dev, PIRP Irp, PSCSI_REQUEST_BLOCK srb, ULONG t);

NTSTATUS NtScsiSrb(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK srb = sp->Parameters.Scsi.Srb;
    ULONG t;

    if (srb == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }
    t = srb->TargetId;
    if (srb->PathId != 0 || t >= UsbScsiSlotCount || srb->Lun != 0) {
        return CompleteSrb(Irp, srb, SRB_STATUS_SELECTION_TIMEOUT, STATUS_DEVICE_DOES_NOT_EXIST);
    }
    return SrbForSlot(Dev, Irp, srb, t);
}

/* Windows 2000 / XP: the disk PDO of a slot (the SRB address fields are not used) */
NTSTATUS NtScsiSrbSlot(PDEVICE_OBJECT Dev, PIRP Irp, ULONG Slot)
{
    PSCSI_REQUEST_BLOCK srb = IoGetCurrentIrpStackLocation(Irp)->Parameters.Scsi.Srb;

    if (srb == NULL) {
        Irp->IoStatus.Status = STATUS_INVALID_PARAMETER;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_INVALID_PARAMETER;
    }
    if (Slot >= UsbScsiSlotCount) {
        return CompleteSrb(Irp, srb, SRB_STATUS_NO_DEVICE, STATUS_DEVICE_DOES_NOT_EXIST);
    }
    return SrbForSlot(Dev, Irp, srb, Slot);
}

static NTSTATUS SrbForSlot(PDEVICE_OBJECT Dev, PIRP Irp, PSCSI_REQUEST_BLOCK srb, ULONG t)
{
    NT_SREQ *r;

    switch (srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        break;
    case SRB_FUNCTION_CLAIM_DEVICE:
    case SRB_FUNCTION_ATTACH_DEVICE:
#ifndef USB_WDM
        if (Claimed[t] && srb->Function == SRB_FUNCTION_CLAIM_DEVICE) {
            return CompleteSrb(Irp, srb, SRB_STATUS_BUSY, STATUS_DEVICE_BUSY);
        }
#endif
        /* Windows 2000 / XP: the PDO of the slot has one function driver at a time; a new one
           (after REMOVE_DEVICE, which sends no RELEASE) claims it again */
        Claimed[t] = TRUE;
        srb->DataBuffer = Dev;
        return CompleteSrb(Irp, srb, SRB_STATUS_SUCCESS, STATUS_SUCCESS);
    case SRB_FUNCTION_RELEASE_DEVICE:
    case SRB_FUNCTION_REMOVE_DEVICE:
        Claimed[t] = FALSE;
        return CompleteSrb(Irp, srb, SRB_STATUS_SUCCESS, STATUS_SUCCESS);
    case SRB_FUNCTION_SHUTDOWN:
    case SRB_FUNCTION_FLUSH:
    case SRB_FUNCTION_RELEASE_QUEUE:
    case SRB_FUNCTION_FLUSH_QUEUE:
    case SRB_FUNCTION_ABORT_COMMAND:
    case SRB_FUNCTION_RESET_BUS:
    case SRB_FUNCTION_RESET_DEVICE:
    case SRB_FUNCTION_RELEASE_RECOVERY:
    case SRB_FUNCTION_TERMINATE_IO:
        return CompleteSrb(Irp, srb, SRB_STATUS_SUCCESS, STATUS_SUCCESS);
    default:
        return CompleteSrb(Irp, srb, SRB_STATUS_INVALID_REQUEST, STATUS_INVALID_DEVICE_REQUEST);
    }

    if (srb->CdbLength == 0 || srb->CdbLength > 16) {
        return CompleteSrb(Irp, srb, SRB_STATUS_INVALID_REQUEST, STATUS_INVALID_DEVICE_REQUEST);
    }
    NoteClassDevice(t, Irp);
    r = (NT_SREQ *)OsAlloc(sizeof(NT_SREQ));
    if (r == NULL) {
        return CompleteSrb(Irp, srb, SRB_STATUS_ERROR, STATUS_INSUFFICIENT_RESOURCES);
    }
    r->Irp = Irp;
    r->Srb = srb;
    r->Req.Target = (UCHAR)t;
    r->Req.Lun = 0;
    r->Req.CdbLength = srb->CdbLength;
    RtlMoveMemory(r->Req.Cdb, srb->Cdb, srb->CdbLength);
    r->Req.DataLength = srb->DataTransferLength;
    if (srb->DataTransferLength != 0 && (srb->SrbFlags & SRB_FLAGS_DATA_IN)) {
        r->Req.Direction = SCSI_DIR_IN;
    } else if (srb->DataTransferLength != 0 && (srb->SrbFlags & SRB_FLAGS_DATA_OUT)) {
        r->Req.Direction = SCSI_DIR_OUT;
    } else {
        r->Req.Direction = SCSI_DIR_NONE;
        r->Req.DataLength = 0;
    }
    if (r->Req.Direction != SCSI_DIR_NONE) {
        r->Req.Data = SrbData(Irp, srb);
        if (r->Req.Data == NULL) {
            OsFree(r);
            return CompleteSrb(Irp, srb, SRB_STATUS_ERROR, STATUS_INSUFFICIENT_RESOURCES);
        }
        SrbPages(Irp, srb, &r->Req);
    }
    r->Req.Complete = SrbDone;
    srb->SrbStatus = SRB_STATUS_PENDING;
    IoMarkIrpPending(Irp);
    UsbScsiQueue(&r->Req);
    return STATUS_PENDING;
}

static NTSTATUS GetInquiryData(PIRP Irp, ULONG OutLen)
{
    PSCSI_ADAPTER_BUS_INFO info = (PSCSI_ADAPTER_BUS_INFO)Irp->AssociatedIrp.SystemBuffer;
    ULONG entry = (FIELD_OFFSET(SCSI_INQUIRY_DATA, InquiryData) + 36 + 3) & ~3UL;
    ULONG first = (sizeof(SCSI_ADAPTER_BUS_INFO) + 3) & ~3UL;
    ULONG need = first + entry * UsbScsiSlotCount;
    ULONG i;

    UsbScsiFreeze();
    if (OutLen < need) {
        Irp->IoStatus.Information = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(info, need);
    info->NumberOfBuses = 1;
    info->BusData[0].NumberOfLogicalUnits = (UCHAR)UsbScsiSlotCount;
    info->BusData[0].InitiatorBusId = 7;
    info->BusData[0].InquiryDataOffset = first;
    for (i = 0; i < UsbScsiSlotCount; i++) {
        PSCSI_INQUIRY_DATA d = (PSCSI_INQUIRY_DATA)((UCHAR *)info + first + i * entry);
        SCSI_SLOT_INFO si;
        UsbScsiGetSlot((int)i, &si);
        d->PathId = 0;
        d->TargetId = (UCHAR)i;
        d->Lun = 0;
        d->DeviceClaimed = Claimed[i];
        d->InquiryDataLength = 36;
        d->NextInquiryDataOffset = (i + 1 < UsbScsiSlotCount) ? first + (i + 1) * entry : 0;
        RtlMoveMemory(d->InquiryData, si.Inquiry, 36);
    }
    Irp->IoStatus.Information = need;
    return STATUS_SUCCESS;
}

typedef struct _PT_CTX {
    KEVENT Event;
} PT_CTX;

static NTSTATUS PassThroughAt(PIRP Irp, ULONG InLen, ULONG OutLen, ULONG Slot);

static NTSTATUS PassThrough(PIRP Irp, ULONG InLen, ULONG OutLen)
{
    PSCSI_PASS_THROUGH pt = (PSCSI_PASS_THROUGH)Irp->AssociatedIrp.SystemBuffer;

    if (InLen < sizeof(SCSI_PASS_THROUGH) || pt->Length != sizeof(SCSI_PASS_THROUGH)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pt->PathId != 0 || pt->Lun != 0 || pt->TargetId >= UsbScsiSlotCount) {
        return STATUS_INVALID_PARAMETER;
    }
    return PassThroughAt(Irp, InLen, OutLen, pt->TargetId);
}

NTSTATUS NtScsiPassThroughSlot(PIRP Irp, ULONG InLen, ULONG OutLen, ULONG Slot)
{
    PSCSI_PASS_THROUGH pt = (PSCSI_PASS_THROUGH)Irp->AssociatedIrp.SystemBuffer;

#if defined(USB_WDM) && defined(_WIN64)
    if (IoIs32bitProcess(Irp)) {
        return STATUS_NOT_SUPPORTED;            /* 32 bit layout (SCSI_PASS_THROUGH32): not translated */
    }
#endif
    if (InLen < sizeof(SCSI_PASS_THROUGH) || pt->Length != sizeof(SCSI_PASS_THROUGH) || Slot >= UsbScsiSlotCount) {
        return STATUS_INVALID_PARAMETER;
    }
    return PassThroughAt(Irp, InLen, OutLen, Slot);
}

static NTSTATUS PassThroughAt(PIRP Irp, ULONG InLen, ULONG OutLen, ULONG Slot)
{
    PSCSI_PASS_THROUGH pt = (PSCSI_PASS_THROUGH)Irp->AssociatedIrp.SystemBuffer;
    NT_SREQ *r;
    KEVENT ev;
    ULONG sz;

    if (pt->CdbLength == 0 || pt->CdbLength > 16) {
        return STATUS_INVALID_PARAMETER;
    }
    sz = InLen > OutLen ? InLen : OutLen;
    if (pt->DataTransferLength != 0 && (pt->DataBufferOffset + pt->DataTransferLength > sz ||
                                        pt->DataBufferOffset < sizeof(SCSI_PASS_THROUGH))) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pt->SenseInfoLength != 0 && pt->SenseInfoOffset + pt->SenseInfoLength > sz) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    r = (NT_SREQ *)OsAlloc(sizeof(NT_SREQ));
    if (r == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    r->Event = &ev;
    r->Req.Target = (UCHAR)Slot;
    r->Req.CdbLength = pt->CdbLength;
    RtlMoveMemory(r->Req.Cdb, pt->Cdb, pt->CdbLength);
    r->Req.DataLength = pt->DataTransferLength;
    r->Req.Data = (UCHAR *)pt + pt->DataBufferOffset;
    if (pt->DataTransferLength == 0) {
        r->Req.Direction = SCSI_DIR_NONE;
    } else {
        r->Req.Direction = (pt->DataIn == SCSI_IOCTL_DATA_IN) ? SCSI_DIR_IN : SCSI_DIR_OUT;
    }
    r->Req.Complete = SrbDone;
    UsbScsiQueue(&r->Req);
    KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);

    pt->ScsiStatus = r->Req.ScsiStatus;
    pt->DataTransferLength = r->Req.Transferred;
    if (pt->SenseInfoLength != 0 && r->Req.SenseLength != 0) {
        ULONG n = r->Req.SenseLength;
        if (n > pt->SenseInfoLength) {
            n = pt->SenseInfoLength;
        }
        RtlMoveMemory((UCHAR *)pt + pt->SenseInfoOffset, r->Req.Sense, n);
        pt->SenseInfoLength = (UCHAR)n;
    } else {
        pt->SenseInfoLength = 0;
    }
    if ((r->Req.SrbStatus & 0x3F) == SRB_STATUS_SELECTION_TIMEOUT) {
        OsFree(r);
        return STATUS_DEVICE_DOES_NOT_EXIST;
    }
    OsFree(r);
    Irp->IoStatus.Information = (pt->DataIn == SCSI_IOCTL_DATA_IN && pt->DataTransferLength != 0) ?
                                pt->DataBufferOffset + pt->DataTransferLength : sizeof(SCSI_PASS_THROUGH);
    if (pt->SenseInfoLength != 0 && pt->SenseInfoOffset + pt->SenseInfoLength > Irp->IoStatus.Information) {
        Irp->IoStatus.Information = pt->SenseInfoOffset + pt->SenseInfoLength;
    }
    if (Irp->IoStatus.Information > OutLen) {
        Irp->IoStatus.Information = OutLen;
    }
    return STATUS_SUCCESS;
}

NTSTATUS NtCtlEject(ULONG Slot)
{
    NT_SREQ *r;
    KEVENT ev;

    if (Slot >= UsbScsiSlotCount || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_PARAMETER;
    }
#ifdef USB_WDM
    if (WdmEjectSlot(Slot)) {
        return STATUS_SUCCESS;                  /* Plug and Play takes the disk away (Safely Remove Hardware) */
    }
#endif
    r = (NT_SREQ *)OsAlloc(sizeof(NT_SREQ));
    if (r == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    r->Event = &ev;
    r->Req.Target = (UCHAR)Slot;
    r->Req.Internal = 1;
    r->Req.Complete = SrbDone;
    UsbScsiQueue(&r->Req);
    KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
    OsFree(r);
    return STATUS_SUCCESS;
}

NTSTATUS NtScsiDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    NTSTATUS st;

    (void)Dev;
    Irp->IoStatus.Information = 0;
    switch (code) {
    case IOCTL_SCSI_GET_CAPABILITIES:
        if (outLen == sizeof(PVOID)) {
            *(PVOID *)Irp->AssociatedIrp.SystemBuffer = &Caps;
            Irp->IoStatus.Information = sizeof(PVOID);
            st = STATUS_SUCCESS;
        } else if (outLen >= sizeof(IO_SCSI_CAPABILITIES)) {
            RtlMoveMemory(Irp->AssociatedIrp.SystemBuffer, &Caps, sizeof(Caps));
            Irp->IoStatus.Information = sizeof(Caps);
            st = STATUS_SUCCESS;
        } else {
            st = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    case IOCTL_SCSI_GET_INQUIRY_DATA:
        st = GetInquiryData(Irp, outLen);
        break;
    case IOCTL_SCSI_GET_ADDRESS:
        if (outLen < sizeof(SCSI_ADDRESS)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            PSCSI_ADDRESS a = (PSCSI_ADDRESS)Irp->AssociatedIrp.SystemBuffer;
            a->Length = sizeof(SCSI_ADDRESS);
            a->PortNumber = (UCHAR)NtScsiPortNumber;
            a->PathId = 0;
            a->TargetId = 0;
            a->Lun = 0;
            Irp->IoStatus.Information = sizeof(SCSI_ADDRESS);
            st = STATUS_SUCCESS;
        }
        break;
    case IOCTL_SCSI_PASS_THROUGH:
        st = PassThrough(Irp, inLen, outLen);
        break;
    case IOCTL_SCSI_RESCAN_BUS:
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}
