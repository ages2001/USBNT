/*
 * usbvideo.c - USB Video Class (UVC 1.0 / 1.1 / 1.5) cameras.
 *
 * The VideoControl interface is parsed for the processing unit (brightness,
 * contrast, ... controls), the VideoStreaming interface for its formats
 * (uncompressed YUY2/UYVY/NV12/I420/Y800 and MJPEG) and frame sizes.  A stream
 * is negotiated with the probe/commit controls, runs on an isochronous
 * alternate setting chosen by bandwidth (or on the bulk endpoint of bulk-only
 * cameras), and the payload headers are put back together into frames.
 * Completed frames are kept triple buffered: the capture side always has a
 * buffer to fill and a reader gets the newest whole frame.
 *
 * Applications reach the cameras through \\.\UsbNt (IOCTL_USBNT_VIDEO_*,
 * usbntioc.h); usbcam.exe shows and records them.
 */

#include "usbnt.h"
#include "usbntioc.h"
#include "usbvideo.h"

#define CC_VIDEO                0x0E
#define SC_VIDEOCONTROL         0x01
#define SC_VIDEOSTREAMING       0x02
#define CS_INTERFACE            0x24

#define VC_HEADER               0x01
#define VC_PROCESSING_UNIT      0x05
#define VS_INPUT_HEADER         0x01
#define VS_FORMAT_UNCOMPRESSED  0x04
#define VS_FRAME_UNCOMPRESSED   0x05
#define VS_FORMAT_MJPEG         0x06
#define VS_FRAME_MJPEG          0x07
#define VS_FORMAT_FRAME_BASED   0x10
#define VS_FRAME_FRAME_BASED    0x11

#define SET_CUR                 0x01
#define GET_CUR                 0x81
#define GET_MIN                 0x82
#define GET_MAX                 0x83
#define GET_RES                 0x84
#define GET_DEF                 0x87
#define VS_PROBE_CONTROL        0x01
#define VS_COMMIT_CONTROL       0x02

#define MAX_CAMS                USBNT_MAX_CAMS
#define MAX_ALTS                16
#define NXFER                   6
#define NPKT_HS                 32
#define NPKT_FS                 16
#define BULK_XFER               0x8000
#define MAX_FRAME_BYTES         (16UL * 1024 * 1024)

typedef struct _UVC_ALT {
    UCHAR       Alt;
    const UCHAR *Ep;
    ULONG       Bytes;              /* per service interval */
} UVC_ALT;

typedef struct _UVC_XFER {
    struct _UVC_DEV *U;
    USB_XFER    X;
    USB_ISO_PACKET Pkt[NPKT_HS];
    ULONG       NPkt;
    UCHAR      *Buf;
    ULONG       Phys;
    ULONG       Size;
    UCHAR       Busy;
} UVC_XFER;

typedef struct _UVC_DEV {
    USB_DEV    *Dev;
    ULONG       Id;
    UCHAR       VcIf, VsIf, PuId;
    USHORT      Bcd;
    ULONG       PuControls;
    USBNT_CAMERA Pub;
    UCHAR       FmtIdx[USBNT_VID_FORMATS];              /* bFormatIndex */
    UCHAR       FrIdx[USBNT_VID_FORMATS][USBNT_VID_FRAMES];
    UCHAR       Mjpeg[USBNT_VID_FORMATS];
    UVC_ALT     Alts[MAX_ALTS];
    int         NAlts;
    const UCHAR *BulkEp;
    /* stream */
    BOOLEAN     Running, Stopping, Gone;
    USB_PIPE    Pipe;
    UVC_XFER    X[NXFER];
    ULONG       Pending;
    UCHAR      *Frame[3];
    ULONG       FrameAlloc;
    ULONG       Expected;           /* bytes of an uncompressed frame, 0 = any */
    int         Fill, Ready, Read;
    ULONG       FillLen;
    UCHAR       Fid, FidValid, FillErr, FillOver;
    ULONG       ReadyLen, ReadySeq, ReadyFlags, ReadyTime, Seq;
    ULONG       ReadSeq;
    ULONG       FourCC;
    USHORT      Width, Height;
    ULONG       Interval;
    OS_EVENT    FrameEv;
    ULONG       Frames, Errors, Payloads, SubmitErrors;
    UCHAR       Retry;
    UCHAR       DeferEof, PendingEof;
    LONG        Readers;
    ULONG       BulkPayload, PayloadLeft;
} UVC_DEV;

static UVC_DEV *Cams[MAX_CAMS];
static ULONG NextId = 1;

/* ------------------------------------------------------------------ */
/* descriptor parsing                                                  */
/* ------------------------------------------------------------------ */
static ULONG IsoBytes(USB_DEV *Dev, const UCHAR *Ep, const UCHAR *End)
{
    ULONG mp = GET16(Ep + 4);
    const UCHAR *c = UsbNextDesc(Ep, End);
    if (Dev->Speed == USB_SPEED_SUPER && c != NULL && c[1] == USB_DT_SS_EP_COMP && c[0] >= 6) {
        ULONG bpi = GET16(c + 4);
        if (bpi) return bpi;
        return (mp & 0x7FF) * (c[2] + 1UL) * ((c[3] & 3) + 1UL);
    }
    if (Dev->Speed == USB_SPEED_HIGH) return (mp & 0x7FF) * (((mp >> 11) & 3) + 1);
    return mp & 0x7FF;
}

