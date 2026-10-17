"""ACPI method execution for HID over I2C, on the bare metal harness.

The SSDT describes a touch pad the way notebook firmware does and nothing else:
  * its I2C address is chosen by a conditional _CRS (a "touch pad type" variable),
  * its HID descriptor register comes from _DSM through a Package,
  * _PS0 writes a SystemIO "power" register the QEMU i2c-hid test device needs
    before it answers its HID descriptor (gate-port), and _PS3 clears it,
  * _STA says the device is present.

So the device is found only when the AML interpreter runs the methods. With
I2cHidAml=0 the methods are not run: _PS0 never opens the gate and the _DSM
register is unknown, so the device is not found (today's fallback).

  python3 tacpitp.py [aml|off]      (default: both)

The GpioInt / GpioIo and pinctrl code is covered by the host unit test
(test/host/taml.c) against a fake MMIO controller.
"""
import os, struct, sys, time
from qharn import Harness

os.makedirs("/tmp/usbt", exist_ok=True)
fails = []
GATE = 0x1100
ADDR = 0x2C
DESC = 0x30


def chk(c, what):
    if not c:
        fails.append(what)
    print("%s %s" % ("ok  " if c else "FAIL", what))


# ------------------------------------------------------------------ AML encoder
def pkglen(n):
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


def seg(s):
    s = (s + "____")[:4]
    return s.encode()


def integer(v):
    if v == 0:
        return b"\x00"
    if v == 1:
        return b"\x01"
    if v <= 0xFF:
        return bytes([0x0A, v])
    if v <= 0xFFFF:
        return b"\x0B" + struct.pack("<H", v)
    return b"\x0C" + struct.pack("<I", v)


def nameobj(name, obj):
    return b"\x08" + seg(name) + obj


def method(name, argc, body):
    inner = seg(name) + bytes([argc]) + body
    return b"\x14" + pkglen(len(inner)) + inner


def device(name, body):
    inner = seg(name) + body
    return b"\x5B\x82" + pkglen(len(inner)) + inner


def scope(pathbytes, body):
    inner = pathbytes + body
    return b"\x10" + pkglen(len(inner)) + inner


def field(region, flags, name, bits):
    inner = seg(region) + bytes([flags]) + seg(name) + bytes([bits])
    return b"\x5B\x81" + pkglen(len(inner)) + inner


def opregion_io(name, port, length):
    return b"\x5B\x80" + seg(name) + b"\x01" + integer(port) + integer(length)


def i2c_desc(addr, speed, src):
    d = bytes([2, 0, 1, 0x02, 0, 0, 1]) + struct.pack("<HIH", 6, speed, addr) + src.encode() + b"\0"
    return bytes([0x8E]) + struct.pack("<H", len(d)) + d


def resbuf(descs):
    body = descs + b"\x79\x00"
    return b"\x11" + pkglen(len(integer(len(body)) + body)) + integer(len(body)) + body


def iff(pred, body):
    inner = pred + body
    return b"\xA0" + pkglen(len(inner)) + inner


def package(elems):
    inner = bytes([len(elems)]) + b"".join(elems)
    return b"\x12" + pkglen(len(inner)) + inner


