/*
 * usbhid.c - HID keyboard (boot protocol) and mouse / tablet (report
 * protocol with boot protocol fallback) driver.
 */

#include "usbinput.h"

#define HID_MAX         16
#define HID_KBD         1
#define HID_MOUSE       2

#define HID_REQ_SET_REPORT      0x09
#define HID_REQ_SET_IDLE        0x0A
#define HID_REQ_SET_PROTOCOL    0x0B

typedef struct _HID_DEV {
    USB_DEV            *Dev;
    UCHAR               Iface;
    UCHAR               Kind;
    UCHAR               BootMode;
    UCHAR               Active;
    UCHAR               NeedService;
    UCHAR               LedSent;
    UCHAR               Gone;
    UCHAR               PrevButtons;
    ULONG               Errors;
    USB_PIPE            In;
    USB_XFER            Xfer;
    UCHAR              *Buf;
    ULONG               BufPhys;
    ULONG               BufSize;
    UCHAR               Prev[8];
    HID_MOUSE_LAYOUT    Layout;
} HID_DEV;

static HID_DEV *HidDevs[HID_MAX];
static volatile ULONG LedWanted;
static ULONG TypeDelay = 500;
static ULONG TypeRate = 30;

/* typematic state, protected by the global lock */
static USHORT RepCode;
static USHORT RepFlags;
static UCHAR RepUsage;
static UCHAR RepActive;
static ULONG RepNext;

/* USB usage (page 7) to scan code set 1; 0x100 marks an E0 prefix */
static const USHORT KeyMap[0x74] = {
    0, 0, 0, 0,
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,     /* 04 a..l */
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,     /* 10 m..x */
    0x15, 0x2C,                                                                 /* 1c y z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                 /* 1e 1..0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27,     /* 28 */
    0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,                                         /* 34 */
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,     /* 3a F1..F12 */
    0x137, 0x46, 0x45,                                                          /* 46 prtsc scroll pause */
    0x152, 0x147, 0x149, 0x153, 0x14F, 0x151, 0x14D, 0x14B, 0x150, 0x148,       /* 49 ins..up */
    0x45, 0x135, 0x37, 0x4A, 0x4E, 0x11C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D,   /* 53 numlock.. */
    0x47, 0x48, 0x49, 0x52, 0x53,                                               /* 5f kp7..kp. */
    0x56, 0x15D, 0x15E, 0x59,                                                   /* 64 */
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x76      /* 68 F13..F24 */
};

static const USHORT ModMap[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };

static USHORT MapUsage(UCHAR u)
{
    if (u < 0x74) {
        return KeyMap[u];
    }
    switch (u) {
    case 0x85: return 0x7E;
    case 0x87: return 0x73;
    case 0x88: return 0x70;
    case 0x89: return 0x7D;
    case 0x8A: return 0x79;
    case 0x8B: return 0x7B;
    case 0x8C: return 0x5C;
    default:   return 0;
    }
}

static void AddKey(USB_KEY_EVENT *Ev, ULONG *N, USHORT Code, USHORT Flags)
{
    if (*N >= 48) {
        return;
    }
    Ev[*N].MakeCode = (USHORT)(Code & 0xFF);
    Ev[*N].Flags = (USHORT)(Flags | ((Code & 0x100) ? UKEY_E0 : 0));
    (*N)++;
}

static BOOLEAN InReport(const UCHAR *Rep, UCHAR Key)
{
    int i;

    for (i = 2; i < 8; i++) {
        if (Rep[i] == Key) {
            return TRUE;
        }
    }
    return FALSE;
}

