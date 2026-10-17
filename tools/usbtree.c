/*
 * usbtree.c - shows the USB controllers, ports and devices seen by
 * usbnt.sys, with speed and USB version, and for USB disks the drive
 * letter, size, file system and cluster size. Refreshes by itself when
 * devices come and go.
 */

#include "devmap.h"
#include <commdlg.h>
#include "fsdetect.h"
#include "pnpeject.h"

#define ID_LIST         100
#define ID_INFO         101
#define ID_REFRESH      102
#define ID_EJECT        103
#define ID_REPORT       104
#define WM_TREE_CHANGED (WM_USER + 1)

#define ITEM_NONE       0
#define ITEM_HC         1
#define ITEM_DEV        2
#define ITEM_SLOT       3
#define ITEM_PORT       4
#define ITEM_EXTRA      5
#define ITEM(kind, n)   (((DWORD)(kind) << 16) | (DWORD)(n))
#define ITEM_KIND(v)    ((int)((v) >> 16))
#define ITEM_NUM(v)     ((int)((v) & 0xFFFF))

#ifndef FSCTL_LOCK_VOLUME
#define FSCTL_LOCK_VOLUME       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 6, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_DISMOUNT_VOLUME   CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 8, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

static HINSTANCE Inst;
static HWND MainWnd;
static HWND ListWnd;
static HWND InfoWnd;
static HWND RefreshBtn;
static HWND EjectBtn, ReportBtn;
static HFONT Font;
static HBITMAP IconBmp, MaskBmp;
static HANDLE Dev;
static USBNT_TREE2 Tree2;
#define Tree (Tree2.Base)
static BOOL TreeValid;
static DEV_SLOT_MAP Map[USBNT_MAX_SLOTS];
static BOOL MapDone;
static char Text[8192];

/* ------------------------------------------------------------------ */
/* Text helpers                                                         */
/* ------------------------------------------------------------------ */

static void Cat(char *Dst, const char *Src)
{
    int n = lstrlenA(Dst);
    int m = lstrlenA(Src);

    if (n + m < (int)sizeof(Text) - 1) {
        lstrcpyA(Dst + n, Src);
    }
}

static void Line(const char *Fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, Fmt);
    wvsprintfA(buf, Fmt, ap);
    va_end(ap);
    Cat(Text, buf);
    Cat(Text, "\r\n");
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

/* 64-bit byte counts without compiler helper routines */
typedef struct _U64 {
    ULONG Hi;
    ULONG Lo;
} U64;

static U64 Mul(ULONG A, ULONG B)
{
    ULONG al = A & 0xFFFF, ah = A >> 16, bl = B & 0xFFFF, bh = B >> 16;
    ULONG ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    ULONG mid = (ll >> 16) + (lh & 0xFFFF) + (hl & 0xFFFF);
    U64 r;

    r.Lo = (ll & 0xFFFF) | (mid << 16);
    r.Hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    return r;
}

static U64 Add64(U64 A, ULONG B)
{
    U64 r;

    r.Lo = A.Lo + B;
    r.Hi = A.Hi + (r.Lo < A.Lo ? 1 : 0);
    return r;
}

static U64 Shr64(U64 V, int N)
{
    U64 r;

    if (N == 0) {
        return V;
    }
    if (N >= 32) {
        r.Lo = V.Hi >> (N - 32);
        r.Hi = 0;
    } else {
        r.Lo = (V.Lo >> N) | (V.Hi << (32 - N));
        r.Hi = V.Hi >> N;
    }
    return r;
}

/* Formats a byte count as "14.9 GB" */
static void SizeText(char *Out, U64 Bytes)
{
    static const char *units[] = { "bytes", "KB", "MB", "GB", "TB" };
    int u;
    U64 w;
    ULONG tenth;

    for (u = 4; u > 0; u--) {
        w = Shr64(Bytes, 10 * u);
        if (w.Hi != 0 || w.Lo != 0) {
            break;
        }
    }
    if (u == 0) {
        wsprintfA(Out, "%lu bytes", Bytes.Lo);
        return;
    }
    w = Shr64(Bytes, 10 * u);
    tenth = (Shr64(Bytes, 10 * u - 10).Lo & 1023) * 10 / 1024;
    wsprintfA(Out, "%lu.%lu %s", w.Lo, tenth, units[u]);
}

static const char *SpeedText(const USBNT_DEV_INFO *d)
{
    switch (d->Speed) {
    case USBNT_SPEED_LOW:   return "Low speed (USB 1), 1.5 Mbit/s";
    case USBNT_SPEED_HIGH:  return "High speed (USB 2), 480 Mbit/s";
    case USBNT_SPEED_SUPER:
        if (d->LinkGbps >= 20) return "SuperSpeed+ (USB 3.2 Gen 2x2), 20 Gbit/s";
        if (d->LinkGbps >= 10) return "SuperSpeed+ (USB 3.2 Gen 2), 10 Gbit/s";
        return "SuperSpeed (USB 3.2 Gen 1), 5 Gbit/s";
    default:                return "Full speed (USB 1), 12 Mbit/s";
    }
}

static const char *SpeedShort(const USBNT_DEV_INFO *d)
{
    switch (d->Speed) {
    case USBNT_SPEED_LOW:   return "USB1 1.5M";
    case USBNT_SPEED_HIGH:  return "USB2 480M";
    case USBNT_SPEED_SUPER:
        if (d->LinkGbps >= 20) return "USB3 20G";
        if (d->LinkGbps >= 10) return "USB3 10G";
        return "USB3 5G";
    default:                return "USB1 12M";
    }
}

/* "QEMU" + "QEMU USB Mouse" reads better as just the product name */
static BOOL StartsWith(const char *S, const char *Prefix)
{
    while (*Prefix) {
        char a = *S++;
        char b = *Prefix++;
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) {
            return FALSE;
        }
    }
    return TRUE;
}

static void UsbVersion(char *Out, USHORT Bcd)
{
    if ((Bcd & 0x0F) != 0) {
        wsprintfA(Out, "%x.%x%x", Bcd >> 8, (Bcd >> 4) & 0xF, Bcd & 0xF);
    } else {
        wsprintfA(Out, "%x.%x", Bcd >> 8, (Bcd >> 4) & 0xF);
    }
}

static const char *HcName(int Type)
{
    switch (Type) {
    case USBNT_HC_UHCI: return "UHCI (USB 1.1)";
    case USBNT_HC_OHCI: return "OHCI (USB 1.1)";
    case USBNT_HC_EHCI: return "EHCI (USB 2.0)";
    default:            return "xHCI (USB 3.x)";
    }
}

