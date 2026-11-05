# USBNT 1.3 release status

> [!NOTE]
> **Status:** USBNT 1.3 is a validation candidate, not a completed release.
> The changelog, drivers, tools and INF metadata all carry version 1.3.
> All results below come from QEMU guests and the bare-metal harness.

## Changes of 2026-11-03

These changes are listed in the [changelog](CHANGELOG.md):

- Fresco Logic FL1100 short packets
- card reader residues
- drive letters after the last internal drive
- usbcam controls
- mouse and touch pad settings
- clock check against the RTC
- uninstall cleanup
- vendor configuration of CDC-first network adapters

Results:

| Check | Result |
|---|---|
| Harness basic test, xHCI / EHCI / UHCI / OHCI | PASS |
| Harness, FL1100 short-packet emulation | PASS |
| Harness, hot plug on xHCI and EHCI | PASS |
| Harness, CDC-first RTL8153 on xHCI and EHCI configured in its vendor configuration | PASS |
| NT 3.1, standard HAL, xHCI and EHCI: plug, remove and plug again a keyboard, mouse, stick and USB audio | PASS |
| NT 3.1, stick swap and stick plugged in only after logon | PASS |
| XP x64, USBNT xHCI: CDC-first RTL8153 (Lenovo id) with RLTKLANT, DHCP and ping | PASS |
| NT 3.1 with the MPS HAL (HALMPCPU) | not working under QEMU; analysed in the delivery notes, not fixed here |


## Versioned candidate — 2026-11-02

`frozen-builds/usbnt-13-candidate-20261102/manifest.json` records three drivers,
nine tools, verified 1.3.0.0 PE resources, 58/106/95 real-kernel imports and
byte-for-byte readback from all three 1.44 MB OEM images. Kernel behavior is
unchanged from controller-sleep-v3; version resources and file layout changed.
Exact installed tests and package-installer upgrades are now closed for the
recorded fixtures; broader release boundaries remain below.

`nt351-timerprobe-closed-proof-20261102.json` records measured 10 ms / 1 ms /
10 ms timer phases, nine RTL8159 attachments at 2.5–3.3 seconds and 36 verified
RTL8139 payload replies. This does not reproduce or explain the prior slow
attachments; no causal timing fix is claimed. The original usbtree resource
warning is now explained by an intermediate linker output-size check, fixed
through legal file alignment, and checked with actual NT3.51 tree/icon rendering.
The before/after loaded code and resources are identical.

## Exact 1.3 upgrade and regression closure

The newer tools are frozen in `usbnt-13-upgrade-v2-20261102`. Windows 2000
upgrades now preserve the existing host-controller parent instead of creating
a new root bus that changes child IDs and loses NIC bindings. The same-parent
before/after comparison retains the failing old installer and verifies the
fixed upgrade without reinstalling the NIC. XP x64 preserves its already
existing root-bus instance; both x86 and native x64 installer copies are read
back exactly after normal shutdown.

`usbnt-13-legacy-closed-proof-20261102.json` covers NT3.1/NT4 and two NT3.51
fixtures, including actual RTL8139 TCP/reset/cable checks and separate USB IP.
`usbnt-13-nt5-upgrade-closed-proof-20261102.json` covers W2K AHCI and XP x64 IDE:
six retained-PDO controller power pairs, three shared NIC/HID resets, removal
in D3 and original-port recovery, ten Windows key checks, and four ordinary
models with twelve function-power pairs/four stalled removals/four resets/four
carrier cycles. Each OS has 108 successful full-payload replies.

The fictional composite's accidental W2K port-2 attachment required new binding;
that failed topology and a removal-dialog input failure are retained separately.
This does not assert automatic installation at every new port.

All twelve distributed PE files and OEM payloads reproduce exactly from the
310-file independently extracted source archive, with clean build logs. See
`usbnt-13-v2-archived-source-rebuild-proof-20261102.json`. The corrected tree
label and icons are observed on NT3.51/W2K/XP. Existing frozen drivers and
historical evidence were preserved.

The version remains a validation candidate: unexplained historical latency,
unsupported controller sleep topologies, full platform ACPI S3 and untested
physical/OS combinations are not closed by these results.

## Historical exact-build validation (product 1.2)

