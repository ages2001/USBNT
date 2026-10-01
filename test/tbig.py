"""Large transfers (page lists, splitting, queued BOT) and UAS on every controller."""
import os, sys
from qharn import Harness, disk
os.makedirs("/tmp/usbt", exist_ok=True)
HCS = {"xhci": "qemu-xhci", "ehci": "usb-ehci", "uhci": "piix3-usb-uhci", "ohci": "pci-ohci"}
name = sys.argv[1]
uas = len(sys.argv) > 2 and sys.argv[2] == "uas"
img = "/tmp/usbt/big_%s.img" % name
disk(img, 64)
if uas:
    dev = ["-device", "usb-uas,id=uas,bus=hc.0", "-device", "scsi-hd,bus=uas.0,scsi-id=0,lun=0,drive=d1"]
else:
    dev = ["-device", "usb-storage,bus=hc.0,drive=d1"]
h = Harness(["-device", HCS[name] + ",id=hc", "-drive", "if=none,id=d1,format=raw,file=" + img] + dev,
            log="/tmp/usbt/big_%s%s.log" % (name, "_uas" if uas else ""))
fails = []
def chk(c, what):
    if not c:
        fails.append(what)
def run(c, want):
    r = h.cmd(c, 120)
    for l in r:
        print(l)
    chk(any(want in l for l in r), c)
    return r
try:
    h.wait("harness: ready", 90); h.wait("READY")
    for l in h.cmd("ls"):
        print(l)
    small = name in ("uhci", "ohci")
    n = 600 if small else 2048
    for sg in (0, 1):
        for off in (0, 512, 4094):
            seed = sg * 10 + off % 7
            run("bigw 0 100 %d %d %d %d" % (n, seed, off, sg), "srb=01")
            run("bigv 0 100 %d %d %d %d" % (n, seed, off, sg), "VERIFY bad=0")
    if not small:
        run("bigw 0 9000 4000 77 0 1", "srb=01")
        run("bigv 0 9000 4000 77 4096 1", "VERIFY bad=0")
        run("bigw 0 20000 512 5 0 1", "srb=01")
        run("bigw 0 20512 512 5 0 1", "srb=01")
        r = run("qv 0 20000 256 4 5", "QV fails=0")
    run("bigv 0 100 8 %d 0 0" % (10 + 4094 % 7), "srb=01")
finally:
    h.close()
print("RESULT big", name, "uas" if uas else "bot", "PASS" if not fails else "FAIL", fails)
