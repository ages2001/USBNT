/*
 * acpi.c - finds the ACPI tables of the firmware (RSDP in the EBDA or the
 * BIOS area, RSDT / XSDT, FADT -> DSDT, SSDTs). Windows NT 3.x / 4.0 have no
 * ACPI support of their own; the HID over I2C code reads the definition
 * blocks to learn where touch pads and touch screens are.
 */

#include "usbnt.h"
#include "acpi.h"

#define ACPI_MAX_TABLES 48

static ACPI_TABLE Tables[ACPI_MAX_TABLES];
static int NTables;
static BOOLEAN Done;

static BOOLEAN SumOk(const UCHAR *p, ULONG n)
{
    UCHAR s = 0;
    ULONG i;

    for (i = 0; i < n; i++) {
        s = (UCHAR)(s + p[i]);
    }
    return (BOOLEAN)(s == 0);
}

static ULONG Rd32(const UCHAR *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);
}

/* maps one table; NULL when the header or the checksum is wrong */
static const UCHAR *MapTable(ULONG Phys, ULONG *Len)
{
    UCHAR *h;
    ULONG len;
    UCHAR *t;

    if (Phys == 0 || Phys >= 0xFFFFF000UL) {
        return NULL;
    }
    h = (UCHAR *)OsMapMmio(0, Phys, 36);
    if (h == NULL) {
        return NULL;
    }
    len = Rd32(h + 4);
    OsUnmapMmio(h, 36);
    if (len < 36 || len > 4UL * 1024 * 1024) {
        return NULL;
    }
    t = (UCHAR *)OsMapMmio(0, Phys, len);
    if (t == NULL) {
        return NULL;
    }
    if (!SumOk(t, len)) {
        UsbLog(LOG_DBG, "acpi: table %c%c%c%c at %08x has a bad checksum\n", t[0], t[1], t[2], t[3], Phys);
        OsUnmapMmio(t, len);
        return NULL;
    }
    *Len = len;
    return t;
}

static void Add(ULONG Phys)
{
    const UCHAR *t;
    ULONG len;
    int i;

    if (NTables >= ACPI_MAX_TABLES) {
        return;
    }
    for (i = 0; i < NTables; i++) {
        if (Tables[i].Phys == Phys) {
            return;
        }
    }
    t = MapTable(Phys, &len);
    if (t == NULL) {
        return;
    }
    Tables[NTables].Sig = Rd32(t);
    Tables[NTables].Va = t;
    Tables[NTables].Len = len;
    Tables[NTables].Phys = Phys;
    NTables++;
    /* the FADT points at the DSDT */
    if (Rd32(t) == ACPI_SIG('F', 'A', 'C', 'P')) {
        ULONG dsdt = (len >= 44) ? Rd32(t + 40) : 0;
        if (len >= 148 && Rd32(t + 144) == 0 && Rd32(t + 140) != 0) {
            dsdt = Rd32(t + 140);
        }
        Add(dsdt);
    }
}

static ULONG FindRsdp(ULONG Base, ULONG Len)
{
    UCHAR *p = (UCHAR *)OsMapMmio(0, Base, Len);
    ULONG o, found = 0;

    if (p == NULL) {
        return 0;
    }
    for (o = 0; o + 20 <= Len; o += 16) {
        if (p[o] == 'R' && p[o + 1] == 'S' && p[o + 2] == 'D' && p[o + 3] == ' ' && p[o + 4] == 'P' &&
            p[o + 5] == 'T' && p[o + 6] == 'R' && p[o + 7] == ' ' && SumOk(p + o, 20)) {
            found = Base + o;
            break;
        }
    }
    OsUnmapMmio(p, Len);
    return found;
}

int AcpiInit(void)
{
    ULONG rsdp = 0;
    UCHAR *r;
    UCHAR *bda;
    ULONG root = 0;
    int entry = 4;
    const UCHAR *t;
    ULONG len, i;

    if (Done) {
        return NTables;
    }
    Done = TRUE;
    bda = (UCHAR *)OsMapMmio(0, 0x400, 0x100);
    if (bda != NULL) {
        ULONG ebda = ((ULONG)bda[0x0E] | ((ULONG)bda[0x0F] << 8)) << 4;
        OsUnmapMmio(bda, 0x100);
        if (ebda >= 0x80000 && ebda < 0xA0000) {
            rsdp = FindRsdp(ebda, 1024);
        }
    }
    if (rsdp == 0) {
        rsdp = FindRsdp(0xE0000, 0x20000);
    }
    if (rsdp == 0) {
        UsbLog(LOG_DBG, "acpi: no RSDP\n");
        return 0;
    }
    r = (UCHAR *)OsMapMmio(0, rsdp, 36);
    if (r == NULL) {
        return 0;
    }
    if (r[15] >= 2 && Rd32(r + 28) == 0 && Rd32(r + 24) != 0) {
        root = Rd32(r + 24);
        entry = 8;
    }
    t = root ? MapTable(root, &len) : NULL;
    if (t == NULL) {
        root = Rd32(r + 16);
        entry = 4;
        t = MapTable(root, &len);
    }
    OsUnmapMmio(r, 36);
    if (t == NULL) {
        return 0;
    }
    for (i = 36; i + entry <= len; i += entry) {
        if (entry == 8 && Rd32(t + i + 4) != 0) {
            continue;
        }
        Add(Rd32(t + i));
    }
    UsbLog(LOG_INFO, "acpi: %d tables (%s at %08x)\n", NTables, entry == 8 ? "XSDT" : "RSDT", root);
    return NTables;
}

const ACPI_TABLE *AcpiTable(int Index)
{
    if (Index < 0 || Index >= NTables) {
        return NULL;
    }
    return &Tables[Index];
}
