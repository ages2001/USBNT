/*
 * usbeject.c - prepares a USB disk for removal on Windows NT 3.1 - 4.0.
 *
 * usbeject D:        flush, lock and dismount D:, then release the USB disk
 * usbeject D: /f     dismount even if files are still open
 * usbeject /slot N   release USB disk slot N without touching a volume
 * usbeject /list     show the USB disk slots
 */

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include "../src/usbntioc.h"

#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_DISMOUNT_VOLUME   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

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

static const char *SkipSpace(const char *p)
{
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static const char *SkipArg0(const char *p)
{
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            p++;
        }
        if (*p) {
            p++;
        }
    } else {
        while (*p && *p != ' ' && *p != '\t') {
            p++;
        }
    }
    return SkipSpace(p);
}

static BOOL QuerySlots(HANDLE Dev, USBNT_SLOTS *S)
{
    DWORD got = 0;

    return DeviceIoControl(Dev, IOCTL_USBNT_QUERY_SLOTS, NULL, 0, S, sizeof(*S), &got, NULL) &&
           got == sizeof(*S);
}

static void Trim(char *Dst, const UCHAR *Src, int Len)
{
    int i;

    for (i = 0; i < Len; i++) {
        Dst[i] = (Src[i] >= 32 && Src[i] < 127) ? (char)Src[i] : ' ';
    }
    Dst[Len] = 0;
    while (Len > 0 && Dst[Len - 1] == ' ') {
        Dst[--Len] = 0;
    }
}

static void List(HANDLE Dev)
{
    USBNT_SLOTS s;
    DWORD i;

    if (!QuerySlots(Dev, &s)) {
        Print("Cannot query the USB driver.\r\n");
        return;
    }
    Print("USB disk slots on SCSI port %lu:\r\n", s.PortNumber);
    for (i = 0; i < s.SlotCount && i < USBNT_MAX_SLOTS; i++) {
        char v[9], p[17];
        Trim(v, s.Slot[i].Vendor, 8);
        Trim(p, s.Slot[i].Product, 16);
        if (s.Slot[i].Present) {
            Print("  slot %lu: %s %s\r\n", i, v, p);
        } else {
            Print("  slot %lu: empty\r\n", i);
        }
    }
}

static BOOL EjectSlot(HANDLE Dev, DWORD Slot)
{
    DWORD got = 0;

    if (!DeviceIoControl(Dev, IOCTL_USBNT_EJECT, &Slot, sizeof(Slot), NULL, 0, &got, NULL)) {
        Print("The driver refused to release slot %lu (error %lu).\r\n", Slot, GetLastError());
        return FALSE;
    }
    Print("The USB disk in slot %lu can now be removed safely.\r\n", Slot);
    return TRUE;
}

static int EjectDrive(HANDLE Dev, char Letter, BOOL Force)
{
    char path[8];
    HANDLE vol;
    DWORD got = 0;
    SCSI_ADDRESS addr;
    USBNT_SLOTS s;
    DWORD slot = 0xFFFFFFFF;
    DWORD i;
    DWORD present = 0;

    if (!QuerySlots(Dev, &s)) {
        Print("Cannot query the USB driver.\r\n");
        return 1;
    }
    wsprintfA(path, "\\\\.\\%c:", Letter);
    vol = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);
    if (vol == INVALID_HANDLE_VALUE) {
        Print("Cannot open drive %c: (error %lu).\r\n", Letter, GetLastError());
        return 1;
    }
    if (DeviceIoControl(vol, IOCTL_SCSI_GET_ADDRESS, NULL, 0, &addr, sizeof(addr), &got, NULL)) {
        if (addr.PortNumber != s.PortNumber) {
            Print("Drive %c: is not a USB disk.\r\n", Letter);
            CloseHandle(vol);
            return 1;
        }
        slot = addr.TargetId;
    } else {
        for (i = 0; i < s.SlotCount; i++) {
            if (s.Slot[i].Present) {
                present++;
                slot = i;
            }
        }
        if (present != 1) {
            Print("Cannot tell which USB disk drive %c: is; use usbeject /list and /slot N.\r\n", Letter);
            CloseHandle(vol);
            return 1;
        }
    }
    /* the cache manager may keep recently used files open for a few seconds */
    for (i = 0; i < 20; i++) {
        FlushFileBuffers(vol);
        if (DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
            break;
        }
        Sleep(500);
    }
    if (i == 20) {
        if (!Force) {
            Print("Drive %c: is in use; close all files and windows on it and try again,\r\n"
                  "or use usbeject %c: /f to dismount it anyway.\r\n", Letter, Letter);
            CloseHandle(vol);
            return 1;
        }
        Print("Drive %c: is in use, dismounting anyway.\r\n", Letter);
    }
    DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
    i = EjectSlot(Dev, slot) ? 0 : 1;
    CloseHandle(vol);
    return (int)i;
}

void __stdcall Entry(void)
{
    const char *p = SkipArg0(GetCommandLineA());
    HANDLE dev;
    int rc = 0;

    Out = GetStdHandle(STD_OUTPUT_HANDLE);
    dev = CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (dev == INVALID_HANDLE_VALUE) {
        Print("The USB driver (usbnt.sys) is not running.\r\n");
        ExitProcess(2);
    }
    if (*p == 0 || (p[0] == '/' && (p[1] == 'l' || p[1] == 'L'))) {
        if (*p == 0) {
            Print("usage: usbeject D: [/f]  |  usbeject /slot N  |  usbeject /list\r\n");
        }
        List(dev);
    } else if (p[0] == '/' && (p[1] == 's' || p[1] == 'S')) {
        const char *q = p + 2;
        DWORD n = 0;
        while (*q && *q != ' ') {
            q++;
        }
        q = SkipSpace(q);
        while (*q >= '0' && *q <= '9') {
            n = n * 10 + (DWORD)(*q - '0');
            q++;
        }
        rc = EjectSlot(dev, n) ? 0 : 1;
    } else if (((p[0] >= 'a' && p[0] <= 'z') || (p[0] >= 'A' && p[0] <= 'Z')) && p[1] == ':') {
        char letter = (char)((p[0] >= 'a') ? p[0] - 32 : p[0]);
        const char *q = SkipSpace(p + 2);
        BOOL force = (q[0] == '/' || q[0] == '-') && (q[1] == 'f' || q[1] == 'F');
        rc = EjectDrive(dev, letter, force);
    } else {
        Print("usage: usbeject D: [/f]  |  usbeject /slot N  |  usbeject /list\r\n");
        rc = 1;
    }
    CloseHandle(dev);
    ExitProcess((UINT)rc);
}
