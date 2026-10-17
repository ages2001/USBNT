#!/usr/bin/env python3
"""Makes PAGEFILE.SYS in the root of a FAT16 volume one contiguous run (NT
refuses a fragmented paging file on some file systems).  contigpf.py IMG OFFSET"""
import struct, sys


def main(img, off):
    f = open(img, 'r+b'); f.seek(off); bs = f.read(512)
    bps, spc, res, nf, rootn, tot16, _, spf = struct.unpack('<HBHBHHBH', bs[11:24])
    tot = tot16 or struct.unpack('<I', bs[32:36])[0]
    fat_off = off + res * bps
    f.seek(fat_off); fat = bytearray(f.read(spf * bps))
    root_off = fat_off + nf * spf * bps
    data_sec = res + nf * spf + (rootn * 32 + bps - 1) // bps
    nclus = (tot - data_sec) // spc + 2
    f.seek(root_off); root = bytearray(f.read(rootn * 32))
    g = lambda c: struct.unpack('<H', fat[2 * c:2 * c + 2])[0]
    def s(c, v): fat[2 * c:2 * c + 2] = struct.pack('<H', v)
    for i in range(0, len(root), 32):
        if root[i:i + 11] == b'PAGEFILESYS':
            c = struct.unpack('<H', root[i + 26:i + 28])[0]
            chain = []
            while c < 0xFFF8:
                chain.append(c); c = g(c)
            for c in chain:
                s(c, 0)
            n = len(chain)
            best, run, start = None, 0, 2
            for c in range(2, nclus):
                if g(c) == 0:
                    run += 1
                    if run == 1: start = c
                    if run >= n and best is None: best = start
                else:
                    run = 0
            if best is None:
                raise SystemExit('no contiguous room for %d clusters' % n)
            for k in range(n):
                s(best + k, best + k + 1 if k < n - 1 else 0xFFFF)
            root[i + 26:i + 28] = struct.pack('<H', best)
            for k in range(nf):
                f.seek(fat_off + k * spf * bps); f.write(fat)
            f.seek(root_off); f.write(root)
            print('PAGEFILE.SYS: %d clusters at %d' % (n, best))
            return
    print('no PAGEFILE.SYS')


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]))
