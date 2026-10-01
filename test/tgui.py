# Development script: NT 3.51 images in $USBNT_NTIMG (~/ntimg), DDK/MSVC tree and outputs in $USBNT_DDK (/tmp/ddkt).
import time, sys, os, socket, threading, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
NTIMG = os.environ.get("USBNT_NTIMG", os.path.expanduser("~/ntimg"))
DDK = os.environ.get("USBNT_DDK", "/tmp/ddkt")
os.environ.setdefault("NTVM_EXTRA", "-icount shift=auto,sleep=on")
os.environ["USBNT_SYS"] = DDK + "/out/usbnt.sys"
os.environ["NTVM_SOCK"] = "/tmp/usbt/com2g.sock"
import ntsetup
import mkimg
mkimg.make()
img = NTIMG + "/nt351_gui.raw"
O = DDK + "/out/"
subprocess.check_call(["cp", "/tmp/usbt/stick16.img", DDK + "/g1.img"])
subprocess.check_call(["cp", "/tmp/usbt/stick2.img", DDK + "/g2.img"])
ntsetup.prepare(NTIMG + '/nt351.raw', img, params={'DebugPort':0x2f8,'DebugLevel':3}, setup_cmd='cmd.exe', no_ps2=True, start={"Cdfs":1},
    extra_files=[(O+"usbwheel.exe","USBWHEEL.EXE"),(O+"wheeltst.exe","WHEELTST.EXE"),(O+"usbeject.exe","USBEJECT.EXE")])
from ntvm import NtVm
vm = NtVm(["-drive","file=%s,format=raw,if=none,id=hd"%img,"-device","ahci,id=ahci","-device","ide-hd,drive=hd,bus=ahci.0",
  "-device","qemu-xhci,id=hc","-drive","file=%s/g1.img,format=raw,if=none,id=st" % DDK,"-device","usb-storage,bus=hc.0,drive=st,id=stick",
  "-drive","if=none,id=cd,media=cdrom,format=raw,file=/tmp/usbt/cd.iso","-device","usb-storage,bus=hc.0,drive=cd",
  "-device","usb-kbd,bus=hc.0,id=ukbd","-device","usb-mouse,bus=hc.0,id=umouse"])
t0=time.time(); lines=[]
def reader():
    s=socket.socket(socket.AF_UNIX)
    for i in range(100):
        try: s.connect(os.environ["NTVM_SOCK"]); break
        except OSError: time.sleep(0.1)
    buf=b""
    while True:
        d=s.recv(4096)
        if not d: break
        buf+=d
        while b"\n" in buf:
            l,buf=buf.split(b"\n",1); lines.append("%7.2f %s"%(time.time()-t0,l.decode("latin1").strip()))
threading.Thread(target=reader,daemon=True).start()
def waitlog(txt, to=120):
    end=time.time()+to
    while time.time()<end:
        if any(txt in l for l in lines): return True
        time.sleep(0.5)
    return False
try:
    waitlog("keyboard class driver connected"); time.sleep(12)
    vm.typ("c:\\usbeject /list\n"); time.sleep(4)
    vm.typ("dir e:\n"); time.sleep(4)
    vm.shot(DDK + "/g_list.png")
    vm.typ("c:\\usbeject d:\n"); time.sleep(5)
    vm.cmd("device_del", id="stick"); waitlog("device removed", 30)
    vm.hmp("drive_add 0 if=none,id=st2,format=raw,file=%s/g2.img" % DDK)
    vm.cmd("device_add", driver="usb-storage", bus="hc.0", drive="st2", id="stick2"); time.sleep(8)
    vm.typ("cls\n"); vm.typ("dir d:\n"); time.sleep(5)
    vm.shot(DDK + "/g_swap.png")
    vm.typ("start c:\\usbwheel\n"); time.sleep(4); vm.typ("start c:\\wheeltst\n"); time.sleep(6)
    vm.mouse(wheel=-2); time.sleep(1); vm.mouse(wheel=1); time.sleep(2); vm.mouse(dx=40,dy=30); time.sleep(3)
    vm.shot(DDK + "/g_wheel.png")
    time.sleep(30)
finally:
    vm.close()
open(DDK + "/gui.log","w").write("\n".join(lines))
