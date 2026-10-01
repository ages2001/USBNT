/*
 * usbinst.c - installs or removes usbnt.sys on Windows NT 3.1 - 4.0.
 *
 * usbinst /install [/maxdisks N] [/irq] [/nomon] [/set Name=Value ...]
 *     copies usbnt.sys, usbmon.exe, usbtree.exe and usbeject.exe from the
 *     directory of usbinst.exe, registers the boot start driver, lets
 *     kbdclass and mouclass serve more than one port and starts usbmon.exe
 *     at every logon. /set stores a DWORD driver parameter (decimal or 0x
 *     hex), for example /set DebugPort=0x2f8.
 * usbinst /remove
 *     removes the driver service and the logon entry (the files stay).
 * usbinst /status
 *     shows the service configuration.
 */

#include <windows.h>

static BOOL SameNoCase(const char *A, const char *B, int Len)
{
    while (Len-- > 0) {
        char a = *A++;
        char b = *B++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) {
            return FALSE;
        }
    }
    return TRUE;
}

static HANDLE Out;

static void Print(const char *Fmt, ...)
{
    char buf[512];
    DWORD n;
    va_list ap;

    va_start(ap, Fmt);
    wvsprintfA(buf, Fmt, ap);
    va_end(ap);
    WriteFile(Out, buf, lstrlenA(buf), &n, NULL);
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

static DWORD ArgNumber(const char *Cmd, const char *Arg, DWORD Default)
{
    const char *p;
    int len = lstrlenA(Arg);

    for (p = Cmd; *p; p++) {
        if (SameNoCase(p, Arg, len)) {
            DWORD v = 0;
            p += len;
            while (*p == ' ' || *p == ':' || *p == '=') {
                p++;
            }
            if (*p < '0' || *p > '9') {
                return Default;
            }
            while (*p >= '0' && *p <= '9') {
                v = v * 10 + (DWORD)(*p - '0');
                p++;
            }
            return v;
        }
    }
    return Default;
}

static BOOL SetDword(HKEY Root, const char *Key, const char *Name, DWORD Value);

/* every "/set Name=Value" on the command line becomes a driver parameter */
static void SetParams(const char *Cmd)
{
    const char *p;

    for (p = Cmd; *p; p++) {
        char name[64];
        int n = 0;
        DWORD v = 0;
        int base = 10;
        if (!SameNoCase(p, "/set ", 5)) {
            continue;
        }
        p += 5;
        while (*p == ' ') {
            p++;
        }
        while (*p && *p != '=' && *p != ' ' && n < (int)sizeof(name) - 1) {
            name[n++] = *p++;
        }
        name[n] = 0;
        if (*p != '=' || n == 0) {
            Print("  ignored /set %s (use /set Name=Value)\r\n", name);
            continue;
        }
        p++;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            base = 16;
            p += 2;
        }
        for (;;) {
            char c = *p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (base == 16 && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (base == 16 && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * (DWORD)base + (DWORD)d;
            p++;
        }
        SetDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\usbnt\\Parameters", name, v);
        Print("  %s = %lu\r\n", name, v);
        p--;
    }
}

static BOOL SetDword(HKEY Root, const char *Key, const char *Name, DWORD Value)
{
    HKEY k;
    DWORD disp;
    LONG r;

    r = RegCreateKeyExA(Root, Key, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
    if (r != ERROR_SUCCESS) {
        return FALSE;
    }
    r = RegSetValueExA(k, Name, 0, REG_DWORD, (const BYTE *)&Value, sizeof(Value));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void ExeDir(char *Buf, DWORD Size)
{
    DWORD n = GetModuleFileNameA(NULL, Buf, Size);

    while (n > 0 && Buf[n - 1] != '\\') {
        n--;
    }
    Buf[n] = 0;
}

static BOOL CopyOne(const char *SrcDir, const char *DstDir, const char *Name, BOOL Required)
{
    char src[MAX_PATH];
    char dst[MAX_PATH];

    wsprintfA(src, "%s%s", SrcDir, Name);
    wsprintfA(dst, "%s\\%s", DstDir, Name);
    if (GetFileAttributesA(src) == 0xFFFFFFFF) {
        if (Required) {
            Print("  %s not found next to usbinst.exe\r\n", Name);
        }
        return !Required;
    }
    if (!CopyFileA(src, dst, FALSE)) {
        Print("  cannot copy %s to %s (error %lu)\r\n", Name, DstDir, GetLastError());
        return FALSE;
    }
    Print("  copied %s\r\n", dst);
    return TRUE;
}

#define WINLOGON "Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon"

/* Finds Name as one entry of a comma separated list, case insensitive */
static char *FindEntry(char *List, const char *Name)
{
    char *p = List;
    int n = lstrlenA(Name);

    while (*p) {
        char *e = p;
        char *b;
        while (*e && *e != ',') {
            e++;
        }
        b = e;
        while (b > p && b[-1] == ' ') {
            b--;
        }
        if (b - p >= n && SameNoCase(b - n, Name, n) &&
            (b - n == p || b[-n - 1] == '\\' || b[-n - 1] == ' ')) {
            return p;
        }
        p = (*e == ',') ? e + 1 : e;
    }
    return NULL;
}

/* usbmon.exe runs at every logon through the Userinit list of Winlogon */
static void SetLogonEntry(BOOL Add)
{
    HKEY k;
    char val[600];
    DWORD type = 0;
    DWORD len = sizeof(val) - 32;
    char *e;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WINLOGON, 0, KEY_ALL_ACCESS, &k) != ERROR_SUCCESS) {
        Print("  cannot open the Winlogon key\r\n");
        return;
    }
    val[0] = 0;
    if (RegQueryValueExA(k, "Userinit", NULL, &type, (BYTE *)val, &len) != ERROR_SUCCESS || type != REG_SZ) {
        lstrcpyA(val, "userinit");
    }
    e = FindEntry(val, "usbmon.exe");
    if (Add && e == NULL) {
        int n = lstrlenA(val);
        while (n > 0 && (val[n - 1] == ',' || val[n - 1] == ' ')) {
            val[--n] = 0;
        }
        lstrcatA(val, ",usbmon.exe");
        RegSetValueExA(k, "Userinit", 0, REG_SZ, (const BYTE *)val, (DWORD)lstrlenA(val) + 1);
        Print("  usbmon.exe starts at every logon\r\n");
    } else if (!Add && e != NULL) {
        char *end = e;
        while (*end && *end != ',') {
            end++;
        }
        if (*end == ',') {
            end++;
        } else if (e > val && e[-1] == ',') {
            e--;
        }
        lstrcpyA(e, end);
        RegSetValueExA(k, "Userinit", 0, REG_SZ, (const BYTE *)val, (DWORD)lstrlenA(val) + 1);
        Print("  usbmon.exe removed from logon\r\n");
    }
    RegCloseKey(k);
}

static int Install(const char *Cmd)
{
    char dir[MAX_PATH];
    char sys[MAX_PATH];
    char drv[MAX_PATH];
    SC_HANDLE scm;
    SC_HANDLE svc;
    DWORD maxDisks = ArgNumber(Cmd, "/maxdisks", 0);

    ExeDir(dir, sizeof(dir));
    GetSystemDirectoryA(sys, sizeof(sys));
    wsprintfA(drv, "%s\\drivers", sys);

    Print("Installing the USB driver\r\n");
    if (!CopyOne(dir, drv, "usbnt.sys", TRUE)) {
        return 1;
    }
    CopyOne(dir, sys, "usbeject.exe", FALSE);
    CopyOne(dir, sys, "usbtree.exe", FALSE);
    CopyOne(dir, sys, "usbmon.exe", FALSE);

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (scm == NULL) {
        Print("  cannot open the service manager (error %lu); log on as an administrator\r\n", GetLastError());
        return 1;
    }
    svc = CreateServiceA(scm, "usbnt", "USB host controller driver", SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
                         SERVICE_BOOT_START, SERVICE_ERROR_NORMAL, "System32\\drivers\\usbnt.sys",
                         "SCSI miniport", NULL, NULL, NULL, NULL);
    if (svc == NULL && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceA(scm, "usbnt", SERVICE_ALL_ACCESS);
        if (svc != NULL && !ChangeServiceConfigA(svc, SERVICE_KERNEL_DRIVER, SERVICE_BOOT_START, SERVICE_ERROR_NORMAL,
                                                 "System32\\drivers\\usbnt.sys", "SCSI miniport", NULL, NULL, NULL,
                                                 NULL, NULL)) {
            Print("  cannot update the service (error %lu)\r\n", GetLastError());
        }
    }
    if (svc == NULL) {
        Print("  cannot create the service (error %lu)\r\n", GetLastError());
        CloseServiceHandle(scm);
        return 1;
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    Print("  service usbnt registered (boot start, group SCSI miniport)\r\n");

    SetDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\Kbdclass\\Parameters", "ConnectMultiplePorts", 1);
    SetDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\Mouclass\\Parameters", "ConnectMultiplePorts", 1);
    Print("  kbdclass and mouclass set to serve USB and PS/2 together\r\n");

    if (maxDisks != 0) {
        SetDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\usbnt\\Parameters", "MaxDisks", maxDisks);
        Print("  MaxDisks = %lu\r\n", maxDisks);
    }
    if (HasArg(Cmd, "/irq")) {
        SetDword(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\usbnt\\Parameters", "UseInterrupts", 1);
        Print("  interrupts enabled\r\n");
    }
    SetParams(Cmd);
    if (!HasArg(Cmd, "/nomon")) {
        SetLogonEntry(TRUE);
    }
    Print("Done. Restart Windows NT to load the driver.\r\n");
    return 0;
}

static int Remove(void)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    SC_HANDLE svc;

    if (scm == NULL) {
        Print("Cannot open the service manager (error %lu)\r\n", GetLastError());
        return 1;
    }
    svc = OpenServiceA(scm, "usbnt", SERVICE_ALL_ACCESS);
    if (svc == NULL) {
        Print("The usbnt service is not installed.\r\n");
        CloseServiceHandle(scm);
        return 1;
    }
    SetLogonEntry(FALSE);
    if (!DeleteService(svc)) {
        Print("Cannot remove the service (error %lu)\r\n", GetLastError());
    } else {
        Print("The usbnt service was removed. Restart Windows NT.\r\n");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int Status(void)
{
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    SC_HANDLE svc;
    char buf[1024];
    LPQUERY_SERVICE_CONFIGA cfg = (LPQUERY_SERVICE_CONFIGA)buf;
    SERVICE_STATUS st;
    DWORD need;

    if (scm == NULL) {
        Print("Cannot open the service manager (error %lu)\r\n", GetLastError());
        return 1;
    }
    svc = OpenServiceA(scm, "usbnt", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    if (svc == NULL) {
        Print("The usbnt service is not installed.\r\n");
        CloseServiceHandle(scm);
        return 1;
    }
    if (QueryServiceConfigA(svc, cfg, sizeof(buf), &need)) {
        Print("usbnt: start type %lu, group %s, image %s\r\n", cfg->dwStartType,
              cfg->lpLoadOrderGroup ? cfg->lpLoadOrderGroup : "", cfg->lpBinaryPathName ? cfg->lpBinaryPathName : "");
    }
    if (QueryServiceStatus(svc, &st)) {
        Print("usbnt: %s\r\n", st.dwCurrentState == SERVICE_RUNNING ? "running" : "not running");
    }
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    {
        HKEY k;
        DWORD type = 0;
        DWORD len = sizeof(buf) - 1;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, WINLOGON, 0, KEY_READ, &k) == ERROR_SUCCESS) {
            if (RegQueryValueExA(k, "Userinit", NULL, &type, (BYTE *)buf, &len) == ERROR_SUCCESS && type == REG_SZ) {
                buf[len] = 0;
                Print("logon programs: %s\r\n", buf);
            }
            RegCloseKey(k);
        }
    }
    return 0;
}

void __stdcall Entry(void)
{
    const char *cmd = GetCommandLineA();
    int rc;

    Out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (HasArg(cmd, "/install")) {
        rc = Install(cmd);
    } else if (HasArg(cmd, "/remove")) {
        rc = Remove();
    } else if (HasArg(cmd, "/status")) {
        rc = Status();
    } else {
        Print("usage: usbinst /install [/maxdisks N] [/irq] [/nomon] [/set Name=Value ...]\r\n"
              "       usbinst /remove\r\n"
              "       usbinst /status\r\n");
        rc = 1;
    }
    ExitProcess((UINT)rc);
}
