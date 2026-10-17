# Changelog

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
