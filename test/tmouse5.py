"""Five button mouse on the harness: QEMU's usb-mouse reports buttons 4 and 5
(side / extra, the back and forward buttons); the HID driver must give them as
MOUSE_BUTTON_4/5 down and up (0040/0080, 0100/0200) next to left, right and
middle.

  tmouse5.py HC        HC = uhci | ohci | ehci | xhci
"""
import os, re, sys, time
from qharn import Harness
name = sys.argv[1]
CFG = {"uhci": "piix3-usb-uhci", "ohci": "pci-ohci", "ehci": "usb-ehci", "xhci": "qemu-xhci"}
os.makedirs("/tmp/usbt", exist_ok=True)
h = Harness(["-device", CFG[name] + ",id=hc", "-device", "usb-mouse,bus=hc.0,id=m"],
            log="/tmp/usbt/m5_%s_%d.log" % (name, os.getpid()))
WANT = [("left", 0x0001, 0x0002), ("right", 0x0004, 0x0008), ("middle", 0x0010, 0x0020),
        ("side", 0x0040, 0x0080), ("extra", 0x0100, 0x0200)]
ok = True
try:
    h.wait("harness: ready", 60)
    h.wait("READY")
    h.collect(1)
    info = [l for l in h.all.splitlines() if "buttons" in l]
    five = any("5 buttons" in l for l in info)
    print("\n".join(info))
    print("%s five buttons parsed" % ("ok  " if five else "FAIL"))
    ok &= five
    for b, down, up in WANT:
        got = []
        for d in (True, False):
            h.qmp_cmd("input-send-event", events=[{"type": "btn", "data": {"down": d, "button": b}}])
            time.sleep(0.25)
            got += [int(m.group(1), 16) for m in (re.search(r"MOUSE flags=([0-9a-f]+)", l) for l in h.collect(0.4)) if m]
        good = got == [down, up]
        ok &= good
        print("%s %-6s %s" % ("ok  " if good else "FAIL", b, " ".join("%04x" % f for f in got)))
    # two buttons in one report: back held while forward is pressed
    h.qmp_cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "side"}},
                                          {"type": "btn", "data": {"down": True, "button": "extra"}}])
    time.sleep(0.3)
    h.qmp_cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "side"}},
                                          {"type": "btn", "data": {"down": False, "button": "extra"}}])
    got = [int(m.group(1), 16) for m in (re.search(r"MOUSE flags=([0-9a-f]+)", l) for l in h.collect(1)) if m]
    good = got == [0x0140, 0x0280]
    ok &= good
    print("%s both   %s" % ("ok  " if good else "FAIL", " ".join("%04x" % f for f in got)))
finally:
    h.close()
print("RESULT mouse5", name, "PASS" if ok else "FAIL")
