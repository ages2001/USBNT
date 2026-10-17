/*
 * gpio.c - GPIO pads for i2chid.c (see gpio.h). Ported from AUDIONT.
 *
 * Sunrise Point and later ("pinctrl-intel"): the controller device (INT344B,
 * INT34BB, INT34C5, INTC1057, ...) has one memory resource per pad community;
 * when the firmware fills those in at run time the community lies at
 * SBREG_BAR + (port id << 16) with SBREG_BAR from the DSDT (SBRG) or the P2SB
 * device. ACPI pin numbers map to pads through the pad groups of each
 * community (tables generated from Linux drivers/pinctrl/intel). PADBAR gives
 * the pad registers; PADCFG0 bit 0 is the output level, bit 1 the input level,
 * bit 8 disables the output. Bay Trail (INT33FC) and Cherry Trail (INT33FF)
 * have their own layouts. AMD FCH (AMDI0030 / AMD0030 / AMDI0031, Linux
 * pinctrl-amd): one 32-bit register per pin at pin*4, output value bit 22,
 * output enable bit 23, input level bit 16. Unknown controllers with one
 * memory resource: pin = pad.
 */
#include "usbinput.h"
#include "acpi.h"
#include "aml.h"
#include "gpio.h"

#define MMIO32(a, o)  (*(volatile ULONG *)((UCHAR *)(a) + (o)))
#ifndef ARRAYSIZE
#define ARRAYSIZE(a)  (sizeof(a) / sizeof((a)[0]))
#endif

typedef struct { USHORT Base, Size; SHORT GpioBase; } GPP;           /* GpioBase -1: no ACPI numbers */
typedef struct { UCHAR Bar; CHAR Com; USHORT PinBase, NPins; UCHAR NGpp; const GPP *Gpp; } COMM;
typedef struct { const char *Hid; UCHAR Uid; UCHAR Fam; const COMM *C; } SOC;   /* Uid 255: any */

#include "gpiotab.h"

#define FAM_SPT   1     /* port id 0xAF - community */
#define FAM_CNL   2     /* 0x6E - community (Cannon Lake .. Alder Lake, Jasper Lake) */
#define FAM_EHL   3
#define FAM_MTL   4
#define FAM_APL   5
#define FAM_GLK   6

#define PADBAR          0x0C
#define PADCFG0_TX      0x001
#define PADCFG0_RX      0x002
#define PADCFG0_TXDIS   0x100
#define PADCFG0_RXDIS   0x200
#define PADCFG0_PMODE   0x3C00

typedef struct {
    char    Name[5];
    UCHAR   Kind;               /* 1 pinctrl-intel, 2 Bay Trail, 3 Cherry Trail, 4 linear, 5 AMD */
    const COMM *C;
    const UCHAR *BytMap;
    ULONG   BytLen;
    UCHAR  *Va[8];
    ULONG   Pa[8], PadOff[8], PadSize[8];
    BOOLEAN Ok;
} GPIO_CTRL;

#define MAX_GCTRL 8
static GPIO_CTRL g_Ctrl[MAX_GCTRL];
static ULONG g_NCtrl;

static ULONG SbregBase(UCHAR fam)
{
    ULONG dev = (fam == FAM_APL || fam == FAM_GLK) ? 0x0D : 0x1F, fn = (fam == FAM_APL || fam == FAM_GLK) ? 0 : 1;
    ULONG bar, hidden;
    ULONGLONG sbrg = 0;
    if (AmlGlobalInt("SBRG", &sbrg) && sbrg && !(sbrg & 0xFFFFFF)) {
        return (ULONG)sbrg;
    }
    hidden = (OsPciRead(0, (UCHAR)dev, (UCHAR)fn, 0, 2) == 0xFFFF);
    if (hidden) {
        OsPciWrite(0, (UCHAR)dev, (UCHAR)fn, 0xE1, 0, 1);
    }
    bar = OsPciRead(0, (UCHAR)dev, (UCHAR)fn, 0x10, 4) & ~0xFUL;
    if (OsPciRead(0, (UCHAR)dev, (UCHAR)fn, 0x14, 4)) {
        bar = 0;
    }
    if (hidden) {
        OsPciWrite(0, (UCHAR)dev, (UCHAR)fn, 0xE1, 1, 1);
    }
    return (bar && bar != 0xFFFFFFF0UL) ? bar : 0;
}

