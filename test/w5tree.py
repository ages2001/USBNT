"""USBNT in Windows 2000 / XP x64 Device Manager, and the devices working (short test,
one boot per guest): usbinst /install, then on an xHCI a hub with a keyboard and a
mouse behind it and a stick on a root port.

  QEMU=... flock /tmp/claude-0/vm.lock python3 w5tree.py [w2k|xp64 ...] [--keep]

Checked with fstest devlist (what Device Manager lists): the controller's own name
("QEMU USB 3.0 Host Controller (USBNT)"), "USB 3.x Root Hub (USBNT)" below it, the hub
("USB 1.1 Hub (USBNT)") below the root hub, a USB network device for Windows' drivers by
its product name (Windows 2000: one node per function, "... - Communications (Interfaces
0-1)"), the keyboard and mouse below the hub, the stick below the root hub,
"USBNT USB Keyboard" / "USBNT USB Mouse" in their classes and the disk - all without a
problem code; then keys typed and the mouse moved and clicked on the USB devices only
reach a window, the stick is read and written.
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import vm5
import ntmsc
from ntmsc import stick, W
from w5usb import driver_files, no_signing_prompt, OUT

INP = os.path.join(OUT, 'inptest.exe')


def send(vm, events):
    vm.qmp('input-send-event', device='vga0', events=events)
    time.sleep(0.08)


def typing(vm, word):
    for ch in word:
        for down in (True, False):
            send(vm, [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': ch}}}])


def mouse(vm):
    for _ in range(10):
        send(vm, [{'type': 'rel', 'data': {'axis': 'x', 'value': 12}},
                  {'type': 'rel', 'data': {'axis': 'y', 'value': 6}}])
    for down in (True, False):
        send(vm, [{'type': 'btn', 'data': {'down': down, 'button': 'left'}}])


def inptest_window(vm):
    """inptest's window: the whole screen white"""
    ppm = os.path.join(vm.dir, 'probe.ppm')
    try:
        vm.qmp('screendump', filename=ppm)
        time.sleep(0.3)
        from PIL import Image
        im = Image.open(ppm).convert('L').resize((64, 48))
        px = list(im.getdata())
    except Exception:
        return False
    return sum(1 for p in px if p >= 250) >= len(px) * 0.97


def devs(out):
    """FSTEST dev lines: (class, name, service, problem, id, parent)"""
    r = []
    for m in re.finditer(r'FSTEST dev (.*?) \| (.*?) \| svc=(.*?) \| prob=(\d+) \| (.*?) \| parent=(.*?)\r?\n', out):
        r.append(dict(cls=m.group(1), name=m.group(2), svc=m.group(3), prob=int(m.group(4)), id=m.group(5).upper(),
                      parent=m.group(6).upper()))
    return r


CLASSES = {'USB': '{36FC9E60-C465-11CF-8056-444553540000}', 'Keyboard': '{4D36E96B-E325-11CE-BFC1-08002BE10318}',
           'Mouse': '{4D36E96F-E325-11CE-BFC1-08002BE10318}', 'DiskDrive': '{4D36E967-E325-11CE-BFC1-08002BE10318}'}


def isclass(d, name):
    """the class name, or its GUID (a device without an INF has only the GUID)"""
    return d['cls'].upper() in (name.upper(), CLASSES[name])


def find(ds, text):
    return [d for d in ds if text.lower() in d['name'].lower()]