static const char *PciVendor(USHORT Id)
{
    switch (Id) {
    case 0x8086: return "Intel";
    case 0x1022: return "AMD";
    case 0x1002: return "ATI";
    case 0x1106: return "VIA";
    case 0x10DE: return "NVIDIA";
    case 0x1039: return "SiS";
    case 0x10B9: return "ALi";
    case 0x1033: return "NEC";
    case 0x1912: return "Renesas";
    case 0x1B21: return "ASMedia";
    case 0x1B73: return "Fresco Logic";
    case 0x104C: return "Texas Instruments";
    case 0x1B6F: return "Etron";
    case 0x1B36: return "QEMU";
    case 0x106B: return "Apple";
    default:     return "";
    }
}

static const char *ProtoName(int Proto)
{
    switch (Proto) {
    case 0x50: return "Bulk-Only";
    case 0x62: return "UAS";
    case 0x00:
    case 0x01: return "CBI";
    case 0xFE: return "SD card reader (PCI / vendor USB)";
    default:   return "?";
    }
}

static void Functions(char *Out, const USBNT_DEV_INFO *d)
{
    Out[0] = 0;
    if (d->Functions & USBNT_FUNC_HUB) lstrcatA(Out, "Hub, ");
    if (d->Functions & USBNT_FUNC_KEYBOARD) lstrcatA(Out, "Keyboard, ");
    if (d->Functions & USBNT_FUNC_MOUSE) lstrcatA(Out, "Mouse, ");
    if (d->Functions & USBNT_FUNC_TABLET) lstrcatA(Out, "Tablet, ");
    if (d->Functions & USBNT_FUNC_STORAGE) lstrcatA(Out, "Storage, ");
    if (d->Functions & USBNT_FUNC_AUDIO) lstrcatA(Out, "Audio, ");
    if (d->Functions & USBNT_FUNC_MIDI) lstrcatA(Out, "MIDI, ");
    if (d->Functions & USBNT_FUNC_VIDEO) lstrcatA(Out, "Camera, ");
    if (Out[0] == 0) {
        lstrcpyA(Out, "No driver");
    } else {
        Out[lstrlenA(Out) - 2] = 0;
    }
}

static void DevName(char *Out, const USBNT_DEV_INFO *d)
{
    if (d->Product[0] != 0) {
        if (d->Manufacturer[0] != 0 && !StartsWith(d->Product, d->Manufacturer)) {
            wsprintfA(Out, "%s %s", d->Manufacturer, d->Product);
        } else {
            lstrcpyA(Out, d->Product);
        }
    } else {
        wsprintfA(Out, "Device %04X:%04X", d->VendorId, d->ProductId);
    }
}

/* ------------------------------------------------------------------ */
/* Volumes                                                              */
/* ------------------------------------------------------------------ */

/* file systems found on the media themselves, cached per slot until the tree changes */
#define MAX_PARTS 8
static FS_PART Parts[USBNT_MAX_SLOTS][MAX_PARTS];
static int PartCount[USBNT_MAX_SLOTS];
static ULONG PartKey[USBNT_MAX_SLOTS];          /* ChangeCount + 1 of the detection, 0 = none */

static int SlotParts(int Slot)
{
    USBNT_SLOT_INFO *si = &Tree.Slot[Slot];
    ULONG key = Tree.ChangeCount + 1;
    if (!si->Present || !si->MediaReady) {
        PartKey[Slot] = 0;
        return -1;
    }
    if (PartKey[Slot] != key) {
        PartCount[Slot] = FsDetectSlot(Tree.ScsiPort, (ULONG)Slot, si->BlockSize, si->LastLba, Parts[Slot], MAX_PARTS);
        PartKey[Slot] = key;
    }
    return PartCount[Slot];
}

/* file system of the first partition with a recognised one, "" if none */
static const char *SlotFs(int Slot)
{
    int i, n = SlotParts(Slot);
    for (i = 0; i < n; i++)
        if (lstrcmpA(Parts[Slot][i].Fs, "unknown") && lstrcmpA(Parts[Slot][i].Fs, "unreadable")) return Parts[Slot][i].Fs;
    return n > 0 ? Parts[Slot][0].Fs : "";
}

static char SlotLetter(int Slot)
{
    char l[27];

    if (!MapDone) {
        DevMapSlots(Tree.ScsiPort, Map, USBNT_MAX_SLOTS);
        MapDone = TRUE;
    }
    if (DevMapSlotLetters(&Map[Slot], l, NULL, 26) > 0) {
        return l[0];                            /* the lowest partition with a letter */
    }
    return DevMapLetter(Map[Slot].Path);
}

/* "D: E: F:" - every drive letter on the slot's disk, "" if fewer than two */
static const char *SlotMoreLetters(int Slot, int *Count)
{
    static char text[80];
    char l[27];
    int n, i;

    SlotLetter(Slot);
    n = DevMapSlotLetters(&Map[Slot], l, NULL, 26);
    text[0] = 0;
    for (i = 0; i < n && n > 1; i++) {
        wsprintfA(text + lstrlenA(text), "%s%c:", i ? " " : "", l[i]);
    }
    if (Count != NULL) {
        *Count = n;
    }
    return text;
}

