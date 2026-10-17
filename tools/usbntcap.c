/*
 * usbntcap.c - Video for Windows capture driver for USBNT's USB Video Class
 * cameras (usbntcap.dll, registered as "msvideo" in Drivers32).
 *
 * Programs that capture through Video for Windows (avicap32: capCreateCaptureWindow,
 * VidCap, NetMeeting, ...) and, on Windows 2000 / XP, through DirectShow
 * (its "VFW Capture" source wraps every Video for Windows driver) get the
 * pictures of the camera usbnt.sys streams (\\.\UsbNt, IOCTL_USBNT_VIDEO_*).
 * The picture goes out as 24 bit RGB (decoded from YUY2, UYVY, NV12, I420,
 * grey or MJPEG), as YUY2 when the camera sends YUY2, or as MJPEG.
 *
 * Works on Windows NT 3.51 / 4.0 and 2000 / XP (32 and 64 bit).
 */

#include <windows.h>
#include <mmsystem.h>
#include <vfw.h>
#include <winioctl.h>
#include "../src/usbntioc.h"
#include "jpegdec.h"
#include "res/usbntcap.h"

#ifndef DRV_USER
#define DRV_USER 0x4000
#endif
/* msviddrv.h */
#define VIDEOAPIVERSION             4
#define DVM_START                   DRV_USER
#define DVM_GETERRORTEXT            (DVM_START + 0)
#define DVM_GETVIDEOAPIVER          (DVM_START + 1)
#define DVM_DIALOG                  (DVM_START + 100)
#define DVM_CONFIGURESTORAGE        (DVM_START + 101)
#define DVM_GET_CHANNEL_CAPS        (DVM_START + 102)
#define DVM_UPDATE                  (DVM_START + 103)
#define DVM_FRAME                   (DVM_START + 200)
#define DVM_STREAM_ADDBUFFER        (DVM_START + 300)
#define DVM_STREAM_FINI             (DVM_START + 301)
#define DVM_STREAM_GETERROR         (DVM_START + 302)
#define DVM_STREAM_GETPOSITION      (DVM_START + 303)
#define DVM_STREAM_INIT             (DVM_START + 304)
#define DVM_STREAM_PREPAREHEADER    (DVM_START + 305)
#define DVM_STREAM_RESET            (DVM_START + 306)
#define DVM_STREAM_START            (DVM_START + 307)
#define DVM_STREAM_STOP             (DVM_START + 308)
#define DVM_STREAM_UNPREPAREHEADER  (DVM_START + 309)

typedef struct {
    DWORD   dwSize;
    DWORD   fccType;
    DWORD   fccComp;
    DWORD   dwVersion;
    DWORD   dwFlags;
    DWORD   dwError;
    LPVOID  pV1Reserved;
    LPVOID  pV2Reserved;
    DWORD   dnDevNode;
} VID_OPEN_PARMS;

typedef struct {
    DWORD   dwErrorNumber;
    LPWSTR  lpText;
    DWORD   dwLength;
} VID_ERRTEXT;

typedef struct {
    LPDWORD lpdwReturn;
    LPVOID  lpData1;
    DWORD   dwSize1;
    LPVOID  lpData2;
    DWORD   dwSize2;
} VID_CONFIG_PARMS;

typedef struct {
    DWORD     dwMicroSecPerFrame;
    DWORD_PTR dwCallback;
    DWORD_PTR dwCallbackInst;
    DWORD     dwFlags;
    DWORD_PTR hVideo;
} VID_STREAM_INIT;

#define MAX_W 4096
#define MAX_H 3072
#define FCC_RGB 0                       /* BI_RGB */

/* ------------------------------------------------------------------ */
/* State (one camera stream for the driver; the channels share it)      */
/* ------------------------------------------------------------------ */

typedef struct _CHAN {
    DWORD   Kind;                       /* VIDEO_IN, VIDEO_EXTERNALIN, VIDEO_EXTERNALOUT */
} CHAN;

static HINSTANCE Inst;
static HANDLE Heap;
static CRITICAL_SECTION Lock;
static HANDLE Dev = INVALID_HANDLE_VALUE;
static USBNT_CAMERAS Cams;
static int Cam = 0;                     /* index into Cams.Cam */
static int Fmt = -1, Frm = -1;          /* the camera's format and frame */
static ULONG OutFcc = FCC_RGB;          /* what the program gets: BI_RGB, YUY2, MJPG */
static ULONG IntervalWanted;            /* 100 ns, 0 = fastest */
static ULONG CamId;                     /* running stream, 0 = none */
static USBNT_VID_STATUS Stream;
static UCHAR *Raw;
static ULONG RawSize;
static void *JWork;
static ULONG JWorkLen;
static ULONG Seq;
static LONG Opens;

/* streaming */
static VID_STREAM_INIT Init;
static BOOL Streaming, StreamInit;
static HANDLE Thread, StopEv;
static LPVIDEOHDR Queue[64];
static int QHead, QCount;
static DWORD StartTick, Captured, Skipped, LastErr;

