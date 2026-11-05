"""Restart the same controller with live storage and HID, preserving media data.

The bare-metal allocator does not recycle OsAlloc blocks; leak/double-free
checks are in the ASan host harness. This tests real QEMU DMA/list reuse.
"""
import sys,tempfile
from pathlib import Path
from qharn import Harness,disk
hc=sys.argv[1];assert hc in ('qemu-xhci','usb-ehci','piix3-usb-uhci','pci-ohci')
cycles=int(sys.argv[2]) if len(sys.argv)>2 else 10
controllers=int(sys.argv[3]) if len(sys.argv)>3 else 1
assert 1 <= controllers <= 16
with tempfile.TemporaryDirectory(prefix='usbnt-hcrestart-') as temp:
 media=str(Path(temp)/'disk.img');disk(media,32)
 extras=[]
 for index in range(1,controllers):extras += ['-device',hc+',id=extra'+str(index)]
 h=Harness(['-device',hc+',id=hc','-drive','if=none,id=d1,format=raw,file='+media,
            '-device','usb-storage,bus=hc.0,drive=d1',
            '-device','usb-kbd,bus=hc.0','-device','usb-mouse,bus=hc.0']+extras)
 def cmd(line):
  result=h.cmd(line)
  for text in result:print(text,flush=True)
  return result
 def need(lines,text):assert any(text in l for l in lines),(text,lines)
 try:
  h.wait('harness: ready',90);h.wait('READY');cmd('tur 0');need(cmd('tur 0'),'TUR srb=01')
  need(cmd('wpat 0 32 64 23'),'WRITE srb=01 scsi=00 xfer=32768')
  for cycle in range(cycles):
   need(cmd('hcreset 0'),'HCRESET index=0 result=0 count='+str(controllers))
   need(cmd('ls'),'KBDS 1 MICE 1');cmd('tur 0');need(cmd('tur 0'),'TUR srb=01')
   need(cmd('vpat 0 32 64 23'),'VERIFY bad=0')
   h.hmp('sendkey a');lines=h.collect(.4);need(lines,'KEY 1e down');need(lines,'KEY 1e up')
   h.hmp('mouse_move 5 7');need(h.collect(.3),'MOUSE ')
   print('PASS controller restart',cycle+1,flush=True)
 finally:h.close()
print('RESULT controller-restart',hc,'PASS',cycles,flush=True)
