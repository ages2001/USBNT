/*
 * tsmbtp.c - host test of the SMBus touch pad decoders (src/smbtpdec.c): the
 * PS/2 identification of Synaptics InterTouch and Elan pads against scripted
 * PS/2 devices, RMI4 register descriptors, F11 / F12 / F30 / F3A / F03 data,
 * Elan reports, PS/2 packets and the PNP id list.
 *
 *   gcc -m32 -DUSB_HARNESS -I../../src tsmbtp.c ../../src/smbtpdec.c -o /tmp/tsmbtp && /tmp/tsmbtp
 */
#include <stdio.h>
#include <string.h>
#include "smbtp.h"

void UsbMemSet(void *Dst, int Val, ULONG Len) { memset(Dst, Val, Len); }
void UsbMemCpy(void *Dst, const void *Src, ULONG Len) { memcpy(Dst, Src, Len); }
int UsbMemCmp(const void *A, const void *B, ULONG Len) { return memcmp(A, B, Len); }

static int fails;

static void chk(int c, const char *what)
{
    if (!c) fails++;
    printf("%s %s\n", c ? "ok  " : "FAIL", what);
}

/* ------------------------------------------------------------------ */
/* scripted PS/2 devices                                                */
/* ------------------------------------------------------------------ */

#define DEV_MOUSE   0
#define DEV_SYN     1
#define DEV_ELAN    2

typedef struct {
    int kind;
    unsigned char out[16];
    int nout, pos;
    int arg;                    /* the next byte is the argument of E8 / F3 */
    int slices, sliced;         /* Synaptics special command */
    int e6;                     /* E6 in a row (Elantech knock) */
    int f8;                     /* Elantech v3/v4: F8 then the command */
    int etpcmd;
    unsigned long synq[32];     /* Synaptics query results */
    unsigned long etp[8];       /* Elan sliced / F8 query results */
    int log[64], nlog;
} PS2DEV;

static void reply3(PS2DEV *d, unsigned long v)
{
    d->out[d->nout++] = (unsigned char)(v >> 16);
    d->out[d->nout++] = (unsigned char)(v >> 8);
    d->out[d->nout++] = (unsigned char)v;
}

static BOOLEAN DevSend(void *Ctx, UCHAR B)
{
    PS2DEV *d = (PS2DEV *)Ctx;

    if (d->nlog < 64) d->log[d->nlog++] = B;
    d->nout = d->pos = 0;
    if (d->arg) {                                       /* E8 argument: a slice */
        d->arg = 0;
        d->sliced = (d->sliced << 2) | (B & 3);
        d->slices++;
        return TRUE;
    }
    if (d->f8 == 1) {                                   /* the Elantech command after F8 */
        d->f8 = 2;
        d->etpcmd = B;
        return TRUE;
    }
    switch (B) {
    case 0xE8:
        d->arg = 1;
        d->e6 = 0;
        return TRUE;
    case 0xE6:
        d->slices = 0;
        d->sliced = 0;
        d->e6++;
        return TRUE;
    case 0xF8:
        d->f8 = d->kind == DEV_ELAN ? 1 : 0;
        return TRUE;
    case 0xE9:
        if (d->f8 == 2) {
            reply3(d, d->etp[d->etpcmd & 7]);
            d->f8 = 0;
        } else if (d->kind == DEV_ELAN && d->e6 == 3) {
            reply3(d, 0x3C03C8);
        } else if (d->slices == 4 && d->kind == DEV_SYN) {
            reply3(d, d->synq[d->sliced & 31]);
        } else if (d->slices == 4 && d->kind == DEV_ELAN && d->sliced == 0x01) {
            reply3(d, d->etp[1]);
        } else {
            reply3(d, 0x000264);                         /* a mouse's status: resolution 2, 100/s */
        }
        d->slices = 0;
        d->sliced = 0;
        d->e6 = 0;
        return TRUE;
    case 0xF5: case 0xF6: case 0xF4:
        d->e6 = 0;
        return TRUE;
    default:
        return TRUE;
    }
}

static int DevRead(void *Ctx, ULONG Ms)
{
    PS2DEV *d = (PS2DEV *)Ctx;
    (void)Ms;
    return d->pos < d->nout ? d->out[d->pos++] : -1;
}

