/*
 * hidparse.c - small HID report descriptor parser.
 *
 * HidParseMouse finds the buttons, X, Y, wheel and horizontal pan fields of
 * a mouse or pointer report; when the device has no mouse collection it
 * falls back to a touch screen / pen (absolute, tip switch = left button)
 * or a touch pad (absolute finger position turned into relative motion).
 * HidParseKeyboard finds the modifier bits, the key array or bitmap and the
 * LED output report of a keyboard; HidKbdToBoot turns any such report into
 * the 8 byte boot protocol report the keyboard code works with.
 */

#include "usbinput.h"

#define MAX_USAGES      32
#define MAX_STACK       4
#define MAX_IDS         16

typedef struct _HP_GLOBAL {
    ULONG   UsagePage;
    LONG    LogMin;
    LONG    LogMax;
    ULONG   LogMaxRaw;
    UCHAR   LogMaxSize;
    UCHAR   ReportSize;
    UCHAR   ReportId;
    UCHAR   Pad;
    ULONG   ReportCount;
} HP_GLOBAL;

/* one main item (input, output or feature) as seen by the walker callback */
typedef struct _HP_ITEM {
    ULONG           AppUsage;       /* top level application collection */
    ULONG           CollUsage;      /* innermost collection */
    UCHAR           Main;           /* 8 input, 9 output, 0xB feature */
    UCHAR           LogicalNo;      /* logical collections seen so far in the application (fingers) */
    ULONG           Flags;
    const HP_GLOBAL *G;
    ULONG           BitOff;
    ULONG           Count;
    ULONG           Size;
    const ULONG    *Usages;
    ULONG           NUsages;
    ULONG           UMin;
    ULONG           UMax;
    int             HasRange;
} HP_ITEM;

typedef void (*HP_CALLBACK)(void *Ctx, const HP_ITEM *It);

typedef struct _HP_REPORT {
    UCHAR   Id;
    USHORT  Bits[3];                /* input, output, feature */
} HP_REPORT;

static LONG SignExtend(ULONG v, int size)
{
    if (size == 1) return (LONG)(CHAR)(UCHAR)v;
    if (size == 2) return (LONG)(SHORT)(USHORT)v;
    return (LONG)v;
}

static ULONG UsageAt(const HP_ITEM *It, ULONG k)
{
    if (It->HasRange) {
        ULONG u = It->UMin + k;
        return u > It->UMax ? It->UMax : u;
    }
    if (It->NUsages != 0) {
        return It->Usages[k < It->NUsages ? k : It->NUsages - 1];
    }
    return 0;
}

/* walks the descriptor; returns the report table so callers can get the
   total size of a report */
