"""PS/2 touch pad protocols on the bare metal harness: QEMU tp-ps2pad (in
place of the i8042's PS/2 mouse) as a Synaptics, an Elantech v4 and an ALPS v7
pad with a trackpoint; src/ps2pad.c detects it, switches it to its absolute
mode, and the gesture engine (src/ptp.c) makes pointer motion, a tap click,
two finger scrolling (wheel) and trackpoint motion of it.

  QEMU=.../qemu-system-x86_64 python3 tps2.py [synaptics elantech alps]
"""
import os, sys, time
from qharn import Harness

os.makedirs("/tmp/usbt", exist_ok=True)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def mouse(ls):
    out = []
    for l in ls:
        if l.startswith("MOUSE"):
            d = dict(kv.split("=") for kv in l.split()[1:])
            out.append({k: int(v, 16) if k == "flags" else int(v) for k, v in d.items()})
    return out


def touch(h, frame, wait=0.03):
    h.qmp_cmd("qom-set", path="/machine/peripheral/tp", property="touch", value=frame)
    time.sleep(wait)


def one(proto):
    h = Harness(["-device", "tp-ps2pad,id=tp,proto=%s,stick=on" % proto, "-smp", "1"],
                log="/tmp/usbt/ps2_%s.log" % proto)
    try:
        h.wait("harness: ready", 60)
        h.wait("READY")
        r = h.cmd("ps2", 30)
        want = {"synaptics": 1, "elantech": 2, "alps": 3}[proto]
        line = [l for l in r if l.startswith("PS2")]
        chk(line and ("proto=%d" % want) in line[0], "%s: detected (%s)" % (proto, line[0] if line else r[-3:]))
        chk(h.qmp_cmd("qom-get", path="/machine/peripheral/tp", property="mode") == 1, "%s: absolute mode" % proto)
        h.collect(0.2)
        # one finger: right and down
        for i in range(10):
            touch(h, "%d:%d" % (300 + i * 20, 300 + i * 10))
        touch(h, "", 0.3)
        m = mouse(h.collect(0.3))
        sx, sy = sum(e["x"] for e in m), sum(e["y"] for e in m)
        chk(sx > 20 and sy > 5 and not [e for e in m if e["flags"] & 0x3FF],
            "%s: one finger moves the pointer (%d, %d), no click" % (proto, sx, sy))
        # tap
        touch(h, "500:500", 0.04)
        touch(h, "", 0.3)
        m = mouse(h.collect(0.3))
        chk([e["flags"] for e in m if e["flags"]] == [1, 2], "%s: tap = left click %s" % (proto, [e["flags"] for e in m]))
        # two fingers down the pad: wheel up (content follows the fingers)
        for i in range(12):
            touch(h, "400:%d 600:%d" % (200 + i * 40, 200 + i * 40))
        touch(h, "", 0.3)
        m = mouse(h.collect(0.3))
        w = sum(e["wheel"] for e in m)
        chk(w >= 4 and sum(e["x"] for e in m) == 0 and not [e for e in m if e["flags"] & 0x3FF],
            "%s: two finger scroll = wheel %d" % (proto, w))
        # trackpoint
        touch(h, "s=5:3")
        touch(h, "s=5:3", 0.1)
        m = mouse(h.collect(0.2))
        chk(sum(e["x"] for e in m) == 10 and sum(e["y"] for e in m) == 6,
            "%s: trackpoint motion (%d, %d)" % (proto, sum(e["x"] for e in m), sum(e["y"] for e in m)))
    finally:
        h.close()


for p in (sys.argv[1:] or ["synaptics", "elantech", "alps"]):
    one(p)
print("RESULT ps2 %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
