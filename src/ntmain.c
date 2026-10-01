/*
 * ntmain.c - driver entry, dispatch routines, worker thread, poll timer
 * and optional interrupt handling for Windows NT 3.1 - 4.0.
 */

#include "ntdrv.h"

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING RegistryPath);

static KTIMER PollTimer;
static KDPC PollDpc;
static KDPC IsrDpc;
static PKINTERRUPT Interrupts[USB_MAX_HC];
static volatile LONG Stopping;

/* ------------------------------------------------------------------ */
/* Dispatch                                                             */
/* ------------------------------------------------------------------ */

static NTSTATUS Complete(PIRP Irp, NTSTATUS St)
{
    Irp->IoStatus.Status = St;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return St;
}

static NTSTATUS NTAPI DispatchCreateClose(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NT_EXT *ext = (NT_EXT *)Dev->DeviceExtension;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    if (ext->Kind == EXT_CTL) {
        if (sp->MajorFunction == IRP_MJ_CLEANUP) {
            NtCtlCleanup(Dev, Irp);
        } else {
            NtCtlCreateClose(Dev, Irp, (BOOLEAN)(sp->MajorFunction == IRP_MJ_CREATE));
        }
    }
    Irp->IoStatus.Information = 0;
    return Complete(Irp, STATUS_SUCCESS);
}

