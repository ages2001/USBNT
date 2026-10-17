/*
 * wdmflt.c - Windows 2000 / XP: usbnt.sys as a lower filter of disk.sys on
 * the disks of Windows' own USB mass storage driver (usbstor - on Windows'
 * USB controllers, or on USBNT's with UsbPdo=2).
 *
 * disk.sys shows only the first partition of a removable medium, and usbstor
 * reports every USB stick as removable (the RMB bit of its INQUIRY data).
 * When the disk starts, the filter reads the medium's partition table; a
 * medium with several partitions (RemovableAsFixed 1; every medium with 2) is
 * reported as fixed: RemovableMedia of the StorageDeviceProperty descriptor
 * and the RMB bit of INQUIRY data are cleared on their way up, as the cfadisk
 * filter does.  Everything else passes through.
 *
 * disk.sys never looks at a fixed disk's medium again.  When the medium of a
 * disk reported as fixed goes away or changes (card readers), the filter
 * fails the disk's requests from then on (nothing is written to the new
 * medium through the old partition table) and asks Plug and Play to restart
 * the disk; a card with several partitions put into a removable reported
 * reader does the same, so that the disk comes back as fixed.
 *
 * usbinst /install (and usbmon for USB disks that come later) put usbnt into
 * the LowerFilters of the USBSTOR disk device nodes - only of those: as a
 * filter of every disk, a missing usbnt.sys would stop Windows from booting.
 */

#include "wdm.h"
#include <ntddstor.h>

#define FLT_TAG         0x46544E55UL           /* "UNTF" */
#define FLT_BUF         0x10000

typedef struct _FLT_IO {
    WDM_EXT        *X;
    PIO_WORKITEM    Work;
} FLT_IO;

/* the PDO is usbstor's (a USB disk or a LUN of a card reader): usbnt is its filter */
BOOLEAN WdmFltWanted(PDEVICE_OBJECT Pdo)
{
    UNICODE_STRING us;

    if (Pdo->DriverObject == NULL || Pdo->DriverObject == WdmDriver) {
        return FALSE;
    }
    RtlInitUnicodeString(&us, L"\\Driver\\USBSTOR");
    return RtlEqualUnicodeString(&Pdo->DriverObject->DriverName, &us, TRUE);
}

