"""USB disks with several partitions on Windows 2000 SP0 / XP x64 SP1 (usbinst /install).

  python3 w5part.py [w2k|xp64 ...] [--hc xhci|ehci] [--usbstor | --gpt | --card] [--keep]

The first boot installs USBNT (usbinst /install), the second one runs the
test: a stick (removable medium, as USB sticks say) with FAT16, FAT16, FAT32
primary and logical FAT16 and NTFS partitions (mkpart.py 'w5', labels PARTn,
file Pn.TXT) is there at boot.  usbnt.sys
reports it as a fixed disk (RemovableAsFixed=1), so Windows' disk driver gives
every partition a drive letter.  Every file is read, the NTFS partition
written (and checked on the host); usbeject on partition 2's letter goes the
way of Safely Remove Hardware, the stick is pulled, a single partition stick is
plugged into the same port and pulled, the multi-partition stick is plugged in
again (all letters back) and at last pulled out while mounted.

--gpt: a GPT stick (FAT16, FAT32, NTFS) instead; XP x64 reads GPT, Windows 2000 and
32 bit XP do not.

--card: the stick is a card reader: its medium is taken out and another put in.

--usbstor: UsbPdo=2, so Windows' usbstor drives the stick through USBNT's
controller; usbinst /install puts usbnt.sys as a lower filter under disk.sys
on the USBSTOR disks (usbmon does it for those that come later), and the
filter reports the stick as a fixed disk.

Work: W5_OUT (/tmp/claude-0/wtpart/w5out), image <W5_IMG>/work-wtpart-<guest>.raw
(deleted at the end).
"""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
os.environ.setdefault('W5_OUT', '/tmp/claude-0/wtpart/w5out')
import vm5, w5usb, ntmsc, mkpart

W = os.path.join(os.environ['W5_OUT'], 'part')
LETTERS = 'D E F G H I J K L M N O P Q R S T U V W X Y Z'


F = 'C:\\USBNT\\FSTEST.EXE'


def on(n, cmd):
    """a batch line that runs cmd with ? = the drive that has Pn.TXT (n a number) or n.TXT"""
    name = ('P%d.TXT' % n) if isinstance(n, int) else n + '.TXT'
    return '%s on %s %s' % (F, name, cmd)


