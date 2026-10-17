/* tbt.h - Thunderbolt 3 / 4 and USB4 host routers (tbt.c) */
#ifndef TBT_H
#define TBT_H

/* PCI start scan: an NHI (Thunderbolt / USB4 host interface) */
void    TbtProbe(UCHAR Bus, UCHAR Dev, UCHAR Fn, ULONG Id, ULONG Cls);
/* worker: control channel events, approvals, hot plug of routers */
void    TbtService(void);
void    TbtShutdown(void);
/* for usbtree: Index-th host router or connected device */
BOOLEAN TbtInfo(int Index, char *Name, int Size, char *Detail, int DSize);
extern int TbtHostCount;

/* SHA-256 and HMAC-SHA-256 (Thunderbolt secure connect) */
void    Sha256(const UCHAR *Data, ULONG Len, UCHAR *Out);
void    HmacSha256(const UCHAR *Key, ULONG KeyLen, const UCHAR *Data, ULONG Len, UCHAR *Out);

#endif