static ULONG PortId(UCHAR fam, int com, ULONG uid)
{
    static const UCHAR mtl[6] = { 0xD1, 0xD2, 0, 0xD3, 0xD4, 0xD5 };
    static const UCHAR apl[5] = { 0, 0xC5, 0xC4, 0xC7, 0xC0 };
    static const UCHAR glk[5] = { 0, 0xC4, 0xC5, 0xC9, 0xC8 };
    switch (fam) {
    case FAM_SPT: return com >= 0 && com <= 3 ? 0xAF - com : 0;
    case FAM_CNL: return com >= 0 && com <= 5 ? 0x6E - com : 0;
    case FAM_EHL: return uid <= 5 ? 0x6E - uid : 0;
    case FAM_MTL: return com >= 0 && com <= 5 ? mtl[com] : 0;
    case FAM_APL: return uid <= 4 ? apl[uid] : 0;
    case FAM_GLK: return uid <= 4 ? glk[uid] : 0;
    }
    return 0;
}

static BOOLEAN MapComm(GPIO_CTRL *g, ULONG i, ULONG pa)
{
    UCHAR *va;
    ULONG rev, pad;
    if (!pa) {
        return FALSE;
    }
    va = (UCHAR *)OsMapMmio(0, pa, 0x10000);
    if (!va) {
        return FALSE;
    }
    rev = MMIO32(va, 0);
    pad = MMIO32(va, PADBAR);
    if (rev == 0xFFFFFFFF || !pad || pad >= 0x10000) {
        UsbLog(LOG_INFO, "gpio: %s community %u at %08x not present (%08x %08x)\n", g->Name, i, pa, rev, pad);
        OsUnmapMmio(va, 0x10000);
        return FALSE;
    }
    g->Va[i] = va;
    g->Pa[i] = pa;
    g->PadOff[i] = pad;
    g->PadSize[i] = ((rev >> 16) >= 0x092) ? 16 : 8;
    return TRUE;
}

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}

