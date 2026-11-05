/* Included by usbdi.c: worker-only controller D3/D0 with retained child objects.
 * No PnP arrival/removal is synthesized for an unchanged root device. Hubs,
 * unknown native owners and generic clients with retained device state are
 * rejected before mutation. This is not a platform ACPI implementation.
 */
static void SleepCloseWindows(USB_DEV *Dev)
{
    UD_FUNC *f;
    int i;
    for (f = Funcs; f != NULL; f = f->Next) if (f->Dev == Dev && !f->Dead)
        for (i = 0; i < UD_PIPES; i++) if (f->Pipe[i].Used) {
            AbortPipe(&f->Pipe[i]);
            UsbClosePipe(&f->Pipe[i].P);
        }
}

static void SleepBeginDevice(USB_DEV *Dev)
{
    UD_FUNC *f;
    LIST_ENTRY done;
    KIRQL irql;
    Dev->SleepConfig = Dev->ConfigValue;
    Dev->Sleeping = 1;
    KeAcquireSpinLock(&UdLock, &irql);
    for (f = Funcs; f != NULL; f = f->Next) if (f->Dev == Dev && !f->Dead) {
        f->Resetting = TRUE;
        if (f->Config) Dev->SleepConfig = f->Config;
    }
    KeReleaseSpinLock(&UdLock, irql);
    for (f = Funcs; f != NULL; f = f->Next) if (f->Dev == Dev && !f->Dead)
        UsbLog(LOG_INFO, "sleep: device %u gen %u PDO %p retained\n", Dev->Index, Dev->Gen, f->Pdo);
    HidBeginDeviceReset(Dev);
    MscBeginDeviceReset(Dev);
    RtsxUsbBeginDeviceReset(Dev);
    UvcBeginDeviceReset(Dev);
    AudioBeginDeviceReset(Dev);
    SleepCloseWindows(Dev);
    InitializeListHead(&done);
    KeAcquireSpinLock(&UdLock, &irql);
    while (!IsListEmpty(&Ep0[Dev->Index].Wait)) {
        UD_REQ *r = CONTAINING_RECORD(RemoveHeadList(&Ep0[Dev->Index].Wait), UD_REQ, Link);
        Finish(r, USBD_STATUS_CANCELED, &done);
    }
    KeReleaseSpinLock(&UdLock, irql);
    CompleteList(&done);
    UsbClosePipe(&Dev->Ep0);
    if (Dev->Hc->Ops->DevFree != NULL) Dev->Hc->Ops->DevFree(Dev);
}

static void SleepEndNative(USB_DEV *Dev, int Result)
{
    AudioEndDeviceReset(Dev, Result);
    UvcEndDeviceReset(Dev, Result);
    RtsxUsbEndDeviceReset(Dev, Result);
    MscEndDeviceReset(Dev, Result);
    HidEndDeviceReset(Dev, Result);
}

