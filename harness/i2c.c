/* i2c.c - harness commands for HID over I2C
 *
 *   i2c        one line per DesignWare controller and HID over I2C device
 *   pcicfg B D F OFF   one dword of PCI configuration space (hot plug tests)
 */
#include "../src/usbnt.h"
#include "harness.h"

static ULONG Num(const char **p)
{
    ULONG v = 0;
    const char *s = *p;
    while (*s == ' ') s++;
    if (s[0] == '0' && s[1] == 'x') {
        s += 2;
        for (;; s++) {
            if (*s >= '0' && *s <= '9') v = v * 16 + (ULONG)(*s - '0');
            else if (*s >= 'a' && *s <= 'f') v = v * 16 + (ULONG)(*s - 'a' + 10);
            else break;
        }
    } else {
        while (*s >= '0' && *s <= '9') v = v * 10 + (ULONG)(*s++ - '0');
    }
    *p = s;
    return v;
}

int I2cHarnessCommand(const char *c)
{
    char line[200];
    int i;

    /* pcicfg BUS DEV FN OFF: one dword of PCI configuration space */
    if (c[0] == 'p' && c[1] == 'c' && c[2] == 'i' && c[3] == 'c' && c[4] == 'f' && c[5] == 'g') {
        const char *p = c + 6;
        ULONG b = Num(&p), d = Num(&p), f = Num(&p), o = Num(&p);
        HarnessPrintf("PCICFG %02x:%02x.%u %03x = %08x\n", b, d, f, o, OsPciRead((UCHAR)b, (UCHAR)d, (UCHAR)f, o, 4));
        return 1;
    }

    if (!(c[0] == 'i' && c[1] == '2' && c[2] == 'c' && (c[3] == 0 || c[3] == ' '))) {
        return 0;
    }
    for (i = 0; I2cHidDescribe(i, line, sizeof(line)) > 0; i++) {
        HarnessPrintf("%s\n", line);
    }
    HarnessPrintf("I2C %d devices\n", I2cHidCount);
    return 1;
}