/* One line about the volume on a drive letter, Detail adds more lines */
static void VolumeText(char Letter, char *Short, BOOL Detail, const char *RawFs)
{
    char root[4];
    char label[64];
    char fs[32];
    char a[32];
    char b[32];
    DWORD serial = 0, maxlen = 0, flags = 0;
    DWORD spc = 0, bps = 0, freec = 0, total = 0;
    ULONG clus;

    root[0] = Letter;
    root[1] = ':';
    root[2] = '\\';
    root[3] = 0;
    label[0] = 0;
    fs[0] = 0;
    if (!GetVolumeInformationA(root, label, sizeof(label), &serial, &maxlen, &flags, fs, sizeof(fs))) {
        if (RawFs && RawFs[0])
            wsprintfA(Short, "%c: %s (no Windows driver mounted it)", Letter, RawFs);
        else
            wsprintfA(Short, "%c: no medium or unknown file system", Letter);
        if (Detail) {
            Line("Drive letter:   %c:", Letter);
            Line("Volume:         not mounted (no medium or no driver for the file system)");
        }
        return;
    }
    /* the file system as found on the medium (with its version) beats the name the
       driver reports ("Unknown", "FAT" for FAT32, no NTFS version, ...) */
    if (RawFs && RawFs[0] && lstrcmpA(RawFs, "unknown") && lstrcmpA(RawFs, "unreadable")) {
        int k;
        for (k = 0; k < (int)sizeof(fs) - 1 && RawFs[k]; k++) fs[k] = RawFs[k];
        fs[k] = 0;
    }
    GetDiskFreeSpaceA(root, &spc, &bps, &freec, &total);
    clus = spc * bps;
    SizeText(a, Mul(clus, total));
    wsprintfA(Short, "%c: %s, %s", Letter, fs, a);
    if (clus != 0) {
        SizeText(b, Mul(clus, 1));
        wsprintfA(Short + lstrlenA(Short), ", %s clusters", b);
    }
    if (Detail) {
        Line("Drive letter:   %c:", Letter);
        Line("Label:          %s", label[0] ? label : "(none)");
        Line("File system:    %s", fs);
        Line("Serial number:  %04X-%04X", serial >> 16, serial & 0xFFFF);
        Line("Volume size:    %s", a);
        SizeText(a, Mul(clus, freec));
        Line("Free space:     %s", a);
        SizeText(a, Mul(clus, 1));
        Line("Cluster size:   %s (%lu sectors of %lu bytes)", a, spc, bps);
        Line("Clusters:       %lu total, %lu free", total, freec);
    }
}

/* ------------------------------------------------------------------ */
/* List                                                                 */
/* ------------------------------------------------------------------ */

static void Add(DWORD Item, const char *Fmt, ...)
{
    char buf[512];
    va_list ap;
    LRESULT i;

    va_start(ap, Fmt);
    wvsprintfA(buf, Fmt, ap);
    va_end(ap);
    i = SendMessageA(ListWnd, LB_ADDSTRING, 0, (LPARAM)buf);
    if (i >= 0) {
        SendMessageA(ListWnd, LB_SETITEMDATA, (WPARAM)i, (LPARAM)Item);
    }
}

static void Indent(char *Out, int Depth)
{
    int i;

    for (i = 0; i < Depth * 4 && i < 60; i++) {
        Out[i] = ' ';
    }
    Out[i] = 0;
}

static void AddSlots(int DevIndex, int Depth)
{
    ULONG s;
    char ind[64];
    char vol[160];
    char v[12], p[20];

    Indent(ind, Depth);
    for (s = 0; s < Tree.SlotCount && s < USBNT_MAX_SLOTS; s++) {
        USBNT_SLOT_INFO *si = &Tree.Slot[s];
        char letter;
        if (!si->Present || si->DevIndex != DevIndex) {
            continue;
        }
        Trim(v, si->Vendor, 8);
        Trim(p, si->Product, 16);
        if (StartsWith(p, v)) {
            v[0] = 0;
        }
        letter = SlotLetter((int)s);
        if (letter != 0) {
            int nl = 0;
            const char *more = SlotMoreLetters((int)s, &nl);
            VolumeText(letter, vol, FALSE, SlotFs((int)s));
            if (nl > 1) {
                wsprintfA(vol + lstrlenA(vol), " (partitions: %s)", more);
            }
        } else if (SlotFs((int)s)[0]) {
            wsprintfA(vol, "%s, no drive letter", SlotFs((int)s));
        } else {
            lstrcpyA(vol, "no drive letter");
        }
        Add(ITEM(ITEM_SLOT, s), "%sLUN %u: %s - %s%s%s", ind, si->Lun, vol, v, v[0] ? " " : "", p);
    }
}

static void AddDevice(int Index, int Depth)
{
    USBNT_DEV_INFO *d = &Tree.Dev[Index];
    char name[128];
    char fn[64];
    char ind[64];
    int i;

    DevName(name, d);
    Functions(fn, d);
    Indent(ind, Depth);
    Add(ITEM(ITEM_DEV, Index), "%sPort %u [%s]: %s - %s", ind, d->Port, SpeedShort(d), fn, name);
    if (d->Functions & USBNT_FUNC_STORAGE) {
        AddSlots(Index, Depth + 1);
    }
    for (i = 0; i < USBNT_MAX_DEVS; i++) {
        if (Tree.Dev[i].Index != 0xFF && Tree.Dev[i].Parent == Index) {
            AddDevice(i, Depth + 1);
        }
    }
}

/* card readers that are not USB devices, HID over I2C */
static int Has(const char *S, const char *W);

/* disk slots that are not on a USB port: card readers (Kind READER), NVMe drives (Kind NVME) */
static void AddExtraSlots(UCHAR Kind, const char *Head)
{
    ULONG e;
    BOOL head = FALSE;

    for (e = 0; e < Tree2.ExtraCount && e < USBNT_MAX_EXTRA; e++) {
        USBNT_EXTRA_INFO *x = &Tree2.Extra[e];
        USBNT_SLOT_INFO *si;
        char vol[160], v[12], p[20];
        char letter;
        if (x->Kind != Kind || x->Slot >= USBNT_MAX_SLOTS) {
            continue;
        }
        if (!head) {
            Add(ITEM(ITEM_NONE, 0), "%s", Head);
            head = TRUE;
        }
        si = &Tree.Slot[x->Slot];
        Trim(v, si->Vendor, 8);
        Trim(p, si->Product, 16);
        letter = SlotLetter(x->Slot);
        if (!si->MediaReady || si->BlockSize == 0) {
            lstrcpyA(vol, Kind == USBNT_EXTRA_NVME ? "shut down" : "no card");
        } else if (letter != 0) {
            int nl = 0;
            const char *more = SlotMoreLetters(x->Slot, &nl);
            VolumeText(letter, vol, FALSE, SlotFs(x->Slot));
            if (nl > 1) {
                wsprintfA(vol + lstrlenA(vol), " (partitions: %s)", more);
            }
        } else if (SlotFs(x->Slot)[0]) {
            wsprintfA(vol, "%s, no drive letter", SlotFs(x->Slot));
        } else {
            lstrcpyA(vol, "no drive letter");
        }
        if (Kind == USBNT_EXTRA_NVME) {
            Add(ITEM(ITEM_SLOT, x->Slot), "    %s: %s", x->Name, vol);
        } else {
            Add(ITEM(ITEM_SLOT, x->Slot), "    %s: %s - %s", x->Name, vol, p);
        }
    }
}

