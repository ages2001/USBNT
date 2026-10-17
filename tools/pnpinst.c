/*
 * pnpinst.c - usbinst on Windows 2000 / XP (x86 and x64): Plug and Play
 * installation of the WDM driver from the 2kxp folder next to usbinst.exe.
 *
 *  - USBNT INF files of an earlier installation are taken out of the INF
 *    folder, the new ones go in (SetupCopyOEMInf)
 *  - the USBNT device bus is created (root enumerated, USBNT\Bus) - not
 *    where text-mode setup installed the driver (TXTSETUP.OEM): there the
 *    first host controller is the bus (the system may boot from its disks)
 *  - every present device that usbnt.inf names gets usbnt.sys - but USB host
 *    controllers only as the controller policy says:
 *      UHCI, OHCI   Windows' own driver (2000: uhcd / openhci, XP: usbuhci /
 *                   usbohci), which both systems have
 *      EHCI         Windows' driver where Windows has one (2000 SP4 and
 *                   later, XP SP1 and later, XP x64 / Server 2003, or a USB
 *                   2.0 package that put usbehci.sys in), otherwise USBNT
 *      xHCI, USB4   USBNT (Windows 2000 / XP have no driver)
 *    /all gives every controller to USBNT (as earlier test builds did), /ehci the EHCI
 *    controllers, /noehci none of them.  A controller an earlier installation
 *    gave to USBNT that the policy now leaves to Windows gets Windows' driver
 *    back.  Card readers, Thunderbolt / USB4 host interfaces and I2C
 *    controllers are always USBNT's.
 *  - the driver parameters that follow from it: IntelRouteToXhci (2 when
 *    Windows drives the EHCI: only SuperSpeed goes to xHCI), WindowsCamera
 *    (1 when Windows has its camera driver usbvideo.sys)
 *  - the keyboard and mouse of the bus get their driver
 *  - i8042prt's PS/2 mouse gets usbnt as an upper filter (touch pads, see
 *    wdmps2.c) unless /nops2 is given
 *  - the disks of Windows' USB storage driver (USBSTOR\...) get usbnt as a
 *    lower filter of disk.sys (RemovableAsFixed, see wdmflt.c); usbinst
 *    /filters does that for disks that came later (usbmon runs it)
 *
 * Removal gives the devices back to Windows' drivers.  SetupAPI cannot
 * install devices from a 32 bit program on 64 bit Windows: there the 64 bit
 * usbinst.exe (2kxp\amd64) does the work.
 *
 * SetupAPI and newdev are loaded at run time: usbinst.exe also runs on NT 3.1.
 */

#include <windows.h>
#include <setupapi.h>

void PnpPrint(const char *Fmt, ...);
#define Print PnpPrint

#ifndef INSTALLFLAG_FORCE
#define INSTALLFLAG_FORCE 0x00000001
#endif
#ifndef DICD_GENERATE_ID
#define DICD_GENERATE_ID 0x00000001
#endif

typedef BOOL (WINAPI *P_UpdateDriver)(HWND, LPCSTR, LPCSTR, DWORD, PBOOL);
typedef BOOL (WINAPI *P_CopyOEMInf)(PCSTR, PCSTR, DWORD, DWORD, PSTR, DWORD, PDWORD, PSTR *);
typedef HDEVINFO (WINAPI *P_GetClassDevs)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *P_EnumDeviceInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_GetDevRegProp)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *P_SetDevRegProp)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, const BYTE *, DWORD);
typedef HDEVINFO (WINAPI *P_CreateList)(const GUID *, HWND);
typedef BOOL (WINAPI *P_CreateInfo)(HDEVINFO, PCSTR, const GUID *, PCSTR, HWND, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_CallClassInstaller)(DI_FUNCTION, HDEVINFO, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_DestroyList)(HDEVINFO);
typedef BOOL (WINAPI *P_UninstallOEMInf)(PCSTR, DWORD, PVOID);
typedef DWORD (WINAPI *P_LocateDevNode)(PDWORD, PCSTR, ULONG);
typedef DWORD (WINAPI *P_Reenumerate)(DWORD, ULONG);
typedef BOOL (WINAPI *P_BuildDrivers)(HDEVINFO, PSP_DEVINFO_DATA, DWORD);
typedef BOOL (WINAPI *P_EnumDriver)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, DWORD, PSP_DRVINFO_DATA_V2_A);
typedef BOOL (WINAPI *P_DriverParams)(HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_V2_A, PSP_DRVINSTALL_PARAMS);
typedef BOOL (WINAPI *P_DriverDetail)(HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_V2_A, PSP_DRVINFO_DETAIL_DATA_A, DWORD,
                                      PDWORD);
typedef BOOL (WINAPI *P_SelectDriver)(HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_V2_A);
typedef BOOL (WINAPI *P_DestroyDrivers)(HDEVINFO, PSP_DEVINFO_DATA, DWORD);
typedef BOOL (WINAPI *P_DevParams)(HDEVINFO, PSP_DEVINFO_DATA, PSP_DEVINSTALL_PARAMS_A);
typedef BOOL (WINAPI *P_SetClassInstallParams)(HDEVINFO, PSP_DEVINFO_DATA, PSP_CLASSINSTALL_HEADER, DWORD);
typedef BOOL (WINAPI *P_GetInstanceId)(HDEVINFO, PSP_DEVINFO_DATA, PSTR, DWORD, PDWORD);

static P_UpdateDriver UpdateDriver;
static P_CopyOEMInf CopyOEMInf;
static P_GetClassDevs GetClassDevs;
static P_EnumDeviceInfo EnumDeviceInfo;
static P_GetDevRegProp GetDevRegProp;
static P_SetDevRegProp SetDevRegProp;
static P_CreateList CreateList;
static P_CreateInfo CreateInfo;
static P_CallClassInstaller CallClassInstaller;
static P_DestroyList DestroyList;
static P_UninstallOEMInf UninstallOEMInf;
static P_LocateDevNode LocateDevNode;
static P_Reenumerate Reenumerate;
static P_BuildDrivers BuildDrivers;
static P_EnumDriver EnumDriver;
static P_DriverParams DriverParams;
static P_DriverDetail DriverDetail;
static P_SelectDriver SelectDriver;
static P_DestroyDrivers DestroyDrivers;
static P_DevParams GetDevParams;
static P_SetClassInstallParams SetClassInstallParams;
static P_GetInstanceId GetInstanceId;

