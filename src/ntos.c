/*
 * ntos.c - OS layer for Windows NT 3.1 - 4.0.
 *
 * Only exports that exist in every version from NT 3.1 to 4.0 are used.
 */

#include "ntdrv.h"

#ifdef ExAllocatePool
#undef ExAllocatePool
#endif
#ifdef ExFreePool
#undef ExFreePool
#endif

#ifdef __GNUC__
/* the MinGW headers only declare the tagged variants */
NTKERNELAPI PVOID NTAPI ExAllocatePool(POOL_TYPE PoolType, SIZE_T NumberOfBytes);
NTKERNELAPI VOID NTAPI ExFreePool(PVOID P);
#endif

static ULONG TickCountLow(void)
{
    LARGE_INTEGER t;

    KeQueryTickCount(&t);
    return t.LowPart;
}

UNICODE_STRING NtRegistryPath;
KEVENT NtWorkerEvent;

static KSPIN_LOCK GlobalLock;
static KSPIN_LOCK LogLock;
static KSPIN_LOCK TimeLock;
static ULONG TickIncrement;         /* 100 ns units per clock tick */
static ULONG LastTick;
static ULONG TimeMs;
static ULONG TimeFrac;              /* 100 ns units below one millisecond */
static ULONG DebugPort;
static ULONG FastPollUs = 3000;

typedef struct _CFG_ENTRY {
    const char *Name;
    const WCHAR *WName;
    ULONG Value;
    BOOLEAN Present;
} CFG_ENTRY;

static CFG_ENTRY Config[] = {
    { "DebugLevel",         L"DebugLevel",         0, FALSE },
    { "DebugPort",          L"DebugPort",          0, FALSE },
    { "UseInterrupts",      L"UseInterrupts",      0, FALSE },
    { "MaxDisks",           L"MaxDisks",           0, FALSE },
    { "MaxPciBus",          L"MaxPciBus",          0, FALSE },
    { "DisableControllers", L"DisableControllers", 0, FALSE },
    { "IntelRouteToXhci",   L"IntelRouteToXhci",   0, FALSE },
    { "WheelMode",          L"WheelMode",          0, FALSE },
    { "ReportWheel",        L"ReportWheel",        0, FALSE },
    { "FastPollUs",         L"FastPollUs",         0, FALSE },
    { "BootWaitMs",         L"BootWaitMs",         0, FALSE },
    { "DisableKeyboard",    L"DisableKeyboard",    0, FALSE },
    { "DisableMouse",       L"DisableMouse",       0, FALSE },
    { "DisableStorage",     L"DisableStorage",     0, FALSE },
    { "NoPollTimer",        L"NoPollTimer",        0, FALSE },
    { "UseUas",             L"UseUas",             0, FALSE },
    { "MaxTransferKB",      L"MaxTransferKB",      0, FALSE },
    { "Usb3Hubs",           L"Usb3Hubs",           0, FALSE },
    { "TimerResolution",    L"TimerResolution",    0, FALSE },
    { NULL, NULL, 0, FALSE }
};

static LARGE_INTEGER RelativeMs(ULONG Ms)
{
    LARGE_INTEGER t;
    LONG v;

    if (Ms > 200000) {
        Ms = 200000;
    }
    v = -(LONG)(Ms * 10000);
    t.LowPart = (ULONG)v;
    t.HighPart = (v < 0) ? -1 : 0;
    return t;
}

static int StrEq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

ULONG OsGetConfig(const char *Name, ULONG Default)
{
    int i;

    for (i = 0; Config[i].Name != NULL; i++) {
        if (StrEq(Config[i].Name, Name)) {
            return Config[i].Present ? Config[i].Value : Default;
        }
    }
    return Default;
}

static BOOLEAN QueryDword(const WCHAR *Path, const WCHAR *Name, ULONG *Out)
{
    RTL_QUERY_REGISTRY_TABLE table[2];
    ULONG value = 0xFFFFFFFF;
    ULONG def = 0xFFFFFFFF;
    NTSTATUS st;

    RtlZeroMemory(table, sizeof(table));
    table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
    table[0].Name = (PWSTR)Name;
    table[0].EntryContext = &value;
    table[0].DefaultType = REG_DWORD;
    table[0].DefaultData = &def;
    table[0].DefaultLength = sizeof(ULONG);
    st = RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE, (PWSTR)Path, table, NULL, NULL);
    if (NT_SUCCESS(st) && value != 0xFFFFFFFF) {
        *Out = value;
        return TRUE;
    }
    return FALSE;
}

