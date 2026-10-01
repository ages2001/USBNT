# USBNT - USB Driver for Windows NT 3.1 / 3.5 / 3.51 / 4.0

## Overview

USBNT is a USB stack for the first generation of Windows NT. A single
driver file, `usbnt.sys`, runs unchanged on **NT 3.1, NT 3.5, NT 3.51 and
NT 4.0** and covers every common USB host controller, keyboards, mice,
tablets and mass storage, including USB 3 SuperSpeed and UAS.

None of these Windows versions has USB support of its own. A Turkish user
guide that ships with the release package is in [README.TXT](README.TXT).

## Why?

Old NT installations on newer machines (and in virtual machines) often
have no PS/2 ports and no floppy drive left; USB is the only way to get a
keyboard, a mouse and removable storage. USBNT makes a USB-only PC usable
under NT 3.x and NT 4.0.

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

**Devices**

- Hubs: USB 1.1, USB 2.0 and USB 3 (SuperSpeed) hubs, chained
- Keyboard: boot protocol, typematic repeat, Caps/Num/Scroll lock LEDs
- Mouse: buttons and wheel
- Tablet / absolute pointer (QEMU and VirtualBox "USB Tablet")
- Mass storage: Bulk-Only, CBI and **UAS** (USB Attached SCSI, with
  streams on xHCI: up to 8 commands in flight). USB sticks, external
  disks, multi-LUN card readers, USB CD/DVD drives. Hot plug and surprise
  removal.

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
| `usbmon.exe` | Started at logon: dynamic drive letters, wheel support for NT 3.x, 1 ms timer |
| `usbtree.exe` | GUI tree of controllers, ports, hubs and devices with USB version, speed, disk size, file system, cluster size; safe removal |
| `usbeject.exe` | Command line safe removal |
| `wheeltst.exe` | Wheel test window |

## Supported systems

| | NT 3.1 | NT 3.5 | NT 3.51 | NT 4.0 |
|---|---|---|---|---|
| Driver (one binary) | yes | yes | yes | yes |
| Keyboard, mouse, tablet | yes | yes | yes | yes |
| Mouse wheel | via usbmon (WM_VSCROLL) | via usbmon | via usbmon | native WM_MOUSEWHEEL |
| Mass storage, UAS, hot plug | yes | yes | yes | yes |
| Dynamic drive letters | yes | yes | yes | yes, Explorer refreshes itself |

The driver imports only kernel and HAL functions that exist in every
version from NT 3.1 RTM to NT 4.0. Entry points that appeared later
(`HalGetBusDataByOffset`, for example) are looked up at run time; NT 3.1,
whose HAL has no PCI support, gets PCI configuration mechanism #1 through
ports 0CF8h/0CFCh. The build scripts check every import against the
export lists of all five kernels (3.1 RTM, 3.1 SP3, 3.5, 3.51, 4.0) and
reject P6-only instructions, so the binaries also run on 486 and Pentium
machines.

## Architecture

```
src/      usbcore.c   device enumeration, pipes, transfer queues
          hcuhci.c hcohci.c hcehci.c hcxhci.c   host controller drivers
          usbhub.c usbhid.c hidparse.c usbmsc.c class drivers
          usbos.h     the only interface to the operating system
          ntos.c ntmain.c ntscsi.c ntinput.c    Windows NT layer
tools/    user mode tools (no C run time)
harness/  bare metal test kernel that runs the portable core under QEMU
test/     QEMU test scripts: harness tests and full NT VM tests
build/    MinGW build scripts and the NT export lists used for checking
```

The USB core and class drivers are portable C and talk to the OS only
through `usbos.h`. The NT layer creates a SCSI port device for the class
drivers, keyboard and pointer port devices for kbdclass/mouclass, and a
control device (`\\.\UsbNt`) for the tools. The same core is linked into
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
   - `/set Name=Value` - store a driver parameter (decimal or 0x hex),
     may be repeated, e.g. `/set MaxTransferKB=512 /set DebugPort=0x2F8`
3. Restart Windows NT.

