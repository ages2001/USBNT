/* ps2.c - harness commands for PS/2 touch pads (src/ps2pad.c on the i8042, polled)
 *
 *   ps2         reset the aux device, detect the touch pad, reporting on:
 *               "PS2 proto=N name=..." (N: 0 none, 1 Synaptics, 2 Elantech, 3 ALPS)
 * Afterwards the aux bytes are decoded in the main loop; pointer events are
 * printed as MOUSE lines (main.c).
 */
#include "../src/usbnt.h"
#include "../src/usbinput.h"
#include "../src/ps2pad.h"
#include "harness.h"

static inline void outb(unsigned short p, unsigned char v) { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline unsigned char inb(unsigned short p) { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static PS2PAD Pad;
static int On;
static UCHAR Prev;
static ULONG LastTick;

static int WaitIn(void)
{
    int i;
    for (i = 0; i < 20000; i++) {
        if (!(inb(0x64) & 0x02)) return 1;
        OsStallUs(1);
    }
    return 0;
}

static void Cmd(UCHAR c) { if (WaitIn()) outb(0x64, c); }
static void Data(UCHAR d) { if (WaitIn()) outb(0x60, d); }

static int Read(void *Ctx, ULONG Ms)
{
    ULONG i;
    (void)Ctx;
    for (i = 0; i < Ms * 100; i++) {
        UCHAR st = inb(0x64);
        if (st & 0x01) {
            UCHAR b = inb(0x60);
            if (st & 0x20) return b;
            continue;                                   /* keyboard: not used here */
        }
        OsStallUs(10);
    }
    return -1;
}

static BOOLEAN Send(void *Ctx, UCHAR b)
{
    int r, t;
    for (t = 0; t < 3; t++) {
        Cmd(0xD4);
        Data(b);
        r = Read(Ctx, 30);
        if (r == 0xFA) return TRUE;
        if (r != 0xFE) return FALSE;
    }
    return FALSE;
}

static void Out(void *Ctx, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan)
{
    USB_MOUSE_EVENT e;
    (void)Ctx;
    if (Ps2PadEvent(&Prev, Buttons, Dx, Dy, Wheel, Pan, &e)) {
        OsMouseInput(&e);
    }
}

int Ps2HarnessCommand(const char *c)
{
    static const PS2PAD_IO io = { NULL, Send, Read };
    int r, i;

    if (!(c[0] == 'p' && c[1] == 's' && c[2] == '2' && c[3] == 0)) {
        return 0;
    }
    On = 0;
    Cmd(0xAD);                                          /* keyboard off */
    Cmd(0xA8);                                          /* aux on */
    for (i = 0; i < 64 && (inb(0x64) & 0x01); i++) (void)inb(0x60);
    Cmd(0x60); Data(0x44);                              /* no interrupts, translation */
    if (!Send(NULL, 0xFF)) {
        HarnessPrintf("PS2 proto=0 name=none (no aux device)\n");
        return 1;
    }
    r = Read(NULL, 800);
    if (r == 0xAA) Read(NULL, 100);
    r = Ps2PadDetect(&Pad, &io, Out, NULL);
    Send(NULL, 0xF4);
    if (r != PS2PAD_NONE) {
        Ps2PadStarted(&Pad, &io);
        On = 1;
    }
    Prev = 0;
    if (Pad.Info[0]) HarnessPrintf("ps2pad: %s\n", Pad.Info);
    HarnessPrintf("PS2 proto=%d name=%s\n", r, Pad.Name[0] ? Pad.Name : "mouse");
    return 1;
}

/* main loop */
void Ps2HarnessPoll(void)
{
    int i;
    if (!On) return;
    for (i = 0; i < 32; i++) {
        UCHAR st = inb(0x64);
        if (!(st & 0x01)) break;
        if (st & 0x20) Ps2PadByte(&Pad, inb(0x60));
        else (void)inb(0x60);
    }
    if (OsTimeMs() - LastTick >= 10) {
        LastTick = OsTimeMs();
        Ps2PadTick(&Pad);
    }
}
