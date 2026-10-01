/* clicktst.c - shows the last mouse button messages with their times (test helper) */
#include <windows.h>

static char Lines[16][64];
static int Count;
static DWORD Last;

static void Add(const char *Name, LPARAM lp)
{
    int i;
    DWORD t = GetMessageTime();

    for (i = 15; i > 0; i--) {
        lstrcpyA(Lines[i], Lines[i - 1]);
    }
    wsprintfA(Lines[0], "%s %d,%d t=%lu dt=%lu", Name, (int)(short)LOWORD(lp), (int)(short)HIWORD(lp), t, t - Last);
    Last = t;
    Count++;
}

static LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    PAINTSTRUCT ps;
    HDC dc;
    int i;
    char buf[64];

    switch (m) {
    case WM_LBUTTONDOWN: Add("down", lp); InvalidateRect(w, NULL, TRUE); return 0;
    case WM_LBUTTONUP: Add("up", lp); InvalidateRect(w, NULL, TRUE); return 0;
    case WM_LBUTTONDBLCLK: Add("DBLCLK", lp); InvalidateRect(w, NULL, TRUE); return 0;
    case WM_PAINT:
        dc = BeginPaint(w, &ps);
        wsprintfA(buf, "dblclk time %u", GetDoubleClickTime());
        TextOutA(dc, 4, 4, buf, lstrlenA(buf));
        for (i = 0; i < 16; i++) {
            TextOutA(dc, 4, 24 + i * 16, Lines[i], lstrlenA(Lines[i]));
        }
        EndPaint(w, &ps);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

void WINAPI Entry(void)
{
    WNDCLASSA wc;
    MSG msg;
    HWND w;

    { char *p = (char *)&wc; int i; for (i = 0; i < (int)sizeof(wc); i++) { ((volatile char *)p)[i] = 0; } }
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "ClickTst";
    RegisterClassA(&wc);
    w = CreateWindowA("ClickTst", "clicktst", WS_OVERLAPPEDWINDOW, 100, 50, 400, 340, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(w, SW_SHOW);
    while (GetMessageA(&msg, NULL, 0, 0)) {
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
}
