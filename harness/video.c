/* video.c - harness commands for USB video cameras
 *
 *   vls                         cameras, formats, controls
 *   vstart ID FMT FRAME [IV]    start streaming (format/frame index, interval in 100 ns)
 *   vgrab ID N [TIMEOUT]        wait for N frames, print each and a summary
 *   vctl ID SEL [VALUE]         query (and set) a processing unit control
 *   vstop ID
 */
#include "../src/usbnt.h"
#include "../src/usbntioc.h"
#include "../src/usbvideo.h"
#include "harness.h"

static UCHAR FrameBuf[640 * 480 * 2 + 64 * 1024];

static int Is(const char *c, const char *w)
{
    while (*w) {
        if (*c++ != *w++) {
            return 0;
        }
    }
    return *c == 0 || *c == ' ';
}

static ULONG Num(const char **p)
{
    ULONG v = 0;
    int neg = 0, hex = 0;
    while (**p == ' ') {
        (*p)++;
    }
    if (**p == '-') {
        neg = 1;
        (*p)++;
    }
    if ((*p)[0] == '0' && ((*p)[1] == 'x' || (*p)[1] == 'X')) {
        hex = 1;
        *p += 2;
    }
    for (;;) {
        char ch = **p;
        if (ch >= '0' && ch <= '9') {
            v = v * (hex ? 16 : 10) + (ULONG)(ch - '0');
        } else if (hex && ch >= 'a' && ch <= 'f') {
            v = v * 16 + (ULONG)(ch - 'a' + 10);
        } else if (hex && ch >= 'A' && ch <= 'F') {
            v = v * 16 + (ULONG)(ch - 'A' + 10);
        } else {
            break;
        }
        (*p)++;
    }
    return neg ? (ULONG)(-(LONG)v) : v;
}

static void Fcc(char *o, ULONG f)
{
    o[0] = (char)f;
    o[1] = (char)(f >> 8);
    o[2] = (char)(f >> 16);
    o[3] = (char)(f >> 24);
    o[4] = 0;
}

/* luma of the 8 colour bars and of the moving box position (YUY2 / MJPEG markers) */
static void Analyse(const USBNT_VID_FRAME_HDR *h, const UCHAR *d)
{
    ULONG b, w = h->Width, ht = h->Height;
    if (h->FourCC == USBNT_FCC_YUY2 && h->Bytes >= w * ht * 2) {
        ULONG box = 99;
        HarnessPrintf("VBARS");
        for (b = 0; b < 8; b++) {
            ULONG x = b * w / 8 + 2;
            HarnessPrintf(" %u", d[(4 * w + x) * 2]);
        }
        /* the grey box sits in the middle row, in one of the bars */
        for (b = 0; b < 8; b++) {
            ULONG x = b * w / 8 + w / 16;
            ULONG y = d[(ht / 2 * w + x) * 2];
            ULONG top = d[(4 * w + x) * 2];
            if (y != top) {
                box = b;
            }
        }
        HarnessPrintf(" box=%u\n", box);
    } else if (h->FourCC == USBNT_FCC_MJPG) {
        ULONG n = h->Bytes, i, dht = 0, sof = 0;
        for (i = 0; i + 1 < n && i < 2048; i++) {
            if (d[i] == 0xFF && d[i + 1] == 0xC4) {
                dht = 1;
            }
            if (d[i] == 0xFF && d[i + 1] == 0xC0) {
                sof = 1;
            }
        }
        HarnessPrintf("VJPEG soi=%u eoi=%u sof=%u dht=%u bytes=%u\n", n >= 2 && d[0] == 0xFF && d[1] == 0xD8,
                      n >= 2 && d[n - 2] == 0xFF && d[n - 1] == 0xD9, sof, dht, n);
    }
}

