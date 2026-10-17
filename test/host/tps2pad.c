/*
 * tps2pad.c - host test of the PS/2 touch pad protocols (src/ps2pad.c) and
 * the touch pad gesture engine (src/ptp.c): a mock Synaptics, Elantech v4 and
 * ALPS v7 pad answers the detection commands; packets are made the way
 * Documentation/input/devices/elantech.rst / alps.rst and synaptics.c
 * describe them.
 *
 *   gcc -m32 -DUSB_HARNESS -I../../src tps2pad.c ../../src/ps2pad.c ../../src/ptp.c -o /tmp/tps2pad && /tmp/tps2pad
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "ps2pad.h"

/* ---- OS stubs ---- */
static ULONG Now = 1000;
static ULONG CfgEdge = 2;
void UsbMemSet(void *D, int V, ULONG L) { memset(D, V, L); }
void UsbMemCpy(void *D, const void *S, ULONG L) { memcpy(D, S, L); }
ULONG OsTimeMs(void) { return Now; }
ULONG OsGetConfig(const char *N, ULONG D) { return strcmp(N, "TouchpadEdgeScroll") == 0 ? CfgEdge : D; }
int UsbFormat(char *B, int N, const char *F, ...) { va_list a; int r; va_start(a, F); r = vsnprintf(B, N, F, a); va_end(a); return r; }
void HidExtMouse(void *H, UCHAR B, LONG X, LONG Y, SHORT W, SHORT P) { (void)H; (void)B; (void)X; (void)Y; (void)W; (void)P; }
LONG HidGetField(const UCHAR *R, ULONG L, const HID_FIELD *F, ULONG I) { (void)R; (void)L; (void)F; (void)I; return 0; }

static int fails;
static void chk(int c, const char *what)
{
    if (!c) fails++;
    printf("%s %s\n", c ? "ok  " : "FAIL", what);
}

/* ---- events ---- */
static LONG SumX, SumY, SumW;
static UCHAR Btn, Clicks[32];
static int NClicks;

static void Out(void *Ctx, UCHAR B, LONG Dx, LONG Dy, SHORT W, SHORT P)
{
    (void)Ctx; (void)P;
    SumX += Dx; SumY += Dy; SumW += W;
    if (B != Btn && NClicks < 32) Clicks[NClicks++] = B;
    Btn = B;
}

static void Clear(void) { SumX = SumY = SumW = 0; NClicks = 0; }

/* ---- mock device ---- */
#define M_SYN   1
#define M_ELAN  2
#define M_ALPS  3
static int Kind;
static UCHAR Hist[16];
static int NHist;
static UCHAR Q[16];
static int Qh, Qt;
static int CmdMode, AddrNib, Addr, ArgFor;
static UCHAR Regs[0x10000 / 64];        /* only a few registers matter */

static void Put(UCHAR b) { Q[Qt++ % 16] = b; }
/* the bytes before the E9 that is being answered (H(1) = the one just before it) */
static int H(int back) { return NHist - 1 - back >= 0 ? Hist[(NHist - 1 - back) % 16] : -1; }

static int Nibble(UCHAR c, int arg)
{
    static const UCHAR rate[7] = { 0x0A, 0x14, 0x28, 0x3C, 0x50, 0x64, 0xC8 };
    int i;
    switch (c) {
    case 0xF0: return 0;
    case 0xF6: return 1;
    case 0xE7: return 2;
    case 0xF2: return 10;
    case 0xE6: return 15;
    case 0xE8: return 11 + arg;
    case 0xF3: for (i = 0; i < 7; i++) if (rate[i] == arg) return 3 + i;
    }
    return -1;
}

