/*
 * ntcli.c - \Device\UsbNtDev: exposes every enumerated USB device to other
 * kernel drivers (RLTKLANT for Realtek USB LAN, and any other) through the
 * generic interface in usbntcli.h.  A client registers, is told when a
 * device arrives or leaves, and drives the ones it wants with control and
 * bulk/interrupt transfers.  The network counterpart of ntaudio.c.
 *
 * Copyright (C) 2026 ages2001.
 */
#ifdef USBNT_CLI_HOST_TEST
#include "ntcli_test_shim.h"
#else
#include "ntdrv.h"
#include "usbnt.h"
#include "usbos.h"
#include "usbntcli.h"
#endif

#ifndef USBNT_CLI_HOST_TEST
static PDEVICE_OBJECT DevDevice;
#endif

/* registered clients */
static UC_CLIENT g_Cli[UC_MAX_CLIENTS];
static UCHAR     g_CliUsed[UC_MAX_CLIENTS]; /* 0 free, 1 live, 2 unregistering */
static ULONG     g_Callbacks[UC_MAX_CLIENTS];
/* IDs are opaque attachment tokens, never a reusable array index. */
static ULONG     g_Ids[USB_MAX_DEVICES];
static ULONG     g_NextId;
static UCHAR     g_Active[USB_MAX_DEVICES];
static UCHAR     g_Resetting[USB_MAX_DEVICES];
static UCHAR     g_ResetFailed[USB_MAX_DEVICES];
static ULONG     g_Announcing[USB_MAX_DEVICES];
static ULONG     g_Announced[UC_MAX_CLIENTS][USB_MAX_DEVICES];

/* per-device state: claim flag and a small cache of open pipes so the data
   toggle is preserved across bulk transfers */
#define CLI_MAX_PIPES   8
static UCHAR      g_Claimed[USB_MAX_DEVICES];
static USB_PIPE  *g_Pipe[USB_MAX_DEVICES][CLI_MAX_PIPES];
static UCHAR      g_PipeEp[USB_MAX_DEVICES][CLI_MAX_PIPES];

/* ------------------------------------------------------------------ */
/* One synchronous request at a time per device. This also protects the
 * pipe cache and the shared endpoint-zero setup buffer from client races.
 * Never hold the spin lock while waiting for a transfer or a callback. */
static int AcquireDevice(ULONG Id, USB_DEV **Device)
{
    ULONG i;
    OS_IRQL irql = OsLock();
    int result = UC_ERR_NODEV;
    *Device = NULL;
    for (i = 0; Id && i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        if (g_Ids[i] != Id || !d->InUse || d->Gone || !d->Ready) continue;
        if (g_Active[i] || g_Resetting[i]) { result = UC_ERR_BUSY; break; }
        if (g_ResetFailed[i]) { result = UC_ERR_IO; break; }
        g_Active[i] = 1;
        *Device = d;
        result = UC_OK;
        break;
    }
    OsUnlock(irql);
    return result;
}
static void ReleaseDevice(USB_DEV *Device)
{
    OS_IRQL irql = OsLock();
    g_Active[Device->Index] = 0;
    OsUnlock(irql);
}
static void NotifyClient(int Slot, const UC_DEVINFO *Info, ULONG RemovedId)
{
    UC_CLIENT client;
    int device = -1, i;
    OS_IRQL irql = OsLock();
    if (g_CliUsed[Slot] != 1) { OsUnlock(irql); return; }
    if (Info) {
        for (i = 0; i < USB_MAX_DEVICES; i++)
            if (g_Ids[i] == Info->Id && UsbDevs[i].InUse &&
                UsbDevs[i].Ready && !UsbDevs[i].Gone) { device = i; break; }
        if (device < 0 || g_Announced[Slot][device] == Info->Id) {
            OsUnlock(irql); return;
        }
        g_Announced[Slot][device] = Info->Id;
        g_Announcing[device]++;
    } else {
        for (i = 0; i < USB_MAX_DEVICES; i++)
            if (g_Announced[Slot][i] == RemovedId) { device = i; break; }
        if (device < 0) { OsUnlock(irql); return; }
        g_Announced[Slot][device] = 0;
    }
    client = g_Cli[Slot];
    g_Callbacks[Slot]++;
    OsUnlock(irql);
    if (Info) {
        if (client.Arrival) client.Arrival(client.Context, Info);
    } else if (client.Removal) client.Removal(client.Context, RemovedId);
    irql = OsLock();
    g_Callbacks[Slot]--;
    if (Info) g_Announcing[device]--;
    OsUnlock(irql);
}

