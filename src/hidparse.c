/*
 * hidparse.c - minimal HID report descriptor parser. Finds the buttons,
 * X, Y, wheel and horizontal pan fields of a mouse or pointer report.
 */

#include "usbinput.h"

#define MAX_USAGES      16
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

static LONG SignExtend(ULONG v, int size)
{
    if (size == 1) return (LONG)(CHAR)(UCHAR)v;
    if (size == 2) return (LONG)(SHORT)(USHORT)v;
    return (LONG)v;
}

static void SetField(HID_FIELD *f, const HP_GLOBAL *g, ULONG Offset, ULONG Flags, UCHAR Count)
{
    f->Valid = 1;
    f->ReportId = g->ReportId;
    f->Size = g->ReportSize;
    f->Count = Count;
    f->Offset = (USHORT)Offset;
    f->Min = g->LogMin;
    f->Max = g->LogMax;
    f->Signed = (UCHAR)(g->LogMin < 0);
    f->Relative = (UCHAR)((Flags & 4) ? 1 : 0);
}

int HidParseMouse(const UCHAR *Desc, ULONG Len, HID_MOUSE_LAYOUT *Out)
{
    HP_GLOBAL g;
    HP_GLOBAL stack[MAX_STACK];
    int sp = 0;
    ULONG usages[MAX_USAGES];
    ULONG nusages = 0;
    ULONG umin = 0, umax = 0;
    int hasMin = 0, hasMax = 0;
    ULONG appUsage = 0;
    int depth = 0;
    UCHAR ids[MAX_IDS];
    USHORT bits[MAX_IDS];
    int nids = 1;
    ULONG pos = 0;
    int i;

    UsbMemSet(Out, 0, sizeof(*Out));
    UsbMemSet(&g, 0, sizeof(g));
    ids[0] = 0;
    bits[0] = 0;

    while (pos < Len) {
        UCHAR prefix = Desc[pos];
        ULONG size;
        ULONG type;
        ULONG tag;
        ULONG data = 0;
        int slot;

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
            case 0x8:
                g.ReportId = (UCHAR)data;
                Out->UsesIds = 1;
                break;
            case 0x9: g.ReportCount = data; break;
            case 0xA:
                if (sp < MAX_STACK) {
                    stack[sp++] = g;
                }
                break;
            case 0xB:
                if (sp > 0) {
                    g = stack[--sp];
                }
                break;
            default:
                break;
            }
            if (g.LogMin >= 0 && g.LogMaxSize != 0) {
                g.LogMax = (LONG)g.LogMaxRaw;
            }
            continue;
        }
        if (type == 2) {
            ULONG full = (size == 4) ? data : ((g.UsagePage << 16) | data);
            switch (tag) {
            case 0x0:
                if (nusages < MAX_USAGES) {
                    usages[nusages++] = full;
                }
                break;
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
            if (depth == 0 && data == 1) {
                appUsage = nusages ? usages[0] : (hasMin ? umin : 0);
            }
            depth++;
        } else if (tag == 0xC) {
            if (depth > 0) {
                depth--;
            }
            if (depth == 0) {
                appUsage = 0;
            }
        } else if (tag == 0x8) {
            ULONG count = g.ReportCount;
            ULONG fsize = g.ReportSize;
            ULONG off;
            BOOLEAN mouseApp = (BOOLEAN)(appUsage == 0x00010002 || appUsage == 0x00010001);

            for (slot = 0; slot < nids; slot++) {
                if (ids[slot] == g.ReportId) {
                    break;
                }
            }
            if (slot == nids) {
                if (nids == MAX_IDS) {
                    slot = MAX_IDS - 1;
                } else {
                    ids[nids] = g.ReportId;
                    bits[nids] = 0;
                    nids++;
                }
            }
            off = bits[slot];

            if (mouseApp && !(data & 1) && (data & 2) && fsize != 0 && fsize <= 32) {
                ULONG k;
                for (k = 0; k < count; k++) {
                    ULONG u;
                    if (hasMin && hasMax) {
                        u = umin + k;
                        if (u > umax) {
                            u = umax;
                        }
                    } else if (nusages != 0) {
                        u = usages[k < nusages ? k : nusages - 1];
                    } else {
                        u = 0;
                    }
                    if ((u >> 16) == 0x09) {
                        if (!Out->Buttons.Valid) {
                            SetField(&Out->Buttons, &g, off + k * fsize, data, (UCHAR)(count - k > 8 ? 8 : count - k));
                        }
                        break;
                    }
                    if (u == 0x00010030 && !Out->X.Valid) {
                        SetField(&Out->X, &g, off + k * fsize, data, 1);
                    } else if (u == 0x00010031 && !Out->Y.Valid) {
                        SetField(&Out->Y, &g, off + k * fsize, data, 1);
                    } else if (u == 0x00010038 && !Out->Wheel.Valid) {
                        SetField(&Out->Wheel, &g, off + k * fsize, data, 1);
                    } else if (u == 0x000C0238 && !Out->Pan.Valid) {
                        SetField(&Out->Pan, &g, off + k * fsize, data, 1);
                    }
                }
            }
            bits[slot] = (USHORT)(off + count * fsize);
        }
        nusages = 0;
        hasMin = 0;
        hasMax = 0;
    }

    if (!Out->X.Valid || !Out->Y.Valid || Out->X.ReportId != Out->Y.ReportId) {
        return USB_ERR_PARAM;
    }
    Out->ReportId = Out->X.ReportId;
    if (Out->Buttons.Valid && Out->Buttons.ReportId != Out->ReportId) Out->Buttons.Valid = 0;
    if (Out->Wheel.Valid && Out->Wheel.ReportId != Out->ReportId) Out->Wheel.Valid = 0;
    if (Out->Pan.Valid && Out->Pan.ReportId != Out->ReportId) Out->Pan.Valid = 0;
    for (i = 0; i < nids; i++) {
        if (ids[i] == Out->ReportId) {
            Out->ReportBits = bits[i];
        }
    }
    return USB_OK;
}

LONG HidGetField(const UCHAR *Report, ULONG Len, const HID_FIELD *F, ULONG Index)
{
    ULONG bit = F->Offset + Index * F->Size;
    ULONG v = 0;
    ULONG i;

    if (!F->Valid || F->Size == 0 || bit + F->Size > Len * 8) {
        return 0;
    }
    for (i = 0; i < F->Size; i++) {
        ULONG b = bit + i;
        if (Report[b >> 3] & (1 << (b & 7))) {
            v |= 1UL << i;
        }
    }
    if (F->Signed && F->Size < 32 && (v & (1UL << (F->Size - 1)))) {
        v |= ~0UL << F->Size;
    }
    return (LONG)v;
}
