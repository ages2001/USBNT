/*
 * aml.h - a small, evaluation-only AML interpreter for the HID over I2C code
 * (i2chid.c). It does NOT take ACPI over: it builds a namespace from the
 * firmware's DSDT / SSDTs (acpi.c) and evaluates single methods on demand
 * (_STA, _CRS, _DSM, _PS0, _PS3, _INI) so touch pads and touch screens whose
 * address, descriptor register or power / reset lines the firmware only sets
 * at run time can be found on every OS USBNT supports (NT 3.1 - XP x64).
 *
 * Only the objects reached from the methods above are touched; OperationRegion
 * writes happen only to regions those methods use, and anything unknown or
 * unsafe makes the evaluation fail so i2chid.c falls back to its old
 * behaviour (the usual addresses, blind polling). GeneralPurposeIO regions go
 * through the GPIO driver (gpio.c).
 */
#ifndef AML_H
#define AML_H

/* value kinds */
#define AMLV_UNINIT 0
#define AMLV_INT    1
#define AMLV_BUF    2
#define AMLV_PKG    3
#define AMLV_STR    4
#define AMLV_REF    5          /* reference to a namespace node (Index / RefOf target) */

typedef struct _AML_VAL AML_VAL;
struct _AML_VAL {
    UCHAR      Kind;
    ULONGLONG  Int;
    UCHAR     *Buf;            /* AMLV_BUF / AMLV_STR (NUL terminated for STR), owned */
    ULONG      Len;
    AML_VAL   *Pkg;            /* AMLV_PKG, owned array of Len elements */
    int        Node;           /* AMLV_REF: namespace node index, or -1 with Buf/Index set */
    ULONG      Index;          /* AMLV_REF into a buffer / package element */
};

/* result of AmlEval */
#define AML_OK       0
#define AML_NOTFOUND 1        /* no such method / object */
#define AML_FAIL     2        /* evaluation hit something unsupported -> caller falls back */

/* Builds the namespace from the ACPI tables (idempotent). Returns the number
   of named objects, 0 when there are no tables or AML evaluation is off. */
int  AmlInit(void);
void AmlShutdown(void);

/* Evaluate the method or read the data object at an absolute or scope-relative
   path (e.g. "\\_SB.PCI0.I2C0.TPD0._STA"). Args/NArgs are the method arguments
   (integers / buffers built by the caller). On AML_OK *Out holds the result
   (AmlFree it). A non existent path is AML_NOTFOUND; anything unsupported is
   AML_FAIL. */
int  AmlEval(const char *Path, const AML_VAL *Args, int NArgs, AML_VAL *Out);

/* Convenience: evaluate <DevicePath>.<Name> (Name is 4 chars like "_STA"); the
   device path is what AmlFindDev stored. */
int  AmlEvalMethod(const char *DevPath, const char *Name, const AML_VAL *Args, int NArgs, AML_VAL *Out);

/* TRUE when the device at DevPath has the method / object Name. */
int  AmlHasObject(const char *DevPath, const char *Name);

/* Walk every Device() in the namespace: fn gets its absolute path and its
   _HID / _CID text (either may be ""). Stops when fn returns FALSE. */
typedef int (*AML_DEV_FN)(const char *Path, const char *Hid, const char *Cid, void *Ctx);
void AmlForEachDevice(AML_DEV_FN Fn, void *Ctx);

/* value helpers */
void      AmlFree(AML_VAL *V);
void      AmlSetInt(AML_VAL *V, ULONGLONG N);
BOOLEAN   AmlAsInt(const AML_VAL *V, ULONGLONG *N);     /* int, or buffer's first bytes */
const AML_VAL *AmlPkgElem(const AML_VAL *V, ULONG I);   /* element of a package, else NULL */

/* resource descriptors found in a _CRS result buffer (ResourceTemplate) */
typedef struct _AML_I2C_RES {
    USHORT Addr;
    ULONG  Speed;
    char   Ctrl[32];          /* resource source path, last-segment form in Ctrl4 */
    char   Ctrl4[5];
} AML_I2C_RES;
typedef struct _AML_GPIO_RES {
    USHORT Pin;
    UCHAR  IsInt;             /* GpioInt (1) vs GpioIo (0) */
    UCHAR  ActiveLow;         /* polarity: 1 active low */
    UCHAR  Level;             /* GpioInt: 1 level, 0 edge */
    UCHAR  Shared;
    char   Ctrl[32];
    char   Ctrl4[5];
} AML_GPIO_RES;
typedef struct _AML_CRS {
    int          NI2c;
    AML_I2C_RES  I2c[4];
    int          NGpio;
    AML_GPIO_RES Gpio[8];
    int          NMem;
    ULONG        MemBase[8], MemLen[8];
} AML_CRS;
/* parse a ResourceTemplate buffer value (e.g. a _CRS result) */
void AmlParseCrs(const AML_VAL *Buf, AML_CRS *Crs);

/* GPIO controller lookup for gpio.c: find the Device whose last name segment is
   Name4 (e.g. "GPO0"); fills its _HID text, _UID and parsed _CRS. FALSE when
   there is no such device. */
BOOLEAN AmlGpioController(const char *Name4, char *HidOut, ULONG HidSize, ULONG *Uid, AML_CRS *Crs);
/* Name (SEG, integer) anywhere in the tables (e.g. SBRG for the GPIO SBREG bar) */
BOOLEAN AmlGlobalInt(const char *Seg4, ULONGLONG *Val);
/* absolute device path of a controller found by its last segment (gpio.c logs) */

#endif