static void KbdReport(HID_DEV *h, const UCHAR *Rep, ULONG Len)
{
    USB_KEY_EVENT ev[48];
    ULONG n = 0;
    UCHAR cur[8];
    UCHAR changed;
    int i;
    OS_IRQL irql;

    if (Len < 3) {
        return;
    }
    UsbMemSet(cur, 0, sizeof(cur));
    UsbMemCpy(cur, Rep, Len > 8 ? 8 : Len);
    UsbLog(LOG_TRACE, "kbd report %02x %02x %02x at %u ms\n", cur[0], cur[2], cur[3], OsTimeMs());
    if (cur[2] == 0x01 || cur[3] == 0x01) {
        return;
    }

    changed = (UCHAR)(cur[0] ^ h->Prev[0]);
    for (i = 0; i < 8; i++) {
        if (changed & (1 << i)) {
            AddKey(ev, &n, ModMap[i], (USHORT)((cur[0] & (1 << i)) ? UKEY_MAKE : UKEY_BREAK));
        }
    }

    irql = OsLock();
    for (i = 2; i < 8; i++) {
        UCHAR k = h->Prev[i];
        USHORT code;
        if (k <= 3 || InReport(cur, k)) {
            continue;
        }
        if (RepActive && RepUsage == k) {
            RepActive = 0;
        }
        if (k == 0x48) {
            continue;
        }
        code = MapUsage(k);
        if (k == 0x46 && (cur[0] & 0x44)) {
            code = 0x54;
        }
        if (code != 0) {
            AddKey(ev, &n, code, UKEY_BREAK);
        }
    }
    for (i = 2; i < 8; i++) {
        UCHAR k = cur[i];
        USHORT code;
        if (k <= 3 || InReport(h->Prev, k)) {
            continue;
        }
        if (k == 0x48) {
            if (cur[0] & 0x11) {
                AddKey(ev, &n, 0x146, UKEY_MAKE);
                AddKey(ev, &n, 0x146, UKEY_BREAK);
            } else {
                AddKey(ev, &n, 0x1D, UKEY_MAKE | UKEY_E1);
                AddKey(ev, &n, 0x45, UKEY_MAKE);
                AddKey(ev, &n, 0x1D, UKEY_BREAK | UKEY_E1);
                AddKey(ev, &n, 0x45, UKEY_BREAK);
            }
            continue;
        }
        code = MapUsage(k);
        if (k == 0x46 && (cur[0] & 0x44)) {
            code = 0x54;
        }
        if (code == 0) {
            continue;
        }
        AddKey(ev, &n, code, UKEY_MAKE);
        RepActive = 1;
        RepUsage = k;
        RepCode = code;
        RepFlags = UKEY_MAKE;
        RepNext = OsTimeMs() + TypeDelay;
    }
    OsUnlock(irql);

    UsbMemCpy(h->Prev, cur, 8);
    if (n != 0) {
        OsKbdInput(ev, n);
    }
}

static LONG ScaleAbs(LONG v, const HID_FIELD *f)
{
    ULONG range = (ULONG)(f->Max - f->Min);
    ULONG val;

    if (f->Max <= f->Min) {
        return 0;
    }
    if (v < f->Min) {
        v = f->Min;
    }
    if (v > f->Max) {
        v = f->Max;
    }
    val = (ULONG)(v - f->Min);
    while (range > 65535) {
        range >>= 1;
        val >>= 1;
    }
    return (LONG)(val * 65535UL / range);
}

static void MouseReport(HID_DEV *h, const UCHAR *Rep, ULONG Len)
{
    USB_MOUSE_EVENT e;
    UCHAR buttons = 0;
    UCHAR diff;
    static const USHORT down[5] = { UMOU_LEFT_DOWN, UMOU_RIGHT_DOWN, UMOU_MIDDLE_DOWN, UMOU_B4_DOWN, UMOU_B5_DOWN };
    static const USHORT up[5] = { UMOU_LEFT_UP, UMOU_RIGHT_UP, UMOU_MIDDLE_UP, UMOU_B4_UP, UMOU_B5_UP };
    int i;

    UsbMemSet(&e, 0, sizeof(e));
    if (h->BootMode) {
        if (Len < 3) {
            return;
        }
        buttons = (UCHAR)(Rep[0] & 0x07);
        e.X = (CHAR)Rep[1];
        e.Y = (CHAR)Rep[2];
    } else {
        HID_MOUSE_LAYOUT *l = &h->Layout;
        if (l->UsesIds) {
            if (Len < 1 || Rep[0] != l->ReportId) {
                return;
            }
            Rep++;
            Len--;
        }
        if (l->Buttons.Valid) {
            for (i = 0; i < l->Buttons.Count && i < 5; i++) {
                if (HidGetField(Rep, Len, &l->Buttons, (ULONG)i)) {
                    buttons |= (UCHAR)(1 << i);
                }
            }
        }
        if (l->X.Relative) {
            e.X = HidGetField(Rep, Len, &l->X, 0);
            e.Y = HidGetField(Rep, Len, &l->Y, 0);
        } else {
            e.Absolute = 1;
            e.X = ScaleAbs(HidGetField(Rep, Len, &l->X, 0), &l->X);
            e.Y = ScaleAbs(HidGetField(Rep, Len, &l->Y, 0), &l->Y);
        }
        if (l->Wheel.Valid) {
            e.Wheel = (SHORT)HidGetField(Rep, Len, &l->Wheel, 0);
        }
        if (l->Pan.Valid) {
            e.Pan = (SHORT)HidGetField(Rep, Len, &l->Pan, 0);
        }
    }
    diff = (UCHAR)(buttons ^ h->PrevButtons);
    for (i = 0; i < 5; i++) {
        if (diff & (1 << i)) {
            e.ButtonFlags |= (buttons & (1 << i)) ? down[i] : up[i];
        }
    }
    h->PrevButtons = buttons;
    if (e.Wheel != 0) {
        e.ButtonFlags |= UMOU_WHEEL;
    }
    if (e.ButtonFlags == 0 && e.X == 0 && e.Y == 0 && !e.Absolute && e.Pan == 0) {
        return;
    }
    OsMouseInput(&e);
}

