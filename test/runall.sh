#!/bin/bash
# Builds the QEMU test kernel and runs every harness test.
cd "$(dirname "$0")"
HCS="xhci ehci uhci ohci" ../harness/build.sh >/dev/null || exit 1
for hc in xhci ehci uhci ohci; do python3 tbasic.py $hc 2>&1 | grep RESULT; done
for hc in xhci ehci uhci ohci; do python3 tmouse5.py $hc 2>&1 | grep RESULT; done
gcc -m32 -w -DUSB_HARNESS -I../src host/thidparse.c ../src/hidparse.c -o /tmp/thidparse && /tmp/thidparse | grep RESULT
gcc -m32 -w -DUSB_HARNESS -I../src host/taml.c ../src/aml.c ../src/gpio.c -o /tmp/taml && /tmp/taml | grep RESULT
gcc -m32 -w -DUSB_HARNESS -I../src host/tps2pad.c ../src/ps2pad.c ../src/ptp.c -o /tmp/tps2pad && /tmp/tps2pad | grep RESULT
gcc -m32 -w -DUSB_HARNESS -I../src host/tsmbtp.c ../src/smbtpdec.c -o /tmp/tsmbtp && /tmp/tsmbtp | grep RESULT
# QEMU's usb-hub is full speed and cannot sit on a stand-alone EHCI bus
for hc in qemu-xhci piix3-usb-uhci pci-ohci; do python3 thotplug.py $hc 2>&1 | grep RESULT; done
for k in uhci ohci; do python3 tcompan.py $k 2>&1 | grep RESULT; done
python3 tmisc.py 2>&1 | grep RESULT
for hc in xhci ehci uhci ohci; do python3 tbig.py $hc 2>&1 | grep RESULT; done
for hc in xhci ehci; do python3 tbig.py $hc uas 2>&1 | grep RESULT; done
for hc in xhci ehci uhci ohci; do python3 tyank.py $hc 2>&1 | grep RESULT; done
for hc in xhci ehci; do python3 tyank.py $hc uas 2>&1 | grep RESULT; done
# USB Audio: needs the QEMU with the usb-uac test device (see test/qemu/README.txt)
if [ -n "$QEMU" ] && "$QEMU" -device help 2>/dev/null | grep -q usb-uac; then
    while read -r args; do
        python3 tuac.py $args 2>&1 | grep RESULT | sed "s/\$/ [$args]/"
    done <<'LIST'
uhci 1
uhci 2 midi=on
uhci 1 feedback=off
uhci 1 implicit=on fbppm=1000
xhci 2 hs implicit=on fbppm=-700
ohci 1 midi=on
ohci 2
ehci 2 hs
ehci 2 hs hsint=2
ehci 2 hs hsint=4 fbppm=400
ehci-tt 1
ehci-tt 2 midi=on
ehci-tt 2 hs
xhci 2 hs midi=on
xhci 2 hs hsint=3
xhci 1 fbppm=-500
xhci 2
LIST
else
    echo "RESULT audio SKIPPED (set QEMU to a build with usb-uac)"
fi
# cameras, card readers, HID over I2C, PCIe hot plug: need the QEMU test devices
if [ -n "$QEMU" ] && "$QEMU" -device help 2>/dev/null | grep -q usb-uvc; then
    for hc in xhci ehci uhci ohci; do python3 tuvc.py $hc 2>&1 | grep RESULT; python3 tuvc.py $hc bulk 2>&1 | grep RESULT; done
    python3 tsd.py sdhc 2>&1 | grep RESULT
    python3 tsd.py sdsc spec=3 2>&1 | grep RESULT
    python3 trtsx.py all 2>&1 | grep "RESULT rtsx summary"
    python3 ti2c.py 2>&1 | grep "RESULT"
    python3 tacpitp.py 2>&1 | grep "RESULT"
    "$QEMU" -device help 2>/dev/null | grep -q tp-ps2pad && python3 tps2.py 2>&1 | grep "RESULT"
    python3 thotpci.py 2>&1 | grep "RESULT"
    python3 tnvme.py 2>&1 | grep "RESULT"
    python3 ttbt.py 2>&1 | grep "RESULT"
else
    echo "RESULT camera / card reader / I2C / hot plug SKIPPED (set QEMU to the test build)"
fi
# touch pads on SMBus: the SMBus test pads (test/qemu/smbus-touchpad.patch)
if [ -n "$QEMU" ] && "$QEMU" -device help 2>/dev/null | grep -q smbus-rmi4tp; then
    python3 tsmbus.py 2>&1 | grep "RESULT"
else
    echo "RESULT smbus SKIPPED (set QEMU to a build with smbus-rmi4tp)"
fi
