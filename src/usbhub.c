/*
 * usbhub.c - hub class driver for USB 1.1 / 2.0 hubs and the SuperSpeed
 * part of USB 3 hubs.
 */

#include "usbnt.h"

#define HUB_MAX_PORTS       15

#define HUB_PORT_CONNECTION 0
#define HUB_PORT_ENABLE     1
#define HUB_PORT_RESET      4
#define HUB_PORT_POWER      8
#define HUB_C_CONNECTION    16
#define HUB_C_ENABLE        17
#define HUB_C_SUSPEND       18
#define HUB_C_OVER_CURRENT  19
#define HUB_C_RESET         20

#define HPS_CONNECTION      0x0001
#define HPS_ENABLE          0x0002
#define HPS_OVER_CURRENT    0x0008
#define HPS_RESET           0x0010
#define HPS_POWER           0x0100
#define HPS_LOW_SPEED       0x0200
#define HPS_HIGH_SPEED      0x0400

/* SuperSpeed hubs */
#define HUB_REQ_SET_DEPTH   12
#define SSH_BH_RESET        28
#define SSH_C_LINK_STATE    25
#define SSH_C_CONFIG_ERROR  26
#define SSH_C_BH_RESET      29
#define SSPS_POWER          0x0200
#define SSPS_LINK_MASK      0x01E0
#define SSPS_LINK_INACTIVE  0x00C0      /* SS.Inactive (6 << 5) */
#define SSPC_BH_RESET       0x0020
#define SSPC_LINK_STATE     0x0040
#define SSPC_CONFIG_ERROR   0x0080

typedef struct _HUB {
    USB_DEV    *Dev;
    UCHAR       Ports;
    UCHAR       Changed;
    UCHAR       PollAll;
    UCHAR       IntrActive;
    UCHAR       IntrError;
    UCHAR       Ss;             /* SuperSpeed hub */
    UCHAR       Pad[2];
    ULONG       PowerGoodMs;
    ULONG       LastPoll;
    USB_PIPE    Intr;
    USB_XFER    Xfer;
    USB_DEV    *Child[HUB_MAX_PORTS + 1];
    ULONG       Retry[HUB_MAX_PORTS + 1];
} HUB;

static HUB *Hubs[USB_MAX_DEVICES];

static int HubPortFeature(HUB *h, UCHAR Req, USHORT Feature, int Port)
{
    return UsbControl(h->Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_OTHER, Req, Feature, (USHORT)Port,
                      0, NULL, 1000, NULL);
}

static int HubPortStatus(HUB *h, int Port, USHORT *Status, USHORT *Change)
{
    UCHAR buf[4];
    ULONG got = 0;
    int r;

    r = UsbControl(h->Dev, USB_RT_IN | USB_RT_CLASS | USB_RT_OTHER, USB_REQ_GET_STATUS, 0, (USHORT)Port,
                   4, buf, 1000, &got);
    if (r != USB_OK) {
        return r;
    }
    if (got < 4) {
        return USB_ERR_IO;
    }
    *Status = GET16(buf);
    *Change = GET16(buf + 2);
    return USB_OK;
}

static void HubIntrDone(USB_XFER *X)
{
    HUB *h = (HUB *)X->Context;

    h->IntrActive = 0;
    if (X->Status == USB_OK) {
        h->Changed = 1;
    } else if (X->Status != USB_ERR_ABORTED && X->Status != USB_ERR_NODEV) {
        h->IntrError = 1;
    }
    OsWakeWorker();
}

static void HubArm(HUB *h)
{
    ULONG len = (h->Ports + 8) / 8;

    if (h->IntrActive || !h->Intr.Opened || h->Dev->Gone) {
        return;
    }
    UsbMemSet(&h->Xfer, 0, sizeof(h->Xfer));
    h->Xfer.Pipe = &h->Intr;
    h->Xfer.Buf = h->Dev->Page + USB_DEV_SCRATCH_OFF;
    h->Xfer.Phys = h->Dev->PagePhys + USB_DEV_SCRATCH_OFF;
    h->Xfer.Length = (len > h->Intr.MaxPacket) ? h->Intr.MaxPacket : len;
    h->Xfer.DirIn = 1;
    h->Xfer.Complete = HubIntrDone;
    h->Xfer.Context = h;
    h->IntrActive = 1;
    if (UsbSubmit(&h->Xfer) != USB_OK) {
        h->IntrActive = 0;
    }
}

int HubAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *ep = If;
    HUB *h;
    UCHAR desc[16];
    ULONG got = 0;
    int r;
    int port;

    if (Dev->Depth + 1 >= USB_MAX_DEPTH) {
        UsbLog(LOG_ERR, "device %u: hub nested too deep\n", Dev->Index);
        return USB_ERR_PARAM;
    }
    while ((ep = UsbFindDesc(ep, End, USB_DT_ENDPOINT)) != NULL) {
        if ((ep[3] & 3) == USB_EP_INTERRUPT && (ep[2] & 0x80)) {
            break;
        }
    }
    if (ep == NULL) {
        return USB_ERR_PARAM;
    }

    if (Dev->Speed == USB_SPEED_SUPER && !OsGetConfig("Usb3Hubs", 1)) {
        UsbLog(LOG_INFO, "device %u: USB 3 hub, its USB 2 part is used\n", Dev->Index);
        return USB_ERR_PARAM;
    }
    r = UsbControl(Dev, USB_RT_IN | USB_RT_CLASS | USB_RT_DEVICE, USB_REQ_GET_DESCRIPTOR,
                   (USHORT)(((Dev->Speed == USB_SPEED_SUPER) ? USB_DT_SS_HUB : USB_DT_HUB) << 8), 0,
                   (USHORT)((Dev->Speed == USB_SPEED_SUPER) ? 12 : sizeof(desc)), desc, 1000, &got);
    if (r != USB_OK || got < 7) {
        UsbLog(LOG_ERR, "device %u: hub descriptor failed (%d)\n", Dev->Index, r);
        return USB_ERR_IO;
    }

    h = (HUB *)OsAlloc(sizeof(HUB));
    if (h == NULL) {
        return USB_ERR_NOMEM;
    }
    h->Dev = Dev;
    h->Ports = desc[2];
    if (h->Ports > HUB_MAX_PORTS) {
        h->Ports = HUB_MAX_PORTS;
    }
    h->PowerGoodMs = (ULONG)desc[5] * 2;
    if (h->PowerGoodMs < 100) {
        h->PowerGoodMs = 100;
    }

    h->Ss = (UCHAR)(Dev->Speed == USB_SPEED_SUPER);
    if (h->Ss) {
        r = UsbControl(Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_DEVICE, HUB_REQ_SET_DEPTH, Dev->Depth, 0,
                       0, NULL, 1000, NULL);
        if (r != USB_OK) {
            UsbLog(LOG_ERR, "device %u: set hub depth failed (%d)\n", Dev->Index, r);
            OsFree(h);
            return USB_ERR_IO;
        }
    }

    Dev->IsHub = 1;
    Dev->Function |= USB_FUNC_HUB;
    Dev->HubPorts = h->Ports;
    Dev->HubTtt = (UCHAR)(h->Ss ? 0 : (GET16(desc + 3) >> 5) & 3);
    Dev->HubMtt = 0;
    Dev->Hub = h;
    Hubs[Dev->Index] = h;

    r = UsbOpenPipe(Dev, &h->Intr, ep);
    if (r != USB_OK) {
        UsbLog(LOG_ERR, "device %u: hub pipe failed (%d)\n", Dev->Index, r);
        Hubs[Dev->Index] = NULL;
        Dev->Hub = NULL;
        OsFree(h);
        return r;
    }

    for (port = 1; port <= h->Ports; port++) {
        HubPortFeature(h, USB_REQ_SET_FEATURE, HUB_PORT_POWER, port);
    }
    OsSleepMs(h->PowerGoodMs);

    UsbLog(LOG_INFO, "device %u: %shub with %u ports\n", Dev->Index, h->Ss ? "SuperSpeed " : "", h->Ports);
    h->PollAll = 1;
    OsWakeWorker();
    return USB_OK;
}

void HubRemoveChildren(USB_DEV *Dev)
{
    HUB *h = (HUB *)Dev->Hub;
    int port;

    if (h == NULL) {
        return;
    }
    for (port = 1; port <= h->Ports; port++) {
        if (h->Child[port] != NULL) {
            UsbDevDetach(h->Child[port]);
            h->Child[port] = NULL;
        }
    }
}

void HubDetach(USB_DEV *Dev)
{
    HUB *h = (HUB *)Dev->Hub;

    if (h == NULL) {
        return;
    }
    HubRemoveChildren(Dev);
    UsbClosePipe(&h->Intr);
    Hubs[Dev->Index] = NULL;
    Dev->Hub = NULL;
    OsFree(h);
}

static int HubResetPort(HUB *h, int Port, UCHAR *Speed)
{
    USHORT st = 0, ch = 0;
    ULONG start;
    int r;
    BOOLEAN warm = FALSE;

    if (h->Ss) {
        r = HubPortStatus(h, Port, &st, &ch);
        if (r != USB_OK) {
            return r;
        }
        warm = (BOOLEAN)((st & SSPS_LINK_MASK) == SSPS_LINK_INACTIVE);
    }
    r = HubPortFeature(h, USB_REQ_SET_FEATURE, (USHORT)(warm ? SSH_BH_RESET : HUB_PORT_RESET), Port);
    if (r != USB_OK) {
        return r;
    }
    start = OsTimeMs();
    for (;;) {
        OsSleepMs(20);
        r = HubPortStatus(h, Port, &st, &ch);
        if (r != USB_OK) {
            return r;
        }
        if (!(st & HPS_CONNECTION)) {
            return USB_ERR_NODEV;
        }
        if (!(st & HPS_RESET) && (ch & (HPS_RESET | (h->Ss ? SSPC_BH_RESET : 0)))) {
            break;
        }
        if (OsTimeMs() - start > 500) {
            return USB_ERR_TIMEOUT;
        }
    }
    HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_C_RESET, Port);
    if (h->Ss) {
        if (ch & SSPC_BH_RESET) {
            HubPortFeature(h, USB_REQ_CLEAR_FEATURE, SSH_C_BH_RESET, Port);
        }
        if (ch & SSPC_LINK_STATE) {
            HubPortFeature(h, USB_REQ_CLEAR_FEATURE, SSH_C_LINK_STATE, Port);
        }
    }
    if (!(st & HPS_ENABLE)) {
        return USB_ERR_IO;
    }
    if (h->Ss) {
        *Speed = USB_SPEED_SUPER;
    } else if (st & HPS_LOW_SPEED) {
        *Speed = USB_SPEED_LOW;
    } else if (st & HPS_HIGH_SPEED) {
        *Speed = USB_SPEED_HIGH;
    } else {
        *Speed = USB_SPEED_FULL;
    }
    OsSleepMs(20);
    return USB_OK;
}

