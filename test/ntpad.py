#!/usr/bin/env python3
"""PS/2 touch pad in an NT 4.0 guest (smoke test, two boots).

  QEMU=.../qemu-system-x86_64 flock /tmp/claude-0/vm.lock python3 ntpad.py [synaptics|elantech|alps]

QEMU's tp-ps2pad takes the place of the PS/2 mouse; usbinst disables
i8042prt (boot 1), usbnt.sys finds the touch pad and runs it in its native
mode (boot 2), wheeltst.exe (/center) logs what reaches its window into
C:\\WHEEL.LOG.  Checked: a tap clicks in the window; the pointer moves (out of
the window to the left: a tap there does not reach it; back: it does); two
fingers down the pad turn into WM_MOUSEWHEEL for the window.
Work image: $NTPAD_WORK (default <image dir>/wt-ps2-pad.raw, deleted at the end).
"""
import json, os, socket, sys, time
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile('rwb')
        self.f.readline()
        self.call('qmp_capabilities')

    def call(self, cmd, **args):
        self.f.write((json.dumps({'execute': cmd, 'arguments': args}) + '\n').encode())
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def touch(self, frame, wait=0.03):
        self.call('qom-set', path='/machine/peripheral/tp', property='touch', value=frame)
        time.sleep(wait)

    def tap(self):
        self.touch('500:500', 0.05)
        self.touch('', 0.8)


def main():
    proto = sys.argv[1] if len(sys.argv) > 1 else 'synaptics'
    os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    os.environ.setdefault('AUDIONT_OUT', os.path.join(ROOT, 'build', 'out', 'vm-ntpad'))
    import vm
    win = 'WINNT4'
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.environ.get('NTPAD_WORK') or os.path.join(vm.IMG_DIR, 'wt-ps2-pad.raw')
    vm.sh('cp', '--sparse=always', img, work)
    out = os.path.join(ROOT, 'build/out')
    vm.mcopy(work, os.path.join(out, 'usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.path.join(out, 'usbmon.exe'), '/%s/SYSTEM32/USBMON.EXE' % win)
    for f in ('usbinst.exe', 'usbnt.sys', 'usbmon.exe', 'wheeltst.exe', 'fstest.exe'):
        vm.mcopy(work, os.path.join(out, f), '/USBNT/' + f.upper())
    os.makedirs(vm.OUT, exist_ok=True)
    com2 = os.path.join(vm.OUT, 'com2.log')
    qmp_path = os.path.join(vm.OUT, 'qmp.sock')
    extra = ['-device', 'tp-ps2pad,id=tp,proto=%s' % proto, '-device', 'qemu-xhci,id=hc',
             '-qmp', 'unix:%s,server,nowait' % qmp_path, '-serial', 'file:' + com2]
    # boot 1: the installer disables i8042prt, usbnt drives the PS/2 keyboard and the pad
    pre = ['@echo off', 'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=3', '']
    p = os.path.join(vm.OUT, 'pad-pre.bat')
    open(p, 'w').write('\r\n'.join(pre))
    vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
    for f in (com2, qmp_path):
        if os.path.exists(f):
            os.remove(f)
    v = vm.VM(work, 'none', extra)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
    ok1 = v.wait_serial('##AGENT-SHUTDOWN##', 300)
    time.sleep(10)
    v.quit()
    print('boot 1 (install):', 'done' if ok1 else 'TIMEOUT')
    # boot 2
    bat = ['@echo off', 'C:\\USBNT\\FSTEST.EXE sleep 5000', 'start C:\\USBNT\\WHEELTST.EXE /center',
           'start C:\\USBNT\\FSTEST.EXE front Wheel_test 20000', 'C:\\USBNT\\FSTEST.EXE sleep 80000', '']
    p = os.path.join(vm.OUT, 'pad-run.bat')
    open(p, 'w').write('\r\n'.join(bat))
    vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
    for f in (com2, qmp_path):
        if os.path.exists(f):
            os.remove(f)
    v = vm.VM(work, 'none', extra)
    q = Qmp(qmp_path)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    time.sleep(16)                                  # wheeltst starts, comes to the front
    v.shot('pad0')
    q.tap()                                         # 1: in the window (the pointer starts in the middle)
    for i in range(12):                             # far to the left: off the window
        q.touch('%d:500' % (800 - i * 40))
    q.touch('', 0.5)
    q.tap()                                         # 2: on the desktop
    for i in range(70):                             # back to the right, slowly (no acceleration): ~300 pixels
        q.touch('%d:500' % (200 + i * 3))
    q.touch('', 0.5)
    v.shot('pad1')
    q.tap()                                         # 3: in the window again
    for i in range(14):                             # two fingers down the pad: wheel up
        q.touch('400:%d 600:%d' % (200 + i * 40, 200 + i * 40))
    q.touch('', 1.0)
    v.shot('pad2')
    done = v.wait_serial('##AGENT-SHUTDOWN##', 150)
    time.sleep(8)
    v.quit()
    log_path = os.path.join(vm.OUT, 'WHEEL.LOG')
    if os.path.exists(log_path):
        os.remove(log_path)
    vm.mget(work, '/WHEEL.LOG', log_path)
    log = open(log_path, 'rb').read().decode('latin1') if os.path.exists(log_path) else ''
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    print('\n'.join(l for l in ulog.splitlines() if 'ps2' in l))
    lb = log.count('LBUTTONDOWN')
    wheel = [int(l.split()[1]) for l in log.splitlines() if l.startswith('MOUSEWHEEL')]
    checks = [
        ('touch pad in its native mode', 'in its native mode' in ulog),
        ('taps 1 and 3 reached the window, 2 (pointer moved off it) did not: %d' % lb, lb == 2),
        ('two finger scroll: WM_MOUSEWHEEL up %s' % wheel, len(wheel) >= 3 and all(w > 0 for w in wheel)),
    ]
    ok = done and bool(log)
    for name, good in checks:
        ok &= good
        print('%s %s' % ('ok  ' if good else 'FAIL', name))
    if not os.environ.get('NTPAD_KEEP'):
        os.remove(work)
    print('RESULT ntpad %s %s' % (proto, 'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