int VideoHarnessCommand(const char *c)
{
    const char *p;
    static USBNT_CAMERAS cams;
    ULONG i, k;

    if (Is(c, "vls")) {
        UvcList(&cams);
        for (i = 0; i < cams.Count; i++) {
            USBNT_CAMERA *m = &cams.Cam[i];
            HarnessPrintf("CAM id=%x dev=%u uvc=%x formats=%u controls=%x streaming=%u name=%s\n", m->Id, m->DevIndex,
                          m->UvcMinor, m->NumFormats, m->Controls, m->Streaming, m->Name);
            for (k = 0; k < m->NumFormats; k++) {
                USBNT_VID_FORMAT *f = &m->Format[k];
                ULONG j;
                char cc[5];
                Fcc(cc, f->FourCC);
                HarnessPrintf("VFMT %u %s bpp=%u frames=%u:", k, cc, f->BitsPerPixel, f->NumFrames);
                for (j = 0; j < f->NumFrames; j++) {
                    HarnessPrintf(" %ux%u/%u/%u", f->Frame[j].Width, f->Frame[j].Height, f->Frame[j].Interval[0],
                                  f->Frame[j].NumIntervals);
                }
                HarnessPrintf("\n");
            }
        }
        HarnessPrintf("VLS count=%u\n", cams.Count);
        return 1;
    }
    if (Is(c, "vstart")) {
        USBNT_VID_START s;
        USBNT_VID_STATUS st;
        int r;
        char cc[5];
        p = c + 6;
        s.Id = Num(&p);
        s.Format = Num(&p);
        s.Frame = Num(&p);
        s.Interval = Num(&p);
        r = UvcStart(&s, &st);
        Fcc(cc, st.FourCC);
        HarnessPrintf("VSTART r=%d %s %ux%u max=%u interval=%u\n", r, cc, st.Width, st.Height, st.MaxFrameSize,
                      st.Interval);
        return 1;
    }
    if (Is(c, "vgrab")) {
        USBNT_VID_GET g;
        ULONG n, got = 0, errs = 0, t0 = 0, t1 = 0, first = 0, timeout;
        USBNT_VID_FRAME_HDR last;
        p = c + 5;
        g.Id = Num(&p);
        n = Num(&p);
        timeout = Num(&p);
        g.LastSequence = 0;
        g.TimeoutMs = timeout ? timeout : 3000;
        UsbMemSet(&last, 0, sizeof(last));
        for (i = 0; i < n; i++) {
            LONG r = UvcGetFrame(&g, FrameBuf, sizeof(FrameBuf));
            USBNT_VID_FRAME_HDR *h = (USBNT_VID_FRAME_HDR *)FrameBuf;
            char cc[5];
            if (r <= 0) {
                HarnessPrintf("VFRAME none r=%d\n", (int)r);
                break;
            }
            if (got == 0) {
                t0 = OsTimeMs();
                first = h->Sequence;
            }
            t1 = OsTimeMs();
            got++;
            if (h->Flags) {
                errs++;
            }
            Fcc(cc, h->FourCC);
            HarnessPrintf("VFRAME seq=%u bytes=%u %s %ux%u flags=%x\n", h->Sequence, h->Bytes, cc, h->Width,
                          h->Height, h->Flags);
            g.LastSequence = h->Sequence;
            last = *h;
            if (i + 1 == n || i == 0) {
                Analyse(h, FrameBuf + sizeof(*h));
            }
        }
        HarnessPrintf("VGRAB frames=%u errors=%u seqspan=%u ms=%u\n", got, errs,
                      got ? last.Sequence - first : 0, t1 - t0);
        return 1;
    }
    if (Is(c, "vctl")) {
        USBNT_VID_CONTROL v;
        int r;
        p = c + 4;
        UsbMemSet(&v, 0, sizeof(v));
        v.Id = Num(&p);
        v.Selector = Num(&p);
        while (*p == ' ') {
            p++;
        }
        if (*p) {
            v.Set = 1;
            v.Value = (LONG)Num(&p);
        }
        r = UvcControl(&v);
        HarnessPrintf("VCTL r=%d sel=%u value=%d min=%d max=%d def=%d res=%d\n", r, v.Selector, (int)v.Value,
                      (int)v.Min, (int)v.Max, (int)v.Def, (int)v.Res);
        return 1;
    }
    if (Is(c, "vstop")) {
        p = c + 5;
        HarnessPrintf("VSTOP r=%d\n", UvcStop(Num(&p)));
        return 1;
    }
    return 0;
}
