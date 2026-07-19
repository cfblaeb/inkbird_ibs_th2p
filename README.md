# Inkbird IBS-TH2 Plus Custom Firmware

Custom BLE firmware for the Inkbird IBS-TH2 Plus (PHY6222): reads
temperature/humidity from the main MCU's inter-chip UART stream and
broadcasts it as unencrypted BTHome v2, with per-device button-press events
for Home Assistant. Based on [pvvx/THB2](https://github.com/pvvx/THB2).

## Current Status

Current runtime: **V19** (hardware-validated). Highlights by version:

- **V15** — first complete stock-OTA migration path (baseline, in git history)
- **V16** — UART sleep-lock released on first good frame (battery fix)
- **V17** — physical button presses become BTHome button events (identify a
  unit in Home Assistant by pressing it)
- **V18** — 60 s fast-advertising window after any reset: pull the battery
  and the device is connectable from anything (incl. Linux/desktop Chrome)
  for a minute, at zero steady-state battery cost
- **V19** — UART framing hardened: CRC-16/MODBUS validation and proper
  resync (fixes a latent bug where payload bytes equal to `'R'` — e.g. the
  main-MCU battery byte at 82% — broke every frame)

See [IBSTH2P_PROJECT_PLAN.md](IBSTH2P_PROJECT_PLAN.md) for the full
engineering log: architecture, per-version notes, reverse-engineered
inter-chip UART protocol, reproducible-build toolchain recipe, and current
flashing paths.

## Flashing

**Update a device already on custom firmware (V18+):** pull the battery,
open `inkbird_fw/InkbirdOTA.html` in Chrome, load
`inkbird_fw/BOOT_IBSTH2P_v19_ota.bin`, Connect & Flash within ~60 s.

**Convert a stock device:** same page, load
`inkbird_fw/STAGE3_IBSTH2P_v19_stock_bundle_installer.hex16`, connect to
the stock `sps` device and follow the PPlusOTA prompt.

The page auto-detects which kind of device you connected to and refuses a
mismatched file. Interrupted transfers on the custom path are safe — the
image only activates after an on-device CRC32 check.

**Scripted/recovery paths:** `ble_phy_ota_flash.py` (BLE, scripted),
`flash_pogo.py` + `rdwr_phy62x2.py` (direct UART via pogo pins),
`bthome_phy6222/orig/orig.bin` (stock fullflash restore).
`ble_le_conn_ext.py` opens a BLE connection to pre-V18 firmware from Linux
(the kernel's ~4 s LE connect window cannot reach a 10 s advertiser).

## Monitoring

`python3 bthome_monitor.py` — curses TUI: one live row per BTHome device
(RSSI, measurements, time since last new data / last re-broadcast / last
button press).

## Building

The firmware source lives in `bthome_phy6222/` (pvvx THB2 tree; build with
`DEVICE=DEVICE_IBSTH2P BOOT_OTA=1`). Release builds use a pinned Debian
gcc-arm-none-eabi 14.2.rel1 toolchain — see the recipe in the project plan;
Ubuntu's packaged 13.2 cross-compiler produces different (unverified)
codegen. Host unit tests for the UART framing:
`cd bthome_phy6222/tests && gcc -o t test_ucap_frame.c && ./t`.
