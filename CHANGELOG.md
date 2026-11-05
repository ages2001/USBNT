# Changelog

## v1.3 (validation candidate)

Changelog:

- Fixed USB disks behind a Fresco Logic FL1100 xHCI controller (ExpressCard USB 3.0, 1B73:1100) that showed up in usbtree but were never mounted: the controller reports a short packet as success with bytes left, which was not seen as the end of the transfer, so the transfer timed out. A success event with bytes left is now a short packet on every xHCI controller (as Linux does). xHCI 0.96 controllers get the TD size field in their own unit (KB left).
- Fixed card readers and USB sticks whose CSW data residue is wrong: a full INQUIRY or READ CAPACITY reported with a residue marks the device, and its residues are ignored from then on (Linux finds them the same way). Before, such a reader failed with "inquiry failed" or returned short reads. Windows 2000 / XP have 8 disk slots by default (was 4), so a multi slot card reader (four or more LUNs) fits next to other USB disks.
- Drive letters of USB disks come after the last internal partition and CD / DVD drive (LettersAfterLast = 1), not in the first free letter after C:. NT 3.x / 4.0: the letters NT gave the USB slots at boot are taken back and a CD-ROM that came right after them moves down into the gap, unless it has a letter set in Disk Administrator (CompactBootLetters = 1). Windows 2000 / XP: a USB volume the mount manager put before the last internal drive is moved after it once; a letter changed later in Disk Management stays.
- Fixed usbcam freezing when Brightness or Contrast was changed while the camera ran: the window and the capture thread shared one handle, on which Windows runs one request at a time, so every change waited behind frame requests. Frames now come through a handle of their own and the controls are set by a separate thread that sends only the newest value.
- Touch pads: a two button ClickPad (Synaptics) is a ClickPad too, and a ClickPad's button is also taken from the left button bit; the tap time is TouchpadTapTime (250 ms, was a fixed 200 ms, which short real taps often missed).
- New mouse settings for every mouse and touch pad: MouseSpeed (percent), MouseAccel, MouseInvertWheel, MouseSwapButtons; PS/2 mice: Ps2MouseRate (reports per second) and Ps2MouseResolution; USB mice: UsbMouseInterval (polling interval in ms).
- A USB network adapter whose first configuration is CDC Ethernet (ECM / NCM) and which has a vendor-specific configuration too runs in the vendor configuration, which is the one its vendor drivers (RLTKLANT for the Realtek RTL815x family) use; Linux chooses it the same way. Under Windows 2000 / XP a whole-device driver sees that configuration as configuration 0. UsbVendorConfig = 0 keeps the first configuration.
- usbinst /remove also deletes the tools, the driver file (at the restart when it is in use) and HKLM\Software\UsbNt.
- NT 3.1 - 4.0: the clock tick is checked once against the real time clock (12 s after the start); when the HAL's clock interrupt does not come at the rate it reports (an MPS HAL with a wrong local APIC timer calibration), the measured rate is used, so port reset, enumeration and the other timeouts of hot plug keep their real length (ClockCheck = 0 turns this off).
- Preserve the existing host-controller parent during upgrades without a root
  USBNT bus. A started, healthy USBNT controller is detected through dynamically
  loaded Configuration Manager APIs, keeping NT 3.x imports unchanged.
- Describe external generic clients as "No built-in driver" in the tree; the
  class flags alone cannot determine whether an external network client exists.
- Add consistent 1.3.0.0 file/product version resources to all distributed
  drivers and tools. Generate INF and archive versions from one product header,
  separately from the generic kernel interface version. Keep the intentionally
  old INF ranking date so inbox controller drivers retain their default priority.
- Avoid GNU ld 2.44's premature resource-section extent diagnostic by using
  valid 4 KiB file alignment. The original usbtree fix preserves every loaded
  section and resource payload; icons and the tree render on NT 3.51.
- Include the generic client module and version resource in the legacy DDK
  source list. Preserve historical unversioned OEM images by placing new images
  inside the versioned distribution directory.
- Preserve root USB device objects, native owners and Windows pipe/PDO handles
  across controller D3/D0. Drain all owners before stopping DMA, restore device
  identity/configuration/interfaces on D0, and retain ownership gates on failed
  restoration so a later D0 can retry. Report an actually unplugged or changed
  device through PnP. Reject hubs, unknown owners, retained generic-client state
  and active EHCI companions before entering unsupported sleep topologies.
- Drain polling before releasing controller-private memory and serialize
  controller table replacement with polling readers. Copy PnP controller
  metadata under the same lock, so names and identifiers do not dereference
  retired controller objects. Resume companion polling when a sleeping EHCI
  owner has been removed.
