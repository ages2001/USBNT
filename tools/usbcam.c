/*
 * usbcam.c - USB camera viewer: live picture, photos (BMP, and the original
 * JPEG of MJPEG cameras) and video recording (AVI) from the USB Video Class
 * cameras that usbnt.sys drives.  Runs on Windows NT 3.1 and later, no C
 * runtime.
 *
 * On Windows 2000 / XP it also shows the cameras of Windows' own drivers
 * (XP SP2 and later, XP x64: usbvideo.sys, which takes the cameras where
 * usbinst set WindowsCamera) through Video for Windows - the "Microsoft WDM
 * Image Capture" driver: frames by capGrabFrameNoStop, the format from
 * capGetVideoFormat / capDlgVideoFormat, the camera's settings in
 * capDlgVideoSource.  usbntcap.dll (USBNT's own VfW driver) is not listed
 * there: its cameras are the ones usbnt.sys lists.
 *
 *   usbcam                         the window
 *   usbcam /test LOG [/cam N] [/fmt N] [/res N] [/list] [/snap FILE.BMP]
 *          [/jpeg FILE.JPG] [/record FILE.AVI SECONDS] [/bright V] [/allmodes]
 *                                  automatic use, results go to LOG
 *                                  (/allmodes: every format and size of the
 *                                  camera in turn, a few frames each)
 *   usbcam /gui N SECONDS LOG [CAM]
 *                                  the window, picture item N (of camera
 *                                  CAM, 1 = the first) started at once, a
 *                                  photo after SECONDS, then it stops,
 *                                  writes LOG and closes (tests)
 *
 * The cameras of usbnt.sys come first, then the Windows cameras (/cam N
 * counts both; /fmt, /res, /bright and /allmodes are for USBNT's cameras).
 */

#include <windows.h>
#include <commdlg.h>
#include <winioctl.h>
#include <mmsystem.h>
#include <vfw.h>
#include "../src/usbntioc.h"
#include "jpegdec.h"
#include "avi.h"

#define ID_CAM          100
#define ID_FMT          101
#define ID_START        102
#define ID_PHOTO        103
#define ID_RECORD       104
#define ID_FOLDER       105
#define ID_JPEG         106
#define ID_BRIGHT       107
#define ID_CONTRAST     108
#define ID_SOURCE       109
#define WM_CAM_FRAME    (WM_USER + 1)
#define WM_CAM_CHANGED  (WM_USER + 2)
#define WM_CAM_STOPPED  (WM_USER + 3)

#define MAX_W           4096
#define MAX_H           3072
#define AVI_SPLIT       (1000UL * 1024 * 1024)
#define AVI_MAX_FRAMES  200000UL
#define TOP_H           128
#define FCC_RGB32       USBNT_FOURCC('R', 'G', 'B', '4')   /* Video for Windows only */
#define HDR_TOPDOWN     2                                   /* frame header flag: RGB rows top-down */

typedef struct _FMT_ITEM {
    int Fmt, Frame;
} FMT_ITEM;

static HINSTANCE Inst;
static HWND MainWnd, CamBox, FmtBox, StartBtn, PhotoBtn, RecBtn, FolderBtn, JpegChk, BrightSb, ContrastSb, StatusWnd;
static HWND BrightLbl, ContrastLbl, SourceBtn;
static HFONT Font;
static HANDLE Dev = INVALID_HANDLE_VALUE;
static HANDLE Heap;
static USBNT_CAMERAS Cams;
static FMT_ITEM FmtItems[USBNT_VID_FORMATS * USBNT_VID_FRAMES];
static int NFmtItems;
static int CurCam = -1;
static char Folder[MAX_PATH];
static char LastFile[MAX_PATH];

static int Vfw = -1;                    /* Windows camera: the stream comes from VfwDrv[Vfw] */
static int NVfw;
static BOOL IsVfwCam(int i);

/* capture thread state */
static CRITICAL_SECTION Lock;
static HANDLE CapThread;
static volatile LONG CapRun;
static volatile LONG PhotoReq;
static volatile LONG RecReq;            /* 1 = start, 2 = stop */
static ULONG CamId;
static USBNT_VID_STATUS Stream;
static UCHAR *Raw;                      /* frame header + data from the driver */
static ULONG RawSize;
static UCHAR *Dib;                      /* decoded picture (capture thread) */
static UCHAR *Show;                     /* copy for painting */
static int ShowW, ShowH, ShowValid;
static void *JpegWork;
static ULONG JpegWorkLen;
static UCHAR *JpegBuf;                  /* MJPEG frame with DHT (photos, AVI) */
static ULONG JpegBufLen;
static ULONG DibCap, ShowCap;           /* bytes allocated */
static int DecW, DecH;                  /* size of the picture in Dib after Decode */
static volatile LONG JpegToo = 1;       /* "Photos also as JPEG" (the window thread keeps it) */
static ULONG LastErr;                   /* of the last failed frame request */
static volatile LONG FramePending;      /* a new picture waits to be painted */
static int GuiItem = -1;                /* /gui */
static int GuiCam;
static ULONG GuiSecs;
static char GuiLog[MAX_PATH];

/* statistics shown in the status line */
static ULONG StFrames, StErrors, StFps, StRecFrames, StDecodeErr;
static char StMsg[160];

/* recording */
static HANDLE AviFile = INVALID_HANDLE_VALUE;
static AVI_WRITER Avi;
static AVI_INDEX *AviIndex;
static ULONG AviStartMs, AviPart;
static char AviBase[MAX_PATH];
static ULONG AviUsPerFrame;

/* ------------------------------------------------------------------ */
/* small helpers (no C runtime)                                        */
/* ------------------------------------------------------------------ */
static void Mz(void *p, ULONG n)
{
    volatile UCHAR *d = (volatile UCHAR *)p;
    while (n--) *d++ = 0;
}

static void Mc(void *dst, const void *src, ULONG n)
{
    volatile UCHAR *d = (volatile UCHAR *)dst;
    const UCHAR *s = (const UCHAR *)src;
    while (n >= 4) {
        *(volatile ULONG *)d = *(const ULONG *)s;
        d += 4; s += 4; n -= 4;
    }
    while (n--) *d++ = *s++;
}

/* gcc may still emit calls to these for structure copies */
void *memcpy(void *d, const void *s, unsigned int n) { Mc(d, s, n); return d; }
void *memset(void *d, int c, unsigned int n)
{
    volatile UCHAR *p = (volatile UCHAR *)d;
    while (n--) *p++ = (UCHAR)c;
    return d;
}

/* lstrcpynA is missing on NT 3.1 */
static void Cpyn(char *d, const char *s, int n)
{
    int i;
    for (i = 0; i + 1 < n && s[i]; i++) d[i] = s[i];
    if (n > 0) d[i] = 0;
}

static void *Alloc(ULONG n) { return HeapAlloc(Heap, HEAP_ZERO_MEMORY, n); }
static void Free(void *p) { if (p) HeapFree(Heap, 0, p); }

static void Fcc(char *o, ULONG f)
{
    int i;
    for (i = 0; i < 4; i++) {
        char c = (char)(f >> (8 * i));
        o[i] = (c >= 32 && c < 127) ? c : '?';
    }
    o[4] = 0;
}

static ULONG Stride(int w) { return ((ULONG)w * 3 + 3) & ~3UL; }

static BOOL WriteAll(HANDLE h, const void *d, ULONG n)
{
    DWORD got = 0;
    return WriteFile(h, d, n, &got, NULL) && got == n;
}

static void StatusSet(const char *s)
{
    Cpyn(StMsg, s, sizeof(StMsg));
    if (MainWnd) PostMessageA(MainWnd, WM_CAM_FRAME, 1, 0);
}

/* ------------------------------------------------------------------ */
/* colour conversion to bottom-up 24-bit BGR                           */
/* ------------------------------------------------------------------ */
static long TY[256], TRV[256], TGU[256], TGV[256], TBU[256];