/* Values are read from <service>\Parameters, then from the service key itself */
static void ReadConfig(void)
{
    WCHAR root[256];
    WCHAR params[256];
    ULONG n = NtRegistryPath.Length / sizeof(WCHAR);
    const WCHAR *suffix = L"\\Parameters";
    ULONG i;
    int k;

    if (n + 12 >= 256) {
        return;
    }
    for (i = 0; i < n; i++) {
        root[i] = NtRegistryPath.Buffer[i];
        params[i] = NtRegistryPath.Buffer[i];
    }
    root[n] = 0;
    for (k = 0; suffix[k]; k++) {
        params[n + k] = suffix[k];
    }
    params[n + k] = 0;

    for (k = 0; Config[k].Name != NULL; k++) {
        if (QueryDword(params, Config[k].WName, &Config[k].Value) ||
            QueryDword(root, Config[k].WName, &Config[k].Value)) {
            Config[k].Present = TRUE;
        }
    }
}

void NtOsInit(PUNICODE_STRING RegistryPath)
{

    KeInitializeSpinLock(&GlobalLock);
    KeInitializeSpinLock(&LogLock);
    KeInitializeEvent(&NtWorkerEvent, SynchronizationEvent, FALSE);

    NtRegistryPath.Buffer = (PWSTR)ExAllocatePool(NonPagedPool, RegistryPath->Length + sizeof(WCHAR));
    if (NtRegistryPath.Buffer != NULL) {
        RtlMoveMemory(NtRegistryPath.Buffer, RegistryPath->Buffer, RegistryPath->Length);
        NtRegistryPath.Buffer[RegistryPath->Length / sizeof(WCHAR)] = 0;
        NtRegistryPath.Length = RegistryPath->Length;
        NtRegistryPath.MaximumLength = (USHORT)(RegistryPath->Length + sizeof(WCHAR));
        ReadConfig();
    }

    KeInitializeSpinLock(&TimeLock);
    TickIncrement = KeQueryTimeIncrement();
    if (TickIncrement == 0) {
        TickIncrement = 100000;
    }
    LastTick = TickCountLow();
    DebugPort = OsGetConfig("DebugPort", 0);
    FastPollUs = OsGetConfig("FastPollUs", 3000);
    if (DebugPort != 0) {
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 1), 0x00);
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 3), 0x80);
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 0), 0x01);
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 1), 0x00);
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 3), 0x03);
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 2), 0xC7);
        WRITE_PORT_UCHAR((PUCHAR)(DebugPort + 4), 0x03);
    }
}

/* ------------------------------------------------------------------ */

void *OsAlloc(ULONG Size)
{
    void *p = ExAllocatePool(NonPagedPool, Size ? Size : 1);

    if (p != NULL) {
        RtlZeroMemory(p, Size);
    }
    return p;
}

void OsFree(void *Ptr)
{
    if (Ptr != NULL) {
        ExFreePool(Ptr);
    }
}

void *OsDmaAlloc(ULONG Size, ULONG *Phys)
{
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS pa;
    void *p;

    Size = (Size + 4095) & ~4095UL;
    high.LowPart = 0xFFFFFFFF;
    high.HighPart = 0;
    p = MmAllocateContiguousMemory(Size, high);
    if (p == NULL) {
        UsbLog(LOG_ERR, "contiguous allocation of %u bytes failed\n", Size);
        return NULL;
    }
    RtlZeroMemory(p, Size);
    pa = MmGetPhysicalAddress(p);
    *Phys = pa.LowPart;
    return p;
}

void OsDmaFree(void *Ptr, ULONG Size)
{
    (void)Size;
    if (Ptr != NULL) {
        MmFreeContiguousMemory(Ptr);
    }
}

void OsStallUs(ULONG Us)
{
    while (Us > 50) {
        KeStallExecutionProcessor(50);
        Us -= 50;
    }
    if (Us != 0) {
        KeStallExecutionProcessor(Us);
    }
}

ULONG OsTimeMs(void)
{
    KIRQL old;
    ULONG now;
    ULONG delta;
    ULONG ms;

    /* NT clock ticks, the same time base as KeDelayExecutionThread and timers */
    KeAcquireSpinLock(&TimeLock, &old);
    now = TickCountLow();
    delta = now - LastTick;
    LastTick = now;
    while (delta != 0) {
        ULONG chunk = (delta > 1000) ? 1000 : delta;
        TimeFrac += chunk * TickIncrement;
        TimeMs += TimeFrac / 10000;
        TimeFrac %= 10000;
        delta -= chunk;
    }
    ms = TimeMs;
    KeReleaseSpinLock(&TimeLock, old);
    return ms;
}

