#!/usr/bin/env python3
"""NT 3.1 hot plug of USB keyboard, mouse, storage, audio; standard or MPS HAL (8 CPUs).
  hp31.py std|mps [xhci|ehci|uhci|ohci] [cpus]"""
import os, sys, time, json, socket, subprocess
A = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(A, 'tools'))
os.environ.setdefault('AUDIONT_RTC', '2026-10-02')
os.environ['MTOOLS_SKIP_CHECK'] = '1'
hal = sys.argv[1] if len(sys.argv) > 1 else 'std'
hc = sys.argv[2] if len(sys.argv) > 2 else 'xhci'
cpus = int(sys.argv[3]) if len(sys.argv) > 3 else (8 if hal == 'mps' else 1)
os.environ['AUDIONT_OUT'] = '/tmp/usbt/hp31_%s_%s%s' % (hal, hc, os.environ.get('TAG', ''))
import vm
OUTD = vm.OUT
os.makedirs(OUTD, exist_ok=True)
U = os.environ.get('USBNT_OUT', os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'build', 'out'))
ATT = os.environ.get('MPS_HAL_DIR', '/mnt/user-data/attachments/a7d9dfc5-aa32-5dc0-8040-3e0689f7d9d0/')
HC = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}
work = os.path.join(OUTD, 'work.raw')
vm.sh('cp', '--sparse=always', vm.BASE, work)
vm.mmd(work, '/USBNT')
for f in ('usbinst.exe', 'usbnt.sys', 'usbmon.exe', 'inptest.exe', 'usbeject.exe', 'fstest.exe'):
    vm.mcopy(work, os.path.join(U, f), '/USBNT/' + f.upper())
if hal == 'mps':
    vm.mcopy(work, ATT + '3b239f60-HALMPCPU.DLL', '/WINNT31/SYSTEM32/HAL.DLL')
    vm.mcopy(work, ATT + '71187daa-NTKRCPU.EXE', '/WINNT31/SYSTEM32/NTOSKRNL.EXE')
stick = os.path.join(OUTD, 'stick.img')
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import ntmsc; ntmsc.stick(stick, 64, 'USBSTICK', [('hello.txt', b'hello from usb\r\n')])
DBG = os.environ.get('DBGPORT', '0x2f8')
com2 = os.path.join(OUTD, 'com2.log' if DBG == '0x2f8' else 'debugcon.log'); qmpp = os.path.join(OUTD, 'qmp.sock')
for f in (com2, qmpp):
    if os.path.exists(f): os.remove(f)
os.environ['AUDIONT_CPU'] = os.environ.get('CPU', 'pentium3')
extra = os.environ.get('QX', '').split() + ['-smp', str(cpus), '-device', '%s,id=hc' % HC[hc], '-qmp', 'unix:%s,server,nowait' % qmpp,
         ] + (['-serial', 'file:' + com2] if DBG == '0x2f8' else []) + ['-drive', 'if=none,id=st,format=raw,file=' + stick,
         '-audiodev', 'none,id=ua']
def bat(lines, name):
    p = os.path.join(OUTD, name); open(p, 'w').write('\r\n'.join(lines + [''])); vm.mcopy(work, p, '/AUDIONT/TEST/AUTORUN.BAT')
bat(['@echo off', 'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=' + DBG + ' /set DebugLevel=3'], 'pre.bat')
v = vm.VM(work, 'none', extra)
ok1 = v.wait_serial('##AGENT-SHUTDOWN##', 300); time.sleep(10); v.quit()
print('boot 1 (install):', 'done' if ok1 else 'TIMEOUT', flush=True)
if os.environ.get('STOP2'):
    v = vm.VM(work, 'none', extra); v.wait_serial('##AGENT-START##', int(os.environ['STOP2']), 30); v.shot('stop2'); v.quit(); sys.exit(0)