static void Info(void)
{
    /* the command bytes before E9 tell what is asked */
    int sliced = H(1) >= 0 && H(2) == 0xE8 && H(4) == 0xE8 && H(6) == 0xE8 && H(8) == 0xE8;
    int q = sliced ? (H(7) << 6) | (H(5) << 4) | (H(3) << 2) | H(1) : -1;
    int e6 = sliced && H(9) == 0xE6;
    if (Kind == M_SYN) {
        if (sliced && !e6) { Put(0x00); Put(0x47); Put(0x00); return; }     /* detect */
        if (e6) {
            switch (q) {
            case 0x00: Put(0x01); Put(0x47); Put(0x18); return;             /* 8.1 */
            case 0x01: Put(0x00); Put(0x00); Put(0x00); return;
            case 0x02: Put(0xC0); Put(0x47); Put(0x83); return;             /* extended, 4 requests; pass-through, multi finger, palm */
            case 0x03: Put(0x01); Put(0x00); Put(0xB1); return;             /* new absolute format */
            case 0x0C: Put(0x08); Put(0x00); Put(0x00); return;             /* advanced gesture mode */
            }
        }
        Put(0x00); Put(0x00); Put(0x64);
        return;
    }
    if (Kind == M_ELAN) {
        if (H(1) == 0xE6 && H(2) == 0xE6 && H(3) == 0xE6) { Put(0x3C); Put(0x03); Put(0xC8); return; }
        if (e6 && q == 0x01) { Put(0x46); Put(0x1F); Put(0x01); return; }   /* v4, button pad */
        if (H(2) == 0xF8) {
            switch (H(1)) {
            case 0x02: Put(0x00); Put(0x10); Put(0x0A); return;
            case 0x03: Put(0x00); Put(0x00); Put(0x00); return;
            case 0x00: Put(0x7B); Put(0xB8); Put(0xD0); return;             /* 3000 x 2000 */
            }
        }
        Put(0x00); Put(0x00); Put(0x64);
        return;
    }
    /* ALPS v7 */
    if (CmdMode && AddrNib == 4) {
        Put((UCHAR)(Addr >> 8)); Put((UCHAR)Addr); Put(Regs[Addr & 0x3FF]);
        return;
    }
    if (H(1) == 0xE6 && H(2) == 0xE6 && H(3) == 0xE6) { Put(0x00); Put(0x00); Put(0x0A); return; }
    if (H(1) == 0xE7 && H(2) == 0xE7 && H(3) == 0xE7) { Put(0x73); Put(0x03); Put(0x0A); return; }
    if (H(1) == 0xEC && H(2) == 0xEC && H(3) == 0xEC) { Put(0x88); Put(0xBA); Put(0x00); CmdMode = 1; AddrNib = -1; return; }
    Put(0x00); Put(0x00); Put(0x64);
}

static BOOLEAN Send(void *Ctx, UCHAR b)
{
    (void)Ctx;
    Put(0xFA);
    if (ArgFor) {                       /* argument of E8 / F3 */
        UCHAR c = (UCHAR)ArgFor;
        ArgFor = 0;
        Hist[NHist++ % 16] = b;
        if (Kind == M_ALPS && CmdMode && AddrNib >= 0) {
            int n = Nibble(c, b);
            if (AddrNib < 4) { Addr = (Addr << 4) | n; AddrNib++; }
            else if (n >= 0) { Regs[Addr & 0x3FF] = (UCHAR)((Regs[Addr & 0x3FF] << 4) | n); }
        }
        return TRUE;
    }
    Hist[NHist++ % 16] = b;
    if (b == 0xE8 || b == 0xF3) { ArgFor = b; return TRUE; }
    if (Kind == M_ALPS && CmdMode) {
        if (b == 0xEA) { CmdMode = 0; return TRUE; }
        if (b == 0xEC) { AddrNib = 0; Addr = 0; return TRUE; }
        if (b == 0xE9) { Info(); return TRUE; }
        if (AddrNib >= 0) {
            int n = Nibble(b, 0);
            if (b == 0xF2) Put(0x00);
            if (AddrNib < 4) { Addr = (Addr << 4) | n; AddrNib++; }
            else if (n >= 0) { Regs[Addr & 0x3FF] = (UCHAR)((Regs[Addr & 0x3FF] << 4) | n); }
        }
        return TRUE;
    }
    if (b == 0xFF) { Put(0xAA); Put(0x00); }
    if (b == 0xF2) Put(0x00);
    if (b == 0xE9) Info();
    return TRUE;
}