NTSTATUS WdmFltAdd(PDRIVER_OBJECT Drv, PDEVICE_OBJECT Pdo)
{
    PDEVICE_OBJECT fdo;
    WDM_EXT *x;
    NTSTATUS st;

    if (UsbScsiFixedMode == 0) {
        return STATUS_SUCCESS;                  /* RemovableAsFixed 0: not in the stack at all */
    }
    st = IoCreateDevice(Drv, sizeof(WDM_EXT), NULL, Pdo->DeviceType, Pdo->Characteristics & ~FILE_REMOVABLE_MEDIA,
                        FALSE, &fdo);
    if (!NT_SUCCESS(st)) {
        return STATUS_SUCCESS;                  /* not filtered: the disk works as it would without us */
    }
    x = (WDM_EXT *)fdo->DeviceExtension;
    RtlZeroMemory(x, sizeof(*x));
    x->Nt.Kind = EXT_DFLT;
    x->Nt.Self = fdo;
    x->Pdo = Pdo;
    x->Lower = IoAttachDeviceToDeviceStack(fdo, Pdo);
    if (x->Lower == NULL) {
        IoDeleteDevice(fdo);
        return STATUS_SUCCESS;
    }
    fdo->Flags |= x->Lower->Flags & (DO_DIRECT_IO | DO_BUFFERED_IO | DO_POWER_PAGABLE);
    fdo->DeviceType = x->Lower->DeviceType;
    fdo->Characteristics = x->Lower->Characteristics;
    fdo->AlignmentRequirement = x->Lower->AlignmentRequirement;
    fdo->Flags &= ~DO_DEVICE_INITIALIZING;
    UsbLog(LOG_INFO, "filter: on a USBSTOR disk\n");
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Our own commands to the disk (PASSIVE_LEVEL)                         */
/* ------------------------------------------------------------------ */

static NTSTATUS NTAPI ScsiDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    (void)Dev;
    (void)Irp;
    KeSetEvent((PKEVENT)Ctx, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

/* one command with In bytes of data into Buf (nonpaged); returns the SRB status, the sense key
   and code in *Sense (0 if none) */
static UCHAR FltScsi(WDM_EXT *x, const UCHAR *Cdb, UCHAR Len, UCHAR *Buf, ULONG In, ULONG *Sense)
{
    PSCSI_REQUEST_BLOCK srb;
    UCHAR *sense;
    PIRP irp;
    PIO_STACK_LOCATION sp;
    KEVENT ev;
    UCHAR status;

    *Sense = 0;
    srb = (PSCSI_REQUEST_BLOCK)ExAllocatePoolWithTag(NonPagedPool, sizeof(SCSI_REQUEST_BLOCK) + 32, FLT_TAG);
    if (srb == NULL) {
        return SRB_STATUS_ERROR;
    }
    irp = IoAllocateIrp(x->Lower->StackSize, FALSE);
    if (irp == NULL) {
        ExFreePool(srb);
        return SRB_STATUS_ERROR;
    }
    sense = (UCHAR *)(srb + 1);
    RtlZeroMemory(srb, sizeof(SCSI_REQUEST_BLOCK) + 32);
    srb->Length = sizeof(SCSI_REQUEST_BLOCK);
    srb->Function = SRB_FUNCTION_EXECUTE_SCSI;
    srb->CdbLength = Len;
    RtlCopyMemory(srb->Cdb, Cdb, Len);
    srb->SrbFlags = SRB_FLAGS_DISABLE_SYNCH_TRANSFER | SRB_FLAGS_NO_QUEUE_FREEZE |
                    (In ? SRB_FLAGS_DATA_IN : SRB_FLAGS_NO_DATA_TRANSFER);
    srb->DataBuffer = In ? Buf : NULL;
    srb->DataTransferLength = In;
    srb->TimeOutValue = 10;
    srb->SenseInfoBuffer = sense;
    srb->SenseInfoBufferLength = 18;
    srb->OriginalRequest = irp;
    if (In != 0) {
        PMDL mdl = IoAllocateMdl(Buf, In, FALSE, FALSE, irp);
        if (mdl == NULL) {
            IoFreeIrp(irp);
            ExFreePool(srb);
            return SRB_STATUS_ERROR;
        }
        MmBuildMdlForNonPagedPool(mdl);
    }
    KeInitializeEvent(&ev, NotificationEvent, FALSE);
    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_SCSI;
    sp->Parameters.Scsi.Srb = srb;
    IoSetCompletionRoutine(irp, ScsiDone, &ev, TRUE, TRUE, TRUE);
    if (IoCallDriver(x->Lower, irp) == STATUS_PENDING) {
        KeWaitForSingleObject(&ev, Executive, KernelMode, FALSE, NULL);
    }
    status = SRB_STATUS(srb->SrbStatus);
    if (status == SRB_STATUS_DATA_OVERRUN) {
        status = SRB_STATUS_SUCCESS;
    }
    if (status == SRB_STATUS_SUCCESS && In != 0 && srb->DataTransferLength < In) {
        status = SRB_STATUS_ERROR;              /* short */
    }
    if ((srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) && srb->SenseInfoBufferLength >= 14) {
        *Sense = ((ULONG)(sense[2] & 0x0F) << 8) | sense[12];
    }
    if (irp->MdlAddress != NULL) {
        IoFreeMdl(irp->MdlAddress);
    }
    IoFreeIrp(irp);
    ExFreePool(srb);
    return status;
}

typedef struct _FLT_READ {
    WDM_EXT    *X;
    ULONG       Bs;
} FLT_READ;

static BOOLEAN FltRead(void *Ctx, ULONG Lba, ULONG Count, UCHAR *Buf)
{
    FLT_READ *r = (FLT_READ *)Ctx;
    UCHAR cdb[10];
    ULONG sense;

    RtlZeroMemory(cdb, sizeof(cdb));
    cdb[0] = 0x28;
    PUTBE32(cdb + 2, Lba);
    PUTBE16(cdb + 7, (USHORT)Count);
    return (BOOLEAN)(FltScsi(r->X, cdb, 10, Buf, Count * r->Bs, &sense) == SRB_STATUS_SUCCESS);
}

/* the medium: partitions, and whether it is to be fixed; FALSE if there is no medium */
static BOOLEAN FltProbe(WDM_EXT *x, UCHAR *Parts, BOOLEAN *Fixed)
{
    UCHAR cdb[10];
    UCHAR *buf;
    ULONG sense, bs, blocks;
    int i;
    UCHAR st = SRB_STATUS_ERROR;
    FLT_READ rd;

    *Parts = 0;
    *Fixed = FALSE;
    buf = (UCHAR *)ExAllocatePoolWithTag(NonPagedPool, FLT_BUF, FLT_TAG);
    if (buf == NULL) {
        return FALSE;
    }
    for (i = 0; i < 3 && st != SRB_STATUS_SUCCESS; i++) {
        RtlZeroMemory(cdb, sizeof(cdb));
        cdb[0] = 0x25;                          /* READ CAPACITY, again after a unit attention */
        st = FltScsi(x, cdb, 10, buf, 8, &sense);
        if (st != SRB_STATUS_SUCCESS && (sense >> 8) != 0x06) {
            break;
        }
    }
    if (st != SRB_STATUS_SUCCESS) {
        ExFreePool(buf);
        return FALSE;
    }
    blocks = GETBE32(buf) + 1;
    bs = GETBE32(buf + 4);
    rd.X = x;
    rd.Bs = bs;
    *Parts = UsbCountPartitions(FltRead, &rd, buf, FLT_BUF, bs, blocks);
    *Fixed = (BOOLEAN)(UsbScsiFixedMode >= 2 || (UsbScsiFixedMode == 1 && *Parts > 1));
    if (*Fixed && UsbScsiFixedMode == 1 && !UsbSignedDisk(FltRead, &rd, buf)) {
        UsbLog(LOG_INFO, "filter: no disk signature in the MBR, the disk stays removable\n");
        *Fixed = FALSE;                         /* (Windows would mount no volume of it) */
    }
    ExFreePool(buf);
    return TRUE;
}

/* the disk is to be restarted (stale medium): Plug and Play asks for the device state */
static void FltRestart(WDM_EXT *x, const char *Why)
{
    if (!x->Gone) {
        x->Gone = TRUE;
        UsbLog(LOG_INFO, "filter: %s, the disk is restarted\n", Why);
        IoInvalidateDeviceState(x->Pdo);
    }
}

/* a card came into a removable reported reader: does it want to be fixed? */
static VOID NTAPI FltCheckWork(PDEVICE_OBJECT Dev, PVOID Ctx)
{
    FLT_IO *io = (FLT_IO *)Ctx;
    WDM_EXT *x = io->X;
    UCHAR parts;
    BOOLEAN fixed;

    (void)Dev;
    if (x->Started && !x->Fixed && !x->Gone && FltProbe(x, &parts, &fixed) && fixed) {
        FltRestart(x, "a medium with several partitions in a removable disk");
    }
    InterlockedExchange(&x->Checking, 0);
    IoFreeWorkItem(io->Work);
    ExFreePool(io);
}

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

static NTSTATUS PassDown(WDM_EXT *x, PIRP Irp)
{
    IoSkipCurrentIrpStackLocation(Irp);
    return IoCallDriver(x->Lower, Irp);
}

NTSTATUS WdmFltPass(PDEVICE_OBJECT Dev, PIRP Irp)
{
    return PassDown((WDM_EXT *)Dev->DeviceExtension, Irp);
}

NTSTATUS WdmFltPower(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;

    PoStartNextPowerIrp(Irp);
    IoSkipCurrentIrpStackLocation(Irp);
    return PoCallDriver(x->Lower, Irp);
}

NTSTATUS WdmFltPnp(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st;

    switch (sp->MinorFunction) {
    case IRP_MN_START_DEVICE:
        st = WdmForwardSync(x->Lower, Irp);
        if (NT_SUCCESS(st)) {
            UCHAR parts = 0;
            BOOLEAN fixed = FALSE, medium = FltProbe(x, &parts, &fixed);
            x->Fixed = (UCHAR)fixed;
            x->Gone = FALSE;
            x->Started = TRUE;
            if (fixed) {
                Dev->Characteristics &= ~FILE_REMOVABLE_MEDIA;
            }
            UsbLog(LOG_INFO, "filter: USBSTOR disk started, %s, %u partition(s)%s\n", medium ? "medium" : "no medium",
                   parts, fixed ? ", reported as a fixed disk" : "");
        }
        return WdmComplete(Irp, st, Irp->IoStatus.Information);
    case IRP_MN_QUERY_PNP_DEVICE_STATE:
        st = WdmForwardSync(x->Lower, Irp);
        if (x->Gone) {
            /* failed, with other resource needs: Plug and Play stops and starts the disk again */
            Irp->IoStatus.Information |= PNP_DEVICE_FAILED | PNP_DEVICE_RESOURCE_REQUIREMENTS_CHANGED;
            st = STATUS_SUCCESS;
        }
        return WdmComplete(Irp, st, Irp->IoStatus.Information);
    case IRP_MN_DEVICE_USAGE_NOTIFICATION:
        if (Dev->AttachedDevice == NULL || (Dev->AttachedDevice->Flags & DO_POWER_PAGABLE)) {
            Dev->Flags |= DO_POWER_PAGABLE;
        }
        st = WdmForwardSync(x->Lower, Irp);
        if (!(x->Lower->Flags & DO_POWER_PAGABLE)) {
            Dev->Flags &= ~DO_POWER_PAGABLE;
        }
        return WdmComplete(Irp, st, Irp->IoStatus.Information);
    case IRP_MN_STOP_DEVICE:
    case IRP_MN_SURPRISE_REMOVAL:
        x->Started = FALSE;
        return PassDown(x, Irp);
    case IRP_MN_REMOVE_DEVICE:
        x->Started = FALSE;
        st = PassDown(x, Irp);
        IoDetachDevice(x->Lower);
        IoDeleteDevice(Dev);
        return st;
    default:
        return PassDown(x, Irp);
    }
}

static NTSTATUS NTAPI PropertyDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    (void)Dev;
    (void)Ctx;
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    if (NT_SUCCESS(Irp->IoStatus.Status) &&
        Irp->IoStatus.Information > FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RemovableMedia)) {
        PSTORAGE_DEVICE_DESCRIPTOR d = (PSTORAGE_DEVICE_DESCRIPTOR)Irp->AssociatedIrp.SystemBuffer;
        d->RemovableMedia = FALSE;
        /* the INQUIRY data usbstor appends */
        if (Irp->IoStatus.Information >= FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) + 2 &&
            d->RawPropertiesLength >= 2) {
            d->RawDeviceProperties[1] &= 0x7F;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS WdmFltDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PSTORAGE_PROPERTY_QUERY q = (PSTORAGE_PROPERTY_QUERY)Irp->AssociatedIrp.SystemBuffer;

    if (x->Fixed && sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_STORAGE_QUERY_PROPERTY &&
        sp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(STORAGE_PROPERTY_QUERY) - 1 &&
        q->PropertyId == StorageDeviceProperty && q->QueryType == PropertyStandardQuery) {
        IoCopyCurrentIrpStackLocationToNext(Irp);
        IoSetCompletionRoutine(Irp, PropertyDone, NULL, TRUE, FALSE, FALSE);
        return IoCallDriver(x->Lower, Irp);
    }
    return PassDown(x, Irp);
}

static NTSTATUS NTAPI SrbDone(PDEVICE_OBJECT Dev, PIRP Irp, PVOID Ctx)
{
    WDM_EXT *x = (WDM_EXT *)Ctx;
    PSCSI_REQUEST_BLOCK srb = IoGetCurrentIrpStackLocation(Irp)->Parameters.Scsi.Srb;

    (void)Dev;
    if (Irp->PendingReturned) {
        IoMarkIrpPending(Irp);
    }
    if (srb == NULL) {
        return STATUS_SUCCESS;
    }
    if (SRB_STATUS(srb->SrbStatus) == SRB_STATUS_SUCCESS || SRB_STATUS(srb->SrbStatus) == SRB_STATUS_DATA_OVERRUN) {
        if (x->Fixed && srb->Cdb[0] == 0x12 && !(srb->Cdb[1] & 1) && srb->DataBuffer != NULL &&
            srb->DataTransferLength >= 2) {
            ((UCHAR *)srb->DataBuffer)[1] &= 0x7F;      /* not removable */
        }
    } else if ((srb->SrbStatus & SRB_STATUS_AUTOSENSE_VALID) && srb->SenseInfoBuffer != NULL &&
               srb->SenseInfoBufferLength >= 13) {
        UCHAR key = (UCHAR)(((UCHAR *)srb->SenseInfoBuffer)[2] & 0x0F), asc = ((UCHAR *)srb->SenseInfoBuffer)[12];
        if (x->Fixed && ((key == 0x06 && asc == 0x28) || (key == 0x02 && asc == 0x3A))) {
            FltRestart(x, key == 0x02 ? "the medium of a fixed reported disk went away" :
                       "the medium of a fixed reported disk changed");
        } else if (!x->Fixed && key == 0x06 && asc == 0x28 && UsbScsiFixedMode != 0 && x->Started && !x->Gone &&
                   InterlockedExchange(&x->Checking, 1) == 0) {
            /* another medium: a look at its partitions, outside the completion */
            FLT_IO *io = (FLT_IO *)ExAllocatePoolWithTag(NonPagedPool, sizeof(FLT_IO), FLT_TAG);
            if (io != NULL && (io->Work = IoAllocateWorkItem(x->Nt.Self)) != NULL) {
                io->X = x;
                IoQueueWorkItem(io->Work, FltCheckWork, DelayedWorkQueue, io);
            } else {
                if (io != NULL) {
                    ExFreePool(io);
                }
                InterlockedExchange(&x->Checking, 0);
            }
        }
    }
    return STATUS_SUCCESS;
}

/* IRP_MJ_SCSI from disk.sys */
NTSTATUS WdmFltInternal(PDEVICE_OBJECT Dev, PIRP Irp)
{
    WDM_EXT *x = (WDM_EXT *)Dev->DeviceExtension;
    PSCSI_REQUEST_BLOCK srb = IoGetCurrentIrpStackLocation(Irp)->Parameters.Scsi.Srb;

    if (srb != NULL && srb->Function == SRB_FUNCTION_EXECUTE_SCSI && x->Gone && x->Fixed) {
        /* the medium is not the one disk.sys knows: nothing goes to it until the restart */
        srb->SrbStatus = SRB_STATUS_NO_DEVICE;
        return WdmComplete(Irp, STATUS_NO_MEDIA_IN_DEVICE, 0);
    }
    if (srb == NULL || srb->Function != SRB_FUNCTION_EXECUTE_SCSI) {
        return PassDown(x, Irp);
    }
    IoCopyCurrentIrpStackLocationToNext(Irp);
    IoSetCompletionRoutine(Irp, SrbDone, x, TRUE, TRUE, TRUE);
    return IoCallDriver(x->Lower, Irp);
}
