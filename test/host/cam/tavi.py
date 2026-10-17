"""Writes AVIs with avi.c and checks them with a RIFF parser (and PIL for the frames)."""
import io, struct, subprocess, sys
from PIL import Image
T = '/tmp/claude-0/tavi'
subprocess.check_call(['gcc', '-O2', '-o', T, 'test/host/cam/tavi.c', 'tools/avi.c'])
ok = True

def parse(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'AVI ', 'riff'
    assert struct.unpack('<I', d[4:8])[0] == len(d) - 8, 'riff size'
    chunks = {}
    def walk(off, end, depth):
        while off + 8 <= end:
            cid = d[off:off + 4]; sz = struct.unpack('<I', d[off + 4:off + 8])[0]
            if cid == b'LIST':
                lt = d[off + 8:off + 12]
                chunks.setdefault(b'LIST' + lt, []).append((off, sz))
                walk(off + 12, off + 8 + sz, depth + 1)
            else:
                chunks.setdefault(cid, []).append((off, sz))
            off += 8 + sz + (sz & 1)
        assert off == end, 'chunk sizes do not add up at depth %d (%d != %d)' % (depth, off, end)
    walk(12, len(d), 0)
    return d, chunks

def check(path, n, fcc, jpeg=None):
    global ok
    d, c = parse(path)
    avih = d[c[b'avih'][0][0] + 8:][:56]
    us, _, _, flags, frames = struct.unpack('<5I', avih[:20])
    strh = d[c[b'strh'][0][0] + 8:][:56]
    typ, handler = strh[:4], strh[4:8]
    scale, rate, start, length = struct.unpack('<4I', strh[20:36])
    strf = d[c[b'strf'][0][0] + 8:][:40]
    w, h = struct.unpack('<ii', strf[4:12]); comp = strf[16:20]
    frames_ck = c.get(b'00dc', [])
    idx_off, idx_sz = c[b'idx1'][0]
    movi = c[b'LISTmovi'][0][0] + 8                 # offset of the 'movi' fourcc
    entries = [struct.unpack('<4sIII', d[idx_off + 8 + 16 * i:][:16]) for i in range(idx_sz // 16)]
    good = (frames == n == length == len(frames_ck) == len(entries) and us == 40000 and scale == 40000 and rate == 1000000
            and typ == b'vids' and bool(flags & 0x10))
    for (cid, fl, off, sz), (coff, csz) in zip(entries, frames_ck):
        good = good and cid == b'00dc' and movi + off == coff and sz == csz and bool(fl & 0x10)
    if jpeg:
        first = d[frames_ck[0][0] + 8:][:frames_ck[0][1]]
        im = Image.open(io.BytesIO(first)); im.load()
        good = good and im.size == (w, h) and comp == b'MJPG' and handler == b'MJPG'
    print('%s %s: %d frames, %dx%d %r, %d us/frame' % ('ok  ' if good else 'FAIL', path, frames, w, h, comp, us))
    ok &= bool(good)

subprocess.check_call([T, '/tmp/claude-0/t_dib.avi', '25'])
check('/tmp/claude-0/t_dib.avi', 25, 0)
subprocess.check_call([T, '/tmp/claude-0/t_mjpg.avi', '30', '/tmp/claude-0/uvc.jpg'])
check('/tmp/claude-0/t_mjpg.avi', 30, 'MJPG', jpeg=True)
print('RESULT avi %s' % ('PASS' if ok else 'FAIL'))
