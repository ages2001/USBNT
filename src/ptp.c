/*
 * ptp.c - touch pad gestures, one engine for every touch pad: Windows
 * Precision Touchpads in multi-touch mode (HID over I2C, later USB) and the
 * pads whose own protocol a caller decodes into contacts (PS/2 Synaptics,
 * Elantech, ALPS in ntps2.c; SMBus pads) - see ptp.h for that interface.
 * One finger moves the pointer (with acceleration), two fingers scroll
 * (vertical wheel and horizontal pan), a finger along the right edge (or the
 * bottom edge) scrolls on pads without a finger count, the pad button clicks
 * (with two fingers on the pad, or in the bottom right corner of a ClickPad:
 * right button), taps click (one finger left, two right, three middle).
 *
 * Reports may carry all contacts at once ("parallel") or a few per report
 * ("hybrid": the contact count is only in the first report of a frame);
 * contacts are kept by their contact identifier until a frame is complete.
 *
 * Settings (registry / harness config):
 *   TouchpadTap         1 = tap to click (default), 0 = off
 *   TouchpadScroll      1 = content follows the fingers (Windows default),
 *                       2 = the other way round, 0 = no two finger scrolling
 *   TouchpadEdgeScroll  1 = on, 0 = off, 2 = on for pads without a finger count (default)
 *   TouchpadSpeed       pointer speed in percent (100)
 *   TouchpadAccel       1 = pointer acceleration (default), 0 = linear
 *   TouchpadScrollSpeed scrolling speed in percent (100)
 */

#include "usbinput.h"
#include "ptp.h"

#define TAP_MS          180
#define CLICK_MS        40

static void Settings(PTP_STATE *S)
{
    S->Tap = (UCHAR)OsGetConfig("TouchpadTap", 1);
    S->ScrollMode = (UCHAR)OsGetConfig("TouchpadScroll", 1);
    S->Speed = OsGetConfig("TouchpadSpeed", 100);
    if (S->Speed < 10 || S->Speed > 1000) {
        S->Speed = 100;
    }
    S->ScrollSpeed = OsGetConfig("TouchpadScrollSpeed", 100);
    if (S->ScrollSpeed < 10 || S->ScrollSpeed > 1000) {
        S->ScrollSpeed = 100;
    }
    S->Accel = (UCHAR)OsGetConfig("TouchpadAccel", 1);
    S->EdgeScroll = (UCHAR)OsGetConfig("TouchpadEdgeScroll", 2);
    if (S->EdgeScroll == 2) {
        S->EdgeScroll = (UCHAR)((S->Flags & PTP_PAD_ONEFINGER) != 0);
    }
}

void PtpInit(PTP_STATE *S, const HID_PTP_LAYOUT *L)
{
    UsbMemSet(S, 0, sizeof(*S));
    S->L = *L;
    Settings(S);
    S->MinX = L->F[0].X.Min;
    S->MaxX = L->F[0].X.Max;
    S->MinY = L->F[0].Y.Min;
    S->MaxY = L->F[0].Y.Max;
    S->RangeX = S->MaxX - S->MinX;
    S->RangeY = S->MaxY - S->MinY;
    if (S->RangeX <= 0) S->RangeX = 1000;
    if (S->RangeY <= 0) S->RangeY = S->RangeX;
}

void PtpInitPad(PTP_STATE *S, LONG MinX, LONG MaxX, LONG MinY, LONG MaxY, ULONG Flags, PTP_OUT Out, void *Ctx)
{
    UsbMemSet(S, 0, sizeof(*S));
    S->Flags = (UCHAR)Flags;
    S->Out = Out;
    S->Ctx = Ctx;
    Settings(S);
    S->MinX = MinX;
    S->MaxX = MaxX;
    S->MinY = MinY;
    S->MaxY = MaxY;
    S->RangeX = MaxX - MinX;
    S->RangeY = MaxY - MinY;
    if (S->RangeX <= 0) S->RangeX = 1000;
    if (S->RangeY <= 0) S->RangeY = S->RangeX;
}

/* feature report that switches the pad to touch pad mode (Input Mode = 3);
   returns its length with the report id first, 0 when there is none */
