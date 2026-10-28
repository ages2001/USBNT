"""PS/2 touch pad on Windows XP x64 (or 2000): usbnt.sys as the upper filter of
i8042prt's mouse (smoke test, two boots).

  QEMU=.../qemu-system-x86_64 flock /tmp/claude-0/vm.lock python3 w5pad.py [xp64|w2k] [synaptics|elantech|alps]

QEMU's tp-ps2pad takes the place of the PS/2 mouse.  Boot 1: usbinst /install
puts usbnt into the UpperFilters of the PS/2 mouse.  Boot 2: the filter finds
the touch pad once i8042prt has set it up and switches it to its absolute
mode; wheeltst (/center, kept on top) logs to C:\\WHEEL.LOG.  Checked: the
filter's log line, a tap = WM_LBUTTONDOWN, two fingers down the pad =
WM_MOUSEWHEEL up, the pointer moves (off the window: a tap does not reach it).
Work image: $W5_IMG/wt-ps2-pad-<guest>.raw (deleted at the end unless --keep).
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault('W5_OUT', os.path.join(HERE, '..', 'build', 'out', 'w5pad'))
import vm5
from w5usb import driver_files, no_signing_prompt
from w5mouse import white

ROOT = os.path.join(HERE, '..')
OUT = os.path.join(ROOT, 'build', 'out')


def touch(vm, frame, wait=0.03):
    vm.qmp('qom-set', path='/machine/peripheral/tp', property='touch', value=frame)
    time.sleep(wait)


def tap(vm):
    touch(vm, '500:500', 0.05)
    touch(vm, '', 0.8)


def double_tap(vm):
    """two quick taps, the second a little off the first (a finger's jitter)"""
    touch(vm, '500:500', 0.05)
    touch(vm, '', 0.12)
    touch(vm, '505:503', 0.03)
    touch(vm, '508:505', 0.03)
    touch(vm, '', 0.8)


def run(guest, proto, keep=False, boot2=False):
    F = 'C:\\USBNT\\FSTEST.EXE'
    bat = [
        'start /b %s clicker C:\\USBNT\\STOP 600000' % F,
        'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=3',
        '%s sleep 5000' % F,
        '%s write C:\\USBNT\\STOP x' % F,
        '%s sleep 2000' % F,
    ]
    files = driver_files() + [(os.path.join(OUT, 'wheeltst.exe'), '/USBNT/WHEELTST.EXE')]
    work = os.path.join(vm5.W5_IMG, 'wt-ps2-pad-%s.raw' % guest)
    extra = ['-device', 'tp-ps2pad,id=tp,proto=%s' % proto]
    res = {}
    g = vm5.GUESTS[guest]
    if boot2 and os.path.exists(work):
        # --boot2 (with an image kept by --keep): the new driver into it, only the second boot
        vm5.mcopy(work, os.path.join(OUT, '2kxp', 'amd64' if g['x64'] else 'i386', 'usbnt.sys'),
                  '/%s/system32/drivers/usbnt.sys' % g['win'])
    else:
        work, g = vm5.prepare(guest, work=work, files=files, bat=bat)
        no_signing_prompt(work, g)
        boot2 = False
    try:
        if not boot2:
            vm = vm5.VM(guest, work, extra=extra, name='pad-' + guest)
            res['boot 1: installed'] = vm.wait_serial('##AGENT-DONE##', 900)
            out1 = vm.serial()
            vm.wait_serial('##AGENT-SHUTDOWN##', 60)
            time.sleep(15)
            vm.quit()
            print('\n'.join(l for l in out1.splitlines() if 'PS/2' in l or 'filter' in l or 'i8042' in l.lower()))
            res['usbinst: filter added'] = 'as its upper filter' in out1
        vm5.set_bat(work, [
            'start /b %s clicker C:\\USBNT\\STOP 600000' % F,
            '%s sleep 3000' % F,
            'start C:\\USBNT\\WHEELTST.EXE /center',
            'start %s front Wheel_test 30000' % F,
            '%s sleep 50000' % F,
            '%s write C:\\USBNT\\STOP x' % F,
            '%s sleep 2000' % F,
        ])
        vm = vm5.VM(guest, work, extra=extra, name='pad-' + guest)
        try:
            res['boot 2: filter, pad in its native mode'] = vm.wait_log('touch pad in its native mode', 600)
            t0 = time.time()
            while time.time() - t0 < 300 and not white(vm.shot('p0')):
                time.sleep(3)
            res['wheeltst up'] = time.time() - t0 < 300
            time.sleep(3)
            tap(vm)                                         # 1: in the window
            double_tap(vm)                                  # a double click in the window
            for i in range(14):                             # two fingers down the pad: wheel up
                touch(vm, '400:%d 600:%d' % (200 + i * 40, 200 + i * 40))
            touch(vm, '', 1.0)
            for i in range(12):                             # far to the left, off the window
                touch(vm, '%d:500' % (800 - i * 40))
            touch(vm, '', 0.5)
            tap(vm)                                         # 2: on the desktop
            vm.shot('p1')
            res['agent finished'] = vm.wait_serial('##AGENT-DONE##', 300)
            time.sleep(2)
        finally:
            log = vm.log()
            vm.quit()
        lp = os.path.join(vm.dir, 'WHEEL.LOG')
        if os.path.exists(lp):
            os.remove(lp)
        vm5.mget(work, '/WHEEL.LOG', lp)
        wl = open(lp, 'rb').read().decode('latin1') if os.path.exists(lp) else ''
        print('\n'.join(l for l in log.splitlines() if 'ps2' in l))
        lb = wl.count('LBUTTONDOWN')
        wheel = [int(l.split()[1]) for l in wl.splitlines() if l.startswith('MOUSEWHEEL')]
        res['tap 1 and the double tap reached the window, tap 2 (pointer moved off it) not: %d' % lb] = lb == 2
        res['double tap = double click'] = 'LBUTTONDBLCLK' in wl
        res['two finger scroll: WM_MOUSEWHEEL up %s' % wheel[:6]] = len(wheel) >= 3 and all(w > 0 for w in wheel)
    finally:
        if not keep and os.path.exists(work):
            os.remove(work)
    for k, v in res.items():
        print('%-60s %s' % (k, 'PASS' if v else 'FAIL'))
    ok = all(res.values())
    print('RESULT w5pad %s %s %s' % (guest, proto, 'PASS' if ok else 'FAIL'))
    return ok


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    guest = args[0] if args else 'xp64'
    proto = args[1] if len(args) > 1 else 'synaptics'
    sys.exit(0 if run(guest, proto, '--keep' in sys.argv, '--boot2' in sys.argv) else 1)
