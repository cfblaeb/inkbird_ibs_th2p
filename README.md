# Inkbird IBS-TH2 Plus Custom Firmware

This repository is currently centered on the Inkbird IBS-TH2 Plus PHY6222 BLE
firmware and the migration path from stock firmware to the custom V15 runtime.

## Current Status

As of 2026-05-25, the end-to-end migration path is working.

- Direct UART flash of the clean V15 runtime succeeds and BLE comes up.
- Restoring stock from `bthome_phy6222/orig/orig.bin` succeeds.
- Stock BLE OTA of `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16`
  through `inkbird_fw/InkbirdOTA.html` succeeds.
- After OTA install, the device reboots into the custom firmware and advertises
  as `IBSTH2P-973B39`, with temperature and humidity visible in nRF Connect.

The practical migration architecture is now proven:

1. Stock Inkbird BLE OTA transports the bundle.
2. A tiny SRAM installer runs from staged SRAM.
3. The installer copies the verified V15 image into low flash and resets.
4. The final custom runtime boots normally and advertises over BLE.

## Key Files

- `inkbird_fw/BOOT_IBSTH2P_v15.hex`: hardware-validated final low-flash image
  layout.
- `inkbird_fw/BOOT_IBSTH2P_v15_fullflash.bin`: UART-flashable full image for
  direct recovery and validation.
- `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16`: stock OTA bundle
  containing the mini installer and staged final payload.
- `inkbird_fw/InkbirdOTA.html`: Web Bluetooth page for flashing the stock OTA
  bundle onto stock firmware.
- `flash_pogo.py`: UART flashing utility used for fullflash restore and direct
  runtime flashing.
- `bthome_phy6222/orig/orig.bin`: preserved stock fullflash dump.

## Proven Workflows

### Restore stock fullflash

```bash
sudo python3 flash_pogo.py -p /dev/ttyUSB0 -e -r wf 0x0 bthome_phy6222/orig/orig.bin
```

### Direct-flash the verified V15 runtime

```bash
sudo python3 flash_pogo.py -p /dev/ttyUSB0 -e -r wf 0x0 inkbird_fw/BOOT_IBSTH2P_v15_fullflash.bin
```

### Rebuild the clean V15 runtime from sources

Use an isolated object directory. Do not rely on the shared
`bthome_phy6222/build` tree.

```bash
make -B -C bthome_phy6222 \
  OBJ_DIR=build_boot_ibsth2p_stage3_clean \
  PROJECT_NAME=BOOT_IBSTH2P \
  PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P" \
  BOOT_OTA=1
```

The resulting `bthome_phy6222/build_boot_ibsth2p_stage3_clean/BOOT_IBSTH2P.hex`
matches `inkbird_fw/BOOT_IBSTH2P_v15.hex` byte-for-byte.

### Build the BLE OTA mini-installer bundle

```bash
make -C inkbird_fw/stock_bundle_installer
python3 inkbird_fw/make_stock_stage3_bundle.py
```

By default, `inkbird_fw/make_stock_stage3_bundle.py` now uses the
hardware-validated `inkbird_fw/BOOT_IBSTH2P_v15.hex` as the final payload.

If you want to test a fresh source build instead, override it explicitly:

```bash
python3 inkbird_fw/make_stock_stage3_bundle.py \
  --final-hex bthome_phy6222/build_boot_ibsth2p_stage3_clean/BOOT_IBSTH2P.hex
```

### Flash the stock OTA bundle over BLE

1. Restore stock first if needed.
2. Open `inkbird_fw/InkbirdOTA.html` in a browser that supports Web Bluetooth.
3. Load `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16`.
4. Connect to `sps`, allow the mode switch, then reconnect to `PPlusOTA`.
5. Wait for all partitions to complete and the device to reboot.

On the successful 2026-05-25 test, the stock OTA bundle loaded 9 partitions,
the device rebooted, and the final custom runtime came up over BLE with live
temperature and humidity.

## Current Repo Intent

The repository now has a proven baseline:

- `master`: clean V15 plus the working stock-OTA mini-installer path.
- `ibsth2p-v15-good`: exact clean V15 baseline.
- `ibsth2p-current-experimental`: preserved historical experimental branch.

Future changes should be brought back onto `master` incrementally and validated
against the verified V15 runtime and the working stock OTA installer path.