static void InitTables(void)
{
    int i;
    for (i = 0; i < 256; i++) {
        /* BT.601, video range (cameras' uncompressed formats) in 16.16 */
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

static void ConvPacked(const UCHAR *s, int w, int h, int rows, UCHAR *dib, int uyvy)
{
    int x, y;
    ULONG st = Stride(w);
    for (y = 0; y < rows; y++) {
        UCHAR *o = dib + (ULONG)(h - 1 - y) * st;
        const UCHAR *p = s + (ULONG)y * w * 2;
        for (x = 0; x + 1 < w; x += 2, p += 4, o += 6) {
            if (uyvy) { Px(o, p[1], p[0], p[2]); Px(o + 3, p[3], p[0], p[2]); }
            else { Px(o, p[0], p[1], p[3]); Px(o + 3, p[2], p[1], p[3]); }
        }
    }
}

static void ConvPlanar(const UCHAR *s, int w, int h, UCHAR *dib, int nv12)
{
    int x, y;
    ULONG st = Stride(w), ysz = (ULONG)w * h;
    const UCHAR *U = s + ysz, *V = s + ysz + ysz / 4;
    for (y = 0; y < h; y++) {
        UCHAR *o = dib + (ULONG)(h - 1 - y) * st;
        const UCHAR *Y = s + (ULONG)y * w;
        for (x = 0; x < w; x++, o += 3) {
            int u, v;
            if (nv12) { const UCHAR *c = s + ysz + (ULONG)(y / 2) * w + (x & ~1); u = c[0]; v = c[1]; }
            else { u = U[(ULONG)(y / 2) * (w / 2) + x / 2]; v = V[(ULONG)(y / 2) * (w / 2) + x / 2]; }
            Px(o, Y[x], u, v);
        }
    }
}

/* 24 / 32 bit RGB (Video for Windows: bottom-up, or top-down with a negative height) */
static void ConvRgb(const UCHAR *s, int w, int h, int rows, UCHAR *dib, int bpp, int topDown)
{
    int x, y;
    ULONG st = Stride(w), ss = ((ULONG)w * (bpp / 8) + 3) & ~3UL;
    for (y = 0; y < rows; y++) {
        UCHAR *o = dib + (ULONG)(topDown ? h - 1 - y : y) * st;
        const UCHAR *p = s + (ULONG)y * ss;
        if (bpp == 24) {
            Mc(o, p, (ULONG)w * 3);
        } else {
            for (x = 0; x < w; x++, o += 3, p += 4) { o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; }
        }
    }
}

static void ConvGrey(const UCHAR *s, int w, int h, int rows, UCHAR *dib)
{
    int x, y;
    ULONG st = Stride(w);
    for (y = 0; y < rows; y++) {
        UCHAR *o = dib + (ULONG)(h - 1 - y) * st;
        for (x = 0; x < w; x++, o += 3) o[0] = o[1] = o[2] = s[(ULONG)y * w + x];
    }
}

/* Dib large enough for w x h (the capture thread owns Dib) */
static BOOL DibRoom(int w, int h)
{
    ULONG need = Stride(w) * h;
    if (need > DibCap) {
        UCHAR *n = (UCHAR *)Alloc(need);
        if (n == NULL) return FALSE;
        Free(Dib);
        Dib = n;
        DibCap = need;
    }
    return TRUE;
}

/* raw frame -> Dib (DecW x DecH); FALSE when the data cannot be shown.  The
   picture size is taken from the data where it carries one (JPEG), and a
   short uncompressed frame shows the lines that came. */
static BOOL Decode(const USBNT_VID_FRAME_HDR *h, const UCHAR *d)
{
    int w = h->Width, ht = h->Height, rows;
    ULONG n = h->Bytes;
    if (w <= 0 || ht <= 0 || w > MAX_W || ht > MAX_H) return FALSE;
    switch (h->FourCC) {
    case USBNT_FCC_YUY2:
    case USBNT_FCC_UYVY:
        rows = (int)(n / ((ULONG)w * 2));
        if (rows > ht) rows = ht;
        if (rows < ht / 2 || !DibRoom(w, ht)) { StDecodeErr++; return FALSE; }
        if (rows < ht) StDecodeErr++;
        ConvPacked(d, w, ht, rows, Dib, h->FourCC == USBNT_FCC_UYVY);
        break;
    case USBNT_FCC_NV12:
    case USBNT_FCC_I420:
        if (n < (ULONG)w * ht * 3 / 2 || !DibRoom(w, ht)) { StDecodeErr++; return FALSE; }
        ConvPlanar(d, w, ht, Dib, h->FourCC == USBNT_FCC_NV12);
        break;
    case USBNT_FCC_RGB24:
    case FCC_RGB32: {
        int bpp = h->FourCC == FCC_RGB32 ? 32 : 24;
        rows = (int)(n / ((((ULONG)w * (bpp / 8)) + 3) & ~3UL));
        if (rows > ht) rows = ht;
        if (rows < ht / 2 || !DibRoom(w, ht)) { StDecodeErr++; return FALSE; }
        if (rows < ht) StDecodeErr++;
        ConvRgb(d, w, ht, rows, Dib, bpp, (h->Flags & HDR_TOPDOWN) != 0);
        break;
    }
    case USBNT_FCC_GREY:
        rows = (int)(n / (ULONG)w);
        if (rows > ht) rows = ht;
        if (rows < ht / 2 || !DibRoom(w, ht)) { StDecodeErr++; return FALSE; }
        ConvGrey(d, w, ht, rows, Dib);
        break;
    case USBNT_FCC_MJPG: {
        JPEG_INFO ji;
        int r = JpegInfo(d, n, &ji);
        if (r != JPEG_OK || ji.Width <= 0 || ji.Height <= 0 || ji.Width > MAX_W || ji.Height > MAX_H) {
            StDecodeErr++;
            return FALSE;
        }
        /* the camera may send another size than it announced */
        w = ji.Width;
        ht = ji.Height;
        if (JpegWorkSize(w) > JpegWorkLen) {
            void *nw = Alloc(JpegWorkSize(w));
            if (nw == NULL) return FALSE;
            Free(JpegWork);
            JpegWork = nw;
            JpegWorkLen = JpegWorkSize(w);
        }
        if (!DibRoom(w, ht)) return FALSE;
        r = JpegDecode(d, n, Dib, (long)Stride(w), 1, w, ht, JpegWork, JpegWorkLen, &ji);
        if (r != JPEG_OK && r != JPEG_EDATA) { StDecodeErr++; return FALSE; }
        break;
    }
    default:
        return FALSE;
    }
    DecW = w;
    DecH = ht;
    return TRUE;
}

/* the decoded picture to the window's copy */
static void ToShow(void)
{
    ULONG need = Stride(DecW) * DecH;
    EnterCriticalSection(&Lock);
    if (need > ShowCap) {
        UCHAR *n = (UCHAR *)Alloc(need);
        if (n == NULL) { LeaveCriticalSection(&Lock); return; }
        Free(Show);
        Show = n;
        ShowCap = need;
    }
    Mc(Show, Dib, need);
    ShowW = DecW;
    ShowH = DecH;
    ShowValid = 1;
    LeaveCriticalSection(&Lock);
}

/* only formats this program can show are offered */
static BOOL CanShow(ULONG FourCC)
{
    return FourCC == USBNT_FCC_YUY2 || FourCC == USBNT_FCC_UYVY || FourCC == USBNT_FCC_NV12 ||
           FourCC == USBNT_FCC_I420 || FourCC == USBNT_FCC_GREY || FourCC == USBNT_FCC_MJPG ||
           FourCC == USBNT_FCC_RGB24 || FourCC == FCC_RGB32;
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */
static void FileName(char *out, const char *ext, int part)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (part > 1) {
        wsprintfA(out, "%s\\CAM%04u%02u%02u-%02u%02u%02u-%u.%s", Folder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, part, ext);
    } else {
        wsprintfA(out, "%s\\CAM%04u%02u%02u-%02u%02u%02u.%s", Folder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                  t.wSecond, ext);
    }
}

static BOOL SaveBmp(const char *path, const UCHAR *dib, int w, int h)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER bi;
    ULONG img = Stride(w) * h;
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    Mz(&fh, sizeof(fh));
    Mz(&bi, sizeof(bi));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + img;
    bi.biSize = sizeof(bi);
    bi.biWidth = w;
    bi.biHeight = h;
    bi.biPlanes = 1;
    bi.biBitCount = 24;
    bi.biCompression = BI_RGB;
    bi.biSizeImage = img;
    ok = WriteAll(f, &fh, sizeof(fh)) && WriteAll(f, &bi, sizeof(bi)) && WriteAll(f, dib, img);
    CloseHandle(f);
    return ok;
}

static BOOL SaveJpeg(const char *path, const UCHAR *d, ULONG n)
{
    ULONG m = JpegAddDht(d, n, JpegBuf, JpegBufLen);
    HANDLE f;
    BOOL ok;
    if (m == 0) return FALSE;
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteAll(f, JpegBuf, m);
    CloseHandle(f);
    return ok;
}

static int AviWrite(void *Ctx, const void *Data, unsigned long Len)
{
    return WriteAll((HANDLE)Ctx, Data, Len) ? 0 : -1;
}

static int AviSeek(void *Ctx, unsigned long Pos)
{
    return SetFilePointer((HANDLE)Ctx, (LONG)Pos, NULL, FILE_BEGIN) == 0xFFFFFFFF ? -1 : 0;
}

static ULONG AviCompression(void)
{
    if (Stream.FourCC == USBNT_FCC_MJPG) return AVI_FCC('M', 'J', 'P', 'G');
    if (Stream.FourCC == USBNT_FCC_YUY2) return AVI_FCC('Y', 'U', 'Y', '2');
    return 0;
}

static BOOL AviBegin(const char *path)
{
    AVI_IO io;
    ULONG comp = AviCompression();
    if (AviIndex == NULL) AviIndex = (AVI_INDEX *)Alloc(sizeof(AVI_INDEX) * AVI_MAX_FRAMES);
    if (AviIndex == NULL) return FALSE;
    AviFile = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (AviFile == INVALID_HANDLE_VALUE) return FALSE;
    io.Ctx = AviFile;
    io.Write = AviWrite;
    io.Seek = AviSeek;
    AviUsPerFrame = Stream.Interval ? Stream.Interval / 10 : 33333;
    if (AviOpen(&Avi, &io, comp, Stream.Width, Stream.Height, comp == AVI_FCC('Y', 'U', 'Y', '2') ? 16 : 24,
                AviUsPerFrame, AviIndex, AVI_MAX_FRAMES)) {
        CloseHandle(AviFile);
        AviFile = INVALID_HANDLE_VALUE;
        return FALSE;
    }
    AviStartMs = GetTickCount();
    lstrcpyA(LastFile, path);
    return TRUE;
}

static void AviEnd(void)
{
    ULONG ms;
    if (AviFile == INVALID_HANDLE_VALUE) return;
    ms = GetTickCount() - AviStartMs;
    /* the real frame rate (cameras deliver less in poor light or on a slow bus) */
    AviClose(&Avi, Avi.Frames > 1 ? (ms * 1000 / Avi.Frames) : 0);
    CloseHandle(AviFile);
    AviFile = INVALID_HANDLE_VALUE;
}

static void AviAdd(const USBNT_VID_FRAME_HDR *h, const UCHAR *d, BOOL decoded)
{
    const UCHAR *data = d;
    ULONG len = h->Bytes;
    ULONG comp = AviCompression();
    if (AviFile == INVALID_HANDLE_VALUE) return;
    if (comp == AVI_FCC('M', 'J', 'P', 'G')) {
        len = JpegAddDht(d, h->Bytes, JpegBuf, JpegBufLen);
        if (len == 0) return;
        data = JpegBuf;
    } else if (comp == 0) {
        if (!decoded || DecW != (int)Stream.Width || DecH != (int)Stream.Height) return;
        data = Dib;
        len = Stride(DecW) * DecH;
    }
    if (Avi.Pos + AVI_FRAME_COST(len) + 16 > AVI_SPLIT || Avi.Frames + 1 >= AVI_MAX_FRAMES) {
        /* next part */
        char path[MAX_PATH];
        AviEnd();
        AviPart++;
        wsprintfA(path, "%s-%u.AVI", AviBase, AviPart);
        if (!AviBegin(path)) return;
    }
    if (AviFrame(&Avi, data, len) == 0) StRecFrames++;
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */
static BOOL ListCams(void)
{
    DWORD got = 0;
    Mz(&Cams, sizeof(Cams));
    return DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_LIST, NULL, 0, &Cams, sizeof(Cams), &got, NULL) &&
           got >= sizeof(Cams);
}

static BOOL StartCam(ULONG id, int fmt, int frame)
{
    union { USBNT_VID_START In; USBNT_VID_STATUS Out; } u;
    DWORD got = 0;
    Mz(&u, sizeof(u));
    u.In.Id = id;
    u.In.Format = (ULONG)fmt;
    u.In.Frame = (ULONG)frame;
    u.In.Interval = 0;
    if (!DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_START, &u, sizeof(u.In), &u, sizeof(u.Out), &got, NULL)) {
        Stream.Result = -1;
        return FALSE;
    }
    Stream = u.Out;
    CamId = id;
    return Stream.Result == 0;
}