static GPIO_CTRL *OpenCtrl(const char *name)
{
    GPIO_CTRL *g;
    AML_CRS crs;
    char hid[16];
    ULONG uid = 0, i, n;
    for (i = 0; i < g_NCtrl; i++) {
        if (!UsbMemCmp(g_Ctrl[i].Name, name, 4)) {
            return g_Ctrl[i].Ok ? &g_Ctrl[i] : NULL;
        }
    }
    if (g_NCtrl >= MAX_GCTRL) {
        return NULL;
    }
    g = &g_Ctrl[g_NCtrl++];
    UsbMemSet(g, 0, sizeof(*g));
    UsbMemCpy(g->Name, name, 4);
    if (!AmlGpioController(name, hid, sizeof(hid), &uid, &crs)) {
        UsbLog(LOG_ERR, "gpio: controller %s not in the ACPI tables\n", name);
        return NULL;
    }
    if (StrEq(hid, "INT33FC") || StrEq(hid, "INT33FF")) {        /* Bay Trail / Cherry Trail */
        ULONG len = crs.NMem && crs.MemLen[0] ? crs.MemLen[0] : 0x8000;
        if (!crs.NMem || !crs.MemBase[0]) {
            UsbLog(LOG_ERR, "gpio: %s (%s) has no memory resource\n", name, hid);
            return NULL;
        }
        g->Kind = (UCHAR)(hid[5] == 'F' && hid[6] == 'C' ? 2 : 3);
        if (g->Kind == 2) {
            if (uid == 1) { g->BytMap = g_BytScore; g->BytLen = sizeof(g_BytScore); }
            else if (uid == 2) { g->BytMap = g_BytNcore; g->BytLen = sizeof(g_BytNcore); }
            else { g->BytMap = g_BytSus; g->BytLen = sizeof(g_BytSus); }
        }
        g->Pa[0] = crs.MemBase[0];
        g->PadSize[0] = len > 0x10000 ? 0x10000 : len;
        g->Va[0] = (UCHAR *)OsMapMmio(0, g->Pa[0], g->PadSize[0]);
        g->Ok = g->Va[0] != NULL;
        UsbLog(LOG_INFO, "gpio: %s %s uid %u at %08x\n", name, hid, uid, g->Pa[0]);
        return g->Ok ? g : NULL;
    }
    if (StrEq(hid, "AMDI0030") || StrEq(hid, "AMD0030") || StrEq(hid, "AMDI0031")) {
        ULONG len = crs.NMem && crs.MemLen[0] ? crs.MemLen[0] : 0x400;
        if (!crs.NMem || !crs.MemBase[0]) {
            UsbLog(LOG_ERR, "gpio: %s (%s) has no memory resource\n", name, hid);
            return NULL;
        }
        g->Kind = 5;
        g->Pa[0] = crs.MemBase[0];
        g->PadSize[0] = len > 0x2000 ? 0x2000 : len;
        g->Va[0] = (UCHAR *)OsMapMmio(0, g->Pa[0], g->PadSize[0]);
        g->Ok = g->Va[0] != NULL;
        UsbLog(LOG_INFO, "gpio: %s %s (AMD) at %08x\n", name, hid, g->Pa[0]);
        return g->Ok ? g : NULL;
    }
    for (i = 0; i < ARRAYSIZE(g_Socs); i++) {
        if (StrEq(g_Socs[i].Hid, hid) && (g_Socs[i].Uid == 255 || g_Socs[i].Uid == uid)) {
            break;
        }
    }
    if (i < ARRAYSIZE(g_Socs)) {
        const SOC *soc = &g_Socs[i];
        ULONG sbreg = 0;
        g->Kind = 1;
        g->C = soc->C;
        for (n = 0; soc->C[n].Bar != 0xFF && n < 8; n++) {
            ULONG bar = soc->C[n].Bar, pa = bar < (ULONG)crs.NMem ? crs.MemBase[bar] : 0, pid;
            int com = soc->C[n].Com;
            if (soc->Fam == FAM_SPT && com < 0) {
                com = n == 2 ? 3 : (int)n;
            }
            if (!pa && (pid = PortId(soc->Fam, com, uid))) {
                if (!sbreg) {
                    sbreg = SbregBase(soc->Fam);
                }
                if (sbreg) {
                    pa = sbreg + (pid << 16);
                }
            }
            if (MapComm(g, n, pa)) {
                g->Ok = TRUE;
            }
        }
        UsbLog(LOG_INFO, "gpio: %s %s uid %u: %u communities%s\n", name, hid, uid, n,
               g->Ok ? "" : ", none reachable");
        return g->Ok ? g : NULL;
    }
    if (crs.NMem == 1 && crs.MemBase[0] && MapComm(g, 0, crs.MemBase[0])) {
        g->Kind = 4;
        g->Ok = TRUE;
        UsbLog(LOG_INFO, "gpio: %s %s: unknown controller, pins taken as pads\n", name, hid);
        return g;
    }
    UsbLog(LOG_ERR, "gpio: %s %s: controller not supported\n", name, hid);
    return NULL;
}

