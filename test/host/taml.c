/*
 * taml.c - host test of the AML interpreter (src/aml.c) and the GPIO resolver
 * (src/gpio.c) against hand-built ACPI tables and a fake MMIO / IO space.
 * No iasl or QEMU needed: the AML is assembled here (the few opcodes the test
 * exercises), the firmware memory the methods touch is a plain array.
 *
 *   gcc -m32 -w -DUSB_HARNESS -I../../src taml.c ../../src/aml.c ../../src/gpio.c \
 *       ../../src/acpi.c -o /tmp/taml && /tmp/taml
 *
 * Covered: _STA (present / absent), conditional _CRS (returns one of two
 * ResourceTemplates by a firmware "touch pad type" field), _DSM returning the
 * descriptor register from a Package, _PS0 writing a SystemMemory field, a
 * While loop with a method call and arithmetic, and the GPIO resolver /
 * set / get against a fake pinctrl controller.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usbnt.h"
#include "aml.h"
#include "gpio.h"
#include "acpi.h"

/* ------------------------------------------------------------------ */
/* OS stubs                                                             */
/* ------------------------------------------------------------------ */
void  UsbMemSet(void *d, int v, ULONG n) { memset(d, v, n); }
void  UsbMemCpy(void *d, const void *s, ULONG n) { memcpy(d, s, n); }
int   UsbMemCmp(const void *a, const void *b, ULONG n) { return memcmp(a, b, n); }
void *OsAlloc(ULONG n) { void *p = malloc(n ? n : 1); if (p) memset(p, 0, n ? n : 1); return p; }
void  OsFree(void *p) { free(p); }
void  OsStallUs(ULONG u) { (void)u; }
void  OsSleepMs(ULONG m) { (void)m; }
ULONG OsTimeMs(void) { return 0; }
ULONG UsbDebugLevel = 0;

