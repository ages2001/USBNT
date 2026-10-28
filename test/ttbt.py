"""Thunderbolt 1 to 5 and USB4 (tbt.c) on the bare metal harness with the
usb4-nhi QEMU model (test/qemu/usbnt-qemu-test.patch).

USB4 host without firmware connection manager: the driver enumerates the
routers that are plugged in (and one that is there from the start), sets up
the PCIe and USB 3 tunnels, a chained second router, a Thunderbolt 3 device
router, unplugging; an NVMe drive arrives through the tunnel (PCIe hot plug)
and is written and read.  Thunderbolt 5 / USB4 v2: Gen 4 link, CM version 2,
PCIe extended encapsulation, a USB4 v1 device on it.  Thunderbolt 1 / 2 hosts
of Macs (driver as connection manager), of PCs (the BIOS runs them) and Alpine
Ridge in USB only mode.  Intel firmware connection manager: Falcon / Alpine /
Titan Ridge message formats, security levels none / user / secure (new key,
challenge with the stored key, wrong key, device that lost its key), approval
switched off, Ice Lake force power and Tiger Lake.

The model counts protocol errors (bad CRC, wrong paths, missing acks ...);
every run must end with none.

  python3 ttbt.py
"""
import os, time
from qharn import Harness, disk

os.makedirs("/tmp/usbt", exist_ok=True)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def wait_log(h, text, secs=10, count=1):
    end = time.time() + secs
    while time.time() < end:
        h.cmd("work")
        if h.all.count(text) >= count:
            return True
        time.sleep(0.2)
    return False


def qget(h, prop):
    return h.qmp_cmd("qom-get", path="/machine/peripheral/tb", property=prop)


def qset(h, prop, val):
    return h.qmp_cmd("qom-set", path="/machine/peripheral/tb", property=prop, value=val)


def wait_prop(h, prop, val, secs=8):
    end = time.time() + secs
    while time.time() < end:
        h.cmd("work")
        if qget(h, prop) == val:
            return True
        time.sleep(0.2)
    return False


def clean(h, what):
    v = qget(h, "violations")
    chk(v == 0, "%s: no protocol errors (%s)" % (what, qget(h, "vlog") if v else "0"))


def show(h, words=("thunderbolt",)):
    for l in h.all.splitlines():
        if any(w in l for w in words):
            print("     " + l)


def find_slot(h):
    for i in range(4):
        r = h.cmd("cap %d" % i)
        if any("CAP srb=01" in l for l in r):
            return i
    return None


def boot(nhi, extra=(), append=None, log="tbt"):
    args = ["-global", "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off",
            "-device", "qemu-xhci,id=xh",
            "-device", "pcie-root-port,id=rp1,chassis=1,slot=1,bus-reserve=4,mem-reserve=16M",
            "-device", "usb4-nhi,id=tb," + nhi] + list(extra)
    if append:
        args += ["-append", append]
    h = Harness(args, log="/tmp/usbt/%s.log" % log, machine="q35")
    h.wait("harness: ready", 90)
    h.wait("READY")
    return h


