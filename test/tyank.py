import os, sys, time, threading
from qharn import Harness, disk
hc = sys.argv[1]
uas = len(sys.argv) > 2 and sys.argv[2] == "uas"
def stor(drive):
    if uas:
        return [("usb-uas", {"bus": "hc.0", "id": "s1", "attached": "off"}), ("scsi-hd", {"bus": "s1.0", "scsi-id": 0, "lun": 0, "drive": drive})]
    return [("usb-storage", {"bus": "hc.0", "drive": drive, "id": "s1"})]
def args(drive):
    out = []
    for drv, p in stor(drive):
        p = dict((k, v) for k, v in p.items() if k != "attached")
        out += ["-device", drv + "," + ",".join("%s=%s" % kv for kv in p.items())]
    return out
CFG = {"xhci":"qemu-xhci","ehci":"usb-ehci","uhci":"piix3-usb-uhci","ohci":"pci-ohci"}
disk("/tmp/usbt/y.img", 64)
h = Harness(["-device", CFG[hc]+",id=hc","-drive","if=none,id=d1,format=raw,file=/tmp/usbt/y.img",] + args("d1") +
            ["-device","usb-kbd,bus=hc.0"], log="/tmp/usbt/yank_%s%s.log" % (hc, "_uas" if uas else ""))
ok = True
try:
    h.wait("harness: ready", 90); h.wait("READY")
    for i in range(3):
        h.ser.sendall(b"bigw 0 0 2048 1 0 1\n" if uas else b"wpat 0 0 128 1\n")
        time.sleep(0.05 + 0.1 * i)
        h.qmp_cmd("device_del", id="s1")
        r = h.wait("READY", 60)
        print(i, [l for l in r if "WRITE" in l or "BIGW" in l])
        time.sleep(1.5)
        h.hmp("drive_add 0 if=none,id=d%d,format=raw,file=/tmp/usbt/y.img" % (i + 10))
        for drv, p in stor("d%d" % (i + 10)):
            p = dict(p)
            if p.get("attached") == "off":
                p["attached"] = False
            h.qmp_cmd("device_add", driver=drv, **p)
        if uas:
            h.qmp_cmd("qom-set", path="/machine/peripheral/s1", property="attached", value=True)
        time.sleep(3)
        r = h.cmd("tur 0"); r = h.cmd("tur 0"); print(r)
        ok &= any("srb=01" in l for l in r)
    h.hmp("sendkey x"); r = h.collect(1); ok &= any("KEY 2d" in l for l in r)
finally:
    h.close()
print("RESULT yank", hc, "uas" if uas else "", "PASS" if ok else "FAIL")
