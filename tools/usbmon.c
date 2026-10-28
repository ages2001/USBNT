/*
 * usbmon.c - background helper for usbnt.sys, started at logon.
 *
 * - Drive letters: a USB disk gets a letter when it is plugged in and
 *   loses it when it is removed, instead of every empty slot holding a
 *   letter from boot on; every partition of a disk with several gets one.
 *   A disk (partition) gets the letter it had last time if that letter is
 *   free.
 * - Mouse wheel on NT 3.x (no WM_MOUSEWHEEL there): wheel movements are
 *   sent to the window under the cursor as WM_VSCROLL.  The horizontal wheel
 *   (tilt wheels, touch pads; no Windows before Vista has a message for it)
 *   becomes WM_HSCROLL on every version.
 * - Requests a 1 ms system timer so that transfers finish without
 *   waiting for a 10-15 ms clock tick.
 * - Back / forward mouse buttons on NT 3.x / 4.0 (which have no X buttons):
 *   typed as Alt+Left / Alt+Right.
 * - Autoscroll: a click of the wheel button scrolls the window under the
 *   pointer as the mouse moves away from that point (every version).
 * - Windows 2000 / XP: when a disk comes, usbinst /filters gives the disks
 *   of Windows' USB storage driver the usbnt filter (a stick with several
 *   partitions is then a fixed disk, see wdmflt.c; RemovableAsFixed).
 *
 * Settings, REG_DWORD unless noted:
 *   HKCU\Software\UsbNt  WheelLines (3), WheelTarget (0 = window under the
 *                        cursor, 1 = foreground window)
 *                        MiddleButton (1): 0 = ordinary middle click,
 *                        1 = autoscroll, 2 = autoscroll where a scroll bar
 *                        is, an ordinary middle click elsewhere
 *                        AutoscrollSpeed (100): percent
 *                        XButtons (1): NT 3.x / 4.0, 0 = back / forward off
 *                        BackKeys, ForwardKeys (REG_SZ, "Alt+Left",
 *                        "Alt+Right"): keys for buttons 4 and 5, e.g.
 *                        "Backspace", "Ctrl+PageUp", "0xA6", "" = nothing
 *   HKLM\Software\UsbNt  DriveLetters (1), TimerResolution (1)
 *   HKLM\Software\UsbNt\Letters  remembered letter per disk (ID) and partition (ID.n)
 *
 * usbmon            start in the background (logon)
 * usbmon /show      start, or bring up the window of the copy that runs:
 *                   driver state, drive letters of the USB disks and SD
 *                   cards, what the monitor did; Hide keeps it running
 * usbmon /wheel     handle the wheel also on NT 4.0 and later
 * usbmon /nowheel   never handle the wheel
 * usbmon /restart  end the copy that runs and start again (new settings)
 * usbmon /nohook    autoscroll through usbnt.sys even where Windows has a
 *                   low level mouse hook (NT 4.0 SP3 and later; a test aid)
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
/* What happened (shown in the window)                                  */
/* ------------------------------------------------------------------ */

#define NEVENTS 8
static CRITICAL_SECTION EvLock;
static char Events[NEVENTS][96];
static int NEv;
static HWND MonWnd, InfoWnd;
static volatile LONG DriverState;               /* 0 waiting, 1 running, 2 not found */
static BOOL WheelOn, LettersOn, TimerOn;
static char SlotText[USBNT_MAX_SLOTS][96];

static void Event(const char *Fmt, ...)
{
    char buf[96];
    SYSTEMTIME t;
    va_list ap;
    int i;

    GetLocalTime(&t);
    wsprintfA(buf, "%02u:%02u:%02u  ", t.wHour, t.wMinute, t.wSecond);
    va_start(ap, Fmt);
    wvsprintfA(buf + lstrlenA(buf), Fmt, ap);
    va_end(ap);
    EnterCriticalSection(&EvLock);
    if (NEv == NEVENTS) {
        for (i = 1; i < NEVENTS; i++) lstrcpyA(Events[i - 1], Events[i]);
        NEv--;
    }
    lstrcpyA(Events[NEv++], buf);
    LeaveCriticalSection(&EvLock);
    if (MonWnd) PostMessageA(MonWnd, WM_USER + 11, 0, 0);
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

static void LetterName(char *Name, ULONG Identity, int Part)
{
    if (Part <= 1) {
        wsprintfA(Name, "%08lX", Identity);
    } else {
        wsprintfA(Name, "%08lX.%u", Identity, Part);
    }
}

static char RememberedLetter(ULONG Identity, int Part)
{
    char name[16];
    HKEY key;
    DWORD type = 0;
    DWORD val = 0;
    DWORD len = sizeof(val);

    LetterName(name, Identity, Part);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Software\\UsbNt\\Letters", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return 0;
    }
    if (RegQueryValueExA(key, name, NULL, &type, (BYTE *)&val, &len) != ERROR_SUCCESS || type != REG_DWORD) {
        val = 0;
    }
    RegCloseKey(key);
    return (val >= 'C' && val <= 'Z') ? (char)val : 0;
}

