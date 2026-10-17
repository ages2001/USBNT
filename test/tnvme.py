"""NVMe drives that arrive later (Thunderbolt / USB4 enclosure, PCIe hot plug)
on the bare metal harness: q35 with hot plug capable pcie-root-ports; QMP
device_add puts an NVMe controller behind a port while the driver runs, the
drive gets a disk slot and is written and read back (small commands and large
ones that are split by MDTS), device_del (attention button) shuts it down and
takes it away, and again; a controller with two namespaces (512 and 4096 byte
blocks); a drive present at boot is left alone (nvme2k drives those).

  python3 tnvme.py
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
        time.sleep(0.3)
    return False


def find_slot(h, bs=512):
    for i in range(4):
        r = h.cmd("cap %d" % i)
        if any("CAP srb=01" in l for l in r) and any("bs=%d" % bs in l for l in r):
            return i
    return None


a, b, c4, boot = "/tmp/usbt/nv_a.img", "/tmp/usbt/nv_b.img", "/tmp/usbt/nv_c4.img", "/tmp/usbt/nv_boot.img"
for f, mb in ((a, 64), (b, 32), (c4, 32), (boot, 16)):
    disk(f, mb)
h = Harness(["-global", "ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off",
             "-device", "pcie-root-port,id=rp1,chassis=1,slot=1,bus-reserve=4,mem-reserve=16M",
             "-device", "pcie-root-port,id=rp2,chassis=2,slot=2,bus-reserve=4,mem-reserve=16M",
             "-device", "pcie-root-port,id=rp3,chassis=3,slot=3",
             # a drive in the machine from the start: nvme2k's
             "-drive", "if=none,id=nboot,format=raw,file=" + boot,
             "-device", "nvme,id=nvboot,serial=BOOTDRIVE,drive=nboot,bus=rp3",
             "-drive", "if=none,id=na,format=raw,file=" + a,
             "-drive", "if=none,id=nb,format=raw,file=" + b,
             "-drive", "if=none,id=nc,format=raw,file=" + c4],
            log="/tmp/usbt/nvme.log", machine="q35")
try:
    boot_lines = h.wait("harness: ready", 90)
    h.wait("READY")
    chk(not any("BOOTDRIVE" in l for l in boot_lines) and find_slot(h) is None,
        "drive present at boot left alone")
    # an enclosure arrives (MDTS 5 = 128 KB per command; 256 blocks are split in two)
    print("  ", h.qmp_cmd("device_add", driver="nvme", bus="rp1", id="nv1", serial="ENCL0001", drive="na", mdts=5))
    chk(wait_log(h, "namespace 1: 64 MB", 15), "hot added NVMe started")
    for l in h.all.splitlines():
        if "nvme" in l or "hotplug" in l:
            print("  " + l)
    slot = find_slot(h)
    chk(slot is not None, "NVMe drive in a disk slot")
    if slot is not None:
        r = h.cmd("cap %d" % slot)
        chk(any("last=131071 bs=512" in l for l in r), "capacity 64 MB")
        r = h.cmd("wpat %d 100 64 3" % slot) + h.cmd("vpat %d 100 64 3" % slot)
        chk(any("VERIFY bad=0" in l for l in r), "write / read 64 blocks")
        r = h.cmd("wpat %d 131000 71 9" % slot) + h.cmd("vpat %d 131000 71 9" % slot)
        chk(any("VERIFY bad=0" in l for l in r), "write / read at the end")
        r = h.cmd("read %d 131071 2 0" % slot)
        chk(any("sense=5/21" in l for l in r), "read past the end refused")
        r = h.cmd("bigw %d 2000 2048 5 0 1" % slot) + h.cmd("bigv %d 2000 2048 5 0 1" % slot)
        print("  ", [l for l in r if "BIG" in l])
        chk(any("bad=0" in l for l in r), "1 MB commands (split by MDTS)")
    # the drive must keep the data: compare with the image file on the host
    h.cmd("work")
    data = open(a, "rb").read()
    chk(data[100 * 512] == (100 * 7 + 0 + 3) & 0xFF and data[100 * 512 + 5] == (100 * 7 + 5 + 3) & 0xFF,
        "data on the medium")
    # unplug: attention button -> orderly shutdown, then QEMU removes it
    t0 = time.time()
    h.qmp_cmd("device_del", id="nv1")
    chk(wait_log(h, "shut down", 30), "unplug: button, 5 s, shutdown (%.0f s)" % (time.time() - t0))
    time.sleep(4)
    chk("nv1" not in str(h.qmp_cmd("qom-list", path="/machine/peripheral")), "QEMU finished the unplug")
    chk(find_slot(h) is None, "slot gone with the drive")
    # again, another drive in the same port
    h.qmp_cmd("device_add", driver="nvme", bus="rp1", id="nv2", serial="ENCL0002", drive="nb")
    chk(wait_log(h, "namespace 1: 32 MB", 15), "plugged again (another drive)")
    slot = find_slot(h)
    if slot is not None:
        r = h.cmd("wpat %d 7 16 1" % slot) + h.cmd("vpat %d 7 16 1" % slot)
        chk(any("VERIFY bad=0" in l for l in r), "second drive read / write")
        # safe removal: the drive is shut down, the slot reports no medium
        r = h.cmd("eject %d" % slot)
        chk(wait_log(h, "shut down for removal", 5), "safe removal shuts the drive down")
        r = h.cmd("tur %d" % slot)
        chk(any("sense=2/3a" in l.lower() for l in r), "no medium after safe removal %s" % r[-1:])
    else:
        chk(False, "second drive in a slot")
    # a 4 KB block drive in the second port
    h.qmp_cmd("device_add", driver="nvme", bus="rp2", id="nv3", serial="ENCL4K01", drive="nc",
              logical_block_size=4096, physical_block_size=4096)
    chk(wait_log(h, "4096 byte blocks", 15), "4 KB block drive started")
    chk(find_slot(h, 4096) is not None, "4 KB block drive in a slot (bs=4096)")
finally:
    h.close()
print("RESULT nvme %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