static void *Alloc(DWORD n) { return HeapAlloc(Heap, HEAP_ZERO_MEMORY, n ? n : 1); }
static void Free(void *p) { if (p) HeapFree(Heap, 0, p); }
static void Mc(void *d, const void *s, DWORD n) { UCHAR *a = d; const UCHAR *b = s; while (n--) *a++ = *b++; }
static void Mz(void *d, DWORD n) { UCHAR *a = d; while (n--) *a++ = 0; }
void *memcpy(void *d, const void *s, size_t n) { Mc(d, s, (DWORD)n); return d; }
void *memset(void *d, int c, size_t n) { UCHAR *a = d; while (n--) *a++ = (UCHAR)c; return d; }

static ULONG Stride(int w, int bits) { return (((ULONG)w * bits + 31) / 32) * 4; }

/* ------------------------------------------------------------------ */
/* Log (HKLM\SOFTWARE\USBNT\Camera, Log = 1: %windir%\usbntcap.log)     */
/* ------------------------------------------------------------------ */

static BOOL LogOn = -1;

static DWORD RegDword(HKEY Root, const char *Name, DWORD Def)
{
    HKEY k;
    DWORD v = Def, t, n = 4;
    if (RegOpenKeyExA(Root, "SOFTWARE\\USBNT\\Camera", 0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, Name, NULL, &t, (BYTE *)&v, &n) != ERROR_SUCCESS || t != REG_DWORD) v = Def;
        RegCloseKey(k);
    }
    return v;
}

static void RegSetDw(const char *Name, DWORD v)
{
    HKEY k;
    DWORD disp;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "SOFTWARE\\USBNT\\Camera", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp) ==
        ERROR_SUCCESS) {
        RegSetValueExA(k, Name, 0, REG_DWORD, (const BYTE *)&v, 4);
        RegCloseKey(k);
    }
}

static void Log(const char *Fmt_, ...)
{
    char buf[300], path[MAX_PATH];
    va_list ap;
    HANDLE f;
    DWORD w;
    int n;
    GetWindowsDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\usbntcap.log");
    /* the log is written when Log = 1 or the file is there already */
    if (LogOn == -1) LogOn = RegDword(HKEY_LOCAL_MACHINE, "Log", 0) != 0 || GetFileAttributesA(path) != 0xFFFFFFFF;
    if (!LogOn) return;
    va_start(ap, Fmt_);
    n = wvsprintfA(buf, Fmt_, ap);
    va_end(ap);
    buf[n++] = '\r';
    buf[n++] = '\n';
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        SetFilePointer(f, 0, NULL, FILE_END);
        WriteFile(f, buf, n, &w, NULL);
        CloseHandle(f);
    }
}

/* ------------------------------------------------------------------ */
/* Camera                                                               */
/* ------------------------------------------------------------------ */

static BOOL OpenDev(void)
{
    DWORD got = 0;
    if (Dev == INVALID_HANDLE_VALUE) {
        Dev = CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL);
        if (Dev == INVALID_HANDLE_VALUE) return FALSE;
    }
    Mz(&Cams, sizeof(Cams));
    if (!DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_LIST, NULL, 0, &Cams, sizeof(Cams), &got, NULL)) return FALSE;
    return TRUE;
}

static USBNT_CAMERA *CurCam(void)
{
    if (Cam < 0 || Cam >= USBNT_MAX_CAMS || Cams.Cam[Cam].Id == 0) {
        int i;
        for (i = 0; i < USBNT_MAX_CAMS; i++) {
            if (Cams.Cam[i].Id != 0) { Cam = i; return &Cams.Cam[i]; }
        }
        return NULL;
    }
    return &Cams.Cam[Cam];
}

static BOOL Decodable(ULONG f)
{
    return f == USBNT_FCC_YUY2 || f == USBNT_FCC_UYVY || f == USBNT_FCC_NV12 || f == USBNT_FCC_I420 ||
           f == USBNT_FCC_GREY || f == USBNT_FCC_MJPG;
}

/* the format and frame for w x h; prefers YUY2 (no decoding), then MJPEG */
static BOOL PickMode(int w, int h, ULONG want, int *pf, int *pr)
{
    USBNT_CAMERA *c = CurCam();
    int f, r, best = -1, bf = -1, br = -1;
    if (c == NULL) return FALSE;
    for (f = 0; f < c->NumFormats && f < USBNT_VID_FORMATS; f++) {
        USBNT_VID_FORMAT *vf = &c->Format[f];
        if (!Decodable(vf->FourCC)) continue;
        if (want == USBNT_FCC_YUY2 && vf->FourCC != USBNT_FCC_YUY2) continue;
        if (want == USBNT_FCC_MJPG && vf->FourCC != USBNT_FCC_MJPG) continue;
        for (r = 0; r < vf->NumFrames && r < USBNT_VID_FRAMES; r++) {
            int score;
            if (vf->Frame[r].Width != w || vf->Frame[r].Height != h) continue;
            score = vf->FourCC == USBNT_FCC_YUY2 ? 3 : vf->FourCC == USBNT_FCC_MJPG ? 2 : 1;
            if (score > best) { best = score; bf = f; br = r; }
        }
    }
    if (bf < 0) return FALSE;
    *pf = bf;
    *pr = br;
    return TRUE;
}

