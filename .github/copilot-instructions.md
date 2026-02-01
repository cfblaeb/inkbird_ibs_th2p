# Copilot instructions for this repo

## Project overview
- I'm attempting to utilize this repository for developing custom firmware for Inkbird IBS-TH2 Plus. It also has a Phy6222 IC, but it currently only uses it for BLE advertising. I want to use the existing code here to do the following:
  - Get it to advertise using the BTHome format
  - Get OTA updating to work
  - Get it to read temperature and humidity from the existing sensors
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
- Flash via USB-UART using rdwr_phy62x2.py; OTA updates are done through PHY62x2BTHome.html.

## Project-specific conventions
- Device-specific pin/feature configuration is controlled by the DEVICE_* preprocessor define; avoid hardcoding device assumptions in shared modules.
- LCD and sensor support is split per-model in lcd_*.c and detected/initialized through sensors.c and dev_i2c.c.
- Boot/App separation is intentional: App has no OTA; it reboots into Boot for OTA.

## Integration points
- BLE advertising and GATT services are the main external interfaces (BTHome format); see bthome_beacon.c, thservice.c, sbp_profile.c.
- Persistent settings are stored in flash EEPROM emulation in flash_eep.c and config.c; changes here impact OTA compatibility and settings resets.
