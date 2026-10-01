/*
 * main.c - bare metal test kernel. Runs the portable USB stack on QEMU and
 * accepts simple commands over COM1 so that a host script can exercise
 * enumeration, mass storage and HID paths.
 */

#include "../src/usbscsi.h"
#include "../src/usbinput.h"
#include "harness.h"

static UCHAR DataBuf[0x10000];
static UCHAR BigBuf[0x401000] __attribute__((aligned(4096)));
static ULONG BigPfn[0x402];
static volatile ULONG ReqDone;
static void HarnessReqDone(SCSI_REQ *r);
static char Line[128];
static int LinePos;

void OsKbdInput(const USB_KEY_EVENT *Events, ULONG Count)
{
    ULONG i;

    for (i = 0; i < Count; i++) {
        HarnessPrintf("KEY %02x %s%s%s\n", Events[i].MakeCode,
                      (Events[i].Flags & UKEY_BREAK) ? "up" : "down",
                      (Events[i].Flags & UKEY_E0) ? " e0" : "",
                      (Events[i].Flags & UKEY_E1) ? " e1" : "");
    }
}

void OsMouseInput(const USB_MOUSE_EVENT *E)
{
    HarnessPrintf("MOUSE flags=%04x x=%d y=%d wheel=%d pan=%d abs=%u\n",
                  E->ButtonFlags, E->X, E->Y, E->Wheel, E->Pan, E->Absolute);
}

static ULONG ParseNum(const char **p)
{
    ULONG v = 0;
    ULONG base = 10;
    const char *s = *p;

    while (*s == ' ') {
        s++;
    }
    if (s[0] == '0' && s[1] == 'x') {
        base = 16;
        s += 2;
    }
    for (;;) {
        char c = *s;
        ULONG d;
        if (c >= '0' && c <= '9') d = (ULONG)(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') d = (ULONG)(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') d = (ULONG)(c - 'A' + 10);
        else break;
        v = v * base + d;
        s++;
    }
    *p = s;
    return v;
}

static int StartsWith(const char *s, const char *w)
{
    while (*w) {
        if (*s++ != *w++) {
            return 0;
        }
    }
    return *s == ' ' || *s == 0;
}

static void Scsi(UCHAR Slot, const UCHAR *Cdb, UCHAR Len, UCHAR Dir, ULONG DataLen, SCSI_REQ *r)
{
    UsbMemSet(r, 0, sizeof(*r));
    r->Target = Slot;
    r->CdbLength = Len;
    UsbMemCpy(r->Cdb, Cdb, Len);
    r->Direction = Dir;
    r->Data = DataBuf;
    r->DataLength = DataLen;
    r->Complete = HarnessReqDone;
    ReqDone = 0;
    if (!UsbScsiExecute(r)) {
        while (ReqDone == 0) {
            UsbWorkerIteration();
        }
    }
}

static void HarnessReqDone(SCSI_REQ *r)
{
    (void)r;
    ReqDone++;
}

/* Request on BigBuf + Off; with Sg the data is described by page frames */
static void ScsiBig(UCHAR Slot, const UCHAR *Cdb, UCHAR Len, UCHAR Dir, ULONG DataLen, ULONG Off, int Sg, SCSI_REQ *r)
{
    ULONG i;

    for (i = 0; i < sizeof(BigPfn) / sizeof(BigPfn[0]); i++) {
        BigPfn[i] = ((ULONG)BigBuf >> 12) + i;
    }
    UsbMemSet(r, 0, sizeof(*r));
    r->Target = Slot;
    r->CdbLength = Len;
    UsbMemCpy(r->Cdb, Cdb, Len);
    r->Direction = Dir;
    r->Data = BigBuf + Off;
    if (Sg) {
        r->Pfn = BigPfn + Off / 4096;
        r->PageOff = Off % 4096;
    }
    r->DataLength = DataLen;
    r->Complete = HarnessReqDone;
    ReqDone = 0;
    if (UsbScsiExecute(r)) {
        return;
    }
    while (ReqDone == 0) {
        UsbWorkerIteration();
    }
}

static void BigPattern(UCHAR *Buf, ULONG Lba, ULONG Count, ULONG Seed)
{
    ULONG i, j;

    for (i = 0; i < Count; i++) {
        for (j = 0; j < 512; j++) {
            Buf[i * 512 + j] = (UCHAR)((Lba + i) * 13 + (j >> 1) + Seed + ((Lba + i) >> 8));
        }
    }
}