`usbnt-controller-sleep-v3-five-os-closed-proof-20261102.json` closes all five
available guest fixtures after normal shutdown and exact SYS readback.
NT3.1/NT4 pass 104/332 full-payload replies. NT3.51 actual RTL8139 passes
3 MiB TCP each direction, three registered MAC resets and 68 PCI replies
through four USB interruptions. Windows 2000 AHCI and XP x64 IDE each retain
the same generation/PDO over six controller power pairs, process real device
removal during D3, restore the reinserted NIC and keyboard, and pass three
shared physical resets with ten Windows-observed key pairs. Their four-model
matrices add twelve function power pairs, four stalled removals, four resets
and four carrier cycles. Total replies are 108/112; XP additionally verifies
unsupported hub preflight refusal without disrupting traffic. All four HCDs
also pass the native hot-plug harness for the same frozen source snapshot.

These bounded results supersede pending exact-v3 checks below, not the
physical-hardware, platform-sleep or final release-media limitations.

## Verified development changes

- Synchronous transfer completion now drains its final callback access before
  returning, including after cancellation. The old source fails the early-return
  regression; the fix passes 200 delayed callback/event/timeout schedules and
  100 submission failures, plus 35 native QEMU resets across storage, audio/MIDI,
  video and card readers. The current snapshot is `usbnt-sync-rundown-v2-20261102`;
  exact installed-Windows regression for this snapshot is now closed in
  `usbnt-syncrun-four-os-closed-proof-20261102.json` and
  `nt351-syncrun-closed-proof-20261102.json`. NT3.1/NT4 pass 104/332 payload
  replies; W2K/XP x64 pass 12 function-power pairs, four stalled removals,
  four port resets, four carrier cycles and 64 replies each. NT3.51 RTL8139
  passes 3 MiB TCP each way, registered resets, cable recovery and 68 PCI
  replies during USB interruptions. The prior attachment-latency finding
  remains open; a passing run does not prove a deterministic timing fix.

- Native UVC reset passes 4,000 lifetime schedules, 2,400 restore/fault/retry
  schedules and 20 physical QEMU resets across four HCDs. Audio/MIDI reset
  passes 10,000 lifetime schedules, 4,050 production restore/failure cases,
  four UAC2 clock outcomes and 25 physical resets across five UAC/HCD fixtures.
  Camera/audio identity, successful control requests and stream settings survive;
  failed restoration remains gated and retryable. See
  `usbnt-uvc-reset-host-qemu-proof-20261102.json` and
  `usbnt-audio-reset-host-qemu-proof-20261102.json`. Installed Windows evidence
  for the audio-reset-v3 snapshot is closed for NT3.1, NT4, Windows 2000 and
  XP x64 in `usbnt-classreset-four-os-closed-proof-20261102.json`. NT3.51 passes
  RTL8139 TCP/reset/cable checks, but its two 30-second RTL8159 attachment
  failures remain open; later passing comparisons do not erase those failures.

- Native MSC reset and BOT/UAS lifetime tests now pass: 5,000 concurrent
  completion/submission schedules, 500 transport/fault fixtures, and 30 actual
  QEMU physical resets (BOT on four HCDs, UAS on xHCI/EHCI). Slots and written
  data survive; removal rejects disk access while the keyboard sibling works.
- Audio/MIDI rundown passes 5,000 host schedules; UVC callback/submit/reader
  rundown passes 3,000. Old production source fails the lifetime regressions.
  UAC1/UAC2 and camera streaming/removal tests pass on all four HCDs; EHCI UAC1
  uses a high-speed hub translator. Configuration preflight passes 26 cases;
  the old source mutates a foreign interface and fails the regression.
  See `usbnt-msc-media-host-qemu-proof-20261102.json`. These native-harness and
  host tests do not replace installed-Windows regression for the newer hashes.

- Generic interface v2 attachment tokens, callback rundown and synchronous
  transfer ownership are covered by the production `ntcli.c` host harness.
- Pipe clear-halt/toggle failures propagate to Windows clients.
- Actual port reset restores addressing, identity, configuration, alternate
  settings and endpoint contexts for Windows-owned functions. Host tests
  cover failed restoration and composite Windows-function coordination.
- The Windows 2000 QEMU USBNT path has actual RESET_PORT and subsequent
  network-recovery evidence. XP inbox-stack evidence is separate.
- Generic-client reset exclusion and failed-reset gating pass the production
  lifetime harness, including active transfers, claims, cached endpoints,
  callbacks and recovery on retry (2026-11-02).
- Function D0/D3 rundown passes 24 production-code host cases. New legacy,
  WDM x86 and WDM x64 drivers build successfully. Windows 2000 successfully
  completes three direct-PDO D3/D0 pairs issued at DISPATCH_LEVEL. An initial
  post-power network failure exposed RLTKLANT's lost pending pipe-reset
  recovery; that failure is preserved. After the RLTKLANT retry fix, the same
  RTL8153 test passes with four full-MTU replies. The closed four-model
  Windows 2000 matrix then passes 12 D3/D0 pairs, four RESET_PORT requests,
  four hot-plugs and four cable cycles, with 64 final full-MTU replies and
  exact stopped-guest SYS readback. See
  `win2000-usbnt-power-v2-complete-proof-20261102.json`.