/* card readers and NVMe drives that are not USB devices, Thunderbolt, HID over I2C */
static void AddExtra(void)
{
    ULONG e;
    BOOL head = FALSE;

    AddExtraSlots(USBNT_EXTRA_READER, "SD card readers");
    AddExtraSlots(USBNT_EXTRA_NVME, "NVMe drives (Thunderbolt / USB4, PCIe hot plug)");
    head = FALSE;
    for (e = 0; e < Tree2.ExtraCount && e < USBNT_MAX_EXTRA; e++) {
        USBNT_EXTRA_INFO *x = &Tree2.Extra[e];
        if (x->Kind != USBNT_EXTRA_TBT) {
            continue;
        }
        if (!head) {
            Add(ITEM(ITEM_NONE, 0), "Thunderbolt / USB4");
            head = TRUE;
        }
        Add(ITEM(ITEM_EXTRA, e), "%s%s", Has(x->Name, "host") ? "    " : "        ", x->Name);
    }
    head = FALSE;
    for (e = 0; e < Tree2.ExtraCount && e < USBNT_MAX_EXTRA; e++) {
        USBNT_EXTRA_INFO *x = &Tree2.Extra[e];
        if (x->Kind != USBNT_EXTRA_I2C_CTRL && x->Kind != USBNT_EXTRA_I2C_HID) {
            continue;
        }
        if (!head) {
            Add(ITEM(ITEM_NONE, 0), "HID over I2C and SMBus (touch pads, touch screens)");
            head = TRUE;
        }
        Add(ITEM(ITEM_EXTRA, e), "%s%s", x->Kind == USBNT_EXTRA_I2C_CTRL ? "    " : "        ", x->Name);
    }
}

static void ExtraDetails(int e)
{
    USBNT_EXTRA_INFO *x = &Tree2.Extra[e];
    char line[200];
    int i = 0, k = 0;

    Line("%s", x->Name);
    for (;;) {
        char ch = x->Detail[i++];
        if (ch == '\n' || ch == 0) {
            line[k] = 0;
            if (k) Line("%s", line);
            k = 0;
            if (ch == 0) break;
        } else if (k < (int)sizeof(line) - 1) {
            line[k++] = ch;
        }
    }
}

static void FillList(void)
{
    LRESULT sel = SendMessageA(ListWnd, LB_GETCURSEL, 0, 0);
    DWORD selItem = (sel >= 0) ? (DWORD)SendMessageA(ListWnd, LB_GETITEMDATA, (WPARAM)sel, 0) : 0;
    ULONG h;
    int i;
    int p;
    int count;

    SendMessageA(ListWnd, WM_SETREDRAW, FALSE, 0);
    SendMessageA(ListWnd, LB_RESETCONTENT, 0, 0);
    TreeValid = DevMapQueryTree2(Dev, &Tree2);
    if (!TreeValid) {
        Add(ITEM(ITEM_NONE, 0), "The USB driver (usbnt.sys) does not answer.");
    } else {
        for (h = 0; h < Tree.HcCount && h < USBNT_MAX_HCS; h++) {
            USBNT_HC_INFO *hc = &Tree.Hc[h];
            Add(ITEM(ITEM_HC, h), "%s - %s %04X:%04X at %02X:%02X.%u, %u ports%s", HcName(hc->Type),
                PciVendor(hc->VendorId), hc->VendorId, hc->DeviceId, hc->Bus, hc->Device, hc->Function,
                hc->NumPorts, hc->Running == 2 ? " (unplugged)" : hc->Running == 3 ? " (hot plugged, polled)" :
                hc->Running ? "" : " (not running)");
            for (p = 1; p <= hc->NumPorts; p++) {
                BOOL used = FALSE;
                for (i = 0; i < USBNT_MAX_DEVS; i++) {
                    USBNT_DEV_INFO *d = &Tree.Dev[i];
                    if (d->Index != 0xFF && d->Hc == h && d->Parent == 0xFF && d->RootPort == p) {
                        AddDevice(i, 1);
                        used = TRUE;
                    }
                }
                if (!used) {
                    const char *kind = "";
                    if (hc->Type == USBNT_HC_XHCI && p <= USBNT_MAX_PORTS) {
                        kind = (hc->PortProto[p - 1] == 3) ? " (USB 3 port)" : " (USB 2 port)";
                    }
                    Add(ITEM(ITEM_PORT, (h << 8) | p), "    Port %u%s: -", p, kind);
                }
            }
        }
        AddExtra();
    }
    count = (int)SendMessageA(ListWnd, LB_GETCOUNT, 0, 0);
    for (i = 0; i < count; i++) {
        if ((DWORD)SendMessageA(ListWnd, LB_GETITEMDATA, (WPARAM)i, 0) == selItem && selItem != 0) {
            SendMessageA(ListWnd, LB_SETCURSEL, (WPARAM)i, 0);
            break;
        }
    }
    SendMessageA(ListWnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ListWnd, NULL, TRUE);
}

/* ------------------------------------------------------------------ */
/* Details                                                              */
/* ------------------------------------------------------------------ */

static void HcDetails(int h)
{
    USBNT_HC_INFO *hc = &Tree.Hc[h];
    int p;

    Line("Controller:     %s", HcName(hc->Type));
    Line("PCI device:     %s %04X:%04X", PciVendor(hc->VendorId), hc->VendorId, hc->DeviceId);
    Line("PCI location:   bus %u, device %u, function %u", hc->Bus, hc->Device, hc->Function);
    Line("Interrupt line: %u", hc->Irq);
    Line("Root ports:     %u", hc->NumPorts);
    Line("State:          %s", hc->Running == 2 ? "unplugged" : hc->Running == 3 ? "running, hot plugged (polled)" :
         hc->Running ? "running" : "not running");
    if (hc->Companion != 0xFF) {
        Line("Companion of:   controller %u (EHCI); low and full speed devices on its ports", hc->Companion);
    }
    if (hc->Type == USBNT_HC_XHCI) {
        Cat(Text, "USB 3 ports:    ");
        for (p = 1; p <= hc->NumPorts && p <= USBNT_MAX_PORTS; p++) {
            if (hc->PortProto[p - 1] == 3) {
                char n[8];
                wsprintfA(n, "%u ", p);
                Cat(Text, n);
            }
        }
        Cat(Text, "\r\n");
    }
}

