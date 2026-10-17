/* smbhar.c - harness commands for the SMBus touch pads
 *
 *   smbus      one line per SMBus host controller and touch pad
 *
 * HarnessPs2=1 (-append): before the first worker pass the PS/2 mouse is reset
 * through the i8042 (polled) and given to SmbusTouchpadTakeOver, as ntps2.c does.
 * HarnessPs2=2 / 3: instead of the i8042, a scripted PS/2 side of a Synaptics
 * InterTouch pad (firmware 8.1, click pad) / an Elan v4 pad (new IC, SMBus host
 * notify) answers the identification - QEMU has no such PS/2 device.
 */
#include "../src/smbtp.h"
#include "harness.h"

#define P_DATA      0x60
#define P_STATUS    0x64

static BOOLEAN WaitIn(void)
{
    ULONG i;
    for (i = 0; i < 20000; i++) {
        if (!(OsIn8(P_STATUS) & 2)) return TRUE;
        OsStallUs(1);
    }
    return FALSE;
}

static int AuxRead(void *Ctx, ULONG Ms)
{
    ULONG i;
    (void)Ctx;
    for (i = 0; i < Ms * 100; i++) {
        UCHAR st = OsIn8(P_STATUS);
        if (st & 1) {
            UCHAR b = OsIn8(P_DATA);
            if (st & 0x20) return b;
            continue;                                   /* keyboard byte */
        }
        OsStallUs(10);
    }
    return -1;
}

static BOOLEAN AuxSend(void *Ctx, UCHAR B)
{
    int r, t;
    for (t = 0; t < 3; t++) {
        if (!WaitIn()) return FALSE;
        OsOut8(P_STATUS, 0xD4);
        if (!WaitIn()) return FALSE;
        OsOut8(P_DATA, B);
        r = AuxRead(Ctx, 30);
        if (r == 0xFA) return TRUE;
        if (r != 0xFE) return FALSE;
    }
    return FALSE;
}

/* scripted PS/2 pad: E8 slices + E9 answer the Synaptics queries, E6 E6 E6 E9 and F8 <cmd> E9 the Elan ones */
static struct {
    int Elan, Arg, Slices, Sliced, E6, F8, Cmd, Pos, N, F5;
    UCHAR Out[3];
} Fk;

static ULONG FakeQuery(int Cmd)
{
    if (!Fk.Elan) {
        switch (Cmd) {
        case 0x00: return 0x014718;                     /* identity: 8.1 */
        case 0x01: return 0xA20B00;                     /* modes: board id, the 0x10 query exists */
        case 0x02: return 0xD04773;                     /* capabilities: extended, 5 queries */
        case 0x0C: return 0x104000;                     /* click pad, InterTouch */
        default: return 0;
        }
    }
    return Cmd == 0x01 ? 0x5F2001 : Cmd == 0x04 ? 0x000004 : 0;
}

static BOOLEAN FakeSend(void *Ctx, UCHAR B)
{
    ULONG v = 0x000264;
    (void)Ctx;
    Fk.Pos = Fk.N = 0;
    if (Fk.Arg) {
        Fk.Arg = 0;
        Fk.Sliced = (Fk.Sliced << 2) | (B & 3);
        Fk.Slices++;
        return TRUE;
    }
    if (Fk.F8 == 1) {
        Fk.F8 = 2;
        Fk.Cmd = B;
        return TRUE;
    }
    if (B == 0xE8) { Fk.Arg = 1; Fk.E6 = 0; return TRUE; }
    if (B == 0xE6) { Fk.Slices = Fk.Sliced = 0; Fk.E6++; return TRUE; }
    if (B == 0xF8) { Fk.F8 = Fk.Elan; return TRUE; }
    if (B == 0xF5) { Fk.F5++; HarnessPrintf("PS2 fake: F5 (data reporting off)\n"); }
    if (B != 0xE9) { Fk.E6 = 0; return TRUE; }
    if (Fk.F8 == 2) v = FakeQuery(Fk.Cmd);
    else if (Fk.Elan && Fk.E6 == 3) v = 0x3C03C8;
    else if (Fk.Slices == 4 && (!Fk.Elan || Fk.Sliced == 0x01)) v = FakeQuery(Fk.Sliced);
    Fk.F8 = Fk.Slices = Fk.Sliced = Fk.E6 = 0;
    Fk.Out[0] = (UCHAR)(v >> 16); Fk.Out[1] = (UCHAR)(v >> 8); Fk.Out[2] = (UCHAR)v;
    Fk.N = 3;
    return TRUE;
}

static int FakeRead(void *Ctx, ULONG Ms)
{
    (void)Ctx; (void)Ms;
    return Fk.Pos < Fk.N ? Fk.Out[Fk.Pos++] : -1;
}

void HarnessPs2Init(void)
{
    static const SMB_PS2_IO io = { NULL, AuxSend, AuxRead };
    int ccb, r;
    ULONG i;

    if (OsGetConfig("HarnessPs2", 0) >= 2) {
        static const SMB_PS2_IO fake = { NULL, FakeSend, FakeRead };
        Fk.Elan = OsGetConfig("HarnessPs2", 0) == 3;
        HarnessPrintf("PS2 fake %s pad, takeover=%u\n", Fk.Elan ? "Elan" : "Synaptics", SmbusTouchpadTakeOver(&fake));
        return;
    }
    WaitIn();
    OsOut8(P_STATUS, 0x20);                             /* controller configuration: no interrupts */
    for (i = 0; i < 1000 && !(OsIn8(P_STATUS) & 1); i++) OsStallUs(10);
    ccb = (OsIn8(P_STATUS) & 1) ? OsIn8(P_DATA) : 0x47;
    WaitIn();
    OsOut8(P_STATUS, 0x60);
    WaitIn();
    OsOut8(P_DATA, (UCHAR)(ccb & ~3));
    WaitIn();
    OsOut8(P_STATUS, 0xA8);                             /* aux port on */
    while (OsIn8(P_STATUS) & 1) (void)OsIn8(P_DATA);
    if (!AuxSend(NULL, 0xFF)) {
        HarnessPrintf("PS2 no mouse\n");
        return;
    }
    r = AuxRead(NULL, 800);
    if (r == 0xAA) (void)AuxRead(NULL, 100);
    HarnessPrintf("PS2 reset %02x, takeover=%u\n", r & 0xFF, SmbusTouchpadTakeOver(&io));
}

int SmbusHarnessCommand(const char *c)
{
    char line[200];
    int i;

    if (!(c[0] == 's' && c[1] == 'm' && c[2] == 'b' && c[3] == 'u' && c[4] == 's' && (c[5] == 0 || c[5] == ' '))) {
        return 0;
    }
    for (i = 0; SmbusDescribe(i, line, sizeof(line)) > 0; i++) {
        HarnessPrintf("%s\n", line);
    }
    HarnessPrintf("SMBUS %d pads\n", SmbTpCount);
    return 1;
}
