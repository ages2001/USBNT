#!/bin/bash
# Builds the QEMU test kernel and runs every harness test.
cd "$(dirname "$0")"
HCS="xhci ehci uhci ohci" ../harness/build.sh >/dev/null || exit 1
for hc in xhci ehci uhci ohci; do python3 tbasic.py $hc 2>&1 | grep RESULT; done
# QEMU's usb-hub is full speed and cannot sit on a stand-alone EHCI bus
for hc in qemu-xhci piix3-usb-uhci pci-ohci; do python3 thotplug.py $hc 2>&1 | grep RESULT; done
for k in uhci ohci; do python3 tcompan.py $k 2>&1 | grep RESULT; done
python3 tmisc.py 2>&1 | grep RESULT
for hc in xhci ehci uhci ohci; do python3 tbig.py $hc 2>&1 | grep RESULT; done
for hc in xhci ehci; do python3 tbig.py $hc uas 2>&1 | grep RESULT; done
for hc in xhci ehci uhci ohci; do python3 tyank.py $hc 2>&1 | grep RESULT; done
for hc in xhci ehci; do python3 tyank.py $hc uas 2>&1 | grep RESULT; done