/* no C runtime: ZeroMemory needs memset */
void *memset(void *D, int C, size_t N)
{
    volatile unsigned char *d = (volatile unsigned char *)D;

    while (N-- > 0) {
        *d++ = (unsigned char)C;
    }
    return D;
}

static const GUID GuidUsb = { 0x36FC9E60, 0xC465, 0x11CF, { 0x80, 0x56, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const char *Infs[] = { "usbnt.inf", "usbntkbd.inf", "usbntmou.inf" };

static BOOL Load(void)
{
    HMODULE s = LoadLibraryA("setupapi.dll");
    HMODULE n = LoadLibraryA("newdev.dll");

    if (s == NULL || n == NULL) {
        Print("SetupAPI is not available (error %lu)\r\n", GetLastError());
        return FALSE;
    }
    UpdateDriver = (P_UpdateDriver)GetProcAddress(n, "UpdateDriverForPlugAndPlayDevicesA");
    CopyOEMInf = (P_CopyOEMInf)GetProcAddress(s, "SetupCopyOEMInfA");
    GetClassDevs = (P_GetClassDevs)GetProcAddress(s, "SetupDiGetClassDevsA");
    EnumDeviceInfo = (P_EnumDeviceInfo)GetProcAddress(s, "SetupDiEnumDeviceInfo");
    GetDevRegProp = (P_GetDevRegProp)GetProcAddress(s, "SetupDiGetDeviceRegistryPropertyA");
    SetDevRegProp = (P_SetDevRegProp)GetProcAddress(s, "SetupDiSetDeviceRegistryPropertyA");
    CreateList = (P_CreateList)GetProcAddress(s, "SetupDiCreateDeviceInfoList");
    CreateInfo = (P_CreateInfo)GetProcAddress(s, "SetupDiCreateDeviceInfoA");
    CallClassInstaller = (P_CallClassInstaller)GetProcAddress(s, "SetupDiCallClassInstaller");
    DestroyList = (P_DestroyList)GetProcAddress(s, "SetupDiDestroyDeviceInfoList");
    UninstallOEMInf = (P_UninstallOEMInf)GetProcAddress(s, "SetupUninstallOEMInfA");    /* XP and later */
    LocateDevNode = (P_LocateDevNode)GetProcAddress(s, "CM_Locate_DevNodeA");
    Reenumerate = (P_Reenumerate)GetProcAddress(s, "CM_Reenumerate_DevNode");
    BuildDrivers = (P_BuildDrivers)GetProcAddress(s, "SetupDiBuildDriverInfoList");
    EnumDriver = (P_EnumDriver)GetProcAddress(s, "SetupDiEnumDriverInfoA");
    DriverParams = (P_DriverParams)GetProcAddress(s, "SetupDiGetDriverInstallParamsA");
    DriverDetail = (P_DriverDetail)GetProcAddress(s, "SetupDiGetDriverInfoDetailA");
    SelectDriver = (P_SelectDriver)GetProcAddress(s, "SetupDiSetSelectedDriverA");
    DestroyDrivers = (P_DestroyDrivers)GetProcAddress(s, "SetupDiDestroyDriverInfoList");
    GetDevParams = (P_DevParams)GetProcAddress(s, "SetupDiGetDeviceInstallParamsA");
    SetClassInstallParams = (P_SetClassInstallParams)GetProcAddress(s, "SetupDiSetClassInstallParamsA");
    GetInstanceId = (P_GetInstanceId)GetProcAddress(s, "SetupDiGetDeviceInstanceIdA");
    if (UpdateDriver == NULL || CopyOEMInf == NULL || GetClassDevs == NULL || EnumDeviceInfo == NULL ||
        GetDevRegProp == NULL || SetDevRegProp == NULL || CreateList == NULL || CreateInfo == NULL ||
        CallClassInstaller == NULL || DestroyList == NULL || BuildDrivers == NULL || EnumDriver == NULL ||
        DriverParams == NULL || DriverDetail == NULL || SelectDriver == NULL || DestroyDrivers == NULL ||
        GetDevParams == NULL) {
        Print("SetupAPI lacks a function this program needs\r\n");
        return FALSE;
    }
    return TRUE;
}

static BOOL SameNoCase(const char *A, const char *B)
{
    while (*A && *B) {
        char a = *A++, b = *B++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) {
            return FALSE;
        }
    }
    return *A == 0 && *B == 0;
}

static BOOL Contains(const char *Text, const char *Part)
{
    int n = lstrlenA(Part);
    const char *p;

    for (p = Text; *p; p++) {
        int i;
        for (i = 0; i < n && p[i] == Part[i]; i++) {
        }
        if (i == n) {
            return TRUE;
        }
    }
    return FALSE;
}

/* the hardware ids usbnt.inf names: "%x.Desc% = Section, ID" lines of the [USBNT] section */
#define MAX_IDS 512
static char IdBuf[32768];
static char *Ids[MAX_IDS];
static int NIds;

static BOOL ReadIds(const char *Inf)
{
    HANDLE f = CreateFileA(Inf, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    char *p, *line;
    BOOL in = FALSE;

    if (f == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    ReadFile(f, IdBuf, sizeof(IdBuf) - 1, &n, NULL);
    CloseHandle(f);
    IdBuf[n] = 0;
    for (p = IdBuf; *p && NIds < MAX_IDS;) {
        line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        while (*p == '\r' || *p == '\n') *p++ = 0;
        if (line[0] == '[') {
            in = SameNoCase(line, "[USBNT]");
            continue;
        }
        if (in && line[0] == '%') {
            char *c = line;
            while (*c && *c != ',') c++;
            if (*c == ',') {
                c++;
                while (*c == ' ') c++;
                Ids[NIds++] = c;
            }
        }
    }
    return NIds > 0;
}

static BOOL Named(const char *MultiSz)
{
    const char *s;
    int i;

    for (s = MultiSz; *s; s += lstrlenA(s) + 1) {
        for (i = 0; i < NIds; i++) {
            if (SameNoCase(s, Ids[i])) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL BusExists(void)
{
    HDEVINFO h = GetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    SP_DEVINFO_DATA d;
    char buf[1024];
    DWORD i, type;
    BOOL found = FALSE;

    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    for (i = 0; !found; i++) {
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) {
            break;
        }
        ZeroMemory(buf, sizeof(buf));
        if (GetDevRegProp(h, &d, SPDRP_HARDWAREID, &type, (PBYTE)buf, sizeof(buf) - 2, NULL) &&
            SameNoCase(buf, "USBNT\\Bus")) {
            found = TRUE;
        }
    }
    DestroyList(h);
    return found;
}

/* text-mode setup installed usbnt.sys from the driver disk (TXTSETUP.OEM [Config.usbnt]) */
static BOOL FromTextSetup(void)
{
    HKEY k;
    DWORD v = 0, type = 0, len = sizeof(v);

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\usbnt\\Parameters", 0, KEY_READ,
                      &k) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (RegQueryValueExA(k, "OemSetup", NULL, &type, (LPBYTE)&v, &len) != ERROR_SUCCESS || type != REG_DWORD) {
        v = 0;
    }
    RegCloseKey(k);
    return v == 1;
}

static BOOL MakeBus(void)
{
    HDEVINFO h = CreateList(&GuidUsb, NULL);
    SP_DEVINFO_DATA d;
    static const char hw[] = "USBNT\\Bus\0";
    BOOL ok;

    if (h == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    d.cbSize = sizeof(d);
    ok = CreateInfo(h, "USB", &GuidUsb, NULL, NULL, DICD_GENERATE_ID, &d) &&
         SetDevRegProp(h, &d, SPDRP_HARDWAREID, (const BYTE *)hw, sizeof(hw)) &&
         CallClassInstaller(DIF_REGISTERDEVICE, h, &d);
    if (!ok) {
        Print("  cannot create the USBNT device bus (error %lu)\r\n", GetLastError());
    }
    DestroyList(h);
    return ok;
}

/* usbnt.sys for the device with this id (and the others that have it), also over Windows' own driver */
static BOOL Update(const char *Id, const char *Inf, BOOL *Reboot)
{
    BOOL r = FALSE;

    if (UpdateDriver(NULL, Id, Inf, INSTALLFLAG_FORCE, &r)) {
        if (r) {
            *Reboot = TRUE;
        }
        return TRUE;
    }
    return FALSE;
}

/* Dir ends with a backslash: the INF files are in Dir\2kxp (usbinst.exe of the
   release), Dir (usbinst.exe inside 2kxp) or Dir\.. (2kxp\amd64\usbinst.exe) */
static void InfPath(char *Out, const char *Dir, const char *Name)
{
    static const char *where[] = { "2kxp\\", "", "..\\" };
    int i;

    for (i = 0; i < 3; i++) {
        char full[MAX_PATH], *part;
        wsprintfA(full, "%s%s%s", Dir, where[i], Name);
        if (GetFullPathNameA(full, MAX_PATH, Out, &part) == 0) {
            lstrcpyA(Out, full);
        }
        if (GetFileAttributesA(Out) != 0xFFFFFFFF) {
            return;
        }
    }
    wsprintfA(Out, "%s2kxp\\%s", Dir, Name);
}

BOOL PnpWow64(void)
{
    typedef BOOL (WINAPI *P_IsWow64)(HANDLE, PBOOL);
    P_IsWow64 f = (P_IsWow64)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process");
    BOOL w = FALSE;

    return f != NULL && f(GetCurrentProcess(), &w) && w;
}

/* 64 bit Windows: the 64 bit usbinst.exe does it */
static int RunNative(const char *Dir, const char *Cmd)
{
    char exe[MAX_PATH], line[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1;
    const char *args = Cmd;

    /* the arguments after the program name */
    if (*args == '"') {
        args++;
        while (*args && *args != '"') args++;
        if (*args) args++;
    } else {
        while (*args && *args != ' ') args++;
    }
    wsprintfA(exe, "%s2kxp\\amd64\\usbinst.exe", Dir);
    if (GetFileAttributesA(exe) == 0xFFFFFFFF) {
        wsprintfA(exe, "%samd64\\usbinst.exe", Dir);
    }
    {
        /* the copy the 64 bit usbinst left in the 64 bit system folder (usbmon runs usbinst
           /filters from the system folder); a 32 bit program sees that folder only with the file
           system redirection off */
        typedef BOOL (WINAPI *P_Redir)(PVOID *);
        HMODULE k = GetModuleHandleA("kernel32.dll");
        P_Redir off = (P_Redir)GetProcAddress(k, "Wow64DisableWow64FsRedirection");
        P_Redir on = (P_Redir)GetProcAddress(k, "Wow64RevertWow64FsRedirection");
        PVOID old = NULL;
        char win[MAX_PATH];
        BOOL ok = FALSE;
        if (GetFileAttributesA(exe) == 0xFFFFFFFF && off != NULL && on != NULL && GetWindowsDirectoryA(win, MAX_PATH)) {
            wsprintfA(exe, "%s\\System32\\usbinst64.exe", win);
            if (off(&old)) {
                wsprintfA(line, "\"%s\"%s /pnp", exe, args);
                ZeroMemory(&si, sizeof(si));
                si.cb = sizeof(si);
                ok = GetFileAttributesA(exe) != 0xFFFFFFFF &&
                     CreateProcessA(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
                on(old);
            }
            if (ok) {
                goto started;
            }
        }
    }
    if (GetFileAttributesA(exe) == 0xFFFFFFFF) {
        Print("This is 64 bit Windows: 2kxp\\amd64\\usbinst.exe is needed and missing.\r\n");
        return 1;
    }
    wsprintfA(line, "\"%s\"%s /pnp", exe, args);
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessA(exe, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        Print("Cannot start %s (error %lu)\r\n", exe, GetLastError());
        return 1;
    }
started:
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)rc;
}

/* ------------------------------------------------------------------ */
/* What Windows has                                                     */
/* ------------------------------------------------------------------ */

static BOOL FileExists(const char *Path)
{
    return GetFileAttributesA(Path) != 0xFFFFFFFF;
}

/* lstrcpynA is missing on NT 3.1 */
static void Cpyn(char *D, const char *S, int N)
{
    int i;

    for (i = 0; i + 1 < N && S[i]; i++) {
        D[i] = S[i];
    }
    if (N > 0) {
        D[i] = 0;
    }
}

/* Windows version and service pack (GetVersionExA is looked up: usbinst.exe also runs on NT 3.1) */
static void WinVersion(DWORD *Maj, DWORD *Min, DWORD *Sp)
{
    typedef BOOL (WINAPI *P_GetVersionEx)(OSVERSIONINFOA *);
    P_GetVersionEx gv = (P_GetVersionEx)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetVersionExA");
    OSVERSIONINFOEXA v;

    ZeroMemory(&v, sizeof(v));
    v.dwOSVersionInfoSize = sizeof(v);
    *Maj = 5;
    *Min = 0;
    *Sp = 0;
    if (gv != NULL && gv((OSVERSIONINFOA *)&v)) {
        *Maj = v.dwMajorVersion;
        *Min = v.dwMinorVersion;
        *Sp = v.wServicePackMajor;
    }
}

/*
 * Windows has a USB 2.0 (EHCI) driver: Windows 2000 from SP4 on (usb2.inf, usbehci.sys, usbhub20.sys),
 * XP from SP1 on (usbport.inf), XP x64 and Server 2003 always.  Windows 2000 SP0 - SP3 and XP RTM have
 * none in the box; the USB 2.0 packages of the time (Windows Update / OEMs) put usbehci.sys in.
 */
static BOOL EhciByOs(void)
{
    DWORD maj, min, sp;
    char f[MAX_PATH];

    WinVersion(&maj, &min, &sp);
    if (maj > 5 || (maj == 5 && min >= 2) || (maj == 5 && min == 1 && sp >= 1) || (maj == 5 && min == 0 && sp >= 4)) {
        return TRUE;
    }
    GetSystemDirectoryA(f, sizeof(f));
    lstrcatA(f, "\\drivers\\usbehci.sys");
    return FileExists(f);
}

/* Windows' camera driver (usbvideo.sys, USB Video Class): XP SP2 and later, Server 2003 SP1 and XP x64 */
static BOOL UvcByOs(void)
{
    char f[MAX_PATH];

    GetWindowsDirectoryA(f, sizeof(f));
    lstrcatA(f, "\\inf\\usbvideo.inf");
    return FileExists(f);
}

/* "/set Name=" on the command line: the user decides that parameter */
static BOOL UserSets(const char *Cmd, const char *Name)
{
    char pat[80];
    int n, i;
    const char *p;

    wsprintfA(pat, "/set %s=", Name);
    n = lstrlenA(pat);
    for (p = Cmd; *p; p++) {
        for (i = 0; i < n && p[i]; i++) {
            char a = p[i], b = pat[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) {
                break;
            }
        }
        if (i == n) {
            return TRUE;
        }
    }
    return FALSE;
}

static BOOL Switch(const char *Cmd, const char *Arg)
{
    int n = lstrlenA(Arg), i;
    const char *p;

    for (p = Cmd; *p; p++) {
        for (i = 0; i < n; i++) {
            char a = p[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (a != Arg[i]) {
                break;
            }
        }
        if (i == n && (p[n] == 0 || p[n] == ' ' || p[n] == '\t')) {
            return TRUE;
        }
    }
    return FALSE;
}

#define PARAMS "SYSTEM\\CurrentControlSet\\Services\\usbnt\\Parameters"

/* a driver parameter; TRUE when it changed */
static BOOL SetParam(const char *Name, DWORD Value)
{
    HKEY k;
    DWORD old = 0xFFFFFFFF, t = 0, n = sizeof(old);
    BOOL changed;

    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, PARAMS, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (RegQueryValueExA(k, Name, NULL, &t, (BYTE *)&old, &n) != ERROR_SUCCESS || t != REG_DWORD) {
        old = 0xFFFFFFFF;
    }
    changed = old != Value;
    RegSetValueExA(k, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
    RegCloseKey(k);
    return changed;
}

/* WindowsCamera: cameras go to Windows' usbvideo.sys where Windows has it (or as /set says) */
BOOL PnpWindowsCamera(const char *Cmd)
{
    DWORD v = 0, t = 0, n = sizeof(v);
    HKEY k;

    if (UserSets(Cmd, "WindowsCamera")) {
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, PARAMS, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "WindowsCamera", NULL, &t, (BYTE *)&v, &n) != ERROR_SUCCESS || t != REG_DWORD) {
                v = 0;
            }
            RegCloseKey(k);
        }
        return v != 0;
    }
    return UvcByOs();
}

/* ------------------------------------------------------------------ */
/* Controller policy                                                    */
/* ------------------------------------------------------------------ */

enum { K_OTHER, K_UHCI, K_OHCI, K_EHCI, K_XHCI, K_USB4, K_NONE };
static const char *KindName[] = { "", "UHCI", "OHCI", "EHCI", "xHCI", "USB4", "USB" };

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* the controller type from the class code in the ids (PCI\CC_0C03pp, ...&CC_0C03pp) */
static int KindOf(const char *Hw, const char *Cp)
{
    const char *lists[2], *s;
    int l;

    lists[0] = Hw;
    lists[1] = Cp;
    for (l = 0; l < 2; l++) {
        for (s = lists[l]; *s; s += lstrlenA(s) + 1) {
            const char *p;
            for (p = s; *p; p++) {
                if ((p[0] == 'C' || p[0] == 'c') && (p[1] == 'C' || p[1] == 'c') && p[2] == '_' && p[3] == '0' &&
                    (p[4] == 'C' || p[4] == 'c') && p[5] == '0' && p[6] == '3' && HexDigit(p[7]) >= 0 &&
                    HexDigit(p[8]) >= 0) {
                    int pi = HexDigit(p[7]) * 16 + HexDigit(p[8]);
                    return pi == 0x00 ? K_UHCI : pi == 0x10 ? K_OHCI : pi == 0x20 ? K_EHCI : pi == 0x30 ? K_XHCI :
                           pi == 0x40 ? K_USB4 : K_NONE;
                }
            }
        }
    }
    return K_OTHER;
}

/* the best driver for the device that is not USBNT's (selected in the set); FALSE: Windows has none */
static BOOL WindowsDriver(HDEVINFO H, PSP_DEVINFO_DATA D, char *Inf, char *Desc)
{
    SP_DRVINFO_DATA_V2_A dr, best;
    SP_DRVINSTALL_PARAMS ip;
    DWORD i, rank = 0xFFFFFFFF;
    static char detail[4096];

    Inf[0] = Desc[0] = 0;
    if (!BuildDrivers(H, D, SPDIT_COMPATDRIVER)) {
        return FALSE;
    }
    for (i = 0;; i++) {
        ZeroMemory(&dr, sizeof(dr));
        dr.cbSize = sizeof(dr);
        if (!EnumDriver(H, D, SPDIT_COMPATDRIVER, i, &dr)) {
            break;
        }
        if (SameNoCase(dr.ProviderName, "USBNT")) {
            continue;
        }
        ZeroMemory(&ip, sizeof(ip));
        ip.cbSize = sizeof(ip);
        if (!DriverParams(H, D, &dr, &ip)) {
            continue;
        }
        if (ip.Rank < rank) {
            rank = ip.Rank;
            best = dr;
        }
    }
    if (rank == 0xFFFFFFFF) {
        return FALSE;
    }
    if (!SelectDriver(H, D, &best)) {
        return FALSE;
    }
    {
        PSP_DRVINFO_DETAIL_DATA_A dd = (PSP_DRVINFO_DETAIL_DATA_A)detail;
        DWORD need = 0;
        ZeroMemory(detail, sizeof(detail));
        dd->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_A);
        if (DriverDetail(H, D, &best, dd, sizeof(detail), &need)) {
            const char *b = dd->InfFileName, *q;
            for (q = b; *q; q++) {
                if (*q == '\\') b = q + 1;
            }
            Cpyn(Inf, b, 64);
        }
    }
    Cpyn(Desc, best.Description, 128);
    return TRUE;
}

/* Windows' driver (selected by WindowsDriver) goes on the device */
static BOOL InstallWindows(HDEVINFO H, PSP_DEVINFO_DATA D, BOOL *Reboot)
{
    SP_DEVINSTALL_PARAMS_A dp;

    if (!CallClassInstaller(DIF_INSTALLDEVICE, H, D)) {
        return FALSE;
    }
    ZeroMemory(&dp, sizeof(dp));
    dp.cbSize = sizeof(dp);
    if (GetDevParams(H, D, &dp) && (dp.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) {
        *Reboot = TRUE;
    }
    return TRUE;
}

/* usbnt.sys names its controllers itself when they start ("Renesas uPD720202 USB 3.0 Host
   Controller (USBNT)", wdmtree.c) - also where text-mode setup installed it: no name from here */
static const char *FriendlyName(int Kind)
{
    (void)Kind;
    return NULL;
}

/* the INF copies of USBNT (oemN.inf whose provider is USBNT) leave the INF folder */
static void RemoveOurInfs(void)
{
    char win[MAX_PATH], pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE f;

    GetWindowsDirectoryA(win, sizeof(win));
    wsprintfA(pat, "%s\\inf\\oem*.inf", win);
    f = FindFirstFileA(pat, &fd);
    while (f != INVALID_HANDLE_VALUE) {
        HANDLE fh;
        char head[1024];
        DWORD got = 0;
        wsprintfA(path, "%s\\inf\\%s", win, fd.cFileName);
        fh = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (fh != INVALID_HANDLE_VALUE) {
            ReadFile(fh, head, sizeof(head) - 1, &got, NULL);
            CloseHandle(fh);
            head[got] = 0;
            if (got > 0 && Contains(head, "Provider=%USBNT%")) {
                if (UninstallOEMInf == NULL || !UninstallOEMInf(fd.cFileName, 1, NULL)) {
                    int l = lstrlenA(path);
                    DeleteFileA(path);
                    path[l - 3] = 'p';                  /* the .pnf next to it */
                    DeleteFileA(path);
                }
                Print("  %s (USBNT) removed\r\n", fd.cFileName);
            }
        }
        if (!FindNextFileA(f, &fd)) {
            FindClose(f);
            break;
        }
    }
}

static BOOL ServiceRunning(const char *Name)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT), svc;
    SERVICE_STATUS st;
    BOOL run = FALSE;

    if (scm == NULL) {
        return FALSE;
    }
    svc = OpenServiceA(scm, Name, SERVICE_QUERY_STATUS);
    if (svc != NULL) {
        run = QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return run;
}

/* what the policy says: TRUE = USBNT; *Why for the message */
#define POL_AUTO    0
#define POL_ALL     1
#define POL_EHCI    2
#define POL_NOEHCI  3

static int Policy(const char *Cmd)
{
    if (Switch(Cmd, "/all")) return POL_ALL;
    if (Switch(Cmd, "/noehci")) return POL_NOEHCI;
    if (Switch(Cmd, "/ehci")) return POL_EHCI;
    return POL_AUTO;
}

/* D_USBNT: usbnt.sys; D_WINDOWS: Windows' driver; D_NONE: no driver (EHCI with /noehci and no Windows driver);
   D_SKIP: not a device of ours */
enum { D_SKIP, D_USBNT, D_WINDOWS, D_NONE };

static int Decide(int Pol, int Kind, BOOL HaveWin, BOOL EhciOs)
{
    switch (Kind) {
    case K_NONE:
        return D_SKIP;
    case K_UHCI:
    case K_OHCI:
        return (Pol == POL_ALL || !HaveWin) ? D_USBNT : D_WINDOWS;
    case K_EHCI:
        if (Pol == POL_ALL || Pol == POL_EHCI) return D_USBNT;
        if (Pol == POL_NOEHCI) return HaveWin ? D_WINDOWS : D_NONE;
        return (EhciOs && HaveWin) ? D_WINDOWS : D_USBNT;
    default:
        return D_USBNT;
    }
}

#define MAX_DEVS 512

int PnpInstall(const char *Dir, const char *Cmd)
{
    static char inf[MAX_PATH], oem[MAX_PATH], buf[2048], hw[1024], cp[1024], svc[64], winf[64], wdesc[128];
    static UCHAR decision[MAX_DEVS], kinds[MAX_DEVS];
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type, maj, min, sp;
    BOOL reboot = FALSE, ehciOs, running, ehciWin = FALSE, cam;
    int k, done = 0, pol = Policy(Cmd);
    char *seen[64];
    int nseen = 0;

    if (PnpWow64()) {
        return RunNative(Dir, Cmd);
    }
    if (!Load()) {
        return 1;
    }
    WinVersion(&maj, &min, &sp);
    ehciOs = EhciByOs();
    running = ServiceRunning("usbnt");
    Print("Installing the USB driver (Windows %lu.%lu SP%lu, %s)\r\n", maj, min, sp, sizeof(void *) == 8 ? "x64" : "x86");
    Print("  Windows' own drivers: UHCI / OHCI yes, EHCI %s, cameras (usbvideo.sys) %s\r\n", ehciOs ? "yes" : "no",
          UvcByOs() ? "yes" : "no");
    Print("  controllers: %s\r\n", pol == POL_ALL ? "all to USBNT (/all)" : pol == POL_EHCI ? "EHCI to USBNT (/ehci)" :
          pol == POL_NOEHCI ? "EHCI not to USBNT (/noehci)" : "Windows' drivers where Windows has one, USBNT for the rest");
    /* an older USBNT INF in the INF folder would still win Windows' own driver search for controllers */
    RemoveOurInfs();
    for (k = 0; k < 3; k++) {
        InfPath(inf, Dir, Infs[k]);
        oem[0] = 0;
        {
            /* Windows may be looking at INF files just now (a Found New Hardware Wizard): try again */
            int t;
            BOOL ok = FALSE;
            DWORD e = 0;
            for (t = 0; t < 30 && !ok; t++) {
                ok = CopyOEMInf(inf, NULL, SPOST_PATH, 0, oem, sizeof(oem), NULL, NULL);
                if (!ok) {
                    e = GetLastError();
                    if (e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) {
                        break;
                    }
                    Sleep(1000);
                }
            }
            if (!ok) {
                Print("  %s: cannot copy (error %lu)\r\n", inf, e);
                return 1;
            }
        }
        Print("  %s -> %s\r\n", Infs[k], oem);
    }
    InfPath(inf, Dir, "usbnt.inf");
    if (!ReadIds(inf)) {
        Print("  %s names no devices\r\n", inf);
        return 1;
    }

    /* what goes where */
    h = GetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) {
        Print("  cannot list the devices (error %lu)\r\n", GetLastError());
        return 1;
    }
    for (i = 0; i < MAX_DEVS; i++) {
        BOOL win = FALSE;
        decision[i] = D_SKIP;
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) {
            break;
        }
        ZeroMemory(hw, sizeof(hw));
        ZeroMemory(cp, sizeof(cp));
        GetDevRegProp(h, &d, SPDRP_HARDWAREID, &type, (PBYTE)hw, sizeof(hw) - 2, NULL);
        GetDevRegProp(h, &d, SPDRP_COMPATIBLEIDS, &type, (PBYTE)cp, sizeof(cp) - 2, NULL);
        if (hw[0] == 0 || SameNoCase(hw, "USBNT\\Bus") || (!Named(hw) && !Named(cp))) {
            continue;
        }
        kinds[i] = (UCHAR)KindOf(hw, cp);
        if (kinds[i] == K_UHCI || kinds[i] == K_OHCI || kinds[i] == K_EHCI) {
            win = WindowsDriver(h, &d, winf, wdesc);
        }
        decision[i] = (UCHAR)Decide(pol, kinds[i], win, ehciOs);
        if (kinds[i] == K_EHCI && decision[i] == D_WINDOWS) {
            ehciWin = TRUE;
        }
    }
    /* parameters before the driver (re)starts on a controller */
    if (!UserSets(Cmd, "IntelRouteToXhci")) {
        if (SetParam("IntelRouteToXhci", ehciWin ? 2 : 1) && running) {
            reboot = TRUE;
        }
        Print("  IntelRouteToXhci = %d (%s)\r\n", ehciWin ? 2 : 1,
              ehciWin ? "Intel chipsets: only SuperSpeed to xHCI, USB 2.0 ports stay with Windows' EHCI driver" :
              "Intel chipsets: USB 2.0 ports to xHCI");
    }
    cam = PnpWindowsCamera(Cmd);
    if (!UserSets(Cmd, "WindowsCamera")) {
        if (SetParam("WindowsCamera", cam ? 1 : 0) && running) {
            reboot = TRUE;
        }
    }
    Print("  WindowsCamera = %d (%s)\r\n", cam ? 1 : 0,
          cam ? "USB cameras go to Windows' usbvideo.sys" : "USB cameras on USBNT's controllers: USBNT's camera driver");

    if (!BusExists() && FromTextSetup()) {
        /* no USBNT\Bus device: usbnt.sys makes the first host controller the bus, the disks
           are there at boot then; a root device now would be the bus from the next start on */
        Print("  USBNT device bus: on the first host controller (installed by text-mode setup)\r\n");
    } else {
        if (!BusExists() && !MakeBus()) {
            DestroyList(h);
            return 1;
        }
        if (Update("USBNT\\Bus", inf, &reboot)) {
            Print("  USBNT device bus installed\r\n");
        } else {
            Print("  USBNT device bus: error %lu\r\n", GetLastError());
        }
    }
    /* the controllers and the rest the INF names */
    for (i = 0; i < MAX_DEVS; i++) {
        char *id;
        int j;
        BOOL dup = FALSE, ours;
        const char *fn;
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) {
            break;
        }
        if (decision[i] == D_SKIP) {
            continue;
        }
        ZeroMemory(hw, sizeof(hw));
        ZeroMemory(svc, sizeof(svc));
        ZeroMemory(buf, sizeof(buf));
        GetDevRegProp(h, &d, SPDRP_HARDWAREID, &type, (PBYTE)hw, sizeof(hw) - 2, NULL);
        GetDevRegProp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, NULL);
        GetDevRegProp(h, &d, SPDRP_DEVICEDESC, &type, (PBYTE)buf, sizeof(buf) - 2, NULL);
        ours = SameNoCase(svc, "usbnt");
        fn = FriendlyName(kinds[i]);
        if (decision[i] != D_USBNT) {
            if (!ours) {
                Print("  %s %s: %s%s\r\n", KindName[kinds[i]], buf, svc[0] ? svc : "no driver",
                      decision[i] == D_WINDOWS ? " (Windows' driver stays)" : "");
                continue;
            }
            /* an earlier installation took it: back to Windows */
            SetDevRegProp(h, &d, SPDRP_FRIENDLYNAME, NULL, 0);
            if (decision[i] == D_WINDOWS && WindowsDriver(h, &d, winf, wdesc) && InstallWindows(h, &d, &reboot)) {
                Print("  %s %s: back to Windows' driver (%s, %s)\r\n", KindName[kinds[i]], buf, wdesc, winf);
            } else if (decision[i] == D_NONE && CallClassInstaller(DIF_REMOVE, h, &d)) {
                Print("  %s %s: usbnt.sys removed, no driver\r\n", KindName[kinds[i]], buf);
                reboot = TRUE;
            } else {
                Print("  %s %s: cannot give it back to Windows (error %lu)\r\n", KindName[kinds[i]], buf,
                      GetLastError());
            }
            continue;
        }
        for (j = 0; j < nseen; j++) {
            if (SameNoCase(seen[j], hw)) dup = TRUE;
        }
        if (dup || nseen >= 64) {
            if (fn != NULL) {
                SetDevRegProp(h, &d, SPDRP_FRIENDLYNAME, (const BYTE *)fn, (DWORD)lstrlenA(fn) + 1);
            }
            continue;
        }
        id = (char *)LocalAlloc(LMEM_FIXED, lstrlenA(hw) + 1);
        lstrcpyA(id, hw);
        seen[nseen++] = id;
        if (Update(id, inf, &reboot)) {
            Print("  %s %s (%s): usbnt.sys\r\n", KindName[kinds[i]], buf, id);
            if (fn != NULL) {
                SetDevRegProp(h, &d, SPDRP_FRIENDLYNAME, (const BYTE *)fn, (DWORD)lstrlenA(fn) + 1);
            }
            done++;
        } else {
            Print("  %s %s (%s): error %lu\r\n", KindName[kinds[i]], buf, id, GetLastError());
        }
    }
    DestroyList(h);
    if (done == 0) {
        Print("  no device for usbnt.sys found (the USB controllers stay with Windows' drivers)\r\n");
    }
    /* the keyboard and mouse of the bus come once it runs */
    Sleep(3000);
    InfPath(inf, Dir, "usbntkbd.inf");
    Update("USBNT\\Keyboard", inf, &reboot);
    InfPath(inf, Dir, "usbntmou.inf");
    Update("USBNT\\Mouse", inf, &reboot);
    Print(reboot ? "Done. Restart Windows to finish.\r\n" : "Done.\r\n");
    return 0;
}

