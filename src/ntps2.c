/*
 * ntps2.c - PS/2 keyboard and mouse (i8042 controller) for the usbnt input
 * ports, so one driver serves every keyboard and pointing device and the
 * stock i8042prt can stay disabled.
 *
 * The i8042 is set up the way i8042prt does it: keyboard scan code set 2 with
 * the controller translating to set 1, mouse in IntelliMouse (wheel) or
 * IntelliMouse Explorer (wheel + buttons 4/5) mode when it answers the magic
 * sample rate knocks.  Bytes are read in the IRQ 1 / IRQ 12 service routines
 * and decoded in a DPC; keyboard and mouse data join the USB devices' data on
 * the same \Device\KeyboardPortN / \Device\PointerPortN, so the class drivers
 * and USER see one keyboard and one mouse however many are connected.
 * Without working interrupts the controller is polled by the usbnt timer.
 *
 * Touch pads: before the IntelliMouse knocks the mouse is asked whether it is
 * a Synaptics, ALPS or Elantech touch pad (ps2pad.c); one that is runs in its
 * native absolute mode, and the touch pad engine (ptp.c) makes pointer
 * motion, taps, two finger / edge scrolling (wheel) and ClickPad buttons of
 * its contacts; a trackpoint behind it moves the pointer, middle button +
 * motion scrolls.
 *
 * Registry (usbnt Parameters): Ps2 = 0 off, 1 on, 2 auto (default: on when
 * the i8042prt service is disabled or missing); Ps2Touchpad = 0 plain mouse,
 * 1 native touch pad protocols (default).
 */

#include "ntdrv.h"
#include "ps2pad.h"
#include "smbtp.h"

#define P_DATA          0x60
#define P_STATUS        0x64
#define P_CMD           0x64

#define ST_OBF          0x01
#define ST_IBF          0x02
#define ST_AUX          0x20
#define ST_TIMEOUT      0x40
#define ST_PARITY       0x80

#define CCB_KBD_INT     0x01
#define CCB_AUX_INT     0x02
#define CCB_KBD_OFF     0x10
#define CCB_AUX_OFF     0x20
#define CCB_XLATE       0x40

#define RING            256

typedef struct _PS2 {
    BOOLEAN     Present;            /* controller found */
    BOOLEAN     Kbd;                /* keyboard answered */
    BOOLEAN     Aux;                /* mouse answered */
    UCHAR       MouseId;            /* 0 standard, 3 wheel, 4 wheel + 5 buttons */
    UCHAR       Ccb;
    PKINTERRUPT KbdInt;
    PKINTERRUPT AuxInt;
    KSPIN_LOCK  IntLock;
    KDPC        Dpc;
    BOOLEAN     Polling;
    /* bytes from the ISRs: value | 0x100 for the mouse */
    USHORT      Ring[RING];
    volatile ULONG Head, Tail;
    /* keyboard decoder */
    UCHAR       KeyPrefix;          /* 0, 0xE0, 0xE1 */
    /* mouse decoder */
    UCHAR       Pkt[4];
    UCHAR       PktLen;
    UCHAR       Buttons;
    UCHAR       AuxReset;           /* saw 0xAA from the mouse: re-arm it */
    BOOLEAN     AuxSmbus;           /* the mouse is a touch pad driven on SMBus (smbtp.c) */
    UCHAR       Leds;
    UCHAR       Typematic;          /* PS/2 typematic byte */
    UCHAR       Native;             /* touch pad in its own protocol (Pad) */
    UCHAR       PadPrev;            /* buttons of the last pad event */
    USHORT      AuxLast2;           /* native: the last two bytes (AA 00 then silence = the pad reset itself) */
    ULONG       AuxLastMs;
    ULONG       KeyCount, MouseCount, Resyncs;
    PS2PAD      Pad;
} PS2;

static PS2 Ps;

/* ------------------------------------------------------------------ */
/* polled controller access (initialisation and synchronised sections) */
/* ------------------------------------------------------------------ */
static BOOLEAN WaitIn(void)
{
    ULONG i;
    for (i = 0; i < 20000; i++) {                       /* ~20 ms */
        if (!(READ_PORT_UCHAR((PUCHAR)P_STATUS) & ST_IBF)) return TRUE;
        KeStallExecutionProcessor(1);
    }
    return FALSE;
}