static int Read(void *Ctx, ULONG Ms)
{
    (void)Ctx; (void)Ms;
    if (Qh == Qt) return -1;
    return Q[Qh++ % 16];
}

static BOOLEAN SendAck(void *Ctx, UCHAR b)
{
    Send(Ctx, b);
    return Read(Ctx, 30) == 0xFA;
}

static PS2PAD Pad;
static PS2PAD_IO Io = { NULL, SendAck, Read };

static int Detect(int kind)
{
    Kind = kind; NHist = 0; Qh = Qt = 0; CmdMode = 0; ArgFor = 0;
    memset(Regs, 0, sizeof(Regs));
    return Ps2PadDetect(&Pad, &Io, Out, NULL);
}

static void Feed(const UCHAR *p, int n)
{
    int i;
    for (i = 0; i < n; i++) Ps2PadByte(&Pad, p[i]);
    Now += 12;
    Ps2PadTick(&Pad);
}

static void Idle(int ms)
{
    int i;
    for (i = 0; i < ms; i += 10) { Now += 10; Ps2PadTick(&Pad); }
}

/* ---- Synaptics packets (synaptics.c: synaptics_parse_hw_state) ---- */
static void Syn(int x, int y, int z, int w, int btn)
{
    UCHAR p[6];
    p[0] = (UCHAR)(0x80 | ((w & 0x0C) << 2) | ((w & 0x02) << 1) | (btn & 3));
    p[1] = (UCHAR)(((y >> 4) & 0xF0) | ((x >> 8) & 0x0F));
    p[2] = (UCHAR)z;
    p[3] = (UCHAR)(0xC0 | ((y & 0x1000) >> 7) | ((x & 0x1000) >> 8) | ((w & 1) << 2) | (btn & 1));
    p[4] = (UCHAR)x;
    p[5] = (UCHAR)y;
    Feed(p, 6);
}

static void SynAgm(int x, int y)
{
    UCHAR p[6];
    x >>= 1; y >>= 1;
    p[0] = 0x80 | 0x04;                                 /* w = 2 */
    p[1] = (UCHAR)x; p[2] = (UCHAR)y;
    p[3] = 0xC0;
    p[4] = (UCHAR)(((y >> 4) & 0xF0) | ((x >> 8) & 0x0F));
    p[5] = 0x10;                                        /* gesture packet */
    Ps2PadByte(&Pad, p[0]); Ps2PadByte(&Pad, p[1]); Ps2PadByte(&Pad, p[2]);
    Ps2PadByte(&Pad, p[3]); Ps2PadByte(&Pad, p[4]); Ps2PadByte(&Pad, p[5]);
}

static void SynStick(int b, int dx, int dy)
{
    UCHAR p[6];
    p[0] = 0x84; p[3] = 0xC4;
    p[1] = (UCHAR)(0x08 | b | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0));
    p[4] = (UCHAR)dx; p[5] = (UCHAR)dy; p[2] = 0;
    Feed(p, 6);
}

/* ---- Elantech v4 packets (elantech.rst: "hardware version 4") ---- */
static void ElanStatus(int mask, int btn)
{
    UCHAR p[6] = { 0, 0, 0, 0x10, 0, 0 };
    p[0] = (UCHAR)btn; p[1] = (UCHAR)mask;
    Feed(p, 6);
}

static void ElanHead(int id, int x, int y, int btn)
{
    UCHAR p[6];
    int yr = 2000 - y;
    p[0] = (UCHAR)(0x10 | btn);
    p[1] = (UCHAR)(0x40 | ((x >> 8) & 0x0F));
    p[2] = (UCHAR)x;
    p[3] = (UCHAR)(((id + 1) << 5) | 0x10 | 0x01);
    p[4] = (UCHAR)(0x80 | ((yr >> 8) & 0x0F));
    p[5] = (UCHAR)yr;
    Feed(p, 6);
}

