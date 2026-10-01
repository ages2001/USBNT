import os, sys
from qharn import Harness, disk
os.makedirs("/tmp/usbt", exist_ok=True)
CFG = {
 "xhci": ["-device","qemu-xhci,id=hc"],
 "ehci": ["-device","usb-ehci,id=hc"],
 "uhci": ["-device","piix3-usb-uhci,id=hc"],
 "ohci": ["-device","pci-ohci,id=hc"],
}
name = sys.argv[1]
extra = sys.argv[2:] 
disk("/tmp/usbt/b_%s.img" % name, 64)
args = CFG[name] + ["-drive","if=none,id=d1,format=raw,file=/tmp/usbt/b_%s.img" % name,
        "-device","usb-storage,bus=hc.0,drive=d1",
        "-device","usb-kbd,bus=hc.0", "-device","usb-mouse,bus=hc.0"] + extra
h = Harness(args, log="/tmp/usbt/b_%s.log" % name)
ok = True
fails=[]
def chk(c, what):
    global ok
    if not c:
        ok=False; fails.append(what)
def show(ls):
    for l in ls: print(l)
    return ls
try:
    show(h.wait("harness: ready", 90)); h.wait("READY")
    show(h.cmd("ls"))
    show(h.cmd("tur 0")); r = show(h.cmd("tur 0")); chk(any("srb=01" in l for l in r),"tur")
    show(h.cmd("cap 0"))
    r = show(h.cmd("wpat 0 1000 128 3")); chk(any("srb=01" in l and "xfer=65536" in l for l in r),"write")
    r = show(h.cmd("vpat 0 1000 128 3")); chk(any("VERIFY bad=0" in l for l in r),"verify")
    r = show(h.cmd("wpat 0 5 1 4")); r = show(h.cmd("vpat 0 5 1 4")); chk(any("VERIFY bad=0" in l for l in r),"verify1")
    r = show(h.cmd("read6 0 1000 3")) 
    r = show(h.cmd("inq 0"))
    h.hmp("sendkey ctrl-alt-delete"); r = show(h.collect(1.5)); chk(sum("KEY" in l for l in r) == 6,"cad")
    h.hmp("sendkey kp_divide"); r = show(h.collect(1.0)); chk(any("KEY 35 down e0" in l for l in r),"kpdiv")
    h.hmp("mouse_move 5 7"); h.hmp("mouse_move 0 0 -1"); h.hmp("mouse_button 2"); h.hmp("mouse_button 0")
    r = show(h.collect(1.5)); chk(any("wheel=" in l and "flags=0400" in l for l in r) and any("flags=0004" in l for l in r),"mouse")
    show(h.cmd("leds 6"))
finally:
    h.close()
print("RESULT", name, "PASS" if ok else "FAIL", fails)
