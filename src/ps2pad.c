/*
 * ps2pad.c - native protocols of PS/2 touch pads, after Linux
 * (drivers/input/mouse/synaptics.c, elantech.c, alps.c, psmouse-base.c):
 *
 *   Synaptics  identify / model / capabilities / extended capability
 *              queries, absolute mode with W mode (finger count, palm width),
 *              advanced gesture mode (a second contact: semi-mt, or the real
 *              position on image sensors), ClickPads, the pass-through port
 *              (trackpoint), stick buttons wired to the pad
 *   Elantech   hardware versions 2 (semi-mt), 3 (two contacts, head / tail
 *              packets) and 4 (five contacts, status / head / motion
 *              packets), ClickPads ("buttonpads"), trackpoint packets
 *   ALPS       v3 (Pinnacle, Rushmore), v5 (Dolphin) - position packets
 *              with the bitmap packets of two fingers (semi-mt) - and v7
 *              (two contacts, button pads), trackstick packets, bare PS/2
 *              packets of an external mouse
 *
 * Detection order as in psmouse-base.c: Synaptics, ALPS, Elantech.  Other
 * pads (ALPS v1 / v2 / v4 / v6 / v8, Elantech v1, Synaptics with the old
 * absolute format, Focaltech, Cypress, Sentelic, Lifebook) are not switched:
 * they stay plain (IntelliMouse) mice.
 *
 * Contacts go to the touch pad engine (ptp.c: pointer, taps, two finger and
 * edge scrolling, ClickPad buttons); trackpoint motion is relative, the
 * middle button with motion scrolls, a middle press without motion clicks.
 *
 * Detection may run at the interrupt level of the controller (the Windows
 * 2000 / XP filter): nothing here logs or allocates; what was found is in
 * P->Info for the caller's log.
 */

#include "ps2pad.h"

#define BIT(n)          (1UL << (n))
#define STICK_STEP      12              /* trackpoint counts per wheel detent */

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */
static BOOLEAN Cmd(const PS2PAD_IO *Io, UCHAR C)
{
    return Io->Send(Io->Ctx, C);
}

static BOOLEAN Arg(const PS2PAD_IO *Io, UCHAR C, UCHAR A)
{
    return (BOOLEAN)(Cmd(Io, C) && Cmd(Io, A));
}

/* a command with N result bytes */
static BOOLEAN Get(const PS2PAD_IO *Io, UCHAR C, UCHAR *R, int N)
{
    int i, b;

    if (!Cmd(Io, C)) {
        return FALSE;
    }
    for (i = 0; i < N; i++) {
        b = Io->Read(Io->Ctx, 50);
        if (b < 0) {
            return FALSE;
        }
        R[i] = (UCHAR)b;
    }
    return TRUE;
}

static BOOLEAN Info(const PS2PAD_IO *Io, UCHAR *R)
{
    return Get(Io, 0xE9, R, 3);                         /* status request */
}

static BOOLEAN Reset(const PS2PAD_IO *Io)
{
    int b;

    if (!Cmd(Io, 0xFF)) {
        return FALSE;
    }
    b = Io->Read(Io->Ctx, 800);
    if (b == 0xAA) {
        (void)Io->Read(Io->Ctx, 100);                   /* device id 00 */
    }
    return TRUE;
}

/* Synaptics "special command": E6, then the byte two bits at a time in E8 arguments */
static BOOLEAN Sliced(const PS2PAD_IO *Io, UCHAR C)
{
    int i;

    if (!Cmd(Io, 0xE6)) {
        return FALSE;
    }
    for (i = 6; i >= 0; i -= 2) {
        if (!Arg(Io, 0xE8, (UCHAR)((C >> i) & 3))) {
            return FALSE;
        }
    }
    return TRUE;
}

static ULONG Be3(const UCHAR *R)
{
    return ((ULONG)R[0] << 16) | ((ULONG)R[1] << 8) | R[2];
}

static void Name(PS2PAD *P, const char *S)
{
    int i;

    for (i = 0; S[i] && i < (int)sizeof(P->Name) - 1; i++) {
        P->Name[i] = S[i];
    }
    P->Name[i] = 0;
}

/* ------------------------------------------------------------------ */
/* output: pad (through the engine) and trackpoint on one pointer      */
/* ------------------------------------------------------------------ */
static void Emit(PS2PAD *P, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan)
{
    P->LastOut = (UCHAR)(P->PadButtons | P->StickButtons);
    P->Out(P->OutCtx, P->LastOut, Dx, Dy, Wheel, Pan);
}

static void PadOut(void *Ctx, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan)
{
    PS2PAD *P = (PS2PAD *)Ctx;

    P->PadButtons = Buttons;
    Emit(P, Dx, Dy, Wheel, Pan);
}

/* trackpoint: Buttons bit 0 left, 1 right, 2 middle; Dy grows downwards */
static void Stick(PS2PAD *P, UCHAR Buttons, LONG Dx, LONG Dy)
{
    UCHAR mid = (UCHAR)(Buttons & 4);
    SHORT wheel = 0;

    Buttons &= 3;
    if (mid && !P->StickMid) {
        P->StickMid = 1;
        P->StickScrolled = 0;
        P->StickAcc = 0;
    }
    if (P->StickMid) {
        if (mid) {
            /* middle button held: motion scrolls (stick pushed away = wheel away from the user) */
            P->StickAcc += Dy;
            while (P->StickAcc >= STICK_STEP) { wheel--; P->StickAcc -= STICK_STEP; P->StickScrolled = 1; }
            while (P->StickAcc <= -STICK_STEP) { wheel++; P->StickAcc += STICK_STEP; P->StickScrolled = 1; }
            Dx = Dy = 0;
        } else {
            P->StickMid = 0;
            if (!P->StickScrolled) {
                /* a middle press without motion: a middle click */
                P->StickButtons = (UCHAR)(Buttons | 4);
                Emit(P, 0, 0, 0, 0);
            }
        }
    }
    if (P->StickButtons != Buttons || Dx || Dy || wheel) {
        P->StickButtons = Buttons;
        Emit(P, Dx, Dy, wheel, 0);
    }
}

/* a standard 3 byte PS/2 packet of a trackpoint / external mouse */
static void StickPacket(PS2PAD *P, UCHAR B0, UCHAR Bx, UCHAR By)
{
    LONG dx, dy;

    if ((B0 & 0xC0) || !(B0 & 0x08)) {
        return;                                         /* overflow, or an ACK of the guest */
    }
    dx = (LONG)Bx - ((B0 & 0x10) ? 256 : 0);
    dy = (LONG)By - ((B0 & 0x20) ? 256 : 0);
    Stick(P, (UCHAR)(B0 & 7), dx, -dy);
}

/* stick buttons that come in touch pad packets */
static UCHAR StickState(const PS2PAD *P)
{
    return (UCHAR)(P->StickButtons | (P->StickMid ? 4 : 0));
}

static void StickButtons(PS2PAD *P, UCHAR B)
{
    if ((UCHAR)(B & 7) != StickState(P)) {
        Stick(P, (UCHAR)(B & 7), 0, 0);
    }
}

static void Frame(PS2PAD *P, PTP_TOUCH *T, ULONG N, ULONG Have, UCHAR Buttons)
{
    ULONG k;

    if (N > 4) N = 4;
    if (Have == 0 && N > 0) {
        return;
    }
    /* fingers the pad counts but does not locate: copies of the last located one */
    for (k = Have; k < N; k++) {
        T[k] = T[Have - 1];
        T[k].Id = k;
    }
    P->Packets++;
    PtpFrame(&P->Ptp, T, N, Buttons);
}

static void Start(PS2PAD *P, ULONG Flags, PTP_OUT Out, void *Ctx)
{
    P->Out = Out;
    P->OutCtx = Ctx;
    P->PktSize = 6;
    P->Count = 0;
    PtpInitPad(&P->Ptp, P->MinX, P->MaxX, P->MinY, P->MaxY, Flags, PadOut, P);
}