static void DevDetails(int i)
{
    USBNT_DEV_INFO *d = &Tree.Dev[i];
    char ver[16];
    char rel[16];
    char fn[64];
    char path[64];
    int depth = 0;
    int cur = i;
    int chain[8];
    int k;

    UsbVersion(ver, d->UsbVersion);
    UsbVersion(rel, d->DeviceVersion);
    Functions(fn, d);
    Line("Manufacturer:   %s", d->Manufacturer[0] ? d->Manufacturer : "-");
    Line("Product:        %s", d->Product[0] ? d->Product : "-");
    Line("Serial number:  %s", d->Serial[0] ? d->Serial : "-");
    Line("Vendor/product: %04X:%04X, release %s", d->VendorId, d->ProductId, rel);
    Line("USB version:    %s", ver);
    Line("Speed:          %s", SpeedText(d));
    Line("Class:          %02X / %02X / %02X, %u interface(s)", d->Class, d->SubClass, d->Protocol, d->Interfaces);
    Line("Driver:         %s", fn);
    if (d->Functions & USBNT_FUNC_HUB) {
        Line("Hub ports:      %u", d->HubPorts);
    }
    Line("Max power:      %u mA", (ULONG)d->MaxPower * (d->Speed == USBNT_SPEED_SUPER ? 8 : 2));
    Line("Address:        %u", d->Address);
    while (cur != 0xFF && depth < 8) {
        chain[depth++] = cur;
        cur = Tree.Dev[cur].Parent;
    }
    lstrcpyA(path, "");
    for (k = depth - 1; k >= 0; k--) {
        char n[8];
        wsprintfA(n, k == depth - 1 ? "%u" : "-%u", Tree.Dev[chain[k]].Port);
        lstrcatA(path, n);
    }
    Line("Location:       controller %u, port %s", d->Hc, path);
}

static void SlotDetails(int s)
{
    USBNT_SLOT_INFO *si = &Tree.Slot[s];
    char v[12], p[20], r[8];
    char a[32];
    char letter;

    Trim(v, si->Vendor, 8);
    Trim(p, si->Product, 16);
    Trim(r, si->Revision, 4);
    Line("Disk:           %s %s %s", v, p, r);
    Line("Type:           %s", si->DeviceType == 5 ? "CD/DVD" : (si->DeviceType == 0 ? "disk" : "other"));
    Line("Transport:      %s, LUN %u, SCSI target %u", ProtoName(si->Protocol), si->Lun, s);
    if (si->BlockSize != 0) {
        SizeText(a, Add64(Mul(si->LastLba, si->BlockSize), si->BlockSize));
        Line("Capacity:       %s (%lu blocks of %lu bytes)", a, si->LastLba + 1, si->BlockSize);
    } else {
        Line("Capacity:       unknown (no medium?)");
    }
    Line("Command size:   up to %lu KB", si->MaxTransfer / 1024);
    Line("Write protect:  %s", si->WriteProtect ? "yes" : "no");
    letter = SlotLetter(s);
    if (letter != 0) {
        char dummy[160];
        int nl = 0;
        const char *more = SlotMoreLetters(s, &nl);
        VolumeText(letter, dummy, TRUE, SlotFs(s));
        if (nl > 1) {
            Line("Drive letters:  %s (one per partition)", more);
        }
    } else {
        Line("Drive letter:   none");
    }
    {
        int i, n = SlotParts(s);
        if (n == 0) {
            Line("Partitions:     none found");
        } else if (n > 0) {
            Line("Partitions:     %d (%s)", n, Parts[s][0].Scheme);
            for (i = 0; i < n; i++) {
                FS_PART *p = &Parts[s][i];
                char sz[32];
                SizeText(sz, Mul((ULONG)p->Length, si->BlockSize ? si->BlockSize : 512));
                if (p->Length > 0xFFFFFFFFUL) lstrcpyA(sz, "> 2 TB");
                Line("  %d: %-30s %s%s%s%s%s", i + 1, p->Fs, sz, p->Label[0] ? ", label " : "", p->Label,
                     p->PartName[0] ? ", " : "", p->PartName);
                if (p->MbrType && p->MbrType != 0xEE)
                    Line("     MBR type %02X, starts at block %lu", p->MbrType, (ULONG)p->Start);
            }
        }
    }
}

static void ShowDetails(void)
{
    LRESULT sel = SendMessageA(ListWnd, LB_GETCURSEL, 0, 0);
    DWORD item;
    BOOL eject = FALSE;

    Text[0] = 0;
    if (sel >= 0 && TreeValid) {
        item = (DWORD)SendMessageA(ListWnd, LB_GETITEMDATA, (WPARAM)sel, 0);
        switch (ITEM_KIND(item)) {
        case ITEM_HC:
            HcDetails(ITEM_NUM(item));
            break;
        case ITEM_DEV:
            DevDetails(ITEM_NUM(item));
            eject = (Tree.Dev[ITEM_NUM(item)].Functions & USBNT_FUNC_STORAGE) != 0;
            break;
        case ITEM_SLOT:
            SlotDetails(ITEM_NUM(item));
            eject = TRUE;
            break;
        case ITEM_PORT:
            Line("Port %u of controller %u: nothing connected", ITEM_NUM(item) & 0xFF, ITEM_NUM(item) >> 8);
            break;
        case ITEM_EXTRA:
            if (ITEM_NUM(item) < USBNT_MAX_EXTRA) {
                ExtraDetails(ITEM_NUM(item));
            }
            break;
        default:
            break;
        }
    }
    SetWindowTextA(InfoWnd, Text);
    EnableWindow(EjectBtn, eject);
}

static BOOL EjectSlot(DWORD slot)
{
    char letter;
    char msg[400];
    DWORD got = 0;

    letter = SlotLetter((int)slot);
    if ((GetVersion() & 0x80000000) == 0 && (GetVersion() & 0xFF) >= 5) {
        /* Windows 2000 / XP: the way of Safely Remove Hardware */
        char veto[300];
        int r = PnpEjectSlot(slot, veto, sizeof(veto));
        if (r == 0) {
            if (letter != 0) {
                wsprintfA(msg, "Windows cannot stop drive %c: now: %s.\nClose the files and windows on it and try again.",
                          letter, veto);
            } else {
                wsprintfA(msg, "Windows cannot stop the disk now: %s.", veto);
            }
            MessageBoxA(MainWnd, msg, "USB Devices", MB_OK | MB_ICONEXCLAMATION);
            return FALSE;
        }
        if (r == 1) {
            return TRUE;
        }
    }
    if (letter != 0 && slot < USBNT_MAX_SLOTS && Map[slot].Disk >= 0) {
        /* every partition of the disk */
        DEV_LOCKED locked;
        char busy = DevMapLockSlot(&Map[slot], FALSE, 10, &locked);
        if (busy != 0) {
            wsprintfA(msg, "Drive %c: is in use. Close the files and windows on it and try again.", busy);
            MessageBoxA(MainWnd, msg, "USB Devices", MB_OK | MB_ICONEXCLAMATION);
            return FALSE;
        }
        DeviceIoControl(Dev, IOCTL_USBNT_EJECT, &slot, sizeof(slot), NULL, 0, &got, NULL);
        DevMapUnlock(&locked);
    } else if (letter != 0) {
        char path[8];
        HANDLE vol;
        int i;
        wsprintfA(path, "\\\\.\\%c:", letter);
        vol = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                          OPEN_EXISTING, 0, NULL);
        if (vol != INVALID_HANDLE_VALUE) {
            for (i = 0; i < 10; i++) {
                FlushFileBuffers(vol);
                if (DeviceIoControl(vol, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL)) {
                    break;
                }
                Sleep(500);
            }
            if (i == 10) {
                CloseHandle(vol);
                wsprintfA(msg, "Drive %c: is in use. Close the files and windows on it and try again.", letter);
                MessageBoxA(MainWnd, msg, "USB Devices", MB_OK | MB_ICONEXCLAMATION);
                return FALSE;
            }
            DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
            DeviceIoControl(Dev, IOCTL_USBNT_EJECT, &slot, sizeof(slot), NULL, 0, &got, NULL);
            CloseHandle(vol);
        }
    } else {
        DeviceIoControl(Dev, IOCTL_USBNT_EJECT, &slot, sizeof(slot), NULL, 0, &got, NULL);
    }
    return TRUE;
}