void OsSleepMs(ULONG Ms)
{
    LARGE_INTEGER t;

    if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
        OsStallUs(Ms * 1000);
        return;
    }
    t = RelativeMs(Ms);
    KeDelayExecutionThread(KernelMode, FALSE, &t);
}

/* ------------------------------------------------------------------ */

typedef ULONG (NTAPI *HAL_BUS_DATA)(BUS_DATA_TYPE, ULONG, ULONG, PVOID, ULONG, ULONG);

static HAL_BUS_DATA PciGet;
static HAL_BUS_DATA PciSet;
static BOOLEAN PciLooked;
static KSPIN_LOCK PciLock;

/* HalGet/SetBusDataByOffset appeared in NT 3.5; NT 3.1 gets mechanism #1 */
static void PciLookup(void)
{
    if (!PciLooked) {
        KeInitializeSpinLock(&PciLock);
        PciGet = (HAL_BUS_DATA)NtHalExport("HalGetBusDataByOffset");
        PciSet = (HAL_BUS_DATA)NtHalExport("HalSetBusDataByOffset");
        if (PciGet == NULL || PciSet == NULL) {
            PciGet = PciSet = NULL;
            UsbLog(LOG_INFO, "PCI configuration through ports 0CF8/0CFC\n");
        }
        PciLooked = TRUE;
    }
}

static ULONG PciPortAccess(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, ULONG Val, int Size, BOOLEAN Write)
{
    ULONG addr = 0x80000000UL | ((ULONG)Bus << 16) | ((ULONG)(Dev & 31) << 11) | ((ULONG)(Fn & 7) << 8) | (Off & 0xFC);
    PUCHAR data = (PUCHAR)(ULONG)(0xCFC + (Off & 3));
    KIRQL old;
    ULONG v = 0xFFFFFFFF;

    KeAcquireSpinLock(&PciLock, &old);
    WRITE_PORT_ULONG((PULONG)(ULONG)0xCF8, addr);
    if (Write) {
        if (Size == 1) {
            WRITE_PORT_UCHAR(data, (UCHAR)Val);
        } else if (Size == 2) {
            WRITE_PORT_USHORT((PUSHORT)data, (USHORT)Val);
        } else {
            WRITE_PORT_ULONG((PULONG)data, Val);
        }
    } else {
        if (Size == 1) {
            v = READ_PORT_UCHAR(data);
        } else if (Size == 2) {
            v = READ_PORT_USHORT((PUSHORT)data);
        } else {
            v = READ_PORT_ULONG((PULONG)data);
        }
    }
    WRITE_PORT_ULONG((PULONG)(ULONG)0xCF8, 0);
    KeReleaseSpinLock(&PciLock, old);
    return v;
}

ULONG OsPciRead(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, int Size)
{
    PCI_SLOT_NUMBER slot;
    ULONG v = 0xFFFFFFFF;
    ULONG n;

    PciLookup();
    if (PciGet == NULL) {
        return PciPortAccess(Bus, Dev, Fn, Off, 0, Size, FALSE);
    }
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = Dev;
    slot.u.bits.FunctionNumber = Fn;
    n = PciGet(PCIConfiguration, Bus, slot.u.AsULONG, &v, Off, (ULONG)Size);
    if (n != (ULONG)Size) {
        return 0xFFFFFFFF;
    }
    if (Size == 1) {
        v &= 0xFF;
    } else if (Size == 2) {
        v &= 0xFFFF;
    }
    return v;
}

void OsPciWrite(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Off, ULONG Val, int Size)
{
    PCI_SLOT_NUMBER slot;

    PciLookup();
    if (PciSet == NULL) {
        PciPortAccess(Bus, Dev, Fn, Off, Val, Size, TRUE);
        return;
    }
    slot.u.AsULONG = 0;
    slot.u.bits.DeviceNumber = Dev;
    slot.u.bits.FunctionNumber = Fn;
    PciSet(PCIConfiguration, Bus, slot.u.AsULONG, &Val, Off, (ULONG)Size);
}

LONG NtAtomicAdd(LONG volatile *Value, LONG Delta)
{
#ifdef _MSC_VER
    LONG r;
    __asm {
        mov     ecx, Value
        mov     eax, Delta
        lock xadd [ecx], eax
        add     eax, Delta
        mov     r, eax
    }
    return r;
#else
    return __sync_add_and_fetch(Value, Delta);
#endif
}

