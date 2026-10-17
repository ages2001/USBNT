/*
 * avi.c - AVI 1.0 writer for usbcam (see avi.h)
 *
 *   RIFF 'AVI ' { LIST 'hdrl' { avih, LIST 'strl' { strh 'vids', strf BITMAPINFOHEADER } },
 *                 LIST 'movi' { '00dc' frames }, idx1 }
 */
#include "avi.h"

#define AVIF_HASINDEX       0x00000010
#define AVIIF_KEYFRAME      0x00000010

/* header offsets (from the start of the file) patched at the end */
#define OFF_RIFF_SIZE       4
#define OFF_AVIH            32              /* avih data */
#define OFF_STRH            (OFF_AVIH + 56 + 20)
#define OFF_STRF            (OFF_STRH + 56 + 8)
#define HDR_SIZE            (OFF_STRF + 40 + 12)    /* up to and including the 'movi' fourcc */

static void Put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void Put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

static int Out(AVI_WRITER *A, const void *d, unsigned long n)
{
    if (A->Error) return -1;
    if (A->Io.Write(A->Io.Ctx, d, n)) { A->Error = 1; return -1; }
    A->Pos += n;
    return 0;
}

static void Headers(AVI_WRITER *A, unsigned char *h, unsigned long comp, int bits, unsigned long riffSize,
                    unsigned long moviSize)
{
    unsigned long i, rate = A->UsPerFrame ? 1000000UL : 30, scale = A->UsPerFrame ? A->UsPerFrame : 1;
    unsigned long img = comp ? (unsigned long)A->Width * A->Height * 2 : (((unsigned long)A->Width * 3 + 3) & ~3UL) * A->Height;
    for (i = 0; i < HDR_SIZE; i++) h[i] = 0;
    Put32(h, AVI_FCC('R', 'I', 'F', 'F')); Put32(h + 4, riffSize); Put32(h + 8, AVI_FCC('A', 'V', 'I', ' '));
    Put32(h + 12, AVI_FCC('L', 'I', 'S', 'T')); Put32(h + 16, HDR_SIZE - 12 - 20); Put32(h + 20, AVI_FCC('h', 'd', 'r', 'l'));
    Put32(h + 24, AVI_FCC('a', 'v', 'i', 'h')); Put32(h + 28, 56);
    Put32(h + OFF_AVIH, A->UsPerFrame);
    Put32(h + OFF_AVIH + 4, A->UsPerFrame >= 1000 ? (A->MaxChunk + 8) * 1000UL / (A->UsPerFrame / 1000) : 0);
    Put32(h + OFF_AVIH + 12, AVIF_HASINDEX);
    Put32(h + OFF_AVIH + 16, A->Frames);
    Put32(h + OFF_AVIH + 24, 1);
    Put32(h + OFF_AVIH + 28, A->MaxChunk + 8);
    Put32(h + OFF_AVIH + 32, (unsigned long)A->Width);
    Put32(h + OFF_AVIH + 36, (unsigned long)A->Height);
    Put32(h + OFF_AVIH + 56, AVI_FCC('L', 'I', 'S', 'T')); Put32(h + OFF_AVIH + 60, 4 + 64 + 48);
    Put32(h + OFF_AVIH + 64, AVI_FCC('s', 't', 'r', 'l'));
    Put32(h + OFF_STRH - 8, AVI_FCC('s', 't', 'r', 'h')); Put32(h + OFF_STRH - 4, 56);
    Put32(h + OFF_STRH, AVI_FCC('v', 'i', 'd', 's'));
    Put32(h + OFF_STRH + 4, comp ? comp : AVI_FCC('D', 'I', 'B', ' '));
    Put32(h + OFF_STRH + 20, scale);
    Put32(h + OFF_STRH + 24, rate);
    Put32(h + OFF_STRH + 32, A->Frames);
    Put32(h + OFF_STRH + 36, A->MaxChunk + 8);
    Put32(h + OFF_STRH + 40, 0xFFFFFFFFUL);
    Put16(h + OFF_STRH + 52, (unsigned)A->Width);
    Put16(h + OFF_STRH + 54, (unsigned)A->Height);
    Put32(h + OFF_STRF - 8, AVI_FCC('s', 't', 'r', 'f')); Put32(h + OFF_STRF - 4, 40);
    Put32(h + OFF_STRF, 40);
    Put32(h + OFF_STRF + 4, (unsigned long)A->Width);
    Put32(h + OFF_STRF + 8, (unsigned long)A->Height);
    Put16(h + OFF_STRF + 12, 1);
    Put16(h + OFF_STRF + 14, (unsigned)bits);
    Put32(h + OFF_STRF + 16, comp);
    Put32(h + OFF_STRF + 20, img);
    Put32(h + OFF_STRF + 40, AVI_FCC('L', 'I', 'S', 'T')); Put32(h + OFF_STRF + 44, moviSize);
    Put32(h + OFF_STRF + 48, AVI_FCC('m', 'o', 'v', 'i'));
}

