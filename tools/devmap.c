/*
 * devmap.c - finds the NT disk and CD-ROM devices behind the usbnt slots
 * and the drive letters that point at them. Shared by usbmon and usbtree.
 */

#include "devmap.h"

#define PROBE_NAME "UsbNtProbe"

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
    HANDLE h;
    DWORD got = 0;
    BOOL ok;

    if (!DefineDosDeviceA(DDD_RAW_TARGET_PATH, PROBE_NAME, NtPath)) {
        return FALSE;
    }
    h = CreateFileA("\\\\.\\" PROBE_NAME, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    ok = FALSE;
    if (h != INVALID_HANDLE_VALUE) {
        ok = DeviceIoControl(h, IOCTL_SCSI_GET_ADDRESS, NULL, 0, Addr, sizeof(*Addr), &got, NULL);
        CloseHandle(h);
    }
    DefineDosDeviceA(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE, PROBE_NAME, NtPath);
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
    }
    for (i = 0; i < 32; i++) {
        wsprintfA(path, "\\Device\\Harddisk%d\\Partition0", i);
        if (ProbeAddress(path, &a) && a.PortNumber == ScsiPort && a.TargetId < Count) {
            wsprintfA(Map[a.TargetId].Path, "\\Device\\Harddisk%d\\Partition1", i);
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

BOOL DevMapLetterUsed(char Letter)
{
    char name[4];
    char target[256];

    name[0] = Letter;
    name[1] = ':';
    name[2] = 0;
    return QueryDosDeviceA(name, target, sizeof(target)) != 0;
}

BOOL DevMapQueryTree(HANDLE Dev, USBNT_TREE *Tree)
{
    DWORD got = 0;

    return DeviceIoControl(Dev, IOCTL_USBNT_QUERY_TREE, NULL, 0, Tree, sizeof(*Tree), &got, NULL) &&
           got == sizeof(*Tree) && Tree->Version == USBNT_TREE_VERSION;
}
