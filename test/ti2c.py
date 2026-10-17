"""HID over I2C test on the bare metal harness: QEMU dw-i2c-pci (Intel LPSS
DesignWare I2C, starts in reset) with i2c-hid test devices.

  python3 ti2c.py [scenario ...]     (default: all)

scenarios:
  ptp     precision touch pad at 0x2c (found at a usual address): multi-touch
          mode, pointer, two finger scrolling, taps, pad button
  ptphybrid  the same with one contact per report and an 8 byte FIFO
  ptpmouse   the touch pad left in mouse mode (TouchpadGestures=0)
  acpi    touch screen at 0x33, descriptor register 0x30: only the ACPI SSDT
          (-acpitable) tells where it is
  kbd     keyboard at 0x15 (register 1): keys, LEDs
  tpad    touch pad without a mouse collection: finger motion to relative
  repeat  firmware that answers every poll with its last report
  mixed   touch pad + USB keyboard + USB mouse together, 2 controllers
  smallfifo  controller with an 8 entry FIFO (reads longer than the FIFO)
  off     registry style switch I2cHid=0 (harness: -fw_cfg) -> nothing
"""
import os, struct, sys, time
from qharn import Harness

os.makedirs("/tmp/usbt", exist_ok=True)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def pkglen(n):
    """AML PkgLength for n bytes of content"""
    if n + 1 < 64:
        return bytes([n + 1])
    for k in (2, 3, 4):
        v = n + k
        if v < (1 << (4 + 8 * (k - 1))):
            out = [((k - 1) << 6) | (v & 0x0F)]
            v >>= 4
            for _ in range(k - 1):
                out.append(v & 0xFF)
                v >>= 8
            return bytes(out)
    raise ValueError


def ssdt(addr, ctrl_path, dsm_reg):
    src = ctrl_path.encode() + b"\0"
    d = bytes([2, 0, 1, 0x02, 0, 0, 1]) + struct.pack("<HIH", 6, 400000, addr) + src   # rev .. address, source
    desc = bytes([0x8E]) + struct.pack("<H", len(d)) + d + bytes([0x79, 0])
    buf = bytes([0x0A, len(desc)]) + desc
    crs = b"\x08_CRS" + b"\x11" + pkglen(len(buf)) + buf
    hid = b"\x08_HID\x0DTEST0001\0"
    cid = b"\x08_CID\x0C\x41\xD0\x0C\x50"
    meth = b"_DSM\x04\xA4\x0A" + bytes([dsm_reg])
    dsm = b"\x14" + pkglen(len(meth)) + meth
    body = b"TPD1" + hid + cid + crs + dsm
    dev = b"\x5B\x82" + pkglen(len(body)) + body
    sbody = b"\\_SB_" + dev
    scope = b"\x10" + pkglen(len(sbody)) + sbody
    length = 36 + len(scope)
    hdr = b"SSDT" + struct.pack("<IB", length, 2) + b"\0" + b"USBNT " + b"I2CTEST " + struct.pack("<I", 1) + \
        b"USBN" + struct.pack("<I", 1)
    t = bytearray(hdr + scope)
    t[9] = (256 - sum(t) % 256) % 256
    p = "/tmp/usbt/i2c_ssdt.aml"
    open(p, "wb").write(bytes(t))
    return p


def run(c, h, show=True):
    r = h.cmd(c, 60)
    if show:
        for l in r:
            print("  " + l)
    return r


def start(args, name):
    h = Harness(args, log="/tmp/usbt/i2c_%s.log" % name)
    boot = h.wait("harness: ready", 90)
    for l in boot:
        if "i2c" in l or "acpi" in l:
            print("  " + l)
    h.wait("READY")
    return h, boot


def qget(h, path, prop):
    return h.qmp_cmd("qom-get", path=path, property=prop)


def mouse_lines(ls):
    out = []
    for l in ls:
        if l.startswith("MOUSE"):
            d = dict(kv.split("=") for kv in l.split()[1:])
            out.append({k: int(v, 16) if k == "flags" else int(v) for k, v in d.items()})
    return out


def abs_ev(h, x, y, btn=None):
    ev = [{"type": "abs", "data": {"axis": "x", "value": x}}, {"type": "abs", "data": {"axis": "y", "value": y}}]
    if btn is not None:
        ev.append({"type": "btn", "data": {"button": "left", "down": btn}})
    h.qmp_cmd("input-send-event", events=ev)