ULONG PtpModeReport(const HID_PTP_LAYOUT *L, UCHAR *Out, ULONG Max)
{
    ULONG bytes = (L->ModeBits + 7) / 8, i, v = 3;

    if (!L->ModeValid || bytes == 0 || bytes + 1 > Max) {
        return 0;
    }
    UsbMemSet(Out, 0, bytes + 1);
    Out[0] = L->ModeId;
    for (i = 0; i < L->ModeSize && i < 8; i++) {
        ULONG b = L->ModeOff + i;
        if (v & (1UL << i)) {
            Out[1 + (b >> 3)] |= (UCHAR)(1 << (b & 7));
        }
    }
    return bytes + 1;
}

static PTP_CONTACT *Slot(PTP_STATE *S, ULONG Id)
{
    int i, freeSlot = -1;

    for (i = 0; i < PTP_MAX_CONTACTS; i++) {
        if (S->C[i].Used && S->C[i].Id == Id) {
            return &S->C[i];
        }
        if (!S->C[i].Used && freeSlot < 0) {
            freeSlot = i;
        }
    }
    if (freeSlot < 0) {
        return NULL;
    }
    UsbMemSet(&S->C[freeSlot], 0, sizeof(S->C[freeSlot]));
    S->C[freeSlot].Used = 1;
    S->C[freeSlot].Id = Id;
    return &S->C[freeSlot];
}

static void Emit(PTP_STATE *S, void *Mouse, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan)
{
    UCHAR b = (UCHAR)(S->ClickButtons | S->TapButtons | S->PhysButtons);
    if (S->Out != NULL) {
        S->Out(S->Ctx, b, Dx, Dy, Wheel, Pan);
    } else {
        HidExtMouse(Mouse, b, Dx, Dy, Wheel, Pan);
    }
}

/* the ClickPad's button area: the bottom 18 % of the pad */
static BOOLEAN InButtons(const PTP_STATE *S, const PTP_CONTACT *C)
{
    return (BOOLEAN)(C->Y >= S->MaxY - S->RangeY * 18 / 100);
}

/* which button a press of the pad's button is */
static UCHAR PadClick(const PTP_STATE *S, PTP_CONTACT **F, int N)
{
    int i, inArea = 0, right = 0;

    if (S->Flags & PTP_PAD_CLICKPAD) {
        for (i = 0; i < N; i++) {
            if (InButtons(S, F[i])) {
                inArea++;
                if (F[i]->X >= S->MinX + S->RangeX / 2) right = 1;
            }
        }
        if (inArea) {
            /* the finger that presses is the one in the button area; a second finger elsewhere rests */
            return (UCHAR)(right ? 2 : 1);
        }
    }
    return (UCHAR)(N >= 3 ? 4 : N == 2 ? 2 : 1);
}

/* scroll motion to detents (step = pad units per detent) */
static SHORT Detents(LONG *Acc, LONG D, LONG Step)
{
    SHORT n = 0;
    *Acc += D;
    while (*Acc >= Step) { n++; *Acc -= Step; }
    while (*Acc <= -Step) { n--; *Acc += Step; }
    return n;
}