/* ------------------------------------------------------------------ */
/* Synaptics                                                          */
/* ------------------------------------------------------------------ */
#define SYN_MAJOR(i)        ((i) & 0x0F)
#define SYN_FULL(i)         ((((i) & 0x0F) << 8) | (((i) >> 16) & 0xFF))
#define SYN_EXTENDED        BIT(23)
#define SYN_MIDDLE          BIT(18)
#define SYN_PASS            BIT(7)
#define SYN_MULTIFINGER     BIT(1)
#define SYN_X_CLICKPAD      BIT(20)     /* 0x0c */
#define SYN_X_ADV_GESTURE   BIT(19)
#define SYN_X_MAX           BIT(17)
#define SYN_X_MIN           BIT(13)
#define SYN_X_IMAGE         BIT(11)
#define SYN_X10_STICK_BTNS  BIT(16)     /* 0x10 */

static BOOLEAN SynQuery(const PS2PAD_IO *Io, UCHAR Q, UCHAR *R)
{
    return (BOOLEAN)(Sliced(Io, Q) && Info(Io, R));
}

static int Synaptics(PS2PAD *P, const PS2PAD_IO *Io, PTP_OUT Out, void *Ctx)
{
    UCHAR r[3];
    ULONG req, flags = 0;
    int i;

    for (i = 0; i < 4; i++) {
        if (!Arg(Io, 0xE8, 0)) return PS2PAD_NONE;
    }
    if (!Info(Io, r) || r[1] != 0x47) {
        return PS2PAD_NONE;
    }
    Reset(Io);
    if (!SynQuery(Io, 0x00, r) || r[1] != 0x47) {
        return PS2PAD_NONE;
    }
    P->Ident = Be3(r);
    Name(P, "Synaptics");
    if (!SynQuery(Io, 0x03, r)) return PS2PAD_NONE;
    P->Model = Be3(r);
    if (SYN_FULL(P->Ident) >= 0x705 && SynQuery(Io, 0x01, r) && (r[0] & 0x02) && SynQuery(Io, 0x10, r)) {
        P->Ext10 = Be3(r);
    }
    if (!SynQuery(Io, 0x02, r)) return PS2PAD_NONE;
    P->Caps = Be3(r);
    if (SYN_FULL(P->Ident) < 0x705 && ((P->Caps >> 8) & 0xFF) != 0x47) {
        return PS2PAD_NONE;
    }
    if (!(P->Caps & SYN_EXTENDED)) {
        P->Caps = 0;
    }
    req = (P->Caps >> 20) & 7;
    if (req >= 1 && SynQuery(Io, 0x09, r)) {
        P->ExtCap = Be3(r);
        if (((P->ExtCap >> 12) & 0x0F) > 8) P->ExtCap &= ~0xF000UL;
    }
    if (req >= 4 && SynQuery(Io, 0x0C, r)) {
        P->Ext0C = Be3(r);
    }
    P->MinX = 1472; P->MaxX = 5472;
    P->MinY = 1408; P->MaxY = 4448;
    if (SYN_MAJOR(P->Ident) >= 4) {
        if (req >= 5 && (P->Ext0C & SYN_X_MAX) && SynQuery(Io, 0x0D, r)) {
            LONG mx = ((LONG)r[0] << 5) | ((r[1] & 0x0F) << 1), my = ((LONG)r[2] << 5) | ((r[1] & 0xF0) >> 3);
            if (mx > P->MinX && my > P->MinY) { P->MaxX = mx; P->MaxY = my; }
        }
        if ((P->Ext0C & SYN_X_MIN) && (req >= 7 || SYN_FULL(P->Ident) == 0x801) && SynQuery(Io, 0x0F, r)) {
            LONG mx = ((LONG)r[0] << 5) | ((r[1] & 0x0F) << 1), my = ((LONG)r[2] << 5) | ((r[1] & 0xF0) >> 3);
            if (mx < P->MaxX && my < P->MaxY) { P->MinX = mx; P->MinY = my; }
        }
    }
    UsbFormat(P->Info, sizeof(P->Info), "Synaptics %u.%u, model %06x, caps %06x/%06x/%06x/%06x, x %d..%d y %d..%d",
           SYN_MAJOR(P->Ident), (P->Ident >> 16) & 0xFF, P->Model, P->Caps, P->ExtCap, P->Ext0C, P->Ext10,
           P->MinX, P->MaxX, P->MinY, P->MaxY);
    if (!(P->Model & BIT(7))) {
        UsbFormat(P->Info, sizeof(P->Info), "Synaptics with the old absolute format: relative mode");
        return PS2PAD_NONE;
    }
    /* absolute, 80 packets/s, no gestures of the pad itself, W mode */
    P->SynMode = 0x80 | 0x40;
    if (SYN_MAJOR(P->Ident) >= 4) P->SynMode |= 0x04;
    if (P->Caps & SYN_EXTENDED) P->SynMode |= 0x01;
    if (!Sliced(Io, P->SynMode) || !Arg(Io, 0xF3, 0x14)) {
        return PS2PAD_NONE;
    }
    P->Agm = (UCHAR)((P->Ext0C & (SYN_X_ADV_GESTURE | SYN_X_IMAGE)) != 0);
    if (P->Agm && (!Sliced(Io, 0x03) || !Arg(Io, 0xF3, 0xC8))) {
        P->Agm = 0;
    }
    P->ClickPad = (UCHAR)((P->Ext0C & SYN_X_CLICKPAD) != 0);
    P->Stick = (UCHAR)((P->Caps & SYN_PASS) != 0);
    if (P->ClickPad) flags |= PTP_PAD_CLICKPAD;
    if (!(P->Caps & SYN_MULTIFINGER) && !P->Agm) flags |= PTP_PAD_ONEFINGER;
    if (P->Agm && !(P->Ext0C & SYN_X_IMAGE)) flags |= PTP_PAD_SEMIMT;
    P->Proto = PS2PAD_SYNAPTICS;
    Name(P, P->ClickPad ? "Synaptics ClickPad" : "Synaptics TouchPad");
    Start(P, flags, Out, Ctx);
    return PS2PAD_SYNAPTICS;
}

static BOOLEAN SynValid(const PS2PAD *P, int I)
{
    if (I == 0) return (BOOLEAN)((P->Pkt[0] & 0xC0) == 0x80);
    if (I == 3) return (BOOLEAN)((P->Pkt[3] & 0xC0) == 0xC0);
    return TRUE;
}

