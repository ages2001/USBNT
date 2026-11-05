"""Native storage/keyboard hot-plug, retained data and unaffected root sibling.

Every asserted observation comes from the running USB core and SCSI/HID
paths. Temporary disk images are private to this invocation.
"""
import sys, tempfile
from pathlib import Path
from qharn import Harness, disk

hc = sys.argv[1] if len(sys.argv) > 1 else 'qemu-xhci'
assert hc in ('qemu-xhci', 'usb-ehci', 'piix3-usb-uhci', 'pci-ohci')

def show(lines):
    for line in lines:
        print(line, flush=True)
    return lines

def require(lines, marker):
    assert any(marker in line for line in lines), (marker, lines)

with tempfile.TemporaryDirectory(prefix='usbnt-hotplug-') as temp:
    first, second = str(Path(temp)/'first.img'), str(Path(temp)/'second.img')
    disk(first); disk(second, 32)
    # EHCI needs a high-speed hub; a full-speed root device belongs to a
    # companion controller. The retained QEMU model implements hs=on.
    hub = 'usb-hub,bus=hc.0,port=1,id=hub1' + (',hs=on' if hc == 'usb-ehci' else '')
    h = Harness(['-device', hc+',id=hc',
                 '-device', hub,
                 '-drive', 'if=none,id=d1,format=raw,file='+first,
                 '-drive', 'if=none,id=d2,format=raw,file='+second,
                 '-device', 'usb-kbd,bus=hc.0,port=1.2,id=k1'])
    def command(text):
        return show(h.cmd(text))
    def keyboard(key, scan):
        h.hmp('sendkey shift-'+key)
        lines=show(h.collect(1))
        for marker in ('KEY 2a down', 'KEY '+scan+' down', 'KEY '+scan+' up', 'KEY 2a up'):
            require(lines, marker)
    try:
        show(h.wait('harness: ready', 60)); h.wait('READY')
        require(command('ls'), 'KBDS 1 MICE 0')
        print('== plug storage behind hub', flush=True)
        h.qmp_cmd('device_add', driver='usb-storage', bus='hc.0', port='1.3', drive='d1', id='s1')
        show(h.collect(3)); command('tur 0')
        require(command('tur 0'), 'TUR srb=01')
        require(command('wpat 0 7 64 9'), 'WRITE srb=01 scsi=00 xfer=32768')
        require(command('vpat 0 7 64 9'), 'VERIFY bad=0')
        keyboard('b','30')
        print('== unplug storage; keyboard must remain usable', flush=True)
        h.qmp_cmd('device_del', id='s1'); show(h.collect(3))
        require(command('tur 0'), 'TUR srb=84')
        keyboard('c','2e')
        print('== plug independent root disk', flush=True)
        h.qmp_cmd('device_add', driver='usb-storage', bus='hc.0', port='2', drive='d2', id='s2')
        show(h.collect(3)); command('tur 0')
        require(command('tur 0'), 'TUR srb=01')
        require(command('wpat 0 11 64 17'), 'WRITE srb=01 scsi=00 xfer=32768')
        require(command('vpat 0 11 64 17'), 'VERIFY bad=0')
        print('== replug first disk; verify retained contents', flush=True)
        h.hmp('drive_add 0 if=none,id=d1b,format=raw,file='+first)
        h.qmp_cmd('device_add', driver='usb-storage', bus='hc.0', port='1.3', drive='d1b', id='s1b')
        show(h.collect(3)); command('tur 1')
        require(command('tur 1'), 'TUR srb=01')
        require(command('vpat 1 7 64 9'), 'VERIFY bad=0')
        print('== unplug populated hub; preserve root disk', flush=True)
        h.qmp_cmd('device_del', id='hub1'); show(h.collect(3))
        lines=command('ls')
        require(lines,'SLOT 0 present=1'); require(lines,'SLOT 1 present=0'); require(lines,'KBDS 0 MICE 0')
        require(command('tur 1'),'TUR srb=84')
        require(command('vpat 0 11 64 17'),'VERIFY bad=0')
    finally:
        h.close()
print('RESULT hotplug',hc,'PASS',flush=True)