# ---------------------------------------------------------------------------
print("== USB4 host, driver as connection manager")
nv = "/tmp/usbt/tb_nv.img"
disk(nv, 32)
h = boot("mode=usb4", ["-drive", "if=none,id=na,format=raw,file=" + nv], log="tbt_usb4")
try:
    chk("the driver is the connection manager" in h.all, "USB4 host router found, 2 ports")
    qset(h, "plug0", True)
    chk(wait_log(h, "PCIe tunnel 0:6 <-> 1:5", 10), "router plugged: PCIe tunnel")
    chk("USB 3 tunnel 0:8 <-> 1:7" in h.all, "USB 3 tunnel")
    chk(wait_prop(h, "pcie0", True) and qget(h, "usb3-0"), "model: both tunnels valid")
    chk(qget(h, "gbps0") == 40 and "link to router 1: 20 Gb/s x 2 = 40 Gb/s" in h.all, "lanes bonded: 40 Gb/s")
    clean(h, "enumeration")
    # an NVMe drive in the dock: PCIe hot plug behind the tunnel
    h.qmp_cmd("device_add", driver="nvme", bus="rp1", id="nv1", serial="DOCK0001", drive="na")
    chk(wait_log(h, "namespace 1: 32 MB", 15), "NVMe behind the tunnel started")
    slot = find_slot(h)
    if slot is not None:
        r = h.cmd("wpat %d 10 64 7" % slot) + h.cmd("vpat %d 10 64 7" % slot)
        chk(any("VERIFY bad=0" in l for l in r), "NVMe write / read through the dock")
    else:
        chk(False, "NVMe slot")
    # a second router behind the first one
    qset(h, "plug1", True)
    chk(wait_log(h, "PCIe tunnel 1:6 <-> 301:5", 10), "chained router: PCIe tunnel")
    chk(wait_prop(h, "pcie1", True) and qget(h, "usb3-1"), "model: chained tunnels valid")
    chk(qget(h, "gbps1") == 40, "chained link bonded: 40 Gb/s")
    clean(h, "chain")
    r = h.cmd("tbt")
    print("\n".join("     " + l for l in r if l.startswith("TBT")))
    chk(any("USB4 (AMD) host router" in l and "Connection manager: the driver" in l for l in r) and
        sum("USB4 router 8086:0b26 - PCIe + USB 3" in l for l in r) == 2, "usbtree lines (host + 2 routers)")
    # unplug everything: drive first (attention button), then the cable
    h.qmp_cmd("device_del", id="nv1")
    chk(wait_log(h, "shut down", 12), "NVMe shut down before the cable goes")
    qset(h, "plug0", False)
    chk(wait_log(h, "removed", 8, 2), "both routers removed")
    chk(wait_prop(h, "host-clean", True), "host router has no tunnel left")
    clean(h, "unplug")
    # Thunderbolt 3 device on the USB4 host
    qset(h, "kind0", "tb3")
    qset(h, "plug0", True)
    chk(wait_log(h, "Thunderbolt 3 router", 10), "Thunderbolt 3 device router enumerated")
    chk(wait_prop(h, "pcie0", True), "Thunderbolt 3 device: PCIe tunnel")
    chk(qget(h, "gbps0") == 40, "Thunderbolt 3 device: lanes bonded (link controller says so)")
    clean(h, "Thunderbolt 3 device")
    qset(h, "plug0", False)
    chk(wait_log(h, "removed", 8, 3), "Thunderbolt 3 device removed")
    clean(h, "end")
    show(h, ("thunderbolt", "nvme"))
finally:
    h.close()

print("== USB4 router there from the start, Intel id without firmware CM, USB4 v2 (host router reset)")
h = boot("mode=usb4,device-id=0x7EC2,caps-version=0x40,plug0=on", log="tbt_boot")
try:
    chk("the driver is the connection manager" in h.all, "no firmware: driver takes over")
    chk(wait_log(h, "PCIe tunnel 0:6 <-> 1:5", 5), "router present at boot enumerated")
    chk(wait_prop(h, "pcie0", True), "model: tunnel valid")
    clean(h, "boot")
finally:
    h.close()

print("== USB4 host, TbtApprove=0: no PCIe tunnel, TbtLaneBonding=0")
h = boot("mode=usb4", append="TbtApprove=0 TbtLaneBonding=0", log="tbt_noapp")
try:
    qset(h, "plug0", True)
    chk(wait_log(h, "no PCIe tunnel (TbtApprove = 0)", 8), "PCIe not tunnelled")
    chk(qget(h, "usb3-0") and not qget(h, "pcie0"), "USB 3 only")
    chk(qget(h, "gbps0") == 20 and "20 Gb/s x 1 = 20 Gb/s" in h.all, "TbtLaneBonding=0: one lane, 20 Gb/s")
    clean(h, "TbtApprove=0")
finally:
    h.close()

