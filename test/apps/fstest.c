/*
 * fstest.c - file system probe for the NT storage tests (not shipped).
 *
 *   fstest wait PATH MS         wait until PATH exists (prints FOUND/TIMEOUT)
 *   fstest gone PATH MS         wait until PATH no longer exists
 *   fstest cat FILE             print a text file between markers
 *   fstest dir PATH             list a directory (names and sizes)
 *   fstest write FILE TEXT      create FILE with TEXT
 *   fstest sleep MS
 *   fstest front TITLE MS       keep window TITLE ('_' for blanks) on top for MS
 *   fstest sum FILE             FNV-1a 32 of the file's bytes and its size
 *   fstest reg KEY NAME dword:N | sz:TEXT   set a value under HKLM (the key is made)
 *   fstest regget KEY NAME      print a DWORD or string value under HKLM
 *   fstest start NAME           start a service (a driver loaded late, like text-mode setup does)
 *   fstest disk                 find the test medium (MBR marker "USBNT-SECTORS")
 *                               and read it through \\.\PhysicalDriveN and its
 *                               Partition1 at many offsets and lengths; every
 *                               sector holds its own number (see ntsd.py)
 *
 *   fstest parts [N]            the partition devices of disk N (all disks), their
 *                               place on the disk, the drive layout, the drive letters
 *   fstest echo TEXT            print TEXT
 *   fstest devlist [TEXT]       the present devices as Device Manager has them (name, class,
 *                               service, problem code, instance id, parent) - those whose line
 *                               holds TEXT (Windows 2000 / XP)
 *   fstest none NAME            no drive (D: .. Z:) has \NAME (prints OK or the drive)
 *   fstest grep FILE TEXT       print the lines of FILE that hold TEXT
 *   fstest run CMDLINE          run a program and wait for it
 *   fstest on NAME CMDLINE      run CMDLINE with every '?' replaced by the drive (D: .. Z:)
 *                               that has \NAME (with critical-error pop-ups off, unlike a
 *                               "for ... if exist" in cmd, which stops at an empty drive)
 *
 * Critical-error pop-ups are disabled so a missing medium does not block.
 * With FSTEST_COM=COMn in the environment every line also goes to that port
 * at once (the test agent shows the output only when the batch file ends).
 */
#include <windows.h>
#include <setupapi.h>

static HANDLE Out;

