/*
 * pcireloc.c - move a 64-bit memory BAR that the firmware placed above 4 GB to a
 * free spot below 4 GB, where a 32-bit NT kernel (no PAE) can map it.
 *
 * Current UEFI firmware puts many chipset functions (HD Audio, xHCI, ...) at
 * 0x40_0000_0000-style addresses.  NT 3.x/4.0 can only map physical addresses
 * below 4 GB, so the BAR is reprogrammed into the 32-bit PCI memory hole:
 *
 *   - the hole starts at the top of low DRAM (Intel TOLUD, AMD TOP_MEM MSR; or
 *     the lowest 32-bit BAR seen when neither is known) and ends at 0xFC000000
 *     (above that live the I/O APIC, HPET, LAPIC, BIOS flash, PCH sideband, ...)
 *   - the MMCONFIG window is excluded
 *   - every 32-bit BAR and bridge window found on the buses is excluded; BAR sizes
 *     are not probed (other devices are live), the natural alignment of the base
 *     is an upper bound for the size
 *   - a candidate must read back all ones (nothing decodes it) before it is used
 *   - for a function behind bridges the spot must lie in every upstream bridge's
 *     non-prefetchable window; bridges without one (the firmware put everything
 *     behind them in a 64-bit prefetchable window above 4 GB) get a new 32-bit
 *     window, opened from the topmost such bridge down to the function
 *
 * PciRelocateAll() does this for every function in the system (largest BARs
 * first), shrinking BARs that do not fit through the Resizable BAR capability,
 * and closes the 64-bit prefetchable bridge windows above 4 GB that are left
 * empty: after it, nothing decodes above 4 GB any more.
 *
 * Shared between AUDIONT and USBNT: the includer defines
 *   RL_CFG32(b,d,f,o), RL_CFGW32(b,d,f,o,v), RL_MAP(pa,size), RL_UNMAP(va,size), RL_LOG(...)
 */

#define RL_HOLE_TOP   0xFC000000UL
#define RL_MAX_USED   384
#define RL_MAX_SIZE   0x40000000UL    /* 1 GB */

typedef struct { ULONG Lo, Hi; USHORT Win; } RL_RANGE;   /* [Lo, Hi); Win: bridge bus:dev.fn + 1, 0 for BARs */

static RL_RANGE g_RlUsed[RL_MAX_USED];
static ULONG g_RlNUsed;
static ULONG g_RlLowest;

static void RlAddW(ULONG lo, ULONG hi, USHORT win)
{
    if (hi <= lo) return;
    if (lo && lo < g_RlLowest) g_RlLowest = lo;
    if (g_RlNUsed < RL_MAX_USED) {
        g_RlUsed[g_RlNUsed].Lo = lo; g_RlUsed[g_RlNUsed].Hi = hi; g_RlUsed[g_RlNUsed].Win = win;
        g_RlNUsed++;
    }
}
#define RlAdd(lo, hi) RlAddW(lo, hi, 0)

/* the bridges the function sits behind: their windows may contain the new spot */
static USHORT g_RlChain[16];
static ULONG g_RlNChain;

static ULONG RlAlignSpan(ULONG base)
{
    ULONG span = base & (~base + 1);                 /* lowest set bit */
    if (!span || span > 0x10000000UL) span = 0x10000000UL;
    return span;
}

static BOOLEAN RlOverlaps(ULONG lo, ULONG hi)
{
    ULONG i, k;
    for (i = 0; i < g_RlNUsed; i++) {
        if (!(lo < g_RlUsed[i].Hi && g_RlUsed[i].Lo < hi)) continue;
        if (g_RlUsed[i].Win && g_RlUsed[i].Lo <= lo && hi <= g_RlUsed[i].Hi) {
            for (k = 0; k < g_RlNChain && g_RlChain[k] != g_RlUsed[i].Win; k++) ;
            if (k < g_RlNChain) continue;                  /* inside an upstream bridge's window */
        }
        return TRUE;
    }
    return FALSE;
}