static int HidWalk(const UCHAR *Desc, ULONG Len, HP_CALLBACK Cb, void *Ctx, HP_REPORT *Reps, int *NReps,
                   UCHAR *UsesIds)
{
    HP_GLOBAL g;
    HP_GLOBAL stack[MAX_STACK];
    int sp = 0;
    ULONG usages[MAX_USAGES];
    ULONG nusages = 0;
    ULONG umin = 0, umax = 0;
    int hasMin = 0, hasMax = 0;
    ULONG appUsage = 0;
    ULONG collStack[8];
    UCHAR logical = 0;
    int depth = 0;
    int nids = 1;
    ULONG pos = 0;
    int i;

    UsbMemSet(&g, 0, sizeof(g));
    UsbMemSet(Reps, 0, sizeof(HP_REPORT) * MAX_IDS);
    *UsesIds = 0;
    collStack[0] = 0;

    while (pos < Len) {
        UCHAR prefix = Desc[pos];
        ULONG size, type, tag;
        ULONG data = 0;

        if (prefix == 0xFE) {
            if (pos + 2 >= Len) {
                break;
            }
            pos += 3 + Desc[pos + 1];
            continue;
        }
        size = prefix & 3;
        if (size == 3) {
            size = 4;
        }
        type = (prefix >> 2) & 3;
        tag = prefix >> 4;
        if (pos + 1 + size > Len) {
            break;
        }
        for (i = 0; i < (int)size; i++) {
            data |= (ULONG)Desc[pos + 1 + i] << (8 * i);
        }
        pos += 1 + size;

        if (type == 1) {
            switch (tag) {
            case 0x0: g.UsagePage = data; break;
            case 0x1: g.LogMin = SignExtend(data, (int)size); break;
            case 0x2: g.LogMaxRaw = data; g.LogMaxSize = (UCHAR)size; g.LogMax = SignExtend(data, (int)size); break;
            case 0x7: g.ReportSize = (UCHAR)data; break;
            case 0x8: g.ReportId = (UCHAR)data; *UsesIds = 1; break;
            case 0x9: g.ReportCount = data; break;
            case 0xA: if (sp < MAX_STACK) stack[sp++] = g; break;
            case 0xB: if (sp > 0) g = stack[--sp]; break;
            default: break;
            }
            if (g.LogMin >= 0 && g.LogMaxSize != 0) {
                g.LogMax = (LONG)g.LogMaxRaw;
            }
            continue;
        }
        if (type == 2) {
            ULONG full = (size == 4) ? data : ((g.UsagePage << 16) | data);
            switch (tag) {
            case 0x0: if (nusages < MAX_USAGES) usages[nusages++] = full; break;
            case 0x1: umin = full; hasMin = 1; break;
            case 0x2: umax = full; hasMax = 1; break;
            default: break;
            }
            continue;
        }
        if (type != 0) {
            continue;
        }

        /* main item */
        if (tag == 0xA) {
            ULONG u = nusages ? usages[0] : (hasMin ? umin : 0);
            if (depth == 0) {
                appUsage = (data == 1) ? u : 0;
                logical = 0;
            } else if (data == 2) {
                logical++;
            }
            depth++;
            if (depth < 8) {
                collStack[depth] = u;
            }
        } else if (tag == 0xC) {
            if (depth > 0) {
                depth--;
            }
            if (depth == 0) {
                appUsage = 0;
            }
        } else if (tag == 0x8 || tag == 0x9 || tag == 0xB) {
            int slot;
            int kind = (tag == 0x8) ? 0 : (tag == 0x9) ? 1 : 2;
            HP_ITEM it;

            for (slot = 0; slot < nids; slot++) {
                if (Reps[slot].Id == g.ReportId) {
                    break;
                }
            }
            if (slot == nids) {
                if (nids == MAX_IDS) {
                    slot = MAX_IDS - 1;
                } else {
                    Reps[nids].Id = g.ReportId;
                    nids++;
                }
            }
            it.AppUsage = appUsage;
            it.CollUsage = collStack[depth < 8 ? depth : 7];
            it.Main = (UCHAR)tag;
            it.LogicalNo = logical;
            it.Flags = data;
            it.G = &g;
            it.BitOff = Reps[slot].Bits[kind];
            it.Count = g.ReportCount;
            it.Size = g.ReportSize;
            it.Usages = usages;
            it.NUsages = nusages;
            it.UMin = umin;
            it.UMax = umax;
            it.HasRange = hasMin && hasMax;
            if (Cb != NULL && g.ReportSize != 0 && g.ReportSize <= 32) {
                Cb(Ctx, &it);
            }
            Reps[slot].Bits[kind] = (USHORT)(Reps[slot].Bits[kind] + g.ReportCount * g.ReportSize);
        }
        nusages = 0;
        hasMin = 0;
        hasMax = 0;
    }
    *NReps = nids;
    return USB_OK;
}

static void SetField(HID_FIELD *f, const HP_ITEM *It, ULONG k, UCHAR Count)
{
    f->Valid = 1;
    f->ReportId = It->G->ReportId;
    f->Size = (UCHAR)It->Size;
    f->Count = Count;
    f->Offset = (USHORT)(It->BitOff + k * It->Size);
    f->Min = It->G->LogMin;
    f->Max = It->G->LogMax;
    f->Signed = (UCHAR)(It->G->LogMin < 0);
    f->Relative = (UCHAR)((It->Flags & 4) ? 1 : 0);
}

/* ---- mouse ---------------------------------------------------------- */

