#!/usr/bin/env python3
"""usbtree in an NT guest with an SD card (QEMU sdhci-pci + sd-card), a USB
stick and a HID over I2C touch pad (dw-i2c-pci + i2c-hid): usbtree /dump lists
them, the card is readable through its drive letter.

  nttree.py [--nt4]
"""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ntmsc import stick, W, AUDIONT
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))


def main():
    nt4 = '--nt4' in sys.argv
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    os.makedirs(W, exist_ok=True)
    sd, us = os.path.join(W, 'tree_sd.img'), os.path.join(W, 'tree_usb.img')
    stick(sd, 64, 'SDCARD', [('sd.txt', b'SDCARD-OK\r\n')])
    stick(us, 32, 'USBSTICK', [('u.txt', b'USB-OK\r\n')])
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'usbwork-tree.raw')
    vm.sh('cp', '--sparse=always', img, work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbtree.exe'), '/USBNT/USBTREE.EXE')
    com2 = os.path.join(vm.OUT, 'com2.log')
    dev = ['-device', 'qemu-xhci,id=hc', '-drive', 'if=none,id=u1,format=raw,file=' + us,
           '-device', 'usb-storage,bus=hc.0,drive=u1',
           '-device', 'sdhci-pci', '-drive', 'if=none,id=sdcard0,format=raw,file=' + sd,
           '-device', 'sd-card,drive=sdcard0,bus=sd-bus',
           '-device', 'dw-i2c-pci,id=i2c0', '-device', 'i2c-hid,bus=i2c0.0,address=0x2c,kind=ptp',
           '-serial', 'file:' + com2]
    bat = ['@echo off',
           'C:\\USBNT\\USBTREE.EXE /dump C:\\USBNT\\TREE.TXT',
           'for %%d in (D E F G H I) do if exist %%d:\\SD.TXT type %%d:\\SD.TXT',
           'for %%d in (D E F G H I) do if exist %%d:\\U.TXT type %%d:\\U.TXT',
           'type C:\\USBNT\\TREE.TXT', '']
    open('/tmp/tree-run.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/tree-run.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    if os.path.exists(com2):
        os.remove(com2)
    v = vm.VM(work, 'none', dev)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        v.key('ctrl-alt-delete', 4); v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    done = v.wait_serial('##AGENT-SHUTDOWN##', 300)
    time.sleep(3)
    v.shot('tree')
    v.quit()
    ser = v.serial()
    tree = os.path.join(vm.OUT, 'TREE.TXT')
    if os.path.exists(tree):
        os.remove(tree)
    got = vm.mget(work, '/USBNT/TREE.TXT', tree)
    t = open(tree, 'rb').read().decode('latin1') if got else ''
    print(t)
    ulog = open(com2, 'rb').read().decode('latin1') if os.path.exists(com2) else ''
    print('\n'.join(l for l in ulog.splitlines() if 'sd' in l.lower() or 'i2c' in l or 'slot' in l))
    checks = [('guest finished', bool(done)),
              ('SD card readable', 'SDCARD-OK' in ser),
              ('USB stick readable', 'USB-OK' in ser),
              ('reader section', 'SD card readers' in t),
              ('reader with the card and its file system', any('SD Card Reader' in l and 'FAT16' in l for l in t.splitlines())),
              ('reader transport', 'SD card reader (PCI / vendor USB)' in t),
              ('I2C section', 'HID over I2C (touch pads, touch screens)' in t),
              ('I2C controller', 'I2C controller 00:' in t and 'DesignWare I2C, PCI 8086:9D61' in t),
              ('touch pad', 'Touch pad (mouse mode) 06CB:7001' in t and 'address 2C' in t),
              ('USB stick in the tree', 'USBSTICK' in t or 'QEMU' in t)]
    ok = True
    for name, c in checks:
        ok &= bool(c)
        print('%s %s' % ('ok  ' if c else 'FAIL', name))
    print('RESULT tree%s %s' % (' nt4' if nt4 else '', 'PASS' if ok else 'FAIL'))


if __name__ == '__main__':
    main()