/* a LUN line ejects that LUN, a storage device line all of its LUNs */
static void Eject(void)
{
    LRESULT sel = SendMessageA(ListWnd, LB_GETCURSEL, 0, 0);
    DWORD item;
    DWORD s;
    BOOL ok = TRUE;

    if (sel < 0 || !TreeValid) {
        return;
    }
    item = (DWORD)SendMessageA(ListWnd, LB_GETITEMDATA, (WPARAM)sel, 0);
    if (ITEM_KIND(item) == ITEM_SLOT) {
        ok = EjectSlot((DWORD)ITEM_NUM(item));
    } else if (ITEM_KIND(item) == ITEM_DEV) {
        for (s = 0; s < Tree.SlotCount && s < USBNT_MAX_SLOTS && ok; s++) {
            if (Tree.Slot[s].Present && Tree.Slot[s].DevIndex == ITEM_NUM(item)) {
                ok = EjectSlot(s);
            }
        }
    } else {
        return;
    }
    FillList();
    ShowDetails();
    if (ok) {
        MessageBoxA(MainWnd, "The device can now be removed safely.", "USB Devices", MB_OK | MB_ICONINFORMATION);
    }
}

/* ------------------------------------------------------------------ */
/* Window                                                               */
/* ------------------------------------------------------------------ */

static DWORD WINAPI ChangeThread(LPVOID Param)
{
    HANDLE h = CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    ULONG count = 0xFFFFFFFF;

    (void)Param;
    if (h == INVALID_HANDLE_VALUE) {
        return 1;
    }
    for (;;) {
        ULONG now = count;
        DWORD got = 0;
        if (!DeviceIoControl(h, IOCTL_USBNT_WAIT_CHANGE, &now, sizeof(now), &now, sizeof(now), &got, NULL)) {
            Sleep(2000);
            continue;
        }
        if (now != count) {
            count = now;
            /* give the drive letter helper a moment to act on the same change */
            Sleep(700);
            PostMessageA(MainWnd, WM_TREE_CHANGED, 0, 0);
        }
    }
}

static void Layout(int W, int H)
{
    int btnH = 26;
    int area = H - btnH - 12;

    if (W >= 900) {
        int listW = W * 3 / 5;
        MoveWindow(ListWnd, 4, 4, listW - 6, area, TRUE);
        MoveWindow(InfoWnd, listW + 2, 4, W - listW - 6, area, TRUE);
    } else {
        int listH = area * 11 / 20;
        MoveWindow(ListWnd, 4, 4, W - 8, listH, TRUE);
        MoveWindow(InfoWnd, 4, listH + 8, W - 8, area - listH - 4, TRUE);
    }
    MoveWindow(RefreshBtn, 4, H - btnH - 4, 90, btnH, TRUE);
    MoveWindow(EjectBtn, 100, H - btnH - 4, 130, btnH, TRUE);
    MoveWindow(ReportBtn, 236, H - btnH - 4, 130, btnH, TRUE);
}

/* ------------------------------------------------------------------ */
/* Pictures in the list                                                 */
/* ------------------------------------------------------------------ */

/* the 16x16 pictures of treeicons.bmp, in this order */
enum { G_HC, G_HUB, G_STICK, G_DISK, G_KBD, G_MOUSE, G_TABLET, G_CAM, G_AUDIO, G_MIDI, G_SD, G_READER, G_PORT,
       G_TOUCHPAD, G_I2C, G_PLUG, G_NVME, G_TBT };