print("== Thunderbolt 5 / USB4 v2 host (Barlow Ridge): Gen 4 link, CM version 2, PCIe extended encapsulation")
h = boot("mode=usb4,v2=on", log="tbt_v2")
try:
    chk("USB4 v2 (Thunderbolt 5) host router 8086:5781" in h.all, "USB4 v2 host router, driver is the CM")
    qset(h, "kind0", "usb4v2")
    qset(h, "plug0", True)
    chk(wait_log(h, "(extended encapsulation)", 10), "USB4 v2 router: PCIe tunnel with extended encapsulation")
    chk(wait_prop(h, "pcie0", True) and qget(h, "usb3-0") and qget(h, "ee0"), "model: tunnels valid, EE on both ends")
    chk(qget(h, "gbps0") == 80 and "40 Gb/s x 2 = 80 Gb/s (Gen 4" in h.all, "Gen 4 link: 80 Gb/s, bonded by the hardware")
    r = h.cmd("tbt")
    print("\n".join("     " + l for l in r if l.startswith("TBT")))
    chk(any("Thunderbolt 5 (Barlow Ridge 80G) host router" in l for l in r) and
        any("USB4 v2 router 8086:5786 - PCIe + USB 3" in l and "Thunderbolt 5 / USB4 v2 Gen 4" in l for l in r),
        "usbtree lines (Thunderbolt 5 host + router)")
    clean(h, "Gen 4")
    qset(h, "plug0", False)
    chk(wait_log(h, "removed", 8) and wait_prop(h, "host-clean", True), "USB4 v2 router removed, host clean")
    qset(h, "kind0", "usb4")
    qset(h, "plug0", True)
    chk(wait_log(h, "PCIe tunnel 0:6 <-> 1:5", 10, 2) and wait_prop(h, "pcie0", True), "USB4 v1 device on it: PCIe")
    chk(qget(h, "gbps0") == 40 and not qget(h, "ee0"), "v1 device: lanes bonded (40 Gb/s), no extended encapsulation")
    clean(h, "v1 device on a v2 host")
    show(h)
finally:
    h.close()

print("== Thunderbolt 1 host (Cactus Ridge) of a Mac: the driver is the connection manager")
h = boot("mode=legacy,device-id=0x1547", log="tbt_tb1")
try:
    chk("Thunderbolt 1 host router 8086:1547" in h.all and "the driver is the connection manager" in h.all,
        "Thunderbolt 1 host router started")
    qset(h, "kind0", "tb3")
    qset(h, "plug0", True)
    chk(wait_log(h, "PCIe tunnel 0:6 <-> 1:5", 10) and wait_prop(h, "pcie0", True), "Thunderbolt device: PCIe tunnel")
    chk(qget(h, "gbps0") == 20 and not qget(h, "usb3-0"), "one lane (Thunderbolt 1 does not bond), no USB 3 tunnel")
    r = h.cmd("tbt")
    print("\n".join("     " + l for l in r if l.startswith("TBT")))
    chk(any("Thunderbolt 1 (Cactus Ridge) host router" in l and "Connection manager: the driver" in l for l in r),
        "usbtree: Thunderbolt 1 host")
    qset(h, "plug0", False)
    chk(wait_log(h, "removed", 8), "removed")
    clean(h, "Thunderbolt 1")
    show(h)
finally:
    h.close()

print("== Thunderbolt 2 host (Falcon Ridge) of a Mac, firmware CM off: driver CM, lane bonding through the LC")
h = boot("mode=legacy,device-id=0x156C", log="tbt_tb2")
try:
    chk(wait_log(h, "the driver does it", 5), "no firmware connection manager: the driver does it")
    qset(h, "kind0", "tb3")
    qset(h, "plug0", True)
    chk(wait_log(h, "PCIe tunnel 0:6 <-> 1:5", 10) and wait_prop(h, "pcie0", True), "PCIe tunnel")
    chk(qget(h, "gbps0") == 40, "lanes bonded: 40 Gb/s")
    clean(h, "Thunderbolt 2")
finally:
    h.close()

print("== Thunderbolt 1 controller of a PC (BIOS assist) and Alpine Ridge in USB only mode: left alone")
h = boot("mode=legacy,device-id=0x1547,apple=off,plug0=on", log="tbt_bios")
try:
    chk(wait_log(h, "the BIOS manages it", 5), "PC: the BIOS manages it")
    r = h.cmd("tbt")
    chk(any("Connection manager: the BIOS" in l for l in r), "usbtree: the BIOS")
    clean(h, "BIOS assist")
finally:
    h.close()
h = boot("mode=ar,device-id=0x15DD", log="tbt_usbonly")
try:
    chk(wait_log(h, "USB only mode", 5), "Alpine Ridge USB only mode recognised")
    clean(h, "USB only")
finally:
    h.close()