/* one complete frame of contacts */
static void Frame(PTP_STATE *S, void *Mouse, ULONG Now)
{
    PTP_CONTACT *f[PTP_MAX_CONTACTS];
    int n = 0, i;
    LONG dx = 0, dy = 0, sx, sy;
    LONG step = S->RangeY * 100 / 24 / (LONG)S->ScrollSpeed;
    SHORT wheel = 0, pan = 0;
    UCHAR pressed;

    if (step < 1) step = 1;
    for (i = 0; i < PTP_MAX_CONTACTS; i++) {
        if (S->C[i].Used && S->C[i].Down) {
            f[n++] = &S->C[i];
        }
    }
    /* the pad's button: right button when two fingers are on the pad */
    pressed = S->ButtonDown;
    if (pressed && !S->ClickButtons) {
        S->ClickButtons = PadClick(S, f, n);
        S->TapPossible = 0;
    } else if (!pressed && S->ClickButtons) {
        S->ClickButtons = 0;
    }
    if (S->PhysButtons) {
        S->TapPossible = 0;
    }
    /* a release of a tap click is due */
    if (S->TapButtons && (LONG)(Now - S->TapUntil) >= 0) {
        S->TapButtons = 0;
    }
    /* a finger that comes down at the right (bottom) edge scrolls while it is the only one */
    if (n > 0 && S->PrevN == 0) {
        S->Edge = 0;
        if (S->EdgeScroll && n == 1) {
            if (f[0]->X >= S->MaxX - S->RangeX * 8 / 100) {
                S->Edge = 1;
            } else if (!(S->Flags & PTP_PAD_CLICKPAD) && f[0]->Y >= S->MaxY - S->RangeY * 10 / 100) {
                S->Edge = 2;
            }
            S->AccX = S->AccY = 0;
        }
    }
    if (n != 1) {
        S->Edge = 0;
    }
    if (n == 1 && S->PrevN == 1 && f[0]->Id == S->PrevId && S->Edge) {
        sx = f[0]->X - S->PrevX;
        sy = f[0]->Y - S->PrevY;
        if (S->ScrollMode == 2) { sx = -sx; sy = -sy; }
        if (S->Edge == 1) wheel = Detents(&S->AccY, sy, step);
        else pan = (SHORT)-Detents(&S->AccX, sx, step);
        S->RemX = S->RemY = 0;
    } else if (n == 1 && S->PrevN == 1 && f[0]->Id == S->PrevId &&
               !(S->ClickButtons && (S->Flags & PTP_PAD_CLICKPAD) && InButtons(S, f[0]))) {
        /* pointer: the same scale on both axes, from the width of the pad */
        LONG ax = (f[0]->X - S->PrevX) * 1600 * (LONG)S->Speed / 100;
        LONG ay = (f[0]->Y - S->PrevY) * 1600 * (LONG)S->Speed / 100;
        if (S->Accel) {
            /* gain from the speed (counts in this frame): slow motion finer, fast motion further */
            LONG v = ((ax < 0 ? -ax : ax) + (ay < 0 ? -ay : ay)) / S->RangeX;
            LONG g = v < 3 ? 70 + v * 10 : 100 + (v - 3) * 4;
            if (g > 220) g = 220;
            ax = ax / 10 * g / 10;
            ay = ay / 10 * g / 10;
        }
        ax += S->RemX;
        ay += S->RemY;
        dx = ax / S->RangeX;
        dy = ay / S->RangeX;
        S->RemX = ax - dx * S->RangeX;
        S->RemY = ay - dy * S->RangeX;
    } else {
        S->RemX = S->RemY = 0;
    }
    if (n == 2 && S->ScrollMode) {
        LONG cx = (f[0]->X + f[1]->X) / 2, cy = (f[0]->Y + f[1]->Y) / 2;
        if (S->PrevN == 2) {
            sx = cx - S->PrevCx;
            sy = cy - S->PrevCy;
            if (S->ScrollMode == 2) { sx = -sx; sy = -sy; }
            /* fingers down (y grows): the content moves down = wheel away from the user */
            wheel = Detents(&S->AccY, sy, step);
            pan = (SHORT)-Detents(&S->AccX, sx, step);
        } else {
            S->AccX = S->AccY = 0;
        }
        S->PrevCx = cx;
        S->PrevCy = cy;
    }
    /* taps */
    if (n > 0 && S->PrevN == 0) {
        S->TapStart = Now;
        S->TapMax = 0;
        S->TapMove = 0;
        S->TapPossible = (UCHAR)!S->PhysButtons;
        S->TapX0 = f[0]->X;
        S->TapY0 = f[0]->Y;
    }
    if (n > S->TapMax) S->TapMax = (UCHAR)n;
    if (n > 0) {
        LONG mx = f[0]->X - S->TapX0, my = f[0]->Y - S->TapY0;
        if (mx < 0) mx = -mx;
        if (my < 0) my = -my;
        if (mx + my > S->TapMove) S->TapMove = mx + my;
    }
    if (n == 0 && S->PrevN > 0 && S->Tap && S->TapPossible && Now - S->TapStart <= TAP_MS &&
        S->TapMove < S->RangeX / 30 && !S->ClickButtons) {
        UCHAR b = (UCHAR)(S->TapMax >= 3 ? 4 : S->TapMax == 2 ? 2 : 1);
        S->TapButtons = b;
        S->TapUntil = Now + CLICK_MS;
        Emit(S, Mouse, 0, 0, 0, 0);                         /* down; up after CLICK_MS (PtpTick) */
    }
    if (n == 0) {
        S->TapPossible = S->TapPossible && Now - S->TapStart <= TAP_MS;
    }
    if (dx || dy || wheel || pan || (UCHAR)(S->ClickButtons | S->TapButtons | S->PhysButtons) != S->LastButtons) {
        Emit(S, Mouse, dx, dy, wheel, pan);
        S->LastButtons = (UCHAR)(S->ClickButtons | S->TapButtons | S->PhysButtons);
    }
    S->PrevN = n;
    if (n >= 1) {
        S->PrevId = f[0]->Id;
        S->PrevX = f[0]->X;
        S->PrevY = f[0]->Y;
    }
    /* contacts that left the pad are forgotten */
    for (i = 0; i < PTP_MAX_CONTACTS; i++) {
        if (S->C[i].Used && !S->C[i].Down) {
            S->C[i].Used = 0;
        }
    }
}

