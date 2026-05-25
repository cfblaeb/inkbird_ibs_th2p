# Copilot instructions for this repo

## Project overview
- I'm attempting to utilize this repository for developing custom firmware for Inkbird IBS-TH2 Plus. It also has a Phy6222 IC, but it currently only uses it for BLE advertising. I want to use the existing code here to do the following:
  - Get it to advertise using the BTHome format [done]
    - I made a fork that adds DEVICE_IBSTH2P for this purpose
    - I set it to always stay in BOOT mode
  - Get OTA updating to work [done for stock-to-custom migration]
    - Stock BLE OTA now successfully flashes `inkbird_fw/STAGE3_IBSTH2P_stock_bundle_installer.hex16`
    - The staged SRAM installer copies the verified V15 image into low flash and reboots successfully
  - Get it to read temperature and humidity from the existing sensors [done in the V15 runtime]
- Firmware is split into two binaries: FW Boot (minimal, handles OTA) and FW App (full features). Boot/App use different linker scripts and OTA modes.
- Core firmware sources live in bthome_phy6222/source, built against the bundled PHY62x2 SDK in bthome_phy6222/SDK.

## Key directories and files
- bthome_phy6222/Makefile: authoritative build flags, device define selection, linker scripts, and SDK include paths.
- bthome_phy6222/mk_all.py: batch build of App + Boot for all supported devices (expects arm-none-eabi-gcc in PATH).
- bthome_phy6222/source/*: main firmware modules (BLE advertising in bthome_beacon.c, sensors in sensors.c, LCD drivers in lcd_*.c, OTA logic in ble_ota.c, config/persistence in config.c and flash_eep.c).
- bin/: released Boot/App binaries; update_boot/: OTA Boot updates.
- web/ and bthome_phy6222/web/PHY62x2BTHome.html: BLE OTA and configuration UI.
- Root python tools (rdwr_phy62x2.py, auto_time_sync.py) rely on requirements.txt.

## Build and flash workflow
- Build firmware with GNU Arm Embedded Toolchain:
  - Single target: make -C bthome_phy6222 PROJECT_NAME=<NAME> PROJECT_DEF="-DDEVICE=DEVICE_<DEVICE>"
  - Boot variant: add BOOT_OTA=1 (uses OTA boot linker script and OTA_TYPE=OTA_TYPE_BOOT).
  - All devices: run bthome_phy6222/mk_all.py (creates bin/ and boot/ outputs).
- For reproducible IBS-TH2 Plus runtime builds, use an isolated `OBJ_DIR` instead of the shared `bthome_phy6222/build` tree.
- Preferred IBS-TH2 Plus UART flasher is `flash_pogo.py` at the repo root.
- Preferred stock OTA page for the migration bundle is `inkbird_fw/InkbirdOTA.html`.
- `inkbird_fw/make_stock_stage3_bundle.py` now defaults to the hardware-verified `inkbird_fw/BOOT_IBSTH2P_v15.hex` payload.

## Project-specific conventions
- Device-specific pin/feature configuration is controlled by the DEVICE_* preprocessor define; avoid hardcoding device assumptions in shared modules.
- LCD and sensor support is split per-model in lcd_*.c and detected/initialized through sensors.c and dev_i2c.c.
- Boot/App separation is intentional: App has no OTA; it reboots into Boot for OTA.
- Current working deployment model for IBS-TH2 Plus is not the old Boot/App split; it is `stock OTA -> SRAM installer -> final custom boot runtime`.

## Integration points
- BLE advertising and GATT services are the main external interfaces (BTHome format); see bthome_beacon.c, thservice.c, sbp_profile.c.
- Persistent settings are stored in flash EEPROM emulation in flash_eep.c and config.c; changes here impact OTA compatibility and settings resets.