def run(guest, hc='xhci', usbstor=False, keep=False, kind='w5'):
    os.makedirs(W, exist_ok=True)
    multi = os.path.join(W, 'multi-%s.img' % guest)
    single = os.path.join(W, 'single-%s.img' % guest)
    parts = mkpart.mkstick(multi, kind)
    f32 = [p[0] for p in parts if p[4] == 'fat32'][0]
    ntmsc.W = W
    ntmsc.stick(single, 32, 'SINGLE', [('one.txt', b'SINGLE\r\n')])
    nums = [p[0] for p in parts]
    F = 'C:\\USBNT\\FSTEST.EXE'
    E = 'C:\\USBNT\\USBEJECT.EXE'
    sets = '/set DebugPort=0x2f8 /set DebugLevel=%s' % os.environ.get('W5_DEBUG', '2')
    if usbstor:
        sets += ' /set UsbPdo=2'
    inst = ['start /b %s clicker C:\\USBNT\\STOP 500000' % F,
            'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon %s' % sets,
            'C:\\USBNT\\USBINST.EXE /status', '%s sleep 20000' % F,
            '%s write C:\\USBNT\\STOP x' % F, '%s sleep 2000' % F]
    bat = ['set FSTEST_COM=COM2', 'start /b %s clicker C:\\USBNT\\STOP2 500000' % F,
           '%s echo ##PHASE-START##' % F,
           '%s find P1.TXT 120000' % F, '%s sleep 15000' % F]
    if usbstor:
        # the stick came before the filter was there: usbmon (here: usbinst /filters) adds it and
        # restarts the disk
        bat += ['C:\\USBNT\\USBINST.EXE /filters', '%s sleep 15000' % F, '%s find P1.TXT 60000' % F,
                '%s sleep 10000' % F]
    bat += ['%s find P%d.TXT 60000' % (F, n) for n in nums]
    bat += ['%s echo ##PHASE-MULTI##' % F, '%s parts' % F]
    bat += [on(n, '%s cat ?:\\P%d.TXT' % (F, n)) for n in nums]
    last = nums[-1]
    bat += [on(last, '%s write ?:\\NEW%d.TXT written-%d' % (F, last, last)),
            on(last, '%s fill ?:\\BIG.BIN 16' % F),
            on(f32, '%s write ?:\\NEW%d.TXT written-%d' % (F, f32, f32)),
            on(last, '%s cat ?:\\NEW%d.TXT' % (F, last)),
            on(2, '%s ?:' % E),
            '%s echo ##PHASE-EJECTED##' % F,
            '%s gone P1.TXT 60000' % F, '%s find ONE.TXT 120000' % F, '%s sleep 5000' % F,
            '%s echo ##PHASE-SINGLE##' % F, '%s parts' % F,
            on('ONE', '%s cat ?:\\ONE.TXT' % F),
            '%s none P2.TXT' % F,
            '%s gone ONE.TXT 120000' % F, '%s find P1.TXT 120000' % F, '%s sleep 15000' % F]
    bat += ['%s find P%d.TXT 60000' % (F, n) for n in nums]
    bat += ['%s echo ##PHASE-AGAIN##' % F, '%s parts' % F,
            on(last, '%s sum ?:\\BIG.BIN' % F),
            '%s gone P1.TXT 180000' % F,
            '%s echo ##PHASE-YANKED##' % F,
            '%s write C:\\USBNT\\STOP2 x' % F, '%s sleep 2000' % F]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, 'work-wtpart-%s.raw' % guest),
                          files=w5usb.driver_files(), bat=inst)
    w5usb.no_signing_prompt(work, g)
    bus = {'xhci': 'qemu-xhci', 'ehci': 'usb-ehci'}[hc]
    rm = os.environ.get('W5_REMOVABLE', 'on')
    hcdev = ['-device', '%s,id=hc' % bus]
    v = vm5.VM(guest, work, extra=hcdev, name='part-inst-' + guest)
    inst_ok = v.wait_serial('##AGENT-SHUTDOWN##', 900)
    time.sleep(15)
    v.quit()
    print(v.serial()[-3000:])
    print('host: installed' if inst_ok else 'host: INSTALL DID NOT FINISH')
    vm5.set_bat(work, bat)
    extra = hcdev + ['-blockdev', 'driver=file,filename=%s,node-name=mf' % multi,
                     '-blockdev', 'driver=raw,file=mf,node-name=m',
                     '-device', 'usb-storage,drive=m,bus=hc.0,port=1,id=mdev,removable=%s' % rm,
                     '-blockdev', 'driver=file,filename=%s,node-name=sf' % single,
                     '-blockdev', 'driver=raw,file=sf,node-name=s']
    v = vm5.VM(guest, work, extra=extra, name='part-' + guest)
    t0 = time.time()
    ok = v.wait_log('ejected by Windows', 1100)
    print('host: eject %s at %.0fs' % ('seen' if ok else 'NOT SEEN', time.time() - t0))
    time.sleep(3)
    v.qmp('device_del', id='mdev')
    time.sleep(6)
    v.qmp('device_add', driver='usb-storage', drive='s', bus='hc.0', port='1', id='sdev', removable=(rm == 'on'))
    print('host: single plugged at %.0fs' % (time.time() - t0))
    time.sleep(50)
    v.qmp('device_del', id='sdev')
    print('host: single removed at %.0fs' % (time.time() - t0))
    time.sleep(15)
    v.qmp('device_add', driver='usb-storage', drive='m', bus='hc.0', port='1', id='mdev', removable=(rm == 'on'))
    print('host: multi plugged again at %.0fs' % (time.time() - t0))
    # pulled out while mounted, once the letters are back and the checksum is read
    n0 = v.log().count('to Windows')
    end = time.time() + 120
    while time.time() < end and v.running() and v.log().count('to Windows') == n0:
        time.sleep(1)
    time.sleep(int(os.environ.get('YANK_AFTER', '100')))
    try:
        v.qmp('device_del', id='mdev')
        print('host: multi pulled out at %.0fs' % (time.time() - t0))
    except Exception as e:
        print('yank:', e)
    done = v.wait_serial('##AGENT-DONE##', 900, shots=120)
    time.sleep(2)
    v.shot('end')
    ser = v.serial()
    log = v.log()
    v.quit()
    if '##PHASE-START##' not in ser:
        ser = log                               # the batch ran out of time: what fstest sent to COM2
    print('\n'.join(l for l in ser.splitlines() if l.strip())[-12000:])
    print('---- COM2 (usbnt.sys) ----')
    print('\n'.join(l for l in log.splitlines() if 'slot' in l or 'partition' in l or 'PnP' in l or 'filter' in l)[-8000:])

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

    head = ser[:ser.find('##PHASE-MULTI##')] if '##PHASE-MULTI##' in ser else ser
    mp = phase('MULTI', 'EJECTED')
    for n in nums:
        check('partition %d got a letter' % n, ('find P%d.TXT' % n) in head and ('find P%d.TXT TIMEOUT' % n) not in
              head[head.rfind('find P1.TXT'):])
        check('partition %d read' % n, ('\\P%d.TXT [PARTITION %d]' % (n, n)) in mp)
    check('NTFS partition written', ('NEW%d.TXT [written-%d]' % (last, last)) in mp and 'BIG.BIN 16 MB OK' in mp)
    check('usbeject: Safely Remove Hardware', 'can now be removed safely' in mp and ok)
    ej = phase('EJECTED', 'SINGLE')
    check('letters gone after the removal', 'gone P1.TXT' in ej and 'gone P1.TXT TIMEOUT' not in ej)
    sg = phase('SINGLE', 'AGAIN')
    check('single partition stick read', 'ONE.TXT [SINGLE]' in sg)
    check('nothing of the multi stick with the single one', 'none P2.TXT OK' in sg)
    ag = phase('AGAIN', 'YANKED')
    for n in nums:
        check('partition %d back after the re-plug' % n, ('find P%d.TXT' % n) in sg and
              ('find P%d.TXT TIMEOUT' % n) not in sg)
    check('data kept', 'BIG.BIN 16777216' in ag)
    check('pulled out while mounted', 'gone P1.TXT' in phase('AGAIN') and
          'gone P1.TXT TIMEOUT' not in phase('AGAIN'))
    # host side: the NTFS partition and the FAT32 one hold what Windows wrote, all file systems intact
    for n, start, cnt, t, fs in parts:
        if fs == 'ntfs':
            tmp = os.path.join(W, 'p.img')
            subprocess.run(['dd', 'if=%s' % multi, 'of=%s' % tmp, 'bs=512', 'skip=%d' % start, 'count=%d' % cnt,
                            'status=none'])
            r = subprocess.run(['ntfscat', '-f', tmp, 'NEW%d.TXT' % n], capture_output=True)
            check('host reads NEW%d.TXT (NTFS)' % n, b'written-%d' % n in r.stdout)
            r = subprocess.run(['ntfsls', '-f', tmp], capture_output=True)
            check('partition %d NTFS intact' % n, (b'P%d.TXT' % n) in r.stdout)
            os.remove(tmp)
        else:
            r = subprocess.run(['mdir', '-i', '%s@@%d' % (multi, start * 512), '::/'], capture_output=True)
            check('partition %d %s intact' % (n, fs), r.returncode == 0 and (b'P%d' % n) in r.stdout)
    p32 = [p for p in parts if p[0] == f32][0]
    r = subprocess.run(['mtype', '-i', '%s@@%d' % (multi, p32[1] * 512), '::/NEW%d.TXT' % f32], capture_output=True)
    check('host reads NEW%d.TXT (FAT32)' % f32, (b'written-%d' % f32) in r.stdout)
    if 'BUGCHECK' in log or 'STOP 0x' in ser:
        check('no bug check', False)
    check('agent finished', done)
    print('RESULT w5part %s %s%s %s' % (guest, hc, ' usbstor' if usbstor else '', 'PASS' if res[0] else 'FAIL'))
    if not keep:
        os.remove(work)
    return res[0]