static void AddFrame(UVC_DEV *u, int f, const UCHAR *d)
{
    USBNT_VID_FORMAT *F = &u->Pub.Format[f];
    USBNT_VID_FRAME *R;
    ULONG n, k, type, defo;
    if (F->NumFrames >= USBNT_VID_FRAMES || d[0] < 26) return;
    R = &F->Frame[F->NumFrames];
    u->FrIdx[f][F->NumFrames] = d[3];
    R->Width = GET16(d + 5);
    R->Height = GET16(d + 7);
    R->MaxFrameSize = (d[2] == VS_FRAME_FRAME_BASED) ? (ULONG)R->Width * R->Height * 2 : GET32(d + 17);
    type = d[2] == VS_FRAME_FRAME_BASED ? d[21] : d[25];
    defo = d[2] == VS_FRAME_FRAME_BASED ? 17 : 21;
    n = 0;
    if (type == 0) {
        ULONG base = 26;
        if (d[0] >= base + 12) {
            ULONG mn = GET32(d + base), mx = GET32(d + base + 4), def = GET32(d + defo);
            R->Interval[n++] = mn;
            if (def > mn && def < mx) R->Interval[n++] = def;
            if (mx > mn) R->Interval[n++] = mx;
        }
    } else {
        ULONG base = 26;
        for (k = 0; k < type && n < USBNT_VID_INTERVALS && base + 4 * k + 4 <= d[0]; k++) {
            ULONG v = GET32(d + base + 4 * k), j;
            /* keep them sorted, shortest (fastest) first */
            for (j = n; j > 0 && R->Interval[j - 1] > v; j--) R->Interval[j] = R->Interval[j - 1];
            R->Interval[j] = v;
            n++;
        }
    }
    if (n == 0) R->Interval[n++] = GET32(d + defo) ? GET32(d + defo) : 333333;
    R->NumIntervals = n;
    F->NumFrames++;
}

