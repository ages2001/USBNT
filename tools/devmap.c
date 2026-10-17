/*
 * devmap.c - finds the NT disk and CD-ROM devices behind the usbnt slots
 * and the drive letters that point at them. Shared by usbmon and usbtree.
 */

#include "devmap.h"

#ifndef IOCTL_DISK_GET_PARTITION_INFO
#define IOCTL_DISK_GET_PARTITION_INFO   CTL_CODE(IOCTL_DISK_BASE, 0x0001, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif
#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_DISMOUNT_VOLUME   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#define IOCTL_STORAGE_GET_DEVICE_NUMBER_    0x002D1080

typedef struct _DEV_NUMBER {
    DWORD   DeviceType;
    DWORD   DeviceNumber;
    DWORD   PartitionNumber;
} DEV_NUMBER;

/* a DOS device name of our own (usbmon, usbtree and usbeject may run at the same time) */
static const char *ProbeName(void)
{
    static char name[24];

    if (name[0] == 0) {
        wsprintfA(name, "UsbNtProbe%lu", GetCurrentProcessId());
    }
    return name;
}

static HANDLE ProbeOpen(const char *NtPath, DWORD Access)
{
    char dos[32];
    HANDLE h;

    if (!DefineDosDeviceA(DDD_RAW_TARGET_PATH, ProbeName(), NtPath)) {
        return INVALID_HANDLE_VALUE;
    }
    wsprintfA(dos, "\\\\.\\%s", ProbeName());
    h = CreateFileA(dos, Access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    DefineDosDeviceA(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, ProbeName(), NtPath);
    return h;
}

static int Lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

int DevMapSame(const char *A, const char *B)
{
    while (*A && Lower(*A) == Lower(*B)) {
        A++;
        B++;
    }
    return *A == 0 && *B == 0;
}

/* SCSI address of an NT device, opened without touching its media */
static BOOL ProbeAddress(const char *NtPath, SCSI_ADDRESS *Addr)
{
    HANDLE h = ProbeOpen(NtPath, 0);
    DWORD got = 0;
    BOOL ok = FALSE;

    if (h != INVALID_HANDLE_VALUE) {
        ok = DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, NULL, 0, Addr, sizeof(*Addr), &got, NULL);
        CloseHandle(h);
    }
    return ok;
}

int DevMapSlots(ULONG ScsiPort, DEV_SLOT_MAP *Map, int Count)
{
    char path[64];
    SCSI_ADDRESS a;
    int found = 0;
    int i;

    for (i = 0; i < Count; i++) {
        Map[i].Path[0] = 0;
        Map[i].Cd = 0;
        Map[i].Disk = -1;
    }
    for (i = 0; i < 32; i++) {
        wsprintfA(path, "\\Device\\Harddisk%d\\Partition0", i);
        if (ProbeAddress(path, &a) && a.PortNumber == ScsiPort && a.TargetId < Count) {
            wsprintfA(Map[a.TargetId].Path, "\\Device\\Harddisk%d\\Partition1", i);
            Map[a.TargetId].Disk = i;
            found++;
        }
    }
    for (i = 0; i < 16; i++) {
        wsprintfA(path, "\\Device\\CdRom%d", i);
        if (ProbeAddress(path, &a) && a.PortNumber == ScsiPort && a.TargetId < Count) {
            lstrcpyA(Map[a.TargetId].Path, path);
            Map[a.TargetId].Cd = 1;
            found++;
        }
    }
    return found;
}

char DevMapLetter(const char *NtPath)
{
    char name[4];
    char target[256];
    char c;

    if (NtPath[0] == 0) {
        return 0;
    }
    for (c = 'A'; c <= 'Z'; c++) {
        name[0] = c;
        name[1] = ':';
        name[2] = 0;
        if (QueryDosDeviceA(name, target, sizeof(target)) != 0 && DevMapSame(target, NtPath)) {
            return c;
        }
    }
    return 0;
}

void DevMapPartPath(const DEV_SLOT_MAP *M, int Part, char *Path)
{
    if (M->Disk < 0) {
        Path[0] = 0;
    } else {
        wsprintfA(Path, "\\Device\\Harddisk%d\\Partition%d", M->Disk, Part);
    }
}

/* NT 3.x / 4.0: partition device Part of the slot's disk maps a partition of the medium now
   (1; the disk driver maps it to the medium when it is asked, and a file system mounts),
   exists but maps nothing (0), does not exist (-1) */
int DevMapPartInUse(const DEV_SLOT_MAP *M, int Part)
{
    char path[64];
    HANDLE h;
    PARTITION_INFORMATION pi;
    DWORD got = 0;
    int r;

    DevMapPartPath(M, Part, path);
    if (path[0] == 0) {
        return -1;
    }
    h = ProbeOpen(path, 0);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    CloseHandle(h);
    h = ProbeOpen(path, GENERIC_READ);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    /* (NT 3.1 takes an extended partition of type 0F for a partition of its own) */
    r = DeviceIoControl(h, IOCTL_DISK_GET_PARTITION_INFO, NULL, 0, &pi, sizeof(pi), &got, NULL) &&
        pi.PartitionLength.QuadPart != 0 && pi.PartitionType != 0x05 && pi.PartitionType != 0x0F &&
        pi.PartitionType != 0x85;
    CloseHandle(h);
    return r;
}