def run_usbstor(guest, hc='xhci', keep=False):
    """UsbPdo=2: Windows' usbstor drives the stick (through USBNT's controller); usbinst /filters
    puts the usbnt filter on its disk and restarts it, the filter reports it as fixed"""
    os.makedirs(W, exist_ok=True)
    multi = os.path.join(W, 'multi-st-%s.img' % guest)
    parts = mkpart.mkstick(multi, 'w5')
    nums = [p[0] for p in parts]
    last = nums[-1]
    F = 'C:\\USBNT\\FSTEST.EXE'
    sets = '/set DebugPort=0x2f8 /set DebugLevel=%s /set UsbPdo=2' % os.environ.get('W5_DEBUG', '2')
    inst = ['start /b %s clicker C:\\USBNT\\STOP 500000' % F,
            'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon %s' % sets,
            '%s sleep 20000' % F, '%s write C:\\USBNT\\STOP x' % F, '%s sleep 2000' % F]
    bat = ['set FSTEST_COM=COM2', 'start /b %s clicker C:\\USBNT\\STOP2 500000' % F,
           '%s echo ##PHASE-START##' % F,
           '%s find P1.TXT 180000' % F, '%s sleep 10000' % F, '%s parts' % F,
           # the stick is new to Windows: no filter yet (usbmon would run this)
           'C:\\USBNT\\USBINST.EXE /filters', '%s sleep 20000' % F,
           '%s find P1.TXT 60000' % F, '%s sleep 10000' % F]
    bat += ['%s find P%d.TXT 60000' % (F, n) for n in nums]
    bat += ['%s echo ##PHASE-MULTI##' % F, '%s parts' % F]
    bat += [on(n, '%s cat ?:\\P%d.TXT' % (F, n)) for n in nums]
    bat += [on(last, '%s write ?:\\NEW%d.TXT written-%d' % (F, last, last)),
            on(last, '%s fill ?:\\BIG.BIN 16' % F),
            '%s echo ##PHASE-YANK##' % F,
            '%s gone P1.TXT 120000' % F, '%s find P1.TXT 120000' % F, '%s sleep 15000' % F]
    bat += ['%s find P%d.TXT 60000' % (F, n) for n in nums]
    bat += ['%s echo ##PHASE-AGAIN##' % F, '%s parts' % F,
            on(last, '%s sum ?:\\BIG.BIN' % F),
            '%s echo ##PHASE-END##' % F, '%s write C:\\USBNT\\STOP2 x' % F, '%s sleep 2000' % F]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, 'work-wtpart-%s.raw' % guest),
                          files=w5usb.driver_files(), bat=inst)
    w5usb.no_signing_prompt(work, g)
    bus = {'xhci': 'qemu-xhci', 'ehci': 'usb-ehci'}[hc]
    hcdev = ['-device', '%s,id=hc' % bus]
    v = vm5.VM(guest, work, extra=hcdev, name='part-sinst-' + guest)
    inst_ok = v.wait_serial('##AGENT-SHUTDOWN##', 900)
    time.sleep(15)
    v.quit()
    print('host: installed' if inst_ok else 'host: INSTALL DID NOT FINISH')
    vm5.set_bat(work, bat)
    extra = hcdev + ['-blockdev', 'driver=file,filename=%s,node-name=mf' % multi,
                     '-blockdev', 'driver=raw,file=mf,node-name=m',
                     '-device', 'usb-storage,drive=m,bus=hc.0,port=1,id=mdev,removable=%s' %
                     os.environ.get('W5_REMOVABLE', 'on')]
    v = vm5.VM(guest, work, extra=extra, name='part-st-' + guest)
    t0 = time.time()
    ok = False
    for k in range(10):
        ok = v.wait_log('reported as a fixed disk', 60)
        v.shot('st%02d' % k)
        if ok:
            break
    print('host: fixed disk through the filter %s at %.0fs' % ('seen' if ok else 'NOT SEEN', time.time() - t0))
    v.wait_log('##PHASE-YANK##', 300)
    time.sleep(5)
    v.qmp('device_del', id='mdev')
    print('host: pulled out at %.0fs' % (time.time() - t0))
    time.sleep(15)
    v.qmp('device_add', driver='usb-storage', drive='m', bus='hc.0', port='1', id='mdev',
          removable=os.environ.get('W5_REMOVABLE', 'on') == 'on')
    print('host: plugged again at %.0fs' % (time.time() - t0))
    done = v.wait_serial('##AGENT-DONE##', 600)
    ser = v.serial()
    log = v.log()
    v.quit()
    if '##PHASE-START##' not in ser:
        ser = log
    print('\n'.join(l for l in ser.splitlines() if l.strip())[-10000:])
    print('---- COM2 (usbnt.sys) ----')
    print('\n'.join(l for l in log.splitlines() if 'filter' in l or 'PnP' in l)[-4000:])
    res = [True]

    def check(what, cond):
        print('%s %s' % ('ok  ' if cond else 'FAIL', what))
        res[0] &= bool(cond)

    a = ser.find('##PHASE-MULTI##')
    head, mp = (ser[:a], ser[a:ser.find('##PHASE-YANK##')]) if a >= 0 else (ser, '')
    ag = ser[ser.find('##PHASE-AGAIN##'):] if '##PHASE-AGAIN##' in ser else ''
    check('the filter reported the stick as fixed', ok and 'USBSTOR disk started' in log)
    for n in nums:
        check('partition %d read' % n, ('\\P%d.TXT [PARTITION %d]' % (n, n)) in mp)
    check('NTFS partition written', 'BIG.BIN 16 MB OK' in mp)
    check('all partitions back after pulling out and plugging in',
          all(('find P%d.TXT' % n) in ag or ('find P%d.TXT' % n) in ser[ser.find('##PHASE-YANK##'):] for n in nums)
          and 'TIMEOUT' not in ser[ser.find('##PHASE-YANK##'):])
    check('data kept', 'BIG.BIN 16777216' in ag)
    check('filter at once on the second plug (no restart)', log.count('reported as a fixed disk') >= 2)
    check('agent finished', done)
    print('RESULT w5part %s %s usbstor %s' % (guest, hc, 'PASS' if res[0] else 'FAIL'))
    if not keep:
        os.remove(work)
    return res[0]


