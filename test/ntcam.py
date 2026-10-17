"""USB camera test in an NT guest: the usb-uvc test camera on a USB controller,
usbcam.exe /test lists it, takes a photo (BMP + JPEG) and records an AVI; the
host checks the log, the colours of the photo and the AVI file.

  ntcam.py [uhci|ohci|ehci|xhci] [--nt4] [bulk] [yuy2]
  ntcam.py [hc] [--nt4] allmodes [mjpeg420] [force=N] [smallmax]
  ntcam.py [hc] [--nt4] gui [mjpeg420] ...
      the usbcam window with every picture item in turn: live picture,
      photo, stop, close (usbcam /gui)
      every format and size in turn (usbcam /allmodes), with camera quirks:
      4:2:0 MJPEG, a camera that answers every probe with frame N, a camera
      that reports a far too small dwMaxVideoFrameSize
"""
import os, re, struct, sys, time, io
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, '/home/claude/audiont/tools')

HC = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}
# the test camera's colour bars (white, yellow, cyan, green, magenta, red, blue, black)
BARS = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0), (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]


def main():
    hc = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'xhci'
    nt4 = '--nt4' in sys.argv
    bulk = 'bulk' in sys.argv
    yuy2 = 'yuy2' in sys.argv
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'usbwork-cam.raw')
    vm.sh('cp', '--sparse=always', img, work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.environ.get('USBCAM_EXE', os.path.join(ROOT, 'build/out/usbcam.exe')), '/USBNT/USBCAM.EXE')
    com2 = os.path.join(vm.OUT, 'com2.log')
    dev = 'usb-uvc,id=cam,bus=hc.0,debug=1%s' % (',bulk=on' if bulk else '')
    allm = 'allmodes' in sys.argv
    force = 0
    for a in sys.argv:
        if a.startswith('force='):
            force = int(a[6:])
            dev += ',force-frame=%d' % force
    if 'mjpeg420' in sys.argv:
        dev += ',mjpeg420=on'
    if 'smallmax' in sys.argv:
        dev += ',small-maxframe=on'
    usb = ['-device', '%s,id=hc' % HC[hc], '-device', dev, '-serial', 'file:' + com2]
    fs = hc in ('uhci', 'ohci')
    # format 0 = YUY2, 1 = MJPEG; the largest size full speed can carry at a few fps
    fmt = 0 if yuy2 else 1
    res = (0 if fs else 2) if yuy2 else (0 if fs else 1)
    secs = 6
    bat = ['@echo off',
           'C:\\USBNT\\USBCAM.EXE /test C:\\USBNT\\CAM.TXT /list /fmt %d /res %d /frames 5 /snap C:\\USBNT\\CAM.BMP '
           '/jpeg C:\\USBNT\\CAM.JPG /record C:\\USBNT\\CAM.AVI %d' % (fmt, res, secs),
           'C:\\USBNT\\USBCAM.EXE /test C:\\USBNT\\CAM2.TXT /fmt %d /res %d /bright 50 /snap C:\\USBNT\\CAM2.BMP' % (fmt, res),
           'type C:\\USBNT\\CAM.TXT', 'type C:\\USBNT\\CAM2.TXT', '']
    gui = 'gui' in sys.argv
    vfw = 'vfw' in sys.argv
    if vfw:
        # usbntcap.dll as the Video for Windows driver, captest (avicap32) grabs and captures
        vm.mcopy(work, os.path.join(ROOT, 'build/out/usbntcap.dll'), '/%s/SYSTEM32/USBNTCAP.DLL' % win)
        vm.mcopy(work, os.path.join(ROOT, 'build/out/captest.exe'), '/USBNT/CAPTEST.EXE')
        from ntsetup import set_values
        hv = '/tmp/cam-software'
        vm.mget(work, '/%s/SYSTEM32/CONFIG/SOFTWARE' % win, hv)
        set_values(hv, '\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32', {'msvideo': 'string:usbntcap.dll'})
        vm.mcopy(work, hv, '/%s/SYSTEM32/CONFIG/SOFTWARE' % win)
        bat = ['@echo off', 'echo x > C:\\%s\\usbntcap.log' % win, 'C:\\USBNT\\CAPTEST.EXE vfw > C:\\USBNT\\CAM.TXT',
               'type C:\\USBNT\\CAM.TXT', 'type C:\\%s\\usbntcap.log' % win, '']
    if gui:
        bat = ['@echo off']
        for i in range(5):
            bat.append('C:\\USBNT\\USBCAM.EXE /gui %d 4 C:\\USBNT\\CAM.TXT' % i)
        bat += ['type C:\\USBNT\\CAM.TXT', '']
    elif allm:
        bat = ['@echo off', 'C:\\USBNT\\USBCAM.EXE /test C:\\USBNT\\CAM.TXT /list /allmodes',
               'type C:\\USBNT\\CAM.TXT', '']
    open('/tmp/cam-run.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/cam-run.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    if os.path.exists(com2):
        os.remove(com2)
    v = vm.VM(work, 'none', usb)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        v.key('ctrl-alt-delete', 4); v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    done = v.wait_serial('##AGENT-SHUTDOWN##', 400)
    time.sleep(5)
    v.shot('cam')
    v.quit()
    out = vm.OUT
    got = {}
    for f in ('CAM.TXT', 'CAM2.TXT', 'CAM.BMP', 'CAM2.BMP', 'CAM.JPG', 'CAM.AVI'):
        p = os.path.join(out, f)
        if os.path.exists(p):
            os.remove(p)
        got[f] = vm.mget(work, '/USBNT/' + f, p)
    log = open(os.path.join(out, 'CAM.TXT'), 'rb').read().decode('latin1') if got['CAM.TXT'] else ''
    log2 = open(os.path.join(out, 'CAM2.TXT'), 'rb').read().decode('latin1') if got['CAM2.TXT'] else ''
    print(log)
    print(log2)
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    print('\n'.join(l for l in ulog.splitlines() if 'video' in l or 'camera' in l or 'format' in l))
    checks = []
    checks.append(('guest finished', bool(done)))
    if vfw:
        seq = re.search(r'CAPTEST vfw sequence (\d+) frames', log)
        checks.append(('vfw connect', 'CAPTEST vfw connect OK' in log))
        checks.append(('vfw frames', 'CAPTEST vfw grab OK' in log))
        checks.append(('vfw sequence', seq is not None and int(seq.group(1)) >= 5))
        ok = True
        for name, c in checks:
            ok &= bool(c)
            print('%s %s' % ('ok  ' if c else 'FAIL', name))
        print('RESULT camera-vfw %s%s %s' % (hc, ' nt4' if nt4 else '', 'PASS' if ok else 'FAIL'))
        return
    if gui:
        g = re.findall(r'GUI item=(\d+) (\w+) (\d+)x(\d+) frames=(\d+) damaged=(\d+) picture=(\d+)x(\d+) photo=(\S+)', log)
        checks.append(('5 picture items ran and closed', len(g) == 5))
        for m in g:
            name = 'item %s %s %sx%s' % m[:4]
            checks.append((name + ' live picture', int(m[4]) >= (3 if fs else 10) and m[6] != '0'))   # full speed: a few fps
            checks.append((name + ' photo', m[8] != 'none'))
        ok = True
        for name, c in checks:
            ok &= bool(c)
            print('%s %s' % ('ok  ' if c else 'FAIL', name))
        print('RESULT camera-gui %s%s %s %s' % (hc, ' nt4' if nt4 else '', ' '.join(a for a in sys.argv[2:]
                                               if a != hc), 'PASS' if ok else 'FAIL'))
        return
    if allm:
        # YUY2 160x120 320x240 640x480, MJPEG 320x240 640x480
        sizes = {0: [(160, 120), (320, 240), (640, 480)], 1: [(320, 240), (640, 480)]}
        modes = re.findall(r'MODE (\d+) (\d+) (\w+) (\d+)x(\d+) stream=(\d+)x(\d+) max=(\d+) frames=(\d+) '
                           r'decoded=(\d+) picture=(\d+)x(\d+)', log)
        checks.append(('all 5 modes ran', len(modes) == 5 and 'start-failed' not in log and 'no-memory' not in log))
        for m in modes:
            f, r = int(m[0]), int(m[1])
            want = sizes[f][min(force, len(sizes[f])) - 1] if force else sizes[f][r]
            sw, sh, pw, ph = int(m[5]), int(m[6]), int(m[10]), int(m[11])
            name = 'mode %s %sx%s' % (m[2], m[3], m[4])
            # full speed cannot carry the large uncompressed sizes in full: some pictures still
            checks.append((name + ' pictures', int(m[9]) >= (1 if fs else 4)))
            checks.append((name + ' size %dx%d' % want, (sw, sh) == want and (pw, ph) == want))
        checks.append(('driver log', 'chose' in ulog if force else True))
        ok = True
        for name, c in checks:
            ok &= bool(c)
            print('%s %s' % ('ok  ' if c else 'FAIL', name))
        print('RESULT camera-allmodes %s%s %s %s' % (hc, ' nt4' if nt4 else '', ' '.join(a for a in sys.argv[2:]
                                                     if a != hc), 'PASS' if ok else 'FAIL'))
        return
    checks.append(('camera listed', 'CAM 1 ' in log and 'QEMU UVC Test Camera' in log))
    checks.append(('formats', 'FMT 0 YUY2' in log and 'FMT 1 MJPG' in log))
    checks.append(('stream started', 'START ok' in log))
    fr = re.findall(r'FRAME seq=\d+ bytes=(\d+) flags=(\w+) decode=(\d)', log)
    checks.append(('5 frames decoded', len(fr) == 5 and all(d == '1' for _, _, d in fr)))
    bars_ok = False
    m = re.search(r'COLOR avgR=\d+ avgG=\d+ avgB=\d+ bars= (.*)', log)
    if m:
        vals = [tuple(int(x) for x in t.split(',')) for t in m.group(1).split()]
        bars_ok = len(vals) == 8 and all(max(abs(a - b) for a, b in zip(v1, v2)) <= 24 for v1, v2 in zip(vals, BARS))
        print('bars', vals)
    checks.append(('photo colours', bars_ok))
    try:
        from PIL import Image
        im = Image.open(os.path.join(out, 'CAM.BMP')).convert('RGB')
        w, h = im.size
        px = [im.getpixel((b * w // 8 + w // 16, h // 16)) for b in range(8)]
        checks.append(('BMP file', all(max(abs(a - b) for a, b in zip(p, q)) <= 24 for p, q in zip(px, BARS))))
        if not yuy2:
            jm = Image.open(os.path.join(out, 'CAM.JPG')).convert('RGB')
            jp = [jm.getpixel((b * w // 8 + w // 16, h // 16)) for b in range(8)]
            checks.append(('JPEG file opens in other programs', jm.size == (w, h) and
                           all(max(abs(a - b) for a, b in zip(p, q)) <= 24 for p, q in zip(jp, BARS))))
        im2 = Image.open(os.path.join(out, 'CAM2.BMP')).convert('RGB')
        checks.append(('brightness control', im2.getpixel((w - w // 16, h // 16))[0] >= 30 and 'BRIGHT ok value=50' in log2))
    except Exception as e:
        print('image check:', e)
        checks.append(('image files', False))
    m = re.search(r'RECORD ok frames=(\d+) written=(\d+) damaged=(\d+) ms=(\d+) fps=(\d+)', log)
    rec_ok = False
    if m and got['CAM.AVI']:
        frames, written, fps = int(m.group(1)), int(m.group(2)), int(m.group(5))
        d = open(os.path.join(out, 'CAM.AVI'), 'rb').read()
        riff = d[:4] == b'RIFF' and struct.unpack('<I', d[4:8])[0] == len(d) - 8 and b'idx1' in d
        n = d.count(b'00dc') - 1                     # chunk ids, minus the ones in idx1 ... roughly
        tot = struct.unpack('<I', d[48:52])[0]       # avih.dwTotalFrames
        print('avi: %d bytes, total frames %d, written %d, fps %d' % (len(d), tot, written, fps))
        want = (3 if fs else 10) if yuy2 else (5 if fs else 15)
        rec_ok = riff and tot == written and written >= want * secs // 2
        try:
            import subprocess
            r = subprocess.run(['ffprobe', '-v', 'error', '-count_frames', '-show_entries',
                                'stream=codec_name,width,height,nb_read_frames', '-of', 'csv',
                                os.path.join(out, 'CAM.AVI')], capture_output=True, text=True)
            print('ffprobe:', r.stdout.strip(), r.stderr.strip()[:200])
            rec_ok = rec_ok and ('stream,' in r.stdout) and not r.stderr.strip()
        except OSError:
            pass
    checks.append(('AVI recording', rec_ok))
    ok = True
    for name, c in checks:
        ok &= bool(c)
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
    print('RESULT camera %s%s%s%s %s' % (hc, ' nt4' if nt4 else '', ' bulk' if bulk else '', ' yuy2' if yuy2 else '',
                                        'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
