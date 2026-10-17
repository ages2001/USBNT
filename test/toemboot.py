#!/usr/bin/env python3
"""USBNT installed the way text-mode setup installs it from the driver disk
(TXTSETUP.OEM), without usbinst, on the installed test guests.  (There are
no installation CDs here: test/ttxtsetup.py runs setup itself with them.)

  toemboot.py GUEST [late] [boot] [usbboot] [--keep]     GUEST: nt31 nt4 w2k xp64

What text-mode setup leaves behind is made in a first boot of a work copy
(fstest svc / reg): the service usbnt (boot start, group SCSI miniport,
System32\\drivers\\usbnt.sys), the [Config.usbnt] values, and on 2000 / XP
the CriticalDeviceDatabase entries of the [HardwareIds.scsi.*] ids of the
chosen entry (read from build/out/oem/<NT|2KXP|XP64>/TXTSETUP.OEM; class SCSIAdapter, as
setup gives a SCSI driver).  DebugPort=0x2F8 is added for the test's log.

  late     (NT) the driver is started after logon, after kbdclass, as text-
           mode setup loads it: with nothing typing but a USB keyboard on xHCI
           (bound to a second display head, so QEMU's PS/2 keyboard stays
           quiet) the keys must reach a window (through i8042prt's class)
  boot     boot from the guest's disk, a USB stick on xHCI and a USB keyboard:
           the stick is read and written, the USB keyboard types; NT: kbdclass
           connects (ConnectMultiplePorts set by the driver); 2000 / XP: the
           first host controller is the device bus (no USBNT\\Bus device)
  usbboot  the same image as the only disk: a USB disk on xHCI, SeaBIOS boots
           it; the system must come up from USBNT's disk and write to it
"""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUTB = os.path.join(ROOT, 'build', 'out')
W = os.environ.setdefault('MSC_W', '/tmp/usbt-wttxt')
os.environ.setdefault('AUDIONT_OUT', os.path.join(W, 'nt'))
os.environ.setdefault('W5_OUT', os.path.join(W, 'w5'))
os.environ.setdefault('QEMU', '/home/claude/qemu-src/build/qemu-system-i386')
os.environ.setdefault('QEMU64', '/home/claude/qemu-src/build/qemu-system-x86_64')
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
sys.path.insert(0, HERE)
os.makedirs(W, exist_ok=True)
F = 'C:\\USBNT\\FSTEST.EXE'
SVC = 'SYSTEM\\CurrentControlSet\\Services\\usbnt'
SCSI_GUID = '{4D36E97B-E325-11CE-BFC1-08002BE10318}'
VGA2 = ['-vga', 'none', '-device', 'VGA,id=vga0', '-device', 'secondary-vga,id=vga1']


def oem_sections(choice):
    """sections of the TXTSETUP.OEM of the driver disk that has choice: name -> [(key, [values])]"""
    disk = 'NT' if choice == 'usbntnt' else 'XP64' if choice.startswith('usbnt64') else '2KXP'
    text = open(os.path.join(OUTB, 'oem', disk, 'TXTSETUP.OEM'), 'rb').read().decode('ascii')
    sec, cur = {}, None
    for line in text.split('\r\n'):
        s = line.strip()
        if not s or s[0] == ';':
            continue
        if s.startswith('['):
            cur = s[1:-1].lower()
            sec[cur] = []
        elif '=' in s:
            k, v = s.split('=', 1)
            sec[cur].append((k.strip(), [x.strip().strip('"') for x in v.split(',')]))
    return sec