static void t_ps2(void)
{
    PS2DEV d;
    SMB_PS2_IO io = { &d, DevSend, DevRead };
    SMB_PS2_INFO in;

    /* Synaptics 8.1 pad with InterTouch, click pad, trackpoint buttons (a ThinkPad) */
    memset(&d, 0, sizeof(d));
    d.kind = DEV_SYN;
    d.synq[0x00] = 0x014718;                    /* identity: minor 1, 47h, major 8 (firmware 8.1) */
    d.synq[0x01] = 0xA20B00;                    /* modes: board id, bit 1 -> 0x10 query */
    d.synq[0x10] = 0x010000;                    /* trackpoint buttons on the pad */
    d.synq[0x02] = 0xD04773;                    /* capExtended, 5 extended queries */
    d.synq[0x09] = 0x000000;
    d.synq[0x0C] = 0x104000;                    /* click pad, InterTouch */
    SmbPs2Identify(&io, &in);
    chk(in.Kind == SMB_PAD_SYNAPTICS, "ps2: Synaptics identified (E8 00 x4, E9: 47)");
    chk(in.SmbusCapable && in.ClickPad && in.StickButtons, "ps2: InterTouch, click pad, trackpoint buttons");
    chk(in.Identity == 0x014718 && in.ExtCap0c == 0x104000 && in.ExtCap10 == 0x010000,
        "ps2: queries 00, 0C, 10 through the sliced command");
    chk(in.BoardId == ((0xA0u << 6) | 0x0B), "ps2: board id from the modes query");

    /* the same pad without InterTouch */
    memset(&d, 0, sizeof(d));
    d.kind = DEV_SYN;
    d.synq[0x00] = 0x014718;
    d.synq[0x02] = 0xD04773;
    d.synq[0x0C] = 0x100000;
    SmbPs2Identify(&io, &in);
    chk(in.Kind == SMB_PAD_SYNAPTICS && !in.SmbusCapable, "ps2: Synaptics without InterTouch: PS/2");

    /* capExtended clear: no extended queries */
    memset(&d, 0, sizeof(d));
    d.kind = DEV_SYN;
    d.synq[0x00] = 0x014718;
    d.synq[0x02] = 0x504773;
    d.synq[0x0C] = 0x104000;
    SmbPs2Identify(&io, &in);
    chk(!in.SmbusCapable && in.ExtCap0c == 0, "ps2: capExtended clear: 0C not asked");

    /* Elan hardware version 4, a new IC with host notify (fw 0x5F2001 -> ic 0xF, bit 13, pattern 1) */
    memset(&d, 0, sizeof(d));
    d.kind = DEV_ELAN;
    d.etp[1] = 0x5F2001;
    d.etp[4] = 0x000004;                        /* resolution query: bus 4 (PS/2 + SMBus host notify) */
    SmbPs2Identify(&io, &in);
    chk(in.Kind == SMB_PAD_ELAN && in.HwVersion == 4 && in.FwVersion == 0x5F2001, "ps2: Elan v4 identified (knock, fw)");
    chk(in.NewIcHostNotify && in.SmbusCapable && in.Bus == 4, "ps2: Elan new IC with SMBus host notify, bus 4");
    {
        int i, f8 = 0;
        for (i = 0; i + 1 < d.nlog; i++) if (d.log[i] == 0xF8 && d.log[i + 1] == 0x04) f8 = 1;
        chk(f8, "ps2: Elan v4 queries with F8 <cmd> E9");
    }
    /* an older Elan IC that only names an alert bus */
    memset(&d, 0, sizeof(d));
    d.kind = DEV_ELAN;
    d.etp[1] = 0x461F02;
    d.etp[4] = 0x000003;
    SmbPs2Identify(&io, &in);
    chk(in.Kind == SMB_PAD_ELAN && !in.NewIcHostNotify && in.SmbusCapable && in.Bus == 3,
        "ps2: older Elan IC, SMBus alert bus: capable, not a new IC");

    /* an ordinary mouse */
    memset(&d, 0, sizeof(d));
    d.kind = DEV_MOUSE;
    SmbPs2Identify(&io, &in);
    chk(in.Kind == SMB_PAD_NONE, "ps2: ordinary mouse: neither");
}

/* ------------------------------------------------------------------ */

