#!/usr/bin/env python3
"""USB Monitor (usbmon.exe) in an NT guest: the copy started at logon has no
window; "usbmon /show" (the program group item) must bring up its window.
A stick is plugged while it runs (a drive letter is given) and pulled (the
letter is taken back); screenshots show the window (ntmon-*.png).

  ntmon.py [HC] [--nt4]
"""
import os, sys, time
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ntmsc import stick, HC, W


def main():
    hc = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'xhci'
    nt4 = '--nt4' in sys.argv
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    os.makedirs(W, exist_ok=True)
    a = os.path.join(W, 'monA.img')
    stick(a, 32, 'MONSTICK', [('a.txt', b'ALPHA\r\n')])
    work = os.path.join(vm.IMG_DIR, 'usbwork-mon.raw')
    vm.sh('cp', '--sparse=always', os.path.join(vm.IMG_DIR, 'usbbase.raw'), work)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    # the copy Winlogon starts (if the image has it in Userinit) and the one the batch starts
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbmon.exe'), '/%s/SYSTEM32/USBMON.EXE' % win)
    vm.mcopy(work, os.path.join(ROOT, 'build/out/usbmon.exe'), '/USBNT/USBMON.EXE')
    vm.mcopy(work, os.path.join(ROOT, 'build/out/fstest.exe'), '/USBNT/FSTEST.EXE')
    F = 'C:\\USBNT\\FSTEST.EXE'
    bat = ['@echo off',
           'start C:\\USBNT\\USBMON.EXE',               # like logon: no window
           '%s sleep 10000' % F,
           'start C:\\USBNT\\USBMON.EXE /show',         # the program group item
           '%s sleep 60000' % F, '']
    open('/tmp/monrun.bat', 'w').write('\r\n'.join(bat))
    vm.mcopy(work, '/tmp/monrun.bat', '/AUDIONT/TEST/AUTORUN.BAT')
    v = vm.VM(work, 'none', ['-device', '%s,id=hc' % HC[hc]])
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        v.key('ctrl-alt-delete', 4); v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 120)
    time.sleep(8)
    v.shot('ntmon-1-hidden')
    time.sleep(10)
    v.shot('ntmon-2-shown')
    v.cmd('drive_add 0 if=none,id=dA,format=raw,file=%s' % a)
    v.cmd('device_add usb-storage,bus=hc.0,drive=dA,id=sA')
    time.sleep(15)
    v.shot('ntmon-3-plugged')
    v.cmd('device_del sA')
    time.sleep(12)
    v.shot('ntmon-4-pulled')
    v.quit()
    print('screenshots in', vm.OUT)


if __name__ == '__main__':
    main()