static int RlCpuIsAmd(void)
{
#if defined(__GNUC__) && defined(__x86_64__)
    ULONG a, b, c, d;
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    return (b == 0x68747541 && d == 0x69746E65) || (b == 0x6F677948 && d == 0x6E65476E);
#elif defined(__GNUC__) && defined(__i386__)
    ULONG a, b, c, d, f1, f2;
    __asm__ __volatile__("pushfl; popl %0; movl %0, %1; xorl $0x200000, %0; pushl %0; popfl; pushfl; popl %0; pushl %1; popfl"
                         : "=&r"(f1), "=&r"(f2));
    if (!((f1 ^ f2) & 0x200000)) return 0;          /* no CPUID */
    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    return (b == 0x68747541 && d == 0x69746E65) ||   /* AuthenticAMD */
           (b == 0x6F677948 && d == 0x6E65476E);     /* HygonGenuine */
#else
    return 0;
#endif
}

static ULONGLONG RlRdMsr(ULONG msr)
{
#if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    ULONG lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((ULONGLONG)hi << 32) | lo;
#else
    (void)msr;
    return 0;
#endif
}

/* exact size of a 32-bit memory BAR (memory decoding off around it) */
static ULONG RlSize32(ULONG bus, ULONG dev, ULONG fn, ULONG off)
{
    ULONG v = RL_CFG32(bus, dev, fn, off), cmd = RL_CFG32(bus, dev, fn, 0x04), m;
    RL_CFGW32(bus, dev, fn, 0x04, cmd & ~0x0002UL);
    RL_CFGW32(bus, dev, fn, off, 0xFFFFFFFFUL);
    m = RL_CFG32(bus, dev, fn, off) & ~0xFUL;
    RL_CFGW32(bus, dev, fn, off, v);
    RL_CFGW32(bus, dev, fn, 0x04, cmd);
    return m ? ~m + 1 : 0;
}

/* collect the 32-bit memory ranges in use; the BARs of the function being moved
   (its decoding is ours to switch) are sized exactly, the others by their alignment */
