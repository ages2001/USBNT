/*
 * captest.c - test of usbntcap.dll in the Windows 2000 / XP guests: Video for
 * Windows (avicap32: driver list, format, single frames, a two second
 * sequence) and DirectShow (video input devices, a preview graph for three
 * seconds, frames delivered).  Output on stdout, lines start with "CAPTEST".
 */
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <vfw.h>
#define STRSAFE_NO_DEPRECATE
#include <dshow.h>

static HANDLE Out;

static void P(const char *f, ...)
{
    char b[512];
    DWORD n;
    va_list ap;
    va_start(ap, f);
    wvsprintfA(b, f, ap);
    va_end(ap);
    WriteFile(Out, b, lstrlenA(b), &n, NULL);
}

static LONG Frames, Stream;
static DWORD SumB, SumG, SumR, Pix;

static LRESULT CALLBACK OnFrame(HWND w, LPVIDEOHDR h)
{
    BITMAPINFO bi;
    (void)w;
    Frames++;
    capGetVideoFormat(w, &bi, sizeof(bi));
    if (bi.bmiHeader.biCompression == BI_RGB && bi.bmiHeader.biBitCount == 24 && h->dwBytesUsed) {
        DWORD i, n = h->dwBytesUsed / 3;
        SumB = SumG = SumR = 0;
        for (i = 0; i < n; i++) {
            SumB += h->lpData[i * 3];
            SumG += h->lpData[i * 3 + 1];
            SumR += h->lpData[i * 3 + 2];
        }
        Pix = n;
    }
    P("CAPTEST frame %ld: %lu bytes, flags %lx\r\n", Frames, h->dwBytesUsed, h->dwFlags);
    return TRUE;
}

static LRESULT CALLBACK OnStream(HWND w, LPVIDEOHDR h)
{
    (void)w;
    if (h->dwBytesUsed) Stream++;
    return TRUE;
}

static void Vfw(void)
{
    char name[128], ver[128];
    int i, n = 0, drv = -1;
    HWND w;
    BITMAPINFO bi;
    CAPTUREPARMS cp;

    for (i = 0; i < 10; i++) {
        if (capGetDriverDescriptionA(i, name, sizeof(name), ver, sizeof(ver))) {
            P("CAPTEST vfw driver %d: %s (%s)\r\n", i, name, ver);
            if (drv < 0 && (strstr(name, "USBNT") || strstr(ver, "usbntcap"))) drv = i;
            n++;
        }
    }
    if (drv < 0) drv = 0;
    w = capCreateCaptureWindowA("captest", WS_POPUP | WS_VISIBLE, 0, 0, 320, 240, NULL, 0);
    if (w == NULL) { P("CAPTEST vfw window ERROR %lu\r\n", GetLastError()); return; }
    P("CAPTEST vfw window OK, connecting driver %d\r\n", drv);
    if (!capDriverConnect(w, drv)) {
        P("CAPTEST vfw connect ERROR\r\n");
        DestroyWindow(w);
        return;
    }
    P("CAPTEST vfw connect OK\r\n");
    capGetVideoFormat(w, &bi, sizeof(bi));
    P("CAPTEST vfw format %ldx%ld bits %d comp %08lx\r\n", bi.bmiHeader.biWidth, bi.bmiHeader.biHeight,
      bi.bmiHeader.biBitCount, bi.bmiHeader.biCompression);
    capSetCallbackOnFrame(w, OnFrame);
    for (i = 0; i < 3; i++) capGrabFrameNoStop(w);
    if (Pix) {
        P("CAPTEST vfw grab OK %ld frames, mean B %lu G %lu R %lu\r\n", Frames, SumB / Pix, SumG / Pix, SumR / Pix);
    } else {
        P("CAPTEST vfw grab %ld frames (no RGB data)\r\n", Frames);
    }
    capCaptureGetSetup(w, &cp, sizeof(cp));
    cp.fYield = FALSE;
    cp.fLimitEnabled = TRUE;
    cp.wTimeLimit = 2;
    cp.fCaptureAudio = FALSE;
    cp.fAbortLeftMouse = cp.fAbortRightMouse = FALSE;
    cp.vKeyAbort = 0;
    cp.dwRequestMicroSecPerFrame = 66667;
    capCaptureSetSetup(w, &cp, sizeof(cp));
    capSetCallbackOnVideoStream(w, OnStream);
    capCaptureSequenceNoFile(w);
    P("CAPTEST vfw sequence %ld frames\r\n", Stream);
    capSetCallbackOnFrame(w, NULL);
    capSetCallbackOnVideoStream(w, NULL);
    capDriverDisconnect(w);
    DestroyWindow(w);
}

static void Pump(DWORD ms)
{
    DWORD t0 = GetTickCount();
    MSG m;
    while (GetTickCount() - t0 < ms) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        Sleep(10);
    }
}