/* first use: the largest picture up to 640 x 480 (or what the user chose last time) */
static void DefaultMode(void)
{
    USBNT_CAMERA *c = CurCam();
    int f, r, bw = 0, bh = 0;
    DWORD sw = RegDword(HKEY_CURRENT_USER, "Width", 0), sh = RegDword(HKEY_CURRENT_USER, "Height", 0);
    if (c == NULL) return;
    if (sw && sh && PickMode((int)sw, (int)sh, 0, &Fmt, &Frm)) return;
    for (f = 0; f < c->NumFormats && f < USBNT_VID_FORMATS; f++) {
        if (!Decodable(c->Format[f].FourCC)) continue;
        for (r = 0; r < c->Format[f].NumFrames && r < USBNT_VID_FRAMES; r++) {
            int w = c->Format[f].Frame[r].Width, h = c->Format[f].Frame[r].Height;
            if (w <= 640 && h <= 480 && w * h > bw * bh) { bw = w; bh = h; }
        }
    }
    if (bw == 0) {
        for (f = 0; f < c->NumFormats && bw == 0; f++)
            if (Decodable(c->Format[f].FourCC) && c->Format[f].NumFrames) {
                bw = c->Format[f].Frame[0].Width;
                bh = c->Format[f].Frame[0].Height;
            }
    }
    if (!PickMode(bw, bh, 0, &Fmt, &Frm)) { Fmt = -1; Frm = -1; }
}

static int CurW(void) { USBNT_CAMERA *c = CurCam(); return (c && Fmt >= 0) ? c->Format[Fmt].Frame[Frm].Width : 0; }
static int CurH(void) { USBNT_CAMERA *c = CurCam(); return (c && Fmt >= 0) ? c->Format[Fmt].Frame[Frm].Height : 0; }

static void FillBih(BITMAPINFOHEADER *b)
{
    int w = CurW(), h = CurH();
    Mz(b, sizeof(*b));
    b->biSize = sizeof(*b);
    b->biWidth = w;
    b->biHeight = h;
    b->biPlanes = 1;
    if (OutFcc == USBNT_FCC_YUY2) {
        b->biBitCount = 16;
        b->biCompression = USBNT_FCC_YUY2;
        b->biSizeImage = (DWORD)w * h * 2;
    } else if (OutFcc == USBNT_FCC_MJPG) {
        USBNT_CAMERA *c = CurCam();
        b->biBitCount = 24;
        b->biCompression = USBNT_FCC_MJPG;
        b->biSizeImage = c ? c->Format[Fmt].Frame[Frm].MaxFrameSize + JPEG_DHT_SIZE : (DWORD)w * h * 3;
    } else {
        b->biBitCount = 24;
        b->biCompression = BI_RGB;
        b->biSizeImage = Stride(w, 24) * h;
    }
}

static void StopCam(void)
{
    DWORD got = 0;
    if (CamId) {
        DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_STOP, &CamId, sizeof(CamId), NULL, 0, &got, NULL);
        Log("camera %u stopped", CamId);
    }
    CamId = 0;
}

static BOOL StartCam(void)
{
    union { USBNT_VID_START In; USBNT_VID_STATUS Out; } u;
    DWORD got = 0;
    USBNT_CAMERA *c = CurCam();
    if (CamId) return TRUE;
    if (c == NULL || Fmt < 0) return FALSE;
    Mz(&u, sizeof(u));
    u.In.Id = c->Id;
    u.In.Format = (ULONG)Fmt;
    u.In.Frame = (ULONG)Frm;
    u.In.Interval = IntervalWanted;
    if (!DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_START, &u, sizeof(u.In), &u, sizeof(u.Out), &got, NULL) ||
        u.Out.Result != 0) {
        Log("camera start failed (%u, result %d)", GetLastError(), u.Out.Result);
        return FALSE;
    }
    Stream = u.Out;
    CamId = c->Id;
    if (RawSize < Stream.MaxFrameSize + sizeof(USBNT_VID_FRAME_HDR) + 4096) {
        Free(Raw);
        RawSize = Stream.MaxFrameSize + sizeof(USBNT_VID_FRAME_HDR) + 4096;
        Raw = (UCHAR *)Alloc(RawSize);
        if (Raw == NULL) { RawSize = 0; StopCam(); return FALSE; }
    }
    Seq = 0;
    Log("camera %u: %ux%u %c%c%c%c, interval %u", CamId, Stream.Width, Stream.Height, (char)Stream.FourCC,
        (char)(Stream.FourCC >> 8), (char)(Stream.FourCC >> 16), (char)(Stream.FourCC >> 24), Stream.Interval);
    return TRUE;
}

/* the next frame into Raw; FALSE: none within Timeout */
static BOOL NextFrame(DWORD Timeout)
{
    USBNT_VID_GET g;
    DWORD n = 0;
    g.Id = CamId;
    g.LastSequence = Seq;
    g.TimeoutMs = Timeout;
    if (!DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_FRAME, &g, sizeof(g), Raw, RawSize, &n, NULL) ||
        n < sizeof(USBNT_VID_FRAME_HDR)) {
        return FALSE;
    }
    Seq = ((USBNT_VID_FRAME_HDR *)Raw)->Sequence;
    return ((USBNT_VID_FRAME_HDR *)Raw)->Bytes != 0;
}

/* ------------------------------------------------------------------ */
/* Conversion to what the program wants                                 */
/* ------------------------------------------------------------------ */

static long TY[256], TRV[256], TGU[256], TGV[256], TBU[256];

