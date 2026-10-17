"""Touch pads on SMBus, bare metal harness: Intel ICH9 SMBus (q35 machine) and an
AMD FCH SMBus (amd-fch-smbus test device on the pc machine) with the RMI4 and
Elan SMBus test pads (test/qemu/smbus-touchpad.patch).

  python3 tsmbus.py [scenario ...]     (default: all)

scenarios:
  i801rmi   q35, RMI4 pad (F12, F30): set up, pointer, two finger scroll -> wheel, taps, pad click
  i801elan  q35, Elan pad: the same
  fchrmi    pc, AMD FCH rev 59h (PM registers in memory), RMI4 pad (F11, F3A) on port 1: port select and
            semaphore, gestures
  fchelan   pc, AMD FCH rev 42h (PM through 0CD6h), Elan pad on port 1: gestures
  list      Smbus=1 (default): an RMI4 pad is used only with a listed ACPI PS/2 mouse id (SSDT LEN2054)
  ps2       HarnessPs2=1: QEMU's PS/2 mouse is not a Synaptics / Elan pad -> stays PS/2, the SMBus pad is
            left alone; SmbusTouchpad=0x2c takes it anyway
  switch    the PS/2 -> SMBus switch with a scripted PS/2 side (HarnessPs2=2 / 3, QEMU has no PS/2 touch pad):
            Synaptics InterTouch with a listed id -> F5, SMBus; not listed -> stays PS/2; Elan new IC -> SMBus
  off       Smbus=0: no SMBus access
"""
import os, struct, sys, time
from qharn import Harness

os.makedirs("/tmp/usbt", exist_ok=True)
fails = []


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


def start(args, name, machine):
    h = Harness(["-smp", "1"] + args, log="/tmp/usbt/smbus_%s.log" % name, machine=machine)
    boot = h.wait("harness: ready", 60)
    for l in boot:
        if "smbus" in l or "PS2" in l:
            print("  " + l)
    h.wait("READY")
    return h, "\n".join(boot)


def qget(h, dev, prop):
    return h.qmp_cmd("qom-get", path="/machine/peripheral/" + dev, property=prop)


def mouse_lines(ls):
    out = []
    for l in ls:
        if l.startswith("MOUSE"):
            d = dict(kv.split("=") for kv in l.split()[1:])
            out.append({k: int(v, 16) if k == "flags" else int(v) for k, v in d.items()})
    return out


def ev(h, x=None, y=None, **btn):
    e = []
    if x is not None:
        e.append({"type": "abs", "data": {"axis": "x", "value": x}})
    if y is not None:
        e.append({"type": "abs", "data": {"axis": "y", "value": y}})
    for b, down in btn.items():
        e.append({"type": "btn", "data": {"button": b, "down": down}})
    h.qmp_cmd("input-send-event", events=e)


def smbus(h):
    return [l for l in h.cmd("smbus") if l.startswith("SMB")]


def gestures(h, name, maxx, maxy):
    """left button = finger 1, right = finger 2, middle = the pad's click"""
    ev(h, 0x2000, 0x2000, left=True); time.sleep(0.25)
    for i in range(1, 5):
        ev(h, 0x2000 + i * 0x400, 0x2000 + i * 0x200); time.sleep(0.05)
    ev(h, left=False); time.sleep(0.4)
    a = mouse_lines(h.collect(0.5))
    sx, sy = sum(e["x"] for e in a), sum(e["y"] for e in a)
    ex, ey = 0x1000 * maxx / 0x7fff * 1600 / maxx, 0x800 * maxy / 0x7fff * 1600 / maxx
    # linear about (ex, ey); pointer acceleration (TouchpadAccel) makes this fast stroke up to 2.3 times longer
    chk(ex - 12 < sx < ex * 2.3 + 12 and abs(sx * ey - sy * ex) <= 12 * ex and not [e for e in a if e["flags"]],
        "%s: one finger moves the pointer (%d, %d), about (%d, %d)" % (name, sx, sy, ex, ey))
    ev(h, 0x3000, 0x1000, left=True, right=True); time.sleep(0.25)
    for i in range(1, 9):
        ev(h, 0x3000, 0x1000 + i * 0x400); time.sleep(0.05)
    ev(h, left=False, right=False); time.sleep(0.4)
    b = mouse_lines(h.collect(0.5))
    wheel = sum(e["wheel"] for e in b)
    chk(wheel >= 5 and all(e["x"] == 0 and e["y"] == 0 for e in b) and all(e["flags"] in (0, 0x400) for e in b),
        "%s: two finger scroll -> wheel %d" % (name, wheel))
    ev(h, 0x4000, 0x4000, left=True); time.sleep(0.06); ev(h, left=False); time.sleep(0.4)
    c = mouse_lines(h.collect(0.4))
    chk([e["flags"] for e in c if e["flags"]] == [1, 2], "%s: tap = left click %s" % (name, [e["flags"] for e in c]))
    ev(h, 0x4000, 0x4000, left=True, right=True); time.sleep(0.06); ev(h, left=False, right=False); time.sleep(0.4)
    c = mouse_lines(h.collect(0.4))
    chk([e["flags"] for e in c if e["flags"]] == [4, 8], "%s: two finger tap = right click %s" %
        (name, [e["flags"] for e in c]))
    ev(h, 0x4000, 0x4000, left=True); time.sleep(0.3)
    ev(h, middle=True); time.sleep(0.2); ev(h, middle=False); time.sleep(0.2)
    ev(h, left=False); time.sleep(0.4)
    ev(h, 0x4000, 0x4000, left=True, right=True); time.sleep(0.3)
    ev(h, middle=True); time.sleep(0.2); ev(h, middle=False); time.sleep(0.2)
    ev(h, left=False, right=False); time.sleep(0.4)
    c = mouse_lines(h.collect(0.5))
    chk([e["flags"] for e in c if e["flags"] & 0xF] == [1, 2, 4, 8], "%s: pad click, left / right %s" %
        (name, [e["flags"] for e in c]))


