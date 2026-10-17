"""USBNT on Windows 2000 (x86) and Windows XP x64: Plug and Play installation
with usbinst /install, then the controllers (xHCI, which neither system has a
driver for, EHCI, UHCI), a USB keyboard, mouse and stick through usbnt.sys.

  python3 w5usb.py [w2k|xp64 ...] [--keep]
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5
import ntmsc
from ntmsc import stick, W

ROOT = os.path.join(HERE, '..')
OUT = os.path.join(ROOT, 'build', 'out')
FST = os.path.join(OUT, 'fstest.exe')


def driver_files():
    f = [(os.path.join(OUT, 'usbinst.exe'), '/USBNT/USBINST.EXE'), (FST, '/USBNT/FSTEST.EXE')]
    for t in ('usbtree', 'usbeject', 'usbmon', 'usbcam'):
        f.append((os.path.join(OUT, t + '.exe'), '/USBNT/%s.EXE' % t.upper()))
    f.append((os.path.join(OUT, 'usbntcap.dll'), '/USBNT/USBNTCAP.DLL'))
    f.append((os.path.join(OUT, '2kxp', 'amd64', 'usbntcap.dll'), '/USBNT/2KXP/AMD64/USBNTCAP.DLL'))
    for inf in ('usbnt.inf', 'usbntkbd.inf', 'usbntmou.inf'):
        f.append((os.path.join(OUT, '2kxp', inf), '/USBNT/2KXP/' + inf.upper()))
    f.append((os.path.join(OUT, '2kxp', 'i386', 'usbnt.sys'), '/USBNT/2KXP/I386/USBNT.SYS'))
    f.append((os.path.join(OUT, '2kxp', 'amd64', 'usbnt.sys'), '/USBNT/2KXP/AMD64/USBNT.SYS'))
    f.append((os.path.join(OUT, '2kxp', 'amd64', 'usbinst.exe'), '/USBNT/2KXP/AMD64/USBINST.EXE'))
    return f


def no_signing_prompt(work, g):
    """unsigned drivers install without asking (the tests have nobody to click).  Only values of
    keys that exist: hivex writes subkey lists Windows 2000 cannot read."""
    sw = vm5.Hive(work, g['win'], 'software')
    sw.set('\\Microsoft\\Driver Signing', {'Policy': 'hex:3:00'})
    k = '\\Policies\\Microsoft\\Windows NT\\Driver Signing'
    if sw.exists(k):
        sw.set(k, {'BehaviorOnFailedVerify': 'dword:0x0'})
    sw.save()
    sy = vm5.Hive(work, g['win'], 'system')
    sy.set('\\ControlSet001\\Control\\CrashControl', {'AutoReboot': 'dword:0x0'})     # a blue screen stays
    sy.save()
    user = g['user'] or 'test'
    path = '/Documents and Settings/%s/NTUSER.DAT' % user
    local = os.path.join(vm5.OUT, 'ntuser_%d' % os.getpid())
    if vm5.mget(work, path, local):
        h = vm5.Hive.__new__(vm5.Hive)
        h.img, h.path, h.local = work, path, local
        for k, v in (('\\Software\\Microsoft\\Driver Signing', {'Policy': 'dword:0x0'}),
                     ('\\Software\\Policies\\Microsoft\\Windows NT\\Driver Signing',
                      {'BehaviorOnFailedVerify': 'dword:0x0'})):
            if h.exists(k):
                h.set(k, v)
                print('user hive:', k)
        h.save()
    else:
        print('no user hive', path)


def run(guest, keep=False):
    g = vm5.GUESTS[guest]
    os.makedirs(W, exist_ok=True)
    img = os.path.join(W, 'w5stick-%s.img' % guest)
    if os.path.exists(img):
        os.remove(img)
    ntmsc.W = os.path.join(W, 'tmp-' + guest)       # its temporary files: one place per guest
    os.makedirs(ntmsc.W, exist_ok=True)
    stick(img, 64, 'USBSTICK', [('hello.txt', b'hello from usb\n')])
    bat = [
        'start /b C:\\USBNT\\FSTEST.EXE clicker C:\\USBNT\\STOP 600000',
        'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=%s' % os.environ.get('W5_DEBUG', '2'),
        'C:\\USBNT\\USBINST.EXE /status',
        'C:\\USBNT\\FSTEST.EXE find HELLO.TXT 60000',
        'C:\\USBNT\\FSTEST.EXE cat ?:\\HELLO.TXT',
        'C:\\USBNT\\FSTEST.EXE fill ?:\\BIG.BIN 16',
        'C:\\USBNT\\USBEJECT.EXE /list',
        'C:\\USBNT\\USBEJECT.EXE /slot 0',
        'C:\\USBNT\\FSTEST.EXE gone ?:\\HELLO.TXT 30000',
        'C:\\USBNT\\USBEJECT.EXE /list',
        'C:\\USBNT\\FSTEST.EXE find HELLO.TXT 120000',
        'C:\\USBNT\\FSTEST.EXE sum ?:\\BIG.BIN',
        'C:\\USBNT\\FSTEST.EXE gone ?:\\HELLO.TXT 120000',
        'C:\\USBNT\\FSTEST.EXE write C:\\USBNT\\STOP x',
        'C:\\USBNT\\FSTEST.EXE sleep 2000',
    ]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, 'work-usb-%s.raw' % guest), files=driver_files(),
                          bat=bat)
    no_signing_prompt(work, g)
    extra = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-ehci,id=ehci', '-device', 'piix3-usb-uhci,id=uhci',
             '-blockdev', 'driver=file,filename=%s,node-name=stickf' % img,
             '-blockdev', 'driver=raw,file=stickf,node-name=stick',
             '-device', 'usb-storage,drive=stick,bus=xhci.0,id=stickdev',
             '-device', 'usb-kbd,bus=ehci.0', '-device', 'usb-mouse,bus=uhci.0']
    vm = vm5.VM(guest, work, extra=extra, name='usb-' + guest)
    replug = yank = False
    # Safely Remove Hardware, then the stick is pulled and plugged in again; later pulled while mounted
    if vm.wait_log('ejected by Windows', 600):
        time.sleep(2)
        vm.qmp('device_del', id='stickdev')
        time.sleep(4)
        vm.qmp('device_add', driver='usb-storage', drive='stick', bus='xhci.0', id='stickdev')
        replug = True
        t0 = time.time()
        while time.time() - t0 < 180 and vm.log().count('to Windows') < 2:
            time.sleep(1)
        if vm.log().count('to Windows') >= 2:
            time.sleep(25)
            try:
                vm.qmp('device_del', id='stickdev')
                yank = True
            except Exception as e:
                print('yank:', e)
    ok = vm.wait_serial('##AGENT-DONE##', 900, shots=60)
    time.sleep(2)
    vm.shot('end')
    out = vm.serial()
    log = vm.log()
    vm.quit()
    print(out[-6000:])
    print('---- COM2 (usbnt.sys) ----')
    print(log[-8000:])
    checks = {
        'agent finished': ok,
        'installed': 'usbnt.sys' in out and 'Done' in out,
        'driver started': 'Windows 2000 / XP' in log,
        'xhci started': 'xHCI' in log or 'xhci' in log,
        'stick found': 'FSTEST find HELLO.TXT' in out and 'OK after' in out,
        'read': 'FSTEST cat' in out and '[hello from usb]' in out,
        'write 16 MB': 'MB OK' in out,
        'eject': 'FSTEST gone ?:\\HELLO.TXT OK' in out and 'ejected by Windows' in log,
        'plug in again': replug and out.count('FSTEST find HELLO.TXT') == 2 and out.count('OK after') >= 2,
        'data kept': 'BIG.BIN 16777216' in out,
        'pulled out': yank and out.count('FSTEST gone ?:\\HELLO.TXT OK') == 2,
    }
    for k, v in checks.items():
        print('%-16s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    return all(checks.values())


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    keep = '--keep' in sys.argv
    res = {gname: run(gname, keep) for gname in (args or ['w2k', 'xp64'])}
    print(res)
    sys.exit(0 if all(res.values()) else 1)