def run_card(guest, hc='xhci', keep=False):
    """a card reader: the medium of a removable stick is taken out and another put in (QEMU's
    eject / change) - the multi-partition card (a fixed disk), no card, a single partition card
    (removable), the multi-partition card again"""
    os.makedirs(W, exist_ok=True)
    multi = os.path.join(W, 'card-multi-%s.img' % guest)
    single = os.path.join(W, 'card-single-%s.img' % guest)
    parts = mkpart.mkstick(multi, 'w5')
    ntmsc.W = W
    ntmsc.stick(single, 32, 'SINGLE', [('one.txt', b'SINGLE\r\n')])
    nums = [p[0] for p in parts]
    sets = '/set DebugPort=0x2f8 /set DebugLevel=%s' % os.environ.get('W5_DEBUG', '2')
    inst = ['start /b %s clicker C:\\USBNT\\STOP 500000' % F,
            'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon %s' % sets,
            '%s sleep 20000' % F, '%s write C:\\USBNT\\STOP x' % F, '%s sleep 2000' % F]
    bat = ['set FSTEST_COM=COM2', 'start /b %s clicker C:\\USBNT\\STOP2 500000' % F,
           '%s echo ##PHASE-START##' % F, '%s find P1.TXT 120000' % F, '%s sleep 10000' % F]
    bat += ['%s find P%d.TXT 60000' % (F, n) for n in nums]
    bat += ['%s echo ##PHASE-MULTI##' % F, on(5, '%s cat ?:\\P5.TXT' % F),
            '%s gone P1.TXT 90000' % F, '%s echo ##PHASE-NOCARD##' % F,
            '%s find ONE.TXT 90000' % F, on('ONE', '%s cat ?:\\ONE.TXT' % F), '%s echo ##PHASE-SINGLE##' % F,
            '%s gone ONE.TXT 90000' % F, '%s find P1.TXT 90000' % F, '%s sleep 10000' % F]
    bat += ['%s find P%d.TXT 60000' % (F, n) for n in nums]
    bat += ['%s echo ##PHASE-AGAIN##' % F, on(6, '%s cat ?:\\P6.TXT' % F),
            '%s echo ##PHASE-END##' % F, '%s write C:\\USBNT\\STOP2 x' % F, '%s sleep 2000' % F]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, 'work-wtpart-%s.raw' % guest),
                          files=w5usb.driver_files(), bat=inst)
    w5usb.no_signing_prompt(work, g)
    bus = {'xhci': 'qemu-xhci', 'ehci': 'usb-ehci'}[hc]
    hcdev = ['-device', '%s,id=hc' % bus]
    v = vm5.VM(guest, work, extra=hcdev, name='part-cinst-' + guest)
    inst_ok = v.wait_serial('##AGENT-SHUTDOWN##', 900)
    time.sleep(15)
    v.quit()
    print('host: installed' if inst_ok else 'host: INSTALL DID NOT FINISH')
    vm5.set_bat(work, bat)
    extra = hcdev + ['-drive', 'if=none,id=card,format=raw,file=%s' % multi,
                     '-device', 'usb-storage,drive=card,bus=hc.0,port=1,id=mdev,removable=on']
    v = vm5.VM(guest, work, extra=extra, name='part-card-' + guest)
    t0 = time.time()

    def step(marker, what, cmd, timeout=400):
        ok = v.wait_log(marker, timeout)
        time.sleep(5)
        v.hmp(cmd)
        print('host: %s at %.0fs%s' % (what, time.time() - t0, '' if ok else ' (marker not seen)'))

    step('##PHASE-MULTI##', 'card out', 'eject -f card')
    step('##PHASE-NOCARD##', 'single card in', 'change card %s raw' % single)
    step('##PHASE-SINGLE##', 'card out', 'eject -f card')
    time.sleep(10)
    v.hmp('change card %s raw' % multi)
    print('host: multi card in again at %.0fs' % (time.time() - t0))
    done = v.wait_serial('##AGENT-DONE##', 600)
    ser = v.serial()
    log = v.log()
    v.quit()
    if '##PHASE-START##' not in ser:
        ser = log
    print('\n'.join(l for l in ser.splitlines() if l.strip())[-8000:])
    print('---- COM2 (usbnt.sys) ----')
    print('\n'.join(l for l in log.splitlines() if 'slot' in l or 'PnP' in l)[-4000:])
    res = [True]

    def check(what, cond):
        print('%s %s' % ('ok  ' if cond else 'FAIL', what))
        res[0] &= bool(cond)

    def between(a, b):
        i = ser.find('##PHASE-%s##' % a)
        j = ser.find('##PHASE-%s##' % b, i + 1) if i >= 0 else -1
        return ser[i:j] if i >= 0 and j > 0 else ''

    head = ser[:ser.find('##PHASE-MULTI##')]
    check('every partition of the card got a letter', all(('find P%d.TXT' % n) in head for n in nums) and
          'TIMEOUT' not in head[head.find('##PHASE-START##'):])
    check('partition 5 read', '[PARTITION 5]' in between('MULTI', 'NOCARD'))
    check('letters gone with the card', 'gone P1.TXT OK' in between('MULTI', 'NOCARD'))
    check('single partition card read', '[SINGLE]' in between('NOCARD', 'SINGLE'))
    again = ser[ser.find('##PHASE-SINGLE##'):]
    check('the multi-partition card again: every letter back', all(('find P%d.TXT' % n) in again for n in nums) and
          'find P1.TXT TIMEOUT' not in again and '[PARTITION 6]' in again)
    check('agent finished', done)
    print('RESULT w5part %s %s card %s' % (guest, hc, 'PASS' if res[0] else 'FAIL'))
    if not keep:
        os.remove(work)
    return res[0]


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--') and a not in ('xhci', 'ehci')]
    hc = 'ehci' if '--hc' in sys.argv and sys.argv[sys.argv.index('--hc') + 1] == 'ehci' else 'xhci'
    if '--usbstor' in sys.argv:
        res = {g: run_usbstor(g, hc, '--keep' in sys.argv) for g in (args or ['w2k', 'xp64'])}
    elif '--card' in sys.argv:
        res = {g: run_card(g, hc, '--keep' in sys.argv) for g in (args or ['w2k', 'xp64'])}
    else:
        kind = 'gpt' if '--gpt' in sys.argv else 'w5'
        res = {g: run(g, hc, False, '--keep' in sys.argv, kind) for g in (args or ['w2k', 'xp64'])}
    print(res)
    sys.exit(0 if all(res.values()) else 1)
