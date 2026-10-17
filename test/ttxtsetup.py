#!/usr/bin/env python3
"""Text-mode setup with the USBNT driver disk (TXTSETUP.OEM), from the user's
installation media.  NOT RUN YET: there are no Windows CDs on the machine the
script was written on; it needs them as arguments.

  ttxtsetup.py OS --iso SETUP.ISO [--floppies DISK1.IMG,DISK2.IMG,DISK3.IMG]
               [--oem build/out/USBNT2K.IMG] [--hc xhci|ehci|uhci|ohci] [--timeout 1800]

  OS: nt31 nt35 nt351 nt4 w2k xp xp64 2003 2003x64

NT 3.1, 3.5 and 3.51 CDs do not boot: give their setup boot floppies
(--floppies; NT 3.1 / 3.5 / 3.51 "Setup Boot Disk", "Disk #2", "Disk #3" as
1.44 MB images); NT 4.0 and later boot from the CD (floppies optional).

QEMU (i440fx) gets the CD on IDE, the floppy drive, a 2 GB empty USB disk and
a USB keyboard on the chosen USB controller (xHCI by default: no Windows of
the time has a driver for it, so whatever disk setup shows there is
USBNT's).  The screen is read from VGA text memory (setup runs in text mode),
and the keys are sent like a person would:

  2000 / XP / 2003     F6 at "Press F6 if you need to install a third party
                       SCSI or RAID driver", S at "Setup could not determine
                       the type of one or more mass storage devices"
  NT 3.1 - 4.0         Custom setup (NT 3.x), S at the mass storage screen
                       (skip / specify additional device), "Other (Requires
                       disk provided by a hardware manufacturer)"
  all                  the floppy is changed to the driver disk (USBNTNT / USBNT2K / USBNT64.IMG) when setup asks
                       for the manufacturer's disk (and to the next boot
                       floppy when it asks for one), the USBNT entry for the
                       OS is chosen

Checks: setup lists the USBNT choices from TXTSETUP.OEM, loads the driver
(the mass storage list names USBNT), the keys of the USB keyboard reach
setup after the driver took the controller (one Enter is sent through it),
and setup's partition screen shows the USB disk (a disk "on usbnt" or of the
USB disk's size).  Nothing is installed: setup is left with F3.  Screens go
to $MSC_W/txtsetup-OS/ (text and PNG).
"""
import argparse, json, os, re, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
W = os.environ.get('MSC_W', '/tmp/usbt-wttxt')
QEMU = os.environ.get('QEMU', '/home/claude/qemu-src/build/qemu-system-i386')
QEMU64 = os.environ.get('QEMU64', '/home/claude/qemu-src/build/qemu-system-x86_64')
HCS = {'xhci': 'qemu-xhci', 'ehci': 'usb-ehci', 'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci'}
# the TXTSETUP.OEM choice of each OS (its description starts like this)
CHOICE = {
    'nt31': 'USBNT - Windows NT 3.1 to 4.0', 'nt35': 'USBNT - Windows NT 3.1 to 4.0',
    'nt351': 'USBNT - Windows NT 3.1 to 4.0', 'nt4': 'USBNT - Windows NT 3.1 to 4.0',
    'w2k': 'USBNT - Windows 2000 / XP: USB 3', 'xp': 'USBNT - Windows 2000 / XP: USB 3',
    '2003': 'USBNT - Windows 2000 / XP: USB 3',
    'xp64': 'USBNT - Windows XP / 2003 x64: USB 3', '2003x64': 'USBNT - Windows XP / 2003 x64: USB 3',
}
PNP = ('w2k', 'xp', 'xp64', '2003', '2003x64')