/* devices with usbnt.sys go back to Windows' drivers (or have none); the INF copies go */
int PnpRemove(const char *Dir)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type, root;
    char svc[64], desc[256], winf[64], wdesc[128];
    int n = 0;
    BOOL reboot = FALSE;

    (void)Dir;
    if (PnpWow64()) {
        return RunNative(Dir, GetCommandLineA());
    }
    if (!Load()) {
        return 1;
    }
    h = GetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (h != INVALID_HANDLE_VALUE) {
        for (i = 0;; i++) {
            d.cbSize = sizeof(d);
            if (!EnumDeviceInfo(h, i, &d)) {
                break;
            }
            ZeroMemory(svc, sizeof(svc));
            if (!GetDevRegProp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, NULL) ||
                !SameNoCase(svc, "usbnt")) {
                continue;
            }
            ZeroMemory(desc, sizeof(desc));
            GetDevRegProp(h, &d, SPDRP_DEVICEDESC, &type, (PBYTE)desc, sizeof(desc) - 1, NULL);
            SetDevRegProp(h, &d, SPDRP_FRIENDLYNAME, NULL, 0);
            if (WindowsDriver(h, &d, winf, wdesc) && InstallWindows(h, &d, &reboot)) {
                Print("  %s: Windows' driver again (%s, %s)\r\n", desc, wdesc, winf);
                n++;
            } else if (CallClassInstaller(DIF_REMOVE, h, &d)) {
                Print("  %s removed\r\n", desc);
                n++;
            } else {
                Print("  %s: error %lu\r\n", desc, GetLastError());
            }
        }
        DestroyList(h);
    }
    RemoveOurInfs();
    /* Windows looks for drivers again: its own USB drivers come back */
    if (LocateDevNode != NULL && Reenumerate != NULL && LocateDevNode(&root, NULL, 0) == 0) {
        Reenumerate(root, 1);
    }
    Print(n ? "Done. Windows' own drivers are back; restart Windows.\r\n" : "No device had usbnt.sys.\r\n");
    return 0;
}

