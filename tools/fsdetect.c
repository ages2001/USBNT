/*
 * fsdetect.c - identifies the partitions and file systems on a USB disk slot
 * by reading the medium directly (SCSI pass-through on the usbnt SCSI port),
 * whether or not Windows has a file system driver for them.
 *
 * Partition tables: MBR (with extended partitions), GPT, none ("superfloppy").
 * File systems: FAT12/16/32, exFAT (+version), NTFS (+version from $Volume),
 * HPFS (+version), ReFS (+version), ext2/3/4, Btrfs, XFS, HFS/HFS+/HFSX, APFS,
 * F2FS, ISO 9660, UDF, Linux swap, LVM2, LUKS, BitLocker.
 */

#include "fsdetect.h"

#define MAXRD 0x10000

typedef struct _RDCTX {
    HANDLE  Port;
    UCHAR   Target;
    ULONG   Bs;             /* block size */
    ULONGLONG Blocks;
    UCHAR   Io[sizeof(SCSI_PASS_THROUGH) + 32 + MAXRD];
} RDCTX;

static RDCTX Rc;

static ULONG Get16(const UCHAR *p) { return p[0] | ((ULONG)p[1] << 8); }
static ULONG Get32(const UCHAR *p) { return p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24); }
static ULONGLONG Get64(const UCHAR *p) { return Get32(p) | ((ULONGLONG)Get32(p + 4) << 32); }
static ULONG GetBe16(const UCHAR *p) { return ((ULONG)p[0] << 8) | p[1]; }

static void Mz(void *d, ULONG n) { UCHAR *p = (UCHAR *)d; while (n--) *p++ = 0; }
static void Mc(void *d, const void *s, ULONG n) { UCHAR *p = (UCHAR *)d; const UCHAR *q = (const UCHAR *)s; while (n--) *p++ = *q++; }

static int Eq(const UCHAR *p, const char *s)
{
    while (*s) {
        if (*p++ != (UCHAR)*s++) return 0;
    }
    return 1;
}

static int Ilog2(ULONG v)
{
    int n = 0;
    while (v > 1) { v >>= 1; n++; }
    return n;
}

#ifdef FSD_HOST
BOOL HostRead(ULONGLONG Lba, ULONG Count, ULONG Bs, UCHAR *Buf);
static BOOL ReadBlocks(ULONGLONG Lba, ULONG Count, UCHAR *Buf) { return HostRead(Lba, Count, Rc.Bs, Buf); }
#else
/* READ(10)/READ(16) of Count blocks into Buf */
static BOOL ReadBlocks(ULONGLONG Lba, ULONG Count, UCHAR *Buf)
{
    SCSI_PASS_THROUGH *pt = (SCSI_PASS_THROUGH *)Rc.Io;
    ULONG len = Count * Rc.Bs, off = (sizeof(SCSI_PASS_THROUGH) + 31) & ~15UL, got = 0;
    if (len == 0 || len > MAXRD) return FALSE;
    Mz(pt, off);
    pt->Length = sizeof(SCSI_PASS_THROUGH);
    pt->TargetId = Rc.Target;
    pt->DataIn = SCSI_IOCTL_DATA_IN;
    pt->DataTransferLength = len;
    pt->TimeOutValue = 10;
    pt->DataBufferOffset = off;
    pt->SenseInfoLength = 0;
    if (Lba + Count <= 0xFFFFFFFFUL) {
        pt->CdbLength = 10;
        pt->Cdb[0] = 0x28;
        pt->Cdb[2] = (UCHAR)(Lba >> 24); pt->Cdb[3] = (UCHAR)(Lba >> 16);
        pt->Cdb[4] = (UCHAR)(Lba >> 8);  pt->Cdb[5] = (UCHAR)Lba;
        pt->Cdb[7] = (UCHAR)(Count >> 8); pt->Cdb[8] = (UCHAR)Count;
    } else {
        int i;
        pt->CdbLength = 16;
        pt->Cdb[0] = 0x88;
        for (i = 0; i < 8; i++) pt->Cdb[2 + i] = (UCHAR)(Lba >> (56 - 8 * i));
        pt->Cdb[10] = (UCHAR)(Count >> 24); pt->Cdb[11] = (UCHAR)(Count >> 16);
        pt->Cdb[12] = (UCHAR)(Count >> 8);  pt->Cdb[13] = (UCHAR)Count;
    }
    if (!DeviceIoControl(Rc.Port, IOCTL_SCSI_PASS_THROUGH, Rc.Io, off + len, Rc.Io, off + len, &got, NULL))
        return FALSE;
    if (pt->ScsiStatus != 0 || pt->DataTransferLength < len) return FALSE;
    Mc(Buf, Rc.Io + off, len);
    return TRUE;
}
#endif