int verbose;
void UsbLog(ULONG lvl, const char *fmt, ...)
{
    va_list ap;
    if (!verbose && lvl > LOG_INFO) return;
    if (!verbose) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

/* fake physical memory window [BASE, BASE+SIZE) */
#define FBASE 0x50000000UL
#define FSIZE 0x10000
static unsigned char g_mem[FSIZE];
void *OsMapMmio(UCHAR bus, ULONG phys, ULONG len)
{
    (void)bus; (void)len;
    if (phys >= FBASE && phys < FBASE + FSIZE) return &g_mem[phys - FBASE];
    return NULL;
}
void OsUnmapMmio(void *va, ULONG len) { (void)va; (void)len; }

static unsigned char g_io[0x10000];
UCHAR  OsIn8(ULONG p) { return g_io[p & 0xFFFF]; }
USHORT OsIn16(ULONG p) { return g_io[p & 0xFFFF] | (g_io[(p + 1) & 0xFFFF] << 8); }
ULONG  OsIn32(ULONG p) { return OsIn16(p) | ((ULONG)OsIn16(p + 2) << 16); }
void   OsOut8(ULONG p, UCHAR v) { g_io[p & 0xFFFF] = v; }
void   OsOut16(ULONG p, USHORT v) { g_io[p & 0xFFFF] = (UCHAR)v; g_io[(p + 1) & 0xFFFF] = (UCHAR)(v >> 8); }
void   OsOut32(ULONG p, ULONG v) { OsOut16(p, (USHORT)v); OsOut16(p + 2, (USHORT)(v >> 16)); }
ULONG  OsPciRead(UCHAR b, UCHAR d, UCHAR f, ULONG o, int s) { (void)b;(void)d;(void)f;(void)o;(void)s; return 0xFFFFFFFF; }
void   OsPciWrite(UCHAR b, UCHAR d, UCHAR f, ULONG o, ULONG v, int s) { (void)b;(void)d;(void)f;(void)o;(void)v;(void)s; }

static ULONG g_aml = 1, g_gpio = 1;
ULONG OsGetConfig(const char *name, ULONG def)
{
    if (!strcmp(name, "I2cHidAml")) return g_aml;
    if (!strcmp(name, "I2cHidGpio")) return g_gpio;
    return def;
}

/* ------------------------------------------------------------------ */
/* ACPI table hook (one hand-built SSDT)                                */
/* ------------------------------------------------------------------ */
static ACPI_TABLE g_table;
static int g_have;
int AcpiInit(void) { return g_have; }
const ACPI_TABLE *AcpiTable(int i) { return (i == 0 && g_have) ? &g_table : NULL; }

/* ------------------------------------------------------------------ */
/* tiny AML assembler                                                   */
/* ------------------------------------------------------------------ */
typedef struct { unsigned char d[4096]; int n; } Buf;
static void a1(Buf *b, int v) { b->d[b->n++] = (unsigned char)v; }
static void ad(Buf *b, const void *p, int n) { memcpy(b->d + b->n, p, n); b->n += n; }
static void astr(Buf *b, const char *s) { ad(b, s, (int)strlen(s) + 1); }   /* incl NUL */
static void aname(Buf *b, const char *s) { ad(b, s, 4); }

static int enc_pkglen(unsigned char *o, int content)
{
    int k, v;
    for (k = 1; k <= 4; k++) {
        v = content + k;
        if (k == 1) { if (v < 64) { o[0] = v; return 1; } continue; }
        if (v < (1 << (4 + 8 * (k - 1)))) {
            o[0] = ((k - 1) << 6) | (v & 0x0F);
            v >>= 4;
            { int i; for (i = 1; i < k; i++) { o[i] = v & 0xFF; v >>= 8; } }
            return k;
        }
    }
    return 0;
}

/* op + pkglen(body) + body */
static void wrap1(Buf *out, int op, Buf *body)
{
    unsigned char pl[4];
    int k = enc_pkglen(pl, body->n);
    a1(out, op);
    ad(out, pl, k);
    ad(out, body->d, body->n);
}
static void wrap2(Buf *out, int op0, int op1, Buf *body)
{
    unsigned char pl[4];
    int k = enc_pkglen(pl, body->n);
    a1(out, op0); a1(out, op1);
    ad(out, pl, k);
    ad(out, body->d, body->n);
}

static void aint(Buf *b, unsigned v)
{
    if (v == 0) a1(b, 0x00);
    else if (v == 1) a1(b, 0x01);
    else if (v <= 0xFF) { a1(b, 0x0A); a1(b, v); }
    else if (v <= 0xFFFF) { a1(b, 0x0B); a1(b, v); a1(b, v >> 8); }
    else { a1(b, 0x0C); a1(b, v); a1(b, v >> 8); a1(b, v >> 16); a1(b, v >> 24); }
}

/* Name(seg, <already-encoded object in ob>) */
static void o_name(Buf *b, const char *seg, Buf *ob)
{
    a1(b, 0x08); aname(b, seg); ad(b, ob->d, ob->n);
}

/* I2cSerialBus descriptor into b (source = a path string) */
static void d_i2c(Buf *b, unsigned addr, unsigned speed, const char *src)
{
    Buf d; d.n = 0;
    a1(&d, 2); a1(&d, 0); a1(&d, 1); a1(&d, 0x02); a1(&d, 0); a1(&d, 0); a1(&d, 1);
    a1(&d, 6); a1(&d, 0);                              /* type data length = 6 */
    a1(&d, speed); a1(&d, speed >> 8); a1(&d, speed >> 16); a1(&d, speed >> 24);
    a1(&d, addr); a1(&d, addr >> 8);
    astr(&d, src);
    a1(b, 0x8E); a1(b, d.n & 0xFF); a1(b, d.n >> 8); ad(b, d.d, d.n);
}

/* Memory32Fixed descriptor */
static void d_mem(Buf *b, unsigned base, unsigned len)
{
    a1(b, 0x86); a1(b, 9); a1(b, 0);                   /* tag, len=9 */
    a1(b, 1);                                          /* writeable */
    a1(b, base); a1(b, base >> 8); a1(b, base >> 16); a1(b, base >> 24);
    a1(b, len); a1(b, len >> 8); a1(b, len >> 16); a1(b, len >> 24);
}

/* a ResourceTemplate Buffer object containing descriptors already in rt */
static void o_resbuf(Buf *b, Buf *rt)
{
    Buf body; body.n = 0;
    Buf withend; withend.n = 0;
    ad(&withend, rt->d, rt->n);
    a1(&withend, 0x79); a1(&withend, 0x00);            /* EndTag */
    aint(&body, withend.n);                            /* buffer size */
    ad(&body, withend.d, withend.n);
    wrap1(b, 0x11, &body);                             /* Buffer */
}

/* ------------------------------------------------------------------ */
/* the test SSDT                                                        */
/* ------------------------------------------------------------------ */
static void build_table(void)
{
    Buf sb; sb.n = 0;        /* \_SB body */
    /* OperationRegion(NVSR, SystemMemory, 0x50002000, 0x10) */
    { Buf o; o.n = 0; a1(&o, 0x5B); a1(&o, 0x80); aname(&o, "NVSR"); a1(&o, 0x00);
      aint(&o, 0x50002000); aint(&o, 0x10); ad(&sb, o.d, o.n); }
    /* Field(NVSR, ByteAcc){ TPTY, 8 } */
    { Buf f; f.n = 0; aname(&f, "NVSR"); a1(&f, 0x01); aname(&f, "TPTY"); a1(&f, 8);
      wrap2(&sb, 0x5B, 0x81, &f); }
    /* OperationRegion(PWRR, SystemMemory, 0x50003000, 4) */
    { Buf o; o.n = 0; a1(&o, 0x5B); a1(&o, 0x80); aname(&o, "PWRR"); a1(&o, 0x00);
      aint(&o, 0x50003000); aint(&o, 4); ad(&sb, o.d, o.n); }
    /* Field(PWRR, DWordAcc){ PWST, 32 } */
    { Buf f; f.n = 0; aname(&f, "PWRR"); a1(&f, 0x03); aname(&f, "PWST"); a1(&f, 32);
      wrap2(&sb, 0x5B, 0x81, &f); }
    /* Name(RTA, ResourceTemplate(){ I2cSerialBus(0x2C,...) }) */
    { Buf rt; rt.n = 0; Buf ob; ob.n = 0; d_i2c(&rt, 0x2C, 400000, "\\_SB.I2C0");
      o_resbuf(&ob, &rt); o_name(&sb, "RTA_", &ob); }
    /* Name(RTB, ResourceTemplate(){ I2cSerialBus(0x15,...) }) */
    { Buf rt; rt.n = 0; Buf ob; ob.n = 0; d_i2c(&rt, 0x15, 100000, "\\_SB.I2C0");
      o_resbuf(&ob, &rt); o_name(&sb, "RTB_", &ob); }

    /* Device(TPD0) */
    { Buf dev; dev.n = 0;
      aname(&dev, "TPD0");
      /* Name(_HID, "PNP0C50") */
      { Buf o; o.n = 0; a1(&o, 0x0D); astr(&o, "PNP0C50"); o_name(&dev, "_HID", &o); }
      /* Method(_STA){ Return(0x0F) } */
      { Buf m; m.n = 0; aname(&m, "_STA"); a1(&m, 0); a1(&m, 0xA4); aint(&m, 0x0F);
        wrap1(&dev, 0x14, &m); }
      /* Method(_CRS){ If(LEqual(TPTY,1)){Return(RTA)} Return(RTB) } */
      { Buf m; m.n = 0; aname(&m, "_CRS"); a1(&m, 0);
        { Buf ifb; ifb.n = 0;
          a1(&ifb, 0x93); aname(&ifb, "TPTY"); aint(&ifb, 1);     /* LEqual(TPTY,1) */
          a1(&ifb, 0xA4); aname(&ifb, "RTA_");                    /* Return(RTA) */
          wrap1(&m, 0xA0, &ifb); }
        a1(&m, 0xA4); aname(&m, "RTB_");                          /* Return(RTB) */
        wrap1(&dev, 0x14, &m); }
      /* Method(_DSM, 4){ Return(Package(){0x0020, 0}) } */
      { Buf m; m.n = 0; aname(&m, "_DSM"); a1(&m, 4);
        a1(&m, 0xA4);
        { Buf pk; pk.n = 0; a1(&pk, 2); aint(&pk, 0x0020); aint(&pk, 0);
          wrap1(&m, 0x12, &pk); }
        wrap1(&dev, 0x14, &m); }
      /* Method(_PS0){ Store(0xABCD, PWST) } */
      { Buf m; m.n = 0; aname(&m, "_PS0"); a1(&m, 0);
        a1(&m, 0x70); aint(&m, 0xABCD); aname(&m, "PWST");
        wrap1(&dev, 0x14, &m); }
      /* Method(CALC, 1){ Return(Add(Arg0, One)) } */
      { Buf m; m.n = 0; aname(&m, "CALC"); a1(&m, 1);
        a1(&m, 0xA4); a1(&m, 0x72); a1(&m, 0x68); a1(&m, 0x01); a1(&m, 0x00);   /* Add(Arg0,1,Zero) */
        wrap1(&dev, 0x14, &m); }
      /* Method(LOOP){ Store(5,Local0); Store(0,Local1);
                       While(LGreater(Local0,0)){ Add(Local1,CALC(0),Local1); Decrement(Local0) }
                       Return(Local1) } */
      { Buf m; m.n = 0; aname(&m, "LOOP"); a1(&m, 0);
        a1(&m, 0x70); aint(&m, 5); a1(&m, 0x60);              /* Store(5,Local0) */
        a1(&m, 0x70); aint(&m, 0); a1(&m, 0x61);              /* Store(0,Local1) */
        { Buf wb; wb.n = 0;
          a1(&wb, 0x94); a1(&wb, 0x60); aint(&wb, 0);         /* LGreater(Local0,0) */
          a1(&wb, 0x72); a1(&wb, 0x61);                       /* Add(Local1, */
          aname(&wb, "CALC"); aint(&wb, 0);                   /*   CALC(0), */
          a1(&wb, 0x61);                                      /*   Local1) */
          a1(&wb, 0x76); a1(&wb, 0x60);                       /* Decrement(Local0) */
          wrap1(&m, 0xA2, &wb); }
        a1(&m, 0xA4); a1(&m, 0x61);                           /* Return(Local1) */
        wrap1(&dev, 0x14, &m); }
      wrap2(&sb, 0x5B, 0x82, &dev);
    }

    /* Device(TPD1): present=0 */
    { Buf dev; dev.n = 0; aname(&dev, "TPD1");
      { Buf o; o.n = 0; a1(&o, 0x0D); astr(&o, "PNP0C50"); o_name(&dev, "_HID", &o); }
      { Buf m; m.n = 0; aname(&m, "_STA"); a1(&m, 0); a1(&m, 0xA4); aint(&m, 0);
        wrap1(&dev, 0x14, &m); }
      wrap2(&sb, 0x5B, 0x82, &dev);
    }

    /* Device(GPO0): a fake pinctrl controller with one memory resource */
    { Buf dev; dev.n = 0; aname(&dev, "GPO0");
      { Buf o; o.n = 0; a1(&o, 0x0D); astr(&o, "TEST9999"); o_name(&dev, "_HID", &o); }
      { Buf rt; rt.n = 0; Buf ob; ob.n = 0; d_mem(&rt, 0x50001000, 0x1000);
        o_resbuf(&ob, &rt); o_name(&dev, "_CRS", &ob); }
      wrap2(&sb, 0x5B, 0x82, &dev);
    }

    /* Scope(\_SB) { sb } */
    Buf scope; scope.n = 0;
    {
        Buf body; body.n = 0;
        a1(&body, 0x5C); aname(&body, "_SB_");
        ad(&body, sb.d, sb.n);
        wrap1(&scope, 0x10, &body);
    }

    /* SSDT header (36 bytes) + scope */
    {
        static unsigned char tbl[8192];
        int total = 36 + scope.n, i, sum = 0;
        memset(tbl, 0, 36);
        memcpy(tbl, "SSDT", 4);
        tbl[4] = total; tbl[5] = total >> 8; tbl[6] = total >> 16; tbl[7] = total >> 24;
        tbl[8] = 2;
        memcpy(tbl + 10, "USBNT ", 6);
        memcpy(tbl + 16, "AMLTEST ", 8);
        tbl[24] = 1;
        memcpy(tbl + 28, "USBN", 4);
        tbl[32] = 1;
        memcpy(tbl + 36, scope.d, scope.n);
        for (i = 0; i < total; i++) sum = (sum + tbl[i]) & 0xFF;
        tbl[9] = (256 - sum) & 0xFF;
        g_table.Sig = ACPI_SIG('S', 'S', 'D', 'T');
        g_table.Va = tbl;
        g_table.Len = total;
        g_table.Phys = 0;
        g_have = 1;
    }
}

/* ------------------------------------------------------------------ */
/* checks                                                               */
/* ------------------------------------------------------------------ */
static int fails;
static void chk(int cond, const char *what)
{
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) fails++;
}

