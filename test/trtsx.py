"""Realtek card readers on the bare metal harness, with the QEMU test models:
usb-rtsx (RTS5129 "USB2.0-CRW" vendor protocol) and rtsx-pci (every PCIe chip
id the driver knows).  The SD card (sd-card) is found, written, read back,
removed and inserted again.

  python3 trtsx.py usb [pid=0x0129] [lqfp] [hc=xhci|ehci|uhci]
  python3 trtsx.py pci [did=0x5227 | all]
  python3 trtsx.py all
"""
import os, sys, time
from qharn import Harness, disk

os.makedirs("/tmp/usbt", exist_ok=True)
HC = {"uhci": "piix3-usb-uhci", "ohci": "pci-ohci", "ehci": "usb-ehci", "xhci": "qemu-xhci"}
PCI_IDS = ["0x5209", "0x5229", "0x5289", "0x5227", "0x522A", "0x5249", "0x5287", "0x5286", "0x524A", "0x525A",
           "0x5260", "0x5261", "0x5228", "0x5264"]
results = []


def one(kind, ident, extra, size_mb=64):
    fails = []

    def chk(c, what):
        if not c:
            fails.append(what)
        print("%s %s" % ("ok  " if c else "FAIL", what))

    img = "/tmp/usbt/rtsx_%s.img" % kind
    disk(img, size_mb)
    if kind == "usb":
        hc = extra.get("hc", "xhci")
        dev = ["-device", "%s,id=hc" % HC[hc],
               "-device", "usb-rtsx,id=crw,bus=hc.0,pid=%s%s" % (ident, ",lqfp=on" if extra.get("lqfp") else "")]
        bus = "crw.0"
        name = "usb %s %s%s" % (ident, hc, " lqfp" if extra.get("lqfp") else "")
    else:
        dev = ["-device", "rtsx-pci,id=rts,device-id=%s" % ident]
        bus = "rts.0"
        name = "pci %s" % ident
    dev += ["-drive", "if=none,id=card0,format=raw,file=" + img, "-device", "sd-card,drive=card0,bus=" + bus]
    h = Harness(dev, log="/tmp/usbt/rtsx_%s_%s.log" % (kind, ident))
    try:
        boot = h.wait("harness: ready", 120)
        h.wait("READY")
        for _ in range(6):
            h.cmd("work")
            time.sleep(0.3)
        for l in h.all.splitlines():
            if "rtsx" in l.lower() or "realtek" in l.lower() or "sd" in l.lower()[:12] or "card" in l.lower():
                print("  " + l)
        slot = None
        for i in range(4):
            r = h.cmd("cap %d" % i)
            if any("CAP srb=01" in l for l in r):
                last = [int(l.split()[1].split("=")[1]) for l in r if l.startswith("CAPDATA")][0]
                print("  slot %d: %d blocks" % (i, last + 1))
                if abs((last + 1) * 512 - size_mb * 1024 * 1024) < 4 * 1024 * 1024:
                    slot = i
        chk(slot is not None, "%s: card capacity" % name)
        if slot is not None:
            for lba, cnt, seed in ((0, 1, 3), (100, 8, 5), (2047, 128, 9), (size_mb * 2048 - 64, 64, 11)):
                r = h.cmd("wpat %d %d %d %d" % (slot, lba, cnt, seed)) + h.cmd("vpat %d %d %d %d" % (slot, lba, cnt, seed))
                chk(any("VERIFY bad=0" in l for l in r), "%s: write / read %d blocks at %d" % (name, cnt, lba))
            h.cmd("work")
            time.sleep(0.5)
            with open(img, "rb") as f:
                f.seek(100 * 512)
                b = f.read(2)
            chk(b[0] == (100 * 7 + 5) & 0xFF and b[1] == (100 * 7 + 1 + 5) & 0xFF, "%s: data in the card image" % name)
            print("  eject:", h.hmp("eject -f card0"))
            for _ in range(8):
                h.cmd("work")
                time.sleep(0.3)
            r = h.cmd("tur %d" % slot)
            chk(any("TUR" in l and "srb=01" not in l for l in r), "%s: no medium after removal" % name)
            print("  change:", h.hmp("change card0 %s raw" % img))
            for _ in range(10):
                h.cmd("work")
                time.sleep(0.3)
            h.cmd("tur %d" % slot)
            h.cmd("tur %d" % slot)
            r = h.cmd("vpat %d 100 8 5" % slot)
            chk(any("VERIFY bad=0" in l for l in r), "%s: card again after insertion" % name)
    except Exception as e:
        chk(False, "%s: %s" % (name, e))
    finally:
        h.close()
    results.append((name, not fails))
    print("RESULT rtsx %s %s" % (name, "PASS" if not fails else "FAIL: " + ", ".join(fails)))


args = sys.argv[1:] or ["all"]
opts = dict(a.split("=", 1) for a in args if "=" in a)
if "lqfp" in args:
    opts["lqfp"] = True
if args[0] in ("usb", "all"):
    pids = [opts.get("pid", "0x0129")] if args[0] == "usb" else ["0x0129", "0x0139", "0x0140"]
    for p in pids:
        one("usb", p, opts)
    if args[0] == "all":
        one("usb", "0x0129", {"lqfp": True, "hc": "ehci"})
        one("usb", "0x0129", {"hc": "uhci"})
if args[0] in ("pci", "all"):
    dids = PCI_IDS if args[0] == "all" or opts.get("did") == "all" else [opts.get("did", "0x5227")]
    for d in dids:
        one("pci", d, opts)
print("RESULT rtsx summary %d/%d PASS %s" % (sum(1 for _, ok in results if ok), len(results),
                                             " ".join(n for n, ok in results if not ok)))