/* Len bytes at byte offset Off of the partition starting at block Start */
static BOOL ReadAt(ULONGLONG Start, ULONGLONG Off, ULONG Len, UCHAR *Buf)
{
    static UCHAR tmp[MAXRD];
    ULONGLONG first = Start + (Off >> Ilog2(Rc.Bs));
    ULONG skip = (ULONG)(Off & (Rc.Bs - 1)), n;
    n = (skip + Len + Rc.Bs - 1) / Rc.Bs;
    if (n * Rc.Bs > MAXRD) return FALSE;
    if (Rc.Blocks && first + n > Rc.Blocks) return FALSE;
    if (!ReadBlocks(first, n, tmp)) return FALSE;
    Mc(Buf, tmp + skip, Len);
    return TRUE;
}

static void CopyLabel(char *Out, const UCHAR *In, int Len, int Utf16)
{
    int i, n = 0;
    for (i = 0; i < Len && n < FSD_LABEL - 1; i += Utf16 ? 2 : 1) {
        UCHAR c = In[i];
        if (Utf16 && In[i + 1]) c = '?';
        if (c == 0) break;
        Out[n++] = (char)(c < 0x20 ? '?' : c);
    }
    while (n > 0 && Out[n - 1] == ' ') n--;
    Out[n] = 0;
}

/* ---------------------------------------------------------------- */
/* file systems                                                      */
/* ---------------------------------------------------------------- */

static const char *NtfsWindows(ULONG maj, ULONG min)
{
    if (maj == 1 && min == 0) return " (Windows NT 3.1)";
    if (maj == 1 && min == 1) return " (Windows NT 3.5)";
    if (maj == 1 && min == 2) return " (Windows NT 3.51 / 4.0)";
    if (maj == 3 && min == 0) return " (Windows 2000)";
    if (maj == 3 && min == 1) return " (Windows XP and later)";
    return "";
}

/* NTFS version: $Volume (MFT record 3), attribute $VOLUME_INFORMATION (0x70) */
static void NtfsVersion(ULONGLONG Start, const UCHAR *Bs, FS_PART *P)
{
    static UCHAR rec[4096];
    ULONG bps = Get16(Bs + 0x0B), spc = Bs[0x0D], clus, rsz, usa, usn, i, a;
    LONG cpr = (signed char)Bs[0x40];
    ULONGLONG mft = Get64(Bs + 0x30);
    if (spc > 0x80) spc = 1UL << (256 - spc);
    clus = bps * spc;
    rsz = cpr < 0 ? (1UL << -cpr) : (ULONG)cpr * clus;
    if (!clus || rsz < 512 || rsz > sizeof(rec)) return;
    if (!ReadAt(Start, mft * clus + 3 * (ULONGLONG)rsz, rsz, rec) || !Eq(rec, "FILE")) return;
    /* update sequence fixups */
    usa = Get16(rec + 4); usn = Get16(rec + 6);
    if (usa + usn * 2 > rsz) return;
    for (i = 1; i < usn; i++) {
        ULONG end = i * 512 - 2;
        if (end + 2 > rsz) break;
        rec[end] = rec[usa + i * 2]; rec[end + 1] = rec[usa + i * 2 + 1];
    }
    for (a = Get16(rec + 0x14); a + 16 < rsz;) {
        ULONG type = Get32(rec + a), len = Get32(rec + a + 4);
        if (type == 0xFFFFFFFFUL || len < 16 || a + len > rsz) break;
        if (type == 0x60 && rec[a + 8] == 0) {                    /* $VOLUME_NAME */
            ULONG vl = Get32(rec + a + 0x10), vo = Get16(rec + a + 0x14);
            if (a + vo + vl <= rsz) CopyLabel(P->Label, rec + a + vo, (int)vl, 1);
        }
        if (type == 0x70 && rec[a + 8] == 0) {
            ULONG vo = Get16(rec + a + 0x14);
            if (a + vo + 12 <= rsz) {
                ULONG maj = rec[a + vo + 8], min = rec[a + vo + 9];
                wsprintfA(P->Fs, "NTFS %u.%u%s", maj, min, NtfsWindows(maj, min));
                P->Version = (maj << 8) | min;
            }
        }
        a += len;
    }
}