- Retain synchronous transfer events, transfer records and DMA buffers until
  their completion callback finishes, including timeout/cancellation races.
  Restore the caller context after each segment and after submission failure.
- Drain queued BOT stages, UAS submissions and collected callbacks before
  transport recovery or removal. Reuse UAS tags only after their callbacks
  finish. Restore native BOT/CBI/UAS endpoints during shared physical reset
  while preserving disk slots; fail queued/new I/O after failed restoration.
- Pin audio, feedback, MIDI and camera callbacks/submissions until their last
  access. Close admission before teardown, drain camera readers, and notify
  audio clients of removal only after callbacks stop using their context.
- Validate the complete Windows SELECT_CONFIGURATION interface list before
  closing pipes or changing hardware. Reject foreign composite interfaces,
  truncated records and insufficient endpoint storage without side effects.
- Coordinate Realtek USB card-reader reset with native and Windows siblings.
  Rebuild register endpoints, power-cycle/reinitialize the SD protocol session,
  retain the SCSI target, and keep failed-reset devices unavailable until retry.
- Restore native camera streams and cached controls after shared physical reset,
  retaining camera IDs and frame sequence continuity. Drain readers and callbacks
  before endpoint teardown; failed restoration remains gated for retry.
- Restore native audio/MIDI interfaces after shared physical reset, preserving
  requested formats, client contexts, volume and input selection. Propagate clock,
  rate, pipe and initial submission failures; retain the original settings across
  failed retries instead of replacing them with partially initialized state.

- Version the generic kernel device interface separately from the product.
  Interface version 2 uses attachment tokens that are not reused after unplug,
  rejects stale handles, and drains in-flight transfers before freeing pipes.
- Serialize generic device operations and wait for active arrival/removal
  callbacks during client unregister. Registration replay and device removal
  now share callback ownership; clients must not unregister inside their own
  callback.
- Return endpoint clear-halt and host-toggle-reset failures to Windows USB
  clients instead of reporting success. Keep queued work from running through
  a failed reset.
- Implement actual root-port and hub-port reset for Windows-owned USB device
  functions. Drain sibling functions, rebuild device addressing and endpoint
  zero, verify device identity, restore configuration and alternate settings,
  and reopen endpoint contexts while retaining public pipe handles.
- Retain stopped state after partial reset/restore failure, clean up partial
  endpoint allocations, and allow a later reset attempt to recover.
- Serialize physical reset against generic client operations. Reject reset
  without modifying hardware when a generic client has an active call,
  attachment callback, device claim or cached endpoint. Reject new client
  transfers while reset is running and after failed restoration.
- Drain a Windows-owned function's queued and active transfers on D3,
  preserving sibling functions and endpoint contexts/data toggles. Resume
  admission on D0 without clearing a failed-reset condition. Defer power
  requests received at DISPATCH_LEVEL to the USB worker and reject
  unadvertised D1/D2 states. This is function idle, not port suspend or a
  claim of complete system sleep/resume support.
- Build source archives from the current files with a per-file SHA-256
  manifest and verified ZIP readback. Preserve existing versioned release
  outputs instead of deleting them or archiving an unrelated Git HEAD.
- Defer controller device-power transitions to passive work items, retain
  removal protection until completion, and propagate allocation, lower-stack
  and controller-restart failures. Request matching device power IRPs for
  system power requests without blocking a system worker waiting for another
  worker. Keep the controller stopped after a failed resume and attempt to
  restore it when the lower stack rejects a power-down.
- Make the WDM build's DDK include directory configurable through
  `MINGW_DDK_INCLUDE`.
- Propagate controller START_DEVICE failures instead of publishing a bus
  whose hardware failed to start.
- Release private UHCI/OHCI/EHCI/xHCI allocations before replacing a stopped
  controller. Refuse release while DMA or endpoint owners remain active;
  retain xHCI scratch-buffer virtual addresses for correct cleanup. Release
  the old MMIO mapping when replacement succeeds. Reuse the stopped
  controller table slot, including when all 16 slots are occupied.

- Defer a replacement Windows USB function while the previous PDO with the
  same instance ID is still being removed, including valid serial IDs and
  composite-interface/topology IDs.
- Retain function allocations while deferred requests still own them after
  removal. Reap them on the USB worker after final rundown instead of freeing
  live state after a fixed five-second timeout.
- Reject a fifth native HID interface on one composite device before opening
  its pipe. Previously it could be published outside the per-device removal
  table, leaving an active transfer and allocation behind after unplug.

