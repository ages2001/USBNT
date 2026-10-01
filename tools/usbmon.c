/*
 * usbmon.c - background helper for usbnt.sys, started at logon.
 *
 * - Drive letters: a USB disk gets a letter when it is plugged in and
 *   loses it when it is removed, instead of every empty slot holding a
 *   letter from boot on. A disk gets the letter it had last time if that
 *   letter is free.
 * - Mouse wheel on NT 3.x (no WM_MOUSEWHEEL there): wheel movements are
 *   sent to the window under the cursor as WM_VSCROLL.
 * - Requests a 1 ms system timer so that transfers finish without
 *   waiting for a 10-15 ms clock tick.
 *
 * Settings, REG_DWORD:
 *   HKCU\Software\UsbNt  WheelLines (3), WheelTarget (0 = window under the
 *                        cursor, 1 = foreground window)
 *   HKLM\Software\UsbNt  DriveLetters (1), TimerResolution (1)
 *   HKLM\Software\UsbNt\Letters  remembered letter per disk
 *
 * usbmon            start in the background
 * usbmon /wheel     handle the wheel also on NT 4.0 and later
 * usbmon /nowheel   never handle the wheel
 */

#include "devmap.h"

#define WM_DEVICECHANGE_        0x0219
#define DBT_DEVICEARRIVAL_      0x8000
#define DBT_DEVICEREMOVECOMPLETE_ 0x8004
#define DBT_DEVTYP_VOLUME_      0x00000002

typedef struct _VOL_BROADCAST {
    DWORD   Size;
    DWORD   DeviceType;
    DWORD   Reserved;
    DWORD   UnitMask;
    WORD    Flags;
} VOL_BROADCAST;

typedef LONG (WINAPI *BSM_FN)(DWORD, LPDWORD, UINT, WPARAM, LPARAM);
typedef UINT (WINAPI *TBP_FN)(UINT);

static DWORD Lines = 3;
static DWORD Target = 0;
static DEV_SLOT_MAP Map[USBNT_MAX_SLOTS];
static BOOL MapDone;

/* ------------------------------------------------------------------ */

