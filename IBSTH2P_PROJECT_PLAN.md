# Inkbird IBS-TH2 Plus — Project Plan

## Objective

Replace the stock BLE firmware on the Inkbird IBS-TH2 Plus PHY6222 with a custom
runtime that:

1. Reads temperature and humidity from the existing main-chip UART stream.
2. Advertises the data over BLE in BTHome format.
3. Can be installed onto a fully stock device through the stock BLE OTA path.

## Current State

The main migration milestone is complete.

### Proven on hardware

- Clean V15 direct UART flash works.
- Stock fullflash restore works.
- Stock BLE OTA of the mini-installer bundle works.
- The mini installer copies the verified V15 image into low flash.
- After reboot, the final custom firmware advertises correctly and exposes
  temperature/humidity over BLE.

### Latest successful end-to-end test

Date: 2026-05-25

Validated sequence:

1. Restored stock with `bthome_phy6222/orig/orig.bin` over UART.
2. Loaded `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16` into
   `inkbird_fw/InkbirdOTA.html`.
3. Flashed from stock `sps` -> `PPlusOTA` over BLE.
4. Device rebooted into custom firmware and appeared as `IBSTH2P-973B39` in
   nRF Connect with temperature and humidity present.

This is the first complete proof that the stock transport, SRAM installer, and
final low-flash image all work together on real hardware.

## Known-Good Artifacts

Only the current version is kept in the working tree; every older image
(V12–V18 experiments, fullflash captures, the V15 baseline set) remains
available in git history.

- `inkbird_fw/BOOT_IBSTH2P_v19.hex` — current hardware-validated runtime
- `inkbird_fw/BOOT_IBSTH2P_v19_ota.bin` — pvvx-path OTA image
- `inkbird_fw/STAGE3_IBSTH2P_v19_stock_bundle_installer.hex16` (+ payload) —
  stock-path bundle
- `inkbird_fw/ibs_thx_b_2p7_48M_phy6222.hex16` — stock updater image
  (input for the bundle generator; keep)
- `bthome_phy6222/orig/orig.bin` — stock fullflash backup (UART recovery)
- `inkbird_fw/InkbirdOTA.html`
- `flash_pogo.py`

## Proven Architecture

1. Stock firmware advertises as `sps` and can switch into `PPlusOTA`.
2. `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16` preserves the stock
   XIP updater partitions while staging a high-flash payload plus a small SRAM
   installer.
3. The SRAM installer validates the `IBI3` payload, erases the low-flash target
   sectors, writes the final records, verifies them in place, clears OTA mode,
   and resets.
4. The final low-flash runtime is the current verified image (V19).

## Reproducible Build Paths

### Final low-flash runtime

Build with the pinned toolchain — see "Reproducible builds on Ubuntu 24.04
(toolchain recipe)" below. Expected result: the produced
`BOOT_IBSTH2P.hex` is functionally identical to
`inkbird_fw/BOOT_IBSTH2P_v19.hex` (byte-identical except for linker
veneer ordering; verify with the veneer-aware comparison described in the
toolchain section).

### Mini installer and stock OTA bundle

```bash
make -C inkbird_fw/stock_bundle_installer
python3 inkbird_fw/make_stock_stage3_bundle.py
```

The bundle generator defaults to the current verified
`inkbird_fw/BOOT_IBSTH2P_v19.hex` payload (regenerating with defaults
reproduces the committed V19 bundle byte-for-byte). Use `--final-hex` only
when testing a fresh source build intentionally.

## Branch Layout

- `master`: current validated runtime (V19) plus all tooling. Older
  baselines (the V15 known-good set, experimental images) live in git
  history rather than the working tree.
- `ibsth2p-v15-good`: exact clean V15 baseline.
- `ibsth2p-current-experimental`: preserved pre-cleanup experimental work.

## V16 Battery Candidate (pending hardware validation)

V15 holds the PHY6222 fully awake for one whole advertising interval (10 s)
every measurement cycle while it waits for a UART frame from the main MCU
(`hal_pwrmgr_lock(MOD_UART0)` in `ucap_start_grab`, released only at the next
`read_sensors()`). That awake window is the dominant PHY-side battery cost:
roughly 10 s awake (~1-2 mA) out of every 300 s, i.e. tens of µA average,
versus ~2-3 µA for everything else combined.

V16 (`inkbird_fw/BOOT_IBSTH2P_v16.hex`) releases the UART power lock as soon
as the first complete valid frame is parsed, instead of holding it for the
full window. If no frame arrives, behavior is identical to V15 (the lock is
still released at `read_sensors()`), so the failure mode is the status quo.
Delta is +32 bytes of code, no extra RAM, three touched functions in
`cmd_parser.c` plus the version string (`IBS-V16`).

Toolchain provenance: with Debian gcc-arm-none-eabi 15:14.2.rel1-1,
binutils 2.42, newlib 4.5.0.20241231-1, the unmodified source reproduces
`inkbird_fw/BOOT_IBSTH2P_v15.hex` byte-for-byte; `BOOT_IBSTH2P_v16.hex` is
that same build plus only the early-unlock change.

