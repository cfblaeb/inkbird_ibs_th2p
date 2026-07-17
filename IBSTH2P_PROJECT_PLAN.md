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

- `inkbird_fw/BOOT_IBSTH2P_v15.hex`
- `inkbird_fw/BOOT_IBSTH2P_v15_fullflash.bin`
- `inkbird_fw/BOOT_IBSTH2P_v15_ota.bin`
- `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16`
- `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_payload.bin`
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
4. The final low-flash runtime is the verified V15 image.

## Reproducible Build Paths

### Final low-flash runtime

```bash
make -B -C bthome_phy6222 \
  OBJ_DIR=build_boot_ibsth2p_stage3_clean \
  PROJECT_NAME=BOOT_IBSTH2P \
  PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P" \
  BOOT_OTA=1
```

Expected result: `bthome_phy6222/build_boot_ibsth2p_stage3_clean/BOOT_IBSTH2P.hex`
matches `inkbird_fw/BOOT_IBSTH2P_v15.hex` exactly.

### Mini installer and stock OTA bundle

```bash
make -C inkbird_fw/stock_bundle_installer
python3 inkbird_fw/make_stock_stage3_bundle.py
```

The bundle generator now defaults to the verified
`inkbird_fw/BOOT_IBSTH2P_v15.hex` payload. Use `--final-hex` only when testing a
fresh source build intentionally.

## Branch Layout

- `master`: clean V15 baseline plus the working stock-OTA mini-installer path.
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

## Next Work

1. Keep `master` reproducible against the verified V15 runtime.
2. Reintroduce useful code from `ibsth2p-current-experimental` in small,
   validated slices.
3. Verify Home Assistant behavior against the successful stock-installed build.
4. Measure battery behavior of the installed V15 runtime over time.
5. Clean up or archive no-longer-needed experimental artifacts once the working
   path is fully documented and preserved.

## Historical Note

The long stage1 diagnostic campaign was useful to localize stock OTA
compatibility constraints, SRAM layout issues, and installer handoff problems,
but it is no longer the active blocker. The stock OTA transport and mini
installer are now proven. The project has moved from “can this work?” to “keep
the working path stable while reintegrating only the changes we actually want.”