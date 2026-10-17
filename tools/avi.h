/*
 * avi.h - AVI 1.0 (RIFF) writer for usbcam: one video stream, idx1 index
 */
#ifndef AVI_H
#define AVI_H

typedef struct _AVI_IO {
    void *Ctx;
    int (*Write)(void *Ctx, const void *Data, unsigned long Len);   /* 0 = ok */
    int (*Seek)(void *Ctx, unsigned long Pos);                       /* absolute */
} AVI_IO;

typedef struct _AVI_INDEX {
    unsigned long Id, Flags, Offset, Size;
} AVI_INDEX;

typedef struct _AVI_WRITER {
    AVI_IO          Io;
    unsigned long   Pos;            /* bytes written */
    unsigned long   MoviPos;        /* offset of the 'movi' fourcc */
    unsigned long   Frames;
    unsigned long   MaxChunk;
    unsigned long   UsPerFrame;
    AVI_INDEX      *Index;
    unsigned long   IndexCap;
    int             Width, Height;
    unsigned long   Comp;
    int             Bits;
    int             Error;
} AVI_WRITER;

#define AVI_FCC(a, b, c, d) ((unsigned long)(unsigned char)(a) | ((unsigned long)(unsigned char)(b) << 8) | \
                             ((unsigned long)(unsigned char)(c) << 16) | ((unsigned long)(unsigned char)(d) << 24))

/* Compression: AVI_FCC('M','J','P','G'), AVI_FCC('Y','U','Y','2') or 0 (24-bit bottom-up DIB) */
int AviOpen(AVI_WRITER *A, const AVI_IO *Io, unsigned long Compression, int Width, int Height, int BitCount,
            unsigned long UsPerFrame, AVI_INDEX *Index, unsigned long IndexCap);
int AviFrame(AVI_WRITER *A, const void *Data, unsigned long Len);
/* writes the index and patches the headers; UsPerFrame (measured) replaces the
   rate given to AviOpen when not 0 */
int AviClose(AVI_WRITER *A, unsigned long UsPerFrame);
/* bytes a frame of Len adds (chunk header, padding, index entry) */
#define AVI_FRAME_COST(len) ((((len) + 1UL) & ~1UL) + 8UL + 16UL)

#endif