static void StopCam(void)
{
    DWORD got = 0;
    if (CamId) DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_STOP, &CamId, sizeof(CamId), NULL, 0, &got, NULL);
}

static BOOL Control(ULONG sel, BOOL set, LONG value, USBNT_VID_CONTROL *out)
{
    USBNT_VID_CONTROL c;
    DWORD got = 0;
    if (Vfw >= 0 || (!CamId && (CurCam < 0 || IsVfwCam(CurCam)))) return FALSE;    /* VfW: the driver's dialog */
    Mz(&c, sizeof(c));
    c.Id = CamId ? CamId : Cams.Cam[CurCam].Id;
    c.Selector = sel;
    c.Set = set ? 1 : 0;
    c.Value = value;
    if (!DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_CONTROL, &c, sizeof(c), &c, sizeof(c), &got, NULL)) return FALSE;
    if (out) *out = c;
    return TRUE;
}

/* frame buffers for a stream */
static BOOL Buffers(void)
{
    ULONG need = Stream.MaxFrameSize + sizeof(USBNT_VID_FRAME_HDR) + 4096;
    ULONG dib = Stride(Stream.Width) * Stream.Height;
    if (Stream.Width == 0 || Stream.Height == 0 || Stream.Width > MAX_W || Stream.Height > MAX_H) return FALSE;
    Free(Raw); Free(Dib); Free(JpegWork); Free(JpegBuf);
    RawSize = need;
    Raw = (UCHAR *)Alloc(need);
    Dib = (UCHAR *)Alloc(dib);
    DibCap = Dib ? dib : 0;
    EnterCriticalSection(&Lock);
    Free(Show);
    Show = (UCHAR *)Alloc(dib);
    ShowCap = Show ? dib : 0;
    ShowValid = 0;
    LeaveCriticalSection(&Lock);
    JpegWorkLen = JpegWorkSize(Stream.Width);
    JpegWork = Alloc(JpegWorkLen);
    JpegBufLen = Stream.MaxFrameSize + JPEG_DHT_SIZE + 1024;
    JpegBuf = (UCHAR *)Alloc(JpegBufLen);
    /* the driver writes frames straight into Raw, whose pages are locked for
       that: a large picture needs a larger working set than the default */
    {
        typedef BOOL (WINAPI *SWS_FN)(HANDLE, DWORD, DWORD);
        SWS_FN sws = (SWS_FN)GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetProcessWorkingSetSize");
        if (sws != NULL) {
            ULONG mb = need + 2 * dib + JpegBufLen + JpegWorkLen;
            sws(GetCurrentProcess(), mb + 2UL * 1024 * 1024, 2 * mb + 8UL * 1024 * 1024);
        }
    }
    return Raw && Dib && Show && JpegWork && JpegBuf;
}

/* one frame: FALSE when the camera is gone (or stopped); *got = a new frame
   came; other failures (memory) leave LastErr set */