def sc_ptpmouse():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0", "-append", "TouchpadGestures=0",
                     "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x2c,kind=ptp"], "ptpmouse")
    try:
        log = "\n".join(boot)
        chk("DesignWare I2C controller 00:05.0" in log, "ptpmouse: controller found (taken out of LPSS reset)")
        chk("06cb:7001" in log and "touch pad in mouse mode" in log, "ptpmouse: touch pad found at 0x2c, mouse collection")
        r = run("i2c", h)
        chk(any("I2CHID 0" in l and "addr=2c" in l and "reg=0020" in l for l in r), "ptpmouse: listed")
        chk(qget(h, "/machine/peripheral/tp", "resets") == 1 and qget(h, "/machine/peripheral/tp", "power") == 0,
            "ptpmouse: reset and powered on")
        h.hmp("mouse_move 10 5")
        time.sleep(0.3)
        h.hmp("mouse_move -300 7")
        time.sleep(0.3)
        h.hmp("mouse_button 1")
        time.sleep(0.2)
        h.hmp("mouse_button 0")
        time.sleep(0.2)
        h.hmp("mouse_button 2")
        time.sleep(0.2)
        h.hmp("mouse_button 0")
        m = mouse_lines(h.collect(1.5))
        for e in m:
            print("  ", e)
        sx = sum(e["x"] for e in m)
        sy = sum(e["y"] for e in m)
        chk(sx == -290 and sy == 12, "ptpmouse: relative motion (%d, %d)" % (sx, sy))
        fl = [e["flags"] for e in m if e["flags"]]
        chk(fl == [1, 2, 4, 8], "ptpmouse: left and right button %s" % fl)
        chk(all(e["abs"] == 0 for e in m), "ptpmouse: relative")
        time.sleep(1)
        chk(not mouse_lines(h.collect(0.5)), "ptpmouse: no reports while idle")
    finally:
        h.close()


def ptp_ev(h, x=None, y=None, **btn):
    ev = []
    if x is not None:
        ev.append({"type": "abs", "data": {"axis": "x", "value": x}})
    if y is not None:
        ev.append({"type": "abs", "data": {"axis": "y", "value": y}})
    for b, down in btn.items():
        ev.append({"type": "btn", "data": {"button": b, "down": down}})
    h.qmp_cmd("input-send-event", events=ev)


def gestures(h, name):
    """left button = finger 1 on the pad, right = finger 2, middle = the pad's click"""
    m = []
    # one finger moves the pointer
    ptp_ev(h, 0x2000, 0x2000, left=True); time.sleep(0.25)
    for i in range(1, 5):
        ptp_ev(h, 0x2000 + i * 0x400, 0x2000 + i * 0x200); time.sleep(0.05)
    ptp_ev(h, left=False); time.sleep(0.4)
    a = mouse_lines(h.collect(0.5))
    sx, sy = sum(e["x"] for e in a), sum(e["y"] for e in a)
    # 200 x 100 counts linear; pointer acceleration (TouchpadAccel) makes this fast stroke up to 2.2 times longer
    chk(180 < sx < 460 and abs(sx - 2 * sy) <= 6 and not [e for e in a if e["flags"]],
        "%s: one finger moves the pointer (%d, %d), no click" % (name, sx, sy))
    # two fingers scroll: down = wheel up (content follows the fingers)
    ptp_ev(h, 0x3000, 0x1000, left=True, right=True); time.sleep(0.25)
    for i in range(1, 9):
        ptp_ev(h, 0x3000, 0x1000 + i * 0x400); time.sleep(0.05)
    ptp_ev(h, left=False, right=False); time.sleep(0.4)
    b = mouse_lines(h.collect(0.5))
    wheel = sum(e["wheel"] for e in b)
    chk(wheel >= 5 and all(e["x"] == 0 and e["y"] == 0 for e in b) and
        all(e["flags"] in (0, 0x400) for e in b), "%s: two finger scroll (wheel %d)" % (name, wheel))
    for i in range(1, 5):
        pass
    # taps: one finger = left click, two = right click
    ptp_ev(h, 0x4000, 0x4000, left=True); time.sleep(0.06); ptp_ev(h, left=False); time.sleep(0.4)
    c = mouse_lines(h.collect(0.4))
    chk([e["flags"] for e in c if e["flags"]] == [1, 2], "%s: tap = left click %s" % (name, [e["flags"] for e in c]))
    ptp_ev(h, 0x4000, 0x4000, left=True, right=True); time.sleep(0.06)
    ptp_ev(h, left=False, right=False); time.sleep(0.4)
    c = mouse_lines(h.collect(0.4))
    chk([e["flags"] for e in c if e["flags"]] == [4, 8], "%s: two finger tap = right click %s" %
        (name, [e["flags"] for e in c]))
    # the pad's button: left with one finger, right with two
    ptp_ev(h, 0x4000, 0x4000, left=True); time.sleep(0.3)
    ptp_ev(h, middle=True); time.sleep(0.2); ptp_ev(h, middle=False); time.sleep(0.2)
    ptp_ev(h, left=False); time.sleep(0.4)
    ptp_ev(h, 0x4000, 0x4000, left=True, right=True); time.sleep(0.3)
    ptp_ev(h, middle=True); time.sleep(0.2); ptp_ev(h, middle=False); time.sleep(0.2)
    ptp_ev(h, left=False, right=False); time.sleep(0.4)
    c = mouse_lines(h.collect(0.5))
    chk([e["flags"] for e in c if e["flags"] & 0xF] == [1, 2, 4, 8], "%s: pad click, left / right %s" %
        (name, [e["flags"] for e in c]))


