import sys, time
from qharn import Harness
name=sys.argv[1]
CFG={"uhci":"piix3-usb-uhci","ohci":"pci-ohci","ehci":"usb-ehci","xhci":"qemu-xhci"}
h = Harness(["-device",CFG[name]+",id=hc","-device","usb-mouse,bus=hc.0","-device","usb-kbd,bus=hc.0"], log="/tmp/usbt/m.log")
try:
    h.wait("harness: ready", 60); h.wait("READY")
    for i in range(3):
        h.hmp("mouse_button 2"); time.sleep(0.3); h.hmp("mouse_button 0"); time.sleep(0.3)
    for l in h.collect(1): print(l)
    h.hmp("mouse_move 3 3"); h.hmp("mouse_button 1"); h.hmp("mouse_button 0")
    for l in h.collect(1): print(l)
finally: h.close()
