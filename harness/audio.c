/*
 * audio.c - USB audio test client of the bare metal test kernel.
 *
 * Commands (ids in hex as printed by "als"):
 *   als                            list audio devices
 *   aopen id dir rate ch bits      open a stream (dir 0 playback, 1 capture)
 *   astart id dir / astop / aclose
 *   astat id dir                   statistics; capture also reports the
 *                                  measured left channel frequency and peak
 *   avol id dir left right mute    hardware volume in 1/256 dB
 *   asel id index                  capture source
 *   amidi id b0 b1 ...             MIDI bytes (hex) to cable 0
 *   cfg name value                 OsGetConfig value for later attaches
 */

#include "../src/usbaudio.h"
#include "harness.h"

#define MAXA 8

static ULONG Ids[MAXA];
static UA_STREAM_INFO Si[MAXA][2];
/* playback generator */
static ULONG Phase[MAXA];
static ULONG Step[MAXA];
/* capture analysis */
static ULONG CapFrames[MAXA];
static ULONG CapCross[MAXA];
static LONG CapPeak[MAXA];
static LONG CapLast[MAXA];

static int Slot(ULONG Id)
{
    int i;

    for (i = 0; i < MAXA; i++) {
        if (Ids[i] == Id) {
            return i;
        }
    }
    return -1;
}

static void UA_CALL Arrival(void *Ctx, const UA_INFO *I)
{
    int i = Slot(0);

    (void)Ctx;
    if (i >= 0) {
        Ids[i] = I->Id;
    }
    HarnessPrintf("AUDIO ARRIVE id=%x uac=%u speed=%u play=%u rec=%u fmts=%u/%u vol=%u/%u midi=%u inputs=%u name=%s\n",
                  I->Id, I->Uac, I->Speed, I->HasDir[0], I->HasDir[1], I->NumFormats[0], I->NumFormats[1],
                  I->HasVolume[0], I->HasVolume[1], I->MidiOutCables, I->NumInputs, I->Name);
}

static void UA_CALL Removal(void *Ctx, ULONG Id)
{
    int i = Slot(Id);

    (void)Ctx;
    if (i >= 0) {
        Ids[i] = 0;
    }
    HarnessPrintf("AUDIO GONE id=%x\n", Id);
}

/* triangle wave, 1 kHz at the stream rate, about -6 dBFS */
static void UA_CALL Fill(void *Ctx, UCHAR *Buf, ULONG Frames)
{
    int k = (int)(ULONG)Ctx;
    UA_STREAM_INFO *s = &Si[k][0];
    ULONG f, c, b;

    for (f = 0; f < Frames; f++) {
        ULONG ph = Phase[k];
        LONG v;
        Phase[k] += Step[k];
        /* ph: 32 bit phase, triangle from it */
        if (ph < 0x40000000UL) {
            v = (LONG)(ph >> 15);
        } else if (ph < 0xC0000000UL) {
            v = 0x8000L - (LONG)((ph - 0x40000000UL) >> 15);
        } else {
            v = (LONG)((ph - 0xC0000000UL) >> 15) - 0x8000L;
        }
        v /= 2;
        for (c = 0; c < s->Channels; c++) {
            if (s->SubSize == 1) {
                *Buf++ = (UCHAR)((v >> 8) + 128);
                continue;
            }
            for (b = 0; b < s->SubSize; b++) {
                if (b + 2 < s->SubSize) {
                    *Buf++ = 0;
                } else if (b + 2 == s->SubSize) {
                    *Buf++ = (UCHAR)v;
                } else {
                    *Buf++ = (UCHAR)(v >> 8);
                }
            }
        }
    }
}

static void UA_CALL Data(void *Ctx, const UCHAR *Buf, ULONG Frames)
{
    int k = (int)(ULONG)Ctx;
    UA_STREAM_INFO *s = &Si[k][1];
    ULONG fb = (ULONG)s->Channels * s->SubSize;
    ULONG f;

    for (f = 0; f < Frames; f++) {
        const UCHAR *p = Buf + f * fb;
        LONG v = (s->SubSize == 1) ? ((LONG)p[0] - 128) << 8 : (LONG)(SHORT)GET16(p + s->SubSize - 2);
        if ((v >= 0) != (CapLast[k] >= 0)) {
            CapCross[k]++;
        }
        CapLast[k] = v;
        if (v < 0) {
            v = -v;
        }
        if (v > CapPeak[k]) {
            CapPeak[k] = v;
        }
    }
    CapFrames[k] += Frames;
}

static LONG ParseS(const char **p)
{
    const char *s = *p;
    ULONG v = 0;
    int neg = 0;

    while (*s == ' ') {
        s++;
    }
    if (*s == '-') {
        neg = 1;
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (ULONG)(*s++ - '0');
    }
    *p = s;
    return neg ? -(LONG)v : (LONG)v;
}

static ULONG ParseHex(const char **p)
{
    const char *s = *p;
    ULONG v = 0;

    while (*s == ' ') {
        s++;
    }
    for (;;) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + (ULONG)(c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (ULONG)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = v * 16 + (ULONG)(c - 'A' + 10);
        else break;
        s++;
    }
    *p = s;
    return v;
}

static int Is(const char *s, const char *w)
{
    while (*w) {
        if (*s++ != *w++) {
            return 0;
        }
    }
    return *s == ' ' || *s == 0;
}

void AudioHarnessInit(void)
{
    UA_CLIENT c;

    c.Context = NULL;
    c.Arrival = Arrival;
    c.Removal = Removal;
    UaRegister(&c);
}

