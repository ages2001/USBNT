/*
 * aml.c - evaluation-only AML interpreter (see aml.h).
 *
 * Two parts: a namespace built once from the DSDT / SSDTs (Scope, Device,
 * Method, Name, OperationRegion, Field / IndexField / BankField, Mutex,
 * Event, Alias), and a recursive evaluator that runs a single named method or
 * reads a data object. It understands the opcodes the HID over I2C methods of
 * notebook firmware use: Store, the arithmetic / logic / comparison ops,
 * If / Else / While (with an iteration limit), Return, Package, Buffer, the
 * CreateXxxField ops, ToInteger, SizeOf, Index, DerefOf, Local / Arg, method
 * calls (with a depth limit), Mutex Acquire / Release and Sleep / Stall as
 * no-ops / delays, Notify ignored, and OperationRegion + Field reads / writes
 * for SystemMemory, SystemIO, PCI_Config and GeneralPurposeIO (through
 * gpio.c). _OSI answers like Windows XP, _REV is 2, \_OS is
 * "Microsoft Windows NT". Anything else makes the evaluation fail so the
 * caller falls back to the old behaviour. Writes happen only to the regions
 * the evaluated method actually touches.
 */

#include "usbinput.h"
#include "acpi.h"
#include "aml.h"
#include "gpio.h"

#define AML_MAX_NODE   3072
#define AML_PATH       64
#define AML_MAX_DYN    96
#define AML_MAX_BF     48
#define AML_DEPTH      12
#define AML_WHILE_MAX  200000UL
#define AML_BUF_MAX    4096

/* node types */
#define NT_SCOPE   0
#define NT_NAME    1
#define NT_METHOD  2
#define NT_REGION  3
#define NT_FIELD   4
#define NT_MUTEX   5
#define NT_EVENT   6
#define NT_ALIAS   7

typedef struct {
    char        Path[AML_PATH];
    UCHAR       Type;
    const UCHAR *T;
    ULONG       A, B;            /* NAME: A=data offset. METHOD: A=body, B=end. */
    UCHAR       ArgCount;        /* METHOD */
    UCHAR       Space;           /* REGION */
    ULONG       RgOffOff, RgLenOff;
    ULONGLONG   RgBase, RgLen;
    UCHAR       RgResolved;
    int         Region;          /* FIELD -> region node */
    int         IndexN, DataN;   /* IndexField */
    int         BankN;           /* BankField */
    ULONGLONG   BankVal;
    ULONG       BitOff, BitLen;  /* FIELD */
    UCHAR       Access;          /* 0 any,1 byte,2 word,3 dword,4 qword */
    ULONG       ConnOff, ConnEnd;/* FIELD GeneralPurposeIO connection (0 none) */
    int         Target;          /* ALIAS */
} NODE;

static NODE  g_Node[AML_MAX_NODE];
static int   g_NNode;
static BOOLEAN g_Done;
static BOOLEAN g_Off;

/* dynamic (method-local) names and buffer fields */
typedef struct { char Path[AML_PATH]; AML_VAL Val; UCHAR Used; } DYN;
typedef struct { char Path[AML_PATH]; int Owner; ULONG BitOff, BitLen; UCHAR Used; } BUFFLD;

typedef struct {
    AML_VAL   Local[8];
    AML_VAL   Arg[7];
    AML_VAL   Ret;
    BOOLEAN   Returned;
    BOOLEAN   Broke;
    int       Depth;
    char      Scope[AML_PATH];
    DYN       Dyn[AML_MAX_DYN];
    BUFFLD    Bf[AML_MAX_BF];
} CTX;

#define G16(p)  ((ULONG)(p)[0] | ((ULONG)(p)[1] << 8))
#define G32(p)  (G16(p) | ((ULONG)G16((p) + 2) << 16))

static ULONGLONG G64(const UCHAR *p)
{
    return (ULONGLONG)G32(p) | ((ULONGLONG)G32(p + 4) << 32);
}

/* 64-bit unsigned divide (the driver links no libgcc, so no __udivmoddi4) */
static ULONGLONG Div64(ULONGLONG a, ULONGLONG b, ULONGLONG *rem)
{
    ULONGLONG q = 0, r = 0;
    int i;
    if (b == 0) {
        if (rem) *rem = 0;
        return 0;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) {
            r -= b;
            q |= (ULONGLONG)1 << i;
        }
    }
    if (rem) {
        *rem = r;
    }
    return q;
}

/* ------------------------------------------------------------------ */
/* value helpers                                                        */
/* ------------------------------------------------------------------ */
void AmlFree(AML_VAL *V)
{
    ULONG i;
    if (V == NULL) {
        return;
    }
    if (V->Kind == AMLV_BUF || V->Kind == AMLV_STR) {
        if (V->Buf) {
            OsFree(V->Buf);
        }
    } else if (V->Kind == AMLV_PKG && V->Pkg) {
        for (i = 0; i < V->Len; i++) {
            AmlFree(&V->Pkg[i]);
        }
        OsFree(V->Pkg);
    }
    UsbMemSet(V, 0, sizeof(*V));
}

void AmlSetInt(AML_VAL *V, ULONGLONG N)
{
    UsbMemSet(V, 0, sizeof(*V));
    V->Kind = AMLV_INT;
    V->Int = N;
}

static BOOLEAN NewBuf(AML_VAL *V, ULONG Len)
{
    UsbMemSet(V, 0, sizeof(*V));
    if (Len > AML_BUF_MAX) {
        return FALSE;
    }
    V->Buf = (UCHAR *)OsAlloc(Len ? Len + 1 : 1);
    if (V->Buf == NULL) {
        return FALSE;
    }
    V->Kind = AMLV_BUF;
    V->Len = Len;
    return TRUE;
}

static BOOLEAN CopyVal(AML_VAL *Dst, const AML_VAL *Src)
{
    ULONG i;
    UsbMemSet(Dst, 0, sizeof(*Dst));
    Dst->Kind = Src->Kind;
    Dst->Int = Src->Int;
    Dst->Node = Src->Node;
    Dst->Index = Src->Index;
    if (Src->Kind == AMLV_BUF || Src->Kind == AMLV_STR) {
        ULONG n = Src->Len + (Src->Kind == AMLV_STR ? 1 : 0);
        Dst->Buf = (UCHAR *)OsAlloc(n ? n : 1);
        if (Dst->Buf == NULL) {
            return FALSE;
        }
        UsbMemCpy(Dst->Buf, Src->Buf, Src->Len + (Src->Kind == AMLV_STR ? 1 : 0));
        Dst->Len = Src->Len;
    } else if (Src->Kind == AMLV_PKG) {
        Dst->Pkg = (AML_VAL *)OsAlloc(sizeof(AML_VAL) * (Src->Len ? Src->Len : 1));
        if (Dst->Pkg == NULL) {
            return FALSE;
        }
        Dst->Len = Src->Len;
        for (i = 0; i < Src->Len; i++) {
            if (!CopyVal(&Dst->Pkg[i], &Src->Pkg[i])) {
                return FALSE;
            }
        }
    }
    return TRUE;
}

BOOLEAN AmlAsInt(const AML_VAL *V, ULONGLONG *N)
{
    if (V == NULL) {
        return FALSE;
    }
    if (V->Kind == AMLV_INT) {
        *N = V->Int;
        return TRUE;
    }
    if (V->Kind == AMLV_BUF && V->Buf) {
        ULONGLONG v = 0;
        ULONG i, n = V->Len < 8 ? V->Len : 8;
        for (i = 0; i < n; i++) {
            v |= (ULONGLONG)V->Buf[i] << (8 * i);
        }
        *N = v;
        return TRUE;
    }
    return FALSE;
}

const AML_VAL *AmlPkgElem(const AML_VAL *V, ULONG I)
{
    if (V == NULL || V->Kind != AMLV_PKG || I >= V->Len) {
        return NULL;
    }
    return &V->Pkg[I];
}

/* ------------------------------------------------------------------ */
/* AML byte helpers (name strings, package lengths)                     */
/* ------------------------------------------------------------------ */
static BOOLEAN NameChar(UCHAR c, BOOLEAN first)
{
    if ((c >= 'A' && c <= 'Z') || c == '_') {
        return TRUE;
    }
    return (BOOLEAN)(!first && c >= '0' && c <= '9');
}

static BOOLEAN SegOk(const UCHAR *p)
{
    return (BOOLEAN)(NameChar(p[0], TRUE) && NameChar(p[1], FALSE) && NameChar(p[2], FALSE) && NameChar(p[3], FALSE));
}

static ULONG PkgLen(const UCHAR *p, ULONG avail, ULONG *hdr)
{
    ULONG n = p[0] >> 6, len, i;
    if (avail < n + 1) {
        return 0;
    }
    if (n == 0) {
        *hdr = 1;
        return p[0] & 0x3F;
    }
    len = p[0] & 0x0F;
    for (i = 0; i < n; i++) {
        len |= (ULONG)p[1 + i] << (4 + 8 * i);
    }
    *hdr = n + 1;
    return len;
}

/* decode a NameString at p; prefix info and segment bytes; returns length (0 bad) */
static ULONG NameStr(const UCHAR *p, ULONG avail, int *up, int *root, const UCHAR **segs, int *nseg)
{
    ULONG o = 0;
    int r = 0, u = 0, ns = 1;
    if (avail == 0) {
        return 0;
    }
    if (p[0] == '\\') {
        r = 1;
        o = 1;
    } else {
        while (o < avail && p[o] == '^') {
            u++;
            o++;
        }
    }
    if (o >= avail) {
        return 0;
    }
    if (p[o] == 0x2E) {              /* DualNamePrefix */
        ns = 2;
        o++;
    } else if (p[o] == 0x2F) {       /* MultiNamePrefix */
        if (o + 1 >= avail) {
            return 0;
        }
        ns = p[o + 1];
        o += 2;
    } else if (p[o] == 0x00) {       /* NullName */
        *up = u;
        *root = r;
        *segs = NULL;
        *nseg = 0;
        return o + 1;
    }
    if (ns == 0 || o + (ULONG)ns * 4 > avail) {
        return 0;
    }
    {
        int k;
        for (k = 0; k < ns; k++) {
            if (!SegOk(p + o + k * 4)) {
                return 0;
            }
        }
    }
    *up = u;
    *root = r;
    *segs = p + o;
    *nseg = ns;
    return o + (ULONG)ns * 4;
}

/* append ".SEG" (SEG = 4 raw bytes) to a path */
static void PathAddSeg(char *path, const UCHAR *seg)
{
    ULONG n = 0;
    while (path[n]) {
        n++;
    }
    if (n && n + 1 < AML_PATH) {
        path[n++] = '.';
    }
    if (n + 4 < AML_PATH) {
        path[n++] = (char)seg[0];
        path[n++] = (char)seg[1];
        path[n++] = (char)seg[2];
        path[n++] = (char)seg[3];
    }
    path[n] = 0;
}

/* drop the last segment of a dotted path */
static void PathDrop(char *path)
{
    int n = 0, last = -1;
    while (path[n]) {
        if (path[n] == '.') {
            last = n;
        }
        n++;
    }
    path[last < 0 ? 0 : last] = 0;
}