static void ParseStreaming(UVC_DEV *u, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *d = If;
    int f = -1;
    UCHAR alt = If[3];
    while ((d = UsbNextDesc(d, End)) != NULL) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9) {
            if (d[2] != u->VsIf) break;
            alt = d[3];
            continue;
        }
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7 && (d[2] & 0x80)) {
            if ((d[3] & 3) == USB_EP_ISOCH && GET16(d + 4) != 0 && u->NAlts < MAX_ALTS) {
                u->Alts[u->NAlts].Alt = alt;
                u->Alts[u->NAlts].Ep = d;
                u->Alts[u->NAlts].Bytes = IsoBytes(u->Dev, d, End);
                u->NAlts++;
            } else if ((d[3] & 3) == USB_EP_BULK && alt == 0 && u->BulkEp == NULL) {
                u->BulkEp = d;
            }
            continue;
        }
        if (d[1] != CS_INTERFACE || d[0] < 3 || alt != 0) continue;
        switch (d[2]) {
        case VS_FORMAT_UNCOMPRESSED:
        case VS_FORMAT_FRAME_BASED:
            if (u->Pub.NumFormats >= USBNT_VID_FORMATS || d[0] < 21) { f = -1; break; }
            f = u->Pub.NumFormats++;
            u->FmtIdx[f] = d[3];
            u->Pub.Format[f].FourCC = GET32(d + 5);
            if (u->Pub.Format[f].FourCC == USBNT_FOURCC('M', 'J', 'P', 'G') || u->Pub.Format[f].FourCC == USBNT_FOURCC('m', 'j', 'p', 'g')) {
                u->Pub.Format[f].FourCC = USBNT_FCC_MJPG;
                u->Mjpeg[f] = 1;
            }
            u->Pub.Format[f].BitsPerPixel = (UCHAR)(d[2] == VS_FORMAT_UNCOMPRESSED ? d[21] : 0);
            break;
        case VS_FORMAT_MJPEG:
            if (u->Pub.NumFormats >= USBNT_VID_FORMATS) { f = -1; break; }
            f = u->Pub.NumFormats++;
            u->FmtIdx[f] = d[3];
            u->Pub.Format[f].FourCC = USBNT_FCC_MJPG;
            u->Mjpeg[f] = 1;
            break;
        case VS_FRAME_UNCOMPRESSED:
        case VS_FRAME_MJPEG:
        case VS_FRAME_FRAME_BASED:
            if (f >= 0) AddFrame(u, f, d);
            break;
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* attach / detach                                                     */
/* ------------------------------------------------------------------ */
int UvcAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *d = If, *vs = NULL;
    UVC_DEV *u;
    int slot = -1, i;

    if (If[5] != CC_VIDEO || If[6] != SC_VIDEOCONTROL) return USB_ERR_PARAM;
    if (OsGetConfig("DisableVideo", 0)) return USB_ERR_PARAM;
    for (i = 0; i < MAX_CAMS; i++) if (Cams[i] == NULL) { slot = i; break; }
    if (slot < 0) return USB_ERR_BUSY;
    u = (UVC_DEV *)OsAlloc(sizeof(UVC_DEV));
    if (u == NULL) return USB_ERR_NOMEM;
    u->Dev = Dev;
    u->VcIf = If[2];
    u->VsIf = 0xFF;
    OsEventInit(&u->FrameEv);
    while ((d = UsbNextDesc(d, End)) != NULL) {
        if (d[1] == USB_DT_INTERFACE) break;
        if (d[1] != CS_INTERFACE || d[0] < 3) continue;
        if (d[2] == VC_HEADER && d[0] >= 12) {
            u->Bcd = GET16(d + 3);
            if (d[11] >= 1 && d[0] >= 13) u->VsIf = d[12];
        } else if (d[2] == VC_PROCESSING_UNIT && d[0] >= 8 && !u->PuId) {
            ULONG k, n = d[7];
            u->PuId = d[3];
            for (k = 0; k < n && k < 3; k++) u->PuControls |= (ULONG)d[8 + k] << (8 * k);
        }
    }
    /* the streaming interface (alternate setting 0) */
    d = Dev->Config;
    while ((d = UsbNextDesc(d, End)) != NULL) {
        if (d[1] == USB_DT_INTERFACE && d[0] >= 9 && d[5] == CC_VIDEO && d[6] == SC_VIDEOSTREAMING && d[3] == 0 &&
            (u->VsIf == 0xFF || d[2] == u->VsIf)) {
            vs = d;
            u->VsIf = d[2];
            break;
        }
    }
    if (vs == NULL) {
        OsFree(u);
        return USB_ERR_PARAM;
    }
    ParseStreaming(u, vs, End);
    if (u->Pub.NumFormats == 0 || (u->NAlts == 0 && u->BulkEp == NULL) || (u->NAlts && !Dev->Hc->IsoOk && !u->BulkEp)) {
        UsbLog(LOG_INFO, "device %u: video camera without usable formats or endpoints\n", Dev->Index);
        OsFree(u);
        return USB_ERR_PARAM;
    }
    u->Id = (NextId++ << 8) | (Dev->Index & 0xFF);
    u->Pub.Id = u->Id;
    u->Pub.DevIndex = Dev->Index;
    u->Pub.NumFormats = u->Pub.NumFormats;
    u->Pub.UvcMinor = (UCHAR)(u->Bcd & 0xFF);
    /* processing unit bitmap (bit 0 brightness, 1 contrast, 2 hue, 3 saturation, 4 sharpness, 5 gamma,
       6 WB temperature, 9 gain, 10 power line) to selector numbers */
    {
        static const UCHAR sel[16] = { 2, 3, 6, 7, 8, 9, 0x0A, 0, 0, 4, 5, 0, 0, 0, 0, 0 };
        ULONG k;
        for (k = 0; k < 16; k++) if ((u->PuControls & (1UL << k)) && sel[k]) u->Pub.Controls |= 1UL << sel[k];
    }
    if (Dev->Product[0]) UsbFormat(u->Pub.Name, sizeof(u->Pub.Name), "%s", Dev->Product);
    else UsbFormat(u->Pub.Name, sizeof(u->Pub.Name), "USB Camera %04x:%04x", GET16(Dev->DevDesc + 8), GET16(Dev->DevDesc + 10));
    Cams[slot] = u;
    Dev->Function |= USB_FUNC_VIDEO;
    UsbLog(LOG_INFO, "device %u: video camera, UVC %x.%02x, %u format(s), %d iso setting(s)%s\n", Dev->Index,
           u->Bcd >> 8, u->Bcd & 0xFF, u->Pub.NumFormats, u->NAlts, u->BulkEp ? ", bulk" : "");
    for (i = 0; i < u->Pub.NumFormats; i++) {
        USBNT_VID_FORMAT *F = &u->Pub.Format[i];
        UsbLog(LOG_INFO, "  format %u: %c%c%c%c, %u frame size(s), first %ux%u\n", u->FmtIdx[i], (char)F->FourCC,
               (char)(F->FourCC >> 8), (char)(F->FourCC >> 16), (char)(F->FourCC >> 24), F->NumFrames,
               F->NumFrames ? F->Frame[0].Width : 0, F->NumFrames ? F->Frame[0].Height : 0);
    }
    UsbNotifyChange();
    return USB_OK;
}

static void StopNow(UVC_DEV *u);

void UvcDetach(USB_DEV *Dev)
{
    int i;
    for (i = 0; i < MAX_CAMS; i++) {
        UVC_DEV *u = Cams[i];
        if (u == NULL || u->Dev != Dev) continue;
        u->Gone = TRUE;
        OsEventSet(&u->FrameEv);
        StopNow(u);
        {
            OS_IRQL irql = OsLock();
            Cams[i] = NULL;
            OsUnlock(irql);
        }
        {
            ULONG t0 = OsTimeMs();          /* readers of a frame leave first */
            while (u->Readers && OsTimeMs() - t0 < 3000) {
                OsEventSet(&u->FrameEv);
                OsSleepMs(5);
            }
        }
        UsbLog(LOG_INFO, "device %u: video camera removed\n", Dev->Index);
        OsFree(u);
    }
}

