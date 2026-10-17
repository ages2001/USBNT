#!/usr/bin/env python3
"""NVMe drives in an NT guest: one in the machine from the start, driven by
nvme2k (SCSI miniport), and one in a Thunderbolt / USB4 dock that is plugged
in after logon, driven by USBNT (PCIe tunnel, PCIe hot plug, disk slot).

  ntnvme.py [--nt4] [mode=usb4|tr|ar] [security=1|2] [nonvme2k] [bootbus=rp2|pcie.0]
  ntnvme.py ... nousbnt | pc     diagnosis of nvme2k alone / on i440fx

With mode=tr security=2 the device key must land in the registry, and a
second boot must read it and pass the challenge.

Machine: q35 (PCIe hot plug), the boot disk on a PIIX IDE controller (NT has
no AHCI driver), the usb4-nhi test model (test/qemu/usbnt-qemu-test.patch)
and a hot plug pcie-root-port for the dock's PCIe link.  The guest reads a
file from each NVMe drive, writes one to the dock's drive, lists everything
with usbtree; the host checks the written file and that the dock's drive was
shut down before it is pulled.  NVME2K_SYS: nvme2k.sys to install.
"""
import os, re, socket, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ntmsc import W, AUDIONT, stick
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
from ntsetup import hivexsh, set_values
NVME2K = os.environ.get('NVME2K_SYS', '/tmp/claude-0/nvme2k-src/nvme2k/bin/NT/nvme2k.sys')


class Q35:
    """vm.VM on q35 with the boot disk on piix3-ide: rewrites its QEMU command line"""
    def __init__(self, vm, img, extra):
        real = subprocess.Popen

        def popen(args, **kw):
            a = list(args)
            i = a.index('-M')
            a[i + 1] = 'q35'
            for k, v in enumerate(a):
                if v.startswith('file=%s,' % img) and v.endswith('if=ide'):
                    a[k] = 'file=%s,format=raw,if=none,id=boot' % img
                    a[k + 1:k + 1] = ['-device', 'piix3-ide,id=pide', '-device', 'ide-hd,drive=boot,bus=pide.0']
                    break
            a += ['-global', 'ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off']
            return real(a, **kw)

        vm.subprocess.Popen = popen
        try:
            self.v = vm.VM(img, 'none', extra)
        finally:
            vm.subprocess.Popen = real

    def hmp(self, line):
        """monitor command with its answer"""
        s = self.v.sock
        self.v._drain()
        s.sendall((line + '\n').encode())
        out = b''
        s.settimeout(5)
        try:
            while not out.rstrip().endswith(b'(qemu)'):
                out += s.recv(65536)
        except socket.timeout:
            pass
        s.settimeout(None)
        txt = out.decode('latin1').replace('\r', '')
        lines = [l for l in txt.split('\n') if l.strip() and not l.startswith('(qemu)') and line not in l]
        return '\n'.join(lines)

    def qget(self, prop):
        return self.hmp('qom-get /machine/peripheral/tb %s' % prop).strip()

    def qset(self, prop, val):
        return self.hmp('qom-set /machine/peripheral/tb %s %s' % (prop, val))