static void RememberLetter(ULONG Identity, int Part, char Letter)
{
    char name[16];
    HKEY key;
    DWORD disp;
    DWORD val = (DWORD)(UCHAR)Letter;

    LetterName(name, Identity, Part);
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

/* gives the NT device Path a drive letter if it has none: the one remembered for (Identity, Part) if free */
static void GiveLetter(const char *Path, ULONG Identity, int Part, ULONG Slot)
{
    char name[4];
    char l;

    if (DevMapLetter(Path) != 0) {
        return;
    }
    l = FreeLetter(RememberedLetter(Identity, Part));
    if (l == 0) {
        return;
    }
    name[0] = l;
    name[1] = ':';
    name[2] = 0;
    if (DefineDosDeviceA(DDD_RAW_TARGET_PATH, name, Path)) {
        RememberLetter(Identity, Part, l);
        Broadcast(l, TRUE);
        if (Part <= 1) {
            Event("%c: given to disk slot %u", l, Slot);
        } else {
            Event("%c: given to disk slot %u, partition %u", l, Slot, Part);
        }
    }
}

static void TakeLetter(const char *Path, ULONG Slot, const char *Why)
{
    char name[4];
    char cur = DevMapLetter(Path);

    if (cur == 0) {
        return;
    }
    name[0] = cur;
    name[1] = ':';
    name[2] = 0;
    if (DefineDosDeviceA(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, name, Path)) {
        Broadcast(cur, FALSE);
        Event("%c: taken back (slot %u %s)", cur, Slot, Why);
    }
}

/* NT 3.1 - 4.0: the disk driver made partition devices for each slot when it started
   (MaxPartitions of them, see usbnt.sys) and maps them to the partitions of the medium in the
   slot: those that map one get letters, the others lose theirs */
static ULONG MediumKey[USBNT_MAX_SLOTS];

static void PartitionLetters(ULONG Slot, const USBNT_SLOT_INFO *S)
{
    ULONG key = S->Present ? (S->Identity ^ S->LastLba ^ (S->BlockSize << 7) ^ ((ULONG)S->MediaReady << 31) ^
                              ((ULONG)S->Partitions << 20)) | 1 : 0;
    char path[64];
    int k;

    if (key == MediumKey[Slot] || Map[Slot].Cd || Map[Slot].Disk < 0) {
        return;
    }
    MediumKey[Slot] = key;
    for (k = 2; k <= DEV_MAX_PARTS; k++) {
        int use = key ? DevMapPartInUse(&Map[Slot], k) : 0;
        DevMapPartPath(&Map[Slot], k, path);
        if (use < 0) {
            break;
        }
        if (use > 0) {
            GiveLetter(path, S->Identity, k, Slot);
        } else {
            TakeLetter(path, Slot, key ? "has no such partition" : "is empty");
        }
    }
}

static void ManageLetters(HANDLE Dev)
{
    static USBNT_TREE tree;
    ULONG i;
    int pass;
    BOOL nt5 = (GetVersion() & 0x80000000) == 0 && (GetVersion() & 0xFF) >= 5;

    if (!DevMapQueryTree(Dev, &tree)) {
        return;
    }
    if (!MapDone) {
        DevMapSlots(tree.ScsiPort, Map, USBNT_MAX_SLOTS);
        MapDone = TRUE;
    }
    /* the letters of empty slots are taken back first: a disk that came gets the lowest free ones */
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < tree.SlotCount && i < USBNT_MAX_SLOTS; i++) {
            USBNT_SLOT_INFO *s = &tree.Slot[i];
            char cur;
            char name[4];

            if (Map[i].Path[0] == 0 || (pass == 0) != !s->Present) {
                continue;
            }
            cur = DevMapLetter(Map[i].Path);
            {
                char v[12], p[20];
                int k;
                for (k = 0; k < 8; k++) v[k] = (char)(s->Vendor[k] >= 32 && s->Vendor[k] < 127 ? s->Vendor[k] : ' ');
                v[8] = 0;
                for (k = 7; k >= 0 && v[k] == ' '; k--) v[k] = 0;
                for (k = 0; k < 16; k++) p[k] = (char)(s->Product[k] >= 32 && s->Product[k] < 127 ? s->Product[k] : ' ');
                p[16] = 0;
                for (k = 15; k >= 0 && p[k] == ' '; k--) p[k] = 0;
                if (s->Present) wsprintfA(SlotText[i], "slot %u: %s %s", i, v, p);
                else wsprintfA(SlotText[i], "slot %u: empty", i);
            }
            if (nt5) {
                continue;                       /* Windows 2000 / XP: the mount manager gives the letters */
            }
            name[1] = ':';
            name[2] = 0;
            if (s->Present && cur == 0) {
                GiveLetter(Map[i].Path, s->Identity, 1, i);
            } else if (!s->Present && cur != 0) {
                name[0] = cur;
                if (DefineDosDeviceA(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, name,
                                     Map[i].Path)) {
                    Broadcast(cur, FALSE);
                    Event("%c: taken back (slot %u is empty)", cur, i);
                }
            }
            PartitionLetters(i, s);
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
    BOOL Vert;
} FIND_SB;

static BOOL CALLBACK FindBar(HWND Child, LPARAM Param)
{
    FIND_SB *f = (FIND_SB *)Param;

    if (IsClass(Child, "ScrollBar") && ((GetWindowLongA(Child, GWL_STYLE) & SBS_VERT) != 0) == f->Vert &&
        IsWindowVisible(Child)) {
        f->Found = Child;
        return FALSE;
    }
    return TRUE;
}

/* Finds the window that should receive WM_VSCROLL (Vert) or WM_HSCROLL, and the
   scroll bar control if any */
static HWND FindTargetDir(HWND Start, HWND *Bar, BOOL Vert)
{
    LONG want = Vert ? WS_VSCROLL : WS_HSCROLL;
    HWND w;

    *Bar = NULL;
    for (w = Start; w != NULL; w = GetParent(w)) {
        LONG style = GetWindowLongA(w, GWL_STYLE);
        if (IsClass(w, "MDIClient")) {
            DWORD act = 0;
            if (SendMessageTimeoutA(w, WM_MDIGETACTIVE, 0, 0, SMTO_ABORTIFHUNG, 500, &act) && act != 0 &&
                (GetWindowLongA((HWND)act, GWL_STYLE) & want)) {
                return (HWND)act;
            }
        }
        if (IsClass(w, "ScrollBar") && ((style & SBS_VERT) != 0) == Vert) {
            *Bar = w;
            return GetParent(w);
        }
        if (style & want) {
            return w;
        }
        {
            FIND_SB f;
            f.Found = NULL;
            f.Vert = Vert;
            EnumChildWindows(w, FindBar, (LPARAM)&f);
            if (f.Found != NULL) {
                *Bar = f.Found;
                return w;
            }
        }
    }
    return NULL;
}

static HWND FindTarget(HWND Start, HWND *Bar)
{
    return FindTargetDir(Start, Bar, TRUE);
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

static void ScrollH(LONG Delta)
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
    target = FindTargetDir(start, &bar, FALSE);
    if (target == NULL) {
        return;
    }
    code = (Delta > 0) ? SB_LINERIGHT : SB_LINELEFT;
    n = (DWORD)((Delta > 0) ? Delta : -Delta) * Lines;
    if (n > 200) {
        n = 200;
    }
    for (i = 0; i < n; i++) {
        PostMessageA(target, WM_HSCROLL, code, (LPARAM)bar);
    }
    PostMessageA(target, WM_HSCROLL, SB_ENDSCROLL, (LPARAM)bar);
}

static BOOL WheelVert;                  /* the vertical wheel too (NT 3.x, /wheel) */

static DWORD WINAPI WheelLoop(LPVOID Param)
{
    HANDLE dev = OpenUsbNt();

    (void)Param;
    if (dev == INVALID_HANDLE_VALUE) {
        return 1;
    }
    Lines = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "WheelLines", 3);
    Target = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "WheelTarget", 0);
    if (Lines < 1 || Lines > 50) {
        Lines = 3;
    }
    for (;;) {
        LONG delta[2];
        DWORD got = 0;
        delta[0] = delta[1] = 0;
        /* room for two: vertical and horizontal (a driver before 1.2 fills only the first) */
        if (!DeviceIoControl(dev, IOCTL_USBNT_WHEEL_WAIT, NULL, 0, delta, sizeof(delta), &got, NULL)) {
            Sleep(500);
            continue;
        }
        if (got >= sizeof(LONG) && delta[0] != 0 && WheelVert) {
            Scroll(delta[0]);
        }
        if (got >= 2 * sizeof(LONG) && delta[1] != 0) {
            ScrollH(delta[1]);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Back / forward buttons and autoscroll                                */
/* ------------------------------------------------------------------ */
/*
 * Back / forward (buttons 4 and 5): Windows 2000 / XP know them (WM_XBUTTON*,
 * WM_APPCOMMAND).  NT 3.x / 4.0 do not; there usbnt.sys hands their presses
 * to usbmon (IOCTL_USBNT_MOUSE_*) and usbmon types BackKeys / ForwardKeys
 * (Alt+Left / Alt+Right: back / forward in browsers and Explorer).
 *
 * Autoscroll: a click of the wheel button puts an origin mark where the
 * pointer is and turns the pointer into a scroll cursor; moving away from
 * the mark scrolls the window under it, faster the farther the pointer is.
 * Any click ends it (the click goes nowhere); holding the wheel button and
 * dragging scrolls until it is released.  The button presses come from a
 * low level mouse hook (NT 4.0 SP3 and later: every mouse) or, where there
 * is none, from usbnt.sys (its own mice: USB, PS/2, I2C).  Scrolling:
 * WM_MOUSEWHEEL to the window under the mark (NT 4.0 and later) or
 * WM_VSCROLL (NT 3.x), WM_HSCROLL sideways.
 */

#define WM_MOUSEWHEEL_          0x020A
#define WM_XBUTTONDOWN_         0x020B
#define WM_XBUTTONUP_           0x020C
#define WH_MOUSE_LL_            14
#define SPI_GETWHEELSCROLLLINES_ 104
#define SPI_SETCURSORS_         87
#define MOUSEEVENTF_MIDDLEDOWN_ 0x0020
#define MOUSEEVENTF_MIDDLEUP_   0x0040
#define KEYEVENTF_EXTENDEDKEY_  0x0001
#define KEYEVENTF_KEYUP_        0x0002
#define WS_EX_TOOLWINDOW_       0x00000080L
#define MA_NOACTIVATE_          3
#define USBMON_INJECTED         0x554D4F4EUL    /* dwExtraInfo of the input usbmon makes itself */

#define AS_PRESS                (WM_USER + 20)  /* wParam: button 0..4 (left, right, middle, 4, 5) */
#define AS_RELEASE              (WM_USER + 21)
#define AS_STOP                 (WM_USER + 22)
#define AS_TIMER                1
#define AS_TICK                 20              /* ms */
#define AS_DEAD                 10              /* pixels around the origin that do not scroll */
#define AS_MARK                 28              /* size of the origin mark */
#define AS_NCUR                 11

typedef struct _LLMOUSE {                       /* MSLLHOOKSTRUCT */
    POINT   Pt;
    DWORD   MouseData;
    DWORD   Flags;
    DWORD   Time;
    DWORD   ExtraInfo;
} LLMOUSE;

typedef struct _KEYS {
    BYTE    Mods;                               /* 1 Ctrl, 2 Alt, 4 Shift, 8 Win */
    BYTE    Vk;                                 /* 0 = nothing */
    BYTE    Ext;
} KEYS;

typedef BOOL (WINAPI *SSC_FN)(HCURSOR, DWORD);
typedef int (WINAPI *SWR_FN)(HWND, HRGN, BOOL);

static DWORD MiddleMode = 1;                    /* MiddleButton: 0 normal, 1 autoscroll, 2 where a scroll bar is */
static DWORD XKeysOn = 1;                       /* XButtons */
static DWORD AsSpeed = 100;                     /* AutoscrollSpeed, percent */
static KEYS BackKeys, FwdKeys;
static char BackName[32], FwdName[32];
static BOOL NoHook;                             /* /nohook: the driver even where a hook exists */
static DWORD WinVer;                            /* major version */

static HANDLE BtnCtl = INVALID_HANDLE_VALUE;    /* sets the driver's capture */
static CRITICAL_SECTION CapLock;
static ULONG CapBase;                           /* USBNT_CAP_* usbmon always wants */
static BOOL BtnOn;                              /* driver channel runs */
static BOOL UseHook, UseDriver;
static HHOOK Hook;
static volatile LONG HookGrab;                  /* the hook takes every press (autoscroll runs) */
static DWORD HookSwallow;                       /* buttons whose press the hook took (hook thread) */
static HANDLE HookReady;

static HWND AsWnd, AsMark;
static volatile LONG AsActive;
static HCURSOR AsCur[AS_NCUR];
static int AsCurShown = -1;
static BOOL AsBoth = TRUE;                      /* horizontal scrolling possible: 4-way mark */
static SSC_FN SetSysCursor;
static const DWORD SysCurId[3] = { 32512, 32513, 32649 };      /* OCR_NORMAL, OCR_IBEAM, OCR_HAND */
static HCURSOR SavedCur[3];
static LONG XCount, AsCount;

static struct {
    POINT   O;                                  /* origin */
    HWND    Wheel;                              /* window under the origin */
    HWND    VTarget, VBar, HTarget, HBar;
    BOOL    Held, Dragged, Pass, Driver;
    LONG    AccV, AccH;                         /* 1/1000 lines (WM_MOUSEWHEEL: 1/1000 wheel delta) */
    DWORD   Last;
    DWORD   Ticks;
} As;

/* ---- key combinations ---------------------------------------------- */

static BOOL SameText(const char *A, int N, const char *B)
{
    int i;

    for (i = 0; i < N; i++) {
        char x = A[i], y = B[i];
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y || y == 0) {
            return FALSE;
        }
    }
    return B[N] == 0;
}

/* "Alt+Left", "Ctrl+Shift+Tab", "Backspace", "F5", "0xA6", "" (nothing) */
static void ParseKeys(const char *Text, KEYS *K)
{
    static const struct { const char *Name; BYTE Vk; BYTE Ext; } names[] = {
        { "Left", 0x25, 1 }, { "Right", 0x27, 1 }, { "Up", 0x26, 1 }, { "Down", 0x28, 1 },
        { "Backspace", 0x08, 0 }, { "Back", 0x08, 0 }, { "Tab", 0x09, 0 }, { "Enter", 0x0D, 0 },
        { "Return", 0x0D, 0 }, { "Esc", 0x1B, 0 }, { "Escape", 0x1B, 0 }, { "Space", 0x20, 0 },
        { "PageUp", 0x21, 1 }, { "PgUp", 0x21, 1 }, { "PageDown", 0x22, 1 }, { "PgDn", 0x22, 1 },
        { "End", 0x23, 1 }, { "Home", 0x24, 1 }, { "Insert", 0x2D, 1 }, { "Delete", 0x2E, 1 },
        { "Del", 0x2E, 1 }, { "BrowserBack", 0xA6, 1 }, { "BrowserForward", 0xA7, 1 },
    };
    const char *p = Text;

    K->Mods = K->Vk = K->Ext = 0;
    while (*p) {
        const char *t;
        int n;
        int i;
        while (*p == ' ' || *p == '+') p++;
        t = p;
        while (*p && *p != '+' && *p != ' ') p++;
        n = (int)(p - t);
        if (n == 0) {
            break;
        }
        if (SameText(t, n, "Ctrl") || SameText(t, n, "Control")) K->Mods |= 1;
        else if (SameText(t, n, "Alt")) K->Mods |= 2;
        else if (SameText(t, n, "Shift")) K->Mods |= 4;
        else if (SameText(t, n, "Win")) K->Mods |= 8;
        else if (SameText(t, n, "None") || SameText(t, n, "Off")) { K->Mods = K->Vk = 0; return; }
        else if (n == 1 && ((*t >= '0' && *t <= '9') || (*t >= 'A' && *t <= 'Z') || (*t >= 'a' && *t <= 'z'))) {
            K->Vk = (BYTE)((*t >= 'a') ? *t - 32 : *t);
        } else if ((t[0] == 'F' || t[0] == 'f') && n >= 2 && n <= 3 && t[1] >= '1' && t[1] <= '9') {
            int f = t[1] - '0';
            if (n == 3) f = f * 10 + (t[2] - '0');
            if (f >= 1 && f <= 24) K->Vk = (BYTE)(0x6F + f);
        } else if (n > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
            int v = 0;
            for (i = 2; i < n; i++) {
                char c = t[i];
                v = v * 16 + ((c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 :
                              (c >= 'A' && c <= 'F') ? c - 'A' + 10 : 0);
            }
            K->Vk = (BYTE)v;
        } else {
            for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
                if (SameText(t, n, names[i].Name)) {
                    K->Vk = names[i].Vk;
                    K->Ext = names[i].Ext;
                }
            }
        }
    }
}

static void KeyEvent(BYTE Vk, BOOL Ext, BOOL Up)
{
    keybd_event(Vk, (BYTE)MapVirtualKeyA(Vk, 0), (DWORD)((Ext ? KEYEVENTF_EXTENDEDKEY_ : 0) |
                (Up ? KEYEVENTF_KEYUP_ : 0)), USBMON_INJECTED);
}

static void SendKeys(const KEYS *K)
{
    static const BYTE mod[4] = { VK_CONTROL, VK_MENU, VK_SHIFT, 0x5B };
    int i;

    if (K->Vk == 0) {
        return;
    }
    for (i = 0; i < 4; i++) {
        if (K->Mods & (1 << i)) KeyEvent(mod[i], i == 3, FALSE);
    }
    KeyEvent(K->Vk, K->Ext, FALSE);
    KeyEvent(K->Vk, K->Ext, TRUE);
    for (i = 3; i >= 0; i--) {
        if (K->Mods & (1 << i)) KeyEvent(mod[i], i == 3, TRUE);
    }
}

static void RegString(const char *Name, const char *Default, char *Out, DWORD Size)
{
    HKEY key;
    DWORD type = 0;
    DWORD len = Size;

    lstrcpyA(Out, Default);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\UsbNt", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return;
    }
    if (RegQueryValueExA(key, Name, NULL, &type, (BYTE *)Out, &len) != ERROR_SUCCESS || type != REG_SZ) {
        lstrcpyA(Out, Default);
    }
    Out[Size - 1] = 0;
    RegCloseKey(key);
}