static UVC_DEV *Find(ULONG Id)
{
    int i;
    for (i = 0; i < MAX_CAMS; i++) if (Cams[i] != NULL && Cams[i]->Id == Id) return Cams[i];
    return NULL;
}

void UvcList(USBNT_CAMERAS *Out)
{
    int i;
    UsbMemSet(Out, 0, sizeof(*Out));
    Out->Version = USBNT_VIDEO_VERSION;
    for (i = 0; i < MAX_CAMS; i++) {
        if (Cams[i] == NULL) continue;
        Out->Cam[Out->Count] = Cams[i]->Pub;
        Out->Cam[Out->Count].Streaming = Cams[i]->Running;
        Out->Count++;
    }
}

/* ------------------------------------------------------------------ */
/* streaming                                                           */
/* ------------------------------------------------------------------ */
static void FrameDone(UVC_DEV *u)
{
    OS_IRQL irql;
    int t;
    if (u->FillLen == 0) return;
    if (u->Expected && u->FillLen != u->Expected) u->FillErr = 1;
    if (u->FillOver) u->FillErr = 1;
    irql = OsLock();
    t = u->Ready;
    u->Ready = u->Fill;
    u->Fill = t;
    u->ReadyLen = u->FillLen;
    u->ReadySeq = ++u->Seq;
    u->ReadyFlags = u->FillErr ? 1 : 0;
    u->ReadyTime = OsTimeMs();
    OsUnlock(irql);
    u->Frames++;
    if (u->FillErr) u->Errors++;
    u->FillLen = 0;
    u->FillErr = 0;
    u->FillOver = 0;
    OsEventSet(&u->FrameEv);
}

static void Payload(UVC_DEV *u, const UCHAR *p, ULONG len);

static void AddData(UVC_DEV *u, const UCHAR *p, ULONG n)
{
    if (u->FillLen + n <= u->FrameAlloc) {
        UsbMemCpy(u->Frame[u->Fill] + u->FillLen, p, n);
        u->FillLen += n;
    } else {
        u->FillOver = 1;
    }
}

/* a bulk payload larger than one transfer: the following transfers carry
   more of its data without a header, a short transfer ends it */
static void BulkData(UVC_DEV *u, const UCHAR *p, ULONG len, ULONG size)
{
    if (u->PayloadLeft) {
        AddData(u, p, len > u->PayloadLeft ? u->PayloadLeft : len);
        u->PayloadLeft = (len < size || len >= u->PayloadLeft) ? 0 : u->PayloadLeft - len;
        if (!u->PayloadLeft && u->PendingEof) {
            u->PendingEof = 0;
            FrameDone(u);
        }
        return;
    }
    if (len == size && u->BulkPayload > len && len >= 2 && p[0] >= 2 && p[0] <= len) {
        u->PayloadLeft = u->BulkPayload - len;
        u->DeferEof = 1;
    }
    Payload(u, p, len);
    u->DeferEof = 0;
}

static void Payload(UVC_DEV *u, const UCHAR *p, ULONG len)
{
    ULONG hl, n;
    UCHAR fl;
    if (len < 2) return;
    hl = p[0];
    fl = p[1];
    if (hl < 2 || hl > len) return;
    u->Payloads++;
    if (u->FidValid && (fl & 1) != u->Fid) FrameDone(u);       /* a new frame began */
    u->Fid = (UCHAR)(fl & 1);
    u->FidValid = 1;
    if (fl & 0x40) u->FillErr = 1;
    n = len - hl;
    if (n) AddData(u, p + hl, n);
    if (fl & 0x02) {                                            /* end of frame */
        if (u->DeferEof) u->PendingEof = 1;                     /* after the rest of the payload */
        else FrameDone(u);
    }
}

static int Submit(UVC_XFER *x);

static void XferDone(USB_XFER *X)
{
    UVC_XFER *x = (UVC_XFER *)X->Context;
    UVC_DEV *u = x->U;
    OS_IRQL irql;
    BOOLEAN again;
    ULONG i;

    if (X->Status != USB_ERR_ABORTED && X->Status != USB_ERR_NODEV && !u->Stopping) {
        if (X->Iso != NULL) {
            for (i = 0; i < x->NPkt; i++) {
                if (x->Pkt[i].Status == USB_OK && x->Pkt[i].Actual) Payload(u, x->Buf + x->Pkt[i].Offset, x->Pkt[i].Actual);
                else if (x->Pkt[i].Status != USB_OK) u->FillErr = 1;
            }
        } else if (X->Status == USB_OK) {
            BulkData(u, x->Buf, X->Actual, x->Size);
        }
    }
    irql = OsLock();
    x->Busy = 0;
    if (u->Pending) u->Pending--;
    again = (BOOLEAN)(u->Running && !u->Stopping && !u->Gone && !u->Dev->Gone);
    OsUnlock(irql);
    if (again && Submit(x) != USB_OK) {
        u->Retry = 1;                                   /* the worker tries again */
        OsWakeWorker();
    }
}