static void HubServicePort(HUB *h, int Port)
{
    USHORT st = 0, ch = 0;
    USB_DEV *child;
    UCHAR speed = USB_SPEED_FULL;

    if (HubPortStatus(h, Port, &st, &ch) != USB_OK) {
        return;
    }
    if (ch & HPS_CONNECTION) HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_C_CONNECTION, Port);
    if (h->Ss) {
        if (ch & SSPC_BH_RESET)     HubPortFeature(h, USB_REQ_CLEAR_FEATURE, SSH_C_BH_RESET, Port);
        if (ch & SSPC_LINK_STATE)   HubPortFeature(h, USB_REQ_CLEAR_FEATURE, SSH_C_LINK_STATE, Port);
        if (ch & SSPC_CONFIG_ERROR) HubPortFeature(h, USB_REQ_CLEAR_FEATURE, SSH_C_CONFIG_ERROR, Port);
    } else {
        if (ch & HPS_ENABLE)     HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_C_ENABLE, Port);
        if (ch & 0x0004)         HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_C_SUSPEND, Port);
    }
    if (ch & HPS_OVER_CURRENT) {
        HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_C_OVER_CURRENT, Port);
        UsbLog(LOG_ERR, "device %u: hub port %d over current\n", h->Dev->Index, Port);
    }
    if (ch & HPS_RESET)      HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_C_RESET, Port);

    child = h->Child[Port];
    if (child != NULL && (!(st & HPS_CONNECTION) || (ch & HPS_CONNECTION) || !(st & HPS_ENABLE))) {
        UsbDevDetach(child);
        h->Child[Port] = NULL;
        child = NULL;
    }
    if (!(st & HPS_CONNECTION)) {
        h->Retry[Port] = 0;
        return;
    }
    if (child != NULL || h->Retry[Port] >= 3) {
        return;
    }
    if (!(st & (h->Ss ? SSPS_POWER : HPS_POWER))) {
        HubPortFeature(h, USB_REQ_SET_FEATURE, HUB_PORT_POWER, Port);
        OsSleepMs(h->PowerGoodMs);
    }
    OsSleepMs(100);
    if (HubPortStatus(h, Port, &st, &ch) != USB_OK || !(st & HPS_CONNECTION)) {
        return;
    }
    if (h->Ss && (st & HPS_ENABLE)) {
        speed = USB_SPEED_SUPER;
    } else if (HubResetPort(h, Port, &speed) != USB_OK) {
        h->Retry[Port]++;
        return;
    }
    child = UsbDevAttach(h->Dev->Hc, h->Dev, Port, speed);
    if (child == NULL) {
        h->Retry[Port]++;
        if (!h->Ss) {
            HubPortFeature(h, USB_REQ_CLEAR_FEATURE, HUB_PORT_ENABLE, Port);
        }
        return;
    }
    h->Child[Port] = child;
    h->Retry[Port] = 0;
}

static void HubService(HUB *h)
{
    ULONG now = OsTimeMs();
    UCHAR bits[4];
    int port;

    if (h->Dev->Gone) {
        return;
    }
    if (h->IntrError) {
        h->IntrError = 0;
        if (h->Intr.Halted) {
            UsbClearHalt(&h->Intr);
        }
        h->PollAll = 1;
    }
    if (!h->Changed && !h->PollAll && now - h->LastPoll < 2000) {
        HubArm(h);
        return;
    }
    UsbMemSet(bits, 0xFF, sizeof(bits));
    if (h->Changed && !h->PollAll) {
        UsbMemCpy(bits, h->Dev->Page + USB_DEV_SCRATCH_OFF, sizeof(bits));
    }
    h->Changed = 0;
    h->PollAll = 0;
    h->LastPoll = now;

    for (port = 1; port <= h->Ports && !h->Dev->Gone; port++) {
        if (bits[port >> 3] & (1 << (port & 7))) {
            HubServicePort(h, port);
        }
    }
    HubArm(h);
}

void HubServiceAll(void)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (Hubs[i] != NULL) {
            HubService(Hubs[i]);
        }
    }
}
