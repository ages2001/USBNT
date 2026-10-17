"""Windows 2000 / XP (x86 and x64) guests in QEMU for the USBNT and AUDIONT
2000/XP tests.

Images (FAT partition at sector 63) come from the VHDs the author made:
  W5_IMG/w2k.raw    Windows 2000 Professional SP0, boot disk on AHCI (AHCINT)
  W5_IMG/xp64.raw   Windows XP Professional x64 SP1, boot disk on IDE

prepare() makes a work copy, logs on automatically and starts the test agent
(AUDIONT's nttest.exe: runs C:\\AUDIONT\\TEST\\AUTORUN.BAT, its output goes to
COM1, then shuts down).  VM boots it with the controllers the test wants.
"""
import os, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from ntsetup import hivexsh, set_values

W5_IMG = os.environ.get('W5_IMG', '/home/claude/vhd')
OUT = os.environ.get('W5_OUT', '/tmp/usbt/w5out')
AGENT = os.environ.get('NTTEST_EXE', '/home/claude/audiont/build/nttest.exe')
OFF = '@@32256'
GUESTS = {
    'w2k': dict(raw='w2k.raw', win='WINNT', x64=False, disk='ahci', user=None, machine='pc'),
    'xp64': dict(raw='xp64.raw', win='WINDOWS', x64=True, disk='ide', user='Administrator', machine='pc'),
}
os.environ['MTOOLS_SKIP_CHECK'] = '1'


def sh(*a, **kw):
    return subprocess.run(a, check=kw.pop('check', True), **kw)


def mcopy(img, src, dst):
    sh('mcopy', '-o', '-i', img + OFF, src, '::' + dst)


def mget(img, src, dst):
    return subprocess.run(['mcopy', '-o', '-i', img + OFF, '::' + src, dst], stderr=subprocess.DEVNULL).returncode == 0


def mmd(img, d):
    parts = [x for x in d.split('/') if x]
    for i in range(1, len(parts) + 1):
        subprocess.run(['mmd', '-i', img + OFF, '::/' + '/'.join(parts[:i])], stderr=subprocess.DEVNULL)


def mdel(img, path):
    subprocess.run(['mdel', '-i', img + OFF, '::' + path], stderr=subprocess.DEVNULL)


class Hive:
    """offline edit of a registry hive of the image"""
    def __init__(self, img, win, name):
        self.img, self.path = img, '/%s/system32/config/%s' % (win, name)
        self.local = os.path.join(OUT, 'hive_%s_%d' % (name, os.getpid()))
        os.makedirs(OUT, exist_ok=True)
        if not mget(img, self.path, self.local):
            raise RuntimeError('no hive ' + self.path)

    def set(self, key, values, create=False):
        set_values(self.local, key, values, create=create)

    def exists(self, key):
        return hivexsh(self.local, 'cd %s\n' % key).returncode == 0

    def ensure(self, key):
        parts = key.strip('\\').split('\\')
        for i in range(1, len(parts) + 1):
            k = '\\' + '\\'.join(parts[:i])
            if not self.exists(k):
                parent = '\\' + '\\'.join(parts[:i - 1]) if i > 1 else '\\'
                r = hivexsh(self.local, 'cd %s\nadd %s\ncommit\n' % (parent, parts[i - 1]), write=True)
                if r.returncode != 0:
                    raise RuntimeError(r.stderr.decode())

    def values(self, key):
        return hivexsh(self.local, 'cd %s\nlsval\n' % key).stdout.decode('latin1')

    def save(self):
        mcopy(self.img, self.local, self.path)


def prepare(guest, work=None, files=(), bat=None):
    """work copy of the guest image with the agent; files: (src, guest path); returns (work, info)"""
    g = GUESTS[guest]
    base = os.path.join(W5_IMG, g['raw'])
    work = work or os.path.join(W5_IMG, 'work-%s.raw' % guest)
    sh('cp', '--sparse=always', base, work)
    mmd(work, '/AUDIONT/TEST')
    mcopy(work, AGENT, '/AUDIONT/TEST/NTTEST.EXE')
    for src, dst in files:
        mmd(work, os.path.dirname(dst))
        mcopy(work, src, dst)
    if bat is not None:
        set_bat(work, bat)
    sw = Hive(work, g['win'], 'software')
    wl = '\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon'
    v = {'Userinit': 'string:C:\\%s\\system32\\userinit.exe,C:\\AUDIONT\\TEST\\NTTEST.EXE' % g['win'],
         'AutoAdminLogon': 'string:1'}
    if g['user']:
        v['DefaultUserName'] = 'string:' + g['user']
        v['DefaultPassword'] = 'string:'
    sw.set(wl, v)
    sw.save()
    return work, g


def set_bat(work, lines):
    p = os.path.join(OUT, 'autorun_%d.bat' % os.getpid())
    os.makedirs(OUT, exist_ok=True)
    open(p, 'w').write('\r\n'.join(lines + ['']))
    mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')


