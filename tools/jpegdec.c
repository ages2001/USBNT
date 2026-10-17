/*
 * jpegdec.c - small baseline JPEG decoder for usbcam (see jpegdec.h)
 *
 * Own implementation of ITU-T T.81 baseline decoding: marker parsing, canonical
 * Huffman decoding with a 9-bit lookup table, the Arai-Agui-Nakajima scaled
 * IDCT (integer, the scale factors folded into the dequantisation table),
 * triangle filter chroma upsampling and BT.601 (JFIF) colour conversion in
 * fixed point.  No floating point, so it is quick on emulated CPUs too.
 */
#include <stddef.h>
#include "jpegdec.h"

#define FAST_BITS 9

typedef struct _HUFF {
    unsigned char  Present;
    unsigned char  Fast[1 << FAST_BITS];    /* index into Val + 1, 0 = slow path */
    unsigned char  FastLen[1 << FAST_BITS];
    unsigned char  Val[256];
    unsigned short Code[256];
    unsigned char  Size[256];
    long           MaxCode[18];             /* largest code of each length (left aligned 16 bit), -1 none */
    int            ValPtr[17];              /* index of the first code of each length */
    unsigned short MinCode[17];
    int            Count;
} HUFF;

typedef struct _COMP {
    int Id, H, V, Tq, Td, Ta;
    int Pred;
    unsigned char *Row[3];                  /* samples of an MCU row: previous, current, next */
    long Stride;                            /* bytes per sample line of Row */
} COMP;

typedef struct _DEC {
    const unsigned char *P, *End;
    unsigned int   Acc;
    int            Bits;
    int            Marker;                  /* a marker was reached in the entropy data */
    int            Short;                   /* ran out of data */
    long           Q[4][64];                /* dequantisation x AAN scale x 8 (pass bits), 24.8 */
    int            QPresent[4];
    HUFF           Dc[4], Ac[4];
    COMP           C[3];
    int            Nc, Width, Height, Hmax, Vmax;
    int            Restart;
    int            Sof;
    int            HasDht;
    unsigned char *Line[3];                 /* one upsampled output line per component */
    int           *Col;                     /* vertical filter sums of one line (RowStride + 2) */
} DEC;

