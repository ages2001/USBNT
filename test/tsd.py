"""SD card reader test on the bare metal harness: QEMU's sdhci-pci host with an
sd-card (SD v2, SDHC and SDSC sizes), seen through the virtual disk slot;
also with a USB stick at the same time, and card removal / insertion.

  python3 tsd.py [sdhc|sdsc] [spec=2|3]
"""
import os, sys, time
from qharn import Harness, disk
os.makedirs("/tmp/usbt", exist_ok=True)
kind = sys.argv[1] if len(sys.argv) > 1 else "sdhc"
spec = [a.split("=")[1] for a in sys.argv[2:] if a.startswith("spec=")]
size = 64 if kind == "sdhc" else 32                   # MB; QEMU makes cards over 1 GB SDHC: use the
img = "/tmp/usbt/sd_%s.img" % kind                    # sd-card's own rule (size > 1 GB) for SDHC
if kind == "sdhc":
    size = 4096
disk(img, size)
usbimg = "/tmp/usbt/sd_usb.img"
disk(usbimg, 16)
args = ["-device", "sdhci-pci,id=sdh%s" % ((",sd-spec-version=" + spec[0]) if spec else ""),
        "-drive", "if=none,id=sd0,format=raw,file=" + img, "-device", "sd-card,drive=sd0,id=card,bus=sd-bus",
        "-device", "qemu-xhci,id=hc", "-drive", "if=none,id=u1,format=raw,file=" + usbimg,
        "-device", "usb-storage,bus=hc.0,drive=u1"]
h = Harness(args, log="/tmp/usbt/sd_%s.log" % kind)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def run(c, want=None, t=120):
    r = h.cmd(c, t)
    for l in r:
        print("  " + l)
    if want:
        chk(any(want in l for l in r), c)
    return r


def find_slot():
    for l in h.cmd("ls"):
        pass
    slots = {}
    for i in range(4):
        r = h.cmd("inq %d" % i)
        if any("srb=01" in l for l in r):
            slots[i] = r
    return slots


try:
    print("\n".join(h.wait("harness: ready", 90)))
    h.wait("READY")
    run("work")
    run("ls")
    log = h.all
    chk("sdhci" in log.lower(), "SDHCI host found")
    sd = None
    for i in range(4):
        r = h.cmd("cap %d" % i)
        for l in r:
            if l.startswith("CAPDATA"):
                last = int(l.split()[1].split("=")[1])
                print("  slot %d: %d blocks" % (i, last + 1))
                if abs((last + 1) * 512 - size * 1024 * 1024) < 4 * 1024 * 1024 and sd is None:
                    sd = i
    chk(sd is not None, "SD card capacity (%d MB)" % size)
    if sd is not None:
        run("inq %d" % sd, "srb=01")
        for lba, cnt, seed in ((0, 1, 3), (100, 8, 5), (2047, 128, 9), (size * 2048 - 64, 64, 11)):
            run("wpat %d %d %d %d" % (sd, lba, cnt, seed), "srb=01")
            run("vpat %d %d %d %d" % (sd, lba, cnt, seed), "VERIFY bad=0")
        run("bigw %d 5000 1000 21 0 1" % sd, "srb=01")
        run("bigv %d 5000 1000 21 512 1" % sd, "VERIFY bad=0")
        # the data really is on the card image
        h.cmd("work")
        time.sleep(0.5)
        with open(img, "rb") as f:
            f.seek(100 * 512)
            b = f.read(512)
        chk(b[0] == (100 * 7 + 0 + 5) & 0xFF and b[1] == (100 * 7 + 1 + 5) & 0xFF, "written data in the card image")
        # card removed: the slot reports no medium; inserted again: readable
        print("  eject:", h.hmp("eject -f sd0"))
        time.sleep(1)
        for _ in range(5):
            h.cmd("work")
            time.sleep(0.3)
        r = run("tur %d" % sd)
        chk(any("srb=01" not in l and "TUR" in l for l in r), "no medium after removal")
        print("  change:", h.hmp("change sd0 %s raw" % img))
        time.sleep(1)
        for _ in range(8):
            h.cmd("work")
            time.sleep(0.3)
        run("tur %d" % sd)
        run("tur %d" % sd)
        run("vpat %d 100 8 5" % sd, "VERIFY bad=0")
    # the USB stick works next to it
    usb = [i for i in range(4) if i != sd and any("srb=01" in l for l in h.cmd("inq %d" % i))]
    chk(bool(usb), "USB stick next to the card")
    if usb:
        run("wpat %d 10 4 1" % usb[0], "srb=01")
        run("vpat %d 10 4 1" % usb[0], "VERIFY bad=0")
finally:
    h.close()
print("RESULT sd %s%s %s" % (kind, (" spec=" + spec[0]) if spec else "", "PASS" if not fails else "FAIL: " + ", ".join(fails)))