def setup_registry(choice, nt):
    """fstest lines that make what text-mode setup makes for TXTSETUP.OEM choice"""
    sec = oem_sections(choice)
    drv = [v for k, v in sec['files.scsi.' + choice] if k == 'driver'][0]
    key = drv[2]
    lines = ['%s svc %s SCSI miniport' % (F, key)]
    for k, v in sec.get('config.' + key, []):
        sub, name, typ, data = v[:4]
        path = SVC + ('\\' + sub if sub else '')
        if typ == 'REG_DWORD':
            lines.append('%s reg %s %s dword:0x%s' % (F, path, name, data))
        else:
            lines.append('%s reg %s %s sz:%s' % (F, path, name, data))
    lines += ['%s reg %s\\Parameters DebugPort dword:0x2f8' % (F, SVC),
              '%s reg %s\\Parameters DebugLevel dword:2' % (F, SVC)]
    if not nt:
        for k, v in sec.get('hardwareids.scsi.' + choice, []):
            cddb = 'SYSTEM\\CurrentControlSet\\Control\\CriticalDeviceDatabase\\' + v[0].lower().replace('\\', '#')
            lines += ['%s reg %s Service sz:%s' % (F, cddb, v[1]), '%s reg %s ClassGUID sz:%s' % (F, cddb, SCSI_GUID)]
    return lines


def results(ser, word):
    m = re.search(r'INPTEST chars=\[(.*?)\]', ser)
    return m.group(1) if m else None