static int Has(const char *S, const char *W)
{
    int i, k;
    for (i = 0; S[i]; i++) {
        for (k = 0; W[k] && S[i + k]; k++) {
            char a = S[i + k], b = W[k];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (!W[k]) return 1;
    }
    return 0;
}

static int ItemPicture(DWORD Item, const char *Text)
{
    int n = ITEM_NUM(Item);
    ULONG e;
    switch (ITEM_KIND(Item)) {
    case ITEM_HC:
        return G_HC;
    case ITEM_PORT:
        return G_PORT;
    case ITEM_SLOT:
        for (e = 0; e < Tree2.ExtraCount && e < USBNT_MAX_EXTRA; e++)
            if (Tree2.Extra[e].Slot == n && Tree2.Extra[e].Kind == USBNT_EXTRA_READER) return G_SD;
            else if (Tree2.Extra[e].Slot == n && Tree2.Extra[e].Kind == USBNT_EXTRA_NVME) return G_NVME;
        return Has(Text, "SD") || Has(Text, "card") ? G_SD : G_DISK;
    case ITEM_EXTRA:
        if (Tree2.Extra[n].Kind == USBNT_EXTRA_I2C_CTRL) return G_I2C;
        if (Tree2.Extra[n].Kind == USBNT_EXTRA_TBT) return Has(Tree2.Extra[n].Name, "host") ? G_TBT : G_PLUG;
        if (Has(Tree2.Extra[n].Name, "keyboard")) return G_KBD;
        if (Has(Tree2.Extra[n].Name, "screen") || Has(Tree2.Extra[n].Name, "pen")) return G_TABLET;
        return G_TOUCHPAD;
    case ITEM_DEV: {
        UCHAR f = Tree.Dev[n].Functions;
        if (f & USBNT_FUNC_HUB) return G_HUB;
        if (f & USBNT_FUNC_VIDEO) return G_CAM;
        if (f & USBNT_FUNC_STORAGE) return Has(Text, "card") || Has(Text, "CRW") ? G_READER : G_STICK;
        if (f & USBNT_FUNC_KEYBOARD) return G_KBD;
        if (f & USBNT_FUNC_TABLET) return G_TABLET;
        if (f & USBNT_FUNC_MOUSE) return G_MOUSE;
        if (f & USBNT_FUNC_AUDIO) return G_AUDIO;
        if (f & USBNT_FUNC_MIDI) return G_MIDI;
        return G_PLUG;
    }
    default:
        if (Has(Text, "SD card readers")) return G_READER;
        if (Has(Text, "HID over I2C")) return G_TOUCHPAD;
        return -1;
    }
}

static int ListLineHeight(void)
{
    HDC dc = GetDC(NULL);
    TEXTMETRICA tm;
    HGDIOBJ old = SelectObject(dc, GetStockObject(ANSI_FIXED_FONT));
    int h;
    GetTextMetricsA(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    h = tm.tmHeight + 2;
    return h < 18 ? 18 : h;
}

static void DrawListItem(DRAWITEMSTRUCT *Di)
{
    char text[512];
    int len, sp = 0, x, pic;
    HDC dc = Di->hDC;
    RECT rc = Di->rcItem;
    BOOL sel = (Di->itemState & ODS_SELECTED) != 0;
    TEXTMETRICA tm;
    HGDIOBJ oldFont;

    if (Di->itemID == (UINT)-1) {
        if (Di->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
        return;
    }
    len = (int)SendMessageA(ListWnd, LB_GETTEXT, Di->itemID, (LPARAM)text);
    if (len < 0) len = 0;
    text[len] = 0;
    oldFont = SelectObject(dc, Font ? (HGDIOBJ)Font : GetStockObject(ANSI_FIXED_FONT));
    GetTextMetricsA(dc, &tm);
    {
        HBRUSH br = CreateSolidBrush(GetSysColor(sel ? COLOR_HIGHLIGHT : COLOR_WINDOW));
        FillRect(dc, &rc, br);
        DeleteObject(br);
    }
    while (text[sp] == ' ') sp++;
    x = rc.left + 2 + sp * tm.tmAveCharWidth;
    pic = ItemPicture((DWORD)Di->itemData, text);
    if (pic >= 0 && IconBmp && MaskBmp) {
        HDC mem = CreateCompatibleDC(dc);
        int y = rc.top + (rc.bottom - rc.top - 16) / 2;
        HGDIOBJ old = SelectObject(mem, MaskBmp);
        COLORREF ob = SetBkColor(dc, RGB(255, 255, 255)), ot = SetTextColor(dc, RGB(0, 0, 0));
        BitBlt(dc, x, y, 16, 16, mem, pic * 16, 0, SRCAND);
        SetBkColor(dc, ob);
        SetTextColor(dc, ot);
        SelectObject(mem, IconBmp);
        BitBlt(dc, x, y, 16, 16, mem, pic * 16, 0, SRCPAINT);
        SelectObject(mem, old);
        DeleteDC(mem);
        x += 20;
    } else if (sp == 0) {
        x += 0;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
    TextOutA(dc, x, rc.top + (rc.bottom - rc.top - tm.tmHeight) / 2, text + sp, len - sp);
    SelectObject(dc, oldFont);
    if (Di->itemState & ODS_FOCUS) DrawFocusRect(dc, &rc);
}

static BOOL WriteReport(const char *path);

/* "Save report...": the same file as usbtree /dump, for a bug report */
static void SaveReport(void)
{
    OPENFILENAMEA ofn;
    char name[MAX_PATH], msg[MAX_PATH + 80];
    lstrcpyA(name, "USBNT.TXT");
    {
        char *z = (char *)&ofn;
        unsigned k;
        for (k = 0; k < sizeof(ofn); k++) z[k] = 0;
    }
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = MainWnd;
    ofn.lpstrFilter = "Text files (*.TXT)\0*.TXT\0All files\0*.*\0";
    ofn.lpstrFile = name;
    ofn.nMaxFile = sizeof(name);
    ofn.lpstrTitle = "Save the USB report (devices, details and the driver log)";
    ofn.lpstrDefExt = "TXT";
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&ofn)) return;
    if (WriteReport(name)) {
        wsprintfA(msg, "Saved to %s.", name);
        MessageBoxA(MainWnd, msg, "USB Devices", MB_OK | MB_ICONINFORMATION);
    } else {
        wsprintfA(msg, "Cannot write %s.", name);
        MessageBoxA(MainWnd, msg, "USB Devices", MB_OK | MB_ICONSTOP);
    }
}

static LRESULT CALLBACK WndProc(HWND Wnd, UINT Msg, WPARAM W, LPARAM L)
{
    switch (Msg) {
    case WM_CREATE:
        IconBmp = LoadBitmapA(Inst, MAKEINTRESOURCEA(100));
        MaskBmp = LoadBitmapA(Inst, MAKEINTRESOURCEA(101));
        ListWnd = CreateWindowA("LISTBOX", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | WS_HSCROLL |
                                LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, 0, 0, 10, 10, Wnd, (HMENU)ID_LIST, Inst, NULL);
        InfoWnd = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE |
                                ES_READONLY | ES_AUTOVSCROLL, 0, 0, 10, 10, Wnd, (HMENU)ID_INFO, Inst, NULL);
        RefreshBtn = CreateWindowA("BUTTON", "&Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, Wnd,
                                   (HMENU)ID_REFRESH, Inst, NULL);
        EjectBtn = CreateWindowA("BUTTON", "&Safe removal", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, Wnd,
                                 (HMENU)ID_EJECT, Inst, NULL);
        ReportBtn = CreateWindowA("BUTTON", "Save re&port...", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10,
                                  Wnd, (HMENU)ID_REPORT, Inst, NULL);
        {
            /* loaded at run time so the tools build needs no gdi32.lib */
            typedef HGDIOBJ (WINAPI *GET_STOCK)(int);
            GET_STOCK getStock = (GET_STOCK)GetProcAddress(LoadLibraryA("GDI32.DLL"), "GetStockObject");
            Font = getStock ? (HFONT)getStock(ANSI_FIXED_FONT) : NULL;
        }
        SendMessageA(ListWnd, WM_SETFONT, (WPARAM)Font, 0);
        SendMessageA(InfoWnd, WM_SETFONT, (WPARAM)Font, 0);
        SendMessageA(ListWnd, LB_SETHORIZONTALEXTENT, 1600, 0);
        return 0;
    case WM_MEASUREITEM: {
        MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)L;
        mi->itemHeight = (UINT)ListLineHeight();
        return TRUE;
    }
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)L)->CtlID == ID_LIST) {
            DrawListItem((DRAWITEMSTRUCT *)L);
            return TRUE;
        }
        return DefWindowProcA(Wnd, Msg, W, L);
    case WM_SIZE:
        Layout(LOWORD(L), HIWORD(L));
        return 0;
    case WM_COMMAND:
        switch (LOWORD(W)) {
        case ID_LIST:
            if (HIWORD(W) == LBN_SELCHANGE) {
                ShowDetails();
            }
            break;
        case ID_REFRESH:
            MapDone = FALSE;
            FillList();
            ShowDetails();
            break;
        case ID_EJECT:
            Eject();
            break;
        case ID_REPORT:
            SaveReport();
            break;
        default:
            break;
        }
        return 0;
    case WM_TREE_CHANGED:
        FillList();
        ShowDetails();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(Wnd, Msg, W, L);
    }
}