int AviOpen(AVI_WRITER *A, const AVI_IO *Io, unsigned long Compression, int Width, int Height, int BitCount,
            unsigned long UsPerFrame, AVI_INDEX *Index, unsigned long IndexCap)
{
    unsigned char h[HDR_SIZE];
    unsigned long i;
    unsigned char *z = (unsigned char *)A;
    for (i = 0; i < sizeof(*A); i++) z[i] = 0;
    A->Io = *Io;
    A->Width = Width;
    A->Height = Height;
    A->UsPerFrame = UsPerFrame;
    A->Index = Index;
    A->IndexCap = IndexCap;
    A->Comp = Compression;
    A->Bits = BitCount;
    Headers(A, h, Compression, BitCount, 0, 4);
    if (Out(A, h, HDR_SIZE)) return -1;
    A->MoviPos = HDR_SIZE - 4;
    return 0;
}

int AviFrame(AVI_WRITER *A, const void *Data, unsigned long Len)
{
    unsigned char ck[8], pad = 0;
    if (A->Error || A->Frames >= A->IndexCap) return -1;
    Put32(ck, AVI_FCC('0', '0', 'd', 'c'));
    Put32(ck + 4, Len);
    A->Index[A->Frames].Id = AVI_FCC('0', '0', 'd', 'c');
    A->Index[A->Frames].Flags = AVIIF_KEYFRAME;
    A->Index[A->Frames].Offset = A->Pos - A->MoviPos;
    A->Index[A->Frames].Size = Len;
    if (Out(A, ck, 8) || Out(A, Data, Len) || ((Len & 1) && Out(A, &pad, 1))) return -1;
    A->Frames++;
    if (Len > A->MaxChunk) A->MaxChunk = Len;
    return 0;
}

int AviClose(AVI_WRITER *A, unsigned long UsPerFrame)
{
    unsigned char h[HDR_SIZE], ck[8], e[16];
    unsigned long moviSize = A->Pos - A->MoviPos, i;
    Put32(ck, AVI_FCC('i', 'd', 'x', '1'));
    Put32(ck + 4, A->Frames * 16);
    Out(A, ck, 8);
    for (i = 0; i < A->Frames && !A->Error; i++) {
        Put32(e, A->Index[i].Id);
        Put32(e + 4, A->Index[i].Flags);
        Put32(e + 8, A->Index[i].Offset);
        Put32(e + 12, A->Index[i].Size);
        Out(A, e, 16);
    }
    if (UsPerFrame) A->UsPerFrame = UsPerFrame;
    Headers(A, h, A->Comp, A->Bits, A->Pos - 8, moviSize);
    if (A->Error || A->Io.Seek(A->Io.Ctx, 0) || A->Io.Write(A->Io.Ctx, h, HDR_SIZE)) return -1;
    return 0;
}