def sc_i801rmi():
    h, log = start(["-append", "Smbus=2", "-device", "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"], "i801rmi", "q35")
    try:
        chk("Intel SMBus 2930 at 00:1f.3" in log and "block buffer" in log, "i801rmi: ICH9 SMBus found")
        chk("TM3053-003, RMI4 SMBus version 2, F12 3000x2000, 5 objects, F30 buttons (click pad)" in log,
            "i801rmi: RMI4 pad: F01, F12 register descriptors, F30")
        chk(qget(h, "tp", "configured") == 1 and qget(h, "tp", "irq-enable") == 0x0B, "i801rmi: configured, interrupts F01 F12 F30")
        gestures(h, "i801rmi", 3000, 2000)
        r = smbus(h)
        chk(any("SMBPAD 0" in l and "kind=rmi4" in l for l in r), "i801rmi: listed")
        chk(qget(h, "tp", "bad") == 0 and qget(h, "tp", "reports") > 20, "i801rmi: no protocol errors, %u data reads" %
            qget(h, "tp", "reports"))
    finally:
        h.close()


def sc_i801elan():
    h, log = start(["-append", "Smbus=2", "-device", "smbus-elantp,id=tp,bus=i2c,address=0x15"], "i801elan", "q35")
    try:
        chk("Elan product 100E, firmware 22, 3200x2100 (click pad)" in log, "i801elan: Elan pad: hello, enable, mode, range")
        chk(qget(h, "tp", "enabled") == 1 and qget(h, "tp", "mode") == 1, "i801elan: enabled, absolute mode")
        gestures(h, "i801elan", 3200, 2100)
        chk(qget(h, "tp", "bad") == 0, "i801elan: no protocol errors")
    finally:
        h.close()


def fch(rev):
    return ["-device", "amd-fch-smbus,id=fch,addr=14.0,rev=0x%x" % rev]


def sc_fchrmi():
    h, log = start(["-append", "Smbus=2"] + fch(0x59) +
                   ["-device", "smbus-rmi4tp,id=tp,bus=fchsmb1,address=0x2c,f11=on,f3a=on"], "fchrmi", "pc")
    try:
        chk("AMD SMBus 790B rev 59 at 00:14.0, I/O 0b00, 2 ports (select 02), PM memory" in log, "fchrmi: FCH found, PM in memory")
        chk("rmi4 2c (port 1)" in log and "F11 3000x2000" in log and "F3A buttons" in log, "fchrmi: pad on port 1, F11, F3A")
        gestures(h, "fchrmi", 3000, 2000)
        # the model counts a transfer without the semaphore and a semaphore given back with the port not restored
        chk(qget(h, "fch", "bad") == 0 and qget(h, "fch", "transactions") > 50,
            "fchrmi: semaphore taken, port put back (%u transfers)" % qget(h, "fch", "transactions"))
        chk(qget(h, "tp", "bad") == 0, "fchrmi: no protocol errors")
    finally:
        h.close()


def sc_fchelan():
    h, log = start(["-append", "Smbus=2"] + fch(0x42) +
                   ["-device", "smbus-elantp,id=tp,bus=fchsmb1,address=0x15"], "fchelan", "pc")
    try:
        chk("AMD SMBus 790B rev 42 at 00:14.0, I/O 0b00, 2 ports (select 02), PM 0CD6h" in log, "fchelan: FCH found, PM 0CD6h")
        chk("elan 15 (port 1)" in log, "fchelan: pad on port 1")
        gestures(h, "fchelan", 3200, 2100)
        chk(qget(h, "fch", "bad") == 0, "fchelan: semaphore, port put back")
    finally:
        h.close()


def pkglen(n):
    if n + 1 < 64:
        return bytes([n + 1])
    v = n + 2
    return bytes([(1 << 6) | (v & 0x0F), v >> 4])