def install_nvme2k(vm, work, win):
    vm.mcopy(work, NVME2K, '/%s/SYSTEM32/DRIVERS/NVME2K.SYS' % win)
    hive = os.path.join(W, 'nvme_system_%s.hive' % os.path.basename(work))
    if not vm.mget(work, '/%s/SYSTEM32/CONFIG/SYSTEM' % win, hive):
        raise RuntimeError('no SYSTEM hive')
    svc = {"Type": "dword:0x1", "Start": "dword:0x0", "ErrorControl": "dword:0x1",
           "Group": "string:SCSI miniport", "ImagePath": "expandstring:system32\\drivers\\nvme2k.sys"}
    done = 0
    for cs in ("ControlSet001", "ControlSet002"):
        if hivexsh(hive, "cd \\%s\\Services\n" % cs).returncode != 0:
            continue
        exists = hivexsh(hive, "cd \\%s\\Services\\nvme2k\n" % cs).returncode == 0
        set_values(hive, "\\%s\\Services\\nvme2k" % cs, svc, create=not exists)
        done += 1
    if done:
        vm.mcopy(work, hive, '/%s/SYSTEM32/CONFIG/SYSTEM' % win)
        return
    # NT 3.1's hive format is older than hivex knows: the service is made in the guest (one boot)
    open('/tmp/nvme-svc.bat', 'w').write('@echo off\r\nC:\\USBNT\\FSTEST.EXE svc nvme2k SCSI miniport\r\n')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/fstest.exe'), '/USBNT/FSTEST.EXE')
    vm.mcopy(work, '/tmp/nvme-svc.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    v = vm.VM(work, 'none')
    if not v.wait_serial('##AGENT-START##', 150):
        for _ in range(3):
            v.key('ctrl-alt-delete', 4); v.key('ret', 4)
            if v.wait_serial('##AGENT-START##', 40):
                break
    ok = v.wait_serial('##AGENT-SHUTDOWN##', 120)
    time.sleep(8)
    v.quit()
    print('host: nvme2k service made in the guest:', 'FSTEST svc nvme2k OK' in v.serial() and ok)


def main():
    nt4 = '--nt4' in sys.argv
    opts = dict(a.split('=', 1) for a in sys.argv[1:] if '=' in a)
    mode = opts.get('mode', 'usb4')
    sec = opts.get('security', '1')
    use2k = 'nonvme2k' not in sys.argv
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    os.makedirs(W, exist_ok=True)
    tag = ('4' if nt4 else '31') + mode + sec
    bootnv, docknv = os.path.join(W, 'nt_bootnv%s.img' % tag), os.path.join(W, 'nt_docknv%s.img' % tag)
    stick(bootnv, 32, 'BOOTNVME', [('boot.txt', b'NVME2K-OK\r\n')])
    stick(docknv, 64, 'DOCKNVME', [('nv.txt', b'DOCK-NVME-OK\r\n'), ('more.txt', b'y' * 70000)])
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'usbwork-nvme%s.raw' % tag)
    vm.sh('cp', '--sparse=always', img, work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbtree.exe'), '/USBNT/USBTREE.EXE')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbeject.exe'), '/USBNT/USBEJECT.EXE')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/fstest.exe'), '/USBNT/FSTEST.EXE')
    if use2k:
        install_nvme2k(vm, work, win)
    if 'nousbnt' in sys.argv:                       # diagnosis: nvme2k alone
        subprocess.call(['mdel', '-i', work + vm.OFF, '::/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win])
    F = 'C:\\USBNT\\FSTEST.EXE'
    L = 'D E F G H I J K L M N O P'
    bat = ['@echo off',
           '%s svcstat nvme2k' % F,
           '%s svcstat usbnt' % F,
           '%s find BOOT.TXT 30000' % F,
           'for %%%%d in (%s) do if exist %%%%d:\\BOOT.TXT %s cat %%%%d:\\BOOT.TXT' % (L, F),
           'C:\\USBNT\\USBTREE.EXE /dump C:\\USBNT\\TREE0.TXT',
           'echo ##READY-FOR-DOCK##',
           '%s find NV.TXT 240000' % F,
           'for %%%%d in (%s) do if exist %%%%d:\\NV.TXT %s cat %%%%d:\\NV.TXT' % (L, F),
           'for %%%%d in (%s) do if exist %%%%d:\\NV.TXT %s sum %%%%d:\\MORE.TXT' % (L, F),
           'for %%%%d in (%s) do if exist %%%%d:\\NV.TXT %s write %%%%d:\\NEW.TXT written-on-dock-nvme' % (L, F),
           'for %%%%d in (%s) do if exist %%%%d:\\NV.TXT %s fill %%%%d:\\BIG.BIN 20' % (L, F),
           'C:\\USBNT\\USBTREE.EXE /dump C:\\USBNT\\TREE.TXT',
           'type C:\\USBNT\\TREE.TXT',
           'C:\\USBNT\\USBEJECT.EXE /list',
           # safe removal before the cable goes, as a user does
           'for %%%%d in (%s) do if exist %%%%d:\\NV.TXT C:\\USBNT\\USBEJECT.EXE %%%%d:' % L,
           'echo ##EJECT##',
           '%s sleep 20000' % F, '']
    open('/tmp/nvme-run.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/nvme-run.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    com2 = os.path.join(vm.OUT, 'com2.log')
    if os.path.exists(com2):
        os.remove(com2)
    extra = ['-device', 'qemu-xhci,id=hc',
             '-device', 'pcie-root-port,id=rp1,chassis=1,slot=1,bus-reserve=4,mem-reserve=16M',
             '-device', 'pcie-root-port,id=rp2,chassis=2,slot=2',
             '-device', 'usb4-nhi,id=tb,mode=%s,security=%s' % (mode, sec),
             '-drive', 'if=none,id=nboot,format=raw,file=' + bootnv,
             '-device', 'nvme,id=nvboot,serial=BOOTNV01,drive=nboot,bus=%s' % opts.get('bootbus', 'rp2'),
             '-drive', 'if=none,id=ndock,format=raw,file=' + docknv,
             '-serial', 'file:' + com2]
    if 'pc' in sys.argv:                            # diagnosis: i440fx, the boot NVMe only
        extra = ['-drive', 'if=none,id=nboot,format=raw,file=' + bootnv,
                 '-device', 'nvme,id=nvboot,serial=BOOTNV01,drive=nboot', '-serial', 'file:' + com2]
        v = vm.VM(work, 'none', extra, noreboot=True)
        if not v.wait_serial('##AGENT-START##', 150):
            v.key('ctrl-alt-delete', 4); v.key('ret', 4)
        v.wait_serial('##AGENT-SHUTDOWN##', 500)
        v.quit()
        print('\n'.join(l for l in v.serial().splitlines() if 'FSTEST' in l))
        return
    q = Q35(vm, work, extra)
    v = q.v
    t0 = time.time()
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        v.shot('logon')
        for _ in range(3):
            v.key('ctrl-alt-delete', 4); v.key('ret', 4)
            if v.wait_serial('##AGENT-START##', 40):
                break
    # the agent's output comes at the end; give the batch time to read the boot NVMe
    time.sleep(25)
    print('host: plugging the dock at %.0fs' % (time.time() - t0))
    q.qset('plug0', 'true')
    tunnel = False
    for _ in range(60):
        if q.qget('pcie0') == 'true':
            tunnel = True
            break
        time.sleep(1)
    print('host: PCIe tunnel %s at %.0fs' % ('up' if tunnel else 'NOT up', time.time() - t0))
    if tunnel:
        print(q.hmp('device_add nvme,bus=rp1,id=nvdock,serial=DOCKNV01,drive=ndock'))
    # usbeject in the guest: the driver log (COM2) says when the drive is shut down
    end = time.time() + 400
    while time.time() < end and v.p.poll() is None:
        if os.path.exists(com2) and b'shut down for removal' in open(com2, 'rb').read():
            break
        time.sleep(2)
    print('host: drive ejected at %.0fs' % (time.time() - t0))
    # the cable goes: attention button first (orderly), then the dock
    print(q.hmp('device_del nvdock'))
    time.sleep(12)
    q.qset('plug0', 'false')
    time.sleep(6)
    viol = q.qget('violations')
    vlog = q.qget('vlog')
    done = v.wait_serial('##AGENT-SHUTDOWN##', 120)
    v.shot('nvme')
    v.quit()
    ser = v.serial()
    print(ser[-3500:])
    tree = os.path.join(vm.OUT, 'TREE.TXT')
    if os.path.exists(tree):
        os.remove(tree)
    t = open(tree, 'rb').read().decode('latin1') if vm.mget(work, '/USBNT/TREE.TXT', tree) else ''
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    print('\n'.join(l for l in ulog.splitlines() if 'thunderbolt' in l or 'nvme' in l or 'hotplug' in l
                    or 'error' in l.lower())[-5000:])
    wrote = subprocess.run(['mtype', '-i', docknv + '@@32256', '::/NEW.TXT'], capture_output=True).stdout
    big = subprocess.run(['mdir', '-i', docknv + '@@32256', '::/BIG.BIN'], capture_output=True).stdout.decode()
    h = 2166136261
    for c in b'y' * 70000:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    checks = [('guest finished', bool(done)),
              ('nvme2k: boot NVMe read' if use2k else 'no nvme2k', ('cat' in ser and 'NVME2K-OK' in ser) or not use2k),
              ('PCIe tunnel set up by the driver', tunnel),
              ('dock NVMe found by the guest', 'FSTEST find NV.TXT' in ser and 'OK after' in ser.split('find NV.TXT')[-1][:60]),
              ('dock NVMe file read', 'DOCK-NVME-OK' in ser),
              ('70 KB file checksum', ('70000 %08X' % h) in ser),
              ('file written to the dock NVMe', b'written-on-dock-nvme' in wrote),
              ('20 MB written', 'BIG' in big),
              ('usbtree lists the drive and the dock', 'NVMe drives' in t and 'Thunderbolt / USB4' in t),
              ('drive shut down before the cable (usbeject)', 'shut down for removal' in ulog),
              ('dock removed', 'removed' in ulog or 'disconnected' in ulog),
              ('model: no protocol errors', viol == '0')]
    if mode != 'usb4' and sec == '2':
        hive = os.path.join(W, 'nvme_system_after.hive')
        keys = ''
        if vm.mget(work, '/%s/SYSTEM32/CONFIG/SYSTEM' % win, hive):
            for cs in ('ControlSet001', 'ControlSet002'):
                keys += hivexsh(hive, 'cd \\%s\\Services\\usbnt\nlsval\n' % cs).stdout.decode('latin1')
        print('\n'.join(l for l in keys.splitlines() if l.startswith('"Tbt')))
        checks.append(('device key saved in the registry', '"Tbt' in keys or not nt4))
        m = re.search(r'"Tbt[0-9a-f]+"=hex\(3\):([0-9a-f,]+)', keys)
        if m:
            # next boot: the device still has the key, the driver reads it from the registry and challenges
            key = m.group(1).replace(',', '')
            open('/tmp/nvme-rekey.bat', 'w').write('@echo off\r\nC:\\USBNT\\FSTEST.EXE sleep 70000\r\n')
            vm.mcopy(work, '/tmp/nvme-rekey.bat', '/AUDIONT/TEST/AUTORUN.BAT')
            os.remove(com2)
            q = Q35(vm, work, extra)
            v = q.v
            if not v.wait_serial('##AGENT-START##', 150):
                v.key('ctrl-alt-delete', 4); v.key('ret', 4)
                v.wait_serial('##AGENT-START##', 60)
            q.qset('devkey0', 'hex:' + key)
            q.qset('plug0', 'true')
            ok2 = False
            for _ in range(40):
                if b'key checked' in open(com2, 'rb').read() and q.qget('approved0') == 'true':
                    ok2 = True
                    break
                time.sleep(1)
            v.wait_serial('##AGENT-SHUTDOWN##', 120)
            v.quit()
            print('\n'.join(l for l in open(com2, 'rb').read().decode('latin1').splitlines() if 'thunderbolt' in l))
            checks.append(('next boot: stored key read, challenge passed', ok2))
    if viol != '0':
        print('model violations:', viol, vlog)
    print('\n'.join(l for l in t.splitlines() if 'NVMe' in l or 'Thunderbolt' in l or 'USB4' in l or 'router' in l
                    or 'Device' in l))
    ok = True
    for name, c in checks:
        ok &= bool(c)
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
    for bad in ('BUGCHECK', 'KMODE', 'STOP:'):
        if bad in ser or bad in ulog:
            ok = False; print('FAIL', bad)
    print('RESULT ntnvme %s%s %s' % (' '.join(sys.argv[1:]), ' nt4' if nt4 else '', 'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
