"""Keeps an NtVm running and executes Python snippets sent through a FIFO (development helper).
usage: vmd.py <dir> <qemu args...>   then: vmc.sh <dir> 'vm.shot("/tmp/x.png")'"""
import os, sys, time, traceback
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ntvm import NtVm
d = sys.argv[1]
machine = os.environ.get("NTVM_MACHINE")
vm = NtVm(sys.argv[2:], com2=os.path.join(d, "com2.log"), machine=machine, mem=int(os.environ.get("NTVM_MEM", "128")), cpu=os.environ.get("NTVM_CPU", "pentium3"))
fifo = os.path.join(d, "cmd")
if os.path.exists(fifo):
    os.unlink(fifo)
os.mkfifo(fifo)
open(os.path.join(d, "ready"), "w").write("1")
open(os.path.join(d, "pid"), "w").write(str(os.getpid()))
g = {"vm": vm, "time": time}
while True:
    with open(fifo) as f:
        code = f.read()
    out = os.path.join(d, "out")
    try:
        if code.strip() == "quit":
            vm.close()
            open(out, "w").write("closed\n")
            break
        exec(code, g)
        open(out, "w").write("ok\n")
    except Exception:
        open(out, "w").write(traceback.format_exc())
    open(os.path.join(d, "done"), "w").write("1")