static BOOL NextFrame(ULONG *seq, ULONG timeout, BOOL *got)
{
    USBNT_VID_GET g;
    DWORD n = 0;
    *got = FALSE;
    LastErr = 0;
    g.Id = CamId;
    g.LastSequence = *seq;
    g.TimeoutMs = timeout;
    if (!DeviceIoControl(Dev, IOCTL_USBNT_VIDEO_FRAME, &g, sizeof(g), Raw, RawSize, &n, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_DEV_NOT_EXIST || e == ERROR_BAD_UNIT || e == ERROR_NOT_READY ||
            e == ERROR_GEN_FAILURE || e == ERROR_BAD_COMMAND || e == ERROR_INVALID_HANDLE) {
            return FALSE;
        }
        LastErr = e ? e : 1;
        return TRUE;
    }
    if (n >= sizeof(USBNT_VID_FRAME_HDR)) {
        *seq = ((USBNT_VID_FRAME_HDR *)Raw)->Sequence;
        *got = TRUE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Windows' cameras: Video for Windows                                 */
/* ------------------------------------------------------------------ */
#define MAX_VFW         10
typedef HWND (VFWAPI *P_CAPCREATE)(LPCSTR, DWORD, int, int, int, int, HWND, int);
typedef BOOL (VFWAPI *P_CAPDESC)(UINT, LPSTR, int, LPSTR, int);
static P_CAPCREATE CapCreate;
static P_CAPDESC CapDesc;
                                        /* NVfw: VfW drivers with a camera, listed after USBNT's cameras */
static UINT VfwDrv[MAX_VFW];
static char VfwName[MAX_VFW][80];
static char VfwFmtText[MAX_VFW][40];
static HWND VfwWnd;                     /* its capture window (made by the thread that grabs) */
static BITMAPINFOHEADER VfwHdr;
static UCHAR *VfwWant;                  /* format chosen in the driver's dialog, set at the start */
static ULONG VfwWantLen;
static volatile LONG VfwGot, VfwDlgReq;
static ULONG VfwSeq, VfwFails;
static HANDLE VfwStartEv;
static volatile LONG VfwStartOk;

static BOOL IsVfwCam(int i) { return i >= (int)Cams.Count && i < (int)Cams.Count + NVfw; }

static BOOL Has(const char *s, const char *part)
{
    int n = lstrlenA(part), i;
    for (; *s; s++) {
        for (i = 0; i < n && s[i] == part[i]; i++) ;
        if (i == n) return TRUE;
    }
    return FALSE;
}

/* avicap32 (Windows 2000 and later; NT 3.x has none, NT 4.0 has no Windows camera drivers) */
static BOOL LoadVfw(void)
{
    static int tried;
    HMODULE m;
    if (tried) return CapCreate != NULL;
    tried = 1;
    if ((GetVersion() & 0x80000000) || (GetVersion() & 0xFF) < 5) return FALSE;
    m = LoadLibraryA("avicap32.dll");
    if (m == NULL) return FALSE;
    CapCreate = (P_CAPCREATE)GetProcAddress(m, "capCreateCaptureWindowA");
    CapDesc = (P_CAPDESC)GetProcAddress(m, "capGetDriverDescriptionA");
    if (CapCreate == NULL || CapDesc == NULL) CapCreate = NULL;
    return CapCreate != NULL;
}

static ULONG VfwFourCC(const BITMAPINFOHEADER *b)
{
    ULONG c = b->biCompression;
    if (c == BI_RGB) return b->biBitCount == 24 ? USBNT_FCC_RGB24 : b->biBitCount == 32 ? FCC_RGB32 : 0;
    if (c == USBNT_FOURCC('Y', 'U', 'Y', 'V') || c == USBNT_FOURCC('Y', 'U', 'N', 'V')) return USBNT_FCC_YUY2;
    if (c == USBNT_FOURCC('I', 'Y', 'U', 'V')) return USBNT_FCC_I420;
    if (c == USBNT_FOURCC('G', 'R', 'E', 'Y') || c == USBNT_FOURCC('Y', '8', ' ', ' ')) return USBNT_FCC_GREY;
    if (c == USBNT_FOURCC('y', 'u', 'y', '2')) return USBNT_FCC_YUY2;
    if (c == USBNT_FOURCC('m', 'j', 'p', 'g')) return USBNT_FCC_MJPG;
    return c;                           /* YUY2, UYVY, NV12, I420, MJPG, Y800 as they are */
}

/* the format of a connected capture window */
static BOOL VfwFormat(HWND w, BITMAPINFOHEADER *b, UCHAR **all, ULONG *len)
{
    DWORD n = capGetVideoFormatSize(w);
    UCHAR *f;
    if (n < sizeof(BITMAPINFOHEADER)) return FALSE;
    f = (UCHAR *)Alloc(n + 64);
    if (f == NULL) return FALSE;
    if (!capGetVideoFormat(w, f, n)) { Free(f); return FALSE; }
    Mc(b, f, sizeof(*b));
    if (all) { *all = f; *len = n; } else Free(f);
    return TRUE;
}

static HWND VfwConnect(int v)
{
    HWND w = CapCreate("usbcam", WS_POPUP, 0, 0, 16, 16, NULL, 0);
    if (w == NULL) return NULL;
    if (!capDriverConnect(w, VfwDrv[v])) { DestroyWindow(w); return NULL; }
    return w;
}

static void VfwDisconnect(HWND w)
{
    capSetCallbackOnFrame(w, NULL);
    capDriverDisconnect(w);
    DestroyWindow(w);
}

/* the VfW drivers that have a camera now (connecting tells) */
static void VfwList(void)
{
    UINT i;
    NVfw = 0;
    if (!LoadVfw()) return;
    for (i = 0; i < 10 && NVfw < MAX_VFW; i++) {
        char name[80], ver[80];
        HWND w;
        BITMAPINFOHEADER b;
        name[0] = ver[0] = 0;
        if (!CapDesc(i, name, sizeof(name), ver, sizeof(ver)) || !name[0]) continue;
        if (Has(name, "USBNT")) continue;               /* usbntcap.dll: its cameras are usbnt.sys's */
        VfwDrv[NVfw] = i;
        w = VfwConnect(NVfw);
        if (w == NULL) continue;
        Cpyn(VfwName[NVfw], name, sizeof(VfwName[0]));
        VfwFmtText[NVfw][0] = 0;
        if (VfwFormat(w, &b, NULL, NULL)) {
            char cc[5];
            Fcc(cc, VfwFourCC(&b));
            wsprintfA(VfwFmtText[NVfw], "%s %dx%d", cc, (int)b.biWidth, (int)(b.biHeight < 0 ? -b.biHeight : b.biHeight));
        }
        VfwDisconnect(w);
        NVfw++;
    }
}

static LRESULT CALLBACK VfwFrame(HWND w, LPVIDEOHDR vh)
{
    USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
    ULONG n, room;
    (void)w;
    if (Raw == NULL || vh == NULL || vh->lpData == NULL) return TRUE;
    room = RawSize - sizeof(*h);
    n = vh->dwBytesUsed ? vh->dwBytesUsed : VfwHdr.biSizeImage;
    if (n > room) n = room;
    if (n > vh->dwBufferLength && vh->dwBufferLength) n = vh->dwBufferLength;
    Mz(h, sizeof(*h));
    h->Sequence = ++VfwSeq;
    h->Bytes = n;
    h->FourCC = Stream.FourCC;
    h->Width = Stream.Width;
    h->Height = Stream.Height;
    h->TimeMs = GetTickCount();
    h->Flags = VfwHdr.biHeight < 0 ? HDR_TOPDOWN : 0;
    Mc(Raw + sizeof(*h), vh->lpData, n);
    VfwGot = 1;
    return TRUE;
}

/* connects driver v in this thread and sets the stream up like StartCam */
static BOOL VfwOpen(int v)
{
    CAPTUREPARMS cp;
    VfwWnd = VfwConnect(v);
    if (VfwWnd == NULL) return FALSE;
    if (VfwWant != NULL) capSetVideoFormat(VfwWnd, VfwWant, VfwWantLen);
    if (!VfwFormat(VfwWnd, &VfwHdr, NULL, NULL)) { VfwDisconnect(VfwWnd); VfwWnd = NULL; return FALSE; }
    Mz(&Stream, sizeof(Stream));
    Stream.FourCC = VfwFourCC(&VfwHdr);
    Stream.Width = (USHORT)VfwHdr.biWidth;
    Stream.Height = (USHORT)(VfwHdr.biHeight < 0 ? -VfwHdr.biHeight : VfwHdr.biHeight);
    Stream.MaxFrameSize = (ULONG)Stream.Width * Stream.Height * 4;
    if (VfwHdr.biSizeImage > Stream.MaxFrameSize) Stream.MaxFrameSize = VfwHdr.biSizeImage;
    Mz(&cp, sizeof(cp));
    if (capCaptureGetSetup(VfwWnd, &cp, sizeof(cp)) && cp.dwRequestMicroSecPerFrame) {
        Stream.Interval = cp.dwRequestMicroSecPerFrame * 10;
    }
    Vfw = v;
    CamId = 0;
    VfwSeq = VfwFails = 0;
    capSetCallbackOnFrame(VfwWnd, VfwFrame);
    if (!CanShow(Stream.FourCC) || !Buffers()) {
        VfwDisconnect(VfwWnd);
        VfwWnd = NULL;
        Vfw = -1;
        return FALSE;
    }
    return TRUE;
}

static void VfwClose(void)
{
    if (VfwWnd != NULL) VfwDisconnect(VfwWnd);
    VfwWnd = NULL;
    Vfw = -1;
}

/* NextFrame for a VfW camera: one frame grabbed (in the thread of the capture window) */
static BOOL VfwNextFrame(ULONG *seq, BOOL *got)
{
    MSG m;
    *got = FALSE;
    LastErr = 0;
    if (VfwDlgReq) {
        VfwDlgReq = 0;
        capDlgVideoSource(VfwWnd);
    }
    VfwGot = 0;
    if (!capGrabFrameNoStop(VfwWnd)) {
        Sleep(20);
        return ++VfwFails < 50;         /* the camera is gone */
    }
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    if (VfwGot) {
        VfwFails = 0;
        *seq = ((USBNT_VID_FRAME_HDR *)Raw)->Sequence;
        *got = TRUE;
    } else if (++VfwFails >= 50) {
        return FALSE;
    } else {
        Sleep(10);
    }
    return TRUE;
}

/* the driver's own dialogs while nothing streams: Format = 1, Source = 2 */
static void VfwDialog(int v, int which)
{
    HWND w = VfwConnect(v);
    BITMAPINFOHEADER b;
    if (w == NULL) { StatusSet("The camera does not answer."); return; }
    if (which == 1) {
        if (capDlgVideoFormat(w)) {
            Free(VfwWant);
            VfwWant = NULL;
            if (VfwFormat(w, &b, &VfwWant, &VfwWantLen)) {
                char cc[5];
                Fcc(cc, VfwFourCC(&b));
                wsprintfA(VfwFmtText[v], "%s %dx%d", cc, (int)b.biWidth, (int)(b.biHeight < 0 ? -b.biHeight : b.biHeight));
            }
        }
    } else {
        capDlgVideoSource(w);
    }
    VfwDisconnect(w);
}

/* ------------------------------------------------------------------ */
/* capture thread                                                      */
/* ------------------------------------------------------------------ */
static BOOL AnyNextFrame(ULONG *seq, ULONG timeout, BOOL *got)
{
    if (Vfw >= 0) return VfwNextFrame(seq, got);
    return NextFrame(seq, timeout, got);
}

static DWORD WINAPI Capture(LPVOID p)
{
    ULONG seq = 0, t0 = GetTickCount(), cnt = 0, fails = 0, lastDec = 0, decMs = 0, memFails = 0;
    if (p != NULL) {
        /* a Windows camera: its capture window belongs to this thread */
        VfwStartOk = VfwOpen((int)(ULONG_PTR)p - 1);
        SetEvent(VfwStartEv);
        if (!VfwStartOk) return 0;
    }
    while (CapRun) {
        BOOL got;
        USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
        const UCHAR *d = Raw + sizeof(*h);
        BOOL ok;
        if (!AnyNextFrame(&seq, 500, &got)) {
            if (++fails > 3) {
                StatusSet("The camera was unplugged or stopped sending pictures.");
                break;
            }
            Sleep(100);
            continue;
        }
        fails = 0;
        if (LastErr) {
            /* e.g. no memory to lock the frame buffer: say so, do not spin */
            char m[120];
            if (++memFails == 20) {
                wsprintfA(m, "Windows cannot take the pictures (error %u): try a smaller picture size.", LastErr);
                StatusSet(m);
            }
            Sleep(50);
            continue;
        }
        memFails = 0;
        if (RecReq == 1) {
            char path[MAX_PATH];
            RecReq = 0;
            FileName(path, "AVI", 0);
            Cpyn(AviBase, path, lstrlenA(path) - 3);           /* without ".AVI" */
            AviPart = 1;
            StRecFrames = 0;
            if (!AviBegin(path)) StatusSet("Cannot create the video file.");
        } else if (RecReq == 2) {
            RecReq = 0;
            AviEnd();
        }
        if (!got) continue;
        StFrames++;
        if (h->Flags & 1) StErrors++;
        /* the preview may skip frames when decoding is slow (at most half the
           time goes to it); a recording of converted frames needs every one */
        if (PhotoReq || (AviFile != INVALID_HANDLE_VALUE && AviCompression() == 0) ||
            GetTickCount() - lastDec >= decMs) {
            ULONG t = GetTickCount();
            ok = Decode(h, d);
            decMs = GetTickCount() - t;
            lastDec = GetTickCount();
        } else {
            ok = FALSE;
        }
        AviAdd(h, d, ok);
        if (PhotoReq && ok) {
            char path[MAX_PATH], msg[MAX_PATH + 40];
            BOOL saved;
            PhotoReq = 0;
            FileName(path, "BMP", 0);
            saved = SaveBmp(path, Dib, DecW, DecH);
            if (saved && h->FourCC == USBNT_FCC_MJPG && JpegToo) {
                FileName(path, "JPG", 0);
                saved = SaveJpeg(path, d, h->Bytes);
            }
            lstrcpyA(LastFile, path);
            wsprintfA(msg, saved ? "Saved %s" : "Cannot write %s", path);
            StatusSet(msg);
        }
        if (ok) {
            ToShow();
            /* one picture message at a time: posted messages come before
               mouse, keyboard and timer input, so posting every frame while
               painting is slower than the camera (large uncompressed
               pictures, a slow display) froze the window */
            if (InterlockedExchange((LONG *)&FramePending, 1) == 0) {
                PostMessageA(MainWnd, WM_CAM_FRAME, 0, 0);
            }
        }
        cnt++;
        if (GetTickCount() - t0 >= 1000) {
            StFps = cnt * 1000 / (GetTickCount() - t0);
            cnt = 0;
            t0 = GetTickCount();
        }
    }
    AviEnd();
    if (Vfw >= 0) VfwClose();
    PostMessageA(MainWnd, WM_CAM_STOPPED, 0, 0);
    return 0;
}

/* ------------------------------------------------------------------ */
/* window                                                              */
/* ------------------------------------------------------------------ */
static void FillFormats(void)
{
    int f, r;
    USBNT_CAMERA *c;
    SendMessageA(FmtBox, CB_RESETCONTENT, 0, 0);
    NFmtItems = 0;
    if (CurCam < 0) return;
    if (IsVfwCam(CurCam)) {
        /* Video for Windows lists no formats: the current one, or the driver's dialog */
        char t[80];
        wsprintfA(t, "%s", VfwFmtText[CurCam - Cams.Count][0] ? VfwFmtText[CurCam - Cams.Count] : "Current format");
        SendMessageA(FmtBox, CB_ADDSTRING, 0, (LPARAM)t);
        FmtItems[0].Fmt = -1;
        SendMessageA(FmtBox, CB_ADDSTRING, 0, (LPARAM)"Other format... (the driver's dialog)");
        FmtItems[1].Fmt = -2;
        NFmtItems = 2;
        SendMessageA(FmtBox, CB_SETCURSEL, 0, 0);
        return;
    }
    c = &Cams.Cam[CurCam];
    for (f = 0; f < c->NumFormats; f++) {
        if (!CanShow(c->Format[f].FourCC)) continue;      /* H.264 and others */
        for (r = 0; r < c->Format[f].NumFrames; r++) {
            USBNT_VID_FRAME *fr = &c->Format[f].Frame[r];
            char s[80], cc[5];
            ULONG iv = fr->Interval[0] ? fr->Interval[0] : 333333;
            Fcc(cc, c->Format[f].FourCC);
            wsprintfA(s, "%s %ux%u  %u fps", cc, fr->Width, fr->Height, (10000000UL + iv / 2) / iv);
            SendMessageA(FmtBox, CB_ADDSTRING, 0, (LPARAM)s);
            FmtItems[NFmtItems].Fmt = f;
            FmtItems[NFmtItems].Frame = r;
            NFmtItems++;
        }
    }
    /* default: MJPEG 640x480 when there is one, else the first */
    for (r = 0; r < NFmtItems; r++) {
        USBNT_VID_FORMAT *F = &c->Format[FmtItems[r].Fmt];
        if (F->FourCC == USBNT_FCC_MJPG && F->Frame[FmtItems[r].Frame].Width == 640) break;
    }
    SendMessageA(FmtBox, CB_SETCURSEL, r < NFmtItems ? r : 0, 0);
    if (NFmtItems == 0 && c->NumFormats) StatusSet("This camera only sends formats this program cannot show.");
}

static void SetupControl(HWND sb, HWND lbl, ULONG sel)
{
    USBNT_VID_CONTROL c;
    BOOL ok = CurCam >= 0 && !IsVfwCam(CurCam) && (Cams.Cam[CurCam].Controls & (1UL << sel)) &&
              Control(sel, FALSE, 0, &c) && c.Max > c.Min;
    EnableWindow(sb, ok);
    EnableWindow(lbl, ok);
    if (ok) {
        SetScrollRange(sb, SB_CTL, (int)c.Min, (int)c.Max, FALSE);
        SetScrollPos(sb, SB_CTL, (int)c.Value, TRUE);
    }
}

static void FillCams(void)
{
    ULONG i;
    ULONG keep = (CurCam >= 0 && !IsVfwCam(CurCam)) ? Cams.Cam[CurCam].Id : 0;
    int keepVfw = IsVfwCam(CurCam) ? (int)VfwDrv[CurCam - Cams.Count] : -1;
    int sel = -1, total;
    if (Dev != INVALID_HANDLE_VALUE) ListCams(); else Mz(&Cams, sizeof(Cams));
    VfwList();
    total = (int)Cams.Count + NVfw;
    SendMessageA(CamBox, CB_RESETCONTENT, 0, 0);
    for (i = 0; i < Cams.Count; i++) {
        char s[80];
        wsprintfA(s, "%u: %s", i + 1, Cams.Cam[i].Name);
        SendMessageA(CamBox, CB_ADDSTRING, 0, (LPARAM)s);
        if (Cams.Cam[i].Id == keep) sel = (int)i;
    }
    for (i = 0; i < (ULONG)NVfw; i++) {
        char s[120];
        wsprintfA(s, "%u: Windows camera (%s)", Cams.Count + i + 1, VfwName[i]);
        SendMessageA(CamBox, CB_ADDSTRING, 0, (LPARAM)s);
        if ((int)VfwDrv[i] == keepVfw) sel = (int)(Cams.Count + i);
    }
    if (sel < 0 && total) sel = 0;
    CurCam = sel;
    SendMessageA(CamBox, CB_SETCURSEL, sel, 0);
    FillFormats();
    SetupControl(BrightSb, BrightLbl, USBNT_VCTL_BRIGHTNESS);
    SetupControl(ContrastSb, ContrastLbl, USBNT_VCTL_CONTRAST);
    EnableWindow(SourceBtn, IsVfwCam(CurCam));
    EnableWindow(StartBtn, total != 0 || CapRun);
    if (!total && !CapRun) StatusSet("No USB camera is connected.");
}

static void UpdateButtons(void)
{
    SetWindowTextA(StartBtn, CapRun ? "Stop" : "Start");
    EnableWindow(PhotoBtn, CapRun != 0);
    EnableWindow(RecBtn, CapRun != 0);
    SetWindowTextA(RecBtn, AviFile != INVALID_HANDLE_VALUE || RecReq == 1 ? "Stop recording" : "Record video");
    EnableWindow(CamBox, !CapRun);
    EnableWindow(FmtBox, !CapRun);
    EnableWindow(JpegChk, CapRun && Stream.FourCC == USBNT_FCC_MJPG);
}

static void StopCapture(void)
{
    if (!CapRun && CapThread == NULL) return;
    CapRun = 0;
    if (CapThread) {
        /* the thread waits at most 500 ms for a frame; stopping the stream
           ends a wait at once */
        if (WaitForSingleObject(CapThread, 2000) != WAIT_OBJECT_0) {
            StopCam();
            if (WaitForSingleObject(CapThread, 8000) != WAIT_OBJECT_0) {
                VfwWnd = NULL;
                /* never free buffers a running thread uses: forget them */
                TerminateThread(CapThread, 1);
                Raw = Dib = JpegBuf = NULL;
                JpegWork = NULL;
                DibCap = 0;
                if (AviFile != INVALID_HANDLE_VALUE) { CloseHandle(AviFile); AviFile = INVALID_HANDLE_VALUE; }
            }
        }
        CloseHandle(CapThread);
        CapThread = NULL;
    }
    StopCam();
    CamId = 0;
    Vfw = -1;
}

static void StartCapture(void)
{
    int i = (int)SendMessageA(FmtBox, CB_GETCURSEL, 0, 0);
    DWORD tid;
    char cc[5], msg[120];
    if (CurCam < 0 || i < 0 || i >= NFmtItems || CapThread != NULL) return;
    if (IsVfwCam(CurCam)) {
        int v = CurCam - (int)Cams.Count;
        if (FmtItems[i].Fmt == -2) {
            VfwDialog(v, 1);                /* then it starts with what was chosen there */
            FillFormats();
        }
        LastErr = 0;
        FramePending = 0;
        StFrames = StErrors = StFps = StRecFrames = StDecodeErr = 0;
        if (VfwStartEv == NULL) VfwStartEv = CreateEventA(NULL, TRUE, FALSE, NULL);
        ResetEvent(VfwStartEv);
        VfwStartOk = 0;
        CapRun = 1;
        CapThread = CreateThread(NULL, 0, Capture, (LPVOID)(ULONG_PTR)(v + 1), 0, &tid);
        if (CapThread == NULL || WaitForSingleObject(VfwStartEv, 30000) != WAIT_OBJECT_0 || !VfwStartOk) {
            CapRun = 0;
            if (CapThread != NULL) {
                WaitForSingleObject(CapThread, 10000);
                CloseHandle(CapThread);
                CapThread = NULL;
            }
            StatusSet("The Windows camera did not start (or sends a format this program cannot show).");
            return;
        }
        Fcc(cc, Stream.FourCC);
        wsprintfA(msg, "%s %ux%u (Windows' camera driver)", cc, Stream.Width, Stream.Height);
        StatusSet(msg);
        return;
    }
    if (!StartCam(Cams.Cam[CurCam].Id, FmtItems[i].Fmt, FmtItems[i].Frame)) {
        StatusSet(Stream.Result == -6 ? "The camera did not start: not enough memory for this picture size." :
                  "The camera did not start (no bandwidth left on this USB controller?).");
        CamId = 0;
        return;
    }
    if (!Buffers()) {
        StopCam();
        CamId = 0;
        StatusSet("Not enough memory for this picture size.");
        return;
    }
    LastErr = 0;
    FramePending = 0;
    StFrames = StErrors = StFps = StRecFrames = StDecodeErr = 0;
    Fcc(cc, Stream.FourCC);
    wsprintfA(msg, "%s %ux%u", cc, Stream.Width, Stream.Height);
    StatusSet(msg);
    CapRun = 1;
    CapThread = CreateThread(NULL, 0, Capture, NULL, 0, &tid);
    if (CapThread == NULL) {
        CapRun = 0;
        StopCam();
        CamId = 0;
    }
}

static void PickFolder(void)
{
    OPENFILENAMEA ofn;
    char name[MAX_PATH];
    int i;
    lstrcpyA(name, "CAMERA.BMP");
    Mz(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = MainWnd;
    ofn.lpstrFilter = "Pictures (*.BMP)\0*.BMP\0All files\0*.*\0";
    ofn.lpstrFile = name;
    ofn.nMaxFile = sizeof(name);
    ofn.lpstrInitialDir = Folder;
    ofn.lpstrTitle = "Folder for photos and videos (the file name is not used)";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOREADONLYRETURN;
    if (!GetSaveFileNameA(&ofn)) return;
    for (i = lstrlenA(name); i > 0 && name[i - 1] != '\\' && name[i - 1] != ':'; i--) ;
    if (i > 0 && name[i - 1] == '\\') i--;
    name[i] = 0;
    if (i == 2 && name[1] == ':') lstrcatA(name, "\\");
    lstrcpyA(Folder, name);
    if (lstrlenA(Folder) > 3 && Folder[lstrlenA(Folder) - 1] == '\\') Folder[lstrlenA(Folder) - 1] = 0;
    {
        char msg[MAX_PATH + 20];
        wsprintfA(msg, "Saving to %s", Folder);
        StatusSet(msg);
    }
}

static void Layout(int w, int h)
{
    (void)h;
    MoveWindow(StatusWnd, 8, 100, w - 16, 20, TRUE);
}

static HWND Make(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, MainWnd, (HMENU)id, Inst, NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)Font, 0);
    return c;
}

static void Paint(HWND wnd)
{
    PAINTSTRUCT ps;
    RECT rc;
    HDC dc = BeginPaint(wnd, &ps);
    int aw, ah, dw, dh, x, y;
    GetClientRect(wnd, &rc);
    aw = rc.right - 16;
    ah = rc.bottom - TOP_H - 8;
    EnterCriticalSection(&Lock);
    if (ShowValid && aw > 0 && ah > 0) {
        BITMAPINFO bi;
        Mz(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = ShowW;
        bi.bmiHeader.biHeight = ShowH;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 24;
        bi.bmiHeader.biCompression = BI_RGB;
        /* keep the aspect ratio */
        dw = aw;
        dh = MulDiv(aw, ShowH, ShowW);
        if (dh > ah) { dh = ah; dw = MulDiv(ah, ShowW, ShowH); }
        x = 8 + (aw - dw) / 2;
        y = TOP_H + (ah - dh) / 2;
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchDIBits(dc, x, y, dw, dh, 0, 0, ShowW, ShowH, Show, &bi, DIB_RGB_COLORS, SRCCOPY);
        /* black around the picture */
        PatBlt(dc, 8, TOP_H, aw, y - TOP_H, BLACKNESS);
        PatBlt(dc, 8, y + dh, aw, TOP_H + ah - y - dh, BLACKNESS);
        PatBlt(dc, 8, y, x - 8, dh, BLACKNESS);
        PatBlt(dc, x + dw, y, 8 + aw - x - dw, dh, BLACKNESS);
    } else if (aw > 0 && ah > 0) {
        PatBlt(dc, 8, TOP_H, aw, ah, BLACKNESS);
    }
    LeaveCriticalSection(&Lock);
    EndPaint(wnd, &ps);
    FramePending = 0;                   /* the capture thread may announce the next one */
}

static void ShowStatus(void)
{
    char s[300];
    if (CapRun) {
        char cc[5];
        Fcc(cc, Stream.FourCC);
        wsprintfA(s, "%s %ux%u, %u fps, %u frames%s", cc, Stream.Width, Stream.Height, StFps, StFrames,
                  AviFile != INVALID_HANDLE_VALUE ? ", recording" : "");
        if (AviFile != INVALID_HANDLE_VALUE) wsprintfA(s + lstrlenA(s), " %u frames", StRecFrames);
        if (StErrors || StDecodeErr) wsprintfA(s + lstrlenA(s), ", %u damaged", StErrors + StDecodeErr);
        if (StMsg[0]) wsprintfA(s + lstrlenA(s), " - %s", StMsg);
    } else {
        lstrcpyA(s, StMsg);
    }
    SetWindowTextA(StatusWnd, s);
}

static void Scroll(HWND sb, int code, int pos)
{
    int mn, mx, cur = GetScrollPos(sb, SB_CTL), step;
    ULONG sel = sb == BrightSb ? USBNT_VCTL_BRIGHTNESS : USBNT_VCTL_CONTRAST;
    GetScrollRange(sb, SB_CTL, &mn, &mx);
    step = (mx - mn) / 16 ? (mx - mn) / 16 : 1;
    switch (code) {
    case SB_LINELEFT: cur -= 1; break;
    case SB_LINERIGHT: cur += 1; break;
    case SB_PAGELEFT: cur -= step; break;
    case SB_PAGERIGHT: cur += step; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: cur = pos; break;
    case SB_LEFT: cur = mn; break;
    case SB_RIGHT: cur = mx; break;
    default: return;
    }
    if (cur < mn) cur = mn;
    if (cur > mx) cur = mx;
    SetScrollPos(sb, SB_CTL, cur, TRUE);
    Control(sel, TRUE, cur, NULL);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        MainWnd = wnd;
        Make("STATIC", "Camera:", SS_LEFT, 8, 11, 52, 18, -1);
        CamBox = Make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 62, 8, 250, 160, ID_CAM);
        Make("STATIC", "Picture:", SS_LEFT, 324, 11, 52, 18, -1);
        FmtBox = Make("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 378, 8, 200, 260, ID_FMT);
        StartBtn = Make("BUTTON", "Start", BS_PUSHBUTTON | WS_TABSTOP, 8, 38, 80, 24, ID_START);
        PhotoBtn = Make("BUTTON", "Photo", BS_PUSHBUTTON | WS_TABSTOP, 96, 38, 80, 24, ID_PHOTO);
        RecBtn = Make("BUTTON", "Record video", BS_PUSHBUTTON | WS_TABSTOP, 184, 38, 110, 24, ID_RECORD);
        FolderBtn = Make("BUTTON", "Folder...", BS_PUSHBUTTON | WS_TABSTOP, 302, 38, 80, 24, ID_FOLDER);
        JpegChk = Make("BUTTON", "Photos also as JPEG", BS_AUTOCHECKBOX | WS_TABSTOP, 392, 40, 170, 20, ID_JPEG);
        BrightLbl = Make("STATIC", "Brightness:", SS_LEFT, 8, 73, 70, 18, -1);
        BrightSb = Make("SCROLLBAR", "", SBS_HORZ, 80, 72, 180, 16, ID_BRIGHT);
        ContrastLbl = Make("STATIC", "Contrast:", SS_LEFT, 276, 73, 60, 18, -1);
        ContrastSb = Make("SCROLLBAR", "", SBS_HORZ, 338, 72, 180, 16, ID_CONTRAST);
        SourceBtn = Make("BUTTON", "Camera settings...", BS_PUSHBUTTON | WS_TABSTOP, 530, 68, 130, 24, ID_SOURCE);
        StatusWnd = Make("STATIC", "", SS_LEFT | SS_NOPREFIX, 8, 100, 560, 20, -1);
        SendMessageA(JpegChk, BM_SETCHECK, JpegToo ? 1 : 0, 0);
        FillCams();
        UpdateButtons();
        ShowStatus();
        return 0;
    case WM_SIZE:
        Layout(LOWORD(lp), HIWORD(lp));
        InvalidateRect(wnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
        Paint(wnd);
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(wnd, &rc);
        rc.bottom = TOP_H;
        FillRect((HDC)wp, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }
    case WM_CAM_FRAME:
        if (wp == 0 && IsIconic(wnd)) {
            FramePending = 0;
        } else if (wp == 0) {
            RECT rc;
            GetClientRect(wnd, &rc);
            rc.top = TOP_H;
            InvalidateRect(wnd, &rc, FALSE);
        }
        {
            static ULONG last;
            if (wp || GetTickCount() - last > 500) {
                last = GetTickCount();
                ShowStatus();
                UpdateButtons();
            }
        }
        return 0;
    case WM_CAM_STOPPED:
        StopCapture();
        UpdateButtons();
        ShowStatus();
        FillCams();
        return 0;
    case WM_CAM_CHANGED:
        if (!CapRun) FillCams();
        return 0;
    case WM_DEVICECHANGE:
        /* Windows' cameras come and go without usbnt.sys: look again when it has settled */
        if (LoadVfw()) SetTimer(wnd, 79, 2000, NULL);
        return TRUE;
    case WM_HSCROLL:
        if ((HWND)lp == BrightSb || (HWND)lp == ContrastSb) Scroll((HWND)lp, LOWORD(wp), (short)HIWORD(wp));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CAM:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                CurCam = (int)SendMessageA(CamBox, CB_GETCURSEL, 0, 0);
                EnableWindow(SourceBtn, IsVfwCam(CurCam));
                FillFormats();
                SetupControl(BrightSb, BrightLbl, USBNT_VCTL_BRIGHTNESS);
                SetupControl(ContrastSb, ContrastLbl, USBNT_VCTL_CONTRAST);
            }
            break;
        case ID_START:
            if (CapRun) StopCapture();
            else StartCapture();
            StMsg[0] = 0;
            UpdateButtons();
            ShowStatus();
            break;
        case ID_PHOTO:
            PhotoReq = 1;
            break;
        case ID_RECORD:
            RecReq = (AviFile != INVALID_HANDLE_VALUE) ? 2 : 1;
            UpdateButtons();
            if (RecReq == 2) {
                char m[MAX_PATH + 20];
                wsprintfA(m, "Saved %s", LastFile);
                Cpyn(StMsg, m, sizeof(StMsg));
            }
            break;
        case ID_FOLDER:
            PickFolder();
            break;
        case ID_SOURCE:
            /* the settings dialog of the VfW driver (brightness, contrast, the device) */
            if (CapRun && Vfw >= 0) VfwDlgReq = 1;
            else if (IsVfwCam(CurCam)) VfwDialog(CurCam - (int)Cams.Count, 2);
            break;
        case ID_JPEG:
            JpegToo = SendMessageA(JpegChk, BM_GETCHECK, 0, 0) != 0;
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == 79) {
            KillTimer(wnd, 79);
            if (!CapRun) FillCams();
        } else if (wp == 77) {              /* /gui: photo */
            KillTimer(wnd, 77);
            PhotoReq = 1;
            SetTimer(wnd, 78, 2000, NULL);
        } else if (wp == 78) {              /* /gui: stop, log, close */
            char line[400], cc[5];
            HANDLE f;
            ULONG frames = StFrames, dmg = StErrors + StDecodeErr, t0 = GetTickCount();
            int w = ShowW, h = ShowH;
            KillTimer(wnd, 78);
            Fcc(cc, Stream.FourCC);
            StopCapture();
            wsprintfA(line, "GUI item=%d %s %ux%u frames=%u damaged=%u picture=%dx%d photo=%s stop_ms=%u jpeg=%d,%d\r\n",
                      GuiItem, cc, Stream.Width, Stream.Height, frames, dmg, ShowValid ? w : 0, ShowValid ? h : 0,
                      PhotoReq ? "none" : LastFile, GetTickCount() - t0, (int)JpegToo,
                      (int)SendMessageA(JpegChk, BM_GETCHECK, 0, 0));
            f = CreateFileA(GuiLog, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f != INVALID_HANDLE_VALUE) {
                SetFilePointer(f, 0, NULL, FILE_END);
                WriteAll(f, line, lstrlenA(line));
                CloseHandle(f);
            }
            DestroyWindow(wnd);
        }
        return 0;
    case WM_DESTROY:
        StopCapture();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

static DWORD WINAPI ChangeThread(LPVOID p)
{
    HANDLE h = CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    ULONG count = 0xFFFFFFFF;
    (void)p;
    if (h == INVALID_HANDLE_VALUE) return 1;
    for (;;) {
        ULONG now = count;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_USBNT_WAIT_CHANGE, &now, sizeof(now), &now, sizeof(now), &got, NULL)) {
            Sleep(2000);
            continue;
        }
        if (now != count) {
            count = now;
            PostMessageA(MainWnd, WM_CAM_CHANGED, 0, 0);
        }
    }
}

