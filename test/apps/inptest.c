/*
 * inptest.c - input test window for the NT keyboard/mouse tests (not shipped).
 *
 *   inptest SECONDS
 *
 * Opens a window in the foreground, records the characters typed, mouse
 * clicks per button, wheel turns and movement for SECONDS, then prints
 *   INPTEST chars=[...] left=N right=N middle=N x1=N x2=N wheel=N moves=N dx=N dy=N dbl=N
 * (dbl: double clicks of the left button; the second press counts in left as well)
 * on standard output.
 */
#include <windows.h>

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef WM_XBUTTONDOWN
#define WM_XBUTTONDOWN 0x020B
#endif

static char Chars[512];
static int NChars;
static LONG Left, Right, Middle, X1, X2, Wheel, Moves, Dbl;
static int Dismissed;
static LONG LastX = -100000, LastY, SumDx, SumDy;
static HWND Wnd;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CHAR:
        if (NChars < (int)sizeof(Chars) - 1 && w >= 0x20 && w < 0x7F) Chars[NChars++] = (char)w;
        return 0;
    case WM_LBUTTONDOWN: Left++; return 0;
    case WM_LBUTTONDBLCLK: Left++; Dbl++; return 0;   /* the second press of a double click */
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: Right++; return 0;
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: Middle++; return 0;
    case WM_XBUTTONDOWN: if (HIWORD(w) == 1) X1++; else X2++; return 0;
    case WM_MOUSEWHEEL: Wheel += (SHORT)HIWORD(w) / 120; return 0;
    case WM_MOUSEMOVE: {
        LONG x = (SHORT)LOWORD(l), y = (SHORT)HIWORD(l);
        if (LastX != -100000) { SumDx += x - LastX; SumDy += y - LastY; }
        LastX = x; LastY = y; Moves++;
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static DWORD Num(const char *s)
{
    DWORD v = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (DWORD)(*s++ - '0');
    return v;
}

void __stdcall Entry(void)
{
    WNDCLASSA wc;
    MSG msg;
    char *cl = GetCommandLineA(), out[1024];
    DWORD secs, t0, n;
    HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);

    while (*cl && *cl != ' ') cl++;
    secs = Num(cl);
    if (!secs) secs = 60;
    { char *p = (char *)&wc; int i; for (i = 0; i < (int)sizeof(wc); i++) p[i] = 0; }
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = "InpTest";
    RegisterClassA(&wc);
    /* full screen so every mouse movement stays inside */
    /* topmost: the NT 4.0 welcome screen and other windows stay behind it */
    Wnd = CreateWindowExA(WS_EX_TOPMOST, "InpTest", "input test", WS_POPUP | WS_VISIBLE, 0, 0,
                          GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), NULL, NULL, wc.hInstance, NULL);
    ShowWindow(Wnd, SW_SHOW);
    SetForegroundWindow(Wnd);
    SetFocus(Wnd);
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
    WriteFile(so, "INPTEST ready\r\n", 15, &n, NULL);
    t0 = GetTickCount();
    SetTimer(Wnd, 1, 200, NULL);
    while (GetTickCount() - t0 < secs * 1000 && GetMessageA(&msg, NULL, 0, 0)) {
        if (msg.message == WM_TIMER) {
            /* the NT 3.1 test image shows a system modal "service failed" box at logon */
            HWND box = FindWindowA(NULL, "Service Control Manager");
            if (box && Dismissed < 20) {
                /* it is system modal and owned by another process: click its button */
                HWND ok = GetDlgItem(box, IDOK);
                RECT r;
                POINT pt;
                if (!ok) ok = GetDlgItem(box, IDCANCEL);
                if (ok) GetWindowRect(ok, &r);
                else { GetWindowRect(box, &r); r.top = r.bottom - 40; r.bottom -= 20; }
                pt.x = (r.left + r.right) / 2; pt.y = (r.top + r.bottom) / 2;
                SetCursorPos(pt.x, pt.y);
                mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
                mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
                PostMessageA(box, WM_COMMAND, IDOK, 0);
                Dismissed++;
                Left = Right = Middle = 0; Dbl = 0;
            }
            if (!box && Dismissed && Dismissed < 100) {
                Dismissed = 100;
                SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, GetSystemMetrics(SM_CYSCREEN) / 2);
                Left = Right = Middle = 0; Dbl = 0;
                Moves = 0; SumDx = SumDy = 0;
            }
            if (GetForegroundWindow() != Wnd) SetForegroundWindow(Wnd);
            SetWindowPos(Wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    Chars[NChars] = 0;
    wsprintfA(out, "INPTEST chars=[%s] left=%d right=%d middle=%d x1=%d x2=%d wheel=%d moves=%d dx=%d dy=%d dbl=%d\r\n",
              Chars, Left, Right, Middle, X1, X2, Wheel, Moves, SumDx, SumDy, Dbl);
    WriteFile(so, out, lstrlenA(out), &n, NULL);
    ExitProcess(0);
}