static void FillInfo(USB_DEV *d, UC_DEVINFO *o)
{
    ULONG k;
    UsbMemSet(o, 0, sizeof(*o));
    o->Id        = g_Ids[d->Index];
    o->Vid       = (USHORT)(d->DevDesc[8]  | (d->DevDesc[9]  << 8));
    o->Pid       = (USHORT)(d->DevDesc[10] | (d->DevDesc[11] << 8));
    o->BcdDevice = (USHORT)(d->DevDesc[12] | (d->DevDesc[13] << 8));
    o->Class     = d->DevDesc[4];
    o->SubClass  = d->DevDesc[5];
    o->Protocol  = d->DevDesc[6];
    o->Speed     = d->Speed;
    o->NumInterfaces = (d->Config && d->ConfigLen >= 5) ? d->Config[4] : 0;
    for (k = 0; k < sizeof(o->Manufacturer) - 1 && d->Mfg[k]; k++)
        o->Manufacturer[k] = d->Mfg[k];
    for (k = 0; k < sizeof(o->Product) - 1 && d->Product[k]; k++)
        o->Product[k] = d->Product[k];
}

/* find (or open and cache) the pipe for an endpoint address on a device */
static USB_PIPE *GetPipe(USB_DEV *d, UCHAR Ep)
{
    ULONG idx = d->Index, i;
    const UCHAR *ep, *end;
    USB_PIPE *p;

    for (i = 0; i < CLI_MAX_PIPES; i++)
        if (g_Pipe[idx][i] && g_PipeEp[idx][i] == Ep)
            return g_Pipe[idx][i];

    if (!d->Config) return NULL;
    end = d->Config + d->ConfigLen;
    /* UsbFindDesc stops at an interface boundary, so the scan must start at
       the interface descriptor, not the config header; then each iteration
       advances exactly one descriptor (UsbFindDesc does the advance). */
    ep = UsbFindDesc(d->Config, end, USB_DT_INTERFACE);
    if (!ep) return NULL;
    for (;;) {
        ep = UsbFindDesc(ep, end, USB_DT_ENDPOINT);
        if (!ep) return NULL;
        if (ep[0] >= 7 && ep[2] == Ep) break;
    }

    for (i = 0; i < CLI_MAX_PIPES; i++)
        if (!g_Pipe[idx][i]) break;
    if (i == CLI_MAX_PIPES) return NULL;

    p = (USB_PIPE *)OsAlloc(sizeof(USB_PIPE));
    if (!p) return NULL;
    if (UsbOpenPipe(d, p, ep) != USB_OK) { OsFree(p); return NULL; }
    g_Pipe[idx][i]   = p;
    g_PipeEp[idx][i] = Ep;
    return p;
}

static void FreePipes(ULONG idx)
{
    ULONG i;
    for (i = 0; i < CLI_MAX_PIPES; i++) {
        if (g_Pipe[idx][i]) {
            UsbClosePipe(g_Pipe[idx][i]);
            OsFree(g_Pipe[idx][i]);
            g_Pipe[idx][i] = NULL;
            g_PipeEp[idx][i] = 0;
        }
    }
}