BOOLEAN GpioResolvePin(const char *Ctrl4, ULONG Pin, GPIO_PIN *P)
{
    GPIO_CTRL *g;
    ULONG n, k;
    UsbMemSet(P, 0, sizeof(*P));
    if (Ctrl4 == NULL || Ctrl4[0] == 0) {
        return FALSE;
    }
    UsbMemCpy(P->Ctrl, Ctrl4, 4);
    P->Pin = (USHORT)Pin;
    g = OpenCtrl(Ctrl4);
    if (!g) {
        return FALSE;
    }
    P->Kind = g->Kind;
    switch (g->Kind) {
    case 2:                                                   /* Bay Trail: 16 bytes a pad, VAL at +8 */
        if (Pin >= g->BytLen) {
            return FALSE;
        }
        P->Cfg = g->Va[0] + g->BytMap[Pin] * 16 + 8;
        break;
    case 3:                                                   /* Cherry Trail: families of 15 pads */
        if (0x4400 + (Pin / 15) * 0x400 + (Pin % 15) * 8 + 8 > g->PadSize[0]) {
            return FALSE;
        }
        P->Cfg = g->Va[0] + 0x4400 + (Pin / 15) * 0x400 + (Pin % 15) * 8;
        break;
    case 4:
        P->Cfg = g->Va[0] + g->PadOff[0] + Pin * g->PadSize[0];
        break;
    case 5:
        if (Pin * 4 + 4 > g->PadSize[0]) {
            return FALSE;
        }
        P->Cfg = g->Va[0] + Pin * 4;
        break;
    default:
        for (n = 0; g->C[n].Bar != 0xFF && n < 8; n++) {
            const COMM *c = &g->C[n];
            for (k = 0; k < c->NGpp; k++) {
                const GPP *gp = &c->Gpp[k];
                if (gp->GpioBase < 0 || Pin < (ULONG)gp->GpioBase || Pin >= (ULONG)gp->GpioBase + gp->Size) {
                    continue;
                }
                if (!g->Va[n]) {
                    return FALSE;
                }
                P->Pad = gp->Base + (Pin - gp->GpioBase) - c->PinBase;
                P->Cfg = g->Va[n] + g->PadOff[n] + P->Pad * g->PadSize[n];
                return TRUE;
            }
        }
        UsbLog(LOG_INFO, "gpio: %s pin %u not in any pad group\n", g->Name, Pin);
        return FALSE;
    }
    P->Pad = Pin;
    return TRUE;
}

void GpioSet(GPIO_PIN *P, BOOLEAN high)
{
    ULONG v;
    if (!P || !P->Cfg) {
        return;
    }
    v = MMIO32(P->Cfg, 0);
    switch (P->Kind) {
    case 2:                                                   /* VAL: bit 1 output disable (active low) */
        v = (v & ~2UL & ~1UL) | (high ? 1 : 0);
        break;
    case 3:                                                   /* PADCTRL0: GPIOEN 15, GPIOCFG 8-10 = GPO */
        v = (v & ~(0x700UL | 2UL)) | 0x8000UL | 0x100UL | (high ? 2 : 0);
        break;
    case 5:                                                   /* AMD: OUTPUT_VALUE 22, OUTPUT_ENABLE 23 */
        v = (v & ~(1UL << 22)) | (1UL << 23) | (high ? (1UL << 22) : 0);
        break;
    default:
        v = (v & ~(PADCFG0_PMODE | PADCFG0_TXDIS | PADCFG0_TX)) | (high ? PADCFG0_TX : 0);
        break;
    }
    MMIO32(P->Cfg, 0) = v;
    UsbLog(LOG_DBG, "gpio: %s pin %u -> %u (%08x)\n", P->Ctrl, P->Pin, high ? 1 : 0, MMIO32(P->Cfg, 0));
}

int GpioGet(GPIO_PIN *P)
{
    ULONG v;
    if (!P || !P->Cfg) {
        return -1;
    }
    v = MMIO32(P->Cfg, 0);
    switch (P->Kind) {
    case 2: return (int)(v & 1);
    case 3: return (int)(v & 1);
    case 5: return (v & (1UL << 16)) ? 1 : 0;                 /* PIN_STS */
    default: return (v & PADCFG0_RX) ? 1 : 0;
    }
}

void GpioCloseAll(void)
{
    ULONG i, n;
    for (i = 0; i < g_NCtrl; i++) {
        for (n = 0; n < 8; n++) {
            if (g_Ctrl[i].Va[n]) {
                OsUnmapMmio(g_Ctrl[i].Va[n], (g_Ctrl[i].Kind >= 2 && g_Ctrl[i].Kind <= 3) ||
                            g_Ctrl[i].Kind == 5 ? g_Ctrl[i].PadSize[0] : 0x10000);
            }
        }
    }
    UsbMemSet(g_Ctrl, 0, sizeof(g_Ctrl));
    g_NCtrl = 0;
}