static void HidArm(HID_DEV *h);

static void HidDone(USB_XFER *X)
{
    HID_DEV *h = (HID_DEV *)X->Context;

    h->Active = 0;
    if (h->Gone) {
        return;
    }
    if (X->Status == USB_OK) {
        h->Errors = 0;
        if (X->Actual != 0) {
            if (h->Kind == HID_KBD) {
                KbdReport(h, h->Buf, X->Actual);
            } else {
                MouseReport(h, h->Buf, X->Actual);
            }
        }
        HidArm(h);
        return;
    }
    if (X->Status == USB_ERR_ABORTED || X->Status == USB_ERR_NODEV) {
        return;
    }
    h->Errors++;
    h->NeedService = 1;
    OsWakeWorker();
}

static void HidArm(HID_DEV *h)
{
    if (h->Active || h->Gone || !h->In.Opened) {
        return;
    }
    UsbMemSet(&h->Xfer, 0, sizeof(h->Xfer));
    h->Xfer.Pipe = &h->In;
    h->Xfer.Buf = h->Buf;
    h->Xfer.Phys = h->BufPhys;
    h->Xfer.Length = h->BufSize;
    h->Xfer.DirIn = 1;
    h->Xfer.Complete = HidDone;
    h->Xfer.Context = h;
    h->Active = 1;
    if (UsbSubmit(&h->Xfer) != USB_OK) {
        h->Active = 0;
    }
}

static int HidClassReq(USB_DEV *Dev, UCHAR Req, USHORT Value, UCHAR Iface, USHORT Len, void *Data)
{
    return UsbControl(Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE, Req, Value, Iface, Len, Data, 1000, NULL);
}

int HidAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *hd = UsbFindDesc(If, End, USB_DT_HID);
    const UCHAR *ep = If;
    UCHAR sub = If[6];
    UCHAR proto = If[7];
    UCHAR iface = If[2];
    HID_DEV *h;
    HID_MOUSE_LAYOUT layout;
    UCHAR kind = 0;
    UCHAR boot = 0;
    int slot;
    int r;

    while ((ep = UsbFindDesc(ep, End, USB_DT_ENDPOINT)) != NULL) {
        if ((ep[3] & 3) == USB_EP_INTERRUPT && (ep[2] & 0x80)) {
            break;
        }
    }
    if (ep == NULL) {
        return USB_ERR_PARAM;
    }
    UsbMemSet(&layout, 0, sizeof(layout));

    if (sub == 1 && proto == 1) {
        kind = HID_KBD;
        boot = 1;
    } else {
        USHORT rlen = 0;
        if (hd != NULL && hd[0] >= 9 && hd[6] == USB_DT_REPORT) {
            rlen = GET16(hd + 7);
        }
        if (rlen != 0 && rlen <= USB_DEV_DATA_MAX) {
            UCHAR *desc = (UCHAR *)OsAlloc(rlen);
            ULONG got = 0;
            if (desc != NULL) {
                r = UsbControl(Dev, USB_RT_IN | USB_RT_STD | USB_RT_INTERFACE, USB_REQ_GET_DESCRIPTOR,
                               USB_DT_REPORT << 8, iface, rlen, desc, 1000, &got);
                if (r == USB_OK && HidParseMouse(desc, got, &layout) == USB_OK) {
                    kind = HID_MOUSE;
                }
                OsFree(desc);
            }
        }
        if (kind == 0 && sub == 1 && proto == 2) {
            kind = HID_MOUSE;
            boot = 1;
        }
    }
    if (kind == 0) {
        UsbLog(LOG_INFO, "device %u: HID interface %u (%u/%u) not supported\n", Dev->Index, iface, sub, proto);
        return USB_ERR_PARAM;
    }

    for (slot = 0; slot < HID_MAX; slot++) {
        if (HidDevs[slot] == NULL) {
            break;
        }
    }
    if (slot == HID_MAX) {
        return USB_ERR_NOMEM;
    }
    h = (HID_DEV *)OsAlloc(sizeof(HID_DEV));
    if (h == NULL) {
        return USB_ERR_NOMEM;
    }
    h->Dev = Dev;
    h->Iface = iface;
    h->Kind = kind;
    h->BootMode = boot;
    h->Layout = layout;
    h->LedSent = 0xFF;

    if (sub == 1) {
        r = HidClassReq(Dev, HID_REQ_SET_PROTOCOL, (USHORT)(boot ? 0 : 1), iface, 0, NULL);
        if (r != USB_OK) {
            UsbLog(LOG_DBG, "device %u: SET_PROTOCOL failed (%d)\n", Dev->Index, r);
        }
    }
    HidClassReq(Dev, HID_REQ_SET_IDLE, 0, iface, 0, NULL);

    r = UsbOpenPipe(Dev, &h->In, ep);
    if (r != USB_OK) {
        OsFree(h);
        return r;
    }
    h->BufSize = h->In.MaxPacket;
    if (h->BufSize > 1024) {
        h->BufSize = 1024;
    }
    h->Buf = (UCHAR *)UsbDmaAlloc(h->BufSize < 8 ? 8 : h->BufSize, &h->BufPhys);
    if (h->Buf == NULL) {
        UsbClosePipe(&h->In);
        OsFree(h);
        return USB_ERR_NOMEM;
    }
    for (r = 0; r < 4; r++) {
        if (Dev->Hid[r] == NULL) {
            Dev->Hid[r] = h;
            break;
        }
    }
    HidDevs[slot] = h;
    Dev->Function |= (kind == HID_KBD) ? USB_FUNC_KEYBOARD : (!boot && !layout.X.Relative) ? USB_FUNC_TABLET : USB_FUNC_MOUSE;

    if (kind == HID_KBD) {
        UsbLog(LOG_INFO, "device %u: keyboard\n", Dev->Index);
    } else if (boot) {
        UsbLog(LOG_INFO, "device %u: mouse (boot protocol)\n", Dev->Index);
    } else {
        UsbLog(LOG_INFO, "device %u: %s, %u buttons%s%s\n", Dev->Index,
               layout.X.Relative ? "mouse" : "absolute pointer",
               layout.Buttons.Valid ? layout.Buttons.Count : 0,
               layout.Wheel.Valid ? ", wheel" : "", layout.Pan.Valid ? ", pan" : "");
    }
    HidArm(h);
    OsWakeWorker();
    return USB_OK;
}

void HidDetach(USB_DEV *Dev)
{
    int i, j;
    OS_IRQL irql;

    for (i = 0; i < 4; i++) {
        HID_DEV *h = (HID_DEV *)Dev->Hid[i];
        if (h == NULL) {
            continue;
        }
        h->Gone = 1;
        UsbClosePipe(&h->In);
        if (h->Kind == HID_KBD) {
            irql = OsLock();
            RepActive = 0;
            OsUnlock(irql);
            if (h->Prev[0] != 0 || h->Prev[2] != 0) {
                UCHAR empty[8];
                UsbMemSet(empty, 0, sizeof(empty));
                KbdReport(h, empty, 8);
            }
        } else if (h->PrevButtons != 0) {
            USB_MOUSE_EVENT e;
            UsbMemSet(&e, 0, sizeof(e));
            if (h->PrevButtons & 1) e.ButtonFlags |= UMOU_LEFT_UP;
            if (h->PrevButtons & 2) e.ButtonFlags |= UMOU_RIGHT_UP;
            if (h->PrevButtons & 4) e.ButtonFlags |= UMOU_MIDDLE_UP;
            if (h->PrevButtons & 8) e.ButtonFlags |= UMOU_B4_UP;
            if (h->PrevButtons & 16) e.ButtonFlags |= UMOU_B5_UP;
            OsMouseInput(&e);
        }
        for (j = 0; j < HID_MAX; j++) {
            if (HidDevs[j] == h) {
                HidDevs[j] = NULL;
            }
        }
        UsbDmaFree(h->Buf, h->BufSize < 8 ? 8 : h->BufSize);
        OsFree(h);
        Dev->Hid[i] = NULL;
    }
}

