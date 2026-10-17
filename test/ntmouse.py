#!/usr/bin/env python3
"""Back / forward buttons and autoscroll in an NT 3.1 or NT 4.0 guest.

  ntmouse.py [HC] [--nt4] [--nohook]

usbnt.sys drives a PS/2 mouse (IntelliMouse Explorer: buttons 4 / 5) and a
USB mouse (QEMU's usb-mouse has five buttons); usbmon runs, wheeltst.exe
(/center) logs what reaches its window into C:\\WHEEL.LOG.  Checked:

  - button 4 of the PS/2 mouse and button 5 of the USB mouse arrive as
    Alt+Left / Alt+Right (NT has no X buttons; usbmon types the keys)
  - a middle click starts autoscroll (origin mark, the system arrow is the
    scroll cursor - hot spot in the middle), moving down / right scrolls the
    window (NT 3.x: WM_VSCROLL / WM_HSCROLL, NT 4.0: WM_MOUSEWHEEL /
    WM_HSCROLL), a left click ends it and does not reach the window, the
    cursor is the arrow again
  - holding the middle button of the USB mouse and dragging up scrolls up,
    releasing it ends autoscroll
  - afterwards a left click reaches the window again (nothing is held back)

NT 3.1 and --nohook: autoscroll through the driver (the middle button goes
to usbmon); NT 4.0 (SP6) without --nohook: through the low level mouse hook.
The work image is $NTMOUSE_WORK (default <image dir>/usbwork-mouse.raw).
"""
import json, os, re, socket, sys, time
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
HC = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}


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
                if 'error' in r:
                    print('qmp error', cmd, r['error'])
                return r

    def ev(self, dev, events):
        a = {'events': events}
        if dev:
            a['device'] = dev
        self.call('input-send-event', **a)

    def btn(self, dev, b, down):
        self.ev(dev, [{'type': 'btn', 'data': {'down': down, 'button': b}}])
        time.sleep(0.15)

    def click(self, dev, b):
        self.btn(dev, b, True)
        self.btn(dev, b, False)

    def move(self, dev, dx, dy, steps):
        """small steps: NT's mouse acceleration leaves moves below 6 counts alone"""
        for _ in range(steps):
            self.ev(dev, [{'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
                          {'type': 'rel', 'data': {'axis': 'y', 'value': dy}}])
            time.sleep(0.05)


def scm_box(png):
    """NT 3.1's "a service failed" box (127..515 x 160..320) is there: its yellow '!' icon"""
    try:
        from PIL import Image
        im = Image.open(png).convert('RGB')
        n = sum(1 for y in range(205, 250) for x in range(150, 175) if im.getpixel((x, y)) == (255, 255, 0))
        return n > 100
    except Exception:
        return False


def white(png):
    """wheeltst is up: the right half of its client area (no text) is white"""
    try:
        from PIL import Image
        im = Image.open(png).convert('RGB')
        # wheeltst's window is at 160, 100 (320 x 290) whatever the screen size
        return all(im.getpixel((x, y)) == (255, 255, 255) for x in (410, 430, 450) for y in (185, 245, 305))
    except Exception:
        return False


def to_corner(q, dev):
    """the pointer to 0, 0 (it stops at the edge)"""
    q.move(dev, -40, -40, 30)


def parse(log):
    c = {'alt-left': 0, 'alt-right': 0, 'vdown': 0, 'vup': 0, 'hright': 0, 'hleft': 0, 'wheel+': 0, 'wheel-': 0,
         'lbutton': 0, 'mbutton': 0, 'rbutton': 0, 'on': [], 'off': [], 'xbutton': 0, 'starts': 0}
    state = ''
    for l in log.splitlines():
        w = l.split()
        if not w:
            continue
        if w[0] == 'KEY' and len(w) > 2 and w[2] == 'alt':
            if w[1] == '37': c['alt-left'] += 1
            if w[1] == '39': c['alt-right'] += 1
        elif w[0] == 'VSCROLL':
            if w[1] == '1': c['vdown'] += 1
            if w[1] == '0': c['vup'] += 1
        elif w[0] == 'HSCROLL':
            if w[1] == '1': c['hright'] += 1
            if w[1] == '0': c['hleft'] += 1
        elif w[0] == 'MOUSEWHEEL':
            c['wheel+' if int(w[1]) > 0 else 'wheel-'] += 1
        elif w[0] == 'LBUTTONDOWN': c['lbutton'] += 1
        elif w[0] == 'MBUTTONDOWN': c['mbutton'] += 1
        elif w[0] == 'RBUTTONDOWN': c['rbutton'] += 1
        elif w[0] == 'XBUTTONDOWN': c['xbutton'] += 1
        elif w[0] == 'AUTOSCROLL':
            state = w[1]
            c['starts'] += state == 'ON'
        elif w[0] == 'CURSOR':                      # hot spot of the cursor shown, a moment after
            c['on' if state == 'ON' else 'off'].append((int(w[1]), int(w[2])))
    return c


def main():
    hc = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'xhci'
    nt4 = '--nt4' in sys.argv
    nohook = '--nohook' in sys.argv
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    # PS/2 events go to the unbound PS/2 mouse, the USB mouse is bound to the second head
    os.environ['AUDIONT_VGA'] = '-vga none -device VGA,id=vga0 -device secondary-vga,id=vga1'
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.environ.get('NTMOUSE_WORK') or os.path.join(vm.IMG_DIR, 'usbwork-mouse.raw')
    vm.sh('cp', '--sparse=always', img, work)
    out = os.path.join(ROOT, 'build/out')
    vm.mcopy(work, os.path.join(out, 'usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    # the copy Winlogon starts (if the image has it in Userinit) and the one the batch starts
    vm.mcopy(work, os.path.join(out, 'usbmon.exe'), '/%s/SYSTEM32/USBMON.EXE' % win)
    for f in ('usbinst.exe', 'usbnt.sys', 'usbmon.exe', 'wheeltst.exe', 'fstest.exe'):
        vm.mcopy(work, os.path.join(out, f), '/USBNT/' + f.upper())
    com2 = os.path.join(vm.OUT, 'com2.log')
    qmp_path = os.path.join(vm.OUT, 'qmp.sock')
    usb = ['-device', '%s,id=hc' % HC[hc], '-device', 'usb-mouse,bus=hc.0,display=vga1,id=m1',
           '-qmp', 'unix:%s,server,nowait' % qmp_path, '-serial', 'file:' + com2]
    # boot 1: the installer disables i8042prt, usbnt drives the PS/2 keyboard and mouse
    pre = ['@echo off', 'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=3', '']
    p = os.path.join(vm.OUT, 'mouse-pre.bat')
    os.makedirs(vm.OUT, exist_ok=True)
    open(p, 'w').write('\r\n'.join(pre))
    vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
    for f in (com2, qmp_path):
        if os.path.exists(f):
            os.remove(f)
    v = vm.VM(work, 'none', usb)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
    ok1 = v.wait_serial('##AGENT-SHUTDOWN##', 300)
    time.sleep(12)
    v.quit()
    print('boot 1 (install):', 'done' if ok1 else 'TIMEOUT')
    # boot 2: the test
    # (fstest front keeps wheeltst on top: NT 4.0 shows its Welcome screen and a
    # "service failed" box over it)
    # NT 3.1: wheeltst comes after the "service failed" box (see below), NT 4.0: fstest
    # front keeps it above the Welcome screen and the box
    bat = ['@echo off', 'start C:\\USBNT\\USBMON.EXE%s' % (' /restart /nohook' if nohook else ''),
           'C:\\USBNT\\FSTEST.EXE sleep %d' % (6000 if nt4 else 50000),
           'start C:\\USBNT\\WHEELTST.EXE /center'] + \
          (['start C:\\USBNT\\FSTEST.EXE front Wheel_test 25000'] if nt4 else []) + \
          ['C:\\USBNT\\FSTEST.EXE sleep 150000', '']
    p = os.path.join(vm.OUT, 'mouse-run.bat')
    open(p, 'w').write('\r\n'.join(bat))
    vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
    for f in (com2, qmp_path):
        if os.path.exists(f):
            os.remove(f)
    v = vm.VM(work, 'none', usb)
    q = Qmp(qmp_path)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    PS2, USB = None, 'vga1'
    if nt4:
        time.sleep(20)                              # usbmon and wheeltst start
    else:
        # the NT 3.1 test image shows a system modal "a service failed" box some time
        # after logon, which takes the mouse: click its OK button, then wait for wheeltst
        t0 = time.time()
        while time.time() - t0 < 60 and not scm_box(v.shot('scm')):
            time.sleep(2)
        if time.time() - t0 < 60:
            to_corner(q, PS2)
            q.move(PS2, 5, 0, 64)                   # OK at 320, 296
            q.move(PS2, 0, 4, 74)
            q.click(PS2, 'left')
            print('closed the "service failed" box')
        t0 = time.time()
        while time.time() - t0 < 90 and not white(v.shot('wheeltst')):
            time.sleep(2)
        time.sleep(2)
        to_corner(q, PS2)                           # the middle of wheeltst (320, 245)
        q.move(PS2, 5, 0, 64)
        q.move(PS2, 0, 5, 49)
    v.shot('mouse0')
    q.click(PS2, 'left')                            # an ordinary click
    time.sleep(0.8)
    q.click(PS2, 'side')                            # back
    time.sleep(0.5)
    q.click(USB, 'extra')                           # forward
    time.sleep(0.8)
    # autoscroll with a click of the PS/2 wheel button
    q.click(PS2, 'middle')
    time.sleep(0.6)
    v.shot('mouse1-origin')
    q.move(PS2, 0, 5, 12)                           # 60 pixels down: scrolls down
    time.sleep(1.5)
    v.shot('mouse2-down')
    q.move(PS2, 0, -5, 12)
    q.move(PS2, 5, 0, 12)                           # right
    time.sleep(1.5)
    q.move(PS2, -5, 0, 12)
    time.sleep(0.3)
    q.click(PS2, 'left')                            # ends it, the click goes nowhere
    time.sleep(0.8)
    # hold the USB mouse's wheel button and drag up
    q.btn(USB, 'middle', True)
    time.sleep(0.3)
    q.move(USB, 0, -5, 10)
    time.sleep(1.5)
    v.shot('mouse3-drag')
    q.btn(USB, 'middle', False)
    time.sleep(0.8)
    q.move(USB, 0, 5, 10)
    time.sleep(0.5)
    q.click(PS2, 'left')                            # an ordinary click again
    time.sleep(0.5)
    q.click(USB, 'right')
    time.sleep(1)
    v.shot('mouse4')
    done = v.wait_serial('##AGENT-SHUTDOWN##', 240)
    time.sleep(8)
    v.quit()
    log_path = os.path.join(vm.OUT, 'WHEEL.LOG')
    if os.path.exists(log_path):
        os.remove(log_path)
    vm.mget(work, '/WHEEL.LOG', log_path)
    log = open(log_path, 'rb').read().decode('latin1') if os.path.exists(log_path) else ''
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    print('\n'.join(l for l in ulog.splitlines() if 'ps2' in l or 'mouse' in l))
    c = parse(log)
    print({k: v for k, v in c.items()})
    hook = nt4 and not nohook
    checks = [
        ('PS/2 mouse in IntelliMouse Explorer mode (id 4)', 'id 4)' in ulog),
        ('button 4 (PS/2) -> Alt+Left', c['alt-left'] == 1),
        ('button 5 (USB) -> Alt+Right', c['alt-right'] == 1),
        ('no X button messages (NT has none)', c['xbutton'] == 0),
        ('autoscroll started twice', c['starts'] == 2),
        ('scroll cursor while it runs (hot spot 16,16)', len(c['on']) == 2 and all(h == (16, 16) for h in c['on'])),
        ('arrow cursor again afterwards', len(c['off']) == 2 and all(h != (16, 16) for h in c['off'])),
        ('scrolled down', (c['wheel-'] if nt4 else c['vdown']) > 0),
        ('scrolled right', c['hright'] > 0),
        ('dragged up: scrolled up', (c['wheel+'] if nt4 else c['vup']) > 0),
        ('middle clicks did not reach the window', c['mbutton'] == 0),
        ('the click that ended it did not reach the window; the others did', c['lbutton'] == 2),
        ('right click after autoscroll', c['rbutton'] == 1),
        ('autoscroll through %s' % ('the mouse hook' if hook else 'the driver'),
         ('usbmon takes buttons 1' in ulog and 'usbmon takes buttons 3' not in ulog) if hook else
         'usbmon takes buttons 3' in ulog),
    ]
    ok = done and bool(log)
    for name, good in checks:
        ok &= good
        print('%s %s' % ('ok  ' if good else 'FAIL', name))
    print('RESULT mouse %s%s%s %s' % (hc, ' nt4' if nt4 else ' nt31', ' nohook' if nohook else '',
                                      'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
