"""USB Audio class test on the bare metal harness.

Needs the patched QEMU with the usb-uac device (QEMU=... QEMU_BIOS=...).

  python3 tuac.py <hc> <uac> [hs] [device options]

hc: uhci ohci ehci xhci, ehci-tt (full speed device behind a high speed hub)
"""
import re
import sys
import time
from qharn import Harness

hc = sys.argv[1]
uac = sys.argv[2]
rest = sys.argv[3:]
HC = {
    "uhci": ["-device", "piix3-usb-uhci,id=hc"],
    "ohci": ["-device", "pci-ohci,id=hc"],
    "ehci": ["-device", "usb-ehci,id=hc"],
    "ehci-tt": ["-device", "usb-ehci,id=hc", "-device", "usb-hub,bus=hc.0,port=1,hs=on,id=hub"],
    "xhci": ["-device", "qemu-xhci,id=hc"],
}
hs = "on" if "hs" in rest else "off"
opts = ",".join(o for o in rest if o != "hs")
port = "port=1.1," if hc == "ehci-tt" else ""
dev = "usb-uac,id=ua,bus=hc.0,%suac=%s,hs=%s,debug=1,audiodev=snd%s" % (port, uac, hs, ("," + opts) if opts else "")
args = HC[hc] + ["-audiodev", "none,id=snd", "-device", dev]
tag = "%s_uac%s%s" % (hc, uac, "_hs" if hs == "on" else "")
h = Harness(args, log="/tmp/usbt/uac_%s.log" % tag)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def show(ls):
    for l in ls:
        print("  " + l)
    return ls


def stats():
    st = [l for l in h.qemu_lines("uac: stats")]
    if not st:
        return {}
    return dict(kv.split("=", 1) for kv in st[-1].split()[2:])


def astat(d):
    r = show(h.cmd("astat %s %d" % (aid, d)))
    for l in r:
        if l.startswith("ASTAT"):
            return dict(kv.split("=", 1) for kv in l.split()[1:])
    return {}