/* usbtree /dump FILE: writes the list and the details of every line to FILE
   and exits (tests, bug reports) */
static BOOL WriteReport(const char *path);

static void DumpIfAsked(void)
{
    const char *cl = GetCommandLineA();
    const char *p = cl;
    char path[MAX_PATH];
    int n;

    while (*p && !(p[0] == '/' && (p[1] == 'd' || p[1] == 'D') && (p[2] == 'u' || p[2] == 'U'))) {
        p++;
    }
    if (*p == 0) {
        return;
    }
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    for (n = 0; *p && *p != ' ' && n < MAX_PATH - 1; n++) {
        path[n] = *p++;
    }
    path[n] = 0;
    ExitProcess(WriteReport(path) ? 0 : 2);
}

/* everything usbtree shows, every detail and the driver's log, into a text file */
static BOOL WriteReport(const char *path)
{
    HANDLE f;
    int i, count;
    DWORD w;
    char line[300];
    LRESULT sel = SendMessageA(ListWnd, LB_GETCURSEL, 0, 0);

    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    count = (int)SendMessageA(ListWnd, LB_GETCOUNT, 0, 0);
    for (i = 0; i < count; i++) {
        line[0] = 0;
        SendMessageA(ListWnd, LB_GETTEXT, (WPARAM)i, (LPARAM)line);
        lstrcatA(line, "\r\n");
        WriteFile(f, line, (DWORD)lstrlenA(line), &w, NULL);
    }
    for (i = 0; i < count; i++) {
        SendMessageA(ListWnd, LB_SETCURSEL, (WPARAM)i, 0);
        ShowDetails();
        if (Text[0]) {
            wsprintfA(line, "--- line %d\r\n", i);
            WriteFile(f, line, (DWORD)lstrlenA(line), &w, NULL);
            WriteFile(f, Text, (DWORD)lstrlenA(Text), &w, NULL);
        }
    }
    {
        /* the driver's log (the latest 32 KB) */
        static char log[32768 + 1];
        DWORD n = 0;
        if (DeviceIoControl(Dev, IOCTL_USBNT_GET_LOG, NULL, 0, log, sizeof(log) - 1, &n, NULL) && n != 0) {
            static const char head[] = "\r\n==== usbnt.sys log ====\r\n";
            DWORD k, a = 0;
            WriteFile(f, head, sizeof(head) - 1, &w, NULL);
            for (k = 0; k < n; k++) {
                if (log[k] == '\n') {                       /* CR LF for Notepad */
                    WriteFile(f, log + a, k - a, &w, NULL);
                    WriteFile(f, "\r\n", 2, &w, NULL);
                    a = k + 1;
                }
            }
            if (a < n) WriteFile(f, log + a, n - a, &w, NULL);
        }
    }
    CloseHandle(f);
    SendMessageA(ListWnd, LB_SETCURSEL, (WPARAM)sel, 0);
    ShowDetails();
    return TRUE;
}

void __stdcall Entry(void)
{
    WNDCLASSA wc;
    MSG msg;
    DWORD tid;

    Inst = GetModuleHandleA(NULL);
    SetErrorMode(SEM_FAILCRITICALERRORS);
    Dev = CreateFileA(USBNT_WIN32_NAME, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, 0, NULL);
    if (Dev == INVALID_HANDLE_VALUE) {
        MessageBoxA(NULL, "The USB driver (usbnt.sys) is not running.", "USB Devices", MB_OK | MB_ICONSTOP);
        ExitProcess(1);
    }
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = Inst;
    wc.hIcon = LoadIconA(Inst, MAKEINTRESOURCEA(1));
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = "UsbNtTree";
    RegisterClassA(&wc);
    {
        int w = GetSystemMetrics(SM_CXSCREEN);
        int h = GetSystemMetrics(SM_CYSCREEN);
        w = (w > 1000) ? 960 : w * 15 / 16;
        h = (h > 700) ? 640 : h * 3 / 4;
        MainWnd = CreateWindowA("UsbNtTree", "USB Devices", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                NULL, NULL, Inst, NULL);
    }
    FillList();
    ShowDetails();
    DumpIfAsked();
    ShowWindow(MainWnd, SW_SHOWNORMAL);
    UpdateWindow(MainWnd);
    CreateThread(NULL, 0, ChangeThread, NULL, 0, &tid);
    while (GetMessageA(&msg, NULL, 0, 0)) {
        /* Alt+R, Alt+S and F5 work wherever the focus is */
        if (msg.message == WM_SYSCHAR && (msg.wParam == 'r' || msg.wParam == 'R')) {
            SendMessageA(MainWnd, WM_COMMAND, ID_REFRESH, 0);
            continue;
        }
        if (msg.message == WM_SYSCHAR && (msg.wParam == 's' || msg.wParam == 'S')) {
            if (IsWindowEnabled(EjectBtn)) {
                SendMessageA(MainWnd, WM_COMMAND, ID_EJECT, 0);
            }
            continue;
        }
        if (msg.message == WM_SYSCHAR && (msg.wParam == 'p' || msg.wParam == 'P')) {
            SendMessageA(MainWnd, WM_COMMAND, ID_REPORT, 0);
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_F5) {
            SendMessageA(MainWnd, WM_COMMAND, ID_REFRESH, 0);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    ExitProcess(0);
}
