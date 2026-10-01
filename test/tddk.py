# Development script: NT 3.51 images in $USBNT_NTIMG (~/ntimg), DDK/MSVC tree and outputs in $USBNT_DDK (/tmp/ddkt).
import time, sys, os, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
NTIMG = os.environ.get("USBNT_NTIMG", os.path.expanduser("~/ntimg"))
DDK = os.environ.get("USBNT_DDK", "/tmp/ddkt")
os.environ.setdefault("NTVM_EXTRA", "-icount shift=auto,sleep=on")
import ntsetup
img=NTIMG + '/nt351_ddk.raw'
ntsetup.prepare(NTIMG + '/nt351.raw', img, setup_cmd='cmd.exe /k c:\\go.bat', no_ps2=False)
ntsetup.mtools_rc(img)
subprocess.check_call("cd %s/root && mcopy -s -o -Q * n:/" % DDK, shell=True)
from ntvm import NtVm
vm = NtVm(["-drive","file=%s,format=raw,if=none,id=hd"%img,"-device","ahci,id=ahci","-device","ide-hd,drive=hd,bus=ahci.0"], com2=DDK + "/com2.log")
try:
    t0=time.time()
    while time.time()-t0 < int(sys.argv[1] if len(sys.argv)>1 else 600):
        time.sleep(30)
        vm.shot(DDK + "/s.png")
        r=subprocess.run(["mtype","n:/DONE.TXT"],capture_output=True)
        if b"ALLDONE" in r.stdout:
            ok = True
            for f in ["n:/OUT/usbnt.sys", "n:/OUT/wheeltst.exe"]:
                d = subprocess.run(["mtype", f], capture_output=True).stdout
                ok = ok and d[:2] == b"MZ"
            if ok:
                time.sleep(10); break
    vm.shot(DDK + "/s.png")
finally:
    vm.close()
