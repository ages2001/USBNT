/*
 * jpegdec.h - small baseline JPEG decoder for usbcam (MJPEG camera frames)
 *
 * Baseline sequential Huffman JPEG, 8-bit, 1 or 3 components, any sampling
 * factors up to 2x2, restart intervals.  Frames from UVC cameras usually come
 * without Huffman tables (DHT); the standard tables of JPEG Annex K are used
 * then, as the MJPEG convention says.  No C runtime needed.
 */
#ifndef JPEGDEC_H
#define JPEGDEC_H

typedef struct _JPEG_INFO {
    int Width;
    int Height;
    int Components;             /* 1 = grey, 3 = YCbCr */
    int HasDht;                 /* the data carries its own Huffman tables */
    int H[3], V[3];             /* sampling factors */
} JPEG_INFO;

#define JPEG_OK         0
#define JPEG_EFORMAT    -1      /* not a JPEG / damaged headers */
#define JPEG_EUNSUPP    -2      /* progressive, arithmetic, 12-bit, ... */
#define JPEG_EDATA      -3      /* entropy coded data ended early (picture partly decoded) */
#define JPEG_ESIZE      -4      /* larger than the output buffer */

/* headers only */
int JpegInfo(const unsigned char *Data, unsigned long Len, JPEG_INFO *Info);

/* decodes into 24-bit BGR pixels (a DIB when BottomUp is set): Stride bytes per
   row, room for MaxW x MaxH pixels.  Work is a scratch buffer of at least
   JpegWorkSize(Width) bytes. */
int JpegDecode(const unsigned char *Data, unsigned long Len, unsigned char *Bgr, long Stride, int BottomUp,
               int MaxW, int MaxH, void *Work, unsigned long WorkSize, JPEG_INFO *Info);
unsigned long JpegWorkSize(int Width);

/* copies a JPEG and inserts the standard Huffman tables when it has none, so
   ordinary viewers and AVI players can open it; returns the new length, 0 when
   Cap is too small */
unsigned long JpegAddDht(const unsigned char *In, unsigned long Len, unsigned char *Out, unsigned long Cap);
#define JPEG_DHT_SIZE   420     /* bytes the standard DHT segment adds */

#endif