int AudioCommand(const char *c)
{
    const char *p;
    ULONG id, dir;
    int k;

    if (Is(c, "cfg")) {
        char name[32];
        int n = 0;
        p = c + 3;
        while (*p == ' ') {
            p++;
        }
        while (*p && *p != ' ' && n < 31) {
            name[n++] = *p++;
        }
        name[n] = 0;
        HarnessSetConfig(name, (ULONG)ParseS(&p));
        HarnessPrintf("CFG ok\n");
        return 1;
    }
    if (Is(c, "als")) {
        for (k = 0; k < MAXA; k++) {
            UA_INFO I;
            ULONG d, f;
            if (Ids[k] == 0 || UaGetInfo(Ids[k], &I) != USB_OK) {
                continue;
            }
            HarnessPrintf("AUDIO id=%x uac=%u play=%u rec=%u vol=%u/%u volrange=%d..%d midi=%u inputs=%u\n", I.Id,
                          I.Uac, I.HasDir[0], I.HasDir[1], I.HasVolume[0], I.HasVolume[1], I.VolMin[0], I.VolMax[0],
                          I.MidiOutCables, I.NumInputs);
            for (d = 0; d < 2; d++) {
                for (f = 0; f < I.NumFormats[d]; f++) {
                    UA_FORMAT *x = &I.Formats[d][f];
                    HarnessPrintf("  FMT dir=%u alt=%u ch=%u sub=%u bits=%u type=%u sync=%u fb=%u rates=%u %u %u %u\n", d,
                                  x->Alt, x->Channels, x->SubSize, x->Bits, x->Type, x->Sync, x->Feedback,
                                  x->NumRates, x->NumRates > 0 ? x->Rates[0] : x->RateMin,
                                  x->NumRates > 1 ? x->Rates[1] : x->RateMax, x->NumRates > 2 ? x->Rates[2] : 0);
                }
            }
        }
        return 1;
    }
    if (c[0] != 'a' || c[1] == 0) {
        return 0;
    }
    if (!(Is(c, "aopen") || Is(c, "astart") || Is(c, "astop") || Is(c, "aclose") || Is(c, "astat") ||
          Is(c, "avol") || Is(c, "asel") || Is(c, "amidi"))) {
        return 0;
    }
    p = c;
    while (*p && *p != ' ') {
        p++;
    }
    id = ParseHex(&p);
    k = Slot(id);
    if (k < 0) {
        HarnessPrintf("AERR nodev\n");
        return 1;
    }
    if (Is(c, "amidi")) {
        UCHAR b[64];
        ULONG n = 0;
        int r;
        while (*p == ' ') {
            p++;
        }
        while (*p && n < sizeof(b)) {
            b[n++] = (UCHAR)ParseHex(&p);
            while (*p == ' ') {
                p++;
            }
        }
        r = UaMidiOut(id, 0, b, n);
        HarnessPrintf("AMIDI r=%d n=%u\n", r, n);
        return 1;
    }
    if (Is(c, "asel")) {
        HarnessPrintf("ASEL r=%d\n", UaSelectInput(id, (ULONG)ParseS(&p)));
        return 1;
    }
    dir = (ULONG)ParseS(&p) & 1;
    if (Is(c, "aopen")) {
        UA_STREAM_OPS ops;
        ULONG rate = (ULONG)ParseS(&p);
        ULONG ch = (ULONG)ParseS(&p);
        ULONG bits = (ULONG)ParseS(&p);
        int r;
        ops.Context = (void *)(ULONG)k;
        ops.Fill = Fill;
        ops.Data = Data;
        r = UaOpen(id, dir, rate, ch, bits, &ops, &Si[k][dir]);
        if (r == USB_OK && dir == 0) {
            /* 2^32 * 1000 / rate */
            Step[k] = (ULONG)(((4294967295UL / Si[k][0].Rate) * 1000UL));
            Phase[k] = 0;
        }
        if (dir == 1) {
            CapFrames[k] = CapCross[k] = 0;
            CapPeak[k] = 0;
        }
        HarnessPrintf("AOPEN r=%d rate=%u ch=%u sub=%u bits=%u alt=%u type=%u lat=%u\n", r, Si[k][dir].Rate,
                      Si[k][dir].Channels, Si[k][dir].SubSize, Si[k][dir].Bits, Si[k][dir].Alt, Si[k][dir].Type,
                      Si[k][dir].LatencyMs);
    } else if (Is(c, "astart")) {
        HarnessPrintf("ASTART r=%d\n", UaStart(id, dir));
    } else if (Is(c, "astop")) {
        UaStop(id, dir);
        HarnessPrintf("ASTOP ok\n");
    } else if (Is(c, "aclose")) {
        UaClose(id, dir);
        HarnessPrintf("ACLOSE ok\n");
    } else if (Is(c, "astat")) {
        UA_STATS st;
        ULONG hz = 0;
        UaGetStats(id, dir, &st);
        if (dir == 1 && CapFrames[k] != 0) {
            /* crossings / 2 per frames / rate */
            hz = (ULONG)(((CapCross[k] / 2) * (Si[k][1].Rate / 10)) / (CapFrames[k] / 10 ? CapFrames[k] / 10 : 1));
        }
        HarnessPrintf("ASTAT pkts=%u frames=%u late=%u err=%u fb=%u fbn=%u run=%u cap=%u hz=%u peak=%d\n", st.Packets,
                      st.Frames, st.Late, st.Errors, st.Feedback, st.FeedbackCount, st.Running,
                      dir ? CapFrames[k] : 0, hz, dir ? CapPeak[k] : 0);
    } else if (Is(c, "avol")) {
        LONG l = ParseS(&p);
        LONG r = ParseS(&p);
        LONG m = ParseS(&p);
        HarnessPrintf("AVOL r=%d\n", UaSetVolume(id, dir, (SHORT)l, (SHORT)r, (BOOLEAN)(m != 0)));
    }
    return 1;
}