static BOOL FatBpb(const UCHAR *b, ULONG *Clusters, ULONG *Type)
{
    ULONG bps = Get16(b + 0x0B), spc = b[0x0D], rsv = Get16(b + 0x0E), nf = b[0x10], root = Get16(b + 0x11);
    ULONG tot = Get16(b + 0x13) ? Get16(b + 0x13) : Get32(b + 0x20), fsz = Get16(b + 0x16) ? Get16(b + 0x16) : Get32(b + 0x24);
    ULONG rootsec, data;
    if (bps < 512 || bps > 4096 || (bps & (bps - 1)) || !spc || (spc & (spc - 1)) || !rsv || nf < 1 || nf > 4 || !fsz)
        return FALSE;
    if (b[0x15] != 0xF0 && b[0x15] < 0xF8) return FALSE;
    rootsec = (root * 32 + bps - 1) / bps;
    if (tot <= rsv + nf * fsz + rootsec) return FALSE;
    data = tot - (rsv + nf * fsz + rootsec);
    *Clusters = data / spc;
    *Type = !Get16(b + 0x16) && root == 0 ? 32 : *Clusters < 4085 ? 12 : *Clusters < 65525 ? 16 : 32;
    return TRUE;
}

static void Detect(ULONGLONG Start, ULONGLONG Len, FS_PART *P)
{
    static UCHAR b[4096], s[4096];
    ULONG cl, ft;
    P->Fs[0] = 0; P->Label[0] = 0; P->Version = 0;
    if (!ReadAt(Start, 0, 4096, b)) { lstrcpyA(P->Fs, "unreadable"); return; }

    if (Eq(b + 3, "NTFS    ")) {
        lstrcpyA(P->Fs, "NTFS");
        NtfsVersion(Start, b, P);
        return;
    }
    if (Eq(b + 3, "EXFAT   ")) {
        ULONG bsh = b[0x6C], csh = b[0x6D], heap = Get32(b + 0x58), root = Get32(b + 0x60), k;
        wsprintfA(P->Fs, "exFAT %u.%02u", b[0x69], b[0x68]);
        P->Version = Get16(b + 0x68);
        /* volume label: directory entry 0x83 in the first cluster of the root directory */
        if (bsh >= 9 && bsh <= 12 && bsh + csh <= 25 && root >= 2 &&
            ReadAt(Start, ((ULONGLONG)heap << bsh) + ((ULONGLONG)(root - 2) << (bsh + csh)), 4096, s)) {
            for (k = 0; k < 4096; k += 32) {
                if (s[k] == 0x83 && s[k + 1] <= 11) { CopyLabel(P->Label, s + k + 2, s[k + 1] * 2, 1); break; }
                if (s[k] == 0) break;
            }
        }
        return;
    }
    if (Eq(b + 3, "ReFS") && b[7] == 0 && Eq(b + 0x10, "FSRS")) {
        wsprintfA(P->Fs, "ReFS %u.%u", b[0x28], b[0x29]);
        P->Version = (b[0x28] << 8) | b[0x29];
        return;
    }
    if (Eq(b + 3, "-FVE-FS-")) { lstrcpyA(P->Fs, "BitLocker (encrypted)"); return; }
    if (Eq(b, "LUKS\xBA\xBE")) { wsprintfA(P->Fs, "LUKS%u (encrypted)", GetBe16(b + 6)); return; }
    if (Eq(b, "XFSB")) { CopyLabel(P->Label, b + 0x6C, 12, 0); wsprintfA(P->Fs, "XFS v%u", GetBe16(b + 0x64) & 0xF); return; }
    if (Eq(b + 0x20, "NXSB")) { lstrcpyA(P->Fs, "APFS"); return; }
    if (Eq(b + 0x200, "LABELONE") || Eq(b + 0x218, "LVM2 001")) { lstrcpyA(P->Fs, "LVM2 physical volume"); return; }
    if (Eq(b + 4086, "SWAPSPACE2") || Eq(b + 4086, "SWAP-SPACE")) { lstrcpyA(P->Fs, "Linux swap"); return; }

    /* superblocks at 1 KB: ext2/3/4, HFS/HFS+, F2FS */
    if (Get16(b + 1024 + 0x38) == 0xEF53) {
        const UCHAR *e = b + 1024;
        ULONG compat = Get32(e + 0x5C), incompat = Get32(e + 0x60), ro = Get32(e + 0x64);
        CopyLabel(P->Label, e + 0x78, 16, 0);
        if ((incompat & (0x40 | 0x80 | 0x200 | 0x400 | 0x8000 | 0x10000)) || (ro & (0x08 | 0x20 | 0x40 | 0x400)))
            lstrcpyA(P->Fs, "ext4");
        else if (compat & 0x4)
            lstrcpyA(P->Fs, "ext3");
        else
            lstrcpyA(P->Fs, "ext2");
        return;
    }
    if (Eq(b + 1024, "H+") || Eq(b + 1024, "HX")) { lstrcpyA(P->Fs, b[1025] == 'X' ? "HFSX" : "HFS+"); return; }
    if (Eq(b + 1024, "BD")) { lstrcpyA(P->Fs, "HFS"); return; }
    if (Get32(b + 1024) == 0xF2F52010UL) { lstrcpyA(P->Fs, "F2FS"); return; }

    /* HPFS: super block at sector 16 */
    if (ReadAt(Start, 8192, 512, s) && Get32(s) == 0xF995E849UL && Get32(s + 4) == 0xFA53E9C5UL) {
        wsprintfA(P->Fs, "HPFS%s", s[9] >= 3 ? " (> 4 GB volume)" : "");
        P->Version = (s[8] << 8) | s[9];
        if (Eq(b + 0x36, "HPFS")) CopyLabel(P->Label, b + 0x2B, 11, 0);
        return;
    }
    /* Btrfs: super block at 64 KB */
    if (Len > 0x20000 / Rc.Bs && ReadAt(Start, 0x10000, 4096, s) && Eq(s + 0x40, "_BHRfS_M")) {
        CopyLabel(P->Label, s + 0x12B, 64, 0);
        lstrcpyA(P->Fs, "Btrfs");
        return;
    }
    /* ISO 9660 / UDF: volume descriptors from 32 KB */
    if (ReadAt(Start, 0x8000, 2048, s)) {
        if (Eq(s + 1, "CD001")) { CopyLabel(P->Label, s + 40, 32, 0); lstrcpyA(P->Fs, "ISO 9660"); }
        if (Eq(s + 1, "BEA01") || Eq(s + 1, "NSR02") || Eq(s + 1, "NSR03")) lstrcpyA(P->Fs, "UDF");
        if (P->Fs[0]) {
            int k;
            for (k = 1; k < 8 && ReadAt(Start, 0x8000 + k * 2048, 2048, s); k++)
                if (Eq(s + 1, "NSR02") || Eq(s + 1, "NSR03")) { lstrcpyA(P->Fs, "UDF"); break; }
            return;
        }
    }
    /* FAT last: the BPB checks are the weakest signature */
    if (Get16(b + 510) == 0xAA55 || b[0] == 0xE9 || b[0] == 0xEB) {
        if (FatBpb(b, &cl, &ft)) {
            if (ft == 32) {
                CopyLabel(P->Label, b + 0x47, 11, 0);
                wsprintfA(P->Fs, "FAT32");
            } else {
                if (b[0x26] == 0x29) CopyLabel(P->Label, b + 0x2B, 11, 0);
                wsprintfA(P->Fs, "FAT%u", ft);
            }
            if (!lstrcmpA(P->Label, "NO NAME")) P->Label[0] = 0;
            return;
        }
    }
    lstrcpyA(P->Fs, "unknown");
}

