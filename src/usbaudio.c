/*
 * usbaudio.c - USB Audio Class 1.0 and 2.0 streaming and USB MIDI output.
 *
 * Attach parses the audio control topology (terminals, units, clock
 * entities), the streaming alternate settings and MIDI streaming
 * interfaces, unmutes the feature units on the playback and capture paths
 * and sets them to 0 dB. A client (the AUDIONT sound driver) is told about
 * the device from the worker context and opens a stream: the class picks
 * the alternate setting closest to the requested format, sets the sample
 * rate (UAC1 endpoint control, UAC2 clock source / selector) and keeps
 * isochronous transfers in flight from then on. Playback packet sizes
 * follow the explicit feedback endpoint of asynchronous devices.
 */

#include "usbaudio.h"

/* audio control descriptor subtypes */
#define AC_HEADER           0x01
#define AC_INPUT            0x02
#define AC_OUTPUT           0x03
#define AC_MIXER            0x04
#define AC_SELECTOR         0x05
#define AC_FEATURE          0x06
#define AC1_PROCESSING      0x07
#define AC1_EXTENSION       0x08
#define AC2_EFFECT          0x07
#define AC2_PROCESSING      0x08
#define AC2_EXTENSION       0x09
#define AC2_CLOCK_SOURCE    0x0A
#define AC2_CLOCK_SELECTOR  0x0B
#define AC2_CLOCK_MULT      0x0C

#define AS_GENERAL          0x01
#define AS_FORMAT_TYPE      0x02
#define USB_DT_CS_INTERFACE 0x24
#define USB_DT_CS_ENDPOINT  0x25
#define USB_DT_IAD          0x0B

/* class requests */
#define UAC1_SET_CUR        0x01
#define UAC1_GET_CUR        0x81
#define UAC1_GET_MIN        0x82
#define UAC1_GET_MAX        0x83
#define UAC1_GET_RES        0x84
#define UAC2_CUR            0x01
#define UAC2_RANGE          0x02

#define TT_USB_STREAMING    0x0101

#define UA_MAX_ENT          64
#define UA_NX               12          /* transfers per stream at most */
#define UA_NFB              3
#define MIDI_RING           1024        /* event packets */

typedef struct _UA_ENT {
    UCHAR           Id;
    UCHAR           Kind;
    UCHAR           NIn;
    UCHAR           Clock;              /* UAC2 terminals */
    UCHAR           In[16];
    USHORT          TermType;
    UCHAR           Channels;
    UCHAR           Pad;
    const UCHAR    *D;
} UA_ENT;

typedef struct _UA_ALT {
    UA_FORMAT       Fmt;
    UCHAR           Iface;
    UCHAR           Terminal;           /* bTerminalLink */
    UCHAR           FreqCtl;            /* UAC1: endpoint sampling frequency control */
    UCHAR           Pad;
    const UCHAR    *Ep;
    const UCHAR    *Fb;
} UA_ALT;

struct _UA_STREAM;

typedef struct _UA_XFER {
    USB_XFER        X;
    USB_ISO_PACKET *Pkt;
    UCHAR          *Buf;
    ULONG           Phys;
    ULONG           Size;
    ULONG           NPkt;
    struct _UA_STREAM *S;
    UCHAR           Busy;
    UCHAR           IsFb;
} UA_XFER;

typedef struct _UA_STREAM {
    struct _UA_DEV *U;
    UCHAR           Dir;
    UCHAR           Open;
    UCHAR           Running;
    UCHAR           Stopping;
    UCHAR           HasFb;
    UCHAR           Pad[3];
    UA_ALT         *Alt;
    UA_STREAM_INFO  Info;
    UA_STREAM_OPS   Ops;
    USB_PIPE        Pipe;
    USB_PIPE        FbPipe;
    ULONG           FrameBytes;
    ULONG           Slots;              /* microframes per packet */
    ULONG           PktPerXfer;
    ULONG           NX;                 /* transfers in flight */
    ULONG           NXAlloc;            /* transfers allocated: the queue grows when it runs dry */
    ULONG           MaxFrames;          /* per packet */
    ULONG           Nominal;            /* Q16.16 frames per packet */
    ULONG           Current;
    ULONG           Acc;
    ULONG           Pending;
    ULONG           LastDone;
    ULONG           FbLogged;
    ULONG           Measured;           /* capture: Q16.16 frames per packet the device delivers */
    UA_XFER         Xf[UA_NX];
    UA_XFER         Fbx[UA_NFB];
    UA_STATS        Stats;
} UA_STREAM;

typedef struct _UA_MIDI {
    UCHAR           Iface;
    UCHAR           Cables;
    UCHAR           Busy;
    UCHAR           Opened;
    USB_PIPE        Pipe;
    USB_XFER        X;
    UCHAR          *Buf;
    ULONG           BufPhys;
    ULONG           Ring[MIDI_RING];
    ULONG           Head, Tail;
    /* parser state per cable */
    UCHAR           Running[16];
    UCHAR           Msg[16][3];
    UCHAR           Have[16];
    UCHAR           Need[16];
    UCHAR           Sysex[16];
} UA_MIDI;

typedef struct _UA_DEV {
    USB_DEV        *Dev;
    ULONG           Id;
    UCHAR           Gone;
    UCHAR           Notified;
    UCHAR           AcIf;
    UCHAR           Uac;
    UA_ENT          Ent[UA_MAX_ENT];
    ULONG           NEnt;
    UA_ALT          Alts[2][UA_MAX_FORMATS];
    ULONG           NAlt[2];
    UCHAR           AsIf[2];
    UCHAR           Term[2];            /* USB streaming terminal of each direction */
    UCHAR           Fu[2][8];           /* feature units on the path, closest to the jack first */
    UCHAR           NFu[2];
    UCHAR           VolFu[2];           /* unit carrying the volume control, 0 = none */
    UCHAR           VolCh[2];           /* 0 = master control, else per channel */
    UCHAR           VolChannels[2];
    UCHAR           SelUnit[2][4];      /* selectors on the path and the pin to choose */
    UCHAR           SelPin[2][4];
    UCHAR           NSel[2];
    UCHAR           InputTerm[UA_MAX_INPUTS];
    UCHAR           InSelUnit[UA_MAX_INPUTS];
    UCHAR           InSelPin[UA_MAX_INPUTS];
    /* deferred control requests */
    UCHAR           VolPending[2];
    UCHAR           VolMute[2];
    UCHAR           SelPending;
    UCHAR           SelIndex;
    SHORT           VolL[2], VolR[2];
    UA_INFO         Info;
    UA_STREAM       S[2];
    UA_MIDI        *Midi;
} UA_DEV;

static UA_DEV *UaDevs[UA_MAX_DEVS];
static UCHAR UaGen[UA_MAX_DEVS];
static UA_CLIENT UaClient;
static volatile UCHAR UaClientSet;
static ULONG UaLatencyMs;
static ULONG TickLast, TickMaxGap;

/* ------------------------------------------------------------------ */
/* Handles                                                              */
/* ------------------------------------------------------------------ */

static UA_DEV *Lookup(ULONG Id)
{
    ULONG i = (Id & 0xFF) - 1;
    UA_DEV *u;

    if (i >= UA_MAX_DEVS) {
        return NULL;
    }
    u = UaDevs[i];
    if (u == NULL || u->Id != Id || u->Gone) {
        return NULL;
    }
    return u;
}