/* ---- driver channel ------------------------------------------------- */

static void SetCap(ULONG Flags)
{
    DWORD got = 0;

    if (BtnCtl == INVALID_HANDLE_VALUE) {
        return;
    }
    EnterCriticalSection(&CapLock);
    DeviceIoControl(BtnCtl, IOCTL_USBNT_MOUSE_CAPTURE, &Flags, sizeof(Flags), NULL, 0, &got, NULL);
    LeaveCriticalSection(&CapLock);
}

static DWORD WINAPI ButtonLoop(LPVOID Param)
{
    static USBNT_MOUSE_EVENTS ev;
    HANDLE dev = (HANDLE)Param;
    ULONG i;
    int b;

    for (;;) {
        DWORD got = 0;
        if (!DeviceIoControl(dev, IOCTL_USBNT_MOUSE_WAIT, NULL, 0, &ev, sizeof(ev), &got, NULL)) {
            Sleep(200);
            continue;
        }
        for (i = 0; i < ev.Count && i < USBNT_MOUSE_MAX_EVENTS; i++) {
            USHORT f = ev.Ev[i].Buttons;
            for (b = 0; b < 5; b++) {
                if (f & (1 << (2 * b))) {
                    if (b >= 3 && !ev.Ev[i].Grab && !AsActive) {
                        InterlockedIncrement(&XCount);
                        SendKeys(b == 3 ? &BackKeys : &FwdKeys);
                    } else if (AsWnd != NULL) {
                        PostMessageA(AsWnd, AS_PRESS, (WPARAM)b, 1);
                    }
                }
                if ((f & (2 << (2 * b))) && b < 3 && AsWnd != NULL) {
                    PostMessageA(AsWnd, AS_RELEASE, (WPARAM)b, 1);
                }
            }
        }
    }
    return 0;
}

