/*
 * pnpeject.c - Windows 2000 / XP: a USBNT disk slot is taken away the way
 * Safely Remove Hardware does it (CM_Request_Device_Eject on the disk the
 * USBNT device bus reports for the slot), so Windows closes the volumes
 * first and says who still uses the disk.  usbeject and usbtree use it; on
 * NT 3.1 - 4.0 (no Plug and Play) PnpEjectSlot returns -1 and they ask
 * usbnt.sys directly.
 *
 * SetupAPI is loaded at run time: the tools also run on NT 3.1.
 */

#include <windows.h>
#include <setupapi.h>
#include "pnpeject.h"

typedef HDEVINFO (WINAPI *P_GetClassDevs)(const GUID *, PCSTR, HWND, DWORD);
typedef BOOL (WINAPI *P_EnumDeviceInfo)(HDEVINFO, DWORD, PSP_DEVINFO_DATA);
typedef BOOL (WINAPI *P_GetInstanceId)(HDEVINFO, PSP_DEVINFO_DATA, PSTR, DWORD, PDWORD);
typedef BOOL (WINAPI *P_DestroyList)(HDEVINFO);
typedef DWORD (WINAPI *P_Eject)(DWORD, PDWORD, LPSTR, ULONG, ULONG);

/* PNP_VETO_TYPE */
static const char *VetoText[] = {
    "refused", "a legacy device", "files are still being closed", "a program uses it",
    "a service uses it", "files or windows are open on it", "another device needs it", "a driver refused",
    "the request was not valid", "not enough power", "it cannot be stopped", "a legacy driver",
    "you lack the rights (log on as an administrator)",
};

static BOOL Prefix(const char *S, const char *P)
{
    while (*P) {
        char c = *S++;
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c != *P++) {
            return FALSE;
        }
    }
    return TRUE;
}

static void CopyN(char *D, const char *S, int N)
{
    while (N > 1 && *S) {
        *D++ = *S++;
        N--;
    }
    *D = 0;
}

static BOOL PnpSystem(void)
{
    DWORD v = GetVersion();

    return (v & 0x80000000) == 0 && (v & 0xFF) >= 5;
}

int PnpEjectSlot(DWORD Slot, char *Veto, int VetoSize)
{
    HMODULE s;
    P_GetClassDevs getDevs;
    P_EnumDeviceInfo enumInfo;
    P_GetInstanceId getId;
    P_DestroyList destroy;
    P_Eject eject;
    HDEVINFO h;
    SP_DEVINFO_DATA d;
    DWORD i, inst = 0;
    char id[400], tail[8];
    int r = -1;

    if (Veto != NULL && VetoSize > 0) {
        Veto[0] = 0;
    }
    if (!PnpSystem() || (s = LoadLibraryA("setupapi.dll")) == NULL) {
        return -1;
    }
    getDevs = (P_GetClassDevs)GetProcAddress(s, "SetupDiGetClassDevsA");
    enumInfo = (P_EnumDeviceInfo)GetProcAddress(s, "SetupDiEnumDeviceInfo");
    getId = (P_GetInstanceId)GetProcAddress(s, "SetupDiGetDeviceInstanceIdA");
    destroy = (P_DestroyList)GetProcAddress(s, "SetupDiDestroyDeviceInfoList");
    eject = (P_Eject)GetProcAddress(s, "CM_Request_Device_EjectA");
    if (getDevs == NULL || enumInfo == NULL || getId == NULL || destroy == NULL || eject == NULL) {
        return -1;
    }
    /* the disk PDOs of the bus: USBNT\Disk&...\<parent>&<identity>&<slot> or USBNT\CdRom&... */
    wsprintfA(tail, "&%lX", Slot);
    h = getDevs(NULL, "USBNT", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (h == INVALID_HANDLE_VALUE) {
        return -1;
    }
    for (i = 0; inst == 0; i++) {
        int n, t = lstrlenA(tail);
        d.cbSize = sizeof(d);
        if (!enumInfo(h, i, &d)) {
            break;
        }
        if (!getId(h, &d, id, sizeof(id), NULL)) {
            continue;
        }
        n = lstrlenA(id);
        if ((Prefix(id, "USBNT\\DISK") || Prefix(id, "USBNT\\CDROM")) && n > t && Prefix(id + n - t, tail)) {
            inst = d.DevInst;
        }
    }
    destroy(h);
    if (inst != 0) {
        DWORD vt = 0;
        char name[MAX_PATH];
        DWORD cr;
        name[0] = 0;
        cr = eject(inst, &vt, name, sizeof(name), 0);
        if (cr == 0 && vt == 0) {
            r = 1;
        } else {
            r = 0;
            if (Veto != NULL && VetoSize > 0) {
                char buf[MAX_PATH + 96];
                wsprintfA(buf, "%s%s%s", vt < sizeof(VetoText) / sizeof(VetoText[0]) ? VetoText[vt] : "refused",
                          name[0] ? ": " : "", name);
                CopyN(Veto, buf, VetoSize);
            }
        }
    }
    return r;
}