Validation steps:

1. Direct UART flash the V16 build (or OTA it with
   `make_stock_stage3_bundle.py --final-hex inkbird_fw/BOOT_IBSTH2P_v16.hex`).
2. Confirm `IBS-V16` in the Software Revision characteristic.
3. Confirm temperature/humidity still update every 5 min over BTHome.
4. Compare sleep current vs V15 (expect the 10 s awake window to shrink to
   the main MCU's frame period).

Further battery ideas, in descending value (not implemented):

- Drop `rf_tx_power` from `RF_PHY_TX_POWER_MAX` to 0 dBm (runtime-settable
  via config command; small win, adv is only every 10 s).
- Test removing the `hal_pwrmgr_lock(MOD_USR0)` full-awake lock held during
  BLE connections (irrelevant for passive BTHome use, but a stuck/idle
  connection currently costs ~mA continuously).

## Second-chip findings (vendor OTA analysis)

The stock updater package `inkbird_fw/ibs_thx_b_2p7_48M_phy6222.hex16`
contains three Intel-hex segments, all in the PHY6222 address space: 36.5 KB
at flash 0x20000 (XIP window 0x11020000) and two SRAM pieces at 0x1FFF0000 /
0x1FFF1838. All of it is ARM Thumb code; there is no payload for any other
architecture and no second-stage protocol for reflashing another chip.
Strings inside ("history data", "recoder frame", "run/stop recoder",
"Real time data", "cfg data", DTM RF calibration banners) show the Inkbird
app-facing feature set (data logger, config, RF cal) lives in the PHY6222.

Conclusion: the vendor BLE upgrade only ever updates the PHY6222. The second
MCU (sensor + LCD controller, source of the 13-byte 'R'…'E' frames at
9600 baud) is not field-updatable and runs fixed firmware.

## V17 Button-to-Home-Assistant Candidate (pending hardware validation)

`inkbird_fw/BOOT_IBSTH2P_v17.hex` adds a per-device button event so dozens of
otherwise-identical units can be told apart: press a device's button and it
emits a BTHome button-press that Home Assistant surfaces as a device trigger,
tagged by that unit's MAC / `IBSTH2P-xxxxxx` name.

How it works (zero added battery cost):

- Frame byte [8] is the button-driven BLE state (see protocol section). It is
  a latched level that flips on each press. `ucap_process_frame()` compares it
  across grab windows and increments `ucap.btn_clicks` on each change. No new
  wakeups, no wake-on-UART — detection piggybacks on the existing 5-min grab
  windows, so a press can take up to ~5 min to appear in HA. This is fine for
  the label-my-devices use case (press each, they announce over a few minutes).
- `adv_measure()` (IBSTH2P block) watches `btn_clicks`. On a change it bumps
  the BTHome packet id once, sets `adv_button_press`, and pushes an
  advertisement whose payload now carries a button object (id 0x3a, value 0x01
  = press). It then returns early for `BTN_ADV_HOLD` (6) broadcasts (~60 s),
  so the controller repeats that exact frozen-packet-id payload undisturbed.
  HA de-duplicates the repeats by packet id into a single press event. The
  early return also stops a coincident measurement refresh from rebuilding the
  packet and causing a double trigger. After the hold, it reverts to the
  normal temperature/humidity packet.

Limitations / notes:

- Latency up to one measurement cycle (~5 min); by design (the zero-battery
  option). A responsive version would need wake-on-UART plus knowing the main
  MCU's transmit cadence.
- A connection starting mid-hold cancels the hold (GAPROLE_CONNECTED clears
  `adv_button_press`/`adv_button_hold`). Otherwise the post-disconnect
  advertisement rebuild would re-emit the button object under a fresh packet
  id and fire a phantom press in HA. Consequence: a press immediately
  followed by a connection may not reach HA — re-press after disconnecting.
- The premise that frame byte [8] still toggles when the main MCU talks to
  our silent firmware (which never sends the stock 'S'-frame replies) is
  unverified on hardware: all captures so far show 0x01. First validation
  step is simply confirming the icon still toggles and the event arrives.
- While a hold is active, `adv_measure()` returns early, so the measurement
  schedule (meas_count), battery check, and post-connection interval restore
  are each delayed by up to the hold (~60 s). Self-heals afterwards.
- Detection is on net level change, so an even number of presses between two
  grab windows cancels out. Single presses (the intended use) always register.
- Adds ~200 bytes of code; the button object grows the advert from 21 to 23
  bytes (well under the 31-byte legacy limit). No effect on other devices
  (verified: a TH04 build still compiles; all new code is under
  `#if DEVICE == DEVICE_IBSTH2P`).

Validation steps:

1. Flash V17 (direct UART, or OTA via
   `make_stock_stage3_bundle.py --final-hex inkbird_fw/BOOT_IBSTH2P_v17.hex`).
2. Confirm `IBS-V17` in the Software Revision characteristic.
3. In HA (BTHome integration), confirm each unit exposes a button entity/
   trigger; press a button and confirm exactly one press event fires for that
   device within a measurement cycle.
4. Confirm temperature/humidity still update normally between presses.

## Stock inter-chip UART protocol (reverse-engineered)

Decoded by disassembling the plaintext SRAM/XIP segments of
`inkbird_fw/ibs_thx_b_2p7_48M_phy6222.hex16` (stock UART0 ISR located via the
SDK jump table entry `V11_IRQ_HANDLER` at 0x1FFF3A15; frame parser at
0x1FFF2A48; app task ProcessEvent at 0x1FFF5F81).

Main MCU -> PHY frames: `'R' + 11 payload bytes + 'E'` at 9600 baud.
Payload (0-indexed relative to the 11-byte payload):

- `[6]` frame type. Type 1 = periodic measurement with temperature at
  `[2..3]` (LE s16, x0.01 C) and humidity at `[4..5]` (LE s16, x0.01 %).
  Types 0/2/3 are variants carrying temperature in `[0..1]`; a non-type-1
  frame triggers a small reply frame from the PHY.
- `[7]` **BLE enable state driven by the device button**: 1 = on, 0 = off.
  The button is wired to the main MCU only. On every valid frame the stock
  firmware fires OSAL event 0x100 and applies this byte to
  `GAPRole_SetParameter(GAPROLE_ADVERT_ENABLED /* 0x305 */, 1, &flag)`;
  when it transitions to 0 with a live connection, the connection is
  terminated first. There is no dedicated button GPIO to the PHY — the
  state rides in-band in the measurement stream.
- `[8]` battery percent computed by the main MCU (observed 0x50-0x55 =
  80-85%). The stock firmware copies it into the BLE scan-response data it
  refreshes on a 5-second timer.
- `[9..10]` CRC-16/MODBUS (reflected poly 0xA001, init 0xFFFF) over payload
  bytes `[0..8]`, stored little-endian.

PHY -> main MCU frames: `'S' + 9 payload bytes + 'E'`, same CRC over the
first 7 payload bytes at `[7..8]`. Before transmitting, the stock firmware
raises a GPIO (pin id 4) for ~300 us as a wake signal to the main MCU, and
drops it ~200 us after the frame.

Other stock details recovered along the way: the stock app advertises as
`sps` (an alternate `tps` name exists for a second mode/model), refreshes
scan-response data with live sensor values every 5 s (that is how the
Inkbird app reads without connecting), and stores history in an external
I2C EEPROM bit-banged on pins 17/19 with pin 16 as write-protect.

Implications for the custom firmware (not yet implemented):

1. Watching payload `[7]` in `ucap_process_frame()` would make the device
   button work: advertising on/off in sync with the LCD icon the main MCU
   already draws.
2. Payload `[8]` provides the main MCU's own battery estimate as a
   cross-check for the PHY's ADC measurement.
3. Frames can be validated with the now-known CRC instead of only the
   start/end markers.

## V18: Fast-connect window after any reset (hardware-validated 2026-07-19)

Problem: the Linux kernel arms its LE create-connection window for only ~4 s,
and only after seeing an advertisement. With the IBSTH2P's 10 s advertising
interval the next advertisement always falls outside that window, so BLE
connections from Linux hosts (bluez/bleak/desktop Chrome) fail
deterministically — confirmed with a btmon capture (single
`LE Extended Create Connection` cancelled 4 s later, CONNECT_IND never sent).
Workarounds before V18: raw-HCI extended create connection with a long budget
(`ble_le_conn_ext.py`, needs sudo), or Android (30 s connect timeout).

V18 reuses the existing pvvx fast-advertising mechanism (the same one that
runs for 60 s after an OTA-mode boot) on *any* power-on/reset: 60 s at
`DEF_CON_ADV_INTERVAL` (~1.56 s), after which `adv_measure()`'s existing
`adv_reload_count` expiry path restores the configured 10 s interval.
Steady-state battery cost: zero (only triggers on reset).

Flash-from-Chrome workflow this enables: pull the battery, reinsert, and
connect within ~60 s — no sudo tooling required.

Validated on hardware: `IBS-V18` read via a plain no-sudo bleak connect
during the post-reboot window; steady state returns to 10 s advertising.

## V19: UART framing fix — CRC validation + resync (hardware-validated 2026-07-20)

Two defects in the V10..V18 frame parser (`cmd_parser.c`):

1. It restarted frame collection on *any* 0x52 ('R') byte, including inside
   a frame. Payload bytes can legally be 0x52 — notably main-MCU battery
   byte [9] at 82% — in which case *every* frame breaks for as long as the
   value persists: stale temperature/humidity keep advertising, button
   detection goes blind, and the V16 early sleep-release never fires (10 s
   awake per grab window again). The device battery was at 80–83% when this
   was found.
2. The stock protocol's CRC-16/MODBUS (bytes [10..11] over [1..9]) was
   never checked, so corrupted UART bytes were accepted into temperature/
   humidity/button state.

V19 moves framing into `bthome_phy6222/source/ucap_frame.h` (SDK-independent
header): frames are accepted only with a valid end marker *and* CRC; on
validation failure the buffer resyncs by sliding to the next 'R' inside it
(mid-frame 'R' bytes never restart collection). `crc_bad` and `noise`
counters replace the old `bad_bytes` in the debug fallback and the stats
dump command. Note: the ROM symbol table provides `memcpy` but not
`memmove`; the resync shift is a manual forward loop.

Host unit tests: `bthome_phy6222/tests/test_ucap_frame.c`
(`gcc -o t test_ucap_frame.c && ./t`) — 11 checks including the batt=82%
regression, corrupted-button-byte rejection, truncation resync, and a
pseudo-random-noise false-frame sweep.

Validated on hardware: flashed via InkbirdOTA.html in desktop Chrome using
the V18 battery-pull window (first end-to-end proof of that path);
`IBS-V19` confirmed and live sensor frames parsed (temp/humidity plausible,
battery reading in the 82% danger zone at the time).

## Reproducible builds on Ubuntu 24.04 (toolchain recipe)

The shipped V15+ hexes were built with Debian gcc-arm-none-eabi
15:14.2.rel1-1 / binutils 2.42 / newlib 4.5.0.20241231-1. Ubuntu 24.04
packages gcc 13.2 for arm-none-eabi (different codegen — do not use for
release builds). Working recipe without root:

1. Download from snapshot.debian.org and extract with `dpkg -x` into a
   scratch dir (`$TC/root`): `gcc-arm-none-eabi_14.2.rel1-1_amd64.deb`,
   `libnewlib-arm-none-eabi_4.5.0.20241231-1_all.deb`,
   `libnewlib-dev_4.5.0.20241231-1_all.deb`. Ubuntu's system
   `binutils-arm-none-eabi` 2.42-1ubuntu1+23 provides as/ld (matches the
   2.42 the original build used).
2. Create `$TC/xbin` with `as`/`ld` symlinks to `/usr/bin/arm-none-eabi-{as,ld}`.
3. Build:

```bash
PATH=$TC/root/usr/bin:$PATH make -B -C bthome_phy6222 \
  OBJ_DIR=build_boot_ibsth2p_vXX PROJECT_NAME=BOOT_IBSTH2P \
  PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P" BOOT_OTA=1 \
  CROSS_COMPILE="arm-none-eabi-" \
  CC="arm-none-eabi-gcc -B$TC/xbin -B$TC/root/usr/lib/arm-none-eabi/newlib \
      -isystem $TC/root/usr/include/newlib"
```

Output is functionally identical to the shipped hexes but not raw
byte-identical: GNU ld emits its long-branch veneers in a different order,
which shifts BL immediates. Equivalence is machine-checkable — every BL must
resolve through the veneers to the same final target and the veneer target
multisets must match (verified for V15/V16/V17 rebuilds vs the shipped,
hardware-validated hexes).

## Flashing paths (current)

- **Custom firmware (V18+), from anywhere incl. desktop Chrome**: pull the
  battery, load `inkbird_fw/BOOT_IBSTH2P_vXX_ota.bin` in
  `inkbird_fw/InkbirdOTA.html`, Connect & Flash within ~60 s. The page
  auto-detects stock SHB vs custom BTHome devices by GATT services and
  validates the file kind (.hex/.hex16 = stock path, PHY6 `_ota.bin` = pvvx
  path). Interrupted pvvx transfers are safe: the device only writes the
  PHY6 magic after the whole staged image passes CRC32.
- **Custom firmware, scripted from Linux**: `ble_phy_ota_flash.py <ota.bin>`
  (waits for an ACL link); pre-V18 firmwares additionally need
  `sudo ble_le_conn_ext.py` to open the link (kernel 4 s limit).
- **Stock firmware**: unchanged stage3 stock-bundle path
  (`STAGE3_*_stock_bundle_installer.hex16` via InkbirdOTA.html).
- OTA bins are produced by
  `python3 phy62x2_ota.py -w 0x2F00 -f ota_upboot.add <hex>` (in
  `bthome_phy6222/`) — verified to reproduce the hardware-proven
  `BOOT_IBSTH2P_v15_ota.bin` byte-for-byte.

## V20: BTHome firmware version + sleep-lock leak fix (pending hardware validation)

### Fix: sleep lock leaked after supervision-timeout disconnects

Present in V15..V19: `peripheralStateNotificationCB` released the
`MOD_USR0` full-awake lock only in the `GAPROLE_WAITING` case (clean
disconnect). A link that died by supervision timeout — client walked out
of range, phone died, script killed without disconnecting — reports
`GAPROLE_WAITING_AFTER_TIMEOUT` (`thb2_peripheral.c`, reason
`LL_SUPERVISION_TIMEOUT_TERM`), and that case only logged. The SDK pwrmgr
lock is a flag, not a refcount, so nothing else cleared it: the device
resumed advertising and looked healthy while never sleeping again
(~1-2 mA, 2xAAA dead in weeks) until the next *clean* connect/disconnect
cycle or a battery pull. Trigger is the most natural phone workflow there
is (connect with nRF Connect, walk away), so this may explain any
disappointing battery observations on units that were connected to.

V20 folds `GAPROLE_WAITING_AFTER_TIMEOUT` into the `GAPROLE_WAITING`
cleanup (unlock, advert rebuild, latency/counter reset, `wrk.reboot`
honored). The change is device-generic: on non-IBSTH2P builds the timeout
path previously skipped all disconnect cleanup too (there it had no sleep
lock to leak, but also never rebuilt the advert or honored the OTA reboot
flag).

Validation: connect (e.g. nRF Connect), then kill the link ungracefully —
walk out of range or toggle the phone's Bluetooth off — wait ~4 s
(supervision timeout), confirm advertising resumes, and check sleep
current is back at the idle baseline rather than 1-2 mA.

### Feature: BTHome firmware version in the advertisement

The BTHome payload now ends with the firmware version object (id 0xF2,
uint24, little-endian patch/minor/major), so the firmware level can be
read passively by any scanner implementing the full BTHome v2 spec,
without connecting to read the Software Revision characteristic.

Home Assistant caveat (verified against bthome-ble 3.23.5, the parser HA
uses, by feeding it the exact V20 payload): bthome-ble does not implement
the device-information objects (0xF0/0xF1/0xF2) yet. It logs a debug-level
"Invalid Object ID" and stops at that object — harmless here because 0xF2
is last, and the resulting entities are byte-for-byte identical to a V19
packet (battery/temperature/humidity/voltage/packet id all intact). HA
keeps showing "BTHome BLE v2" as the device firmware until bthome-ble
learns 0xF2, at which point already-deployed V20 devices surface their
version with no reflash. `bthome_monitor.py` shows it today.

If HA visibility is wanted *now*, the working alternative (verified with
the same parser) is the BTHome text object 0x53 carrying e.g. "IBS-V20"
(9 bytes vs 4), which HA exposes as an extra "Text" sensor entity rather
than the device firmware field. Not implemented — entity clutter vs a
debug-only object was judged the worse trade; revisit on request.

Implementation notes:

- The version number comes from a single macro, `IBS_FW_VERSION` in
  `config.h` (currently 20). It generates both the `IBS-VNN` Software
  Revision string and the advertised BTHome version `NN.0.0` — bump one
  number per release, nothing else to keep in sync.
- Object id 0xF2 is the highest sent, preserving BTHome's ascending object
  order. Advert grows 21 → 25 bytes (limit 31).
- The version object and the V17 button object are mutually exclusive:
  during a button burst the packet carries the button instead of the
  version (23 bytes), and the version returns on the first post-burst
  packet. This bounds the worst case of a hypothetical encrypted build
  (8 bytes counter+MIC overhead) at exactly 31 bytes.
- `bthome_monitor.py` decodes 0xF2 and shows it in a new FW column
  (latched across button bursts).
- This takes the V20 number; the button-responsiveness candidates below
  become V21 when implemented.

Built artifacts (committed, pending hardware validation):

- `inkbird_fw/BOOT_IBSTH2P_v20.hex` — direct UART flash
- `inkbird_fw/BOOT_IBSTH2P_v20_ota.bin` — pvvx-path OTA (InkbirdOTA.html on
  a device already running custom firmware, or `ble_phy_ota_flash.py`)
- `inkbird_fw/STAGE3_IBSTH2P_v20_stock_bundle_installer.hex16` (+ payload
  bin) — stock-path OTA for devices still on Inkbird firmware

Build provenance: pinned toolchain per the recipe below (Debian
gcc-arm-none-eabi 15:14.2.rel1-1 + Ubuntu 24.04 binutils 2.42-1ubuntu1+23
+ newlib 4.5.0.20241231-1). The environment was validated before building
V20: the same setup rebuilt master (V19 source) into a hex byte-identical
to the shipped hardware-validated `BOOT_IBSTH2P_v19.hex` (after CRLF
normalization — this toolchain reproduces even the veneer order), and both
packagers regenerated the committed v19 artifacts byte-for-byte
(`phy62x2_ota.py` → `BOOT_IBSTH2P_v19_ota.bin`;
`make_stock_stage3_bundle.py` defaults → the committed v19 STAGE3 bundle
and payload). The v19 artifact set stays in the tree as the validated
fallback until V20 passes hardware validation; `make_stock_stage3_bundle.py`
keeps defaulting to the v19 hex for the same reason.

Validation steps:

1. Flash the build: `BOOT_IBSTH2P_v20_ota.bin` via InkbirdOTA.html for
   devices on custom firmware (battery-pull fast window),
   `STAGE3_IBSTH2P_v20_stock_bundle_installer.hex16` for stock devices,
   or direct UART with the v20 hex.
2. Confirm `IBS-V20` in the Software Revision characteristic.
3. Confirm `bthome_monitor.py` shows FW `20.0.0` and that
   temperature/humidity/battery decode unchanged.
4. In HA, confirm the sensor entities are unchanged (bthome-ble ignores
   0xF2 for now, see caveat above) and a button press still fires exactly
   one event (version object absent during the burst must not confuse
   the integration).

Validation status (2026-07-21, unit 38:1F:8D:97:3B:39):

- Step 1 (flash): **done** — release `BOOT_IBSTH2P_v20_ota.bin` flashed
  over the pvvx OTA path (bleak, fast-adv window; 3173 blocks ~56 s,
  on-device CRC32 verified before boot-magic write).
- Step 2 (revision): **done** — post-reboot GATT read returned `IBS-V20`.
- Step 3 (0xF2 advert): **done** — passive scan of the release image
  captured `...f2 00 00 14` (version 20.0.0) at the end of the payload,
  with packet id/battery/temperature/humidity/voltage decoding unchanged
  (first verified on the X20 probe image, then re-verified post-flash).
- Step 4 (HA entities + button event): **pending**.
- Sleep-lock leak fix (cb0575e): **FAILED — V20 introduces a worse bug
  on this same path.** Reproduced cleanly (connect with keep-alive
  reads, `sudo hciconfig hci0 reset` to kill the link without a
  disconnect packet, hands off the device):
  1. After the supervision timeout the device advertises at ~1.6 s
     (fast-reconnect window, BTHome payload intact and updating).
  2. ~45-60 s later, when that window expires, the advertising interval
     falls to **~20 ms** (the BLE minimum; ~180-220 reports/s observed)
     instead of returning to 10 s — consistent with an interval
     variable reading zero and being clamped to the floor. Measured
     draw in this state: **7-8 mA** (vs ~1-2 mA for the V19 leak it
     replaced; 2xAAA dead in days).
  3. The state **survives connect + clean disconnect** (still ~177
     reports/s after) — only a battery pull clears it. Clean
     disconnects from the normal state are unaffected (normal BTHome
     advertising resumes, verified).
  Also noted: post-timeout packets showed BTHome packet id stuck at 1
  across measurement updates. **Do not deploy V20 to the main unit** —
  V19's 1-2 mA leak is the lesser evil. Fix candidate: find where the
  timeout-path advert rebuild/window-expiry switch loses the normal
  advertising interval (suspect the interval variable is never
  (re)initialized on the `GAPROLE_WAITING_AFTER_TIMEOUT` → fast-window
  → expiry sequence added/folded in cb0575e).

## V21: advertising-storm fix + wake-on-RX (2026-07-22, flashed to bench unit)

### Fix: V20 advertising storm after supervision timeout

Root cause (code-traced, then confirmed by the fix's behavior on hardware):
`set_new_adv_interval()` in thb2_main.c changes the advertising interval by
stopping advertising and **faking `gapRole_state = GAPROLE_WAITING_AFTER_
TIMEOUT`** — the state the SDK's `GAP_END_DISCOVERABLE_DONE_EVENT` handler
special-cases to auto-restart advertising (thb2_peripheral.c). That handler
also notifies the app, so the app callback receives a fake "supervision
timeout" on every interval change. Harmless through V19 (the case only
logged); V20's folded disconnect cleanup ran on it and re-armed
`adv_reload_count = 1`, whose expiry calls `set_new_adv_interval()` again —
a teardown/restart loop at radio speed (~200 pkt/s, 7-8 mA, frozen packet
id, survives clean disconnects).

V21 fix: an `adv_restart_pending` counter set by `set_new_adv_interval()`
and consumed by the state callback — the fake-state bounce skips the
disconnect cleanup, a real supervision timeout still gets it (including the
V20 MOD_USR0 unlock). A connection clears the counter (belt-and-braces).

### Feature: wake-on-RX (frame-synced UART listen windows)

Replaces the 5-minute grab cycle. The scheduler (`source/ucap_sync.h`,
pure C, host-tested in `tests/test_ucap_sync.c` — 24 checks incl. a
2000-frame drift simulation: 100% catch rate, 0.78% listen duty) predicts
each ~10.4 s main-MCU frame from the last one and opens a short adaptive
window around it: guard starts at ±250 ms, narrows to ±60 ms only after
8 consecutive catches, widens on any miss, escalates to full-period
reacquire after 2 misses and to one attempt per 5 min after 6 (a dead main
MCU cannot pin the receiver on). Per the one-device caveat, nothing is
hard-assumed about the period: the estimate trains per unit (EMA, bounds
8-13 s) and button-inserted off-schedule frames anchor phase but never
train it. Device glue in cmd_parser.c (three OSAL events, reusing the
V16-validated UART lock/early-release machinery); `cfg.measure_interval`
pinned to 1 so the advertised payload refreshes from the latest frame on
every 10 s advertising event (packet id now advances every ~10 s).
Estimated cost: ~10-20 µA average vs ~26-60 µA for the old grabs —
years of battery, with 10 s sensor freshness and ≤~20 s button latency.

### Build provenance and artifacts

Toolchain re-pinned from Debian (gcc-arm-none-eabi 15:14.2.rel1-1 +
newlib 4.5.0.20241231-1, deb.debian.org; Ubuntu binutils 2.42-1ubuntu1+23)
and re-validated: rebuilt V19 and V20 sources reproduce the committed
hexes **byte-for-byte**. Pitfall for future rebuilds: pass exactly
`CC="<debtc>/root/usr/bin/arm-none-eabi-gcc -B<debtc>/xbin
-B<debtc>/root/usr/lib/arm-none-eabi/newlib -isystem
<debtc>/root/usr/include/newlib"` — adding an extra `-B` for the gcc
libdir changes the linker's library search order and reorders veneers
(same map, different BL encodings).

Artifacts: `inkbird_fw/BOOT_IBSTH2P_v21.hex` / `..._ota.bin`.
Stock-path STAGE3 bundle not yet built.

### Validation status

- Flashed to bench unit 38:1F:8D:97:3B:39 over the pvvx OTA path
  2026-07-22 (from the V20 storm state — instantly connectable);
  post-reboot revision reads `IBS-V21`, on-air 0xF2 = 21.0.0.
- Wake-on-RX: **working on air** — packet id advances +1 per 10 s
  advertisement with live humidity/battery movement (V20: one change per
  5 min).
- Storm fix, interval-restore path: **validated** — after a clean
  disconnect + 60 s fast-window expiry the cadence settles at 10.0 s
  (this exact transition stormed on V20).
- Overnight soak (2026-07-22, 7 hourly samples): steady 10 s cadence,
  packet id advancing on schedule across ~49 RTC wraps — no storm, no
  silence.
- Meter (2026-07-22): idle **10-15 µA** with a few-mA blip every ~10 s
  (listen window + advertisement) — the device genuinely sleeps between
  windows; no stuck UART lock.
- Supervision-timeout repro (2026-07-22, `sudo hciconfig hci0 reset`
  while connected with keep-alives, hands off; run twice, second run
  meter-observed): advertising settled straight back to clean 10.0 s
  cadence, and the meter showed ~3.5 mA briefly (timeout ride-out) then
  ~0 with 3-5 mA blips every 10 s — **storm fix and sleep-lock release
  both validated on the exact transition that melted V20** (the V15-V19
  leak would park at 1-2 mA, the V20 storm at 7-8 mA).
- Button press: event on air (`3a 01`, version object absent during the
  burst as designed) ~27 s after the operator's wall-clock press — within
  the ≤ ~20 s design chain plus unsynchronized-clock slack.
- Still open: HA entity check (validation step 4 of V20 carried over),
  stock-path STAGE3 bundle build, commit of the V21 tree, main-unit
  (38:1F:8D:CF:77:6F) upgrade decision.

## Frame-period probe build (branch `v21-frame-probe`, experimental)

Purpose: run the prerequisite experiment for the V21 button-responsiveness
decision (see next section) — measure (a) the main MCU's real inter-frame
period and (b) whether a button press emits an immediate extra frame.

This branch (based on V20) hardcodes `UCAP_PROBE` in `config.h`:

- UART RX is permanently powered and locked (never sleeps, ~1-2 mA —
  bench use only, do not deploy). Sensor/button/BTHome behavior is
  otherwise V20.
- Every valid frame is timestamped (24-bit RTC @32768 Hz, wraps 512 s)
  into a 64-entry ring with frame bytes [7] (type) and [8] (button
  level).
- Debug command op 4 (`CMD_ID_I2C_SCAN`, char 0xFFF4) dumps the ring;
  while notifications are enabled on 0xFFF4, each frame is also streamed
  live (marker 0x51 with delta-ms precomputed).
- Software Revision reads `IBS-X20` ('X' = experimental probe).

Artifacts: `inkbird_fw/BOOT_IBSTH2P_x20_frameprobe.hex` /
`..._ota.bin` (pvvx OTA path), built with the validated pinned toolchain.

How to run the experiment:

1. Flash `BOOT_IBSTH2P_x20_frameprobe_ota.bin` on one bench unit
   (battery-pull + InkbirdOTA.html as usual). Confirm `IBS-X20`.
2. `python3 frame_probe_monitor.py <MAC>` — live mode. Let it run a few
   minutes to see the steady frame cadence, then press the device button
   several times: if a frame with a flipped btn byte arrives within ~a
   second of the press, the main MCU pushes presses immediately and
   wake-on-RX (option 2) is effectively lossless; if the flip only shows
   on the next periodic frame, the frame period is the latency floor.
3. `python3 frame_probe_monitor.py <MAC> --dump` — fetches the ring and
   prints min/median/max frame period (answers (a), fixing option 1's
   real battery cost and loss window).
4. Record both answers in this plan, pick option 1 or 2 for V21, then
   reflash the unit with the release V20 image.

### Results (2026-07-21, unit 38:1F:8D:97:3B:39, 14 min live capture)

- **(a) Steady frame period: ~10.38 s** — min 10.34 / median 10.38 /
  max 10.41 s over 79 clean intervals. The slow 10.35→10.41 s drift over
  the run (and the non-integer value) marks this as the main MCU's own
  free-running clock, not the BLE side's 10 s connected-grab timer
  (probe UART is always-on, so grab windows play no role). Boot behavior
  (from `--dump` right after battery pull): 2 frames ~2 s apart, then
  the steady cadence.
- **(b) Button press emits an immediate extra frame: YES, 3/3.** The
  btn byte (`[8]`) toggles per press (0x01→0x00→0x01→0x00). The press
  frame is *inserted* — the next periodic frame still arrives on the
  original schedule (press-dt + following-dt = 10.38-10.39 s in all
  three cases).
- Note: `--dump` cannot measure (a) in practice — the ring resets on
  boot and connecting requires a battery pull, so it only ever shows
  the boot burst. Use live/guided mode (stays connected) instead.
  `--guided` now walks the whole experiment interactively.

Consequences for the V21 choice: option 2 (wake-on-RX) is viable — an
immediate press frame always exists to wake on; the waking frame itself
is lost mid-byte, so press latency ≈ remainder of one frame period,
worst case ~10.4 s. Option 1's loss window per grab gap is now exact:
a 60 s `measure_interval` window misses a press pair only if both land
in the same ~50 s gap; each grab costs ~one 10.4 s frame period awake.

## Button responsiveness options (V21 candidates, 2026-07-21)

Background: the stock inter-chip frame carries no press counter — payload is
fully decoded (see protocol section) and the main MCU is not field-updatable,
so the latched level in payload `[7]` is all we get. A press is only detected
when a grab window observes a level change, so today: up to ~5 min latency,
and an even number of presses between two windows cancels out. The main MCU
streams frames continuously (measured 2026-07-21: ~10.38 s period, plus an
immediate extra frame per button press — see probe results above), so the
loss window equals our observation gap — nothing inherent about 5 min.

Note: `cfg.measure_interval`/`cfg.advertising_interval` are hard-pinned for
IBSTH2P in `test_config()` (`config.c:167-168`), so every option below needs a
new build; none are runtime-configurable today. V18's 60 s post-reset fast
window is advertising-interval only — it does not extend UART listening
(boot grab establishes the button baseline without firing, one extra grab
lands ~45-50 s after boot while adv events are fast, then back to 5 min).

Options, in descending preference:

1. **Shrink the pinned `measure_interval`** (one line, `30` → e.g. `6` for
   60 s windows). Latency ≤ 60 s; double press lost only if both land in the
   same gap. With the V19 early release each window costs ~one frame period
   awake, so ~5x today's grab duty — order of 50 uA average, still years on
   2xAAA. Bonus: temp/humidity update every minute.
2. **Wake-on-RX-line**: GPIO wake on P10 (UART RX idles high, toggles on any
   traffic); the waking frame is lost mid-byte, but the next frame arrives
   one period later. Near-frame-period press latency at roughly today's idle
   cost (~20 ms awake per frame period). More code and sleep/re-init race
   risk than option 1.
3. **Always-on UART RX**: catches everything, but blocks sleep at 1-2 mA
   continuous — battery dead in weeks. Rejected.

Prerequisite experiment (cheap, do first): a debug build that holds the UART
open for ~60 s and logs (a) the actual main-MCU frame period and (b) whether
a button press emits an immediate extra frame (plausible — stock BLE toggle
reacts promptly — but unverified). (a) fixes option 1's real cost and loss
window; (b) decides whether option 2 is effectively lossless.

Existing workaround, no new build: while a BLE connection is open, grabs run
every 10 s (`GAPROLE_CONNECTED` path, `thb2_main.c:1106-1108`) — connecting
with e.g. nRF Connect is an ad-hoc "live button mode", though presses during
the connection collapse into a single press event fired after disconnect.

## Next Work

1. Measure battery behavior of V16+ (early UART sleep-release) against the
   V15 baseline over time.
2. Consider a stuck-connection watchdog (a *live but idle* connection still
   costs ~mA due to the `MOD_USR0` full-awake lock; the related
   supervision-timeout lock leak was fixed in V20) and a lower default TX
   power. Also test whether the `MOD_USR0` lock is needed at all —
   upstream pvvx sleeps through connections.
3. Verify Home Assistant behavior (BTHome sensors + V17 button trigger)
   against the current build.
4. Reintroduce useful code from `ibsth2p-current-experimental` in small,
   validated slices.
5. Button responsiveness (see "Button responsiveness options" above): run the
   frame-period experiment, then pick option 1 (60 s windows) or 2
   (wake-on-RX) for V20.

## Historical Note

The long stage1 diagnostic campaign was useful to localize stock OTA
compatibility constraints, SRAM layout issues, and installer handoff problems,
but it is no longer the active blocker. The stock OTA transport and mini
installer are now proven. The project has moved from “can this work?” to “keep
the working path stable while reintegrating only the changes we actually want.”