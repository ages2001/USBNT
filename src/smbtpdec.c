/*
 * smbtpdec.c - the parts of the SMBus touch pad support that only look at
 * bytes: the PS/2 identification of Synaptics and Elan pads, the RMI4 register
 * descriptors and report formats (F11, F12, F30 / F3A, F03), the Elan SMBus
 * report, standard PS/2 packets and Linux's InterTouch PNP id list.  No OS
 * calls, so test/host/tsmbtp.c builds it as it is.
 *
 * The formats follow Linux: drivers/input/mouse/synaptics.c, elantech.c,
 * elan_i2c_core.c, drivers/input/rmi4/rmi_driver.c, rmi_f11.c, rmi_f12.c,
 * rmi_f30.c, rmi_f3a.c, rmi_f03.c.
 */

#include "smbtp.h"

/* ------------------------------------------------------------------ */
/* PS/2 identification                                                  */
/* ------------------------------------------------------------------ */

static BOOLEAN Out(const SMB_PS2_IO *Io, UCHAR B)
{
    return Io->Send(Io->Ctx, B);
}

/* E9: three status / information bytes */
static BOOLEAN GetInfo(const SMB_PS2_IO *Io, UCHAR *R)
{
    int i, v;

    if (!Out(Io, 0xE9)) {
        return FALSE;
    }
    for (i = 0; i < 3; i++) {
        v = Io->Read(Io->Ctx, 100);
        if (v < 0) {
            return FALSE;
        }
        R[i] = (UCHAR)v;
    }
    return TRUE;
}