typedef struct _MOUSE_CTX {
    HID_MOUSE_LAYOUT Mouse;         /* mouse / pointer collection */
    HID_MOUSE_LAYOUT Digi;          /* touch screen, pen or touch pad */
    ULONG DigiApp;
} MOUSE_CTX;

static void MouseItem(void *Ctx, const HP_ITEM *It)
{
    MOUSE_CTX *c = (MOUSE_CTX *)Ctx;
    HID_MOUSE_LAYOUT *l;
    BOOLEAN digi = FALSE;
    ULONG k;

    if (It->Main != 8 || (It->Flags & 1) || !(It->Flags & 2)) {
        return;                                     /* input, variable, not constant */
    }
    if (It->AppUsage == 0x00010002 || It->AppUsage == 0x00010001) {
        l = &c->Mouse;
    } else if (It->AppUsage == 0x000D0004 || It->AppUsage == 0x000D0002 || It->AppUsage == 0x000D0001 ||
               It->AppUsage == 0x000D0005) {
        if (c->DigiApp != 0 && c->DigiApp != It->AppUsage) {
            return;
        }
        if (It->LogicalNo > 1) {
            return;                                 /* only the first finger */
        }
        c->DigiApp = It->AppUsage;
        l = &c->Digi;
        digi = TRUE;
    } else {
        return;
    }
    for (k = 0; k < It->Count; k++) {
        ULONG u = UsageAt(It, k);
        if ((u >> 16) == 0x09) {
            /* buttons: a mouse may report 1-3 in one field and 4, 5 in another */
            UCHAR n = (UCHAR)(It->Count - k > 8 ? 8 : It->Count - k);
            UCHAR first = (UCHAR)((u & 0xFFFF) == 0 || (u & 0xFFFF) > 8 ? 1 : (u & 0xFFFF));
            if (!l->Buttons.Valid) {
                SetField(&l->Buttons, It, k, n);
                l->ButtonsFirst = first;
            } else if (!l->Buttons2.Valid && It->G->ReportId == l->Buttons.ReportId &&
                       first > l->ButtonsFirst && first <= 5) {
                SetField(&l->Buttons2, It, k, n);
                l->Buttons2First = first;
            }
            break;
        }
        if (u == 0x00010030 && !l->X.Valid) {
            SetField(&l->X, It, k, 1);
        } else if (u == 0x00010031 && !l->Y.Valid) {
            SetField(&l->Y, It, k, 1);
        } else if (u == 0x00010038 && !l->Wheel.Valid) {
            SetField(&l->Wheel, It, k, 1);
        } else if (u == 0x000C0238 && !l->Pan.Valid) {
            SetField(&l->Pan, It, k, 1);
        } else if (digi && u == 0x000D0042 && !l->Tip.Valid) {
            SetField(&l->Tip, It, k, 1);
        } else if (digi && u == 0x000D0044 && !l->Barrel.Valid) {
            SetField(&l->Barrel, It, k, 1);
        }
    }
}

static int Finish(HID_MOUSE_LAYOUT *l, const HP_REPORT *Reps, int NReps, UCHAR UsesIds)
{
    int i;

    if (!l->X.Valid || !l->Y.Valid || l->X.ReportId != l->Y.ReportId) {
        return USB_ERR_PARAM;
    }
    l->UsesIds = UsesIds;
    l->ReportId = l->X.ReportId;
    if (l->Buttons.Valid && l->Buttons.ReportId != l->ReportId) l->Buttons.Valid = 0;
    if (l->Buttons2.Valid && l->Buttons2.ReportId != l->ReportId) l->Buttons2.Valid = 0;
    if (l->Wheel.Valid && l->Wheel.ReportId != l->ReportId) l->Wheel.Valid = 0;
    if (l->Pan.Valid && l->Pan.ReportId != l->ReportId) l->Pan.Valid = 0;
    if (l->Tip.Valid && l->Tip.ReportId != l->ReportId) l->Tip.Valid = 0;
    if (l->Barrel.Valid && l->Barrel.ReportId != l->ReportId) l->Barrel.Valid = 0;
    for (i = 0; i < NReps; i++) {
        if (Reps[i].Id == l->ReportId) {
            l->ReportBits = Reps[i].Bits[0];
        }
    }
    return USB_OK;
}