static BOOLEAN Cmd(UCHAR c)
{
    if (!WaitIn()) return FALSE;
    WRITE_PORT_UCHAR((PUCHAR)P_CMD, c);
    return TRUE;
}

static BOOLEAN Data(UCHAR d)
{
    if (!WaitIn()) return FALSE;
    WRITE_PORT_UCHAR((PUCHAR)P_DATA, d);
    return TRUE;
}

static void RingPut(USHORT v)
{
    ULONG n = (Ps.Head + 1) % RING;
    if (n != Ps.Tail) { Ps.Ring[Ps.Head] = v; Ps.Head = n; }
}

/* next byte from the device class (aux or not) within ms; other bytes go to the ring */
static int Read(BOOLEAN aux, ULONG ms)
{
    ULONG i;
    for (i = 0; i < ms * 100; i++) {
        UCHAR st = READ_PORT_UCHAR((PUCHAR)P_STATUS);
        if (st & ST_OBF) {
            UCHAR b = READ_PORT_UCHAR((PUCHAR)P_DATA);
            BOOLEAN isAux = (st & ST_AUX) != 0;
            if (isAux == aux) return b;
            RingPut((USHORT)(b | (isAux ? 0x100 : 0)));
            continue;
        }
        KeStallExecutionProcessor(10);
    }
    return -1;
}

static void Flush(void)
{
    ULONG i;
    for (i = 0; i < 64 && (READ_PORT_UCHAR((PUCHAR)P_STATUS) & ST_OBF); i++) {
        (void)READ_PORT_UCHAR((PUCHAR)P_DATA);
        KeStallExecutionProcessor(50);
    }
}

/* controller command with a result byte */
static int CmdRead(UCHAR c)
{
    ULONG i;
    if (!Cmd(c)) return -1;
    for (i = 0; i < 2000; i++) {
        if (READ_PORT_UCHAR((PUCHAR)P_STATUS) & ST_OBF) return READ_PORT_UCHAR((PUCHAR)P_DATA);
        KeStallExecutionProcessor(10);
    }
    return -1;
}

/* byte to a device, wait for its ACK (resend up to 3 times) */
static BOOLEAN Send(BOOLEAN aux, UCHAR b)
{
    int r, tries;
    for (tries = 0; tries < 3; tries++) {
        if (aux && !Cmd(0xD4)) return FALSE;
        if (!Data(b)) return FALSE;
        r = Read(aux, 30);
        if (r == 0xFA) return TRUE;
        if (r != 0xFE) return FALSE;
    }
    return FALSE;
}

static BOOLEAN SetRate(UCHAR rate)
{
    return Send(TRUE, 0xF3) && Send(TRUE, rate);
}

static int MouseId(void)
{
    if (!Send(TRUE, 0xF2)) return -1;
    return Read(TRUE, 30);
}

/* ------------------------------------------------------------------ */
/* device set-up                                                       */
/* ------------------------------------------------------------------ */
static BOOLEAN KbdInit(void)
{
    int r;
    if (!Send(FALSE, 0xFF)) {                             /* reset */
        /* some keyboards (and KVMs) do not ACK a reset: try to talk to it anyway */
        if (!Send(FALSE, 0xF5)) return FALSE;
    } else {
        r = Read(FALSE, 800);                             /* basic assurance test */
        if (r != 0xAA) UsbLog(LOG_INFO, "ps2: keyboard self test %x\n", r);
    }
    Send(FALSE, 0xED); Send(FALSE, Ps.Leds);
    Send(FALSE, 0xF3); Send(FALSE, Ps.Typematic);
    return Send(FALSE, 0xF4);                             /* enable scanning */
}

/* aux port access for the touch pad identification of smbtp.c */
static BOOLEAN IoSend(void *Ctx, UCHAR B) { (void)Ctx; return Send(TRUE, B); }
static int IoRead(void *Ctx, ULONG Ms) { (void)Ctx; return Read(TRUE, Ms); }
static const SMB_PS2_IO Ps2Io = { NULL, IoSend, IoRead };