static int Submit(UVC_XFER *x)
{
    UVC_DEV *u = x->U;
    OS_IRQL irql;
    ULONG i;
    int r;
    irql = OsLock();
    if (x->Busy) {                                  /* already queued (worker and completion race) */
        OsUnlock(irql);
        return USB_OK;
    }
    x->Busy = 1;
    u->Pending++;
    OsUnlock(irql);
    UsbMemSet(&x->X, 0, sizeof(x->X));
    x->X.Pipe = &u->Pipe;
    x->X.Buf = x->Buf;
    x->X.Phys = x->Phys;
    x->X.DirIn = 1;
    x->X.Complete = XferDone;
    x->X.Context = x;
    if (x->NPkt) {
        ULONG per = x->Size / x->NPkt;
        for (i = 0; i < x->NPkt; i++) {
            x->Pkt[i].Offset = i * per;
            x->Pkt[i].Length = (USHORT)per;
            x->Pkt[i].Actual = 0;
            x->Pkt[i].Status = 0;
        }
        x->X.Iso = x->Pkt;
        x->X.IsoCount = x->NPkt;
    }
    x->X.Length = x->Size;
    r = UsbSubmit(&x->X);
    if (r != USB_OK) {
        irql = OsLock();
        x->Busy = 0;
        u->Pending--;
        OsUnlock(irql);
        if (u->SubmitErrors++ < 5) {
            UsbLog(LOG_ERR, "video %u: submit failed (%d)\n", u->Dev->Index, r);
        }
    }
    return r;
}

static void FreeBuffers(UVC_DEV *u)
{
    int i;
    for (i = 0; i < NXFER; i++) {
        if (u->X[i].Buf) OsDmaFree(u->X[i].Buf, u->X[i].Size);
        u->X[i].Buf = NULL;
    }
    for (i = 0; i < 3; i++) {
        if (u->Frame[i]) OsFree(u->Frame[i]);
        u->Frame[i] = NULL;
    }
}

static void StopNow(UVC_DEV *u)
{
    ULONG t0 = OsTimeMs();
    if (!u->Running && u->Pipe.Opened == 0) {
        FreeBuffers(u);
        return;
    }
    u->Stopping = TRUE;
    UsbCancelPipe(&u->Pipe);
    while (u->Pending && OsTimeMs() - t0 < 1000) OsSleepMs(5);
    UsbClosePipe(&u->Pipe);
    if (!u->Dev->Gone && u->NAlts) UsbSetInterface(u->Dev, u->VsIf, 0);
    u->Running = FALSE;
    u->Stopping = FALSE;
    t0 = OsTimeMs();                    /* a reader may be copying the last frame */
    while (u->Readers && OsTimeMs() - t0 < 2000) {
        OsEventSet(&u->FrameEv);
        OsSleepMs(2);
    }
    FreeBuffers(u);
    u->Pub.Streaming = 0;
}

static int VsCtl(UVC_DEV *u, UCHAR req, UCHAR cs, UCHAR *buf, USHORT len)
{
    ULONG got = 0;
    return UsbControl(u->Dev, (UCHAR)((req & 0x80 ? USB_RT_IN : USB_RT_OUT) | USB_RT_CLASS | USB_RT_INTERFACE), req,
                      (USHORT)(cs << 8), u->VsIf, len, buf, 1000, &got);
}