# ---------------------------------------------------------------------------
for mode in ("fr", "ar", "tr"):
    print("== firmware connection manager, %s, security user" % mode)
    h = boot("mode=%s,security=1" % mode, log="tbt_" + mode)
    try:
        chk(wait_log(h, "firmware connection manager ready", 8), "%s: driver ready" % mode)
        qset(h, "plug0", True)
        chk(wait_log(h, "approved, PCIe tunnel up", 8), "%s: device approved" % mode)
        chk(qget(h, "approved0"), "%s: model: approved" % mode)
        chk("depth 1, 40 Gb/s" in h.all, "%s: two lanes reported by the firmware (40 Gb/s)" % mode)
        qset(h, "plug0", False)
        chk(wait_log(h, "disconnected", 8), "%s: disconnected" % mode)
        qset(h, "plug0", True)
        chk(wait_log(h, "approved, PCIe tunnel up", 8, 2), "%s: approved again (new connection id)" % mode)
        clean(h, mode)
        show(h)
    finally:
        h.close()

print("== firmware connection manager, Titan Ridge, security secure")
h = boot("mode=tr,security=2", log="tbt_sl2")
try:
    chk(wait_log(h, "security level secure", 8), "secure level reported")
    qset(h, "plug0", True)
    chk(wait_log(h, "paired (new key)", 8) and wait_log(h, "approved, PCIe tunnel up", 8), "new device: key added")
    chk("harness: stored Tbt" in h.all, "key stored")
    qset(h, "plug0", False)
    wait_log(h, "disconnected", 8)
    qset(h, "plug0", True)
    chk(wait_log(h, "key checked", 8) and wait_log(h, "approved, PCIe tunnel up", 8, 2), "known device: challenge")
    r = h.cmd("tbt")
    print("\n".join("     " + l for l in r if l.startswith("TBT")))
    chk(any("Device 8086:15ef: connected, key checked" in l and "key stored" in l for l in r) and
        any("Security level: secure" in l for l in r), "usbtree lines (firmware, device)")
    qset(h, "plug0", False)
    wait_log(h, "disconnected", 8, 2)
    qset(h, "devkey0", "bad")
    qset(h, "plug0", True)
    chk(wait_log(h, "wrong key; not connected", 8), "device with another key refused")
    chk(not qget(h, "approved0"), "model: not approved")
    qset(h, "plug0", False)
    wait_log(h, "disconnected", 8, 3)
    qset(h, "devkey0", "clear")
    qset(h, "plug0", True)
    chk(wait_log(h, "has lost its key, pairing again", 8) and wait_log(h, "approved, PCIe tunnel up", 8, 3),
        "device that lost its key pairs again")
    clean(h, "secure")
    show(h)
finally:
    h.close()

print("== firmware connection manager, Alpine Ridge, security none / TbtApprove=0")
h = boot("mode=ar,security=0", log="tbt_sl0")
try:
    wait_log(h, "firmware connection manager ready", 8)
    qset(h, "plug0", True)
    chk(wait_log(h, "Gb/s, approved", 8), "firmware approved it by itself")
    clean(h, "security none")
finally:
    h.close()
h = boot("mode=ar,security=1", append="TbtApprove=0", log="tbt_ar0")
try:
    wait_log(h, "firmware connection manager ready", 8)
    qset(h, "plug0", True)
    chk(wait_log(h, "not approved (TbtApprove = 0)", 8) and not qget(h, "approved0"), "TbtApprove=0: not approved")
    clean(h, "TbtApprove=0")
finally:
    h.close()

for mode in ("icl", "tgl"):
    print("== firmware connection manager, %s" % mode)
    h = boot("mode=%s" % mode, log="tbt_" + mode)
    try:
        chk(wait_log(h, "firmware connection manager ready", 8), "%s: firmware up (force power on Ice Lake)" % mode)
        qset(h, "plug0", True)
        chk(wait_log(h, "approved", 8), "%s: device connected by the firmware" % mode)
        clean(h, mode)
        show(h)
    finally:
        h.close()

print("\n%d failure(s)" % len(fails))
for f in fails:
    print("  " + f)
print("RESULT thunderbolt %s" % ("FAIL" if fails else "PASS"))
raise SystemExit(1 if fails else 0)