/* "usbnt" in (Add) or out of (!Add) a REG_MULTI_SZ list; TRUE if the list changed */
static BOOL FilterList(char *List, DWORD *Len, BOOL Add)
{
    char *p = List, *end = List + *Len;
    BOOL found = FALSE;

    while (p < end && *p) {
        int n = lstrlenA(p) + 1;
        if (SameNoCase(p, "usbnt")) {
            found = TRUE;
            if (!Add) {
                char *q;
                for (q = p; q + n < end; q++) {
                    *q = q[n];
                }
                end -= n;
                *end = 0;
                continue;
            }
        }
        p += n;
    }
    if (Add && !found) {
        lstrcpyA(p, "usbnt");
        p += 6;
        *p++ = 0;
        *Len = (DWORD)(p - List);
        return TRUE;
    }
    if (!Add && found) {
        *Len = (DWORD)(end - List);
        if (*Len <= 1) {
            *Len = 0;
        }
        return TRUE;
    }
    return FALSE;
}

/* the disks of Windows' USB storage driver: usbnt as a lower filter of disk.sys (Add) or not
   (it reports media with several partitions as fixed, wdmflt.c); disks that are there now are
   restarted.  Quiet: no output when nothing changed (usbmon). */
int PnpFilters(const char *Dir, BOOL Add, BOOL Quiet)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type, len;
    char svc[64], id[512];
    static char list[2048];
    int changed = 0, failed = 0;

    if (PnpWow64()) {
        return RunNative(Dir, GetCommandLineA());
    }
    if (!Load()) {
        return 1;
    }
    h = GetClassDevs(NULL, "USBSTOR", NULL, DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (i = 0;; i++) {
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) {
            break;
        }
        ZeroMemory(svc, sizeof(svc));
        if (!GetDevRegProp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, NULL) ||
            !SameNoCase(svc, "disk")) {
            continue;                           /* a CD-ROM or a disk without its driver yet */
        }
        ZeroMemory(list, sizeof(list));
        len = 0;
        if (!GetDevRegProp(h, &d, SPDRP_LOWERFILTERS, &type, (PBYTE)list, sizeof(list) - 8, &len) || type != REG_MULTI_SZ) {
            ZeroMemory(list, sizeof(list));
            len = 0;
        }
        if (!FilterList(list, &len, Add)) {
            continue;
        }
        id[0] = 0;
        if (GetInstanceId != NULL) {
            GetInstanceId(h, &d, id, sizeof(id), NULL);
        }
        if (!SetDevRegProp(h, &d, SPDRP_LOWERFILTERS, len ? (const BYTE *)list : NULL, len)) {
            Print("  %s: cannot change its filters (error %lu)\r\n", id, GetLastError());
            failed++;
            continue;
        }
        changed++;
        /* the new driver stack: stopped and started again (a disk that is not there gets it when it comes) */
        if (SetClassInstallParams != NULL) {
            SP_PROPCHANGE_PARAMS pc;
            ZeroMemory(&pc, sizeof(pc));
            pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            pc.StateChange = DICS_PROPCHANGE;
            pc.Scope = DICS_FLAG_CONFIGSPECIFIC;
            pc.HwProfile = 0;
            if (SetClassInstallParams(h, &d, &pc.ClassInstallHeader, sizeof(pc)) &&
                CallClassInstaller(DIF_PROPERTYCHANGE, h, &d)) {
                Print("  %s: usbnt filter %s, disk restarted\r\n", id, Add ? "added" : "removed");
                continue;
            }
        }
        Print("  %s: usbnt filter %s\r\n", id, Add ? "added" : "removed");
    }
    DestroyList(h);
    if (!Quiet || changed || failed) {
        Print("%d USB storage disk(s) %s the usbnt filter\r\n", changed, Add ? "got" : "lost");
    }
    return failed ? 1 : 0;
}