static ULONG g_RlTb = 0xFFFF, g_RlTd, g_RlTf;
static void RlScan(void)
{
    ULONG bus, dev, fn;
    g_RlNUsed = 0;
    g_RlLowest = 0xFFFFFFFFUL;
    for (bus = 0; bus < 256; bus++) {
        for (dev = 0; dev < 32; dev++) {
            for (fn = 0; fn < 8; fn++) {
                ULONG id = RL_CFG32(bus, dev, fn, 0), hdr, nbar, i;
                if (id == 0xFFFFFFFFUL || id == 0) { if (fn == 0) break; continue; }
                hdr = (RL_CFG32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
                nbar = hdr == 0 ? 6 : hdr == 1 ? 2 : 0;
                for (i = 0; i < nbar; i++) {
                    ULONG v = RL_CFG32(bus, dev, fn, 0x10 + i * 4), hiw = 0, base;
                    if (v & 1) continue;                                /* I/O */
                    if ((v & 6) == 4) { hiw = RL_CFG32(bus, dev, fn, 0x14 + i * 4); i++; }
                    base = v & ~0xFUL;
                    if (hiw || !base) continue;
                    if (bus == g_RlTb && dev == g_RlTd && fn == g_RlTf) {
                        ULONG sz = RlSize32(bus, dev, fn, 0x10 + (i - ((v & 6) == 4)) * 4);
                        RlAdd(base, base + (sz ? sz : RlAlignSpan(base)));
                    } else RlAdd(base, base + RlAlignSpan(base));
                }
                if (hdr == 1) {
                    ULONG m = RL_CFG32(bus, dev, fn, 0x20), p = RL_CFG32(bus, dev, fn, 0x24);
                    ULONG mb = (m & 0xFFF0) << 16, ml = (m & 0xFFF00000UL) | 0xFFFFF;
                    ULONG pb = (p & 0xFFF0) << 16, pl = (p & 0xFFF00000UL) | 0xFFFFF;
                    USHORT w = (USHORT)(((bus << 8) | (dev << 3) | fn) + 1);
                    if (ml > mb) RlAddW(mb, ml + 1 ? ml + 1 : 0xFFFFFFFFUL, w);
                    if ((p & 0xF) == 1 && (RL_CFG32(bus, dev, fn, 0x28) || RL_CFG32(bus, dev, fn, 0x2C))) pl = 0;
                    if (pl > pb) RlAddW(pb, pl + 1 ? pl + 1 : 0xFFFFFFFFUL, w);
                }
                if (fn == 0 && !((RL_CFG32(bus, dev, 0, 0x0C) >> 16) & 0x80)) break;
            }
        }
    }
}

/* bottom of the PCI hole and the MMCONFIG window */
static ULONG RlHoleBottom(ULONG *mcLo, ULONG *mcHi)
{
    ULONG hb = RL_CFG32(0, 0, 0, 0), bottom = 0;
    *mcLo = *mcHi = 0;
    if ((hb & 0xFFFF) == 0x8086 && (RL_CFG32(0, 0, 0, 0x08) >> 8) == 0x060000) {
        ULONG tolud = RL_CFG32(0, 0, 0, 0xBC);                   /* Sandy Bridge and later */
        ULONG old = RL_CFG32(0, 0, 0, 0xB0) & 0xFFFF;            /* G33/Q35/G4x: TOLUD[31:20] in 15:4 */
        ULONG pex = RL_CFG32(0, 0, 0, 0x60), pexh = RL_CFG32(0, 0, 0, 0x64);
        tolud &= 0xFFF00000UL;
        old = (old & 0xFFF0) << 16;
        if ((hb >> 16) >= 0x0100 && (hb >> 16) < 0x2000 && tolud >= 0x04000000UL && tolud < RL_HOLE_TOP) bottom = tolud;
        else if ((hb >> 16) >= 0x2000 && old >= 0x04000000UL && old < RL_HOLE_TOP) bottom = old;
        else if (tolud >= 0x04000000UL && tolud < RL_HOLE_TOP) bottom = tolud;
        if ((pex & 1) && !pexh) {
            ULONG len = 0x10000000UL >> ((pex >> 1) & 3);
            if (((pex >> 1) & 3) == 3) len = 0x20000000UL;
            *mcLo = pex & 0xF0000000UL & ~(len - 1);
            if (((pex >> 1) & 3) == 1) *mcLo = pex & 0xF8000000UL;
            if (((pex >> 1) & 3) == 2) *mcLo = pex & 0xFC000000UL;
            *mcHi = *mcLo + len;
        }
    } else if (RlCpuIsAmd()) {
        ULONGLONG tom = RlRdMsr(0xC001001AUL), mc = RlRdMsr(0xC0010058UL);
        if (tom && tom < RL_HOLE_TOP) bottom = (ULONG)tom;
        if ((mc & 1) && !(mc >> 32)) {
            ULONG buses = 1UL << ((ULONG)(mc >> 2) & 0xF);
            *mcLo = (ULONG)mc & 0xFFF00000UL;
            *mcHi = *mcLo + buses * 0x100000UL;
        }
    }
#ifdef RL_CMOS
    if (!bottom) {
        /* memory size the BIOS keeps in CMOS: 64 KB units above 16 MB (0x34/0x35) */
        ULONG k = RL_CMOS(0x34) | ((ULONG)RL_CMOS(0x35) << 8);
        if (k && k != 0xFFFF) bottom = 0x01000000UL + k * 0x10000UL;
        if (bottom < 0x04000000UL || bottom >= RL_HOLE_TOP) bottom = 0;
        else bottom = (bottom + 0xFFFFFUL) & ~0xFFFFFUL;
    }
#endif
    /* unknown host: stay inside the area the firmware already uses for 32-bit BARs */
    if (!bottom || bottom < 0x04000000UL) bottom = g_RlLowest != 0xFFFFFFFFUL ? g_RlLowest : 0xE0000000UL;
    return bottom;
}

static BOOLEAN RlUnclaimed(ULONG pa, ULONG size)
{
    volatile ULONG *va;
    ULONG off, step = size > 0x10000 ? size / 16 : 0x1000;
    BOOLEAN ok = TRUE;
    if (step < 0x1000) step = 0x1000;
    va = (volatile ULONG *)RL_MAP(pa, size);
    if (!va) return FALSE;
    /* nothing decodes the range: a master abort reads all ones on real chipsets
       (emulators may return zeros); anything else is a device or memory */
    {
        ULONG first = va[0];
        if (first != 0xFFFFFFFFUL && first != 0) ok = FALSE;
        for (off = 0; off < size && ok; off += step)
            if (va[off / 4] != first || va[off / 4 + 1] != first) ok = FALSE;
        if (ok && va[(size - 4) / 4] != first) ok = FALSE;
    }
    RL_UNMAP((PVOID)va, size);
    return ok;
}

/* the bridges above a bus, nearest first; -1 when the bus cannot be traced to bus 0 */
#define RL_MAX_DEPTH 16
typedef struct { UCHAR B, D, F; } RL_BDF;

static int RlBridges(ULONG bus, RL_BDF *out)
{
    int n = 0;
    while (bus && n < RL_MAX_DEPTH) {
        ULONG b, d, f, found = 0;
        for (b = 0; b < 256 && !found; b++)
            for (d = 0; d < 32 && !found; d++)
                for (f = 0; f < 8 && !found; f++) {
                    ULONG id = RL_CFG32(b, d, f, 0), hdr, bn;
                    if (id == 0xFFFFFFFFUL || id == 0) { if (f == 0) break; continue; }
                    hdr = (RL_CFG32(b, d, f, 0x0C) >> 16) & 0x7F;
                    if (hdr != 1) continue;
                    bn = RL_CFG32(b, d, f, 0x18);
                    if (((bn >> 8) & 0xFF) != bus) continue;
                    out[n].B = (UCHAR)b; out[n].D = (UCHAR)d; out[n].F = (UCHAR)f;
                    n++;
                    bus = b;
                    found = 1;
                }
        if (!found) return -1;
    }
    return bus ? -1 : n;
}

/* non-prefetchable window [lo, hi) of a bridge; FALSE when it has none */
static BOOLEAN RlWindow(const RL_BDF *br, ULONG *lo, ULONG *hi)
{
    ULONG m = RL_CFG32(br->B, br->D, br->F, 0x20);
    ULONG mb = (m & 0xFFF0) << 16, ml = (m & 0xFFF00000UL) | 0xFFFFF;
    if (ml <= mb) return FALSE;
    *lo = mb;
    *hi = ml == 0xFFFFFFFFUL ? 0xFFFFFFFFUL : ml + 1;
    return TRUE;
}

static void RlSetWindow(const RL_BDF *br, ULONG lo, ULONG hi)
{
    ULONG cmd = RL_CFG32(br->B, br->D, br->F, 0x04);
    RL_CFGW32(br->B, br->D, br->F, 0x20, ((lo >> 16) & 0xFFF0) | ((hi - 1) & 0xFFF00000UL));
    RL_CFGW32(br->B, br->D, br->F, 0x04, (cmd & 0xFFFF) | 0x0006);      /* memory + bus master */
}

/* PCI Express extended configuration space through the MMCONFIG window */
static ULONG g_RlMcLo, g_RlMcHi;
static BOOLEAN g_RlMcDone;

static volatile ULONG *RlExtMap(ULONG bus, ULONG dev, ULONG fn)
{
    ULONG pa;
    if (!g_RlMcDone) { g_RlMcDone = TRUE; RlHoleBottom(&g_RlMcLo, &g_RlMcHi); }
    if (!g_RlMcLo) return NULL;
    pa = g_RlMcLo + (bus << 20) + (dev << 15) + (fn << 12);
    if (pa + 0x1000 > g_RlMcHi) return NULL;
    return (volatile ULONG *)RL_MAP(pa, 0x1000);
}

/* Resizable BAR (extended capability 0x15): the smallest size the BAR supports */
static BOOLEAN RlShrink(ULONG bus, ULONG dev, ULONG fn, ULONG off)
{
    volatile ULONG *x = RlExtMap(bus, dev, fn);
    ULONG cap = 0x100, n, i, bar = (off - 0x10) / 4, guard;
    BOOLEAN done = FALSE;
    if (!x) return FALSE;
    if (x[0] != RL_CFG32(bus, dev, fn, 0)) {            /* not the MMCONFIG window of this function */
        RL_UNMAP((PVOID)x, 0x1000);
        return FALSE;
    }
    for (guard = 0; cap && cap < 0x1000 && guard < 64; guard++) {
        ULONG h = x[cap / 4];
        if (h == 0 || h == 0xFFFFFFFFUL) break;
        if ((h & 0xFFFF) == 0x15) {
            n = (x[(cap + 8) / 4] >> 5) & 7;
            for (i = 0; i < n && i < 6; i++) {
                ULONG sup = x[(cap + 4 + 8 * i) / 4], ctl = x[(cap + 8 + 8 * i) / 4], k, cmd;
                if ((ctl & 7) != bar) continue;
                for (k = 0; k < 28; k++) if (sup & (1UL << (4 + k))) break;     /* smallest: 2^(k+20) */
                if (k >= 28 || (1UL << (k + 20)) > RL_MAX_SIZE / 2 || ((ctl >> 8) & 0x3F) == k) break;
                cmd = RL_CFG32(bus, dev, fn, 0x04);
                RL_CFGW32(bus, dev, fn, 0x04, cmd & ~0x0002UL);
                x[(cap + 8 + 8 * i) / 4] = (ctl & ~(0x3FUL << 8)) | (k << 8);
                RL_CFGW32(bus, dev, fn, 0x04, cmd);
                RL_LOG("pcireloc: %u:%u.%u BAR%u resized from %u MB to %u MB", bus, dev, fn, bar,
                       1UL << ((ctl >> 8) & 0x3F), 1UL << k);
                done = TRUE;
                break;
            }
            break;
        }
        cap = h >> 20;
    }
    RL_UNMAP((PVOID)x, 0x1000);
    return done;
}

static ULONG PciRelocateBar(ULONG bus, ULONG dev, ULONG fn, ULONG off);

/* Returns the new 32-bit BAR value, or 0 when the BAR could not be moved.  'off' is
   the configuration offset of the low dword of a 64-bit memory BAR. */
static ULONG PciRelocateBar(ULONG bus, ULONG dev, ULONG fn, ULONG off)
{
    ULONG lo = RL_CFG32(bus, dev, fn, off), hiw = RL_CFG32(bus, dev, fn, off + 4);
    ULONG cmd = RL_CFG32(bus, dev, fn, 0x04), size, mask, bottom, top, mcLo, mcHi, cand, span, wlo, whi;
    RL_BDF br[RL_MAX_DEPTH];
    int nbr, open, i;
    if ((lo & 7) != 4 || !hiw) return 0;
    /* size the BAR with memory decoding off */
    RL_CFGW32(bus, dev, fn, 0x04, cmd & ~0x0002UL);
    RL_CFGW32(bus, dev, fn, off, 0xFFFFFFFFUL);
    mask = RL_CFG32(bus, dev, fn, off) & ~0xFUL;
    RL_CFGW32(bus, dev, fn, off, lo);
    RL_CFGW32(bus, dev, fn, 0x04, cmd);
    if (!mask || ~mask + 1 > RL_MAX_SIZE) {
        /* larger than the hole can take: make it smaller first (Resizable BAR) */
        if (!RlShrink(bus, dev, fn, off)) {
            RL_LOG("pcireloc: %u:%u.%u BAR %x too large to move (%x)", bus, dev, fn, off, mask);
            return 0;
        }
        return PciRelocateBar(bus, dev, fn, off);
    }
    size = ~mask + 1;
    if (size < 0x1000) size = 0x1000;

    g_RlTb = bus; g_RlTd = dev; g_RlTf = fn;
    RlScan();
    g_RlTb = 0xFFFF;
    bottom = RlHoleBottom(&mcLo, &mcHi);
    top = RL_HOLE_TOP;
    nbr = RlBridges(bus, br);
    if (nbr < 0) {
        RL_LOG("pcireloc: %u:%u.%u: bus %u not reachable from bus 0", bus, dev, fn, bus);
        return 0;
    }
    for (g_RlNChain = 0; g_RlNChain < (ULONG)nbr && g_RlNChain < 16; g_RlNChain++)
        g_RlChain[g_RlNChain] = (USHORT)(((br[g_RlNChain].B << 8) | (br[g_RlNChain].D << 3) | br[g_RlNChain].F) + 1);
    /* walk from the root port down: the windows narrow the range; the first bridge
       without a 32-bit window and every bridge below it get one (they nest) */
    open = -1;
    {
        ULONG wb = 0, wt = 0xFFFFFFFFUL;
        BOOLEAN win = FALSE;
        for (i = nbr - 1; i >= 0; i--) {
            if (!RlWindow(&br[i], &wlo, &whi)) { open = i; break; }
            win = TRUE;
            if (wlo > wb) wb = wlo;
            if (whi < wt) wt = whi;
        }
        /* a window the firmware gave a bridge is decoded space for sure, even above
           the conservative hole top (firmware likes 0xFE000000 for bridges) */
        if (win) { bottom = wb; top = wt; }
    }
    span = size;
    if (open >= 0 && span < 0x100000UL) span = 0x100000UL;      /* bridge window granularity */
    if (mcHi > mcLo) RlAdd(mcLo, mcHi);
    RL_LOG("pcireloc: %u:%u.%u BAR %x at %x%08x (%x bytes), hole %08x-%08x, mmcfg %08x-%08x, %u ranges in use, "
           "%d bridge(s), %d to open", bus, dev, fn, off, hiw, lo & ~0xFUL, size, bottom, top, mcLo, mcHi,
           g_RlNUsed, nbr, open + 1);
    if (top <= bottom || top - bottom < span) {
        RL_LOG("pcireloc: %u:%u.%u: no room in the upstream window %08x-%08x", bus, dev, fn, bottom, top);
        return 0;
    }
    for (cand = (top - span) & ~(span - 1); cand >= bottom && cand + span <= top; cand -= span) {
        if (RlOverlaps(cand, cand + span)) continue;
        if (!RlUnclaimed(cand, span)) continue;
        /* open the missing bridge windows, the topmost first */
        for (i = open; i >= 0; i--) {
            RlSetWindow(&br[i], cand, cand + span);
            RL_LOG("pcireloc: bridge %u:%u.%u: 32-bit window %08x-%08x opened", br[i].B, br[i].D, br[i].F,
                   cand, cand + span - 1);
        }
        RL_CFGW32(bus, dev, fn, 0x04, cmd & ~0x0002UL);
        RL_CFGW32(bus, dev, fn, off, cand | (lo & 0xF));
        RL_CFGW32(bus, dev, fn, off + 4, 0);
        RL_CFGW32(bus, dev, fn, 0x04, cmd | 0x0002UL);
        if ((RL_CFG32(bus, dev, fn, off) & ~0xFUL) != cand || RL_CFG32(bus, dev, fn, off + 4)) {
            RL_LOG("pcireloc: BAR did not take %08x", cand);
            RL_CFGW32(bus, dev, fn, 0x04, cmd & ~0x0002UL);
            RL_CFGW32(bus, dev, fn, off, lo);
            RL_CFGW32(bus, dev, fn, off + 4, hiw);
            RL_CFGW32(bus, dev, fn, 0x04, cmd);
            return 0;
        }
        RL_LOG("pcireloc: %u:%u.%u BAR moved to %08x", bus, dev, fn, cand);
        return cand | (lo & 0xF);
    }
    if (size > 0x100000UL && RlShrink(bus, dev, fn, off)) return PciRelocateBar(bus, dev, fn, off);
    RL_LOG("pcireloc: no free %x byte spot below 4 GB for %u:%u.%u", span, bus, dev, fn);
    return 0;
}

/* ------------------------------------------------------------------ */
/* everything                                                          */
/* ------------------------------------------------------------------ */
typedef struct { UCHAR B, D, F, Off; ULONG Size; } RL_CAND;

/* every 64-bit memory BAR above 4 GB of every function, largest first; then the
   64-bit prefetchable bridge windows above 4 GB with nothing left in them are
   closed.  Returns the number of BARs moved. */
static ULONG PciRelocateAll(void)
{
    static RL_CAND c[96];
    ULONG n = 0, bus, dev, fn, i, j, moved = 0, failed = 0;
    for (bus = 0; bus < 256; bus++)
        for (dev = 0; dev < 32; dev++)
            for (fn = 0; fn < 8; fn++) {
                ULONG id = RL_CFG32(bus, dev, fn, 0), hdr, nbar;
                if (id == 0xFFFFFFFFUL || id == 0) { if (fn == 0) break; continue; }
                hdr = (RL_CFG32(bus, dev, fn, 0x0C) >> 16) & 0x7F;
                nbar = hdr == 0 ? 6 : hdr == 1 ? 2 : 0;
                for (i = 0; i + 1 < nbar; i++) {
                    ULONG v = RL_CFG32(bus, dev, fn, 0x10 + i * 4), cmd, mask;
                    if ((v & 7) != 4) { continue; }
                    if (RL_CFG32(bus, dev, fn, 0x14 + i * 4) && n < 96) {
                        cmd = RL_CFG32(bus, dev, fn, 0x04);
                        RL_CFGW32(bus, dev, fn, 0x04, cmd & ~0x0002UL);
                        RL_CFGW32(bus, dev, fn, 0x10 + i * 4, 0xFFFFFFFFUL);
                        mask = RL_CFG32(bus, dev, fn, 0x10 + i * 4) & ~0xFUL;
                        RL_CFGW32(bus, dev, fn, 0x10 + i * 4, v);
                        RL_CFGW32(bus, dev, fn, 0x04, cmd);
                        c[n].B = (UCHAR)bus; c[n].D = (UCHAR)dev; c[n].F = (UCHAR)fn; c[n].Off = (UCHAR)(0x10 + i * 4);
                        c[n].Size = mask ? ~mask + 1 : 0xFFFFFFFFUL;
                        n++;
                    }
                    i++;
                }
                if (fn == 0 && !((RL_CFG32(bus, dev, 0, 0x0C) >> 16) & 0x80)) break;
            }
    /* largest first: the alignment of big BARs is the hard part */
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (c[j].Size > c[i].Size) { RL_CAND t = c[i]; c[i] = c[j]; c[j] = t; }
    for (i = 0; i < n; i++) {
        if (PciRelocateBar(c[i].B, c[i].D, c[i].F, c[i].Off)) moved++;
        else failed++;
    }
    /* bridges: 64-bit prefetchable windows above 4 GB that nothing decodes any more */
    for (bus = 0; bus < 256; bus++)
        for (dev = 0; dev < 32; dev++)
            for (fn = 0; fn < 8; fn++) {
                ULONG id = RL_CFG32(bus, dev, fn, 0), sec, sub, b2, d2, f2, p;
                BOOLEAN busy = FALSE;
                if (id == 0xFFFFFFFFUL || id == 0) { if (fn == 0) break; continue; }
                if (((RL_CFG32(bus, dev, fn, 0x0C) >> 16) & 0x7F) == 1) {
                    p = RL_CFG32(bus, dev, fn, 0x24);
                    if ((p & 0xF) == 1 && (RL_CFG32(bus, dev, fn, 0x28) || RL_CFG32(bus, dev, fn, 0x2C)) &&
                        (p & 0xFFF00000UL) >= ((p & 0xFFF0) << 16)) {
                        sec = (RL_CFG32(bus, dev, fn, 0x18) >> 8) & 0xFF;
                        sub = (RL_CFG32(bus, dev, fn, 0x18) >> 16) & 0xFF;
                        for (b2 = sec; b2 <= sub && b2 && !busy; b2++)
                            for (d2 = 0; d2 < 32 && !busy; d2++)
                                for (f2 = 0; f2 < 8 && !busy; f2++) {
                                    ULONG id2 = RL_CFG32(b2, d2, f2, 0), h2, k;
                                    if (id2 == 0xFFFFFFFFUL || id2 == 0) { if (f2 == 0) break; continue; }
                                    h2 = (RL_CFG32(b2, d2, f2, 0x0C) >> 16) & 0x7F;
                                    for (k = 0; k + 1 < (h2 == 0 ? 6UL : h2 == 1 ? 2UL : 0UL); k++) {
                                        ULONG v = RL_CFG32(b2, d2, f2, 0x10 + k * 4);
                                        if ((v & 7) == 4) {
                                            if (RL_CFG32(b2, d2, f2, 0x14 + k * 4)) busy = TRUE;
                                            k++;
                                        }
                                    }
                                    if (f2 == 0 && !((RL_CFG32(b2, d2, 0, 0x0C) >> 16) & 0x80)) break;
                                }
                        if (!busy) {
                            RL_CFGW32(bus, dev, fn, 0x24, 0x0000FFF1UL);        /* base > limit: closed */
                            RL_CFGW32(bus, dev, fn, 0x28, 0);
                            RL_CFGW32(bus, dev, fn, 0x2C, 0);
                            RL_LOG("pcireloc: bridge %u:%u.%u: empty prefetchable window above 4 GB closed", bus, dev, fn);
                        }
                    }
                }
                if (fn == 0 && !((RL_CFG32(bus, dev, 0, 0x0C) >> 16) & 0x80)) break;
            }
    if (n) RL_LOG("pcireloc: %u BAR(s) above 4 GB, %u moved below 4 GB, %u could not be moved", n, moved, failed);
    return moved;
}