static NTSTATUS NTAPI DispatchDeviceControl(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NT_EXT *ext = (NT_EXT *)Dev->DeviceExtension;

    switch (ext->Kind) {
    case EXT_SCSI:
        return NtScsiDeviceControl(Dev, Irp);
    case EXT_KBD:
    case EXT_MOUSE:
        return NtInputIoctl(Dev, Irp);
    case EXT_CTL:
        return NtCtlIoctl(Dev, Irp);
    default:
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

static NTSTATUS NTAPI DispatchInternal(PDEVICE_OBJECT Dev, PIRP Irp)
{
    NT_EXT *ext = (NT_EXT *)Dev->DeviceExtension;

    switch (ext->Kind) {
    case EXT_SCSI:
        return NtScsiSrb(Dev, Irp);
    case EXT_KBD:
    case EXT_MOUSE:
        return NtInputIoctl(Dev, Irp);
    default:
        return Complete(Irp, STATUS_INVALID_DEVICE_REQUEST);
    }
}

static NTSTATUS NTAPI DispatchShutdown(PDEVICE_OBJECT Dev, PIRP Irp)
{
    (void)Dev;
    Irp->IoStatus.Information = 0;
    return Complete(Irp, STATUS_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* Polling and interrupts                                               */
/* ------------------------------------------------------------------ */

static void ArmTimer(void)
{
    LARGE_INTEGER due;

    due.LowPart = (ULONG)-10000;
    due.HighPart = -1;
    KeSetTimer(&PollTimer, due, &PollDpc);
}

static ULONG DpcCount;

static VOID NTAPI PollDpcRoutine(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    (void)Dpc;
    (void)Ctx;
    (void)A1;
    (void)A2;
    DpcCount++;
    UsbTick();
    if (!Stopping) {
        ArmTimer();
    }
}

static VOID NTAPI IsrDpcRoutine(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    (void)Dpc;
    (void)Ctx;
    (void)A1;
    (void)A2;
    UsbPollAll();
}

static volatile ULONG IsrCount[USB_MAX_HC];

static BOOLEAN NTAPI Isr(PKINTERRUPT Int, PVOID Ctx)
{
    USB_HC *hc = (USB_HC *)Ctx;

    (void)Int;
    if (hc->Running && hc->Ops->Interrupt != NULL && hc->Ops->Interrupt(hc)) {
        IsrCount[hc->Index]++;
        KeInsertQueueDpc(&IsrDpc, NULL, NULL);
        return TRUE;
    }
    return FALSE;
}

static void ConnectInterrupts(void)
{
    int i;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        KIRQL irql = 0;
        KAFFINITY aff = 0;
        ULONG vector;
        NTSTATUS st;

        if (!hc->Running || hc->IrqLine == 0 || hc->IrqLine == 0xFF) {
            continue;
        }
        vector = HalGetInterruptVector(PCIBus, hc->Bus, hc->IrqLine, hc->IrqLine, &irql, &aff);
        if (vector == 0) {
            vector = HalGetInterruptVector(Isa, 0, hc->IrqLine, hc->IrqLine, &irql, &aff);
        }
        if (vector == 0) {
            UsbLog(LOG_ERR, "%s: no interrupt vector for irq %u, polling\n", hc->Ops->Name, hc->IrqLine);
            continue;
        }
        st = IoConnectInterrupt(&Interrupts[i], Isr, hc, NULL, vector, irql, irql, LevelSensitive,
                                TRUE, aff, FALSE);
        if (!NT_SUCCESS(st)) {
            UsbLog(LOG_ERR, "%s: IoConnectInterrupt failed (%x), polling\n", hc->Ops->Name, st);
            Interrupts[i] = NULL;
        } else {
            UsbLog(LOG_INFO, "%s: irq %u connected\n", hc->Ops->Name, hc->IrqLine);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Worker thread                                                        */
/* ------------------------------------------------------------------ */

static VOID NTAPI WorkerThread(PVOID Ctx)
{
    LARGE_INTEGER t;
    ULONG start = OsTimeMs();
    BOOLEAN reported = FALSE;

    (void)Ctx;
    KeSetPriorityThread(KeGetCurrentThread(), LOW_REALTIME_PRIORITY);
    t.LowPart = (ULONG)(-(LONG)(200 * 10000));
    t.HighPart = -1;
    for (;;) {
        KeWaitForSingleObject(&NtWorkerEvent, Executive, KernelMode, FALSE, &t);
        UsbWorkerIteration();
        if (!reported && OsTimeMs() - start > 20000) {
            int i;
            reported = TRUE;
            for (i = 0; i < UsbHcCount; i++) {
                if (Interrupts[i] != NULL) {
                    UsbLog(LOG_INFO, "%s: %u interrupts in the first 20 s\n", UsbHcs[i]->Ops->Name, IsrCount[i]);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Entry                                                                */
/* ------------------------------------------------------------------ */

NTSTATUS NTAPI DriverEntry(PDRIVER_OBJECT Drv, PUNICODE_STRING RegistryPath)
{
    HANDLE thread;
    NTSTATUS st;
    ULONG waitMs;
    ULONG start;

    NtOsInit(RegistryPath);
    NtKernelInit(Drv);
    UsbDebugLevel = OsGetConfig("DebugLevel", LOG_INFO);
    UsbLog(LOG_INFO, "USB driver %s for Windows NT 3.1-4.0, build %s\n", USBNT_VERSION_STR, __DATE__);

    Drv->MajorFunction[IRP_MJ_CREATE] = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_CLOSE] = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_CLEANUP] = DispatchCreateClose;
    Drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DispatchDeviceControl;
    Drv->MajorFunction[IRP_MJ_INTERNAL_DEVICE_CONTROL] = DispatchInternal;
    Drv->MajorFunction[IRP_MJ_SHUTDOWN] = DispatchShutdown;

    UsbScsiInit();
    KeInitializeDpc(&PollDpc, PollDpcRoutine, NULL);
    KeInitializeDpc(&IsrDpc, IsrDpcRoutine, NULL);
    KeInitializeTimer(&PollTimer);

    if (UsbInit() != USB_OK) {
        UsbLog(LOG_ERR, "no USB host controller found\n");
        return STATUS_NO_SUCH_DEVICE;
    }

    if (!OsGetConfig("DisableStorage", 0)) {
        st = NtScsiCreate(Drv);
        if (!NT_SUCCESS(st)) {
            return st;
        }
    }
    NtInputCreate(Drv);

    if (OsGetConfig("UseInterrupts", 0)) {
        ConnectInterrupts();
    }
    /* a 1 ms clock lets polling notice finished transfers sooner; the kernel
       export exists from Windows 2000 on, on NT 3.x/4.0 usbmon.exe asks for it */
    if (OsGetConfig("TimerResolution", 1)) {
        typedef ULONG (NTAPI *SET_RES)(ULONG, BOOLEAN);
        SET_RES setRes = (SET_RES)NtKernelExport("ExSetTimerResolution");
        if (setRes != NULL) {
            ULONG now = setRes(10000, TRUE);
            UsbLog(LOG_INFO, "timer resolution %u us\n", now / 10);
        } else {
            UsbLog(LOG_INFO, "timer resolution: left to usbmon\n");
        }
    }
    if (!OsGetConfig("NoPollTimer", 0)) {
        ArmTimer();
    }

    /* configure devices present at boot before the class drivers look */
    waitMs = OsGetConfig("BootWaitMs", 1500);
    start = OsTimeMs();
    do {
        UsbWorkerIteration();
        OsSleepMs(100);
    } while (OsTimeMs() - start < waitMs);
    UsbWorkerIteration();

    st = PsCreateSystemThread(&thread, 0, NULL, NULL, NULL, WorkerThread, NULL);
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "cannot create worker thread (%x)\n", st);
        return st;
    }
    ZwClose(thread);
    UsbLog(LOG_INFO, "initialization complete\n");
    return STATUS_SUCCESS;
}