def type_usb(qmp, word):
    """keys of the USB keyboard only (it is bound to display head vga1)"""
    for ch in word:
        for down in (True, False):
            qmp('input-send-event', device='vga1',
                events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': ch}}}])
            time.sleep(0.06)
        time.sleep(0.1)


class Qmp:
    def __init__(self, path):
        import json, socket
        self.json = json
        self.s = socket.socket(socket.AF_UNIX)
        for _ in range(200):
            try:
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        self.f = self.s.makefile('rwb')
        self.f.readline()
        self('qmp_capabilities')

    def __call__(self, cmd, **args):
        self.f.write((self.json.dumps({'execute': cmd, 'arguments': args}) + '\n').encode())
        self.f.flush()
        while True:
            r = self.json.loads(self.f.readline())
            if 'return' in r or 'error' in r:
                return r


def rewrite_popen(mod, drop_img, usb_img):
    """the guest's disk off its own controller, onto xHCI as a USB disk SeaBIOS boots from"""
    real = mod.subprocess.Popen

    def popen(args, **kw):
        a = list(args)
        out, i = [], 0
        while i < len(a):
            if a[i] == '-drive' and a[i + 1].startswith('file=%s,' % drop_img):
                i += 2
                continue
            if usb_img and a[i] == '-device' and 'drive=boot' in a[i + 1]:
                i += 2
                continue
            out.append(a[i])
            i += 1
        if usb_img:
            out += ['-drive', 'file=%s,format=raw,if=none,id=ub' % usb_img,
                    '-device', 'usb-storage,bus=xhci.0,drive=ub,bootindex=0']
        if '-vga' in out and out[out.index('-vga') + 1] == 'std':
            j = out.index('-vga')
            out[j:j + 2] = VGA2                     # two heads: the USB keyboard is bound to the second
        return real(out, **kw)
    return real, popen


# ---------------------------------------------------------------------------
# NT 3.1 / NT 4.0 (AUDIONT's vm.py: IDE boot disk, agent on COM1)
# ---------------------------------------------------------------------------
def nt_vm(vm, work, extra, usbboot=False):
    qmp_path = os.path.join(vm.OUT, 'qmp.sock')
    com2 = os.path.join(vm.OUT, 'com2.log')
    for f in (qmp_path, com2):
        if os.path.exists(f):
            os.remove(f)
    extra = extra + ['-qmp', 'unix:%s,server,nowait' % qmp_path, '-serial', 'file:' + com2]
    os.environ['AUDIONT_VGA'] = ' '.join(VGA2)
    real, popen = rewrite_popen(vm, work if usbboot else '\0', work if usbboot else None)
    vm.subprocess.Popen = popen
    try:
        v = vm.VM(work, 'none', extra)
    finally:
        vm.subprocess.Popen = real
    v.qmp = Qmp(qmp_path)
    v.com2 = com2
    return v


def nt_logon(v, wait=150):
    if not v.wait_serial('##AGENT-START##', wait):
        for _ in range(3):
            if v.p.poll() is not None:
                return False
            v.key('ctrl-alt-delete', 4)
            v.key('ret', 4)
            if v.wait_serial('##AGENT-START##', 60):
                return True
        return False
    return True


def nt_boot(vm, work, bat, extra, usbboot=False, typing=None, timeout=600, name='b'):
    p = os.path.join(W, 'autorun_%s.bat' % name)
    open(p, 'w', newline='\r\n').write('\n'.join(['@echo off'] + bat) + '\n')
    vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
    v = nt_vm(vm, work, extra, usbboot)
    started = nt_logon(v, 240 if usbboot else 150)
    if started and typing:
        time.sleep(20)
        type_usb(v.qmp, ['shift'])                  # the first key finds the class driver (late load)
        time.sleep(3)
        type_usb(v.qmp, typing)
        time.sleep(2)
        type_usb(v.qmp, ['spc'] + typing)
    done = v.wait_serial('##AGENT-SHUTDOWN##', timeout, shots=120)
    v.shot('end_' + name)
    time.sleep(10)
    v.quit()
    log = open(v.com2, 'rb').read().decode('latin1') if os.path.exists(v.com2) else ''
    open(os.path.join(W, '%s_com2.log' % name), 'w').write(log)
    open(os.path.join(W, '%s_serial.log' % name), 'w').write(v.serial())
    return started and done, v.serial(), log


def run_nt(guest, modes, keep):
    from ntmsc import stick
    if guest == 'nt4':
        os.environ['AUDIONT_IMG'] = '/home/claude/nt4'
        base_name = 'usbbase.raw'                   # agent and auto logon; its usbinst settings are undone below
    else:
        os.environ['AUDIONT_IMG'] = '/home/claude/nt31'
        base_name = 'base.raw'                      # agent and auto logon, no USBNT
    import vm
    win = 'WINNT4' if guest == 'nt4' else vm.WIN
    work = os.path.join(vm.IMG_DIR, 'wttxt-%s.raw' % guest)
    vm.sh('cp', '--sparse=always', os.path.join(vm.IMG_DIR, base_name), work)
    vm.mcopy(work, os.path.join(OUTB, 'usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    # the current agent (start / done markers on COM1)
    vm.mcopy(work, os.environ.get('NTTEST_EXE', os.path.join(AUDIONT, 'build', 'nttest.exe')),
             '/AUDIONT/TEST/NTTEST.EXE')
    vm.mmd(work, '/USBNT')
    for f in ('fstest.exe', 'inptest.exe'):
        vm.mcopy(work, os.path.join(OUTB, f), '/USBNT/' + f.upper())
    sk = os.path.join(W, 'oemstick-%s.img' % guest)
    stick(sk, 32, 'OEMSTICK', [('hello.txt', b'hello from usb\r\n')])
    kbd = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,display=vga1,id=ukbd']
    stk = ['-drive', 'file=%s,format=raw,if=none,id=st' % sk, '-device', 'usb-storage,bus=xhci.0,drive=st']
    res = {}
    late_log = ''
    kc = 'SYSTEM\\CurrentControlSet\\Services\\Kbdclass\\Parameters'
    # what text-mode setup makes; Start 3 first for the "late" boot, ConnectMultiplePorts back to NT's 0
    prep = setup_registry('usbntnt', True) + [
        '%s reg %s ConnectMultiplePorts dword:0' % (F, kc),
        '%s reg %s Start dword:%d' % (F, SVC, 3 if 'late' in modes else 0),
        '%s regget %s Start' % (F, SVC), '%s regget %s\\Parameters OemSetup' % (F, SVC)]
    if guest == 'nt31':
        # the NT 3.1 image is on IDE (atdisk): the SCSI disk class driver is not boot start there.
        # Setup makes it boot start when the system disk is on a SCSI adapter (USBNT's disks).
        prep.append('%s reg SYSTEM\\CurrentControlSet\\Services\\Scsidisk Start dword:0' % F)
    ok, ser, log = nt_boot(vm, work, prep, [], name=guest + '_prep', timeout=300)
    print('prep:', ok, '\n'.join(l for l in ser.splitlines() if 'FSTEST' in l and 'ERROR' in l))
    res['prep'] = ok and 'FSTEST svc usbnt OK' in ser
    if 'late' in modes:
        bat = ['%s start usbnt' % F, 'C:\\USBNT\\INPTEST.EXE 60',
               '%s reg %s Start dword:0' % (F, SVC)]
        ok, ser, log = nt_boot(vm, work, bat, kbd, typing=list('lentok'), name=guest + '_late')
        chars = results(ser, 'lentok')
        print('late: start', 'FSTEST start usbnt OK' in ser, 'chars', chars)
        print('\n'.join(l for l in log.splitlines() if 'keyboard' in l or 'class driver' in l))
        # started by fstest, or already by the service controller (1056: the NT 4.0 image has AUDIONT,
        # which needs usbnt) - either way after kbdclass and i8042prt (KeyboardPort0)
        res['late: driver loaded after kbdclass'] = ('FSTEST start usbnt OK' in ser or
                                                     'FSTEST start usbnt ERROR 1056' in ser) and 'keyboard port 1' in log
        late_log = log
        res['late: keys to i8042prt\'s class'] = 'USB keys go to the class driver of' in log
        res['late: USB keyboard types'] = bool(chars) and 'lentok' in chars
    if 'boot' in modes:
        bat = ['C:\\USBNT\\INPTEST.EXE 50', '%s find HELLO.TXT 60000' % F, '%s cat ?:\\HELLO.TXT' % F,
               '%s write ?:\\FROMNT.TXT written-by-%s' % (F, guest), '%s cat ?:\\FROMNT.TXT' % F,
               '%s regget %s ConnectMultiplePorts' % (F, kc), '%s regget %s Start' % (F, SVC)]
        ok, ser, log = nt_boot(vm, work, bat, kbd + stk, typing=list('usbkey'), name=guest + '_boot')
        chars = results(ser, 'usbkey')
        print('boot:', '\n'.join(l for l in ser.splitlines() if 'FSTEST' in l or 'INPTEST' in l))
        print('\n'.join(l for l in log.splitlines() if 'ConnectMultiple' in l or 'class driver' in l or 'slot' in l))
        res['boot: agent ran'] = ok
        res['boot: stick read'] = '[hello from usb' in ser
        res['boot: stick written'] = 'written-by-%s' % guest in ser
        res['boot: ConnectMultiplePorts set by usbnt'] = 'ConnectMultiplePorts set to 1' in (log + late_log) and \
            'ConnectMultiplePorts = 1' in ser
        res['boot: kbdclass on our port'] = 'keyboard class driver connected' in log
        res['boot: USB keyboard types'] = bool(chars) and 'usbkey' in chars
    if 'usbboot' in modes:
        bat = ['%s write C:\\USBBOOT.TXT booted-from-usb' % F, '%s cat C:\\USBBOOT.TXT' % F,
               '%s regget %s Start' % (F, SVC)]
        ok, ser, log = nt_boot(vm, work, bat, ['-device', 'qemu-xhci,id=xhci'], usbboot=True, name=guest + '_usbboot',
                               timeout=900)
        print('usbboot:', '\n'.join(l for l in ser.splitlines() if 'FSTEST' in l))
        print('\n'.join(l for l in log.splitlines()[:40]))
        got = os.path.join(W, 'usbboot_%s.txt' % guest)
        if os.path.exists(got):
            os.remove(got)
        vm.mget(work, '/USBBOOT.TXT', got)
        res['usbboot: system came up from the USB disk'] = ok and '[booted-from-usb' in ser
        res['usbboot: file on the disk (host check)'] = os.path.exists(got) and b'booted-from-usb' in open(got, 'rb').read()
        res['usbboot: disk through usbnt'] = 'slot 0' in log
    if not keep:
        os.remove(work)
    return res


# ---------------------------------------------------------------------------
# Windows 2000 / XP x64 (vm5.py: QMP, agent on COM1)
# ---------------------------------------------------------------------------
def w5_boot(vm5, guest, work, bat, extra, usbboot=False, typing=None, timeout=900, name='b'):
    vm5.set_bat(work, bat)
    real, popen = rewrite_popen(vm5, work if usbboot else '\0', work if usbboot else None)
    vm5.subprocess.Popen = popen
    try:
        v = vm5.VM(guest, work, extra=extra, name='oem-' + name)
    finally:
        vm5.subprocess.Popen = real
    t0 = time.time()
    # SeaBIOS reads a USB disk slowly: XP x64 needs several minutes to the desktop from one
    started = v.wait_serial('##AGENT-START##', 1200 if usbboot else 400, shots=60)
    print('%s: logon after %d s' % (name, time.time() - t0))
    if started and typing:
        time.sleep(25)
        type_usb(v.qmp, ['shift'])
        time.sleep(3)
        type_usb(v.qmp, typing)
        time.sleep(2)
        type_usb(v.qmp, ['spc'] + typing)
    done = v.wait_serial('##AGENT-DONE##', timeout, shots=120)
    time.sleep(2)
    v.shot('end')
    ser, log = v.serial(), v.log()
    v.quit()
    open(os.path.join(W, '%s_com2.log' % name), 'w').write(log)
    open(os.path.join(W, '%s_serial.log' % name), 'w').write(ser)
    return started and done, ser, log


def run_w5(guest, modes, keep):
    import vm5
    sys.path.insert(0, HERE)
    from w5usb import no_signing_prompt
    from ntmsc import stick
    g = vm5.GUESTS[guest]
    arch = 'amd64' if g['x64'] else 'i386'
    choice = 'usbnt64' if g['x64'] else 'usbnt2k'
    work = os.path.join(vm5.W5_IMG, 'wttxt-%s.raw' % guest)
    files = [(os.path.join(OUTB, '2kxp', arch, 'usbnt.sys'), '/%s/system32/drivers/usbnt.sys' % g['win']),
             (os.path.join(OUTB, 'fstest.exe'), '/USBNT/FSTEST.EXE'),
             (os.path.join(OUTB, 'inptest.exe'), '/USBNT/INPTEST.EXE')]
    prep = setup_registry(choice, False) + ['%s regget %s Start' % (F, SVC), '%s regget %s Group' % (F, SVC)]
    work, g = vm5.prepare(guest, work=work, files=files, bat=prep)
    no_signing_prompt(work, g)
    res = {}
    ok, ser, log = w5_boot(vm5, guest, work, prep, [], name=guest + '_prep', timeout=400)
    res['prep'] = ok and 'FSTEST svc usbnt OK' in ser and 'ERROR' not in ser
    print('prep:', ok, '\n'.join(l for l in ser.splitlines() if 'ERROR' in l or 'regget' in l))
    sk = os.path.join(W, 'oemstick-%s.img' % guest)
    stick(sk, 64, 'OEMSTICK', [('hello.txt', b'hello from usb\r\n')])
    kbd = ['-device', 'qemu-xhci,id=xhci', '-device', 'usb-kbd,bus=xhci.0,display=vga1,id=ukbd',
           '-device', 'usb-mouse,bus=xhci.0,id=umou']
    stk = ['-drive', 'file=%s,format=raw,if=none,id=st' % sk, '-device', 'usb-storage,bus=xhci.0,drive=st']
    if 'boot' in modes:
        bat = ['start /b %s clicker C:\\USBNT\\STOP 900000' % F, 'C:\\USBNT\\INPTEST.EXE 60',
               '%s find HELLO.TXT 120000' % F, '%s cat ?:\\HELLO.TXT' % F,
               '%s write ?:\\FROMXP.TXT written-by-%s' % (F, guest), '%s cat ?:\\FROMXP.TXT' % F,
               '%s write C:\\USBNT\\STOP x' % F, '%s sleep 2000' % F, '%s devlist USBNT' % F]
        ok, ser, log = w5_boot(vm5, guest, work, bat, kbd + stk, typing=list('usbkey'), name=guest + '_boot')
        chars = results(ser, 'usbkey')
        print('boot:', '\n'.join(l for l in ser.splitlines() if 'FSTEST' in l or 'INPTEST' in l))
        print('\n'.join(l for l in log.splitlines() if 'PnP' in l or 'keyboard' in l or 'settled' in l))
        res['boot: agent ran'] = ok
        res['boot: device bus on the host controller'] = 'USBNT device bus on the host controller' in log
        res['boot: stick read'] = '[hello from usb' in ser
        res['boot: stick written'] = 'written-by-%s' % guest in ser
        res['boot: USB keyboard types'] = bool(chars) and 'usbkey' in chars
        # text-mode setup gives every [HardwareIds.scsi] id the SCSI adapter class: usbnt.sys puts
        # USBNT\\Keyboard / Mouse into their classes, so kbdclass / mouclass are on them (not the
        # PS/2 keyboard's class driver borrowed - a machine without one would have no keyboard)
        res['boot: kbdclass on USBNT\\Keyboard, not borrowed'] = 'keyboard class driver connected' in log and \
            'USB keys go to the class driver of' not in log
        res['boot: mouclass on USBNT\\Mouse'] = 'pointer class driver connected' in log
        # Device Manager without an INF (text-mode setup): the root hub and the devices below it, started
        dl = [l for l in ser.splitlines() if l.startswith('FSTEST dev ')]
        print('\n'.join(dl))
        res['boot: Device Manager: root hub and devices, no problem'] = \
            any('Root Hub (USBNT)' in l and 'prob=0' in l for l in dl) and \
            any('USB Keyboard (' in l and 'prob=0' in l for l in dl) and \
            any('USB Mass Storage Device' in l and 'prob=0' in l for l in dl) and \
            all('prob=0' in l for l in dl)
        res['boot: the controller with the USB controllers, named'] = \
            any('USB 3.0 Host Controller (USBNT)' in l and (l.startswith('FSTEST dev USB |') or
                '{36FC9E60-C465-11CF-8056-444553540000}' in l.upper()) for l in dl)
    if 'usbboot' in modes:
        bat = ['%s write C:\\USBBOOT.TXT booted-from-usb' % F, '%s cat C:\\USBBOOT.TXT' % F]
        ok, ser, log = w5_boot(vm5, guest, work, bat, ['-device', 'qemu-xhci,id=xhci'], usbboot=True,
                               name=guest + '_usbboot', timeout=1200)
        print('usbboot:', '\n'.join(l for l in ser.splitlines() if 'FSTEST' in l))
        print('\n'.join(l for l in log.splitlines() if 'PnP' in l or 'settled' in l or 'msc' in l))
        got = os.path.join(W, 'usbboot_%s.txt' % guest)
        if os.path.exists(got):
            os.remove(got)
        vm5.mget(work, '/USBBOOT.TXT', got)
        res['usbboot: system came up from the USB disk'] = ok and '[booted-from-usb' in ser
        res['usbboot: file on the disk (host check)'] = os.path.exists(got) and b'booted-from-usb' in open(got, 'rb').read()
        res['usbboot: disk through usbnt'] = 'disk slot 0 to Windows' in log
    if not keep:
        os.remove(work)
    return res


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    guest = args[0] if args else 'nt4'
    modes = set(args[1:]) or ({'late', 'boot', 'usbboot'} if guest in ('nt31', 'nt4') else {'boot', 'usbboot'})
    keep = '--keep' in sys.argv
    if not os.path.exists(os.path.join(OUTB, 'oem', '2KXP', 'TXTSETUP.OEM')):
        sys.exit('build first (build/wdmbuild.sh, build/mkoem.py)')
    res = run_nt(guest, modes, keep) if guest in ('nt31', 'nt4') else run_w5(guest, modes, keep)
    for k, v in res.items():
        print('%-48s %s' % (k, 'PASS' if v else 'FAIL'))
    print('RESULT toemboot %s %s %s' % (guest, ' '.join(sorted(modes)), 'PASS' if all(res.values()) else 'FAIL'))
    return 0 if all(res.values()) else 1


if __name__ == '__main__':
    sys.exit(main())