/* a frame of contacts decoded by the caller (ptp.h) */
void PtpFrame(PTP_STATE *S, const PTP_TOUCH *T, ULONG N, UCHAR Buttons)
{
    ULONG k;

    if (S->Flags & PTP_PAD_CLICKPAD) {
        S->ButtonDown = (UCHAR)(Buttons & 1);
        S->PhysButtons = (UCHAR)(Buttons & 6);
    } else {
        S->ButtonDown = 0;
        S->PhysButtons = (UCHAR)(Buttons & 7);
    }
    for (k = 0; k < PTP_MAX_CONTACTS; k++) {
        S->C[k].Down = 0;
    }
    for (k = 0; k < N && k < PTP_MAX_CONTACTS; k++) {
        PTP_CONTACT *c = Slot(S, T[k].Id);
        if (c == NULL) {
            continue;
        }
        c->Down = 1;
        c->X = T[k].X;
        c->Y = T[k].Y;
    }
    Frame(S, NULL, OsTimeMs());
}

void PtpReport(PTP_STATE *S, const UCHAR *Rep, ULONG Len, void *Mouse)
{
    const HID_PTP_LAYOUT *L = &S->L;
    ULONG now = OsTimeMs();
    ULONG count = 0, k;

    if (L->UsesIds) {
        if (Len < 1 || Rep[0] != L->ReportId) {
            return;
        }
        Rep++;
        Len--;
    }
    if (L->Button.Valid) {
        S->ButtonDown = (UCHAR)(HidGetField(Rep, Len, &L->Button, 0) != 0);
    }
    if (L->Count.Valid) {
        count = (ULONG)HidGetField(Rep, Len, &L->Count, 0);
        if (count != 0) {
            S->Expect = count;                              /* a new frame starts */
            S->Seen = 0;
        } else if (S->Seen >= S->Expect) {
            /* nothing on the pad any more (or only the button changed) */
            for (k = 0; k < PTP_MAX_CONTACTS; k++) S->C[k].Down = 0;
            Frame(S, Mouse, now);
            return;
        }
    } else {
        S->Expect = L->NFingers;
        S->Seen = 0;
    }
    for (k = 0; k < L->NFingers && S->Seen < S->Expect; k++) {
        const HID_PTP_FINGER *fg = &L->F[k];
        PTP_CONTACT *c;
        ULONG id = fg->Id.Valid ? (ULONG)HidGetField(Rep, Len, &fg->Id, 0) : k;
        if (!fg->X.Valid || !fg->Tip.Valid) {
            continue;
        }
        S->Seen++;
        c = Slot(S, id);
        if (c == NULL) {
            continue;
        }
        c->Down = (UCHAR)(HidGetField(Rep, Len, &fg->Tip, 0) != 0);
        if (fg->Conf.Valid && !HidGetField(Rep, Len, &fg->Conf, 0)) {
            c->Down = 0;                                    /* a palm: not a finger */
        }
        c->X = HidGetField(Rep, Len, &fg->X, 0);
        c->Y = fg->Y.Valid ? HidGetField(Rep, Len, &fg->Y, 0) : 0;
    }
    if (S->Seen >= S->Expect) {
        Frame(S, Mouse, now);
    }
}

/* timer: ends tap clicks */
void PtpTick(PTP_STATE *S, void *Mouse)
{
    if (S->TapButtons && (LONG)(OsTimeMs() - S->TapUntil) >= 0) {
        S->TapButtons = 0;
        Emit(S, Mouse, 0, 0, 0, 0);
        S->LastButtons = (UCHAR)(S->ClickButtons | S->PhysButtons);
    }
}

BOOLEAN PtpBusy(const PTP_STATE *S)
{
    return (BOOLEAN)(S->TapButtons != 0);
}
