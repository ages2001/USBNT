#!/usr/bin/env python3
"""Windows 2000 / XP text-mode setup from a CD with USBNT integrated the way nLite
does it from TXTSETUP.OEM ("Insert - Drivers - txtsetup.oem, text-mode driver"),
on a machine whose keyboard controller has no PS/2 keyboard (QEMU -global ps2-kbd.absent=on,
test/qemu/ps2-kbd-absent.patch: nothing answers on the keyboard port, no key comes from it -
Windows finds no PS/2 keyboard, like a PC with only a USB keyboard; without a controller
at all NTDETECT does not come back): the only keyboard is a USB keyboard on an xHCI,
which only USBNT drives.

  tnlite.py --iso W2K.ISO [--choice usbnt2k|usbnt2ka] [--hc xhci] [--debug] [--steps N]

The CD is rebuilt (W/<name>.iso): USBNT.SYS and the INFs into I386, TXTSETUP.SIF
gets what nLite writes - [SourceDisksFiles] usbnt.sys = 1,,,,,,4_,4,1,,,1,4,
[HardwareIdsDatabase] id = "usbnt" for every id of the choice (nLite keeps only
the id and the service), [SCSI.Load] usbnt = usbnt.sys,4, [SCSI] usbnt = "...";
DOSNET.INF [Files] d1,usbnt.sys.  --debug also gives usbnt.sys DebugPort=0x3F8 in
the setup hive (SETUPREG.HIV), so its log comes out of COM1.

Then text-mode setup is driven with the USB keyboard only, reading the screen
from VGA text memory: Welcome (ENTER), license (F8), the partition list (ENTER on
the empty disk), format (ENTER), until "Setup is copying files" / "Setup is
formatting" - every step needs a key that only the USB keyboard can give.
Screens go to W/<name>/.
"""
import struct, argparse, json, os, re, shutil, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OEMD = os.path.join(ROOT, 'build', 'out', 'oem')
W = os.environ.get('NLITE_W', '/home/claude/w2k/work')
QEMU = os.environ.get('QEMU', '/home/claude/qemu-src/build/qemu-system-x86_64')


def sif_insert(text, section, lines):
    """lines appended at the end of the first [section]"""
    m = re.search(r'^\[%s\]\s*$' % re.escape(section), text, re.M | re.I)
    if not m:
        return text + '\r\n[%s]\r\n' % section + ''.join(l + '\r\n' for l in lines)
    nxt = re.search(r'^\[', text[m.end():], re.M)
    end = m.end() + (nxt.start() if nxt else len(text) - m.end())
    body = text[m.end():end].rstrip('\r\n') + '\r\n'
    return text[:m.end()] + body + ''.join(l + '\r\n' for l in lines) + '\r\n' + text[end:]


def oem(disk):
    sec, cur = {}, None
    for line in open(os.path.join(OEMD, disk, 'TXTSETUP.OEM'), 'rb').read().decode('ascii').split('\r\n'):
        s = line.strip()
        if not s or s[0] == ';':
            continue
        if s.startswith('['):
            cur = s[1:-1].lower()
            sec[cur] = []
        elif '=' in s:
            k, v = s.split('=', 1)
            # commas inside a quoted string stay in it
            sec[cur].append((k.strip(), [x.strip().strip('"') for x in re.findall(r'"[^"]*"|[^,]+', v)]))
    return sec


def lf_lists(hiv):
    """hivex writes new subkey lists as 'lh' (Windows XP's hives); Windows 2000's setup hive
    (version 1.3) only knows 'lf' - with an 'lh' the kernel stops with 0x67
    CONFIG_INITIALIZATION_FAILED. Every list becomes 'lf' with the name-prefix hints."""
    d = bytearray(open(hiv, 'rb').read())
    off = 0x1000
    while off < len(d):
        size = struct.unpack_from('<I', d, off + 8)[0]
        c = off + 0x20
        while c < off + size:
            n = struct.unpack_from('<i', d, c)[0]
            if n < 0 and d[c + 4:c + 6] in (b'lf', b'lh'):
                d[c + 4:c + 6] = b'lf'
                for i in range(struct.unpack_from('<H', d, c + 6)[0]):
                    nk = 0x1004 + struct.unpack_from('<I', d, c + 8 + i * 8)[0]
                    ln = struct.unpack_from('<H', d, nk + 0x48)[0]
                    name = bytes(d[nk + 0x4c:nk + 0x4c + min(ln, 4)])
                    d[c + 12 + i * 8:c + 16 + i * 8] = name.ljust(4, b'\0')
            c += abs(n)
        off += size
    open(hiv, 'wb').write(bytes(d))