def build_ssdt():
    body = b""
    body += nameobj("_HID", b"\x0D" + b"ATML1000\0")
    body += nameobj("_CID", b"\x0D" + b"PNP0C50\0")
    body += nameobj("TPTY", integer(1))                 # BIOS "touch pad type"
    body += opregion_io("PWIO", GATE, 4)
    body += field("PWIO", 0x03, "PWEN", 32)             # DWordAcc
    body += method("_STA", 0, b"\xA4" + integer(0x0F))
    body += nameobj("RTA_", resbuf(i2c_desc(ADDR, 400000, "\\_SB.PCI0.S28")))
    body += nameobj("RTB_", resbuf(i2c_desc(0x10, 100000, "\\_SB.PCI0.S28")))
    crs = iff(b"\x93" + seg("TPTY") + integer(1), b"\xA4" + seg("RTA_")) + b"\xA4" + seg("RTB_")
    body += method("_CRS", 0, crs)
    body += method("_DSM", 4, b"\xA4" + package([integer(DESC)]))
    body += method("_PS0", 0, b"\x70" + integer(1) + seg("PWEN"))
    body += method("_PS3", 0, b"\x70" + integer(0) + seg("PWEN"))
    dev = device("TPD9", body)
    sc = scope(b"\x5C" + seg("_SB"), dev)
    length = 36 + len(sc)
    hdr = b"SSDT" + struct.pack("<IB", length, 2) + b"\0" + b"USBNT " + b"ACPITP  " + \
        struct.pack("<I", 1) + b"USBN" + struct.pack("<I", 1)
    t = bytearray(hdr + sc)
    t[9] = (256 - sum(t) % 256) % 256
    p = "/tmp/usbt/acpitp.aml"
    open(p, "wb").write(bytes(t))
    return p


def mouse_lines(ls):
    out = []
    for l in ls:
        if l.startswith("MOUSE"):
            d = dict(kv.split("=") for kv in l.split()[1:])
            out.append({k: int(v, 16) if k == "flags" else int(v) for k, v in d.items()})
    return out


def start(extra, name):
    aml = build_ssdt()
    args = ["-acpitable", "file=" + aml,
            "-device", "dw-i2c-pci,id=i2c0,addr=05.0", "-append", "TouchpadGestures=0 " + extra,
            "-device", "i2c-hid,id=tp,bus=i2c0.0,address=0x%x,kind=ptp,desc-reg=0x%x,gate-port=0x%x" %
            (ADDR, DESC, GATE)]
    h = Harness(args, log="/tmp/usbt/acpitp_%s.log" % name)
    boot = h.wait("harness: ready", 90)
    for l in boot:
        if "i2c" in l or "aml" in l or "acpi" in l:
            print("  " + l)
    h.wait("READY")
    return h, boot


def qget(h, prop):
    return h.qmp_cmd("qom-get", path="/machine/peripheral/tp", property=prop)


def sc_aml():
    h, boot = start("", "aml")
    try:
        log = "\n".join(boot)
        chk("TPD9" in log and "descriptor register 0030 (_DSM)" in log,
            "aml: _DSM gave the descriptor register 0x30")
        chk("at 2c" in log.lower() or "address 2c" in log.lower() or ("%02x" % ADDR) in log.lower() or
            "2c on" in log, "aml: conditional _CRS chose address 0x2c")
        chk(qget(h, "gate-open") is True, "aml: _PS0 opened the power gate")
        r = h.cmd("i2c", 60)
        chk(any("I2CHID" in l and "addr=2c" in l and "reg=0030" in l for l in r),
            "aml: touch pad found at 0x2c, register 0x30")
        h.hmp("mouse_move 9 4")
        time.sleep(0.3)
        h.hmp("mouse_move -5 2")
        time.sleep(0.3)
        m = mouse_lines(h.collect(1.0))
        chk(sum(e["x"] for e in m) == 4 and sum(e["y"] for e in m) == 6, "aml: touch pad reports motion")
    finally:
        h.close()


def sc_off():
    h, boot = start("I2cHidAml=0", "off")
    try:
        chk(qget(h, "gate-open") is False, "off: _PS0 did not run (gate closed)")
        r = h.cmd("i2c", 60)
        chk(not any("I2CHID" in l for l in r), "off: device not found without AML (fallback)")
    finally:
        h.close()


ALL = ["aml", "off"]
for s in (sys.argv[1:] or ALL):
    print("== " + s)
    try:
        globals()["sc_" + s]()
    except Exception as e:
        chk(False, "%s: %s" % (s, e))
print("RESULT acpitp %s" % ("PASS" if not fails else "FAIL: " + ", ".join(fails)))
