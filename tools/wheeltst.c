/*
 * wheeltst.c - test window for the wheel support. Shows a list box and
 * appends every WM_VSCROLL / WM_MOUSEWHEEL it receives to C:\WHEEL.LOG.
 */

#include <windows.h>

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif

static HANDLE Log;
static LONG TopIndex;

static void LogLine(const char *Fmt, ...)
{
    char buf[128];
    DWORD n;
    va_list ap;

    va_start(ap, Fmt);
    wvsprintfA(buf, Fmt, ap);
    va_end(ap);
    WriteFile(Log, buf, lstrlenA(buf), &n, NULL);
    FlushFileBuffers(Log);
}

static LRESULT CALLBACK WndProc(HWND Wnd, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case WM_VSCROLL:
        LogLine("VSCROLL %u\r\n", (unsigned)LOWORD(Wp));
        if (LOWORD(Wp) == SB_LINEDOWN) TopIndex++;
        if (LOWORD(Wp) == SB_LINEUP && TopIndex > 0) TopIndex--;
        if (LOWORD(Wp) == SB_ENDSCROLL) LogLine("TOP %ld\r\n", TopIndex);
        SetScrollPos(Wnd, SB_VERT, (int)TopIndex, TRUE);
        return 0;
    case WM_MOUSEWHEEL:
        LogLine("MOUSEWHEEL %d\r\n", (int)(short)HIWORD(Wp));
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(Wnd, Msg, Wp, Lp);
    }
}

void __stdcall Entry(void)
{
    WNDCLASSA wc;
    HWND w;
    MSG m;
    HINSTANCE inst = GetModuleHandleA(NULL);

    Log = CreateFileA("C:\\WHEEL.LOG", GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    {
        char *p = (char *)&wc;
        unsigned i;
        for (i = 0; i < sizeof(wc); i++) {
            p[i] = 0;
        }
    }
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "WheelTest";
    RegisterClassA(&wc);
    w = CreateWindowA("WheelTest", "Wheel test", WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_VISIBLE,
                      160, 120, 320, 240, NULL, NULL, inst, NULL);
    SetScrollRange(w, SB_VERT, 0, 100, FALSE);
    SetForegroundWindow(w);
    LogLine("START\r\n");
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    ExitProcess(0);
}