static BOOLEAN PadSend(void *Ctx, UCHAR B) { (void)Ctx; return Send(TRUE, B); }
static int PadRead(void *Ctx, ULONG Ms) { (void)Ctx; return Read(TRUE, Ms); }
static const PS2PAD_IO PadIo = { NULL, PadSend, PadRead };

/* touch pad events (DPC) */
static void PadOut(void *Ctx, UCHAR Buttons, LONG Dx, LONG Dy, SHORT Wheel, SHORT Pan)
{
    USB_MOUSE_EVENT e;
    (void)Ctx;
    if (Ps2PadEvent(&Ps.PadPrev, Buttons, Dx, Dy, Wheel, Pan, &e)) {
        Ps.MouseCount++;
        OsMouseInput(&e);
    }
}

static BOOLEAN MouseReset(void)
{
    int r;
    if (!Send(TRUE, 0xFF)) return FALSE;
    r = Read(TRUE, 800);
    if (r == 0xAA) (void)Read(TRUE, 100);                 /* device id 00 */
    return TRUE;
}

static BOOLEAN MouseInit(void)
{
    if (!MouseReset()) return FALSE;
    Ps.Native = 0;
    /* a touch pad with an SMBus side (Synaptics InterTouch, Elan) may go to SMBus */
    if (SmbusTouchpadTakeOver(&Ps2Io)) { Ps.AuxSmbus = TRUE; return FALSE; }
    if (OsGetConfig("Ps2Touchpad", 1)) {
        int r = Ps2PadDetect(&Ps.Pad, &PadIo, PadOut, NULL);
        if (Ps.Pad.Info[0]) UsbLog(LOG_INFO, "ps2: %s\n", Ps.Pad.Info);
        if (r != PS2PAD_NONE) {
            Ps.Native = 1;
            Ps.PadPrev = 0;
            Ps.PktLen = 0;
            if (!Send(TRUE, 0xF4)) return FALSE;          /* data reporting */
            Ps2PadStarted(&Ps.Pad, &PadIo);
            UsbLog(LOG_INFO, "ps2: %s in its native mode\n", Ps.Pad.Name);
            return TRUE;
        }
        /* not a touch pad we know (or one left relative): from the start, as a mouse */
        if (Ps.Pad.Name[0]) UsbLog(LOG_INFO, "ps2: %s as a plain mouse\n", Ps.Pad.Name);
        Send(TRUE, 0xF6);
        if (!MouseReset()) return FALSE;
    }
    Send(TRUE, 0xF6);                                     /* defaults */
    Ps.MouseId = 0;
    /* IntelliMouse knock: 200, 100, 80 -> id 3 */
    if (SetRate(200) && SetRate(100) && SetRate(80) && MouseId() == 3) {
        Ps.MouseId = 3;
        /* IntelliMouse Explorer: 200, 200, 80 -> id 4 */
        if (SetRate(200) && SetRate(200) && SetRate(80) && MouseId() == 4) Ps.MouseId = 4;
    }
    SetRate(100);
    Send(TRUE, 0xE8); Send(TRUE, 0x02);                   /* 4 counts/mm */
    Send(TRUE, 0xE6);                                     /* 1:1 scaling */
    Ps.PktLen = 0;
    return Send(TRUE, 0xF4);                              /* data reporting */
}

/* ------------------------------------------------------------------ */
/* decoding (DPC)                                                      */
/* ------------------------------------------------------------------ */
static void KbdByte(UCHAR b)
{
    USB_KEY_EVENT e;
    if (b == 0xFA || b == 0xFE || b == 0x00 || b == 0xFF) return;   /* ACK, resend, overrun */
    if (b == 0xAA && !Ps.KeyPrefix) {
        /* keyboard re-plugged or reset itself: restore LEDs (best effort, async) */
        return;
    }
    if (b == 0xE0 || b == 0xE1) { Ps.KeyPrefix = b; return; }
    e.MakeCode = (USHORT)(b & 0x7F);
    e.Flags = (USHORT)((b & 0x80) ? UKEY_BREAK : UKEY_MAKE);
    if (Ps.KeyPrefix == 0xE0) e.Flags |= UKEY_E0;
    if (Ps.KeyPrefix == 0xE1) e.Flags |= UKEY_E1;
    Ps.KeyPrefix = 0;
    /* fake shifts around the extended keys (E0 2A / E0 AA / E0 36 / E0 B6) are dropped,
       as i8042prt does */
    if ((e.Flags & UKEY_E0) && (e.MakeCode == 0x2A || e.MakeCode == 0x36)) return;
    Ps.KeyCount++;
    OsKbdInput(&e, 1);
}