def sc_ptp():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0",
                     "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x2c,kind=ptp"], "ptp")
    try:
        log = "\n".join(boot)
        chk("precision touch pad (2 contact(s) per report)" in log, "ptp: precision touch pad, multi-touch mode")
        chk(qget(h, "/machine/peripheral/tp", "input-mode") == 3, "ptp: Input Mode feature set to 3")
        gestures(h, "ptp")
    finally:
        h.close()


def sc_ptphybrid():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0,fifo=8",
                     "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x2c,kind=ptp,hybrid=on"], "ptphybrid")
    try:
        chk("precision touch pad" in "\n".join(boot), "ptphybrid: multi-touch mode (8 byte FIFO)")
        gestures(h, "ptphybrid")
    finally:
        h.close()


def sc_acpi():
    aml = ssdt(0x33, "\\_SB.PCI0.S28", 0x30)
    h, boot = start(["-acpitable", "file=" + aml, "-device", "dw-i2c-pci,id=i2c0,addr=05.0",
                     "-device", "i2c-hid,id=ts,bus=i2c0.0,address=0x33,kind=touch,desc-reg=0x30"], "acpi")
    try:
        log = "\n".join(boot)
        chk("TPD1 (TEST0001) HID over I2C at 33 on S28_, 400 kHz" in log, "acpi: SSDT device read")
        chk("descriptor register 0030" in log, "acpi: descriptor register from _DSM")
        chk("touch screen" in log, "acpi: touch screen")
        abs_ev(h, 0x4000, 0x2000)
        time.sleep(0.2)
        abs_ev(h, 0x4000, 0x2000, True)
        time.sleep(0.2)
        abs_ev(h, 0x6000, 0x7fff)
        time.sleep(0.2)
        abs_ev(h, 0x6000, 0x7fff, False)
        m = mouse_lines(h.collect(1.5))
        for e in m:
            print("  ", e)
        chk(m and all(e["abs"] == 1 for e in m), "acpi: absolute pointer")
        chk(any(abs(e["x"] - 32775) < 100 and abs(e["y"] - 16387) < 100 for e in m), "acpi: position scaled")
        chk(any(e["x"] > 49000 and e["y"] > 65400 for e in m), "acpi: second position")
        fl = [e["flags"] for e in m if e["flags"]]
        chk(fl == [1, 2], "acpi: tip switch = left button %s" % fl)
    finally:
        h.close()


def sc_kbd():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0",
                     "-device", "i2c-hid,id=kb,bus=i2c0.0,address=0x15,kind=kbd,desc-reg=1"], "kbd")
    try:
        log = "\n".join(boot)
        chk("keyboard" in log and "descriptor register 0001" in log, "kbd: keyboard found (register 1)")
        h.hmp("sendkey a")
        time.sleep(0.3)
        h.hmp("sendkey shift-b")
        time.sleep(0.3)
        h.hmp("sendkey kp_enter")
        k = [l for l in h.collect(1.5) if l.startswith("KEY")]
        for l in k:
            print("  ", l)
        chk(k[:2] == ["KEY 1e down", "KEY 1e up"], "kbd: a")
        chk("KEY 2a down" in k and "KEY 30 down" in k and "KEY 30 up" in k and "KEY 2a up" in k, "kbd: shift-b")
        chk("KEY 1c down e0" in k, "kbd: keypad enter (E0)")
        run("leds 6", h)          # caps + num
        time.sleep(0.5)
        run("work", h, False)
        leds = qget(h, "/machine/peripheral/kb", "leds")
        chk(leds == 3, "kbd: LED output report (%s)" % leds)
    finally:
        h.close()