def build_iso(src_iso, choice, debug, out_iso, tree, which='all', sets=()):
    if os.path.exists(tree):
        shutil.rmtree(tree)
    os.makedirs(tree)
    subprocess.check_call(['7z', 'x', '-o' + tree, src_iso], stdout=subprocess.DEVNULL)
    boot = os.path.join(tree, '[BOOT]')
    bimg = [f for f in os.listdir(boot)][0]
    shutil.move(os.path.join(boot, bimg), os.path.join(W, 'boot.img'))
    shutil.rmtree(boot)
    i386 = os.path.join(tree, 'I386')
    disk = '2KXP'
    sec = oem(disk)
    for f in os.listdir(os.path.join(OEMD, disk)):
        if f.upper().endswith(('.SYS', '.INF')):
            shutil.copyfile(os.path.join(OEMD, disk, f), os.path.join(i386, f.upper()))
    desc = [v[0] for k, v in sec['scsi'] if k == choice][0]
    ids = [v[0] for k, v in sec['hardwareids.scsi.' + choice]]
    # --ids: what of them goes into [HardwareIdsDatabase] - all (nLite), the PCI ones, none
    # (a setup that knows no service for the controller, as Windows 2000 with an F6 disk)
    sif_ids = [i for i in ids if which == 'all' or (which == 'pci' and i.upper().startswith('PCI\\'))]
    p = os.path.join(i386, 'TXTSETUP.SIF')
    t = open(p, 'rb').read().decode('latin1')
    t = sif_insert(t, 'SourceDisksFiles', ['usbnt.sys = 1,,,,,,4_,4,1,,,1,4'])
    if sif_ids:
        t = sif_insert(t, 'HardwareIdsDatabase', ['%s = "usbnt"' % i for i in sif_ids])
    t = sif_insert(t, 'SCSI.Load', ['usbnt = usbnt.sys,4'])
    t = sif_insert(t, 'SCSI', ['usbnt = "%s"' % desc])
    open(p, 'wb').write(t.encode('latin1'))
    p = os.path.join(i386, 'DOSNET.INF')
    t = open(p, 'rb').read().decode('latin1')
    t = sif_insert(t, 'Files', ['d1,usbnt.sys'])
    open(p, 'wb').write(t.encode('latin1'))
    if debug or sets:
        hiv = os.path.join(i386, 'SETUPREG.HIV')
        vals = ([('DebugPort', 0x3f8), ('DebugLevel', 3)] if debug else []) + \
               [(k, int(v, 0)) for k, v in (x.split('=') for x in sets)]
        script = ('cd ControlSet001\\Services\nadd usbnt\ncd usbnt\nadd Parameters\ncd Parameters\nsetval %d\n'
                  % len(vals)) + ''.join('%s\ndword:0x%x\n' % kv for kv in vals) + 'commit\n'
        sh = os.path.join(W, 'hiv.hsh')
        open(sh, 'w').write(script)
        r = subprocess.run(['hivexsh', '-w', '-f', sh, hiv], capture_output=True, text=True)
        if r.returncode != 0:
            print('setup hive:', r.stderr)
        lf_lists(hiv)
    # the boot image at the root of the tree, like the CD's own layout
    shutil.copyfile(os.path.join(W, 'boot.img'), os.path.join(tree, 'BOOT.BIN'))
    subprocess.check_call(['genisoimage', '-quiet', '-o', out_iso, '-b', 'BOOT.BIN', '-no-emul-boot',
                           '-boot-load-size', '4', '-hide', 'BOOT.BIN', '-N', '-J', '-joliet-long', '-D',
                           '-relaxed-filenames', '-iso-level', '2', '-V', 'W2KUSBNT', tree])
    print('CD:', out_iso, '-', len(ids), 'ids,', desc)