try:
    r = show(h.wait("harness: ready", 90))
    h.wait("READY")
    r = show(h.cmd("work"))
    r += show(h.cmd("als"))
    arr = [l for l in h.all.splitlines() if "AUDIO ARRIVE" in l]
    chk(len(arr) == 1, "arrival")
    aid = re.search(r"id=(\w+)", arr[0]).group(1) if arr else "0"
    chk(any("play=1" in l and "rec=1" in l for l in arr), "both directions")
    chk(any(("uac=%s" % uac) in l for l in arr), "uac version")
    fmts = [l for l in r if "FMT" in l]
    chk(len(fmts) >= 4, "formats listed")
    # playback, 48 kHz 16 bit
    for rate, bits in ((48000, 16), (44100, 24), (96000, 16)):
        r = show(h.cmd("aopen %s 0 %d 2 %d" % (aid, rate, bits)))
        ok = any("AOPEN r=0" in l and ("rate=%d" % rate) in l for l in r)
        if not ok and rate == 96000 and hs == "off" and uac == "1":
            # UAC1 full speed: 96 kHz stereo 16 bit still fits (384 bytes/frame)
            pass
        chk(ok, "open playback %d/%d" % (rate, bits))
        r = show(h.cmd("astart %s 0" % aid))
        chk(any("ASTART r=0" in l for l in r), "start playback %d" % rate)
        before = stats()
        h.collect(3.0)
        a = astat(0)
        after = stats()
        show(h.qemu_lines("uac: stats")[-1:])
        pk = int(after.get("pkts", 0)) - int(before.get("pkts", 0))
        fr = int(after.get("frames", 0)) - int(before.get("frames", 0))
        chk(pk > 1000, "packets flowing %d (%d)" % (rate, pk))
        chk(int(after.get("bad", 1)) == 0, "no malformed packets %d" % rate)
        # an emulated controller may skip frames when the host is busy: allow 3 %
        chk(int(a.get("err", 1)) <= max(40, int(a.get("pkts", 0)) * 3 // 100), "no errors %d (%s)" % (rate, a.get("err")))
        if pk:
            per = fr / pk
            print("    frames/packet %.2f" % per)
        chk(int(after.get("rate", 0)) == rate, "device rate %d" % rate)
        r = show(h.cmd("astop %s 0" % aid))
        r = show(h.cmd("aclose %s 0" % aid))
    chk(any("SET rate" in l for l in h.qemu_lines("uac: SET")), "rate request")
    # feedback
    if "feedback=off" not in opts and "implicit=on" not in opts:
        a = astat(0)
        r = show(h.cmd("aopen %s 0 48000 2 16" % aid))
        show(h.cmd("astart %s 0" % aid))
        h.collect(2.0)
        a = astat(0)
        chk(int(a.get("fbn", 0)) > 10, "feedback received")
        show(h.cmd("astop %s 0" % aid))
        show(h.cmd("aclose %s 0" % aid))
    # capture
    r = show(h.cmd("aopen %s 1 48000 2 16" % aid))
    chk(any("AOPEN r=0" in l for l in r), "open capture")
    show(h.cmd("astart %s 1" % aid))
    h.collect(2.0)
    a = astat(1)
    chk(int(a.get("cap", 0)) > 48000, "captured frames")
    chk(abs(int(a.get("hz", 0)) - 1000) < 30, "capture frequency %s" % a.get("hz"))
    chk(int(a.get("peak", 0)) > 12000, "capture level")
    show(h.cmd("astop %s 1" % aid))
    show(h.cmd("aclose %s 1" % aid))
    # full duplex
    show(h.cmd("aopen %s 0 48000 2 16" % aid))
    show(h.cmd("aopen %s 1 48000 2 16" % aid))
    show(h.cmd("astart %s 0" % aid))
    show(h.cmd("astart %s 1" % aid))
    before = stats()
    h.collect(2.0)
    after = stats()
    a = astat(1)
    chk(int(after.get("pkts", 0)) - int(before.get("pkts", 0)) > 1000, "duplex playback")
    chk(abs(int(a.get("hz", 0)) - 1000) < 30, "duplex capture")
    if "implicit=on" in opts:
        a = astat(0)
        chk(int(a.get("fbn", 0)) > 10, "implicit feedback from the capture stream (%s)" % a.get("fbn"))
        before = stats()
        h.collect(2.0)
        after = stats()
        pk = int(after.get("pkts", 0)) - int(before.get("pkts", 0))
        fr = int(after.get("frames", 0)) - int(before.get("frames", 0))
        print("    implicit: frames/packet %.4f" % (fr / pk if pk else 0))
    # volume
    show(h.cmd("avol %s 0 -2560 -2560 0" % aid))
    show(h.cmd("work"))
    h.collect(0.5)
    chk(any("= -2560" in l for l in h.qemu_lines("uac: SET volume")), "hardware volume")
    # USB MIDI out
    if "midi=on" in opts:
        chk(any("midi=1" in l for l in arr), "midi interface")
        show(h.cmd("amidi %s 90 3c 64 3c 00 80 3c 40 f0 7e 7f 06 01 f7 fe" % aid))
        h.collect(0.5)
        ev = [l.split(None, 2)[2] for l in h.qemu_lines("uac: midi")]
        print("    midi events:", ev)
        chk(ev[:6] == ["09 90 3c 64", "09 90 3c 00", "08 80 3c 40", "04 f0 7e 7f", "07 06 01 f7",
                       "0f fe 00 00"], "midi events")
    # hot unplug while streaming
    h.hmp("device_del ua")
    r = show(h.collect(3.0))
    chk(any("AUDIO GONE" in l for l in r), "removal")
    show(h.cmd("ls"))
    # plug back
    h.hmp("device_add " + dev.replace(",", ",", 1))
    r = show(h.collect(4.0))
    show(h.cmd("work"))
    chk(any("AUDIO ARRIVE" in l for l in h.all.splitlines()[-200:]), "re-arrival")
finally:
    h.close()
print("RESULT %s %s" % (tag, "PASS" if not fails else "FAIL " + ", ".join(fails)))
sys.exit(1 if fails else 0)
