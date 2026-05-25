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