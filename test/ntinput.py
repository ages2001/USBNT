#!/usr/bin/env python3
"""Keyboard and mouse test in an NT guest: PS/2 keyboard + PS/2 mouse (now
driven by usbnt, i8042prt disabled) together with two USB keyboards and two USB
mice, all typing / clicking into one window at the same time.

  ntinput.py HC [--nt4] [--keep-i8042] [--i2c] [--bat MS]

Also: the shift keys let go (PS/2 left and right shift, USB left shift: "!1@2#3"), double
clicks (PS/2 and USB mouse).  --bat MS: the PS/2 mouse's reset self test takes MS ms (QEMU
ps2-mouse bat-delay; slow trackpoints and touch pads).

--i2c adds a HID over I2C touch pad and keyboard (QEMU dw-i2c-pci + i2c-hid,
bound to a third display head) that type and click into the same window.

Each source types its own word and uses its own mouse button; the guest's
input test window must see all of them.  QEMU routes events by console: the
USB devices are bound to the two display heads (vga0, vga1) and get their
events through QMP input-send-event; HMP sendkey / mouse_* go to the PS/2
devices.  Needs the QEMU build with display binding for usb-mouse.
"""
import json, os, socket, sys, time
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HC = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}
QCODE = {' ': 'spc'}


class Qmp:
    def __init__(self, path):
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.s.connect(path); break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile('rwb')
        self.f.readline()
        self.call('qmp_capabilities')

    def call(self, cmd, **args):
        self.f.write((json.dumps({'execute': cmd, 'arguments': args}) + '\n').encode()); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                if 'error' in r:
                    print('qmp error', cmd, r['error'])
                return r

    def key(self, dev, k):
        for down in (True, False):
            self.call('input-send-event', device=dev,
                      events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}])
            time.sleep(0.05)

    def btn(self, dev, b):
        for down in (True, False):
            self.call('input-send-event', device=dev, events=[{'type': 'btn', 'data': {'down': down, 'button': b}}])
            time.sleep(0.08)

    def move(self, dev, dx, dy):
        self.call('input-send-event', device=dev, events=[{'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
                                                           {'type': 'rel', 'data': {'axis': 'y', 'value': dy}}])

    def wheel(self, dev, up):
        self.btn(dev, 'wheel-up' if up else 'wheel-down')


def main():
    hc = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'xhci'
    nt4 = '--nt4' in sys.argv
    keep = '--keep-i8042' in sys.argv
    i2c = '--i2c' in sys.argv
    batms = sys.argv[sys.argv.index('--bat') + 1] if '--bat' in sys.argv else None
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    os.environ['AUDIONT_VGA'] = '-vga none -device VGA,id=vga0 -device secondary-vga,id=vga1' + \
        (' -device secondary-vga,id=vga2' if i2c else '')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'usbwork-inp.raw')
    vm.sh('cp', '--sparse=always', img, work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/inptest.exe'), '/USBNT/INPTEST.EXE')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbinst.exe'), '/USBNT/USBINST.EXE')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/USBNT/USBNT.SYS')
    t = 'C:\\AUDIONT\\TEST\\ATTEST.EXE'
    com2 = os.path.join(vm.OUT, 'com2.log')
    qmp_path = os.path.join(vm.OUT, 'qmp.sock')
    usb = ['-device', '%s,id=hc' % HC[hc],
           '-device', 'usb-kbd,bus=hc.0,display=vga0,id=k1', '-device', 'usb-mouse,bus=hc.0,display=vga0,id=m1',
           '-device', 'usb-kbd,bus=hc.0,display=vga1,id=k2', '-device', 'usb-mouse,bus=hc.0,display=vga1,id=m2',
           '-qmp', 'unix:%s,server,nowait' % qmp_path, '-serial', 'file:' + com2]
    if batms:
        usb += ['-global', 'ps2-mouse.bat-delay=%s' % batms]
    if i2c:
        usb += ['-device', 'dw-i2c-pci,id=i2c0', '-device', 'i2c-hid,bus=i2c0.0,address=0x2c,kind=ptp,display=vga2',
                '-device', 'i2c-hid,bus=i2c0.0,address=0x15,kind=kbd,desc-reg=1,display=vga2']
    # boot 1: i8042prt off (unless kept), usbnt drives PS/2
    # the installer disables i8042prt and hands the PS/2 devices to usbnt (/keepi8042: not)
    pre = ['@echo off', 'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=2%s'
           % (' /keepi8042' if keep else '')]
    open('/tmp/inp-pre.bat', 'w').write('\r\n'.join(pre) + '\r\n')
    vm.mcopy(work, '/tmp/inp-pre.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    for f in (com2, qmp_path):
        if os.path.exists(f): os.remove(f)
    v = vm.VM(work, 'none', usb)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        v.key('ctrl-alt-delete', 4); v.key('ret', 2)
    ok1 = v.wait_serial('##AGENT-SHUTDOWN##', 300)
    time.sleep(12)
    v.quit()
    print('boot 1 (settings):', 'done' if ok1 else 'TIMEOUT')
    print('\n'.join(l for l in v.serial().splitlines() if 'i8042' in l or 'PS/2' in l or 'error' in l))
    # boot 2: the test
    bat = ['@echo off', 'C:\\USBNT\\INPTEST.EXE 75', '']
    open('/tmp/inp-run.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/inp-run.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    for f in (com2, qmp_path):
        if os.path.exists(f): os.remove(f)
    v = vm.VM(work, 'none', usb)
    q = Qmp(qmp_path)
    t0 = time.time()
    logon_by_hand = False
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        # PS/2 keyboard (usbnt) answers the logon box
        logon_by_hand = True
        v.key('ctrl-alt-delete', 4); v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    time.sleep(15)
    v.shot('inp0')
    # the NT 3.1 test image shows a system modal "a service failed" box (no
    # network, no sound card): inptest.exe clicks it away itself
    # interleave the sources
    words = [('psa', 'ps2'), ('usba', 'vga0'), ('psb', 'ps2'), ('usbb', 'vga1'), ('usbc', 'vga0')]
    if i2c:
        words.append(('itc', 'vga2'))
    for word, src in words:
        for ch in word:
            if src == 'ps2':
                v.key(QCODE.get(ch, ch), 0.1)
            else:
                q.key(src, QCODE.get(ch, ch))
        time.sleep(0.5)
    # shift keys: pressed with a digit, let go, the digit again (the left shift's break code
    # is 0xAA in set 1)
    v.key('shift-1', 0.3); v.key('1', 0.3)                                                  # PS/2: "!1"
    v.key('shift_r-2', 0.3); v.key('2', 0.3)                                                # PS/2: "@2"
    for k, down in (('shift', True), ('3', True), ('3', False), ('shift', False)):
        q.call('input-send-event', device='vga0',
               events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}])
        time.sleep(0.08)
    time.sleep(0.2)
    q.key('vga0', '3')                                                                      # USB: "#3"
    time.sleep(0.5)
    for _ in range(2):
        v.cmd('mouse_button 1'); time.sleep(0.1); v.cmd('mouse_button 0'); time.sleep(0.3)   # PS/2 left
    time.sleep(1.0)
    for _ in range(2):
        v.cmd('mouse_button 1'); time.sleep(0.05); v.cmd('mouse_button 0'); time.sleep(0.1)  # PS/2 double click
    time.sleep(1.0)
    for _ in range(2):
        q.call('input-send-event', device='vga1', events=[{'type': 'btn', 'data': {'down': True, 'button': 'left'}}])
        time.sleep(0.05)
        q.call('input-send-event', device='vga1', events=[{'type': 'btn', 'data': {'down': False, 'button': 'left'}}])
        time.sleep(0.1)                                                                     # USB double click
    time.sleep(1.0)
    for _ in range(3):
        q.btn('vga0', 'right'); time.sleep(0.3)                                              # USB mouse 1
    for _ in range(2):
        q.btn('vga1', 'middle'); time.sleep(0.3)                                             # USB mouse 2
    if i2c:
        for _ in range(3):
            q.btn('vga2', 'left'); time.sleep(0.3)                                           # I2C touch pad
    for _ in range(4):
        v.cmd('mouse_move 10 5'); time.sleep(0.2)                                            # PS/2 movement
        q.move('vga0', 12, 0); time.sleep(0.2)
        q.move('vga1', 0, 9); time.sleep(0.2)
        if i2c:
            q.move('vga2', -7, -3); time.sleep(0.2)
    for _ in range(3):
        v.cmd('mouse_move 0 0 -1'); time.sleep(0.3)                                          # PS/2 wheel
    for _ in range(2):
        q.wheel('vga0', True); time.sleep(0.3)                                               # USB wheel
    v.shot('inp1')
    done = v.wait_serial('##AGENT-SHUTDOWN##', 200)
    time.sleep(3)
    v.quit()
    ser = v.serial()
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    import re
    m = re.search(r'INPTEST chars=\[(.*?)\] left=(-?\d+) right=(-?\d+) middle=(-?\d+) x1=\d+ x2=\d+ wheel=(-?\d+) moves=(\d+)', ser)
    md = re.search(r'INPTEST .* dbl=(\d+)', ser)
    dbl = int(md.group(1)) if md else 0
    print('\n'.join(l for l in ulog.splitlines() if 'ps2' in l or 'class driver' in l or 'i2c' in l))
    print(m.group(0) if m else 'no INPTEST result')
    ok = bool(m) and done
    if m:
        chars, left, right, middle, wheel, moves = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))
        checks = [('PS/2 keyboard', 'psa' in chars and 'psb' in chars),
                  ('USB keyboard 1', 'usba' in chars and 'usbc' in chars),
                  ('USB keyboard 2', 'usbb' in chars),
                  ('order kept', chars.find('psa') < chars.find('usba') < chars.find('psb') < chars.find('usbb') < chars.find('usbc')),
                  ('shift keys let go (PS/2 left, right; USB)', '!1@2#3' in chars),
                  ('PS/2 mouse buttons', left == (9 if i2c else 6)), ('USB mouse 1 buttons', right == 3), ('USB mouse 2 buttons', middle == 2),
                  ('double clicks (PS/2, USB)', dbl >= 2),
                  ('movement', moves >= 4)]
        if i2c:
            checks.append(('I2C keyboard', 'itc' in chars and chars.find('usbc') < chars.find('itc')))
            checks.append(('I2C touch pad found (multi-touch mode)', 'i2c: ' in ulog and 'precision touch pad' in ulog))
        if nt4:
            checks.append(('wheels (PS/2 + USB)', wheel != 0))
        if not keep:
            checks.append(('usbnt drives PS/2', 'ps2: controller' in ulog and 'keyboard yes' in ulog))
        for name, c in checks:
            ok &= c
            print('%s %s' % ('ok  ' if c else 'FAIL', name))
    print('logon by hand' if logon_by_hand else 'auto logon')
    print('dbl=%d' % dbl)
    print('RESULT input %s%s%s%s%s %s' % (hc, ' nt4' if nt4 else '', ' i8042prt' if keep else '', ' i2c' if i2c else '',
                                        (' bat %s' % batms) if batms else '',
                                        'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