def sc_tpad():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0",
                     "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x2a,kind=tpad"], "tpad")
    try:
        log = "\n".join(boot)
        chk("touch pad," in log or ": touch pad" in log, "tpad: touch pad without mouse collection")
        abs_ev(h, 0x2000, 0x2000)
        time.sleep(0.2)
        abs_ev(h, 0x2000, 0x2000, True)       # finger down
        time.sleep(0.2)
        for i in range(1, 6):
            abs_ev(h, 0x2000 + i * 0x400, 0x2000 + i * 0x200)
            time.sleep(0.1)
        abs_ev(h, 0x2000 + 5 * 0x400, 0x2000 + 5 * 0x200, False)   # finger up
        time.sleep(0.2)
        abs_ev(h, 0x7000, 0x7000)             # moves without the finger: nothing
        time.sleep(0.3)
        m = mouse_lines(h.collect(1.5))
        for e in m:
            print("  ", e)
        sx = sum(e["x"] for e in m)
        sy = sum(e["y"] for e in m)
        chk(all(e["abs"] == 0 for e in m), "tpad: relative")
        chk(230 < sx < 270 and 115 < sy < 135, "tpad: finger motion (%d, %d)" % (sx, sy))
        chk(all(e["flags"] == 0 for e in m), "tpad: tip is not a click")
    finally:
        h.close()


def sc_repeat():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0",
                     "-device", "i2c-hid,id=ms,bus=i2c0.0,address=0x10,kind=mouse,repeat=on"], "repeat")
    try:
        h.hmp("mouse_move 4 4")
        time.sleep(0.2)
        m = mouse_lines(h.collect(3))
        print("   %d mouse events, first %s" % (len(m), m[:2]))
        log = h.all
        chk("repeats its last report" in log, "repeat: detected")
        late = mouse_lines(h.collect(1))
        chk(not late, "repeat: no more events after detection")
        h.hmp("mouse_move 7 0")
        time.sleep(0.3)
        m2 = mouse_lines(h.collect(1))
        chk(any(e["x"] == 7 for e in m2), "repeat: new reports still come through")
    finally:
        h.close()


def sc_mixed():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0", "-device", "dw-i2c-pci,id=i2c1,addr=06.0,fifo=32",
                     "-append", "TouchpadGestures=0",
                     "-device", "i2c-hid,id=kb,bus=i2c0.0,address=0x15,kind=kbd,desc-reg=1",
                     "-device", "qemu-xhci,id=hc", "-device", "usb-mouse,bus=hc.0",
                     "-device", "i2c-hid,id=tp,bus=i2c1.0,address=0x2c,kind=ptp"], "mixed")
    try:
        r = run("i2c", h)
        chk(sum(1 for l in r if l.startswith("I2CCTRL")) == 2, "mixed: two controllers")
        chk(sum(1 for l in r if l.startswith("I2CHID")) == 2, "mixed: keyboard and touch pad")
        h.hmp("mouse_move 3 3")
        h.hmp("sendkey z")
        time.sleep(0.5)
        ls = h.collect(1.5)
        chk(any(l.startswith("KEY 2c down") for l in ls), "mixed: I2C keyboard")
        chk(any(l.startswith("MOUSE") for l in ls), "mixed: pointer")
        chk("device 0: mouse" in h.all, "mixed: USB mouse attached")
    finally:
        h.close()


def sc_smallfifo():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0,fifo=8", "-append", "TouchpadGestures=0",
                     "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x2c,kind=ptp"], "smallfifo")
    try:
        log = "\n".join(boot)
        chk("FIFO 8/8" in log and "touch pad in mouse mode" in log, "smallfifo: report descriptor through an 8 byte FIFO")
        h.hmp("mouse_move 20 -9")
        time.sleep(0.3)
        h.hmp("mouse_button 1")
        time.sleep(0.2)
        h.hmp("mouse_button 0")
        m = mouse_lines(h.collect(1.5))
        chk(sum(e["x"] for e in m) == 20 and sum(e["y"] for e in m) == -9, "smallfifo: motion")
        chk([e["flags"] for e in m if e["flags"]] == [1, 2], "smallfifo: button")
    finally:
        h.close()


def sc_off():
    h, boot = start(["-device", "dw-i2c-pci,id=i2c0,addr=05.0", "-append", "I2cHid=0",
                     "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x2c,kind=ptp"], "off")
    try:
        r = run("i2c", h)
        chk(not any(l.startswith("I2CHID") for l in r), "off: I2cHid=0 leaves the bus alone")
    finally:
        h.close()


ALL = ["ptp", "ptphybrid", "ptpmouse", "acpi", "kbd", "tpad", "repeat", "mixed", "smallfifo", "off"]
for s in (sys.argv[1:] or ALL):
    print("== " + s)
    try:
        globals()["sc_" + s]()
    except Exception as e:
        chk(False, "%s: %s" % (s, e))
print("RESULT i2c %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
