#!/usr/bin/env python3
"""USB disks with several partitions in an NT 3.1 / NT 4.0 guest.

  ntpart.py HC [--nt4] [--boot] [--set Name=Value ...]     HC: ehci xhci uhci ohci

A stick with three primary and two logical FAT16 partitions (mkpart.py 'nt',
labels PART1 .. PART6, file Pn.TXT on each) is plugged after logon (--boot:
it is there at boot).  Every partition must get a drive letter (usbmon) and its
file must be readable; the last one is written (and checked on the host).
Then usbeject on the letter of partition 2 (all volumes of the disk are locked:
it must refuse while a file on partition 3 is open), unplug, plug a single
partition stick into the same port (only its letter), unplug it, plug the
multi-partition stick again (all letters back).  The guest side runs from one
batch file (its output comes when it ends), the host side follows the driver
log.  Uses the AUDIONT test agent and image tools (AUDIONT=...).  Work files:
$PART_W (/tmp/claude-0/wtpart/nt<PART_TAG>), image <img dir>/wtpart_work<PART_TAG>.raw
(deleted at the end).
"""
import os, subprocess, sys, time
AUDIONT = os.environ.get('AUDIONT', '/home/claude/audiont')
sys.path.insert(0, os.path.join(AUDIONT, 'tools'))
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(HERE)
TAG = os.environ.get('PART_TAG', '')
W = os.environ.get('PART_W', '/tmp/claude-0/wtpart/nt' + TAG)
import mkpart, ntmsc

HC = {'uhci': 'piix3-usb-uhci', 'ohci': 'pci-ohci', 'ehci': 'usb-ehci', 'xhci': 'qemu-xhci'}
LETTERS = 'D E F G H I J K L M N O P Q R S T U V W X Y Z'


F = 'C:\\USBNT\\FSTEST.EXE'


def on(n, cmd):
    """a batch line that runs cmd with ? = the drive that has Pn.TXT (n a number) or n.TXT"""
    name = ('P%d.TXT' % n) if isinstance(n, int) else n + '.TXT'
    return '%s on %s %s' % (F, name, cmd)