ULONG UaDeviceCount(void)
{
    ULONG i, n = 0;

    for (i = 0; i < UA_MAX_DEVS; i++) {
        if (UaDevs[i] != NULL && !UaDevs[i]->Gone) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Class requests                                                       */
/* ------------------------------------------------------------------ */

static int AcGet(UA_DEV *u, UCHAR Req, UCHAR Ent, UCHAR Cs, UCHAR Cn, void *Buf, USHORT Len, ULONG *Got)
{
    return UsbControl(u->Dev, USB_RT_IN | USB_RT_CLASS | USB_RT_INTERFACE, Req, (USHORT)((Cs << 8) | Cn),
                      (USHORT)((Ent << 8) | u->AcIf), Len, Buf, 1000, Got);
}

static int AcSet(UA_DEV *u, UCHAR Req, UCHAR Ent, UCHAR Cs, UCHAR Cn, void *Buf, USHORT Len)
{
    return UsbControl(u->Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_INTERFACE, Req, (USHORT)((Cs << 8) | Cn),
                      (USHORT)((Ent << 8) | u->AcIf), Len, Buf, 1000, NULL);
}

static UA_ENT *Ent(UA_DEV *u, UCHAR Id)
{
    ULONG i;

    for (i = 0; i < u->NEnt; i++) {
        if (u->Ent[i].Id == Id) {
            return &u->Ent[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Topology                                                             */
/* ------------------------------------------------------------------ */

static void AddEnt(UA_DEV *u, const UCHAR *d)
{
    UA_ENT *e;
    ULONG n = 0, i;
    UCHAR st = d[2];
    BOOLEAN v2 = (BOOLEAN)(u->Uac == 2);

    if (u->NEnt >= UA_MAX_ENT || d[0] < 4) {
        return;
    }
    e = &u->Ent[u->NEnt];
    UsbMemSet(e, 0, sizeof(*e));
    e->Id = d[3];
    e->Kind = st;
    e->D = d;
    switch (st) {
    case AC_INPUT:
        if (d[0] >= 8) {
            e->TermType = GET16(d + 4);
            if (v2 && d[0] >= 17) {
                e->Clock = d[7];
                e->Channels = d[8];
            } else if (!v2 && d[0] >= 12) {
                e->Channels = d[7];
            }
        }
        break;
    case AC_OUTPUT:
        if (d[0] >= 9) {
            e->TermType = GET16(d + 4);
            e->In[n++] = d[7];
            if (v2 && d[0] >= 12) {
                e->Clock = d[8];
            }
        }
        break;
    case AC_MIXER:
    case AC_SELECTOR:
        if (d[0] >= 5) {
            for (i = 0; i < d[4] && 5 + i < d[0] && n < 16; i++) {
                e->In[n++] = d[5 + i];
            }
        }
        break;
    case AC_FEATURE:
        if (d[0] >= 5) {
            e->In[n++] = d[4];
        }
        break;
    default:
        if (v2 && st == AC2_EFFECT && d[0] >= 7) {
            e->In[n++] = d[6];
        } else if ((v2 && (st == AC2_PROCESSING || st == AC2_EXTENSION)) ||
                   (!v2 && (st == AC1_PROCESSING || st == AC1_EXTENSION))) {
            if (d[0] >= 7) {
                for (i = 0; i < d[6] && 7 + i < d[0] && n < 16; i++) {
                    e->In[n++] = d[7 + i];
                }
            }
        } else if (v2 && st == AC2_CLOCK_SELECTOR && d[0] >= 5) {
            for (i = 0; i < d[4] && 5 + i < d[0] && n < 16; i++) {
                e->In[n++] = d[5 + i];
            }
        } else if (v2 && st == AC2_CLOCK_MULT && d[0] >= 5) {
            e->In[n++] = d[4];
        } else if (!(v2 && st == AC2_CLOCK_SOURCE)) {
            return;
        }
        break;
    }
    e->NIn = (UCHAR)n;
    u->NEnt++;
}

/*
 * Depth first search from entity Id back to entity Target. Records feature
 * units (closest to Id first) and the selector pins taken.
 */
static BOOLEAN PathTo(UA_DEV *u, UCHAR Id, UCHAR Target, UCHAR *Fu, UCHAR *NFu, UCHAR *SelU, UCHAR *SelP,
                      UCHAR *NSel, int Depth)
{
    UA_ENT *e = Ent(u, Id);
    ULONG i;
    UCHAR nfu = *NFu, nsel = *NSel;

    if (e == NULL || Depth > 16) {
        return FALSE;
    }
    if (Id == Target) {
        return TRUE;
    }
    if (e->Kind == AC_FEATURE && *NFu < 8) {
        Fu[(*NFu)++] = Id;
    }
    for (i = 0; i < e->NIn; i++) {
        if (e->Kind == AC_SELECTOR && *NSel < 4) {
            SelU[*NSel] = Id;
            SelP[*NSel] = (UCHAR)(i + 1);
            (*NSel)++;
        }
        if (PathTo(u, e->In[i], Target, Fu, NFu, SelU, SelP, NSel, Depth + 1)) {
            return TRUE;
        }
        *NFu = (UCHAR)(nfu + ((e->Kind == AC_FEATURE && nfu < 8) ? 1 : 0));
        *NSel = nsel;
    }
    *NFu = nfu;
    return FALSE;
}

static int TermRank(USHORT t, BOOLEAN Out)
{
    if (Out) {
        switch (t) {
        case 0x0301: return 1;      /* speaker */
        case 0x0302: return 1;      /* headphones */
        case 0x0402: return 2;      /* headset */
        case 0x0603: return 2;      /* line connector */
        default:     return (t >= 0x0300 && t < 0x0800) ? 3 : 4;
        }
    }
    if (t >= 0x0201 && t <= 0x0206) return 1;  /* microphones */
    if (t == 0x0402 || t == 0x0403) return 1;   /* headset / speakerphone */
    if (t == 0x0603 || t == 0x0601) return 2;   /* line in */
    return 3;
}

static void BuildPaths(UA_DEV *u)
{
    ULONG i;
    int best;

    /* playback: an output jack reachable from the USB streaming input terminal */
    if (u->Term[UA_PLAY]) {
        best = 99;
        for (i = 0; i < u->NEnt; i++) {
            UA_ENT *e = &u->Ent[i];
            UCHAR fu[8], su[4], sp[4], nfu = 0, ns = 0;
            int r;
            if (e->Kind != AC_OUTPUT || e->TermType == TT_USB_STREAMING) {
                continue;
            }
            r = TermRank(e->TermType, TRUE);
            if (r < best && PathTo(u, e->Id, u->Term[UA_PLAY], fu, &nfu, su, sp, &ns, 0)) {
                best = r;
                UsbMemCpy(u->Fu[UA_PLAY], fu, sizeof(fu));
                u->NFu[UA_PLAY] = nfu;
                UsbMemCpy(u->SelUnit[UA_PLAY], su, sizeof(su));
                UsbMemCpy(u->SelPin[UA_PLAY], sp, sizeof(sp));
                u->NSel[UA_PLAY] = ns;
            }
        }
    }
    /* capture: every physical input reachable from the USB streaming output terminal */
    if (u->Term[UA_REC]) {
        best = 99;
        for (i = 0; i < u->NEnt; i++) {
            UA_ENT *e = &u->Ent[i];
            UCHAR fu[8], su[4], sp[4], nfu = 0, ns = 0;
            int r;
            if (e->Kind != AC_INPUT || e->TermType == TT_USB_STREAMING) {
                continue;
            }
            if (!PathTo(u, u->Term[UA_REC], e->Id, fu, &nfu, su, sp, &ns, 0)) {
                continue;
            }
            if (u->Info.NumInputs < UA_MAX_INPUTS) {
                ULONG k = u->Info.NumInputs++;
                u->Info.InputType[k] = e->TermType;
                u->InputTerm[k] = e->Id;
                u->InSelUnit[k] = ns ? su[ns - 1] : 0;
                u->InSelPin[k] = ns ? sp[ns - 1] : 0;
            }
            r = TermRank(e->TermType, FALSE);
            if (r < best) {
                best = r;
                UsbMemCpy(u->Fu[UA_REC], fu, sizeof(fu));
                u->NFu[UA_REC] = nfu;
                UsbMemCpy(u->SelUnit[UA_REC], su, sizeof(su));
                UsbMemCpy(u->SelPin[UA_REC], sp, sizeof(sp));
                u->NSel[UA_REC] = ns;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Feature units                                                        */
/* ------------------------------------------------------------------ */

/* control bitmap of channel Ch (0 = master) */
static ULONG FuControls(UA_DEV *u, UA_ENT *e, ULONG Ch, ULONG *Channels)
{
    const UCHAR *d = e->D;

    if (u->Uac == 2) {
        ULONG n = (d[0] >= 6) ? (d[0] - 6) / 4 : 0;
        *Channels = n ? n - 1 : 0;
        if (Ch >= n) {
            return 0;
        }
        return GET32(d + 5 + 4 * Ch);
    } else {
        ULONG sz = d[5];
        ULONG n, v = 0, k;
        if (sz == 0 || d[0] < 7) {
            *Channels = 0;
            return 0;
        }
        n = (d[0] - 7) / sz;
        *Channels = n ? n - 1 : 0;
        if (Ch >= n) {
            return 0;
        }
        for (k = 0; k < sz && k < 4; k++) {
            v |= (ULONG)d[6 + Ch * sz + k] << (8 * k);
        }
        return v;
    }
}

static BOOLEAN HasMute(UA_DEV *u, ULONG c)
{
    return (u->Uac == 2) ? (BOOLEAN)((c & 3) == 3) : (BOOLEAN)((c & 1) != 0);
}

static BOOLEAN HasVol(UA_DEV *u, ULONG c)
{
    return (u->Uac == 2) ? (BOOLEAN)(((c >> 2) & 3) == 3) : (BOOLEAN)((c & 2) != 0);
}

static int GetVolRange(UA_DEV *u, UCHAR Fu, UCHAR Ch, SHORT *Min, SHORT *Max, SHORT *Res)
{
    UCHAR b[8];
    ULONG got = 0;

    if (u->Uac == 2) {
        if (AcGet(u, UAC2_RANGE, Fu, 2, Ch, b, 8, &got) != USB_OK || got < 8) {
            return USB_ERR_IO;
        }
        *Min = (SHORT)GET16(b + 2);
        *Max = (SHORT)GET16(b + 4);
        *Res = (SHORT)GET16(b + 6);
        return USB_OK;
    }
    if (AcGet(u, UAC1_GET_MIN, Fu, 2, Ch, b, 2, &got) != USB_OK || got < 2) {
        return USB_ERR_IO;
    }
    *Min = (SHORT)GET16(b);
    if (AcGet(u, UAC1_GET_MAX, Fu, 2, Ch, b, 2, &got) != USB_OK || got < 2) {
        return USB_ERR_IO;
    }
    *Max = (SHORT)GET16(b);
    if (AcGet(u, UAC1_GET_RES, Fu, 2, Ch, b, 2, &got) != USB_OK || got < 2) {
        b[0] = 1;
        b[1] = 0;
    }
    *Res = (SHORT)GET16(b);
    return USB_OK;
}

static void SetVol(UA_DEV *u, UCHAR Fu, UCHAR Ch, SHORT V)
{
    UCHAR b[2];

    PUT16(b, (USHORT)V);
    AcSet(u, u->Uac == 2 ? UAC2_CUR : UAC1_SET_CUR, Fu, 2, Ch, b, 2);
}

static void SetMute(UA_DEV *u, UCHAR Fu, UCHAR Ch, BOOLEAN M)
{
    UCHAR b[1];

    b[0] = (UCHAR)(M ? 1 : 0);
    AcSet(u, u->Uac == 2 ? UAC2_CUR : UAC1_SET_CUR, Fu, 1, Ch, b, 1);
}

static SHORT Clamp(LONG v, SHORT Min, SHORT Max)
{
    if (Min > Max) {
        return Min;
    }
    if (v < Min) {
        return Min;
    }
    if (v > Max) {
        return Max;
    }
    return (SHORT)v;
}

/*
 * Unmutes every feature unit of a path and sets volumes to 0 dB (or the
 * nearest value the unit has). The unit closest to the jack that has a
 * volume control becomes the hardware volume of the direction.
 */
static void InitPath(UA_DEV *u, ULONG Dir)
{
    ULONG k;

    for (k = 0; k < u->NFu[Dir]; k++) {
        UA_ENT *e = Ent(u, u->Fu[Dir][k]);
        ULONG chans = 0, ch;
        if (e == NULL) {
            continue;
        }
        FuControls(u, e, 0, &chans);
        for (ch = 0; ch <= chans && ch <= 8; ch++) {
            ULONG c = FuControls(u, e, ch, &chans);
            if (HasMute(u, c)) {
                SetMute(u, e->Id, (UCHAR)ch, FALSE);
                if (Dir < 2) {
                    u->Info.HasMute[Dir] = 1;
                }
            }
            if (HasVol(u, c)) {
                SHORT mn = 0, mx = 0, rs = 0;
                if (GetVolRange(u, e->Id, (UCHAR)ch, &mn, &mx, &rs) == USB_OK) {
                    SetVol(u, e->Id, (UCHAR)ch, Clamp(0, mn, mx));
                    if (u->VolFu[Dir] == 0 || (u->VolFu[Dir] == e->Id && u->VolCh[Dir] != 0 && ch == 0)) {
                        u->VolFu[Dir] = e->Id;
                        u->VolCh[Dir] = (UCHAR)ch;
                        u->VolChannels[Dir] = (UCHAR)chans;
                        u->Info.HasVolume[Dir] = 1;
                        u->Info.VolMin[Dir] = mn;
                        u->Info.VolMax[Dir] = mx;
                        u->Info.VolRes[Dir] = rs;
                    }
                } else {
                    SetVol(u, e->Id, (UCHAR)ch, 0);
                }
            }
        }
    }
    for (k = 0; k < u->NSel[Dir]; k++) {
        UCHAR v = u->SelPin[Dir][k];
        AcSet(u, u->Uac == 2 ? UAC2_CUR : UAC1_SET_CUR, u->SelUnit[Dir][k], (UCHAR)(u->Uac == 2 ? 1 : 0), 0, &v, 1);
    }
    UsbLog(LOG_INFO, "audio %u: %s path: %u feature unit(s), volume unit %u (%d..%d/256 dB), %u selector(s)\n",
           u->Dev->Index, Dir == UA_PLAY ? "playback" : "capture", u->NFu[Dir], u->VolFu[Dir],
           u->Info.VolMin[Dir], u->Info.VolMax[Dir], u->NSel[Dir]);
}

static void ApplyVolume(UA_DEV *u, ULONG Dir)
{
    UA_ENT *e = Ent(u, u->VolFu[Dir]);
    ULONG chans = 0, ch;
    SHORT mn = u->Info.VolMin[Dir], mx = u->Info.VolMax[Dir];

    if (e == NULL) {
        return;
    }
    if (u->VolCh[Dir] == 0) {
        LONG avg = ((LONG)u->VolL[Dir] + u->VolR[Dir]) / 2;
        SetVol(u, e->Id, 0, Clamp(avg, mn, mx));
        if (HasMute(u, FuControls(u, e, 0, &chans))) {
            SetMute(u, e->Id, 0, (BOOLEAN)u->VolMute[Dir]);
        }
        return;
    }
    for (ch = 1; ch <= u->VolChannels[Dir]; ch++) {
        SHORT v = (ch == 2) ? u->VolR[Dir] : u->VolL[Dir];
        if (u->VolMute[Dir]) {
            v = mn;
        }
        SetVol(u, e->Id, (UCHAR)ch, Clamp(v, mn, mx));
    }
}

/* ------------------------------------------------------------------ */
/* Clocks and sample rates                                              */
/* ------------------------------------------------------------------ */

/* UAC2: clock source behind entity Id; selector pins chosen on the way are set when Apply */
static UCHAR ClockSource(UA_DEV *u, UCHAR Id, ULONG Rate, BOOLEAN Apply, int Depth);

static BOOLEAN ClockHasRate(UA_DEV *u, UCHAR Src, ULONG Rate)
{
    UCHAR b[2 + 12 * 8];
    ULONG got = 0, n, i;

    if (AcGet(u, UAC2_RANGE, Src, 1, 0, b, 2, &got) != USB_OK || got < 2) {
        return TRUE;                        /* no range: assume it can */
    }
    n = GET16(b);
    if (n > 8) {
        n = 8;
    }
    if (AcGet(u, UAC2_RANGE, Src, 1, 0, b, (USHORT)(2 + 12 * n), &got) != USB_OK || got < 2 + 12) {
        return TRUE;
    }
    n = (got - 2) / 12;
    for (i = 0; i < n; i++) {
        ULONG mn = GET32(b + 2 + 12 * i), mx = GET32(b + 6 + 12 * i);
        if (Rate >= mn && Rate <= mx) {
            return TRUE;
        }
    }
    return FALSE;
}

static UCHAR ClockSource(UA_DEV *u, UCHAR Id, ULONG Rate, BOOLEAN Apply, int Depth)
{
    UA_ENT *e = Ent(u, Id);
    ULONG i;

    if (e == NULL || Depth > 8) {
        return 0;
    }
    if (e->Kind == AC2_CLOCK_SOURCE) {
        return (Rate == 0 || ClockHasRate(u, Id, Rate)) ? Id : 0;
    }
    if (e->Kind == AC2_CLOCK_MULT && e->NIn) {
        return ClockSource(u, e->In[0], Rate, Apply, Depth + 1);
    }
    if (e->Kind == AC2_CLOCK_SELECTOR) {
        for (i = 0; i < e->NIn; i++) {
            UCHAR s = ClockSource(u, e->In[i], Rate, Apply, Depth + 1);
            if (s != 0) {
                if (Apply) {
                    UCHAR pin = (UCHAR)(i + 1);
                    AcSet(u, UAC2_CUR, Id, 1, 0, &pin, 1);
                }
                return s;
            }
        }
    }
    return 0;
}

/* UAC2 rate list of a clock source into a format */
static void ClockRates(UA_DEV *u, UCHAR Src, UA_FORMAT *f)
{
    UCHAR b[2 + 12 * UA_MAX_RATES];
    ULONG got = 0, n, i;

    f->NumRates = 0;
    f->RateMin = f->RateMax = 0;
    if (AcGet(u, UAC2_RANGE, Src, 1, 0, b, 2, &got) != USB_OK || got < 2) {
        goto cur;
    }
    n = GET16(b);
    if (n > UA_MAX_RATES) {
        n = UA_MAX_RATES;
    }
    if (n == 0 || AcGet(u, UAC2_RANGE, Src, 1, 0, b, (USHORT)(2 + 12 * n), &got) != USB_OK || got < 14) {
        goto cur;
    }
    n = (got - 2) / 12;
    for (i = 0; i < n; i++) {
        ULONG mn = GET32(b + 2 + 12 * i), mx = GET32(b + 6 + 12 * i), rs = GET32(b + 10 + 12 * i);
        if (mn == mx || rs == 0) {
            if (f->NumRates < UA_MAX_RATES) {
                f->Rates[f->NumRates++] = mn;
            }
            if (mx != mn && f->NumRates < UA_MAX_RATES) {
                f->Rates[f->NumRates++] = mx;
            }
        } else {
            /* a real range: keep it as continuous when it is the only one */
            if (n == 1) {
                f->RateMin = mn;
                f->RateMax = mx;
                return;
            }
            if (f->NumRates < UA_MAX_RATES) {
                f->Rates[f->NumRates++] = mn;
            }
        }
    }
    if (f->NumRates) {
        return;
    }
cur:
    if (AcGet(u, UAC2_CUR, Src, 1, 0, b, 4, &got) == USB_OK && got >= 4 && GET32(b) != 0) {
        f->Rates[0] = GET32(b);
        f->NumRates = 1;
    } else {
        f->Rates[0] = 48000;
        f->NumRates = 1;
    }
}

static int SetRate(UA_DEV *u, UA_STREAM *s, ULONG Rate)
{
    UCHAR b[4];
    int r = USB_OK;

    if (u->Uac == 2) {
        UA_ENT *t = Ent(u, s->Alt->Terminal);
        UCHAR src = t ? ClockSource(u, t->Clock, Rate, TRUE, 0) : 0;
        if (src == 0 && t != NULL) {
            src = ClockSource(u, t->Clock, 0, TRUE, 0);
        }
        if (src != 0) {
            UA_ENT *cs = Ent(u, src);
            /* programmable frequency control (bits 0-1 == 3) */
            if (cs != NULL && cs->D[0] >= 6 && (cs->D[5] & 3) == 3) {
                PUT32(b, Rate);
                r = AcSet(u, UAC2_CUR, src, 1, 0, b, 4);
            }
        }
    } else if (s->Alt->FreqCtl) {
        b[0] = (UCHAR)Rate;
        b[1] = (UCHAR)(Rate >> 8);
        b[2] = (UCHAR)(Rate >> 16);
        r = UsbControl(u->Dev, USB_RT_OUT | USB_RT_CLASS | USB_RT_ENDPOINT, UAC1_SET_CUR, 0x0100,
                       s->Alt->Ep[2], 3, b, 1000, NULL);
    }
    if (r != USB_OK) {
        UsbLog(LOG_INFO, "audio %u: setting %u Hz failed (%d)\n", u->Dev->Index, Rate, r);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Descriptor parsing                                                   */
/* ------------------------------------------------------------------ */

static void ParseAlt(UA_DEV *u, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *d = If;
    UA_ALT a;
    ULONG dir;
    BOOLEAN pcm = FALSE;
    UCHAR fmtType = 0;
    UCHAR syncAddr = 0;

    UsbMemSet(&a, 0, sizeof(a));
    a.Iface = If[2];
    a.Fmt.Alt = If[3];
    while ((d = UsbNextDesc(d, End)) != NULL && d[1] != USB_DT_INTERFACE) {
        if (d[1] == USB_DT_CS_INTERFACE && d[2] == AS_GENERAL) {
            a.Terminal = d[3];
            if (u->Uac == 2 && d[0] >= 15) {
                ULONG fm = GET32(d + 6);
                fmtType = d[5];
                a.Fmt.Channels = d[10];
                if (fm & 1) {
                    pcm = TRUE;
                    a.Fmt.Type = UA_FMT_PCM;
                } else if (fm & 2) {
                    pcm = TRUE;
                    a.Fmt.Type = UA_FMT_PCM8;
                } else if (fm & 4) {
                    pcm = TRUE;
                    a.Fmt.Type = UA_FMT_FLOAT;
                }
            } else if (u->Uac != 2 && d[0] >= 7) {
                USHORT tag = GET16(d + 5);
                pcm = (BOOLEAN)(tag >= 1 && tag <= 3);
                a.Fmt.Type = (UCHAR)(tag == 2 ? UA_FMT_PCM8 : (tag == 3 ? UA_FMT_FLOAT : UA_FMT_PCM));
            }
        } else if (d[1] == USB_DT_CS_INTERFACE && d[2] == AS_FORMAT_TYPE && d[3] == 1) {
            fmtType = 1;
            if (u->Uac == 2 && d[0] >= 6) {
                a.Fmt.SubSize = d[4];
                a.Fmt.Bits = d[5];
            } else if (u->Uac != 2 && d[0] >= 8) {
                ULONG n = d[7], i;
                a.Fmt.Channels = d[4];
                a.Fmt.SubSize = d[5];
                a.Fmt.Bits = d[6];
                if (n == 0 && d[0] >= 14) {
                    a.Fmt.RateMin = d[8] | ((ULONG)d[9] << 8) | ((ULONG)d[10] << 16);
                    a.Fmt.RateMax = d[11] | ((ULONG)d[12] << 8) | ((ULONG)d[13] << 16);
                }
                for (i = 0; i < n && i < UA_MAX_RATES && 8 + 3 * i + 2 < d[0]; i++) {
                    a.Fmt.Rates[i] = d[8 + 3 * i] | ((ULONG)d[9 + 3 * i] << 8) | ((ULONG)d[10 + 3 * i] << 16);
                }
                a.Fmt.NumRates = (UCHAR)i;
            }
        } else if (d[1] == USB_DT_ENDPOINT && d[0] >= 7 && (d[3] & 3) == USB_EP_ISOCH) {
            UCHAR usage = (UCHAR)((d[3] >> 4) & 3);
            if (usage == 1 || (a.Ep != NULL && syncAddr != 0 && d[2] == syncAddr)) {
                a.Fb = d;
            } else if (a.Ep == NULL) {
                a.Ep = d;
                if (u->Uac != 2 && d[0] >= 9) {
                    syncAddr = d[8];
                }
            }
        } else if (d[1] == USB_DT_CS_ENDPOINT && d[2] == 1 && a.Ep != NULL && a.Fb == NULL && d[0] >= 4) {
            a.FreqCtl = (UCHAR)(u->Uac != 2 && (d[3] & 1));
        }
    }
    if (a.Ep == NULL || !pcm || fmtType != 1 || a.Fmt.Channels == 0 || a.Fmt.SubSize == 0 || a.Fmt.SubSize > 4) {
        return;
    }
    if (a.Fmt.Bits == 0 || a.Fmt.Bits > a.Fmt.SubSize * 8) {
        a.Fmt.Bits = (UCHAR)(a.Fmt.SubSize * 8);
    }
    if (a.Fmt.Type == UA_FMT_FLOAT && a.Fmt.SubSize != 4) {
        return;
    }
    dir = (a.Ep[2] & 0x80) ? UA_REC : UA_PLAY;
    a.Fmt.Sync = (UCHAR)((a.Ep[3] >> 2) & 3);
    a.Fmt.Feedback = (UCHAR)(a.Fb != NULL && dir == UA_PLAY);
    if (dir == UA_REC) {
        a.Fb = NULL;
    }
    if (u->NAlt[dir] >= UA_MAX_FORMATS) {
        return;
    }
    u->Alts[dir][u->NAlt[dir]++] = a;
    u->AsIf[dir] = a.Iface;
    u->Term[dir] = a.Terminal;
}

static void ParseMidi(UA_DEV *u, const UCHAR *If, const UCHAR *End)
{
    const UCHAR *d = If;
    const UCHAR *out = NULL;
    UCHAR cables = 0;
    UA_MIDI *m;

    while ((d = UsbNextDesc(d, End)) != NULL && d[1] != USB_DT_INTERFACE) {
        if (d[1] == USB_DT_ENDPOINT && d[0] >= 7 && (d[3] & 3) == USB_EP_BULK && !(d[2] & 0x80) && out == NULL) {
            out = d;
            cables = 0;
        } else if (d[1] == USB_DT_CS_ENDPOINT && d[2] == 1 && d[0] >= 4 && out != NULL && cables == 0) {
            cables = d[3];
        } else if (d[1] == USB_DT_ENDPOINT && d[0] >= 7 && (d[2] & 0x80)) {
            u->Info.MidiInCables = 1;
        }
    }
    if (out == NULL || u->Midi != NULL) {
        return;
    }
    m = (UA_MIDI *)OsAlloc(sizeof(UA_MIDI));
    if (m == NULL) {
        return;
    }
    m->Buf = (UCHAR *)UsbDmaAlloc(512, &m->BufPhys);
    if (m->Buf == NULL || UsbOpenPipe(u->Dev, &m->Pipe, out) != USB_OK) {
        if (m->Buf != NULL) {
            UsbDmaFree(m->Buf, 512);
        }
        OsFree(m);
        return;
    }
    m->Opened = 1;
    m->Iface = If[2];
    m->Cables = (UCHAR)(cables ? (cables > 16 ? 16 : cables) : 1);
    u->Midi = m;
    u->Info.MidiOutCables = m->Cables;
}

/* interfaces of the audio function: UAC1 header list or UAC2 interface association */
static BOOLEAN InCollection(UA_DEV *u, const UCHAR *Hdr, UCHAR First, UCHAR Count, UCHAR Ifnum)
{
    ULONG i;

    if (u->Uac != 2 && Hdr != NULL && Hdr[0] >= 8) {
        for (i = 0; i < Hdr[7] && 8 + i < Hdr[0]; i++) {
            if (Hdr[8 + i] == Ifnum) {
                return TRUE;
            }
        }
        return FALSE;
    }
    if (Count != 0) {
        return (BOOLEAN)(Ifnum >= First && Ifnum < First + Count);
    }
    return TRUE;
}

static void FreeStreamBuffers(UA_STREAM *s)
{
    ULONG i;

    for (i = 0; i < UA_NX; i++) {
        if (s->Xf[i].Buf != NULL) {
            OsDmaFree(s->Xf[i].Buf, s->Xf[i].Size);
            s->Xf[i].Buf = NULL;
        }
        if (s->Xf[i].Pkt != NULL) {
            OsFree(s->Xf[i].Pkt);
            s->Xf[i].Pkt = NULL;
        }
    }
    for (i = 0; i < UA_NFB; i++) {
        if (s->Fbx[i].Buf != NULL) {
            UsbDmaFree(s->Fbx[i].Buf, 64);
            s->Fbx[i].Buf = NULL;
        }
        if (s->Fbx[i].Pkt != NULL) {
            OsFree(s->Fbx[i].Pkt);
            s->Fbx[i].Pkt = NULL;
        }
    }
}

int AudioAttach(USB_DEV *Dev, const UCHAR *If, const UCHAR *End)
{
    UA_DEV *u;
    const UCHAR *d;
    const UCHAR *hdr = NULL;
    UCHAR first = 0, count = 0;
    ULONG slot, dir, i;

    if (If[6] == 2) {
        /* streaming interface: belongs to the control interface already handled */
        return (Dev->Audio != NULL) ? USB_OK : USB_ERR_PARAM;
    }
    if (If[6] == 3 && Dev->Audio != NULL) {
        return USB_OK;
    }
    if (If[6] != 1 && If[6] != 3) {
        return USB_ERR_PARAM;
    }
    if (Dev->Audio != NULL) {
        return USB_OK;                          /* second audio function: not supported */
    }
    if (OsGetConfig("DisableAudio", 0)) {
        return USB_ERR_PARAM;
    }
    if (!Dev->Hc->IsoOk && If[6] == 1) {
        UsbLog(LOG_INFO, "audio %u: %s has no isochronous support\n", Dev->Index, Dev->Hc->Ops->Name);
    }
    for (slot = 0; slot < UA_MAX_DEVS && UaDevs[slot] != NULL; slot++) {
    }
    if (slot == UA_MAX_DEVS) {
        return USB_ERR_NOMEM;
    }
    u = (UA_DEV *)OsAlloc(sizeof(UA_DEV));
    if (u == NULL) {
        return USB_ERR_NOMEM;
    }
    if (UaLatencyMs == 0) {
        UaLatencyMs = OsGetConfig("AudioLatencyMs", 48);
        if (UaLatencyMs < 24) {
            UaLatencyMs = 24;
        }
        if (UaLatencyMs > 96) {
            UaLatencyMs = 96;
        }
    }
    u->Dev = Dev;
    u->AcIf = If[2];
    u->Uac = (UCHAR)((If[7] == 0x20) ? 2 : 1);
    UaGen[slot]++;
    u->Id = (slot + 1) | ((ULONG)UaGen[slot] << 8);
    for (i = 0; i < 2; i++) {
        u->S[i].U = u;
        u->S[i].Dir = (UCHAR)i;
    }

    if (If[6] == 1) {
        /* class specific AC descriptors */
        d = If;
        while ((d = UsbNextDesc(d, End)) != NULL && d[1] != USB_DT_INTERFACE) {
            if (d[1] != USB_DT_CS_INTERFACE || d[0] < 3) {
                continue;
            }
            if (d[2] == AC_HEADER) {
                hdr = d;
                if (d[0] >= 5 && GET16(d + 3) >= 0x0200) {
                    u->Uac = 2;
                }
            } else {
                AddEnt(u, d);
            }
        }
        /* interface association in front of the control interface */
        for (d = Dev->Config; d != NULL && d < If; d = UsbNextDesc(d, End)) {
            if (d[1] == USB_DT_IAD && d[0] >= 8 && d[2] == If[2]) {
                first = d[2];
                count = d[3];
            }
        }
        /* streaming and MIDI interfaces */
        for (d = Dev->Config; d != NULL; d = UsbNextDesc(d, End)) {
            if (d[1] != USB_DT_INTERFACE || d[0] < 9 || d[5] != USB_CLASS_AUDIO ||
                !InCollection(u, hdr, first, count, d[2])) {
                continue;
            }
            if (d[6] == 2 && d[3] != 0) {
                ParseAlt(u, d, End);
            } else if (d[6] == 3 && d[3] == 0) {
                ParseMidi(u, d, End);
            }
        }
    } else {
        /* MIDI streaming interface without audio control interface */
        ParseMidi(u, If, End);
    }

    if (u->NAlt[UA_PLAY] == 0 && u->NAlt[UA_REC] == 0 && u->Midi == NULL) {
        UsbLog(LOG_INFO, "audio %u: no usable PCM stream\n", Dev->Index);
        OsFree(u);
        return USB_ERR_PARAM;
    }

    u->Info.Id = u->Id;
    u->Info.Vid = GET16(Dev->DevDesc + 8);
    u->Info.Pid = GET16(Dev->DevDesc + 10);
    u->Info.Uac = u->Uac;
    u->Info.Speed = Dev->Speed;
    if (Dev->Product[0] != 0) {
        UsbFormat(u->Info.Name, sizeof(u->Info.Name), "%s", Dev->Product);
    } else if (Dev->Mfg[0] != 0) {
        UsbFormat(u->Info.Name, sizeof(u->Info.Name), "%s USB Audio", Dev->Mfg);
    } else {
        UsbFormat(u->Info.Name, sizeof(u->Info.Name), "USB Audio Device");
    }
    BuildPaths(u);
    for (dir = 0; dir < 2; dir++) {
        u->Info.HasDir[dir] = (UCHAR)(u->NAlt[dir] != 0 && Dev->Hc->IsoOk);
        u->Info.NumFormats[dir] = (UCHAR)u->NAlt[dir];
        if (u->NAlt[dir] != 0) {
            UsbSetInterface(Dev, u->AsIf[dir], 0);
        }
        for (i = 0; i < u->NAlt[dir]; i++) {
            UA_FORMAT *f = &u->Alts[dir][i].Fmt;
            if (u->Uac == 2) {
                UA_ENT *t = Ent(u, u->Alts[dir][i].Terminal);
                UCHAR src = t ? ClockSource(u, t->Clock, 0, FALSE, 0) : 0;
                if (src != 0) {
                    ClockRates(u, src, f);
                } else {
                    f->Rates[0] = 48000;
                    f->NumRates = 1;
                }
            }
            if (f->NumRates == 0 && f->RateMax == 0) {
                f->Rates[0] = 48000;
                f->NumRates = 1;
            }
            u->Info.Formats[dir][i] = *f;
            UsbLog(LOG_INFO, "audio %u: %s alt %u: %u ch %u/%u bit%s %s, %u rate(s) %u..%u%s\n", Dev->Index,
                   dir == UA_PLAY ? "out" : "in", f->Alt, f->Channels, f->Bits, f->SubSize * 8,
                   f->Type == UA_FMT_FLOAT ? " float" : "",
                   f->Sync == 1 ? "async" : (f->Sync == 2 ? "adaptive" : (f->Sync == 3 ? "sync" : "none")),
                   f->NumRates, f->NumRates ? f->Rates[0] : f->RateMin,
                   f->NumRates ? f->Rates[f->NumRates - 1] : f->RateMax, f->Feedback ? ", feedback" : "");
        }
        if (u->NAlt[dir] != 0) {
            InitPath(u, dir);
        }
    }
    UsbLog(LOG_INFO, "audio %u: USB Audio %u.0 \"%s\"%s%s%s\n", Dev->Index, u->Uac, u->Info.Name,
           u->NAlt[UA_PLAY] ? ", playback" : "", u->NAlt[UA_REC] ? ", capture" : "",
           u->Midi ? ", MIDI out" : "");
    Dev->Audio = u;
    Dev->Function |= (UCHAR)((u->NAlt[0] || u->NAlt[1]) ? USB_FUNC_AUDIO : 0) | (u->Midi ? USB_FUNC_MIDI : 0);
    UaDevs[slot] = u;
    OsWakeWorker();
    return USB_OK;
}

/* ------------------------------------------------------------------ */
/* Streaming                                                            */
/* ------------------------------------------------------------------ */

/* Q16.16 frames per packet at Rate for a packet every Slots microframes */
static ULONG PerPacket(ULONG Rate, ULONG Slots)
{
    ULONG pps = 8000 / Slots;

    if (pps == 0) {
        pps = 1;
    }
    return ((Rate / pps) << 16) + (((Rate % pps) << 16) / pps);
}

static int Submit(UA_XFER *x);

static void XferDone(USB_XFER *X)
{
    UA_XFER *x = (UA_XFER *)X->Context;
    UA_STREAM *s = x->S;
    UA_DEV *u = s->U;
    ULONG i;
    OS_IRQL irql;
    BOOLEAN again;
    UA_XFER *grow = NULL;

    if (!x->IsFb && s->Pipe.IsoLate) {
        s->Pipe.IsoLate = 0;
        s->Stats.Late++;
        /* the schedule ran dry: keep one more transfer queued from now on */
        if (s->NX < s->NXAlloc && s->Running && !s->Stopping) {
            grow = &s->Xf[s->NX];
            s->NX++;
            s->Info.LatencyMs = s->NX * s->PktPerXfer * s->Slots / 8;
        }
    }
    if (!x->IsFb) {
        ULONG now = OsTimeMs();
        if (s->LastDone != 0 && s->Running && !s->Stopping) {
            ULONG gap = now - s->LastDone;
            if (gap > s->Stats.MaxGapMs) {
                s->Stats.MaxGapMs = gap;
            }
            if (gap > 30) {
                s->Stats.Gaps++;
            }
        }
        s->LastDone = now;
    }
    if (X->Status == USB_ERR_ABORTED || X->Status == USB_ERR_NODEV) {
        /* cancelled: nothing to look at */
    } else if (x->IsFb) {
        if (UsbDebugLevel >= LOG_INFO && s->Stats.FeedbackCount == 0 && x->Pkt[0].Status != USB_OK &&
            s->FbLogged < 3) {
            s->FbLogged++;
            UsbLog(LOG_INFO, "audio: feedback packet status %d actual %u (xfer %d)\n", x->Pkt[0].Status,
                   x->Pkt[0].Actual, X->Status);
        }
        for (i = 0; i < x->NPkt; i++) {
            USB_ISO_PACKET *p = &x->Pkt[i];
            if (p->Status == USB_OK && p->Actual >= 3) {
                const UCHAR *b = x->Buf + p->Offset;
                ULONG raw = (p->Actual >= 4) ? GET32(b) : (b[0] | ((ULONG)b[1] << 8) | ((ULONG)b[2] << 16));
                ULONG nominal = s->Nominal;
                ULONG c[3], k;
                if (u->Dev->Speed == USB_SPEED_HIGH || u->Dev->Speed == USB_SPEED_SUPER) {
                    /* 16.16 frames per microframe (some devices: 10.14) */
                    c[0] = raw * s->Slots;
                    c[1] = (raw << 2) * s->Slots;
                    c[2] = (raw >> 3) * s->Slots;
                } else {
                    /* 10.14 frames per frame (some devices: 16.16) */
                    c[0] = (raw << 2) * (s->Slots / 8);
                    c[1] = raw * (s->Slots / 8);
                    c[2] = c[0];
                }
                for (k = 0; k < 3; k++) {
                    if (c[k] > nominal - nominal / 8 && c[k] < nominal + nominal / 8) {
                        s->Current = c[k];
                        s->Stats.Feedback = c[k];
                        s->Stats.FeedbackCount++;
                        break;
                    }
                }
            }
        }
    } else {
        ULONG got = 0, good = 0;
        for (i = 0; i < x->NPkt; i++) {
            USB_ISO_PACKET *p = &x->Pkt[i];
            if (p->Status != USB_OK) {
                s->Stats.Errors++;
                continue;
            }
            good++;
            got += p->Actual / s->FrameBytes;
            s->Stats.Packets++;
            if (s->Dir == UA_REC) {
                ULONG frames = p->Actual / s->FrameBytes;
                s->Stats.Frames += frames;
                if (frames != 0 && s->Ops.Data != NULL && !s->Stopping) {
                    s->Ops.Data(s->Ops.Context, x->Buf + p->Offset, frames);
                }
            } else {
                s->Stats.Frames += p->Length / s->FrameBytes;
            }
        }
        if (s->Dir == UA_REC && good != 0 && good == x->NPkt) {
            /* the device's own rate, for playback without a feedback endpoint */
            ULONG m = (got << 16) / good;
            s->Measured = s->Measured ? (s->Measured / 8) * 7 + m / 8 : m;
        }
    }

    irql = OsLock();
    x->Busy = 0;
    if (s->Pending) {
        s->Pending--;
    }
    again = (BOOLEAN)(s->Running && !s->Stopping && !u->Gone && s->Open);
    if (!again && s->Pending == 0) {
        s->Running = 0;
        s->Stopping = 0;
    }
    OsUnlock(irql);
    if (again) {
        Submit(x);
        if (grow != NULL && !grow->Busy) {
            Submit(grow);
        }
    }
}

/* fills (playback) or prepares (capture, feedback) a transfer and submits it */
static int Submit(UA_XFER *x)
{
    UA_STREAM *s = x->S;
    ULONG i;
    ULONG off = 0;
    int r;
    OS_IRQL irql;

    if (x->IsFb) {
        for (i = 0; i < x->NPkt; i++) {
            x->Pkt[i].Offset = i * 8;
            x->Pkt[i].Length = (USHORT)(s->FbPipe.MaxPacket < 8 ? s->FbPipe.MaxPacket : 8);
        }
    } else if (s->Dir == UA_PLAY) {
        ULONG total = 0;
        if (!s->HasFb && s->Alt->Fmt.Sync == 1) {
            /* asynchronous without a feedback endpoint: implicit feedback, the
               capture stream of the same device paces playback */
            UA_STREAM *r = &s->U->S[UA_REC];
            if (r->Running && r->Measured != 0 && r->Info.Rate == s->Info.Rate && r->Slots != 0) {
                ULONG m = (r->Slots == s->Slots) ? r->Measured : r->Measured / r->Slots * s->Slots;
                if (m > s->Nominal - s->Nominal / 8 && m < s->Nominal + s->Nominal / 8) {
                    s->Current = m;
                    s->Stats.Feedback = m;
                    s->Stats.FeedbackCount++;
                }
            }
        }
        for (i = 0; i < x->NPkt; i++) {
            ULONG f;
            s->Acc += s->Current;
            f = s->Acc >> 16;
            s->Acc &= 0xFFFF;
            if (f > s->MaxFrames) {
                f = s->MaxFrames;
            }
            x->Pkt[i].Offset = off;
            x->Pkt[i].Length = (USHORT)(f * s->FrameBytes);
            off += f * s->FrameBytes;
            total += f;
        }
        if (s->Ops.Fill != NULL) {
            s->Ops.Fill(s->Ops.Context, x->Buf, total);
        } else {
            UsbMemSet(x->Buf, 0, off);
        }
    } else {
        ULONG room = s->MaxFrames * s->FrameBytes;
        for (i = 0; i < x->NPkt; i++) {
            x->Pkt[i].Offset = off;
            x->Pkt[i].Length = (USHORT)room;
            off += room;
        }
    }
    UsbMemSet(&x->X, 0, sizeof(x->X));
    x->X.Pipe = x->IsFb ? &s->FbPipe : &s->Pipe;
    x->X.Buf = x->Buf;
    x->X.Phys = x->Phys;
    x->X.Length = x->IsFb ? x->NPkt * 8 : off;
    x->X.DirIn = (UCHAR)(x->IsFb || s->Dir == UA_REC);
    x->X.Iso = x->Pkt;
    x->X.IsoCount = x->NPkt;
    x->X.Complete = XferDone;
    x->X.Context = x;
    irql = OsLock();
    x->Busy = 1;
    s->Pending++;
    OsUnlock(irql);
    r = UsbSubmit(&x->X);
    if (r != USB_OK) {
        irql = OsLock();
        x->Busy = 0;
        s->Pending--;
        if (s->Pending == 0) {
            s->Running = 0;
            s->Stopping = 0;
        }
        OsUnlock(irql);
        if (s->Stats.Errors < 3) {
            UsbLog(LOG_INFO, "audio %u: %s transfer not accepted (%d)\n", s->U->Dev->Index,
                   x->IsFb ? "feedback" : (s->Dir == UA_PLAY ? "playback" : "capture"), r);
        }
        s->Stats.Errors++;
    }
    return r;
}

static UA_ALT *ChooseAlt(UA_DEV *u, ULONG Dir, ULONG Rate, ULONG Channels, ULONG Bits, ULONG *RateOut)
{
    static const ULONG pref[] = { 48000, 96000, 44100, 88200, 192000, 32000, 176400, 24000, 22050, 16000,
                                  11025, 8000, 0 };
    UA_ALT *best = NULL;
    ULONG bestScore = 0xFFFFFFFF, bestRate = 0;
    ULONG i, k;

    for (i = 0; i < u->NAlt[Dir]; i++) {
        UA_ALT *a = &u->Alts[Dir][i];
        UA_FORMAT *f = &a->Fmt;
        ULONG rate = 0, rs, cs, bs, ts, score;
        /* rate: the requested one, else the most preferred the format offers */
        if (f->NumRates == 0) {
            if (Rate >= f->RateMin && Rate <= f->RateMax) {
                rate = Rate;
            }
            for (k = 0; rate == 0 && pref[k]; k++) {
                if (pref[k] >= f->RateMin && pref[k] <= f->RateMax) {
                    rate = pref[k];
                }
            }
            if (rate == 0) {
                rate = f->RateMax;
            }
        } else {
            for (k = 0; k < f->NumRates; k++) {
                if (f->Rates[k] == Rate) {
                    rate = Rate;
                }
            }
            for (k = 0; rate == 0 && pref[k]; k++) {
                ULONG j;
                for (j = 0; j < f->NumRates; j++) {
                    if (f->Rates[j] == pref[k]) {
                        rate = pref[k];
                    }
                }
            }
            if (rate == 0) {
                rate = f->Rates[0];
            }
        }
        if (rate == 0) {
            continue;
        }
        rs = (rate == Rate) ? 0 : 100;
        for (k = 0; rs && pref[k]; k++) {
            if (pref[k] == rate) {
                rs += k * 10;
            }
        }
        if (f->Channels == Channels) {
            cs = 0;
        } else if (f->Channels > Channels) {
            cs = 50 + (f->Channels - Channels);
        } else {
            cs = 400;
        }
        if (f->SubSize * 8 == Bits && f->Bits >= Bits) {
            bs = 0;
        } else if (f->SubSize * 8 > Bits) {
            bs = 5 + f->SubSize;
        } else {
            bs = 300;
        }
        ts = (f->Type == UA_FMT_PCM) ? 0 : (f->Type == UA_FMT_FLOAT ? 20 : 200);
        score = rs + cs + bs + ts;
        if (score < bestScore) {
            bestScore = score;
            best = a;
            bestRate = rate;
        }
    }
    *RateOut = bestRate;
    return best;
}

int UA_CALL UaGetInfo(ULONG Id, UA_INFO *Info)
{
    UA_DEV *u = Lookup(Id);

    if (u == NULL) {
        return USB_ERR_NODEV;
    }
    UsbMemCpy(Info, &u->Info, sizeof(*Info));
    return USB_OK;
}

static void CloseNow(ULONG Id, ULONG Dir)
{
    UA_DEV *u = Lookup(Id);
    UA_STREAM *s;
    ULONG start;

    if (u == NULL || Dir > 1) {
        return;
    }
    s = &u->S[Dir];
    if (!s->Open) {
        return;
    }
    UaStop(Id, Dir);
    start = OsTimeMs();
    while (s->Pending != 0 && OsTimeMs() - start < 500) {
        UsbPollAll();
        OsSleepMs(5);
    }
    s->Open = 0;
    UsbClosePipe(&s->Pipe);
    if (s->HasFb) {
        UsbClosePipe(&s->FbPipe);
    }
    s->Running = 0;
    s->Pending = 0;
    FreeStreamBuffers(s);
    if (!u->Dev->Gone && s->Alt != NULL) {
        UsbSetInterface(u->Dev, s->Alt->Iface, 0);
    }
}

static int OpenNow(ULONG Id, ULONG Dir, ULONG Rate, ULONG Channels, ULONG Bits, const UA_STREAM_OPS *Ops,
                   UA_STREAM_INFO *Out)
{
    UA_DEV *u = Lookup(Id);
    UA_STREAM *s;
    UA_ALT *a;
    ULONG rate = 0;
    ULONG i;
    ULONG mps;
    int r;

    if (u == NULL || Dir > 1 || !u->Info.HasDir[Dir]) {
        return USB_ERR_NODEV;
    }
    s = &u->S[Dir];
    if (s->Open) {
        CloseNow(Id, Dir);
    }
    a = ChooseAlt(u, Dir, Rate, Channels, Bits, &rate);
    if (a == NULL) {
        return USB_ERR_PARAM;
    }
    UsbMemSet(&s->Info, 0, sizeof(s->Info));
    UsbMemSet(&s->Stats, 0, sizeof(s->Stats));
    s->Alt = a;
    s->Ops = *Ops;
    r = UsbSetInterface(u->Dev, a->Iface, a->Fmt.Alt);
    if (r != USB_OK) {
        UsbLog(LOG_ERR, "audio %u: set interface %u alt %u failed (%d)\n", u->Dev->Index, a->Iface, a->Fmt.Alt, r);
        return r;
    }
    SetRate(u, s, rate);
    r = UsbOpenPipe(u->Dev, &s->Pipe, a->Ep);
    if (r != USB_OK) {
        UsbSetInterface(u->Dev, a->Iface, 0);
        return r;
    }
    s->HasFb = 0;
    if (a->Fb != NULL && Dir == UA_PLAY) {
        if (UsbOpenPipe(u->Dev, &s->FbPipe, a->Fb) == USB_OK) {
            s->HasFb = 1;
        }
    }
    s->FrameBytes = (ULONG)a->Fmt.Channels * a->Fmt.SubSize;
    s->Slots = s->Pipe.IsoSlots ? s->Pipe.IsoSlots : 8;
    s->Nominal = PerPacket(rate, s->Slots);
    s->Current = s->Nominal;
    s->Acc = 0;
    mps = (ULONG)s->Pipe.MaxPacket * (s->Pipe.Mult ? s->Pipe.Mult : 1);
    if (u->Dev->Speed == USB_SPEED_SUPER) {
        mps *= (ULONG)s->Pipe.MaxBurst + 1;
    }
    s->MaxFrames = mps / s->FrameBytes;
    if (s->MaxFrames == 0) {
        s->MaxFrames = 1;
    }
    /* about 8 ms per transfer */
    s->PktPerXfer = (8000 / s->Slots) * 8 / 1000;
    if (s->PktPerXfer == 0) {
        s->PktPerXfer = 1;
    }
    if (s->PktPerXfer > 64) {
        s->PktPerXfer = 64;
    }
    s->NX = UaLatencyMs * (8000 / s->Slots) / 1000 / s->PktPerXfer;
    if (s->NX < 3) {
        s->NX = 3;
    }
    if (s->NX > UA_NX) {
        s->NX = UA_NX;
    }
    s->NXAlloc = UA_NX;
    for (i = 0; i < s->NXAlloc; i++) {
        UA_XFER *x = &s->Xf[i];
        x->S = s;
        x->IsFb = 0;
        x->NPkt = s->PktPerXfer;
        x->Size = s->PktPerXfer * s->MaxFrames * s->FrameBytes;
        x->Buf = (UCHAR *)OsDmaAlloc(x->Size, &x->Phys);
        x->Pkt = (USB_ISO_PACKET *)OsAlloc(sizeof(USB_ISO_PACKET) * x->NPkt);
        if (x->Buf == NULL || x->Pkt == NULL) {
            UsbClosePipe(&s->Pipe);
            if (s->HasFb) {
                UsbClosePipe(&s->FbPipe);
            }
            FreeStreamBuffers(s);
            UsbSetInterface(u->Dev, a->Iface, 0);
            return USB_ERR_NOMEM;
        }
    }
    if (s->HasFb) {
        for (i = 0; i < UA_NFB; i++) {
            UA_XFER *x = &s->Fbx[i];
            x->S = s;
            x->IsFb = 1;
            x->NPkt = 8;                /* 8 service intervals per transfer */
            x->Buf = (UCHAR *)UsbDmaAlloc(64, &x->Phys);
            x->Pkt = (USB_ISO_PACKET *)OsAlloc(sizeof(USB_ISO_PACKET) * 8);
            if (x->Buf == NULL || x->Pkt == NULL) {
                s->HasFb = 0;
            }
        }
    }
    s->Info.Rate = rate;
    s->Info.Type = a->Fmt.Type;
    s->Info.Channels = a->Fmt.Channels;
    s->Info.SubSize = a->Fmt.SubSize;
    s->Info.Bits = a->Fmt.Bits;
    s->Info.Alt = a->Fmt.Alt;
    s->Info.LatencyMs = s->NX * s->PktPerXfer * s->Slots / 8;
    s->Open = 1;
    if (Out != NULL) {
        *Out = s->Info;
    }
    UsbLog(LOG_INFO, "audio %u: %s opened: alt %u, %u Hz, %u ch, %u bit, %u x %u packets every %u uframe(s)%s\n",
           u->Dev->Index, Dir == UA_PLAY ? "playback" : "capture", a->Fmt.Alt, rate, a->Fmt.Channels,
           a->Fmt.SubSize * 8, s->NX, s->PktPerXfer, s->Slots, s->HasFb ? ", feedback" : "");
    return USB_OK;
}

/*
 * Control transfers of a device are issued from the worker only, so
 * opening and closing streams from other threads goes through it.
 */
#define UA_CALLS    8

typedef struct _UA_REQ {
    ULONG           Close;
    ULONG           Id, Dir, Rate, Channels, Bits;
    const UA_STREAM_OPS *Ops;
    UA_STREAM_INFO *Out;
    int             Result;
    OS_EVENT        Done;
} UA_REQ;

static UA_REQ *UaCalls[UA_CALLS];

static int CallWorker(UA_REQ *c)
{
    OS_IRQL irql;
    ULONG i;
    BOOLEAN queued = FALSE;

    OsEventInit(&c->Done);
    c->Result = USB_ERR_TIMEOUT;
    while (!queued) {
        irql = OsLock();
        for (i = 0; i < UA_CALLS && !queued; i++) {
            if (UaCalls[i] == NULL) {
                UaCalls[i] = c;
                queued = TRUE;
            }
        }
        OsUnlock(irql);
        if (!queued) {
            OsSleepMs(10);
        }
    }
    OsWakeWorker();
    if (!OsEventWait(&c->Done, 15000)) {
        /* the worker never got to it: take it back if still queued */
        irql = OsLock();
        for (i = 0; i < UA_CALLS; i++) {
            if (UaCalls[i] == c) {
                UaCalls[i] = NULL;
                queued = FALSE;
            }
        }
        OsUnlock(irql);
        if (queued) {
            /* being executed right now */
            OsEventWait(&c->Done, 0xFFFFFFFF);
        }
    }
    return c->Result;
}

static void RunCalls(void)
{
    ULONG i;

    for (i = 0; i < UA_CALLS; i++) {
        UA_REQ *c;
        OS_IRQL irql = OsLock();
        c = UaCalls[i];
        OsUnlock(irql);
        if (c == NULL) {
            continue;
        }
        if (c->Close) {
            CloseNow(c->Id, c->Dir);
            c->Result = USB_OK;
        } else {
            c->Result = OpenNow(c->Id, c->Dir, c->Rate, c->Channels, c->Bits, c->Ops, c->Out);
        }
        irql = OsLock();
        UaCalls[i] = NULL;
        OsUnlock(irql);
        OsEventSet(&c->Done);
    }
}

int UA_CALL UaOpen(ULONG Id, ULONG Dir, ULONG Rate, ULONG Channels, ULONG Bits, const UA_STREAM_OPS *Ops,
           UA_STREAM_INFO *Out)
{
    UA_REQ c;

    if (OsInWorker()) {
        return OpenNow(Id, Dir, Rate, Channels, Bits, Ops, Out);
    }
    UsbMemSet(&c, 0, sizeof(c));
    c.Id = Id;
    c.Dir = Dir;
    c.Rate = Rate;
    c.Channels = Channels;
    c.Bits = Bits;
    c.Ops = Ops;
    c.Out = Out;
    return CallWorker(&c);
}

void UA_CALL UaClose(ULONG Id, ULONG Dir)
{
    UA_REQ c;

    if (OsInWorker()) {
        CloseNow(Id, Dir);
        return;
    }
    UsbMemSet(&c, 0, sizeof(c));
    c.Close = 1;
    c.Id = Id;
    c.Dir = Dir;
    CallWorker(&c);
}

int UA_CALL UaStart(ULONG Id, ULONG Dir)
{
    UA_DEV *u = Lookup(Id);
    UA_STREAM *s;
    OS_IRQL irql;
    ULONG i;
    BOOLEAN fresh;

    if (u == NULL || Dir > 1) {
        return USB_ERR_NODEV;
    }
    s = &u->S[Dir];
    irql = OsLock();
    if (!s->Open) {
        OsUnlock(irql);
        return USB_ERR_NODEV;
    }
    fresh = (BOOLEAN)!s->Running;
    s->Running = 1;
    s->Stopping = 0;
    s->Stats.Running = 1;
    OsUnlock(irql);
    if (!fresh) {
        /* still draining from a stop: transfers keep going */
        for (i = 0; i < s->NX; i++) {
            if (!s->Xf[i].Busy) {
                Submit(&s->Xf[i]);
            }
        }
        for (i = 0; s->HasFb && i < UA_NFB; i++) {
            if (!s->Fbx[i].Busy) {
                Submit(&s->Fbx[i]);
            }
        }
        return USB_OK;
    }
    s->Acc = 0;
    s->LastDone = 0;
    s->Measured = 0;
    s->Current = s->Nominal;
    for (i = 0; i < s->NX; i++) {
        if (Submit(&s->Xf[i]) != USB_OK) {
            break;
        }
    }
    if (s->HasFb) {
        for (i = 0; i < UA_NFB; i++) {
            Submit(&s->Fbx[i]);
        }
    }
    return s->Running ? USB_OK : USB_ERR_IO;
}

void UA_CALL UaStop(ULONG Id, ULONG Dir)
{
    UA_DEV *u = Lookup(Id);
    OS_IRQL irql;

    if (u == NULL || Dir > 1) {
        return;
    }
    irql = OsLock();
    if (u->S[Dir].Running) {
        u->S[Dir].Stopping = 1;
    }
    u->S[Dir].Stats.Running = 0;
    OsUnlock(irql);
}

void UA_CALL UaGetStats(ULONG Id, ULONG Dir, UA_STATS *Stats)
{
    UA_DEV *u = Lookup(Id);

    if (u == NULL || Dir > 1) {
        UsbMemSet(Stats, 0, sizeof(*Stats));
        return;
    }
    UsbMemCpy(Stats, &u->S[Dir].Stats, sizeof(*Stats));
    Stats->TickMaxGapMs = TickMaxGap;
}

int UA_CALL UaSetVolume(ULONG Id, ULONG Dir, SHORT Left, SHORT Right, BOOLEAN Mute)
{
    UA_DEV *u = Lookup(Id);

    if (u == NULL || Dir > 1 || !u->Info.HasVolume[Dir]) {
        return USB_ERR_NODEV;
    }
    u->VolL[Dir] = Left;
    u->VolR[Dir] = Right;
    u->VolMute[Dir] = (UCHAR)(Mute ? 1 : 0);
    u->VolPending[Dir] = 1;
    OsWakeWorker();
    return USB_OK;
}

int UA_CALL UaSelectInput(ULONG Id, ULONG Index)
{
    UA_DEV *u = Lookup(Id);

    if (u == NULL || Index >= u->Info.NumInputs) {
        return USB_ERR_PARAM;
    }
    u->SelIndex = (UCHAR)Index;
    u->SelPending = 1;
    OsWakeWorker();
    return USB_OK;
}

/* ------------------------------------------------------------------ */
/* USB MIDI output                                                      */
/* ------------------------------------------------------------------ */

static void MidiKick(UA_DEV *u);

static void MidiDone(USB_XFER *X)
{
    UA_DEV *u = (UA_DEV *)X->Context;
    OS_IRQL irql;

    irql = OsLock();
    if (u->Midi != NULL) {
        u->Midi->Busy = 0;
    }
    OsUnlock(irql);
    if (!u->Gone) {
        MidiKick(u);
    }
}

static void MidiKick(UA_DEV *u)
{
    UA_MIDI *m = u->Midi;
    OS_IRQL irql;
    ULONG n = 0;
    ULONG maxn;

    if (m == NULL || !m->Opened) {
        return;
    }
    maxn = (m->Pipe.MaxPacket >= 4 ? m->Pipe.MaxPacket : 64) / 4;
    if (maxn > 128) {
        maxn = 128;
    }
    irql = OsLock();
    if (m->Busy || m->Head == m->Tail) {
        OsUnlock(irql);
        return;
    }
    while (m->Tail != m->Head && n < maxn) {
        PUT32(m->Buf + n * 4, m->Ring[m->Tail]);
        m->Tail = (m->Tail + 1) % MIDI_RING;
        n++;
    }
    m->Busy = 1;
    OsUnlock(irql);
    UsbMemSet(&m->X, 0, sizeof(m->X));
    m->X.Pipe = &m->Pipe;
    m->X.Buf = m->Buf;
    m->X.Phys = m->BufPhys;
    m->X.Length = n * 4;
    m->X.Complete = MidiDone;
    m->X.Context = u;
    if (UsbSubmit(&m->X) != USB_OK) {
        irql = OsLock();
        m->Busy = 0;
        OsUnlock(irql);
    }
}

static void MidiPut(UA_MIDI *m, ULONG Cable, UCHAR Cin, const UCHAR *b, ULONG n)
{
    ULONG ev = ((Cable & 15) << 4) | Cin;
    ULONG next = (m->Head + 1) % MIDI_RING;

    if (n > 0) ev |= (ULONG)b[0] << 8;
    if (n > 1) ev |= (ULONG)b[1] << 16;
    if (n > 2) ev |= (ULONG)b[2] << 24;
    if (next != m->Tail) {
        m->Ring[m->Head] = ev;
        m->Head = next;
    }
}

static ULONG MidiLen(UCHAR St)
{
    switch (St & 0xF0) {
    case 0xC0:
    case 0xD0: return 2;
    case 0xF0:
        switch (St) {
        case 0xF1:
        case 0xF3: return 2;
        case 0xF2: return 3;
        default:   return 1;
        }
    default:   return 3;
    }
}

/* MIDI byte stream (running status, sysex) into USB-MIDI event packets */
int UA_CALL UaMidiOut(ULONG Id, ULONG Cable, const UCHAR *Data, ULONG Len)
{
    UA_DEV *u = Lookup(Id);
    UA_MIDI *m;
    ULONG i, c = Cable & 15;
    OS_IRQL irql;

    if (u == NULL || u->Midi == NULL) {
        return USB_ERR_NODEV;
    }
    m = u->Midi;
    irql = OsLock();
    for (i = 0; i < Len; i++) {
        UCHAR b = Data[i];
        if (b >= 0xF8) {                                /* real time, anywhere */
            MidiPut(m, c, 0xF, &b, 1);
            continue;
        }
        if (m->Sysex[c]) {
            if (b & 0x80 && b != 0xF7) {
                m->Sysex[c] = 0;                        /* aborted by a status byte */
                m->Have[c] = 0;
            } else {
                m->Msg[c][m->Have[c]++] = b;
                if (b == 0xF7) {
                    MidiPut(m, c, (UCHAR)(4 + m->Have[c]), m->Msg[c], m->Have[c]);
                    m->Have[c] = 0;
                    m->Sysex[c] = 0;
                } else if (m->Have[c] == 3) {
                    MidiPut(m, c, 0x4, m->Msg[c], 3);
                    m->Have[c] = 0;
                }
                continue;
            }
        }
        if (b == 0xF0) {
            m->Sysex[c] = 1;
            m->Msg[c][0] = b;
            m->Have[c] = 1;
            m->Running[c] = 0;
            continue;
        }
        if (b & 0x80) {
            m->Msg[c][0] = b;
            m->Have[c] = 1;
            m->Need[c] = (UCHAR)MidiLen(b);
            m->Running[c] = (UCHAR)((b < 0xF0) ? b : 0);
        } else {
            if (m->Have[c] == 0) {
                if (!m->Running[c]) {
                    continue;                           /* data without status */
                }
                m->Msg[c][0] = m->Running[c];
                m->Have[c] = 1;
                m->Need[c] = (UCHAR)MidiLen(m->Running[c]);
            }
            m->Msg[c][m->Have[c]++] = b;
        }
        if (m->Have[c] >= m->Need[c]) {
            UCHAR st = m->Msg[c][0];
            UCHAR cin;
            if (st < 0xF0) {
                cin = (UCHAR)(st >> 4);
            } else if (m->Need[c] == 1) {
                cin = 0x5;
            } else {
                cin = (UCHAR)m->Need[c];
            }
            MidiPut(m, c, cin, m->Msg[c], m->Have[c]);
            m->Have[c] = 0;
        }
    }
    OsUnlock(irql);
    MidiKick(u);
    return USB_OK;
}

/* ------------------------------------------------------------------ */
/* Clients, worker                                                      */
/* ------------------------------------------------------------------ */

int UA_CALL UaRegister(const UA_CLIENT *Client)
{
    ULONG i;

    UaClient = *Client;
    UaClientSet = 1;
    for (i = 0; i < UA_MAX_DEVS; i++) {
        if (UaDevs[i] != NULL) {
            UaDevs[i]->Notified = 0;
        }
    }
    OsWakeWorker();
    return USB_OK;
}

void UA_CALL UaUnregister(void)
{
    ULONG i;

    UaClientSet = 0;
    for (i = 0; i < UA_MAX_DEVS; i++) {
        if (UaDevs[i] != NULL) {
            UaStop(UaDevs[i]->Id, UA_PLAY);
            UaStop(UaDevs[i]->Id, UA_REC);
        }
    }
}

void AudioService(void)
{
    ULONG i, d;

    RunCalls();

    for (i = 0; i < UA_MAX_DEVS; i++) {
        UA_DEV *u = UaDevs[i];
        if (u == NULL || u->Gone) {
            continue;
        }
        if (!u->Notified && UaClientSet) {
            u->Notified = 1;
            if (UaClient.Arrival != NULL) {
                UaClient.Arrival(UaClient.Context, &u->Info);
            }
        }
        for (d = 0; d < 2; d++) {
            if (u->VolPending[d]) {
                u->VolPending[d] = 0;
                ApplyVolume(u, d);
            }
        }
        if (u->SelPending) {
            ULONG k = u->SelIndex;
            u->SelPending = 0;
            if (u->InSelUnit[k] != 0) {
                UCHAR v = u->InSelPin[k];
                AcSet(u, u->Uac == 2 ? UAC2_CUR : UAC1_SET_CUR, u->InSelUnit[k], (UCHAR)(u->Uac == 2 ? 1 : 0),
                      0, &v, 1);
            }
        }
    }
}

void AudioTick(void)
{
    ULONG now = OsTimeMs();

    if (TickLast != 0 && now - TickLast > TickMaxGap && UaDeviceCount() != 0) {
        TickMaxGap = now - TickLast;
    }
    TickLast = now;
}

void AudioDetach(USB_DEV *Dev)
{
    UA_DEV *u = (UA_DEV *)Dev->Audio;
    ULONG i, d;

    if (u == NULL) {
        return;
    }
    u->Gone = 1;
    if (u->Notified && UaClientSet && UaClient.Removal != NULL) {
        UaClient.Removal(UaClient.Context, u->Id);
    }
    for (d = 0; d < 2; d++) {
        UA_STREAM *s = &u->S[d];
        if (s->Open) {
            ULONG t0;
            s->Open = 0;
            UsbClosePipe(&s->Pipe);
            if (s->HasFb) {
                UsbClosePipe(&s->FbPipe);
            }
            /* a completion may still be running on another processor */
            t0 = OsTimeMs();
            while (s->Pending != 0 && OsTimeMs() - t0 < 100) {
                OsSleepMs(1);
            }
            FreeStreamBuffers(s);
        }
    }
    if (u->Midi != NULL) {
        UsbClosePipe(&u->Midi->Pipe);
        UsbDmaFree(u->Midi->Buf, 512);
        OsFree(u->Midi);
        u->Midi = NULL;
    }
    for (i = 0; i < UA_MAX_DEVS; i++) {
        if (UaDevs[i] == u) {
            UaDevs[i] = NULL;
        }
    }
    Dev->Audio = NULL;
    OsFree(u);
}
