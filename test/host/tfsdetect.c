/* host test of tools/fsdetect.c against disk images: tfsdetect IMAGE [blocksize] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
typedef unsigned char UCHAR; typedef unsigned int ULONG; typedef int LONG; typedef unsigned long long ULONGLONG;
typedef int BOOL; typedef void *HANDLE; typedef unsigned short USHORT;
#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)-1)
#define GENERIC_READ 0
#define GENERIC_WRITE 0
#define FILE_SHARE_READ 0
#define FILE_SHARE_WRITE 0
#define OPEN_EXISTING 0
#define FSD_HOST
typedef struct { USHORT Length; UCHAR ScsiStatus, PathId, TargetId, Lun, CdbLength, SenseInfoLength, DataIn;
                 ULONG DataTransferLength, TimeOutValue; ULONG DataBufferOffset, SenseInfoOffset; UCHAR Cdb[16]; } SCSI_PASS_THROUGH;
static FILE *Img;
static HANDLE CreateFileA(const char *n, int a, int b, void *c, int d, int e, void *f) { return (HANDLE)1; }
static BOOL CloseHandle(HANDLE h) { return 1; }
#define wsprintfA sprintf
#define lstrcpyA strcpy
#define lstrcmpA strcmp
BOOL HostRead(ULONGLONG Lba, ULONG Count, ULONG Bs, UCHAR *Buf)
{
    memset(Buf, 0, Count * Bs);
    if (fseeko(Img, (off_t)(Lba * Bs), SEEK_SET)) return FALSE;
    fread(Buf, 1, Count * Bs, Img);
    return TRUE;
}
#include "../../tools/fsdetect.c"
int main(int argc, char **argv)
{
    FS_PART p[8]; int n, i; ULONG bs = argc > 2 ? atoi(argv[2]) : 512; long long sz;
    Img = fopen(argv[1], "rb"); if (!Img) return 2;
    fseeko(Img, 0, SEEK_END); sz = ftello(Img);
    n = FsDetectSlot(0, 0, bs, (ULONG)(sz / bs - 1), p, 8);
    printf("%s: %d partition(s)", argv[1], n);
    for (i = 0; i < n; i++) printf(" | %s %s @%llu+%llu fs=[%s] label=[%s]%s%s", p[i].Scheme, "", p[i].Start, p[i].Length, p[i].Fs, p[i].Label, p[i].PartName[0] ? " name=" : "", p[i].PartName);
    printf("\n");
    return 0;
}