static void t_desc(void)
{
    /* the descriptors of the QEMU test pad: control 8 (4 bytes), data 1 (5 objects) */
    static const unsigned char cp[] = { 2, 0x00, 0x01 }, cs[] = { 4, 0x01 };
    static const unsigned char dp[] = { 2, 0x02 }, ds[] = { 40, 0x1F };
    /* a real pad's data descriptor: presence with a 16 bit structure size, registers 0, 1, 5, 9, 15;
       register 1 with 10 subpackets (continuation byte), register 5 with a 16 bit size */
    static const unsigned char rp[] = { 0, 12, 0, 0x23, 0x82 };
    static const unsigned char rs[] = { 1, 0x01,  80, 0xFF, 0x07,  0, 0x00, 0x01, 0x01,  2, 0x03,  1, 0x01 };
    RMI_REG_DESC d;

    chk(RmiDescStructSize(cp, 3) == 2 && RmiParseRegDesc(cp, 3, cs, 2, &d) == 0 && RmiDescHas(&d, 8) &&
        d.Size[8] == 4 && (d.SubMap[8] & 1) && RmiDescIndex(&d, 8) == 0, "desc: control 8, index 0, 4 bytes");
    chk(RmiParseRegDesc(dp, 2, ds, 2, &d) == 0 && d.Size[1] == 40 && d.Subpackets[1] == 5 &&
        RmiDescByteOffset(&d, 1) == 0, "desc: data 1, 5 objects");
    chk(RmiDescStructSize(rp, 5) == 12, "desc: 16 bit structure size");
    chk(RmiParseRegDesc(rp, 5, rs, sizeof(rs), &d) == 0 && RmiDescHas(&d, 0) && RmiDescHas(&d, 1) &&
        RmiDescHas(&d, 5) && RmiDescHas(&d, 9) && RmiDescHas(&d, 15) && !RmiDescHas(&d, 2),
        "desc: registers 0 1 5 9 15 present");
    chk(d.Size[1] == 80 && d.Subpackets[1] == 10 && d.Size[5] == 256 && d.Size[9] == 2,
        "desc: continuation byte (10 subpackets), 16 bit register size");
    chk(RmiDescByteOffset(&d, 1) == 1 && RmiDescIndex(&d, 9) == 3 && RmiDescByteOffset(&d, 9) == 1 + 80 + 256,
        "desc: byte offsets and register indexes");
}

static void t_rmi(void)
{
    unsigned char d1[40], f11[2 + 25], g[1];
    unsigned char map[6] = { 1, 0, 0, 0x81, 0x82, 0x83 };
    unsigned char obs[14] = { 0x01, 0xFA, 0x00, 0x00, 0x01, 0x29, 0x41, 0x55, 0x01, 0x05, 0x01, 0xFD, 0, 0 };
    unsigned char bytes[8], btn = 0, stick = 0;
    SMB_FRAME f;
    SMB_PS2_PKT pk;
    LONG dx = 0, dy = 0;
    int n, i, got = 0;

    memset(d1, 0, sizeof(d1));
    d1[0] = 1; d1[1] = 0xE8; d1[2] = 0x03; d1[3] = 0x90; d1[4] = 0x01;     /* finger at 1000, 400 */
    d1[8] = 3;                                                             /* a palm */
    d1[16] = 4; d1[17] = 0x10; d1[18] = 0x27; d1[19] = 0x00; d1[20] = 0x00; /* unclassified at 10000 (clipped), 0 */
    RmiF12Decode(d1, 40, 5, 3000, 2000, TRUE, &f);
    chk(f.N == 2 && f.C[0].Id == 0 && f.C[0].X == 1000 && f.C[0].Y == 1600 && f.C[1].Id == 2 &&
        f.C[1].X == 3000 && f.C[1].Y == 2000, "rmi: F12 objects, Y flipped, palm left out, clipped");
    RmiF12Decode(d1, 16, 5, 3000, 2000, TRUE, &f);
    chk(f.N == 1, "rmi: F12 short read: only whole objects");

    memset(f11, 0, sizeof(f11));
    f11[0] = 0x01 | (0x02 << 2) | (0x01 << 4);                  /* finger 0 present, 1 inaccurate, 2 present */
    f11[2] = 0x3E; f11[3] = 0x19; f11[4] = 0x58;                /* x 0x3E8 = 1000, y 0x195 = 405 */
    f11[2 + 10] = 0x10; f11[2 + 11] = 0x20; f11[2 + 12] = 0x00; /* finger 2: 256, 512 */
    RmiF11Decode(f11, sizeof(f11), 5, 3000, 2000, FALSE, &f);
    chk(f.N == 2 && f.C[0].X == 1000 && f.C[0].Y == 405 && f.C[1].Id == 2 && f.C[1].X == 256 && f.C[1].Y == 512,
        "rmi: F11 finger states and 12 bit positions");

    g[0] = 0xFF & ~(1 | 8 | 32);                                  /* GPIO 0, 3, 5 pressed (low) */
    chk(RmiGpioButtons(g, 6, map, &stick) == 1 && stick == (1 | 4), "rmi: GPIO buttons low = pressed, trackpoint buttons");

    n = RmiF03Bytes(obs, sizeof(obs), bytes);
    chk(n == 4 && bytes[0] == 0xFA && bytes[1] == 0x29 && bytes[2] == 0x05 && bytes[3] == 0xFD,
        "rmi: F03 output buffers: empty and parity-flagged bytes dropped");
    memset(&pk, 0, sizeof(pk));
    for (i = 0; i < n; i++) {
        LONG x, y;
        if (SmbPs2Packet(&pk, bytes[i], &x, &y, &btn)) { dx += x; dy += y; got++; }
    }
    chk(got == 1 && dx == 5 && dy == 3 && btn == 1, "rmi: trackpoint packet behind F03 (ACK skipped)");
}

