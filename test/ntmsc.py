#!/usr/bin/env python3
"""USB disk re-plug test in an NT guest (NT 3.1 / NT 4.0 images with USBNT).

  ntmsc.py HC [--nt4]          HC: uhci ohci ehci xhci

Stick A (A.TXT "ALPHA") is plugged at boot.  The host then pulls it, plugs it
back into the same port, pulls it again and plugs stick B (B.TXT "BRAVO") into
that port.  The guest must see A's file after the re-plug, B's file (and not
A's) after the swap, and write to B.  Uses the AUDIONT test agent and image
tools (AUDIONT=/path/to/audiont).
"""
import os, re, subprocess, sys, time
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
W = os.environ.get('MSC_W', '/tmp/usbt')

HC = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}


def mbr(path, mb, ptype, start=63):
    import struct
    total = (mb << 11) - start
    def chs(lba):
        c, r = divmod(lba, 64 * 32); h, s_ = divmod(r, 32)
        c = min(c, 1023)
        return bytes([h, ((c >> 2) & 0xC0) | (s_ + 1), c & 0xFF])
    e = bytes([0x80]) + chs(start) + bytes([ptype]) + chs(start + total - 1) + struct.pack('<II', start, total)
    with open(path, 'r+b') as f:
        f.seek(446); f.write(e + bytes(48)); f.write(b'\x55\xaa')


def stick(path, mb, label, files, fs='fat16'):
    with open(path, 'wb') as f:
        f.truncate(mb << 20)
    mbr(path, mb, 0x06 if fs == 'fat16' else 0x0B)
    part = '%s@@32256' % path
    args = ['mformat', '-i', part, '-v', label, '-T', str((mb << 11) - 63), '-h', '64', '-s', '32', '-H', '63']
    if fs == 'fat32':
        args.append('-F')
    subprocess.check_call(args + ['::'])
    for name, text in files:
        tmp = os.path.join(W, name)
        open(tmp, 'wb').write(text)
        subprocess.check_call(['mcopy', '-i', part, tmp, '::/' + name.upper()])
        os.unlink(tmp)


def main():
    hc = sys.argv[1] if len(sys.argv) > 1 else 'xhci'
    nt4 = '--nt4' in sys.argv
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    os.makedirs(W, exist_ok=True)
    a, b = os.path.join(W, 'swapA.img'), os.path.join(W, 'swapB.img')
    stick(a, 32, 'STICKA', [('a.txt', b'ALPHA\r\n'), ('common.txt', b'from A\r\n')])
    stick(b, 64, 'STICKB', [('b.txt', b'BRAVO\r\n'), ('common.txt', b'from B\r\n'), ('more.txt', b'x' * 5000)],
          fs=os.environ.get('MSC_FSB', 'fat16'))
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'usbwork.raw')
    vm.sh('cp', '--sparse=always', img, work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/fstest.exe'), '/USBNT/FSTEST.EXE')
    d = os.environ.get('MSC_DRIVE', 'D')
    F = 'C:\\USBNT\\FSTEST.EXE'
    bat = ['@echo off',
           '%s wait %s:\\A.TXT 240000' % (F, d), '%s cat %s:\\A.TXT' % (F, d), '%s dir %s:' % (F, d),
           'start %s hold %s:\\COMMON.TXT %s' % (F, d, os.environ.get('MSC_HOLD', '100000')),
           '%s sleep 45000' % F,
           '%s wait %s:\\A.TXT 60000' % (F, d), '%s cat %s:\\A.TXT' % (F, d), '%s cat %s:\\COMMON.TXT' % (F, d),
           '%s dir %s:' % (F, d),
           '%s sleep 45000' % F,
           '%s dir %s:' % (F, d),
           '%s wait %s:\\B.TXT 30000' % (F, d), '%s cat %s:\\B.TXT' % (F, d), '%s cat %s:\\COMMON.TXT' % (F, d),
           '%s cat %s:\\A.TXT' % (F, d), '%s dir %s:' % (F, d),
           '%s write %s:\\NEW.TXT written-on-B' % (F, d), '%s cat %s:\\NEW.TXT' % (F, d),
           '%s fill %s:\\BIG.BIN 40' % (F, d),
           'C:\\USBNT\\USBEJECT.EXE /list', '%s sleep 30000' % F, '']
    open('/tmp/mscswap.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/mscswap.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    com2 = os.path.join(vm.OUT, 'com2.log')
    if os.path.exists(com2):
        os.remove(com2)
    port = '1' if hc != 'ohci' else '1'
    extra = ['-device', '%s,id=hc' % HC[hc],
             '-drive', 'if=none,id=dA,format=raw,file=%s' % a,
             '-device', 'usb-storage,bus=hc.0,port=%s,drive=dA,id=sA' % port,
             '-serial', 'file:' + com2]
    v = vm.VM(work, 'none', extra)
    t0 = time.time()
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '120'))):
        print('host: logging on by hand')
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 200)
    time.sleep(20)
    v.cmd('device_del sA'); print('host: A removed at %.0fs' % (time.time() - t0))
    time.sleep(2)
    v.cmd('drive_add 0 if=none,id=dA2,format=raw,file=%s' % a)
    v.cmd('device_add usb-storage,bus=hc.0,port=%s,drive=dA2,id=sA2' % port); print('host: A re-plugged at %.0fs' % (time.time() - t0))
    time.sleep(43)
    v.cmd('device_del sA2'); print('host: A removed again at %.0fs' % (time.time() - t0))
    time.sleep(2)
    v.cmd('drive_add 0 if=none,id=dB,format=raw,file=%s' % b)
    v.cmd('device_add usb-storage,bus=hc.0,port=%s,drive=dB,id=sB' % port); print('host: B plugged at %.0fs' % (time.time() - t0))
    done = v.wait_serial('##AGENT-SHUTDOWN##', 700)
    v.shot('final')
    time.sleep(5)
    v.quit()
    ser = v.serial()
    out = '\n'.join(l for l in ser.splitlines() if 'FSTEST' in l or 'slot' in l.lower())
    print(out)
    checks = [
        ('first sight of A', 'cat %s:\\A.TXT [ALPHA]' % d),
        ('A after re-plug', r'wait %s:\A.TXT OK' % d),
        ('B after swap', 'cat %s:\\B.TXT [BRAVO]' % d),
        ('B common file', 'cat %s:\\COMMON.TXT [from B]' % d),
        ('A file gone on B', 'cat %s:\\A.TXT ERROR' % d),
        ('write on B', 'cat %s:\\NEW.TXT [written-on-B]' % d),
        ('40 MB on the larger B', 'fill %s:\\BIG.BIN 40 MB OK' % d),
    ]
    ok = done
    for name, pat in checks:
        hit = pat in ser
        ok &= hit
        print('%s %s' % ('ok  ' if hit else 'FAIL', name))
    # A seen twice (boot + re-plug)
    n = ser.count('cat %s:\\A.TXT [ALPHA]' % d)
    print('%s A read %d times' % ('ok  ' if n >= 2 else 'FAIL', n)); ok &= n >= 2
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    for bad in ('BUGCHECK', 'KMODE', 'STOP:'):
        if bad in ser or bad in ulog:
            ok = False; print('FAIL', bad)
    print('RESULT msc-swap %s%s %s' % (hc, ' nt4' if nt4 else '', 'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