/* ---- ALPS v7 packets (alps.rst: "ALPS Absolute Mode - Protocol Version 7") ---- */
static void Alps7(int n, int x0, int y0, int x1, int y1, int btn)
{
    UCHAR p[6];
    int r0 = 0x7FF - y0, r1 = 0x7FF - y1;
    if (n == 0) {
        UCHAR idle[6] = { 0x48, 0, 0x40, 0x48, 0, 0 };
        Feed(idle, 6);
        return;
    }
    if (n < 2) { x1 = 0xFF0; r1 = 0x7FF; }
    p[0] = (UCHAR)(0x48 | (btn ? 0x80 : 0) | (r0 & 7));
    p[1] = (UCHAR)(r0 >> 3);
    p[2] = (UCHAR)(0x40 | ((x0 & 0x800) >> 4) | ((x0 >> 5) & 0x3F));
    p[3] = (UCHAR)(0x48 | ((x1 & 0x800) >> 4) | ((x0 & 0x18) << 1) | (x0 & 7));
    p[4] = (UCHAR)(0x40 | ((x1 & 0x400) >> 3) | ((x1 >> 4) & 0x3F));
    p[5] = (UCHAR)(((r1 & 0x400) >> 3) | ((r1 >> 4) & 0x3F));
    Feed(p, 6);
}

static void Alps7Stick(int dx, int dy)
{
    UCHAR p[6];
    int y = -dy;
    p[0] = 0x48; p[1] = 0;
    p[2] = (UCHAR)(0x40 | (dx & 0xBF)); p[3] = (UCHAR)(0x48 | ((dx & 0x40) >> 2) | (y & 0x07) | ((y & 0x40) >> 1));
    p[4] = (UCHAR)(0x06 | (y & 0xB8)); p[5] = 0x10;
    Feed(p, 6);
}