- Source packaging now reads current files, verifies archive readback and
  records input hashes. The reproducibility/preservation check covers 277
  files; no versioned release archive was created by that check.

- NT3.51 with the latest NDIS3 and USBNT legacy candidate passes actual
  RTL8139 TCP before/after three MAC resets and cable reconnection (3 MiB
  each direction). Four USB generic attachment/carrier cycles preserve PCI
  traffic (68 full-MTU replies). This fixture has only a PCI network binding.
- The bare-metal native-class harness passes basic storage write/read and
  keyboard/mouse tests on QEMU UHCI, OHCI, EHCI and xHCI. Its NT-client
  notification hooks only log arrivals/removals; production generic-client
  tests and WDM tests remain separate.
- The latest legacy candidate passes actual USB network traffic and sibling
  preservation on NT3.1 (104 full-payload replies) and NT4 (332 replies with
  two USB adapters). Both guests shut down normally and their driver bytes
  were read back. The x64 candidate is now actually installed under XP x64:
  four models pass 12 function D3/D0 pairs, four port resets, four hot-plugs
  and four cable cycles, with 64 final full-payload replies.
- The strengthened native-class hot-plug harness passes on all four HCDs:
  retained storage data, removed-disk rejection, keyboard continuity and
  unaffected root-disk I/O after populated hub removal. EHCI uses the QEMU
  high-speed hub; the original incompatible full-speed topology is retained
  as a failed test setup.

- PnP planning now defers a new function with the same Windows instance ID
  until its old PDO is removed. Host identity/topology/composite tests cover
  1,000 delayed-removal generations. Spaced XP controller recovery passes
  six individually observed IRP pairs with both the old and new candidate;
  this is regression evidence, not deterministic proof that the historical
  first-cycle Code 31 is fixed.
- Function removal no longer frees outstanding work after five seconds.
  Deferred functions remain allocated until the USB worker observes final
  rundown. 1,000 production-code host schedules, including waits beyond the
  old timeout and out-of-order completion, pass ASan/UBSan. The exact WDM
  candidates now pass closed Windows 2000 and XP x64 four-model regressions:
  12 function power pairs, four removals with stalled RX, four port resets,
  four carrier cycles and 64 full-MTU replies per OS. Both guests shut down
  normally; stopped-disk SYS readback matches the frozen builds. See
  `win2000-retirement-closed-proof-20261102.json` and
  `xp64-retirement-closed-proof-20261102.json`.
- Detailed diagnostics identify rapid-transition failures as USB I/O loss
  while another power-down interrupts PHY/register initialization, rather
  than a PHY timeout. Later standalone network checks recover. A malformed
  CMD burst topology is retained as a failed test setup; it is not counted
  as the intended six-pair matrix.
- Native HID attachment now checks the per-device removal table before
  allocating a fifth composite interface. The old source fails the capacity
  regression. The fixed production source passes 1,000 composite/global-limit
  cycles, cancellation of active transfers on detach, and allocation/pipe-open
  failures under ASan/UBSan. All three driver builds and 58/106/95 actual
  supplied-guest import checks pass. Native-class hot-plug tests pass on QEMU
  UHCI/OHCI/EHCI/xHCI with retained disk data and keyboard continuity. Exact
  installed-Windows checks of this later candidate are now closed in
  `usbnt-hidcap-four-os-closed-proof-20261102.json`: NT3.1 four hot-plug/carrier
  cycles with 104 replies, NT4 eight cycles with 332 replies, and Windows 2000
  / XP x64 twelve function power pairs, four stalled removals, four resets,
  four carrier cycles and 64 replies each. All four shut down normally and
  installed SYS hashes match the frozen candidate. NT3.51 RTL8139 final
  revalidation is also closed in `nt351-hidcap-closed-proof-20261102.json`:
  3 MiB TCP each direction, three registered NDIS resets, cable recovery,
  four USB attachment/carrier cycles with 68 unaffected PCI replies, zero RX
  watchdog events, normal shutdown and exact installed-file readback.
  The reproducible sanitizer test is included in the source tree:
  `bash test/host/thidcapacity.sh` (from the USBNT directory).