static ULONG BigCheck(const UCHAR *Buf, ULONG Lba, ULONG Count, ULONG Seed)
{
    ULONG i, j, bad = 0;

    for (i = 0; i < Count; i++) {
        for (j = 0; j < 512; j++) {
            if (Buf[i * 512 + j] != (UCHAR)((Lba + i) * 13 + (j >> 1) + Seed + ((Lba + i) >> 8))) {
                bad++;
            }
        }
    }
    return bad;
}

static void PrintResult(const char *What, SCSI_REQ *r)
{
    HarnessPrintf("%s srb=%02x scsi=%02x xfer=%u", What, r->SrbStatus, r->ScsiStatus, r->Transferred);
    if (r->SrbStatus & SRBST_AUTOSENSE_VALID) {
        HarnessPrintf(" sense=%x/%02x/%02x", r->Sense[2] & 0xF, r->Sense[12], r->Sense[13]);
    }
    HarnessPrintf("\n");
}

static ULONG Sum(const UCHAR *p, ULONG n)
{
    ULONG s = 0;
    ULONG i;

    for (i = 0; i < n; i++) {
        s = (s << 1 | s >> 31) ^ p[i];
    }
    return s;
}

static void FillPattern(ULONG Lba, ULONG Count, ULONG Seed)
{
    ULONG i, j;

    for (i = 0; i < Count; i++) {
        for (j = 0; j < 512; j++) {
            DataBuf[i * 512 + j] = (UCHAR)((Lba + i) * 7 + j + Seed);
        }
    }
}

static void CmdList(void)
{
    int i;
    int p;

    for (i = 0; i < UsbHcCount; i++) {
        USB_HC *hc = UsbHcs[i];
        HarnessPrintf("HC %d %s %s ports=%u\n", i, hc->Ops->Name, hc->Running ? "running" : "stopped", hc->NumPorts);
        for (p = 1; p <= hc->NumPorts; p++) {
            ULONG st = hc->Running ? hc->Ops->PortStatus(hc, p) : 0;
            if (st & PS_CONNECT) {
                HarnessPrintf("  port %d status %04x dev %d\n", p, st, hc->RootDev[p] ? hc->RootDev[p]->Index : -1);
            }
        }
    }
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        USB_DEV *d = &UsbDevs[i];
        if (d->InUse) {
            HarnessPrintf("DEV %d %04x:%04x speed=%u hc=%u root=%u depth=%u port=%u addr=%u hub=%u msc=%u hid=%u\n",
                          i, GET16(d->DevDesc + 8), GET16(d->DevDesc + 10), d->Speed, d->Hc->Index, d->RootPort,
                          d->Depth, d->Port, d->Address, d->Hub != NULL, d->Msc != NULL, d->Hid[0] != NULL);
        }
    }
    for (i = 0; i < (int)UsbScsiSlotCount; i++) {
        SCSI_SLOT_INFO si;
        UsbScsiGetSlot(i, &si);
        HarnessPrintf("SLOT %d present=%u type=%u\n", i, si.Present, si.DeviceType);
    }
    HarnessPrintf("KBDS %u MICE %u\n", HidKeyboardCount(), HidMouseCount());
}