/* ---- low level mouse hook (NT 4.0 SP3 and later) -------------------- */

static LRESULT CALLBACK HookProc(int Code, WPARAM Wp, LPARAM Lp)
{
    if (Code == HC_ACTION) {
        LLMOUSE *m = (LLMOUSE *)Lp;
        int b = -1;
        BOOL down = FALSE;
        switch (Wp) {
        case WM_LBUTTONDOWN: down = TRUE; /* fall through */
        case WM_LBUTTONUP: b = 0; break;
        case WM_RBUTTONDOWN: down = TRUE; /* fall through */
        case WM_RBUTTONUP: b = 1; break;
        case WM_MBUTTONDOWN: down = TRUE; /* fall through */
        case WM_MBUTTONUP: b = 2; break;
        case WM_XBUTTONDOWN_: down = TRUE; /* fall through */
        case WM_XBUTTONUP_: b = (HIWORD(m->MouseData) == 2) ? 4 : 3; break;
        }
        if (b >= 0 && m->ExtraInfo != USBMON_INJECTED) {
            if (down && (HookGrab || (b == 2 && MiddleMode != 0))) {
                HookGrab = 1;
                HookSwallow |= 1UL << b;
                PostMessageA(AsWnd, AS_PRESS, (WPARAM)b, 0);
                return 1;
            }
            if (!down && (HookSwallow & (1UL << b))) {
                HookSwallow &= ~(1UL << b);
                PostMessageA(AsWnd, AS_RELEASE, (WPARAM)b, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(Hook, Code, Wp, Lp);
}

static DWORD WINAPI HookThread(LPVOID Param)
{
    MSG msg;

    (void)Param;
    Hook = SetWindowsHookExA(WH_MOUSE_LL_, HookProc, GetModuleHandleA(NULL), 0);
    UseHook = Hook != NULL;
    SetEvent(HookReady);
    if (Hook == NULL) {
        return 1;
    }
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        DispatchMessageA(&msg);
    }
    UnhookWindowsHookEx(Hook);
    return 0;
}

/* ---- cursors and the origin mark ------------------------------------ */

/* the cursor on the screen changes only when the pointer moves: a move to where it
   is.  Not on NT 3.x, where SetCursorPos waits for the server, which may want to
   send this thread WM_SETCURSOR (the origin mark is under the pointer) */
static void AsRefreshCursor(void)
{
    POINT p;

    if (WinVer >= 4 && GetCursorPos(&p)) {
        SetCursorPos(p.x, p.y);
    }
}

static void AsSetCursor(int K)
{
    int i;

    if (K == AsCurShown || SetSysCursor == NULL || AsCur[K] == NULL) {
        return;
    }
    for (i = 0; i < 3; i++) {
        if (SavedCur[i] != NULL) {
            SetSysCursor((HCURSOR)CopyIcon((HICON)AsCur[K]), SysCurId[i]);
        }
    }
    AsCurShown = K;
    AsRefreshCursor();
}

static void AsRestoreCursors(BOOL Always)
{
    int i;

    if (AsCurShown < 0 && !Always) {
        return;
    }
    AsCurShown = -1;
    if (!SystemParametersInfoA(SPI_SETCURSORS_, 0, NULL, 0) && SetSysCursor != NULL) {
        /* NT 3.x: the copies taken at the start (NT 4.0 and later: the user's cursors again) */
        for (i = 0; i < 3; i++) {
            if (SavedCur[i] != NULL) {
                SetSysCursor((HCURSOR)CopyIcon((HICON)SavedCur[i]), SysCurId[i]);
            }
        }
    }
    AsRefreshCursor();
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *E)
{
    (void)E;
    AsRestoreCursors(FALSE);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void Arrow(HDC Dc, int Cx, int Cy, int Dx, int Dy)
{
    POINT p[3];

    p[0].x = Cx + Dx * 12; p[0].y = Cy + Dy * 12;
    p[1].x = Cx + Dx * 6 - Dy * 4; p[1].y = Cy + Dy * 6 - Dx * 4;
    p[2].x = Cx + Dx * 6 + Dy * 4; p[2].y = Cy + Dy * 6 + Dx * 4;
    Polygon(Dc, p, 3);
}

static LRESULT CALLBACK MarkProc(HWND Wnd, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE_;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(Wnd, &ps);
        int c = AS_MARK / 2;
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(BLACK_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(WHITE_BRUSH));
        Ellipse(dc, 0, 0, AS_MARK, AS_MARK);
        SelectObject(dc, GetStockObject(BLACK_BRUSH));
        Arrow(dc, c, c, 0, -1);
        Arrow(dc, c, c, 0, 1);
        if (AsBoth) {
            Arrow(dc, c, c, -1, 0);
            Arrow(dc, c, c, 1, 0);
        }
        Ellipse(dc, c - 2, c - 2, c + 3, c + 3);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        EndPaint(Wnd, &ps);
        return 0;
    }
    }
    return DefWindowProcA(Wnd, Msg, Wp, Lp);
}

/* ---- autoscroll ------------------------------------------------------ */

/* lines per second for a distance from the origin */
static LONG AsSpeedOf(LONG D)
{
    LONG a = (D < 0 ? -D : D) - AS_DEAD;
    LONG v;

    if (a <= 0) {
        return 0;
    }
    v = a / 4 + a * a / 300 + 1;
    v = v * (LONG)AsSpeed / 100;
    if (v > 2000) v = 2000;
    return D < 0 ? -v : v;
}

static void AsEnd(const char *Why)
{
    if (!AsActive) {
        return;
    }
    Event("autoscroll ended: %s", Why);
    AsActive = 0;
    HookGrab = 0;
    KillTimer(AsWnd, AS_TIMER);
    ShowWindow(AsMark, SW_HIDE);
    AsRestoreCursors(FALSE);
    if (As.Driver) {
        SetCap(CapBase);                        /* the buttons are the system's again */
    }
}

static void AsStart(BOOL Driver)
{
    POINT pt;
    HWND w;
    SWR_FN swr;

    GetCursorPos(&pt);
    w = WindowFromPoint(pt);
    As.Driver = Driver;
    As.O = pt;
    As.Wheel = w;
    As.VTarget = w != NULL ? FindTargetDir(w, &As.VBar, TRUE) : NULL;
    As.HTarget = w != NULL ? FindTargetDir(w, &As.HBar, FALSE) : NULL;
    if (w == NULL || (MiddleMode == 2 && As.VTarget == NULL && As.HTarget == NULL)) {
        /* nothing to scroll here: an ordinary middle click */
        As.Pass = TRUE;
        HookGrab = 0;
        if (Driver) {
            SetCap(CapBase);
        }
        mouse_event(MOUSEEVENTF_MIDDLEDOWN_, 0, 0, 0, USBMON_INJECTED);
        return;
    }
    AsActive = 1;
    AsCount++;
    As.Held = TRUE;
    As.Dragged = FALSE;
    As.AccV = As.AccH = 0;
    As.Last = GetTickCount();
    AsBoth = As.HTarget != NULL;
    /* a round mark where there is SetWindowRgn (NT 4.0); not asked for on NT 3.x,
       whose GetProcAddress need not give NULL for a function it does not have */
    swr = WinVer >= 4 ? (SWR_FN)GetProcAddress(GetModuleHandleA("user32.dll"), "SetWindowRgn") : NULL;
    if (swr != NULL) {
        swr(AsMark, CreateEllipticRgn(0, 0, AS_MARK + 1, AS_MARK + 1), FALSE);
    }
    InvalidateRect(AsMark, NULL, TRUE);
    SetWindowPos(AsMark, HWND_TOPMOST, pt.x - AS_MARK / 2, pt.y - AS_MARK / 2, AS_MARK, AS_MARK,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(AsMark);
    AsSetCursor(AsBoth ? 0 : 1);
    SetTimer(AsWnd, AS_TIMER, AS_TICK, NULL);
}

static void PostLines(HWND Target, HWND Bar, UINT Msg, LONG N)
{
    LONG i;
    WPARAM code = (WPARAM)(N > 0 ? SB_LINEDOWN : SB_LINEUP);   /* SB_LINERIGHT / SB_LINELEFT have the same values */

    if (N < 0) N = -N;
    for (i = 0; i < N && i < 100; i++) {
        PostMessageA(Target, Msg, code, (LPARAM)Bar);
    }
    PostMessageA(Target, Msg, SB_ENDSCROLL, (LPARAM)Bar);
}

static void AsTick(void)
{
    POINT p;
    LONG dx, dy, vy, vx, n;
    DWORD now = GetTickCount();
    DWORD dt = now - As.Last;
    int k;

    As.Last = now;
    if (dt > 100) dt = 100;
    if (++As.Ticks % 25 == 0 && WinVer >= 4) {
        /* stay above windows that made themselves topmost meanwhile */
        SetWindowPos(AsMark, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        AsEnd("Esc");
        return;
    }
    if (!IsWindow(As.Wheel)) {
        AsEnd("the window is gone");
        return;
    }
    GetCursorPos(&p);
    dx = p.x - As.O.x;
    dy = p.y - As.O.y;
    if (As.Held && (dx > AS_DEAD || dx < -AS_DEAD || dy > AS_DEAD || dy < -AS_DEAD)) {
        As.Dragged = TRUE;
    }
    vy = AsSpeedOf(dy);
    vx = As.HTarget != NULL ? AsSpeedOf(dx) : 0;
    /* vertical */
    if (WinVer >= 4) {
        UINT lines = 3;
        LONG per;
        if (!SystemParametersInfoA(SPI_GETWHEELSCROLLLINES_, 0, &lines, 0) || lines == 0 || lines > 100) {
            lines = 3;
        }
        per = 120 / (LONG)lines;                /* wheel delta of one line */
        if (per < 1) per = 1;
        As.AccV += vy * (LONG)dt * 120 / (LONG)lines;
        n = As.AccV / 1000 / per;
        if (n != 0) {
            As.AccV -= n * per * 1000;
            PostMessageA(As.Wheel, WM_MOUSEWHEEL_, (WPARAM)((DWORD)(WORD)(SHORT)(-n * per) << 16),
                         MAKELPARAM(As.O.x, As.O.y));
        }
    } else if (As.VTarget != NULL) {
        As.AccV += vy * (LONG)dt;
        n = As.AccV / 1000;
        if (n != 0) {
            As.AccV -= n * 1000;
            PostLines(As.VTarget, As.VBar, WM_VSCROLL, n);
        }
    }
    if (vy == 0) As.AccV = 0;
    /* horizontal */
    if (As.HTarget != NULL) {
        As.AccH += vx * (LONG)dt;
        n = As.AccH / 1000;
        if (n != 0) {
            As.AccH -= n * 1000;
            PostLines(As.HTarget, As.HBar, WM_HSCROLL, n);
        }
        if (vx == 0) As.AccH = 0;
    }
    /* the cursor shows the direction */
    if (vx == 0 && vy == 0) {
        k = AsBoth ? 0 : 1;
    } else if (vx == 0) {
        k = vy < 0 ? 3 : 4;
    } else if (vy == 0) {
        k = vx > 0 ? 5 : 6;
    } else {
        k = vy < 0 ? (vx > 0 ? 7 : 8) : (vx > 0 ? 9 : 10);
    }
    AsSetCursor(k);
}

static LRESULT CALLBACK AsProc(HWND Wnd, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case AS_PRESS:
        if (AsActive) {
            AsEnd("click");                     /* any click ends it and goes nowhere */
        } else if (Wp == 2 && MiddleMode != 0) {
            AsStart(Lp != 0);
        } else if (Lp == 0) {
            HookGrab = 0;
        } else {
            SetCap(CapBase);
        }
        return 0;
    case AS_RELEASE:
        if (Wp == 2 && As.Pass) {
            As.Pass = FALSE;
            mouse_event(MOUSEEVENTF_MIDDLEUP_, 0, 0, 0, USBMON_INJECTED);
        } else if (Wp == 2 && AsActive && As.Held) {
            As.Held = FALSE;
            if (As.Dragged) {
                AsEnd("wheel button released");  /* held and dragged: released = done */
            }
        }
        return 0;
    case AS_STOP:
        AsEnd("usbmon ends");
        return 0;
    case WM_TIMER:
        if (AsActive) AsTick();
        return 0;
    }
    return DefWindowProcA(Wnd, Msg, Wp, Lp);
}

static DWORD WINAPI AsThread(LPVOID Param)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc;
    MSG msg;
    int i;

    wc.style = 0;
    wc.lpfnWndProc = AsProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = inst;
    wc.hIcon = NULL;
    wc.hCursor = NULL;
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "UsbNtAutoscrollCtl";
    RegisterClassA(&wc);
    wc.lpfnWndProc = MarkProc;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);  /* the system arrow, which is the scroll cursor meanwhile */
    wc.lpszClassName = "UsbNtAutoscroll";
    RegisterClassA(&wc);
    for (i = 0; i < AS_NCUR; i++) {
        AsCur[i] = LoadCursorA(inst, MAKEINTRESOURCEA(101 + i));
    }
    AsWnd = CreateWindowA("UsbNtAutoscrollCtl", "", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    AsMark = CreateWindowExA(WS_EX_TOPMOST | (WinVer >= 4 ? WS_EX_TOOLWINDOW_ : 0), "UsbNtAutoscroll", "",
                             WS_POPUP, 0, 0, AS_MARK, AS_MARK, NULL, NULL, inst, NULL);
    SetEvent((HANDLE)Param);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        DispatchMessageA(&msg);
    }
    return 0;
}

/* stops autoscroll and puts the cursors back (usbmon ends) */
static void MouseShutdown(void)
{
    if (AsActive && AsWnd != NULL) {
        SendMessageTimeoutA(AsWnd, AS_STOP, 0, 0, SMTO_ABORTIFHUNG, 1000, NULL);
    }
    AsRestoreCursors(FALSE);
}

static void MouseSetup(BOOL Driver)
{
    HMODULE user = GetModuleHandleA("user32.dll");
    HANDLE ready;
    DWORD tid;
    int i;

    MiddleMode = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "MiddleButton", 1);
    XKeysOn = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "XButtons", 1);
    AsSpeed = RegDword(HKEY_CURRENT_USER, "Software\\UsbNt", "AutoscrollSpeed", 100);
    if (AsSpeed < 10 || AsSpeed > 1000) AsSpeed = 100;
    if (MiddleMode > 2) MiddleMode = 1;
    RegString("BackKeys", "Alt+Left", BackName, sizeof(BackName));
    RegString("ForwardKeys", "Alt+Right", FwdName, sizeof(FwdName));
    ParseKeys(BackName, &BackKeys);
    ParseKeys(FwdName, &FwdKeys);
    InitializeCriticalSection(&CapLock);
    SetSysCursor = user != NULL ? (SSC_FN)GetProcAddress(user, "SetSystemCursor") : NULL;
    /* an earlier usbmon that was killed while it scrolled may have left its cursor */
    SystemParametersInfoA(SPI_SETCURSORS_, 0, NULL, 0);
    for (i = 0; i < 3; i++) {
        HCURSOR c = LoadCursorA(NULL, MAKEINTRESOURCEA(SysCurId[i]));
        SavedCur[i] = c != NULL ? (HCURSOR)CopyIcon((HICON)c) : NULL;
    }
    SetUnhandledExceptionFilter(CrashFilter);
    if (MiddleMode != 0) {
        ready = CreateEventA(NULL, TRUE, FALSE, NULL);
        CreateThread(NULL, 0, AsThread, ready, 0, &tid);
        WaitForSingleObject(ready, 10000);
        CloseHandle(ready);
        if (WinVer >= 4 && !NoHook && AsWnd != NULL) {
            HookReady = CreateEventA(NULL, TRUE, FALSE, NULL);
            CreateThread(NULL, 0, HookThread, NULL, 0, &tid);
            WaitForSingleObject(HookReady, 10000);
        }
        UseDriver = !UseHook && Driver;
    }
    if (Driver && WinVer < 5 && XKeysOn && (BackKeys.Vk != 0 || FwdKeys.Vk != 0)) {
        CapBase |= USBNT_CAP_XBUTTONS;
    }
    if (UseDriver) {
        CapBase |= USBNT_CAP_MIDDLE;
    }
    if (CapBase != 0) {
        HANDLE wait = OpenUsbNt();
        BtnCtl = OpenUsbNt();
        if (wait != INVALID_HANDLE_VALUE && BtnCtl != INVALID_HANDLE_VALUE) {
            ULONG now = 0;
            DWORD got = 0;
            if (DeviceIoControl(BtnCtl, IOCTL_USBNT_MOUSE_CAPTURE, &CapBase, sizeof(CapBase), &now, sizeof(now),
                                &got, NULL)) {
                BtnOn = TRUE;
                CreateThread(NULL, 0, ButtonLoop, wait, 0, &tid);
            } else {
                Event("usbnt.sys has no button channel (older driver)");
                UseDriver = FALSE;
                CapBase = 0;
            }
        }
        if (!BtnOn) {
            if (wait != INVALID_HANDLE_VALUE) CloseHandle(wait);
            if (BtnCtl != INVALID_HANDLE_VALUE) CloseHandle(BtnCtl);
            BtnCtl = INVALID_HANDLE_VALUE;
        }
    }
    if (MiddleMode != 0) {
        Event("wheel button: autoscroll (%s)", UseHook ? "mouse hook" : UseDriver ? "through usbnt.sys" :
              "not available");
    }
}