static void Tables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        TY[i] = 76309L * (i - 16) + 32768;
        TRV[i] = 104597L * (i - 128);
        TGU[i] = 25675L * (i - 128);
        TGV[i] = 53279L * (i - 128);
        TBU[i] = 132201L * (i - 128);
    }
}

static UCHAR Clip(long v) { v >>= 16; return (UCHAR)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void Px(UCHAR *o, int y, int u, int v)
{
    long Y = TY[y];
    o[0] = Clip(Y + TBU[u]);
    o[1] = Clip(Y - TGU[u] - TGV[v]);
    o[2] = Clip(Y + TRV[v]);
}

/* Raw's frame into Out (Cap bytes); returns the bytes, 0 when it cannot */
static DWORD Convert(UCHAR *Out, DWORD Cap)
{
    USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
    const UCHAR *s = Raw + sizeof(*h);
    int w = CurW(), ht = CurH(), x, y;
    ULONG st = Stride(w, 24), n = h->Bytes;

    if (h->Width != w || h->Height != ht) {
        if (h->FourCC != USBNT_FCC_MJPG) return 0;
    }
    if (OutFcc == USBNT_FCC_MJPG) {
        if (h->FourCC != USBNT_FCC_MJPG) return 0;
        return JpegAddDht(s, n, Out, Cap);
    }
    if (OutFcc == USBNT_FCC_YUY2) {
        DWORD need = (DWORD)w * ht * 2;
        if (h->FourCC != USBNT_FCC_YUY2 || Cap < need) return 0;
        Mc(Out, s, n < need ? n : need);
        if (n < need) Mz(Out + n, need - n);
        return need;
    }
    if (Cap < st * ht) return 0;
    switch (h->FourCC) {
    case USBNT_FCC_YUY2:
    case USBNT_FCC_UYVY: {
        int rows = (int)(n / ((ULONG)w * 2));
        if (rows > ht) rows = ht;
        for (y = 0; y < rows; y++) {
            UCHAR *o = Out + (ULONG)(ht - 1 - y) * st;
            const UCHAR *p = s + (ULONG)y * w * 2;
            for (x = 0; x + 1 < w; x += 2, p += 4, o += 6) {
                if (h->FourCC == USBNT_FCC_UYVY) { Px(o, p[1], p[0], p[2]); Px(o + 3, p[3], p[0], p[2]); }
                else { Px(o, p[0], p[1], p[3]); Px(o + 3, p[2], p[1], p[3]); }
            }
        }
        break;
    }
    case USBNT_FCC_NV12:
    case USBNT_FCC_I420: {
        ULONG ysz = (ULONG)w * ht;
        if (n < ysz * 3 / 2) return 0;
        for (y = 0; y < ht; y++) {
            UCHAR *o = Out + (ULONG)(ht - 1 - y) * st;
            for (x = 0; x < w; x++, o += 3) {
                int u, v;
                if (h->FourCC == USBNT_FCC_NV12) {
                    const UCHAR *c = s + ysz + (ULONG)(y / 2) * w + (x & ~1);
                    u = c[0]; v = c[1];
                } else {
                    u = s[ysz + (ULONG)(y / 2) * (w / 2) + x / 2];
                    v = s[ysz + ysz / 4 + (ULONG)(y / 2) * (w / 2) + x / 2];
                }
                Px(o, s[(ULONG)y * w + x], u, v);
            }
        }
        break;
    }
    case USBNT_FCC_GREY:
        for (y = 0; y < ht && (ULONG)(y + 1) * w <= n; y++) {
            UCHAR *o = Out + (ULONG)(ht - 1 - y) * st;
            for (x = 0; x < w; x++, o += 3) o[0] = o[1] = o[2] = s[(ULONG)y * w + x];
        }
        break;
    case USBNT_FCC_MJPG: {
        JPEG_INFO ji;
        int r;
        if (JpegInfo(s, n, &ji) != JPEG_OK || ji.Width > MAX_W) return 0;
        if (JpegWorkSize(ji.Width) > JWorkLen) {
            Free(JWork);
            JWorkLen = JpegWorkSize(ji.Width);
            JWork = Alloc(JWorkLen);
            if (JWork == NULL) { JWorkLen = 0; return 0; }
        }
        if (ji.Width != w || ji.Height != ht) Mz(Out, st * ht);
        r = JpegDecode(s, n, Out, (long)st, 1, w, ht, JWork, JWorkLen, &ji);
        if (r != JPEG_OK && r != JPEG_EDATA) return 0;
        break;
    }
    default:
        return 0;
    }
    return st * ht;
}

/* ------------------------------------------------------------------ */
/* Streaming                                                            */
/* ------------------------------------------------------------------ */

static void Callback(UINT Msg, DWORD_PTR P1)
{
    if (Init.dwCallback) {
        DriverCallback(Init.dwCallback, HIWORD(Init.dwFlags), (HDRVR)Init.hVideo, Msg, Init.dwCallbackInst, P1, 0);
    }
}

static DWORD WINAPI StreamThread(LPVOID P)
{
    (void)P;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    while (WaitForSingleObject(StopEv, 0) == WAIT_TIMEOUT) {
        LPVIDEOHDR hdr = NULL;
        if (!NextFrame(200)) continue;
        EnterCriticalSection(&Lock);
        if (QCount > 0) {
            hdr = Queue[QHead];
            QHead = (QHead + 1) % 64;
            QCount--;
        }
        LeaveCriticalSection(&Lock);
        if (hdr == NULL) {
            Skipped++;
            LastErr = DV_ERR_NO_BUFFERS;
            continue;
        }
        hdr->dwBytesUsed = Convert(hdr->lpData, hdr->dwBufferLength);
        hdr->dwTimeCaptured = GetTickCount() - StartTick;
        hdr->dwFlags = (hdr->dwFlags & ~VHDR_INQUEUE) | VHDR_DONE | VHDR_KEYFRAME;
        Captured++;
        Callback(MM_DRVM_DATA, (DWORD_PTR)hdr);
    }
    return 0;
}

static void ReturnAll(void)
{
    EnterCriticalSection(&Lock);
    while (QCount > 0) {
        LPVIDEOHDR h = Queue[QHead];
        QHead = (QHead + 1) % 64;
        QCount--;
        h->dwBytesUsed = 0;
        h->dwFlags = (h->dwFlags & ~VHDR_INQUEUE) | VHDR_DONE;
        LeaveCriticalSection(&Lock);
        Callback(MM_DRVM_DATA, (DWORD_PTR)h);
        EnterCriticalSection(&Lock);
    }
    LeaveCriticalSection(&Lock);
}

static void StreamStop(void)
{
    if (Thread != NULL) {
        SetEvent(StopEv);
        WaitForSingleObject(Thread, 5000);
        CloseHandle(Thread);
        Thread = NULL;
    }
    Streaming = FALSE;
}

/* ------------------------------------------------------------------ */
/* Dialogs                                                              */
/* ------------------------------------------------------------------ */

static INT_PTR CALLBACK FormatDlg(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    USBNT_CAMERA *c = CurCam();
    HWND lb = GetDlgItem(d, IDC_LIST);
    (void)lp;
    if (m == WM_INITDIALOG) {
        int f, r;
        if (c == NULL) return TRUE;
        for (f = 0; f < c->NumFormats && f < USBNT_VID_FORMATS; f++) {
            if (!Decodable(c->Format[f].FourCC)) continue;
            for (r = 0; r < c->Format[f].NumFrames && r < USBNT_VID_FRAMES; r++) {
                char t[64];
                ULONG fcc = c->Format[f].FourCC;
                LRESULT i;
                wsprintfA(t, "%u x %u  (%c%c%c%c)", c->Format[f].Frame[r].Width, c->Format[f].Frame[r].Height,
                          (char)fcc, (char)(fcc >> 8), (char)(fcc >> 16), (char)(fcc >> 24));
                i = SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)t);
                SendMessageA(lb, LB_SETITEMDATA, (WPARAM)i, (LPARAM)(f * 256 + r));
                if (f == Fmt && r == Frm) SendMessageA(lb, LB_SETCURSEL, (WPARAM)i, 0);
            }
        }
        CheckDlgButton(d, IDC_RGB, OutFcc == FCC_RGB ? BST_CHECKED : BST_UNCHECKED);
        return TRUE;
    }
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK && c != NULL) {
            LRESULT i = SendMessageA(lb, LB_GETCURSEL, 0, 0);
            if (i != LB_ERR) {
                LPARAM v = SendMessageA(lb, LB_GETITEMDATA, (WPARAM)i, 0);
                BOOL was = CamId != 0;
                if (was) StopCam();
                Fmt = (int)(v / 256);
                Frm = (int)(v % 256);
                OutFcc = IsDlgButtonChecked(d, IDC_RGB) == BST_CHECKED ? FCC_RGB :
                         c->Format[Fmt].FourCC == USBNT_FCC_YUY2 ? USBNT_FCC_YUY2 :
                         c->Format[Fmt].FourCC == USBNT_FCC_MJPG ? USBNT_FCC_MJPG : FCC_RGB;
                RegSetDw("Width", CurW());
                RegSetDw("Height", CurH());
                if (was) StartCam();
            }
        }
        EndDialog(d, LOWORD(wp) == IDOK ? DV_ERR_OK : DV_ERR_NONSPECIFIC);
        return TRUE;
    }
    return FALSE;
}

