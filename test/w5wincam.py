"""Cameras on Windows XP x64 (which has usbvideo.sys, the USB Video Class
driver of XP SP2 / Server 2003 SP1): usbinst sets WindowsCamera = 1, USBNT's
own camera driver leaves the camera alone, the camera (QEMU's usb-uvc test
camera) is reported on the USBNT device bus, usbccgp splits it and Windows'
usbvideo.sys streams it through USBNT's USB interface (isochronous URBs).
usbcam shows it through Video for Windows ("Microsoft WDM Image Capture"):
list, frames, a photo (BMP, colour bars checked), an AVI; captest checks
avicap32 and DirectShow for 32 and 64 bit programs; usbntcap.dll is not
registered.  hc=ehci puts the camera on the EHCI (Windows' own usbehci).

On Windows 2000 (no usbvideo.sys) WindowsCamera stays 0 and usbcam shows
USBNT's camera as before (w5cam.py).

  python3 w5wincam.py [xp64|w2k] [hc=xhci|ehci] [--keep]

W5_UVC adds properties to the test camera.  Found so far: XP x64's
usbvideo.sys does not start on QEMU's usb-uvc (Code 10 before its first
request), on USBNT's xHCI as on Windows' own EHCI (hc=ehci), and also with a
UVC 1.0 variant (a usb-uvc with a "uvc10" property: W5_UVC=,uvc10=on); the
checks after "usbccgp runs" fail for that reason, not because of USBNT.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5, w5usb

OUT = os.path.join(HERE, '..', 'build', 'out')
PREFIX = os.environ.get('W5_PREFIX', 'wt-native-')
U = 'C:\\USBNT\\'
F = U + 'FSTEST.EXE'


def run(guest, hc='xhci', keep=False):
    g = vm5.GUESTS[guest]
    win = g['win']
    bat = ['start /b %s clicker %sSTOP 900000' % (F, U),
           '%sUSBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=%s'
           % (U, os.environ.get('W5_DEBUG', '2')),
           '%s sleep 40000' % F,
           '%sUSBINST.EXE /status' % U,
           '%s svcstat usbccgp' % F,
           '%s svcstat usbvideo' % F,
           'reg query "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32" /v MSVideo8',
           'reg query "HKLM\\Software\\Wow6432Node\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32" /v MSVideo8',
           'reg query "HKLM\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Drivers32" /v msvideo',
           '%sUSBCAM.EXE /test %sCAM1.LOG /list' % (U, U),
           'type %sCAM1.LOG' % U,
           '%sUSBCAM.EXE /test %sCAM2.LOG /cam 1 /frames 5 /snap %sSNAP.BMP /record %sREC.AVI 4' % (U, U, U, U),
           'type %sCAM2.LOG' % U,
           '%sUSBCAM.EXE /gui 0 4 %sGUI.LOG 1' % (U, U),
           'type %sGUI.LOG' % U,
           '%sCAPTEST.EXE' % U]
    files = w5usb.driver_files() + [(os.path.join(OUT, 'captest.exe'), '/USBNT/CAPTEST.EXE')]
    if g['x64']:
        files.append((os.path.join(OUT, '2kxp', 'amd64', 'captest.exe'), '/USBNT/2KXP/AMD64/CAPTEST.EXE'))
        bat.append('%s2KXP\\AMD64\\CAPTEST.EXE' % U)
    bat += ['dir %s*.BMP %s*.AVI' % (U, U),
            '%s write %sSTOP x' % (F, U), '%s sleep 2000' % F]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, PREFIX + 'wcam-%s.raw' % guest), files=files, bat=bat)
    w5usb.no_signing_prompt(work, g)
    cam = 'usb-uvc,id=cam,bus=hc.0' + os.environ.get('W5_UVC', '')     # e.g. ",uvc10=on"
    if hc == 'ehci':
        extra = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-ehci,id=hc', '-device', cam]
    else:
        extra = ['-device', 'qemu-xhci,id=hc', '-device', cam]
    name = PREFIX + 'wcam-%s-%s' % (guest, hc)
    vm = vm5.VM(guest, work, extra=extra, name=name)
    ok = vm.wait_serial('##AGENT-DONE##', 1200, shots=120)
    time.sleep(2)
    vm.shot('end')
    out, log = vm.serial(), vm.log()
    vm.wait_serial('##AGENT-SHUTDOWN##', 60)
    vm.quit()
    snap = os.path.join(vm.dir, 'snap.bmp')
    got_snap = vm5.mget(work, '/USBNT/SNAP.BMP', snap)
    print(out[-9000:])
    print('---- COM2 (usbnt.sys) ----')
    print(log[-6000:])
    seq = [int(x) for x in re.findall(r'CAPTEST vfw sequence (\d+) frames', out)]
    ds = [int(x) for x in re.findall(r'CAPTEST dshow frames (\d+)', out)]
    rec = re.search(r'RECORD ok frames=(\d+) written=(\d+)', out)
    bars = re.search(r'COLOR avgR=\d+ avgG=\d+ avgB=\d+ bars=((?: \d+,\d+,\d+){8})', out)
    windows = guest == 'xp64'
    checks = {'agent finished': ok}
    if windows:
        checks.update({
            'WindowsCamera = 1': 'WindowsCamera = 1' in out,
            'camera to Windows': hc == 'ehci' or ("to Windows' USB drivers" in log and 'uvc:' not in log),
            'usbvideo runs': 'svcstat usbvideo state 4' in out,
            'usbccgp runs': 'svcstat usbccgp state 4' in out,
            'usbcam lists it': re.search(r'USBNT 0 WINDOWS 1', out) is not None,
            'usbntcap not listed': 'USBNT USB Video Camera' not in out,
            'usbcam started': 'START ok' in out and ' vfw' in out,
        })
    else:
        checks.update({
            'WindowsCamera = 0': 'WindowsCamera = 0' in out,
            'usbcam lists USBNT camera': re.search(r'USBNT 1 WINDOWS 0', out) is not None,
            'usbcam started': 'START ok' in out,
        })
    checks.update({
        'frames': len(re.findall(r'FRAME seq=\d+ bytes=\d+ flags=\w+ decode=1', out)) >= 5,
        'photo': re.search(r'SNAP ok \d+x\d+', out) is not None and got_snap,
        'colour bars': bars is not None,
        'video': rec is not None and int(rec.group(2)) >= 8,
        'window': re.search(r'GUI item=0 \w+ \d+x\d+ frames=(\d+)', out) is not None and
                  int(re.search(r'GUI item=0 \w+ \d+x\d+ frames=(\d+)', out).group(1)) >= 5,
        'vfw (32 bit)': len(seq) > 0 and seq[0] >= 5,
        'dshow (32 bit)': len(ds) > 0 and ds[0] >= 5,
    })
    if g['x64']:
        checks['vfw (64 bit)'] = len(seq) > 1 and seq[1] >= 5
        checks['dshow (64 bit)'] = len(ds) > 1 and ds[1] >= 5
    if bars:
        v = [tuple(int(c) for c in b.split(',')) for b in bars.group(1).split()]
        print('bars', v)
        # the test camera's colour bars: white, yellow, cyan, green, magenta, red, blue, black
        checks['colour bars'] = v[0][0] > 180 and v[0][2] > 180 and v[7][0] < 60 and v[1][2] < 90 and v[6][0] < 90
    for k, v in checks.items():
        print('%-24s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    return all(checks.values())


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--') and '=' not in a]
    hc = ([a.split('=')[1] for a in sys.argv[1:] if a.startswith('hc=')] or ['xhci'])[0]
    res = {gname: run(gname, hc, '--keep' in sys.argv) for gname in (args or ['xp64'])}
    print(res)
    sys.exit(0 if all(res.values()) else 1)