static DWORD RegDword(HKEY Root, const char *Path, const char *Name, DWORD Default)
{
    HKEY key;
    DWORD type = 0;
    DWORD val = Default;
    DWORD len = sizeof(val);

    if (RegOpenKeyExA(Root, Path, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return Default;
    }
    if (RegQueryValueExA(key, Name, NULL, &type, (BYTE *)&val, &len) != ERROR_SUCCESS || type != REG_DWORD) {
        val = Default;
    }
    RegCloseKey(key);
    return val;
}

static BOOL HasArg(const char *Cmd, const char *Arg)
{
    const char *p;

    for (p = Cmd; *p; p++) {
        const char *a = Arg;
        const char *q = p;
        while (*a && *q) {
            char x = *q;
            char y = *a;
            if (x >= 'A' && x <= 'Z') x = (char)(x + 32);
            if (y >= 'A' && y <= 'Z') y = (char)(y + 32);
            if (x != y) {
                break;
            }
            a++;
            q++;
        }
        if (*a == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static HANDLE OpenUsbNt(void)
{
    return CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_EXISTING, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Drive letters                                                        */
/* ------------------------------------------------------------------ */

static void Broadcast(char Letter, BOOL Arrival)
{
    static BSM_FN bsm;
    static BOOL looked;
    VOL_BROADCAST vb;
    DWORD recipients = 0x00000008;      /* BSM_APPLICATIONS */

    if (!looked) {
        HMODULE u = GetModuleHandleA("user32.dll");
        looked = TRUE;
        if (u != NULL) {
            bsm = (BSM_FN)GetProcAddress(u, "BroadcastSystemMessageA");
        }
    }
    vb.Size = sizeof(vb);
    vb.DeviceType = DBT_DEVTYP_VOLUME_;
    vb.Reserved = 0;
    vb.UnitMask = 1UL << (Letter - 'A');
    vb.Flags = 0;
    if (bsm != NULL) {
        /* BSF_FORCEIFHUNG | BSF_IGNORECURRENTTASK | BSF_NOHANG */
        bsm(0x00000020 | 0x00000002 | 0x00000008, &recipients, WM_DEVICECHANGE_,
            Arrival ? DBT_DEVICEARRIVAL_ : DBT_DEVICEREMOVECOMPLETE_, (LPARAM)&vb);
    }
}

static char RememberedLetter(ULONG Identity)
{
    char name[16];
    HKEY key;
    DWORD type = 0;
    DWORD val = 0;
    DWORD len = sizeof(val);

    wsprintfA(name, "%08lX", Identity);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\UsbNt\\Letters", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&val, &len) != ERROR_SUCCESS || type != REG_DWORD) {
        val = 0;
    }
    RegCloseKey(key);
    return (val >= 'C' && val <= 'Z') ? (char)val : 0;
}

static void RememberLetter(ULONG Identity, char Letter)
{
    char name[16];
    HKEY key;
    DWORD disp;
    DWORD val = (DWORD)(UCHAR)Letter;

    wsprintfA(name, "%08lX", Identity);
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, "Software\\UsbNt\\Letters", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key,
                        &disp) == ERROR_SUCCESS) {
        RegSetValueExA(key, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
        RegCloseKey(key);
    }
}

static char FreeLetter(char Want)
{
    char c;

    if (Want != 0 && !DevMapLetterUsed(Want)) {
        return Want;
    }
    for (c = 'D'; c <= 'Z'; c++) {
        if (!DevMapLetterUsed(c)) {
            return c;
        }
    }
    return 0;
}

static void ManageLetters(HANDLE Dev)
{
    static USBNT_TREE tree;
    ULONG i;

    if (!DevMapQueryTree(Dev, &tree)) {
        return;
    }
    if (!MapDone) {
        DevMapSlots(tree.ScsiPort, Map, USBNT_MAX_SLOTS);
        MapDone = TRUE;
    }
    for (i = 0; i < tree.SlotCount && i < USBNT_MAX_SLOTS; i++) {
        USBNT_SLOT_INFO *s = &tree.Slot[i];
        char cur;
        char name[4];

        if (Map[i].Path[0] == 0) {
            continue;
        }
        cur = DevMapLetter(Map[i].Path);
        name[1] = ':';
        name[2] = 0;
        if (s->Present && cur == 0) {
            char l = FreeLetter(RememberedLetter(s->Identity));
            if (l != 0) {
                name[0] = l;
                if (DefineDosDeviceA(DDD_RAW_TARGET_PATH, name, Map[i].Path)) {
                    RememberLetter(s->Identity, l);
                    Broadcast(l, TRUE);
                }
            }
        } else if (!s->Present && cur != 0) {
            name[0] = cur;
            if (DefineDosDeviceA(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, name,
                                 Map[i].Path)) {
                Broadcast(cur, FALSE);
            }
        }
    }
}

static DWORD WINAPI LetterThread(LPVOID Param)
{
    HANDLE dev = OpenUsbNt();
    ULONG count = 0xFFFFFFFF;

    (void)Param;
    if (dev == INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (;;) {
        ULONG now = count;
        DWORD got = 0;
        ManageLetters(dev);
        if (!DeviceIoControl(dev, IOCTL_USBNT_WAIT_CHANGE, &now, sizeof(now), &now, sizeof(now), &got, NULL)) {
            Sleep(2000);
            continue;
        }
        count = now;
    }
}

/* ------------------------------------------------------------------ */
/* Mouse wheel (NT 3.x)                                                 */
/* ------------------------------------------------------------------ */

static BOOL IsClass(HWND Wnd, const char *Name)
{
    char cls[64];
    int i;

    if (GetClassNameA(Wnd, cls, sizeof(cls)) == 0) {
        return FALSE;
    }
    for (i = 0; Name[i] && cls[i]; i++) {
        char a = cls[i];
        char b = Name[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) {
            return FALSE;
        }
    }
    return Name[i] == 0 && cls[i] == 0;
}

typedef struct _FIND_SB {
    HWND Found;
} FIND_SB;

static BOOL CALLBACK FindVertBar(HWND Child, LPARAM Param)
{
    FIND_SB *f = (FIND_SB *)Param;

    if (IsClass(Child, "ScrollBar") && (GetWindowLongA(Child, GWL_STYLE) & SBS_VERT) && IsWindowVisible(Child)) {
        f->Found = Child;
        return FALSE;
    }
    return TRUE;
}

/* Finds the window that should receive WM_VSCROLL, and the scroll bar control if any */
static HWND FindTarget(HWND Start, HWND *Bar)
{
    HWND w;

    *Bar = NULL;
    for (w = Start; w != NULL; w = GetParent(w)) {
        LONG style = GetWindowLongA(w, GWL_STYLE);
        if (IsClass(w, "MDIClient")) {
            HWND act = (HWND)SendMessageA(w, WM_MDIGETACTIVE, 0, 0);
            if (act != NULL && (GetWindowLongA(act, GWL_STYLE) & WS_VSCROLL)) {
                return act;
            }
        }
        if (IsClass(w, "ScrollBar") && (style & SBS_VERT)) {
            *Bar = w;
            return GetParent(w);
        }
        if (style & WS_VSCROLL) {
            return w;
        }
        {
            FIND_SB f;
            f.Found = NULL;
            EnumChildWindows(w, FindVertBar, (LPARAM)&f);
            if (f.Found != NULL) {
                *Bar = f.Found;
                return w;
            }
        }
    }
    return NULL;
}

static void Scroll(LONG Delta)
{
    POINT pt;
    HWND start;
    HWND target;
    HWND bar;
    WPARAM code;
    DWORD i;
    DWORD n;

    if (Target == 1) {
        start = GetForegroundWindow();
    } else {
        GetCursorPos(&pt);
        start = WindowFromPoint(pt);
    }
    if (start == NULL) {
        return;
    }
    target = FindTarget(start, &bar);
    if (target == NULL) {
        return;
    }
    code = (Delta > 0) ? SB_LINEUP : SB_LINEDOWN;
    n = (DWORD)((Delta > 0) ? Delta : -Delta) * Lines;
    if (n > 200) {
        n = 200;
    }
    for (i = 0; i < n; i++) {
        PostMessageA(target, WM_VSCROLL, code, (LPARAM)bar);
    }
    PostMessageA(target, WM_VSCROLL, SB_ENDSCROLL, (LPARAM)bar);
}

static void WheelLoop(void)
{
    HANDLE dev = OpenUsbNt();

    if (dev == INVALID_HANDLE_VALUE) {
        return;
    }
    Lines = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "WheelLines", 3);
    Target = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "WheelTarget", 0);
    if (Lines < 1 || Lines > 50) {
        Lines = 3;
    }
    for (;;) {
        LONG delta = 0;
        DWORD got = 0;
        if (!DeviceIoControl(dev, IOCTL_USBNT_WHEEL_WAIT, NULL, 0, &delta, sizeof(delta), &got, NULL)) {
            Sleep(500);
            continue;
        }
        if (got == sizeof(delta) && delta != 0) {
            Scroll(delta);
        }
    }
}

/* ------------------------------------------------------------------ */

void __stdcall Entry(void)
{
    const char *cmd = GetCommandLineA();
    BOOL nt351 = (BOOL)((GetVersion() & 0xFF) < 4);
    BOOL wheel = (nt351 || HasArg(cmd, "/wheel")) && !HasArg(cmd, "/nowheel");
    HANDLE mutex;
    HANDLE dev;
    DWORD tid;
    int i;

    mutex = CreateMutexA(NULL, TRUE, "UsbNtHelper");
    if (mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
        ExitProcess(0);
    }
    /* the driver may still be starting right after boot */
    for (i = 0; i < 30; i++) {
        dev = OpenUsbNt();
        if (dev != INVALID_HANDLE_VALUE) {
            CloseHandle(dev);
            break;
        }
        Sleep(1000);
    }
    if (i == 30) {
        ExitProcess(1);
    }
    if (RegDword(HKEY_LOCAL_MACHINE, "Software\\UsbNt", "TimerResolution", 1)) {
        HMODULE mm = LoadLibraryA("winmm.dll");
        if (mm != NULL) {
            TBP_FN tbp = (TBP_FN)GetProcAddress(mm, "timeBeginPeriod");
            if (tbp != NULL) {
                tbp(1);
            }
        }
    }
    if (RegDword(HKEY_LOCAL_MACHINE, "Software\\UsbNt", "DriveLetters", 1)) {
        CreateThread(NULL, 0, LetterThread, NULL, 0, &tid);
    }
    if (wheel) {
        WheelLoop();
    }
    for (;;) {
        Sleep(INFINITE);
    }
}
