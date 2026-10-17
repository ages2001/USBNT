#!/usr/bin/env python3
"""Builds a USB stick image with several partitions for the partition tests.

  mkpart.py IMAGE [nt|w5|gpt]

'nt' (NT 3.1 - 4.0): three primary FAT16 partitions and an extended partition
with two logical FAT16 partitions.  'w5' (Windows 2000 / XP): FAT16, FAT16,
FAT32, and logical FAT16 and NTFS.  'gpt': protective MBR, GPT with three
partitions (FAT16, FAT32, NTFS).  Partition n has the volume label PARTn and a
file Pn.TXT that holds "PARTITION n" (the NTFS one: P6.TXT / P3.TXT through
ntfscp).  Also usable as a module: mkstick(path, kind) returns the partition
list [(number, start, sectors, type, fs)].
"""
import os, struct, subprocess, sys, tempfile, uuid, zlib

MB = 2048           # sectors


def chs(lba):
    c, r = divmod(lba, 64 * 32)
    h, s = divmod(r, 32)
    if c > 1023:
        return bytes([0xFE, 0xFF, 0xFF])
    return bytes([h, ((c >> 2) & 0xC0) | (s + 1), c & 0xFF])


def entry(boot, ptype, start, count, rel=None):
    lba = start if rel is None else rel
    return bytes([boot]) + chs(start) + bytes([ptype]) + chs(start + count - 1) + struct.pack('<II', lba, count)


def fmt(path, start, count, fs, label, files):
    """formats the partition at sector start (count sectors) and copies files [(name, bytes)]"""
    if fs == 'ntfs':
        with tempfile.TemporaryDirectory() as td:
            vol = os.path.join(td, 'v.img')
            with open(vol, 'wb') as f:
                f.truncate(count * 512)
            subprocess.check_call(['mkntfs', '-F', '-Q', '-q', '-L', label, '-p', str(start), '-H', '64', '-S', '32',
                                   vol], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            for name, data in files:
                src = os.path.join(td, name)
                open(src, 'wb').write(data)
                subprocess.check_call(['ntfscp', '-f', '-q', vol, src, name.upper()], stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
            with open(path, 'r+b') as f, open(vol, 'rb') as v:
                f.seek(start * 512)
                f.write(v.read())
        return
    part = '%s@@%d' % (path, start * 512)
    args = ['mformat', '-i', part, '-v', label, '-N', '%08X' % (0x5A000000 + start), '-T', str(count), '-h', '64',
            '-s', '32', '-H', str(start)]
    if fs == 'fat32':
        args += ['-F', '-c', '1']
    subprocess.check_call(args + ['::'])
    with tempfile.TemporaryDirectory() as td:
        for name, data in files:
            src = os.path.join(td, name)
            open(src, 'wb').write(data)
            subprocess.check_call(['mcopy', '-i', part, src, '::/' + name.upper()])


def layout(kind):
    """[(number, start, sectors, type, fs)], extended start and size (0 if none), total sectors"""
    if kind == 'gpt':
        p = [(1, 2048, 16 * MB, 0, 'fat16'), (2, 2048 + 17 * MB, 40 * MB, 0, 'fat32'),
             (3, 2048 + 58 * MB, 24 * MB, 0, 'ntfs')]
        return p, 0, 0, 96 * MB
    t3, f3, s3 = (0x06, 'fat16', 16 * MB) if kind == 'nt' else (0x0B, 'fat32', 40 * MB)
    t6, f6 = (0x06, 'fat16') if kind == 'nt' else (0x07, 'ntfs')
    p1 = 2048
    p2 = p1 + 16 * MB + 2048
    p3 = p2 + 16 * MB + 2048
    ext = p3 + s3 + 2048
    l5 = ext + 2048                         # EBR at ext, logical 5 2048 sectors behind it
    ebr2 = l5 + 16 * MB + 2048
    l6 = ebr2 + 2048
    end = l6 + 24 * MB
    parts = [(1, p1, 16 * MB, 0x06, 'fat16'), (2, p2, 16 * MB, 0x06, 'fat16'), (3, p3, s3, t3, f3),
             (5, l5, 16 * MB, 0x06, 'fat16'), (6, l6, 24 * MB, t6, f6)]
    return parts, ext, end - ext, end + 4 * MB


def mkstick(path, kind='nt'):
    parts, ext, extlen, total = layout(kind)
    with open(path, 'wb') as f:
        f.truncate(total * 512)
    with open(path, 'r+b') as f:
        if kind == 'gpt':
            pm = bytearray(512)
            pm[446:462] = entry(0, 0xEE, 1, total - 1)
            pm[440:444] = struct.pack('<I', 0x47505431)
            pm[510:] = b'\x55\xaa'
            f.write(pm)
            ents = bytearray(128 * 128)
            basic = uuid.UUID('EBD0A0A2-B9E5-4433-87C0-68B6B72699C7').bytes_le
            for i, (n, start, cnt, t, fs) in enumerate(parts):
                ents[i * 128:(i + 1) * 128] = (basic + uuid.uuid4().bytes_le +
                                               struct.pack('<QQQ', start, start + cnt - 1, 0) +
                                               ('PART%d' % n).encode('utf-16-le').ljust(72, b'\0'))
            for hdr_lba, alt_lba, ent_lba in ((1, total - 1, 2), (total - 1, 1, total - 33)):
                h = bytearray(92)
                h[0:8] = b'EFI PART'
                struct.pack_into('<IIIIQQQQ16sQIII', h, 8, 0x10000, 92, 0, 0, hdr_lba, alt_lba, 34, total - 34,
                                 uuid.UUID('5553424e-542d-5041-5254-000000000001').bytes_le, ent_lba, 128, 128,
                                 zlib.crc32(ents))
                struct.pack_into('<I', h, 16, zlib.crc32(h))
                f.seek(ent_lba * 512)
                f.write(ents)
                f.seek(hdr_lba * 512)
                f.write(h.ljust(512, b'\0'))
        else:
            mbr = bytearray(512)
            mbr[440:444] = struct.pack('<I', 0x5553424E)     # disk signature
            prim = [p for p in parts if p[0] <= 4]
            for i, (n, start, cnt, t, fs) in enumerate(prim):
                mbr[446 + 16 * i:462 + 16 * i] = entry(0x80 if n == 1 else 0, t, start, cnt)
            mbr[446 + 16 * 3:462 + 16 * 3] = entry(0, 0x05, ext, extlen)
            mbr[510:] = b'\x55\xaa'
            f.write(mbr)
            logical = [p for p in parts if p[0] > 4]
            for i, (n, start, cnt, t, fs) in enumerate(logical):
                ebr = start - 2048
                e = bytearray(512)
                e[446:462] = entry(0, t, start, cnt, rel=start - ebr)
                if i + 1 < len(logical):
                    nxt = logical[i + 1][1] - 2048
                    nend = logical[i + 1][1] + logical[i + 1][2]
                    e[462:478] = entry(0, 0x05, nxt, nend - nxt, rel=nxt - ext)
                e[510:] = b'\x55\xaa'
                f.seek(ebr * 512)
                f.write(e)
    for n, start, cnt, t, fs in parts:
        fmt(path, start, cnt, fs, 'PART%d' % n, [('p%d.txt' % n, b'PARTITION %d\r\n' % n)])
    return parts


if __name__ == '__main__':
    for p in mkstick(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'nt'):
        print(p)
