"""USB Video Class camera test on the bare metal harness.

Needs the patched QEMU with the usb-uvc device (QEMU=... QEMU_BIOS=...).

  python3 tuvc.py <hc> [bulk] [dht] [unplug]

hc: uhci ohci ehci xhci
"""
import re
import sys
from qharn import Harness

hc = sys.argv[1]
rest = sys.argv[2:]
HC = {
    "uhci": ["-device", "piix3-usb-uhci,id=hc"],
    "ohci": ["-device", "pci-ohci,id=hc"],
    "ehci": ["-device", "usb-ehci,id=hc"],
    "xhci": ["-device", "qemu-xhci,id=hc"],
}
bulk = "bulk" in rest
dht = "dht" in rest
dev = "usb-uvc,id=cam,bus=hc.0,debug=1%s%s" % (",bulk=on" if bulk else "", ",mjpeg-dht=on" if dht else "")
args = HC[hc] + ["-device", dev]
tag = "%s%s" % (hc, "_bulk" if bulk else "")
h = Harness(args, log="/tmp/usbt/uvc_%s.log" % tag)
fails = []
fullspeed = hc in ("uhci", "ohci")
BARS = [235, 210, 170, 145, 106, 81, 41, 16]     # approx. full range luma of the colour bars


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def show(ls):
    for l in ls:
        print("  " + l)
    return ls


def grab(cid, n, timeout=4000):
    r = show(h.cmd("vgrab %s %d %d" % (cid, n, timeout), timeout=120))
    g = [l for l in r if l.startswith("VGRAB")]
    d = dict(kv.split("=") for kv in g[0].split()[1:]) if g else {}
    return r, {k: int(v) for k, v in d.items()}


try:
    show(h.wait("harness: ready", 90))
    h.wait("READY")
    show(h.cmd("work"))
    r = show(h.cmd("vls"))
    cams = [l for l in r if l.startswith("CAM ")]
    chk(len(cams) == 1, "camera found")
    cid = re.search(r"id=(\w+)", cams[0]).group(1) if cams else "0"
    cid = "0x" + cid
    fm = [l for l in r if l.startswith("VFMT")]
    chk(len(fm) == 2 and "YUY2" in fm[0] and "MJPG" in fm[1], "formats YUY2 + MJPG")
    chk("160x120" in fm[0] and "640x480" in fm[0] if fm else False, "frame sizes")
    chk(cams and "controls=5fc" in cams[0], "processing unit controls")
    chk(cams and "QEMU UVC Test Camera" in cams[0], "name")

    # YUY2 320x240 (full speed: 160x120)
    frm = 0 if fullspeed else 1
    r = show(h.cmd("vstart %s 0 %d 0" % (cid, frm)))
    chk(any("VSTART r=0" in l for l in r), "start YUY2")
    r, g = grab(cid, 8 if not fullspeed else 4)
    chk(g.get("frames", 0) >= (8 if not fullspeed else 4), "YUY2 frames")
    chk(g.get("errors", 1) == 0, "YUY2 frames complete")
    bars = [l for l in r if l.startswith("VBARS")]
    if bars:
        v = [int(x) for x in bars[-1].split()[1:9]]
        chk(all(abs(a - b) <= 25 for a, b in zip(v, BARS)), "colour bars %s" % v)
        boxes = [re.search(r"box=(\d+)", l).group(1) for l in bars]
        chk(len(boxes) == 2 and boxes[0] != boxes[1], "picture moves (%s)" % boxes)
    else:
        chk(False, "colour bars")
    if not fullspeed:
        fps = 1000.0 * g.get("seqspan", 0) / max(g.get("ms", 1), 1)
        print("  fps %.1f" % fps)
        chk(fps > 10, "frame rate")

    # brightness
    r = show(h.cmd("vctl %s 2" % cid))
    chk(any("VCTL r=0" in l and "min=-64" in l and "max=64" in l for l in r), "brightness range")
    show(h.cmd("vctl %s 2 40" % cid))
    r = show(h.cmd("vctl %s 2" % cid))
    chk(any("value=40" in l for l in r), "brightness set")
    r, g = grab(cid, 3)
    bars = [l for l in r if l.startswith("VBARS")]
    if bars:
        v = [int(x) for x in bars[-1].split()[1:9]]
        chk(v[7] >= 35, "brighter picture (black bar %d)" % v[7])
    show(h.cmd("vctl %s 2 0" % cid))
    show(h.cmd("vstop %s" % cid))

    # MJPEG 640x480 (full speed: 320x240)
    frm = 0 if fullspeed else 1
    r = show(h.cmd("vstart %s 1 %d 0" % (cid, frm)))
    chk(any("VSTART r=0" in l and "MJPG" in l for l in r), "start MJPEG")
    r, g = grab(cid, 10 if not fullspeed else 5)
    chk(g.get("frames", 0) >= (10 if not fullspeed else 5), "MJPEG frames")
    js = [l for l in r if l.startswith("VJPEG")]
    chk(js and all("soi=1 eoi=1 sof=1" in l for l in js), "whole JPEG frames")
    chk(js and all(("dht=1" if dht else "dht=0") in l for l in js), "DHT as the camera sends it")
    if not fullspeed:
        fps = 1000.0 * g.get("seqspan", 0) / max(g.get("ms", 1), 1)
        print("  fps %.1f" % fps)
        chk(fps > 15, "MJPEG frame rate")
    # restart with another size while streaming
    r = show(h.cmd("vstart %s 1 0 666666" % cid))
    chk(any("VSTART r=0" in l and "320x240" in l and "interval=666666" in l for l in r), "restart 320x240 15 fps")
    r, g = grab(cid, 4)
    chk(g.get("frames", 0) >= 4, "frames after restart")

    if "unplug" in rest:
        h.hmp("device_del cam")
        r = show(h.cmd("vgrab %s 2 2000" % cid))
        show(h.cmd("work"))
        r = show(h.cmd("vls"))
        chk(any("VLS count=0" in l for l in r), "camera gone after unplug")
    else:
        show(h.cmd("vstop %s" % cid))
        r = show(h.cmd("vls"))
        chk(any("streaming=0" in l for l in r), "stopped")
    show(h.qemu_lines("uvc: ")[-12:])
finally:
    h.close()
print("RESULT uvc %s %s" % (tag, "PASS" if not fails else "FAIL: " + ", ".join(fails)))