void HidService(void)
{
    int i;
    UCHAR hidLeds;
    ULONG want = LedWanted;

    hidLeds = (UCHAR)(((want & ULED_NUM) ? 1 : 0) | ((want & ULED_CAPS) ? 2 : 0) | ((want & ULED_SCROLL) ? 4 : 0));
    for (i = 0; i < HID_MAX; i++) {
        HID_DEV *h = HidDevs[i];
        if (h == NULL || h->Gone || h->Dev->Gone) {
            continue;
        }
        if (h->NeedService) {
            h->NeedService = 0;
            if (h->In.Halted) {
                UsbClearHalt(&h->In);
            }
            if (h->Errors > 20) {
                OsSleepMs(100);
            }
            HidArm(h);
        }
        if (h->Kind == HID_KBD && h->LedSent != hidLeds) {
            if (HidClassReq(h->Dev, HID_REQ_SET_REPORT, 0x0200, h->Iface, 1, &hidLeds) == USB_OK) {
                h->LedSent = hidLeds;
            } else {
                h->LedSent = hidLeds;
            }
        }
        if (!h->Active && !h->NeedService) {
            HidArm(h);
        }
    }
}

void HidTick(void)
{
    USB_KEY_EVENT ev;
    BOOLEAN fire = FALSE;
    OS_IRQL irql;
    ULONG now = OsTimeMs();

    irql = OsLock();
    if (RepActive && (LONG)(now - RepNext) >= 0) {
        ev.MakeCode = (USHORT)(RepCode & 0xFF);
        ev.Flags = (USHORT)(RepFlags | ((RepCode & 0x100) ? UKEY_E0 : 0));
        RepNext = now + (TypeRate ? 1000 / TypeRate : 33);
        fire = TRUE;
        UsbLog(LOG_TRACE, "typematic %02x at %u ms\n", RepCode, now);
    }
    OsUnlock(irql);
    if (fire) {
        OsKbdInput(&ev, 1);
    }
}

void HidSetLeds(ULONG NtIndicators)
{
    LedWanted = NtIndicators;
    OsWakeWorker();
}

void HidSetTypematic(ULONG DelayMs, ULONG RateCps)
{
    if (DelayMs >= 100 && DelayMs <= 2000) {
        TypeDelay = DelayMs;
    }
    if (RateCps >= 2 && RateCps <= 50) {
        TypeRate = RateCps;
    }
}

ULONG HidKeyboardCount(void)
{
    ULONG n = 0;
    int i;

    for (i = 0; i < HID_MAX; i++) {
        if (HidDevs[i] != NULL && HidDevs[i]->Kind == HID_KBD) {
            n++;
        }
    }
    return n;
}

ULONG HidMouseCount(void)
{
    ULONG n = 0;
    int i;

    for (i = 0; i < HID_MAX; i++) {
        if (HidDevs[i] != NULL && HidDevs[i]->Kind == HID_MOUSE) {
            n++;
        }
    }
    return n;
}

ULONG HidMouseButtons(void)
{
    ULONG n = 3;
    int i;

    for (i = 0; i < HID_MAX; i++) {
        HID_DEV *h = HidDevs[i];
        if (h != NULL && h->Kind == HID_MOUSE && !h->BootMode && h->Layout.Buttons.Valid &&
            h->Layout.Buttons.Count > n) {
            n = h->Layout.Buttons.Count > 5 ? 5 : h->Layout.Buttons.Count;
        }
    }
    return n;
}

BOOLEAN HidMouseHasWheel(void)
{
    int i;

    for (i = 0; i < HID_MAX; i++) {
        HID_DEV *h = HidDevs[i];
        if (h != NULL && h->Kind == HID_MOUSE && !h->BootMode && h->Layout.Wheel.Valid) {
            return TRUE;
        }
    }
    return OsGetConfig("ReportWheel", 1) ? TRUE : FALSE;
}