/* The USB worker owns physical reset and detach. Never wait here for a
 * synchronous client: it may itself need the worker to finish. A client
 * with a claim or cached endpoint has device state we cannot restore, so
 * leave its hardware untouched. The lock closes the check/start race. */
int CliBeginDeviceReset(USB_DEV *Dev)
{
    ULONG idx = Dev->Index, i;
    OS_IRQL irql = OsLock();
    if (g_Active[idx] || g_Resetting[idx] || g_Claimed[idx] || g_Announcing[idx]) {
        OsUnlock(irql);
        return 0;
    }
    for (i = 0; i < CLI_MAX_PIPES; i++) {
        if (g_Pipe[idx][i]) { OsUnlock(irql); return 0; }
    }
    g_Resetting[idx] = 1;
    OsUnlock(irql);
    return 1;
}

/* Preflight rollback must not clear an earlier failed-reset gate. */
void CliAbortDeviceReset(USB_DEV *Dev)
{
    OS_IRQL irql = OsLock();
    g_Resetting[Dev->Index] = 0;
    OsUnlock(irql);
}

void CliEndDeviceReset(USB_DEV *Dev, int Result)
{
    OS_IRQL irql = OsLock();
    g_ResetFailed[Dev->Index] = (UCHAR)(Result != USB_OK);
    g_Resetting[Dev->Index] = 0;
    OsUnlock(irql);
}

/* ------------------------------------------------------------------ */
/* interface functions                                                 */
/* ------------------------------------------------------------------ */
static int UC_CALL CliRegister(const UC_CLIENT *Client)
{
    OS_IRQL irql;
    int i, slot = -1;
    if (!Client || !Client->Arrival) return UC_ERR_PARAM;
    irql = OsLock();
    for (i = 0; i < UC_MAX_CLIENTS; i++)
        if (!g_CliUsed[i]) {
            g_Cli[i] = *Client;
            UsbMemSet(g_Announced[i], 0, sizeof(g_Announced[i]));
            g_CliUsed[i] = 1; slot = i; break;
        }
    OsUnlock(irql);
    if (slot < 0) return UC_ERR_NOMEM;

    /* replay the devices already present so a client that loads after the
       hardware was plugged in still sees it */
    for (i = 0; i < (int)USB_MAX_DEVICES; i++) {
        UC_DEVINFO info;
        BOOLEAN present;
        irql = OsLock();
        present = g_Ids[i] && UsbDevs[i].InUse && UsbDevs[i].Ready && !UsbDevs[i].Gone;
        if (present) FillInfo(&UsbDevs[i], &info);
        OsUnlock(irql);
        if (present) NotifyClient(slot, &info, 0);
    }
    return UC_OK;
}

/* Unregister is a rundown barrier. It must not be called by a callback
 * of the same client: that callback is itself part of the rundown set. */
static void UC_CALL CliUnregister(const UC_CLIENT *Client)
{
    OS_IRQL irql;
    int i, slot = -1;
    if (!Client) return;
    irql = OsLock();
    for (i = 0; i < UC_MAX_CLIENTS; i++)
        if (g_CliUsed[i] == 1 && g_Cli[i].Context == Client->Context &&
            g_Cli[i].Arrival == Client->Arrival) {
            g_CliUsed[i] = 2;
            slot = i;
            break;
        }
    OsUnlock(irql);
    if (slot < 0) return;
    for (;;) {
        irql = OsLock();
        if (!g_Callbacks[slot]) {
            UsbMemSet(&g_Cli[slot], 0, sizeof(g_Cli[slot]));
            g_CliUsed[slot] = 0;
            OsUnlock(irql);
            return;
        }
        OsUnlock(irql);
        OsSleepMs(1);
    }
}

static int UC_CALL CliGetInfo(ULONG Id, UC_DEVINFO *Info)
{
    USB_DEV *d;
    int r;
    if (!Info) return UC_ERR_PARAM;
    r = AcquireDevice(Id, &d);
    if (r != UC_OK) return r;
    FillInfo(d, Info);
    ReleaseDevice(d);
    return UC_OK;
}