- Drain native HID submitters and callbacks before closing/freeing their
  endpoint state, including completions already collected by another CPU.
  Prevent worker rearm from reusing a transfer still read by its callback.
- Coordinate Windows physical-port reset with native HID interfaces: pause
  input, release held keys/buttons, restore protocol/idle and endpoint state,
  and resume only after all owners restore successfully. Retain failed-reset
  gating and allow retry without replacing HID handles. HID, storage, card-reader,
  video and audio owners now coordinate reset; hubs and unknown owners remain
  rejected before hardware changes.

Validation and release status:

- The native HID rundown/reset candidate passes 8,000 concurrent host schedules,
  1,300 composite restore/failure schedules and 80 physical HID resets across
  QEMU UHCI/OHCI/EHCI/xHCI. Exact installed network regressions pass NT3.1,
  NT3.51, NT4, Windows 2000 AHCI and XP x64 IDE, with normal shutdown and SYS
  readback. See the dated companion validation proofs for scope and limits.

- Host tests exercise callback/transfer rundown, stale attachment tokens,
  failed endpoint recovery, physical-port reset sequencing and composite
  Windows-function restoration, generic-client reset exclusion and function
  D0/D3 sibling isolation. These mock host-controller behavior.
- QEMU Windows 2000 tests exercise the actual USBNT RESET_PORT path and
  subsequent network recovery. XP inbox USB-stack tests are recorded
  separately and must not be presented as USBNT WDM coverage.
- Native class reset tests pass on the applicable QEMU controller topologies.
  Installed Windows 2000/XP x64 mixed NIC/HID reset and retained root-device
  controller power tests also pass for the prior development snapshot. Hubs,
  unknown native owners and active EHCI companions reject unsupported sleep.
  Full-machine ACPI S3 and broader controller/speed coverage remain open.
- Versioned 1.3 binaries, installers and setup disks are built and undergoing
  exact installed validation; no final release is declared. Historical claims are not new
  validation of this candidate. QEMU does not certify physical USB/PHY/MCU,
  SuperSpeed signal behavior or Thunderbolt/USB4 hardware.

## v1.2

Changelog:

- Added a generic USB device interface, `\Device\UsbNtDev` (`src/usbntcli.h`,
  `src/ntcli.c`, `__cdecl`): USBNT now exposes **every** enumerated USB device
  to other kernel drivers, the network/vendor counterpart of the audio
  interface (`\Device\UsbNtAudio`). A client registers a `UC_CLIENT` (arrival
  / removal callbacks), is told about each device with its vendor / product /
  class, and drives the ones it supports with endpoint-0 control transfers,
  bulk / interrupt transfers (the pipe is opened on first use and kept, so
  the data toggle is preserved) and descriptor reads; it can claim a device.
  This lets drivers such as **RLTKLANT** (Realtek USB Ethernet) run on top of
  USBNT on the systems that have no USB stack of their own. The core calls the
  interface as a device finishes enumerating and as it is removed; nothing
  else in the stack changed.
- Fixed pipe opening in that interface (`GetPipe` in `src/ntcli.c`): the
  endpoint-descriptor scan started at the configuration descriptor, so
  `UsbFindDesc` stopped at the interface boundary before it reached any
  endpoint and every bulk/interrupt transfer failed with "no pipe". It now
  locates the interface descriptor first and then walks its endpoints one
  descriptor at a time, so a client can open the bulk IN/OUT pipes and move
  data. (Control transfers on endpoint 0 were unaffected.) Verified on-target
  under QEMU: with a device **present at boot**, NT 4.0 and NT 3.51 bring a
  Realtek `usb-rtl8153` up through `\Device\UsbNtDev`, read the MAC over
  control transfers and complete a full DHCP DISCOVER/OFFER exchange over the
  bulk pipes.
- Known issue (runtime hot-plug): a USB device **plugged in after boot** is
  enumerated by the pristine 1.1 build under this QEMU/NT 4.0 harness but not
  by this build; a device present at boot works in both. Extensive bisection
  showed the USB enumeration / worker / EHCI / timing code is byte-for-byte
  identical to 1.1, so this is not a located source defect - it tracks the
  larger 1.2 image (the added ACPI/AML, SMBus, Thunderbolt, SD/MMC, card-reader,
  UVC and I2C-HID subsystems being linked in) and reproduces only in the
  emulated-timer environment. On hardware where 1.1 hot-plug works, the same
  generic-interface path applies to USB Ethernet unchanged.
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