PDRIVER_CANCEL NtSetCancelRoutine(PIRP Irp, PDRIVER_CANCEL Routine)
{
    return (PDRIVER_CANCEL)NtAtomicSwap((LONG volatile *)&Irp->CancelRoutine, (LONG)Routine);
}

LONG NtAtomicSwap(LONG volatile *Value, LONG New)
{
#ifdef _MSC_VER
    LONG r;
    __asm {
        mov     ecx, Value
        mov     eax, New
        xchg    [ecx], eax
        mov     r, eax
    }
    return r;
#else
    return __sync_lock_test_and_set(Value, New);
#endif
}

void *OsMapMmio(UCHAR Bus, ULONG Phys, ULONG Len)
{
    PHYSICAL_ADDRESS bus;
    PHYSICAL_ADDRESS tr;
    ULONG space = 0;

    bus.LowPart = Phys;
    bus.HighPart = 0;
    /* the NT 3.1 HAL has no PCI bus handler; on x86 the address is physical */
    if (!HalTranslateBusAddress(PCIBus, Bus, bus, &space, &tr)) {
        space = 0;
        if (!HalTranslateBusAddress(Isa, 0, bus, &space, &tr)) {
            return NULL;
        }
    }
    return MmMapIoSpace(tr, Len, MmNonCached);
}

UCHAR  OsIn8(ULONG Port)   { return READ_PORT_UCHAR((PUCHAR)Port); }
USHORT OsIn16(ULONG Port)  { return READ_PORT_USHORT((PUSHORT)Port); }
ULONG  OsIn32(ULONG Port)  { return READ_PORT_ULONG((PULONG)Port); }
void   OsOut8(ULONG Port, UCHAR Val)   { WRITE_PORT_UCHAR((PUCHAR)Port, Val); }
void   OsOut16(ULONG Port, USHORT Val) { WRITE_PORT_USHORT((PUSHORT)Port, Val); }
void   OsOut32(ULONG Port, ULONG Val)  { WRITE_PORT_ULONG((PULONG)Port, Val); }

/* ------------------------------------------------------------------ */

OS_IRQL OsLock(void)
{
    KIRQL old;

    KeAcquireSpinLock(&GlobalLock, &old);
    return old;
}

void OsUnlock(OS_IRQL Irql)
{
    KeReleaseSpinLock(&GlobalLock, Irql);
}

void OsEventInit(OS_EVENT *Ev)
{
    KeInitializeEvent(&Ev->Event, NotificationEvent, FALSE);
    Ev->Signaled = 0;
}

void OsEventSet(OS_EVENT *Ev)
{
    Ev->Signaled = 1;
    KeSetEvent(&Ev->Event, 0, FALSE);
}

void OsEventReset(OS_EVENT *Ev)
{
    Ev->Signaled = 0;
    KeClearEvent(&Ev->Event);
}

BOOLEAN OsEventWait(OS_EVENT *Ev, ULONG TimeoutMs)
{
    ULONG start = OsTimeMs();
    ULONG spun = 0;
    LARGE_INTEGER t;
    NTSTATUS st;

    /* short busy poll keeps mass storage latency low without interrupts */
    while (spun < FastPollUs) {
        if (Ev->Signaled) {
            return TRUE;
        }
        UsbPollAll();
        if (Ev->Signaled) {
            return TRUE;
        }
        KeStallExecutionProcessor(20);
        spun += 20;
    }
    for (;;) {
        ULONG elapsed = OsTimeMs() - start;
        ULONG slice;
        if (Ev->Signaled) {
            return TRUE;
        }
        if (elapsed >= TimeoutMs) {
            UsbPollAll();
            return Ev->Signaled ? TRUE : FALSE;
        }
        slice = TimeoutMs - elapsed;
        if (slice > 50) {
            slice = 50;
        }
        if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
            UsbPollAll();
            OsStallUs(100);
            continue;
        }
        t = RelativeMs(slice);
        st = KeWaitForSingleObject(&Ev->Event, Executive, KernelMode, FALSE, &t);
        if (st != STATUS_SUCCESS) {
            UsbPollAll();
        }
    }
}

static UCHAR *KernelBase;
static UCHAR *HalBase;

static int NameEqNoCase(const char *A, const char *B)
{
    while (*A != 0) {
        char a = *A++;
        char b = *B++;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) {
            return 0;
        }
    }
    return *B == 0;
}