/* ------------------------------------------------------------------ */
/* Start (waits for the driver) and the window                          */
/* ------------------------------------------------------------------ */

static BOOL Wheel, NoWheel;

static DWORD WINAPI StartThread(LPVOID Param)
{
    HANDLE dev;
    DWORD tid;
    int i;

    (void)Param;
    /* the driver may still be starting right after boot */
    for (i = 0; i < 60; i++) {
        dev = OpenUsbNt();
        if (dev != INVALID_HANDLE_VALUE) {
            CloseHandle(dev);
            break;
        }
        Sleep(1000);
    }
    if (i == 60) {
        DriverState = 2;
        Event("the USB driver (usbnt.sys) is not running");
        MouseSetup(FALSE);                      /* autoscroll through the mouse hook still works */
        return 1;
    }
    DriverState = 1;
    if (RegDword(HKEY_LOCAL_MACHINE, "Software\\UsbNt", "TimerResolution", 1)) {
        HMODULE mm = LoadLibraryA("winmm.dll");
        if (mm != NULL) {
            TBP_FN tbp = (TBP_FN)GetProcAddress(mm, "timeBeginPeriod");
            if (tbp != NULL && tbp(1) == 0) {
                TimerOn = TRUE;
            }
        }
    }
    if (RegDword(HKEY_LOCAL_MACHINE, "Software\\UsbNt", "DriveLetters", 1)) {
        LettersOn = TRUE;
        CreateThread(NULL, 0, LetterThread, NULL, 0, &tid);
    }
    if (!NoWheel) {
        /* the vertical wheel where Windows has no message for it (or /wheel), the horizontal
           one always */
        WheelOn = Wheel;
        WheelVert = Wheel;
        CreateThread(NULL, 0, WheelLoop, NULL, 0, &tid);
    }
    MouseSetup(TRUE);
    Event("started");
    return 0;
}