/* No owner resumes until every retained child has restored successfully. */
static int SleepRestoreDevice(USB_DEV *Dev)
{
    UD_FUNC *f;
    UCHAR *config;
    ULONG got = 0, n;
    int i, r = UsbResetDevice(Dev);
    if (r != USB_OK) return r;
    config = (UCHAR *)OsAlloc(Dev->ConfigLen);
    if (config == NULL) return USB_ERR_NOMEM;
    r = UsbGetDescriptor(Dev, USB_DT_CONFIG, Dev->ConfigIndex, 0, config, Dev->ConfigLen, &got);
    if (r == USB_OK) {
        if (got != Dev->ConfigLen) r = USB_ERR_NODEV;
        else for (n = 0; n < got; n++) if (config[n] != Dev->Config[n]) { r = USB_ERR_NODEV; break; }
    }
    OsFree(config);
    if (r == USB_OK) r = UsbCheckDeviceStrings(Dev);
    if (r == USB_OK) r = UsbControl(Dev, USB_RT_OUT | USB_RT_STD | USB_RT_DEVICE,
                          USB_REQ_SET_CONFIGURATION, Dev->SleepConfig, 0, 0, NULL, 1000, NULL);
    if (r == USB_OK) r = HidRestoreDevice(Dev);
    if (r == USB_OK) r = MscRestoreDevice(Dev);
    if (r == USB_OK) r = RtsxUsbRestoreDevice(Dev);
    if (r == USB_OK) r = UvcRestoreDevice(Dev);
    if (r == USB_OK) r = AudioRestoreDevice(Dev);
    for (f = Funcs; r == USB_OK && f != NULL; f = f->Next) {
        if (f->Dev != Dev || f->Dead) continue;
        for (i = 0; r == USB_OK && i < 32; i++)
            if (f->Iface[i].F != NULL && f->Iface[i].Alt != 0)
                r = UsbSetInterface(Dev, f->Iface[i].Num, f->Iface[i].Alt);
    }
    for (f = Funcs; r == USB_OK && f != NULL; f = f->Next) {
        if (f->Dev != Dev || f->Dead) continue;
        for (i = 0; r == USB_OK && i < UD_PIPES; i++) if (f->Pipe[i].Used) {
            USB_PIPE *p = &f->Pipe[i].P;
            p->Toggle = p->Halted = 0;
            p->IsoNext = 0;
            r = Dev->Hc->Ops->PipeOpen(p);
            if (r == USB_OK) p->Opened = 1;
            else Dev->Hc->Ops->PipeClose(p);
        }
    }
    return r;
}

static void SleepCommitDevice(USB_DEV *Dev)
{
    UD_FUNC *f;
    KIRQL irql;
    for (f = Funcs; f != NULL; f = f->Next) if (f->Dev == Dev && !f->Dead)
        UsbLog(LOG_INFO, "resume: device %u gen %u PDO %p restored\n", Dev->Index, Dev->Gen, f->Pdo);
    SleepEndNative(Dev, USB_OK);
    KeAcquireSpinLock(&UdLock, &irql);
    Ep0[Dev->Index].Reset = Ep0[Dev->Index].Retry = FALSE;
    for (f = Funcs; f != NULL; f = f->Next) if (f->Dev == Dev && !f->Dead) {
        f->ResetFailed = FALSE;
        f->Resetting = FALSE;
        /* Preserve each child's independent Suspended state. */
    }
    Dev->Sleeping = 0;
    Kick = 1;
    KeReleaseSpinLock(&UdLock, irql);
    CliEndDeviceReset(Dev, USB_OK);
}

static int SleepPreflight(USB_HC *Hc, BOOLEAN Reserve)
{
    USB_DEV *reserved[USB_MAX_PORTS + 1], *d;
    USB_HC *other;
    int port, i, count = 0, r = USB_OK;
    if (Hc->Sleeping) return USB_OK;
    if (!Hc->Running || Hc->Removed) return USB_ERR_NODEV;
    if (Hc->Ops->Release == NULL) return USB_ERR_PARAM;
    /* EHCI's CONFIGFLAG affects companions. Do not change it underneath
       active companion devices that this transaction does not own. */
    for (i = 0; i < UsbHcCount; i++) {
        other = UsbHcs[i];
        if (other->Ehci != Hc || other->Removed) continue;
        for (port = 1; port <= other->NumPorts; port++)
            if (other->RootDev[port] != NULL) return USB_ERR_BUSY;
    }
    for (port = 1; port <= Hc->NumPorts; port++) {
        d = Hc->RootDev[port];
        if (d == NULL) continue;
        if (d->Gone || !d->Ready || !d->Config || !d->ConfigLen || d->IsHub || d->Parent ||
            (d->NativeIf & ~(HidResetInterfaces(d) | MscResetInterfaces(d) |
              RtsxUsbResetInterfaces(d) | UvcResetInterfaces(d) | AudioResetInterfaces(d)))) {
            r = USB_ERR_BUSY;
            break;
        }
        if (!CliBeginDeviceReset(d)) { r = USB_ERR_BUSY; break; }
        reserved[count++] = d;
    }
    if (r != USB_OK || !Reserve)
        for (i = 0; i < count; i++) CliAbortDeviceReset(reserved[i]);
    return r;
}

