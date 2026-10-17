"""The controller policy of usbinst on Windows 2000 SP0 and XP x64 SP1, as an
upgrade from USBNT 1.1 (which gave every USB controller to usbnt.sys and
whose INF won Windows' own driver search with Microsoft's ids):

  stage 1  USBNT 1.1 from bin/ installs: xHCI, EHCI, UHCI all usbnt.sys
  stage 2  this usbinst /install: the UHCI goes back to Windows' driver
           (2000: uhcd, XP: usbuhci), the EHCI too where Windows has one
           (XP x64: usbehci; 2000 SP0 has none: it stays USBNT's), the xHCI
           stays USBNT's; the old INF leaves the INF folder
  stage 3  after a restart: the keyboard and mouse on the UHCI through
           Windows' hidusb / kbdhid / mouhid, a stick on the EHCI (usbstor
           on XP x64, a USBNT disk on 2000), a stick on the xHCI (USBNT),
           both read and written

  python3 w5native.py [w2k|xp64 ...] [--keep] [--fresh] [--comp]

--fresh skips stage 1 (a machine that never had USBNT).  --comp: an ICH9
EHCI with its three UHCI companions instead of the separate EHCI and UHCI,
a full speed hub with the keyboard and mouse on an EHCI port: the EHCI
(USBNT's on 2000 SP0) hands it over to a companion (Windows' uhcd).  Work images and
output go to W5_PREFIX-named places (default "wt-native-").
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5, w5usb, ntmsc
from ntmsc import stick, W

ROOT = os.path.join(HERE, '..')
BIN = os.path.join(ROOT, 'bin')
PREFIX = os.environ.get('W5_PREFIX', 'wt-native-')
U = 'C:\\USBNT\\'
F = U + 'FSTEST.EXE'


def old_files():
    """USBNT 1.1 (bin/) under C:\\OLD"""
    f = [(os.path.join(BIN, 'usbinst.exe'), '/OLD/USBINST.EXE')]
    for inf in ('usbnt.inf', 'usbntkbd.inf', 'usbntmou.inf'):
        f.append((os.path.join(BIN, '2kxp', inf), '/OLD/2KXP/' + inf.upper()))
    f.append((os.path.join(BIN, '2kxp', 'i386', 'usbnt.sys'), '/OLD/2KXP/I386/USBNT.SYS'))
    f.append((os.path.join(BIN, '2kxp', 'amd64', 'usbnt.sys'), '/OLD/2KXP/AMD64/USBNT.SYS'))
    f.append((os.path.join(BIN, '2kxp', 'amd64', 'usbinst.exe'), '/OLD/2KXP/AMD64/USBINST.EXE'))
    return f


def boot(guest, work, extra, name, timeout=900):
    vm = vm5.VM(guest, work, extra=extra, name=name)
    ok = vm.wait_serial('##AGENT-DONE##', timeout, shots=120)
    time.sleep(2)
    vm.shot('end')
    out, log = vm.serial(), vm.log()
    vm.wait_serial('##AGENT-SHUTDOWN##', 60)
    t0 = time.time()
    while vm.running() and time.time() - t0 < 90:
        time.sleep(1)
    vm.quit()
    print('==== %s ====' % name)
    print(out[-7000:])
    print('---- COM2 (usbnt.sys) ----')
    print(log[-5000:])
    return ok, out, log


def status_line(out, kind):
    """the driver of the controller of that type in the last usbinst /status"""
    m = re.findall(r'%s controller: .* - (\S+)' % kind, out)
    return m[-1] if m else None


def run(guest, keep=False, fresh=False, comp=False):
    g = vm5.GUESTS[guest]
    os.makedirs(W, exist_ok=True)
    ntmsc.W = os.path.join(W, PREFIX + 'tmp-' + guest)
    os.makedirs(ntmsc.W, exist_ok=True)
    xs = os.path.join(W, PREFIX + 'xstick-%s.img' % guest)
    es = os.path.join(W, PREFIX + 'estick-%s.img' % guest)
    for p, label, fn in ((xs, 'XSTICK', 'xhci.txt'), (es, 'ESTICK', 'ehci.txt')):
        if os.path.exists(p):
            os.remove(p)
        stick(p, 64, label, [(fn, b'hello from ' + fn.encode() + b'\n')])
    files = w5usb.driver_files() + ([] if fresh else old_files())
    stage1 = ['start /b %s clicker %sSTOP 600000' % (F, U),
              'C:\\OLD\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8',
              '%sUSBINST.EXE /status' % U,
              '%s sleep 15000' % F,
              '%s write %sSTOP x' % (F, U), '%s sleep 2000' % F]
    stage2 = ['start /b %s clicker %sSTOP2 600000' % (F, U),
              '%sUSBINST.EXE /status' % U,
              '%sUSBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8' % U,
              '%s sleep 20000' % F,
              '%sUSBINST.EXE /status' % U,
              'dir /b C:\\%s\\inf\\oem*.inf' % g['win'],
              '%s write %sSTOP2 x' % (F, U), '%s sleep 2000' % F]
    stage3 = ['start /b %s clicker %sSTOP3 600000' % (F, U),
              '%s sleep 20000' % F,
              '%sUSBINST.EXE /status' % U,
              '%s find XHCI.TXT 120000' % F,
              '%s cat ?XHCI.TXT:\\XHCI.TXT' % F,
              '%s fill ?XHCI.TXT:\\BIGX.BIN 8' % F,
              '%s sum ?XHCI.TXT:\\BIGX.BIN' % F,
              '%s find EHCI.TXT 120000' % F,
              '%s cat ?EHCI.TXT:\\EHCI.TXT' % F,
              '%s fill ?EHCI.TXT:\\BIGE.BIN 8' % F,
              '%s sum ?EHCI.TXT:\\BIGE.BIN' % F]
    for svc in ('usbnt', 'usbhub', 'hidusb', 'kbdhid', 'mouhid', 'usbstor', 'usbehci', 'usbuhci', 'uhcd'):
        stage3.append('%s svcstat %s' % (F, svc))
    stage3 += ['%s write %sSTOP3 x' % (F, U), '%s sleep 2000' % F]

    tag = guest + ('-comp' if comp else '')
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, PREFIX + 'pol-%s.raw' % tag), files=files,
                          bat=stage2 if fresh else stage1)
    w5usb.no_signing_prompt(work, g)
    if comp:
        hcs = ['-device', 'ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on',
               '-device', 'ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on',
               '-device', 'ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1',
               '-device', 'ich9-usb-uhci3,masterbus=ehci.0,firstport=4,addr=1d.2']
        # QEMU's keyboard and mouse would come up at high speed on the EHCI (and stay USBNT's, rightly);
        # behind QEMU's hub, which is full speed only, they are full speed devices: the hub is handed over
        hcs += ['-device', 'usb-hub,bus=ehci.0,port=2,id=fshub']
        lowbus = 'ehci.0,port=2.1'
        lowbus2 = 'ehci.0,port=2.2'
    else:
        hcs = ['-device', 'usb-ehci,id=ehci', '-device', 'piix3-usb-uhci,id=uhci']
        lowbus = lowbus2 = 'uhci.0'
    extra = ['-device', 'qemu-xhci,id=xhci'] + hcs + [
             '-blockdev', 'driver=file,filename=%s,node-name=xsf' % xs,
             '-blockdev', 'driver=raw,file=xsf,node-name=xs',
             '-device', 'usb-storage,drive=xs,bus=xhci.0,id=xstick',
             '-blockdev', 'driver=file,filename=%s,node-name=esf' % es,
             '-blockdev', 'driver=raw,file=esf,node-name=es',
             '-device', 'usb-storage,drive=es,bus=ehci.0,id=estick',
             '-device', 'usb-kbd,bus=%s' % lowbus, '-device', 'usb-mouse,bus=%s' % lowbus2]
    checks = {}
    if not fresh:
        ok1, out1, log1 = boot(guest, work, extra, PREFIX + 'pol1-' + tag)
        checks['1.1 installed'] = ok1 and all(status_line(out1, k) == 'usbnt' for k in ('xHCI', 'EHCI', 'UHCI'))
        vm5.set_bat(work, stage2)
    ok2, out2, log2 = boot(guest, work, extra, PREFIX + 'pol2-' + tag)
    vm5.set_bat(work, stage3)
    ok3, out3, log3 = boot(guest, work, extra, PREFIX + 'pol3-' + tag)
    win_ehci = 'usbehci' if g['x64'] else 'usbnt'
    win_uhci = 'usbuhci' if g['x64'] else 'uhcd'
    checks.update({
        'stage 2 finished': ok2,
        'old INF removed': fresh or '(USBNT) removed' in out2,
        'UHCI to Windows': status_line(out2, 'UHCI') == win_uhci or 'UHCI' in out2 and 'back to Windows' in out2,
        'stage 3 finished': ok3,
        'xHCI: usbnt': status_line(out3, 'xHCI') == 'usbnt',
        'EHCI: %s' % win_ehci: status_line(out3, 'EHCI') == win_ehci,
        'UHCI: %s' % win_uhci: status_line(out3, 'UHCI') == win_uhci,
        'xHCI stick': re.search(r'FSTEST find XHCI.TXT [D-Z]: OK', out3) is not None and
                      '[hello from xhci.txt]' in out3,
        'EHCI stick': re.search(r'FSTEST find EHCI.TXT [D-Z]: OK', out3) is not None and
                      '[hello from ehci.txt]' in out3,
        'writes': out3.count('MB OK') >= 2 and 'BIGX.BIN 8388608' in out3 and 'BIGE.BIN 8388608' in out3,
        'usbhub runs': 'svcstat usbhub state 4' in out3,
        'hidusb runs': 'svcstat hidusb state 4' in out3,
        'kbdhid runs': 'svcstat kbdhid state 4' in out3,
        'mouhid runs': 'svcstat mouhid state 4' in out3,
        'usbstor runs (XP x64)': not g['x64'] or 'svcstat usbstor state 4' in out3,
        'USBNT: xHCI%s' % ('' if g['x64'] else ' + EHCI'): 'xhci: ' in log3 and 'uhci: ' not in log3 and
                                                        (('ehci: ' in log3) != g['x64']),
    })
    for k, v in checks.items():
        print('%-24s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    return all(checks.values())


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    res = {gname: run(gname, '--keep' in sys.argv, '--fresh' in sys.argv, '--comp' in sys.argv) for gname in (args or ['w2k', 'xp64'])}
    print(res)
    sys.exit(0 if all(res.values()) else 1)
