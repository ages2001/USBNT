"""Physical HID port reset: release held input, restore protocol/endpoints,
retain device identity and a storage sibling, then hot-unplug the hub.
This runs the production native class/core/HCD code, without the NT kernel.
"""
import re
import sys
import tempfile
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

with tempfile.TemporaryDirectory(prefix='usbnt-hid-reset-') as temp:
    backing = str(Path(temp) / 'sibling.img')
    disk(backing, 32)
    hub = 'usb-hub,bus=hc.0,port=1,id=hub' + (',hs=on' if hc == 'usb-ehci' else '')
    h = Harness(['-device', hc+',id=hc', '-device', hub,
                 '-drive', 'if=none,id=d,format=raw,file='+backing,
                 '-device', 'usb-storage,drive=d,bus=hc.0,port=2,id=disk',
                 '-device', 'usb-kbd,bus=hc.0,port=1.2,id=kbd',
                 '-device', 'usb-mouse,bus=hc.0,port=1.3,id=mouse'])
    def command(text):
        return show(h.cmd(text))
    def key(down):
        h.qmp_cmd('input-send-event', events=[{'type':'key','data':{
            'down':down,'key':{'type':'qcode','data':'a'}}}])
    def button(down):
        h.qmp_cmd('input-send-event', events=[{'type':'btn','data':{
            'down':down,'button':'left'}}])
    def identities(lines):
        return {int(m[1]):m[0] for line in lines
                if (m := re.search(r'DEV (\d+) .*hid=1$', line))}
    try:
        show(h.wait('harness: ready', 60)); h.wait('READY')
        baseline = identities(command('ls'))
        assert len(baseline) == 2, baseline
        command('tur 0'); require(command('tur 0'), 'TUR srb=01')
        require(command('wpat 0 11 64 17'), 'WRITE srb=01 scsi=00 xfer=32768')
        for cycle in range(10):
            print('CYCLE',cycle,flush=True)
            key(True); button(True)
            lines = show(h.collect(.4))
            require(lines,'KEY 1e down'); require(lines,'MOUSE flags=0001')
            reset_lines=[]
            for index in baseline:
                lines=command('hidreset '+str(index)); reset_lines.extend(lines)
                require(lines,'HIDRESET index=%d result=0' % index)
            require(reset_lines,'KEY 1e up'); require(reset_lines,'MOUSE flags=0002')
            key(False); button(False); show(h.collect(.2))
            key(True); button(True)
            lines=show(h.collect(.4))
            require(lines,'KEY 1e down'); require(lines,'MOUSE flags=0001')
            key(False); button(False)
            lines=show(h.collect(.4))
            require(lines,'KEY 1e up'); require(lines,'MOUSE flags=0002')
            assert identities(command('ls')) == baseline
            require(command('vpat 0 11 64 17'), 'VERIFY bad=0')
        h.qmp_cmd('device_del',id='hub'); show(h.collect(3))
        require(command('ls'),'KBDS 0 MICE 0')
        require(command('vpat 0 11 64 17'),'VERIFY bad=0')
    finally:
        h.close()
print('RESULT hid-reset',hc,'PASS: 20 physical resets, held-input release, identity, storage sibling, populated-hub removal',flush=True)
