QEMU test devices used by the USBNT tests

usbnt-qemu-test.patch applies to QEMU 8.2.2 (git tag v8.2.2) and contains
everything (it replaces the older usb-audio-test.patch, which is kept for
reference):

  hw/usb/dev-uac.c       "usb-uac": USB Audio Class 1.0 / 2.0 test device
  hw/usb/hcd-ehci.c      siTD (split isochronous) execution
  hw/usb/dev-hub.c       "hs=on": high speed hub with a transaction translator
  hw/usb/dev-hid.c       "display=" / "head=": bind usb-kbd / usb-mouse to one
                         display head (several keyboards and mice in one VM)
  hw/usb/dev-uvc.c       "usb-uvc": USB Video Class camera (YUY2 and MJPEG,
                         isochronous or bulk, processing unit controls;
                         mjpeg420=on, quirks force-frame=N, small-maxframe=on)
  hw/usb/dev-rtsx-usb.c  "usb-rtsx": Realtek RTS5129/5139/5179 USB card reader
                         (vendor protocol); SD card on bus <id>.0
  hw/usb/rtsx-pci.c      "rtsx-pci": Realtek PCIe card reader (command buffer,
                         ADMA, HAIMR, PHY window); device-id= any of the 14
                         chip ids; SD card on bus <id>.0
  hw/usb/rtsx-engine.h   the SD engine shared by both Realtek models
  hw/i2c/dw_i2c_pci.c    "dw-i2c-pci": Intel LPSS DesignWare I2C controller
                         (starts in LPSS reset); I2C bus <id>.0
  hw/i2c/i2c_hid_dev.c   "i2c-hid": HID over I2C device, kind=mouse|ptp|tpad|
                         touch|kbd, desc-reg=, repeat=on, display=,
                         gate-port=<ioport>: a SystemIO "power gate" - the
                         device answers an all-zero (invalid) HID descriptor
                         until that I/O port is written non-zero (what an ACPI
                         _PS0 does); "gate-open" reads the gate state. The full
                         updated device is in test/qemu/i2c_hid_dev.c (used by
                         test/tacpitp.py); drop it into hw/i2c/ and rebuild.
  hw/usb/usb4-nhi.c      "usb4-nhi": Thunderbolt / USB4 host interface, control
                         ring with CRC checks; mode=usb4 (USB4 host router and
                         device routers, the driver is the connection manager)
                         or fr|ar|tr|icl|tgl (Intel firmware connection manager),
                         security=0..2; run time: plug0/plug1, kind0/kind1,
                         devkey0, pcie0/usb3-0/approved0/host-clean, violations
  meson.build changes

Build (only i386 is needed):

  git clone -b v8.2.2 https://gitlab.com/qemu-project/qemu.git && cd qemu
  patch -p1 < usbnt-qemu-test.patch
  ./configure --target-list=i386-softmmu && make -j4

Then: QEMU=$PWD/build/qemu-system-i386 test/runall.sh

Examples:
  -device usb-uvc,bus=hc.0[,bulk=on][,mjpeg420=on][,force-frame=1]
  -device usb-rtsx,id=crw,bus=hc.0 -drive if=none,id=c,file=sd.img,format=raw
      -device sd-card,drive=c,bus=crw.0
  -device rtsx-pci,id=rts,device-id=0x525A -device sd-card,drive=c,bus=rts.0
  -device dw-i2c-pci,id=i2c0 -device i2c-hid,bus=i2c0.0,address=0x2c,kind=ptp
  -machine q35 -device smbus-rmi4tp,bus=i2c,address=0x2c
  -device amd-fch-smbus,addr=14.0 -device smbus-elantp,bus=fchsmb1,address=0x15
  -device usb4-nhi,id=tb,mode=usb4 -device pcie-root-port,id=rp1,slot=1
      (q35; qom-set tb plug0 true, wait for qom-get tb pcie0, then
       device_add nvme,bus=rp1,...)
  -acpitable file=ssdt.aml (test/ti2c.py builds an SSDT that describes an
      I2C touch screen the way notebook firmware does)
  q35: -global ICH9-LPC.acpi-pci-hotplug-with-bridge-support=off
      -device pcie-root-port,id=rp1,chassis=1,slot=1,bus-reserve=4,mem-reserve=16M
      then QMP device_add qemu-xhci,bus=rp1 (native PCIe hot plug)