/* ---------------------------------------------------------------- */
/* partition tables                                                  */
/* ---------------------------------------------------------------- */

static int AddPart(FS_PART *Parts, int N, int Max, ULONGLONG Start, ULONGLONG Len, UCHAR Type, const char *Scheme)
{
    if (N >= Max || Len == 0) return N;
    Parts[N].Start = Start;
    Parts[N].Length = Len;
    Parts[N].MbrType = Type;
    Parts[N].PartName[0] = 0;
    lstrcpyA(Parts[N].Scheme, Scheme);
    Detect(Start, Len, &Parts[N]);
    return N + 1;
}

static BOOL LooksLikeVbr(const UCHAR *b)
{
    ULONG cl, ft;
    if (Eq(b + 3, "NTFS    ") || Eq(b + 3, "EXFAT   ") || (Eq(b + 3, "ReFS") && Eq(b + 0x10, "FSRS")) || Eq(b + 3, "-FVE-FS-") ||
        Eq(b + 0x36, "HPFS    "))
        return TRUE;
    return (b[0] == 0xEB || b[0] == 0xE9) && FatBpb(b, &cl, &ft);
}

int FsDetectSlot(ULONG ScsiPort, ULONG Slot, ULONG BlockSize, ULONG LastLba, FS_PART *Parts, int Max)
{
    static UCHAR b[4096], g[4096];
    char name[32];
    int n = 0, i;

    wsprintfA(name, "\\\\.\\Scsi%u:", ScsiPort);
    Rc.Port = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (Rc.Port == INVALID_HANDLE_VALUE) return -1;
    Rc.Target = (UCHAR)Slot;
    Rc.Bs = (BlockSize >= 512 && BlockSize <= 4096 && !(BlockSize & (BlockSize - 1))) ? BlockSize : 512;
    Rc.Blocks = LastLba ? (ULONGLONG)LastLba + 1 : 0;

    if (!ReadAt(0, 0, 512, b)) { CloseHandle(Rc.Port); return -1; }
    if (Rc.Bs == 2048) {
        /* optical medium: no partition table */
        n = AddPart(Parts, n, Max, 0, Rc.Blocks, 0, "none");
    } else if (Get16(b + 510) == 0xAA55 && !LooksLikeVbr(b)) {
        BOOL gpt = FALSE;
        for (i = 0; i < 4; i++) if (b[446 + i * 16 + 4] == 0xEE) gpt = TRUE;
        if (gpt && ReadAt(0, Rc.Bs, 512, g) && Eq(g, "EFI PART")) {
            ULONGLONG ent = Get64(g + 0x48);
            ULONG cnt = Get32(g + 0x50), esz = Get32(g + 0x54), k;
            if (esz < 128 || esz > 512) esz = 128;
            if (cnt > 128) cnt = 128;
            for (k = 0; k < cnt && n < Max; k++) {
                static UCHAR e[512];
                ULONGLONG first, last;
                ULONG z, nz = 0;
                if (!ReadAt(0, ent * Rc.Bs + k * esz, esz, e)) break;
                for (z = 0; z < 16; z++) nz |= e[z];
                if (!nz) continue;
                first = Get64(e + 32); last = Get64(e + 40);
                if (last < first) continue;
                n = AddPart(Parts, n, Max, first, last - first + 1, 0xEE, "GPT");
                if (n) CopyLabel(Parts[n - 1].PartName, e + 56, 72, 1);
            }
        } else {
            for (i = 0; i < 4 && n < Max; i++) {
                const UCHAR *p = b + 446 + i * 16;
                UCHAR t = p[4];
                ULONG st = Get32(p + 8), ln = Get32(p + 12);
                if (!t || !ln) continue;
                if (t == 0x05 || t == 0x0F || t == 0x85) {
                    /* extended partition: chain of EBRs */
                    ULONG ebr = st, guard;
                    for (guard = 0; guard < 32 && n < Max; guard++) {
                        static UCHAR x[512];
                        ULONG ls, ll, nx;
                        if (!ReadAt(0, (ULONGLONG)ebr * Rc.Bs, 512, x) || Get16(x + 510) != 0xAA55) break;
                        ls = Get32(x + 446 + 8); ll = Get32(x + 446 + 12);
                        if (x[446 + 4] && ll) n = AddPart(Parts, n, Max, (ULONGLONG)ebr + ls, ll, x[446 + 4], "MBR logical");
                        nx = Get32(x + 446 + 16 + 8);
                        if (!x[446 + 16 + 4] || !nx) break;
                        ebr = st + nx;
                    }
                    continue;
                }
                n = AddPart(Parts, n, Max, st, ln, t, "MBR");
            }
        }
        if (n == 0) n = AddPart(Parts, n, Max, 0, Rc.Blocks, 0, "none");   /* no usable entry: whole medium */
    } else {
        n = AddPart(Parts, n, Max, 0, Rc.Blocks, 0, "none");
    }
    CloseHandle(Rc.Port);
    return n;
}