`usbinst` copies the files, registers `usbnt` as a boot driver in group
"SCSI miniport", sets `ConnectMultiplePorts=1` for kbdclass and mouclass
(PS/2 and USB input work side by side) and appends `usbmon.exe` to the
Winlogon `Userinit` value.

`usbinst /remove` removes the service and the logon entry,
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

## Usage

**Drive letters.** At boot NT reserves a letter for every USB disk slot.
`usbmon` removes the letters of empty slots, gives a disk a letter when it
is plugged in (the letter it had last time if that one is free) and takes
it back when the disk is removed. NT 4.0 Explorer updates itself; the
File Manager of NT 3.x reads its drive list only when it starts, so new
letters show up there after File Manager is reopened. Set
`HKLM\Software\UsbNt\DriveLetters` to 0 to keep the fixed boot letters.

**Safe removal.** Select a disk or a storage device in `usbtree` and press
*Safe removal* (Alt+S), or run `usbeject D:`. `usbeject /list` lists the
slots.

**Wheel on NT 3.x.** NT 3.x has no WM_MOUSEWHEEL; `usbmon` turns wheel
movement into WM_VSCROLL for the window under the cursor (MDI children
included). `HKCU\Software\UsbNt`: `WheelLines` (3), `WheelTarget`
(0 = window under the cursor, 1 = foreground window).

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
| IntelRouteToXhci | 1 | Route Intel EHCI ports to xHCI |
| MaxPciBus | 255 | Highest PCI bus scanned |
| WheelMode | 0 | 0 = automatic, 1 = MOUSE_WHEEL only, 2 = usbmon only |
| ReportWheel | 1 | Report the mouse as a wheel mouse |
| DisableKeyboard / DisableMouse / DisableStorage | 0 | Ignore that device class |
| BootWaitMs | 1500 | Time given to devices at boot |
| DebugLevel | 2 | 0 = off, 1 = errors, 2 = info, 3 = debug, 4 = verbose |
| DebugPort | 0 | Serial port for the log (e.g. 0x2F8), 0 = DbgPrint |

`usbmon`: `HKLM\Software\UsbNt` `DriveLetters` (1), `TimerResolution` (1).

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

The tools use no C run time library. Both steps build without warnings;
the release binaries are built this way inside an NT 3.51 VM.

**Test build (Linux, MinGW-w64).** `build/ntbuild.sh` and
`build/tlbuild.sh` build the driver and the tools into `build/out` and
check imports and instruction set against all supported NT versions.

## Testing

- `test/runall.sh` builds the bare metal harness and runs the portable
  core against QEMU's UHCI, OHCI, EHCI (with companions) and xHCI:
  enumeration, keyboard, mouse, tablet, hubs, hot plug, multi-LUN card
  readers, CD-ROM, large transfers, UAS with and without streams, and
  surprise removal during writes.
- `test/tnt.py`, `tgui.py`, `tinst.py`, `tddk.py` boot real NT installs in
  QEMU (images in `$USBNT_NTIMG`).

Verified in NT itself (QEMU): NT 3.1 SP0, NT 3.51 and NT 4.0 SP6 -
installation with usbinst, logon with a USB keyboard, tablet, BOT and UAS
disks with byte-for-byte copy checks, dynamic drive letters, hot plug,
usbtree, safe removal and the wheel. NT 3.5: boot, enumeration and hot
plug in QEMU, plus a full manual test by the author. Real hardware: an Intel
i7-3770 system.

## Known limitations

- No isochronous transfers (audio, cameras).
- Keyboards use the boot protocol; multimedia keys are not supported.
- Not tested on SMP NT.
- On NT 3.51 under QEMU the guest needs `-icount shift=auto,sleep=on`;
  without it the guest clock stalls during boot (an emulator timing
  issue, not seen on real hardware or on NT 3.1/3.5/4.0).

## License

GPL-3.0, see [LICENSE](LICENSE).

**USE AT YOUR OWN RISK.** This driver talks directly to USB host
controllers and storage devices on operating systems that never
supported USB. Keep backups of the data on the disks you use with it.