class Vm:
    def __init__(self, iso, out, hc):
        self.out = out
        self.qmp_path = os.path.join(out, 'qmp.sock')
        if os.path.exists(self.qmp_path):
            os.remove(self.qmp_path)
        disk = os.path.join(out, 'disk.img')
        with open(disk, 'wb') as f:
            f.truncate(2 << 30)
        # a 1 GB USB disk with an empty MBR and a disk signature (USBNT reports such a disk as
        # fixed media): setup must list it on the partition screen
        udisk = os.path.join(out, 'usbdisk.img')
        with open(udisk, 'wb') as f:
            f.truncate(1 << 30)
            f.seek(0x1B8)
            f.write(b'USBD\0\0')
            f.seek(0x1FE)
            f.write(b'\x55\xAA')
        hcdev = {'xhci': 'qemu-xhci', 'ehci': 'usb-ehci', 'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci'}[hc]
        args = [QEMU, '-machine', 'pc', '-global', 'ps2-kbd.absent=on', '-cpu', 'pentium3', '-m', '512',
                '-display', 'none', '-vga', 'none', '-device', 'VGA,id=vga0,addr=0x2', '-nic', 'none', '-rtc', 'base=localtime',
                '-no-reboot',
                '-qmp', 'unix:%s,server=on,wait=off' % self.qmp_path,
                '-drive', 'file=%s,format=raw,if=ide,index=0' % disk,
                '-drive', 'file=%s,media=cdrom,if=ide,index=2,readonly=on' % iso,
                '-device', '%s,id=hc' % hcdev, '-device', 'usb-kbd,bus=hc.0,display=vga0,id=ukbd',
                '-drive', 'file=%s,format=raw,if=none,id=ud' % udisk, '-device', 'usb-storage,bus=hc.0,drive=ud',
                '-serial', 'file:' + os.path.join(out, 'com1.log'), '-boot', 'd']
        open(os.path.join(out, 'cmdline.txt'), 'w').write(' '.join(args))
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

    def key(self, k, wait=1.0):
        """a key of the USB keyboard (the machine has no other keyboard)"""
        for down in (True, False):
            self.qmp('input-send-event', device='vga0', events=[{'type': 'key', 'data': {'down': down,
                                                                         'key': {'type': 'qcode', 'data': k}}}])
            time.sleep(0.1)
        time.sleep(wait)

    def screen(self):
        f = os.path.join(self.out, 'vram.bin')
        self.qmp('pmemsave', val=0xB8000, size=80 * 50 * 2, filename=f)
        time.sleep(0.2)
        try:
            b = open(f, 'rb').read()
        except OSError:
            return []
        return [''.join(chr(c) if 32 <= c < 127 else ' ' for c in b[r * 160:(r + 1) * 160][0::2]) for r in range(50)]

    def save(self, tag):
        self.n += 1
        name = os.path.join(self.out, '%03d-%s' % (self.n, tag))
        open(name + '.txt', 'w').write('\n'.join(r.rstrip() for r in self.screen()))
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


def text(rows):
    return ' '.join(r.strip() for r in rows).lower()


