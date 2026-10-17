"""Compares jpegdec.c with PIL on generated JPEGs (4:2:0, 4:2:2, 4:4:4, grey,
restart intervals, odd sizes, DHT stripped, camera frames from dev-uvc)."""
import io, os, struct, subprocess, sys
from PIL import Image, ImageDraw, ImageFilter
T = sys.argv[1] if len(sys.argv) > 1 else '/tmp/claude-0/tjpeg'
D = '/tmp/claude-0/jt'
os.makedirs(D, exist_ok=True)

def picture(w, h, mode='RGB'):
    im = Image.new('RGB', (w, h))
    dr = ImageDraw.Draw(im)
    for x in range(w):
        dr.line([(x, 0), (x, h)], fill=(x * 255 // w, (x * 7) % 256, 255 - x * 255 // w))
    dr.ellipse([w // 4, h // 4, 3 * w // 4, 3 * h // 4], fill=(240, 200, 30))
    dr.rectangle([10, 10, 60, 40], fill=(0, 0, 255))
    dr.text((20, h // 2), "USBNT camera", fill=(255, 255, 255))
    im = im.filter(ImageFilter.GaussianBlur(0.7))
    return im.convert(mode)

def strip_dht(data):
    out = bytearray(data[:2]); i = 2
    while i < len(data):
        if data[i] != 0xFF: out += data[i:]; break
        m = data[i + 1]
        if m == 0xDA: out += data[i:]; break
        ln = struct.unpack('>H', data[i + 2:i + 4])[0]
        if m != 0xC4: out += data[i:i + 2 + ln]
        i += 2 + ln
    return bytes(out)

def check(name, data, tol=3.0):
    p = os.path.join(D, name + '.jpg'); open(p, 'wb').write(data)
    q = os.path.join(D, name + '.ppm')
    r = subprocess.run([T, p, q], capture_output=True, text=True)
    ref = Image.open(io.BytesIO(data)).convert('RGB')
    if r.returncode != 0:
        print('FAIL', name, r.stdout.strip()); return False
    got = Image.open(q).convert('RGB')
    a, b = ref.tobytes(), got.tobytes()
    mae = sum(abs(x - y) for x, y in zip(a, b)) / len(a)
    worst = max(abs(x - y) for x, y in zip(a, b))
    ok = mae < tol and ref.size == got.size
    print('%s %-28s %dx%d mae=%.2f max=%d' % ('ok  ' if ok else 'FAIL', name, ref.size[0], ref.size[1], mae, worst))
    return ok

def enc(im, **kw):
    b = io.BytesIO(); im.save(b, 'JPEG', **kw); return b.getvalue()

ok = True
im = picture(320, 240)
ok &= check('s420', enc(im, quality=85, subsampling=2))
ok &= check('s422', enc(im, quality=85, subsampling=1))
ok &= check('s444', enc(im, quality=92, subsampling=0))
ok &= check('grey', enc(picture(320, 240, 'L'), quality=85))
ok &= check('odd641x479', enc(picture(641, 479), quality=80, subsampling=2))
ok &= check('hd1280x720', enc(picture(1280, 720), quality=80, subsampling=2))
ok &= check('fhd1920x1080', enc(picture(1920, 1080), quality=80, subsampling=2))
ok &= check('w4096x64', enc(picture(4096, 64), quality=80, subsampling=2))
ok &= check('odd17x9', enc(picture(17, 9), quality=90, subsampling=1))
ok &= check('q30', enc(im, quality=30, subsampling=2), 4.0)
ok &= check('q100', enc(im, quality=100, subsampling=0))
# restart intervals (PIL/libjpeg: restart_marker_blocks or restart_marker_rows)
try:
    ok &= check('restart', enc(im, quality=85, subsampling=2, restart_marker_blocks=3))
except TypeError:
    print('skip restart (PIL too old)')
# DHT stripped: optimize=False keeps the standard tables
d = enc(im, quality=85, subsampling=1, optimize=False)
nd = strip_dht(d)
assert nd != d
p = os.path.join(D, 'nodht.jpg'); open(p, 'wb').write(nd)
q = os.path.join(D, 'withdht.jpg')
r = subprocess.run([T, p, q, 'dht'], capture_output=True, text=True)
print(r.stdout.strip())
same = Image.open(q).convert('RGB').tobytes() == Image.open(io.BytesIO(d)).convert('RGB').tobytes()
ok &= r.returncode == 0 and same       # viewers decode it like the original
print('%s DHT re-insertion: PIL decodes it like the original' % ('ok  ' if same else 'FAIL'))
# decode the stripped one directly (default tables), compare with PIL on the original
r = subprocess.run([T, p, os.path.join(D, 'nodht.ppm')], capture_output=True, text=True)
got = Image.open(os.path.join(D, 'nodht.ppm')).convert('RGB').tobytes()
ref = Image.open(io.BytesIO(d)).convert('RGB').tobytes()
mae = sum(abs(x - y) for x, y in zip(ref, got)) / len(ref)
print('%s DHT-less decode mae=%.2f' % ('ok  ' if mae < 3 else 'FAIL', mae)); ok &= mae < 3
for f in ('/tmp/claude-0/uvc.jpg',):
    if os.path.exists(f):
        ok &= check('dev-uvc', open(f, 'rb').read())
print('RESULT jpegdec %s' % ('PASS' if ok else 'FAIL'))