- Native HID callback/submit rundown now passes 8,000 concurrent removal
  and reset schedules under ASan/UBSan. The old source fails the delayed
  completion lifetime assertion. Native HID restore passes 1,300 four-interface
  schedules covering every protocol/idle/partial-open failure, retry, removal,
  held-key release and sibling preservation. Windows shared-reset orchestration
  is separately host-tested. QEMU UHCI/OHCI/EHCI/xHCI each pass 20 real native
  HID port resets, held-key/button release, resumed input, unchanged device
  identity and storage sibling verification, followed by populated-hub removal.
  All three drivers build and 58/106/95 actual supplied-kernel imports pass.
  See `usbnt-hid-reset-host-qemu-proof-20261102.json`. Exact installed regression
  is now closed in `usbnt-hidreset-four-os-closed-proof-20261102.json` for NT3.1,
  NT4, Windows 2000 AHCI and XP x64 IDE. NT3.51 RTL8139 separately passes 3 MiB
  TCP each direction, three registered NDIS resets, cable recovery and 68 PCI
  replies during four USB attachment/carrier cycles, with zero RX watchdog
  events. `nt351-hidreset-closed-proof-20261102.json` includes closed-capture
  byte/checksum reassembly, normal shutdown and exact installed SYS readback.
  These remain development builds; the source package includes 285 files and
  the reproducible native HID reset/rundown tests.

Evidence is stored in the companion `../validation` directory. Read its
`STATUS.md` and dated manifests; historical README assertions are not a
substitute for checking the exact installed binary.

## Open release work

1. The actual Windows 2000 native-HID/Windows-network composite fixture is
   closed in `win2000-mixed-owner-closed-proof-20261102.json`: three shared
   physical resets, stalled removal/reinsertion, carrier recovery, 24 full
   payload replies and seven Windows-observed key down/up pairs pass. Normal
   shutdown and exact installed files are verified. The fixture is explicitly
   fictional; this does not prove every native-class combination or system
   sleep. Unknown native owners and hubs remain rejected before port reset.
2. Finish power validation. `UsbdiPower` now drains function queues on D3
   and restores admission on D0; initial guest deferred-IRP testing passes
   with the companion RLTKLANT recovery fix on four USB models. Actual
   USBNT x64 installation and the four-model function-power matrix now pass.
   Retained root-device controller sleep is implemented. Host checks cover
   84 power continuation cases, 63 complete transactions with 49 restore fault
   points, descriptor identity, preserved root PDOs and generic-client rollback.
   Polling and metadata lifetime review additionally covers 200 delayed poll/
   release schedules, 100 table replacements and 200 PnP snapshots. The older
   sources fail the targeted regressions. The first installed W2K candidate
   passes six identity-preserving power pairs and removal during D3 followed
   by reattachment: 32 full-payload replies and seven Windows key observations,
   with normal shutdown and exact file readback. See
   `win2000-mixed-sleep-v1-closed-proof-20261102.json`. The later polling/metadata
   fixes now have their own exact installed checks on Windows 2000 and XP x64
   in `usbnt-controller-sleep-v3-five-os-closed-proof-20261102.json`. Both retain
   child identity through power and pass physical mixed-owner reset.
   Hubs, unknown native owners, generic clients retaining state and active
   EHCI companions reject D3 before mutation. Full platform ACPI S3 and wider
   native-class/controller sleep combinations remain unverified.
   The XP x64 controller test exposed transient Realtek hardware-init
   failures during re-enumeration, despite final successful traffic. The
   collector now rejects those failures. The latest candidates each pass six
   separately observed controller IRP pairs with 24 full-MTU replies and no
   initialization failure. This verifies recovery with re-enumeration, not
   a deterministic fix for the historical first-cycle Code 31. See the two
   retirement closed proofs and preserved `xp64-hcd-release-closed-proof-20261102.json`.
3. Completed for this candidate: all three driver targets, tools, INFs and
   OEM payloads are versioned/rebuilt, archived-source reproduction passes,
   and exact installed-Windows upgrades are closed. Fresh OS text-mode
   installation from every OEM disk remains outside this evidence. The
   package uses current working source, not an unrelated or stale Git HEAD.
4. Complete the applicable guest regression matrix and record unsupported
   controller/speed paths explicitly. Current QEMU coverage does not certify
   physical PHY/MCU execution or SuperSpeed/Thunderbolt/USB4 hardware.

Do not distribute the draft as a completed USBNT 1.3 release.

## Archived-source rebuild check

The delivered checkpoint sources reproduce all three tested SYS files byte for
byte in a separate extracted tree. Existing tools also reproduce exactly. The historical build retained the usbtree.exe linker diagnostic (`.rsrc is too
large`); see `delivery-source-build-proof-20261102.json`. The later clean-build
and NT3.51 runtime proof above supersedes that open diagnostic without changing
the archived historical evidence.
