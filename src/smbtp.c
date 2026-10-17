/*
 * smbtp.c - touch pads on SMBus: Synaptics RMI4 "InterTouch" (address 2Ch)
 * and Elan (15h), as Linux drives them (drivers/input/rmi4/rmi_smbus.c,
 * rmi_driver.c, rmi_f01/f03/f11/f12/f30/f3a.c, drivers/input/mouse/
 * elan_i2c_smbus.c, psmouse-smbus.c).
 *
 * Which pads: such a pad is first a PS/2 mouse.  When the PS/2 driver has
 * reset it, it calls SmbusTouchpadTakeOver(): the pad is identified (Synaptics
 * capability bit "InterTouch", Elan "SMBus / host notify"); registry Smbus = 1
 * (default) takes, as Linux does, the Synaptics pads whose ACPI PS/2 mouse
 * device has an id of Linux's list (smbus_pnp_ids) and the Elan pads of the
 * new ICs, Smbus = 2 every pad that says it has an SMBus side, 0 none.  The
 * PS/2 side is then left disabled (Synaptics: F5) and the pad is set up on
 * SMBus.  Where the PS/2 port is not usbnt's (Windows 2000 / XP: i8042prt,
 * NT with i8042prt kept), a pad that already answers on SMBus is used:
 * Synaptics with a listed id (Smbus = 1), every one (Smbus = 2).
 * SmbusTouchpad (registry) names addresses to use without any of this.
 *
 * NT has no SMBus Host Notify interrupt: the pads are polled - RMI4: the
 * interrupt status of F01, then the data of the functions it names; Elan:
 * the report (command A8h) - every 8 ms while fingers are on the pad, every
 * 30 ms when it is idle.  Transfers are started and collected on the next
 * pass, so the worker does not wait for the bus.
 *
 * Reports: the contacts go to the touch pad gestures of ptp.c (pointer, two
 * finger scrolling, taps, the pad's click) like a precision touch pad on I2C;
 * separate buttons and the trackpoint (Synaptics: PS/2 behind F03, Elan:
 * trackpoint reports) are a second pointer.
 */

#include "usbinput.h"
#include "acpi.h"
#include "ptp.h"
#include "smbtp.h"

#define MAX_PAD             2

#define RMI_ADDR            0x2C
#define ELAN_ADDR           0x15

typedef struct _RMI_FN {
    UCHAR   Num;
    UCHAR   IrqCount;
    UCHAR   IrqPos;
    UCHAR   Ver;
    USHORT  Query, Cmd, Ctrl, Data;
} RMI_FN;

typedef struct _RMI_MAP {
    USHORT  Addr;
    UCHAR   Count;
    UCHAR   Flags;                  /* 1: write entry */
} RMI_MAP;

#define JOB_2D      0
#define JOB_GPIO    1
#define JOB_F03     2
#define JOBS        3

typedef struct _SMB_PAD {
    SMB_HOST   *H;
    UCHAR       Port, Addr, Kind;
    UCHAR       Dead;
    UCHAR       ClickPad;           /* the pad's click is the PTP button */
    UCHAR       StickBtns;          /* Synaptics: trackpoint buttons on GPIOs 3-5 */
    UCHAR       Step;               /* polling: 0 idle, 1 irq status, 2 + job */
    UCHAR       Job;
    LONG        MaxX, MaxY;
    char        Name[48];
    char        Info[64];
    /* output */
    void       *Mouse;              /* the pad (gestures) */
    void       *Stick;              /* separate buttons, trackpoint */
    PTP_STATE  *Ptp;
    USHORT      PrevIds;
    LONG        LastX[16], LastY[16];
    UCHAR       PadButton, Buttons, StickButtons, StickReported;
    UCHAR       LastPadButton;
    SMB_FRAME   Fr;
    /* RMI4 */
    RMI_MAP     Map[8];
    UCHAR       MapNext;
    UCHAR       SmbVersion;
    UCHAR       IrqRegs;
    UCHAR       Is2D;               /* 11 or 12 */
    UCHAR       Objects;
    UCHAR       HasGpio, HasF03;
    RMI_FN      F01, F2D, Gpio, F03;
    ULONG       IrqMask, IrqF01, Irq2D, IrqGpio, IrqF03;
    USHORT      DataLen, Data1Off;
    UCHAR       GpioCount, GpioRegs, GpioIsF3a;
    UCHAR       GpioMap[16];
    UCHAR       F03Len;
    SMB_PS2_PKT Pkt;
    UCHAR       StickPs2Buttons;
    UCHAR       Pending;            /* bits of the jobs of this poll */
    UCHAR       Got;                /* bits of the jobs that brought data */
    UCHAR       JobBuf[JOBS][SMB_BLOCK_MAX];
    UCHAR       JobLen[JOBS];
    ULONG       IrqBits;
    /* Elan */
    UCHAR       ElanClick;
    /* polling */
    UCHAR       Buf[SMB_BLOCK_MAX];
    ULONG       NextPoll, LastActive, Errors, Reports, RetryAt, Frames;
} SMB_PAD;

int SmbTpCount;
static SMB_PAD *Pads[MAX_PAD];
static BOOLEAN Inited, Ps2Seen, DirectDone;
static ULONG Mode;

static ULONG GetMode(void)
{
    return OsGetConfig("Smbus", 1);
}

/* ------------------------------------------------------------------ */
/* the PS/2 mouse device in ACPI: its ids (_HID, _CID)                  */
/* ------------------------------------------------------------------ */

#define MAX_IDS     8
static char AcpiIds[MAX_IDS][9];
static int NAcpiIds;
static BOOLEAN AcpiRead;

static ULONG PkgLen(const UCHAR *p, ULONG Avail, ULONG *Hdr)
{
    ULONG n = p[0] >> 6, len, i;

    if (Avail < n + 1) {
        return 0;
    }
    if (n == 0) {
        *Hdr = 1;
        return p[0] & 0x3F;
    }
    len = p[0] & 0x0F;
    for (i = 0; i < n; i++) {
        len |= (ULONG)p[1 + i] << (4 + 8 * i);
    }
    *Hdr = n + 1;
    return len;
}

static void EisaStr(const UCHAR *b, char *Out)
{
    USHORT v = (USHORT)((b[0] << 8) | b[1]);
    static const char hex[] = "0123456789ABCDEF";

    Out[0] = (char)('@' + ((v >> 10) & 0x1F));
    Out[1] = (char)('@' + ((v >> 5) & 0x1F));
    Out[2] = (char)('@' + (v & 0x1F));
    Out[3] = hex[b[2] >> 4];
    Out[4] = hex[b[2] & 15];
    Out[5] = hex[b[3] >> 4];
    Out[6] = hex[b[3] & 15];
    Out[7] = 0;
}