static void MouseByte(UCHAR b)
{
    USB_MOUSE_EVENT e;
    UCHAR need = Ps.MouseId ? 4 : 3, n, chg;
    LONG dx, dy;

    if (Ps.AuxSmbus) return;                              /* the pad reports on SMBus */

    if (Ps.Native) {
        Ps.AuxLast2 = (USHORT)((Ps.AuxLast2 << 8) | b);
        Ps.AuxLastMs = OsTimeMs();
        Ps2PadByte(&Ps.Pad, b);
        return;
    }
    if (Ps.PktLen == 0) {
        if (b == 0xAA) { Ps.AuxReset = 1; return; }       /* hot-plugged mouse: re-arm later */
        if (Ps.AuxReset && b == 0x00) return;
        if (!(b & 0x08)) { Ps.Resyncs++; return; }        /* bit 3 is always set in byte 0 */
    }
    Ps.Pkt[Ps.PktLen++] = b;
    if (Ps.PktLen < need) return;
    Ps.PktLen = 0;
    if (Ps.Pkt[0] & 0xC0) return;                          /* overflow: drop the packet */
    dx = Ps.Pkt[1] - ((Ps.Pkt[0] & 0x10) ? 256 : 0);
    dy = Ps.Pkt[2] - ((Ps.Pkt[0] & 0x20) ? 256 : 0);
    RtlZeroMemory(&e, sizeof(e));
    e.X = dx;
    e.Y = -dy;                                             /* PS/2: up is positive */
    n = (UCHAR)(Ps.Pkt[0] & 0x07);
    if (Ps.MouseId == 4) {
        n |= (UCHAR)((Ps.Pkt[3] & 0x30) >> 1);             /* buttons 4, 5 -> bits 3, 4 */
        e.Wheel = (SHORT)(-((Ps.Pkt[3] & 0x08) ? (LONG)(Ps.Pkt[3] & 0x0F) - 16 : (LONG)(Ps.Pkt[3] & 0x0F)));
    } else if (Ps.MouseId == 3) {
        e.Wheel = (SHORT)(-(LONG)(signed char)Ps.Pkt[3]);
    }
    chg = (UCHAR)(n ^ Ps.Buttons);
    if (chg & 0x01) e.ButtonFlags |= (n & 0x01) ? UMOU_LEFT_DOWN : UMOU_LEFT_UP;
    if (chg & 0x02) e.ButtonFlags |= (n & 0x02) ? UMOU_RIGHT_DOWN : UMOU_RIGHT_UP;
    if (chg & 0x04) e.ButtonFlags |= (n & 0x04) ? UMOU_MIDDLE_DOWN : UMOU_MIDDLE_UP;
    if (chg & 0x08) e.ButtonFlags |= (n & 0x08) ? UMOU_B4_DOWN : UMOU_B4_UP;
    if (chg & 0x10) e.ButtonFlags |= (n & 0x10) ? UMOU_B5_DOWN : UMOU_B5_UP;
    if (e.Wheel) e.ButtonFlags |= UMOU_WHEEL;
    Ps.Buttons = n;
    Ps.MouseCount++;
    OsMouseInput(&e);
}

