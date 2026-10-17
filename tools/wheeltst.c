/*
 * wheeltst.c - test window for the wheel, the back / forward buttons and
 * autoscroll.  Counts what it receives - WM_VSCROLL / WM_HSCROLL, WM_MOUSEWHEEL,
 * WM_XBUTTON* and WM_APPCOMMAND (Windows 2000 / XP), Alt+Left / Alt+Right
 * (what usbmon types for buttons 4 / 5 on NT 3.x / 4.0), mouse clicks - shows
 * the counts and whether usbmon's autoscroll runs, and appends every event
 * to C:\WHEEL.LOG.
 *
 *   wheeltst [/center]     /center puts the pointer into the middle of the window
 */

#include <windows.h>

#define WM_MOUSEWHEEL_      0x020A
#define WM_XBUTTONDOWN_     0x020B
#define WM_XBUTTONUP_       0x020C
#define WM_APPCOMMAND_      0x0319

enum { C_VDOWN, C_VUP, C_HRIGHT, C_HLEFT, C_WHEEL, C_X1, C_X2, C_BACK, C_FWD, C_ALTLEFT, C_ALTRIGHT,
       C_LEFT, C_RIGHT, C_MIDDLE, C_AUTO, C_COUNT };
static const char *const Names[C_COUNT] = {
    "WM_VSCROLL down", "WM_VSCROLL up", "WM_HSCROLL right", "WM_HSCROLL left", "WM_MOUSEWHEEL",
    "WM_XBUTTONDOWN 1 (back)", "WM_XBUTTONDOWN 2 (forward)", "WM_APPCOMMAND back", "WM_APPCOMMAND forward",
    "Alt+Left", "Alt+Right", "left button", "right button", "middle button", "autoscroll started" };

static HANDLE Log;
static LONG TopIndex, LeftIndex;
static LONG Count[C_COUNT];
static LONG WheelSum;
static BOOL AutoOn;
static int CursorDue;

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

static void Bump(HWND Wnd, int C)
{
    Count[C]++;
    InvalidateRect(Wnd, NULL, TRUE);
}

typedef struct _CURSORINFO_ {                   /* CURSORINFO (Windows 2000 and later) */
    DWORD   cbSize;
    DWORD   flags;
    HCURSOR hCursor;
    POINT   ptScreenPos;
} CURSORINFO_;
typedef BOOL (WINAPI *GCI_FN)(CURSORINFO_ *);

/* usbmon's origin mark is visible while autoscroll runs; the hot spot of the
   cursor shown tells whether it is the scroll cursor (hot spot in the middle) */
static void CheckAutoscroll(HWND Wnd)
{
    HWND mark = FindWindowA("UsbNtAutoscroll", NULL);
    BOOL on = mark != NULL && IsWindowVisible(mark);
    static GCI_FN gci;
    CURSORINFO_ ci;
    ICONINFO ii;

    if (on != AutoOn) {
        AutoOn = on;
        LogLine("AUTOSCROLL %s\r\n", on ? "ON" : "OFF");
        if (on) Bump(Wnd, C_AUTO);
        InvalidateRect(Wnd, NULL, TRUE);
        CursorDue = 3;                          /* the cursor a moment later */
        return;
    }
    if (CursorDue == 0 || --CursorDue != 0) {
        return;
    }
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = ii.hbmColor = NULL;
    /* not asked for before Windows 2000: NT 3.1's GetProcAddress does not give NULL
       for "GetCursorInfo" (calling what it gives crashed) */
    if (gci == NULL && (GetVersion() & 0xFF) >= 5) {
        gci = (GCI_FN)GetProcAddress(GetModuleHandleA("user32.dll"), "GetCursorInfo");
    }
    ci.cbSize = sizeof(ci);
    ci.hCursor = NULL;
    if (gci == NULL || !gci(&ci) || ci.hCursor == NULL) {
        /* NT 3.x / 4.0: SetSystemCursor changes what the system arrow handle shows */
        ci.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    }
    if (GetIconInfo((HICON)ci.hCursor, &ii)) {
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    LogLine("CURSOR %lu %lu\r\n", ii.xHotspot, ii.yHotspot);
}

static void Paint(HWND Wnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(Wnd, &ps);
    char buf[80];
    int i, y = 4;

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, GetStockObject(ANSI_VAR_FONT));
    for (i = 0; i < C_COUNT; i++) {
        wsprintfA(buf, "%s: %ld", Names[i], Count[i]);
        if (i == C_WHEEL) wsprintfA(buf + lstrlenA(buf), " (sum %ld)", WheelSum);
        TextOutA(dc, 6, y, buf, lstrlenA(buf));
        y += 14;
    }
    wsprintfA(buf, "Autoscroll: %s   lines %ld, columns %ld", AutoOn ? "ON" : "off", TopIndex, LeftIndex);
    TextOutA(dc, 6, y + 4, buf, lstrlenA(buf));
    EndPaint(Wnd, &ps);
}

