"""PCIe hot plug of USB host controllers (what a Thunderbolt / USB4 dock does)
on the bare metal harness: q35, a hot plug capable pcie-root-port with a
firmware reservation; QMP device_add puts an xHCI (and a USB keyboard and
stick on it) behind the port while the driver runs, device_del takes it away,
and again.  Then a dock-like tree: a PCIe-PCI bridge with an xHCI behind it
(bus numbers and a bridge window to assign).

  python3 thotpci.py
"""
import os, time
from qharn import Harness, disk

os.makedirs("/tmp/usbt", exist_ok=True)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def wait_log(h, text, secs=10):
    end = time.time() + secs
    while time.time() < end:
        h.cmd("work")
        if text in h.all:
            return True
        time.sleep(0.3)
    return False


img = "/tmp/usbt/hp_stick.img"
disk(img, 16)
h = Harness(["-global", "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off",
             "-device", "pcie-root-port,id=rp1,chassis=1,slot=1,bus-reserve=4,mem-reserve=16M",
             "-device", "pcie-root-port,id=rp2,chassis=2,slot=2,bus-reserve=6,mem-reserve=32M",
             "-drive", "if=none,id=st,format=raw,file=" + img],
            log="/tmp/usbt/hotpci.log", machine="q35")
try:
    boot = h.wait("harness: ready", 90)
    h.wait("READY")
    for l in boot:
        if "hot plug" in l:
            print("  " + l)
    chk(any("2 PCIe hot plug port(s) watched" in l for l in boot), "hot plug ports found")
    print("  ", h.qmp_cmd("device_add", driver="qemu-xhci", bus="rp1", id="hx"))
    chk(wait_log(h, "started (polled)"), "hot added xHCI started")
    for l in h.all.splitlines():
        if "hotplug" in l:
            print("  " + l)
    h.qmp_cmd("device_add", driver="usb-kbd", bus="hx.0", id="kb")
    h.qmp_cmd("device_add", driver="usb-storage", bus="hx.0", drive="st", id="ms")
    chk(wait_log(h, "keyboard", 8), "keyboard on the hot added controller")
    time.sleep(1)
    h.hmp("sendkey q")
    ks = [l for l in h.collect(1.5) if l.startswith("KEY")]
    chk("KEY 10 down" in ks, "keyboard types %s" % ks[:2])
    slot = None
    for i in range(4):
        r = h.cmd("cap %d" % i)
        if any("CAP srb=01" in l for l in r):
            slot = i
    chk(slot is not None, "stick on the hot added controller")
    if slot is not None:
        r = h.cmd("wpat %d 20 8 4" % slot) + h.cmd("vpat %d 20 8 4" % slot)
        chk(any("VERIFY bad=0" in l for l in r), "stick read / write")
    # unplug
    h.qmp_cmd("device_del", id="hx")
    chk(wait_log(h, "is gone", 15), "unplug noticed")
    time.sleep(2)
    chk("hx" not in str(h.qmp_cmd("qom-list", path="/machine/peripheral")), "QEMU finished the unplug")
    time.sleep(1)
    r = h.cmd("cap %d" % slot) if slot is not None else []
    chk(not any("CAP srb=01" in l for l in r), "stick gone with the controller")
    # and again
    n = h.all.count("started (polled)")
    h.qmp_cmd("device_add", driver="qemu-xhci", bus="rp1", id="hx2")
    end = time.time() + 12
    while time.time() < end and h.all.count("started (polled)") == n:
        h.cmd("work")
        time.sleep(0.3)
    chk(h.all.count("started (polled)") > n, "plugged again")
    # dock-like: a bridge with an xHCI behind it, added while running
    try:
        h.qmp_cmd("device_add", driver="pcie-pci-bridge", bus="rp2", id="br")
        h.qmp_cmd("device_add", driver="qemu-xhci", bus="br", addr="1", id="hx3")
        n = h.all.count("started (polled)")
        end = time.time() + 15
        while time.time() < end and h.all.count("started (polled)") == n:
            h.cmd("work")
            time.sleep(0.3)
        chk(h.all.count("started (polled)") > n, "xHCI behind a hot added bridge (bus number, window)")
        h.qmp_cmd("device_add", driver="usb-kbd", bus="hx3.0", id="kb3")
        chk(wait_log(h, "device", 8), "device on it")
    except RuntimeError as e:
        print("  bridge hot plug not possible here:", e)
    for l in h.all.splitlines():
        if "hotplug" in l or "removed" in l:
            print("  " + l)
finally:
    h.close()
print("RESULT hotpci %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