class Vm:
    def __init__(self, a, out):
        self.out = out
        self.qmp_path = os.path.join(out, 'qmp.sock')
        if os.path.exists(self.qmp_path):
            os.remove(self.qmp_path)
        x64 = a.os in ('xp64', '2003x64')
        disk = os.path.join(out, 'usbdisk.img')
        with open(disk, 'wb') as f:
            f.truncate(1 << 30)
            # an empty MBR with a disk signature: an initialized disk, which USBNT reports as a fixed
            # disk (an unsigned one stays removable media, which setup does not offer)
            f.seek(0x1B8)
            f.write(b'USBN\0\0')
            f.seek(0x1FE)
            f.write(b'\x55\xAA')
        first_floppy = a.floppies[0] if a.floppies else a.oem
        self.usb_only = a.usb_only
        # --usb-only: the keyboard controller has no PS/2 keyboard (test/qemu/ps2-kbd-absent.patch);
        # setupldr reads the USB keyboard through the BIOS, text-mode setup only through usbnt.sys
        nops2 = ['-global', 'ps2-kbd.absent=on'] if a.usb_only else []
        if a.ide:
            # an empty IDE disk next to the USB one: the usual case (Windows onto the internal disk)
            ide = os.path.join(out, 'idedisk.img')
            with open(ide, 'wb') as f:
                f.truncate(2 << 30)
            nops2 += ['-drive', 'file=%s,format=raw,if=ide,index=0' % ide]
        args = [QEMU64 if x64 else QEMU, '-machine', 'pc'] + nops2 + ['-cpu', 'qemu64' if x64 else 'pentium3',
                '-m', '256' if a.os.startswith('nt3') else '512', '-display', 'none',
                '-vga', 'none', '-device', 'VGA,id=vga0', '-device', 'secondary-vga,id=vga1',
                '-rtc', 'base=localtime', '-no-reboot', '-qmp', 'unix:%s,server=on,wait=off' % self.qmp_path,
                '-drive', 'file=%s,media=cdrom,if=ide,index=2,readonly=on' % a.iso,
                '-drive', 'file=%s,format=raw,if=floppy,index=0' % first_floppy,
                '-device', '%s,id=hc' % HCS[a.hc],
                '-drive', 'file=%s,format=raw,if=none,id=ud' % disk,
                '-device', 'usb-storage,bus=hc.0,drive=ud',
                '-device', 'usb-kbd,bus=hc.0,display=vga1,id=ukbd',
                '-serial', 'file:' + os.path.join(out, 'com1.log'),
                '-boot', 'a' if a.floppies else 'd']
        self.p = subprocess.Popen(args, stdout=open(os.path.join(out, 'qemu.log'), 'w'), stderr=subprocess.STDOUT)
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try:
                self.s.connect(self.qmp_path)
                break
            except OSError:
                time.sleep(0.05)
        self.f = self.s.makefile('rwb')
        self.f.readline()
        self.qmp('qmp_capabilities')
        self.n = 0

    def qmp(self, cmd, **args):
        self.f.write((json.dumps({'execute': cmd, 'arguments': args}) + '\n').encode())
        self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r

    def hmp(self, line):
        return self.qmp('human-monitor-command', **{'command-line': line}).get('return', '')

    def key(self, k, usb=False, wait=0.4):
        """a key on QEMU's PS/2 keyboard, or with usb=True on the USB keyboard only"""
        if usb or self.usb_only:
            for down in (True, False):
                self.qmp('input-send-event', device='vga1',
                         events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': k}}}])
                time.sleep(0.08)
        else:
            self.hmp('sendkey ' + k)
        time.sleep(wait)

    def floppy(self, img):
        print('  floppy:', os.path.basename(img))
        self.hmp('change floppy0 %s raw' % img)
        time.sleep(1)

    def screen(self):
        """(rows of text, rows of attributes) of VGA text memory, 80 columns, 25 or 50 rows"""
        f = os.path.join(self.out, 'vram.bin')
        self.qmp('pmemsave', val=0xB8000, size=80 * 50 * 2, filename=f)
        time.sleep(0.2)
        try:
            b = open(f, 'rb').read()
        except OSError:
            return [], []
        rows, attrs = [], []
        for r in range(50):
            line = b[r * 160:(r + 1) * 160]
            rows.append(''.join(chr(c) if 32 <= c < 127 else ' ' for c in line[0::2]))
            attrs.append(list(line[1::2]))
        return rows, attrs

    def save(self, tag):
        self.n += 1
        rows, _ = self.screen()
        name = os.path.join(self.out, '%03d-%s' % (self.n, tag))
        open(name + '.txt', 'w').write('\n'.join(r.rstrip() for r in rows))
        self.qmp('screendump', filename=name + '.ppm')
        try:
            from PIL import Image
            time.sleep(0.3)
            Image.open(name + '.ppm').save(name + '.png')
            os.remove(name + '.ppm')
        except Exception:
            pass

    def quit(self):
        try:
            self.qmp('quit')
        except Exception:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()


def full_install(vm):
    """partition screen -> format choice -> formatting / copying, with the keyboard of vm.key"""
    def wait(words, t):
        t0 = time.time()
        while time.time() - t0 < t:
            rows = vm.screen()[0]
            if any(has(rows, w) for w in words):
                return True
            time.sleep(2)
        return False
    vm.key('ret', wait=3)
    if not wait(['Format the partition using', 'To continue Setup, press C'], 120):
        vm.save('no-format-screen')
        return False
    if has(vm.screen()[0], 'To continue Setup, press C'):
        vm.key('c', wait=3)
        wait(['Format the partition using'], 60)
    vm.save('format')
    vm.key('ret', wait=3)
    ok = wait(['Setup is formatting', 'Setup is copying', 'Please wait while Setup formats',
               'Please wait while Setup copies'], 180)
    vm.save('formatting' if ok else 'not-formatting')
    return ok


def has(rows, *words):
    t = ' '.join(rows).lower()
    return all(w.lower() in t for w in words)


def highlighted(rows, attrs, text):
    """the row holding text is drawn in another colour than most rows (the menu bar)"""
    for r, line in enumerate(rows):
        c = line.find(text)
        if c >= 0:
            mine = attrs[r][c]
            common = max(set(attrs[r]), key=attrs[r].count)
            others = [attrs[i][c] for i in range(len(rows)) if i != r and rows[i][c:c + 4].strip()]
            return mine != common or (others and mine != max(set(others), key=others.count))
    return False


def select(vm, text, limit=30):
    """moves the menu bar to the entry that starts with text, ENTER"""
    for _ in range(limit):
        rows, attrs = vm.screen()
        if highlighted(rows, attrs, text):
            vm.key('ret', wait=2)
            return True
        vm.key('down', wait=0.5)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('os', choices=sorted(CHOICE))
    ap.add_argument('--iso', required=True)
    ap.add_argument('--floppies', default='')
    ap.add_argument('--oem', default='')
    ap.add_argument('--hc', default='xhci', choices=sorted(HCS))
    ap.add_argument('--timeout', type=int, default=1800)
    ap.add_argument('--ide', action='store_true', help='an empty IDE disk as well')
    ap.add_argument('--full', action='store_true', help='on from the partition screen to formatting')
    ap.add_argument('--usb-only', action='store_true', help='no PS/2 keyboard: every key from the USB keyboard')
    a = ap.parse_args()
    a.floppies = [f for f in a.floppies.split(',') if f]
    if not a.oem:
        a.oem = os.path.join(ROOT, 'build', 'out', 'USBNTNT.IMG' if a.os.startswith('nt') else
                             'USBNT64.IMG' if a.os in ('xp64', '2003x64') else 'USBNT2K.IMG')
    if a.os in ('nt31', 'nt35', 'nt351') and not a.floppies:
        sys.exit('%s: the CD does not boot, give the setup boot floppies (--floppies)' % a.os)
    out = os.path.join(W, 'txtsetup-' + a.os)
    os.makedirs(out, exist_ok=True)
    vm = Vm(a, out)
    res = {'USBNT choices listed': False, 'USBNT driver loaded': False, 'USB keyboard in setup': None,
           'USB disk on the partition screen': False}
    state = {'f6': 0, 'listed': False, 'loaded': False, 'usbkey': False, 'floppy': 1, 'license': 0, 'last': ''}
    t0 = time.time()
    fail = None
    while time.time() - t0 < a.timeout and fail is None:
        if vm.p.poll() is not None:
            fail = 'QEMU ended (the machine reset or powered off)'
            break
        rows, attrs = vm.screen()
        text = '\n'.join(rows)
        if text != state['last']:
            state['last'] = text
            vm.save('screen')
        # ---- the end: errors, the partition screen ----
        if has(rows, 'STOP:') or has(rows, '*** STOP'):
            fail = 'blue screen'
        elif has(rows, 'txtsetup.oem') and (has(rows, 'corrupt') or has(rows, 'invalid') or has(rows, 'error')):
            fail = 'setup rejects TXTSETUP.OEM'
        elif has(rows, 'usbnt.sys') and (has(rows, 'could not') or has(rows, 'cannot')):
            fail = 'setup cannot load usbnt.sys'
        elif state['loaded'] and (has(rows, 'unpartitioned space') or has(rows, 'partition')) and \
                (has(rows, 'on usbnt') or has(rows, '1023 MB') or has(rows, '1024 MB') or has(rows, '1,023')):
            res['USB disk on the partition screen'] = True
            vm.save('partitions')
            if not a.full:
                break
            # --full: on through the USB keyboard - ENTER (the selected disk), ENTER (format with
            # NTFS), until setup formats / copies files
            res['ENTER: partition, format (USB keyboard)'] = full_install(vm)
            break
        elif has(rows, 'To continue Setup, press C'):
            vm.key('c', wait=3)                 # an empty disk: continue
        elif has(rows, 'did not find any hard disk') or has(rows, 'no hard disk'):
            fail = 'setup found no hard disk'
        # ---- 2000 / XP / 2003: F6 ----
        elif has(rows, 'Press F6 if you need to install a third party') and state['f6'] < 20:
            vm.key('f6', wait=1)
            state['f6'] += 1
        # ---- the manufacturer's disk ----
        elif has(rows, 'insert the disk labeled') or has(rows, 'insert the') and has(rows, 'drive A:'):
            low = text.lower()
            if 'usbnt' in low or 'manufacturer' in low or 'hardware support' in low:
                vm.floppy(a.oem)
            else:
                m = re.search(r'disk\s*#?\s*(\d)', low)
                n = int(m.group(1)) if m else state['floppy'] + 1
                if a.floppies and 1 <= n <= len(a.floppies):
                    vm.floppy(a.floppies[n - 1])
                    state['floppy'] = n
            vm.key('ret', wait=4)
        elif has(rows, CHOICE[a.os][:20]):
            # the TXTSETUP.OEM menu (or the list of the chosen devices)
            if has(rows, 'Setup will load support for') or has(rows, 'Setup has recognized') or \
                    has(rows, 'following mass storage device') or has(rows, 'following SCSI'):
                if not state['loaded']:
                    state['loaded'] = True
                    res['USBNT driver loaded'] = True
                    print('  USBNT is in the mass storage list')
                # one ENTER through the USB keyboard (the driver has the controller now)
                if not state['usbkey']:
                    state['usbkey'] = True
                    before = text
                    vm.key('ret', usb=True, wait=8)
                    res['USB keyboard in setup'] = '\n'.join(vm.screen()[0]) != before
                    print('  USB keyboard:', res['USB keyboard in setup'])
                    if res['USB keyboard in setup']:
                        continue
                vm.key('ret', wait=3)
            else:
                res['USBNT choices listed'] = True
                vm.save('choices')
                if not select(vm, CHOICE[a.os]):
                    fail = 'the USBNT choice for %s cannot be selected' % a.os
                time.sleep(10)
        elif has(rows, 'Other (Requires disk provided by a hardware manufacturer)'):
            if not select(vm, 'Other (Requires disk'):
                fail = '"Other" cannot be selected'
        elif not state['loaded'] and (has(rows, 'S=Specify Additional Device') or
                                      has(rows, 'Setup could not determine the type') or
                                      has(rows, 'specify additional') or has(rows, 'S=Skip Detection') or
                                      has(rows, 'To skip mass storage device detection')):
            vm.key('s', wait=3)
        elif has(rows, 'Express Setup') and has(rows, 'Custom Setup'):
            vm.key('c', wait=3)
        elif has(rows, 'F8=I agree') or has(rows, 'F8=I Agree') or has(rows, 'PAGE DOWN'):
            state['license'] += 1
            for _ in range(10):
                vm.key('pgdn', wait=0.2)
            vm.key('f8', wait=3)
        elif has(rows, 'The above list matches my computer'):
            select(vm, 'The above list matches')
        elif has(rows, 'ENTER=Continue') or has(rows, 'To set up Windows') or has(rows, 'ENTER=Install'):
            welcome = has(rows, 'Welcome to Setup') and state['loaded']
            vm.key('ret', wait=3)
            if welcome and a.usb_only:
                # text-mode setup itself (setupdd) read this key, from USBNT's USB keyboard only
                t1 = time.time()
                while time.time() - t1 < 60 and has(vm.screen()[0], 'Welcome to Setup'):
                    time.sleep(1)
                res['USB keyboard at Welcome (no PS/2 keyboard)'] = not has(vm.screen()[0], 'Welcome to Setup')
                if not res['USB keyboard at Welcome (no PS/2 keyboard)']:
                    fail = 'the USB keyboard does not reach text-mode setup'
                print('  USB keyboard at Welcome:', res['USB keyboard at Welcome (no PS/2 keyboard)'])
        else:
            time.sleep(3)
    vm.save('last')
    try:
        for _ in range(3):
            vm.key('f3', wait=1)
            vm.key('f3', wait=1)
    except (OSError, ValueError):
        pass                                    # setup quit: the machine is off (-no-reboot)
    vm.quit()
    os.remove(os.path.join(out, 'usbdisk.img'))
    if a.ide:
        os.remove(os.path.join(out, 'idedisk.img'))
    print('screens in', out)
    if fail:
        print('FAIL:', fail)
    ok = fail is None and res['USBNT choices listed'] and res['USBNT driver loaded'] and \
        res['USB disk on the partition screen']
    for k, v in res.items():
        print('%-36s %s' % (k, {True: 'PASS', False: 'FAIL', None: 'not reached'}[v]))
    print('RESULT ttxtsetup %s %s %s' % (a.os, a.hc, 'PASS' if ok else 'FAIL'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