static int UC_CALL CliGetDescriptor(ULONG Id, UCHAR Type, UCHAR Index,
                                    void *Buf, ULONG Len, ULONG *Actual)
{
    USB_DEV *d;
    int r;
    if (Actual) *Actual = 0;
    if (!Buf || !Len || Len > 0xFFFF) return UC_ERR_PARAM;
    r = AcquireDevice(Id, &d);
    if (r != UC_OK) return r;
    r = UsbGetDescriptor(d, Type, Index, 0, Buf, (USHORT)Len, Actual);
    ReleaseDevice(d);
    return (r == USB_OK) ? UC_OK : UC_ERR_IO;
}

static int UC_CALL CliControl(ULONG Id, UCHAR ReqType, UCHAR Req, USHORT Value,
                              USHORT Index, void *Buf, USHORT Len,
                              ULONG TimeoutMs, ULONG *Actual)
{
    USB_DEV *d;
    int r;
    if (Actual) *Actual = 0;
    if (Len && !Buf) return UC_ERR_PARAM;
    r = AcquireDevice(Id, &d);
    if (r != UC_OK) return r;
    r = UsbControl(d, ReqType, Req, Value, Index, Len, Buf,
                   TimeoutMs ? TimeoutMs : 5000, Actual);
    ReleaseDevice(d);
    return (r == USB_OK) ? UC_OK : UC_ERR_IO;
}

static int UC_CALL CliTransfer(ULONG Id, UCHAR Endpoint, void *Buf, ULONG Len,
                               ULONG TimeoutMs, ULONG *Actual)
{
    USB_DEV *d;
    USB_PIPE *p;
    void *dma;
    ULONG phys, got = 0;
    int r;
    if (Actual) *Actual = 0;
    if ((Len && !Buf) || !(Endpoint & 15) || (Endpoint & 0x70)) return UC_ERR_PARAM;
    r = AcquireDevice(Id, &d);
    if (r != UC_OK) return r;
    p = GetPipe(d, Endpoint);
    if (!p) { ReleaseDevice(d); return UC_ERR_IO; }
    dma = OsDmaAlloc(Len ? Len : 1, &phys);
    if (!dma) { ReleaseDevice(d); return UC_ERR_NOMEM; }
    if (!(Endpoint & 0x80) && Len) UsbMemCpy(dma, Buf, Len);
    r = UsbTransfer(p, dma, phys, Len, TimeoutMs ? TimeoutMs : 5000, &got);
    if (got > Len) { got = 0; r = UC_ERR_IO; }
    if ((Endpoint & 0x80) && got) UsbMemCpy(Buf, dma, got);
    OsDmaFree(dma, Len ? Len : 1);
    if (Actual) *Actual = got;
    ReleaseDevice(d);
    return (r == USB_OK) ? UC_OK : UC_ERR_IO;
}

/* Version 1 has no client identity parameter, so this is advisory only. */
static int UC_CALL CliClaim(ULONG Id, int Claim)
{
    USB_DEV *d;
    int r = AcquireDevice(Id, &d);
    if (r != UC_OK) return r;
    g_Claimed[d->Index] = (UCHAR)(Claim ? 1 : 0);
    ReleaseDevice(d);
    return UC_OK;
}

static const USBNT_DEV_IF DevIf = {
    USBNT_DEV_IF_VERSION,
    sizeof(USBNT_DEV_IF),
    CliRegister,
    CliUnregister,
    CliGetInfo,
    CliGetDescriptor,
    CliControl,
    CliTransfer,
    CliClaim
};