static LRESULT CALLBACK WndProc(HWND Wnd, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case WM_VSCROLL:
        LogLine("VSCROLL %u\r\n", (unsigned)LOWORD(Wp));
        if (LOWORD(Wp) == SB_LINEDOWN) { TopIndex++; Bump(Wnd, C_VDOWN); }
        if (LOWORD(Wp) == SB_LINEUP) { if (TopIndex > 0) TopIndex--; Bump(Wnd, C_VUP); }
        if (LOWORD(Wp) == SB_ENDSCROLL) LogLine("TOP %ld\r\n", TopIndex);
        SetScrollPos(Wnd, SB_VERT, (int)TopIndex, TRUE);
        return 0;
    case WM_HSCROLL:
        LogLine("HSCROLL %u\r\n", (unsigned)LOWORD(Wp));
        if (LOWORD(Wp) == SB_LINERIGHT) { LeftIndex++; Bump(Wnd, C_HRIGHT); }
        if (LOWORD(Wp) == SB_LINELEFT) { if (LeftIndex > 0) LeftIndex--; Bump(Wnd, C_HLEFT); }
        SetScrollPos(Wnd, SB_HORZ, (int)LeftIndex, TRUE);
        return 0;
    case WM_MOUSEWHEEL_:
        LogLine("MOUSEWHEEL %d\r\n", (int)(short)HIWORD(Wp));
        WheelSum += (short)HIWORD(Wp);
        Bump(Wnd, C_WHEEL);
        return 0;
    case WM_XBUTTONDOWN_:
        LogLine("XBUTTONDOWN %u\r\n", (unsigned)HIWORD(Wp));
        Bump(Wnd, HIWORD(Wp) == 2 ? C_X2 : C_X1);
        break;                                  /* DefWindowProc makes WM_APPCOMMAND of it */
    case WM_XBUTTONUP_:
        LogLine("XBUTTONUP %u\r\n", (unsigned)HIWORD(Wp));
        break;
    case WM_APPCOMMAND_: {
        unsigned cmd = (unsigned)(HIWORD(Lp) & 0x0FFF);
        LogLine("APPCOMMAND %u\r\n", cmd);
        if (cmd == 1) Bump(Wnd, C_BACK);        /* APPCOMMAND_BROWSER_BACKWARD */
        if (cmd == 2) Bump(Wnd, C_FWD);
        return TRUE;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        LogLine("KEY %u%s\r\n", (unsigned)Wp, (Lp & (1L << 29)) ? " alt" : "");
        if ((Lp & (1L << 29)) && Wp == VK_LEFT) Bump(Wnd, C_ALTLEFT);
        if ((Lp & (1L << 29)) && Wp == VK_RIGHT) Bump(Wnd, C_ALTRIGHT);
        if (Msg == WM_SYSKEYDOWN && (Wp == VK_LEFT || Wp == VK_RIGHT)) return 0;
        break;
    case WM_LBUTTONDOWN: LogLine("LBUTTONDOWN\r\n"); Bump(Wnd, C_LEFT); return 0;
    case WM_RBUTTONDOWN: LogLine("RBUTTONDOWN\r\n"); Bump(Wnd, C_RIGHT); return 0;
    case WM_MBUTTONDOWN: LogLine("MBUTTONDOWN\r\n"); Bump(Wnd, C_MIDDLE); return 0;
    case WM_LBUTTONUP: LogLine("LBUTTONUP\r\n"); return 0;
    case WM_RBUTTONUP: LogLine("RBUTTONUP\r\n"); return 0;
    case WM_MBUTTONUP: LogLine("MBUTTONUP\r\n"); return 0;
    case WM_TIMER:
        CheckAutoscroll(Wnd);
        return 0;
    case WM_PAINT:
        Paint(Wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(Wnd, Msg, Wp, Lp);
}

static BOOL HasArg(const char *Cmd, const char *Arg)
{
    const char *p;

    for (p = Cmd; *p; p++) {
        const char *a = Arg;
        const char *q = p;
        while (*a && *q && ((*q | 32) == (*a | 32))) {
            a++;
            q++;
        }
        if (*a == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

void __stdcall Entry(void)
{
    WNDCLASSA wc;
    HWND w;
    MSG m;
    RECT r;
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
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "WheelTest";
    RegisterClassA(&wc);
    w = CreateWindowA("WheelTest", "Wheel test", WS_OVERLAPPEDWINDOW | WS_VSCROLL | WS_HSCROLL | WS_VISIBLE,
                      160, 100, 320, 290, NULL, NULL, inst, NULL);
    SetScrollRange(w, SB_VERT, 0, 100, FALSE);
    SetScrollRange(w, SB_HORZ, 0, 100, FALSE);
    SetForegroundWindow(w);
    if (HasArg(GetCommandLineA(), "/center")) {
        GetWindowRect(w, &r);
        SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    SetTimer(w, 1, 100, NULL);
    LogLine("START\r\n");
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    ExitProcess(0);
}