int HidParseMouse(const UCHAR *Desc, ULONG Len, HID_MOUSE_LAYOUT *Out)
{
    MOUSE_CTX c;
    HP_REPORT reps[MAX_IDS];
    int nreps;
    UCHAR ids;

    UsbMemSet(&c, 0, sizeof(c));
    UsbMemSet(Out, 0, sizeof(*Out));
    HidWalk(Desc, Len, MouseItem, &c, reps, &nreps, &ids);
    if (Finish(&c.Mouse, reps, nreps, ids) == USB_OK) {
        c.Mouse.Tip.Valid = 0;
        *Out = c.Mouse;
        return USB_OK;
    }
    if (c.DigiApp != 0 && Finish(&c.Digi, reps, nreps, ids) == USB_OK && !c.Digi.X.Relative) {
        *Out = c.Digi;
        Out->Kind = (c.DigiApp == 0x000D0005) ? HID_PTR_TOUCHPAD : HID_PTR_DIGITIZER;
        if (Out->Kind == HID_PTR_DIGITIZER) {
            /* the tip switch is the left button, the barrel switch the right one */
            Out->Buttons.Valid = 0;
            Out->Buttons2.Valid = 0;
        }
        return USB_OK;
    }
    return USB_ERR_PARAM;
}

/* ---- precision touch pad -------------------------------------------- */

static void PtpItem(void *Ctx, const HP_ITEM *It)
{
    HID_PTP_LAYOUT *l = (HID_PTP_LAYOUT *)Ctx;
    ULONG k;

    if (It->Main == 0xB && (It->Flags & 2) && !(It->Flags & 1)) {
        /* Input Mode (0x52) in the configuration collection */
        for (k = 0; k < It->Count; k++) {
            if (UsageAt(It, k) == 0x000D0052 && !l->ModeValid) {
                l->ModeValid = 1;
                l->ModeId = It->G->ReportId;
                l->ModeOff = (USHORT)(It->BitOff + k * It->Size);
                l->ModeSize = (UCHAR)It->Size;
            }
        }
        return;
    }
    if (It->Main != 8 || It->AppUsage != 0x000D0005 || (It->Flags & 1) || !(It->Flags & 2)) {
        return;
    }
    if (l->Valid && It->G->ReportId != l->ReportId) {
        return;                                     /* another report (diagnostics, ...) */
    }
    for (k = 0; k < It->Count; k++) {
        ULONG u = UsageAt(It, k);
        HID_FIELD *f = NULL;
        if (It->LogicalNo >= 1 && It->LogicalNo <= HID_PTP_FINGERS) {
            HID_PTP_FINGER *fg = &l->F[It->LogicalNo - 1];
            if (u == 0x000D0042) f = &fg->Tip;
            else if (u == 0x000D0047) f = &fg->Conf;
            else if (u == 0x000D0051) f = &fg->Id;
            else if (u == 0x00010030) f = &fg->X;
            else if (u == 0x00010031) f = &fg->Y;
            if (f != NULL && It->LogicalNo > l->NFingers) {
                l->NFingers = (UCHAR)It->LogicalNo;
            }
        }
        if (f == NULL && u == 0x000D0054) f = &l->Count;
        if (f == NULL && u == 0x00090001) f = &l->Button;
        if (f != NULL && !f->Valid) {
            SetField(f, It, k, 1);
            f->Relative = 0;
            if (!l->Valid) {
                l->Valid = 1;
                l->ReportId = It->G->ReportId;
            }
        }
    }
}

