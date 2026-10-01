import os, sys, time
from qharn import Harness, disk
os.makedirs("/tmp/usbt", exist_ok=True)
hc = sys.argv[1] if len(sys.argv) > 1 else "qemu-xhci"
disk("/tmp/usbt/h1.img"); disk("/tmp/usbt/h2.img", 32)
h = Harness(["-device", hc + ",id=hc",
             "-device","usb-hub,bus=hc.0,port=1,id=hub1",
             "-drive","if=none,id=d1,format=raw,file=/tmp/usbt/h1.img",
             "-drive","if=none,id=d2,format=raw,file=/tmp/usbt/h2.img",
             "-device","usb-kbd,bus=hc.0,port=1.2,id=k1"], log="/tmp/usbt/hot.log")
def show(ls):
    for l in ls: print(l)
    return ls
try:
    show(h.wait("harness: ready", 60)); h.wait("READY")
    show(h.cmd("ls"))
    print("== plug storage behind hub")
    h.qmp_cmd("device_add", driver="usb-storage", bus="hc.0", port="1.3", drive="d1", id="s1")
    show(h.collect(3))
    show(h.cmd("ls"))
    show(h.cmd("tur 0")); show(h.cmd("tur 0"))
    show(h.cmd("wpat 0 7 64 9")); show(h.cmd("vpat 0 7 64 9"))
    print("== keyboard behind hub")
    h.hmp("sendkey shift-b"); show(h.collect(1))
    print("== unplug storage")
    h.qmp_cmd("device_del", id="s1")
    show(h.collect(3))
    show(h.cmd("tur 0"))
    print("== plug second disk on root port")
    h.qmp_cmd("device_add", driver="usb-storage", bus="hc.0", port="2", drive="d2", id="s2")
    show(h.collect(3))
    show(h.cmd("ls"))
    show(h.cmd("tur 1")); show(h.cmd("cap 0")); show(h.cmd("cap 1"))
    print("== replug first disk")
    h.hmp("drive_add 0 if=none,id=d1b,format=raw,file=/tmp/usbt/h1.img")
    h.qmp_cmd("device_add", driver="usb-storage", bus="hc.0", port="1.3", drive="d1b", id="s1b")
    show(h.collect(3))
    show(h.cmd("ls"))
    show(h.cmd("tur 1")); r = show(h.cmd("vpat 1 7 64 9"))
    ok = any("VERIFY bad=0" in l for l in r)
    print("RESULT hotplug", "PASS" if ok else "FAIL")
    print("== unplug hub with devices")
    h.qmp_cmd("device_del", id="hub1")
    show(h.collect(3))
    show(h.cmd("ls"))
finally:
    h.close()