/* number n of a "\Device\HarddiskD\Partitionn" path on disk Disk, 0 if it is something else */
static int PartOf(const char *T, int Disk)
{
    static const char a[] = "\\device\\harddisk", b[] = "\\partition";
    int i, d = 0, n = 0;

    for (i = 0; a[i]; i++, T++) {
        if (Lower(*T) != a[i]) return 0;
    }
    if (*T < '0' || *T > '9') return 0;
    while (*T >= '0' && *T <= '9') d = d * 10 + (*T++ - '0');
    for (i = 0; b[i]; i++, T++) {
        if (Lower(*T) != b[i]) return 0;
    }
    if (*T < '0' || *T > '9') return 0;
    while (*T >= '0' && *T <= '9') n = n * 10 + (*T++ - '0');
    return (*T == 0 && d == Disk) ? n : 0;
}

/* the drive letters on the slot's disk, by partition number (Parts may be NULL); returns how many */
int DevMapSlotLetters(const DEV_SLOT_MAP *M, char *Letters, int *Parts, int Max)
{
    char name[8], target[256];
    int n = 0, i, k, part[26];
    char c, l[26];
    BOOL nt5 = (GetVersion() & 0x80000000) == 0 && (GetVersion() & 0xFF) >= 5;

    if (M->Disk < 0) {
        return 0;
    }
    for (c = 'C'; c <= 'Z'; c++) {
        int p = 0;
        name[0] = c;
        name[1] = ':';
        name[2] = 0;
        if (QueryDosDeviceA(name, target, sizeof(target)) == 0) {
            continue;
        }
        p = PartOf(target, M->Disk);
        if (p == 0 && nt5) {
            /* Windows 2000 / XP: the letters name volumes (\Device\HarddiskVolumeN) */
            DEV_NUMBER dn;
            DWORD got = 0;
            HANDLE h;
            char dos[8];
            wsprintfA(dos, "\\\\.\\%c:", c);
            h = CreateFileA(dos, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                if (DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER_, NULL, 0, &dn, sizeof(dn), &got, NULL) &&
                    dn.DeviceType == FILE_DEVICE_DISK && (int)dn.DeviceNumber == M->Disk) {
                    p = dn.PartitionNumber ? (int)dn.PartitionNumber : 1;
                }
                CloseHandle(h);
            }
        }
        if (p > 0 && n < 26) {
            l[n] = c;
            part[n++] = p;
        }
    }
    /* by partition number */
    for (i = 1; i < n; i++) {
        for (k = i; k > 0 && part[k - 1] > part[k]; k--) {
            int tp = part[k]; char tl = l[k];
            part[k] = part[k - 1]; l[k] = l[k - 1];
            part[k - 1] = tp; l[k - 1] = tl;
        }
    }
    for (i = 0; i < n && i < Max; i++) {
        Letters[i] = l[i];
        if (Parts != NULL) {
            Parts[i] = part[i];
        }
    }
    Letters[i] = 0;                             /* Letters holds Max + 1 characters */
    return i;
}

/* NT 3.x / 4.0, before a slot is released: every volume on the slot's disk flushed, locked
   (Tries times, half a second apart) and dismounted.  Returns 0 when done (with Force also when
   a volume could not be locked), else the drive letter that is in use (nothing then stays
   locked).  L holds the handles until DevMapUnlock. */
char DevMapLockSlot(const DEV_SLOT_MAP *M, BOOL Force, int Tries, DEV_LOCKED *L)
{
    char letters[27], path[8];
    int n = DevMapSlotLetters(M, letters, NULL, 26), i, t;
    DWORD got = 0;

    L->Count = 0;
    for (i = 0; i < n; i++) {
        HANDLE v;
        wsprintfA(path, "\\\\.\\%c:", letters[i]);
        v = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                        0, NULL);
        if (v == INVALID_HANDLE_VALUE) {
            continue;                           /* no medium behind it, nothing mounted */
        }
        /* the cache manager may keep recently used files open for a few seconds */
        for (t = 0; t < Tries; t++) {
            FlushFileBuffers(v);
            if (DeviceIoControl(v, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
                break;
            }
            Sleep(500);
        }
        L->Vol[L->Count] = v;
        L->Letter[L->Count++] = letters[i];
        if (t == Tries && !Force) {
            char busy = letters[i];
            DevMapUnlock(L);
            return busy;
        }
    }
    for (i = 0; i < L->Count; i++) {
        DeviceIoControl(L->Vol[i], FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
    }
    return 0;
}

void DevMapUnlock(DEV_LOCKED *L)
{
    int i;

    for (i = 0; i < L->Count; i++) {
        CloseHandle(L->Vol[i]);
    }
    L->Count = 0;
}

BOOL DevMapLetterUsed(char Letter)
{
    char name[4];
    char target[256];

    name[0] = Letter;
    name[1] = ':';
    name[2] = 0;
    return QueryDosDeviceA(name, target, sizeof(target)) != 0;
}

/* the tree with the card readers and HID over I2C devices (older drivers:
   ExtraCount 0) */
BOOL DevMapQueryTree2(HANDLE Dev, USBNT_TREE2 *Tree)
{
    DWORD got = 0;

    if (!DeviceIoControl(Dev, IOCTL_USBNT_QUERY_TREE, NULL, 0, Tree, sizeof(*Tree), &got, NULL)) {
        return FALSE;
    }
    if (got == sizeof(*Tree) && Tree->Base.Version == USBNT_TREE_VERSION2) {
        return TRUE;
    }
    Tree->ExtraCount = 0;
    return got == sizeof(USBNT_TREE) && Tree->Base.Version == USBNT_TREE_VERSION;
}

BOOL DevMapQueryTree(HANDLE Dev, USBNT_TREE *Tree)
{
    DWORD got = 0;

    return DeviceIoControl(Dev, IOCTL_USBNT_QUERY_TREE, NULL, 0, Tree, sizeof(*Tree), &got, NULL) &&
           got == sizeof(*Tree) && Tree->Version == USBNT_TREE_VERSION;
}