static VOID NTAPI Ps2Dpc(PKDPC Dpc, PVOID Ctx, PVOID A1, PVOID A2)
{
    (void)Dpc; (void)Ctx; (void)A1; (void)A2;
    while (Ps.Tail != Ps.Head) {
        USHORT v = Ps.Ring[Ps.Tail];
        Ps.Tail = (Ps.Tail + 1) % RING;
        if (v & 0x100) MouseByte((UCHAR)v);
        else KbdByte((UCHAR)v);
    }
    if (Ps.Native && Ps2PadBusy(&Ps.Pad)) {
        Ps2PadTick(&Ps.Pad);                                /* the end of a tap click */
    }
}

/* ------------------------------------------------------------------ */
/* interrupts                                                          */
/* ------------------------------------------------------------------ */
static BOOLEAN NTAPI Ps2Isr(PKINTERRUPT Int, PVOID Ctx)
{
    BOOLEAN mine = FALSE;
    ULONG i;
    (void)Int; (void)Ctx;
    for (i = 0; i < 16; i++) {
        UCHAR st = READ_PORT_UCHAR((PUCHAR)P_STATUS), b;
        if (!(st & ST_OBF)) break;
        b = READ_PORT_UCHAR((PUCHAR)P_DATA);
        RingPut((USHORT)(b | ((st & ST_AUX) ? 0x100 : 0)));
        mine = TRUE;
    }
    if (mine) KeInsertQueueDpc(&Ps.Dpc, NULL, NULL);
    return mine;
}

/* timer fallback (DISPATCH_LEVEL) when the interrupts could not be connected */
void NtPs2Poll(void)
{
    ULONG i;
    if (Ps.Present && Ps.Native && Ps2PadBusy(&Ps.Pad)) {
        KeInsertQueueDpc(&Ps.Dpc, NULL, NULL);              /* tap click to end */
    }
    if (!Ps.Present || !Ps.Polling) return;
    for (i = 0; i < 16; i++) {
        UCHAR st = READ_PORT_UCHAR((PUCHAR)P_STATUS), b;
        if (!(st & ST_OBF)) break;
        b = READ_PORT_UCHAR((PUCHAR)P_DATA);
        RingPut((USHORT)(b | ((st & ST_AUX) ? 0x100 : 0)));
    }
    if (Ps.Tail != Ps.Head) KeInsertQueueDpc(&Ps.Dpc, NULL, NULL);
}