static const unsigned char ZigZag[64] = {
     0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* JPEG Annex K.3 tables (also what UVC MJPEG implies when DHT is absent) */
static const unsigned char DcLumBits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const unsigned char DcChrBits[16] = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const unsigned char DcVal[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const unsigned char AcLumBits[16] = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d };
static const unsigned char AcLumVal[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};
static const unsigned char AcChrBits[16] = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77 };
static const unsigned char AcChrVal[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

/* AAN scale factors s(u) * s(v) in 2.14 fixed point, s(k) = cos(k pi / 16) * sqrt(2), s(0) = 1 */
static const unsigned short AanScale[64] = {
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    22725, 31521, 29692, 26722, 22725, 17855, 12299,  6270,
    21407, 29692, 27969, 25172, 21407, 16819, 11585,  5906,
    19266, 26722, 25172, 22654, 19266, 15137, 10426,  5315,
    16384, 22725, 21407, 19266, 16384, 12873,  8867,  4520,
    12873, 17855, 16819, 15137, 12873, 10114,  6967,  3552,
     8867, 12299, 11585, 10426,  8867,  6967,  4799,  2446,
     4520,  6270,  5906,  5315,  4520,  3552,  2446,  1247,
};

static void Zero(void *p, unsigned long n)
{
    volatile unsigned char *d = (volatile unsigned char *)p;
    while (n--) *d++ = 0;
}

static unsigned Rd16(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

static int BuildHuff(HUFF *h, const unsigned char *Bits, const unsigned char *Val)
{
    int i, j, k = 0, len;
    unsigned code = 0;
    Zero(h, sizeof(*h));
    for (i = 0; i < 16; i++) k += Bits[i];
    if (k > 256) return 0;
    h->Count = k;
    for (i = 0; i < k; i++) h->Val[i] = Val[i];
    k = 0;
    for (len = 1; len <= 16; len++) {
        h->ValPtr[len] = k;
        h->MinCode[len] = (unsigned short)code;
        for (j = 0; j < Bits[len - 1]; j++) {
            h->Code[k] = (unsigned short)code;
            h->Size[k] = (unsigned char)len;
            code++;
            k++;
        }
        h->MaxCode[len] = Bits[len - 1] ? (long)(code - 1) : -1;
        if (code > (1U << len)) return 0;               /* over-subscribed */
        code <<= 1;
    }
    h->MaxCode[17] = 0x7FFFFFFF;
    for (i = 0; i < k; i++) {
        int s = h->Size[i];
        if (s <= FAST_BITS) {
            int first = h->Code[i] << (FAST_BITS - s), n = 1 << (FAST_BITS - s);
            for (j = 0; j < n; j++) {
                h->Fast[first + j] = (unsigned char)(i + 1 > 255 ? 0 : i + 1);
                h->FastLen[first + j] = (unsigned char)s;
            }
        }
    }
    h->Present = 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* entropy coded data                                                  */
/* ------------------------------------------------------------------ */
static void Fill(DEC *d)
{
    while (d->Bits <= 24) {
        unsigned b = 0;
        if (!d->Marker && d->P < d->End) {
            b = *d->P++;
            if (b == 0xFF) {
                unsigned n = d->P < d->End ? *d->P : 0xD9;
                if (n == 0x00) {
                    d->P++;                             /* stuffed byte */
                } else {
                    d->P--;                             /* a marker: leave it */
                    d->Marker = 1;
                    b = 0;
                }
            }
        } else if (!d->Marker) {
            d->Short = 1;
        }
        d->Acc |= (unsigned int)b << (24 - d->Bits);
        d->Bits += 8;
    }
}

static unsigned GetBits(DEC *d, int n)
{
    unsigned v;
    if (n == 0) return 0;
    if (d->Bits < n) Fill(d);
    v = (unsigned)(d->Acc >> (32 - n));
    d->Acc <<= n;
    d->Bits -= n;
    return v;
}

static int Extend(unsigned v, int s)
{
    return (s && v < (1U << (s - 1))) ? (int)v - (1 << s) + 1 : (int)v;
}

static int Decode(DEC *d, const HUFF *h)
{
    unsigned look;
    int k, len;
    unsigned code;
    if (d->Bits < 16) Fill(d);
    look = (unsigned)(d->Acc >> (32 - FAST_BITS));
    k = h->Fast[look];
    if (k) {
        len = h->FastLen[look];
        d->Acc <<= len;
        d->Bits -= len;
        return h->Val[k - 1];
    }
    code = d->Acc >> 16;                                /* 16 bits, left aligned */
    for (len = FAST_BITS + 1; len <= 16; len++) {
        unsigned c = code >> (16 - len);
        if (h->MaxCode[len] >= 0 && (long)c <= h->MaxCode[len]) {
            int i = h->ValPtr[len] + (int)(c - h->MinCode[len]);
            d->Acc <<= len;
            d->Bits -= len;
            return i < h->Count ? h->Val[i] : 0;
        }
    }
    d->Acc <<= 16;                                      /* bad code */
    d->Bits -= 16;
    return 0;
}

static void ResetBits(DEC *d)
{
    d->Acc = 0;
    d->Bits = 0;
    d->Marker = 0;
}

/* ------------------------------------------------------------------ */
/* IDCT                                                                */
/* ------------------------------------------------------------------ */
static unsigned char Clamp(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

/* the AAN flow graph in integers: constants in 22.10, the input carries 3
   extra bits (pass bits) that the second pass takes off again */
#define FIX(x)          ((long)((x) * 1024 + 0.5))
#define MUL(a, c)       (((a) * (c) + 512) >> 10)
#define C1414           FIX(1.414213562)
#define C1847           FIX(1.847759065)
#define C1082           FIX(1.082392200)
#define C2613           FIX(2.613125930)

static void Idct(long *ws, unsigned char *out, int stride)
{
    int i;
    long t0, t1, t2, t3, t4, t5, t6, t7, t10, t11, t12, t13, z5, z10, z11, z12, z13;
    long *p;
    /* columns */
    for (i = 0; i < 8; i++) {
        p = ws + i;
        if ((p[8] | p[16] | p[24] | p[32] | p[40] | p[48] | p[56]) == 0) {
            long dc = p[0];
            p[8] = p[16] = p[24] = p[32] = p[40] = p[48] = p[56] = dc;
            continue;
        }
        t0 = p[0]; t1 = p[16]; t2 = p[32]; t3 = p[48];
        t10 = t0 + t2; t11 = t0 - t2;
        t13 = t1 + t3; t12 = MUL(t1 - t3, C1414) - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;
        t4 = p[8]; t5 = p[24]; t6 = p[40]; t7 = p[56];
        z13 = t6 + t5; z10 = t6 - t5; z11 = t4 + t7; z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = MUL(z11 - z13, C1414);
        z5 = MUL(z10 + z12, C1847);
        t10 = z5 - MUL(z12, C1082);
        t12 = z5 - MUL(z10, C2613);
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 - t5;
        p[0] = t0 + t7; p[56] = t0 - t7; p[8] = t1 + t6; p[48] = t1 - t6;
        p[16] = t2 + t5; p[40] = t2 - t5; p[24] = t3 + t4; p[32] = t3 - t4;
    }
    /* rows */
    for (i = 0; i < 8; i++) {
        unsigned char *o = out + i * stride;
        p = ws + i * 8;
        t0 = p[0]; t1 = p[2]; t2 = p[4]; t3 = p[6];
        t10 = t0 + t2; t11 = t0 - t2;
        t13 = t1 + t3; t12 = MUL(t1 - t3, C1414) - t13;
        t0 = t10 + t13; t3 = t10 - t13; t1 = t11 + t12; t2 = t11 - t12;
        t4 = p[1]; t5 = p[3]; t6 = p[5]; t7 = p[7];
        z13 = t6 + t5; z10 = t6 - t5; z11 = t4 + t7; z12 = t4 - t7;
        t7 = z11 + z13;
        t11 = MUL(z11 - z13, C1414);
        z5 = MUL(z10 + z12, C1847);
        t10 = z5 - MUL(z12, C1082);
        t12 = z5 - MUL(z10, C2613);
        t6 = t12 - t7; t5 = t11 - t6; t4 = t10 - t5;
#define PX(v) Clamp((int)(((v) + 4) >> 3) + 128)
        o[0] = PX(t0 + t7); o[7] = PX(t0 - t7);
        o[1] = PX(t1 + t6); o[6] = PX(t1 - t6);
        o[2] = PX(t2 + t5); o[5] = PX(t2 - t5);
        o[3] = PX(t3 + t4); o[4] = PX(t3 - t4);
#undef PX
    }
}

static int DecodeBlock(DEC *d, COMP *c, unsigned char *out, int stride)
{
    long ws[64];
    const long *q = d->Q[c->Tq];
    int k, s, r;
    for (k = 0; k < 64; k++) ws[k] = 0;
    s = Decode(d, &d->Dc[c->Td]);
    if (s > 11) s = 11;
    c->Pred += Extend(GetBits(d, s), s);
    ws[0] = (c->Pred * q[0] + 128) >> 8;
    for (k = 1; k < 64;) {
        int rs = Decode(d, &d->Ac[c->Ta]);
        r = rs >> 4;
        s = rs & 15;
        if (s == 0) {
            if (r != 15) break;                         /* end of block */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63) break;
        ws[ZigZag[k]] = (Extend(GetBits(d, s), s) * q[ZigZag[k]] + 128) >> 8;
        k++;
    }
    Idct(ws, out, stride);
    return 0;
}

/* ------------------------------------------------------------------ */
/* headers                                                             */
/* ------------------------------------------------------------------ */
static int Segment(DEC *d, int m, const unsigned char *p, unsigned len)
{
    unsigned i;
    switch (m) {
    case 0xDB:                                          /* DQT */
        while (len > 0) {
            int pq = p[0] >> 4, tq = p[0] & 3, k;
            unsigned need = 1 + (pq ? 128 : 64);
            if (len < need) return JPEG_EFORMAT;
            for (k = 0; k < 64; k++) {
                int z = ZigZag[k];
                unsigned qv = pq ? Rd16(p + 1 + 2 * k) : p[1 + k];
                d->Q[tq][z] = (long)((qv * (unsigned long)AanScale[z] + 32) >> 6);
            }
            d->QPresent[tq] = 1;
            p += need;
            len -= need;
        }
        return JPEG_OK;
    case 0xC4:                                          /* DHT */
        while (len > 17) {
            int tc = p[0] >> 4, th = p[0] & 3, n = 0;
            for (i = 0; i < 16; i++) n += p[1 + i];
            if (len < 17U + n || tc > 1) return JPEG_EFORMAT;
            if (!BuildHuff(tc ? &d->Ac[th] : &d->Dc[th], p + 1, p + 17)) return JPEG_EFORMAT;
            d->HasDht = 1;
            p += 17 + n;
            len -= 17 + n;
        }
        return JPEG_OK;
    case 0xDD:                                          /* DRI */
        if (len >= 2) d->Restart = (int)Rd16(p);
        return JPEG_OK;
    case 0xC0:
    case 0xC1:                                          /* SOF baseline / extended Huffman */
        if (len < 6 || p[0] != 8) return JPEG_EUNSUPP;
        d->Height = (int)Rd16(p + 1);
        d->Width = (int)Rd16(p + 3);
        d->Nc = p[5];
        if ((d->Nc != 1 && d->Nc != 3) || len < 6U + 3 * d->Nc || d->Width == 0 || d->Height == 0) return JPEG_EUNSUPP;
        d->Hmax = d->Vmax = 1;
        for (i = 0; i < (unsigned)d->Nc; i++) {
            COMP *c = &d->C[i];
            c->Id = p[6 + 3 * i];
            c->H = p[7 + 3 * i] >> 4;
            c->V = p[7 + 3 * i] & 15;
            c->Tq = p[8 + 3 * i] & 3;
            if (c->H < 1 || c->H > 2 || c->V < 1 || c->V > 2) return JPEG_EUNSUPP;
            if (c->H > d->Hmax) d->Hmax = c->H;
            if (c->V > d->Vmax) d->Vmax = c->V;
        }
        if (d->Nc == 1) d->C[0].H = d->C[0].V = d->Hmax = d->Vmax = 1;  /* grey: one block per MCU */
        d->Sof = 1;
        return JPEG_OK;
    default:
        if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) return JPEG_EUNSUPP;
        return JPEG_OK;                                 /* APPn, COM, ... */
    }
}

static void DefaultTables(DEC *d)
{
    if (!d->Dc[0].Present) BuildHuff(&d->Dc[0], DcLumBits, DcVal);
    if (!d->Dc[1].Present) BuildHuff(&d->Dc[1], DcChrBits, DcVal);
    if (!d->Ac[0].Present) BuildHuff(&d->Ac[0], AcLumBits, AcLumVal);
    if (!d->Ac[1].Present) BuildHuff(&d->Ac[1], AcChrBits, AcChrVal);
}

/* walks the headers up to SOS; returns the offset of the scan header or < 0 */
static long Headers(DEC *d, const unsigned char *Data, unsigned long Len)
{
    unsigned long i = 2;
    int r;
    if (Len < 4 || Data[0] != 0xFF || Data[1] != 0xD8) return JPEG_EFORMAT;
    while (i + 4 <= Len) {
        int m;
        unsigned len;
        if (Data[i] != 0xFF) { i++; continue; }        /* tolerate junk between segments */
        m = Data[i + 1];
        if (m == 0xFF) { i++; continue; }               /* fill byte */
        if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        if (m == 0xD9) return JPEG_EFORMAT;
        len = Rd16(Data + i + 2);
        if (len < 2 || i + 2 + len > Len) return JPEG_EFORMAT;
        if (m == 0xDA) return (long)i;
        r = Segment(d, m, Data + i + 4, len - 2);
        if (r != JPEG_OK) return r;
        i += 2 + len;
    }
    return JPEG_EFORMAT;
}

int JpegInfo(const unsigned char *Data, unsigned long Len, JPEG_INFO *Info)
{
    static DEC d;                                       /* only small parts used, no tables built */
    long r;
    int i;
    Zero(&d, sizeof(d));
    r = Headers(&d, Data, Len);
    Zero(Info, sizeof(*Info));
    if (r < 0) return (int)r;
    if (!d.Sof) return JPEG_EFORMAT;
    Info->Width = d.Width;
    Info->Height = d.Height;
    Info->Components = d.Nc;
    Info->HasDht = d.HasDht;
    for (i = 0; i < d.Nc; i++) { Info->H[i] = d.C[i].H; Info->V[i] = d.C[i].V; }
    return JPEG_OK;
}

static long RowStride(int Width) { return ((long)(Width + 15) / 16) * 16 + 16; }

unsigned long JpegWorkSize(int Width)
{
    /* 3 components x 3 MCU rows x 16 lines + 3 output lines + the filter sums
       (kept off the stack: the tools have no stack probes, a frame of more than
       a page would jump over the guard page of a thread's stack) */
    return sizeof(DEC) + 16 + (unsigned long)RowStride(Width) * (3 * 3 * 16 + 3) +
           ((unsigned long)RowStride(Width) + 2) * sizeof(int) + 16;
}

/* the sample line of component c for output line y of the MCU row in Row[1]
   (upsampled like libjpeg's "fancy" upsampling: triangle filter) */
static void CompLine(DEC *d, COMP *c, int y, int W, unsigned char *out)
{
    int n = (W + (d->Hmax / c->H) - 1) / (d->Hmax / c->H);  /* samples of this component */
    int i;
    unsigned char *cur;
    if (c->V == d->Vmax) {
        cur = c->Row[1] + (long)y * c->Stride;
        if (c->H == d->Hmax) {
            for (i = 0; i < W; i++) out[i] = cur[i];
        } else {                                            /* h2v1 */
            if (n == 1) { for (i = 0; i < W; i++) out[i] = cur[0]; return; }
            out[0] = cur[0];
            out[1] = (unsigned char)((cur[0] * 3 + cur[1] + 2) >> 2);
            for (i = 1; i < n - 1; i++) {
                out[2 * i] = (unsigned char)((cur[i] * 3 + cur[i - 1] + 1) >> 2);
                out[2 * i + 1] = (unsigned char)((cur[i] * 3 + cur[i + 1] + 2) >> 2);
            }
            out[2 * i] = (unsigned char)((cur[i] * 3 + cur[i - 1] + 1) >> 2);
            if (2 * i + 1 < W) out[2 * i + 1] = cur[i];
        }
        return;
    }
    {
        /* v2: the nearer source line counts 3/4, the farther 1/4 */
        int ys = y >> 1, k;
        const unsigned char *near = c->Row[1] + (long)ys * c->Stride, *far;
        int *col = d->Col;
        col[0] = col[1] = 0;
        if (y & 1) far = ys + 1 < 8 ? near + c->Stride : c->Row[2];                 /* first line of the next row */
        else far = ys > 0 ? near - c->Stride : c->Row[0] + 7 * c->Stride;          /* last line of the previous row */
        if (n > c->Stride) n = (int)c->Stride;
        for (k = 0; k < n; k++) col[k] = near[k] * 3 + far[k];
        if (c->H == d->Hmax) {
            for (i = 0; i < W && i < n; i++) out[i] = (unsigned char)((col[i] + 2) >> 2);
            return;
        }
        if (n == 1) { for (i = 0; i < W; i++) out[i] = (unsigned char)((col[0] + 2) >> 2); return; }
        out[0] = (unsigned char)((col[0] * 4 + 8) >> 4);
        out[1] = (unsigned char)((col[0] * 3 + col[1] + 7) >> 4);
        for (i = 1; i < n - 1; i++) {
            out[2 * i] = (unsigned char)((col[i] * 3 + col[i - 1] + 8) >> 4);
            out[2 * i + 1] = (unsigned char)((col[i] * 3 + col[i + 1] + 7) >> 4);
        }
        out[2 * i] = (unsigned char)((col[i] * 3 + col[i - 1] + 8) >> 4);
        if (2 * i + 1 < W) out[2 * i + 1] = (unsigned char)((col[i] * 4 + 7) >> 4);
    }
}

/* colour converts the MCU row held in Row[1] */
static void StoreRow(DEC *d, int my, unsigned char *Bgr, long Stride, int BottomUp)
{
    int mh = d->Vmax * 8, y, x, W = d->Width, H = d->Height;
    for (y = 0; y < mh; y++) {
        int iy = my * mh + y;
        unsigned char *o;
        if (iy >= H) break;
        o = Bgr + (long)(BottomUp ? H - 1 - iy : iy) * Stride;
        if (d->Nc == 1) {
            const unsigned char *s = d->C[0].Row[1] + (long)y * d->C[0].Stride;
            for (x = 0; x < W; x++, o += 3) o[0] = o[1] = o[2] = s[x];
            continue;
        }
        CompLine(d, &d->C[0], y, W, d->Line[0]);
        CompLine(d, &d->C[1], y, W, d->Line[1]);
        CompLine(d, &d->C[2], y, W, d->Line[2]);
        for (x = 0; x < W; x++, o += 3) {
            int Y = d->Line[0][x] << 16, Cb = d->Line[1][x] - 128, Cr = d->Line[2][x] - 128;
            /* R = Y + 1.402 Cr, G = Y - 0.344136 Cb - 0.714136 Cr, B = Y + 1.772 Cb (16.16) */
            o[2] = Clamp((Y + 91881 * Cr + 32768) >> 16);
            o[1] = Clamp((Y - 22554 * Cb - 46802 * Cr + 32768) >> 16);
            o[0] = Clamp((Y + 116130 * Cb + 32768) >> 16);
        }
    }
}

static void Rotate(DEC *d)
{
    int i;
    for (i = 0; i < d->Nc; i++) {
        unsigned char *t = d->C[i].Row[0];
        d->C[i].Row[0] = d->C[i].Row[1];
        d->C[i].Row[1] = d->C[i].Row[2];
        d->C[i].Row[2] = t;
    }
}

int JpegDecode(const unsigned char *Data, unsigned long Len, unsigned char *Bgr, long Stride, int BottomUp,
               int MaxW, int MaxH, void *Work, unsigned long WorkSize, JPEG_INFO *Info)
{
    DEC *d = (DEC *)Work;
    long sos, rs;
    const unsigned char *p;
    unsigned char *mem;
    unsigned ns, i, len;
    int mcux, mcuy, mx, my, todo, rstn = 0, result = JPEG_OK;

    if (WorkSize < sizeof(DEC)) return JPEG_ESIZE;
    Zero(d, sizeof(*d));
    sos = Headers(d, Data, Len);
    if (Info) Zero(Info, sizeof(*Info));
    if (sos < 0) return (int)sos;
    if (!d->Sof) return JPEG_EFORMAT;
    if (Info) {
        Info->Width = d->Width;
        Info->Height = d->Height;
        Info->Components = d->Nc;
        Info->HasDht = d->HasDht;
        for (i = 0; i < (unsigned)d->Nc; i++) { Info->H[i] = d->C[i].H; Info->V[i] = d->C[i].V; }
    }
    if (d->Width > MaxW || d->Height > MaxH || d->Width > 4096 || WorkSize < JpegWorkSize(d->Width)) return JPEG_ESIZE;
    DefaultTables(d);
    /* row buffers */
    rs = RowStride(d->Width);
    mem = (unsigned char *)(((size_t)(d + 1) + 15) & ~(size_t)15);
    for (i = 0; i < (unsigned)d->Nc; i++) {
        int k;
        d->C[i].Stride = rs;
        for (k = 0; k < 3; k++) { d->C[i].Row[k] = mem; mem += rs * 16; }
    }
    for (i = 0; i < 3; i++) { d->Line[i] = mem; mem += rs; }
    mem = (unsigned char *)(((size_t)mem + 15) & ~(size_t)15);
    d->Col = (int *)mem;
    mem += (rs + 2) * (long)sizeof(int);
    /* scan header */
    p = Data + sos + 4;
    len = Rd16(Data + sos + 2);
    ns = p[0];
    if (ns != (unsigned)d->Nc || len < 6 + 2 * ns) return JPEG_EUNSUPP;   /* one interleaved scan */
    for (i = 0; i < ns; i++) {
        int id = p[1 + 2 * i], k;
        for (k = 0; k < d->Nc; k++) {
            if (d->C[k].Id == id) {
                d->C[k].Td = p[2 + 2 * i] >> 4 & 3;
                d->C[k].Ta = p[2 + 2 * i] & 3;
            }
        }
    }
    for (i = 0; i < (unsigned)d->Nc; i++) {
        if (!d->QPresent[d->C[i].Tq] || !d->Dc[d->C[i].Td].Present || !d->Ac[d->C[i].Ta].Present) return JPEG_EFORMAT;
    }
    d->P = Data + sos + 2 + len;
    d->End = Data + Len;
    ResetBits(d);
    mcux = (d->Width + d->Hmax * 8 - 1) / (d->Hmax * 8);
    mcuy = (d->Height + d->Vmax * 8 - 1) / (d->Vmax * 8);
    todo = d->Restart;
    /* MCU rows are decoded into Row[2] and put out one row later, when the
       following row is there for the vertical chroma filter */
    for (my = 0; my <= mcuy; my++) {
        if (my < mcuy) {
            for (mx = 0; mx < mcux; mx++) {
                if (d->Restart && todo == 0) {
                    /* restart interval: byte align, skip the RSTn marker, reset predictions */
                    ResetBits(d);
                    while (d->P + 1 < d->End && !(d->P[0] == 0xFF && d->P[1] >= 0xD0 && d->P[1] <= 0xD7)) d->P++;
                    if (d->P + 1 < d->End) {
                        if ((d->P[1] & 7) != (rstn & 7)) result = JPEG_EDATA;
                        d->P += 2;
                    }
                    rstn++;
                    for (i = 0; i < (unsigned)d->Nc; i++) d->C[i].Pred = 0;
                    todo = d->Restart;
                }
                for (i = 0; i < (unsigned)d->Nc; i++) {
                    COMP *c = &d->C[i];
                    int bx, by;
                    for (by = 0; by < c->V; by++)
                        for (bx = 0; bx < c->H; bx++)
                            DecodeBlock(d, c, c->Row[2] + by * 8 * c->Stride + (mx * c->H + bx) * 8, (int)c->Stride);
                }
                if (d->Restart) todo--;
            }
            /* columns beyond the image repeat the last sample (filter edges) */
            for (i = 0; i < (unsigned)d->Nc; i++) {
                COMP *c = &d->C[i];
                int y, x, n = mcux * c->H * 8;
                for (y = 0; y < 16; y++)
                    for (x = n; x < c->Stride; x++) c->Row[2][y * c->Stride + x] = c->Row[2][y * c->Stride + n - 1];
            }
        } else {
            /* after the last row: the "next" row repeats the last line */
            for (i = 0; i < (unsigned)d->Nc; i++) {
                COMP *c = &d->C[i];
                long x;
                const unsigned char *last = c->Row[1] + (long)(c->V * 8 - 1) * c->Stride;
                for (x = 0; x < c->Stride * 16; x++) c->Row[2][x] = last[x % c->Stride];
            }
        }
        if (my == 0) {
            /* the first row has no previous row: repeat its first line */
            for (i = 0; i < (unsigned)d->Nc; i++) {
                COMP *c = &d->C[i];
                long x;
                for (x = 0; x < c->Stride * 16; x++) c->Row[1][x] = c->Row[2][x % c->Stride];
            }
        } else {
            StoreRow(d, my - 1, Bgr, Stride, BottomUp);
        }
        Rotate(d);
    }
    if (d->Short) result = JPEG_EDATA;
    return result;
}

/* ------------------------------------------------------------------ */
/* DHT insertion                                                       */
/* ------------------------------------------------------------------ */
static unsigned long PutTable(unsigned char *o, int cls, const unsigned char *bits, const unsigned char *val)
{
    unsigned long n = 0, i, k = 0;
    o[n++] = (unsigned char)cls;
    for (i = 0; i < 16; i++) { o[n++] = bits[i]; k += bits[i]; }
    for (i = 0; i < k; i++) o[n++] = val[i];
    return n;
}

unsigned long JpegAddDht(const unsigned char *In, unsigned long Len, unsigned char *Out, unsigned long Cap)
{
    static DEC d;
    long sos;
    unsigned long n = 0, i, seg;
    Zero(&d, sizeof(d));
    sos = Headers(&d, In, Len);
    if (sos < 0) return 0;
    if (d.HasDht) {
        if (Len > Cap) return 0;
        for (i = 0; i < Len; i++) Out[i] = In[i];
        return Len;
    }
    if (Len + JPEG_DHT_SIZE > Cap) return 0;
    for (i = 0; i < (unsigned long)sos; i++) Out[n++] = In[i];
    Out[n++] = 0xFF;
    Out[n++] = 0xC4;
    seg = n;
    n += 2;
    n += PutTable(Out + n, 0x00, DcLumBits, DcVal);
    n += PutTable(Out + n, 0x10, AcLumBits, AcLumVal);
    n += PutTable(Out + n, 0x01, DcChrBits, DcVal);
    n += PutTable(Out + n, 0x11, AcChrBits, AcChrVal);
    Out[seg] = (unsigned char)((n - seg) >> 8);
    Out[seg + 1] = (unsigned char)(n - seg);
    for (i = (unsigned long)sos; i < Len; i++) Out[n++] = In[i];
    return n;
}
