# Changelog

## v1.2

Changelog:

- Fixed the Shift key staying down after it was let go with a PS/2 keyboard on NT 3.1 - 4.0 (the left Shift's release code 0xAA was taken for the keyboard's self test result); this also fixes digits on layouts that type them with Shift (Czech, French, ...).
- USB keyboards: a Shift, Ctrl or Alt let go while the keyboard reports too many keys (roll over, ghosting) is let go at once.
- PS/2 mouse, touch pad and TrackPoint: the reset self test is waited for up to 2.5 s (slow devices took over 1 s and did not work), late bytes (a delayed AA 00, a packet under way) are skipped while waiting for an answer, and answers get 100 ms instead of 30 ms (NT 3.1 - 4.0 and the Windows 2000 / XP filter).
- PS/2 mouse: the wheel detection (IntelliMouse knock) is tried twice, for mice and KVMs that miss the first one.
- PS/2 mouse: IntelliMouse Explorer 4.0 and tilt wheel mice (6-bit vertical and horizontal wheel packets) no longer press back / forward when the wheel turns or tilts; their wheel scrolls the right way.
- Horizontal wheel (tilt wheels, USB mice with a horizontal wheel, touch pads; also PS/2 touch pads on Windows 2000 / XP) now scrolls: usbmon sends WM_HSCROLL to the window under the pointer on every version (no Windows before Vista has a message for it).
- PS/2 mouse: packets with an overflow bit keep their buttons (a click or a release in such a packet was lost, so double clicks failed with fast movement).
- Touch pads: a finger that may still be a tap no longer moves the pointer, so a double tap is a double click; taps may be up to 200 ms long.
- Touch pads (PS/2 Synaptics / Elantech / ALPS, I2C, SMBus): three fingers swiped right / left are back / forward (TouchpadSwipe); two finger sideways scrolling now reaches programs (WM_HSCROLL through usbmon), also with PS/2 pads on Windows 2000 / XP.
- Windows 2000 / XP text-mode setup: fixed no USB keyboard (and USB install media) in text-mode setup on Intel 7 series and later chipsets (Ivy Bridge and newer) with the "USB 3 xHCI, USB4, card readers" choice, while the installed system worked: without usbinst's setting the USB 2.0 side of the ports was moved to the xHCI, away from the EHCI that setup reads the keyboard through. Windows 2000 / XP now default to IntelRouteToXhci = 2 (only SuperSpeed to the xHCI); the search for the keyboard class driver setup reads is also tried again while text-mode setup runs.
- Windows XP / XP x64 / 2003 text-mode setup: USB keyboards and mice are given to setup's own USB input drivers (hidusb, kbdhid, mouhid, which TXTSETUP.SIF loads and binds by class), as xhci98 does, instead of USBNT being the keyboard itself, so setup reads them like a keyboard on its own UHCI / EHCI controllers; composite devices are split by USBNT. Windows 2000 keeps USBNT's own keyboard (its setup would copy hidusb.sys without its imports and stop at the first start). TextSetupHid chooses.
- Added Thunderbolt 5 / USB4 v2 (Intel Barlow Ridge 80G / 40G): Gen 4 links (2 x 40 = 80 Gbit/s, bonded by the hardware), the routers programmed for a USB4 v2 connection manager, PCIe extended encapsulation between USB4 v2 routers; USB4 v1 and Thunderbolt 3 devices on a Thunderbolt 5 host.
- Added Thunderbolt 1 and 2 (Light, Cactus, Redwood, Win and Falcon Ridge) and Thunderbolt 1 / 2 / 3 host routers without a firmware connection manager (Apple Macs): USBNT is the connection manager there (plug events, link controller, lane bonding from Thunderbolt 2 on). Thunderbolt 1 / 2 controllers of PCs are left to the BIOS (TbtDriverCm).
- Added the Thunderbolt 4 / USB4 controllers of Lunar Lake, Panther Lake and Wildcat Lake (USBNT is the connection manager) and Alpine Ridge in USB only mode.
- Thunderbolt: fixed the path credits of a link that was bonded already when the router was found (they were those of one lane).
- Device Manager (Windows 2000 / XP): USB controllers, root hubs and hubs are named by their USB version ("Intel USB 1.1 Universal Host Controller - 24C2 (USBNT)", "USB 2.0 Root Hub (USBNT)", "USB 1.1 Hub (USBNT)"); Thunderbolt / USB4 host interfaces by their chip ("USBNT Thunderbolt 5 (Barlow Ridge 80G) Host Router").
- Device Manager: devices for Windows' own drivers that had no product name showed as "USB Device"; they get the manufacturer and the class ("USB Printer", "USB Vendor Specific Device (VID_0414 PID_A00B)"), and every function of a composite device says which interfaces it has ("Plugable UD-3900 - Audio Control (Interface 0)"), also for device nodes from earlier versions that have no driver.
- wheeltst shows double clicks.
- Tests: shift keys, double clicks of PS/2 and USB mice, double taps of PS/2 touch pads, and a PS/2 mouse with a slow self test (QEMU patch test/qemu/ps2-mouse-bat.patch).

