"""USB camera on Windows 2000 / XP x64 through USBNT's camera driver
(WindowsCamera=0) and usbntcap.dll: Video for Windows
(avicap32) and DirectShow (the VFW Capture source), with the usb-uvc test
camera of the patched QEMU; on x64 for 32 and 64 bit programs.

  python3 w5cam.py [w2k|xp64 ...] [--keep]
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5, w5usb

OUT = os.path.join(HERE, '..', 'build', 'out')


def run(guest, keep=False):
    g = vm5.GUESTS[guest]
    bat = [
        'start /b C:\\USBNT\\FSTEST.EXE clicker C:\\USBNT\\STOP 900000',
        # USBNT's own camera driver (XP x64 has usbvideo.sys: w5wincam.py tests that way)
        'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set WindowsCamera=0',
        'C:\\USBNT\\FSTEST.EXE sleep 5000',
        'C:\\USBNT\\FSTEST.EXE write C:\\%s\\usbntcap.log x' % vm5.GUESTS[guest]['win'],
        'C:\\USBNT\\CAPTEST.EXE',
    ]
    files = w5usb.driver_files() + [(os.path.join(OUT, 'captest.exe'), '/USBNT/CAPTEST.EXE')]
    if g['x64']:
        files.append((os.path.join(OUT, '2kxp', 'amd64', 'captest.exe'), '/USBNT/2KXP/AMD64/CAPTEST.EXE'))
        bat.append('C:\\USBNT\\2KXP\\AMD64\\CAPTEST.EXE vfw')
    bat += ['type C:\\%s\\usbntcap.log' % g['win'],
            'C:\\USBNT\\FSTEST.EXE write C:\\USBNT\\STOP x', 'C:\\USBNT\\FSTEST.EXE sleep 2000']
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, os.environ.get('W5_PREFIX', 'work-') + 'cam-%s.raw' % guest),
                          files=files, bat=bat)
    w5usb.no_signing_prompt(work, g)
    extra = ['-device', 'qemu-xhci,id=hc', '-device', 'usb-uvc,id=cam,bus=hc.0']
    vm = vm5.VM(guest, work, extra=extra, name='cam-' + guest)
    ok = vm.wait_serial('##AGENT-DONE##', 900, shots=60)
    time.sleep(2)
    vm.shot('end')
    out = vm.serial()
    log = vm.log()
    vm.quit()
    print(out[-7000:])
    print('---- COM2 (usbnt.sys) ----')
    print(log[-4000:])
    seq = [int(x) for x in re.findall(r'CAPTEST vfw sequence (\d+) frames', out)]
    ds = re.search(r'CAPTEST dshow frames (\d+)', out)
    checks = {
        'agent finished': ok,
        'vfw driver listed': 'USBNT USB Video Camera' in out,
        'vfw connect': 'CAPTEST vfw connect OK' in out,
        'vfw frames': 'CAPTEST vfw grab OK' in out,
        'vfw sequence': len(seq) > 0 and seq[0] >= 5,
        'dshow device': 'CAPTEST dshow device: USBNT' in out,
        'dshow frames': ds is not None and int(ds.group(1)) >= 5,
    }
    if g['x64']:
        checks['64 bit vfw'] = len(seq) > 1 and seq[1] >= 5
    for k, v in checks.items():
        print('%-18s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    return all(checks.values())


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    res = {gname: run(gname, '--keep' in sys.argv) for gname in (args or ['w2k', 'xp64'])}
    print(res)
    sys.exit(0 if all(res.values()) else 1)