static void Print(const char *Fmt, ...)
{
    char buf[1024];
    DWORD n;
    va_list ap;
    static char port[16];
    va_start(ap, Fmt);
    wvsprintfA(buf, Fmt, ap);
    va_end(ap);
    WriteFile(Out, buf, lstrlenA(buf), &n, NULL);
    if (port[0] == 0 && GetEnvironmentVariableA("FSTEST_COM", port, sizeof(port)) == 0) {
        port[0] = '-';
    }
    if (port[0] != '-') {
        /* open for each line: another fstest (hold) may want the port too */
        HANDLE c = INVALID_HANDLE_VALUE;
        int i;
        for (i = 0; i < 20 && c == INVALID_HANDLE_VALUE; i++) {
            c = CreateFileA(port, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (c == INVALID_HANDLE_VALUE) Sleep(50);
        }
        if (c != INVALID_HANDLE_VALUE) {
            WriteFile(c, buf, lstrlenA(buf), &n, NULL);
            CloseHandle(c);
        }
    }
}

static char *Arg(char **pp)
{
    char *p = *pp, *s;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        s = ++p;
        while (*p && *p != '"') p++;
    } else {
        s = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (*p) *p++ = 0;
    *pp = p;
    return s;
}

static DWORD Num(const char *s)
{
    DWORD v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (DWORD)(*s++ - '0');
    return v;
}

static int memcmp_(const void *a, const void *b, DWORD n)
{
    const UCHAR *x = (const UCHAR *)a, *y = (const UCHAR *)b;
    while (n--) if (*x++ != *y++) return 1;
    return 0;
}

/* "?:\..." is the drive (D: .. Z:) that has \HELLO.TXT (the test stick) */
/* "?:\X" is the drive with \HELLO.TXT, "?NAME:\X" the drive with \NAME */
static void Resolve(char *p)
{
    char d, t[MAX_PATH];
    if (p[0] == '?' && p[1] != ':' && p[1] != 0) {
        int n = 1;
        while (p[n] && p[n] != ':' && n < 64) n++;
        if (p[n] != ':') return;
        for (d = 'D'; d <= 'Z'; d++) {
            int k;
            wsprintfA(t, "%c:\\", d);
            for (k = 1; k < n; k++) t[2 + k] = p[k];
            t[2 + n] = 0;
            if (GetFileAttributesA(t) != 0xFFFFFFFF) {
                p[0] = d;
                lstrcpyA(p + 1, p + n);
                return;
            }
        }
        return;
    }
    if (p[0] != '?' || p[1] != ':') return;
    for (d = 'D'; d <= 'Z'; d++) {
        wsprintfA(t, "%c:\\HELLO.TXT", d);
        if (GetFileAttributesA(t) != 0xFFFFFFFF) {
            p[0] = d;
            return;
        }
    }
}

static BOOL Exists(const char *p)
{
    char b[MAX_PATH];
    if (lstrlenA(p) >= MAX_PATH) return FALSE;
    lstrcpyA(b, p);
    Resolve(b);
    return GetFileAttributesA(b) != 0xFFFFFFFF;
}

/* ------------------------------------------------------------------ */
/* fstest disk                                                          */
/* ------------------------------------------------------------------ */

static DWORD DiskBuf[1024 * 128 + 1024];             /* 512 KB + slack */

/* reads Count sectors at Lba (relative to the device) through h and checks
   that sector i holds Base + Lba + i in every DWORD */
static int ReadCheck(HANDLE h, const char *What, DWORD Lba, DWORD Count, DWORD Base, DWORD Skew)
{
    DWORD n = 0, i, k, err = 0, lo = Lba << 9, hi = Lba >> 23, *b = DiskBuf + Skew;
    if (SetFilePointer(h, (LONG)lo, (LONG *)&hi, FILE_BEGIN) == 0xFFFFFFFF && GetLastError() != NO_ERROR) {
        Print("DISK %s lba=%u count=%u SEEK-ERROR %u\r\n", What, Lba, Count, GetLastError());
        return 1;
    }
    for (i = 0; i < Count * 128; i++) b[i] = 0xDEADBEEF;
    if (!ReadFile(h, b, Count * 512, &n, NULL) || n != Count * 512) {
        err = GetLastError();
        Print("DISK %s lba=%u count=%u skew=%u READ-ERROR %u got=%u\r\n", What, Lba, Count, Skew * 4, err, n);
        return 1;
    }
    for (i = 0; i < Count; i++) {
        for (k = 0; k < 128; k++) {
            if (b[i * 128 + k] != Base + Lba + i) {
                Print("DISK %s lba=%u count=%u skew=%u WRONG-DATA at sector %u: %08X\r\n", What, Lba, Count, Skew * 4,
                      i, b[i * 128 + k]);
                return 1;
            }
        }
    }
    Print("DISK %s lba=%u count=%u skew=%u OK\r\n", What, Lba, Count, Skew * 4);
    return 0;
}

static DWORD DiskTest(void)
{
    char name[64];
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD n, d, total, start, plen, fails = 0, i, j;
    static const DWORD counts[] = { 1, 2, 7, 8, 16, 63, 64, 127, 128, 129, 255, 256, 257, 512, 1024 };
    UCHAR *mbr = (UCHAR *)DiskBuf;

    for (d = 0; d < 10; d++) {
        wsprintfA(name, "\\\\.\\PhysicalDrive%u", d);
        h = CreateFileA(name, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        if (ReadFile(h, mbr, 512, &n, NULL) && n == 512 && !memcmp_(mbr, "USBNT-SECTORS", 13)) break;
        CloseHandle(h);
        h = INVALID_HANDLE_VALUE;
    }
    if (h == INVALID_HANDLE_VALUE) {
        Print("DISK no test medium found\r\n");
        return 1;
    }
    total = *(DWORD *)(mbr + 16);                       /* sectors of the medium (written by ntsd.py) */
    start = *(DWORD *)(mbr + 446 + 8);
    plen = *(DWORD *)(mbr + 446 + 12);
    Print("DISK found PhysicalDrive%u: %u sectors, partition 1 at %u, %u sectors\r\n", d, total, start, plen);
    {
        static const DWORD at[] = { 1, 100, 8192, 8193, 65536 };
        for (i = 0; i < sizeof(at) / sizeof(at[0]); i++)
            for (j = 0; j < sizeof(counts) / sizeof(counts[0]); j++)
                fails += ReadCheck(h, "disk", at[i], counts[j], 0, 0);
        fails += ReadCheck(h, "disk", total / 2, 256, 0, 0);
        fails += ReadCheck(h, "disk", total - 1024, 1024, 0, 0);
        fails += ReadCheck(h, "disk", total - 1, 1, 0, 0);
        fails += ReadCheck(h, "disk", 8192, 64, 0, 1);       /* buffer not page aligned */
        fails += ReadCheck(h, "disk", 8192, 256, 0, 129);    /* buffer not sector aligned */
    }
    CloseHandle(h);
    /* the partition, as a file system sees it */
    wsprintfA(name, "\\Device\\Harddisk%u\\Partition1", d);
    if (!DefineDosDeviceA(DDD_RAW_TARGET_PATH, "SDPART", name)) {
        Print("DISK DefineDosDevice failed %u\r\n", GetLastError());
        return fails + 1;
    }
    h = CreateFileA("\\\\.\\SDPART", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        Print("DISK partition open failed %u\r\n", GetLastError());
        fails++;
    } else {
        for (j = 0; j < sizeof(counts) / sizeof(counts[0]); j++)
            fails += ReadCheck(h, "part", 1, counts[j], start, 0);
        fails += ReadCheck(h, "part", plen / 2, 128, start, 0);
        fails += ReadCheck(h, "part", plen - 1024, 1024, start, 0);
        fails += ReadCheck(h, "part", plen - 1, 1, start, 0);
        CloseHandle(h);
    }
    DefineDosDeviceA(DDD_REMOVE_DEFINITION, "SDPART", NULL);
    Print("DISK done, %u failure(s)\r\n", fails);
    return fails ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* fstest parts [N]                                                     */
/* ------------------------------------------------------------------ */

typedef struct _PINFO {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    DWORD HiddenSectors;
    DWORD PartitionNumber;
    UCHAR PartitionType;
    UCHAR BootIndicator;
    UCHAR RecognizedPartition;
    UCHAR RewritePartition;
} PINFO;

#define IOCTL_DISK_GET_PARTITION_INFO_  0x00074004
#define IOCTL_DISK_GET_DRIVE_LAYOUT_    0x0007400C

/* every \Device\HarddiskN\PartitionK that exists (N = all disks, or only N), with its
   place on the disk, the drive layout of the disk and the drive letters on the disks */
static DWORD Parts(DWORD Only)
{
    char name[64], target[256], l[4];
    DWORD d, k, n;
    HANDLE h;
    PINFO pi;

    for (d = 0; d < 16; d++) {
        if (Only != 0xFFFFFFFF && d != Only) continue;
        for (k = 0; k < 24; k++) {
            wsprintfA(name, "\\Device\\Harddisk%u\\Partition%u", d, k);
            if (!DefineDosDeviceA(DDD_RAW_TARGET_PATH, "FSTPART", name)) continue;
            h = CreateFileA("\\\\.\\FSTPART", 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h == INVALID_HANDLE_VALUE) {
                DefineDosDeviceA(DDD_REMOVE_DEFINITION, "FSTPART", NULL);
                if (k == 0) break;
                continue;
            }
            CloseHandle(h);
            h = CreateFileA("\\\\.\\FSTPART", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0,
                            NULL);
            if (h != INVALID_HANDLE_VALUE &&
                DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO_, NULL, 0, &pi, sizeof(pi), &n, NULL)) {
                Print("PARTS disk %u part %u: start %u len %u type %02X\r\n", d, k,
                      (DWORD)(pi.StartingOffset.QuadPart >> 9), (DWORD)(pi.PartitionLength.QuadPart >> 9),
                      pi.PartitionType);
            } else {
                Print("PARTS disk %u part %u: exists (info error %u)\r\n", d, k, GetLastError());
            }
            if (k == 0 && h != INVALID_HANDLE_VALUE) {
                static UCHAR lay[8 + 32 * 64];
                if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_, NULL, 0, lay, sizeof(lay), &n, NULL)) {
                    DWORD c = *(DWORD *)lay, i;
                    for (i = 0; i < c && i < 64; i++) {
                        PINFO *e = (PINFO *)(lay + 8 + 32 * i);
                        if (e->PartitionType == 0) continue;
                        Print("PARTS disk %u layout %u: start %u len %u type %02X number %u%s\r\n", d, i,
                              (DWORD)(e->StartingOffset.QuadPart >> 9), (DWORD)(e->PartitionLength.QuadPart >> 9),
                              e->PartitionType, e->PartitionNumber, e->RecognizedPartition ? " recognized" : "");
                    }
                }
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            DefineDosDeviceA(DDD_REMOVE_DEFINITION, "FSTPART", NULL);
        }
    }
    for (l[0] = 'C'; l[0] <= 'Z'; l[0]++) {
        l[1] = ':';
        l[2] = 0;
        if (QueryDosDeviceA(l, target, sizeof(target)) != 0) {
            Print("PARTS letter %s -> %s\r\n", l, target);
        }
    }
    return 0;
}

/* clicker: the questions Windows XP asks while drivers install (nobody is there to answer in
   the tests): "Continue Anyway" for unsigned drivers, Cancel for the Found New Hardware Wizard */
static const char *Want;
static int Clicks;

static BOOL CALLBACK ChildCb(HWND h, LPARAM l)
{
    char t[128];
    (void)l;
    GetWindowTextA(h, t, sizeof(t));
    if (lstrcmpiA(t, Want) == 0 && IsWindowEnabled(h)) {
        DWORD_PTR r;
        SendMessageTimeoutA(h, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 2000, &r);
        Print("FSTEST clicker: %s\r\n", t);
        Clicks++;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK TopCb(HWND h, LPARAM l)
{
    char t[128];
    (void)l;
    if (!IsWindowVisible(h)) return TRUE;
    GetWindowTextA(h, t, sizeof(t));
    if (!lstrcmpiA(t, "Software Installation") || !lstrcmpiA(t, "Hardware Installation")) {
        Want = "&Continue Anyway";
        EnumChildWindows(h, ChildCb, 0);
    } else if (!lstrcmpiA(t, "Found New Hardware Wizard") || !lstrcmpiA(t, "Found New Hardware")) {
        /* through the wizard: Windows installs its own driver; Cancel only when there is no way on */
        int before = Clicks;
        Want = "&Next >";
        EnumChildWindows(h, ChildCb, 0);
        if (Clicks == before) {
            Want = "Finish";
            EnumChildWindows(h, ChildCb, 0);
        }
        if (Clicks == before) {
            Want = "Cancel";
            EnumChildWindows(h, ChildCb, 0);
        }
    }
    return TRUE;
}

/* devlist: SetupAPI / CfgMgr32 loaded at run time (fstest runs on NT 3.1 too) */
typedef HDEVINFO (WINAPI *P_GetClassDevs)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *P_EnumInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_GetProp)(HDEVINFO, PSP_DEVINFO_DATA, DWORD, PDWORD, PBYTE, DWORD, PDWORD);
typedef BOOL (WINAPI *P_GetId)(HDEVINFO, PSP_DEVINFO_DATA, PSTR, DWORD, PDWORD);
typedef BOOL (WINAPI *P_Destroy)(HDEVINFO);
typedef DWORD (WINAPI *P_Status)(PULONG, PULONG, DWORD, ULONG);
typedef DWORD (WINAPI *P_Parent)(PDWORD, DWORD, ULONG);
typedef DWORD (WINAPI *P_DevId)(DWORD, PSTR, ULONG, ULONG);

static int HasText(const char *L, const char *T)
{
    int i, j;
    if (T == NULL || !T[0]) return 1;
    for (i = 0; L[i]; i++) {
        for (j = 0; T[j] && L[i + j]; j++) {
            char a = L[i + j], b = T[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 32);
            if (b >= 'a' && b <= 'z') b = (char)(b - 32);
            if (a != b) break;
        }
        if (!T[j]) return 1;
    }
    return 0;
}

static int DevList(const char *Filter)
{
    HMODULE s = LoadLibraryA("setupapi.dll"), c = LoadLibraryA("cfgmgr32.dll");
    P_GetClassDevs gcd; P_EnumInfo en; P_GetProp gp; P_GetId gid; P_Destroy des;
    P_Status sta; P_Parent par; P_DevId did;
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, type, n = 0;
    static char name[256], cls[64], svc[64], id[256], pid[256], line[1024];

    if (s == NULL) { Print("FSTEST devlist: no setupapi\r\n"); return 1; }
    gcd = (P_GetClassDevs)GetProcAddress(s, "SetupDiGetClassDevsA");
    en = (P_EnumInfo)GetProcAddress(s, "SetupDiEnumDeviceInfo");
    gp = (P_GetProp)GetProcAddress(s, "SetupDiGetDeviceRegistryPropertyA");
    gid = (P_GetId)GetProcAddress(s, "SetupDiGetDeviceInstanceIdA");
    des = (P_Destroy)GetProcAddress(s, "SetupDiDestroyDeviceInfoList");
    sta = (P_Status)GetProcAddress(c ? c : s, "CM_Get_DevNode_Status");
    par = (P_Parent)GetProcAddress(c ? c : s, "CM_Get_Parent");
    did = (P_DevId)GetProcAddress(c ? c : s, "CM_Get_Device_IDA");
    if (!gcd || !en || !gp || !gid || !des) { Print("FSTEST devlist: no setupapi functions\r\n"); return 1; }
    h = gcd(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) return 1;
    for (i = 0;; i++) {
        ULONG st = 0, prob = 0;
        DWORD parent = 0;
        d.cbSize = sizeof(d);
        if (!en(h, i, &d)) break;
        name[0] = cls[0] = svc[0] = id[0] = pid[0] = 0;
        if (!gp(h, &d, SPDRP_FRIENDLYNAME, &type, (PBYTE)name, sizeof(name) - 1, NULL) || !name[0])
            gp(h, &d, SPDRP_DEVICEDESC, &type, (PBYTE)name, sizeof(name) - 1, NULL);
        if (!gp(h, &d, SPDRP_CLASS, &type, (PBYTE)cls, sizeof(cls) - 1, NULL) || !cls[0])
            gp(h, &d, SPDRP_CLASSGUID, &type, (PBYTE)cls, sizeof(cls) - 1, NULL);   /* no INF: the GUID */
        gp(h, &d, SPDRP_SERVICE, &type, (PBYTE)svc, sizeof(svc) - 1, NULL);
        gid(h, &d, id, sizeof(id), NULL);
        if (sta) sta(&st, &prob, d.DevInst, 0);
        if (par && did && par(&parent, d.DevInst, 0) == 0) did(parent, pid, sizeof(pid), 0);
        wsprintfA(line, "FSTEST dev %s | %s | svc=%s | prob=%lu | %s | parent=%s\r\n", cls[0] ? cls : "-", name,
                  svc[0] ? svc : "-", prob, id, pid);
        if (HasText(line, Filter)) { Print("%s", line); n++; }
    }
    des(h);
    Print("FSTEST devlist: %lu device(s)\r\n", n);
    return 0;
}

void __stdcall Entry(void)
{
    char *cl = GetCommandLineA(), *cmd, *a1, *a2;
    DWORD t0, ms, rc = 0;
    Out = GetStdHandle(STD_OUTPUT_HANDLE);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    Arg(&cl);
    cmd = Arg(&cl);
    a1 = Arg(&cl);
    a2 = cl;
    while (*a2 == ' ') a2++;
    if (lstrcmpiA(cmd, "wait") && lstrcmpiA(cmd, "gone")) Resolve(a1);
    if (!lstrcmpiA(cmd, "wait") || !lstrcmpiA(cmd, "gone")) {
        BOOL want = !lstrcmpiA(cmd, "wait");
        ms = Num(a2);
        t0 = GetTickCount();
        if (want) {
            while (!Exists(a1) && GetTickCount() - t0 < ms) Sleep(250);
        } else {
            /* gone: away for 3 s on end (a volume can vanish for a moment while it is mounted) */
            DWORD since = GetTickCount();
            while (GetTickCount() - t0 < ms) {
                if (Exists(a1)) since = GetTickCount();
                else if (GetTickCount() - since >= 3000) break;
                Sleep(250);
            }
        }
        Print("FSTEST %s %s %s after %u ms\r\n", cmd, a1, Exists(a1) == want ? "OK" : "TIMEOUT",
              GetTickCount() - t0);
    } else if (!lstrcmpiA(cmd, "svc")) {
        /* svc NAME GROUP: a boot start kernel driver System32\drivers\NAME.sys (test setup, e.g. nvme2k) */
        SC_HANDLE m = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS), sv;
        char path[MAX_PATH];
        wsprintfA(path, "System32\\drivers\\%s.sys", a1);
        sv = m ? CreateServiceA(m, a1, a1, SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_BOOT_START,
                                SERVICE_ERROR_NORMAL, path, a2, NULL, NULL, NULL, NULL) : NULL;
        if (sv != NULL || GetLastError() == ERROR_SERVICE_EXISTS) {
            Print("FSTEST svc %s OK\r\n", a1);
        } else {
            Print("FSTEST svc %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        }
        if (sv) CloseServiceHandle(sv);
        if (m) CloseServiceHandle(m);
    } else if (!lstrcmpiA(cmd, "svcstat")) {
        SC_HANDLE m = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT), sv;
        SERVICE_STATUS st;
        sv = m ? OpenServiceA(m, a1, SERVICE_QUERY_STATUS) : NULL;
        if (sv && QueryServiceStatus(sv, &st)) {
            Print("FSTEST svcstat %s state %u exit %u/%u\r\n", a1, st.dwCurrentState, st.dwWin32ExitCode,
                  st.dwServiceSpecificExitCode);
        } else {
            Print("FSTEST svcstat %s ERROR %u\r\n", a1, GetLastError());
        }
        if (sv) CloseServiceHandle(sv);
        if (m) CloseServiceHandle(m);
    } else if (!lstrcmpiA(cmd, "find")) {
        /* find NAME ms: waits until some drive D: .. Z: has \NAME */
        char path[MAX_PATH], found = 0, d;
        ms = Num(a2);
        t0 = GetTickCount();
        do {
            for (d = 'D'; d <= 'Z' && !found; d++) {
                wsprintfA(path, "%c:\\%s", d, a1);
                if (Exists(path)) found = d;
            }
            if (!found) Sleep(500);
        } while (!found && GetTickCount() - t0 < ms);
        if (found) {
            Print("FSTEST find %s %c: OK after %u ms\r\n", a1, found, GetTickCount() - t0);
        } else {
            Print("FSTEST find %s TIMEOUT after %u ms\r\n", a1, GetTickCount() - t0);
            rc = 1;
        }
    } else if (!lstrcmpiA(cmd, "cat")) {
        HANDLE h = CreateFileA(a1, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        char buf[512];
        DWORD n = 0;
        if (h == INVALID_HANDLE_VALUE) {
            Print("FSTEST cat %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        } else {
            if (!ReadFile(h, buf, sizeof(buf) - 1, &n, NULL)) n = 0;
            buf[n] = 0;
            while (n && (buf[n - 1] == '\r' || buf[n - 1] == '\n')) buf[--n] = 0;
            Print("FSTEST cat %s [%s]\r\n", a1, buf);
            CloseHandle(h);
        }
    } else if (!lstrcmpiA(cmd, "dir")) {
        WIN32_FIND_DATAA fd;
        char pat[MAX_PATH];
        HANDLE f;
        int n = 0;
        wsprintfA(pat, "%s\\*", a1);
        f = FindFirstFileA(pat, &fd);
        if (f == INVALID_HANDLE_VALUE) {
            Print("FSTEST dir %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        } else {
            do {
                if (fd.cFileName[0] == '.') continue;
                Print("FSTEST dir %s: %s %u\r\n", a1, fd.cFileName, fd.nFileSizeLow);
                n++;
            } while (FindNextFileA(f, &fd));
            FindClose(f);
            Print("FSTEST dir %s: %d entries\r\n", a1, n);
        }
    } else if (!lstrcmpiA(cmd, "write")) {
        HANDLE h = CreateFileA(a1, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        DWORD n;
        if (h == INVALID_HANDLE_VALUE) {
            Print("FSTEST write %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        } else {
            WriteFile(h, a2, lstrlenA(a2), &n, NULL);
            FlushFileBuffers(h);
            CloseHandle(h);
            Print("FSTEST write %s OK\r\n", a1);
        }
    } else if (!lstrcmpiA(cmd, "hold")) {
        /* keep a file open (like File Manager watching a drive) */
        HANDLE h = CreateFileA(a1, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        Print("FSTEST hold %s %s\r\n", a1, h != INVALID_HANDLE_VALUE ? "OPEN" : "FAILED");
        Sleep(Num(a2));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else if (!lstrcmpiA(cmd, "fill")) {
        /* write N MB of a pattern, read it back and compare */
        HANDLE h = CreateFileA(a1, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        static DWORD buf[16384];
        DWORD mb = Num(a2), i, k, n, bad = 0;
        if (h == INVALID_HANDLE_VALUE) {
            Print("FSTEST fill %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        } else {
            for (i = 0; i < mb * 16 && !bad; i++) {
                for (k = 0; k < 16384; k++) buf[k] = i * 16384 + k;
                if (!WriteFile(h, buf, sizeof(buf), &n, NULL) || n != sizeof(buf)) bad = 1;
            }
            SetFilePointer(h, 0, NULL, FILE_BEGIN);
            for (i = 0; i < mb * 16 && !bad; i++) {
                if (!ReadFile(h, buf, sizeof(buf), &n, NULL) || n != sizeof(buf)) { bad = 2; break; }
                for (k = 0; k < 16384; k++) if (buf[k] != i * 16384 + k) { bad = 3; break; }
            }
            CloseHandle(h);
            Print("FSTEST fill %s %u MB %s (%u)\r\n", a1, mb, bad ? "ERROR" : "OK", bad ? GetLastError() : 0);
        }
    } else if (!lstrcmpiA(cmd, "sum")) {
        HANDLE h = CreateFileA(a1, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        DWORD hash = 2166136261UL, total = 0, n, i;
        static UCHAR buf[65536];
        if (h == INVALID_HANDLE_VALUE) {
            Print("FSTEST sum %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        } else {
            while (ReadFile(h, buf, sizeof(buf), &n, NULL) && n != 0) {
                for (i = 0; i < n; i++) hash = (hash ^ buf[i]) * 16777619UL;
                total += n;
            }
            CloseHandle(h);
            Print("FSTEST sum %s %u %08X\r\n", a1, total, hash);
        }
    } else if (!lstrcmpiA(cmd, "disk")) {
        rc = DiskTest();
    } else if (!lstrcmpiA(cmd, "parts")) {
        rc = Parts(a1[0] ? Num(a1) : 0xFFFFFFFF);
    } else if (!lstrcmpiA(cmd, "clicker")) {
        t0 = GetTickCount();
        /* clicker STOPFILE ms: until STOPFILE exists */
        while (GetTickCount() - t0 < Num(a2) && !Exists(a1)) {
            EnumWindows(TopCb, 0);
            Sleep(500);
        }
        Print("FSTEST clicker: %d clicks\r\n", Clicks);
    } else if (!lstrcmpiA(cmd, "front")) {
        /* front TITLE MS: for MS keeps the window TITLE on top and in the foreground;
           closes what the test images show at logon (the Welcome screen of NT 4.0,
           the "a service failed" box) */
        HWND w;
        int found = 0;
        char t[64];
        int i;
        for (i = 0; a1[i] && i < 63; i++) t[i] = a1[i] == '_' ? ' ' : a1[i];    /* "Wheel_test" */
        t[i] = 0;
        t0 = GetTickCount();
        while (GetTickCount() - t0 < Num(a2)) {
            HWND x = FindWindowA(NULL, "Welcome");
            if (x != NULL) PostMessageA(x, WM_CLOSE, 0, 0);
            x = FindWindowA(NULL, "Service Control Manager");
            if (x != NULL) PostMessageA(x, WM_COMMAND, IDOK, 0);
            w = FindWindowA(NULL, t);
            if (w != NULL) {
                found = 1;
                SetWindowPos(w, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
                if (GetForegroundWindow() != w) SetForegroundWindow(w);
            }
            Sleep(250);
        }
        Print("FSTEST front %s %s\r\n", t, found ? "OK" : "NOT FOUND");
    } else if (!lstrcmpiA(cmd, "on")) {
        char path[MAX_PATH], line[1024], d, found = 0;
        int i, k = 0, tries;
        /* (a drive may be away for a moment: Windows installing the disk restarts it) */
        for (tries = 0; tries < 20 && !found; tries++) {
            if (tries) Sleep(1000);
            for (d = 'D'; d <= 'Z' && !found; d++) {
                wsprintfA(path, "%c:\\%s", d, a1);
                if (GetFileAttributesA(path) != 0xFFFFFFFF) found = d;
            }
        }
        if (!found) {
            Print("FSTEST on %s: no drive has it\r\n", a1);
            rc = 1;
        } else {
            STARTUPINFOA si;
            PROCESS_INFORMATION pi;
            SECURITY_ATTRIBUTES sa;
            HANDLE rd = NULL, wr = NULL;
            for (i = 0; a2[i] && k < (int)sizeof(line) - 2; i++) {
                line[k++] = (a2[i] == '?') ? found : a2[i];
            }
            line[k] = 0;
            for (i = 0; i < (int)sizeof(si); i++) ((char *)&si)[i] = 0;
            si.cb = sizeof(si);
            /* the program's output comes through a pipe and is printed (also to FSTEST_COM) */
            sa.nLength = sizeof(sa);
            sa.lpSecurityDescriptor = NULL;
            sa.bInheritHandle = TRUE;
            if (CreatePipe(&rd, &wr, &sa, 0)) {
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
                si.hStdOutput = wr;
                si.hStdError = wr;
            }
            if (CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
                if (wr != NULL) {
                    static char buf[512];
                    DWORD got;
                    CloseHandle(wr);
                    while (ReadFile(rd, buf, sizeof(buf) - 1, &got, NULL) && got > 0) {
                        buf[got] = 0;
                        Print("%s", buf);
                    }
                    CloseHandle(rd);
                }
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &rc);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                Print("FSTEST on %s (%c:) exit %u\r\n", a1, found, rc);
            } else {
                Print("FSTEST on %s: cannot run (%u)\r\n", a1, GetLastError());
                rc = 1;
            }
        }
    } else if (!lstrcmpiA(cmd, "run")) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char line[1024];
        int i;
        wsprintfA(line, "%s %s", a1, a2);
        for (i = 0; i < (int)sizeof(si); i++) ((char *)&si)[i] = 0;
        si.cb = sizeof(si);
        if (CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 120000);
            GetExitCodeProcess(pi.hProcess, &rc);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            Print("FSTEST run %s exit %u\r\n", a1, rc);
        } else {
            Print("FSTEST run %s: cannot run (%u)\r\n", a1, GetLastError());
            rc = 1;
        }
    } else if (!lstrcmpiA(cmd, "grep")) {
        HANDLE h = CreateFileA(a1, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        static char buf[65536];
        DWORD n = 0, i, start = 0, k, tl = (DWORD)lstrlenA(a2);
        if (h == INVALID_HANDLE_VALUE) {
            Print("FSTEST grep %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        } else {
            if (!ReadFile(h, buf, sizeof(buf) - 1, &n, NULL)) n = 0;
            CloseHandle(h);
            buf[n] = 0;
            for (i = 0; i <= n; i++) {
                if (buf[i] == '\n' || buf[i] == 0) {
                    BOOL hit = FALSE;
                    buf[i] = 0;
                    if (i > start && buf[i - 1] == '\r') buf[i - 1] = 0;
                    for (k = start; !hit && tl && buf[k]; k++) {
                        DWORD j;
                        for (j = 0; j < tl && buf[k + j] == a2[j]; j++) {
                        }
                        hit = (j == tl);
                    }
                    if (hit) Print("FSTEST grep: %s\r\n", buf + start);
                    start = i + 1;
                }
            }
        }
    } else if (!lstrcmpiA(cmd, "none")) {
        char path[MAX_PATH], d, found = 0;
        for (d = 'D'; d <= 'Z' && !found; d++) {
            wsprintfA(path, "%c:\\%s", d, a1);
            if (GetFileAttributesA(path) != 0xFFFFFFFF) found = d;
        }
        if (found) {
            Print("FSTEST none %s: FOUND on %c:\r\n", a1, found);
            rc = 1;
        } else {
            Print("FSTEST none %s OK\r\n", a1);
        }
    } else if (!lstrcmpiA(cmd, "devlist")) {
        rc = DevList(a1);
    } else if (!lstrcmpiA(cmd, "echo")) {
        Print("%s %s\r\n", a1, a2);
    } else if (!lstrcmpiA(cmd, "reg") || !lstrcmpiA(cmd, "regget")) {
        HKEY k = NULL;
        char *name = Arg(&a2), *val = a2, sbuf[512];
        DWORD disp, v = 0, type = 0, len;
        LONG e;
        if (!lstrcmpiA(cmd, "reg")) {
            e = RegCreateKeyExA(HKEY_LOCAL_MACHINE, a1, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, &disp);
            if (e == ERROR_SUCCESS) {
                if (val[0] == 'd' && val[5] == ':') {
                    const char *x = val + 6;
                    if (x[0] == '0' && (x[1] == 'x' || x[1] == 'X')) {
                        for (x += 2; *x; x++) {
                            v = v * 16 + (DWORD)(*x <= '9' ? *x - '0' : (*x | 0x20) - 'a' + 10);
                        }
                    } else {
                        v = Num(x);
                    }
                    e = RegSetValueExA(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
                } else {
                    e = RegSetValueExA(k, name, 0, REG_SZ, (const BYTE *)(val + 3), lstrlenA(val + 3) + 1);
                }
                RegCloseKey(k);
            }
            Print("FSTEST reg %s %s %s\r\n", a1, name, e == ERROR_SUCCESS ? "OK" : "ERROR");
            if (e != ERROR_SUCCESS) rc = 1;
        } else {
            e = RegOpenKeyExA(HKEY_LOCAL_MACHINE, a1, 0, KEY_READ, &k);
            len = sizeof(sbuf) - 1;
            if (e == ERROR_SUCCESS) {
                e = RegQueryValueExA(k, name, NULL, &type, (LPBYTE)sbuf, &len);
                RegCloseKey(k);
            }
            if (e != ERROR_SUCCESS) {
                Print("FSTEST regget %s %s NONE\r\n", a1, name);
            } else if (type == REG_DWORD) {
                Print("FSTEST regget %s %s = %u\r\n", a1, name, *(DWORD *)sbuf);
            } else {
                sbuf[len < sizeof(sbuf) ? len : sizeof(sbuf) - 1] = 0;
                Print("FSTEST regget %s %s = [%s]\r\n", a1, name, sbuf);
            }
        }
    } else if (!lstrcmpiA(cmd, "start")) {
        SC_HANDLE m = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS), sv;
        sv = m ? OpenServiceA(m, a1, SERVICE_ALL_ACCESS) : NULL;
        if (sv != NULL && StartServiceA(sv, 0, NULL)) {
            Print("FSTEST start %s OK\r\n", a1);
        } else {
            Print("FSTEST start %s ERROR %u\r\n", a1, GetLastError());
            rc = 1;
        }
        if (sv) CloseServiceHandle(sv);
        if (m) CloseServiceHandle(m);
    } else if (!lstrcmpiA(cmd, "sleep")) {
        Sleep(Num(a1));
    } else {
        Print("usage: fstest wait|gone|find|svc|cat|dir|write|sleep ...\r\n");
        rc = 2;
    }
    ExitProcess(rc);
}