/* the Synaptics "special command": E6, then the command in four E8 slices */
static BOOLEAN Sliced(const SMB_PS2_IO *Io, UCHAR C)
{
    int i;

    if (!Out(Io, 0xE6)) {
        return FALSE;
    }
    for (i = 6; i >= 0; i -= 2) {
        if (!Out(Io, 0xE8) || !Out(Io, (UCHAR)((C >> i) & 3))) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOLEAN SynQuery(const SMB_PS2_IO *Io, UCHAR C, ULONG *Val)
{
    UCHAR r[3];

    if (!Sliced(Io, C) || !GetInfo(Io, r)) {
        return FALSE;
    }
    *Val = ((ULONG)r[0] << 16) | ((ULONG)r[1] << 8) | r[2];
    return TRUE;
}

/* Elantech hardware version 3 and 4 command: F8, the command, E9 */
static BOOLEAN EtpQuery(const SMB_PS2_IO *Io, UCHAR C, UCHAR *R)
{
    return (BOOLEAN)(Out(Io, 0xF8) && Out(Io, C) && GetInfo(Io, R));
}

#define SYN_ID_MAJOR(i)     ((i) & 0x0F)
#define SYN_ID_MINOR(i)     (((i) >> 16) & 0xFF)
#define SYN_ID_FULL(i)      ((SYN_ID_MAJOR(i) << 8) | SYN_ID_MINOR(i))

static void Synaptics(const SMB_PS2_IO *Io, SMB_PS2_INFO *O)
{
    ULONG id = 0, caps = 0, v;
    UCHAR bid[3];
    ULONG ext;

    if (!SynQuery(Io, 0x00, &id) || ((id >> 8) & 0xFF) != 0x47) {
        return;
    }
    O->Kind = SMB_PAD_SYNAPTICS;
    O->Identity = id;
    /* modes: board id, and whether the 0x10 query exists (firmware 7.5 and later) */
    if (SYN_ID_FULL(id) >= 0x705 && Sliced(Io, 0x01) && GetInfo(Io, bid)) {
        O->BoardId = ((ULONG)(bid[0] & 0xFC) << 6) | bid[1];
        if ((bid[0] & 0x02) && SynQuery(Io, 0x10, &v)) {
            O->ExtCap10 = v;
        }
    }
    if (!SynQuery(Io, 0x02, &caps)) {
        return;
    }
    /* older firmware had the sub model id fixed to 0x47 */
    if (SYN_ID_FULL(id) < 0x705 && ((caps >> 8) & 0xFF) != 0x47) {
        O->Kind = SMB_PAD_NONE;
        return;
    }
    if (!(caps & 0x800000)) {
        caps = 0;                                       /* capExtended clear: the rest means nothing */
    }
    O->Caps = caps;
    ext = (caps >> 20) & 7;
    if (ext >= 1 && SynQuery(Io, 0x09, &v)) {
        O->ExtCap = v;
    }
    if (ext >= 4 && SynQuery(Io, 0x0C, &v)) {
        O->ExtCap0c = v;
    }
    O->SmbusCapable = (UCHAR)((O->ExtCap0c & 0x4000) != 0);        /* InterTouch */
    O->ClickPad = (UCHAR)((O->ExtCap0c & 0x100000) != 0);
    O->StickButtons = (UCHAR)((O->ExtCap10 & 0x10000) != 0);
}

static BOOLEAN EtpSignatureOk(const UCHAR *P)
{
    static const UCHAR rates[] = { 200, 100, 80, 60, 40, 20, 10 };
    ULONG i;

    if (P[0] == 0) {
        return FALSE;
    }
    if (P[1] == 0) {
        return TRUE;
    }
    if ((P[0] & 0x0F) >= 0x06 && (P[1] & 0xAF) == 0x0F && P[2] < 40) {
        return TRUE;
    }
    for (i = 0; i < sizeof(rates); i++) {
        if (P[2] == rates[i]) {
            return FALSE;
        }
    }
    return TRUE;
}

static void Elantech(const SMB_PS2_IO *Io, SMB_PS2_INFO *O)
{
    UCHAR p[3];
    ULONG fw, ic;

    /* the magic knock: F6, F5, E6 E6 E6, E9 -> 3C 03 C8 / 3C 03 00 */
    Out(Io, 0xF6);
    if (!Out(Io, 0xF5) || !Out(Io, 0xE6) || !Out(Io, 0xE6) || !Out(Io, 0xE6) || !GetInfo(Io, p)) {
        return;
    }
    if (p[0] != 0x3C || p[1] != 0x03 || (p[2] != 0xC8 && p[2] != 0x00)) {
        return;
    }
    if (!Sliced(Io, 0x01) || !GetInfo(Io, p) || !EtpSignatureOk(p)) {
        return;
    }
    fw = ((ULONG)p[0] << 16) | ((ULONG)p[1] << 8) | p[2];
    O->Kind = SMB_PAD_ELAN;
    O->FwVersion = fw;
    ic = (fw >> 16) & 0x0F;
    if (fw < 0x020030 || fw == 0x020600) {
        O->HwVersion = 1;
    } else if (ic == 2 || ic == 4) {
        O->HwVersion = 2;
    } else if (ic == 5) {
        O->HwVersion = 3;
    } else if (ic >= 6) {
        O->HwVersion = 4;
    }
    O->NewIcHostNotify = (UCHAR)((fw & 0x0F2000) == 0x0F2000 && (fw & 0xFF) > 0);
    /* hardware version 4: the bus the pad offers is in the resolution query */
    if (O->HwVersion == 4 && EtpQuery(Io, 0x04, p)) {
        O->Bus = p[2];
    }
    O->SmbusCapable = (UCHAR)(O->NewIcHostNotify || (O->Bus >= 1 && O->Bus <= 4));
    O->ClickPad = (UCHAR)((fw & 0x001000) != 0);
}

void SmbPs2Identify(const SMB_PS2_IO *Io, SMB_PS2_INFO *Out_)
{
    UCHAR r[3];
    int i;

    UsbMemSet(Out_, 0, sizeof(*Out_));
    /* Synaptics: E8 00 four times, E9: the middle byte is 47h */
    for (i = 0; i < 4; i++) {
        if (!Out(Io, 0xE8) || !Out(Io, 0x00)) {
            return;
        }
    }
    if (GetInfo(Io, r) && r[1] == 0x47) {
        Synaptics(Io, Out_);
        return;
    }
    Elantech(Io, Out_);
}

/* ------------------------------------------------------------------ */
/* RMI4 register descriptors                                            */
/* ------------------------------------------------------------------ */

ULONG RmiDescStructSize(const UCHAR *Presence, ULONG PresenceLen)
{
    if (PresenceLen == 0) {
        return 0;
    }
    if (Presence[0] == 0) {
        return PresenceLen >= 3 ? GET16(Presence + 1) : 0;
    }
    return Presence[0];
}

int RmiParseRegDesc(const UCHAR *Presence, ULONG PresenceLen, const UCHAR *Struct, ULONG StructLen, RMI_REG_DESC *D)
{
    ULONG po = (PresenceLen > 0 && Presence[0] == 0) ? 3 : 1, i, b, off = 0;

    UsbMemSet(D, 0, sizeof(*D));
    if (PresenceLen < po) {
        return -1;
    }
    for (i = po; i < PresenceLen; i++) {
        for (b = 0; b < 8; b++) {
            ULONG reg = (i - po) * 8 + b, size, map = 0, nsub = 0, mo = 0;
            if (!(Presence[i] & (1 << b))) {
                continue;
            }
            if (off >= StructLen) {
                return 0;
            }
            size = Struct[off++];
            if (size == 0) {
                if (off + 2 > StructLen) return -1;
                size = GET16(Struct + off);
                off += 2;
            }
            if (size == 0) {
                if (off + 4 > StructLen) return -1;
                size = GET32(Struct + off);
                off += 4;
            }
            do {
                ULONG k;
                if (off >= StructLen) return -1;
                for (k = 0; k < 7; k++, mo++) {
                    if (Struct[off] & (1 << k)) {
                        nsub++;
                        if (mo < 32) map |= 1UL << mo;
                    }
                }
            } while (Struct[off++] & 0x80);
            if (reg < RMI_DESC_REGS && size != 0) {
                D->Present[reg / 8] |= (UCHAR)(1 << (reg % 8));
                D->Size[reg] = (USHORT)(size > 0xFFFF ? 0xFFFF : size);
                D->Subpackets[reg] = (UCHAR)(nsub > 255 ? 255 : nsub);
                D->SubMap[reg] = map;
            }
        }
    }
    return 0;
}

BOOLEAN RmiDescHas(const RMI_REG_DESC *D, ULONG Reg)
{
    return (BOOLEAN)(Reg < RMI_DESC_REGS && (D->Present[Reg / 8] & (1 << (Reg % 8))));
}

int RmiDescIndex(const RMI_REG_DESC *D, ULONG Reg)
{
    ULONG r;
    int n = 0;

    if (!RmiDescHas(D, Reg)) {
        return -1;
    }
    for (r = 0; r < Reg; r++) {
        if (RmiDescHas(D, r)) n++;
    }
    return n;
}

ULONG RmiDescByteOffset(const RMI_REG_DESC *D, ULONG Reg)
{
    ULONG r, o = 0;

    for (r = 0; r < Reg && r < RMI_DESC_REGS; r++) {
        if (RmiDescHas(D, r)) o += D->Size[r];
    }
    return o;
}

/* ------------------------------------------------------------------ */
/* RMI4 reports                                                         */
/* ------------------------------------------------------------------ */

static void AddContact(SMB_FRAME *F, int Id, LONG X, LONG Y, LONG MaxX, LONG MaxY, BOOLEAN FlipY)
{
    SMB_CONTACT *c;

    if (F->N >= SMB_MAX_CONTACTS) {
        return;
    }
    if (FlipY) Y = MaxY - Y;
    if (X < 0) X = 0;
    if (Y < 0) Y = 0;
    if (MaxX > 0 && X > MaxX) X = MaxX;
    if (MaxY > 0 && Y > MaxY) Y = MaxY;
    c = &F->C[F->N++];
    c->Down = 1;
    c->Id = (UCHAR)Id;
    c->X = X;
    c->Y = Y;
}

void RmiF12Decode(const UCHAR *Data1, ULONG Len, int Objects, LONG MaxX, LONG MaxY, BOOLEAN FlipY, SMB_FRAME *F)
{
    int i;

    F->N = 0;
    for (i = 0; i < Objects && (ULONG)(i + 1) * 8 <= Len; i++) {
        const UCHAR *o = Data1 + i * 8;
        /* finger 1, stylus 2, unclassified 4 count; a palm (3) and nothing (0) do not */
        if (o[0] == 0x01 || o[0] == 0x02 || o[0] == 0x04) {
            AddContact(F, i, GET16(o + 1), GET16(o + 3), MaxX, MaxY, FlipY);
        }
    }
}

void RmiF11Decode(const UCHAR *Data, ULONG Len, int Fingers, LONG MaxX, LONG MaxY, BOOLEAN FlipY, SMB_FRAME *F)
{
    ULONG fs = (ULONG)(Fingers + 3) / 4;
    int i;

    F->N = 0;
    for (i = 0; i < Fingers; i++) {
        const UCHAR *p = Data + fs + i * 5;
        UCHAR state;
        if (fs + (ULONG)(i + 1) * 5 > Len) {
            break;
        }
        state = (UCHAR)((Data[i / 4] >> (2 * (i % 4))) & 3);
        if (state == 1) {                               /* present (2 = inaccurate, 3 = reserved) */
            AddContact(F, i, ((LONG)p[0] << 4) | (p[2] & 0x0F), ((LONG)p[1] << 4) | (p[2] >> 4), MaxX, MaxY, FlipY);
        }
    }
}

UCHAR RmiGpioButtons(const UCHAR *Data, ULONG Count, const UCHAR *Map, UCHAR *Stick)
{
    UCHAR b = 0, s = 0;
    ULONG i;

    for (i = 0; i < Count; i++) {
        BOOLEAN down = (BOOLEAN)!(Data[i / 8] & (1 << (i % 8)));        /* low = pressed */
        if (!down || Map[i] == 0) {
            continue;
        }
        if (Map[i] & 0x80) {
            s |= (UCHAR)(1 << ((Map[i] & 3) - 1));
        } else {
            b |= (UCHAR)(1 << ((Map[i] & 3) - 1));
        }
    }
    if (Stick != NULL) {
        *Stick = s;
    }
    return b;
}

int RmiF03Bytes(const UCHAR *Obs, ULONG Len, UCHAR *Out_)
{
    ULONG i;
    int n = 0;

    for (i = 0; i + 1 < Len; i += 2) {
        if (!(Obs[i] & 0x01)) {
            continue;                                   /* no data in this buffer */
        }
        if (Obs[i] & 0xC0) {
            continue;                                   /* timeout or parity error */
        }
        Out_[n++] = Obs[i + 1];
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Elan                                                                 */
/* ------------------------------------------------------------------ */

int ElanDecode(const UCHAR *Rep, ULONG Len, LONG MaxX, LONG MaxY, SMB_FRAME *F, LONG *Dx, LONG *Dy, UCHAR *StickButtons)
{
    if (Len < 1) {
        return 0;
    }
    if (Rep[0] == 0x5D || Rep[0] == 0x60) {
        BOOLEAN hp = (BOOLEAN)(Rep[0] == 0x60);
        const UCHAR *fd = Rep + 2;
        UCHAR info;
        int i;
        if (Len < 2 + 5 * 5) {
            return 0;
        }
        info = Rep[1];
        F->N = 0;
        for (i = 0; i < 5; i++) {
            LONG x, y;
            if (!(info & (1 << (3 + i)))) {
                continue;
            }
            if (hp) {
                x = GETBE16(fd);
                y = GETBE16(fd + 2);
            } else {
                x = ((LONG)(fd[0] & 0xF0) << 4) | fd[1];
                y = ((LONG)(fd[0] & 0x0F) << 8) | fd[2];
            }
            fd += 5;
            if (x > MaxX || y > MaxY) {
                continue;                               /* Linux drops these */
            }
            AddContact(F, i, x, y, MaxX, MaxY, TRUE);   /* Elan counts Y upwards */
        }
        F->Buttons = (UCHAR)(info & 7);                 /* bit 0 left, 1 right, 2 middle */
        return ELAN_REP_TOUCH;
    }
    if (Rep[0] == 0x5E || Rep[0] == 0x5F) {
        const UCHAR *p = Rep + 1;
        if (Len < 7) {
            return 0;
        }
        *StickButtons = (UCHAR)(p[0] & 7);
        *Dx = *Dy = 0;
        if ((p[3] & 0x0F) == 0x06) {
            *Dx = (LONG)p[4] - (LONG)((p[1] ^ 0x80) << 1);
            *Dy = (LONG)((p[2] ^ 0x80) << 1) - (LONG)p[5];
        }
        return ELAN_REP_STICK;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* PS/2 packets (trackpoint behind the pad)                              */
/* ------------------------------------------------------------------ */

BOOLEAN SmbPs2Packet(SMB_PS2_PKT *P, UCHAR Byte, LONG *Dx, LONG *Dy, UCHAR *Buttons)
{
    if (P->N == 0 && (!(Byte & 0x08) || (Byte & 0xC0))) {
        return FALSE;                                   /* not a first byte (ACK FA, AA 00, lost sync) */
    }
    P->B[P->N++] = Byte;
    if (P->N < 3) {
        return FALSE;
    }
    P->N = 0;
    if (P->B[0] & 0xC0) {
        return FALSE;                                   /* overflow */
    }
    *Dx = (LONG)P->B[1] - ((P->B[0] & 0x10) ? 256 : 0);
    *Dy = -((LONG)P->B[2] - ((P->B[0] & 0x20) ? 256 : 0));
    *Buttons = (UCHAR)(P->B[0] & 7);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* PNP ids                                                              */
/* ------------------------------------------------------------------ */

/* synaptics.c: topbuttonpad_pnp_ids and smbus_pnp_ids */
static const char *const IntertouchIds[] = {
    "LEN0017", "LEN0018", "LEN0019", "LEN0023", "LEN002A", "LEN002B", "LEN002C", "LEN002D", "LEN002E",
    "LEN0033", "LEN0034", "LEN0035", "LEN0036", "LEN0037", "LEN0038", "LEN0039", "LEN0041", "LEN0042",
    "LEN0045", "LEN0047", "LEN2000", "LEN2001", "LEN2002", "LEN2003", "LEN2004", "LEN2005", "LEN2006",
    "LEN2007", "LEN2008", "LEN2009", "LEN200A", "LEN200B",
    "DLL0597", "DLL060d", "LEN0048", "LEN0046", "LEN0049", "LEN004a", "LEN005b", "LEN005e", "LEN006c",
    "LEN007a", "LEN0071", "LEN0072", "LEN0073", "LEN0091", "LEN0092", "LEN0093", "LEN0096", "LEN0097",
    "LEN0099", "LEN009b", "LEN0402", "LEN040f", "LEN0411", "LEN200f", "LEN2044", "LEN2054", "LEN2055",
    "LEN2058", "LEN205b", "LEN2068", "SYN1221", "SYN3003", "SYN3015", "SYN3052", "SYN3221", "SYN323d",
    "SYN3257", "TOS01f6", "TOS0213", NULL
};
static const char *const ForcepadIds[] = { "SYN300D", "SYN3014", NULL };

static BOOLEAN IdEq(const char *A, const char *B)
{
    for (; *A && *B; A++, B++) {
        char a = *A, b = *B;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b) return FALSE;
    }
    return (BOOLEAN)(*A == 0 && *B == 0);
}

static BOOLEAN InIds(const char *Id, const char *const *L)
{
    for (; *L != NULL; L++) {
        if (IdEq(Id, *L)) return TRUE;
    }
    return FALSE;
}

BOOLEAN SmbPnpWhitelisted(const char *Id)
{
    return InIds(Id, IntertouchIds);
}

BOOLEAN SmbPnpForcePad(const char *Id)
{
    return InIds(Id, ForcepadIds);
}
