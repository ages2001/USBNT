"""Native MSC physical reset with retained data/slots and a keyboard sibling.
The production BOT/UAS transports and HCDs run in the bare-metal test kernel.
"""
import re,sys,tempfile
from pathlib import Path
from qharn import Harness,disk
hc=sys.argv[1] if len(sys.argv)>1 else 'qemu-xhci'
uas=len(sys.argv)>2 and sys.argv[2]=='uas'
assert hc in ('qemu-xhci','usb-ehci','piix3-usb-uhci','pci-ohci')
def show(lines):
 for l in lines:print(l,flush=True)
 return lines
def require(lines,text):assert any(text in l for l in lines),(text,lines)
with tempfile.TemporaryDirectory(prefix='usbnt-msc-reset-') as td:
 backing=str(Path(td)/'disk.img');disk(backing,32)
 hub='usb-hub,bus=hc.0,port=1,id=hub'+(',hs=on' if hc=='usb-ehci' else '')
 storage=['-device','usb-uas,id=storage,bus=hc.0,port=2','-device','scsi-hd,bus=storage.0,scsi-id=0,lun=0,drive=d'] if uas else ['-device','usb-storage,id=storage,bus=hc.0,port=2,drive=d']
 h=Harness(['-device',hc+',id=hc','-device',hub,'-device','usb-kbd,bus=hc.0,port=1.2,id=kbd','-drive','if=none,id=d,format=raw,file='+backing]+storage)
 def cmd(s):return show(h.cmd(s,120))
 def keyboard():
  h.hmp('sendkey shift-b');lines=show(h.collect(.4));require(lines,'KEY 30 down');require(lines,'KEY 30 up')
 def devices(lines):return [l for l in lines if l.startswith('DEV ') or l.startswith('SLOT ')]
 try:
  boot=show(h.wait('harness: ready',90));h.wait('READY')
  if uas:assert any('UAS, ' in l for l in boot),boot
  ls=cmd('ls');baseline=devices(ls);storage=[re.search(r'DEV (\d+) .*msc=1',l) for l in ls];storage=[m for m in storage if m];assert len(storage)==1
  index=int(storage[0][1]);cmd('tur 0');require(cmd('tur 0'),'TUR srb=01')
  require(cmd('wpat 0 7 64 9'),'WRITE srb=01 scsi=00 xfer=32768')
  for c in range(5):
   print('CYCLE',c,flush=True)
   require(cmd('bigw 0 20000 512 5 0 1'),'srb=01')
   require(cmd('bigw 0 20512 512 5 0 1'),'srb=01')
   require(cmd('mscreset '+str(index)),'MSCRESET index=%d result=0'%index)
   assert devices(cmd('ls'))==baseline
   cmd('tur 0');require(cmd('tur 0'),'TUR srb=01')
   require(cmd('vpat 0 7 64 9'),'VERIFY bad=0')
   require(cmd('qv 0 20000 256 4 5'),'QV fails=0')
   keyboard()
  h.qmp_cmd('device_del',id='storage');show(h.collect(2))
  require(cmd('tur 0'),'TUR srb=84');keyboard()
 finally:h.close()
print('RESULT msc-reset',hc,'uas' if uas else 'bot','PASS: five physical resets; stable slots/identity; retained data; queued reads; removal and keyboard continuity',flush=True)
