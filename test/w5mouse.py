"""Back / forward buttons and autoscroll on Windows 2000 (x86) and XP x64.

  python3 w5mouse.py [w2k|xp64 ...] [--keep] [--bat MS]

usbinst /install puts the USB mouse (QEMU's usb-mouse: five buttons) on
usbnt.sys (boot 1); after a restart usbmon and wheeltst (/center, kept on
top by fstest front) run and wheeltst logs to C:\\WHEEL.LOG.  Checked:

  - buttons 4 / 5 of the USB mouse arrive as WM_XBUTTONDOWN 1 / 2 and
    WM_APPCOMMAND back / forward (Windows knows X buttons; usbmon types no keys)
  - a click of the wheel button starts autoscroll through the low level mouse
    hook: origin mark, scroll cursor, WM_MOUSEWHEEL down, WM_HSCROLL right; a
    left click ends it and does not reach the window; the arrow is back
    (the PS/2 mouse of the Windows 2000 test image does not work, so only the
    USB mouse is used)
  - holding the USB mouse's wheel button and dragging up scrolls up, releasing
    it ends autoscroll
  - ordinary clicks reach the window before and after
  - XP x64: the PS/2 mouse's wheel (i8042prt's, with usbnt.sys as its upper filter,
    which resets the mouse to look for a touch pad and must give it back with its
    wheel): two notches up, then three down scroll up, then down.  --bat MS: the
    PS/2 mouse's reset self test takes MS ms (test/qemu/ps2-mouse-bat.patch)

Work images: $W5_IMG/work-mouse-<guest>.raw (or $W5MOUSE_WORK).
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5
from w5usb import driver_files, no_signing_prompt
from ntmouse import parse

ROOT = os.path.join(HERE, '..')
OUT = os.path.join(ROOT, 'build', 'out')
USB = 'vga0'


def ev(vm, dev, events):
    a = {'events': events}
    if dev:
        a['device'] = dev
    vm.qmp('input-send-event', **a)


def btn(vm, dev, b, down):
    ev(vm, dev, [{'type': 'btn', 'data': {'down': down, 'button': b}}])
    time.sleep(0.15)


def click(vm, dev, b):
    btn(vm, dev, b, True)
    btn(vm, dev, b, False)


def move(vm, dev, dx, dy, steps):
    for _ in range(steps):
        ev(vm, dev, [{'type': 'rel', 'data': {'axis': 'x', 'value': dx}},
                     {'type': 'rel', 'data': {'axis': 'y', 'value': dy}}])
        time.sleep(0.05)


def white(png):
    try:
        from PIL import Image
        im = Image.open(png).convert('RGB')
        # the right half of wheeltst's client area (no text there, the pointer is in the
        # middle); the window is at 160, 100 (320 x 290) whatever the screen size
        return all(im.getpixel((x, y)) == (255, 255, 255) for x in (410, 430, 450) for y in (185, 245, 305))
    except Exception:
        return False


def run(guest, keep=False, batms=None):
    g = vm5.GUESTS[guest]
    ps2 = guest != 'w2k'                            # the Windows 2000 image's PS/2 mouse does not work
    F = 'C:\\USBNT\\FSTEST.EXE'
    # boot 1: install; Windows starts mouclass for a mouse that was installed while it ran
    # only at the next boot ("System Settings Change")
    bat = [
        'start /b %s clicker C:\\USBNT\\STOP 600000' % F,
        'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=3',
        '%s sleep 30000' % F,
        '%s write C:\\USBNT\\STOP x' % F,
        '%s sleep 2000' % F,
    ]
    files = driver_files() + [(os.path.join(OUT, 'wheeltst.exe'), '/USBNT/WHEELTST.EXE')]
    work = os.environ.get('W5MOUSE_WORK') or os.path.join(vm5.W5_IMG, 'work-mouse-%s.raw' % guest)
    work, g = vm5.prepare(guest, work=work, files=files, bat=bat)
    no_signing_prompt(work, g)
    # the VGA gets an id: the USB mouse is bound to it (QMP device=vga0).  The PCI slots stay
    # those of -vga std (VGA 2, NIC 3, then the disk controller): no new hardware for Windows
    front = ['-vga', 'none', '-device', 'VGA,id=vga0,addr=0x2', '-nic', 'none', '-device', 'e1000,addr=0x3']
    extra = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-mouse,bus=xhci.0,display=vga0,id=um']
    if batms:
        extra += ['-global', 'ps2-mouse.bat-delay=%s' % batms]
    res = {}
    vm = vm5.VM(guest, work, extra=extra, name='mouse-' + guest, front=front)
    res['boot 1: installed'] = vm.wait_serial('##AGENT-DONE##', 900)
    vm.wait_serial('##AGENT-SHUTDOWN##', 60)
    time.sleep(20)
    vm.quit()
    # boot 2: usbmon (which logon starts after an installation without /nomon), wheeltst
    vm5.set_bat(work, [
        'start /b %s clicker C:\\USBNT\\STOP 600000' % F,
        'start C:\\USBNT\\USBMON.EXE',
        '%s sleep 5000' % F,
        'start C:\\USBNT\\WHEELTST.EXE /center',
        'start %s front Wheel_test 30000' % F,
        '%s sleep 70000' % F,
        '%s write C:\\USBNT\\STOP x' % F,
        '%s sleep 2000' % F,
    ])
    vm = vm5.VM(guest, work, extra=extra, name='mouse-' + guest, front=front)
    try:
        res['boot 2: usb mouse on usbnt'] = vm.wait_log('pointer class driver connected', 600)
        # wheeltst is up (and on top) when the middle of the screen turns white
        t0 = time.time()
        while time.time() - t0 < 300 and not white(vm.shot('m0')):
            time.sleep(3)
        res['wheeltst up'] = time.time() - t0 < 300
        time.sleep(3)
        click(vm, USB, 'left')                      # an ordinary click
        time.sleep(0.8)
        click(vm, USB, 'side')                      # back
        time.sleep(0.5)
        click(vm, USB, 'extra')                     # forward
        time.sleep(0.8)
        click(vm, USB, 'middle')                    # autoscroll
        time.sleep(0.6)
        vm.shot('m1-origin')
        move(vm, USB, 0, 5, 12)
        time.sleep(1.5)
        vm.shot('m2-down')
        move(vm, USB, 0, -5, 12)
        move(vm, USB, 5, 0, 12)
        time.sleep(1.5)
        move(vm, USB, -5, 0, 12)
        time.sleep(0.3)
        click(vm, USB, 'left')                      # ends it
        time.sleep(0.8)
        btn(vm, USB, 'middle', True)                # hold and drag up
        time.sleep(0.3)
        move(vm, USB, 0, -5, 10)
        time.sleep(1.5)
        vm.shot('m3-drag')
        btn(vm, USB, 'middle', False)
        time.sleep(0.8)
        move(vm, USB, 0, 5, 10)
        time.sleep(0.5)
        click(vm, USB, 'left')
        time.sleep(0.5)
        click(vm, USB, 'right')
        time.sleep(1)
        if ps2:
            for b in ('wheel-up', 'wheel-up', 'wheel-down', 'wheel-down', 'wheel-down'):
                click(vm, None, b)                  # no device: the unbound PS/2 mouse
                time.sleep(0.4)
            time.sleep(1)
        vm.shot('m4')
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
    print('\n'.join(l for l in log.splitlines() if 'mouse' in l or 'pointer' in l))
    c = parse(wl)
    print(c)
    tail = wl[wl.rfind('RBUTTONDOWN'):] if 'RBUTTONDOWN' in wl else ''
    seq = [('up' if int(w[1]) > 0 else 'down') for w in (l.split() for l in tail.splitlines())
           if len(w) > 1 and w[0] == 'MOUSEWHEEL']
    back = wl.count('APPCOMMAND 1\r')
    fwd = wl.count('APPCOMMAND 2\r')
    res.update({
        'buttons 4, 5 -> WM_XBUTTONDOWN 1, 2': 'XBUTTONDOWN 1\r' in wl and 'XBUTTONDOWN 2\r' in wl and c['xbutton'] == 2,
        'WM_APPCOMMAND back / forward': back == 1 and fwd == 1,
        'no keys typed for them': c['alt-left'] == 0 and c['alt-right'] == 0,
        'autoscroll started twice': c['starts'] == 2,
        'scroll cursor while it runs': len(c['on']) == 2 and all(h == (16, 16) for h in c['on']),
        'arrow afterwards': len(c['off']) == 2 and all(h != (16, 16) for h in c['off']),
        'scrolled down (WM_MOUSEWHEEL)': c['wheel-'] > 0,
        'scrolled right (WM_HSCROLL)': c['hright'] > 0,
        'dragged up: scrolled up': c['wheel+'] > 0,
        'middle clicks did not reach the window': c['mbutton'] == 0,
        'ending click held back, the others not': c['lbutton'] == 2,
        'right click afterwards': c['rbutton'] == 1,
    })
    if ps2:
        res['PS/2 wheel: up, then down %s' % seq] = bool(seq) and 'up' in seq and 'down' in seq and \
            seq.index('down') > seq.index('up') and 'up' not in seq[seq.index('down'):]
    for k, v in res.items():
        print('%-40s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    ok = all(res.values())
    print('RESULT w5mouse %s %s' % (guest, 'PASS' if ok else 'FAIL'))
    return ok


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    keep = '--keep' in sys.argv
    batms = sys.argv[sys.argv.index('--bat') + 1] if '--bat' in sys.argv else None
    if batms:
        args = [a for a in args if a != batms]
    res = {gname: run(gname, keep, batms) for gname in (args or ['w2k', 'xp64'])}
    print(res)
    sys.exit(0 if all(res.values()) else 1)