static void SynPacket(PS2PAD *P)
{
    const UCHAR *b = P->Pkt;
    PTP_TOUCH t[4];
    LONG x, y;
    int z, w, n = 0;
    UCHAR btn, mid;

    if (P->Stick && (b[0] & 0xFC) == 0x84 && (b[3] & 0xCC) == 0xC4) {
        StickPacket(P, b[1], b[4], b[5]);               /* pass-through: the guest's packet */
        return;
    }
    w = ((b[0] & 0x30) >> 2) | ((b[0] & 0x04) >> 1) | ((b[3] & 0x04) >> 2);
    if (P->Agm && w == 2) {
        int type = (b[5] & 0x30) >> 4;
        if (type == 1) {
            P->AgmX = (LONG)((((b[4] & 0x0F) << 8) | b[1]) << 1);
            P->AgmY = (LONG)((((b[4] & 0xF0) << 4) | b[2]) << 1);
        } else if (type == 2) {
            P->AgmCount = b[1];
        }
        return;
    }
    x = ((b[3] & 0x10) << 8) | ((b[1] & 0x0F) << 8) | b[4];
    y = ((b[3] & 0x20) << 7) | ((b[1] & 0xF0) << 4) | b[5];
    z = b[2];
    mid = (UCHAR)((b[0] ^ b[3]) & 0x01);
    btn = (UCHAR)(b[0] & 0x03);
    if (P->ClickPad) {
        btn = (UCHAR)((b[0] & 0x02) | mid);             /* the ClickPad's button comes as the middle bit */
    } else if (P->Caps & SYN_MIDDLE) {
        btn |= (UCHAR)(mid << 2);
    }
    if (((P->ExtCap >> 12) & 0x0F) > 0 && ((b[0] ^ b[3]) & 0x02) && (P->Ext10 & SYN_X10_STICK_BTNS)) {
        /* trackpoint buttons wired to the pad */
        ULONG bits = ((((P->ExtCap >> 12) & 0x0F) + 1) >> 1), mask = (1UL << bits) - 1;
        ULONG eb = (b[4] & mask) | ((b[5] & mask) << bits);
        StickButtons(P, (UCHAR)((eb & 1) | ((eb & 4) >> 1) | ((eb & 2) << 1)));
    }
    if (x > 8176) x -= 8192; else if (x == 8176) x = 6143;
    if (y > 8176) y -= 8192; else if (y == 8176) y = 6143;
    if (z > 30) P->Touching = 1;
    else if (z < 25) P->Touching = 0;
    if (P->Ext0C & SYN_X_IMAGE) {
        n = !P->Touching ? 0 : w >= 4 ? 1 : w == 0 ? 2 : w == 1 ? (P->AgmCount ? P->AgmCount : 3) : 4;
    } else if (P->Touching && x > 1) {
        n = 1;
        if ((P->Caps & SYN_EXTENDED) && w <= 1 && ((P->Caps & SYN_MULTIFINGER) || P->Agm)) {
            n = w + 2;
        }
    }
    t[0].Id = 0;
    t[0].X = x;
    t[0].Y = P->MaxY + P->MinY - y;                     /* Synaptics: y grows upwards */
    if (n >= 2 && P->Agm && (P->AgmX || P->AgmY)) {
        t[1].Id = 1;
        t[1].X = P->AgmX;
        t[1].Y = P->MaxY + P->MinY - P->AgmY;
        Frame(P, t, (ULONG)n, 2, btn);
    } else {
        Frame(P, t, (ULONG)n, 1, btn);
    }
}

/* ------------------------------------------------------------------ */
/* Elantech                                                           */
/* ------------------------------------------------------------------ */
/* query: v2 the Synaptics way, v3 and v4 with the F8 prefix */
static BOOLEAN ElanQuery(const PS2PAD *P, const PS2PAD_IO *Io, UCHAR Q, UCHAR *R)
{
    if (P->Ver >= 3) {
        return (BOOLEAN)(Cmd(Io, 0xF8) && Cmd(Io, Q) && Info(Io, R));
    }
    return SynQuery(Io, Q, R);
}

static BOOLEAN ElanWrite(const PS2PAD *P, const PS2PAD_IO *Io, UCHAR Reg, UCHAR Val)
{
    switch (P->Ver) {
    case 2:
        return (BOOLEAN)(Arg(Io, 0xF8, 0x11) && Arg(Io, 0xF8, Reg) && Arg(Io, 0xF8, Val) && Cmd(Io, 0xE6));
    case 3:
        return (BOOLEAN)(Arg(Io, 0xF8, 0x00) && Arg(Io, 0xF8, Reg) && Arg(Io, 0xF8, Val) && Cmd(Io, 0xE6));
    default:
        return (BOOLEAN)(Arg(Io, 0xF8, 0x00) && Arg(Io, 0xF8, Reg) && Arg(Io, 0xF8, 0x00) && Arg(Io, 0xF8, Val) &&
                         Cmd(Io, 0xE6));
    }
}

static BOOLEAN ElanRead(const PS2PAD *P, const PS2PAD_IO *Io, UCHAR Reg, UCHAR *Val)
{
    UCHAR r[3];

    if (!Arg(Io, 0xF8, (UCHAR)(P->Ver == 2 ? 0x10 : 0x00)) || !Arg(Io, 0xF8, Reg) || !Info(Io, r)) {
        return FALSE;
    }
    *Val = P->Ver == 4 ? r[1] : r[0];
    return TRUE;
}

static BOOLEAN ElanSignature(const UCHAR *R)
{
    static const UCHAR rates[] = { 200, 100, 80, 60, 40, 20, 10 };
    int i;

    if (R[0] == 0) return FALSE;
    if (R[1] == 0) return TRUE;
    if ((R[0] & 0x0F) >= 0x06 && (R[1] & 0xAF) == 0x0F && R[2] < 40) return TRUE;
    for (i = 0; i < (int)sizeof(rates); i++) {
        if (R[2] == rates[i]) return FALSE;
    }
    return TRUE;
}

static int Elantech(PS2PAD *P, const PS2PAD_IO *Io, PTP_OUT Out, void *Ctx)
{
    UCHAR r[3], v;
    ULONG flags = 0;
    int i;

    Cmd(Io, 0xF6);
    if (!Cmd(Io, 0xF5) || !Cmd(Io, 0xE6) || !Cmd(Io, 0xE6) || !Cmd(Io, 0xE6) || !Info(Io, r)) {
        return PS2PAD_NONE;
    }
    if (r[0] != 0x3C || r[1] != 0x03 || (r[2] != 0xC8 && r[2] != 0x00)) {
        return PS2PAD_NONE;
    }
    if (!SynQuery(Io, 0x01, r) || !ElanSignature(r)) {
        return PS2PAD_NONE;
    }
    P->Fw = Be3(r);
    P->IcVer = (UCHAR)((P->Fw >> 16) & 0x0F);
    Name(P, "Elantech");
    if (P->Fw < 0x020030 || P->Fw == 0x020600) {
        UsbFormat(P->Info, sizeof(P->Info), "Elantech v1 (firmware %06x): relative mode", P->Fw);
        return PS2PAD_NONE;
    }
    P->Ver = (UCHAR)(P->IcVer == 2 || P->IcVer == 4 ? 2 : P->IcVer == 5 ? 3 : P->IcVer >= 6 ? 4 : 0);
    if (P->Ver == 0) {
        return PS2PAD_NONE;
    }
    if (!ElanQuery(P, Io, 0x02, P->ECap) || !ElanQuery(P, Io, 0x03, P->ESamples)) {
        return PS2PAD_NONE;
    }
    if (P->ESamples[1] == 0x74 && P->Ver == 3) {
        UsbFormat(P->Info, sizeof(P->Info), "Elantech v3 with a broken absolute mode: relative mode");
        return PS2PAD_NONE;
    }
    P->Crc = (UCHAR)((P->Fw & 0x4000) != 0);
    P->Stick = (UCHAR)((P->ECap[0] & 0x80) != 0);
    P->ClickPad = (UCHAR)(P->Ver >= 3 && (P->Fw & 0x001000));
    P->MinX = P->MinY = 0;
    if (P->Ver == 2) {
        if (P->Fw == 0x020800 || P->Fw == 0x020B00 || P->Fw == 0x020030) {
            P->MaxX = 1152;
            P->MaxY = 768;
        } else {
            int k = (P->Fw > 0x020800 && P->Fw < 0x020900) ? 1 : 2;
            if (!ElanQuery(P, Io, 0x00, r)) return PS2PAD_NONE;
            if ((P->Fw >> 16) == 0x14 && (r[1] & 0x10)) {
                if (!ElanQuery(P, Io, 0x03, r)) return PS2PAD_NONE;
                P->MaxX = (P->ECap[1] - k) * r[1] / 2;
                P->MaxY = (P->ECap[2] - k) * r[2] / 2;
            } else if (P->Fw == 0x040216) {
                P->MaxX = 819; P->MaxY = 405;
            } else if (P->Fw == 0x040219 || P->Fw == 0x040215) {
                P->MaxX = 900; P->MaxY = 500;
            } else {
                P->MaxX = (P->ECap[1] - k) * 64;
                P->MaxY = (P->ECap[2] - k) * 64;
            }
        }
    } else {
        if (!ElanQuery(P, Io, 0x00, r)) return PS2PAD_NONE;
        P->MaxX = ((r[0] & 0x0F) << 8) | r[1];
        P->MaxY = ((r[0] & 0xF0) << 4) | r[2];
    }
    if (P->MaxX <= 0 || P->MaxY <= 0) {
        return PS2PAD_NONE;
    }
    /* absolute mode */
    if (P->Ver == 2) {
        if (!ElanWrite(P, Io, 0x10, 0x54) || !ElanWrite(P, Io, 0x11, 0x88) || !ElanWrite(P, Io, 0x21, 0x60)) {
            return PS2PAD_NONE;
        }
    } else if (P->Ver == 3) {
        if (!ElanWrite(P, Io, 0x10, 0x0B)) return PS2PAD_NONE;
    } else {
        if (!ElanWrite(P, Io, 0x07, 0x01)) return PS2PAD_NONE;
    }
    if (P->Ver < 4) {
        /* v2 may still be starting: it is ready when register 0x10 reads back */
        for (i = 0; i < 5 && !ElanRead(P, Io, 0x10, &v); i++) {
            (void)Io->Read(Io->Ctx, 100);
        }
    }
    UsbFormat(P->Info, sizeof(P->Info), "Elantech v%u, firmware %06x, caps %02x %02x %02x, x 0..%d y 0..%d%s%s", P->Ver,
           P->Fw, P->ECap[0], P->ECap[1], P->ECap[2], P->MaxX, P->MaxY, P->ClickPad ? ", ClickPad" : "",
           P->Stick ? ", trackpoint" : "");
    if (P->ClickPad) flags |= PTP_PAD_CLICKPAD;
    if (P->Ver == 2) flags |= PTP_PAD_SEMIMT;
    P->Proto = PS2PAD_ELANTECH;
    Name(P, P->Ver == 2 ? "Elantech v2" : P->Ver == 3 ? "Elantech v3" : "Elantech v4");
    Start(P, flags, Out, Ctx);
    return PS2PAD_ELANTECH;
}

