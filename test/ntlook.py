#!/usr/bin/env python3
"""Screenshots of the USBNT programs in an NT guest: the USBNT program group
with its icons (usbinst creates it), usbtree with the pictures of its list
(camera, stick, SD card, touch pad), usbcam, usbmon, and their icons when
minimised.  Pictures in $AUDIONT_OUT (look-*.png).

  ntlook.py [--nt4]
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
    sd, us = os.path.join(W, 'look_sd.img'), os.path.join(W, 'look_usb.img')
    stick(sd, 64, 'SDCARD', [('sd.txt', b'SDCARD-OK\r\n')])
    stick(us, 32, 'USBSTICK', [('u.txt', b'USB-OK\r\n')])
    work = os.path.join(vm.IMG_DIR, 'usbwork-look.raw')
    vm.sh('cp', '--sparse=always', os.path.join(vm.IMG_DIR, 'usbbase.raw'), work)
    out = os.path.join(ROOT, 'build/out')
    vm.mcopy(work, os.path.join(out, 'usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    for f in ('usbinst.exe', 'usbtree.exe', 'usbmon.exe', 'usbcam.exe', 'usbeject.exe', 'fstest.exe'):
        vm.mcopy(work, os.path.join(out, f), '/USBNT/' + f.upper())
    vm.mcopy(work, os.path.join(out, 'usbnt.sys'), '/USBNT/USBNT.SYS')
    vm.mcopy(work, os.path.join(ROOT, 'README.md'), '/USBNT/README.TXT')
    F = 'C:\\USBNT\\FSTEST.EXE'
    bat = ['@echo off',
           'C:\\USBNT\\USBINST.EXE /install > C:\\USBNT\\INST.TXT',
           '%s sleep 25000' % F,
           'start C:\\USBNT\\USBTREE.EXE',
           '%s sleep 15000' % F,
           'start C:\\USBNT\\USBCAM.EXE',
           '%s sleep 15000' % F,
           'start C:\\USBNT\\USBMON.EXE /show',
           '%s sleep 90000' % F, '']
    open('/tmp/look-run.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/look-run.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    dev = ['-device', 'qemu-xhci,id=hc', '-drive', 'if=none,id=u1,format=raw,file=' + us,
           '-device', 'usb-storage,bus=hc.0,drive=u1', '-device', 'usb-uvc,bus=hc.0',
           '-device', 'usb-kbd,bus=hc.0', '-device', 'usb-tablet,bus=hc.0',
           '-device', 'sdhci-pci', '-drive', 'if=none,id=sdcard0,format=raw,file=' + sd,
           '-device', 'sd-card,drive=sdcard0,bus=sd-bus',
           '-device', 'dw-i2c-pci,id=i2c0', '-device', 'i2c-hid,bus=i2c0.0,address=0x2c,kind=ptp']
    v = vm.VM(work, 'none', dev)
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        v.key('ctrl-alt-delete', 4); v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    time.sleep(3)
    # the image's "a service failed" message: its OK button (the tablet moves the pointer)
    v.cmd('mouse_move 320 295'); time.sleep(0.5)
    v.cmd('mouse_button 1'); time.sleep(0.2); v.cmd('mouse_button 0'); time.sleep(2)
    time.sleep(12)
    v.shot('look-0-group')
    time.sleep(22)
    v.shot('look-1-tree')
    time.sleep(15)
    v.shot('look-2-cam')
    time.sleep(15)
    v.shot('look-3-mon')
    # minimise the three windows (Alt+Space, N): NT 3.x shows their icons
    for _ in range(3):
        v.key('alt-spc', 1); v.key('n', 2)
    v.shot('look-4-icons')
    v.quit()
    print('screenshots in', vm.OUT)


if __name__ == '__main__':
    main()
