/*
 * fsdetect.h - partition and file system identification of a USB disk slot
 * by reading the medium itself (fsdetect.c).
 */

#ifndef FSDETECT_H
#define FSDETECT_H

#ifndef FSD_HOST
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#endif

#define FSD_LABEL   40

typedef struct _FS_PART {
    ULONGLONG   Start;          /* first block */
    ULONGLONG   Length;         /* blocks */
    UCHAR       MbrType;        /* MBR partition type, 0xEE = GPT entry, 0 = no table */
    char        Scheme[12];     /* "MBR", "MBR logical", "GPT", "none" */
    char        Fs[48];         /* "NTFS 3.1 (Windows XP and later)", "FAT32", "ext4", ... */
    char        Label[FSD_LABEL];
    char        PartName[FSD_LABEL];   /* GPT partition name */
    ULONG       Version;        /* major << 8 | minor where the file system has one */
} FS_PART;

/* returns the number of partitions found, -1 if the medium cannot be read */
int FsDetectSlot(ULONG ScsiPort, ULONG Slot, ULONG BlockSize, ULONG LastLba, FS_PART *Parts, int Max);

#endif