static int StartNow(UVC_DEV *u, const USBNT_VID_START *In, USBNT_VID_STATUS *Out)
{
    UCHAR probe[48];
    USHORT plen = u->Bcd >= 0x0150 ? 48 : u->Bcd >= 0x0110 ? 34 : 26;
    USBNT_VID_FORMAT *F;
    USBNT_VID_FRAME *R;
    ULONG payload, maxFrame, interval, i, best = 0xFFFFFFFFUL, fi = In->Format, pixels;
    int r, a = -1;

    UsbMemSet(Out, 0, sizeof(*Out));
    if (In->Format >= u->Pub.NumFormats) return USB_ERR_PARAM;
    F = &u->Pub.Format[In->Format];
    if (In->Frame >= F->NumFrames) return USB_ERR_PARAM;
    R = &F->Frame[In->Frame];
    if (u->Running) StopNow(u);
    interval = In->Interval ? In->Interval : R->Interval[0];
    /* probe: what we want, then what the camera makes of it */
    UsbMemSet(probe, 0, sizeof(probe));
    VsCtl(u, GET_CUR, VS_PROBE_CONTROL, probe, plen);
    probe[0] = 1; probe[1] = 0;                                 /* bmHint: frame interval fixed */
    probe[2] = u->FmtIdx[In->Format];
    probe[3] = u->FrIdx[In->Format][In->Frame];
    PUT32(probe + 4, interval);
    r = VsCtl(u, SET_CUR, VS_PROBE_CONTROL, probe, plen);
    if (r != USB_OK) {
        UsbLog(LOG_ERR, "video %u: probe rejected (%d)\n", u->Dev->Index, r);
        return r;
    }
    VsCtl(u, GET_CUR, VS_PROBE_CONTROL, probe, plen);
    maxFrame = GET32(probe + 18);
    payload = GET32(probe + 22);
    if (probe[2] != u->FmtIdx[In->Format] || probe[3] != u->FrIdx[In->Format][In->Frame]) {
        /* the camera changed the request: take it as it is, with the size and
           format of what it chose (the pictures come in that size) */
        ULONG f, k;
        UsbLog(LOG_INFO, "video %u: camera chose format %u frame %u\n", u->Dev->Index, probe[2], probe[3]);
        for (f = 0; f < u->Pub.NumFormats; f++) {
            if (u->FmtIdx[f] != probe[2]) continue;
            for (k = 0; k < u->Pub.Format[f].NumFrames; k++) {
                if (u->FrIdx[f][k] == probe[3]) {
                    F = &u->Pub.Format[f];
                    R = &F->Frame[k];
                    fi = f;
                }
            }
        }
    }
    if (GET32(probe + 4)) interval = GET32(probe + 4);
    r = VsCtl(u, SET_CUR, VS_COMMIT_CONTROL, probe, plen);
    if (r != USB_OK) {
        UsbLog(LOG_ERR, "video %u: commit rejected (%d)\n", u->Dev->Index, r);
        return r;
    }
    /* frame buffers: what the camera says (some say too little), at least an
       uncompressed frame; JPEG frames have at most about a byte per pixel */
    pixels = (ULONG)R->Width * R->Height;
    if (maxFrame > MAX_FRAME_BYTES) maxFrame = 0;
    if (R->MaxFrameSize > maxFrame && R->MaxFrameSize <= MAX_FRAME_BYTES) maxFrame = R->MaxFrameSize;
    if (u->Mjpeg[fi]) {
        if (maxFrame > pixels + 65536) maxFrame = pixels + 65536;
        if (maxFrame < pixels / 4 + 65536) maxFrame = pixels / 4 + 65536;
    } else if (F->BitsPerPixel) {
        if (maxFrame < pixels * F->BitsPerPixel / 8) maxFrame = pixels * F->BitsPerPixel / 8;
    } else if (maxFrame == 0) {
        maxFrame = pixels * 2;
    }
    if (maxFrame > MAX_FRAME_BYTES) {
        UsbLog(LOG_ERR, "video %u: %ux%u needs %u bytes per frame, more than %u\n", u->Dev->Index, R->Width, R->Height,
               maxFrame, MAX_FRAME_BYTES);
        return USB_ERR_NOMEM;
    }
    u->FourCC = F->FourCC;
    u->Width = R->Width;
    u->Height = R->Height;
    u->Interval = interval;
    u->Expected = (F->BitsPerPixel && !u->Mjpeg[fi]) ? pixels * F->BitsPerPixel / 8 : 0;
    u->FrameAlloc = maxFrame + 4096;
    for (i = 0; i < 3; i++) {
        u->Frame[i] = (UCHAR *)OsAlloc(u->FrameAlloc);
        if (u->Frame[i] == NULL) {
            UsbLog(LOG_ERR, "video %u: no memory for 3 frames of %u bytes\n", u->Dev->Index, u->FrameAlloc);
            FreeBuffers(u);
            return USB_ERR_NOMEM;
        }
    }
    u->Fill = 0; u->Ready = 1; u->Read = 2;
    u->FillLen = 0; u->FidValid = 0; u->FillErr = 0; u->FillOver = 0;
    u->BulkPayload = payload; u->PayloadLeft = 0; u->PendingEof = 0; u->DeferEof = 0;
    u->ReadyLen = 0;
    /* endpoint: smallest isochronous setting that carries the payload, else bulk */
    if (u->NAlts && u->Dev->Hc->IsoOk) {
        ULONG largest = 0;
        for (i = 0; i < (ULONG)u->NAlts; i++) {
            if (u->Alts[i].Bytes >= payload && u->Alts[i].Bytes < best) { best = u->Alts[i].Bytes; a = (int)i; }
            if (u->Alts[i].Bytes > largest) largest = u->Alts[i].Bytes;
        }
        if (a < 0) {
            for (i = 0; i < (ULONG)u->NAlts; i++) if (u->Alts[i].Bytes == largest) a = (int)i;
            UsbLog(LOG_INFO, "video %u: payload %u larger than any setting, using %u\n", u->Dev->Index, payload, largest);
        }
    }
    if (a >= 0) {
        ULONG npkt = (u->Dev->Speed == USB_SPEED_FULL || u->Dev->Speed == USB_SPEED_LOW) ? NPKT_FS : NPKT_HS;
        r = UsbSetInterface(u->Dev, u->VsIf, u->Alts[a].Alt);
        if (r == USB_OK) r = UsbOpenPipe(u->Dev, &u->Pipe, u->Alts[a].Ep);
        if (r != USB_OK) {
            UsbSetInterface(u->Dev, u->VsIf, 0);
            FreeBuffers(u);
            return r;
        }
        for (i = 0; i < NXFER; i++) {
            u->X[i].U = u;
            u->X[i].NPkt = npkt;
            u->X[i].Size = npkt * u->Alts[a].Bytes;
            u->X[i].Buf = (UCHAR *)OsDmaAlloc(u->X[i].Size, &u->X[i].Phys);
            if (u->X[i].Buf == NULL) break;
        }
        UsbLog(LOG_INFO, "video %u: %ux%u %c%c%c%c every %u.%04u ms, setting %u (%u bytes), payload %u, frame %u\n",
               u->Dev->Index, u->Width, u->Height, (char)u->FourCC, (char)(u->FourCC >> 8), (char)(u->FourCC >> 16),
               (char)(u->FourCC >> 24), interval / 10000, interval % 10000, u->Alts[a].Alt, u->Alts[a].Bytes, payload,
               maxFrame);
    } else if (u->BulkEp != NULL) {
        r = UsbOpenPipe(u->Dev, &u->Pipe, u->BulkEp);
        if (r != USB_OK) {
            FreeBuffers(u);
            return r;
        }
        for (i = 0; i < (u->Dev->Hc->QueueOk ? NXFER : 1); i++) {
            u->X[i].U = u;
            u->X[i].NPkt = 0;
            u->X[i].Size = payload && payload <= 0x40000 ? payload : BULK_XFER;
            if (u->Dev->Hc->MaxXfer && u->X[i].Size > u->Dev->Hc->MaxXfer) {
                /* a payload larger than the controller's transfers: it then spans
                   several transfers, each ending on a whole number of packets */
                u->X[i].Size = u->Dev->Hc->MaxXfer;
            }
            u->X[i].Buf = (UCHAR *)OsDmaAlloc(u->X[i].Size, &u->X[i].Phys);
            if (u->X[i].Buf == NULL) break;
        }
        UsbLog(LOG_INFO, "video %u: %ux%u bulk stream, payload %u, frame %u\n", u->Dev->Index, u->Width, u->Height,
               payload, maxFrame);
    } else {
        FreeBuffers(u);
        return USB_ERR_PARAM;
    }
    if (u->X[0].Buf == NULL) {
        StopNow(u);
        return USB_ERR_NOMEM;
    }
    u->Running = TRUE;
    u->Stopping = FALSE;
    u->Pub.Streaming = 1;
    for (i = 0; i < NXFER && u->X[i].Buf != NULL; i++) Submit(&u->X[i]);
    Out->Result = 0;
    Out->FourCC = u->FourCC;
    Out->Width = u->Width;
    Out->Height = u->Height;
    Out->MaxFrameSize = maxFrame;
    Out->Interval = interval;
    return USB_OK;
}