## v1.1

Changelog:

- Added Windows 2000, XP and XP x64 Edition support (Plug and Play driver for x86 and x64).
- Windows 2000 / XP: USBNT drives what Windows cannot (xHCI, EHCI before 2000 SP4 / XP SP1, USB4 / Thunderbolt, card readers, I2C) and leaves the controllers Windows has drivers for to Windows; /all, /ehci and /noehci choose otherwise.
- Windows 2000 / XP: added a USB bus interface for Windows' own USB drivers (printers, serial adapters, game controllers and more on USBNT's controllers; usbvideo for cameras on XP SP2 and later, XP x64 and Server 2003 SP1).
- Windows 2000 / XP: host controllers, root hubs, hubs and USB devices show in Device Manager; controllers are named after their chips (Renesas uPD720200 / 720200A / 720201 / 720202, ASMedia, VIA, Etron, Fresco Logic, TI, Intel, AMD and more).
- Added driver disks for text-mode setup: USBNTNT.IMG (NT 3.1 - 4.0), USBNT2K.IMG (2000 / XP / 2003) and USBNT64.IMG (XP x64 / 2003 x64), nLite ready (every file in the root of the disk, also in the OEMDISK folder).
- Windows 2000 / XP text-mode setup works with a USB keyboard only (no PS/2 keyboard needed, e.g. a USB 3 keyboard on a Renesas card), with nLite integration and with an F6 driver disk.
- Windows 2000 / XP text-mode setup shows USB disks, so Windows can be installed onto them.
- Windows 2000 / XP: USB keyboard and mouse work in GUI-mode setup and at the logon screen after a text-mode setup installation.
- Added USB Audio Class 1.0 / 2.0 and USB MIDI (through the AUDIONT sound driver), with isochronous transfers on UHCI, OHCI, EHCI and xHCI.
- Added USB cameras (USB Video Class), the usbcam viewer and a Video for Windows / DirectShow capture driver (usbntcap.dll).
- Added SD card readers (SDHCI, Realtek PCIe and Realtek USB readers).
- Added Thunderbolt 3 / 4 and USB4 support (connection manager, PCIe and USB 3 tunnels, 40 Gbit/s lane bonding) and hot plugged NVMe drives.
- Added the PS/2 keyboard and mouse through usbnt.sys, so PS/2, USB and I2C input work at the same time.
- Added HID over I2C touch pads, touch screens and keyboards, with ACPI methods and GPIO lines for notebook touch pads.
- Added touch pads on SMBus (Synaptics RMI4 and Elan; Intel and AMD SMBus controllers) and PS/2 touch pads in their native protocols (Synaptics, Elantech v2 - v4, ALPS v3 / v5 / v7).
- Added two-finger scrolling and tap-to-click on touch pads, back / forward mouse buttons and autoscroll with the wheel button on every NT version.
- Every partition of a USB disk with several partitions gets a drive letter.
- 64-bit PCI BARs that UEFI firmware put above 4 GB are moved below 4 GB.
- Added the usbmon window, icons and the usbtree report with the driver log.

## v1.0

Changelog:

- Initial Release!