#define ID_HIDE     1
#define ID_STOP     2
#define ID_TREE     3

static void Refresh(void)
{
    static char text[2048];
    int i;

    lstrcpyA(text, DriverState == 1 ? "USB driver: running\r\n" : DriverState == 2 ?
             "USB driver: NOT running (usbnt.sys is not installed or did not start)\r\n" :
             "USB driver: waiting for usbnt.sys...\r\n");
    wsprintfA(text + lstrlenA(text), "Drive letters of USB disks and SD cards: %s\r\n",
              LettersOn ? "follow the disks" : "fixed (DriveLetters = 0)");
    wsprintfA(text + lstrlenA(text), "Mouse wheel: %s\r\n", WheelOn ? "turned into scrolling for this NT version" :
              NoWheel ? "Windows handles it" : "Windows handles it; the horizontal wheel is WM_HSCROLL");
    wsprintfA(text + lstrlenA(text), "Wheel button: %s\r\n", MiddleMode == 0 ? "middle click (MiddleButton = 0)" :
              UseHook ? "autoscroll (mouse hook: every mouse)" : UseDriver ? "autoscroll (mice of usbnt.sys)" :
              "middle click (autoscroll not available)");
    if (WinVer >= 5) {
        lstrcatA(text, "Back / forward buttons: Windows handles them\r\n");
    } else if (CapBase & USBNT_CAP_XBUTTONS) {
        wsprintfA(text + lstrlenA(text), "Back / forward buttons: %s / %s (%ld pressed)\r\n",
                  BackKeys.Vk ? BackName : "-", FwdKeys.Vk ? FwdName : "-", XCount);
    } else {
        lstrcatA(text, "Back / forward buttons: off\r\n");
    }
    wsprintfA(text + lstrlenA(text), "1 ms timer: %s\r\n\r\n", TimerOn ? "on" : "not available");
    for (i = 0; i < USBNT_MAX_SLOTS; i++) {
        if (SlotText[i][0]) {
            char l[27];
            int n = Map[i].Disk >= 0 ? DevMapSlotLetters(&Map[i], l, NULL, 26) : 0, k;
            if (n == 0 && Map[i].Path[0] && (l[0] = DevMapLetter(Map[i].Path)) != 0) {
                n = 1;                          /* CD-ROM */
            }
            lstrcatA(text, SlotText[i]);
            for (k = 0; k < n; k++) {
                wsprintfA(text + lstrlenA(text), "%s%c:", k ? " " : "  ->  ", l[k]);
            }
            lstrcatA(text, "\r\n");
        }
    }
    lstrcatA(text, "\r\n");
    EnterCriticalSection(&EvLock);
    for (i = NEv - 1; i >= 0; i--) {                    /* newest first */
        lstrcatA(text, Events[i]);
        lstrcatA(text, "\r\n");
    }
    LeaveCriticalSection(&EvLock);
    SetWindowTextA(InfoWnd, text);
}