/* ------------------------------------------------------------------ */
/* arrival / removal, called by the USB core (usbcore.c) in the worker  */
/* ------------------------------------------------------------------ */
void CliArrival(USB_DEV *Dev)
{
    UC_DEVINFO info;
    OS_IRQL irql;
    int i;
    if (!Dev) return;
    irql = OsLock();
    /* Exhaustion fails closed rather than reusing a stale 32-bit handle. */
    if (g_NextId == 0xFFFFFFFFUL) { OsUnlock(irql); return; }
    g_Ids[Dev->Index] = ++g_NextId;
    g_Resetting[Dev->Index] = g_ResetFailed[Dev->Index] = 0;
    FillInfo(Dev, &info);
    OsUnlock(irql);
    for (i = 0; i < UC_MAX_CLIENTS; i++) NotifyClient(i, &info, 0);
}

void CliDetach(USB_DEV *Dev)
{
    ULONG id;
    OS_IRQL irql;
    int i;
    if (!Dev) return;
    irql = OsLock();
    id = g_Ids[Dev->Index];
    g_Ids[Dev->Index] = 0; /* stop new requests before notifying clients */
    OsUnlock(irql);
    /* Preserve arrival-before-removal ordering, including Register replay. */
    for (;;) {
        ULONG announcing;
        irql = OsLock();
        announcing = g_Announcing[Dev->Index];
        OsUnlock(irql);
        if (!announcing) break;
        OsSleepMs(1);
    }
    if (id) for (i = 0; i < UC_MAX_CLIENTS; i++) NotifyClient(i, NULL, id);
    /* Existing synchronous calls own the pipe and DMA until they return.
     * Do not race UsbClosePipe or DevRelease against their completion.
     * Current requests finish or time out; asynchronous cancellation is a
     * separate API requirement, not provided by this v1 rundown barrier. */
    for (;;) {
        UCHAR active;
        irql = OsLock();
        active = g_Active[Dev->Index];
        OsUnlock(irql);
        if (!active) break;
        OsSleepMs(1);
    }
    FreePipes(Dev->Index);
    g_Claimed[Dev->Index] = 0;
    g_Resetting[Dev->Index] = g_ResetFailed[Dev->Index] = 0;
}

#ifndef USBNT_CLI_HOST_TEST
NTSTATUS NtCliCreate(PDRIVER_OBJECT Drv)
{
    UNICODE_STRING uname;
    NT_EXT *ext;
    NTSTATUS st;

    RtlInitUnicodeString(&uname, USBNT_DEV_DEVICE_NAME);
    st = IoCreateDevice(Drv, sizeof(USBNT_DEV_EXT), &uname, FILE_DEVICE_UNKNOWN,
                        0, FALSE, &DevDevice);
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "cannot create the device interface (%x)\n", st);
        return st;
    }
    ext = (NT_EXT *)DevDevice->DeviceExtension;
    ext->Kind = EXT_CLIENT;
    ext->Self = DevDevice;
    ((USBNT_DEV_EXT *)ext)->Magic = USBNT_DEV_EXT_MAGIC;
    ((USBNT_DEV_EXT *)ext)->If = &DevIf;
    DevDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    return STATUS_SUCCESS;
}

NTSTATUS NtCliInternal(PDEVICE_OBJECT Dev, PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS st = STATUS_INVALID_DEVICE_REQUEST;

    (void)Dev;
    Irp->IoStatus.Information = 0;
    if (sp->Parameters.DeviceIoControl.IoControlCode == IOCTL_USBNT_DEV_INTERFACE) {
        USBNT_DEV_IF *out = (USBNT_DEV_IF *)Irp->UserBuffer;
        ULONG len = sp->Parameters.DeviceIoControl.OutputBufferLength;
        if (out == NULL || len < 2 * sizeof(ULONG)) {
            st = STATUS_BUFFER_TOO_SMALL;
        } else {
            if (len > sizeof(DevIf)) len = sizeof(DevIf);
            UsbMemCpy(out, &DevIf, len);
            out->Size = len;
            Irp->IoStatus.Information = len;
            st = STATUS_SUCCESS;
        }
    }
    Irp->IoStatus.Status = st;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

#endif /* USBNT_CLI_HOST_TEST */