static void t_elan(void)
{
    unsigned char r[32], hp[32], tp[7] = { 0x5E, 0x02, 0x80, 0x81, 0x06, 5, 0 };
    SMB_FRAME f;
    LONG dx = 0, dy = 0;
    UCHAR sb = 0;

    memset(r, 0, sizeof(r));
    r[0] = 0x5D;
    r[1] = (1 << 3) | (1 << 5) | 1;                             /* contacts 0 and 2, left button */
    r[2] = 0x30 | 0x02; r[3] = 0x20; r[4] = 0x10;               /* x 0x320 = 800, y 0x210 = 528 */
    r[7] = 0x40 | 0x04; r[8] = 0x00; r[9] = 0x00;               /* x 0x400 = 1024, y 0x400 = 1024 */
    chk(ElanDecode(r, 32, 3200, 2100, &f, &dx, &dy, &sb) == ELAN_REP_TOUCH && f.N == 2 && f.C[0].Id == 0 &&
        f.C[0].X == 800 && f.C[0].Y == 2100 - 528 && f.C[1].Id == 2 && f.C[1].X == 1024 && f.C[1].Y == 2100 - 1024 &&
        f.Buttons == 1, "elan: report 5D, packed contacts, Y flipped, buttons");
    memset(hp, 0, sizeof(hp));
    hp[0] = 0x60;
    hp[1] = 1 << 4;
    hp[2] = 0x0C; hp[3] = 0x80; hp[4] = 0x02; hp[5] = 0x00;     /* x 3200, y 512 */
    chk(ElanDecode(hp, 32, 3200, 2100, &f, &dx, &dy, &sb) == ELAN_REP_TOUCH && f.N == 1 && f.C[0].Id == 1 &&
        f.C[0].X == 3200 && f.C[0].Y == 2100 - 512, "elan: high precision report 60");
    hp[2] = 0x0D;                                               /* x 3456 > max: dropped */
    chk(ElanDecode(hp, 32, 3200, 2100, &f, &dx, &dy, &sb) == ELAN_REP_TOUCH && f.N == 0, "elan: out of range contact dropped");
    chk(ElanDecode(tp, 7, 3200, 2100, &f, &dx, &dy, &sb) == ELAN_REP_STICK && sb == 2 && dx == 5 && dy == 2,
        "elan: trackpoint report 5E");
}

static void t_ids(void)
{
    chk(SmbPnpWhitelisted("LEN0036") && SmbPnpWhitelisted("len2054") && SmbPnpWhitelisted("SYN323D") &&
        SmbPnpWhitelisted("DLL060D"), "ids: InterTouch list, case-insensitive");
    chk(!SmbPnpWhitelisted("PNP0F13") && !SmbPnpWhitelisted("LEN003") && !SmbPnpWhitelisted("LEN00366"),
        "ids: other ids and partial ids are not on it");
    chk(SmbPnpForcePad("SYN300D") && !SmbPnpForcePad("SYN3003"), "ids: ForcePads");
}

int main(void)
{
    t_ps2();
    t_desc();
    t_rmi();
    t_elan();
    t_ids();
    printf("RESULT smbtp %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