static LRESULT CALLBACK MonProc(HWND Wnd, UINT Msg, WPARAM Wp, LPARAM Lp)
{
    switch (Msg) {
    case WM_CREATE: {
        HFONT font = (HFONT)GetStockObject(ANSI_VAR_FONT);
        HWND b;
        InfoWnd = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                                ES_READONLY | ES_AUTOVSCROLL, 8, 8, 524, 240, Wnd, NULL, NULL, NULL);
        SendMessageA(InfoWnd, WM_SETFONT, (WPARAM)font, 0);
        b = CreateWindowA("BUTTON", "&Hide", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 8, 256, 90, 24, Wnd,
                          (HMENU)ID_HIDE, NULL, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)font, 0);
        b = CreateWindowA("BUTTON", "&USB Devices...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 106, 256, 110, 24, Wnd,
                          (HMENU)ID_TREE, NULL, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)font, 0);
        b = CreateWindowA("BUTTON", "&Stop the monitor", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 402, 256, 130, 24,
                          Wnd, (HMENU)ID_STOP, NULL, NULL);
        SendMessageA(b, WM_SETFONT, (WPARAM)font, 0);
        SetTimer(Wnd, 1, 1000, NULL);
        return 0;
    }
    case WM_DEVICECHANGE_:
        /* Windows 2000 / XP: a new disk of Windows' USB storage driver may want the usbnt filter */
        if (Wp == DBT_DEVICEARRIVAL_ && (GetVersion() & 0x80000000) == 0 && (GetVersion() & 0xFF) >= 5) {
            SetTimer(Wnd, 2, 2000, NULL);
        }
        return TRUE;
    case WM_TIMER:
        if (Wp == 2) {
            char exe[MAX_PATH + 32];
            int n;
            KillTimer(Wnd, 2);
            n = (int)GetModuleFileNameA(NULL, exe, MAX_PATH);
            while (n > 0 && exe[n - 1] != '\\') n--;
            if (n > 0 && RegDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\usbnt\\Parameters",
                                  "RemovableAsFixed", 1) != 0) {
                lstrcpyA(exe + n, "usbinst.exe /filters /quiet");
                WinExec(exe, SW_HIDE);
            }
            return 0;
        }
        if (IsWindowVisible(Wnd)) Refresh();
        return 0;
    case WM_USER + 11:
        if (IsWindowVisible(Wnd)) Refresh();
        return 0;
    case WM_USER + 12:                      /* usbinst replaces usbmon.exe */
        MouseShutdown();
        ExitProcess(0);
        return 0;
    case WM_USER + 10:                      /* usbmon /show from another copy */
        Refresh();
        ShowWindow(Wnd, SW_SHOWNORMAL);
        SetForegroundWindow(Wnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(Wp)) {
        case ID_HIDE:
            ShowWindow(Wnd, SW_HIDE);
            break;
        case ID_TREE:
            WinExec("usbtree.exe", SW_SHOWNORMAL);
            break;
        case ID_STOP:
            if (MessageBoxA(Wnd, "Stop the USB monitor? Drive letters of USB disks are then no longer updated "
                            "until the next logon.", "USB Monitor", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                MouseShutdown();
                ExitProcess(0);
            }
            break;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(Wnd, SW_HIDE);           /* keeps running */
        return 0;
    case WM_ENDSESSION:
        if (Wp) MouseShutdown();
        return 0;
    }
    return DefWindowProcA(Wnd, Msg, Wp, Lp);
}