int HidParsePtp(const UCHAR *Desc, ULONG Len, HID_PTP_LAYOUT *Out)
{
    HP_REPORT reps[MAX_IDS];
    int nreps, i;
    UCHAR ids;

    UsbMemSet(Out, 0, sizeof(*Out));
    HidWalk(Desc, Len, PtpItem, Out, reps, &nreps, &ids);
    Out->UsesIds = ids;
    if (!Out->Valid || Out->NFingers == 0 || !Out->F[0].X.Valid || !Out->F[0].Y.Valid || !Out->F[0].Tip.Valid) {
        Out->Valid = 0;
        return USB_ERR_PARAM;
    }
    for (i = 0; i < nreps; i++) {
        if (Out->ModeValid && reps[i].Id == Out->ModeId) {
            Out->ModeBits = reps[i].Bits[2];
        }
    }
    return USB_OK;
}

/* ---- keyboard ------------------------------------------------------- */

static void KbdItem(void *Ctx, const HP_ITEM *It)
{
    HID_KBD_LAYOUT *l = (HID_KBD_LAYOUT *)Ctx;
    ULONG k;

    if (It->AppUsage != 0x00010006 || (It->Flags & 1)) {
        return;
    }
    if (It->Main == 9) {
        /* LEDs: num, caps, scroll lock */
        for (k = 0; k < It->Count; k++) {
            ULONG u = UsageAt(It, k);
            if (u >= 0x00080001 && u <= 0x00080003 && It->Size == 1) {
                if (!l->LedValid) {
                    l->LedValid = 1;
                    l->LedId = It->G->ReportId;
                }
                if (It->G->ReportId == l->LedId) {
                    l->LedOff[u - 0x00080001] = (USHORT)(It->BitOff + k + 1);
                }
            }
        }
        return;
    }
    if (It->Main != 8) {
        return;
    }
    if (l->Found && It->G->ReportId != l->ReportId) {
        return;
    }
    if (It->Flags & 2) {
        /* variable: modifier bits or a key bitmap */
        for (k = 0; k < It->Count; k++) {
            ULONG u = UsageAt(It, k);
            if (It->Size != 1 || (u >> 16) != 0x07) {
                continue;
            }
            u &= 0xFFFF;
            if (u >= 0xE0 && u <= 0xE7) {
                l->ModOff[u - 0xE0] = (USHORT)(It->BitOff + k + 1);
            } else if (u < 0xE0 && !l->Bmp) {
                l->Bmp = 1;
                l->BmpOff = (USHORT)(It->BitOff + k);
                l->BmpMin = (UCHAR)u;
                l->BmpCount = (UCHAR)((It->Count - k) > 0xE0 - u ? 0xE0 - u : It->Count - k);
                break;
            } else {
                continue;
            }
            l->Found = 1;
            l->ReportId = It->G->ReportId;
        }
        if (l->Bmp && !l->Found) {
            l->Found = 1;
            l->ReportId = It->G->ReportId;
        }
        return;
    }
    /* array of key codes */
    if (!l->Arr && It->Size >= 8 && It->Size <= 16 &&
        (It->HasRange ? (It->UMin >> 16) == 0x07 : (It->NUsages && (It->Usages[0] >> 16) == 0x07) ||
         (It->NUsages == 0 && It->G->UsagePage == 0x07))) {
        l->Arr = 1;
        l->ArrOff = (USHORT)It->BitOff;
        l->ArrSize = (UCHAR)It->Size;
        l->ArrCount = (UCHAR)(It->Count > 16 ? 16 : It->Count);
        l->ArrMin = It->HasRange ? (UCHAR)(It->UMin & 0xFF) : 0;
        l->ArrLogMin = It->G->LogMin;
        l->Found = 1;
        l->ReportId = It->G->ReportId;
    }
}

int HidParseKeyboard(const UCHAR *Desc, ULONG Len, HID_KBD_LAYOUT *Out)
{
    HP_REPORT reps[MAX_IDS];
    int nreps, i;
    UCHAR ids;

    UsbMemSet(Out, 0, sizeof(*Out));
    HidWalk(Desc, Len, KbdItem, Out, reps, &nreps, &ids);
    if (!Out->Found || (!Out->Arr && !Out->Bmp)) {
        return USB_ERR_PARAM;
    }
    Out->UsesIds = ids;
    for (i = 0; i < nreps; i++) {
        if (reps[i].Id == Out->ReportId) {
            Out->ReportBits = reps[i].Bits[0];
        }
        if (Out->LedValid && reps[i].Id == Out->LedId) {
            Out->LedBits = reps[i].Bits[1];
        }
    }
    return USB_OK;
}