#define EP_BAD      0
#define EP_V2       1
#define EP_V3HEAD   2
#define EP_V3TAIL   3
#define EP_STICK    4
#define EP_STATUS   5
#define EP_HEAD     6
#define EP_MOTION   7
#define EP_DEBOUNCE 8

static int ElanType(const PS2PAD *P)
{
    const UCHAR *p = P->Pkt;

    switch (P->Ver) {
    case 2:
        if (p[0] == 0x84 && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0x02 && p[4] == 0xFF && p[5] == 0xFF)
            return EP_DEBOUNCE;
        if (P->Fw >= 0x020800)
            return ((p[0] & 0x0C) == 0x04 && (p[3] & 0x0F) == 0x02) ? EP_V2 : EP_BAD;
        if ((p[0] & 0xC0) == 0x80)
            return ((p[0] & 0x0C) == 0x0C && (p[3] & 0x0E) == 0x08) ? EP_V2 : EP_BAD;
        return ((p[0] & 0x3C) == 0x3C && (p[1] & 0xF0) == 0 && (p[3] & 0x3E) == 0x38 && (p[4] & 0xF0) == 0) ?
               EP_V2 : EP_BAD;
    case 3:
        if (p[0] == 0xC4 && p[1] == 0xFF && p[2] == 0xFF && p[3] == 0x02 && p[4] == 0xFF && p[5] == 0xFF)
            return EP_DEBOUNCE;
        if (P->Crc) {
            if ((p[3] & 0x09) == 0x08) return EP_V3HEAD;
            if ((p[3] & 0x09) == 0x09) return EP_V3TAIL;
        } else {
            if ((p[0] & 0x0C) == 0x04 && (p[3] & 0xCF) == 0x02) return EP_V3HEAD;
            if ((p[0] & 0x0C) == 0x0C && (p[3] & 0xCE) == 0x0C) return EP_V3TAIL;
            if ((p[3] & 0x0F) == 0x06) return EP_STICK;
        }
        return EP_BAD;
    default: {
        BOOLEAN ok;
        if (P->Stick && (p[3] & 0x0F) == 0x06) return EP_STICK;
        if (P->Crc) ok = (BOOLEAN)((p[3] & 0x08) == 0);
        else if (P->IcVer == 7 && P->ESamples[1] == 0x2A) ok = (BOOLEAN)((p[3] & 0x1C) == 0x10);
        else ok = (BOOLEAN)((p[0] & 0x08) == 0 && (p[3] & 0x1C) == 0x10);
        if (!ok) return EP_BAD;
        switch (p[3] & 0x03) {
        case 0: return EP_STATUS;
        case 1: return EP_HEAD;
        case 2: return EP_MOTION;
        }
        return EP_BAD;
    }
    }
}

static void ElanStick(PS2PAD *P)
{
    const UCHAR *p = P->Pkt;
    ULONG t = (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24);

    switch (t & ~7UL) {
    case 0x06000030UL:
    case 0x16008020UL:
    case 0x26800010UL:
    case 0x36808000UL:
        if (p[4] == 0x80 || p[5] == 0x80 || (p[1] >> 7) == (p[4] >> 7) || (p[2] >> 7) == (p[5] >> 7)) {
            return;                                     /* misreported coordinates */
        }
        Stick(P, (UCHAR)(p[0] & 7), (LONG)p[4] - (LONG)((p[1] ^ 0x80) << 1), (LONG)((p[2] ^ 0x80) << 1) - (LONG)p[5]);
        break;
    }
}

static UCHAR ElanButtons(const PS2PAD *P)
{
    if (P->ClickPad) {
        return (UCHAR)((P->Pkt[0] & 0x03) ? 1 : 0);
    }
    return (UCHAR)(P->Pkt[0] & 0x07);
}

static void ElanV4Frame(PS2PAD *P)
{
    PTP_TOUCH t[5];
    ULONG n = 0;
    int i;

    for (i = 0; i < 5; i++) {
        if ((P->MtDown & P->MtValid) & (1 << i)) {
            t[n].Id = (ULONG)i;
            t[n].X = P->MtX[i];
            t[n].Y = P->MtY[i];
            n++;
        }
    }
    P->Packets++;
    PtpFrame(&P->Ptp, t, n > 4 ? 4 : n, ElanButtons(P));
}