class VM:
    def __init__(self, guest, work, extra=(), machine=None, mem=512, name='vm', qemu=None, front=()):
        g = GUESTS[guest]
        machine = machine or g['machine']
        os.makedirs(OUT, exist_ok=True)
        self.dir = os.path.join(OUT, name)
        os.makedirs(self.dir, exist_ok=True)
        self.serial_path = os.path.join(self.dir, 'serial.log')
        self.com2 = os.path.join(self.dir, 'com2.log')
        self.qmp_path = os.path.join(self.dir, 'qmp.sock')
        for f in (self.serial_path, self.com2, self.qmp_path):
            if os.path.exists(f):
                os.remove(f)
        default = os.environ.get('QEMU', 'qemu-system-i386')
        if g['x64']:
            default = os.environ.get('QEMU64', default.replace('i386', 'x86_64'))
        qemu = qemu or default
        bios = ['-L', os.environ['QEMU_BIOS']] if os.environ.get('QEMU_BIOS') else []
        if g['disk'] == 'ahci':
            disk = ['-drive', 'file=%s,format=raw,if=none,id=boot' % work]
            if machine == 'q35':
                disk += ['-device', 'ide-hd,drive=boot,bus=ide.0']        # the q35 chipset's AHCI
            else:
                disk += ['-device', 'ich9-ahci,id=sata', '-device', 'ide-hd,drive=boot,bus=sata.0']
        else:
            if machine == 'q35':
                disk = ['-drive', 'file=%s,format=raw,if=none,id=boot' % work,
                        '-device', 'piix3-ide,id=pide', '-device', 'ide-hd,drive=boot,bus=pide.0']
            else:
                disk = ['-drive', 'file=%s,format=raw,if=ide' % work]
        cpu = os.environ.get('W5_CPU', 'qemu64' if g['x64'] else 'pentium3')
        args = [qemu] + bios + ['-machine', machine, '-cpu', cpu, '-m', str(mem), '-display', 'none',
                                '-vga', 'std', '-rtc', 'base=localtime', '-no-reboot',
                                '-qmp', 'unix:%s,server=on,wait=off' % self.qmp_path,
                                '-serial', 'file:' + self.serial_path, '-serial', 'file:' + self.com2,
                                '-audiodev', 'none,id=snd0'] + list(front) + disk + list(extra)
        if machine == 'q35':
            args += ['-global', 'ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off']
        self.qlog = open(os.path.join(self.dir, 'qemu.log'), 'w')
        self.p = subprocess.Popen(args, stdout=self.qlog, stderr=subprocess.STDOUT)
        for _ in range(200):
            try:
                self.q = socket.socket(socket.AF_UNIX)
                self.q.connect(self.qmp_path)
                break
            except OSError:
                if self.p.poll() is not None:
                    raise RuntimeError(open(os.path.join(self.dir, 'qemu.log')).read())
                time.sleep(0.05)
        self.qbuf = b''
        self._read()
        self.qmp('qmp_capabilities')
        self.shots = 0

    def _read(self):
        import json
        while True:
            while b'\n' in self.qbuf:
                line, self.qbuf = self.qbuf.split(b'\n', 1)
                m = json.loads(line)
                if 'event' in m:
                    continue
                return m
            d = self.q.recv(65536)
            if not d:
                raise RuntimeError('qmp closed')
            self.qbuf += d

    def qmp(self, name, **args):
        import json
        self.q.sendall((json.dumps({'execute': name, 'arguments': args}) + '\n').encode())
        r = self._read()
        if 'error' in r:
            raise RuntimeError('%s: %s' % (name, r['error']))
        return r.get('return')

    def hmp(self, line):
        return self.qmp('human-monitor-command', **{'command-line': line})

    def key(self, k, wait=0.2):
        self.hmp('sendkey ' + k)
        time.sleep(wait)

    def shot(self, name=None):
        self.shots += 1
        name = name or 'shot%02d' % self.shots
        ppm = os.path.join(self.dir, name + '.ppm')
        try:
            self.qmp('screendump', filename=ppm)
            time.sleep(0.4)
            from PIL import Image
            Image.open(ppm).save(os.path.join(self.dir, name + '.png'))
            os.remove(ppm)
        except Exception as e:
            print('screenshot failed', e)
        return os.path.join(self.dir, name + '.png')

    def serial(self):
        try:
            return open(self.serial_path, 'rb').read().decode('latin1')
        except OSError:
            return ''

    def log(self):
        try:
            return open(self.com2, 'rb').read().decode('latin1')
        except OSError:
            return ''

    def wait_serial(self, marker, timeout, shots=0):
        t0 = last = time.time()
        while time.time() - t0 < timeout:
            if marker in self.serial():
                return True
            if self.p.poll() is not None:
                return False
            if shots and time.time() - last >= shots:
                last = time.time()
                self.shot('t%03d' % int(last - t0))
            time.sleep(1)
        return False

    def wait_log(self, text, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if text in self.log():
                return True
            if self.p.poll() is not None:
                return False
            time.sleep(1)
        return False

    def running(self):
        return self.p.poll() is None

    def quit(self):
        try:
            self.qmp('quit')
        except Exception:
            pass
        try:
            self.p.wait(10)
        except subprocess.TimeoutExpired:
            self.p.kill()