static void Command(const char *c)
{
    const char *p;
    UCHAR cdb[16];
    SCSI_REQ r;

    UsbMemSet(cdb, 0, sizeof(cdb));
    if (StartsWith(c, "ls")) {
        CmdList();
    } else if (StartsWith(c, "work")) {
        UsbWorkerIteration();
    } else if (StartsWith(c, "tur")) {
        p = c + 3;
        cdb[0] = 0x00;
        Scsi((UCHAR)ParseNum(&p), cdb, 6, SCSI_DIR_NONE, 0, &r);
        PrintResult("TUR", &r);
    } else if (StartsWith(c, "inq")) {
        p = c + 3;
        cdb[0] = 0x12;
        cdb[4] = 36;
        Scsi((UCHAR)ParseNum(&p), cdb, 6, SCSI_DIR_IN, 36, &r);
        PrintResult("INQ", &r);
        HarnessPrintf("INQDATA type=%02x rmb=%02x\n", DataBuf[0], DataBuf[1]);
    } else if (StartsWith(c, "cap")) {
        p = c + 3;
        cdb[0] = 0x25;
        Scsi((UCHAR)ParseNum(&p), cdb, 10, SCSI_DIR_IN, 8, &r);
        PrintResult("CAP", &r);
        HarnessPrintf("CAPDATA last=%u bs=%u\n", GETBE32(DataBuf), GETBE32(DataBuf + 4));
    } else if (StartsWith(c, "ms6")) {
        p = c + 3;
        cdb[0] = 0x1A;
        cdb[2] = 0x3F;
        cdb[4] = 192;
        Scsi((UCHAR)ParseNum(&p), cdb, 6, SCSI_DIR_IN, 192, &r);
        PrintResult("MS6", &r);
        HarnessPrintf("MS6DATA %02x %02x %02x %02x\n", DataBuf[0], DataBuf[1], DataBuf[2], DataBuf[3]);
    } else if (StartsWith(c, "read") || StartsWith(c, "vpat") || StartsWith(c, "wpat") || StartsWith(c, "read6")) {
        ULONG slot, lba, cnt, seed = 0;
        BOOLEAN w = (BOOLEAN)(c[0] == 'w');
        BOOLEAN six = (BOOLEAN)(c[4] == '6');
        p = c + (six ? 5 : 4);
        slot = ParseNum(&p);
        lba = ParseNum(&p);
        cnt = ParseNum(&p);
        seed = ParseNum(&p);
        if (cnt == 0 || cnt > 128) {
            HarnessPrintf("ERR count\n");
            return;
        }
        if (six) {
            cdb[0] = 0x08;
            cdb[1] = (UCHAR)((lba >> 16) & 0x1F);
            cdb[2] = (UCHAR)(lba >> 8);
            cdb[3] = (UCHAR)lba;
            cdb[4] = (UCHAR)cnt;
        } else {
            cdb[0] = (UCHAR)(w ? 0x2A : 0x28);
            PUTBE32(cdb + 2, lba);
            PUTBE16(cdb + 7, (USHORT)cnt);
        }
        if (w) {
            FillPattern(lba, cnt, seed);
            Scsi((UCHAR)slot, cdb, six ? 6 : 10, SCSI_DIR_OUT, cnt * 512, &r);
            PrintResult("WRITE", &r);
        } else {
            UsbMemSet(DataBuf, 0xEE, cnt * 512);
            Scsi((UCHAR)slot, cdb, six ? 6 : 10, SCSI_DIR_IN, cnt * 512, &r);
            PrintResult("READ", &r);
            if (c[0] == 'v') {
                ULONG i, j, bad = 0;
                for (i = 0; i < cnt; i++) {
                    for (j = 0; j < 512; j++) {
                        if (DataBuf[i * 512 + j] != (UCHAR)((lba + i) * 7 + j + seed)) {
                            bad++;
                        }
                    }
                }
                HarnessPrintf("VERIFY bad=%u\n", bad);
            } else {
                HarnessPrintf("DATA sum=%08x first=%02x %02x %02x %02x last=%02x %02x\n", Sum(DataBuf, cnt * 512),
                              DataBuf[0], DataBuf[1], DataBuf[2], DataBuf[3],
                              DataBuf[cnt * 512 - 2], DataBuf[cnt * 512 - 1]);
            }
        }
    } else if (StartsWith(c, "bigw") || StartsWith(c, "bigv")) {
        ULONG slot, lba, cnt, seed, off, sg;
        ULONG t0;
        BOOLEAN w = (BOOLEAN)(c[3] == 'w');
        p = c + 4;
        slot = ParseNum(&p);
        lba = ParseNum(&p);
        cnt = ParseNum(&p);
        seed = ParseNum(&p);
        off = ParseNum(&p);
        sg = ParseNum(&p);
        if (cnt == 0 || cnt > 8192 || off + cnt * 512 > sizeof(BigBuf)) {
            HarnessPrintf("ERR count\n");
            return;
        }
        cdb[0] = (UCHAR)(w ? 0x2A : 0x28);
        PUTBE32(cdb + 2, lba);
        PUTBE16(cdb + 7, (USHORT)cnt);
        if (w) {
            BigPattern(BigBuf + off, lba, cnt, seed);
        } else {
            UsbMemSet(BigBuf + off, 0xEE, cnt * 512);
        }
        t0 = OsTimeMs();
        ScsiBig((UCHAR)slot, cdb, 10, w ? SCSI_DIR_OUT : SCSI_DIR_IN, cnt * 512, off, (int)sg, &r);
        PrintResult(w ? "BIGW" : "BIGV", &r);
        if (!w) {
            HarnessPrintf("VERIFY bad=%u ms=%u\n", BigCheck(BigBuf + off, lba, cnt, seed), OsTimeMs() - t0);
        }
    } else if (StartsWith(c, "qv")) {
        /* n reads of cnt sectors queued at once, each into its own part of BigBuf */
        static SCSI_REQ qr[8];
        ULONG slot, lba, cnt, n, seed, i, bad = 0, fails = 0;
        p = c + 2;
        slot = ParseNum(&p);
        lba = ParseNum(&p);
        cnt = ParseNum(&p);
        n = ParseNum(&p);
        seed = ParseNum(&p);
        if (n == 0 || n > 8 || cnt == 0 || n * cnt * 512 > sizeof(BigBuf) - 4096) {
            HarnessPrintf("ERR count\n");
            return;
        }
        for (i = 0; i < sizeof(BigPfn) / sizeof(BigPfn[0]); i++) {
            BigPfn[i] = ((ULONG)BigBuf >> 12) + i;
        }
        UsbMemSet(BigBuf, 0xEE, n * cnt * 512);
        ReqDone = 0;
        for (i = 0; i < n; i++) {
            SCSI_REQ *q = &qr[i];
            ULONG off = i * cnt * 512;
            UsbMemSet(q, 0, sizeof(*q));
            q->Target = (UCHAR)slot;
            q->CdbLength = 10;
            q->Cdb[0] = 0x28;
            PUTBE32(q->Cdb + 2, lba + i * cnt);
            PUTBE16(q->Cdb + 7, (USHORT)cnt);
            q->Direction = SCSI_DIR_IN;
            q->Data = BigBuf + off;
            q->Pfn = BigPfn + off / 4096;
            q->PageOff = off % 4096;
            q->DataLength = cnt * 512;
            q->Complete = HarnessReqDone;
            UsbScsiQueue(q);
        }
        while (ReqDone < n) {
            UsbWorkerIteration();
        }
        for (i = 0; i < n; i++) {
            if (qr[i].SrbStatus != SRBST_SUCCESS) {
                fails++;
            }
            bad += BigCheck(BigBuf + i * cnt * 512, lba + i * cnt, cnt, seed);
        }
        HarnessPrintf("QV fails=%u bad=%u\n", fails, bad);
    } else if (StartsWith(c, "leds")) {
        p = c + 4;
        HidSetLeds(ParseNum(&p));
        UsbWorkerIteration();
        HarnessPrintf("LEDS ok\n");
    } else if (StartsWith(c, "eject")) {
        p = c + 5;
        UsbScsiEject((int)ParseNum(&p));
        HarnessPrintf("EJECT ok\n");
    } else if (StartsWith(c, "debug")) {
        p = c + 5;
        UsbDebugLevel = ParseNum(&p);
        HarnessPrintf("DEBUG ok\n");
    } else if (c[0] != 0) {
        HarnessPrintf("ERR unknown\n");
    }
    HarnessPrintf("READY\n");
}

void kmain(unsigned long magic, unsigned long info)
{
    ULONG lastWork;

    (void)magic;
    (void)info;
    SerialInit();
    OsInitHarness();
    HarnessPrintf("harness: start\n");
    UsbDebugLevel = LOG_INFO;
    UsbScsiInit();
    if (UsbInit() != USB_OK) {
        HarnessPrintf("harness: no USB controller started\n");
    }
    UsbWorkerIteration();
    UsbWorkerIteration();
    HarnessPrintf("harness: ready\n");
    HarnessPrintf("READY\n");
    lastWork = OsTimeMs();

    for (;;) {
        int ch;
        UsbPollAll();
        HidTick();
        if (HarnessWorkerWake || OsTimeMs() - lastWork > 100) {
            HarnessWorkerWake = 0;
            lastWork = OsTimeMs();
            UsbWorkerIteration();
        }
        while ((ch = SerialGet()) >= 0) {
            if (ch == '\r' || ch == '\n') {
                Line[LinePos] = 0;
                LinePos = 0;
                Command(Line);
            } else if (LinePos < (int)sizeof(Line) - 1) {
                Line[LinePos++] = (char)ch;
            }
        }
    }
}