static int IsImage(const UCHAR *p)
{
    ULONG e;

    if (p[0] != 'M' || p[1] != 'Z') {
        return 0;
    }
    e = *(const ULONG *)(p + 0x3C);
    return e < 0x1000 - 4 && *(const ULONG *)(p + e) == 0x00004550;
}

/* image base of the module our import from Dll resolved into */
static UCHAR *ImportedImage(UCHAR *Me, const char *Dll)
{
    UCHAR *pe = Me + *(ULONG *)(Me + 0x3C);
    UCHAR *imp;
    UCHAR *p;
    ULONG i;

    if (*(ULONG *)(pe + 24 + 104) == 0) {
        return NULL;
    }
    for (imp = Me + *(ULONG *)(pe + 24 + 104); *(ULONG *)(imp + 12) != 0; imp += 20) {
        if (NameEqNoCase((const char *)(Me + *(ULONG *)(imp + 12)), Dll)) {
            break;
        }
    }
    if (*(ULONG *)(imp + 12) == 0) {
        return NULL;
    }
    p = (UCHAR *)(*(ULONG *)(Me + *(ULONG *)(imp + 16)) & ~0xFFFUL);
    for (i = 0; i < 4096 && (ULONG)p >= 0x80000000UL; i++, p -= 0x1000) {
        if (!MmIsAddressValid(p)) {
            break;
        }
        if (IsImage(p)) {
            return p;
        }
    }
    return NULL;
}

/*
 * Finds the kernel and HAL images through our own import table. Must run in
 * DriverEntry, before the INIT section (which holds the import names) is
 * discarded.
 */
void NtKernelInit(PDRIVER_OBJECT Drv)
{
    UCHAR *me = (UCHAR *)Drv->DriverStart;
    ULONG i;

    /* DriverStart is not filled in for boot drivers on NT 4.0 */
    if (me == NULL) {
        me = (UCHAR *)((ULONG)NtKernelInit & ~0xFFFUL);
        for (i = 0; i < 256 && MmIsAddressValid(me) && !IsImage(me); i++) {
            me -= 0x1000;
        }
    }
    if (!MmIsAddressValid(me) || !IsImage(me)) {
        return;
    }
    KernelBase = ImportedImage(me, "ntoskrnl.exe");
    HalBase = ImportedImage(me, "hal.dll");
}

static PVOID ImageExport(UCHAR *base, const char *Name)
{
    UCHAR *pe;
    UCHAR *ed;
    ULONG *names;
    USHORT *ords;
    ULONG *funcs;
    ULONG count;
    ULONG i;

    if (base == NULL) {
        return NULL;
    }
    pe = base + *(ULONG *)(base + 0x3C);
    if (*(ULONG *)(pe + 24 + 96 + 4) == 0) {
        return NULL;
    }
    ed = base + *(ULONG *)(pe + 24 + 96);
    count = *(ULONG *)(ed + 24);
    funcs = (ULONG *)(base + *(ULONG *)(ed + 28));
    names = (ULONG *)(base + *(ULONG *)(ed + 32));
    ords = (USHORT *)(base + *(ULONG *)(ed + 36));
    for (i = 0; i < count; i++) {
        if (StrEq((const char *)(base + names[i]), Name)) {
            return base + funcs[ords[i]];
        }
    }
    return NULL;
}

/*
 * Address of a HAL or kernel export, NULL if this version does not have it.
 * Used for functions that later NT versions provide but older ones lack.
 */
PVOID NtHalExport(const char *Name)
{
    return ImageExport(HalBase, Name);
}

PVOID NtKernelExport(const char *Name)
{
    return ImageExport(KernelBase, Name);
}

void OsWakeWorker(void)
{
    KeSetEvent(&NtWorkerEvent, 0, FALSE);
}

void OsLogStr(const char *Str)
{
    if (DebugPort != 0) {
        KIRQL old;
        KeAcquireSpinLock(&LogLock, &old);
        while (*Str) {
            ULONG spin = 0;
            if (*Str == '\n') {
                while (!(READ_PORT_UCHAR((PUCHAR)(DebugPort + 5)) & 0x20) && spin++ < 100000) {
                }
                WRITE_PORT_UCHAR((PUCHAR)DebugPort, '\r');
                spin = 0;
            }
            while (!(READ_PORT_UCHAR((PUCHAR)(DebugPort + 5)) & 0x20) && spin++ < 100000) {
            }
            WRITE_PORT_UCHAR((PUCHAR)DebugPort, (UCHAR)*Str++);
        }
        KeReleaseSpinLock(&LogLock, old);
    } else {
        DbgPrint("%s", Str);
    }
}