/* ------------------------------------------------------------------ */
/* automatic use: /test LOG ...                                        */
/* ------------------------------------------------------------------ */
static HANDLE Log = INVALID_HANDLE_VALUE;

static void Out(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    lstrcatA(buf, "\r\n");
    if (Log != INVALID_HANDLE_VALUE) WriteAll(Log, buf, lstrlenA(buf));
}

static const char *Arg(const char *p, char *out, int size)
{
    int n = 0;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (n + 1 < size) out[n++] = *p; p++; }
        if (*p) p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') { if (n + 1 < size) out[n++] = *p; p++; }
    }
    out[n] = 0;
    return p;
}

static long Num(const char *s)
{
    long v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

static int Eq(const char *a, const char *b) { return lstrcmpiA(a, b) == 0; }

/* average colour and the 8 colour bars of the test camera */
static void Analyse(const char *what, int w, int h)
{
    ULONG sr = 0, sg = 0, sb = 0, st = Stride(w), n = 0;
    int x, y, b;
    char bars[200];
    for (y = 0; y < h; y += 2) {
        const UCHAR *p = Dib + (ULONG)y * st;
        for (x = 0; x < w; x += 2, n++) { sb += p[x * 3]; sg += p[x * 3 + 1]; sr += p[x * 3 + 2]; }
    }
    bars[0] = 0;
    for (b = 0; b < 8; b++) {
        const UCHAR *p = Dib + (ULONG)(h - 1 - h / 16) * st + (ULONG)(b * w / 8 + w / 16) * 3;
        wsprintfA(bars + lstrlenA(bars), " %u,%u,%u", p[2], p[1], p[0]);
    }
    Out("%s avgR=%u avgG=%u avgB=%u bars=%s", what, n ? sr / n : 0, n ? sg / n : 0, n ? sb / n : 0, bars);
}

/* every format and size in turn: start, a few frames, decode, stop */
static void AllModes(USBNT_CAMERA *c)
{
    int f, r;
    char cc[5];
    for (f = 0; f < c->NumFormats; f++) {
        for (r = 0; r < c->Format[f].NumFrames; r++) {
            ULONG seq = 0, k, n = 0, dec = 0, t0;
            BOOL got;
            Fcc(cc, c->Format[f].FourCC);
            if (!StartCam(c->Id, f, r)) {
                Out("MODE %u %u %s %ux%u start-failed result=%d", f, r, cc, c->Format[f].Frame[r].Width,
                    c->Format[f].Frame[r].Height, Stream.Result);
                continue;
            }
            if (!Buffers()) {
                Out("MODE %u %u %s no-memory", f, r, cc);
                StopCam();
                continue;
            }
            t0 = GetTickCount();
            DecW = DecH = 0;
            for (k = 0; k < 8 && GetTickCount() - t0 < 8000; k++) {
                USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
                if (!NextFrame(&seq, 3000, &got)) break;
                if (!got) continue;
                n++;
                if (Decode(h, Raw + sizeof(*h))) {
                    dec++;
                    ToShow();
                }
            }
            Out("MODE %u %u %s %ux%u stream=%ux%u max=%u frames=%u decoded=%u picture=%ux%u%s", f, r, cc,
                c->Format[f].Frame[r].Width, c->Format[f].Frame[r].Height, Stream.Width, Stream.Height,
                Stream.MaxFrameSize, n, dec, DecW, DecH, CanShow(c->Format[f].FourCC) ? "" : " (not offered)");
            StopCam();
        }
    }
}

static void TestMode(const char *cmd)
{
    char a[MAX_PATH], logname[MAX_PATH], snap[MAX_PATH], jpeg[MAX_PATH], rec[MAX_PATH];
    long cam = 1, fmt = -1, res = -1, secs = 0, bright = -9999, frames = 0;
    BOOL list = FALSE, all = FALSE;
    ULONG i, seq = 0;
    USBNT_CAMERA *c;
    char cc[5];

    snap[0] = jpeg[0] = rec[0] = logname[0] = 0;
    for (;;) {
        cmd = Arg(cmd, a, sizeof(a));
        if (!a[0]) break;
        if (Eq(a, "/test")) cmd = Arg(cmd, logname, sizeof(logname));
        else if (Eq(a, "/cam")) { cmd = Arg(cmd, a, sizeof(a)); cam = Num(a); }
        else if (Eq(a, "/fmt")) { cmd = Arg(cmd, a, sizeof(a)); fmt = Num(a); }
        else if (Eq(a, "/res")) { cmd = Arg(cmd, a, sizeof(a)); res = Num(a); }
        else if (Eq(a, "/frames")) { cmd = Arg(cmd, a, sizeof(a)); frames = Num(a); }
        else if (Eq(a, "/bright")) { cmd = Arg(cmd, a, sizeof(a)); bright = Num(a); }
        else if (Eq(a, "/list")) list = TRUE;
        else if (Eq(a, "/allmodes")) all = TRUE;
        else if (Eq(a, "/snap")) cmd = Arg(cmd, snap, sizeof(snap));
        else if (Eq(a, "/jpeg")) cmd = Arg(cmd, jpeg, sizeof(jpeg));
        else if (Eq(a, "/record")) { cmd = Arg(cmd, rec, sizeof(rec)); cmd = Arg(cmd, a, sizeof(a)); secs = Num(a); }
    }
    Log = CreateFileA(logname, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Dev == INVALID_HANDLE_VALUE) {
        Out("usbnt.sys is not running");
        Mz(&Cams, sizeof(Cams));
    } else if (!ListCams()) {
        Out("ERROR camera list failed (%u)", GetLastError());
        goto done;
    }
    VfwList();
    Out("CAMERAS %u", Cams.Count + NVfw);
    Out("USBNT %u WINDOWS %u", Cams.Count, NVfw);
    for (i = 0; list && i < (ULONG)NVfw; i++) {
        Out("CAM %u vfw driver=%u name=%s format=%s", Cams.Count + i + 1, VfwDrv[i], VfwName[i], VfwFmtText[i]);
    }
    for (i = 0; list && i < Cams.Count; i++) {
        int f, r;
        c = &Cams.Cam[i];
        Out("CAM %u id=%x name=%s formats=%u controls=%x uvc=%x", i + 1, c->Id, c->Name, c->NumFormats, c->Controls,
            c->UvcMinor);
        for (f = 0; f < c->NumFormats; f++) {
            char line[600];
            Fcc(cc, c->Format[f].FourCC);
            wsprintfA(line, "FMT %u %s", f, cc);
            for (r = 0; r < c->Format[f].NumFrames; r++)
                wsprintfA(line + lstrlenA(line), " %ux%u", c->Format[f].Frame[r].Width, c->Format[f].Frame[r].Height);
            Out("%s", line);
        }
    }
    if (!snap[0] && !jpeg[0] && !rec[0] && !frames && !all) goto done;
    if (cam < 1 || (ULONG)cam > Cams.Count + NVfw) { Out("ERROR no camera %d", cam); goto done; }
    if ((ULONG)cam > Cams.Count) {
        /* a Windows camera through Video for Windows: its current format */
        if (!VfwOpen((int)(cam - 1 - Cams.Count))) { Out("ERROR start failed (Windows camera)"); goto done; }
        Fcc(cc, Stream.FourCC);
        Out("START ok %s %ux%u interval=%u max=%u vfw", cc, Stream.Width, Stream.Height, Stream.Interval,
            Stream.MaxFrameSize);
        goto stream;
    }
    if (all) {
        AllModes(&Cams.Cam[cam - 1]);
        goto done;
    }
    CurCam = cam - 1;
    c = &Cams.Cam[CurCam];
    if (fmt < 0) {                                  /* MJPEG if there is one */
        for (fmt = 0; fmt < c->NumFormats && c->Format[fmt].FourCC != USBNT_FCC_MJPG; fmt++) ;
        if (fmt == c->NumFormats) fmt = 0;
    }
    if (fmt >= c->NumFormats) { Out("ERROR no format %d", fmt); goto done; }
    if (res < 0) res = c->Format[fmt].NumFrames - 1;
    if (res >= c->Format[fmt].NumFrames) { Out("ERROR no frame size %d", res); goto done; }
    if (!StartCam(c->Id, (int)fmt, (int)res)) { Out("ERROR start failed (%u)", GetLastError()); goto done; }
    if (!Buffers()) { Out("ERROR no memory"); goto stop; }
    Fcc(cc, Stream.FourCC);
    Out("START ok %s %ux%u interval=%u max=%u", cc, Stream.Width, Stream.Height, Stream.Interval, Stream.MaxFrameSize);
stream:
    if (bright != -9999 && Vfw < 0) {
        USBNT_VID_CONTROL v;
        BOOL ok = Control(USBNT_VCTL_BRIGHTNESS, TRUE, bright, &v);
        Out("BRIGHT %s value=%d", ok ? "ok" : "failed", ok ? (int)v.Value : 0);
        /* frames already on the way still have the old brightness */
        for (i = 0; i < 4; i++) { BOOL got; AnyNextFrame(&seq, 2000, &got); }
    }
    for (i = 0; (long)i < frames; i++) {
        BOOL got;
        USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
        if (!AnyNextFrame(&seq, 3000, &got) || !got) { Out("ERROR frame %u missing", i); break; }
        Out("FRAME seq=%u bytes=%u flags=%x decode=%d", h->Sequence, h->Bytes, h->Flags,
            Decode(h, Raw + sizeof(*h)) ? 1 : 0);
    }
    if (snap[0] || jpeg[0]) {
        BOOL got = FALSE, ok = FALSE;
        USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
        for (i = 0; i < 20 && !ok; i++) {
            if (!AnyNextFrame(&seq, 3000, &got) || !got) continue;
            ok = !(h->Flags & 1) && Decode(h, Raw + sizeof(*h));
        }
        if (!ok) Out("ERROR no usable frame");
        else {
            if (snap[0]) {
                BOOL w = SaveBmp(snap, Dib, DecW, DecH);
                Fcc(cc, h->FourCC);
                Out("SNAP %s %ux%u %s bytes=%u", w ? "ok" : "write-failed", DecW, DecH, cc, h->Bytes);
                Analyse("COLOR", DecW, DecH);
            }
            if (jpeg[0]) {
                if (h->FourCC != USBNT_FCC_MJPG) Out("JPEG skipped (not an MJPEG stream)");
                else Out("JPEG %s", SaveJpeg(jpeg, Raw + sizeof(*h), h->Bytes) ? "ok" : "write-failed");
            }
        }
    }
    if (rec[0] && secs > 0) {
        ULONG t0, n = 0, errs = 0;
        int k;
        lstrcpyA(Folder, ".");
        for (k = lstrlenA(rec); k > 0 && rec[k - 1] != '.'; k--) ;
        Cpyn(AviBase, rec, k > 0 ? k : lstrlenA(rec) + 1);
        AviPart = 1;
        if (!AviBegin(rec)) { Out("ERROR cannot create %s", rec); goto stop; }
        t0 = GetTickCount();
        while (GetTickCount() - t0 < (ULONG)secs * 1000) {
            BOOL got;
            USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)Raw;
            if (!AnyNextFrame(&seq, 1000, &got)) { Out("ERROR camera gone"); break; }
            if (!got) continue;
            if (h->Flags & 1) errs++;
            AviAdd(h, Raw + sizeof(*h), AviCompression() == 0 ? Decode(h, Raw + sizeof(*h)) : FALSE);
            n++;
        }
        t0 = GetTickCount() - t0;
        AviEnd();
        Out("RECORD ok frames=%u written=%u damaged=%u ms=%u fps=%u file=%s", n, StRecFrames, errs, t0,
            t0 ? n * 1000 / t0 : 0, rec);
    }
stop:
    if (Vfw >= 0) VfwClose();
    else StopCam();
done:
    Out("DONE");
    if (Log != INVALID_HANDLE_VALUE) CloseHandle(Log);
}