usb-uac properties:
  uac=1|2        class version (1 = full speed only)
  hs=on|off      high speed (UAC2)
  hsint=1..4     high speed bInterval (8, 4, 2 or 1 packets per ms)
  capture=on     capture interface (1000 Hz left, 1500 Hz right sine)
  feedback=on    asynchronous playback with a feedback endpoint (off = adaptive)
  fbppm=N        device clock offset in ppm (feedback value, capture rate)
  implicit=on    asynchronous playback without a feedback endpoint: the
                 host must follow the capture stream (implicit feedback)
  ratemask=7     bit 0 44.1 kHz, bit 1 48 kHz, bit 2 96 kHz
  midi=on        USB MIDI streaming interface with a bulk OUT port
  debug=1        log class requests, interface changes and stream
                 statistics ("uac: ...") to stderr

smbus-touchpad.patch (also applies on its own): touch pads on SMBus
(test/tsmbus.py):

  hw/i2c/smbus_rmi4tp.c  "smbus-rmi4tp": Synaptics RMI4 pad on SMBus (version
                         register, mapping table, page description table, F01,
                         F12 with register descriptors or f11=on, F30 or f3a=on
                         buttons); bus=i2c (q35 ICH9 SMBus), address=0x2c
  hw/i2c/smbus_elantp.c  "smbus-elantp": Elan pad on SMBus (hello packet,
                         enable, absolute mode, range, report A8h), address=0x15
  hw/i2c/smbus_fch.c     "amd-fch-smbus": AMD FCH SMBus host (1022:790B, rev=):
                         PM registers at 0CD6h / FED80300h, PIIX4 style
                         transfers, semaphore, two ports "fchsmb0" / "fchsmb1"
  Input of both pads as i2c-hid kind=ptp (absolute position + left = finger 1,
  right = finger 2, middle = click); QOM read-outs configured, irq-enable,
  reports, bad / enabled, mode, reports, bad / transactions, bad, port.

usb-uvc-uvc10.patch (on top of usbnt-qemu-test.patch): "uvc10=on" makes the
usb-uvc camera a UVC 1.0 device (bcdUVC 0x0100, 26 byte probe / commit);
test/w5wincam.py uses it with W5_UVC=,uvc10=on.

tp-ps2pad.patch (on top of the others; the tree under /home/claude/qemu-src
has it): "tp-ps2pad", a PS/2 touch pad in place of the i8042's PS/2 mouse
(hw/input/tp_ps2pad.c, a 5 line hook in hw/input/ps2.c / ps2.h):
  -device tp-ps2pad,id=tp,proto=synaptics|elantech|alps[,stick=on]
      Synaptics TouchPad 8.1 (W mode, advanced gesture mode, pass-through
      port), Elantech v4, ALPS v7 (command mode registers); stick=on adds a
      trackpoint.  Fingers: qom-set /machine/peripheral/tp touch "x:y [x:y]
      [b=N]" (per mille of the pad, y downwards; "" = no finger), "s=dx:dy[:b]"
      a trackpoint packet; qom-get mode = 1 once the pad is absolute.
      test/tps2.py uses it.

ps2-kbd-absent.patch (on top of the others; the tree under /home/claude/qemu-src
has it): "absent" property of the i8042's PS/2 keyboard, for a machine with a
keyboard controller but no PS/2 keyboard plugged in (what a PC with only a USB
keyboard is):
  -global ps2-kbd.absent=on
      the keyboard answers no command (the controller times out, as with no
      keyboard on the port) and no key of any display reaches it.  QEMU
      without the i8042 at all (-machine pc,i8042=off) is no such machine:
      Windows 2000's NTDETECT does not come back there.  test/tnlite.py uses
      it: every key text-mode setup reads comes from the USB keyboard.