static int ControlNow(UVC_DEV *u, USBNT_VID_CONTROL *c)
{
    UCHAR b[4];
    USHORT len = (USHORT)(c->Selector == USBNT_VCTL_POWERLINE ? 1 : 2);
    USHORT val = (USHORT)(c->Selector << 8), idx = (USHORT)((u->PuId << 8) | u->VcIf);
    ULONG got = 0;
    int r;
    if (!u->PuId || !(u->Pub.Controls & (1UL << c->Selector))) return USB_ERR_PARAM;
    if (c->Set) {
        b[0] = (UCHAR)c->Value; b[1] = (UCHAR)(c->Value >> 8);
        r = UsbControl(u->Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE, SET_CUR, val, idx, len, b, 500, &got);
        if (r != USB_OK) return r;
    }
#define RD(req, dst) do { b[0] = b[1] = 0; if (UsbControl(u->Dev, USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE, req, val, idx, \
        len, b, 500, &got) == USB_OK) dst = len == 1 ? b[0] : (LONG)(SHORT)(b[0] | (b[1] << 8)); } while (0)
    RD(GET_CUR, c->Value);
    RD(GET_MIN, c->Min);
    RD(GET_MAX, c->Max);
    RD(GET_DEF, c->Def);
    RD(GET_RES, c->Res);
#undef RD
    if (c->Selector == USBNT_VCTL_WB_TEMP || c->Selector == USBNT_VCTL_GAIN) {
        /* unsigned 16-bit controls */
        c->Value &= 0xFFFF; c->Min &= 0xFFFF; c->Max &= 0xFFFF; c->Def &= 0xFFFF;
    }
    return USB_OK;
}

/* ------------------------------------------------------------------ */
/* requests from other threads go through the worker (control transfers */
/* of a device are issued from the worker only)                         */
/* ------------------------------------------------------------------ */
#define UVC_CALLS 4
typedef struct _UVC_REQ {
    int         Op;                 /* 1 start, 2 stop, 3 control */
    ULONG       Id;
    const USBNT_VID_START *In;
    USBNT_VID_STATUS *Out;
    USBNT_VID_CONTROL *Ctl;
    int         Result;
    OS_EVENT    Done;
} UVC_REQ;

static UVC_REQ *Calls[UVC_CALLS];

static int Execute(UVC_REQ *c)
{
    UVC_DEV *u = Find(c->Id);
    if (u == NULL || u->Gone) return USB_ERR_NODEV;
    switch (c->Op) {
    case 1: return StartNow(u, c->In, c->Out);
    case 2: StopNow(u); return USB_OK;
    default: return ControlNow(u, c->Ctl);
    }
}