void __stdcall Entry(void)
{
    const char *cmd = GetCommandLineA();
    BOOL nt351 = (BOOL)((GetVersion() & 0xFF) < 4);
    BOOL show = HasArg(cmd, "/show");
    HANDLE mutex = NULL;
    HWND other;
    WNDCLASSA wc;
    MSG msg;
    DWORD tid;

    NoWheel = HasArg(cmd, "/nowheel");
    Wheel = (nt351 || HasArg(cmd, "/wheel")) && !NoWheel;
    WinVer = GetVersion() & 0xFF;
    NoHook = HasArg(cmd, "/nohook");
    if (HasArg(cmd, "/restart")) {
        /* the running copy ends (it puts its cursors back), this one takes over
           with the settings as they are now */
        int i;
        for (i = 0; i < 50 && (other = FindWindowA("UsbNtMonitor", NULL)) != NULL; i++) {
            PostMessageA(other, WM_USER + 12, 0, 0);
            Sleep(100);
        }
        for (i = 0; i < 30; i++) {
            mutex = CreateMutexA(NULL, TRUE, "UsbNtHelper");
            if (mutex == NULL || GetLastError() != ERROR_ALREADY_EXISTS) {
                break;
            }
            CloseHandle(mutex);
            mutex = NULL;
            Sleep(100);
        }
    }
    other = FindWindowA("UsbNtMonitor", NULL);
    if (other != NULL) {
        /* already running: started from the program group, show its window */
        PostMessageA(other, WM_USER + 10, 0, 0);
        ExitProcess(0);
    }
    if (mutex == NULL) {
        mutex = CreateMutexA(NULL, TRUE, "UsbNtHelper");
        if (mutex != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
            ExitProcess(0);
        }
    }
    InitializeCriticalSection(&EvLock);
    wc.style = 0;
    wc.lpfnWndProc = MonProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hIcon = LoadIconA(GetModuleHandleA(NULL), MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "UsbNtMonitor";
    RegisterClassA(&wc);
    MonWnd = CreateWindowA("UsbNtMonitor", "USB Monitor", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 548, 318, NULL, NULL, wc.hInstance, NULL);
    CreateThread(NULL, 0, StartThread, NULL, 0, &tid);
    if (show) {
        PostMessageA(MonWnd, WM_USER + 10, 0, 0);
    }
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (!IsDialogMessageA(MonWnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    MouseShutdown();
    ExitProcess(0);
}
