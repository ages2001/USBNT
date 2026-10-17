/* acpi.h - firmware ACPI tables (acpi.c) and a few AML helpers (i2chid.c) */
#ifndef ACPI_H
#define ACPI_H

#define ACPI_SIG(a, b, c, d) ((ULONG)(a) | ((ULONG)(b) << 8) | ((ULONG)(c) << 16) | ((ULONG)(d) << 24))

typedef struct _ACPI_TABLE {
    ULONG        Sig;
    const UCHAR *Va;
    ULONG        Len;
    ULONG        Phys;
} ACPI_TABLE;

int               AcpiInit(void);           /* number of tables found */
const ACPI_TABLE *AcpiTable(int Index);

#endif