int main(void)
{
    int i, r;

    /* ---------------- Synaptics ---------------- */
    r = Detect(M_SYN);
    chk(r == PS2PAD_SYNAPTICS && Pad.Agm && Pad.Stick && Pad.SynMode == 0xC5, "synaptics: detected, AGM, pass-through, mode C5");
    Clear();
    for (i = 0; i < 10; i++) Syn(3000 + i * 40, 3000 - i * 20, 60, 4, 0);  /* right and up on the pad (y up) */
    Syn(3400, 2800, 0, 0, 0); Idle(300);
    chk(SumX > 0 && SumY > 0 && NClicks == 0, "synaptics: one finger moves right / down, no click");
    Clear();
    Syn(3000, 3000, 60, 4, 0); Syn(3000, 3000, 60, 4, 0); Syn(3000, 3000, 0, 4, 0); Idle(200);
    chk(NClicks == 2 && Clicks[0] == 1 && Clicks[1] == 0, "synaptics: tap = left click");
    Clear();
    for (i = 0; i < 12; i++) { SynAgm(3600, 3400 - i * 60); Syn(3000, 3400 - i * 60, 60, 0, 0); }
    Syn(3000, 2700, 0, 0, 0); Idle(300);
    chk(SumW > 0 && SumX == 0 && NClicks == 0, "synaptics: two fingers down the pad = wheel up (content follows)");
    Clear();
    Syn(3000, 3000, 60, 0, 0); Syn(3000, 3000, 60, 0, 0); Syn(3000, 3000, 0, 0, 0); Idle(200);
    chk(NClicks == 2 && Clicks[0] == 2, "synaptics: two finger tap = right click");
    Clear();
    SynStick(0, 5, -3); SynStick(0, 5, -3);
    chk(SumX == 10 && SumY == 6, "synaptics: trackpoint on the pass-through port moves (y down)");
    Clear();
    SynStick(4, 0, 0); for (i = 0; i < 6; i++) SynStick(4, 0, 10); SynStick(0, 0, 0);
    chk(SumW >= 4 && SumY == 0 && NClicks == 0, "synaptics: trackpoint middle button + up = wheel, no middle click");
    Clear();
    SynStick(4, 0, 0); SynStick(0, 0, 0);
    chk(NClicks == 2 && Clicks[0] == 4, "synaptics: trackpoint middle press alone = middle click");

    /* ---------------- Elantech v4 ---------------- */
    r = Detect(M_ELAN);
    chk(r == PS2PAD_ELANTECH && Pad.Ver == 4 && Pad.ClickPad && Pad.MaxX == 3000 && Pad.MaxY == 2000,
        "elantech: v4 button pad 3000 x 2000");
    Clear();
    ElanStatus(1, 0);
    for (i = 0; i < 10; i++) ElanHead(0, 1000 + i * 30, 800 + i * 15, 0);
    ElanStatus(0, 0); Idle(300);
    chk(SumX > 0 && SumY > 0 && SumX > SumY && NClicks == 0, "elantech: one finger moves");
    Clear();
    ElanStatus(3, 0);
    for (i = 0; i < 12; i++) { ElanHead(0, 1000, 600 + i * 40, 0); ElanHead(1, 1500, 600 + i * 40, 0); }
    ElanStatus(0, 0); Idle(300);
    chk(SumW > 0 && SumX == 0, "elantech: two finger scroll = wheel");
    Clear();
    ElanStatus(3, 0); ElanHead(0, 1000, 1000, 0); ElanHead(1, 1400, 1000, 0); ElanStatus(0, 0); Idle(200);
    chk(NClicks == 2 && Clicks[0] == 2, "elantech: two finger tap = right click");
    Clear();
    ElanStatus(1, 0); ElanHead(0, 2600, 1900, 0); Idle(250); ElanHead(0, 2600, 1900, 1); ElanHead(0, 2600, 1900, 0);
    ElanStatus(0, 0); Idle(200);
    chk(NClicks == 2 && Clicks[0] == 2, "elantech: click in the bottom right corner = right button");

    /* ---------------- ALPS v7 ---------------- */
    r = Detect(M_ALPS);
    chk(r == PS2PAD_ALPS && Pad.Ver == 7 && !Pad.ClickPad && (Regs[0xC2C4 & 0x3FF] & 2) && Regs[0xC2C9 & 0x3FF] == 0x64,
        "alps: v7 found, absolute mode registers written");
    Clear();
    for (i = 0; i < 10; i++) Alps7(1, 1500 + i * 25, 900 + i * 10, 0, 0, 0);
    Alps7(0, 0, 0, 0, 0, 0); Idle(300);
    chk(SumX > 0 && SumY > 0 && NClicks == 0, "alps: one finger moves");
    Clear();
    for (i = 0; i < 12; i++) Alps7(2, 1500, 1200 - i * 40, 2200, 1200 - i * 40, 0);
    Alps7(0, 0, 0, 0, 0, 0); Idle(300);
    chk(SumW < 0 && SumX == 0, "alps: two fingers up the pad = wheel down");
    Clear();
    Alps7(1, 1500, 900, 0, 0, 0); Alps7(0, 0, 0, 0, 0, 0); Idle(200);
    chk(NClicks == 2 && Clicks[0] == 1, "alps: tap = left click");
    Clear();
    Pad.Stick = 1;
    Alps7Stick(4, -6);
    chk(SumX == 4 && SumY == -6, "alps: trackstick packet = relative motion");

    /* ---------------- engine: edge scrolling of a pad without a finger count ---------------- */
    {
        PTP_TOUCH t;
        PtpInitPad(&Pad.Ptp, 0, 1000, 0, 800, PTP_PAD_ONEFINGER, Out, NULL);
        Clear();
        t.Id = 0; t.X = 980;
        for (i = 0; i < 12; i++) { t.Y = 100 + i * 30; Now += 12; PtpFrame(&Pad.Ptp, &t, 1, 0); }
        Now += 12; PtpFrame(&Pad.Ptp, &t, 0, 0);
        chk(SumW > 0 && SumX == 0 && SumY == 0, "engine: right edge = vertical scrolling (auto on a one finger pad)");
        CfgEdge = 0;
        PtpInitPad(&Pad.Ptp, 0, 1000, 0, 800, PTP_PAD_ONEFINGER, Out, NULL);
        Clear();
        for (i = 0; i < 12; i++) { t.Y = 100 + i * 30; Now += 12; PtpFrame(&Pad.Ptp, &t, 1, 0); }
        chk(SumW == 0 && SumY > 0, "engine: TouchpadEdgeScroll=0: the edge moves the pointer");
    }
    printf("RESULT tps2pad %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