static ULONG GetBits(const UCHAR *Rep, ULONG Len, ULONG Bit, ULONG Size)
{
    ULONG v = 0, i;

    if (Bit + Size > Len * 8) {
        return 0;
    }
    for (i = 0; i < Size; i++) {
        ULONG b = Bit + i;
        if (Rep[b >> 3] & (1 << (b & 7))) {
            v |= 1UL << i;
        }
    }
    return v;
}

BOOLEAN HidKbdToBoot(const HID_KBD_LAYOUT *L, const UCHAR *Rep, ULONG Len, UCHAR *Boot)
{
    ULONG i, n = 2;

    if (L->UsesIds) {
        if (Len < 1 || Rep[0] != L->ReportId) {
            return FALSE;
        }
        Rep++;
        Len--;
    }
    UsbMemSet(Boot, 0, 8);
    for (i = 0; i < 8; i++) {
        if (L->ModOff[i] && GetBits(Rep, Len, L->ModOff[i] - 1, 1)) {
            Boot[0] |= (UCHAR)(1 << i);
        }
    }
    if (L->Arr) {
        for (i = 0; i < L->ArrCount; i++) {
            LONG v = (LONG)GetBits(Rep, Len, L->ArrOff + i * L->ArrSize, L->ArrSize);
            ULONG u = (ULONG)(v - L->ArrLogMin) + L->ArrMin;
            if (v == 0 && L->ArrLogMin == 0 && L->ArrMin == 0) {
                continue;
            }
            if (u == 0 || u >= 0xE0) {
                if (u >= 0xE0 && u <= 0xE7) {
                    Boot[0] |= (UCHAR)(1 << (u - 0xE0));
                }
                continue;
            }
            if (u == 1) {                           /* roll over: hand it on */
                UsbMemSet(Boot + 2, 1, 6);
                return TRUE;
            }
            if (n < 8) {
                Boot[n++] = (UCHAR)u;
            }
        }
    }
    if (L->Bmp) {
        for (i = 0; i < L->BmpCount; i++) {
            if (GetBits(Rep, Len, L->BmpOff + i, 1)) {
                ULONG u = L->BmpMin + i;
                if (u < 4) {
                    continue;
                }
                if (n < 8) {
                    Boot[n++] = (UCHAR)u;
                } else {
                    UsbMemSet(Boot + 2, 1, 6);
                    return TRUE;
                }
            }
        }
    }
    return TRUE;
}

/* builds the LED output report (report id byte first when the device uses
   ids); returns its length */
ULONG HidKbdLedReport(const HID_KBD_LAYOUT *L, UCHAR HidLeds, UCHAR *Out, ULONG Max)
{
    ULONG bytes, i, o = 0;

    if (!L->LedValid) {
        return 0;
    }
    bytes = (L->LedBits + 7) / 8;
    if (L->UsesIds) {
        o = 1;
    }
    if (bytes + o > Max || bytes == 0) {
        return 0;
    }
    UsbMemSet(Out, 0, bytes + o);
    if (o) {
        Out[0] = L->LedId;
    }
    for (i = 0; i < 3; i++) {
        if (L->LedOff[i] && (HidLeds & (1 << i))) {
            ULONG b = L->LedOff[i] - 1;
            Out[o + (b >> 3)] |= (UCHAR)(1 << (b & 7));
        }
    }
    return bytes + o;
}

LONG HidGetField(const UCHAR *Report, ULONG Len, const HID_FIELD *F, ULONG Index)
{
    ULONG bit = F->Offset + Index * F->Size;
    ULONG v;

    if (!F->Valid || F->Size == 0 || bit + F->Size > Len * 8) {
        return 0;
    }
    v = GetBits(Report, Len, bit, F->Size);
    if (F->Signed && F->Size < 32 && (v & (1UL << (F->Size - 1)))) {
        v |= ~0UL << F->Size;
    }
    return (LONG)v;
}