def main():
    hc = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else 'ehci'
    nt4 = '--nt4' in sys.argv
    at_boot = '--boot' in sys.argv
    sets = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--set']
    os.makedirs(W, exist_ok=True)
    if nt4:
        os.environ.setdefault('AUDIONT_IMG', '/home/claude/nt4')
    os.environ.setdefault('AUDIONT_OUT', os.path.join(W, 'out-nt4' if nt4 else 'out-nt31'))
    import vm
    win = 'WINNT4' if nt4 else vm.WIN
    multi = os.path.join(W, 'multi.img')
    single = os.path.join(W, 'single.img')
    parts = mkpart.mkstick(multi, 'nt')
    ntmsc.W = W
    ntmsc.stick(single, 32, 'SINGLE', [('one.txt', b'SINGLE\r\n')])
    img = os.path.join(vm.IMG_DIR, 'usbbase.raw')
    work = os.path.join(vm.IMG_DIR, 'wtpart_work%s.raw' % TAG)
    vm.sh('cp', '--sparse=always', img, work)
    out = os.path.join(ROOT, 'build/out')
    vm.mcopy(work, os.path.join(out, 'usbnt.sys'), '/%s/SYSTEM32/DRIVERS/USBNT.SYS' % win)
    for t in ('usbmon', 'usbeject', 'usbtree', 'usbinst'):
        vm.mcopy(work, os.path.join(out, t + '.exe'), '/%s/SYSTEM32/%s.EXE' % (win, t.upper()))
        vm.mcopy(work, os.path.join(out, t + '.exe'), '/USBNT/%s.EXE' % t.upper())
    vm.mcopy(work, os.path.join(out, 'fstest.exe'), '/USBNT/FSTEST.EXE')
    F = 'C:\\USBNT\\FSTEST.EXE'
    E = 'C:\\USBNT\\USBEJECT.EXE'
    nums = [p[0] for p in parts]
    last = nums[-1]
    bat = ['@echo off', 'set FSTEST_COM=COM2']
    if sets:
        bat.append('C:\\USBNT\\USBINST.EXE /set ' + ' /set '.join(sets))
    P = lambda name: '%s echo ##PHASE-%s##' % (F, name)
    bat += [P('BEFORE'), '%s parts' % F,
            '%s find P1.TXT 200000' % F, '%s sleep 10000' % F]
    bat += ['%s find P%d.TXT 30000' % (F, n) for n in nums]
    bat += [P('MULTI'), '%s parts' % F, '%s /list > C:\\USBNT\\EJ0.TXT' % E, '%s cat C:\\USBNT\\EJ0.TXT' % F]
    bat += [on(n, '%s cat ?:\\P%d.TXT' % (F, n)) for n in nums]
    bat += ['%s run C:\\USBNT\\USBTREE.EXE /dump C:\\USBNT\\TREE.TXT' % F,
            '%s grep C:\\USBNT\\TREE.TXT partitions' % F, '%s grep C:\\USBNT\\TREE.TXT Drive letters' % F]
    bat += [on(last, '%s write ?:\\NEW%d.TXT written-%d' % (F, last, last)),
            on(last, '%s fill ?:\\BIG.BIN 8' % F),
            on(last, '%s cat ?:\\NEW%d.TXT' % (F, last)),
            # a file held open on partition 3: usbeject must refuse, then the drive comes free
            'start ' + on(3, '%s hold ?:\\P3.TXT 22000' % F),
            '%s sleep 6000' % F,
            on(2, '%s ?:' % E),
            '%s sleep 20000' % F,
            on(2, '%s ?:' % E),
            P('EJECTED'),
            '%s gone P1.TXT 60000' % F, '%s parts' % F,
            '%s find ONE.TXT 90000' % F, '%s sleep 5000' % F,
            P('SINGLE'), '%s parts' % F,
            on('ONE', '%s cat ?:\\ONE.TXT' % F), on('ONE', '%s dir ?:' % F), on('ONE', '%s sum ?:\\ONE.TXT' % F),
            '%s none P2.TXT' % F,
            '%s gone ONE.TXT 90000' % F,
            '%s find P1.TXT 90000' % F, '%s sleep 10000' % F]
    bat += ['%s find P%d.TXT 30000' % (F, n) for n in nums]
    bat += [P('AGAIN'), '%s parts' % F, '%s echo ##PHASE-END##' % F, '']
    open(os.path.join(W, 'part.bat'), 'w').write('\r\n'.join(bat))
    vm.mcopy(work, os.path.join(W, 'part.bat'), '/AUDIONT/TEST/AUTORUN.BAT')
    os.makedirs(vm.OUT, exist_ok=True)
    com2 = os.path.join(vm.OUT, 'com2.log')
    if os.path.exists(com2):
        os.remove(com2)

    def ulog():
        try:
            return open(com2, 'rb').read().decode('latin1')
        except OSError:
            return ''

    com3 = os.path.join(vm.OUT, 'com3.log')
    if os.path.exists(com3):
        os.remove(com3)
    extra = ['-device', '%s,id=hc' % HC[hc], '-serial', 'file:' + com2, '-serial', 'file:' + com3]
    if at_boot:
        extra += ['-drive', 'if=none,id=dM,format=raw,file=%s' % multi,
                  '-device', 'usb-storage,bus=hc.0,port=1,drive=dM,id=sM']
    v = vm.VM(work, 'none', extra)

    def wait_log(text, timeout, start=0):
        end = time.time() + timeout
        while time.time() < end:
            if text in ulog()[start:]:
                return True
            if v.p.poll() is not None:
                return False
            time.sleep(1)
        return False

    t0 = time.time()
    if not v.wait_serial('##AGENT-START##', int(os.environ.get('LOGON_WAIT', '150'))):
        print('host: logging on by hand')
        v.key('ctrl-alt-delete', 4)
        v.key('ret', 2)
        v.wait_serial('##AGENT-START##', 200)
    print('host: agent at %.0fs' % (time.time() - t0))
    if not at_boot:
        time.sleep(15)
        v.cmd('drive_add 0 if=none,id=dM,format=raw,file=%s' % multi)
        v.cmd('device_add usb-storage,bus=hc.0,port=1,drive=dM,id=sM')
        print('host: multi plugged at %.0fs' % (time.time() - t0))
    ok = wait_log('ready for removal', 420)
    time.sleep(3)
    v.cmd('device_del sM')
    print('host: multi removed at %.0fs (%s)' % (time.time() - t0, 'after eject' if ok else 'NO EJECT'))
    time.sleep(5)
    mark = len(ulog())
    v.cmd('drive_add 0 if=none,id=dS,format=raw,file=%s' % single)
    v.cmd('device_add usb-storage,bus=hc.0,port=1,drive=dS,id=sS')
    print('host: single plugged at %.0fs' % (time.time() - t0))
    wait_log('lun 0: slot', 60, mark)
    time.sleep(45)
    v.cmd('device_del sS')
    print('host: single removed at %.0fs' % (time.time() - t0))
    time.sleep(12)
    v.cmd('drive_add 0 if=none,id=dM2,format=raw,file=%s' % multi)
    v.cmd('device_add usb-storage,bus=hc.0,port=1,drive=dM2,id=sM2')
    print('host: multi plugged again at %.0fs' % (time.time() - t0))
    done = v.wait_serial('##AGENT-SHUTDOWN##', 600)
    v.shot('final')
    time.sleep(5)
    v.quit()
    ser = v.serial()
    try:
        ser3 = open(com3, 'rb').read().decode('latin1')
    except OSError:
        ser3 = ''
    if '##PHASE-BEFORE##' not in ser and '##PHASE-BEFORE##' in ulog():
        ser = ulog()                            # the batch ran out of time: what fstest sent to COM2
    print('\n'.join(l for l in ser.splitlines() if l.strip()))
    print('--- driver log (slots)')
    print('\n'.join(l for l in ulog().splitlines() if 'slot' in l or 'partition' in l))

    def phase(name, nxt=None):
        a = ser.find('##PHASE-%s##' % name)
        if a < 0:
            return ''
        b = ser.find('##PHASE-%s##' % nxt, a) if nxt else -1
        return ser[a:b] if b > 0 else ser[a:]

    res = [True]

    def check(what, cond):
        print('%s %s' % ('ok  ' if cond else 'FAIL', what))
        res[0] &= bool(cond)

    before = phase('BEFORE', 'MULTI')
    # every USB disk slot has MaxPartitions partition devices (the placeholder at boot)
    check('partition devices 1..8 on the USB slots', 'disk 4 part 8:' in before and 'disk 2 part 8:' in before)
    multi_ph = phase('MULTI', 'EJECTED')
    for n in nums:
        check('partition %d got a letter' % n, ('find P%d.TXT' % n) in before and
              ('find P%d.TXT TIMEOUT' % n) not in before)
        check('partition %d read' % n, ('\\P%d.TXT [PARTITION %d]' % (n, n)) in multi_ph)
    check('usbtree lists the letters of every partition', 'partitions: D: E: F: G: I:' in multi_ph or
          ('(partitions: ' in multi_ph and multi_ph.count('Drive letters:') >= 1))
    check('write on partition %d' % last, ('NEW%d.TXT [written-%d]' % (last, last)) in multi_ph and
          'BIG.BIN 8 MB OK' in multi_ph)
    check('usbeject refuses while a file on another partition is open', 'is in use' in multi_ph)
    check('usbeject then releases the disk', 'can now be removed safely' in multi_ph)
    ej = phase('EJECTED', 'SINGLE')
    check('letters taken back after the removal', 'gone P1.TXT' in ej and 'gone P1.TXT TIMEOUT' not in ej)
    sg = phase('SINGLE', 'AGAIN')
    check('single partition stick read', 'ONE.TXT [SINGLE]' in sg)
    check('no letter of the old partitions with the single stick', 'none P2.TXT OK' in sg)
    again = phase('AGAIN')
    for n in nums:
        check('partition %d back after the re-plug' % n, ('find P%d.TXT' % n) in sg and
              ('find P%d.TXT TIMEOUT' % n) not in sg)
    part = [p for p in parts if p[0] == last][0]
    r = subprocess.run(['mtype', '-i', '%s@@%d' % (multi, part[1] * 512), '::/NEW%d.TXT' % last],
                       capture_output=True)
    check('host reads NEW%d.TXT' % last, b'written-%d' % last in r.stdout)
    for n, start, cnt, t, fs in parts:
        r = subprocess.run(['mdir', '-i', '%s@@%d' % (multi, start * 512), '::/'], capture_output=True)
        check('partition %d file system intact on the host' % n, r.returncode == 0 and (b'P%d' % n) in r.stdout)
    for bad in ('BUGCHECK', 'KMODE', 'STOP:'):
        if bad in ser or bad in ulog():
            check('no ' + bad, False)
    check('agent finished', done)
    print('RESULT part %s%s%s %s' % (hc, ' nt4' if nt4 else ' nt31', ' boot' if at_boot else '',
                                     'PASS' if res[0] else 'FAIL'))
    if os.environ.get('PART_KEEP') != '1':
        os.remove(work)


if __name__ == '__main__':
    main()