L = 'D E F G H I J K L M'
bat(['@echo off', 'start C:\\USBNT\\USBMON.EXE', 'C:\\USBNT\\INPTEST.EXE 150',
     'C:\\USBNT\\FSTEST.EXE on HELLO.TXT cmd /c echo USBDISK ?',
     'C:\\USBNT\\FSTEST.EXE on HELLO.TXT cmd /c copy c:\\boot.ini ?:\\B1.INI',
     'C:\\USBNT\\FSTEST.EXE parts',
     'C:\\USBNT\\USBEJECT.EXE /list'], 'run.bat')
for f in (com2, qmpp):
    if os.path.exists(f): os.remove(f)
v = vm.VM(work, 'none', extra)
class Q:
    def __init__(s, p):
        for _ in range(100):
            try: s.s = socket.socket(socket.AF_UNIX); s.s.connect(p); break
            except OSError: time.sleep(0.1)
        s.f = s.s.makefile('rwb'); s.f.readline(); s.c('qmp_capabilities')
    def c(s, name, **a):
        s.f.write((json.dumps({'execute': name, 'arguments': a}) + '\n').encode()); s.f.flush()
        while True:
            r = json.loads(s.f.readline())
            if 'event' not in r: return r
q = Q(qmpp)
def keys(text, dev):
    for ch in text:
        for down in (True, False):
            q.c('input-send-event', events=[{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': ch}}}]); time.sleep(0.05)
def move(dev):
    for i in range(10):
        q.c('input-send-event', events=[{'type': 'rel', 'data': {'axis': 'x', 'value': 5}}, {'type': 'rel', 'data': {'axis': 'y', 'value': 3}}]); time.sleep(0.05)
def c2():
    try: return open(com2, 'rb').read().decode('latin1')
    except OSError: return ''
started = v.wait_serial('##AGENT-START##', 240, int(os.environ.get('SHOTS', '0')))
if not started:
    v.key('ctrl-alt-delete', 4); v.key('ret', 2)
time.sleep(25)
res = {}
for cyc, txt in ((1, 'abc'), (2, 'xyz')):
    n0 = len(c2())
    print(q.c('device_add', driver='usb-kbd', bus='hc.0', id='k%d' % cyc))
    print(q.c('device_add', driver='usb-mouse', bus='hc.0', id='m%d' % cyc))
    if cyc > 1:
        print(q.c('human-monitor-command', **{'command-line': 'drive_add 0 if=none,id=st%d,format=raw,file=%s' % (cyc, stick)}))
    print(q.c('device_add', driver='usb-storage', bus='hc.0', id='s%d' % cyc, drive='st' if cyc == 1 else 'st%d' % cyc))
    print(q.c('device_add', driver='usb-uac', bus='hc.0', id='a%d' % cyc, audiodev='ua', uac=1))
    time.sleep(12)
    keys(txt, 'k%d' % cyc); move('m%d' % cyc); time.sleep(2)
    if cyc == 1:
        for d in ('k1', 'm1', 's1', 'a1'):
            print(q.c('device_del', id=d))
        time.sleep(10)
    res['c%d' % cyc] = c2()[n0:]
ok = v.wait_serial('##AGENT-SHUTDOWN##', 300)
time.sleep(5); v.shot('final'); v.quit()
ser = v.serial()
vm.mget(work, '/AUDIONT/TEST/OUT.TXT', os.path.join(OUTD, 'OUT.TXT')); ser += open(os.path.join(OUTD, 'OUT.TXT'), 'rb').read().decode('latin1')
open(os.path.join(OUTD, 'res.txt'), 'w').write(ser + '\n=====\n' + c2())
inp = [l for l in ser.splitlines() if 'INPTEST' in l]
print('\n'.join(inp)); print('\n'.join(l for l in ser.splitlines() if 'USBDISK' in l or 'slot' in l.lower()))
chars = inp[-1] if inp else ''
r = {'abc': 'abc' in chars, 'xyz': 'xyz' in chars, 'disk': any('USBDISK' in l for l in ser.splitlines()), 'done': ok}
print('RESULT', hal, hc, cpus, r)
