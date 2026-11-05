#!/usr/bin/env python3
"""NT 3.1: a USB stick plugged in only after logon (empty slot since boot)."""
import os, sys, time, subprocess
sys.path.insert(0, '/home/claude/audiont/tools'); sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault('AUDIONT_RTC', '2026-10-02'); os.environ['MTOOLS_SKIP_CHECK'] = '1'
os.environ['AUDIONT_OUT'] = '/tmp/usbt/mscl'
import vm, ntmsc
os.makedirs(vm.OUT, exist_ok=True)
hc = sys.argv[1] if len(sys.argv) > 1 else 'xhci'
U = os.environ.get('USBNT_OUT', os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'out'))
a = '/tmp/usbt/lateA.img'
ntmsc.stick(a, 32, 'STICKA', [('a.txt', b'ALPHA\r\n')])
work = os.path.join(vm.OUT, 'w.raw'); vm.sh('cp', '--sparse=always', '/home/claude/nt31/usbbase.raw', work)
vm.mcopy(work, U + '/usbnt.sys', '/WINNT31/SYSTEM32/DRIVERS/USBNT.SYS')
vm.mcopy(work, U + '/fstest.exe', '/USBNT/FSTEST.EXE')
vm.mcopy(work, U + '/usbmon.exe', '/USBNT/USBMON.EXE')
F = 'C:\\USBNT\\FSTEST.EXE'
mon = os.environ.get('MON', '1') == '1'
bat = ['@echo off'] + (['start C:\\USBNT\\USBMON.EXE'] if mon else []) + [
       F + ' sleep %d' % (60000 if os.environ.get('REPLUG') else 5000), F + ' parts', F + ' on A.TXT cmd /c echo FOUND ?', F + ' sleep 60000',
       F + ' on A.TXT cmd /c type ?:\\A.TXT', F + ' parts', 'C:\\USBNT\\USBEJECT.EXE /list', '']
p = os.path.join(vm.OUT, 'a.bat'); open(p, 'w').write('\r\n'.join(bat)); vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
com2 = os.path.join(vm.OUT, 'com2.log')
if os.path.exists(com2): os.remove(com2)
v = vm.VM(work, 'none', ['-device', '%s,id=hc' % ntmsc.HC[hc], '-serial', 'file:' + com2])
v.wait_serial('##AGENT-START##', 200)
time.sleep(20)
v.cmd('drive_add 0 if=none,id=dA,format=raw,file=' + a)
v.cmd('device_add usb-storage,bus=hc.0,drive=dA,id=sA')
if os.environ.get('REPLUG'):
    time.sleep(10); v.cmd('device_del sA'); time.sleep(8)
    v.cmd('drive_add 0 if=none,id=dA2,format=raw,file=' + a)
    v.cmd('device_add usb-storage,bus=hc.0,drive=dA2,id=sA2')
ok = v.wait_serial('##AGENT-SHUTDOWN##', int(os.environ.get('WAIT', '600')), 60)
v.shot('end')
time.sleep(3); v.quit()
vm.mget(work, '/AUDIONT/TEST/OUT.TXT', os.path.join(vm.OUT, 'OUT.TXT'))
out = open(os.path.join(vm.OUT, 'OUT.TXT'), 'rb').read().decode('latin1')
print('\n'.join(l for l in out.splitlines() if 'info error' not in l))
print('RESULT late-plug', hc, 'PASS' if 'ALPHA' in out else 'FAIL')