void __stdcall Entry(void)
{
    WNDCLASSA wc;
    MSG msg;
    DWORD tid;
    const char *cmd = GetCommandLineA();
    int i;

    Inst = GetModuleHandleA(NULL);
    Heap = GetProcessHeap();
    InitTables();
    InitializeCriticalSection(&Lock);
    SetErrorMode(SEM_FAILCRITICALERRORS);
    GetModuleFileNameA(NULL, Folder, sizeof(Folder));
    for (i = lstrlenA(Folder); i > 0 && Folder[i - 1] != '\\'; i--) ;
    Folder[i > 0 ? i - 1 : 0] = 0;
    if (!Folder[0]) lstrcpyA(Folder, ".");
    Dev = CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);
    /* skip the program name */
    if (*cmd == '"') { cmd++; while (*cmd && *cmd != '"') cmd++; if (*cmd) cmd++; }
    else while (*cmd && *cmd != ' ' && *cmd != '\t') cmd++;
    {
        char first[16];
        Arg(cmd, first, sizeof(first));
        if (Eq(first, "/test")) {
            TestMode(cmd);
            ExitProcess(0);
        }
        if (Eq(first, "/gui")) {
            char a[MAX_PATH];
            const char *p = Arg(cmd, a, sizeof(a));
            p = Arg(p, a, sizeof(a));
            GuiItem = (int)Num(a);
            p = Arg(p, a, sizeof(a));
            GuiSecs = (ULONG)Num(a);
            p = Arg(p, GuiLog, sizeof(GuiLog));
            Arg(p, a, sizeof(a));
            GuiCam = (int)Num(a);
            lstrcpyA(Folder, "C:\\USBNT");
        }
    }
    if (Dev == INVALID_HANDLE_VALUE && !LoadVfw()) {
        MessageBoxA(NULL, "The USB driver (usbnt.sys) is not running.", "USB Camera", MB_OK | MB_ICONSTOP);
        ExitProcess(1);
    }
    Font = (HFONT)GetStockObject(ANSI_VAR_FONT);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = Inst;
    wc.hIcon = LoadIconA(Inst, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "UsbNtCamera";
    RegisterClassA(&wc);
    MainWnd = CreateWindowA("UsbNtCamera", "USB Camera", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 680, 640,
                            NULL, NULL, Inst, NULL);
    ShowWindow(MainWnd, SW_SHOWNORMAL);
    UpdateWindow(MainWnd);
    if (Dev != INVALID_HANDLE_VALUE) CreateThread(NULL, 0, ChangeThread, NULL, 0, &tid);
    if (GuiItem >= 0) {
        if (GuiCam > 0 && GuiCam <= (int)Cams.Count + NVfw) {
            CurCam = GuiCam - 1;
            SendMessageA(CamBox, CB_SETCURSEL, CurCam, 0);
            FillFormats();
        }
        SendMessageA(FmtBox, CB_SETCURSEL, GuiItem, 0);
        StartCapture();
        UpdateButtons();
        SetTimer(MainWnd, 77, (GuiSecs ? GuiSecs : 3) * 1000, NULL);
    }
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_SPACE && CapRun) { PhotoReq = 1; continue; }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
}
