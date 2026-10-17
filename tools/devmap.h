/*
 * devmap.h - slot to NT device and drive letter mapping (devmap.c).
 */

#ifndef DEVMAP_H
#define DEVMAP_H

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include "../src/usbntioc.h"

typedef struct _DEV_SLOT_MAP {
    char    Path[64];       /* NT path a drive letter points at, empty if unknown */
    UCHAR   Cd;
    int     Disk;           /* \Device\HarddiskN of a disk slot, -1 if none */
} DEV_SLOT_MAP;

#define DEV_MAX_PARTS   24  /* partitions of one disk the tools look at */

/* the volumes of a slot locked for its removal (NT 3.x / 4.0) */
typedef struct _DEV_LOCKED {
    int     Count;
    HANDLE  Vol[26];
    char    Letter[26];
} DEV_LOCKED;

int     DevMapSlots(ULONG ScsiPort, DEV_SLOT_MAP *Map, int Count);
char    DevMapLetter(const char *NtPath);
void    DevMapPartPath(const DEV_SLOT_MAP *M, int Part, char *Path);
int     DevMapPartInUse(const DEV_SLOT_MAP *M, int Part);
int     DevMapSlotLetters(const DEV_SLOT_MAP *M, char *Letters, int *Parts, int Max);
char    DevMapLockSlot(const DEV_SLOT_MAP *M, BOOL Force, int Tries, DEV_LOCKED *L);
void    DevMapUnlock(DEV_LOCKED *L);
BOOL    DevMapLetterUsed(char Letter);
int     DevMapSame(const char *A, const char *B);
BOOL    DevMapQueryTree(HANDLE Dev, USBNT_TREE *Tree);
BOOL    DevMapQueryTree2(HANDLE Dev, USBNT_TREE2 *Tree);

#endif