/* i8042prt's PS/2 mouse: usbnt as its upper filter (Add) or not - the touch pad protocols of
   wdmps2.c.  Not where another filter is there already (a touch pad vendor's driver). */
int PnpPs2Filter(const char *Dir, BOOL Add)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    SP_DEVINSTALL_PARAMS_A dp;
    DWORD i, type, len;
    char svc[64], cls[32], id[512];
    static char list[2048];
    int changed = 0, failed = 0;
    BOOL reboot = FALSE;

    if (PnpWow64()) {
        return RunNative(Dir, GetCommandLineA());
    }
    if (!Load()) {
        return 1;
    }
    h = GetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (i = 0;; i++) {
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) {
            break;
        }
        ZeroMemory(svc, sizeof(svc));
        ZeroMemory(cls, sizeof(cls));
        if (!GetDevRegProp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, NULL) ||
            !SameNoCase(svc, "i8042prt") ||
            !GetDevRegProp(h, &d, SPDRP_CLASS, &type, (PBYTE)cls, sizeof(cls) - 1, NULL) || !SameNoCase(cls, "Mouse")) {
            continue;
        }
        id[0] = 0;
        if (GetInstanceId != NULL) {
            GetInstanceId(h, &d, id, sizeof(id), NULL);
        }
        ZeroMemory(list, sizeof(list));
        len = 0;
        if (!GetDevRegProp(h, &d, SPDRP_UPPERFILTERS, &type, (PBYTE)list, sizeof(list) - 8, &len) ||
            type != REG_MULTI_SZ) {
            ZeroMemory(list, sizeof(list));
            len = 0;
        }
        if (Add && list[0] && !SameNoCase(list, "usbnt")) {
            Print("  %s: has the filter %s (a touch pad driver?), usbnt is not added\r\n", id, list);
            continue;
        }
        if (!FilterList(list, &len, Add)) {
            continue;
        }
        if (!SetDevRegProp(h, &d, SPDRP_UPPERFILTERS, len ? (const BYTE *)list : NULL, len)) {
            Print("  %s: cannot change its filters (error %lu)\r\n", id, GetLastError());
            failed++;
            continue;
        }
        changed++;
        if (SetClassInstallParams != NULL) {
            SP_PROPCHANGE_PARAMS pc;
            ZeroMemory(&pc, sizeof(pc));
            pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            pc.StateChange = DICS_PROPCHANGE;
            pc.Scope = DICS_FLAG_CONFIGSPECIFIC;
            pc.HwProfile = 0;
            if (SetClassInstallParams(h, &d, &pc.ClassInstallHeader, sizeof(pc))) {
                CallClassInstaller(DIF_PROPERTYCHANGE, h, &d);
            }
        }
        ZeroMemory(&dp, sizeof(dp));
        dp.cbSize = sizeof(dp);
        if (GetDevParams(h, &d, &dp) && (dp.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) {
            reboot = TRUE;
        }
        Print("  %s: usbnt %s as its upper filter (touch pads)\r\n", id, Add ? "added" : "removed");
    }
    DestroyList(h);
    if (changed && reboot) {
        Print("  the PS/2 mouse gets its new drivers when Windows starts again\r\n");
    }
    return failed ? 1 : 0;
}

