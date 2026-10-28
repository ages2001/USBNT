# USBNT - USB Driver for Windows NT 3.1 / 3.50 / 3.51 / 4.0, 2000 and XP (x86, x64)

A complete USB stack for Windows NT 3.1 - 4.0 (which have no USB support at
all), and a Plug and Play USB 3.x / USB4 / card reader driver for Windows
2000, XP and XP x64 (which have no xHCI support) - one code base, from the
host controller registers up to the keyboard, mouse, disk, sound and camera
devices.

## Contents

- [Overview](#overview) - [Highlights](#highlights) - [Supported systems](#supported-systems) - [Quick start](#quick-start)
- [Features](#features) - [Windows 2000 and XP](#windows-2000-and-xp) - [Architecture](#architecture)
- [Installation](#installation) - [Installing Windows with USBNT (text-mode setup)](#installing-windows-with-usbnt-text-mode-setup)
- [Usage](#usage) - [Configuration](#configuration) - [Building from source](#building-from-source)
- [Testing](#testing) - [Known limitations](#known-limitations) - [Changelog](#changelog) - [License](#license)

## Overview

USBNT is a USB stack for the first generation of Windows NT. A single
driver file, `usbnt.sys`, runs unchanged on **NT 3.1, NT 3.5, NT 3.51 and
NT 4.0** and covers every common USB host controller, keyboards, mice,
tablets, mass storage (including USB 3 SuperSpeed and UAS), USB audio
devices (USB Audio Class 1.0 and 2.0), USB MIDI, USB cameras (USB Video
Class), SD card readers (SDHCI, Realtek PCIe, Realtek USB), notebook touch
pads (HID over I2C, SMBus and PS/2), touch screens and keyboards on the I2C
bus, the PS/2 keyboard and mouse, and Thunderbolt 1 - 5 and USB4 (v1, v2)
docks and enclosures (it is the connection manager where the firmware is
not; NVMe drives and USB controllers behind them work).

None of these NT versions has USB support of its own.

On **Windows 2000 and XP** (32 bit, and XP x64 Edition) the same core runs
as a **Plug and Play driver** (`2kxp\i386\usbnt.sys`, `2kxp\amd64\usbnt.sys`)
next to Windows' own USB stack: Windows keeps the controllers it has drivers
for (UHCI and OHCI always, EHCI from Windows 2000 SP4 / XP SP1 on), USBNT
takes what Windows cannot drive - USB 3.x xHCI (which neither system
supports), EHCI on the older service packs, USB4 / Thunderbolt, card readers
and I2C - with every USBNT class above, and - through a USB bus interface of
its own - the devices USBNT has no driver for go to Windows' and vendors'
USB drivers (printers, serial adapters, game controllers, cameras on XP SP2
and later, ...). Its controllers, root hubs, hubs and devices show in Device
Manager. Windows 2000 / XP can also be **installed with USBNT** - a driver
disk for F6 or integration with nLite - with a USB keyboard as the only
keyboard and onto a USB disk. See [Windows 2000 and XP](#windows-2000-and-xp).

### Why?

Old NT installations on newer machines (and in virtual machines) often
have no PS/2 ports and no floppy drive left; USB is the only way to get a
keyboard, a mouse and removable storage. USBNT makes a USB-only PC usable
under NT 3.x and NT 4.0, and gives Windows 2000 / XP the USB 3 ports of
today's machines and add-in cards.

## Highlights

- **Host controllers**: UHCI, OHCI, EHCI and xHCI (USB 3.0 - 3.2, up to
  20 Gbit/s), BIOS hand-off, isochronous transfers on all of them,
  Thunderbolt 1, 2, 3, 4 and 5, USB4 and USB4 v2 (connection manager, PCIe
  and USB 3 tunnels, 80 Gbit/s Gen 4 links, hot plugged NVMe drives)
- **Input**: USB, PS/2 and I2C keyboards and mice, tablets, touch screens,
  touch pads with two-finger scrolling and tap-to-click, back / forward
  buttons and autoscroll on every NT version
- **Storage**: Bulk-Only, CBI and UAS disks, sticks, card readers and CD /
  DVD drives with hot plug, a drive letter for every partition, SD cards in
  SDHCI and Realtek readers, NVMe behind Thunderbolt / USB4; NT can boot
  from a USB disk
- **Multimedia**: USB Audio Class 1.0 / 2.0 sound cards and USB MIDI (with
  [AUDIONT](https://github.com/ages2001/AUDIONT)), UVC cameras with a
  viewer and a Video for Windows / DirectShow capture driver
- **Windows 2000 / XP**: Plug and Play driver for x86 and x64, Device
  Manager tree with the controllers named after their chips, a USB bus
  interface for Windows' own class drivers, text-mode setup driver disks
  (F6 and nLite) that work with a USB keyboard only
- **Tools**: installer, USB device tree with a report, safe removal,
  camera viewer, background monitor for drive letters and mouse extras

## Quick start

| You have | Do this |
|---|---|
| NT 3.1 / 3.5 / 3.51 / 4.0 installed | Copy the release to the machine, run `usbinst /install` as an administrator, restart. See [Installation](#installation). |
| Windows 2000 / XP / XP x64 installed | From the release folder: `usbinst /install` (x64: `2kxp\amd64\usbinst.exe /install`), restart. See [Windows 2000 and XP](#windows-2000-and-xp). |
| Windows 2000 / XP / 2003 to install, F6 | Press **F6**, then **S**, insert `USBNT2K.IMG` (x64: `USBNT64.IMG`) and pick a USBNT entry. |
| Windows 2000 / XP / 2003 to install, nLite | Add `OEMDISK\2KXP` (x64: `OEMDISK\XP64`) as a text-mode driver from its `TXTSETUP.OEM`, pick a USBNT entry. |
| NT 3.1 - 4.0 to install | Use `USBNTNT.IMG` at the mass storage step. See [Installing Windows with USBNT](#installing-windows-with-usbnt-text-mode-setup). |

The release folder has `usbnt.sys` and the tools for NT 3.x / 4.0, the
Windows 2000 / XP drivers and INF files in `2kxp`, and the driver disks
(`USBNTNT.IMG`, `USBNT2K.IMG`, `USBNT64.IMG`, and their files in `OEMDISK`).

## Features

**Host controllers**

- UHCI and OHCI (USB 1.1)
- EHCI (USB 2.0) with UHCI/OHCI companion hand-off and split transactions
  for low/full speed devices behind high speed hubs
- xHCI (USB 3.x): SuperSpeed on USB 3 ports, link rate from the
  controller's protocol speed table (5, 10 or 20 Gbit/s), multi-segment
  transfer rings, bulk streams. Intel chipsets: EHCI ports are routed to
  xHCI.
- BIOS hand-off (legacy USB) for all four controller types
- 64-bit BARs that UEFI firmware put above 4 GB are moved below 4 GB
- **Thunderbolt 1 - 5 and USB4 / USB4 v2**: the driver takes the control
  channel of the host interface (NHI, ring 0, polled):
  - Intel controllers with a firmware connection manager (Falcon, Alpine,
    Titan, Maple Ridge; Ice Lake, Tiger Lake, Alder / Raptor Lake, Meteor
    Lake when the BIOS leaves it on): "driver ready", then every device
    that is plugged in is approved - security level "user" - or paired
    with a 256 bit key that is kept in the registry and checked with a
    HMAC-SHA-256 challenge on later connections - level "secure"; a
    device that answers with another key is not connected. Ice Lake is
    force-powered first.
  - USB4 hosts without one (AMD, Intel Lunar Lake, Panther Lake, Wildcat
    Lake, the **Thunderbolt 5** controllers (Barlow Ridge, USB4 v2), Intel
    when the BIOS chose the OS connection manager, others) and Thunderbolt
    1 / 2 / 3 hosts whose firmware does not do it (**Apple Macs**: Light,
    Cactus, Falcon, Alpine, Titan Ridge): **the driver is the connection
    manager**. It enumerates the routers (also chained ones and those
    that are there at boot), gives them routes, programs the hop tables
    of a PCIe tunnel and a USB 3 tunnel (with the host's USB 3 bandwidth
    handshake) to each of them, handles plug and unplug events and tears
    the tunnels down when a router goes away. Thunderbolt 3 devices on a
    USB4 host get a PCIe tunnel too. Lanes are bonded (40 Gbit/s) through
    the USB4 port or the Thunderbolt 2 / 3 link controller; Thunderbolt 1
    links stay single.
  - **Thunderbolt 5 / USB4 v2**: the routers are told that the connection
    manager knows USB4 v2 (so they show their v2 features), Gen 4 links
    (2 x 40 = 80 Gbit/s) come up bonded and their paths get the credits of
    a bonded link, a PCIe tunnel between two USB4 v2 routers on a Gen 4
    link uses extended encapsulation; USB4 v1 and Thunderbolt 3 devices on
    a Thunderbolt 5 host work as before.
  - Thunderbolt 1 / 2 controllers of PCs (Light, Cactus, Redwood, Win
    Ridge) are run by the BIOS ("BIOS assist"): USBNT leaves them alone
    (`TbtDriverCm`) and picks up the devices through PCIe hot plug. Alpine
    Ridge in USB only mode needs nothing.
  - What comes through the PCIe tunnel arrives at a hot plug capable PCIe
    port, which is watched: the functions behind it get bus numbers,
    bridge windows and BARs inside the range the firmware reserved; USB
    controllers in a dock start (polled), **NVMe drives** become disks
    (see below); when the dock goes away (or the port's attention button
    is pressed: 5 seconds to cancel, as PCIe has it) they stop and are
    removed. The USB 3 tunnel ends at the host's xHCI. `usbtree` shows
    the host routers, the devices and routers, their tunnels and state.
- **NVMe drives that come later** (in a Thunderbolt / USB4 enclosure or
  dock, ExpressCard and other PCIe hot plug slots): one admin and one I/O
  queue pair, polled, every namespace (512 byte to 4 KB blocks) a disk
  like a USB disk, with read, write, flush, large commands split by the
  drive's limit; safe removal (`usbeject`) shuts the drive down. NVMe
  drives in the machine from the start are left to **nvme2k** (an NVMe
  SCSI miniport for NT 3.1 - XP), which works next to USBNT; `NvmeAtBoot
  = 1` lets USBNT take them as well.
- Isochronous transfers on all four: UHCI and OHCI frame-list TDs, EHCI
  iTDs (high speed, high bandwidth endpoints) and siTDs (full speed
  devices behind a high speed hub's transaction translator), xHCI Isoch
  TRBs

**Devices**

- Hubs: USB 1.1, USB 2.0 and USB 3 (SuperSpeed) hubs, chained
- Keyboard: boot protocol, or any report protocol keyboard (modifier bits,
  key arrays and key bitmaps from the report descriptor); typematic repeat,
  Caps/Num/Scroll lock LEDs
- Mouse: buttons and wheel, back / forward buttons (buttons 4 and 5, also
  when a mouse reports them in a field of their own); **autoscroll** with
  the wheel button on every version (see *Back / forward buttons and
  autoscroll* below)
- Tablet / absolute pointer (QEMU and VirtualBox "USB Tablet")
- **PS/2 keyboard and mouse** are driven by usbnt.sys as well (the
  installer disables i8042prt), so every keyboard and pointer, PS/2, USB
  and I2C, works at the same time; wheel and 5-button PS/2 mice; slow
  mice, trackpoints and touch pads (a reset self test of over 1 s, late
  answers) are waited for
- **HID over I2C** (notebooks and tablets): touch pads - Windows Precision
  Touchpads are switched to touch pad mode and get gestures: one finger
  moves the pointer, **two fingers scroll** (up/down = wheel, left/right =
  horizontal wheel), a tap clicks (one finger left, two fingers right,
  three fingers middle), **three fingers swiped right / left are back /
  forward**, the pad button with two fingers on the pad is the
  right button; other pads in their mouse mode, or finger motion of pads
  without a mouse collection -, touch
  screens and pens (absolute pointer, tip = left button) and I2C
  keyboards. Controllers: the DesignWare I2C of Intel LPSS (Haswell ..
  current, Bay Trail / Cherry Trail; out of reset, clock per chipset) as a
  PCI function, controllers that the firmware only describes in ACPI
  (INT33C2/3, INT3432/3, 80860F41, 808622C1, AMD0010, AMDI0010 ...) and the
  fixed AMD FCH addresses. Devices come from the ACPI tables (PNP0C50 /
  ACPI0C50 with their I2cSerialBus resource and _DSM descriptor register).
  On the notebooks where the firmware only sets these at run time USBNT runs
  the ACPI methods itself with a small, evaluation-only AML interpreter (no
  global ACPI takeover): `_STA` (a device reported absent is skipped),
  `_CRS` when it is a method (a conditional _CRS that returns one of several
  resource templates by a BIOS "touch pad type" setting is followed), `_DSM`
  (the HID descriptor register, function 1), `_PS0` / `_PS3` (power on before
  use, off on shutdown) and `_INI`. It answers `_OSI` like Windows XP, `_REV`
  2 and `\_OS` "Microsoft Windows NT". Anything it cannot evaluate safely
  makes it fall back to the old behaviour (the usual touch pad / touch screen
  addresses are tried, `I2cHid = 2` turns that off). The power / reset
  **GpioIo** lines of the device (and the GeneralPurposeIO accesses of its
  `_PS0`) are driven, and its **GpioInt** "data ready" line is read (Intel
  pinctrl SPT..MTL / APL / GLK, Bay Trail / Cherry Trail, and AMD FCH GPIO):
  the input register is read only when the line is asserted instead of blindly
  every cycle, which falls back to polling when the pad cannot be read. NT
  takes no GPIO interrupt (it still polls on the timer, but with the cheap
  MMIO pad read). AML evaluation and GPIO use can be turned off (`I2cHidAml`,
  `I2cHidGpio`).
- **Touch pads on SMBus** (ThinkPad, HP, Dell, Asus ... with a Synaptics
  "InterTouch" or an Elan pad that is a PS/2 mouse and an SMBus device at the
  same time): the pad is switched from PS/2 to SMBus, where it gives every
  finger, and gets the same gestures as the I2C touch pads. Synaptics RMI4
  (F01, F11 / F12, F30 / F3A buttons, the trackpoint behind F03) and Elan.
  Controllers: Intel ICH / PCH SMBus (ICH2 .. current) and AMD SB800,
  Hudson-2, FCH; polled. See *Touch pads on SMBus* under [Usage](#usage).
- **USB cameras** (USB Video Class 1.0 - 1.5): MJPEG and YUY2, every frame
  size and rate the camera offers, isochronous and bulk cameras, brightness,
  contrast and the other processing unit controls. `usbcam.exe` shows the
  picture, takes photos (BMP or JPEG) and records AVI video (MJPEG or YUY2);
  it offers the formats it can show (MJPEG, YUY2, UYVY, NV12, I420, grey),
  not H.264.
- **SD card readers**: SD, SDHC and SDXC cards (1 and 4 bit, high speed) in
  SDHCI readers (PCI class 08 05: Ricoh, O2Micro, TI, JMicron, Intel, AMD,
  ...), Realtek PCIe readers (RTS5209, 5227, 522A, 5229, 5249, 524A, 525A,
  5260, 5261, 5228, 5264, RTL8411, 8411B, 8402) and Realtek USB readers with
  the vendor protocol (RTS5129 "USB2.0-CRW", RTS5139, RTS5179); all other
  USB card readers are mass storage and work as before. Cards show up as
  removable disks, card change is noticed. (eMMC is not supported.)
- Mass storage: Bulk-Only, CBI and **UAS** (USB Attached SCSI, with
  streams on xHCI: up to 8 commands in flight). USB sticks, external
  disks, multi-LUN card readers, USB CD/DVD drives. Hot plug and surprise
  removal. **Every partition** of a stick or disk with several (primary
  and logical MBR partitions; GPT where Windows can read it) gets a drive
  letter, see *Partitions* under [Usage](#usage).

- Audio: **USB Audio Class 1.0 and 2.0** - full speed USB 1.1 sound cards
  and headsets as well as modern high speed UAC2 interfaces and DACs.
  Terminal / unit topology, clock sources, selectors and multipliers,
  alternate setting and sample rate selection (UAC1 endpoint control or
  UAC2 clock source, 8 kHz to 384 kHz, 8/16/24/32-bit PCM and 32-bit
  float, 1 to 8 channels), feature unit volume and mute, capture source
  selectors, asynchronous devices with explicit feedback (10.14 / 16.16
  auto-detected) or implicit feedback (paced by their capture stream),
  adaptive and synchronous devices, playback and capture at the same time,
  a transfer queue that grows when the system is slow to service it, hot
  plug. The sound itself is played by the
  [AUDIONT](https://github.com/ages2001/AUDIONT) sound driver, which uses
  USBNT's audio interface.
- USB MIDI: output port (running status, SysEx), e.g. to synthesizers and
  keyboards, through AUDIONT's MIDI device

USB disks are exposed as a SCSI port, so NT's own disk and CD-ROM class
drivers and file systems (FAT, NTFS, CDFS, or any installed third-party
file system) work on them.

**Speed**

- Requests are queued to the controller, so the next transfer is ready
  when the current one finishes.
- Page lists go straight to EHCI/xHCI (no copying); 1 MB per SCSI command
  on SuperSpeed and UAS disks, 120 KB on high speed, 64 KB on full speed.
  No per-port throttling.
- `usbmon.exe` sets a 1 ms system timer while it runs, so polled
  completions are not held back by the 10-15 ms clock tick.

**User mode tools**

| Tool | Purpose |
|---|---|
| `usbinst.exe` | Installs / removes the driver, sets parameters |
| `usbmon.exe` | Started at logon (no window): dynamic drive letters, wheel support for NT 3.x, back / forward buttons on NT 3.x / 4.0, autoscroll, 1 ms timer. *USB Monitor* in the program group (`usbmon /show`) opens its window: driver state, disks and their letters, what it did |
| `usbtree.exe` | GUI tree of controllers, ports, hubs and devices with USB version, speed, disk size, file system (FAT12/16/32, exFAT, NTFS with its version, HPFS, ReFS, ext2/3/4, Btrfs, XFS, HFS+, APFS ...), cluster size; SD card readers, HID over I2C devices, hot plugged controllers; safe removal; *Save report...* (or `usbtree /dump FILE`) writes everything and the driver's log to a text file |
| `usbcam.exe` | Camera viewer: live picture, photo (BMP / JPEG), video (AVI), camera controls |
| `usbntcap.dll` | Video for Windows capture driver of the USB cameras (NT 3.51 and later; on 2000 / XP also DirectShow), see *Camera* below |
| `usbeject.exe` | Command line safe removal |
| `wheeltst.exe` | Mouse test window: counts wheel, scroll, back / forward (X button, WM_APPCOMMAND or Alt+Left / Alt+Right) and click messages, shows whether autoscroll runs |

## Supported systems

| System | Driver | Notes |
|---|---|---|
| Windows NT 3.1, 3.5, 3.51, 4.0 (x86) | `usbnt.sys` (one binary) | Full USB stack, PS/2 and I2C input; details below |
| Windows 2000 (SP0 - SP4), XP (RTM - SP3), Server 2003 (x86) | `2kxp\i386\usbnt.sys` | Plug and Play, next to Windows' own USB drivers |
| Windows XP x64 Edition, Server 2003 x64 | `2kxp\amd64\usbnt.sys` | Plug and Play, next to Windows' own USB drivers |

On NT 3.x / 4.0:

| | NT 3.1 | NT 3.5 | NT 3.51 | NT 4.0 |
|---|---|---|---|---|
| Driver (one binary) | yes | yes | yes | yes |
| Keyboard, mouse, tablet | yes | yes | yes | yes |
| Mouse wheel | via usbmon (WM_VSCROLL) | via usbmon | via usbmon | native WM_MOUSEWHEEL |
| Back / forward buttons | Alt+Left / Alt+Right (usbmon) | Alt+Left / Alt+Right | Alt+Left / Alt+Right | Alt+Left / Alt+Right |
| Autoscroll (wheel button) | usbnt's mice, WM_VSCROLL | usbnt's mice, WM_VSCROLL | usbnt's mice, WM_VSCROLL | every mouse (SP3+: hook), WM_MOUSEWHEEL |
| Mass storage, UAS, hot plug | yes | yes | yes | yes |
| SD card readers | yes | yes | yes | yes |
| Cameras (usbcam) | yes | yes | yes | yes |
| PS/2 keyboard and mouse through usbnt | yes | yes | yes | yes |
| HID over I2C | yes | yes | yes | yes |
| Touch pads on SMBus | yes | yes | yes | yes |
| Dynamic drive letters | yes | yes | yes | yes, Explorer refreshes itself |
| USB audio and MIDI (with AUDIONT) | yes | yes | yes | yes |
| Camera in Video for Windows programs (usbntcap.dll) | - | - | yes | yes |

The driver imports only kernel and HAL functions that exist in every
version from NT 3.1 RTM to NT 4.0. Entry points that appeared later
(`HalGetBusDataByOffset`, for example) are looked up at run time; NT 3.1,
whose HAL has no PCI support, gets PCI configuration mechanism #1 through
ports 0CF8h/0CFCh. The build scripts check every import against the
export lists of all five kernels (3.1 RTM, 3.1 SP3, 3.5, 3.51, 4.0) and
reject P6-only instructions, so the binaries also run on 486 and Pentium
machines.

## Windows 2000 and XP

USBNT 1.1 adds a Plug and Play build for **Windows 2000, XP and XP x64
Edition**. Everything the NT 3.x / 4.0 driver does works there too, as
Plug and Play devices:

| Device Manager shows | What |
|---|---|
| *Chip* USB 3.x eXtensible Host Controller - *id* (USBNT) | xHCI controllers (USB 3.0 - 3.2, 5 / 10 / 20 Gbit/s); add-in chips by name ("Renesas uPD720202 USB 3.0 Host Controller (USBNT)") |
| *Chip* USB 2.0 Enhanced Host Controller - *id* (USBNT) | EHCI controllers where Windows has no EHCI driver (see the table below) |
| *Chip* USB 1.1 Universal / Open Host Controller - *id* (USBNT) | UHCI and OHCI controllers, only with `usbinst /install /all` |
| USB 1.1 / 2.0 / 3.x Root Hub (USBNT), USB 1.1 / 2.0 / 3.x Hub (USBNT) | root hubs and hubs, by their USB version |
| USBNT Thunderbolt 5 (Barlow Ridge 80G) Host Router, USBNT Thunderbolt 4 / USB4 (Meteor Lake) Host Router, ... | Thunderbolt 1 - 5 / USB4 host interfaces by their chip (connection manager, 40 / 80 Gbit/s links) |
| USBNT SD Card Reader (SDHCI / Realtek PCIe) | card readers |
| USBNT HID over I2C Controller | touch pads, touch screens, I2C keyboards |
| USBNT Device Bus (USB keyboards, mice, disks, UAS) | the bus on which USBNT reports what it serves |
| USBNT USB Keyboard, USBNT USB Mouse | every USB (and I2C) keyboard and pointer (kbdclass / mouclass) |
| *Product name* (USB 3.x UAS), *product name* (USB) | USB disks, sticks, card reader cards, NVMe drives behind Thunderbolt / USB4 - with Windows' disk.sys, so NTFS, FAT32 and drive letters work as usual |
| USBNT USB Audio | while a USB audio / MIDI device is connected: AUDIONT plays and records through it |
| *USB device names* with ids `USB\VID_v&PID_p` | devices USBNT has no driver for, for Windows' drivers (below): the product name; without one the manufacturer and the class ("USB Printer", "USB Vendor Specific Device (VID_0414 PID_A00B)"); a function of a composite device also says which interfaces it has ("Plugable UD-3900 - Audio Control (Interface 0)", `&MI_00`) |

**USB devices for Windows' drivers.** A device (or one function of a
composite device) that USBNT does not serve itself is reported with the
ids Windows' hub driver would give it - `USB\VID_v&PID_p&REV_r`,
`USB\VID_v&PID_p`, `USB\Class_c&SubClass_s&Prot_p`, `&MI_nn` for a
function - and its driver talks to it through the USB driver interface:
URBs with every transfer type (control, bulk, interrupt, isochronous),
configuration and interface selection, pipe abort / reset, descriptors,
features, vendor and class requests, the port requests and the USBDI bus
interface. So printers (usbprint), USB serial adapters, modems,
Bluetooth dongles, game controllers (hidusb), scanners and vendor drivers
work, also on xHCI ports, which Windows 2000 / XP do not support at all.
`UsbPdo` chooses: 1 (default) devices without a USBNT driver go to
Windows, 2 every device but the hubs goes to Windows' drivers (usbstor,
hidusb, usbaudio, ... over USBNT's controllers), 0 none.

**Which driver gets which controller.** Windows' own USB drivers stay
where Windows has them; USBNT drives the rest:

| Controller | 2000 SP0 - SP3 | 2000 SP4 | XP RTM | XP SP1 | XP SP2 / SP3 | XP x64, Server 2003 |
|---|---|---|---|---|---|---|
| UHCI, OHCI (USB 1.1) | Windows (uhcd / openhci) | Windows | Windows (usbuhci / usbohci) | Windows | Windows | Windows |
| EHCI (USB 2.0) | **USBNT** | Windows (usbehci) | **USBNT** | Windows (usbehci) | Windows | Windows |
| xHCI (USB 3.x) | **USBNT** | **USBNT** | **USBNT** | **USBNT** | **USBNT** | **USBNT** |
| USB4 / Thunderbolt, SD card readers, I2C and SMBus (touch pads) | **USBNT** | **USBNT** | **USBNT** | **USBNT** | **USBNT** | **USBNT** |
| Cameras behind USBNT's controllers | USBNT | USBNT | USBNT | USBNT | Windows (usbvideo) | Windows (usbvideo; Server 2003 from SP1) |

Windows 2000 got its USB 2.0 driver with SP4 (`usb2.inf`), XP with SP1;
before that only USB 2.0 packages of the time (Windows Update, OEMs) had
one - where such a package put `usbehci.sys` in, usbinst counts the EHCI
as Windows' too. In every case usbinst first checks that Windows really
has a driver for the controller (its INF files); a controller without one
goes to USBNT. Behind a Windows controller everything is Windows' (its
hubs, usbstor, hidusb, ...); USBNT's keyboard, mouse and disk devices only
carry what is plugged into USBNT's controllers.

- *Companion controllers*: where USBNT drives an EHCI and Windows its UHCI /
  OHCI companions (2000 SP0 - SP3, XP RTM), USBNT hands every full / low
  speed device on the EHCI's ports to the companion (the EHCI port owner
  bit): keyboards, mice and USB 1.1 devices there are Windows' devices,
  high speed devices USBNT's.
- *Intel EHCI / xHCI port switching* (7 series chipsets and later): with
  Windows' EHCI driver usbinst sets `IntelRouteToXhci = 2`: only SuperSpeed
  goes to USBNT's xHCI, the USB 2.0 side of the ports stays with the EHCI, so
  USB 2.0 devices reach Windows' stack. Without it (USBNT drives the EHCI,
  or `/all`) every port goes to xHCI (`IntelRouteToXhci = 1`), as on NT 4.0.
  Where usbinst did not run - text-mode setup, an install through Device
  Manager - the default on Windows 2000 / XP is 2: text-mode setup reads the
  USB keyboard (and the install media) through the EHCI, and moving them to
  the xHCI left setup without a keyboard on Intel 7 series and later.
- USBNT never hands a hub to Windows' hub driver (usbhub.sys cannot run on
  the USBNT bus; earlier test builds did that with a hub they could not set
  up, which showed a failed "USB hub" with no devices behind it).

**Installation.** As an administrator, from the release folder:

```
usbinst /install
```

usbinst copies the INF files (`2kxp\usbnt.inf`, `usbntkbd.inf`,
`usbntmou.inf`; those of an earlier USBNT go first), creates the USBNT
device bus and installs usbnt.sys for the USB host controllers the table
gives to USBNT and for every card reader, Thunderbolt / USB4 host interface
and I2C controller. A controller an earlier installation gave to USBNT that
now stays with Windows gets Windows' driver back - so running `usbinst
/install` of this version on a machine with an earlier test build (which took every
controller, also Windows' own) fixes it. Options:

- `/all` - every USB host controller to USBNT, also UHCI, OHCI and EHCI
  (the behaviour of earlier test builds: USBNT's keyboard, mouse, disk and
  audio drivers on every port)
- `/ehci` - EHCI to USBNT even where Windows has a driver (UHCI / OHCI stay
  Windows')
- `/noehci` - EHCI never to USBNT (where Windows has no EHCI driver the
  ports work at full speed through the companions)

The INF names UHCI, OHCI and EHCI only by the weakest class code id
(`PCI\CC_0C03`) and carries an older date than Windows' USB INFs, so when
Windows looks for a driver itself (*Found New Hardware*) it takes its own
where it has one and offers USBNT where it has none. (Windows XP: usbd.sys,
which Windows' USB class drivers need, comes from Windows' own usbport.inf
on a machine whose controllers are all USBNT's.) No restart is needed for a
first installation; usbinst says when one is (a driver parameter changed
while usbnt.sys was running, a controller changed its driver). On XP x64 the
32 bit usbinst starts `2kxp\amd64\usbinst.exe` for the devices (32 bit
programs cannot install devices on 64 bit Windows). The tools go to System32
(SysWOW64 on x64) and `usbntcap.dll` is registered for 32 and 64 bit
programs (not where Windows' camera driver takes the cameras).
`usbinst /remove` gives every device back to Windows' driver (or leaves it
without one where Windows has none); `/status` shows the Windows version,
which drivers Windows has, every USB host controller with its driver and
the devices with usbnt.sys. The drivers are not signed: Windows XP asks
once per device (*Continue Anyway*). The disks of Windows' USB storage
driver get usbnt.sys as a lower filter (`/filters`, `/nofilters`; see
*Partitions* under [Usage](#usage)). If Windows asks for a restart after the
installation (*System Settings Change*), the USB keyboard and mouse work
after it.

Windows can also be installed with USBNT in place from the start (F6 in
text-mode setup, onto or from a USB disk): see
[Installing Windows with USBNT](#installing-windows-with-usbnt-text-mode-setup).

**Device Manager.** Below each host controller USBNT drives, Device Manager
shows what Windows' own USB stack would show, under *Universal Serial Bus
controllers*:

```
Renesas uPD720202 USB 3.0 Host Controller (USBNT)
  USB 3.x Root Hub (USBNT)
    USB 2.0 Hub (USBNT)
      USB Keyboard (Logitech USB Keyboard)
      USB Mouse (USB Optical Mouse)
    USB Mass Storage Device (SanDisk Cruzer Blade)
```

The controller's name comes from usbnt.sys itself when it starts: the
known USB 3 chips by name (Renesas / NEC uPD720200, 720200A, 720201, 720202,
ASMedia ASM1042 / 1042A / 1142 / 2142 / 3242, VIA VL800 / VL805, Etron
EJ168 / EJ188, Fresco Logic FL1000 / 1009 / 1100, TI TUSB7340), the others
as vendor + type ("Intel USB 2.0 Enhanced Host Controller (USBNT)") - also
where text-mode setup installed the driver without an INF, whose
controllers usbnt.sys also puts back into the USB class. The root hubs,
hubs and devices are nodes usbnt.sys reports (a device given to Windows'
own drivers shows as their device instead); usbnt.sys is their driver and
makes their device nodes itself, so they start without a *Found New
Hardware* for every new device and unplugging them needs no *Safely Remove
Hardware*. What the devices do stays where it was: all keyboards behind
*USBNT USB Keyboard* (Keyboards), all mice behind *USBNT USB Mouse* (Mice),
the disks under *Disk drives*.

The PS/2 keyboard and mouse stay with Windows' i8042prt; usbnt.sys is the
upper filter of i8042prt's mouse (`usbinst /nops2`: not): a Synaptics,
Elantech (v2 - v4) or ALPS (v3, v5, v7) touch pad is switched to its native
absolute mode and gets the touch pad gestures (tap, two finger and edge
scrolling, ClickPad buttons, trackpoint); any other mouse stays i8042prt's.
The tools work as
on NT 4.0 (usbmon's autoscroll works with every mouse; the back / forward
buttons of USBNT's mice reach programs as X buttons); safe removal in
usbtree and usbeject goes the way of *Safely Remove Hardware* (Windows
closes the volumes first and says who still uses the disk), and Windows' own *Safely Remove Hardware* icon lists USBNT's
disks too. Sound: install AUDIONT 1.0 (its `setup.exe`), which is the
Plug and Play driver of the sound controllers and of *USBNT USB Audio*.

**Camera.** Where Windows has a camera driver - `usbvideo.sys`, the USB
Video Class driver of XP SP2 and later, XP x64 and Server 2003 SP1 -
usbinst sets `WindowsCamera = 1`: USBNT's own camera driver leaves the
cameras alone, a camera on one of USBNT's controllers (xHCI) is reported on
the USBNT bus like any device without a USBNT driver (as a composite device:
usbccgp, then usbvideo.sys, which streams through USBNT's isochronous
transfers), and every program uses it the Windows way (DirectShow, Video for
Windows through *Microsoft WDM Image Capture*). `usbcam` lists those
cameras as *Windows camera* next to USBNT's and shows them through Video for
Windows: live picture, photos (BMP, and JPEG of MJPEG cameras), AVI video;
the format is the one the driver has (*Other format...* opens its format
dialog), *Camera settings...* its settings dialog (brightness, contrast,
...). On Windows 2000, XP RTM / SP1 (no usbvideo.sys) or with `/set
WindowsCamera=0` USBNT drives the cameras on its controllers itself:

`usbntcap.dll` is a Video for Windows capture driver ("USBNT
USB Video Camera") for those: avicap programs (VidCap, NetMeeting, ...) and, on 2000 /
XP, every DirectShow program (through the *VFW Capture* source) get the
picture as 24 bit RGB (decoded from YUY2, UYVY, NV12, I420, grey or MJPEG),
YUY2 or MJPEG; *Video Source* chooses the camera and sets brightness,
contrast, saturation and gain, *Video Format* the size. It lists only the
cameras usbnt.sys drives (none where `WindowsCamera = 1`, so usbinst does not
register it there).

## Architecture

```
src/      usbcore.c   device enumeration, pipes, transfer queues
          hcuhci.c hcohci.c hcehci.c hcxhci.c   host controller drivers
          usbhub.c usbhid.c hidparse.c usbmsc.c usbaudio.c usbvideo.c   class drivers
          sdcard.c sdhci.c rtsxpci.c rtsxusb.c   SD card protocol and readers
          i2chid.c acpi.c   HID over I2C, ACPI table reader
          aml.c gpio.c   AML interpreter (ACPI methods), GPIO pads (Intel / AMD)
          smbus.c smbtp.c smbtpdec.c   SMBus host controllers, touch pads on SMBus
          pcireloc.c  BARs above 4 GB
          usbos.h     the only interface to the operating system
          usbntaud.h  audio interface for other drivers (AUDIONT)
          ntos.c ntmain.c ntscsi.c ntinput.c ntaudio.c ntps2.c   Windows NT layer
          wdmmain.c wdmbus.c usbdi.c   Windows 2000 / XP: Plug and Play, device bus,
                                       USB bus interface for Windows' USB drivers
tools/    user mode tools (no C run time)
harness/  bare metal test kernel that runs the portable core under QEMU
test/     QEMU test scripts: harness tests and full NT VM tests
build/    MinGW build scripts and the NT export lists used for checking
```

The USB core and class drivers are portable C and talk to the OS only
through `usbos.h`. The NT layer creates a SCSI port device for the class
drivers, keyboard and pointer port devices for kbdclass/mouclass, and a
control device (`\\.\UsbNt`) for the tools, and `\Device\UsbNtAudio`, through
which a sound driver gets the audio class functions (see `src/usbntaud.h`):
it is told when audio devices arrive and leave, opens a stream in the
format it wants and is then called back every few milliseconds to supply
or take samples; USBNT keeps the isochronous transfers going and follows
the device clock. The same core is linked into
a small bare metal kernel (`harness/`) and tested against QEMU's USB
emulation without Windows.

## Installation

1. Copy the files from a release to the machine.
2. As an administrator:
   ```
   usbinst /install
   ```
   Options:
   - `/maxdisks N` - number of USB disk slots, 1-8 (default 4)
   - `/irq` - use interrupts instead of polling
   - `/nomon` - do not start usbmon.exe at logon
   - `/nogroup` - do not create the USBNT program group
   - `/keepi8042` - leave the PS/2 keyboard and mouse to NT's i8042prt
   - `/set Name=Value` - store a driver parameter (decimal or 0x hex),
     may be repeated, e.g. `/set MaxTransferKB=512 /set DebugPort=0x2F8`
3. Restart Windows NT.

`usbinst` copies the files, registers `usbnt` as a boot driver in group
"SCSI miniport", sets `ConnectMultiplePorts=1` for kbdclass and mouclass
(PS/2 and USB input work side by side), disables i8042prt (usbnt.sys
drives the PS/2 keyboard and mouse), appends `usbmon.exe` to the Winlogon
`Userinit` value and creates a *USBNT* program group with USB Devices,
USB Camera and USB Monitor in Program Manager. A running tool is replaced
too (the old copy is deleted at the next start).

`usbinst /remove` removes the service and the logon entry, enables
i8042prt again,
`usbinst /status` shows the service state and the logon programs.

Manual installation:

```
HKLM\SYSTEM\CurrentControlSet\Services\usbnt
    Type          REG_DWORD      1
    Start         REG_DWORD      0
    ErrorControl  REG_DWORD      1
    Group         REG_SZ         SCSI miniport
    ImagePath     REG_EXPAND_SZ  System32\drivers\usbnt.sys
HKLM\SYSTEM\CurrentControlSet\Services\Kbdclass\Parameters
    ConnectMultiplePorts  REG_DWORD  1
HKLM\SYSTEM\CurrentControlSet\Services\Mouclass\Parameters
    ConnectMultiplePorts  REG_DWORD  1
HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon
    Userinit      append ",usbmon.exe"
```

(usbnt.sys sets `ConnectMultiplePorts` of kbdclass and mouclass to 1 itself
when it finds it 0, so the service key alone is enough for the USB keyboard
and mouse from the next start on.)

## Installing Windows with USBNT (text-mode setup)

A release has three driver disks for the text-mode part of Windows setup,
one per Windows family, every file in the root of the disk (1.44 MB floppy
images; their files are also in the `OEMDISK` folder, for a real floppy or
for nLite):

| Disk (`OEMDISK\...`) | For | Choices |
|---|---|---|
| `USBNTNT.IMG` (`NT`) | NT 3.1, 3.5, 3.51, 4.0 | USBNT - Windows NT 3.1 to 4.0 |
| `USBNT2K.IMG` (`2KXP`) | 2000, XP, Server 2003 (32 bit) | USBNT - Windows 2000 / XP: USB 3 xHCI, USB4, card readers; ... every USB controller |
| `USBNT64.IMG` (`XP64`) | XP x64, Server 2003 x64 | USBNT - Windows XP / 2003 x64: USB 3 xHCI, USB4, readers; ... every USB controller |

**nLite** (and other tools that put text-mode drivers into the setup CD):
add the folder of your Windows - `OEMDISK\2KXP` for 32 bit Windows 2000 /
XP / 2003, `OEMDISK\XP64` for x64 - as a text-mode driver (its `USBNT.INF`
or `TXTSETUP.OEM`) and pick one of its choices. These tools take the files
from the folder of TXTSETUP.OEM; the single disk of earlier test builds had the drivers in
`\NT`, `\I386` and `\AMD64` below it, so nLite wrote usbnt.sys into the
CD's TXTSETUP.SIF without copying the file, and Windows 2000 setup stopped
with *The file usbnt.sys could not be found*. Integrate the driver again
from the new `OEMDISK\2KXP` into a fresh copy of the CD.

The "USB 3 xHCI, USB4, card readers" choices leave the USB 1.1 / 2.0
controllers to Windows' own drivers (and so its USB keyboard support in
setup); "every USB controller" gives them to USBNT too. Setup then sees
USBNT's disks (USB disks on any controller, card reader cards, NVMe drives
behind Thunderbolt / USB4) and can install from them, and the installed
Windows starts with usbnt.sys as a boot driver - also from such a disk
(see the note on removable media below).

**NT 3.1, 3.5, 3.51.** Start setup from its boot floppies, choose *Custom
Setup*; at the list of SCSI adapters press **S** (*Specify additional
device*), choose *Other (Requires disk provided by a hardware
manufacturer)*, insert the USBNT disk when setup asks for the
manufacturer's disk and choose *USBNT - Windows NT 3.1 to 4.0*.

**NT 4.0.** At *Setup will detect mass storage devices* press **S** to skip
detection (or ENTER, then **S** at the list), then as above: *Other*, the
USBNT disk, *USBNT - Windows NT 3.1 to 4.0*.

Setup loads usbnt.sys from the disk right away: the USB disks are on the
disk list after the next screens. Taking the USB controllers from the BIOS
ends the BIOS' PS/2 emulation of a USB keyboard; usbnt.sys was loaded after
kbdclass, which does not connect to new keyboard ports, so it sends the USB
keys to the class driver of the PS/2 keyboard setup reads (i8042prt's) -
the USB keyboard keeps working in setup. Setup copies usbnt.sys and makes
the `usbnt` service (boot start, group SCSI miniport); in the installed
system usbnt.sys sets kbdclass / mouclass `ConnectMultiplePorts` itself, so
USB keyboard and mouse work from the first start (GUI-mode setup included).
`usbinst /install` afterwards adds the tools.

Windows NT 3.x / 4.0 can start from a USB disk when the BIOS can boot from
it: NTLDR reads the system through the BIOS (`multi(0)disk(0)rdisk(N)` in
BOOT.INI - setup writes that when the BIOS shows the disk), then usbnt.sys
takes the disk over. A `scsi(...)` entry (with NTBOOTDD.SYS) cannot work:
usbnt.sys is not a SCSI miniport NTLDR can run. If setup wrote one, change
it to `multi(0)disk(0)rdisk(N)partition(P)`.

**Windows 2000, XP, XP x64, Server 2003.** Press **F6** at *Press F6 if you
need to install a third party SCSI or RAID driver*, then **S** at *Setup
could not determine the type of one or more mass storage devices*, insert
the USBNT disk of your Windows (`USBNT2K.IMG` x86, `USBNT64.IMG` x64) and
choose an entry. Setup
loads usbnt.sys as a boot driver with the `[HardwareIds.scsi.*]` ids (xHCI
`PCI\CC_0C0330`, USB4, the card readers, with "every USB controller" also
EHCI, UHCI, OHCI and every id Windows' own USB INFs name) and copies the INF
files, which GUI-mode setup installs. There is no USBNT device bus device
then (usbinst makes it): **the first host controller that starts is the
device bus**, and each controller lets its devices come up (at most
`BootWaitMs`) before Plug and Play asks for them, so a USB disk is there
when Windows looks for its boot disk. Setup gives the service the
parameter `OemSetup = 1`; `usbinst /install` on such a system adds the
tools and makes no device bus. (usbnt.inf installs the driver boot start,
group SCSI miniport, too.)

Text-mode setup gives every `[HardwareIds.scsi.*]` id the SCSI adapter
class, also `USBNT\Keyboard` and `USBNT\Mouse`; without kbdclass / mouclass
on them the USB keyboard and mouse worked in GUI-mode setup and at the logon
screen only where USBNT could borrow the class driver of a PS/2 keyboard
(earlier test builds: no keyboard on machines without one, e.g. a USB 3 keyboard on a
Renesas card at the setup wizard's *Welcome* page). usbnt.sys now puts them
into the keyboard / mouse classes itself at every start (the
CriticalDeviceDatabase entries and device nodes of another class), before
its bus reports them. The host controllers, root hubs, hubs and devices
then show in Device Manager as after `usbinst` (see *Device Manager* above).

In text-mode setup itself the keyboard is the same question. Setup gives a
device the service its `[HardwareIdsDatabase]` line names, and a class only
when the line has one; nLite (and XP setup with F6) write
`USBNT\Keyboard = "usbnt"` without a class. kbdclass, the class driver
setup reads the keys from, comes onto a keyboard only as the Keyboard
class's upper filter, so the USB keyboard had no class driver; setup kept
working only where USBNT could borrow a PS/2 keyboard's (or the BIOS'
emulation of one). On a machine without a PS/2 keyboard - e.g. a USB 3
keyboard on a Renesas card with Windows 2000 SP4 integrated by nLite -
nothing reacted at *Welcome to Setup*. usbnt.sys now gives its keyboard /
mouse device node the Keyboard / Mouse class itself, as soon as Plug and
Play has made the node and before it builds its driver stack, so kbdclass
comes onto the USB keyboard in text-mode setup too (a class an INF gave is
left alone). Tested with a Windows 2000 CD integrated the way nLite does
it, on a machine with a keyboard controller but no PS/2 keyboard (see
Testing).

Notes: USBNT reports every disk it serves as removable media (that is how
the NT class drivers follow hot plug and media changes). A setup that only
offers fixed disks as the place to install Windows to will then not offer
them - not verified yet (it needs the setup media; `test/ttxtsetup.py` shows
it); a system copied onto a USB disk starts from it (tested, see Testing). A
USB CD / DVD drive can be read once setup runs; booting setup from it is the
BIOS' business. Text-mode setup itself was run with a Windows 2000 (SP0)
CD (see Testing).

**The USB keyboard and mouse in text-mode setup of Windows XP and later.**
Setup brings its own USB input drivers: its TXTSETUP.SIF loads hidusb,
kbdhid and mouhid and binds `USB\Class_03`, `HID_DEVICE_SYSTEM_KEYBOARD`
and `HID_DEVICE_SYSTEM_MOUSE` to them. So in text-mode setup of XP, XP x64
and Server 2003 USBNT does not drive USB keyboards and mice itself: it hands
them (and the HID functions of composite devices, split by USBNT itself) to
those drivers through its USB bus interface, as a USB host controller driver
does (xhci98 works this way), and setup reads the keys from them like from
a keyboard on its own UHCI / EHCI. USB disks stay USBNT's. After text-mode
setup USBNT drives the keyboard and mouse itself again. `TextSetupHid` (in
the setup hive) chooses: 1 setup's drivers (XP and later), 0 USBNT's own
keyboard (Windows 2000: its text-mode setup copies hidusb.sys to the new
system without usbd.sys, hidclass.sys and hidparse.sys, and the first start
stops with STOP c000026c). Tested in Windows 2000 text-mode setup with
`TextSetupHid=1` (every key of the setup screens from the USB keyboard
through setup's kbdhid) and with the default.

**Windows 2000 with F6.** Windows 2000 setup loads usbnt.sys from the
driver disk but does not use the disk's `[HardwareIds.scsi.*]` (XP's setup
does; an exact controller id does not help either): the host controllers
get no driver. usbnt.sys then takes them itself in text-mode setup: the
controllers of the USB classes (and SD hosts) setup left without a driver
are found with IoReportDetectedDevice (PCI's legacy device detection gives
the PCI device), usbnt.sys starts them, and reports its device bus itself;
the keyboard, mouse and disk device nodes are made with their service and
class before the bus reports them, and the disks come once setup has loaded
disk.sys. Setup's registry is thrown away after text-mode setup, so nothing
of this stays in the installed system. The same holds for a device setup
learns of only after its start (a USB disk found by its controller), also
with nLite.

## Usage

**Sound.** Install the [AUDIONT](https://github.com/ages2001/AUDIONT) sound
driver (1.0 or later) after USBNT. Every USB audio device then becomes a
sound card with wave out, wave in, MIDI out and volume devices named after
the product; devices can be plugged in and out at any time (programs started
afterwards see a new device; a device that is pulled out keeps its card, and
gets it back when it returns). USBNT picks the alternate setting closest to
48 kHz / 16-bit / stereo the device offers, sets the sample rate, unmutes
the device and sets it to 0 dB; AUDIONT converts to and from whatever
format and rate that is. `usbtree` shows audio devices as *Audio* / *MIDI*.

**Camera.** Start *USB Camera* (`usbcam.exe`), pick the camera, format
and size; *Photo* saves a BMP or JPEG, *Record* an AVI file (MJPEG cameras
are recorded without re-encoding). `usbcam /test` runs it without a window
for scripts (see the source). Programs that capture through Video for Windows (NT 3.51 / 4.0:
VidCap, avicap32) use the camera through `usbntcap.dll`, which usbinst
registers ("USBNT USB Video Camera").

**SD cards.** A card in a supported reader is a removable disk like a USB
stick: it gets a drive letter from usbmon, `usbtree` lists it under *SD
card readers*, and *Safe removal* works. FAT32 cards (every SDHC card comes
FAT32) need a FAT32 file system driver on NT 3.1 - 4.0, e.g. FATNT; SDXC
cards come exFAT.

**When something does not work.** *Save report...* in `usbtree` (Alt+P)
writes the device tree, every detail and the last 32 KB of the driver's log
into a text file - no serial port needed. Disk commands that fail (a read
the card reader refuses, a short read) are in that log with their block
numbers and sense codes.

**Touch pads.** Nothing to set up; `usbtree` lists the I2C controllers and
devices under *HID over I2C* (a precision touch pad as *Precision touch
pad*). If the touch pad is not found, look at the log (*Save report...* in
usbtree, or `/set DebugPort=0x2F8` for a serial port): it names the
controllers and the ACPI devices seen. Gestures are set in the driver parameters (below): `TouchpadScroll`
1 = the content follows the fingers like on Windows, 2 = the other way
round (like a wheel), 0 = no scrolling; `TouchpadTap` 0 turns tap to click
off; `TouchpadSpeed` is the pointer speed in percent; `TouchpadGestures` 0
leaves the pad in its mouse mode; `TouchpadScrollSpeed` (percent),
`TouchpadAccel` 0 turns pointer acceleration off, `TouchpadEdgeScroll` 1 /
0 turns scrolling at the right edge on / off (2: only on pads that see one
finger), `TouchpadSwipe` 0 turns the three finger back / forward swipes off.

**PS/2 touch pads** (Synaptics, Elantech v2 - v4, ALPS v3 / v5 / v7) run in
their native absolute mode with the same gestures: on NT 3.x / 4.0 through
usbnt.sys' PS/2 driver, on Windows 2000 / XP through its filter on
i8042prt's mouse. Horizontal scrolling (of pads, tilt wheels and USB mice
with a horizontal wheel) reaches programs through usbmon as WM_HSCROLL on
every version - no Windows before Vista has a horizontal wheel message.
`Ps2Touchpad` 0 keeps every PS/2 pad a plain mouse. A finger that may
still be a tap does not move the pointer, so a double tap stays inside
Windows' double click rectangle and is a double click. Older pads (Synaptics
with the old absolute format, Elantech v1, other ALPS versions) and other
makes stay plain wheel mice.

**Touch pads on SMBus.** Many notebook touch pads are a PS/2 mouse that can
also be reached over SMBus, where they report every finger (Synaptics
"InterTouch", RMI4 at address 2Ch; Elan at 15h). usbnt.sys does what Linux
does: after the PS/2 reset it identifies the pad (Synaptics: the InterTouch
capability bit; Elan: the SMBus / host notify capability), turns its PS/2
side off and sets it up on the SMBus controller (Intel ICH2 .. current PCH,
AMD SB800 / Hudson-2 / FCH with its multiplexed ports) - for Synaptics only
when the PS/2 mouse device in the ACPI tables has an id from Linux's list
(`LEN0036`, `LEN2054`, `SYN3221`, ... the ThinkPads, HP EliteBooks, Dell
Precisions and others Linux knows to work), for Elan the newer ICs. The
pad then has the gestures of the I2C touch pads (pointer, two finger
scrolling, taps, the pad's click; `Touchpad*` settings), separate buttons and
the trackpoint (Synaptics: the PS/2 trackpoint behind the pad, F03; Elan:
its trackpoint reports) are a second pointer. NT has no SMBus interrupt: the
pad is polled every 8 ms while it is touched, every 30 ms when idle. While
the firmware uses the SMBus (Intel INUSE_STS semaphore, AMD SMBus semaphore
shared with the IMC) a transfer waits; only the touch pad's address is ever
used. `Smbus` = 2 takes every pad that says it has an SMBus side, 0 none;
`SmbusTouchpad` names the addresses of a pad to use without the PS/2
identification (low byte Synaptics RMI4, second byte Elan, e.g. `0x2C` or
`0x1500`). `usbtree` lists the SMBus controllers and pads under *HID over I2C
and SMBus*.

- *NT 3.x / 4.0*: usbnt.sys owns the PS/2 port (the usual installation), so
  the switch happens at its PS/2 mouse set-up. With `/keepi8042` (or `Ps2`
  = 0) the PS/2 mouse is i8042prt's, see the next point.
- *Windows 2000 / XP*: the PS/2 mouse belongs to i8042prt, which enables the
  pad's PS/2 side; usbnt.sys cannot switch it. A pad that answers on SMBus
  anyway is used: Synaptics with a listed ACPI id (`Smbus` = 1), Synaptics
  and Elan with `Smbus` = 2, or the addresses in `SmbusTouchpad`. (The PS/2
  mouse filter for i8042prt calls the same switch once it is there.)

**USB Monitor.** `usbmon.exe` runs without a window from logon on. *USB
Monitor* in the USBNT program group (`usbmon /show`) opens the window of
the running copy (or starts it): whether usbnt.sys runs, the disk slots
with their drive letters, the last things it did; *Hide* keeps it running,
*Stop the monitor* ends it.

**Drive letters.** At boot NT reserves a letter for every USB disk slot.
`usbmon` removes the letters of empty slots, gives a disk a letter when it
is plugged in - every partition of it one - (the letter it had last time if
that one is free) and takes them back when the disk is removed. NT 4.0 Explorer updates itself; the
File Manager of NT 3.x reads its drive list only when it starts, so new
letters show up there after File Manager is reopened. Set
`HKLM\Software\UsbNt\DriveLetters` to 0 to keep the fixed boot letters.

**Safe removal.** Select a disk or a storage device in `usbtree` and press
*Safe removal* (Alt+S), or run `usbeject D:`. `usbeject /list` lists the
slots. On a disk with several partitions every one of them is flushed,
locked and dismounted (`usbeject` names the drive that is still in use, and
removes nothing then; `/f` dismounts anyway).

**Partitions.** NT's disk drivers treat a removable medium - and every USB
stick and memory card says it is one - as a single volume. USBNT gives
every partition of a disk with several a drive letter:

- *NT 3.1 - 4.0:* the disk driver creates the partition devices of a disk
  (`\Device\HarddiskN\PartitionK`) only once, when it starts: as many as
  the medium in the slot has then, one for an empty slot. A removable disk
  keeps them and maps the K-th one to the K-th partition of whatever medium
  is in the slot. So until the disk driver has read a slot's partition
  table, USBNT shows it a placeholder medium with `MaxPartitions` (8)
  partitions - every slot gets 8 partition devices - and the real medium
  (if any) right after that. `usbmon` gives a letter to every partition
  device that maps a partition of the medium (the letter it had last time,
  if free) and takes it back when the disk goes; `usbtree` lists the
  letters. Which partitions there are is the disk driver's business, as on
  internal disks (NT 3.1 takes an LBA extended partition, type 0F, for a
  partition of its own and does not see the logical ones in it; usbmon
  gives such a container no letter). A partition whose file system NT has
  no driver for gets a letter like any unformatted drive. GPT disks need
  [MBRGPTNT](https://github.com/ages2001/MBRGPTNT).
  The slots stay removable disks: a fixed disk's partitions and size would
  be read only once and never again for another medium (and NT 3.1 cannot
  make partition devices later at all), and a fixed USB disk at boot would
  take drive letters before the internal disks.
- *Windows 2000 / XP:* the mount manager gives the letters; disk.sys
  shows only the first partition of a removable medium. A medium with more
  than one partition is reported to disk.sys as a **fixed** disk
  (`RemovableAsFixed` = 1; 2 = every medium, 0 = never; a device that is a
  fixed disk itself is one too) - when its MBR has a disk signature:
  Windows 2000 / XP mount no volume of a fixed disk without one (it waits
  for Disk Management to write it), so such a disk stays removable with
  its first partition (Windows, Linux fdisk / parted and the usual tools
  write a signature; `RemovableAsFixed` = 2 reports it as fixed anyway, and
  Disk Management can then write one). It stays a hot plug device: *Safely
  Remove Hardware*, `usbeject` and `usbtree` work as before, pulling it out
  works. disk.sys never looks at a fixed disk's medium again, so such a
  disk is a new disk to Windows for each medium: a card reader slot with
  such a card is polled, and when the card goes or is swapped the disk goes
  and comes back with the new card. On XP x64 a GPT stick gets its
  partitions too (Windows 2000 and 32 bit XP cannot read GPT).
- *Disks of Windows' own USB storage driver* (usbstor, on Windows' USB
  controllers or through USBNT's with `UsbPdo=2`): usbnt.sys is a lower
  filter of disk.sys on them and reports a medium with several partitions
  as fixed the same way (as the cfadisk filter does). `usbinst /install`
  sets the filter on every USB storage disk Windows knows, `usbmon` (or
  `usbinst /filters`) on those that come later - such a disk is restarted
  once, then its partitions appear; `usbinst /remove` (or `/nofilters`)
  takes the filter away. Only USBSTOR disks get it: as a filter of every
  disk, a missing usbnt.sys would stop Windows from booting. If the card
  of a card reader disk reported as fixed goes or changes, the filter
  stops the disk's requests and asks Windows to restart the disk.

**Wheel on NT 3.x.** NT 3.x has no WM_MOUSEWHEEL; `usbmon` turns wheel
movement into WM_VSCROLL for the window under the cursor (MDI children
included). `HKCU\Software\UsbNt`: `WheelLines` (3), `WheelTarget`
(0 = window under the cursor, 1 = foreground window).

**Back / forward buttons and autoscroll.** Mice with buttons 4 and 5 (the
back / forward buttons at the thumb) work with every USB, PS/2
(IntelliMouse Explorer protocol) and I2C mouse usbnt.sys drives:

- Windows 2000 / XP know these buttons: they arrive as WM_XBUTTONDOWN /
  WM_XBUTTONUP and WM_APPCOMMAND (back / forward in Explorer, Internet
  Explorer and other programs).
- NT 3.x and NT 4.0 have no such buttons. usbnt.sys hands their presses to
  `usbmon`, which types **Alt+Left** (button 4) and **Alt+Right** (button 5)
  instead - back and forward in the browsers and in Explorer of the time.
  The keys are set in `HKCU\Software\UsbNt`: `BackKeys` and `ForwardKeys`
  (REG_SZ, e.g. `Backspace`, `Ctrl+PageUp`, `Alt+F4`, `F5`, `0xA6`; modifiers
  `Ctrl`, `Alt`, `Shift`, `Win`; empty = nothing), `XButtons` = 0 turns this
  off (the buttons then do nothing, as before).

**Autoscroll**: a click of the wheel button puts a round origin mark where
the pointer is and turns the pointer into a scroll cursor. Moving the mouse
away from the mark scrolls the window under it, faster the farther away the
pointer is (up / down, and sideways in windows with a horizontal scroll
bar); the cursor shows the direction. Any click ends it (that click goes to
no program), as does Esc. Holding the wheel button and dragging scrolls
while the button is held; releasing it ends autoscroll. How it works:

| | NT 3.1 / 3.5 / 3.51, NT 4.0 before SP3 | NT 4.0 SP3 and later, 2000, XP |
|---|---|---|
| wheel button presses | from usbnt.sys (its USB, PS/2 and I2C mice): it gives them, and while autoscroll runs every button press, to usbmon instead of to the system | a low level mouse hook (WH_MOUSE_LL): every mouse, also Windows' own PS/2 and USB drivers |
| scrolling | WM_VSCROLL / WM_HSCROLL | WM_MOUSEWHEEL to the window under the mark, WM_HSCROLL sideways |

The scroll cursor is set with SetSystemCursor (arrow, I-beam and hand) and
the user's cursors come back (SystemParametersInfo SPI_SETCURSORS, on NT 3.x
copies of the cursors) when autoscroll ends, when usbmon ends and when it
crashes; usbnt.sys gives the buttons back to the system as soon as usbmon's
handle is closed or usbmon stops fetching them for a second.
`HKCU\Software\UsbNt`: `MiddleButton` 1 (default) = autoscroll, 0 = an
ordinary middle click, 2 = autoscroll only where a scroll bar is (an
ordinary middle click elsewhere); `AutoscrollSpeed` (100) in percent.
Settings are read when usbmon starts: `usbmon /restart` applies them at
once. `wheeltst.exe` shows what arrives.

## Configuration

`HKLM\SYSTEM\CurrentControlSet\Services\usbnt\Parameters` (or the service
key itself), REG_DWORD:

| Value | Default | Meaning |
|---|---|---|
| MaxDisks | 4 | USB disk slots, 1-8 |
| MaxTransferKB | 0 | Upper limit for one SCSI command in KB, 0 = automatic |
| UseUas | 1 | Use UAS when a disk offers it, 0 = Bulk-Only |
| Usb3Hubs | 1 | Use the SuperSpeed side of USB 3 hubs |
| UseInterrupts | 0 | 1 = interrupts, 0 = polling |
| TimerResolution | 1 | Ask the kernel for a 1 ms timer where it can (Windows 2000 and later; on NT 3.x/4.0 usbmon does it) |
| DisableControllers | 0 | Bit mask: 1 = UHCI, 2 = OHCI, 4 = EHCI, 8 = xHCI |
| IntelRouteToXhci | 1 (2000 / XP: 2) | Intel EHCI / xHCI port switching: 1 = USB 2.0 and SuperSpeed to xHCI, 2 = only SuperSpeed to xHCI, USB 2.0 stays with the EHCI (Windows 2000 / XP: usbinst sets 2 when Windows' EHCI driver drives the EHCI, else 1; without a value, as in text-mode setup, 2), 0 = as the firmware set it |
| MaxPciBus | 255 | Highest PCI bus scanned |
| WheelMode | 0 | 0 = automatic, 1 = MOUSE_WHEEL only, 2 = usbmon only |
| ReportWheel | 1 | Report the mouse as a wheel mouse |
| DisableKeyboard / DisableMouse / DisableStorage / DisableAudio / DisableVideo / DisableSd | 0 | Ignore that device class |
| Ps2 | 2 | PS/2 keyboard and mouse: 0 = never, 1 = always, 2 = when i8042prt is disabled |
| I2cHid | 1 | HID over I2C: 0 = off, 1 = ACPI devices and the usual addresses, 2 = ACPI devices only |
| I2cHidAml | 1 | Run the ACPI methods (_STA, _CRS, _DSM, _PS0/_PS3, _INI) with the built-in AML interpreter; 0 = off (static scan and the usual addresses only) |
| I2cHidGpio | 1 | Drive the touch pad's power / reset GpioIo lines and read its GpioInt "data ready" pad; 0 = off (blind polling, no GPIO) |
| TouchpadGestures | 1 | Precision touch pads in touch pad mode with gestures, 0 = mouse mode |
| TouchpadScroll | 1 | Two finger scrolling: 1 = content follows the fingers, 2 = reversed, 0 = off |
| TouchpadTap | 1 | Tap to click |
| TouchpadSpeed | 100 | Touch pad pointer speed in percent (10-1000) |
| TouchpadScrollSpeed | 100 | Touch pad scrolling speed in percent (10-1000) |
| TouchpadAccel | 1 | Pointer acceleration of touch pads, 0 = off |
| TouchpadEdgeScroll | 2 | Scrolling at the right edge: 1 = on, 0 = off, 2 = only on pads that see one finger |
| TouchpadSwipe | 1 | Three fingers swiped right / left = back / forward (buttons 4 / 5), 0 = off |
| Ps2Touchpad | 1 | PS/2 touch pads (Synaptics, Elantech, ALPS) in their native mode; 0 = plain mouse |
| Smbus | 1 | Touch pads on SMBus: 1 = the pads Linux puts on SMBus (Synaptics with a listed ACPI id, newer Elan ICs), 2 = every pad with an SMBus side, 0 = no SMBus access |
| SmbusTouchpad | 0 | Use a pad at these SMBus addresses without the PS/2 identification: bits 0-7 Synaptics RMI4 (e.g. 0x2C), bits 8-15 Elan (e.g. 0x1500); 0 = after the PS/2 switch only |
| PciHotPlug | 1 | Watch PCIe hot plug ports (Thunderbolt / USB4 docks) |
| DisableTbt | 0 | 1 = leave Thunderbolt / USB4 host interfaces alone |
| TbtApprove | 1 | Devices behind Thunderbolt / USB4: 1 = connect (approve, pair), 2 = only paired devices (secure level), 0 = no PCIe tunnels |
| TbtUsb3Tunnel | 1 | USB4: USB 3 tunnels to docks (0 = the dock's own xHCI over PCIe) |
| TbtLaneBonding | 1 | Bond the two lanes of a Thunderbolt 2 / 3 / USB4 link (40 Gb/s), 0 = one lane (20 Gb/s); Thunderbolt 5 / USB4 v2 Gen 4 links (80 Gb/s) are bonded by the hardware |
| TbtPcieExtEncap | 1 | Thunderbolt 5 / USB4 v2: PCIe tunnels between two USB4 v2 routers on a Gen 4 link with extended encapsulation, 0 = off |
| TbtDriverCm | 2 | Thunderbolt hosts whose firmware does not manage connections: 2 = the driver does it, except Thunderbolt 1 / 2 controllers of PCs (the BIOS runs those; Macs: the driver), 1 = always the driver, 0 = never |
| DisableNvme | 0 | 1 = do not drive hot plugged NVMe drives |
| NvmeAtBoot | 0 | 1 = also drive the NVMe drives that are there at boot (otherwise nvme2k's) |
| RelocateBars | 2 | PCI memory above 4 GB: 0 = leave it, 1 = move only the controllers USBNT drives, 2 = move every device's BARs below 4 GB (bridge windows opened, empty 64-bit windows closed, oversized BARs shrunk through Resizable BAR) when the kernel cannot map above 4 GB (NT 3.x / 4.0, 32-bit 2000 / XP without PAE), 3 = always |
| SdBrokenCd | 0 | Bit mask of SDHCI slots whose card detect does not work |
| AudioLatencyMs | 48 | Audio queued ahead of the device, 24-96 ms |
| BootWaitMs | 1500 | Time given to devices at boot (Windows 2000 / XP: at most this long per started controller, until its devices have settled) |
| DebugLevel | 2 | 0 = off, 1 = errors, 2 = info, 3 = debug, 4 = verbose |
| DebugPort | 0 | Serial port for the log (e.g. 0x2F8), 0 = DbgPrint |
| UsbPdo | 1 | Windows 2000 / XP: devices for Windows' USB drivers - 1 those without a USBNT driver, 2 all but hubs, 0 none |
| TextSetupHid | 1 (2000: 0) | Text-mode setup: 1 = USB keyboards and mice go to setup's own hidusb / kbdhid / mouhid (Windows XP and later), 0 = USBNT is the keyboard / mouse itself |
| UsbInterfaceNames | 1 | Windows XP: the functions usbccgp makes of a composite device for Windows' drivers that have no driver get the name of their interfaces ("Plugable UD-3900 - Audio Control (Interface 0)" instead of several "USB Device"), 0 = off |
| WindowsCamera | 0 | Windows 2000 / XP: 1 = USB cameras go to Windows' usbvideo.sys (with UsbPdo 1 or 2), not to USBNT's camera driver; usbinst sets 1 where Windows has usbvideo.sys (XP SP2 and later, XP x64, Server 2003 SP1), else 0 |
| MaxPartitions | 8 | NT 3.1 - 4.0: partition devices (drive letters) per USB disk slot, 2-24; 1 = only the first partition (as before) |
| RemovableAsFixed | 1 | Windows 2000 / XP: report a removable medium as a fixed disk, so that every partition gets a letter - 1 when it has more than one partition, 2 always, 0 never (also for the disks of Windows' usbstor, through the filter) |
| OemSetup | - | Set to 1 by text-mode setup (TXTSETUP.OEM): Windows 2000 / XP usbinst then makes no USBNT device bus (the first host controller is the bus) |

`usbmon`: `HKLM\Software\UsbNt` `DriveLetters` (1), `TimerResolution` (1);
`HKCU\Software\UsbNt`, REG_DWORD unless noted:

| Value | Default | Meaning |
|---|---|---|
| WheelLines | 3 | NT 3.x: lines per wheel notch |
| WheelTarget | 0 | NT 3.x wheel: 0 = window under the cursor, 1 = foreground window |
| MiddleButton | 1 | Wheel button: 1 = autoscroll, 0 = ordinary middle click, 2 = autoscroll where a scroll bar is |
| AutoscrollSpeed | 100 | Autoscroll speed in percent (10-1000) |
| XButtons | 1 | NT 3.x / 4.0: type keys for the back / forward buttons, 0 = off |
| BackKeys | Alt+Left | REG_SZ, NT 3.x / 4.0: keys for button 4 |
| ForwardKeys | Alt+Right | REG_SZ, NT 3.x / 4.0: keys for button 5 |

## Building from source

**Release build (NT 4.0 DDK + Visual C++ 4.0).** In a DDK "Free Build
Environment" window:

```
cd src
build -cZ
```

The driver lands in `%BASEDIR%\lib\i386\free\usbnt.sys`. Then, with the
MSVC 4.0 `bin` directory on the PATH and `LIB` pointing at the DDK's
`lib\i386\free` (kernel32, user32 and advapi32.lib come from there):

```
cd tools
mktools
```

The tools use no C run time library. Both steps build without warnings.
The 1.0 release binaries were built this way inside an NT 3.51 VM; the
1.1 and 1.2 binaries in `bin/` come from the MinGW build below. The interface that
other drivers use (`usbntaud.h`) is `__cdecl` throughout, so drivers built
with either tool chain work together.

**Test build (Linux, MinGW-w64).** `build/ntbuild.sh` and
`build/tlbuild.sh` build the driver and the tools into `build/out` and
check imports and instruction set against all supported NT versions.
`build/wdmbuild.sh` builds the Windows 2000 / XP drivers
(`out/2kxp/i386`, `out/2kxp/amd64`, imports checked against Windows 2000
and XP x64), the 64 bit usbinst.exe and usbntcap.dll, the INF files and
`out/oem/NT|2KXP|XP64/TXTSETUP.OEM` (`build/mkinf.py`); `build/mkoem.py` then
makes the driver disks `out/USBNTNT.IMG`, `USBNT2K.IMG`, `USBNT64.IMG`
(`build/mkdist.sh` does all of it and puts the disks into `dist/`).

## Testing

- `test/runall.sh` builds the bare metal harness and runs the portable
  core against QEMU's UHCI, OHCI, EHCI (with companions) and xHCI:
  enumeration, keyboard, mouse, tablet, hubs, hot plug, multi-LUN card
  readers, CD-ROM, large transfers, UAS with and without streams, and
  surprise removal during writes.
- `test/tuac.py` (part of `runall.sh` when `$QEMU` points at a QEMU with
  the `usb-uac` test device, see below) plays, records, changes volume,
  sends MIDI and hot plugs a UAC1 / UAC2 device on every controller: full
  speed on UHCI, OHCI, xHCI and behind an EHCI transaction translator, high
  speed with 1, 2, 4 and 8 packets per millisecond on EHCI and xHCI, with
  and without feedback, with a drifting device clock.
- `test/tuvc.py` (cameras, iso and bulk, every controller), `test/tsd.py`
  (SDHCI), `test/trtsx.py` (Realtek USB and all 14 Realtek PCIe chip ids),
  `test/ti2c.py` (HID over I2C: precision touch pad gestures - two finger
  scrolling, taps, parallel and hybrid reports -, touch pad, touch screen found through an
  ACPI SSDT, keyboard with LEDs, finger motion, repeating firmware, small
  FIFO, two controllers next to USB), `test/tacpitp.py` (a touch pad the
  firmware only fully describes in ACPI: a conditional _CRS that chooses the
  address, _DSM returning the descriptor register through a Package, and a
  _PS0 that must write a power register before the device answers - found
  only when the AML interpreter runs, the fallback leaves it alone),
  `test/thotpci.py` (PCIe hot plug of
  xHCI controllers and of a bridge with an xHCI behind it, unplug, plug
  again), `test/tnvme.py` (NVMe drives hot plugged, written and read with
  small and MDTS-split commands, at the end of the disk, 4 KB blocks,
  attention button, safe removal; a drive present at boot left alone) and
  `test/ttbt.py` (Thunderbolt / USB4: the driver as connection manager -
  routers plugged in, at boot, chained, Thunderbolt 3 device, unplug with
  the host's tunnels removed, NVMe through the tunnel; Thunderbolt 5 / USB4
  v2 with a Gen 4 link, CM version 2 and PCIe extended encapsulation, a
  USB4 v1 device on it; Thunderbolt 1 / 2 hosts of a Mac with the driver as
  connection manager, a PC's left to the BIOS; firmware connection
  manager with the Falcon / Alpine / Titan Ridge and Ice Lake / Tiger Lake
  interfaces, security levels none / user / secure with new, stored, wrong
  and lost keys) run on the harness with the QEMU test devices below.
- `test/tnt.py`, `tgui.py`, `tinst.py`, `tddk.py` boot real NT installs in
  QEMU (images in `$USBNT_NTIMG`); `ntinput.py` types and clicks with a
  PS/2 keyboard and mouse, two USB keyboards and mice and (with `--i2c`) an
  I2C touch pad and keyboard into one window at the same time (also: the
  left and right shift keys let go, double clicks; `--bat 1500`: a PS/2
  mouse whose self test takes 1.5 s, `test/qemu/ps2-mouse-bat.patch`);
  `ntpad.py` / `w5pad.py` also double tap a PS/2 touch pad; `ntcam.py`
  takes photos and records video with usbcam (`allmodes`: every format and
  size, also with a 4:2:0 MJPEG camera, a camera that picks its own frame
  size and one that reports a too small frame size); `ntmsc.py` re-plugs
  and swaps USB sticks on one port; `nttree.py` checks usbtree with an SD
  card, a USB stick and an I2C touch pad; `ntmon.py` opens the usbmon
  window and plugs a stick while it runs; `ntsd.py` reads and writes a
  32 GB FAT32 SD card (FATNT as the FAT driver) through every reader, card
  in at boot or put in later, and reads a card through the disk and the
  partition at many offsets and lengths (`sectors`); `ntlook.py` takes
  screenshots of the program group and the programs with their icons;
  `ntpart.py` plugs a stick with three primary and two logical partitions
  (`mkpart.py` makes it) after logon or at boot (`--boot`): every
  partition gets a letter and is read, one written, `usbeject` refuses
  while a file on another partition is open and then releases the disk, a
  single partition stick takes the port, then the first stick comes back
  with all its letters;
  `ntnvme.py` boots NT 3.1 / 4.0 on a q35 machine with an NVMe drive in
  it (nvme2k) and plugs a USB4 dock with an NVMe drive after logon: USBNT
  sets up the tunnel, the drive gets a letter, files are read, written
  (20 MB) and checked on the host, usbeject, unplug; `ntmouse.py`
  (`--nt4`, `--nohook`) presses the back / forward buttons of a PS/2 and a
  USB mouse (Alt+Left / Alt+Right arrive in wheeltst), autoscrolls with a
  click and by dragging with the wheel button held (scroll messages, scroll
  cursor, the ending click held back) - NT 3.1 and NT 4.0 through the
  driver, NT 4.0 SP6 through the mouse hook.
- `test/w5tree.py` (Windows 2000 / XP x64, one boot): Device Manager after
  `usbinst /install` (`fstest devlist`): the controller's name, the root hub
  below it, a hub below the root hub, keyboard and mouse below the hub, the
  stick below the root hub, USBNT USB Keyboard / Mouse and the disk in their
  classes, no device with a problem code; keys, mouse motion and a click on
  the USB devices reach a window, the stick is read and written.
  `test/toemboot.py` checks the same nodes after a text-mode-setup install.
- `test/tmouse5.py` (harness): buttons 4 and 5 of a USB mouse on every
  controller; `test/host/thidparse.c`: report descriptors with buttons 4
  and 5 in a field of their own; `test/host/taml.c`: the AML interpreter and
  the GPIO resolver against hand-built ACPI tables and a fake MMIO / IO space
  (conditional _CRS, _DSM from a Package, _PS0 writing a SystemMemory field,
  _STA, a While loop with a method call, GPIO pad set / get).
- `test/tsmbus.py` (harness, ~40 s): touch pads on SMBus with the QEMU test
  pads `smbus-rmi4tp` (RMI4: F12 or F11, F30 or F3A) and `smbus-elantp` on
  the ICH9 SMBus of the q35 machine and on `amd-fch-smbus` (an AMD FCH model:
  PM registers through 0CD6h or memory, port 1, semaphore) - set up, pointer,
  two finger scrolling (wheel), one and two finger taps, the pad's click; the
  ACPI id list (an SSDT with `LEN2054`); the PS/2 switch: QEMU's PS/2 mouse
  stays PS/2, scripted PS/2 sides of a Synaptics InterTouch and an Elan pad
  (QEMU has no PS/2 touch pad) are switched or, without a listed id, left on
  PS/2; `SmbusTouchpad`; `Smbus=0`. `test/host/tsmbtp.c`: the PS/2
  identification, RMI4 register descriptors, F11 / F12 / F30 / F03 data, Elan
  reports and the trackpoint packets.

Verified in NT itself (QEMU): NT 3.1 SP0, NT 3.51 and NT 4.0 SP6 -
installation with usbinst, logon with a USB keyboard, tablet, BOT and UAS
disks with byte-for-byte copy checks, dynamic drive letters, hot plug,
usbtree, safe removal and the wheel. Thunderbolt / USB4 with NVMe
(`ntnvme.py`, q35 machine, usb4-nhi model): NT 4.0 - USB4 dock with the
driver as connection manager, and Titan Ridge firmware in secure mode
(key stored in the registry, challenge on the next boot) - NVMe in the
dock read and written next to an NVMe drive driven by nvme2k; NT 3.1 -
the same dock and drive (nvme2k's own drive did not come up in this NT
3.1 setup, see nvme2k). Disks with several partitions (`ntpart.py`): NT 3.1
SP0 and NT 4.0 SP6 on EHCI and xHCI, the stick plugged after logon and there
at boot. NT 3.5: boot, enumeration and hot
plug in QEMU, plus a full manual test by the author. Real hardware: an Intel
i7-3770 system.

Windows 2000 / XP (`test/vm5.py`, Windows 2000 SP0 and XP x64 SP1 guests):
`w5usb.py` installs with usbinst and checks the xHCI, EHCI and UHCI
controllers, keyboard, mouse, a stick read and written (16 MB), Safely
Remove, plugging in again with the data kept, and pulling it out while
mounted; `w5part.py` a stick with FAT16, FAT32 and NTFS primary and
logical partitions: every partition gets a letter and is read, the NTFS one
written, Safely Remove, another stick in the same port, the first one back
and pulled out while mounted (`--usbstor`: through Windows' usbstor, the
filter set by `usbinst /filters`, pulled out and plugged in again;
`--card`: the medium of a card reader taken out and another put in;
`--gpt`: a GPT stick) - passes on Windows 2000 SP0 and XP x64 SP1, and with
`--card`, `--usbstor` and `--gpt` on XP x64 (the Windows 2000 image here has
no usbstor.sys); `w5usbdi.py` gives every device to Windows' drivers (`/all`, `UsbPdo=2`):
Windows' own usbstor (a stick on xHCI read, written, pulled out) and
hidusb / kbdhid, a serial adapter as a device without a driver; `w5cam.py`
captures with Video for Windows (single frames, a two second sequence) and
DirectShow (a graph running three seconds), on x64 also from a 64 bit
program (USBNT's camera driver, `WindowsCamera=0`). `w5native.py` checks the
controller policy as an upgrade from an earlier test build (`bin/`, which took every
controller): its usbinst first, then this one, then a restart - the UHCI
goes back to uhcd (2000) / usbuhci (XP x64), the EHCI to usbehci on XP x64
and stays USBNT's on 2000 SP0, the xHCI stays USBNT's, the old INF leaves
the INF folder; keyboard and mouse work through Windows' hidusb, a stick on
the EHCI (usbstor on XP x64, USBNT on 2000) and one on the xHCI (USBNT) are
read and written; `--comp` puts a full speed hub with the keyboard and
mouse on a port of an ICH9 EHCI that USBNT drives while Windows drives its
UHCI companions: USBNT hands the hub over, the keyboard and mouse work
through uhcd and hidusb, the high speed stick on the same EHCI stays USBNT's.
`w5wincam.py`: on Windows 2000 usbcam (frames, photo with the colour bars
checked, AVI, the window) and captest (avicap32, DirectShow) with USBNT's
camera driver; on XP x64 the camera on USBNT's xHCI goes to Windows (USBNT
bus, usbccgp configures it, usbvideo.sys is installed on its function), but
XP x64's usbvideo.sys does not start on QEMU's test camera - neither on
USBNT's xHCI nor on Windows' own EHCI (Code 10 before its first request,
also with a UVC 1.0 variant of the test camera) - so streaming through
usbvideo.sys and usbcam's Video for Windows backend are not verified. `w5mouse.py` checks
WM_XBUTTONDOWN / WM_APPCOMMAND from the USB mouse's back / forward buttons
and autoscroll through the mouse hook. AUDIONT's `tools/w5audio.py` plays through HD Audio, AC'97 and a
USB Audio 2.0 device on USBNT.

**Text-mode setup (TXTSETUP.OEM).** `test/toemfmt.py` checks the driver
disk against the TXTSETUP.OEM rules of every setup version (sections, line
forms, file types each version knows - no `catalog` for NT 3.x / 4.0 -,
8.3 names, `[Config]` values, `[HardwareIds.scsi.*]`, the right driver
binary behind each choice, the INF offering the same PCI ids) and the
layout integration tools need: one Windows family per disk, every file -
also the one the INFs' `[SourceDisksNames]` point to - in the root (the
disk of earlier test builds fails this: its `\I386` layout is what broke nLite). `test/toemboot.py GUEST` makes on an installed guest
what text-mode setup leaves behind (no usbinst: service boot start in group
SCSI miniport, the `[Config.usbnt]` values; 2000 / XP: the
CriticalDeviceDatabase entries of the `[HardwareIds.scsi.*]` ids) and then:
*late* (NT 3.1, 4.0) starts usbnt.sys after logon, after kbdclass, as setup
loads it - a USB keyboard on xHCI, the only thing typing, types into a
window through i8042prt's class driver; *boot* starts with a USB stick and
keyboard on xHCI - stick read and written, the keyboard types, NT: kbdclass
connects to USBNT's port (ConnectMultiplePorts set by the driver), 2000 /
XP: the host controller is the device bus; *usbboot* starts the same
system from a USB disk on xHCI as the only disk (SeaBIOS boots it) and
writes a file to it. Passed on NT 3.1 (with the SCSI disk class driver
boot start, as setup makes it for a system on a SCSI disk), NT 4.0,
Windows 2000 and XP x64. `w5usb.py` (usbinst, USBNT\Bus device) passes
with the boot start INF. `test/ttxtsetup.py OS --iso ...` runs text-mode
setup itself with the driver disk (F6 / S, the USBNT choice, the USB
keyboard in setup, the USB disk on the partition screen); `--usb-only` takes
the PS/2 keyboard away (every key from the USB keyboard, through the BIOS
in setupldr, through usbnt.sys from *Welcome* on), `--ide` adds an empty
IDE disk, `--full` goes on from the partition screen through the format
choice to formatting. Windows 2000 SP0 with F6, no PS/2 keyboard, `--ide
--full`: passed - the USB keyboard from *Welcome* to formatting and the
1 GB USB disk on the partition list (usbnt.sys takes the controller itself,
see above). `tnlite.py` passed with `--ids all`, `pci` and `none`.

`test/tnlite.py --iso W2K.ISO [--choice usbnt2k|usbnt2ka] [--hc xhci|ehci|uhci] [--ids all|pci|none] [--debug]`
integrates `OEMDISK\2KXP` into a copy of a Windows 2000 / XP CD the way
nLite 1.4.9 does with a TXTSETUP.OEM (the driver and INFs into `I386`;
TXTSETUP.SIF `[SourceDisksFiles]`, `[HardwareIdsDatabase]` `id = "usbnt"`
without a class, `[SCSI.Load]`, `[SCSI]`; DOSNET.INF `[Files]`) and runs
text-mode setup from it on a machine whose keyboard controller has no PS/2
keyboard (QEMU `-global ps2-kbd.absent=on`, `test/qemu/ps2-kbd-absent.patch`):
the only keyboard is a USB keyboard, which only USBNT drives on xHCI. It
must take setup from *Welcome to Setup* (ENTER), the new-disk screen (C),
the license (F8), the partition (ENTER) and the format choice (ENTER) to
formatting / copying files, and a 1 GB USB disk (with a disk signature) must
be on the partition list; `--ids pci` / `none` leave the non-PCI / all ids
out of `[HardwareIdsDatabase]`; with `--debug` (usbnt's log on COM1 through the
setup hive) also: kbdclass connected to USBNT's keyboard, no key lent to
another keyboard's class driver. Passed with the Windows 2000 (SP0) CD on
xHCI (both choices), EHCI and UHCI; the driver without the class fix stays at
*Welcome*.

**USB audio test device.** QEMU's own `usb-audio` is a fixed 48 kHz
UAC1 speaker. The tests use a QEMU 8.2 with three additions (patches in
`test/qemu/`): a `usb-uac` device (UAC1 full speed or UAC2 high speed,
16/24/32-bit playback alternate settings, 1000/1500 Hz capture, 44.1 /
48 / 96 kHz, asynchronous feedback with a configurable clock offset,
feature units, USB MIDI), siTD execution in the EHCI emulation and a
high speed `usb-hub` (`hs=on`) with a transaction translator. Further test
devices: `usb-uvc` (camera: YUY2 and MJPEG, iso or bulk, controls),
`dw-i2c-pci` (Intel LPSS DesignWare I2C) with `i2c-hid` (touch pad, touch
screen, keyboard), `usb-rtsx` (Realtek USB card reader protocol) and
`rtsx-pci` (Realtek PCIe card reader programming model) and `usb4-nhi`
(a Thunderbolt / USB4 host interface with ring 0 in raw mode, a USB4
host router and device routers, or an Intel firmware connection manager
in the Falcon / Alpine / Titan Ridge, Ice Lake or Tiger Lake variant). The
Realtek models check the command, DMA and SD protocol paths; they accept
any chip set-up sequence, so the chip specific register values are only
as good as the Linux drivers they follow. `usb4-nhi` checks every frame
(descriptors, CRC-32C), the routes, router set-up order, hop tables,
plug event acknowledgements and the firmware messages, and counts every
deviation; the Thunderbolt protocol facts come from the USB4
specification and Linux's thunderbolt driver (no code taken).

## Known limitations

- Multimedia keys are not supported.
- Autoscroll on NT 3.x and NT 4.0 before SP3 works with the mice usbnt.sys
  drives (not with a mouse left to i8042prt with `/keepi8042`) and scrolls
  windows that have standard scroll bars (WM_VSCROLL / WM_HSCROLL); on NT 4.0
  and later it scrolls with WM_MOUSEWHEEL, sideways only windows with a
  horizontal scroll bar. The origin mark and the scroll cursor cannot stop a
  program from setting its own cursor while the pointer is over it.
- Intel Smart Sound (SST / AVS) DSP audio, SoundWire and I2C amplifiers
  need DSP firmware and ACPI; that is AUDIONT's business and not done
  (HD Audio codecs on SST systems in HDA mode work with AUDIONT).
- Thunderbolt / USB4: tested against the QEMU model only (no real
  controller here). No DisplayPort tunnels (NT has no use for them), no
  host-to-host (XDomain) networking, no NVM firmware updates, no
  Thunderbolt 5 asymmetric links (120 / 40 Gbit/s, a DisplayPort feature).
  Discrete Thunderbolt 3 controllers that the BIOS powers off while nothing
  is connected are not used; Thunderbolt 1 / 2 controllers of PCs are left
  to the BIOS. Hot plugged controllers and drives are polled (no interrupt
  line can be added at run time).
- HID over I2C devices are polled every 8 ms (30 ms when idle); where the
  firmware describes a GpioInt "data ready" line the pad level is read first
  and the device is read only when it is asserted (NT takes no GPIO
  interrupt). The AML interpreter evaluates only the methods listed above and
  writes only the operation regions those methods use; anything it cannot
  evaluate makes it fall back (the usual addresses, blind polling). Gestures
  are scrolling and taps (no pinch zoom, no three/four finger swipes).
  eMMC is not supported.
- Touch pads on SMBus: tested against the QEMU models only (no such
  notebook here); the register layouts and the PS/2 switch follow Linux.
  Polled (no Host Notify), at most four contacts per report are read (enough
  for the gestures), Synaptics ForcePads (F21) are left on PS/2. An Intel
  SMBus that the firmware hides behind the P2SB is not found. Windows 2000 /
  XP: see *Touch pads on SMBus* (no PS/2 switch without the i8042prt
  filter).
- Not tested on SMP NT.
- Partitions: NT 3.1 - 4.0 give letters to at most `MaxPartitions` (8,
  up to 24) partitions per disk (usbtree lists them all). Windows 2000 / XP:
  a disk reported as fixed is a new disk for each medium, so a card swapped
  in a reader is a new device (Windows 2000 shows *Unsafe Removal of
  Device* when one is pulled without *Safely Remove Hardware*, as for any
  fixed USB disk); for the disks of Windows' usbstor that depends on the
  filter, which a disk gets only after it was there once (usbmon then
  restarts it).
- Text-mode setup was run with a Windows 2000 (SP0) CD only (see Testing);
  XP / 2003 / x64 setup media were not available.
  NT 3.x / 4.0 start from a USB disk only through the BIOS
  (`multi(...)` in BOOT.INI), not with `scsi(...)` / NTBOOTDD.SYS.
- Windows 2000 / XP: no selective suspend and no remote wake-up for the
  devices that go to Windows' drivers; an isochronous request of such a
  driver is limited to 256 KB. Controllers are polled unless
  `UseInterrupts` is set. Video for Windows offers no list of a Windows
  camera's formats: usbcam shows the current one and the driver's format
  dialog. Intel's EHCI / xHCI port switching is untested (QEMU's xHCI is
  not an Intel one). A camera on USBNT's controllers with Windows' camera
  driver (`WindowsCamera = 1`) is tested only up to usbvideo.sys being
  installed on it (see Testing); if a camera does not work that way,
  `usbinst /install /set WindowsCamera=0` gives it back to USBNT's camera
  driver. A PCIe device hot plugged behind Thunderbolt /
  USB4 is USBNT's (Windows' PCI driver does not see it). The drivers are
  unsigned.
- On NT 3.51 under QEMU the guest needs `-icount shift=auto,sleep=on`;
  without it the guest clock stalls during boot (an emulator timing
  issue, not seen on real hardware or on NT 3.1/3.5/4.0).

## Changelog

The changes of every release are in [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0, see [LICENSE](LICENSE).

**USE AT YOUR OWN RISK.** This driver talks directly to USB host
controllers and storage devices on operating systems that never
supported USB. Keep backups of the data on the disks you use with it.