static BOOLEAN IdLike(const char *s)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (!((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z') || s[i] == '_')) return FALSE;
    }
    for (i = 3; i < 7; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F') || (s[i] >= 'a' && s[i] <= 'f'))) return FALSE;
    }
    return (BOOLEAN)(s[7] == 0);
}

static void AddId(const char *s)
{
    int i;

    for (i = 0; i < NAcpiIds; i++) {
        if (UsbMemCmp(AcpiIds[i], s, 8) == 0) return;
    }
    if (NAcpiIds < MAX_IDS) {
        UsbMemCpy(AcpiIds[NAcpiIds++], s, 8);
    }
}

/* every EisaId and 7 character id string in T[From..To) */
static void IdsIn(const UCHAR *T, ULONG From, ULONG To)
{
    ULONG i;
    char s[9];

    for (i = From; i + 5 <= To; i++) {
        if (T[i] == 0x0C) {
            EisaStr(T + i + 1, s);
            if (IdLike(s) && s[0] != '@') AddId(s);
        } else if (T[i] == 0x0D && i + 9 <= To && T[i + 8] == 0) {
            UsbMemCpy(s, T + i + 1, 8);
            if (IdLike(s)) AddId(s);
        }
    }
}

/* the innermost Device () around Pos */
static BOOLEAN DeviceAround(const UCHAR *T, ULONG Len, ULONG Pos, ULONG *Body, ULONG *End)
{
    ULONG i, best = 0;
    BOOLEAN found = FALSE;

    for (i = 36; i + 8 < Len && i < Pos; i++) {
        ULONG hdr = 0, plen;
        if (T[i] != 0x5B || T[i + 1] != 0x82) {
            continue;
        }
        plen = PkgLen(T + i + 2, Len - i - 2, &hdr);
        if (plen < hdr + 4 || i + 2 + plen > Len || i + 2 + plen <= Pos) {
            continue;
        }
        if (!found || i > best) {
            best = i;
            *Body = i + 2 + hdr;
            *End = i + 2 + plen;
            found = TRUE;
        }
    }
    return found;
}

/* devices with a PS/2 mouse id (PNP0Fxx): all their ids.  A _HID that is a
   method counts with every id constant in it (the methods are not run). */
static void ReadAcpiIds(void)
{
    int t;

    if (AcpiRead) {
        return;
    }
    AcpiRead = TRUE;
    if (AcpiInit() == 0) {
        return;
    }
    for (t = 0; AcpiTable(t) != NULL; t++) {
        const ACPI_TABLE *a = AcpiTable(t);
        ULONG i;
        if (a->Sig != ACPI_SIG('D', 'S', 'D', 'T') && a->Sig != ACPI_SIG('S', 'S', 'D', 'T')) {
            continue;
        }
        for (i = 36; i + 5 <= a->Len; i++) {
            const UCHAR *p = a->Va + i;
            BOOLEAN ps2 = (BOOLEAN)((p[0] == 0x0C && p[1] == 0x41 && p[2] == 0xD0 && p[3] == 0x0F) ||
                                    (i + 9 <= a->Len && p[0] == 0x0D && p[1] == 'P' && p[2] == 'N' && p[3] == 'P' &&
                                     p[4] == '0' && p[5] == 'F' && p[8] == 0));
            ULONG body, end;
            if (ps2 && DeviceAround(a->Va, a->Len, i, &body, &end)) {
                IdsIn(a->Va, body, end);
                i = end;
            }
        }
    }
    if (NAcpiIds != 0) {
        char line[96];
        int k, o = 0;
        for (k = 0; k < NAcpiIds; k++) {
            o += UsbFormat(line + o, (int)sizeof(line) - o, " %s", AcpiIds[k]);
        }
        UsbLog(LOG_INFO, "smbus: PS/2 mouse in ACPI:%s\n", line);
    }
}

static BOOLEAN AcpiWhitelisted(void)
{
    int i;

    ReadAcpiIds();
    for (i = 0; i < NAcpiIds; i++) {
        if (SmbPnpWhitelisted(AcpiIds[i])) return TRUE;
    }
    return FALSE;
}

static BOOLEAN AcpiForcePad(void)
{
    int i;

    ReadAcpiIds();
    for (i = 0; i < NAcpiIds; i++) {
        if (SmbPnpForcePad(AcpiIds[i])) return TRUE;
    }
    return FALSE;
}

static BOOLEAN AcpiHasId(const char *Id)
{
    int i;

    ReadAcpiIds();
    for (i = 0; i < NAcpiIds; i++) {
        if (UsbMemCmp(AcpiIds[i], Id, 8) == 0) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* output: contacts to the gestures of ptp.c                            */
/* ------------------------------------------------------------------ */

/* The gesture code reads precision touch pad reports; the contacts are put
   into one made up for it: byte 0 bit 0 the pad's click, byte 1 the contact
   count, then 5 contacts of tip (bit), id (byte), X, Y (16 bit). */
#define PTP_REP_LEN     32

static void Field(HID_FIELD *F, USHORT Off, UCHAR Size, LONG Max)
{
    UsbMemSet(F, 0, sizeof(*F));
    F->Valid = 1;
    F->Size = Size;
    F->Count = 1;
    F->Offset = Off;
    F->Max = Max;
}

static void PadLayout(SMB_PAD *p, HID_PTP_LAYOUT *L)
{
    int k;

    UsbMemSet(L, 0, sizeof(*L));
    L->Valid = 1;
    L->NFingers = 5;
    Field(&L->Button, 0, 1, 1);
    Field(&L->Count, 8, 8, 255);
    for (k = 0; k < 5; k++) {
        USHORT b = (USHORT)(16 + 48 * k);
        Field(&L->F[k].Tip, b, 1, 1);
        Field(&L->F[k].Id, (USHORT)(b + 8), 8, 255);
        Field(&L->F[k].X, (USHORT)(b + 16), 16, p->MaxX);
        Field(&L->F[k].Y, (USHORT)(b + 32), 16, p->MaxY);
    }
}

static void PutContact(UCHAR *R, int Slot, BOOLEAN Down, UCHAR Id, LONG X, LONG Y)
{
    UCHAR *c = R + 2 + Slot * 6;

    c[0] = (UCHAR)(Down ? 1 : 0);
    c[1] = Id;
    c[2] = (UCHAR)X;
    c[3] = (UCHAR)(X >> 8);
    c[4] = (UCHAR)Y;
    c[5] = (UCHAR)(Y >> 8);
}

/* a frame of contacts: the ones down, and those that left since the last one */
static void PadFrame(SMB_PAD *p, const SMB_FRAME *F, UCHAR PadButton)
{
    UCHAR rep[PTP_REP_LEN];
    struct { UCHAR Down, Id; LONG X, Y; } e[SMB_MAX_CONTACTS + 16];
    USHORT now = 0;
    int n = 0, i, k;

    if (p->Ptp == NULL || p->Mouse == NULL) {
        return;
    }
    for (i = 0; i < F->N; i++) {
        UCHAR id = (UCHAR)(F->C[i].Id & 15);
        now |= (USHORT)(1 << id);
        e[n].Down = 1; e[n].Id = id; e[n].X = F->C[i].X; e[n].Y = F->C[i].Y;
        p->LastX[id] = F->C[i].X;
        p->LastY[id] = F->C[i].Y;
        n++;
    }
    for (k = 0; k < 16; k++) {
        if ((p->PrevIds & (1 << k)) && !(now & (1 << k))) {
            e[n].Down = 0; e[n].Id = (UCHAR)k; e[n].X = p->LastX[k]; e[n].Y = p->LastY[k];
            n++;
        }
    }
    if (n == 0 && PadButton == p->LastPadButton && p->PrevIds == 0) {
        return;                                     /* nothing on the pad, nothing changed */
    }
    p->PrevIds = now;
    p->LastPadButton = PadButton;
    p->Frames++;
    /* up to five contacts per report; more go into a second one (count 0) */
    for (i = 0; i == 0 || i < n; i += 5) {
        UsbMemSet(rep, 0, sizeof(rep));
        rep[0] = (UCHAR)(PadButton ? 1 : 0);
        rep[1] = (UCHAR)(i == 0 ? n : 0);
        for (k = 0; k < 5 && i + k < n; k++) {
            PutContact(rep, k, e[i + k].Down, e[i + k].Id, e[i + k].X, e[i + k].Y);
        }
        PtpReport(p->Ptp, rep, sizeof(rep), p->Mouse);
    }
    if (F->N != 0) {
        p->LastActive = OsTimeMs();
    }
}

/* separate buttons and the trackpoint */
static void StickOut(SMB_PAD *p, LONG Dx, LONG Dy)
{
    UCHAR b = (UCHAR)(p->Buttons | p->StickButtons | p->StickPs2Buttons);

    if (p->Stick == NULL || (Dx == 0 && Dy == 0 && b == p->StickReported)) {
        return;
    }
    p->StickReported = b;
    HidExtMouse(p->Stick, b, Dx, Dy, 0, 0);
}

static BOOLEAN Attach(SMB_PAD *p)
{
    HID_PTP_LAYOUT pl;
    HID_MOUSE_LAYOUT ml;

    if (p->MaxX <= 0) p->MaxX = 4000;
    if (p->MaxY <= 0) p->MaxY = p->MaxX * 2 / 3;
    p->Ptp = (PTP_STATE *)OsAlloc(sizeof(PTP_STATE));
    if (p->Ptp == NULL) {
        return FALSE;
    }
    PadLayout(p, &pl);
    PtpInit(p->Ptp, &pl);
    /* what the system is told about the pointer: three buttons and a wheel */
    UsbMemSet(&ml, 0, sizeof(ml));
    ml.Kind = HID_PTR_TOUCHPAD;
    ml.ButtonsFirst = 1;
    Field(&ml.Buttons, 0, 1, 1);
    ml.Buttons.Count = 3;
    Field(&ml.Wheel, 8, 8, 127);
    ml.Wheel.Relative = 1;
    p->Mouse = HidExtAdd(HID_EXT_MOUSE, &ml, NULL, NULL);
    /* a second pointer for separate buttons and the trackpoint */
    if (p->Kind == SMB_PAD_ELAN || p->HasF03 || (p->HasGpio && !p->ClickPad)) {
        p->Stick = HidExtAdd(HID_EXT_MOUSE, &ml, NULL, NULL);
    }
    p->NextPoll = OsTimeMs();
    p->LastActive = OsTimeMs();
    return (BOOLEAN)(p->Mouse != NULL);
}

/* ------------------------------------------------------------------ */
/* RMI4 over SMBus                                                      */
/* ------------------------------------------------------------------ */

/* the command code of an RMI address: the device's mapping table (8 entries,
   given out in turn, as rmi_smbus.c does) */
static int RmiCode(SMB_PAD *p, USHORT Addr, UCHAR Count, BOOLEAN Read, UCHAR *Code)
{
    UCHAR e[4];
    int i, r;

    for (i = 0; i < 8; i++) {
        RMI_MAP *m = &p->Map[i];
        if (m->Addr == Addr && ((Read && m->Count == Count && !(m->Flags & 1)) || (!Read && (m->Flags & 1)))) {
            *Code = (UCHAR)i;
            return SMB_OK;
        }
    }
    i = p->MapNext;
    p->MapNext = (UCHAR)((i + 1) % 8);
    e[0] = (UCHAR)Addr;
    e[1] = (UCHAR)(Addr >> 8);
    e[2] = Read ? Count : 0;
    e[3] = (UCHAR)(Read ? 0 : 1);
    r = SmbWriteBlock(p->H, p->Port, p->Addr, (UCHAR)(0x80 + i), e, 4);
    if (r < 0) {
        UsbMemSet(&p->Map[i], 0, sizeof(p->Map[i]));
        return r;
    }
    p->Map[i].Addr = Addr;
    p->Map[i].Count = e[2];
    p->Map[i].Flags = e[3];
    *Code = (UCHAR)i;
    return SMB_OK;
}

static int RmiRead(SMB_PAD *p, USHORT Addr, UCHAR *Buf, ULONG Len)
{
    UCHAR tmp[SMB_BLOCK_MAX];

    UsbMemSet(Buf, 0, Len);
    while (Len > 0) {
        UCHAR n = (UCHAR)(Len > SMB_BLOCK_MAX ? SMB_BLOCK_MAX : Len), code;
        int r = RmiCode(p, Addr, n, TRUE, &code);
        if (r < 0) return r;
        r = SmbReadBlock(p->H, p->Port, p->Addr, code, tmp);
        if (r < 0) return r;
        UsbMemCpy(Buf, tmp, (ULONG)r < n ? (ULONG)r : n);
        Buf += n;
        Len -= n;
        Addr = (USHORT)(Addr + SMB_BLOCK_MAX);
    }
    return SMB_OK;
}

static int RmiWrite(SMB_PAD *p, USHORT Addr, const UCHAR *Buf, ULONG Len)
{
    while (Len > 0) {
        UCHAR n = (UCHAR)(Len > SMB_BLOCK_MAX ? SMB_BLOCK_MAX : Len), code;
        int r = RmiCode(p, Addr, n, FALSE, &code);
        if (r < 0) return r;
        r = SmbWriteBlock(p->H, p->Port, p->Addr, code, Buf, n);
        if (r < 0) return r;
        Buf += n;
        Len -= n;
        Addr = (USHORT)(Addr + n);
    }
    return SMB_OK;
}

static int RmiRead1(SMB_PAD *p, USHORT Addr)
{
    UCHAR b;
    int r = RmiRead(p, Addr, &b, 1);
    return r < 0 ? r : b;
}

/* register descriptor of F12 at Addr: its size, the presence register, the structure */
static BOOLEAN RmiDesc(SMB_PAD *p, USHORT Addr, RMI_REG_DESC *D)
{
    UCHAR pres[35], st[128];
    int n = RmiRead1(p, Addr);
    ULONG ss;

    if (n < 1 || n > (int)sizeof(pres) || RmiRead(p, (USHORT)(Addr + 1), pres, (ULONG)n) < 0) {
        return FALSE;
    }
    ss = RmiDescStructSize(pres, (ULONG)n);
    if (ss == 0 || ss > sizeof(st) || RmiRead(p, (USHORT)(Addr + 2), st, ss) < 0) {
        return FALSE;
    }
    return (BOOLEAN)(RmiParseRegDesc(pres, (ULONG)n, st, ss, D) == 0);
}

static BOOLEAN RmiF12(SMB_PAD *p)
{
    RMI_REG_DESC *d = (RMI_REG_DESC *)OsAlloc(sizeof(RMI_REG_DESC) * 2);
    RMI_FN *f = &p->F2D;
    UCHAR t[16];
    int q, idx;
    BOOLEAN ok = FALSE;

    if (d == NULL) {
        return FALSE;
    }
    q = RmiRead1(p, f->Query);
    if (q < 0 || !(q & 1)) {
        UsbLog(LOG_INFO, "smbus: %s: F12 without register descriptors\n", p->Name);
        goto out;
    }
    /* query + 1: query descriptor, + 4: control, + 7: data */
    if (!RmiDesc(p, (USHORT)(f->Query + 4), &d[0]) || !RmiDesc(p, (USHORT)(f->Query + 7), &d[1])) {
        UsbLog(LOG_INFO, "smbus: %s: F12 register descriptors not readable\n", p->Name);
        goto out;
    }
    /* control 8: sensor tuning, subpacket 0 = maximum X and Y */
    idx = RmiDescIndex(&d[0], 8);
    if (idx < 0 || d[0].Size[8] > sizeof(t) || d[0].Size[8] < 4 || !(d[0].SubMap[8] & 1) ||
        RmiRead(p, (USHORT)(f->Ctrl + idx), t, d[0].Size[8]) < 0) {
        UsbLog(LOG_INFO, "smbus: %s: F12 has no sensor tuning register\n", p->Name);
        goto out;
    }
    p->MaxX = GET16(t);
    p->MaxY = GET16(t + 2);
    if (!RmiDescHas(&d[1], 1)) {
        UsbLog(LOG_INFO, "smbus: %s: F12 reports no objects (data 1)\n", p->Name);
        goto out;
    }
    p->Objects = d[1].Subpackets[1];
    p->Data1Off = (USHORT)RmiDescByteOffset(&d[1], 1);
    /* the objects that fit into one block read (four from offset 0: enough for gestures) */
    p->DataLen = (USHORT)(p->Data1Off + p->Objects * 8);
    if (p->DataLen > SMB_BLOCK_MAX) {
        p->DataLen = SMB_BLOCK_MAX;
    }
    if (p->Data1Off + 8 > p->DataLen) {
        goto out;
    }
    p->Is2D = 12;
    p->Irq2D = 1UL << f->IrqPos;                    /* absolute data (the next one: relative) */
    ok = TRUE;
out:
    OsFree(d);
    return ok;
}

static BOOLEAN RmiF11(SMB_PAD *p)
{
    RMI_FN *f = &p->F2D;
    UCHAR q[4], t[4];
    int fingers;

    if (RmiRead(p, (USHORT)(f->Query + 1), q, 4) < 0 || !(q[0] & 0x10)) {
        UsbLog(LOG_INFO, "smbus: %s: F11 without absolute data\n", p->Name);
        return FALSE;
    }
    fingers = (q[0] & 7) == 5 ? 10 : (q[0] & 7) + 1;
    if (RmiRead(p, (USHORT)(f->Ctrl + 6), t, 4) < 0) {
        return FALSE;
    }
    p->MaxX = GET16(t) & 0x0FFF;
    p->MaxY = GET16(t + 2) & 0x0FFF;
    p->Objects = (UCHAR)fingers;
    p->DataLen = (USHORT)((fingers + 3) / 4 + fingers * 5);
    if (p->DataLen > SMB_BLOCK_MAX) {
        p->DataLen = SMB_BLOCK_MAX;
    }
    p->Is2D = 11;
    p->Irq2D = 1UL << f->IrqPos;
    return TRUE;
}

/* buttons: which GPIOs are buttons, and which button they are (rmi_f30.c / rmi_f3a.c) */
static void RmiGpio(SMB_PAD *p)
{
    RMI_FN *f = &p->Gpio;
    UCHAR q[2], dir[4], btn[4], exist[4];
    int i, count, mapped = 0, stick = 0, n;

    UsbMemSet(p->GpioMap, 0, sizeof(p->GpioMap));
    p->HasGpio = 0;
    if (!p->GpioIsF3a) {
        USHORT a = f->Ctrl;
        if (RmiRead(p, f->Query, q, 2) < 0 || !(q[0] & 0x08)) {
            return;                                     /* no GPIOs */
        }
        count = q[0 + 1] & 0x1F;
        p->GpioRegs = (UCHAR)((count + 7) / 8);
        if (p->GpioRegs == 0 || p->GpioRegs > 4) return;
        if (q[0] & 0x04) a = (USHORT)(a + p->GpioRegs);    /* control 0: GPIO and LED */
        a++;                                                /* control 1 */
        if (RmiRead(p, a, dir, p->GpioRegs) < 0 || RmiRead(p, (USHORT)(a + p->GpioRegs), btn, p->GpioRegs) < 0) {
            return;
        }
        for (i = 0; i < p->GpioRegs; i++) exist[i] = 0xFF;
    } else {
        int r = RmiRead1(p, f->Query);
        if (r < 0) return;
        count = r & 0x7F;
        p->GpioRegs = (UCHAR)((count + 7) / 8);
        if (p->GpioRegs == 0 || p->GpioRegs > 4) return;
        if (RmiRead(p, (USHORT)(f->Query + 1), exist, p->GpioRegs) < 0 ||
            RmiRead(p, (USHORT)(f->Ctrl + 1), dir, p->GpioRegs) < 0) {
            return;
        }
        for (i = 0; i < p->GpioRegs; i++) btn[i] = 0xFF;
    }
    n = count < 6 ? count : 6;
    for (i = 0; i < n; i++) {
        UCHAR bit = (UCHAR)(1 << (i % 8));
        /* F30: direction input (0) and "is a button"; F3A: exists and input */
        if ((dir[i / 8] & bit) || !(btn[i / 8] & bit) || !(exist[i / 8] & bit)) {
            continue;
        }
        if (p->StickBtns && i >= 3 && i < 6) {
            if (stick < 3) p->GpioMap[i] = (UCHAR)(0x81 + stick++);
        } else if (!p->ClickPad || !mapped) {
            if (mapped < 3) p->GpioMap[i] = (UCHAR)(1 + mapped);
            mapped++;
        }
    }
    if (mapped == 1) {
        p->ClickPad = 1;                                /* one button: the pad's click */
    }
    p->GpioCount = (UCHAR)(count > 16 ? 16 : count);
    p->HasGpio = 1;
    p->IrqGpio = 1UL << f->IrqPos;
}

static void RmiF03(SMB_PAD *p)
{
    RMI_FN *f = &p->F03;
    UCHAR q2[8], obs[30], b;
    int q1 = RmiRead1(p, f->Query), devs, bpd, qlen = 7;

    p->HasF03 = 0;
    if (q1 < 0) return;
    devs = q1 & 7;
    bpd = (q1 >> 4) & 7;
    if (devs * bpd >= 1) {
        ULONG l = (ULONG)(devs * bpd) > sizeof(q2) ? sizeof(q2) : (ULONG)(devs * bpd);
        if (RmiRead(p, (USHORT)(f->Query + 1), q2, l) < 0) return;
        qlen = q2[0] & 0x0F;
    }
    if (qlen == 0) return;
    p->F03Len = (UCHAR)(qlen * 2 > (int)sizeof(obs) ? (int)sizeof(obs) : qlen * 2);
    /* pending bytes first (some send AA 00), then defaults and data reporting */
    RmiRead(p, (USHORT)(f->Data + 2), obs, p->F03Len);
    b = 0xF6;
    RmiWrite(p, f->Data, &b, 1);
    OsSleepMs(20);
    b = 0xF4;
    RmiWrite(p, f->Data, &b, 1);
    OsSleepMs(20);
    RmiRead(p, (USHORT)(f->Data + 2), obs, p->F03Len);
    p->Pkt.N = 0;
    p->HasF03 = 1;
    p->IrqF03 = 1UL << f->IrqPos;
}

static BOOLEAN RmiInit(SMB_PAD *p)
{
    UCHAR e[6], q[21], b, mask[4];
    int page, empty = 0, irq = 0, v;
    USHORT a;
    BOOLEAN has2D = FALSE, f12 = FALSE;

    UsbMemSet(p->Map, 0, sizeof(p->Map));
    p->MapNext = 0;
    p->F01.Num = 0;
    p->HasGpio = p->HasF03 = 0;
    p->IrqMask = 0;
    /* the SMBus version register activates the pad; 2 and 3 are known */
    v = SmbReadByteData(p->H, p->Port, p->Addr, 0xFD);
    if (v < 0) {
        return FALSE;
    }
    p->SmbVersion = (UCHAR)(v + 1);
    if (p->SmbVersion != 2 && p->SmbVersion != 3) {
        UsbLog(LOG_INFO, "smbus: %s: unknown RMI SMBus version %u\n", p->Name, p->SmbVersion);
        return FALSE;
    }
    /* page description tables, from 00E9h down on each page, two empty pages end it */
    for (page = 0; page < 8 && empty < 2; page++) {
        USHORT start = (USHORT)(page * 0x100 + 0xE9), end = (USHORT)(page * 0x100 + 5);
        for (a = start; a >= end; a = (USHORT)(a - 6)) {
            RMI_FN f;
            if (RmiRead(p, a, e, 6) < 0) {
                return FALSE;
            }
            if (e[5] == 0x00 || e[5] == 0xFF) {
                break;
            }
            f.Num = e[5];
            f.Query = (USHORT)(page * 0x100 + e[0]);
            f.Cmd = (USHORT)(page * 0x100 + e[1]);
            f.Ctrl = (USHORT)(page * 0x100 + e[2]);
            f.Data = (USHORT)(page * 0x100 + e[3]);
            f.IrqCount = (UCHAR)(e[4] & 7);
            f.Ver = (UCHAR)((e[4] >> 5) & 3);
            f.IrqPos = (UCHAR)irq;
            irq += f.IrqCount;
            if (f.Num == 0x01) {
                p->F01 = f;
            } else if (f.Num == 0x12 && !f12) {
                p->F2D = f;
                has2D = f12 = TRUE;
            } else if (f.Num == 0x11 && !has2D) {
                p->F2D = f;
                has2D = TRUE;
            } else if (f.Num == 0x30 || f.Num == 0x3A) {
                p->Gpio = f;
                p->GpioIsF3a = (UCHAR)(f.Num == 0x3A);
                p->HasGpio = 1;
            } else if (f.Num == 0x03) {
                p->F03 = f;
                p->HasF03 = 1;
            } else if (f.Num == 0x34 && f.Ver > 1) {
                if (RmiRead(p, f.Data, &b, 1) == SMB_OK && (b & 0x80)) {
                    UsbLog(LOG_ERR, "smbus: %s: in boot loader mode\n", p->Name);
                    return FALSE;
                }
            }
            if (a < end + 6) {
                break;
            }
        }
        empty = (a == start) ? empty + 1 : 0;
    }
    p->IrqRegs = (UCHAR)((irq + 7) / 8);
    if (p->F01.Num != 0x01 || !has2D || p->IrqRegs == 0 || p->IrqRegs > 4) {
        UsbLog(LOG_INFO, "smbus: %s: no F01 / 2D sensor function\n", p->Name);
        return FALSE;
    }
    /* F01: product, then configured, normal (not sleep) operation */
    if (RmiRead(p, p->F01.Query, q, sizeof(q)) == SMB_OK) {
        char prod[11];
        int i;
        for (i = 0; i < 10 && q[11 + i] >= 0x20 && q[11 + i] < 0x7F; i++) prod[i] = (char)q[11 + i];
        prod[i] = 0;
        UsbFormat(p->Info, sizeof(p->Info), "%s %s, RMI4 SMBus version %u", q[0] == 1 ? "Synaptics" : "RMI4", prod,
                  p->SmbVersion);
    }
    if (RmiRead(p, p->F01.Data, &b, 1) < 0) {
        return FALSE;
    }
    if (b & 0x40) {
        UsbLog(LOG_ERR, "smbus: %s: in boot loader mode\n", p->Name);
        return FALSE;
    }
    if (RmiRead(p, p->F01.Ctrl, &b, 1) < 0) {
        return FALSE;
    }
    b = (UCHAR)((b & ~0x03) | 0x80);
    if (RmiWrite(p, p->F01.Ctrl, &b, 1) < 0) {
        return FALSE;
    }
    p->IrqF01 = p->F01.IrqCount ? 1UL << p->F01.IrqPos : 0;
    if (!(f12 ? RmiF12(p) : RmiF11(p))) {
        return FALSE;
    }
    if (p->HasGpio) {
        RmiGpio(p);
    }
    if (p->HasF03) {
        RmiF03(p);
    }
    /* interrupt sources on: device status, the 2D data, buttons, pass-through */
    p->IrqMask = p->IrqF01 | p->Irq2D | (p->HasGpio ? p->IrqGpio : 0) | (p->HasF03 ? p->IrqF03 : 0);
    if (RmiRead(p, (USHORT)(p->F01.Ctrl + 1), mask, p->IrqRegs) < 0) {
        return FALSE;
    }
    for (v = 0; v < p->IrqRegs; v++) {
        mask[v] |= (UCHAR)(p->IrqMask >> (8 * v));
    }
    if (RmiWrite(p, (USHORT)(p->F01.Ctrl + 1), mask, p->IrqRegs) < 0) {
        return FALSE;
    }
    RmiRead(p, (USHORT)(p->F01.Data + 1), mask, p->IrqRegs);     /* clears what is pending */
    UsbLog(LOG_INFO, "smbus: %s: %s, F%u %ux%u, %u objects%s%s%s\n", p->Name, p->Info, p->Is2D, p->MaxX, p->MaxY,
           p->Objects, p->HasGpio ? (p->GpioIsF3a ? ", F3A buttons" : ", F30 buttons") : "",
           p->ClickPad ? " (click pad)" : "", p->HasF03 ? ", trackpoint (F03)" : "");
    return TRUE;
}

/* the data of a finished poll */
static void RmiCommit(SMB_PAD *p)
{

    if (p->Got & (1 << JOB_GPIO)) {
        UCHAR stick = 0, b = RmiGpioButtons(p->JobBuf[JOB_GPIO], p->GpioCount, p->GpioMap, &stick);
        if (p->ClickPad) {
            p->PadButton = (UCHAR)(b != 0);
            p->Buttons = 0;
        } else {
            p->Buttons = b;
        }
        p->StickButtons = stick;
    }
    if (p->Got & (1 << JOB_F03)) {
        UCHAR bytes[16], btn;
        int n = RmiF03Bytes(p->JobBuf[JOB_F03], p->JobLen[JOB_F03], bytes), i;
        for (i = 0; i < n; i++) {
            LONG x, y;
            if (SmbPs2Packet(&p->Pkt, bytes[i], &x, &y, &btn)) {
                p->StickPs2Buttons = btn;
                StickOut(p, x, y);
            }
        }
    }
    if (p->Got & (1 << JOB_2D)) {
        if (p->Is2D == 12) {
            RmiF12Decode(p->JobBuf[JOB_2D] + p->Data1Off, (ULONG)(p->JobLen[JOB_2D] - p->Data1Off), p->Objects,
                         p->MaxX, p->MaxY, TRUE, &p->Fr);
        } else {
            RmiF11Decode(p->JobBuf[JOB_2D], p->JobLen[JOB_2D], p->Objects, p->MaxX, p->MaxY, TRUE, &p->Fr);
        }
    }
    if (p->Got & ((1 << JOB_2D) | (1 << JOB_GPIO))) {
        PadFrame(p, &p->Fr, p->PadButton);
    }
    StickOut(p, 0, 0);
    p->Reports++;
}

/* next read of this poll: the address and length of a job */
static BOOLEAN RmiJob(SMB_PAD *p, int Job, USHORT *Addr, UCHAR *Len)
{
    switch (Job) {
    case JOB_2D:
        *Addr = p->F2D.Data;
        *Len = (UCHAR)p->DataLen;
        return TRUE;
    case JOB_GPIO:
        *Addr = p->Gpio.Data;
        *Len = p->GpioRegs;
        return TRUE;
    case JOB_F03:
        *Addr = (USHORT)(p->F03.Data + 2);
        *Len = p->F03Len;
        return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Elan over SMBus                                                      */
/* ------------------------------------------------------------------ */

static BOOLEAN ElanInit(SMB_PAD *p)
{
    static const UCHAR hello[5] = { 0x55, 0x55, 0x55, 0x55, 0x55 };
    UCHAR v[SMB_BLOCK_MAX], mode[4] = { 0x00, 0x07, 0x00, 0x01 };
    int r = SmbReadBlock(p->H, p->Port, p->Addr, 0xA7, v);
    USHORT prod = 0;
    UCHAR sm = 0, fw = 0;

    if (r != 5 || UsbMemCmp(v, hello, 5) != 0) {
        return FALSE;
    }
    if (SmbWriteByte(p->H, p->Port, p->Addr, 0x20) < 0) {      /* enable the touch pad */
        return FALSE;
    }
    if (SmbReadBlock(p->H, p->Port, p->Addr, 0xA3, v) >= 2) {  /* product id / SM version, IC type */
        prod = GETBE16(v);
        sm = v[0];
    }
    if (SmbReadBlock(p->H, p->Port, p->Addr, 0xA1, v) >= 3) {
        fw = v[2];
    }
    if (SmbWriteBlock(p->H, p->Port, p->Addr, 0x00, mode, 4) < 0) {     /* absolute mode */
        return FALSE;
    }
    if (SmbReadBlock(p->H, p->Port, p->Addr, 0xA0, v) != 3) {
        UsbLog(LOG_INFO, "smbus: %s: no dimensions\n", p->Name);
        return FALSE;
    }
    p->MaxX = ((v[0] & 0x0F) << 8) | v[1];
    p->MaxY = ((v[0] & 0xF0) << 4) | v[2];
    p->ElanClick = (UCHAR)((sm & 0x10) != 0);
    if (p->ElanClick) {
        p->ClickPad = 1;
    }
    UsbFormat(p->Info, sizeof(p->Info), "Elan product %04X, firmware %02X", prod, fw);
    UsbLog(LOG_INFO, "smbus: %s: %s, %ux%u%s\n", p->Name, p->Info, p->MaxX, p->MaxY,
           p->ClickPad ? " (click pad)" : "");
    return TRUE;
}

static void ElanReport(SMB_PAD *p, const UCHAR *B, int Len)
{
    LONG dx = 0, dy = 0;
    UCHAR sb = 0;
    int k = ElanDecode(B, (ULONG)Len, p->MaxX, p->MaxY, &p->Fr, &dx, &dy, &sb);

    if (k == ELAN_REP_TOUCH) {
        if (p->ClickPad) {
            p->PadButton = (UCHAR)(p->Fr.Buttons != 0);
            p->Buttons = 0;
        } else {
            p->Buttons = p->Fr.Buttons;
        }
        PadFrame(p, &p->Fr, p->PadButton);
        StickOut(p, 0, 0);
        p->Reports++;
    } else if (k == ELAN_REP_STICK) {
        p->StickPs2Buttons = sb;
        StickOut(p, dx, dy);
        p->Reports++;
    }
}

/* ------------------------------------------------------------------ */
/* finding the pads                                                     */
/* ------------------------------------------------------------------ */

static SMB_PAD *Known(SMB_HOST *H, UCHAR Port, UCHAR Addr)
{
    int i;

    for (i = 0; i < SmbTpCount; i++) {
        if (Pads[i]->H == H && Pads[i]->Port == Port && Pads[i]->Addr == Addr) return Pads[i];
    }
    return NULL;
}

static BOOLEAN StartPad(SMB_PAD *p)
{
    return p->Kind == SMB_PAD_SYNAPTICS ? RmiInit(p) : ElanInit(p);
}

/* a pad of this kind at Addr on any host and port */
static SMB_PAD *Probe(UCHAR Kind, UCHAR Addr, const SMB_PS2_INFO *Info)
{
    int i, port;

    if (SmbTpCount >= MAX_PAD) {
        return NULL;
    }
    for (i = 0; i < SmbHostCount; i++) {
        SMB_HOST *h = SmbHostGet(i);
        for (port = 0; port < SmbHostPorts(h); port++) {
            SMB_PAD *p;
            if (Known(h, (UCHAR)port, Addr) != NULL) {
                return NULL;
            }
            p = (SMB_PAD *)OsAlloc(sizeof(SMB_PAD));
            if (p == NULL) {
                return NULL;
            }
            p->H = h;
            p->Port = (UCHAR)port;
            p->Addr = Addr;
            p->Kind = Kind;
            if (Info != NULL) {
                p->ClickPad = Info->ClickPad;
                p->StickBtns = Info->StickButtons;
            }
            if (SmbHostPorts(h) > 1) {
                UsbFormat(p->Name, sizeof(p->Name), "%s %02x (port %u)", Kind == SMB_PAD_SYNAPTICS ? "rmi4" : "elan",
                          Addr, port);
            } else {
                UsbFormat(p->Name, sizeof(p->Name), "%s %02x", Kind == SMB_PAD_SYNAPTICS ? "rmi4" : "elan", Addr);
            }
            if (StartPad(p) && Attach(p)) {
                Pads[SmbTpCount++] = p;
                UsbLog(LOG_INFO, "smbus: %s on %s: touch pad on SMBus (polled), gestures%s\n", p->Name,
                       SmbHostName(h), p->Stick != NULL && (p->HasF03 || !p->ClickPad) ? ", buttons / trackpoint" : "");
                UsbPollWantMs = 8;
                return p;
            }
            if (p->Ptp != NULL) OsFree(p->Ptp);
            OsFree(p);
        }
    }
    return NULL;
}

void SmbusInit(void)
{
    ULONG dev, fn;

    if (Inited) {
        return;
    }
    Inited = TRUE;
    Mode = GetMode();
    if (Mode == 0) {
        return;
    }
    for (dev = 0; dev < 32; dev++) {
        for (fn = 0; fn < 8; fn++) {
            ULONG id = OsPciRead(0, (UCHAR)dev, (UCHAR)fn, 0, 4);
            if (id == 0xFFFFFFFF || id == 0) {
                if (fn == 0) break;
                continue;
            }
            SmbHostProbe(0, (UCHAR)dev, (UCHAR)fn, id, OsPciRead(0, (UCHAR)dev, (UCHAR)fn, 8, 4) >> 8);
            if (fn == 0 && !(OsPciRead(0, (UCHAR)dev, 0, 0x0E, 1) & 0x80)) break;
        }
    }
}

BOOLEAN SmbusTouchpadTakeOver(const SMB_PS2_IO *Io)
{
    SMB_PS2_INFO info;
    ULONG ov;
    BOOLEAN use = FALSE;
    UCHAR addr;
    const char *why = "";

    SmbusInit();
    Ps2Seen = TRUE;
    if (Mode == 0 || SmbHostCount == 0) {
        return FALSE;
    }
    SmbPs2Identify(Io, &info);
    if (info.Kind == SMB_PAD_SYNAPTICS) {
        UsbLog(LOG_INFO, "smbus: PS/2: Synaptics pad, identity %06x, capabilities %06x, ext %06x 0c %06x 10 %06x, "
               "board %u%s\n", info.Identity, info.Caps, info.ExtCap, info.ExtCap0c, info.ExtCap10, info.BoardId,
               info.SmbusCapable ? ", InterTouch (SMBus)" : "");
    } else if (info.Kind == SMB_PAD_ELAN) {
        UsbLog(LOG_INFO, "smbus: PS/2: Elan pad, firmware %06x, hardware %u, bus %u%s\n", info.FwVersion,
               info.HwVersion, info.Bus, info.SmbusCapable ? ", SMBus" : "");
    } else {
        UsbLog(LOG_INFO, "smbus: PS/2: not a Synaptics or Elan pad, stays a PS/2 mouse\n");
        return FALSE;
    }
    if (!info.SmbusCapable) {
        return FALSE;
    }
    if (info.Kind == SMB_PAD_SYNAPTICS) {
        if (AcpiForcePad()) {
            why = "a ForcePad (not supported)";
        } else if (Mode == 2) {
            use = TRUE;
        } else if (!AcpiWhitelisted()) {
            why = "its id is not on Linux's InterTouch list (Smbus=2 tries every pad)";
        } else if (info.BoardId == 2722 && AcpiHasId("LEN0036")) {
            why = "a T440p board revision that does not work on SMBus";
        } else {
            use = TRUE;
        }
    } else {
        use = (BOOLEAN)(Mode == 2 || info.NewIcHostNotify);
        if (!use) why = "an older IC (Smbus=2 tries it)";
    }
    if (!use) {
        UsbLog(LOG_INFO, "smbus: PS/2: pad stays on PS/2: %s\n", why);
        return FALSE;
    }
    /* the PS/2 side stays disabled: Synaptics with F5, Elan as the reset left it */
    if (info.Kind == SMB_PAD_SYNAPTICS) {
        Io->Send(Io->Ctx, 0xF5);
        OsSleepMs(30);                                  /* reset_delay_ms of synaptics.c */
    }
    ov = OsGetConfig("SmbusTouchpad", 0);
    addr = info.Kind == SMB_PAD_SYNAPTICS ? (UCHAR)(ov & 0xFF) : (UCHAR)((ov >> 8) & 0xFF);
    if (addr == 0) {
        addr = info.Kind == SMB_PAD_SYNAPTICS ? RMI_ADDR : ELAN_ADDR;
    }
    if (Probe(info.Kind, addr, &info) != NULL) {
        return TRUE;
    }
    UsbLog(LOG_INFO, "smbus: PS/2: the SMBus side of the pad does not answer at %02x, stays on PS/2\n", addr);
    return FALSE;
}

/* without the PS/2 identification: addresses from the registry, or pads that
   already answer on SMBus */
static void DirectProbe(void)
{
    ULONG ov = OsGetConfig("SmbusTouchpad", 0);

    DirectDone = TRUE;
    if (Mode == 0 || SmbHostCount == 0) {
        return;
    }
    if (ov != 0) {
        if (ov & 0xFF) Probe(SMB_PAD_SYNAPTICS, (UCHAR)(ov & 0xFF), NULL);
        if ((ov >> 8) & 0xFF) Probe(SMB_PAD_ELAN, (UCHAR)((ov >> 8) & 0xFF), NULL);
        return;
    }
    if (Ps2Seen) {
        return;                                         /* the PS/2 driver has decided */
    }
    if (!AcpiForcePad() && (Mode == 2 || AcpiWhitelisted())) {
        Probe(SMB_PAD_SYNAPTICS, RMI_ADDR, NULL);
    }
    if (Mode == 2) {
        Probe(SMB_PAD_ELAN, ELAN_ADDR, NULL);
    }
}

/* ------------------------------------------------------------------ */
/* polling                                                              */
/* ------------------------------------------------------------------ */

static void Failed(SMB_PAD *p, int r)
{
    p->Errors++;
    p->Step = 0;
    if (p->Errors > 50) {
        UsbLog(LOG_ERR, "smbus: %s does not answer (%d), retrying in 2 s\n", p->Name, r);
        p->RetryAt = OsTimeMs() + 2000;
        p->Errors = 0;
    }
}

/* starts the async read of the next RMI job of this poll; FALSE when none is left */
static BOOLEAN RmiNext(SMB_PAD *p)
{
    while (p->Job < JOBS) {
        USHORT addr;
        UCHAR len, code;
        int r;
        if (!(p->Pending & (1 << p->Job)) || !RmiJob(p, p->Job, &addr, &len) || len == 0) {
            p->Job++;
            continue;
        }
        r = RmiCode(p, addr, len, TRUE, &code);
        if (r == SMB_OK) r = SmbStartReadBlock(p->H, p->Port, p->Addr, code);
        if (r == SMB_OK) {
            p->Step = 2;
            return TRUE;
        }
        if (r == SMB_BUSY) {
            p->Step = 3;                                /* again on the next pass */
            return TRUE;
        }
        Failed(p, r);
        return TRUE;
    }
    return FALSE;
}

static void Service(SMB_PAD *p, ULONG Now, ULONG *Want)
{
    int r;

    if (p->Ptp != NULL && p->Mouse != NULL) {
        PtpTick(p->Ptp, p->Mouse);
    }
    if (p->RetryAt != 0) {
        if ((LONG)(Now - p->RetryAt) < 0 || !SmbHostIdle(p->H)) {
            return;
        }
        p->RetryAt = 0;
        if (!StartPad(p)) {
            p->RetryAt = Now + 2000;
            return;
        }
        UsbLog(LOG_INFO, "smbus: %s set up again\n", p->Name);
    }
    switch (p->Step) {
    case 0:
        if ((LONG)(Now - p->NextPoll) < 0 || !SmbHostIdle(p->H)) {
            return;
        }
        p->NextPoll = Now + ((Now - p->LastActive < 1000) ? 8 : 30);
        if (p->Kind == SMB_PAD_ELAN) {
            r = SmbStartReadBlock(p->H, p->Port, p->Addr, 0xA8);
        } else {
            UCHAR code;
            r = RmiCode(p, (USHORT)(p->F01.Data + 1), p->IrqRegs, TRUE, &code);
            if (r == SMB_OK) r = SmbStartReadBlock(p->H, p->Port, p->Addr, code);
        }
        if (r == SMB_BUSY) {
            p->NextPoll = Now;
            *Want = 2;
            return;
        }
        if (r != SMB_OK) {
            Failed(p, r);
            return;
        }
        p->Step = 1;
        *Want = 2;
        return;
    case 1:                                             /* Elan report / RMI interrupt status */
        r = SmbCollect(p->H, p->Buf);
        if (r == SMB_PENDING) {
            *Want = 2;
            return;
        }
        if (r < 0) {
            Failed(p, r);
            return;
        }
        p->Errors = 0;
        p->Step = 0;
        if (p->Kind == SMB_PAD_ELAN) {
            ElanReport(p, p->Buf, r);
            return;
        }
        {
            ULONG bits = 0;
            int i;
            for (i = 0; i < r && i < 4; i++) bits |= (ULONG)p->Buf[i] << (8 * i);
            bits &= p->IrqMask;
            if (bits & p->IrqF01) {
                /* device status: a reset of the pad makes it unconfigured - set it up again */
                int st = RmiRead1(p, p->F01.Data);
                if (st >= 0 && (st & 0x80)) {
                    UsbLog(LOG_INFO, "smbus: %s was reset\n", p->Name);
                    p->RetryAt = Now;
                    return;
                }
            }
            p->IrqBits = bits;
            p->Pending = (UCHAR)(((bits & p->Irq2D) ? 1 << JOB_2D : 0) |
                                 ((p->HasGpio && (bits & p->IrqGpio)) ? 1 << JOB_GPIO : 0) |
                                 ((p->HasF03 && (bits & p->IrqF03)) ? 1 << JOB_F03 : 0));
            p->Got = 0;
            p->Job = 0;
            if (p->Pending == 0) {
                return;
            }
            if (RmiNext(p)) {
                *Want = 2;
            }
        }
        return;
    case 2:                                             /* RMI job data */
        r = SmbCollect(p->H, p->JobBuf[p->Job]);
        if (r == SMB_PENDING) {
            *Want = 2;
            return;
        }
        if (r < 0) {
            Failed(p, r);
            return;
        }
        p->JobLen[p->Job] = (UCHAR)r;
        p->Got |= (UCHAR)(1 << p->Job);
        p->Job++;
        /* fall through */
    case 3:
        p->Step = 0;
        if (!SmbHostIdle(p->H)) {
            p->Step = 3;
            *Want = 2;
            return;
        }
        if (RmiNext(p)) {
            *Want = 2;
            return;
        }
        RmiCommit(p);
        return;
    }
}

void SmbusService(void)
{
    ULONG now = OsTimeMs(), want = 0;
    int i;

    if (!Inited) {
        return;
    }
    if (!DirectDone) {
        DirectProbe();
    }
    for (i = 0; i < SmbTpCount; i++) {
        if (!Pads[i]->Dead) {
            Service(Pads[i], now, &want);
        }
    }
    if (SmbTpCount != 0) {
        ULONG w = want ? want : 8;
        if (UsbPollWantMs == 0 || UsbPollWantMs > w) {
            UsbPollWantMs = w;
        }
    }
}

void SmbusShutdown(void)
{
    int i;

    for (i = 0; i < SmbTpCount; i++) {
        SMB_PAD *p = Pads[i];
        UCHAR tmp[SMB_BLOCK_MAX];
        if (!SmbHostIdle(p->H)) {
            while (SmbCollect(p->H, tmp) == SMB_PENDING) {
                OsStallUs(100);
            }
        }
        if (p->Mouse) HidExtRemove(p->Mouse);
        if (p->Stick) HidExtRemove(p->Stick);
        p->Mouse = p->Stick = NULL;
        p->Dead = 1;
    }
}

/* harness / diagnostics: one line per host and pad */
int SmbusDescribe(int Index, char *Buf, int Size)
{
    if (Index < SmbHostCount) {
        int n = UsbFormat(Buf, Size, "SMBHOST %d %s ", Index, SmbHostName(SmbHostGet(Index)));
        return n + SmbHostDescribe(SmbHostGet(Index), Buf + n, Size - n);
    }
    Index -= SmbHostCount;
    if (Index < SmbTpCount) {
        SMB_PAD *p = Pads[Index];
        return UsbFormat(Buf, Size, "SMBPAD %d %s kind=%s addr=%02x port=%u max=%ux%u objects=%u f2d=%u gpio=%u f03=%u "
                         "click=%u reports=%u frames=%u", Index, p->Name, p->Kind == SMB_PAD_SYNAPTICS ? "rmi4" : "elan",
                         p->Addr, p->Port, p->MaxX, p->MaxY, p->Objects, p->Is2D, p->HasGpio, p->HasF03, p->ClickPad,
                         p->Reports, p->Frames);
    }
    return 0;
}

/* user mode listing (usbtree): Kind 1 controller, 2 device, like I2cHidInfo */
BOOLEAN SmbusInfo(int Index, UCHAR *Kind, char *Name, int NameSize, char *Detail, int DetailSize)
{
    if (Index < SmbHostCount) {
        SMB_HOST *h = SmbHostGet(Index);
        char d[96];
        SmbHostDescribe(h, d, sizeof(d));
        *Kind = 1;
        UsbFormat(Name, NameSize, "%s", SmbHostName(h));
        UsbFormat(Detail, DetailSize, "SMBus host controller (polled)\n%s", d);
        return TRUE;
    }
    Index -= SmbHostCount;
    if (Index < SmbTpCount) {
        SMB_PAD *p = Pads[Index];
        *Kind = 2;
        UsbFormat(Name, NameSize, "%s touch pad on SMBus", p->Kind == SMB_PAD_SYNAPTICS ? "Synaptics" : "Elan");
        UsbFormat(Detail, DetailSize, "%s\nAddress %02X, port %u on %s, %ux%u, %u reports%s", p->Info, p->Addr, p->Port,
                  SmbHostName(p->H), p->MaxX, p->MaxY, p->Reports, p->Dead ? ", stopped" : "");
        return TRUE;
    }
    return FALSE;
}