int main(int argc, char **argv)
{
    AML_VAL out, args[4];
    ULONGLONG v;
    GPIO_PIN pin;
    int i;
    if (argc > 1 && !strcmp(argv[1], "-v")) verbose = 1;

    build_table();
    chk(AmlInit() > 0, "namespace built from the SSDT");

    /* _STA */
    v = 0;
    chk(AmlEvalMethod("_SB_.TPD0", "_STA", NULL, 0, &out) == AML_OK && AmlAsInt(&out, &v) && v == 0x0F,
        "_STA present returns 0x0F");
    AmlFree(&out);
    v = 1;
    chk(AmlEvalMethod("_SB_.TPD1", "_STA", NULL, 0, &out) == AML_OK && AmlAsInt(&out, &v) && v == 0,
        "_STA of the absent device returns 0");
    AmlFree(&out);

    /* conditional _CRS: field = 1 -> addr 0x2C */
    g_mem[0x2000] = 1;
    {
        AML_CRS crs;
        chk(AmlEvalMethod("_SB_.TPD0", "_CRS", NULL, 0, &out) == AML_OK, "_CRS (type 1) evaluates");
        AmlParseCrs(&out, &crs);
        chk(crs.NI2c == 1 && crs.I2c[0].Addr == 0x2C, "_CRS type 1 -> I2C address 0x2C");
        AmlFree(&out);
    }
    g_mem[0x2000] = 0;
    {
        AML_CRS crs;
        chk(AmlEvalMethod("_SB_.TPD0", "_CRS", NULL, 0, &out) == AML_OK, "_CRS (type 0) evaluates");
        AmlParseCrs(&out, &crs);
        chk(crs.NI2c == 1 && crs.I2c[0].Addr == 0x15, "_CRS type 0 -> I2C address 0x15");
        AmlFree(&out);
    }

    /* _DSM returns a Package whose first element is the descriptor register */
    memset(args, 0, sizeof(args));
    args[0].Kind = AMLV_BUF; args[0].Buf = (UCHAR *)OsAlloc(16); args[0].Len = 16;
    AmlSetInt(&args[1], 1);
    AmlSetInt(&args[2], 1);
    args[3].Kind = AMLV_PKG; args[3].Len = 0; args[3].Pkg = (AML_VAL *)OsAlloc(sizeof(AML_VAL));
    chk(AmlEvalMethod("_SB_.TPD0", "_DSM", args, 4, &out) == AML_OK && out.Kind == AMLV_PKG &&
        out.Len == 2 && out.Pkg[0].Kind == AMLV_INT && out.Pkg[0].Int == 0x20,
        "_DSM returns Package { 0x20, 0 }");
    AmlFree(&out);
    for (i = 0; i < 4; i++) AmlFree(&args[i]);

    /* _PS0 writes 0xABCD into the SystemMemory power field */
    memset(g_mem + 0x3000, 0, 4);
    chk(AmlEvalMethod("_SB_.TPD0", "_PS0", NULL, 0, &out) == AML_OK, "_PS0 evaluates");
    AmlFree(&out);
    chk(g_mem[0x3000] == 0xCD && g_mem[0x3001] == 0xAB, "_PS0 wrote 0xABCD to the power register");

    /* method call + While + arithmetic */
    AmlSetInt(&args[0], 0x41);
    chk(AmlEvalMethod("_SB_.TPD0", "CALC", args, 1, &out) == AML_OK && AmlAsInt(&out, &v) && v == 0x42,
        "method call CALC(0x41) == 0x42");
    AmlFree(&out);
    chk(AmlEvalMethod("_SB_.TPD0", "LOOP", NULL, 0, &out) == AML_OK && AmlAsInt(&out, &v) && v == 5,
        "While loop with a method call sums to 5");
    AmlFree(&out);

    /* GPIO resolver / set / get against the fake pinctrl controller */
    memset(g_mem + 0x1000, 0, 0x1000);
    g_mem[0x100C] = 0x80;                         /* PADBAR = 0x80 */
    chk(GpioResolvePin("GPO0", 5, &pin), "GPIO pin GPO0/5 resolves");
    GpioSet(&pin, 1);
    chk((*(volatile ULONG *)pin.Cfg) & 1, "GpioSet high sets PADCFG0 TX");
    GpioSet(&pin, 0);
    chk(!((*(volatile ULONG *)pin.Cfg) & 1), "GpioSet low clears PADCFG0 TX");
    *(volatile ULONG *)pin.Cfg |= 2;              /* RX bit */
    chk(GpioGet(&pin) == 1, "GpioGet reads the input level");
    *(volatile ULONG *)pin.Cfg &= ~2UL;
    chk(GpioGet(&pin) == 0, "GpioGet reads a low input level");
    GpioCloseAll();

    /* AML off -> no namespace */
    printf("RESULT taml %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