/* build an absolute path from a scope and a NameString; FALSE when it cannot */
static BOOLEAN BuildPath(const char *scope, const UCHAR *name, ULONG avail, char *out, ULONG *consumed)
{
    int up = 0, root = 0, nseg = 0, k;
    const UCHAR *segs = NULL;
    ULONG len = NameStr(name, avail, &up, &root, &segs, &nseg);
    if (len == 0) {
        return FALSE;
    }
    if (consumed) {
        *consumed = len;
    }
    if (root) {
        out[0] = 0;
    } else {
        ULONG n = 0;
        while (scope[n] && n + 1 < AML_PATH) {
            out[n] = scope[n];
            n++;
        }
        out[n] = 0;
        for (k = 0; k < up; k++) {
            PathDrop(out);
        }
    }
    for (k = 0; k < nseg; k++) {
        PathAddSeg(out, segs + k * 4);
    }
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* namespace lookup                                                     */
/* ------------------------------------------------------------------ */
static int FindNode(const char *path)
{
    int i;
    for (i = 0; i < g_NNode; i++) {
        const char *a = g_Node[i].Path, *b = path;
        while (*a && *a == *b) {
            a++;
            b++;
        }
        if (*a == 0 && *b == 0) {
            return i;
        }
    }
    return -1;
}

/* resolve a NameString (single seg -> ACPI search-up rule) to a node index */
static int Resolve(const char *scope, const UCHAR *name, ULONG avail, ULONG *consumed, char *fullOut)
{
    int up = 0, root = 0, nseg = 0, k;
    const UCHAR *segs = NULL;
    char base[AML_PATH], cand[AML_PATH];
    ULONG len = NameStr(name, avail, &up, &root, &segs, &nseg);
    if (len == 0) {
        return -2;
    }
    if (consumed) {
        *consumed = len;
    }
    if (nseg == 0) {
        return -1;
    }
    /* multi-seg, rooted or carets: exact */
    if (root || up || nseg > 1) {
        if (!BuildPath(scope, name, avail, cand, NULL)) {
            return -1;
        }
        if (fullOut) {
            UsbMemCpy(fullOut, cand, AML_PATH);
        }
        return FindNode(cand);
    }
    /* single segment: search upward through the scopes */
    {
        ULONG n = 0;
        while (scope[n] && n + 1 < AML_PATH) {
            base[n] = scope[n];
            n++;
        }
        base[n] = 0;
    }
    for (;;) {
        int idx;
        UsbMemCpy(cand, base, AML_PATH);
        PathAddSeg(cand, segs);
        idx = FindNode(cand);
        if (idx >= 0) {
            if (fullOut) {
                UsbMemCpy(fullOut, cand, AML_PATH);
            }
            return idx;
        }
        if (base[0] == 0) {
            break;
        }
        PathDrop(base);
    }
    if (fullOut) {
        cand[0] = 0;
        PathAddSeg(cand, segs);
        UsbMemCpy(fullOut, cand, AML_PATH);
    }
    (void)k;
    return -1;
}

static int AddNode(const char *path, UCHAR type, const UCHAR *T)
{
    int i;
    if (g_NNode >= AML_MAX_NODE) {
        return -1;
    }
    i = FindNode(path);
    if (i >= 0) {                         /* a later table may re-open a scope */
        if (g_Node[i].Type == NT_SCOPE && type != NT_SCOPE) {
            g_Node[i].Type = type;
            g_Node[i].T = T;
        }
        return i;
    }
    i = g_NNode++;
    UsbMemSet(&g_Node[i], 0, sizeof(g_Node[i]));
    {
        ULONG n = 0;
        while (path[n] && n + 1 < AML_PATH) {
            g_Node[i].Path[n] = path[n];
            n++;
        }
        g_Node[i].Path[n] = 0;
    }
    g_Node[i].Type = type;
    g_Node[i].T = T;
    return i;
}

/* ------------------------------------------------------------------ */
/* namespace build                                                      */
/* ------------------------------------------------------------------ */
static void ParseFieldList(const UCHAR *T, ULONG p, ULONG end, int region, int indexN, int dataN,
                           int bankN, ULONGLONG bankVal, UCHAR flags, const char *scope)
{
    ULONG bit = 0, conn = 0, connEnd = 0;
    UCHAR access = (UCHAR)(flags & 0x0F);
    while (p < end && g_NNode < AML_MAX_NODE) {
        UCHAR c = T[p];
        if (c == 0x00) {                              /* ReservedField */
            ULONG h = 1, w = PkgLen(T + p + 1, end - p - 1, &h);
            bit += w;
            p += 1 + h;
        } else if (c == 0x01) {                       /* AccessField */
            access = (UCHAR)(T[p + 1] & 0x0F);
            p += 3;
        } else if (c == 0x03) {                       /* ExtendedAccessField */
            access = (UCHAR)(T[p + 1] & 0x0F);
            p += 4;
        } else if (c == 0x02) {                       /* ConnectField */
            if (T[p + 1] == 0x11) {                   /* inline buffer (ResourceTemplate) */
                ULONG h = 1, plen = PkgLen(T + p + 2, end - p - 2, &h);
                conn = p + 2 + h;
                connEnd = p + 2 + plen;
                p = p + 2 + plen;
            } else {                                  /* NameString connection: skip */
                int u, r, ns;
                const UCHAR *s;
                ULONG l = NameStr(T + p + 1, end - p - 1, &u, &r, &s, &ns);
                if (l == 0) {
                    return;
                }
                p += 1 + l;
            }
        } else if (SegOk(T + p)) {
            ULONG h = 1, w = PkgLen(T + p + 4, end - p - 4, &h);
            char path[AML_PATH];
            int idx;
            UsbMemCpy(path, scope, AML_PATH);
            PathAddSeg(path, T + p);
            idx = AddNode(path, NT_FIELD, T);
            if (idx >= 0) {
                g_Node[idx].Region = region;
                g_Node[idx].IndexN = indexN;
                g_Node[idx].DataN = dataN;
                g_Node[idx].BankN = bankN;
                g_Node[idx].BankVal = bankVal;
                g_Node[idx].BitOff = bit;
                g_Node[idx].BitLen = w;
                g_Node[idx].Access = access;
                g_Node[idx].ConnOff = conn;
                g_Node[idx].ConnEnd = connEnd;
            }
            bit += w;
            p += 4 + h;
        } else {
            return;
        }
    }
}

/* parse a TermList (namespace objects) within [p,end) at the given scope */
static void BuildScope(const UCHAR *T, ULONG p, ULONG end, const char *scope)
{
    while (p < end && g_NNode < AML_MAX_NODE) {
        UCHAR op = T[p];
        ULONG h, plen;
        if (op == 0x08) {                             /* Name(NameString, DataRefObject) */
            char path[AML_PATH];
            ULONG cons = 0;
            int idx;
            if (!BuildPath(scope, T + p + 1, end - p - 1, path, &cons)) {
                return;
            }
            idx = AddNode(path, NT_NAME, T);
            if (idx >= 0) {
                g_Node[idx].A = p + 1 + cons;
            }
            /* skip the data object: handled lazily on eval, but we must advance.
               Use a conservative skip over one TermArg. */
            p = p + 1 + cons;
            {
                /* skip one object by structure */
                ULONG o = p, oh;
                UCHAR d = T[o];
                if (d == 0x0A) o += 2;
                else if (d == 0x0B) o += 3;
                else if (d == 0x0C) o += 5;
                else if (d == 0x0E) o += 9;
                else if (d == 0x00 || d == 0x01 || d == 0xFF) o += 1;
                else if (d == 0x0D) { o++; while (o < end && T[o]) o++; o++; }
                else if (d == 0x11 || d == 0x12 || d == 0x13) {
                    plen = PkgLen(T + o + 1, end - o - 1, &oh);
                    o = o + 1 + plen;
                } else {
                    o += 1;                           /* best effort */
                }
                p = o;
            }
            continue;
        }
        if (op == 0x10) {                             /* Scope(NameString) TermList */
            char path[AML_PATH];
            ULONG cons = 0;
            plen = PkgLen(T + p + 1, end - p - 1, &h);
            if (plen == 0 || p + 1 + plen > end) {
                return;
            }
            if (BuildPath(scope, T + p + 1 + h, end - p - 1 - h, path, &cons)) {
                AddNode(path, NT_SCOPE, T);
                BuildScope(T, p + 1 + h + cons, p + 1 + plen, path);
            }
            p = p + 1 + plen;
            continue;
        }
        if (op == 0x14) {                             /* Method(NameString, flags) TermList */
            char path[AML_PATH];
            ULONG cons = 0;
            int idx;
            plen = PkgLen(T + p + 1, end - p - 1, &h);
            if (plen == 0 || p + 1 + plen > end) {
                return;
            }
            if (BuildPath(scope, T + p + 1 + h, end - p - 1 - h, path, &cons)) {
                idx = AddNode(path, NT_METHOD, T);
                if (idx >= 0) {
                    g_Node[idx].ArgCount = (UCHAR)(T[p + 1 + h + cons] & 0x07);
                    g_Node[idx].A = p + 1 + h + cons + 1;
                    g_Node[idx].B = p + 1 + plen;
                }
            }
            p = p + 1 + plen;
            continue;
        }
        if (op == 0x06) {                             /* Alias(src, dst) */
            char src[AML_PATH], dst[AML_PATH];
            ULONG c1 = 0, c2 = 0;
            int idx;
            if (!BuildPath(scope, T + p + 1, end - p - 1, src, &c1)) {
                return;
            }
            if (!BuildPath(scope, T + p + 1 + c1, end - p - 1 - c1, dst, &c2)) {
                return;
            }
            idx = AddNode(dst, NT_ALIAS, T);
            if (idx >= 0) {
                g_Node[idx].Target = FindNode(src);
            }
            p = p + 1 + c1 + c2;
            continue;
        }
        if (op == 0x5B) {                             /* ExtOpPrefix */
            UCHAR e = T[p + 1];
            if (e == 0x82 || e == 0x83 || e == 0x84 || e == 0x85) {   /* Device/Processor/PowerRes/ThermalZone */
                char path[AML_PATH];
                ULONG cons = 0, body;
                plen = PkgLen(T + p + 2, end - p - 2, &h);
                if (plen == 0 || p + 2 + plen > end) {
                    return;
                }
                if (BuildPath(scope, T + p + 2 + h, end - p - 2 - h, path, &cons)) {
                    body = p + 2 + h + cons;
                    if (e == 0x83) {
                        body += 6;                    /* ProcID, PblkAddr, PblkLen */
                    } else if (e == 0x84) {
                        /* PowerResource: SystemLevel(byte) ResourceOrder(word) */
                        body += 3;
                    }
                    AddNode(path, NT_SCOPE, T);
                    BuildScope(T, body, p + 2 + plen, path);
                }
                p = p + 2 + plen;
                continue;
            }
            if (e == 0x80) {                          /* OperationRegion(name, space, off, len) */
                char path[AML_PATH];
                ULONG cons = 0, o;
                int idx;
                if (!BuildPath(scope, T + p + 2, end - p - 2, path, &cons)) {
                    return;
                }
                o = p + 2 + cons;
                idx = AddNode(path, NT_REGION, T);
                if (idx >= 0) {
                    g_Node[idx].Space = T[o];
                    g_Node[idx].RgOffOff = o + 1;
                }
                /* advance past space + two TermArgs (off, len) */
                o += 1;
                {
                    int tk;
                    for (tk = 0; tk < 2; tk++) {
                        UCHAR d = T[o];
                        if (d == 0x0A) o += 2;
                        else if (d == 0x0B) o += 3;
                        else if (d == 0x0C) o += 5;
                        else if (d == 0x0E) o += 9;
                        else if (d == 0x00 || d == 0x01 || d == 0xFF) o += 1;
                        else {
                            /* non-constant region bound: evaluate lazily, but we
                               cannot safely skip it here -> give up on this table region.
                               Mark unresolved and try a structural skip of a name/small expr. */
                            int u, r, ns; const UCHAR *s;
                            ULONG l = NameStr(T + o, end - o, &u, &r, &s, &ns);
                            if (l) { o += l; } else { o += 1; }
                        }
                        if (tk == 0 && idx >= 0) {
                            g_Node[idx].RgLenOff = o;
                        }
                    }
                }
                p = o;
                continue;
            }
            if (e == 0x81) {                          /* Field(region, flags) FieldList */
                int region;
                ULONG cons = 0;
                plen = PkgLen(T + p + 2, end - p - 2, &h);
                if (plen == 0 || p + 2 + plen > end) {
                    return;
                }
                region = Resolve(scope, T + p + 2 + h, end - p - 2 - h, &cons, NULL);
                ParseFieldList(T, p + 2 + h + cons + 1, p + 2 + plen, region, -1, -1, -1, 0,
                               T[p + 2 + h + cons], scope);
                p = p + 2 + plen;
                continue;
            }
            if (e == 0x86) {                          /* IndexField(index, data, flags) */
                int ixn, dtn;
                ULONG c1 = 0, c2 = 0;
                plen = PkgLen(T + p + 2, end - p - 2, &h);
                if (plen == 0 || p + 2 + plen > end) {
                    return;
                }
                ixn = Resolve(scope, T + p + 2 + h, end - p - 2 - h, &c1, NULL);
                dtn = Resolve(scope, T + p + 2 + h + c1, end - p - 2 - h - c1, &c2, NULL);
                ParseFieldList(T, p + 2 + h + c1 + c2 + 1, p + 2 + plen, -1, ixn, dtn, -1, 0,
                               T[p + 2 + h + c1 + c2], scope);
                p = p + 2 + plen;
                continue;
            }
            if (e == 0x87) {                          /* BankField(region, bank, bankval, flags) */
                p = p + 2;
                plen = PkgLen(T + p, end - p, &h);
                if (plen == 0 || p + plen > end) {
                    return;
                }
                p = p + plen;                         /* bank fields: skipped (rare for our devices) */
                continue;
            }
            if (e == 0x01) {                          /* Mutex(name, flags) */
                char path[AML_PATH];
                ULONG cons = 0;
                if (BuildPath(scope, T + p + 2, end - p - 2, path, &cons)) {
                    AddNode(path, NT_MUTEX, T);
                }
                p = p + 2 + cons + 1;
                continue;
            }
            if (e == 0x02) {                          /* Event(name) */
                char path[AML_PATH];
                ULONG cons = 0;
                if (BuildPath(scope, T + p + 2, end - p - 2, path, &cons)) {
                    AddNode(path, NT_EVENT, T);
                }
                p = p + 2 + cons;
                continue;
            }
            /* other ext ops at namespace level: skip by package length when present */
            if (e == 0x13) {                          /* CreateField(buf, bit, count, name) - top level rare */
                p += 2;
                continue;
            }
            p += 2;
            continue;
        }
        /* a byte we do not open a scope for: stop (we only parse named objects,
           and real tables keep all named objects at the term-list head of a scope) */
        if (op == 0xA4 || op == 0xA0 || op == 0xA1 || op == 0xA2) {
            return;                                   /* executable term: not in a namespace scope */
        }
        p++;
    }
}

int AmlInit(void)
{
    int ti;
    const ACPI_TABLE *t;
    if (g_Done) {
        return g_Off ? 0 : g_NNode;
    }
    g_Done = TRUE;
    if (OsGetConfig("I2cHidAml", 1) == 0) {
        g_Off = TRUE;
        return 0;
    }
    AcpiInit();
    for (ti = 0; (t = AcpiTable(ti)) != NULL; ti++) {
        if (t->Sig != ACPI_SIG('D', 'S', 'D', 'T') && t->Sig != ACPI_SIG('S', 'S', 'D', 'T')) {
            continue;
        }
        BuildScope(t->Va, 36, t->Len, "");
    }
    UsbLog(LOG_INFO, "aml: %d named objects from the ACPI tables\n", g_NNode);
    return g_NNode;
}

void AmlShutdown(void)
{
    GpioCloseAll();
}

/* ------------------------------------------------------------------ */
/* data object evaluation (reading a Name's value / a DataRefObject)    */
/* ------------------------------------------------------------------ */
static int EvalTermArg(CTX *c, const UCHAR *T, ULONG *pp, ULONG end, AML_VAL *out);
static int ExecList(CTX *c, const UCHAR *T, ULONG p, ULONG end);
static int ReadField(int fnode, AML_VAL *out);
static int WriteField(int fnode, const AML_VAL *val);

/* skip one object (TermArg / DataObject) and return bytes consumed */
static ULONG SkipObj(const UCHAR *T, ULONG p, ULONG end);

/* ------------------------------------------------------------------ */
/* region access                                                        */
/* ------------------------------------------------------------------ */
static BOOLEAN ResolveRegion(int rn)
{
    NODE *r = &g_Node[rn];
    ULONG pp;
    AML_VAL v;
    if (r->RgResolved) {
        return TRUE;
    }
    /* region base / length are constants (or simple) in practice: evaluate them
       with no method context (NULL), which is enough for integer expressions. */
    pp = r->RgOffOff;
    if (EvalTermArg(NULL, r->T, &pp, r->RgOffOff + 64, &v) != AML_OK || !AmlAsInt(&v, &r->RgBase)) {
        AmlFree(&v);
        return FALSE;
    }
    AmlFree(&v);
    pp = r->RgLenOff;
    if (EvalTermArg(NULL, r->T, &pp, r->RgLenOff + 64, &v) == AML_OK) {
        AmlAsInt(&v, &r->RgLen);
    }
    AmlFree(&v);
    r->RgResolved = 1;
    return TRUE;
}

/* read/write up to 64 bits from a field unit's region */
static int FieldMem(int fnode, BOOLEAN write, ULONGLONG *val)
{
    NODE *f = &g_Node[fnode];
    int rn = f->Region;
    NODE *r;
    ULONG byteoff, nbytes, i;
    UCHAR acc;
    if (rn < 0 || rn >= g_NNode || g_Node[rn].Type != NT_REGION) {
        return AML_FAIL;
    }
    r = &g_Node[rn];
    if (!ResolveRegion(rn)) {
        return AML_FAIL;
    }
    if (f->BitLen == 0 || f->BitLen > 64 || (f->BitOff & 7) != 0) {
        return AML_FAIL;                              /* only byte-aligned units */
    }
    acc = f->Access ? f->Access : 1;
    byteoff = f->BitOff / 8;
    nbytes = (f->BitLen + 7) / 8;
    if (acc == 2 && nbytes < 2) nbytes = 2;
    else if (acc == 3 && nbytes < 4) nbytes = 4;
    else if (acc == 4 && nbytes < 8) nbytes = 8;
    if (nbytes > 8) {
        return AML_FAIL;
    }
    if (r->Space == 0) {                              /* SystemMemory */
        ULONGLONG pa = r->RgBase + byteoff;
        UCHAR *va = (UCHAR *)OsMapMmio(0, (ULONG)(pa & ~0xFFFULL), 0x2000);
        ULONG off = (ULONG)(pa & 0xFFF);
        if (va == NULL || pa >= 0xFFFFFFFFULL) {
            if (va) OsUnmapMmio(va, 0x2000);
            return AML_FAIL;
        }
        if (write) {
            for (i = 0; i < nbytes; i++) {
                va[off + i] = (UCHAR)(*val >> (8 * i));
            }
        } else {
            ULONGLONG v = 0;
            for (i = 0; i < nbytes; i++) {
                v |= (ULONGLONG)va[off + i] << (8 * i);
            }
            *val = v;
        }
        OsUnmapMmio(va, 0x2000);
        return AML_OK;
    }
    if (r->Space == 1) {                              /* SystemIO */
        ULONG port = (ULONG)r->RgBase + byteoff;
        if (write) {
            if (nbytes == 1) OsOut8(port, (UCHAR)*val);
            else if (nbytes == 2) OsOut16(port, (USHORT)*val);
            else OsOut32(port, (ULONG)*val);
        } else {
            if (nbytes == 1) *val = OsIn8(port);
            else if (nbytes == 2) *val = OsIn16(port);
            else *val = OsIn32(port);
        }
        return AML_OK;
    }
    if (r->Space == 2) {                              /* PCI_Config */
        char scope[AML_PATH];
        int dn;
        ULONGLONG adr = 0;
        UCHAR bus = 0, dev = 0, fn = 0;
        UsbMemCpy(scope, r->Path, AML_PATH);
        PathDrop(scope);                              /* the device that owns the region */
        {
            char ap[AML_PATH];
            UsbMemCpy(ap, scope, AML_PATH);
            PathAddSeg(ap, (const UCHAR *)"_ADR");
            dn = FindNode(ap);
        }
        if (dn >= 0 && g_Node[dn].Type == NT_NAME) {
            AML_VAL v;
            ULONG pp = g_Node[dn].A;
            if (EvalTermArg(NULL, g_Node[dn].T, &pp, g_Node[dn].A + 16, &v) == AML_OK) {
                AmlAsInt(&v, &adr);
            }
            AmlFree(&v);
        }
        dev = (UCHAR)((adr >> 16) & 0x1F);
        fn = (UCHAR)(adr & 0x7);
        if (write) {
            OsPciWrite(bus, dev, fn, (ULONG)r->RgBase + byteoff, (ULONG)*val, (int)nbytes);
        } else {
            *val = OsPciRead(bus, dev, fn, (ULONG)r->RgBase + byteoff, (int)nbytes);
        }
        return AML_OK;
    }
    if (r->Space == 8) {                              /* GeneralPurposeIO */
        AML_GPIO_RES g;
        AML_CRS crs;
        AML_VAL cb;
        GPIO_PIN pin;
        if (f->ConnOff == 0 || f->ConnEnd <= f->ConnOff) {
            return AML_FAIL;
        }
        UsbMemSet(&crs, 0, sizeof(crs));
        if (!NewBuf(&cb, f->ConnEnd - f->ConnOff)) {
            return AML_FAIL;
        }
        UsbMemCpy(cb.Buf, f->T + f->ConnOff, f->ConnEnd - f->ConnOff);
        AmlParseCrs(&cb, &crs);
        AmlFree(&cb);
        if (crs.NGpio < 1) {
            return AML_FAIL;
        }
        g = crs.Gpio[0];
        if (!GpioResolvePin(g.Ctrl4, (ULONG)(g.Pin + f->BitOff / 8), &pin)) {
            return AML_FAIL;
        }
        if (write) {
            GpioSet(&pin, (BOOLEAN)(*val & 1));
        } else {
            int lv = GpioGet(&pin);
            *val = (lv > 0) ? 1 : 0;
        }
        return AML_OK;
    }
    return AML_FAIL;
}

static int ReadField(int fnode, AML_VAL *out)
{
    NODE *f = &g_Node[fnode];
    ULONGLONG v = 0;
    int r;
    if (f->IndexN >= 0 && f->DataN >= 0) {            /* IndexField: write index, read data */
        ULONGLONG idx = f->BitOff / 8;
        AML_VAL iv;
        AmlSetInt(&iv, idx);
        if (WriteField(f->IndexN, &iv) != AML_OK) {
            return AML_FAIL;
        }
        return ReadField(f->DataN, out);
    }
    r = FieldMem(fnode, FALSE, &v);
    if (r != AML_OK) {
        return r;
    }
    if (f->BitLen < 64) {
        v &= ((ULONGLONG)1 << f->BitLen) - 1;
    }
    AmlSetInt(out, v);
    return AML_OK;
}

static int WriteField(int fnode, const AML_VAL *val)
{
    NODE *f = &g_Node[fnode];
    ULONGLONG v = 0;
    if (!AmlAsInt(val, &v)) {
        return AML_FAIL;
    }
    if (f->IndexN >= 0 && f->DataN >= 0) {
        ULONGLONG idx = f->BitOff / 8;
        AML_VAL iv;
        AmlSetInt(&iv, idx);
        if (WriteField(f->IndexN, &iv) != AML_OK) {
            return AML_FAIL;
        }
        return WriteField(f->DataN, val);
    }
    return FieldMem(fnode, TRUE, &v);
}

/* ------------------------------------------------------------------ */
/* dynamic (method-local) names                                         */
/* ------------------------------------------------------------------ */
static DYN *DynFind(CTX *c, const char *path, BOOLEAN create)
{
    int i, free = -1;
    for (i = 0; i < AML_MAX_DYN; i++) {
        if (c->Dyn[i].Used) {
            const char *a = c->Dyn[i].Path, *b = path;
            while (*a && *a == *b) { a++; b++; }
            if (*a == 0 && *b == 0) {
                return &c->Dyn[i];
            }
        } else if (free < 0) {
            free = i;
        }
    }
    if (create && free >= 0) {
        ULONG n = 0;
        c->Dyn[free].Used = 1;
        while (path[n] && n + 1 < AML_PATH) { c->Dyn[free].Path[n] = path[n]; n++; }
        c->Dyn[free].Path[n] = 0;
        UsbMemSet(&c->Dyn[free].Val, 0, sizeof(AML_VAL));
        return &c->Dyn[free];
    }
    return NULL;
}

static BUFFLD *BfFind(CTX *c, const char *path)
{
    int i;
    for (i = 0; i < AML_MAX_BF; i++) {
        if (c->Bf[i].Used) {
            const char *a = c->Bf[i].Path, *b = path;
            while (*a && *a == *b) { a++; b++; }
            if (*a == 0 && *b == 0) {
                return &c->Bf[i];
            }
        }
    }
    return NULL;
}

static BUFFLD *BfNew(CTX *c, const char *path)
{
    int i;
    for (i = 0; i < AML_MAX_BF; i++) {
        if (!c->Bf[i].Used) {
            ULONG n = 0;
            c->Bf[i].Used = 1;
            while (path[n] && n + 1 < AML_PATH) { c->Bf[i].Path[n] = path[n]; n++; }
            c->Bf[i].Path[n] = 0;
            return &c->Bf[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* object / target handling                                             */
/* ------------------------------------------------------------------ */
#define TGT_NONE  0
#define TGT_LOCAL 1
#define TGT_ARG   2
#define TGT_DYN   3
#define TGT_FIELD 4
#define TGT_BF    5
#define TGT_PKGEL 6
#define TGT_DEBUG 7
#define TGT_NAME  8

typedef struct {
    int      Kind;
    int      Idx;          /* local/arg index, field node, name node */
    DYN     *Dyn;
    BUFFLD  *Bf;
    AML_VAL *PkgOwner;     /* TGT_PKGEL */
    ULONG    PkgIdx;
} TARGET;

static int ParseTarget(CTX *c, const UCHAR *T, ULONG *pp, ULONG end, TARGET *tg);

/* read a named object's current value (NAME data, FIELD read, dyn, bf) */
static int ReadNamed(CTX *c, int node, const char *fullpath, AML_VAL *out)
{
    if (c) {
        DYN *d = DynFind(c, fullpath, FALSE);
        BUFFLD *b;
        if (d) {
            return CopyVal(out, &d->Val) ? AML_OK : AML_FAIL;
        }
        b = BfFind(c, fullpath);
        if (b) {
            DYN *owner = (b->Owner >= 0 && b->Owner < AML_MAX_DYN && c->Dyn[b->Owner].Used) ? &c->Dyn[b->Owner] : NULL;
            ULONGLONG v = 0;
            ULONG i, nbytes = (b->BitLen + 7) / 8, off = b->BitOff / 8;
            if (!owner || owner->Val.Kind != AMLV_BUF) {
                return AML_FAIL;
            }
            for (i = 0; i < nbytes && off + i < owner->Val.Len; i++) {
                v |= (ULONGLONG)owner->Val.Buf[off + i] << (8 * i);
            }
            if (b->BitLen < 64) {
                v &= ((ULONGLONG)1 << b->BitLen) - 1;
            }
            AmlSetInt(out, v);
            return AML_OK;
        }
    }
    if (node < 0) {
        return AML_FAIL;
    }
    while (g_Node[node].Type == NT_ALIAS && g_Node[node].Target >= 0) {
        node = g_Node[node].Target;
    }
    if (g_Node[node].Type == NT_NAME) {
        ULONG pp = g_Node[node].A;
        return EvalTermArg(NULL, g_Node[node].T, &pp, g_Node[node].A + 4096, out);
    }
    if (g_Node[node].Type == NT_FIELD) {
        return ReadField(node, out);
    }
    if (g_Node[node].Type == NT_REGION || g_Node[node].Type == NT_SCOPE) {
        AmlSetInt(out, 0);                            /* a bare device/region ref as a value */
        return AML_OK;
    }
    return AML_FAIL;
}

static int StoreTo(CTX *c, TARGET *tg, const AML_VAL *val)
{
    switch (tg->Kind) {
    case TGT_NONE:
    case TGT_DEBUG:
        return AML_OK;
    case TGT_LOCAL:
        AmlFree(&c->Local[tg->Idx]);
        return CopyVal(&c->Local[tg->Idx], val) ? AML_OK : AML_FAIL;
    case TGT_ARG:
        AmlFree(&c->Arg[tg->Idx]);
        return CopyVal(&c->Arg[tg->Idx], val) ? AML_OK : AML_FAIL;
    case TGT_DYN:
        AmlFree(&tg->Dyn->Val);
        return CopyVal(&tg->Dyn->Val, val) ? AML_OK : AML_FAIL;
    case TGT_FIELD:
        return WriteField(tg->Idx, val);
    case TGT_BF: {
        BUFFLD *b = tg->Bf;
        DYN *owner = (b->Owner >= 0 && c->Dyn[b->Owner].Used) ? &c->Dyn[b->Owner] : NULL;
        ULONGLONG v = 0;
        ULONG i, nbytes = (b->BitLen + 7) / 8, off = b->BitOff / 8;
        if (!owner || owner->Val.Kind != AMLV_BUF || !AmlAsInt(val, &v)) {
            return AML_FAIL;
        }
        for (i = 0; i < nbytes && off + i < owner->Val.Len; i++) {
            owner->Val.Buf[off + i] = (UCHAR)(v >> (8 * i));
        }
        return AML_OK;
    }
    case TGT_PKGEL:
        if (tg->PkgOwner && tg->PkgIdx < tg->PkgOwner->Len) {
            AmlFree(&tg->PkgOwner->Pkg[tg->PkgIdx]);
            return CopyVal(&tg->PkgOwner->Pkg[tg->PkgIdx], val) ? AML_OK : AML_FAIL;
        }
        return AML_FAIL;
    case TGT_NAME:
        if (tg->Idx >= 0 && g_Node[tg->Idx].Type == NT_FIELD) {
            return WriteField(tg->Idx, val);
        }
        return AML_OK;                                /* Store to a Name node: ignored (we evaluate lazily) */
    }
    return AML_FAIL;
}

/* ------------------------------------------------------------------ */
/* the evaluator                                                        */
/* ------------------------------------------------------------------ */
static ULONG SkipObj(const UCHAR *T, ULONG p, ULONG end)
{
    ULONG start = p, h, plen;
    UCHAR d = T[p];
    if (d == 0x0A) return 2;
    if (d == 0x0B) return 3;
    if (d == 0x0C) return 5;
    if (d == 0x0E) return 9;
    if (d == 0x00 || d == 0x01 || d == 0xFF) return 1;
    if (d == 0x0D) { p++; while (p < end && T[p]) p++; return p + 1 - start; }
    if (d == 0x11 || d == 0x12 || d == 0x13) {
        plen = PkgLen(T + p + 1, end - p - 1, &h);
        return 1 + plen;
    }
    return 1;
}

/* a comparison helper */
static ULONGLONG LTrue(BOOLEAN b)
{
    return b ? ~(ULONGLONG)0 : 0;
}

/* parse a Package/VarPackage into out */
static int EvalPackage(CTX *c, const UCHAR *T, ULONG p, ULONG end, AML_VAL *out)
{
    ULONG h, plen, num, q;
    ULONG i;
    plen = PkgLen(T + p, end - p, &h);
    if (plen == 0 || p + plen > end) {
        return AML_FAIL;
    }
    q = p + h;
    if (T[p - 1] == 0x12) {                           /* fixed: byte count (we are called with p at pkglen) */
        num = T[q];
        q++;
    } else {
        AML_VAL n;
        ULONG pp = q;
        if (EvalTermArg(c, T, &pp, p + plen, &n) != AML_OK) {
            return AML_FAIL;
        }
        num = (ULONG)n.Int;
        AmlFree(&n);
        q = pp;
    }
    if (num > 64) {
        return AML_FAIL;
    }
    UsbMemSet(out, 0, sizeof(*out));
    out->Kind = AMLV_PKG;
    out->Len = num;
    out->Pkg = (AML_VAL *)OsAlloc(sizeof(AML_VAL) * (num ? num : 1));
    if (out->Pkg == NULL) {
        return AML_FAIL;
    }
    for (i = 0; i < num && q < p + plen; i++) {
        UCHAR d = T[q];
        if (d == 0x00 || d == 0x01 || d == 0xFF || d == 0x0A || d == 0x0B || d == 0x0C || d == 0x0E ||
            d == 0x0D || d == 0x11 || d == 0x12 || d == 0x13) {
            ULONG pp = q;
            if (EvalTermArg(c, T, &pp, p + plen, &out->Pkg[i]) != AML_OK) {
                AmlSetInt(&out->Pkg[i], 0);
            }
            q = pp;
        } else {
            /* a NameString element: store as uninitialised reference by value 0 */
            int u, r, ns; const UCHAR *s;
            ULONG l = NameStr(T + q, p + plen - q, &u, &r, &s, &ns);
            AmlSetInt(&out->Pkg[i], 0);
            if (l == 0) {
                break;
            }
            q += l;
        }
    }
    return AML_OK;
}

static int EvalBuffer(CTX *c, const UCHAR *T, ULONG p, ULONG end, AML_VAL *out)
{
    ULONG h, plen, q;
    AML_VAL sz;
    ULONGLONG szv = 0;
    ULONG n, avail, i;
    plen = PkgLen(T + p, end - p, &h);
    if (plen == 0 || p + plen > end) {
        return AML_FAIL;
    }
    q = p + h;
    if (EvalTermArg(c, T, &q, p + plen, &sz) != AML_OK || !AmlAsInt(&sz, &szv)) {
        AmlFree(&sz);
        return AML_FAIL;
    }
    n = (ULONG)szv;
    AmlFree(&sz);
    if (!NewBuf(out, n)) {
        return AML_FAIL;
    }
    avail = p + plen - q;
    for (i = 0; i < n && i < avail; i++) {
        out->Buf[i] = T[q + i];
    }
    return AML_OK;
}

/* parse a Target (SuperName or NullName) */
static int ParseTarget(CTX *c, const UCHAR *T, ULONG *pp, ULONG end, TARGET *tg)
{
    ULONG p = *pp;
    UCHAR op = T[p];
    UsbMemSet(tg, 0, sizeof(*tg));
    if (op == 0x00) {                                 /* NullName / no target */
        tg->Kind = TGT_NONE;
        *pp = p + 1;
        return AML_OK;
    }
    if (op >= 0x60 && op <= 0x67) {
        tg->Kind = TGT_LOCAL;
        tg->Idx = op - 0x60;
        *pp = p + 1;
        return AML_OK;
    }
    if (op >= 0x68 && op <= 0x6E) {
        tg->Kind = TGT_ARG;
        tg->Idx = op - 0x68;
        *pp = p + 1;
        return AML_OK;
    }
    if (op == 0x5B && T[p + 1] == 0x31) {             /* Debug */
        tg->Kind = TGT_DEBUG;
        *pp = p + 2;
        return AML_OK;
    }
    if (op == 0x88) {                                 /* Index(src, idx, target) as a store target */
        AML_VAL src, idx;
        int r;
        ULONG q = p + 1;
        TARGET t2;
        r = EvalTermArg(c, T, &q, end, &src);
        if (r != AML_OK) { AmlFree(&src); return r; }
        r = EvalTermArg(c, T, &q, end, &idx);
        if (r != AML_OK) { AmlFree(&src); AmlFree(&idx); return r; }
        ParseTarget(c, T, &q, end, &t2);
        *pp = q;
        if (src.Kind == AMLV_PKG) {
            /* store into the package element: we need a persistent owner. Only
               supported when src came from a Local/Arg/Dyn - not tracked here. */
            AmlFree(&src); AmlFree(&idx);
            tg->Kind = TGT_NONE;
            return AML_OK;
        }
        AmlFree(&src); AmlFree(&idx);
        tg->Kind = TGT_NONE;
        return AML_OK;
    }
    /* a NameString target */
    {
        char full[AML_PATH];
        ULONG cons = 0;
        int node;
        DYN *d;
        BUFFLD *b;
        if (!BuildPath(c ? c->Scope : "", T + p, end - p, full, &cons)) {
            return AML_FAIL;
        }
        *pp = p + cons;
        if (c) {
            b = BfFind(c, full);
            if (b) { tg->Kind = TGT_BF; tg->Bf = b; return AML_OK; }
            d = DynFind(c, full, FALSE);
            if (d) { tg->Kind = TGT_DYN; tg->Dyn = d; return AML_OK; }
        }
        node = Resolve(c ? c->Scope : "", T + p, end - p, NULL, NULL);
        if (node >= 0 && g_Node[node].Type == NT_FIELD) {
            tg->Kind = TGT_FIELD;
            tg->Idx = node;
            return AML_OK;
        }
        /* a brand new local Name target (e.g. Store(x, LOCALNAME) without Name()) */
        if (c) {
            d = DynFind(c, full, TRUE);
            if (d) { tg->Kind = TGT_DYN; tg->Dyn = d; return AML_OK; }
        }
        tg->Kind = TGT_NONE;
        return AML_OK;
    }
}

static int CallMethod(CTX *parent, int node, const AML_VAL *args, int nargs, AML_VAL *out);

/* evaluate one TermArg at *pp, advancing it */
static int EvalTermArg(CTX *c, const UCHAR *T, ULONG *pp, ULONG end, AML_VAL *out)
{
    ULONG p = *pp;
    UCHAR op;
    int r;
    UsbMemSet(out, 0, sizeof(*out));
    if (p >= end) {
        return AML_FAIL;
    }
    op = T[p];
    switch (op) {
    case 0x00: AmlSetInt(out, 0); *pp = p + 1; return AML_OK;
    case 0x01: AmlSetInt(out, 1); *pp = p + 1; return AML_OK;
    case 0xFF: AmlSetInt(out, ~(ULONGLONG)0); *pp = p + 1; return AML_OK;
    case 0x0A: AmlSetInt(out, T[p + 1]); *pp = p + 2; return AML_OK;
    case 0x0B: AmlSetInt(out, G16(T + p + 1)); *pp = p + 3; return AML_OK;
    case 0x0C: AmlSetInt(out, G32(T + p + 1)); *pp = p + 5; return AML_OK;
    case 0x0E: AmlSetInt(out, G64(T + p + 1)); *pp = p + 9; return AML_OK;
    case 0x0D: {                                      /* string */
        ULONG n = 0;
        while (p + 1 + n < end && T[p + 1 + n]) n++;
        if (!NewBuf(out, n)) return AML_FAIL;
        out->Kind = AMLV_STR;
        UsbMemCpy(out->Buf, T + p + 1, n);
        out->Buf[n] = 0;
        *pp = p + 1 + n + 1;
        return AML_OK;
    }
    case 0x11: {
        ULONG h, plen = PkgLen(T + p + 1, end - p - 1, &h);
        r = EvalBuffer(c, T, p + 1, end, out);
        *pp = p + 1 + plen;
        return r;
    }
    case 0x12:
    case 0x13: {
        ULONG h, plen = PkgLen(T + p + 1, end - p - 1, &h);
        r = EvalPackage(c, T, p + 1, end, out);
        *pp = p + 1 + plen;
        return r;
    }
    default: break;
    }
    if (op >= 0x60 && op <= 0x67) {
        *pp = p + 1;
        return c ? (CopyVal(out, &c->Local[op - 0x60]) ? AML_OK : AML_FAIL) : AML_FAIL;
    }
    if (op >= 0x68 && op <= 0x6E) {
        *pp = p + 1;
        return c ? (CopyVal(out, &c->Arg[op - 0x68]) ? AML_OK : AML_FAIL) : AML_FAIL;
    }
    /* binary / unary operators with a target */
    switch (op) {
    case 0x72: case 0x74: case 0x77: case 0x79: case 0x7A: case 0x7B: case 0x7C:
    case 0x7D: case 0x7E: case 0x7F: {                /* Add Sub Mul Shl Shr And Nand Or Nor Xor */
        AML_VAL a, b;
        TARGET tg;
        ULONGLONG x = 0, y = 0, z = 0;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        r = EvalTermArg(c, T, &q, end, &b);
        if (r != AML_OK) { AmlFree(&a); AmlFree(&b); return r; }
        AmlAsInt(&a, &x); AmlAsInt(&b, &y);
        AmlFree(&a); AmlFree(&b);
        switch (op) {
        case 0x72: z = x + y; break;
        case 0x74: z = x - y; break;
        case 0x77: z = x * y; break;
        case 0x79: z = y < 64 ? x << y : 0; break;
        case 0x7A: z = y < 64 ? x >> y : 0; break;
        case 0x7B: z = x & y; break;
        case 0x7C: z = ~(x & y); break;
        case 0x7D: z = x | y; break;
        case 0x7E: z = ~(x | y); break;
        case 0x7F: z = x ^ y; break;
        }
        r = ParseTarget(c, T, &q, end, &tg);
        if (r != AML_OK) return r;
        AmlSetInt(out, z);
        StoreTo(c, &tg, out);
        *pp = q;
        return AML_OK;
    }
    case 0x78: {                                      /* Divide(a,b,rem,quot) */
        AML_VAL a, b;
        TARGET tr, tq;
        ULONGLONG x = 0, y = 0;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        r = EvalTermArg(c, T, &q, end, &b);
        if (r != AML_OK) { AmlFree(&a); AmlFree(&b); return r; }
        AmlAsInt(&a, &x); AmlAsInt(&b, &y);
        AmlFree(&a); AmlFree(&b);
        if (y == 0) return AML_FAIL;
        r = ParseTarget(c, T, &q, end, &tr);
        if (r != AML_OK) return r;
        r = ParseTarget(c, T, &q, end, &tq);
        if (r != AML_OK) return r;
        {
            ULONGLONG rm = 0, qo = Div64(x, y, &rm);
            AML_VAL rem;
            AmlSetInt(&rem, rm);
            StoreTo(c, &tr, &rem);
            AmlFree(&rem);
            AmlSetInt(out, qo);
        }
        StoreTo(c, &tq, out);
        *pp = q;
        return AML_OK;
    }
    case 0x80: case 0x81: case 0x82: {                /* Not FindSetLeftBit FindSetRightBit */
        AML_VAL a;
        TARGET tg;
        ULONGLONG x = 0, z = 0;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        AmlAsInt(&a, &x);
        AmlFree(&a);
        if (op == 0x80) z = ~x;
        else if (op == 0x81) { int i; for (i = 63; i >= 0; i--) if (x & ((ULONGLONG)1 << i)) { z = i + 1; break; } }
        else { int i; for (i = 0; i < 64; i++) if (x & ((ULONGLONG)1 << i)) { z = i + 1; break; } }
        r = ParseTarget(c, T, &q, end, &tg);
        if (r != AML_OK) return r;
        AmlSetInt(out, z);
        StoreTo(c, &tg, out);
        *pp = q;
        return AML_OK;
    }
    case 0x90: case 0x91: {                           /* LAnd LOr */
        AML_VAL a, b;
        ULONGLONG x = 0, y = 0;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        r = EvalTermArg(c, T, &q, end, &b);
        if (r != AML_OK) { AmlFree(&a); AmlFree(&b); return r; }
        AmlAsInt(&a, &x); AmlAsInt(&b, &y);
        AmlFree(&a); AmlFree(&b);
        AmlSetInt(out, LTrue(op == 0x90 ? (x && y) : (x || y)));
        *pp = q;
        return AML_OK;
    }
    case 0x92: {                                      /* LNot, or LNotEqual/LLessEqual/LGreaterEqual */
        UCHAR n = T[p + 1];
        if (n == 0x93 || n == 0x94 || n == 0x95) {
            AML_VAL a, b;
            ULONGLONG x = 0, y = 0;
            ULONG q = p + 2;
            r = EvalTermArg(c, T, &q, end, &a);
            if (r != AML_OK) { AmlFree(&a); return r; }
            r = EvalTermArg(c, T, &q, end, &b);
            if (r != AML_OK) { AmlFree(&a); AmlFree(&b); return r; }
            AmlAsInt(&a, &x); AmlAsInt(&b, &y);
            AmlFree(&a); AmlFree(&b);
            AmlSetInt(out, LTrue(n == 0x93 ? (x != y) : n == 0x94 ? (x <= y) : (x >= y)));
            *pp = q;
            return AML_OK;
        } else {
            AML_VAL a;
            ULONGLONG x = 0;
            ULONG q = p + 1;
            r = EvalTermArg(c, T, &q, end, &a);
            if (r != AML_OK) { AmlFree(&a); return r; }
            AmlAsInt(&a, &x);
            AmlFree(&a);
            AmlSetInt(out, LTrue(x == 0));
            *pp = q;
            return AML_OK;
        }
    }
    case 0x93: case 0x94: case 0x95: {                /* LEqual LGreater LLess */
        AML_VAL a, b;
        ULONGLONG x = 0, y = 0;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        r = EvalTermArg(c, T, &q, end, &b);
        if (r != AML_OK) { AmlFree(&a); AmlFree(&b); return r; }
        AmlAsInt(&a, &x); AmlAsInt(&b, &y);
        AmlFree(&a); AmlFree(&b);
        AmlSetInt(out, LTrue(op == 0x93 ? (x == y) : op == 0x94 ? (x > y) : (x < y)));
        *pp = q;
        return AML_OK;
    }
    case 0x87: {                                      /* SizeOf */
        AML_VAL a;
        ULONG q = p + 1;
        ULONGLONG n = 0;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        if (a.Kind == AMLV_BUF || a.Kind == AMLV_STR) n = a.Len;
        else if (a.Kind == AMLV_PKG) n = a.Len;
        AmlFree(&a);
        AmlSetInt(out, n);
        *pp = q;
        return AML_OK;
    }
    case 0x99: case 0x96: {                           /* ToInteger / ToBuffer (store form) */
        AML_VAL a;
        TARGET tg;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &a);
        if (r != AML_OK) { AmlFree(&a); return r; }
        r = ParseTarget(c, T, &q, end, &tg);
        if (r != AML_OK) { AmlFree(&a); return r; }
        if (op == 0x99) {
            ULONGLONG n = 0;
            AmlAsInt(&a, &n);
            AmlFree(&a);
            AmlSetInt(out, n);
        } else {
            *out = a;                                 /* ToBuffer: pass value through */
            UsbMemSet(&a, 0, sizeof(a));
        }
        StoreTo(c, &tg, out);
        *pp = q;
        return AML_OK;
    }
    case 0x88: {                                      /* Index(src, idx [,target]) -> reference/value */
        AML_VAL src, idx;
        TARGET tg;
        ULONG q = p + 1;
        ULONGLONG i = 0;
        r = EvalTermArg(c, T, &q, end, &src);
        if (r != AML_OK) { AmlFree(&src); return r; }
        r = EvalTermArg(c, T, &q, end, &idx);
        if (r != AML_OK) { AmlFree(&src); AmlFree(&idx); return r; }
        AmlAsInt(&idx, &i);
        AmlFree(&idx);
        r = ParseTarget(c, T, &q, end, &tg);
        if (r != AML_OK) { AmlFree(&src); return r; }
        UsbMemSet(out, 0, sizeof(*out));
        if (src.Kind == AMLV_PKG && i < src.Len) {
            CopyVal(out, &src.Pkg[i]);
        } else if ((src.Kind == AMLV_BUF || src.Kind == AMLV_STR) && i < src.Len) {
            AmlSetInt(out, src.Buf[i]);
        } else {
            AmlSetInt(out, 0);
        }
        AmlFree(&src);
        StoreTo(c, &tg, out);
        *pp = q;
        return AML_OK;
    }
    case 0x83: {                                      /* DerefOf */
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, out);
        *pp = q;
        return r;
    }
    case 0x70: {                                      /* Store used as an expression (rare) */
        AML_VAL v;
        TARGET tg;
        ULONG q = p + 1;
        r = EvalTermArg(c, T, &q, end, &v);
        if (r != AML_OK) { AmlFree(&v); return r; }
        r = ParseTarget(c, T, &q, end, &tg);
        if (r != AML_OK) { AmlFree(&v); return r; }
        StoreTo(c, &tg, &v);
        *out = v;
        UsbMemSet(&v, 0, sizeof(v));
        *pp = q;
        return AML_OK;
    }
    case 0x5B: {                                      /* ExtOp term args */
        UCHAR e = T[p + 1];
        if (e == 0x30) { AmlSetInt(out, 2); *pp = p + 2; return AML_OK; }   /* Revision */
        if (e == 0x33) { AmlSetInt(out, OsTimeMs() * 10000ULL); *pp = p + 2; return AML_OK; } /* Timer (100ns) */
        if (e == 0x12) {                              /* CondRefOf(obj, target) */
            ULONG q = p + 2;
            TARGET tg;
            int node;
            char full[AML_PATH];
            ULONG cons = 0;
            BOOLEAN exist;
            node = Resolve(c ? c->Scope : "", T + q, end - q, &cons, full);
            exist = (node >= 0) || (c && (DynFind(c, full, FALSE) || BfFind(c, full)));
            q += cons;
            r = ParseTarget(c, T, &q, end, &tg);
            if (r != AML_OK) return r;
            AmlSetInt(out, LTrue(exist));
            StoreTo(c, &tg, out);
            *pp = q;
            return AML_OK;
        }
        if (e == 0x23 || e == 0x25) {                 /* Acquire(mutex, timeout) / Wait(event, timeout) */
            ULONG q = p + 2;
            AML_VAL to;
            /* skip the SuperName */
            int u, rr, ns; const UCHAR *s;
            ULONG l = NameStr(T + q, end - q, &u, &rr, &s, &ns);
            if (l == 0) {
                if (T[q] >= 0x60 && T[q] <= 0x6E) l = 1; else return AML_FAIL;
            }
            q += l;
            r = EvalTermArg(c, T, &q, end, &to);       /* timeout word */
            AmlFree(&to);
            AmlSetInt(out, 0);                         /* acquired */
            *pp = q;
            return AML_OK;
        }
        return AML_FAIL;
    }
    default: break;
    }
    /* a NameString: data object reference or method call */
    {
        char full[AML_PATH];
        ULONG cons = 0;
        int node;
        int up = 0, root = 0, nseg = 0;
        const UCHAR *segs = NULL;
        ULONG nlen = NameStr(T + p, end - p, &up, &root, &segs, &nseg);
        if (nlen == 0) {
            return AML_FAIL;
        }
        /* predefined names that may not be in the tables */
        if (nseg >= 1) {
            const UCHAR *last = segs + (nseg - 1) * 4;
            if (last[0] == '_' && last[1] == 'O' && last[2] == 'S' && last[3] == 'I') {
                AML_VAL s;
                ULONG q = p + nlen;
                BOOLEAN sup = FALSE;
                r = EvalTermArg(c, T, &q, end, &s);
                if (r == AML_OK && s.Kind == AMLV_STR && s.Buf) {
                    const char *os = (const char *)s.Buf;
                    const char *w[] = { "Windows 2001", "Windows 2001 SP1", "Windows 2001 SP2",
                                        "Windows 2001.1", "Windows 2006", "Windows 2006.1",
                                        "Windows 2006 SP1", "Windows 2009", "Windows 2012",
                                        "Windows 2013", "Windows 2015", "Extended Address Space Descriptor", 0 };
                    int k;
                    for (k = 0; w[k]; k++) {
                        const char *a = w[k], *b = os;
                        while (*a && *a == *b) { a++; b++; }
                        if (*a == 0 && *b == 0) { sup = TRUE; break; }
                    }
                }
                AmlFree(&s);
                AmlSetInt(out, LTrue(sup));
                *pp = q;
                return AML_OK;
            }
            if (last[0] == '_' && last[1] == 'O' && last[2] == 'S' && last[3] == '_') {
                ULONG n;
                static const char os[] = "Microsoft Windows NT";
                for (n = 0; os[n]; n++) {}
                if (!NewBuf(out, n)) return AML_FAIL;
                out->Kind = AMLV_STR;
                UsbMemCpy(out->Buf, os, n + 1);
                *pp = p + nlen;
                return AML_OK;
            }
            if (last[0] == '_' && last[1] == 'R' && last[2] == 'E' && last[3] == 'V') {
                AmlSetInt(out, 2);
                *pp = p + nlen;
                return AML_OK;
            }
        }
        node = Resolve(c ? c->Scope : "", T + p, end - p, &cons, full);
        if (node >= 0 && g_Node[node].Type == NT_METHOD) {
            AML_VAL margs[7];
            int na = g_Node[node].ArgCount, i, rc;
            ULONG q = p + nlen;
            for (i = 0; i < na && i < 7; i++) {
                if (EvalTermArg(c, T, &q, end, &margs[i]) != AML_OK) {
                    int k; for (k = 0; k < i; k++) AmlFree(&margs[k]);
                    return AML_FAIL;
                }
            }
            rc = CallMethod(c, node, margs, na, out);
            for (i = 0; i < na && i < 7; i++) AmlFree(&margs[i]);
            *pp = q;
            return rc;
        }
        /* a data object / field reference */
        *pp = p + nlen;
        if (c) {
            DYN *d = DynFind(c, full, FALSE);
            if (d) return CopyVal(out, &d->Val) ? AML_OK : AML_FAIL;
        }
        return ReadNamed(c, node, full, out);
    }
}

/* execute a statement list (method body); handles If/Else/While/Return/etc */
static int ExecList(CTX *c, const UCHAR *T, ULONG p, ULONG end)
{
    while (p < end && !c->Returned && !c->Broke) {
        UCHAR op = T[p];
        ULONG h, plen;
        if (op == 0xA4) {                             /* Return(arg) */
            ULONG q = p + 1;
            AmlFree(&c->Ret);
            if (q < end && T[q] != 0 && !(T[q] == 0x00)) {
                if (EvalTermArg(c, T, &q, end, &c->Ret) != AML_OK) {
                    return AML_FAIL;
                }
            } else {
                AmlSetInt(&c->Ret, 0);
                if (q < end && T[q] == 0x00) q++;
            }
            c->Returned = TRUE;
            return AML_OK;
        }
        if (op == 0xA0) {                             /* If(pred) { } [Else] */
            AML_VAL pred;
            ULONG q;
            ULONGLONG pv = 0;
            int r;
            plen = PkgLen(T + p + 1, end - p - 1, &h);
            if (plen == 0 || p + 1 + plen > end) return AML_FAIL;
            q = p + 1 + h;
            r = EvalTermArg(c, T, &q, p + 1 + plen, &pred);
            if (r != AML_OK) { AmlFree(&pred); return r; }
            AmlAsInt(&pred, &pv);
            AmlFree(&pred);
            if (pv) {
                r = ExecList(c, T, q, p + 1 + plen);
                if (r != AML_OK) return r;
                p = p + 1 + plen;
                if (p < end && T[p] == 0xA1) {        /* skip the Else */
                    ULONG h2, pl2 = PkgLen(T + p + 1, end - p - 1, &h2);
                    p = p + 1 + pl2;
                }
            } else {
                p = p + 1 + plen;
                if (p < end && T[p] == 0xA1) {
                    ULONG h2 = 1, pl2 = PkgLen(T + p + 1, end - p - 1, &h2);
                    r = ExecList(c, T, p + 1 + h2, p + 1 + pl2);
                    if (r != AML_OK) return r;
                    p = p + 1 + pl2;
                }
            }
            continue;
        }
        if (op == 0xA1) {                             /* stray Else: skip */
            plen = PkgLen(T + p + 1, end - p - 1, &h);
            p = p + 1 + plen;
            continue;
        }
        if (op == 0xA2) {                             /* While(pred) { } */
            ULONG iter = 0;
            plen = PkgLen(T + p + 1, end - p - 1, &h);
            if (plen == 0 || p + 1 + plen > end) return AML_FAIL;
            for (;;) {
                AML_VAL pred;
                ULONG q = p + 1 + h;
                ULONGLONG pv = 0;
                int r = EvalTermArg(c, T, &q, p + 1 + plen, &pred);
                if (r != AML_OK) { AmlFree(&pred); return r; }
                AmlAsInt(&pred, &pv);
                AmlFree(&pred);
                if (!pv) break;
                r = ExecList(c, T, q, p + 1 + plen);
                if (r != AML_OK) return r;
                if (c->Returned) return AML_OK;
                if (c->Broke) { c->Broke = FALSE; break; }
                if (++iter > AML_WHILE_MAX) {
                    UsbLog(LOG_DBG, "aml: while loop limit\n");
                    return AML_FAIL;
                }
            }
            p = p + 1 + plen;
            continue;
        }
        if (op == 0xA3) { p++; continue; }            /* Noop */
        if (op == 0xA5) { c->Broke = TRUE; p++; return AML_OK; }  /* Break */
        if (op == 0xCC) { p++; continue; }            /* BreakPoint */
        if (op == 0x70) {                             /* Store(src, dst) */
            AML_VAL v;
            TARGET tg;
            ULONG q = p + 1;
            int r = EvalTermArg(c, T, &q, end, &v);
            if (r != AML_OK) { AmlFree(&v); return r; }
            r = ParseTarget(c, T, &q, end, &tg);
            if (r != AML_OK) { AmlFree(&v); return r; }
            StoreTo(c, &tg, &v);
            AmlFree(&v);
            p = q;
            continue;
        }
        if (op == 0x08) {                             /* Name(name, obj) - local */
            char full[AML_PATH];
            ULONG cons = 0, q;
            DYN *d;
            if (!BuildPath(c->Scope, T + p + 1, end - p - 1, full, &cons)) return AML_FAIL;
            q = p + 1 + cons;
            d = DynFind(c, full, TRUE);
            if (d) {
                AmlFree(&d->Val);
                if (EvalTermArg(c, T, &q, end, &d->Val) != AML_OK) {
                    AmlSetInt(&d->Val, 0);
                    q = p + 1 + cons + SkipObj(T, p + 1 + cons, end);
                }
            } else {
                q = p + 1 + cons + SkipObj(T, p + 1 + cons, end);
            }
            p = q;
            continue;
        }
        if (op == 0x75 || op == 0x76) {               /* Increment / Decrement (SuperName) */
            TARGET tg;
            ULONG q = p + 1;
            AML_VAL cur;
            ULONGLONG v = 0;
            int r;
            ULONG savep = q;
            r = EvalTermArg(c, T, &savep, end, &cur);  /* read current */
            if (r == AML_OK) AmlAsInt(&cur, &v);
            AmlFree(&cur);
            r = ParseTarget(c, T, &q, end, &tg);
            if (r != AML_OK) return r;
            v = (op == 0x75) ? v + 1 : v - 1;
            { AML_VAL nv; AmlSetInt(&nv, v); StoreTo(c, &tg, &nv); AmlFree(&nv); }
            p = q;
            continue;
        }
        if (op == 0x8A || op == 0x8B || op == 0x8C || op == 0x8D || op == 0x8F) {
            /* CreateDWord/Word/Byte/BitField/QWordField(buf, index, name) */
            AML_VAL bufv, idxv;
            ULONG q = p + 1;
            char full[AML_PATH];
            ULONG cons = 0;
            BUFFLD *b;
            ULONGLONG bitidx = 0;
            int ownerIdx = -1, i, r;
            UCHAR saveKind = op;
            char srcpath[AML_PATH];
            /* the source buffer must be a dynamic Name we can patch */
            {
                int u, rt, ns; const UCHAR *s;
                ULONG l = NameStr(T + q, end - q, &u, &rt, &s, &ns);
                if (l && BuildPath(c->Scope, T + q, end - q, srcpath, &cons)) {
                    DYN *d = DynFind(c, srcpath, FALSE);
                    if (d) {
                        for (i = 0; i < AML_MAX_DYN; i++) if (&c->Dyn[i] == d) { ownerIdx = i; break; }
                    }
                    q += l;
                } else {
                    r = EvalTermArg(c, T, &q, end, &bufv);   /* non-name source: unsupported for patching */
                    if (r == AML_OK) AmlFree(&bufv);
                    ownerIdx = -1;
                }
            }
            r = EvalTermArg(c, T, &q, end, &idxv);
            if (r != AML_OK) { AmlFree(&idxv); return r; }
            AmlAsInt(&idxv, &bitidx);
            AmlFree(&idxv);
            if (saveKind == 0x8D) {                   /* CreateBitField: index in bits, then a count? no - name */
                /* CreateBitField(buf, bitindex, name) width 1 */
            } else if (saveKind == 0x8C) {
                bitidx *= 8;                          /* CreateByteField: index in bytes */
            } else if (saveKind == 0x8B) {
                bitidx *= 8;
            } else if (saveKind == 0x8A) {
                bitidx *= 8;
            } else if (saveKind == 0x8F) {
                bitidx *= 8;
            }
            if (!BuildPath(c->Scope, T + q, end - q, full, &cons)) return AML_FAIL;
            q += cons;
            b = BfFind(c, full);
            if (!b) b = BfNew(c, full);
            if (b) {
                b->Owner = ownerIdx;
                b->BitOff = (ULONG)bitidx;
                b->BitLen = saveKind == 0x8D ? 1 : saveKind == 0x8C ? 8 : saveKind == 0x8B ? 16 :
                            saveKind == 0x8A ? 32 : 64;
            }
            p = q;
            continue;
        }
        if (op == 0x86) {                             /* Notify(obj, value): ignored */
            ULONG q = p + 1;
            AML_VAL a, b;
            int u, rt, ns; const UCHAR *s;
            ULONG l = NameStr(T + q, end - q, &u, &rt, &s, &ns);
            if (l) q += l; else { if (EvalTermArg(c, T, &q, end, &a) == AML_OK) AmlFree(&a); }
            if (EvalTermArg(c, T, &q, end, &b) == AML_OK) AmlFree(&b);
            p = q;
            continue;
        }
        if (op == 0x5B) {                             /* ExtOp statements */
            UCHAR e = T[p + 1];
            if (e == 0x21) {                          /* Stall(usec) */
                ULONG q = p + 2;
                AML_VAL v; ULONGLONG us = 0;
                if (EvalTermArg(c, T, &q, end, &v) == AML_OK) AmlAsInt(&v, &us);
                AmlFree(&v);
                OsStallUs((ULONG)(us > 1000 ? 1000 : us));
                p = q;
                continue;
            }
            if (e == 0x22) {                          /* Sleep(ms) */
                ULONG q = p + 2;
                AML_VAL v; ULONGLONG ms = 0;
                if (EvalTermArg(c, T, &q, end, &v) == AML_OK) AmlAsInt(&v, &ms);
                AmlFree(&v);
                OsSleepMs((ULONG)(ms > 200 ? 200 : ms));
                p = q;
                continue;
            }
            if (e == 0x27 || e == 0x24 || e == 0x26) {/* Release / Signal / Reset: no-op (skip SuperName) */
                ULONG q = p + 2;
                int u, rt, ns; const UCHAR *s;
                ULONG l = NameStr(T + q, end - q, &u, &rt, &s, &ns);
                if (l) q += l; else q += 1;
                p = q;
                continue;
            }
            if (e == 0x32) {                          /* Fatal: give up */
                return AML_FAIL;
            }
            if (e == 0x80 || e == 0x81 || e == 0x86 || e == 0x87 || e == 0x01 || e == 0x02) {
                /* OperationRegion / Field / IndexField / BankField / Mutex / Event declared
                   inside a method: already in the static namespace from the build pass; skip. */
                if (e == 0x80) {                      /* OperationRegion: name space off len */
                    ULONG q = p + 2;
                    int u, rt, ns; const UCHAR *s;
                    ULONG l = NameStr(T + q, end - q, &u, &rt, &s, &ns);
                    AML_VAL v;
                    q += l + 1;                       /* name + space byte */
                    if (EvalTermArg(c, T, &q, end, &v) == AML_OK) AmlFree(&v);
                    if (EvalTermArg(c, T, &q, end, &v) == AML_OK) AmlFree(&v);
                    p = q;
                    continue;
                }
                plen = PkgLen(T + p + 2, end - p - 2, &h);
                if (e == 0x01 || e == 0x02) {         /* Mutex/Event: not package-prefixed */
                    ULONG q = p + 2;
                    int u, rt, ns; const UCHAR *s;
                    ULONG l = NameStr(T + q, end - q, &u, &rt, &s, &ns);
                    q += l + (e == 0x01 ? 1 : 0);
                    p = q;
                    continue;
                }
                p = p + 2 + plen;
                continue;
            }
            /* other ext statement (Acquire as statement etc.): evaluate as expr */
            {
                ULONG q = p;
                AML_VAL v;
                int r = EvalTermArg(c, T, &q, end, &v);
                AmlFree(&v);
                if (r != AML_OK) return r;
                p = q;
                continue;
            }
        }
        /* method call / expression statement: evaluate and drop the value */
        {
            ULONG q = p;
            AML_VAL v;
            int r = EvalTermArg(c, T, &q, end, &v);
            AmlFree(&v);
            if (r != AML_OK) {
                return r;
            }
            if (q <= p) {
                p++;
            } else {
                p = q;
            }
        }
    }
    return AML_OK;
}

static int CallMethod(CTX *parent, int node, const AML_VAL *args, int nargs, AML_VAL *out)
{
    CTX *c;
    int i, r;
    if (parent && parent->Depth >= AML_DEPTH) {
        return AML_FAIL;
    }
    c = (CTX *)OsAlloc(sizeof(CTX));
    if (c == NULL) {
        return AML_FAIL;
    }
    c->Depth = parent ? parent->Depth + 1 : 0;
    UsbMemCpy(c->Scope, g_Node[node].Path, AML_PATH);  /* local names live in the method's scope */
    for (i = 0; i < nargs && i < 7; i++) {
        CopyVal(&c->Arg[i], &args[i]);
    }
    AmlSetInt(&c->Ret, 0);
    r = ExecList(c, g_Node[node].T, g_Node[node].A, g_Node[node].B);
    if (r == AML_OK) {
        if (!CopyVal(out, &c->Ret)) {
            r = AML_FAIL;
        }
    }
    for (i = 0; i < 8; i++) AmlFree(&c->Local[i]);
    for (i = 0; i < 7; i++) AmlFree(&c->Arg[i]);
    AmlFree(&c->Ret);
    for (i = 0; i < AML_MAX_DYN; i++) if (c->Dyn[i].Used) AmlFree(&c->Dyn[i].Val);
    OsFree(c);
    return r;
}

/* ------------------------------------------------------------------ */
/* public entry points                                                  */
/* ------------------------------------------------------------------ */
int AmlEval(const char *Path, const AML_VAL *Args, int NArgs, AML_VAL *Out)
{
    int node;
    UsbMemSet(Out, 0, sizeof(*Out));
    if (AmlInit() == 0 && g_Off) {
        return AML_FAIL;
    }
    node = FindNode(Path[0] == '\\' ? Path + 1 : Path);
    if (node < 0) {
        return AML_NOTFOUND;
    }
    while (g_Node[node].Type == NT_ALIAS && g_Node[node].Target >= 0) {
        node = g_Node[node].Target;
    }
    if (g_Node[node].Type == NT_METHOD) {
        return CallMethod(NULL, node, Args, NArgs, Out);
    }
    /* a data object or field */
    if (g_Node[node].Type == NT_NAME) {
        ULONG pp = g_Node[node].A;
        return EvalTermArg(NULL, g_Node[node].T, &pp, g_Node[node].A + 4096, Out);
    }
    if (g_Node[node].Type == NT_FIELD) {
        return ReadField(node, Out);
    }
    return AML_FAIL;
}

int AmlEvalMethod(const char *DevPath, const char *Name, const AML_VAL *Args, int NArgs, AML_VAL *Out)
{
    char full[AML_PATH];
    ULONG n = 0;
    const char *d = (DevPath[0] == '\\') ? DevPath + 1 : DevPath;
    while (d[n] && n + 6 < AML_PATH) { full[n] = d[n]; n++; }
    full[n++] = '.';
    full[n++] = Name[0]; full[n++] = Name[1]; full[n++] = Name[2]; full[n++] = Name[3];
    full[n] = 0;
    return AmlEval(full, Args, NArgs, Out);
}

int AmlHasObject(const char *DevPath, const char *Name)
{
    char full[AML_PATH];
    ULONG n = 0;
    const char *d = (DevPath[0] == '\\') ? DevPath + 1 : DevPath;
    AmlInit();
    while (d[n] && n + 6 < AML_PATH) { full[n] = d[n]; n++; }
    full[n++] = '.';
    full[n++] = Name[0]; full[n++] = Name[1]; full[n++] = Name[2]; full[n++] = Name[3];
    full[n] = 0;
    return FindNode(full) >= 0;
}

/* ------------------------------------------------------------------ */
/* device walk and _HID / _CID text                                     */
/* ------------------------------------------------------------------ */
static void EisaStr(const UCHAR *b, char *out)
{
    USHORT v = (USHORT)((b[0] << 8) | b[1]);
    static const char hex[] = "0123456789ABCDEF";
    out[0] = (char)('@' + ((v >> 10) & 0x1F));
    out[1] = (char)('@' + ((v >> 5) & 0x1F));
    out[2] = (char)('@' + (v & 0x1F));
    out[3] = hex[b[2] >> 4];
    out[4] = hex[b[2] & 15];
    out[5] = hex[b[3] >> 4];
    out[6] = hex[b[3] & 15];
    out[7] = 0;
}

static void IdText(int node, char *out, ULONG size)
{
    out[0] = 0;
    if (node < 0 || g_Node[node].Type != NT_NAME) {
        return;
    }
    {
        const UCHAR *T = g_Node[node].T;
        ULONG at = g_Node[node].A, i;
        if (T[at] == 0x0D) {
            for (i = 0; i + 1 < size && T[at + 1 + i] >= 0x20 && T[at + 1 + i] < 0x7F; i++) {
                out[i] = (char)T[at + 1 + i];
            }
            out[i] = 0;
        } else if (T[at] == 0x0C && size >= 8) {
            EisaStr(T + at + 1, out);
        } else if (T[at] == 0x12) {                   /* Package of CIDs: take the first string/eisa */
            ULONG h, plen = PkgLen(T + at + 1, 256, &h), q = at + 1 + h + 1;
            if (T[q] == 0x0D) {
                for (i = 0; i + 1 < size && T[q + 1 + i] >= 0x20 && T[q + 1 + i] < 0x7F; i++) out[i] = (char)T[q + 1 + i];
                out[i] = 0;
            } else if (T[q] == 0x0C && size >= 8) {
                EisaStr(T + q + 1, out);
            }
            (void)plen;
        }
    }
}

void AmlForEachDevice(AML_DEV_FN Fn, void *Ctx)
{
    int i;
    AmlInit();
    for (i = 0; i < g_NNode; i++) {
        char hp[AML_PATH], cp[AML_PATH], hid[16], cid[16];
        if (g_Node[i].Type != NT_SCOPE) {
            continue;
        }
        UsbMemCpy(hp, g_Node[i].Path, AML_PATH);
        PathAddSeg(hp, (const UCHAR *)"_HID");
        UsbMemCpy(cp, g_Node[i].Path, AML_PATH);
        PathAddSeg(cp, (const UCHAR *)"_CID");
        hid[0] = cid[0] = 0;
        IdText(FindNode(hp), hid, sizeof(hid));
        IdText(FindNode(cp), cid, sizeof(cid));
        if (hid[0] == 0 && cid[0] == 0) {
            continue;                                 /* only devices with an identifier */
        }
        if (!Fn(g_Node[i].Path, hid, cid, Ctx)) {
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* ResourceTemplate parsing (_CRS result buffers and Field connections) */
/* ------------------------------------------------------------------ */
static void SrcLast(const UCHAR *T, ULONG s, ULONG end, char *full, char *last4)
{
    ULONG seg = s, k, n, m;
    full[0] = last4[0] = 0;
    for (k = s; k < end && T[k]; k++) {}
    n = k - s;
    if (n == 0 || n >= 32) {
        return;
    }
    for (m = 0; m < n && m + 1 < 32; m++) {
        full[m] = (char)T[s + m];
    }
    full[m] = 0;
    /* last segment */
    seg = s;
    for (k = s; k < end && T[k]; k++) {
        if (T[k] == '.' || T[k] == '\\' || T[k] == '^') {
            seg = k + 1;
        }
    }
    n = k - seg;
    if (n >= 1 && n <= 4) {
        for (m = 0; m < 4; m++) {
            last4[m] = (char)(m < n ? T[seg + m] : '_');
        }
        last4[4] = 0;
    }
}

BOOLEAN AmlGlobalInt(const char *Seg4, ULONGLONG *Val)
{
    int i;
    AmlInit();
    for (i = 0; i < g_NNode; i++) {
        const char *path = g_Node[i].Path;
        int n = 0, last = 0;
        if (g_Node[i].Type != NT_NAME) {
            continue;
        }
        while (path[n]) {
            if (path[n] == '.') {
                last = n + 1;
            }
            n++;
        }
        if (n - last == 4 && path[last] == Seg4[0] && path[last + 1] == Seg4[1] &&
            path[last + 2] == Seg4[2] && path[last + 3] == Seg4[3]) {
            AML_VAL v;
            ULONG pp = g_Node[i].A;
            if (EvalTermArg(NULL, g_Node[i].T, &pp, g_Node[i].A + 16, &v) == AML_OK && AmlAsInt(&v, Val)) {
                AmlFree(&v);
                return TRUE;
            }
            AmlFree(&v);
        }
    }
    return FALSE;
}

BOOLEAN AmlGpioController(const char *Name4, char *HidOut, ULONG HidSize, ULONG *Uid, AML_CRS *Crs)
{
    int i;
    AmlInit();
    if (HidOut) {
        HidOut[0] = 0;
    }
    if (Uid) {
        *Uid = 0;
    }
    UsbMemSet(Crs, 0, sizeof(*Crs));
    for (i = 0; i < g_NNode; i++) {
        const char *path = g_Node[i].Path;
        int n = 0, last = 0;
        char hp[AML_PATH], hid[16];
        if (g_Node[i].Type != NT_SCOPE) {
            continue;
        }
        while (path[n]) {
            if (path[n] == '.') {
                last = n + 1;
            }
            n++;
        }
        if (!(n - last == 4 && path[last] == Name4[0] && path[last + 1] == Name4[1] &&
              path[last + 2] == Name4[2] && path[last + 3] == Name4[3])) {
            continue;
        }
        UsbMemCpy(hp, path, AML_PATH);
        PathAddSeg(hp, (const UCHAR *)"_HID");
        IdText(FindNode(hp), hid, sizeof(hid));
        if (hid[0] == 0) {
            continue;                                 /* must be a real controller device */
        }
        if (HidOut) {
            ULONG k = 0;
            while (hid[k] && k + 1 < HidSize) { HidOut[k] = hid[k]; k++; }
            HidOut[k] = 0;
        }
        {
            AML_VAL v;
            ULONGLONG u = 0;
            if (AmlEvalMethod(path, "_UID", NULL, 0, &v) == AML_OK && AmlAsInt(&v, &u) && Uid) {
                *Uid = (ULONG)u;
            }
            AmlFree(&v);
            if (AmlEvalMethod(path, "_CRS", NULL, 0, &v) == AML_OK) {
                AmlParseCrs(&v, Crs);
            }
            AmlFree(&v);
        }
        return TRUE;
    }
    return FALSE;
}

void AmlParseCrs(const AML_VAL *Buf, AML_CRS *Crs)
{
    const UCHAR *T;
    ULONG len, i;
    UsbMemSet(Crs, 0, sizeof(*Crs));
    if (Buf == NULL || (Buf->Kind != AMLV_BUF && Buf->Kind != AMLV_STR) || Buf->Buf == NULL) {
        return;
    }
    T = Buf->Buf;
    len = Buf->Len;
    for (i = 0; i + 2 < len;) {
        UCHAR tag = T[i];
        if (tag & 0x80) {                             /* large resource */
            ULONG dl = (i + 3 <= len) ? G16(T + i + 1) : 0;
            if (i + 3 + dl > len) {
                break;
            }
            if (tag == 0x8E && (T[i + 3] == 1 || T[i + 3] == 2) && T[i + 5] == 1 && Crs->NI2c < 4) {
                ULONG tl = G16(T + i + 10);
                AML_I2C_RES *c = &Crs->I2c[Crs->NI2c++];
                c->Addr = (USHORT)(G16(T + i + 16) & 0x3FF);
                c->Speed = G32(T + i + 12);
                SrcLast(T, i + 12 + tl, i + 3 + dl, c->Ctrl, c->Ctrl4);
            } else if (tag == 0x8C && T[i + 3] <= 2 && Crs->NGpio < 8) {   /* GpioInt / GpioIo */
                ULONG pinoff = G16(T + i + 14);
                ULONG srcoff = G16(T + i + 17);
                AML_GPIO_RES *g = &Crs->Gpio[Crs->NGpio++];
                if (pinoff >= 7 && i + pinoff + 2 <= i + 3 + dl) {
                    g->Pin = (USHORT)G16(T + i + pinoff);
                }
                g->IsInt = (UCHAR)(T[i + 4] == 0);    /* 0 = interrupt connection, 1 = IO */
                /* flags at +7 (word): bit0 edge/level for int; polarity bits 1-2 */
                {
                    USHORT fl = (USHORT)G16(T + i + 7);
                    if (g->IsInt) {
                        g->Level = (UCHAR)(!(fl & 1));            /* 0 edge, 1 level (HID over I2C: level) */
                        g->ActiveLow = (UCHAR)(((fl >> 1) & 3) == 1);
                        g->Shared = (UCHAR)((T[i + 9]) & 1);
                    } else {
                        g->ActiveLow = 0;
                    }
                }
                if (srcoff && i + srcoff < i + 3 + dl) {
                    SrcLast(T, i + srcoff, i + 3 + dl, g->Ctrl, g->Ctrl4);
                }
            } else if (tag == 0x86 && Crs->NMem < 8 && i + 11 <= len) {   /* Memory32Fixed */
                Crs->MemBase[Crs->NMem] = G32(T + i + 4);
                Crs->MemLen[Crs->NMem] = G32(T + i + 8);
                if (Crs->MemBase[Crs->NMem]) {
                    Crs->NMem++;
                }
            }
            i += 3 + dl;
        } else {                                      /* small resource */
            ULONG sl = tag & 0x07;
            if ((tag >> 3) == 0x0F) {                 /* EndTag */
                break;
            }
            i += 1 + sl;
        }
    }
}
