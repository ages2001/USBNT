# Development script: NT 3.51 images in $USBNT_NTIMG (~/ntimg), DDK/MSVC tree and outputs in $USBNT_DDK (/tmp/ddkt).
import time, sys, os, subprocess, shutil, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
NTIMG = os.environ.get("USBNT_NTIMG", os.path.expanduser("~/ntimg"))
DDK = os.environ.get("USBNT_DDK", "/tmp/ddkt")
os.environ.setdefault("NTVM_EXTRA", "-icount shift=auto,sleep=on")
import ntsetup
import mkimg
mkimg.make()
from ntvm import NtVm
img = NTIMG + "/nt351_inst.raw"
O = DDK + "/out/"
def setcmd(cmd, no_ps2):
    ntsetup.mtools_rc(img)
    tmp = tempfile.mkdtemp(); hive = os.path.join(tmp, "system")
    subprocess.check_call(["mcopy", "-o", "n:/WINNT351/system32/config/system", hive])
    ntsetup.set_values(hive, "\\Setup", {"CmdLine": "string:" + cmd, "SetupType": "dword:0x1"})
    for cs in ("ControlSet001", "ControlSet002"):
        if ntsetup.hivexsh(hive, "cd \\%s\\Services\\i8042prt\n" % cs).returncode == 0:
            ntsetup.set_values(hive, "\\%s\\Services\\i8042prt" % cs, {"Start": "dword:0x%x" % (4 if no_ps2 else 1)})
    if not no_ps2:
        pass
    else:
        for cs in ("ControlSet001", "ControlSet002"):
            if ntsetup.hivexsh(hive, "cd \\%s\\Services\\usbnt\n" % cs).returncode == 0:
                ntsetup.set_values(hive, "\\%s\\Services\\usbnt" % cs, {"DebugPort": "dword:0x2f8"})
            if ntsetup.hivexsh(hive, "cd \\%s\\Control\\CrashControl\n" % cs).returncode == 0:
                ntsetup.set_values(hive, "\\%s\\Control\\CrashControl" % cs, {"AutoReboot": "dword:0x0"})
    subprocess.check_call(["mcopy", "-o", hive, "n:/WINNT351/system32/config/system"])
stage = sys.argv[1]
if stage == "1":
    shutil.copyfile(NTIMG + "/nt351.raw", img)
    ntsetup.mtools_rc(img)
    subprocess.check_call(["mmd", "n:/USBBIN"])
    for f in ["usbnt.sys", "usbinst.exe", "usbeject.exe", "usbwheel.exe"]:
        subprocess.check_call(["mcopy", "-o", O + f, "n:/USBBIN/" + f])
    open(DDK + "/i1.bat","wb").write(b"@echo off\r\nc:\\usbbin\\usbinst /install > c:\\inst.out\r\nc:\\usbbin\\usbinst /status >> c:\\inst.out\r\ncopy c:\\inst.out c:\\inst2.out\r\necho done\r\n")
    subprocess.check_call(["mcopy", "-o", DDK + "/i1.bat", "n:/I1.BAT"])
    setcmd("cmd.exe /k c:\\i1.bat", False)
    vm = NtVm(["-drive","file=%s,format=raw,if=none,id=hd"%img,"-device","ahci,id=ahci","-device","ide-hd,drive=hd,bus=ahci.0"])
    try:
        time.sleep(90); vm.shot(DDK + "/i1.png"); time.sleep(60)
    finally:
        vm.close()
else:
    setcmd("cmd.exe", True)
    subprocess.check_call(["cp", "/tmp/usbt/stick16.img", DDK + "/i_st.img"])
    vm = NtVm(["-drive","file=%s,format=raw,if=none,id=hd"%img,"-device","ahci,id=ahci","-device","ide-hd,drive=hd,bus=ahci.0",
      "-device","pci-ohci,id=hc,num-ports=4","-drive","file=%s/i_st.img,format=raw,if=none,id=st" % DDK,"-device","usb-storage,bus=hc.0,drive=st",
      "-device","usb-kbd,bus=hc.0","-device","usb-mouse,bus=hc.0"])
    try:
        for i in range(60):
            time.sleep(5)
            if "keyboard class driver connected" in open(vm.com2, errors="replace").read(): break
        print(open(vm.com2, errors="replace").read())
        for i in range(4):
            time.sleep(5); vm.shot(DDK + "/i2_%d.png" % i)
        vm.typ("usbeject /list\n"); time.sleep(4); vm.typ("dir d:\n"); time.sleep(4)
        vm.shot(DDK + "/i2.png")
        print(open(vm.com2, errors="replace").read())
    finally:
        vm.close()