def run(guest, keep=False):
    g = vm5.GUESTS[guest]
    os.makedirs(W, exist_ok=True)
    img = os.path.join(W, 'w5tree-%s.img' % guest)
    if os.path.exists(img):
        os.remove(img)
    ntmsc.W = os.path.join(W, 'tmp-tree-' + guest)
    os.makedirs(ntmsc.W, exist_ok=True)
    stick(img, 64, 'TREESTICK', [('hello.txt', b'hello from usb\n')])
    F = 'C:\\USBNT\\FSTEST.EXE'
    bat = [
        'start /b %s clicker C:\\USBNT\\STOP 600000' % F,     # clicks Windows XP's driver dialogs
        'C:\\USBNT\\USBINST.EXE /install /nogroup /nomon /set DebugPort=0x2f8 /set DebugLevel=%s' % os.environ.get('W5_DEBUGLEVEL', '2'),
        'C:\\USBNT\\INPTEST.EXE 90',
        '%s devlist USBNT' % F,
        '%s devlist VID_0525' % F,
        '%s devlist VID_46F4&PID_0102' % F,
        '%s find HELLO.TXT 60000' % F,
        '%s cat ?:\\HELLO.TXT' % F,
        '%s write ?:\\TREE.TXT written-by-%s' % (F, guest),
        '%s cat ?:\\TREE.TXT' % F,
        '%s write C:\\USBNT\\STOP x' % F,
        '%s sleep 2000' % F,
    ]
    files = driver_files() + [(INP, '/USBNT/INPTEST.EXE')]
    work, g = vm5.prepare(guest, work=os.path.join(vm5.W5_IMG, 'wttree-%s.raw' % guest), files=files, bat=bat)
    no_signing_prompt(work, g)
    # the VGA gets an id for the USB devices' input (QMP device=vga0); the PCI slots stay those of
    # -vga std (VGA 2, NIC 3, then the disk controller): no new hardware for Windows
    front = ['-vga', 'none', '-device', 'VGA,id=vga0,addr=0x2', '-nic', 'none', '-device', 'e1000,addr=0x3']
    extra = ['-device', 'qemu-xhci,id=xhci',
             '-device', 'usb-hub,bus=xhci.0,port=1,id=hub1',
             '-device', 'usb-kbd,bus=xhci.0,port=1.1,display=vga0,id=ukbd',
             '-device', 'usb-mouse,bus=xhci.0,port=1.2,display=vga0,id=umou',
             '-blockdev', 'driver=file,filename=%s,node-name=stickf' % img,
             '-blockdev', 'driver=raw,file=stickf,node-name=stick',
             '-device', 'usb-storage,drive=stick,bus=xhci.0,port=2,id=stickdev',
             '-device', 'usb-net,bus=xhci.0,port=3,id=unet',
             '-audiodev', 'none,id=snd0', '-device', 'usb-uac,bus=xhci.0,port=4,id=uac,audiodev=snd0,vendor=on']
    vm = vm5.VM(guest, work, extra=extra, name='tree-' + guest, front=front)
    typed = False
    t0 = time.time()
    # the keys and the mouse go to inptest's window (90 s) a few times once it is on the screen:
    # full screen and white (the agent's output reaches the serial line only at its end, and
    # kbdclass is on USBNT's keyboard already before usbinst - the device node gets its class
    # from usbnt.sys - so neither says when the window is there)
    while time.time() - t0 < 900 and not inptest_window(vm) and '##AGENT-DONE##' not in vm.serial():
        time.sleep(2)
    if inptest_window(vm):
        for _ in range(5):
            time.sleep(6)
            typing(vm, ['shift'])
            typing(vm, list('usbkey') + ['spc'])
            mouse(vm)
        typed = True
    ok = vm.wait_serial('##AGENT-DONE##', 900, shots=60)
    time.sleep(2)
    vm.shot('end')
    out, log = vm.serial(), vm.log()
    vm.quit()
    print('\n'.join(l for l in out.splitlines() if l.startswith('FSTEST') or l.startswith('INPTEST')))
    ds = devs(out)
    hc = [d for d in ds if d['id'].startswith('PCI\\VEN_1B36&DEV_000D')]
    root = find(ds, 'Root Hub (USBNT)')
    hub = [d for d in ds if d['name'].startswith('USB 1.1 Hub') and d['id'].startswith('USBNT\\VID_')]
    net = [d for d in ds if d['id'].startswith('USB\\VID_0525&PID_A4A2')]
    uac = [d for d in ds if d['id'].startswith('USB\\VID_46F4&PID_0102')]
    kbd = find(ds, 'USB Keyboard (')
    mou = find(ds, 'USB Mouse (')
    msd = find(ds, 'USB Mass Storage Device')
    m = re.search(r'INPTEST chars=\[(.*?)\] left=(\d+).*?moves=(\d+)', out)
    chars, left, moves = (m.group(1), int(m.group(2)), int(m.group(3))) if m else ('', 0, 0)

    def under(child, parent):
        return bool(child) and bool(parent) and child[0]['parent'] == parent[0]['id']

    checks = {
        'agent finished': ok,
        'controller named': bool(hc) and hc[0]['name'] == 'QEMU USB 3.0 Host Controller (USBNT)' and hc[0]['prob'] == 0,
        'root hub below the controller': bool(root) and root[0]['prob'] == 0 and isclass(root[0], 'USB') and
                                         under(root, hc),
        'hub below the root hub': bool(hub) and hub[0]['prob'] == 0 and under(hub, root),
        'keyboard / mouse below the hub': bool(kbd) and bool(mou) and kbd[0]['prob'] == 0 and mou[0]['prob'] == 0 and
                                          under(kbd, hub) and under(mou, hub),
        'stick below the root hub': bool(msd) and msd[0]['prob'] == 0 and under(msd, root),
        # a device for Windows' own drivers (usbdi.c): its product name; Windows 2000 (no usbccgp): one
        # node per function, "<product> - <class> (Interface n)"
        'network device named': bool(net) and all(d['name'].startswith('RNDIS/QEMU USB Network Device') for d in net) and
                                (guest != 'w2k' or any(' - ' in d['name'] and '(Interface' in d['name'] for d in net)),
        # an interface association without a name, vendor specific (no driver): Windows 2000 - USBNT's
        # function node, XP - usbccgp's function, named by the string USBNT gives the association
        'function without a name named': any(d['name'] == 'QEMU UAC Test Device - Vendor Specific (Interfaces 0-2)'
                                             for d in uac),
        'USBNT USB Keyboard (Keyboard class)': any(d['name'] == 'USBNT USB Keyboard' and isclass(d, 'Keyboard') and
                                                   d['prob'] == 0 for d in ds),
        'USBNT USB Mouse (Mouse class)': any(d['name'] == 'USBNT USB Mouse' and isclass(d, 'Mouse') and
                                             d['prob'] == 0 for d in ds),
        'disk (DiskDrive class)': any(isclass(d, 'DiskDrive') and d['prob'] == 0 for d in ds),
        'no USBNT device with a problem': bool(ds) and all(d['prob'] == 0 for d in ds if d not in net and d not in uac),
        'keys typed': typed and 'usbkey' in chars,
        'mouse moved and clicked': moves > 0 and left > 0,
        'stick read': '[hello from usb' in out,
        'stick written': 'written-by-%s' % guest in out,
    }
    for k, v in checks.items():
        print('%-40s %s' % (k, 'PASS' if v else 'FAIL'))
    if not keep:
        os.remove(work)
    print('RESULT w5tree %s %s' % (guest, 'PASS' if all(checks.values()) else 'FAIL'))
    return all(checks.values())


if __name__ == '__main__':
    guests = [a for a in sys.argv[1:] if not a.startswith('--')] or ['w2k']
    res = [run(gu, '--keep' in sys.argv) for gu in guests]
    sys.exit(0 if all(res) else 1)