def ssdt_mouse(hid):
    """Device (MOU2) { Name (_HID, hid) Name (_CID, EisaId ("PNP0F13")) } in \\_SB"""
    body = b"MOU2" + b"\x08_HID\x0D" + hid.encode() + b"\0" + b"\x08_CID\x0C\x41\xD0\x0F\x13"
    dev = b"\x5B\x82" + pkglen(len(body)) + body
    sbody = b"\\_SB_" + dev
    scope = b"\x10" + pkglen(len(sbody)) + sbody
    hdr = b"SSDT" + struct.pack("<IB", 36 + len(scope), 2) + b"\0" + b"USBNT " + b"SMBTEST " + struct.pack("<I", 1) + \
        b"USBN" + struct.pack("<I", 1)
    t = bytearray(hdr + scope)
    t[9] = (256 - sum(t) % 256) % 256
    p = "/tmp/usbt/smbus_ssdt.aml"
    open(p, "wb").write(bytes(t))
    return p


def sc_list():
    h, log = start(["-device", "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"], "list0", "q35")
    try:
        chk(not any(l.startswith("SMBPAD") for l in smbus(h)) and qget(h, "tp", "version-reads") == 0,
            "list: QEMU's PS/2 mouse id (PNP0F13) is not listed -> the pad is not touched")
    finally:
        h.close()
    h, log = start(["-acpitable", "file=" + ssdt_mouse("LEN2054"), "-device",
                    "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"], "list1", "q35")
    try:
        chk("PS/2 mouse in ACPI: PNP0F13 LEN2054" in log, "list: SSDT mouse id LEN2054 read")
        chk(any(l.startswith("SMBPAD 0") for l in smbus(h)), "list: E480 id (LEN2054) -> the pad is used")
    finally:
        h.close()


def sc_ps2():
    h, log = start(["-append", "Smbus=2 HarnessPs2=1", "-device", "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"], "ps2", "q35")
    try:
        chk("not a Synaptics or Elan pad, stays a PS/2 mouse" in log and "takeover=0" in log,
            "ps2: QEMU's PS/2 mouse identified, stays PS/2")
        chk(not any(l.startswith("SMBPAD") for l in smbus(h)) and qget(h, "tp", "version-reads") == 0,
            "ps2: the PS/2 driver decided: the SMBus pad is left alone")
    finally:
        h.close()
    h, log = start(["-append", "HarnessPs2=1 SmbusTouchpad=0x2c", "-device", "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"],
                   "ps2ov", "q35")
    try:
        chk(any(l.startswith("SMBPAD 0") for l in smbus(h)), "ps2: SmbusTouchpad=0x2c uses the pad anyway")
    finally:
        h.close()


def sc_switch():
    rmi = ["-device", "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"]
    h, log = start(["-append", "HarnessPs2=2", "-acpitable", "file=" + ssdt_mouse("LEN2054")] + rmi, "switch1", "q35")
    try:
        chk("InterTouch (SMBus)" in log and "PS2 fake: F5" in log and "takeover=1" in log,
            "switch: Synaptics InterTouch, listed id: PS/2 side off (F5), taken over")
        chk(any(l.startswith("SMBPAD 0") and "click=1" in l for l in smbus(h)), "switch: the pad on SMBus, click pad from PS/2")
        ev(h, 0x4000, 0x4000, left=True); time.sleep(0.06); ev(h, left=False); time.sleep(0.4)
        chk([e["flags"] for e in mouse_lines(h.collect(0.4)) if e["flags"]] == [1, 2], "switch: tap")
    finally:
        h.close()
    h, log = start(["-append", "HarnessPs2=2"] + rmi, "switch2", "q35")
    try:
        chk("not on Linux's InterTouch list" in log and "takeover=0" in log and "PS2 fake: F5" not in log,
            "switch: InterTouch pad without a listed id stays on PS/2")
        chk(qget(h, "tp", "version-reads") == 0, "switch: its SMBus side is not touched")
    finally:
        h.close()
    h, log = start(["-append", "HarnessPs2=2 Smbus=2"] + rmi, "switch3", "q35")
    try:
        chk("takeover=1" in log, "switch: Smbus=2 takes it without the list")
    finally:
        h.close()
    h, log = start(["-append", "HarnessPs2=3", "-device", "smbus-elantp,id=tp,bus=i2c,address=0x15"], "switch4", "q35")
    try:
        chk("Elan pad, firmware 5f2001, hardware 4, bus 4, SMBus" in log and "takeover=1" in log,
            "switch: Elan new IC -> SMBus (the PS/2 side stays disabled from the knock)")
        chk(qget(h, "tp", "enabled") == 1 and qget(h, "tp", "mode") == 1, "switch: Elan pad enabled on SMBus")
    finally:
        h.close()


def sc_off():
    h, log = start(["-append", "Smbus=0", "-device", "smbus-rmi4tp,id=tp,bus=i2c,address=0x2c"], "off", "q35")
    try:
        chk(not any(l.startswith("SMBHOST") for l in smbus(h)) and "smbus:" not in log, "off: Smbus=0, no SMBus access")
    finally:
        h.close()


ALL = ["i801rmi", "i801elan", "fchrmi", "fchelan", "list", "ps2", "switch", "off"]
for s in (sys.argv[1:] or ALL):
    print("== " + s)
    try:
        globals()["sc_" + s]()
    except Exception as e:
        chk(False, "%s: %s" % (s, e))
print("RESULT smbus %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