/* the devices with usbnt.sys, and who drives the USB host controllers */
int PnpStatus(void)
{
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type, maj, min, sp;
    static char svc[64], desc[256], hw[1024], cp[1024];
    int n = 0;

    if (!Load()) {
        return 1;
    }
    WinVersion(&maj, &min, &sp);
    Print("Windows %lu.%lu SP%lu: EHCI driver of Windows %s, camera driver of Windows (usbvideo.sys) %s\r\n", maj, min,
          sp, EhciByOs() ? "yes" : "no", UvcByOs() ? "yes" : "no");
    h = GetClassDevs(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (i = 0;; i++) {
        int kind;
        BOOL ours;
        d.cbSize = sizeof(d);
        if (!EnumDeviceInfo(h, i, &d)) {
            break;
        }
        ZeroMemory(svc, sizeof(svc));
        ZeroMemory(hw, sizeof(hw));
        ZeroMemory(cp, sizeof(cp));
        GetDevRegProp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, NULL);
        GetDevRegProp(h, &d, SPDRP_HARDWAREID, &type, (PBYTE)hw, sizeof(hw) - 2, NULL);
        GetDevRegProp(h, &d, SPDRP_COMPATIBLEIDS, &type, (PBYTE)cp, sizeof(cp) - 2, NULL);
        ours = SameNoCase(svc, "usbnt");
        kind = (hw[0] && Contains(hw, "PCI\\")) ? KindOf(hw, cp) : K_OTHER;
        if (!ours && (kind == K_OTHER || kind == K_NONE)) {
            continue;
        }
        ZeroMemory(desc, sizeof(desc));
        if (!GetDevRegProp(h, &d, SPDRP_FRIENDLYNAME, &type, (PBYTE)desc, sizeof(desc) - 1, NULL) || !desc[0]) {
            GetDevRegProp(h, &d, SPDRP_DEVICEDESC, &type, (PBYTE)desc, sizeof(desc) - 1, NULL);
        }
        if (kind != K_OTHER && kind != K_NONE) {
            Print("%s controller: %s - %s\r\n", KindName[kind], desc, ours ? "usbnt" : (svc[0] ? svc : "no driver"));
        } else {
            Print("usbnt: %s\r\n", desc);
        }
        n += ours;
    }
    DestroyList(h);
    Print("%d device(s) with usbnt.sys\r\n", n);
    return 0;
}
