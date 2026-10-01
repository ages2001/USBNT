/*
 * usbutil.c - small runtime helpers without C library dependencies.
 */

#include "usbnt.h"

ULONG UsbDebugLevel = LOG_INFO;

void UsbMemCpy(void *Dst, const void *Src, ULONG Len)
{
    UCHAR *d = (UCHAR *)Dst;
    const UCHAR *s = (const UCHAR *)Src;

    if ((((ULONG)d | (ULONG)s) & 3) == 0) {
        while (Len >= 4) {
            *(ULONG *)d = *(const ULONG *)s;
            d += 4;
            s += 4;
            Len -= 4;
        }
    }
    while (Len--) {
        *d++ = *s++;
    }
}

void UsbMemSet(void *Dst, int Val, ULONG Len)
{
    UCHAR *d = (UCHAR *)Dst;

    while (Len--) {
        *d++ = (UCHAR)Val;
    }
}

int UsbMemCmp(const void *A, const void *B, ULONG Len)
{
    const UCHAR *a = (const UCHAR *)A;
    const UCHAR *b = (const UCHAR *)B;

    while (Len--) {
        if (*a != *b) {
            return (int)*a - (int)*b;
        }
        a++;
        b++;
    }
    return 0;
}

void PUT16(void *P, USHORT V)
{
    UCHAR *p = (UCHAR *)P;
    p[0] = (UCHAR)V;
    p[1] = (UCHAR)(V >> 8);
}

void PUT32(void *P, ULONG V)
{
    UCHAR *p = (UCHAR *)P;
    p[0] = (UCHAR)V;
    p[1] = (UCHAR)(V >> 8);
    p[2] = (UCHAR)(V >> 16);
    p[3] = (UCHAR)(V >> 24);
}

void PUTBE16(void *P, USHORT V)
{
    UCHAR *p = (UCHAR *)P;
    p[0] = (UCHAR)(V >> 8);
    p[1] = (UCHAR)V;
}

void PUTBE32(void *P, ULONG V)
{
    UCHAR *p = (UCHAR *)P;
    p[0] = (UCHAR)(V >> 24);
    p[1] = (UCHAR)(V >> 16);
    p[2] = (UCHAR)(V >> 8);
    p[3] = (UCHAR)V;
}

/* Minimal formatter: %d %u %x %X %s %c %p %%, optional '0' flag, width, 'l' */
int UsbVFormat(char *Buf, int Size, const char *Fmt, va_list Ap)
{
    int pos = 0;
    char tmp[16];

    if (Size <= 0) {
        return 0;
    }
    while (*Fmt) {
        char c = *Fmt++;
        int width = 0;
        int zero = 0;
        int len;
        ULONG v;
        const char *s;

        if (c != '%') {
            if (pos < Size - 1) {
                Buf[pos++] = c;
            }
            continue;
        }
        if (*Fmt == '0') {
            zero = 1;
            Fmt++;
        }
        while (*Fmt >= '0' && *Fmt <= '9') {
            width = width * 10 + (*Fmt - '0');
            Fmt++;
        }
        if (*Fmt == 'l') {
            Fmt++;
        }
        c = *Fmt++;
        len = 0;
        s = tmp;
        switch (c) {
        case 'd':
        case 'u':
        case 'x':
        case 'X':
        case 'p': {
            int neg = 0;
            ULONG base = (c == 'd' || c == 'u') ? 10 : 16;
            const char *digits = (c == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            v = va_arg(Ap, ULONG);
            if (c == 'p') {
                width = 8;
                zero = 1;
            }
            if (c == 'd' && (LONG)v < 0) {
                neg = 1;
                v = (ULONG)(-(LONG)v);
            }
            do {
                tmp[15 - len] = digits[v % base];
                v /= base;
                len++;
            } while (v && len < 15);
            if (neg) {
                if (zero) {
                    if (pos < Size - 1) {
                        Buf[pos++] = '-';
                    }
                    width--;
                } else {
                    tmp[15 - len] = '-';
                    len++;
                }
            }
            s = &tmp[16 - len];
            break;
        }
        case 's':
            s = va_arg(Ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            while (s[len]) {
                len++;
            }
            zero = 0;
            break;
        case 'c':
            tmp[0] = (char)va_arg(Ap, int);
            len = 1;
            zero = 0;
            break;
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        default:
            tmp[0] = '?';
            len = 1;
            break;
        }
        while (width > len) {
            if (pos < Size - 1) {
                Buf[pos++] = zero ? '0' : ' ';
            }
            width--;
        }
        while (len-- > 0) {
            if (pos < Size - 1) {
                Buf[pos++] = *s;
            }
            s++;
        }
    }
    Buf[pos] = 0;
    return pos;
}

int UsbFormat(char *Buf, int Size, const char *Fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, Fmt);
    r = UsbVFormat(Buf, Size, Fmt, ap);
    va_end(ap);
    return r;
}

void UsbLog(ULONG Level, const char *Fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    if (Level > UsbDebugLevel) {
        return;
    }
    n = UsbFormat(buf, sizeof(buf), "usbnt: ");
    va_start(ap, Fmt);
    UsbVFormat(buf + n, sizeof(buf) - n, Fmt, ap);
    va_end(ap);
    OsLogStr(buf);
}

void UsbHexDump(ULONG Level, const char *Title, const void *Data, ULONG Len)
{
    const UCHAR *p = (const UCHAR *)Data;
    char line[80];
    ULONG i;
    int n;

    if (Level > UsbDebugLevel) {
        return;
    }
    UsbLog(Level, "%s (%u bytes)\n", Title, Len);
    for (i = 0; i < Len; i += 16) {
        ULONG j;
        n = UsbFormat(line, sizeof(line), "  %04x:", i);
        for (j = i; j < i + 16 && j < Len; j++) {
            n += UsbFormat(line + n, sizeof(line) - n, " %02x", p[j]);
        }
        UsbFormat(line + n, sizeof(line) - n, "\n");
        OsLogStr(line);
    }
}
