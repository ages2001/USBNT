#!/usr/bin/env python3
"""SD card with a FAT32 file system in an NT guest, read through the card
reader and a FAT32 file system driver (FATNT standing in for Fastfat.sys, as
its installer does), like a real 32 GB SDHC card.

  ntsd.py READER [--nt4] [size=MB] [fs=fat32|fat16] [nofatnt] [insert]
      insert: the reader starts empty, the card goes in after logon
  ntsd.py READER [--nt4] [size=MB] sectors
      no file system: every sector holds its number; fstest disk reads the
      card through PhysicalDriveN and Partition1 (what a file system sees)
      at many offsets and lengths, also at the end of a 32 GB card
      READER: sdhci, rtsxpci, rtsxusb (on xHCI), rtsxusb-ehci, rtsxusb-uhci

The guest lists the card with usbtree, reads a text file and a 3 MB file
(compared on the host), writes a file (checked on the host).
"""
import os, subprocess, sys, time, struct
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ntmsc import W, AUDIONT, mbr
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
os.environ['MTOOLS_SKIP_CHECK'] = '1'      # FATNT leaves C: marked in use when the VM is stopped
FATNT = os.environ.get('FATNT_SYS', '/home/claude/ages2001/fatnt/bin/fatnt.sys')


def card(path, mb, fs):
    with open(path, 'wb') as f:
        f.truncate(mb << 20)
    start = 8192                                    # like SD cards from the factory
    total = (mb << 11) - start
    ptype = 0x0C if fs == 'fat32' else 0x06
    import struct as st
    e = bytes([0x00, 0xfe, 0xff, 0xff, ptype, 0xfe, 0xff, 0xff]) + st.pack('<II', start, total)
    with open(path, 'r+b') as f:
        f.seek(446); f.write(e + bytes(48)); f.write(b'\x55\xaa')
    args = ['mkfs.vfat', '-n', 'SDCARD', '--offset=%d' % start, '-h', str(start)]
    if fs == 'fat32':
        args += ['-F', '32', '-s', '64']            # 32 KB clusters, as SDHC cards come
    else:
        args += ['-F', '16']
    subprocess.check_call(args + [path, str(total // 2)], stdout=subprocess.DEVNULL)
    part = '%s@@%d' % (path, start * 512)
    big = os.urandom(3 * 1024 * 1024 + 4567)
    for name, data in (('sd.txt', b'SDCARD-OK\r\n'), ('big.bin', big)):
        tmp = os.path.join(W, name)
        open(tmp, 'wb').write(data)
        subprocess.check_call(['mcopy', '-i', part, tmp, '::/' + name.upper()])
        os.unlink(tmp)
    return part, big


def pattern_card(path, mb):
    total = mb << 11
    with open(path, 'wb') as f:
        f.truncate(mb << 20)
    start = 8192
    plen = total - start
    def fill(f, a, b):
        f.seek(a * 512)
        f.write(b''.join(struct.pack('<I', i) * 128 for i in range(a, b)))
    with open(path, 'r+b') as f:
        fill(f, 1, 70000)
        fill(f, total // 2 - 16, total // 2 + 300)
        m = start + plen // 2
        fill(f, m - 16, m + 300)
        fill(f, total - 1100, total)
        mbr = bytearray(512)
        mbr[0:13] = b'USBNT-SECTORS'
        mbr[16:20] = struct.pack('<I', total)
        mbr[446:462] = bytes([0, 0xfe, 0xff, 0xff, 0x0C, 0xfe, 0xff, 0xff]) + struct.pack('<II', start, plen)
        mbr[510:512] = b'\x55\xaa'
        f.seek(0); f.write(mbr)


def main():
    reader = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'sdhci'
    nt4 = '--nt4' in sys.argv
    opts = dict(a.split('=', 1) for a in sys.argv[2:] if '=' in a)
    mb = int(opts.get('size', '32768'))
    fs = opts.get('fs', 'fat32')
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    os.makedirs(W, exist_ok=True)
    sd = os.path.join(W, 'ntsd_card.img')
    sectors = 'sectors' in sys.argv
    if sectors:
        pattern_card(sd, mb)
        part, big = None, b''
    else:
        part, big = card(sd, mb, fs)
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'usbwork-sd.raw')
    vm.sh('cp', '--sparse=always', img, work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbtree.exe'), '/USBNT/USBTREE.EXE')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/fstest.exe'), '/USBNT/FSTEST.EXE')
    if 'nofatnt' not in sys.argv and not sectors:
        vm.mcopy(work, FATNT, '/%s/SYSTEM32/DRIVERS/FASTFAT.SYS' % win)
    com2 = os.path.join(vm.OUT, 'com2.log')
    insert = 'insert' in sys.argv
    drv = ['-drive', 'if=none,id=card0' + ('' if insert else ',format=raw,file=' + sd)]
    if reader == 'sdhci':
        dev = ['-device', 'sdhci-pci'] + drv + ['-device', 'sd-card,drive=card0,bus=sd-bus']
    elif reader == 'rtsxpci':
        dev = ['-device', 'rtsx-pci,id=rts,device-id=0x522A'] + drv + ['-device', 'sd-card,drive=card0,bus=rts.0']
    else:
        hc = {'rtsxusb': 'qemu-xhci', 'rtsxusb-ehci': 'usb-ehci', 'rtsxusb-uhci': 'piix3-usb-uhci'}[reader]
        dev = ['-device', '%s,id=hc' % hc, '-device', 'usb-rtsx,id=crw,bus=hc.0,pid=0x0129'] + drv + \
              ['-device', 'sd-card,drive=card0,bus=crw.0']
    dev += ['-serial', 'file:' + com2]
    bat = ['@echo off'] + (['C:\\USBNT\\FSTEST.EXE wait D:\\SD.TXT 90000'] if insert else []) + [
           'C:\\USBNT\\USBTREE.EXE /dump C:\\USBNT\\TREE.TXT',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT echo CARD=%%d',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT type %%d:\\SD.TXT',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT dir %%d:\\',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT C:\\USBNT\\FSTEST.EXE sum %%d:\\BIG.BIN',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT copy %%d:\\BIG.BIN C:\\USBNT\\BIG.BIN',
           'C:\\USBNT\\FSTEST.EXE sum C:\\USBNT\\BIG.BIN',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT copy C:\\USBNT\\TREE.TXT %%d:\\TREE.TXT',
           'type C:\\USBNT\\TREE.TXT', '']
    if sectors:
        bat = ['@echo off', 'C:\\USBNT\\FSTEST.EXE disk > C:\\USBNT\\DISK.TXT',
               'C:\\USBNT\\USBTREE.EXE /dump C:\\USBNT\\TREE.TXT', 'type C:\\USBNT\\DISK.TXT', '']
    open('/tmp/sd-run.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/sd-run.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    if os.path.exists(com2):
        os.remove(com2)
    v = vm.VM(work, 'none', dev)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        v.shot('logon')
        # with FATNT on the FAT boot volume of the test image NT 3.1 runs
        # without a paging file and says so first: dismiss that, log on
        for _ in range(4):
            v.key('ret', 5)
            if v.wait_serial('##AGENT-START##', 20):
                break
            v.key('ctrl-alt-delete', 4); v.key('ret', 4)
            if v.wait_serial('##AGENT-START##', 25):
                break
            v.shot('logon2')
    if insert:
        time.sleep(15)
        v.cmd('change card0 %s raw' % sd)
        print('host: card inserted')
    done = v.wait_serial('##AGENT-SHUTDOWN##', 400)
    time.sleep(3)
    v.shot('sd')
    v.quit()
    ser = v.serial()
    print(ser[-3000:])
    out = {}
    for f in ('TREE.TXT', 'BIG.BIN', 'DISK.TXT'):
        p = os.path.join(vm.OUT, f)
        if os.path.exists(p):
            os.remove(p)
        out[f] = open(p, 'rb').read() if vm.mget(work, '/USBNT/' + f, p) else b''
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    print('\n'.join(l for l in ulog.splitlines() if 'sd' in l.lower() or 'slot' in l or 'rts' in l.lower()
                    or 'error' in l.lower() or 'fail' in l.lower())[-4000:])
    t = out['TREE.TXT'].decode('latin1')
    print('\n'.join(l for l in t.splitlines() if 'SD' in l or 'FAT' in l or 'slot' in l.lower() or 'Volume' in l))
    if sectors:
        d = out['DISK.TXT'].decode('latin1')
        print(d)
        ok = bool(done) and 'DISK done, 0 failure(s)' in d
        print('RESULT sd-sectors %s %s%s %s' % (reader, ' '.join(sys.argv[2:]), ' nt4' if nt4 else '',
                                               'PASS' if ok else 'FAIL'))
        return
    wrote = subprocess.run(['mtype', '-i', part, '::/TREE.TXT'], capture_output=True).stdout
    h = 2166136261
    for c in big:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    want = '%u %08X' % (len(big), h)
    import re
    sums = re.findall(r'FSTEST sum (\S+) (\d+ [0-9A-F]{8})', ser)
    print('host sum', want, 'guest', sums)
    checks = [('guest finished', bool(done)),
              ('3 MB file on the card read correctly', any(p[1:3] == ':\\' and p[0] != 'C' and v == want for p, v in sums)),
              ('copy on C: correct', any(p.startswith('C:') and v == want for p, v in sums)),
              ('card file system in usbtree', fs.upper() in t),
              ('card mounted (text file read)', 'SDCARD-OK' in ser),

              ('file written to the card', len(wrote) > 100)]
    if out['BIG.BIN'] and out['BIG.BIN'] != big:
        a = out['BIG.BIN']
        for i in range(0, min(len(a), len(big)), 512):
            if a[i:i + 512] != big[i:i + 512]:
                print('first bad sector of BIG.BIN at byte', i, 'len', len(a)); break
    ok = True
    for name, c in checks:
        ok &= bool(c)
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
    print('RESULT sd %s %s%s %s' % (reader, ' '.join(sys.argv[2:]), ' nt4' if nt4 else '', 'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