static void CtlRange(HWND d, int id, ULONG sel)
{
    USBNT_VID_CONTROL c;
    DWORD got = 0;
    USBNT_CAMERA *cam = CurCam();
    HWND sb = GetDlgItem(d, id);
    Mz(&c, sizeof(c));
    c.Id = cam ? cam->Id : 0;
    c.Selector = sel;
    if (cam == NULL || !(cam->Controls & (1UL << sel)) ||
        !DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_CONTROL, &c, sizeof(c), &c, sizeof(c), &got, NULL) || c.Max <= c.Min) {
        EnableWindow(sb, FALSE);
        return;
    }
    SetScrollRange(sb, SB_CTL, c.Min, c.Max, FALSE);
    SetScrollPos(sb, SB_CTL, c.Value, TRUE);
}

static void CtlSet(ULONG sel, LONG v)
{
    USBNT_VID_CONTROL c;
    DWORD got = 0;
    USBNT_CAMERA *cam = CurCam();
    if (cam == NULL) return;
    Mz(&c, sizeof(c));
    c.Id = cam->Id;
    c.Selector = sel;
    c.Set = 1;
    c.Value = v;
    DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_CONTROL, &c, sizeof(c), &c, sizeof(c), &got, NULL);
}

static INT_PTR CALLBACK SourceDlg(HWND d, UINT m, WPARAM wp, LPARAM lp)
{
    static const struct { int Id; ULONG Sel; } map[] = {
        { IDC_BRIGHT, USBNT_VCTL_BRIGHTNESS }, { IDC_CONTRAST, USBNT_VCTL_CONTRAST },
        { IDC_SAT, USBNT_VCTL_SATURATION }, { IDC_GAIN, USBNT_VCTL_GAIN },
    };
    int i;
    if (m == WM_INITDIALOG) {
        HWND cb = GetDlgItem(d, IDC_CAMERA);
        for (i = 0; i < USBNT_MAX_CAMS; i++) {
            if (Cams.Cam[i].Id != 0) {
                LRESULT k = SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)Cams.Cam[i].Name);
                SendMessageA(cb, CB_SETITEMDATA, (WPARAM)k, i);
                if (i == Cam) SendMessageA(cb, CB_SETCURSEL, (WPARAM)k, 0);
            }
        }
        for (i = 0; i < 4; i++) CtlRange(d, map[i].Id, map[i].Sel);
        return TRUE;
    }
    if (m == WM_HSCROLL) {
        HWND sb = (HWND)lp;
        int pos = GetScrollPos(sb, SB_CTL), lo, hi;
        GetScrollRange(sb, SB_CTL, &lo, &hi);
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos--; break;
        case SB_LINERIGHT: pos++; break;
        case SB_PAGELEFT: pos -= (hi - lo) / 10 + 1; break;
        case SB_PAGERIGHT: pos += (hi - lo) / 10 + 1; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = (short)HIWORD(wp); break;
        default: break;
        }
        if (pos < lo) pos = lo;
        if (pos > hi) pos = hi;
        SetScrollPos(sb, SB_CTL, pos, TRUE);
        for (i = 0; i < 4; i++)
            if (GetDlgItem(d, map[i].Id) == sb) CtlSet(map[i].Sel, pos);
        return TRUE;
    }
    if (m == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
        if (LOWORD(wp) == IDOK) {
            HWND cb = GetDlgItem(d, IDC_CAMERA);
            LRESULT k = SendMessageA(cb, CB_GETCURSEL, 0, 0);
            if (k != CB_ERR) {
                int n = (int)SendMessageA(cb, CB_GETITEMDATA, (WPARAM)k, 0);
                if (n != Cam) {
                    BOOL was = CamId != 0;
                    if (was) StopCam();
                    Cam = n;
                    RegSetDw("Camera", (DWORD)n);
                    DefaultMode();
                    if (was) StartCam();
                }
            }
        }
        EndDialog(d, LOWORD(wp) == IDOK ? DV_ERR_OK : DV_ERR_NONSPECIFIC);
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Driver entry                                                         */
/* ------------------------------------------------------------------ */

static LRESULT Configure(UINT Msg, DWORD Flags, VID_CONFIG_PARMS *P)
{
    if (Msg != DVM_FORMAT) {
        if (Msg == DVM_SRC_RECT || Msg == DVM_DST_RECT) {
            if ((Flags & VIDEO_CONFIGURE_QUERY)) return DV_ERR_OK;
            if ((Flags & VIDEO_CONFIGURE_GET) && P && P->lpData1 && P->dwSize1 >= sizeof(RECT)) {
                RECT *r = (RECT *)P->lpData1;
                r->left = r->top = 0;
                r->right = CurW();
                r->bottom = CurH();
                return DV_ERR_OK;
            }
            if (Flags & VIDEO_CONFIGURE_SET) return DV_ERR_OK;
        }
        return DV_ERR_NOTSUPPORTED;
    }
    if (Flags & VIDEO_CONFIGURE_QUERY) return DV_ERR_OK;
    if (Flags & VIDEO_CONFIGURE_QUERYSIZE) {
        if (P && P->lpdwReturn) *P->lpdwReturn = sizeof(BITMAPINFOHEADER);
        return DV_ERR_OK;
    }
    if (P == NULL || P->lpData1 == NULL) return DV_ERR_PARAM2;
    if (Flags & VIDEO_CONFIGURE_GET) {
        Log("format get (%u bytes)", P->dwSize1);
        if (P->dwSize1 < sizeof(BITMAPINFOHEADER)) return DV_ERR_SIZEFIELD;
        FillBih((BITMAPINFOHEADER *)P->lpData1);
        if (P->lpdwReturn) *P->lpdwReturn = sizeof(BITMAPINFOHEADER);
        return DV_ERR_OK;
    }
    if (Flags & VIDEO_CONFIGURE_SET) {
        BITMAPINFOHEADER *b = (BITMAPINFOHEADER *)P->lpData1;
        int f, r, h;
        ULONG want;
        if (P->dwSize1 < sizeof(BITMAPINFOHEADER)) return DV_ERR_SIZEFIELD;
        h = b->biHeight < 0 ? -b->biHeight : b->biHeight;
        want = b->biCompression == BI_RGB ? (b->biBitCount == 24 ? FCC_RGB : 0xFFFFFFFF) : b->biCompression;
        if (want == 0xFFFFFFFF) return DV_ERR_BADFORMAT;
        if (want != FCC_RGB && want != USBNT_FCC_YUY2 && want != USBNT_FCC_MJPG) return DV_ERR_BADFORMAT;
        if (!PickMode(b->biWidth, h, want, &f, &r)) return DV_ERR_BADFORMAT;
        if (Streaming) return DV_ERR_ALLOCATED;
        StopCam();
        Fmt = f;
        Frm = r;
        OutFcc = want;
        Log("format %dx%d %08x", b->biWidth, h, want);
        return DV_ERR_OK;
    }
    return DV_ERR_FLAGS;
}

LRESULT CALLBACK DriverProc(DWORD_PTR Id, HDRVR Drv, UINT Msg, LPARAM P1, LPARAM P2)
{
    CHAN *ch = (CHAN *)Id;
    LRESULT r;

    switch (Msg) {
    case DRV_LOAD:
        Heap = GetProcessHeap();
        InitializeCriticalSection(&Lock);
        Tables();
        return 1;
    case DRV_FREE:
        StreamStop();
        StopCam();
        if (Dev != INVALID_HANDLE_VALUE) CloseHandle(Dev);
        Dev = INVALID_HANDLE_VALUE;
        return 1;
    case DRV_ENABLE:
    case DRV_DISABLE:
        return 1;
    case DRV_QUERYCONFIGURE:
        return 0;
    case DRV_OPEN: {
        VID_OPEN_PARMS *op = (VID_OPEN_PARMS *)P2;
        if (op == NULL) return 1;           /* opened for configuration */
        if (!OpenDev() || CurCam() == NULL) {
            op->dwError = DV_ERR_NOTDETECTED;
            Log("open: no camera (usbnt.sys running: %u)", Dev != INVALID_HANDLE_VALUE);
            return 0;
        }
        if (op->dwFlags & VIDEO_EXTERNALOUT) {
            op->dwError = DV_ERR_NOTSUPPORTED;      /* no overlay */
            return 0;
        }
        ch = (CHAN *)Alloc(sizeof(CHAN));
        if (ch == NULL) {
            op->dwError = DV_ERR_NOMEM;
            return 0;
        }
        ch->Kind = op->dwFlags & (VIDEO_IN | VIDEO_EXTERNALIN);
        if (InterlockedIncrement(&Opens) == 1) {
            Cam = (int)RegDword(HKEY_CURRENT_USER, "Camera", 0);
            if (CurCam() == NULL) Cam = 0;
            DefaultMode();
        }
        op->dwError = DV_ERR_OK;
        Log("open channel %x, camera %d \"%s\" %dx%d", ch->Kind, Cam, CurCam()->Name, CurW(), CurH());
        return (LRESULT)ch;
    }
    case DRV_CLOSE:
        if (ch == NULL) return 1;
        if (ch->Kind & VIDEO_IN) {
            StreamStop();
            ReturnAll();
            StreamInit = FALSE;
        }
        if (InterlockedDecrement(&Opens) == 0) StopCam();
        Free(ch);
        return 1;
    case DVM_GETERRORTEXT: {
        VID_ERRTEXT *g = (VID_ERRTEXT *)P1;
        if (g && g->lpText && g->dwLength) {
            static const char *t = "USB camera error";
            DWORD i;
            for (i = 0; i + 1 < g->dwLength && t[i]; i++) g->lpText[i] = (WCHAR)t[i];
            g->lpText[i] = 0;
        }
        return DV_ERR_OK;
    }
    case DVM_GETVIDEOAPIVER:
        if (P1) *(LPDWORD)P1 = VIDEOAPIVERSION;
        return DV_ERR_OK;
    case DVM_GET_CHANNEL_CAPS:
        if (P1 && (DWORD)P2 >= sizeof(CHANNEL_CAPS)) {
            CHANNEL_CAPS *c = (CHANNEL_CAPS *)P1;
            Mz(c, sizeof(*c));
            c->dwSrcRectXMod = c->dwSrcRectYMod = c->dwSrcRectWidthMod = c->dwSrcRectHeightMod = 1;
            c->dwDstRectXMod = c->dwDstRectYMod = c->dwDstRectWidthMod = c->dwDstRectHeightMod = 1;
        }
        return DV_ERR_OK;
    case DVM_UPDATE:
    case DVM_CONFIGURESTORAGE:
        return DV_ERR_OK;
    case DVM_DIALOG:
        if (ch == NULL) return DV_ERR_NOTSUPPORTED;
        if (ch->Kind & VIDEO_IN) {
            if ((DWORD)P2 & VIDEO_DLG_QUERY) return DV_ERR_OK;
            return DialogBoxParamA(Inst, MAKEINTRESOURCEA(IDD_FORMAT), (HWND)P1, FormatDlg, 0);
        }
        if (ch->Kind & VIDEO_EXTERNALIN) {
            if ((DWORD)P2 & VIDEO_DLG_QUERY) return DV_ERR_OK;
            return DialogBoxParamA(Inst, MAKEINTRESOURCEA(IDD_SOURCE), (HWND)P1, SourceDlg, 0);
        }
        return DV_ERR_NOTSUPPORTED;
    case DVM_FORMAT:
    case DVM_SRC_RECT:
    case DVM_DST_RECT:
    case DVM_PALETTE:
    case DVM_PALETTERGB555:
        if (ch == NULL || !(ch->Kind & VIDEO_IN)) {
            if (Msg == DVM_SRC_RECT || Msg == DVM_DST_RECT) return Configure(Msg, (DWORD)P1, (VID_CONFIG_PARMS *)P2);
            return DV_ERR_NOTSUPPORTED;
        }
        r = Configure(Msg, (DWORD)P1, (VID_CONFIG_PARMS *)P2);
        return r;
    case DVM_FRAME: {
        /* one picture now (preview, capGrabFrame) */
        LPVIDEOHDR h = (LPVIDEOHDR)P1;
        DWORD t0 = GetTickCount();
        if (h == NULL || h->lpData == NULL) return DV_ERR_PARAM1;
        if (!StartCam()) return DV_ERR_NOTDETECTED;
        while (!NextFrame(500)) {
            if (GetTickCount() - t0 > 3000) return DV_ERR_NONSPECIFIC;
        }
        h->dwBytesUsed = Convert(h->lpData, h->dwBufferLength);
        h->dwTimeCaptured = GetTickCount();
        h->dwFlags |= VHDR_DONE | VHDR_KEYFRAME;
        return h->dwBytesUsed ? DV_ERR_OK : DV_ERR_BADFORMAT;
    }
    case DVM_STREAM_INIT: {
        VID_STREAM_INIT *si = (VID_STREAM_INIT *)P1;
        if (si == NULL) return DV_ERR_PARAM1;
        if (StreamInit) {
            /* initialized again (avicap does it before a capture): new callback, same camera */
            StreamStop();
            ReturnAll();
        }
        Init = *si;
        if (Init.dwMicroSecPerFrame) IntervalWanted = Init.dwMicroSecPerFrame * 10;
        StopCam();                          /* again with the frame rate asked for */
        if (!StartCam()) return DV_ERR_NOTDETECTED;
        QHead = QCount = 0;
        Captured = Skipped = 0;
        LastErr = 0;
        StreamInit = TRUE;
        if (StopEv == NULL) StopEv = CreateEventA(NULL, TRUE, FALSE, NULL);
        Callback(MM_DRVM_OPEN, 0);
        Log("stream init, %u us per frame", Init.dwMicroSecPerFrame);
        return DV_ERR_OK;
    }
    case DVM_STREAM_FINI:
        StreamStop();
        ReturnAll();
        if (StreamInit) Callback(MM_DRVM_CLOSE, 0);
        StreamInit = FALSE;
        Log("stream fini: %u frames, %u skipped", Captured, Skipped);
        return DV_ERR_OK;
    case DVM_STREAM_ADDBUFFER: {
        LPVIDEOHDR h = (LPVIDEOHDR)P1;
        if (h == NULL) return DV_ERR_PARAM1;
        EnterCriticalSection(&Lock);
        if (QCount >= 64) {
            LeaveCriticalSection(&Lock);
            return DV_ERR_NOMEM;
        }
        h->dwFlags = (h->dwFlags & ~VHDR_DONE) | VHDR_INQUEUE;
        h->dwBytesUsed = 0;
        Queue[(QHead + QCount) % 64] = h;
        QCount++;
        LeaveCriticalSection(&Lock);
        return DV_ERR_OK;
    }
    case DVM_STREAM_PREPAREHEADER:
    case DVM_STREAM_UNPREPAREHEADER:
        return DV_ERR_NOTSUPPORTED;         /* msvideo prepares the headers */
    case DVM_STREAM_START:
        if (!StreamInit) return DV_ERR_NONSPECIFIC;
        if (Streaming) return DV_ERR_OK;
        ResetEvent(StopEv);
        StartTick = GetTickCount();
        Streaming = TRUE;
        {
            DWORD tid;
            Thread = CreateThread(NULL, 0, StreamThread, NULL, 0, &tid);
        }
        return Thread ? DV_ERR_OK : DV_ERR_NOMEM;
    case DVM_STREAM_STOP:
        StreamStop();
        return DV_ERR_OK;
    case DVM_STREAM_RESET:
        StreamStop();
        ReturnAll();
        return DV_ERR_OK;
    case DVM_STREAM_GETERROR:
        if (P1) *(LPDWORD)P1 = LastErr;
        if (P2) *(LPDWORD)P2 = Skipped;
        LastErr = 0;
        return DV_ERR_OK;
    case DVM_STREAM_GETPOSITION:
        if (P1 && (DWORD)P2 >= sizeof(MMTIME)) {
            MMTIME *t = (MMTIME *)P1;
            t->wType = TIME_MS;
            t->u.ms = Streaming ? GetTickCount() - StartTick : 0;
        }
        return DV_ERR_OK;
    default:
        if (Msg >= DRV_USER) {
            /* buffer allocation (DVM_STREAM_ALLOCBUFFER ...) and the rest: msvideo / avicap do it themselves */
            Log("message %x not supported", Msg);
            return DV_ERR_NOTSUPPORTED;
        }
        return DefDriverProc(Id, Drv, Msg, P1, P2);
    }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r)
{
    (void)r;
    if (reason == DLL_PROCESS_ATTACH) Inst = h;
    return TRUE;
}
