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
} DEV_SLOT_MAP;

int     DevMapSlots(ULONG ScsiPort, DEV_SLOT_MAP *Map, int Count);
char    DevMapLetter(const char *NtPath);
BOOL    DevMapLetterUsed(char Letter);
int     DevMapSame(const char *A, const char *B);
BOOL    DevMapQueryTree(HANDLE Dev, USBNT_TREE *Tree);

#endif