/* returns FALSE for a packet that is not one (resynchronise) */
static BOOLEAN ElanPacket(PS2PAD *P)
{
    const UCHAR *p = P->Pkt;
    PTP_TOUCH t[4];
    int type = ElanType(P), fingers, id;
    LONG ymax = P->MaxY;

    switch (type) {
    case EP_BAD:
        return FALSE;
    case EP_DEBOUNCE:
        return TRUE;
    case EP_STICK:
        ElanStick(P);
        return TRUE;
    case EP_V2:
        fingers = (p[0] & 0xC0) >> 6;
        if (fingers == 3 && (p[3] & 0x80)) fingers = 4;
        if (fingers == 2) {
            t[0].Id = 0;
            t[0].X = (LONG)((((p[0] & 0x10) << 4) | p[1]) << 2);
            t[0].Y = ymax - (LONG)((((p[0] & 0x20) << 3) | p[2]) << 2);
            t[1].Id = 1;
            t[1].X = (LONG)((((p[3] & 0x10) << 4) | p[4]) << 2);
            t[1].Y = ymax - (LONG)((((p[3] & 0x20) << 3) | p[5]) << 2);
            Frame(P, t, 2, 2, ElanButtons(P));
        } else {
            t[0].Id = 0;
            t[0].X = ((p[1] & 0x0F) << 8) | p[2];
            t[0].Y = ymax - (((p[4] & 0x0F) << 8) | p[5]);
            Frame(P, t, (ULONG)fingers, 1, ElanButtons(P));
        }
        return TRUE;
    case EP_V3HEAD:
    case EP_V3TAIL:
        fingers = (p[0] & 0xC0) >> 6;
        if (fingers == 2) {
            if (type == EP_V3HEAD) {
                P->MtX[0] = ((p[1] & 0x0F) << 8) | p[2];
                P->MtY[0] = ymax - (((p[4] & 0x0F) << 8) | p[5]);
                return TRUE;                            /* the tail packet follows */
            }
            t[0].Id = 0;
            t[0].X = P->MtX[0];
            t[0].Y = P->MtY[0];
            t[1].Id = 1;
            t[1].X = ((p[1] & 0x0F) << 8) | p[2];
            t[1].Y = ymax - (((p[4] & 0x0F) << 8) | p[5]);
            Frame(P, t, 2, 2, ElanButtons(P));
        } else {
            t[0].Id = 0;
            t[0].X = ((p[1] & 0x0F) << 8) | p[2];
            t[0].Y = ymax - (((p[4] & 0x0F) << 8) | p[5]);
            Frame(P, t, (ULONG)fingers, 1, ElanButtons(P));
        }
        return TRUE;
    case EP_STATUS:
        P->MtDown = (UCHAR)(p[1] & 0x1F);
        P->MtValid &= P->MtDown;
        ElanV4Frame(P);
        return TRUE;
    case EP_HEAD:
        id = ((p[3] & 0xE0) >> 5) - 1;
        if (id < 0 || id >= 5) return TRUE;
        P->MtX[id] = ((p[1] & 0x0F) << 8) | p[2];
        P->MtY[id] = ymax - (((p[4] & 0x0F) << 8) | p[5]);
        P->MtValid |= (UCHAR)(1 << id);
        P->MtDown |= (UCHAR)(1 << id);
        ElanV4Frame(P);
        return TRUE;
    case EP_MOTION: {
        int sid = ((p[3] & 0xE0) >> 5) - 1, weight = (p[0] & 0x10) ? 5 : 1;
        id = ((p[0] & 0xE0) >> 5) - 1;
        if (id < 0 || id >= 5) return TRUE;
        P->MtX[id] += (signed char)p[1] * weight;
        P->MtY[id] -= (signed char)p[2] * weight;
        if (sid >= 0 && sid < 5) {
            P->MtX[sid] += (signed char)p[4] * weight;
            P->MtY[sid] -= (signed char)p[5] * weight;
        }
        ElanV4Frame(P);
        return TRUE;
    }
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* ALPS                                                               */
/* ------------------------------------------------------------------ */
#define ALPS_DUALPOINT  0x01
#define ALPS_BUTTONPAD  0x02

/* report requests: Init (with argument 0), the repeated command three times, E9 */
static BOOLEAN AlpsRpt(const PS2PAD_IO *Io, UCHAR Init, UCHAR Rep, UCHAR *R)
{
    if (Init && !Arg(Io, Init, 0)) return FALSE;
    return (BOOLEAN)(Cmd(Io, Rep) && Cmd(Io, Rep) && Cmd(Io, Rep) && Info(Io, R));
}

static BOOLEAN AlpsEnter(const PS2PAD_IO *Io)
{
    UCHAR r[3];

    if (!AlpsRpt(Io, 0, 0xEC, r)) return FALSE;
    return (BOOLEAN)(r[0] == 0x73 || (r[0] == 0x88 && (r[1] == 0x07 || r[1] == 0x08 || (r[1] & 0xF0) == 0xB0 ||
                                                       (r[1] & 0xF0) == 0xC0)));
}

static void AlpsExit(const PS2PAD_IO *Io)
{
    Cmd(Io, 0xEA);                                      /* stream mode */
}

/* one nibble of a register address or value (the v3 encoding) */
static BOOLEAN AlpsNibble(const PS2PAD_IO *Io, int N)
{
    static const UCHAR rate[7] = { 0x0A, 0x14, 0x28, 0x3C, 0x50, 0x64, 0xC8 };
    UCHAR r;

    switch (N) {
    case 0: return Cmd(Io, 0xF0);
    case 1: return Cmd(Io, 0xF6);
    case 2: return Cmd(Io, 0xE7);
    case 10: return Get(Io, 0xF2, &r, 1);
    case 15: return Cmd(Io, 0xE6);
    default:
        if (N >= 3 && N <= 9) return Arg(Io, 0xF3, rate[N - 3]);
        return Arg(Io, 0xE8, (UCHAR)(N - 11));
    }
}

static BOOLEAN AlpsAddr(const PS2PAD_IO *Io, int Addr)
{
    int i;

    if (!Cmd(Io, 0xEC)) return FALSE;
    for (i = 12; i >= 0; i -= 4) {
        if (!AlpsNibble(Io, (Addr >> i) & 0x0F)) return FALSE;
    }
    return TRUE;
}

static int AlpsReadReg(const PS2PAD_IO *Io, int Addr)
{
    UCHAR r[3];

    if (!AlpsAddr(Io, Addr) || !Info(Io, r) || ((r[0] << 8) | r[1]) != Addr) return -1;
    return r[2];
}

static BOOLEAN AlpsWriteCur(const PS2PAD_IO *Io, int Val)
{
    return (BOOLEAN)(AlpsNibble(Io, (Val >> 4) & 0x0F) && AlpsNibble(Io, Val & 0x0F));
}

static BOOLEAN AlpsWriteReg(const PS2PAD_IO *Io, int Addr, int Val)
{
    return (BOOLEAN)(AlpsAddr(Io, Addr) && AlpsWriteCur(Io, Val));
}

/* read a register, write it back with bits set / cleared (in command mode) */
static BOOLEAN AlpsModify(const PS2PAD_IO *Io, int Addr, int Set, int Clear)
{
    int v = AlpsReadReg(Io, Addr);
    return (BOOLEAN)(v >= 0 && AlpsWriteCur(Io, (v | Set) & ~Clear));
}

static BOOLEAN AlpsPassthrough(const PS2PAD_IO *Io, int Base, BOOLEAN On)
{
    BOOLEAN ok = (BOOLEAN)(AlpsEnter(Io) && AlpsModify(Io, Base + 8, On ? 1 : 0, On ? 0 : 1));
    AlpsExit(Io);
    return ok;
}

static BOOLEAN AlpsStickPresent(const PS2PAD_IO *Io, int Base)
{
    int v = -1;

    if (AlpsEnter(Io)) v = AlpsReadReg(Io, Base + 8);
    AlpsExit(Io);
    return (BOOLEAN)(v >= 0 && (v & 0x80));
}

/* trackstick behind a v3 pad: extended packets to the pad */
static BOOLEAN AlpsStickSetup(const PS2PAD_IO *Io, int Base)
{
    UCHAR r[3];
    BOOLEAN ok;

    if (!AlpsPassthrough(Io, Base, TRUE)) return FALSE;
    ok = (BOOLEAN)(AlpsRpt(Io, 0, 0xE7, r) && Cmd(Io, 0xE6) && Cmd(Io, 0xE6) && Cmd(Io, 0xE6) &&
                   Arg(Io, 0xF3, 0xC8) && Arg(Io, 0xF3, 0x14));
    if (!AlpsPassthrough(Io, Base, FALSE) || !ok) return FALSE;
    ok = (BOOLEAN)(AlpsEnter(Io) && AlpsModify(Io, Base + 8, 0x02, 0));
    AlpsExit(Io);
    return ok;
}

static int Alps(PS2PAD *P, const PS2PAD_IO *Io, PTP_OUT Out, void *Ctx)
{
    UCHAR e6[3], e7[3], ec[3];
    ULONG flags = PTP_PAD_SEMIMT;
    BOOLEAN ok = FALSE;
    int pass;

    Cmd(Io, 0xF6);
    for (pass = 0; pass < 2; pass++) {
        if (!AlpsRpt(Io, 0xE8, 0xE6, e6)) return PS2PAD_NONE;
        if ((e6[0] & 0xF8) != 0 || e6[1] != 0 || (e6[2] != 10 && e6[2] != 100)) return PS2PAD_NONE;
        if (!AlpsRpt(Io, 0xE8, 0xE7, e7) || !AlpsRpt(Io, 0xE8, 0xEC, ec)) return PS2PAD_NONE;
        AlpsExit(Io);
        if (pass == 0) {
            Reset(Io);                                  /* as Linux: identify again after a reset */
        }
    }
    P->Ver = 0;
    if (e7[0] == 0x73 && e7[1] == 0x03 && e7[2] == 0x50 && ec[0] == 0x73 && (ec[1] == 0x01 || ec[1] == 0x02)) {
        P->Ver = 5;
    } else if (ec[0] == 0x88 && ((ec[1] & 0xF0) == 0xB0 || (ec[1] & 0xF0) == 0xC0)) {
        P->Ver = 7;
    } else if (ec[0] == 0x88 && ec[1] == 0x08) {
        P->Ver = 0x31;
    } else if (ec[0] == 0x88 && ec[1] == 0x07 && ec[2] >= 0x90 && ec[2] <= 0x9D) {
        P->Ver = 3;
    }
    Name(P, "ALPS");
    UsbFormat(P->Info, sizeof(P->Info), "ALPS E7 %02x %02x %02x EC %02x %02x %02x: %s", e7[0], e7[1], e7[2], ec[0], ec[1],
           ec[2], P->Ver ? "native" : "relative mode (protocol not supported)");
    if (P->Ver == 0) {
        return PS2PAD_NONE;
    }
    P->MinX = P->MinY = 0;
    P->MaxX = 2000;
    P->MaxY = 1400;
    P->XBits = 15;
    P->YBits = 11;
    P->AFlags = 0;
    switch (P->Ver) {
    case 3:
        P->Byte0 = 0x8F; P->Mask0 = 0x8F;
        if (AlpsStickPresent(Io, 0x0000)) P->AFlags |= ALPS_DUALPOINT;
        if ((P->AFlags & ALPS_DUALPOINT) && !AlpsStickSetup(Io, 0x0000)) P->AFlags &= ~ALPS_DUALPOINT;
        ok = (BOOLEAN)(AlpsEnter(Io) && AlpsModify(Io, 0x0004, 0x06, 0) && AlpsModify(Io, 0x0006, 0x01, 0) &&
                       AlpsModify(Io, 0x0007, 0x01, 0) && AlpsReadReg(Io, 0x0144) >= 0 && AlpsWriteCur(Io, 0x04) &&
                       AlpsReadReg(Io, 0x0159) >= 0 && AlpsWriteCur(Io, 0x03) && AlpsReadReg(Io, 0x0163) >= 0 &&
                       AlpsWriteReg(Io, 0x0163, 0x03) && AlpsReadReg(Io, 0x0162) >= 0 &&
                       AlpsWriteReg(Io, 0x0162, 0x04));
        AlpsExit(Io);
        ok = (BOOLEAN)(ok && Arg(Io, 0xF3, 0x64));
        break;
    case 0x31:
        P->Byte0 = 0x8F; P->Mask0 = 0x8F;
        P->XBits = 16; P->YBits = 12;
        if (AlpsStickPresent(Io, 0xC2C0)) P->AFlags |= ALPS_DUALPOINT;
        if ((P->AFlags & ALPS_DUALPOINT) && !AlpsStickSetup(Io, 0xC2C0)) P->AFlags &= ~ALPS_DUALPOINT;
        ok = (BOOLEAN)(AlpsEnter(Io) && AlpsReadReg(Io, 0xC2D9) >= 0 && AlpsWriteReg(Io, 0xC2CB, 0x00) &&
                       AlpsModify(Io, 0xC2C6, 0, 0x02) && AlpsWriteReg(Io, 0xC2C9, 0x64) &&
                       AlpsModify(Io, 0xC2C4, 0x02, 0));
        AlpsExit(Io);
        break;
    case 5: {
        UCHAR r[3];
        P->Byte0 = 0xC8; P->Mask0 = 0xD8;
        ok = (BOOLEAN)(AlpsEnter(Io) && Cmd(Io, 0xEC) && Cmd(Io, 0xF0) && Cmd(Io, 0xF0) && Arg(Io, 0xF3, 0x0A) &&
                       Arg(Io, 0xF3, 0x0A) && Info(Io, r));
        AlpsExit(Io);
        if (ok) {
            P->XBits = (UCHAR)(8 + (r[2] & 0x0F));
            P->YBits = (UCHAR)(1 + ((r[2] >> 4) & 0x0F));
            P->MaxX = (P->XBits - 1) * 64;
            P->MaxY = (P->YBits - 1) * 64;
            ok = (BOOLEAN)(Arg(Io, 0xF3, 0x64) && Arg(Io, 0xF3, 0x28));
        }
        break;
    }
    case 7:
        P->Byte0 = 0x48; P->Mask0 = 0x48;
        P->MaxX = 0xFFF;
        P->MaxY = 0x7FF;
        flags = 0;
        if (ec[1] != 0xBA) P->AFlags |= ALPS_BUTTONPAD;
        if (AlpsStickPresent(Io, 0xC2C0)) P->AFlags |= ALPS_DUALPOINT;
        ok = (BOOLEAN)(AlpsEnter(Io) && AlpsReadReg(Io, 0xC2D9) >= 0 && AlpsWriteReg(Io, 0xC2C9, 0x64) &&
                       AlpsModify(Io, 0xC2C4, 0x02, 0));
        AlpsExit(Io);
        break;
    }
    if (!ok) {
        UsbFormat(P->Info, sizeof(P->Info), "ALPS v%x: absolute mode failed", P->Ver);
        return PS2PAD_NONE;
    }
    P->ClickPad = (UCHAR)((P->AFlags & ALPS_BUTTONPAD) != 0);
    P->Stick = (UCHAR)((P->AFlags & ALPS_DUALPOINT) != 0);
    if (P->ClickPad) flags |= PTP_PAD_CLICKPAD;
    P->SecondTouch = 0xFF;
    P->Proto = PS2PAD_ALPS;
    Name(P, P->Ver == 3 ? "ALPS v3 (Pinnacle)" : P->Ver == 0x31 ? "ALPS v3 (Rushmore)" : P->Ver == 5 ?
         "ALPS v5 (Dolphin)" : "ALPS v7");
    UsbFormat(P->Info, sizeof(P->Info), "%s, x 0..%d y 0..%d%s%s", P->Name, P->MaxX, P->MaxY,
           P->ClickPad ? ", button pad" : "", P->Stick ? ", trackstick" : "");
    Start(P, flags, Out, Ctx);
    return PS2PAD_ALPS;
}

typedef struct _ALPS_F {
    ULONG   XMap, YMap;
    int     Fingers, Pressure;
    LONG    StX, StY, MtX[2], MtY[2];
    UCHAR   FirstMp, IsMp, Buttons, Ts;
} ALPS_F;

static void AlpsButtonsV3(ALPS_F *F, const UCHAR *p)
{
    F->Buttons = (UCHAR)(p[3] & 0x07);
    F->Ts = (UCHAR)((p[3] >> 4) & 0x07);
}

static void AlpsDecode(const PS2PAD *P, ALPS_F *F, const UCHAR *p)
{
    if (P->Ver == 5) {
        F->FirstMp = (UCHAR)((p[0] & 0x02) != 0);
        F->IsMp = (UCHAR)((p[0] & 0x20) != 0);
        if (!F->IsMp) {
            F->StX = (p[1] & 0x7F) | ((p[4] & 0x0F) << 7);
            F->StY = (p[2] & 0x7F) | ((p[4] & 0xF0) << 3);
            F->Pressure = (p[0] & 4) ? 0 : p[5] & 0x7F;
            AlpsButtonsV3(F, p);
        } else {
            /* 35 bits of profile: y in the low YBits, x above */
            ULONG lo, hi;
            F->Fingers = ((p[0] & 0x06) >> 1) | ((p[0] & 0x10) >> 2);
            lo = (p[1] & 0x7FUL) | ((p[2] & 0x7FUL) << 7) | ((p[4] & 0x7FUL) << 14) | ((p[5] & 0x7FUL) << 21) |
                 ((p[3] & 0x07UL) << 28) | ((p[3] & 0x10UL) << 27);
            hi = ((p[3] & 0x60UL) >> 5) | ((p[0] & 0x01UL) << 2);
            F->YMap = lo & ((1UL << P->YBits) - 1);
            F->XMap = ((lo >> P->YBits) | (hi << (32 - P->YBits))) & ((1UL << P->XBits) - 1);
        }
        return;
    }
    F->FirstMp = (UCHAR)((p[4] & 0x40) != 0);
    F->IsMp = (UCHAR)(P->Ver == 0x31 ? (p[5] & 0x40) != 0 : (p[0] & 0x40) != 0);
    if (F->IsMp) {
        if (P->Ver == 0x31) {
            int a = p[5] & 3, b = (p[5] >> 2) & 3;
            F->Fingers = (a > b ? a : b) + 1;
            F->XMap = ((p[5] & 0x10UL) << 11) | ((p[4] & 0x7EUL) << 8) | ((p[1] & 0x7FUL) << 2) | ((p[0] & 0x30UL) >> 4);
            F->YMap = ((p[5] & 0x20UL) << 6) | ((p[3] & 0x70UL) << 4) | ((p[2] & 0x7FUL) << 1) | (p[4] & 0x01UL);
        } else {
            F->Fingers = (p[5] & 3) + 1;
            F->XMap = ((p[4] & 0x7EUL) << 8) | ((p[1] & 0x7FUL) << 2) | ((p[0] & 0x30UL) >> 4);
            F->YMap = ((p[3] & 0x70UL) << 4) | ((p[2] & 0x7FUL) << 1) | (p[4] & 0x01UL);
        }
    } else {
        F->StX = ((p[1] & 0x7F) << 4) | ((p[4] & 0x30) >> 2) | ((p[0] & 0x30) >> 4);
        F->StY = ((p[2] & 0x7F) << 4) | (p[4] & 0x0F);
        F->Pressure = p[5] & 0x7F;
        AlpsButtonsV3(F, p);
    }
}

typedef struct _ALPS_PT {
    int     Start, Bits;
} ALPS_PT;

static void AlpsPoints(ULONG Map, ALPS_PT *Low, ALPS_PT *High, int *Fingers)
{
    ALPS_PT *pt = Low;
    int i, bit, prev = 0;

    for (i = 0; Map != 0; i++, Map >>= 1) {
        bit = (int)(Map & 1);
        if (bit) {
            if (!prev) {
                pt->Start = i;
                pt->Bits = 0;
                (*Fingers)++;
            }
            pt->Bits++;
        } else if (prev) {
            pt = High;
        }
        prev = bit;
    }
}

/* the bounding box of the fingers from the bitmaps; the second touch is the corner opposite the first */
static int AlpsBitmap(PS2PAD *P, ALPS_F *F)
{
    ALPS_PT xl = { 0, 0 }, xh = { 0, 0 }, yl = { 0, 0 }, yh = { 0, 0 };
    LONG cx[4], cy[4];
    int fx = 0, fy = 0, i, fingers;

    if (!F->XMap || !F->YMap) return 0;
    AlpsPoints(F->XMap, &xl, &xh, &fx);
    AlpsPoints(F->YMap, &yl, &yh, &fy);
    fingers = fx > fy ? fx : fy;
    if (fx == 1) {
        i = (xl.Bits - 1) / 2;
        xl.Bits -= i;
        xh.Start = xl.Start + i;
        xh.Bits = i > 1 ? i : 1;
    }
    if (fy == 1) {
        i = (yl.Bits - 1) / 2;
        yl.Bits -= i;
        yh.Start = yl.Start + i;
        yh.Bits = i > 1 ? i : 1;
    }
#define AX(p) (P->MaxX * (2 * (p).Start + (p).Bits - 1) / (2 * (P->XBits - 1)))
#define AY(p) (P->MaxY * (2 * (p).Start + (p).Bits - 1) / (2 * (P->YBits - 1)))
    cx[0] = AX(xl); cy[0] = AY(yl);
    cx[1] = AX(xh); cy[1] = AY(yl);
    cx[2] = AX(xh); cy[2] = AY(yh);
    cx[3] = AX(xl); cy[3] = AY(yh);
#undef AX
#undef AY
    for (i = 0; i < 4; i++) {
        if (P->Ver == 5) cx[i] = P->MaxX - cx[i];       /* x bitmap reversed on v5 */
        if (P->Ver == 3) cy[i] = P->MaxY - cy[i];       /* y bitmap reversed on v3 (Pinnacle) */
    }
    if (P->SecondTouch == 0xFF) {
        LONG best = 0x7FFFFFFF;
        for (i = 0; i < 4; i++) {
            LONG dx = F->StX - cx[i], dy = F->StY - cy[i], d = dx * dx + dy * dy;
            if (d < best) {
                best = d;
                P->SecondTouch = (UCHAR)i;
            }
        }
        P->SecondTouch = (UCHAR)((P->SecondTouch + 2) % 4);
    }
    F->MtX[0] = F->StX;
    F->MtY[0] = F->StY;
    F->MtX[1] = cx[P->SecondTouch];
    F->MtY[1] = cy[P->SecondTouch];
    return fingers;
}

static void AlpsV3V5(PS2PAD *P)
{
    ALPS_F f;
    PTP_TOUCH t[4];
    int fingers = 0;

    if (P->Ver != 5 && P->Pkt[5] == 0x3F) {
        /* trackstick packet (v3) */
        const UCHAR *p = P->Pkt;
        LONG x, y;
        if (!P->Stick || !(p[0] & 0x40) || (p[1] == 0x7F && p[2] == 0x7F && p[4] == 0x7F)) return;
        x = (signed char)(((p[0] & 0x20) << 2) | (p[1] & 0x7F));
        y = (signed char)(((p[0] & 0x10) << 3) | (p[2] & 0x7F));
        /* a few models send the stick buttons here, not in the touch pad packets */
        if (p[3] & 0x07) P->AQuirk = 1;
        Stick(P, P->AQuirk ? (UCHAR)(p[3] & 0x07) : StickState(P), x / 8, -(y / 8));
        return;
    }
    UsbMemSet(&f, 0, sizeof(f));
    AlpsDecode(P, &f, P->Pkt);
    if (P->MultiPacket) {
        if (f.IsMp) {
            fingers = f.Fingers;
            AlpsDecode(P, &f, P->MultiData);           /* the position packet before the bitmap */
            if (AlpsBitmap(P, &f) == 0) fingers = 0;
        } else {
            P->MultiPacket = 0;
        }
    }
    if (f.IsMp) return;
    if (!P->MultiPacket && f.FirstMp) {
        P->MultiPacket = 1;
        UsbMemCpy(P->MultiData, P->Pkt, 6);
        return;
    }
    P->MultiPacket = 0;
    if (f.StX && f.StY && !f.Pressure) return;          /* a fluke inside a stream */
    if (fingers < 2) {
        f.MtX[0] = f.StX;
        f.MtY[0] = f.StY;
        fingers = f.Pressure > 0 ? 1 : 0;
        P->SecondTouch = 0xFF;
    }
    if (P->Stick && !P->AQuirk) StickButtons(P, f.Ts);
    t[0].Id = 0; t[0].X = f.MtX[0]; t[0].Y = f.MtY[0];
    t[1].Id = 1; t[1].X = f.MtX[1]; t[1].Y = f.MtY[1];
    Frame(P, t, (ULONG)fingers, fingers >= 2 ? 2 : 1, f.Buttons);
}

static void AlpsV7(PS2PAD *P)
{
    const UCHAR *p = P->Pkt;
    PTP_TOUCH t[4];
    LONG mx[2], my[2];
    int id, fingers, have = 0, i;
    UCHAR btn;

    if (p[0] == 0x48 && (p[4] & 0x47) == 0x06) {
        LONG x = (p[2] & 0xBF) | ((p[3] & 0x10) << 2), y = (p[3] & 0x07) | (p[4] & 0xB8) | ((p[3] & 0x20) << 1);
        if (P->Stick) Stick(P, (UCHAR)(p[1] & 7), (signed char)x, -(LONG)(signed char)y);
        return;
    }
    /* packet id */
    if (p[4] & 0x40) id = 1;                            /* one or two fingers */
    else if (p[4] & 0x01) id = 2;                       /* three or more */
    else if ((p[0] & 0x10) && !(p[4] & 0x43)) return;   /* "new": a discontinuity, ignored as Linux does */
    else if (p[1] == 0x00 && p[4] == 0x00) id = 0;      /* idle */
    else return;
    if (id == 0) {
        Frame(P, NULL, 0, 0, 0);
        return;
    }
    mx[0] = ((p[2] & 0x80) << 4) | ((p[2] & 0x3F) << 5) | ((p[3] & 0x30) >> 1) | (p[3] & 0x07);
    my[0] = (p[1] << 3) | (p[0] & 0x07);
    mx[1] = ((p[3] & 0x80) << 4) | ((p[4] & 0x80) << 3) | ((p[4] & 0x3F) << 4);
    my[1] = ((p[5] & 0x80) << 3) | ((p[5] & 0x3F) << 4);
    if (id == 1) {
        mx[1] &= ~0x000F;
        my[1] |= 0x000F;
        if (my[1] == 0x7FF && mx[1] == 0xFF0) mx[1] = 0;
    } else {
        mx[1] &= ~0x003F;
        my[1] &= ~0x0020;
        my[1] |= (p[4] & 0x02) << 4;
        my[1] |= 0x001F;
    }
    my[0] = 0x7FF - my[0];
    my[1] = 0x7FF - my[1];
    for (i = 0; i < 2; i++) {
        if (mx[i] != 0 || my[i] != 0) have++;
    }
    fingers = id == 1 ? have : 3 + (p[5] & 0x03);
    btn = (UCHAR)((p[0] & 0x80) >> 7);
    if (P->AFlags & ALPS_BUTTONPAD) {
        if (p[0] & 0x20) fingers++;
        if (p[0] & 0x10) fingers++;
    } else {
        btn |= (UCHAR)(((p[0] & 0x20) >> 4) | ((p[0] & 0x10) >> 2));
    }
    if (fingers == 1 && mx[0] == 0 && my[0] == 0) {
        mx[0] = mx[1]; my[0] = my[1];
        mx[1] = my[1] = 0;
    }
    have = 0;
    for (i = 0; i < 2; i++) {
        if (mx[i] != 0 || my[i] != 0) {
            t[have].Id = (ULONG)i;
            t[have].X = mx[i];
            t[have].Y = my[i];
            have++;
        }
    }
    if (fingers < have) fingers = have;
    Frame(P, t, (ULONG)fingers, (ULONG)have, btn);
}

/* ------------------------------------------------------------------ */
/* framing                                                            */
/* ------------------------------------------------------------------ */
static BOOLEAN AlpsBare(const PS2PAD *P)
{
    return (BOOLEAN)((P->Pkt[0] & 0xC8) == 0x08);
}

/* is byte I of the packet so far possible */
static BOOLEAN ByteOk(const PS2PAD *P, int I)
{
    UCHAR b = P->Pkt[I];

    switch (P->Proto) {
    case PS2PAD_SYNAPTICS:
        return SynValid(P, I);
    case PS2PAD_ALPS:
        if (I == 0) return (BOOLEAN)(AlpsBare(P) || (b & P->Mask0) == P->Byte0);
        if (AlpsBare(P)) return TRUE;
        if (P->Ver != 5 && P->Ver != 7) return (BOOLEAN)(!(b & 0x80));
        if (P->Ver == 7) {
            if (I == 2) return (BOOLEAN)((b & 0x40) == 0x40);
            if (I == 3) return (BOOLEAN)((b & 0x48) == 0x48);
            if (I == 5) return (BOOLEAN)((b & 0x40) == 0);
        }
        return TRUE;
    }
    return TRUE;
}

static int PacketSize(const PS2PAD *P)
{
    return (P->Proto == PS2PAD_ALPS && AlpsBare(P)) ? 3 : P->PktSize;
}

/* a complete packet; FALSE when it is not one */
static BOOLEAN Packet(PS2PAD *P)
{
    switch (P->Proto) {
    case PS2PAD_SYNAPTICS:
        SynPacket(P);
        return TRUE;
    case PS2PAD_ELANTECH:
        return ElanPacket(P);
    case PS2PAD_ALPS:
        if (AlpsBare(P)) StickPacket(P, P->Pkt[0], P->Pkt[1], P->Pkt[2]);
        else if (P->Ver == 7) AlpsV7(P);
        else AlpsV3V5(P);
        return TRUE;
    }
    return TRUE;
}

static void Drop(PS2PAD *P)
{
    int i;

    for (i = 1; i < P->Count; i++) {
        P->Pkt[i - 1] = P->Pkt[i];
    }
    P->Count--;
    P->Resyncs++;
}

void Ps2PadByte(PS2PAD *P, UCHAR B)
{
    int i;

    if (P->Proto == PS2PAD_NONE) {
        return;
    }
    if (P->Count >= sizeof(P->Pkt)) {
        P->Count = 0;
    }
    P->Pkt[P->Count++] = B;
    for (;;) {
        for (i = 0; i < P->Count; i++) {
            if (!ByteOk(P, i)) break;
        }
        if (i < P->Count) {
            Drop(P);                                    /* out of step: start one byte later */
            if (P->Count == 0) return;
            continue;
        }
        if (P->Count < PacketSize(P)) {
            return;
        }
        if (!Packet(P)) {
            Drop(P);
            if (P->Count == 0) return;
            continue;
        }
        P->Count = 0;
        return;
    }
}

void Ps2PadTick(PS2PAD *P)
{
    if (P->Proto != PS2PAD_NONE) {
        PtpTick(&P->Ptp, NULL);
    }
}

BOOLEAN Ps2PadBusy(const PS2PAD *P)
{
    return (BOOLEAN)(P->Proto != PS2PAD_NONE && PtpBusy(&P->Ptp));
}

int Ps2PadDetect(PS2PAD *P, const PS2PAD_IO *Io, PTP_OUT Out, void *Ctx)
{
    int r;

    UsbMemSet(P, 0, sizeof(*P));
    r = Synaptics(P, Io, Out, Ctx);
    if (r == PS2PAD_NONE && P->Name[0] == 0) {
        r = Alps(P, Io, Out, Ctx);
    }
    if (r == PS2PAD_NONE && P->Name[0] == 0) {
        r = Elantech(P, Io, Out, Ctx);
    }
    if (r == PS2PAD_NONE) {
        char name[sizeof(P->Name)], info[sizeof(P->Info)];
        UsbMemCpy(name, P->Name, sizeof(name));
        UsbMemCpy(info, P->Info, sizeof(info));
        UsbMemSet(P, 0, sizeof(*P));
        UsbMemCpy(P->Name, name, sizeof(name));        /* "ALPS" etc.: a pad left in relative mode */
        UsbMemCpy(P->Info, info, sizeof(info));
    }
    return r;
}

void Ps2PadStarted(PS2PAD *P, const PS2PAD_IO *Io)
{
    if (P->Proto == PS2PAD_SYNAPTICS && P->Stick) {
        /* the guest (trackpoint) on the pass-through port: data reporting on */
        if (!Sliced(Io, 0xF4) || !Arg(Io, 0xF3, 0x28)) {
            UsbFormat(P->Info, sizeof(P->Info), "pass-through port does not answer");
        }
    }
}

BOOLEAN Ps2PadEvent(UCHAR *Prev, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan, USB_MOUSE_EVENT *E)
{
    static const USHORT down[5] = { UMOU_LEFT_DOWN, UMOU_RIGHT_DOWN, UMOU_MIDDLE_DOWN, UMOU_B4_DOWN, UMOU_B5_DOWN };
    UCHAR diff = (UCHAR)((Buttons ^ *Prev) & 0x1F);
    int i;

    UsbMemSet(E, 0, sizeof(*E));
    for (i = 0; i < 5; i++) {
        if (diff & (1 << i)) {
            E->ButtonFlags |= (Buttons & (1 << i)) ? down[i] : (USHORT)(down[i] << 1);
        }
    }
    *Prev = (UCHAR)(Buttons & 0x1F);
    E->X = Dx;
    E->Y = Dy;
    E->Wheel = Wheel;
    E->Pan = Pan;
    if (Wheel) E->ButtonFlags |= UMOU_WHEEL;
    return (BOOLEAN)(E->ButtonFlags || Dx || Dy || Pan);
}