static int Call(UVC_REQ *c)
{
    OS_IRQL irql;
    ULONG i;
    BOOLEAN queued = FALSE;
    if (OsInWorker()) return Execute(c);
    OsEventInit(&c->Done);
    c->Result = USB_ERR_TIMEOUT;
    while (!queued) {
        irql = OsLock();
        for (i = 0; i < UVC_CALLS && !queued; i++) if (Calls[i] == NULL) { Calls[i] = c; queued = TRUE; }
        OsUnlock(irql);
        if (!queued) OsSleepMs(10);
    }
    OsWakeWorker();
    if (!OsEventWait(&c->Done, 15000)) {
        irql = OsLock();
        for (i = 0; i < UVC_CALLS; i++) if (Calls[i] == c) { Calls[i] = NULL; queued = FALSE; }
        OsUnlock(irql);
        if (queued) OsEventWait(&c->Done, 0xFFFFFFFF);
    }
    return c->Result;
}

void UvcService(void)
{
    ULONG i;
    for (i = 0; i < MAX_CAMS; i++) {
        UVC_DEV *u = Cams[i];
        ULONG k;
        if (u == NULL || !u->Retry || !u->Running || u->Stopping) continue;
        u->Retry = 0;
        for (k = 0; k < NXFER; k++) {
            if (u->X[k].Buf != NULL && !u->X[k].Busy && Submit(&u->X[k]) != USB_OK) u->Retry = 1;
        }
    }
    for (i = 0; i < UVC_CALLS; i++) {
        UVC_REQ *c;
        OS_IRQL irql = OsLock();
        c = Calls[i];
        OsUnlock(irql);
        if (c == NULL) continue;
        c->Result = Execute(c);
        irql = OsLock();
        Calls[i] = NULL;
        OsUnlock(irql);
        OsEventSet(&c->Done);
    }
}

int UvcStart(const USBNT_VID_START *In, USBNT_VID_STATUS *Out)
{
    UVC_REQ c;
    UsbMemSet(&c, 0, sizeof(c));
    c.Op = 1; c.Id = In->Id; c.In = In; c.Out = Out;
    return Call(&c);
}

int UvcStop(ULONG Id)
{
    UVC_REQ c;
    UsbMemSet(&c, 0, sizeof(c));
    c.Op = 2; c.Id = Id;
    return Call(&c);
}

int UvcControl(USBNT_VID_CONTROL *Ctl)
{
    UVC_REQ c;
    UsbMemSet(&c, 0, sizeof(c));
    c.Op = 3; c.Id = Ctl->Id; c.Ctl = Ctl;
    return Call(&c);
}

static LONG GetFrame(UVC_DEV *u, const USBNT_VID_GET *Get, UCHAR *Out, ULONG Size)
{
    USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Out;
    ULONG t0 = OsTimeMs(), len;
    OS_IRQL irql;
    int t;
    for (;;) {
        if (u->Gone) return USB_ERR_NODEV;
        if (!u->Running) return USB_ERR_PARAM;
        OsEventReset(&u->FrameEv);
        irql = OsLock();
        if (u->ReadySeq > Get->LastSequence && u->ReadyLen) {
            t = u->Read;
            u->Read = u->Ready;
            u->Ready = t;
            h->Sequence = u->ReadySeq;
            h->Bytes = u->ReadyLen;
            h->Flags = u->ReadyFlags;
            h->TimeMs = u->ReadyTime;
            u->ReadyLen = 0;
            OsUnlock(irql);
            break;
        }
        OsUnlock(irql);
        if (OsTimeMs() - t0 >= Get->TimeoutMs) return 0;
        OsEventWait(&u->FrameEv, Get->TimeoutMs - (OsTimeMs() - t0));
    }
    h->FourCC = u->FourCC;
    h->Width = u->Width;
    h->Height = u->Height;
    len = h->Bytes;
    if (len > Size - sizeof(*h)) {
        len = Size - sizeof(*h);
        h->Flags |= 2;                                          /* truncated */
    }
    if (u->Frame[u->Read] == NULL) return USB_ERR_NODEV;
    UsbMemCpy(Out + sizeof(*h), u->Frame[u->Read], len);
    h->Bytes = len;
    return (LONG)(sizeof(*h) + len);
}

/* newest frame after LastSequence (waits up to TimeoutMs); returns the bytes
   written to Out including the header, 0 on timeout, < 0 on error */
LONG UvcGetFrame(const USBNT_VID_GET *Get, UCHAR *Out, ULONG Size)
{
    UVC_DEV *u;
    OS_IRQL irql;
    LONG r;
    if (Size < sizeof(USBNT_VID_FRAME_HDR)) return USB_ERR_PARAM;
    irql = OsLock();
    u = Find(Get->Id);
    if (u != NULL) u->Readers++;
    OsUnlock(irql);
    if (u == NULL) return USB_ERR_NODEV;
    r = GetFrame(u, Get, Out, Size);
    irql = OsLock();
    u->Readers--;
    OsUnlock(irql);
    return r;
}
