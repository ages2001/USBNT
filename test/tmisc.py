import os, sys, time
from qharn import Harness, disk
os.makedirs("/tmp/usbt", exist_ok=True)
import mkimg
mkimg.make()
disk("/tmp/usbt/l0.img", 16); disk("/tmp/usbt/l1.img", 24)
fails = []
def show(ls):
    for l in ls: print(l)
    return ls
h = Harness(["-device","qemu-xhci,id=hc",
  "-drive","if=none,id=d0,format=raw,file=/tmp/usbt/l0.img","-drive","if=none,id=d1,format=raw,file=/tmp/usbt/l1.img",
  "-device","usb-bot,bus=hc.0,id=bot","-device","scsi-hd,bus=bot.0,scsi-id=0,lun=0,drive=d0","-device","scsi-hd,bus=bot.0,scsi-id=0,lun=1,drive=d1",
  "-drive","if=none,id=cd,media=cdrom,format=raw,file=/tmp/usbt/cd.iso","-device","usb-storage,bus=hc.0,drive=cd",
  "-device","usb-tablet,bus=hc.0"], log="/tmp/usbt/misc.log")
try:
    show(h.wait("harness: ready", 90)); h.wait("READY")
    r = show(h.cmd("ls"))
    r = show(h.cmd("cap 0")); fails += [] if any("last=32767" in l for l in r) else ["lun0"]
    r = show(h.cmd("cap 1")); fails += [] if any("last=49151" in l for l in r) else ["lun1"]
    show(h.cmd("wpat 1 10 8 7")); r = show(h.cmd("vpat 1 10 8 7")); fails += [] if any("bad=0" in l for l in r) else ["lun1rw"]
    r = show(h.cmd("inq 2")); fails += [] if any("type=05" in l for l in r) else ["cdtype"]
    r = show(h.cmd("tur 2")); r = show(h.cmd("cap 2")); fails += [] if any("bs=2048" in l for l in r) else ["cdcap"]
    r = show(h.cmd("ms6 2")); fails += [] if any("MS6 srb=01" in l for l in r) else ["cdms6"]
    h.qmp_cmd("input-send-event", events=[{"type":"abs","data":{"axis":"x","value":16384}},{"type":"abs","data":{"axis":"y","value":8192}}])
    h.qmp_cmd("input-send-event", events=[{"type":"btn","data":{"down":True,"button":"left"}}])
    h.qmp_cmd("input-send-event", events=[{"type":"btn","data":{"down":False,"button":"left"}}])
    r = show(h.collect(1.5))
    fails += [] if any("abs=1" in l for l in r) else ["tablet"]
finally:
    h.close()
print("RESULT misc", "PASS" if not fails else "FAIL", fails)