/* Retain all ownership gates on failed restart/restore. A later D0 retries
 * from stopped hardware; no owner can submit into a partial controller. */
static int SleepParkController(USB_HC *Hc, BOOLEAN FailedRestore)
{
    USB_DEV *d;
    int port;
    if (FailedRestore) for (port = 1; port <= Hc->NumPorts; port++) {
        d = Hc->RootDev[port];
        if (d == NULL) continue;
        SleepCloseWindows(d);
        SleepEndNative(d, USB_ERR_IO);
        UsbClosePipe(&d->Ep0);
        if (Hc->Ops->DevFree != NULL) Hc->Ops->DevFree(d);
    }
    if (Hc->Running) Hc->Ops->Stop(Hc);
    Hc->Running = 0;
    return UsbReleaseController(Hc);
}

static int SleepResumeController(USB_HC *Hc)
{
    USB_DEV *d;
    int port, r, ports = Hc->NumPorts;
    if (!Hc->Sleeping) return USB_OK;
    /* A failed guarded release retains DMA memory. Never overwrite Priv. */
    r = UsbReleaseController(Hc);
    if (r != USB_OK) return r;
    r = Hc->Ops->Start(Hc);
    if (r != USB_OK) {
        SleepParkController(Hc, FALSE);
        return r;
    }
    if (Hc->NumPorts != ports) {
        Hc->NumPorts = (UCHAR)ports;
        SleepParkController(Hc, FALSE);
        return USB_ERR_IO;
    }
    for (port = 1; port <= Hc->NumPorts; port++) {
        d = Hc->RootDev[port];
        if (d == NULL) continue;
        if (!(Hc->Ops->PortStatus(Hc, port) & PS_CONNECT)) r = USB_ERR_NODEV;
        else r = SleepRestoreDevice(d);
        if (r == USB_ERR_NODEV) {
            /* Actual absence or changed identity needs a real PnP removal. */
            SleepEndNative(d, r);
            UsbDevDetach(d);
            Hc->RootDev[port] = NULL;
            Hc->PortRetry[port] = 0;
        } else if (r != USB_OK) {
            SleepParkController(Hc, TRUE);
            return r;
        }
    }
    for (port = 1; port <= Hc->NumPorts; port++)
        if (Hc->RootDev[port] != NULL) SleepCommitDevice(Hc->RootDev[port]);
    Hc->Sleeping = 0;
    OsWakeWorker();
    return USB_OK;
}

/* Operation: 0 query D3 (no hardware mutation), 1 enter D3, 2 resume D0.
 * Sleeping is returned even on error so the FDO can retry/roll back safely. */
int UsbdiControllerPower(UCHAR Bus, UCHAR Dev, UCHAR Fn, int Operation, BOOLEAN *Sleeping)
{
    USB_HC *hc = NULL;
    int i, port, r;
    *Sleeping = FALSE;
    for (i = 0; i < UsbHcCount; i++) if (!UsbHcs[i]->Removed &&
        UsbHcs[i]->Bus == Bus && UsbHcs[i]->Dev == Dev && UsbHcs[i]->Fn == Fn) {
        hc = UsbHcs[i]; break;
    }
    if (hc == NULL) return USB_ERR_NODEV;
    if (Operation == 2) r = SleepResumeController(hc);
    else {
        r = SleepPreflight(hc, (BOOLEAN)(Operation == 1));
        if (r == USB_OK && Operation == 1 && !hc->Sleeping) {
            hc->Sleeping = 1;
            for (port = 1; port <= hc->NumPorts; port++)
                if (hc->RootDev[port] != NULL) SleepBeginDevice(hc->RootDev[port]);
            r = SleepParkController(hc, FALSE);
        }
    }
    *Sleeping = (BOOLEAN)(hc->Sleeping != 0);
    return r;
}
