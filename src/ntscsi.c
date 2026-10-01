/*
 * ntscsi.c - virtual SCSI port (\Device\ScsiPortN) through which disk.sys
 * and cdrom.sys see the USB mass storage slots.
 */

#include "ntdrv.h"

ULONG NtScsiPortNumber;

static PDEVICE_OBJECT ScsiDevice;
static BOOLEAN Claimed[SCSI_MAX_SLOTS];
static IO_SCSI_CAPABILITIES Caps;

typedef struct _NT_SREQ {
    SCSI_REQ                Req;
    PIRP                    Irp;
    PSCSI_REQUEST_BLOCK     Srb;
    PKEVENT                 Event;
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

    ScsiDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    UsbLog(LOG_INFO, "SCSI port %u created, %u slot(s)\n", NtScsiPortNumber, UsbScsiSlotCount);
    return STATUS_SUCCESS;
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
    Req->Pfn = (ULONG *)(mdl + 1) + ofs / PAGE_SIZE;
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

NTSTATUS NtScsiSrb(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PSCSI_REQUEST_BLOCK srb = sp->Parameters.Scsi.Srb;
    NT_SREQ *r;
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

    switch (srb->Function) {
    case SRB_FUNCTION_EXECUTE_SCSI:
        break;
    case SRB_FUNCTION_CLAIM_DEVICE:
    case SRB_FUNCTION_ATTACH_DEVICE:
        if (Claimed[t] && srb->Function == SRB_FUNCTION_CLAIM_DEVICE) {
            return CompleteSrb(Irp, srb, SRB_STATUS_BUSY, STATUS_DEVICE_BUSY);
        }
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

static NTSTATUS PassThrough(PIRP Irp, ULONG InLen, ULONG OutLen)
{
    PSCSI_PASS_THROUGH pt = (PSCSI_PASS_THROUGH)Irp->AssociatedIrp.SystemBuffer;
    NT_SREQ *r;
    KEVENT ev;
    ULONG sz;

    if (InLen < sizeof(SCSI_PASS_THROUGH) || pt->Length != sizeof(SCSI_PASS_THROUGH)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (pt->PathId != 0 || pt->Lun != 0 || pt->TargetId >= UsbScsiSlotCount || pt->CdbLength == 0 ||
        pt->CdbLength > 16) {
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
    r->Req.Target = pt->TargetId;
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