static void Dshow(void)
{
    ICreateDevEnum *de = NULL;
    IEnumMoniker *em = NULL;
    IMoniker *mk = NULL, *ours = NULL;
    IGraphBuilder *gb = NULL;
    ICaptureGraphBuilder2 *cg = NULL;
    IBaseFilter *src = NULL;
    IMediaControl *mc = NULL;
    IAMDroppedFrames *df = NULL;
    HRESULT hr;
    ULONG got;
    long nd = 0, dr = 0;

    if (FAILED(CoInitialize(NULL))) { P("CAPTEST dshow coinit ERROR\r\n"); return; }
    hr = CoCreateInstance(&CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC_SERVER, &IID_ICreateDevEnum, (void **)&de);
    if (FAILED(hr)) { P("CAPTEST dshow no device enumerator %lx\r\n", hr); goto out; }
    hr = ICreateDevEnum_CreateClassEnumerator(de, &CLSID_VideoInputDeviceCategory, &em, 0);
    if (hr != S_OK) { P("CAPTEST dshow no video input devices %lx\r\n", hr); goto out; }
    while (IEnumMoniker_Next(em, 1, &mk, &got) == S_OK) {
        IPropertyBag *pb;
        if (SUCCEEDED(IMoniker_BindToStorage(mk, NULL, NULL, &IID_IPropertyBag, (void **)&pb))) {
            VARIANT v;
            char n[128];
            VariantInit(&v);
            if (SUCCEEDED(IPropertyBag_Read(pb, L"FriendlyName", &v, NULL))) {
                WideCharToMultiByte(CP_ACP, 0, v.bstrVal, -1, n, sizeof(n), NULL, NULL);
                P("CAPTEST dshow device: %s\r\n", n);
                if (ours == NULL && strstr(n, "USBNT")) { ours = mk; IMoniker_AddRef(mk); }
                VariantClear(&v);
            }
            IPropertyBag_Release(pb);
        }
        if (ours == NULL) { ours = mk; IMoniker_AddRef(mk); }
        IMoniker_Release(mk);
    }
    if (ours == NULL) goto out;
    hr = IMoniker_BindToObject(ours, NULL, NULL, &IID_IBaseFilter, (void **)&src);
    if (FAILED(hr)) { P("CAPTEST dshow bind ERROR %lx\r\n", hr); goto out; }
    CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (void **)&gb);
    CoCreateInstance(&CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, &IID_ICaptureGraphBuilder2, (void **)&cg);
    if (gb == NULL || cg == NULL) { P("CAPTEST dshow graph ERROR\r\n"); goto out; }
    ICaptureGraphBuilder2_SetFiltergraph(cg, gb);
    IGraphBuilder_AddFilter(gb, src, L"USBNT camera");
    /* the capture pin (the VFW Capture source streams only for it) to a renderer */
    hr = ICaptureGraphBuilder2_RenderStream(cg, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, (IUnknown *)src, NULL, NULL);
    P("CAPTEST dshow render capture %lx\r\n", hr);
    if (FAILED(hr))
        hr = ICaptureGraphBuilder2_RenderStream(cg, &PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, (IUnknown *)src, NULL, NULL);
    if (FAILED(hr)) { P("CAPTEST dshow render ERROR %lx\r\n", hr); goto out; }
    IGraphBuilder_QueryInterface(gb, &IID_IMediaControl, (void **)&mc);
    hr = IMediaControl_Run(mc);
    P("CAPTEST dshow run %lx\r\n", hr);
    Pump(3000);
    if (SUCCEEDED(ICaptureGraphBuilder2_FindInterface(cg, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, src,
                                                      &IID_IAMDroppedFrames, (void **)&df)) ||
        SUCCEEDED(ICaptureGraphBuilder2_FindInterface(cg, &PIN_CATEGORY_PREVIEW, &MEDIATYPE_Video, src,
                                                      &IID_IAMDroppedFrames, (void **)&df))) {
        IAMDroppedFrames_GetNumNotDropped(df, &nd);
        IAMDroppedFrames_GetNumDropped(df, &dr);
    }
    IMediaControl_Stop(mc);
    P("CAPTEST dshow frames %ld dropped %ld\r\n", nd, dr);
out:
    if (df) IAMDroppedFrames_Release(df);
    if (mc) IMediaControl_Release(mc);
    if (src) IBaseFilter_Release(src);
    if (cg) ICaptureGraphBuilder2_Release(cg);
    if (gb) IGraphBuilder_Release(gb);
    if (ours) IMoniker_Release(ours);
    if (em) IEnumMoniker_Release(em);
    if (de) ICreateDevEnum_Release(de);
    CoUninitialize();
}

int main(int argc, char **argv)
{
    Out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (argc < 2 || lstrcmpiA(argv[1], "dshow") != 0) Vfw();
    if (argc < 2 || lstrcmpiA(argv[1], "vfw") != 0) Dshow();
    P("CAPTEST done\r\n");
    return 0;
}
