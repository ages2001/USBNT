import os, sys, time
from qharn import Harness, disk
os.makedirs("/tmp/usbt", exist_ok=True)
kind = sys.argv[1] if len(sys.argv) > 1 else "uhci"
disk("/tmp/usbt/c1.img"); disk("/tmp/usbt/c2.img", 32)
if kind == "uhci":
    hcs = ["-device","ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on",
           "-device","ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on",
           "-device","ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1,multifunction=on",
           "-device","ich9-usb-uhci3,masterbus=ehci.0,firstport=4,addr=1d.2,multifunction=on"]
else:
    hcs = ["-device","ich9-usb-ehci1,id=ehci,addr=5.1",
           "-device","pci-ohci,masterbus=ehci.0,firstport=0,num-ports=6,addr=5.0,multifunction=on"]
h = Harness(hcs + ["-drive","if=none,id=d1,format=raw,file=/tmp/usbt/c1.img",
             "-drive","if=none,id=d2,format=raw,file=/tmp/usbt/c2.img",
             "-device","usb-storage,bus=ehci.0,port=1,drive=d1",
             "-device","usb-kbd,bus=ehci.0,port=2,usb_version=1",
             "-device","usb-mouse,bus=ehci.0,port=3,usb_version=1",
             "-device","usb-hub,bus=ehci.0,port=4,id=hub"], log="/tmp/usbt/comp_%s.log" % kind)
fails = []
def show(ls):
    for l in ls: print(l)
    return ls
try:
    show(h.wait("harness: ready", 90)); h.wait("READY")
    r = show(h.cmd("ls"))
    if not any("KBDS 1 MICE 1" in l for l in r): fails.append("hid")
    r = show(h.cmd("tur 0")); r = show(h.cmd("tur 0"))
    if not any("srb=01" in l for l in r): fails.append("tur")
    r = show(h.cmd("wpat 0 50 128 1")); r = show(h.cmd("vpat 0 50 128 1"))
    if not any("VERIFY bad=0" in l for l in r): fails.append("hs-disk")
    h.hmp("sendkey q"); r = show(h.collect(1))
    if not any("KEY 10 down" in l for l in r): fails.append("kbd")
    h.hmp("mouse_move 1 1 1"); r = show(h.collect(1))
    if not any("wheel=" in l for l in r): fails.append("mouse")
    print("== FS storage behind FS hub (companion)")
    h.qmp_cmd("device_add", driver="usb-storage", bus="ehci.0", port="4.1", drive="d2", id="s2")
    r = show(h.collect(4)); show(h.cmd("ls"))
    r = show(h.cmd("tur 1")); r = show(h.cmd("tur 1"))
    r = show(h.cmd("wpat 1 3 16 2")); r = show(h.cmd("vpat 1 3 16 2"))
    if not any("VERIFY bad=0" in l for l in r): fails.append("fs-disk")
    print("== unplug kbd, replug as high speed")
    h.qmp_cmd("device_del", id="hub")
    r = show(h.collect(3))
    show(h.cmd("ls"))
finally:
    h.close()
print("RESULT companion", kind, "PASS" if not fails else "FAIL", fails)
