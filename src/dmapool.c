/*
 * dmapool.c - small block allocator for host controller data structures.
 *
 * Blocks of 32..4096 bytes are carved out of contiguous pages. Every block
 * is aligned to its size class, which satisfies all TD/QH/ED/context
 * alignment rules and guarantees that a block never crosses a page.
 */

#include "usbnt.h"

#define DMA_CLASSES     8           /* 32,64,...,4096 */
#define DMA_MAX_PAGES   512

typedef struct _DMA_FREE {
    struct _DMA_FREE *Next;
} DMA_FREE;

typedef struct _DMA_PAGE {
    UCHAR *Virt;
    ULONG  Phys;
} DMA_PAGE;

static DMA_FREE *DmaFree[DMA_CLASSES];
static DMA_PAGE  DmaPages[DMA_MAX_PAGES];
static ULONG     DmaPageCount;

static int DmaClass(ULONG Size)
{
    int c = 0;
    ULONG s = 32;

    while (s < Size && c < DMA_CLASSES - 1) {
        s <<= 1;
        c++;
    }
    return (Size <= s) ? c : -1;
}

static ULONG DmaPhys(void *Ptr)
{
    ULONG i;
    UCHAR *p = (UCHAR *)Ptr;

    for (i = 0; i < DmaPageCount; i++) {
        if (p >= DmaPages[i].Virt && p < DmaPages[i].Virt + USB_PAGE_SIZE) {
            return DmaPages[i].Phys + (ULONG)(p - DmaPages[i].Virt);
        }
    }
    return 0;
}

void *UsbDmaAlloc(ULONG Size, ULONG *Phys)
{
    int c = DmaClass(Size);
    ULONG csize;
    DMA_FREE *blk;
    OS_IRQL irql;

    if (c < 0) {
        return NULL;
    }
    csize = 32UL << c;

    irql = OsLock();
    blk = DmaFree[c];
    if (blk != NULL) {
        DmaFree[c] = blk->Next;
    }
    OsUnlock(irql);

    if (blk == NULL) {
        ULONG pphys;
        UCHAR *page;
        ULONG off;

        if (DmaPageCount >= DMA_MAX_PAGES) {
            return NULL;
        }
        page = (UCHAR *)OsDmaAlloc(USB_PAGE_SIZE, &pphys);
        if (page == NULL) {
            return NULL;
        }
        irql = OsLock();
        DmaPages[DmaPageCount].Virt = page;
        DmaPages[DmaPageCount].Phys = pphys;
        DmaPageCount++;
        for (off = csize; off < USB_PAGE_SIZE; off += csize) {
            DMA_FREE *f = (DMA_FREE *)(page + off);
            f->Next = DmaFree[c];
            DmaFree[c] = f;
        }
        OsUnlock(irql);
        blk = (DMA_FREE *)page;
    }

    UsbMemSet(blk, 0, csize);
    *Phys = DmaPhys(blk);
    return blk;
}

void UsbDmaFree(void *Ptr, ULONG Size)
{
    int c = DmaClass(Size);
    DMA_FREE *f = (DMA_FREE *)Ptr;
    OS_IRQL irql;

    if (Ptr == NULL || c < 0) {
        return;
    }
    irql = OsLock();
    f->Next = DmaFree[c];
    DmaFree[c] = f;
    OsUnlock(irql);
}