static BOOLEAN ConnectIrq(ULONG irq, PKINTERRUPT *out, KIRQL syncIrql)
{
    KIRQL irql;
    KAFFINITY aff;
    ULONG vec = HalGetInterruptVector(Isa, 0, irq, irq, &irql, &aff);
    NTSTATUS st;
    if (!vec) return FALSE;
    st = IoConnectInterrupt(out, Ps2Isr, NULL, &Ps.IntLock, vec, irql, syncIrql, Latched, FALSE, aff, FALSE);
    if (!NT_SUCCESS(st)) {
        UsbLog(LOG_ERR, "ps2: IRQ %u not available (%x)\n", irq, st);
        *out = NULL;
        return FALSE;
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* LEDs / typematic from the keyboard port IOCTLs                      */
/* ------------------------------------------------------------------ */
typedef struct _SYNC_CMD { UCHAR B[2]; } SYNC_CMD;

static BOOLEAN NTAPI SyncSend(PVOID Ctx)
{
    SYNC_CMD *c = (SYNC_CMD *)Ctx;
    if (Send(FALSE, c->B[0])) Send(FALSE, c->B[1]);
    return TRUE;
}

static void KbdCommand(UCHAR a, UCHAR b)
{
    SYNC_CMD c;
    if (!Ps.Present || !Ps.Kbd) return;
    c.B[0] = a; c.B[1] = b;
    if (Ps.KbdInt != NULL) KeSynchronizeExecution(Ps.KbdInt, SyncSend, &c);
    else SyncSend(&c);
    if (Ps.Tail != Ps.Head) KeInsertQueueDpc(&Ps.Dpc, NULL, NULL);
}

void NtPs2SetLeds(ULONG NtIndicators)
{
    /* NT KEYBOARD_*_LOCK_ON bits are the PS/2 LED bits: scroll 1, num 2, caps 4 */
    Ps.Leds = (UCHAR)(NtIndicators & 7);
    KbdCommand(0xED, Ps.Leds);
}

static UCHAR TypematicByte(ULONG DelayMs, ULONG RateCps)
{
    /* PS/2 rate table: 30.0 .. 2.0 chars/s for codes 0 .. 31 */
    static const USHORT rate10[32] = { 300, 267, 240, 218, 200, 185, 171, 160, 150, 133, 120, 109, 100, 92, 86,
                                       80, 75, 67, 60, 55, 50, 46, 43, 40, 37, 33, 30, 27, 25, 23, 21, 20 };
    UCHAR d = (UCHAR)(DelayMs <= 250 ? 0 : DelayMs <= 500 ? 1 : DelayMs <= 750 ? 2 : 3), r = 0;
    ULONG best = 0xFFFF, i;
    for (i = 0; i < 32; i++) {
        ULONG diff = rate10[i] > RateCps * 10 ? rate10[i] - RateCps * 10 : RateCps * 10 - rate10[i];
        if (diff < best) { best = diff; r = (UCHAR)i; }
    }
    return (UCHAR)((d << 5) | r);
}

void NtPs2SetTypematic(ULONG DelayMs, ULONG RateCps)
{
    Ps.Typematic = TypematicByte(DelayMs, RateCps);
    KbdCommand(0xF3, Ps.Typematic);
}

BOOLEAN NtPs2MouseWheel(void) { return Ps.Present && Ps.Aux && (Ps.MouseId >= 3 || Ps.Native); }
ULONG NtPs2MouseButtons(void) { return (Ps.Present && Ps.Aux) ? (Ps.MouseId == 4 ? 5 : 3) : 0; }
BOOLEAN NtPs2Keyboard(void) { return Ps.Present && Ps.Kbd; }
BOOLEAN NtPs2Mouse(void) { return Ps.Present && Ps.Aux; }

/* ------------------------------------------------------------------ */
/* start-up                                                            */
/* ------------------------------------------------------------------ */
static BOOLEAN I8042prtDisabled(void)
{
    ULONG start = 0xFFFFFFFF;
    if (!NtQueryDword(L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\i8042prt", L"Start", &start))
        return TRUE;                                      /* no i8042prt at all */
    return start == 4;
}

/* whether usbnt drives the keyboard controller (registry Ps2: 0 never, 1 always,
   2 = when i8042prt is disabled) */
BOOLEAN NtPs2Wanted(void)
{
    ULONG mode = OsGetConfig("Ps2", 2);
    return (BOOLEAN)(mode == 1 || (mode == 2 && I8042prtDisabled()));
}

void NtPs2Init(void)
{
    ULONG mode = OsGetConfig("Ps2", 2);
    int ccb, t;
    KIRQL sync;

    RtlZeroMemory(&Ps, sizeof(Ps));
    if (mode == 0 || (mode == 2 && !I8042prtDisabled())) {
        UsbLog(LOG_INFO, "ps2: left to i8042prt\n");
        return;
    }
    if (READ_PORT_UCHAR((PUCHAR)P_STATUS) == 0xFF) {
        UsbLog(LOG_INFO, "ps2: no keyboard controller\n");
        return;
    }
    KeInitializeSpinLock(&Ps.IntLock);
    KeInitializeDpc(&Ps.Dpc, Ps2Dpc, NULL);
    Ps.Typematic = TypematicByte(500, 30);
    /* quiet the controller, take its configuration */
    Cmd(0xAD);
    Cmd(0xA7);
    Flush();
    ccb = CmdRead(0x20);
    if (ccb < 0) {
        UsbLog(LOG_ERR, "ps2: controller does not answer\n");
        return;
    }
    Ps.Present = TRUE;
    Ps.Ccb = (UCHAR)((ccb & ~(CCB_KBD_INT | CCB_AUX_INT)) | CCB_XLATE);
    Cmd(0x60); Data(Ps.Ccb);
    /* keyboard */
    Cmd(0xAE);
    Ps.Kbd = KbdInit();
    /* mouse: the aux port test is not reliable everywhere, the mouse itself decides */
    t = CmdRead(0xA9);
    Cmd(0xA8);
    Ps.Aux = MouseInit();
    if (!Ps.Aux && !Ps.AuxSmbus) Cmd(0xA7);
    UsbLog(LOG_INFO, "ps2: controller ccb %02x, keyboard %s, mouse %s (aux test %d, id %u%s%s)\n", ccb,
           Ps.Kbd ? "yes" : "no", Ps.Aux ? "yes" : "no", t, Ps.MouseId, Ps.Native ? ", touch pad " : "",
           Ps.Native ? Ps.Pad.Name : "");
    Flush();
    Ps.Head = Ps.Tail = 0;
    if (!Ps.Kbd && !Ps.Aux) {
        Ps.Present = FALSE;
        return;
    }
    /* interrupts on */
    sync = 0;
    {
        KIRQL i1, i12;
        KAFFINITY a;
        HalGetInterruptVector(Isa, 0, 1, 1, &i1, &a);
        HalGetInterruptVector(Isa, 0, 12, 12, &i12, &a);
        sync = i1 > i12 ? i1 : i12;
    }
    if (Ps.Kbd) ConnectIrq(1, &Ps.KbdInt, sync);
    if (Ps.Aux) ConnectIrq(12, &Ps.AuxInt, sync);
    Ps.Polling = (Ps.Kbd && Ps.KbdInt == NULL) || (Ps.Aux && Ps.AuxInt == NULL);
    Ps.Ccb = (UCHAR)(Ps.Ccb & ~(CCB_KBD_OFF | CCB_AUX_OFF));
    if (!Ps.Aux && !Ps.AuxSmbus) Ps.Ccb |= CCB_AUX_OFF;
    if (Ps.Kbd && Ps.KbdInt) Ps.Ccb |= CCB_KBD_INT;
    if (Ps.Aux && Ps.AuxInt) Ps.Ccb |= CCB_AUX_INT;
    Cmd(0x60); Data(Ps.Ccb);
    if (Ps.Polling) UsbLog(LOG_INFO, "ps2: polled mode\n");
}

/* a mouse that announced itself (AA 00) is in default mode with reporting off:
   knock the wheel mode back on and enable it, no reset (that would take ~0.5 s) */
static BOOLEAN NTAPI MouseRearm(PVOID Ctx)
{
    (void)Ctx;
    Ps.MouseId = 0;
    if (Ps.Native) {
        /* a touch pad that started again: native mode again (it is back in relative mode) */
        if (Ps2PadDetect(&Ps.Pad, &PadIo, PadOut, NULL) != PS2PAD_NONE && Send(TRUE, 0xF4)) {
            Ps2PadStarted(&Ps.Pad, &PadIo);
            Ps.PadPrev = 0;
            return TRUE;
        }
        Ps.Native = 0;
        MouseReset();
        Send(TRUE, 0xF6);
    }
    if (SetRate(200) && SetRate(100) && SetRate(80) && MouseId() == 3) {
        Ps.MouseId = 3;
        if (SetRate(200) && SetRate(200) && SetRate(80) && MouseId() == 4) Ps.MouseId = 4;
    }
    SetRate(100);
    Ps.PktLen = 0;
    Ps.Buttons = 0;
    Send(TRUE, 0xF4);
    return TRUE;
}

/* service from the usbnt worker (PASSIVE_LEVEL): mouse re-plug */
void NtPs2Service(void)
{
    if (Ps.Present && Ps.Native && Ps.AuxLast2 == 0xAA00 && OsTimeMs() - Ps.AuxLastMs > 300) {
        Ps.AuxLast2 = 0;
        Ps.AuxReset = 1;                                    /* the touch pad started again by itself */
    }
    if (!Ps.Present || !Ps.AuxReset) return;
    Ps.AuxReset = 0;
    Ps.Aux = TRUE;
    if (Ps.AuxInt != NULL) KeSynchronizeExecution(Ps.AuxInt, MouseRearm, NULL);
    else MouseRearm(NULL);
    if (Ps.Tail != Ps.Head) KeInsertQueueDpc(&Ps.Dpc, NULL, NULL);
    UsbLog(LOG_INFO, "ps2: mouse re-armed (id %u)\n", Ps.MouseId);
}
