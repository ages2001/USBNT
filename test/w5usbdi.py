"""USBNT's USB driver interface on Windows 2000 / XP x64: with UsbPdo=2 every
device but the hubs goes to Windows' own USB drivers - the stick to usbstor
(bulk transfers), the keyboard to hidusb / kbdhid (interrupt transfers), the
FTDI serial adapter becomes a device without a driver - and the stick is
pulled out while mounted.

  python3 w5usbdi.py [w2k|xp64 ...] [--keep]
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5, w5usb, ntmsc
from ntmsc import stick, W


def run(guest, keep=False):
    os.makedirs(W, exist_ok=True)
    img = os.path.join(W, 'w5di-%s.img' % guest)
    if os.path.exists(img):
        os.remove(img)
    ntmsc.W = os.path.join(W, 'tmp-di-' + guest)
    os.makedirs(ntmsc.W, exist_ok=True)
    stick(img, 64, 'USBSTICK', [('hello.txt', b'hello from usb\n')])
    bat = [
        'start /b C:\\USBNT\\FSTEST.EXE clicker C:\\USBNT\\STOP 900000',
        # /all: every controller to USBNT, so the devices behind EHCI and UHCI are USBNT's to report
        'C:\\USBNT\\USBINST.EXE /install /all /nogroup /nomon /set DebugPort=0x2f8 /set UsbPdo=2 /set DebugLevel=%s'
        % os.environ.get('W5_DEBUG', '2'),
        'C:\\USBNT\\FSTEST.EXE find HELLO.TXT 180000',
        'C:\\USBNT\\FSTEST.EXE cat ?:\\HELLO.TXT',
        'C:\\USBNT\\FSTEST.EXE fill ?:\\BIG.BIN 16',
        'C:\\USBNT\\FSTEST.EXE sum ?:\\BIG.BIN',
        'C:\\USBNT\\FSTEST.EXE svcstat usbstor',
        'C:\\USBNT\\FSTEST.EXE svcstat hidusb',
        'C:\\USBNT\\FSTEST.EXE svcstat kbdhid',
        'C:\\USBNT\\FSTEST.EXE gone ?:\\HELLO.TXT 300000',
        'C:\\USBNT\\FSTEST.EXE write C:\\USBNT\\STOP x',
        'C:\\USBNT\\FSTEST.EXE sleep 2000',
    ]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, os.environ.get('W5_PREFIX', 'work-') + 'di-%s.raw' % guest),
                          files=w5usb.driver_files(),
                          bat=bat)
    w5usb.no_signing_prompt(work, g)
    extra = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-ehci,id=ehci', '-device', 'piix3-usb-uhci,id=uhci',
             '-blockdev', 'driver=file,filename=%s,node-name=stickf' % img,
             '-blockdev', 'driver=raw,file=stickf,node-name=stick',
             '-device', 'usb-storage,drive=stick,bus=xhci.0,id=stickdev',
             ]
    devs = os.environ.get('W5_DEVS', 'kbd,serial').split(',')
    if 'kbd' in devs:
        extra += ['-device', 'usb-kbd,bus=ehci.0']
    if 'serial' in devs:
        extra += ['-chardev', 'file,id=ser0,path=/dev/null', '-device', 'usb-serial,chardev=ser0,bus=uhci.0']
    vm = vm5.VM(guest, work, extra=extra, name='usbdi-' + guest)
    yank = False
    if vm.wait_log("to Windows' USB drivers", 600):
        time.sleep(170)
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
        'devices to Windows': log.count("to Windows' USB drivers") >= 1 + len([d for d in devs if d]),
        'stick (usbstor)': re.search(r'FSTEST find HELLO.TXT [D-Z]: OK', out) is not None,
        'read': '[hello from usb]' in out,
        'write 16 MB': 'MB OK' in out and 'BIG.BIN 16777216' in out,
        'usbstor runs': 'svcstat usbstor state 4' in out,
        'hidusb runs': 'svcstat hidusb state 4' in out or 'kbd' not in devs,
        'pulled out': yank and 'FSTEST gone ?:\\HELLO.TXT OK' in out,
    }
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
