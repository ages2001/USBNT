/*
 * os.c - OS layer for the bare metal test kernel (no interrupts, flat
 * identity mapped memory, polling only).
 */

#include "../src/usbnt.h"
#include "harness.h"

extern char _end[];

static ULONG HeapPtr;
static ULONG HeapEnd = 0x07000000;
static ULONG TscPerMs;
static ULONG TscPerUs;
volatile ULONG HarnessWorkerWake;
static int InPoll;

static inline void outb(unsigned short p, unsigned char v) { __asm__ volatile("outb %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outw(unsigned short p, unsigned short v) { __asm__ volatile("outw %0,%1" : : "a"(v), "Nd"(p)); }
static inline void outl(unsigned short p, unsigned long v) { __asm__ volatile("outl %0,%1" : : "a"(v), "Nd"(p)); }
static inline unsigned char inb(unsigned short p) { unsigned char v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline unsigned short inw(unsigned short p) { unsigned short v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline unsigned long inl(unsigned short p) { unsigned long v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

static void rdtsc(ULONG *lo, ULONG *hi)
{
    __asm__ volatile("rdtsc" : "=a"(*lo), "=d"(*hi));
}

/* 64 bit by 32 bit division without libgcc */
static ULONG div64(ULONG hi, ULONG lo, ULONG d)
{
    ULONG q = 0;
    ULONG r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        ULONG bit = (i >= 32) ? ((hi >> (i - 32)) & 1) : ((lo >> i) & 1);
        ULONG carry = r >> 31;
        r = (r << 1) | bit;
        if (carry || r >= d) {
            r -= d;
            if (i < 32) {
                q |= 1UL << i;
            }
        }
    }
    return q;
}

void SerialInit(void)
{
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x80);
    outb(0x3F8, 0x01);
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);
}

void SerialPut(char c)
{
    while ((inb(0x3FD) & 0x20) == 0) {
    }
    outb(0x3F8, (unsigned char)c);
}

int SerialGet(void)
{
    if (inb(0x3FD) & 1) {
        return inb(0x3F8);
    }
    return -1;
}

void OsLogStr(const char *Str)
{
    while (*Str) {
        if (*Str == '\n') {
            SerialPut('\r');
        }
        SerialPut(*Str++);
    }
}

void HarnessPrintf(const char *Fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, Fmt);
    UsbVFormat(buf, sizeof(buf), Fmt, ap);
    va_end(ap);
    OsLogStr(buf);
}

void OsInitHarness(void)
{
    ULONG lo0, hi0, lo1, hi1;
    ULONG delta;

    HeapPtr = ((ULONG)_end + 0xFFFF) & ~0xFFFFUL;

    /* calibrate TSC with PIT channel 2, 10 ms one shot */
    outb(0x61, (inb(0x61) & ~0x02) | 0x01);
    outb(0x43, 0xB0);
    outb(0x42, 11932 & 0xFF);
    outb(0x42, 11932 >> 8);
    rdtsc(&lo0, &hi0);
    while ((inb(0x61) & 0x20) == 0) {
    }
    rdtsc(&lo1, &hi1);
    delta = lo1 - lo0;
    TscPerMs = delta / 10;
    if (TscPerMs < 1000) {
        TscPerMs = 1000;
    }
    TscPerUs = TscPerMs / 1000;
    if (TscPerUs == 0) {
        TscPerUs = 1;
    }
}

void *OsAlloc(ULONG Size)
{
    ULONG p = (HeapPtr + 15) & ~15UL;

    if (p + Size > HeapEnd) {
        return NULL;
    }
    HeapPtr = p + Size;
    UsbMemSet((void *)p, 0, Size);
    return (void *)p;
}

void OsFree(void *Ptr)
{
    (void)Ptr;
}

void *OsDmaAlloc(ULONG Size, ULONG *Phys)
{
    ULONG p = (HeapPtr + 4095) & ~4095UL;

    if (p + Size > HeapEnd) {
        return NULL;
    }
    HeapPtr = p + Size;
    UsbMemSet((void *)p, 0, Size);
    *Phys = p;
    return (void *)p;
}

void OsDmaFree(void *Ptr, ULONG Size)
{
    (void)Ptr;
    (void)Size;
}

ULONG OsTimeMs(void)
{
    ULONG lo, hi;

    rdtsc(&lo, &hi);
    return div64(hi, lo, TscPerMs);
}

void OsStallUs(ULONG Us)
{
    ULONG lo0, hi0, lo, hi;
    ULONG target = Us * TscPerUs;

    rdtsc(&lo0, &hi0);
    do {
        rdtsc(&lo, &hi);
    } while (lo - lo0 < target);
}

static void PollOnce(void)
{
    if (!InPoll) {
        InPoll = 1;
        UsbPollAll();
        HidTick();
        InPoll = 0;
    }
}

void OsSleepMs(ULONG Ms)
{
    ULONG start = OsTimeMs();

    while (OsTimeMs() - start < Ms) {
        PollOnce();
        OsStallUs(100);
    }
}

ULONG OsPciRead(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, int Size)
{
    ULONG addr = 0x80000000UL | ((ULONG)Bus << 16) | ((ULONG)Dev << 11) | ((ULONG)Fn << 8) | (Off & 0xFC);
    ULONG v;

    outl(0xCF8, addr);
    v = inl(0xCFC);
    v >>= (Off & 3) * 8;
    if (Size == 1) {
        v &= 0xFF;
    } else if (Size == 2) {
        v &= 0xFFFF;
    }
    return v;
}

void OsPciWrite(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, ULONG Val, int Size)
{
    ULONG addr = 0x80000000UL | ((ULONG)Bus << 16) | ((ULONG)Dev << 11) | ((ULONG)Fn << 8) | (Off & 0xFC);

    outl(0xCF8, addr);
    if (Size == 1) {
        outb((unsigned short)(0xCFC + (Off & 3)), (UCHAR)Val);
    } else if (Size == 2) {
        outw((unsigned short)(0xCFC + (Off & 2)), (USHORT)Val);
    } else {
        outl(0xCFC, Val);
    }
}

void *OsMapMmio(UCHAR Bus, ULONG Phys, ULONG Len)
{
    (void)Bus;
    (void)Len;
    return (void *)Phys;
}

UCHAR OsIn8(ULONG Port) { return inb((unsigned short)Port); }
USHORT OsIn16(ULONG Port) { return inw((unsigned short)Port); }
ULONG OsIn32(ULONG Port) { return inl((unsigned short)Port); }
void OsOut8(ULONG Port, UCHAR Val) { outb((unsigned short)Port, Val); }
void OsOut16(ULONG Port, USHORT Val) { outw((unsigned short)Port, Val); }
void OsOut32(ULONG Port, ULONG Val) { outl((unsigned short)Port, Val); }

OS_IRQL OsLock(void)
{
    return 0;
}

void OsUnlock(OS_IRQL Irql)
{
    (void)Irql;
}

void OsEventInit(OS_EVENT *Ev)
{
    Ev->Signaled = 0;
}

void OsEventSet(OS_EVENT *Ev)
{
    Ev->Signaled = 1;
}

void OsEventReset(OS_EVENT *Ev)
{
    Ev->Signaled = 0;
}

BOOLEAN OsEventWait(OS_EVENT *Ev, ULONG TimeoutMs)
{
    ULONG start = OsTimeMs();

    for (;;) {
        if (Ev->Signaled) {
            return TRUE;
        }
        PollOnce();
        if (Ev->Signaled) {
            return TRUE;
        }
        if (OsTimeMs() - start >= TimeoutMs) {
            return FALSE;
        }
        OsStallUs(20);
    }
}

void OsWakeWorker(void)
{
    HarnessWorkerWake = 1;
}

static struct {
    const char *Name;
    ULONG Value;
    int Set;
} Config[16];

void HarnessSetConfig(const char *Name, ULONG Value)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (Config[i].Set && UsbMemCmp(Config[i].Name, Name, 1) == 0) {
            const char *a = Config[i].Name;
            const char *b = Name;
            while (*a && *a == *b) {
                a++;
                b++;
            }
            if (*a == 0 && *b == 0) {
                Config[i].Value = Value;
                return;
            }
        }
    }
    for (i = 0; i < 16; i++) {
        if (!Config[i].Set) {
            Config[i].Name = Name;
            Config[i].Value = Value;
            Config[i].Set = 1;
            return;
        }
    }
}

ULONG OsGetConfig(const char *Name, ULONG Default)
{
    int i;

    for (i = 0; i < 16; i++) {
        if (Config[i].Set) {
            const char *a = Config[i].Name;
            const char *b = Name;
            while (*a && *a == *b) {
                a++;
                b++;
            }
            if (*a == 0 && *b == 0) {
                return Config[i].Value;
            }
        }
    }
    return Default;
}
void OsNotifyChange(void) { }
