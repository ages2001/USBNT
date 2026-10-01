"""End-to-end NT test: boot NT 3.51 with usbnt.sys, type with the USB keyboard,
copy a file to the USB disk, move the mouse, then check the disk on the host."""
import os, sys, time, socket, threading, subprocess, filecmp
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("NTVM_EXTRA", "-icount shift=auto,sleep=on")
from ntvm import NtVm
import ntsetup
import mkimg
mkimg.make()
NTIMG = os.environ.get("USBNT_NTIMG", os.path.expanduser("~/ntimg"))

CFGS = {
 "xhci": (["-device","qemu-xhci,id=hc"], "hc.0", ""),
 "ehci": (["-device","ich9-usb-ehci1,id=ehci,addr=1d.7,multifunction=on",
           "-device","ich9-usb-uhci1,masterbus=ehci.0,firstport=0,addr=1d.0,multifunction=on",
           "-device","ich9-usb-uhci2,masterbus=ehci.0,firstport=2,addr=1d.1",
           "-device","ich9-usb-uhci3,masterbus=ehci.0,firstport=4,addr=1d.2"], "ehci.0", ",usb_version=1"),
 "uhci": (["-device","piix3-usb-uhci,id=hc,addr=0x10"], "hc.0", ""),
 "ohci": (["-device","pci-ohci,id=hc,num-ports=4"], "hc.0", ""),
}

def run(name, irq=0):
    hcargs, bus, kbdopt = CFGS[name]
    img = NTIMG + "/nt351_%s.raw" % name
    stick = "/tmp/usbt/nt_%s_stick.img" % name
    subprocess.check_call(["cp", "/tmp/usbt/stick16.img", stick])
    ntsetup.prepare(NTIMG + "/nt351.raw", img, params={"DebugPort": 0x2f8, "DebugLevel": 3, "UseInterrupts": irq},
                    setup_cmd="cmd.exe", no_ps2=True,
                    extra_files=[(os.environ.get("USBNT_EJECT") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "out", "usbeject.exe"), "USBEJECT.EXE")])
    sock = "/tmp/usbt/com2_%s.sock" % name
    os.environ["NTVM_SOCK"] = sock
    vm = NtVm(["-drive","file=%s,format=raw,if=none,id=hd" % img,"-device","ahci,id=ahci","-device","ide-hd,drive=hd,bus=ahci.0"] + hcargs +
              ["-drive","file=%s,format=raw,if=none,id=st" % stick,"-device","usb-storage,bus=%s,drive=st,id=stick" % bus,
               "-device","usb-kbd,bus=%s,id=ukbd%s" % (bus, kbdopt),"-device","usb-mouse,bus=%s,id=umouse%s" % (bus, kbdopt)])
    lines = []
    t0 = time.time()
    def reader():
        s = socket.socket(socket.AF_UNIX)
        for i in range(100):
            try: s.connect(sock); break
            except OSError: time.sleep(0.1)
        buf = b""
        while True:
            d = s.recv(4096)
            if not d: break
            buf += d
            while b"\n" in buf:
                l, buf = buf.split(b"\n", 1)
                lines.append("%7.2f %s" % (time.time() - t0, l.decode("latin1").strip()))
    threading.Thread(target=reader, daemon=True).start()
    def waitlog(txt, to=150):
        end = time.time() + to
        while time.time() < end:
            if any(txt in l for l in lines): return True
            time.sleep(0.5)
        return False
    res = {}
    try:
        res["boot"] = waitlog("keyboard class driver connected")
        time.sleep(12)
        t1 = time.time()
        vm.typ("copy c:\\winnt351\\system32\\ntoskrnl.exe d:\\nt.bin\n"); time.sleep(8)
        vm.typ("dir d:\n"); time.sleep(3)
        vm.mouse(dx=60, dy=40); time.sleep(2)
        vm.typ("c:\\usbeject d:\n"); time.sleep(5)
        if irq:
            waitlog("interrupts in the first", 60)
        vm.shot("/tmp/usbt/nt_%s.png" % name)
        res["eject"] = any("ready for removal" in l for l in lines)
    finally:
        vm.close()
    open("/tmp/usbt/nt_%s.log" % name, "w").write("\n".join(lines))
    rc = "/tmp/usbt/mt_%s" % name
    open(rc, "w").write('drive s: file="%s" offset=32256\nmtools_skip_check=1\n' % stick)
    env = dict(os.environ, MTOOLSRC=rc)
    open(rc + "n", "w").write('drive n: file="%s" offset=32256\nmtools_skip_check=1\n' % img)
    subprocess.call(["mcopy", "-o", "s:/NT.BIN", "/tmp/usbt/%s_nt.bin" % name], env=env)
    subprocess.call(["mcopy", "-o", "n:/WINNT351/system32/ntoskrnl.exe", "/tmp/usbt/%s_k.bin" % name], env=dict(os.environ, MTOOLSRC=rc + "n"))
    res["copy"] = os.path.exists("/tmp/usbt/%s_nt.bin" % name) and filecmp.cmp("/tmp/usbt/%s_nt.bin" % name, "/tmp/usbt/%s_k.bin" % name, shallow=False)
    res["errors"] = [l for l in lines if "fail" in l or "error" in l or "timeout" in l]
    return res, lines

if __name__ == "__main__":
    name = sys.argv[1]
    irq = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    res, lines = run(name, irq)
    for l in lines:
        if "usbnt" in l: print(l)
    print("RESULT", name, "irq=%d" % irq, "PASS" if res["boot"] and res["copy"] and res.get("eject") and not res["errors"] else "FAIL", res)