def wait_for(vm, words, timeout, tag):
    """until one of words is on the screen; the word, or None"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        t = text(vm.screen())
        for w in words:
            if w.lower() in t:
                vm.save(tag)
                return w
        time.sleep(2)
    vm.save(tag + '-timeout')
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--iso', required=True)
    ap.add_argument('--choice', default='usbnt2k')
    ap.add_argument('--hc', default='xhci')
    ap.add_argument('--debug', action='store_true')
    ap.add_argument('--ids', default='all', choices=['all', 'pci', 'none'])
    ap.add_argument('--keep', action='store_true')
    ap.add_argument('--set', action='append', default=[], help='Name=value: a usbnt parameter in the setup hive')
    a = ap.parse_args()
    os.makedirs(W, exist_ok=True)
    name = 'nlite-%s-%s-%s%s' % (a.choice, a.hc, a.ids, ''.join('-' + x.split('=')[0] + x.split('=')[1] for x in a.set))
    out = os.path.join(W, name)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    iso = os.path.join(W, name + '.iso')
    build_iso(a.iso, a.choice, a.debug, iso, os.path.join(W, 'tree'), a.ids, a.set)
    vm = Vm(iso, out, a.hc)
    res = {}
    # the text-mode screens and the USB key each one wants, until setup formats / copies
    flow = [('Welcome to Setup', 'ret', 'Welcome -> next'),
            ('To continue Setup, press C', 'c', 'C: continue on a new disk'),
            ('F8=I agree', 'f8', 'F8: license'),
            ('ENTER=Install', 'ret', 'ENTER: partition'),
            ('Format the partition using', 'ret', 'ENTER: format'),
            ('Setup will now format', 'f', 'F: format')]
    done = ['Setup is formatting', 'Setup is copying', 'Please wait while Setup formats',
            'Please wait while Setup copies']
    try:
        r = vm.qmp('human-monitor-command', **{'command-line': 'info qtree'})
        qt = r.get('return', '')
        res['no PS/2 keyboard (ps2-kbd absent=true), USB keyboard on the %s' % a.hc] = \
            'absent = true' in qt and 'usb-kbd' in qt
        w = wait_for(vm, ['Welcome to Setup', 'could not be found', 'STOP:', 'Setup cannot continue'], 600,
                     'welcome')
        res['setup starts, loads usbnt.sys (no "could not be found")'] = w == 'Welcome to Setup'
        seen = []
        t0 = time.time()
        while w == 'Welcome to Setup' and time.time() - t0 < 600:
            t = text(vm.screen())
            if any(d.lower() in t for d in done):
                vm.save('copying')
                break
            step = [f for f in flow if f[0].lower() in t and f[0] not in seen]
            if not step:
                time.sleep(2)
                continue
            word, key, label = step[0]
            seen.append(word)
            if word == 'ENTER=Install':
                res['USB disk on the partition screen'] = 'on usbnt' in t or '1023 mb' in t or '1024 mb' in t
            vm.save(key)
            vm.key(key, wait=3)
            # the screen goes away when the key was read
            t1 = time.time()
            while time.time() - t1 < 60 and word.lower() in text(vm.screen()):
                time.sleep(1)
            res['%s on the USB keyboard' % label] = word.lower() not in text(vm.screen())
            if not res['%s on the USB keyboard' % label]:
                vm.save('stuck')
                break
        else:
            pass
        t = text(vm.screen())
        res['setup formats / copies files (all keys from the USB keyboard)'] = any(d.lower() in t for d in done)
    finally:
        vm.save('end')
        vm.quit()
    log = open(os.path.join(out, 'com1.log'), 'rb').read().decode('latin1')
    print('---- usbnt log (COM1) ----')
    print('\n'.join(l for l in log.splitlines() if 'PnP: child' not in l)[-6000:])
    if a.debug and 'TextSetupHid=1' in a.set:
        # the keyboard went to setup's own hidusb / kbdhid through USBNT's USB bus interface
        res["keyboard to setup's hidusb / kbdhid (log)"] = "go to setup's hidusb" in log and \
            "to Windows' USB drivers" in log
        res['no keys lent to i8042prt / kbdhid (log)'] = 'USB keys go to the class driver of' not in log
    elif a.debug:
        # the keys went through kbdclass on USBNT's own keyboard device, not to another keyboard's class
        res['kbdclass on the USBNT keyboard (log)'] = 'keyboard class driver connected' in log
        res['no keys lent to i8042prt / kbdhid (log)'] = 'USB keys go to the class driver of' not in log
    for k, v in res.items():
        print('%-60s %s' % (k, 'PASS' if v else 'FAIL'))
    ok = bool(res) and all(res.values())
    print('RESULT nlite %s %s %s' % (a.choice, a.hc, 'PASS' if ok else 'FAIL'))
    if not a.keep:
        os.remove(os.path.join(out, 'disk.img'))
        os.remove(os.path.join(out, 'usbdisk.img'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
