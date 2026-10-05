# Inkbird IBS-TH2 Plus: custom BTHome firmware

Replacement firmware for the BLE chip (PHY6222) in the Inkbird IBS-TH2 Plus
thermometer. It reads temperature and humidity from the
thermometer's own main MCU and broadcasts them as unencrypted
[BTHome v2](https://bthome.io), so Home Assistant picks the unit up
automatically with no Inkbird app or cloud. Pressing the device's button
sends a BTHome button event, which makes it easy to tell units apart in
Home Assistant.

The firmware is installed over Bluetooth through a web page.
You don't need to open the case or use a serial adapter.

Based on [pvvx/THB2](https://github.com/pvvx/THB2).

## V27

| | |
|---|---|
| BLE name / address | `IBSTH2P-XXXXXX`, chip MAC `38:1F:8D:XX:XX:XX` |
| Advertising | BTHome v2, every 10 s, non-connectable |
| Objects | temperature (0.01 °C), humidity (0.01 %), battery %, battery voltage, firmware version, button press |
| Extra object | `0x09` "count": time from the main MCU's wake line to the first UART byte, in ms (diagnostic; 255 means no reading yet) |
| Connect window | ~60 s after a button press or a battery reinsert, so you can do OTA or read diagnostics |
| Battery life | Estimated to be years |

The steady state is non-connectable on purpose. If a phone or gateway
holds a connection open, the chip stays fully awake and the battery drains.

## Stock to custom, step by step

You need a computer or Android phone with Chrome or Edge (Web Bluetooth),
an IBS-TH2 Plus running stock firmware (tested on stock 2.7), and this repository.

1. **Check the device is stock.** Scan with any BLE scanner (nRF Connect,
   or the Chrome device picker). A stock unit advertises as **`sps`** with
   an address starting `49:`. Write down that address. The custom firmware
   comes up on a different one (see step 6).
2. **Open the flasher.** Open `inkbird_fw/InkbirdOTA.html` in Chrome. A
   local file works; the page only talks to the device over Web Bluetooth.
3. **Load the bundle.** Under *1. Load Firmware*, choose
   `inkbird_fw/STAGE3_IBSTH2P_v27_p03_stock_bundle_installer.hex16`.
4. **Connect and flash.** Click *Connect & Flash* and pick the `sps`
   device. The page switches it into the stock updater, and it reboots and
   advertises as **`PPlusOTA`** (at the stock address + 1). When the page
   asks, click the green **Connect to PPLUSOTA** button and pick it. The
   upload then runs to completion. Keep the device close and don't
   navigate away.
5. **Let it install.** The bundle carries a small installer that runs from
   RAM after the upload. It checks the staged image, writes the custom
   firmware into place, checks it again and reboots. This takes a few
   seconds.
6. **Find the new device.** It now advertises as `IBSTH2P-XXXXXX` with
   address `38:1F:8D:…`. This is the chip's real MAC; the stock firmware
   used a different one. For the first ~60 s after boot it advertises fast
   and is connectable. After that it beacons every 10 s.
7. **Add it to Home Assistant.** The BTHome integration discovers it on
   its own. Press the device's button to confirm which entity is which
   unit.

The page detects which kind of device you connected to and refuses a
mismatched file, so a custom `_ota.bin` can't be sent to a stock unit, or
the reverse.

### Scripted alternative (Linux, bleak)

```bash
pip install -r requirements.txt
python3 tools/fleet_flash_stock.py 49:XX:XX:XX:XX:XX
```

This script does the same as steps 4-6 with no browser. It then watches
for the new `38:1F:8D:*` advertiser, reads its Software Revision (expects
`IBS-W27`) and appends the old and new address pair to
`tools/fleet_flash_mapping.jsonl` (gitignored). Use that file to re-link
the device to its existing records after the address change. Pass a
different `STAGE3_*.hex16` as the second argument to install something
else.

## Updating a unit that already runs custom firmware

Custom-to-custom updates keep the MAC, so Home Assistant entities stay as
they are.

1. Press the device's button. That opens a ~60 s connectable window.
   Reinserting the battery also works.
2. In `InkbirdOTA.html`, load `inkbird_fw/BOOT_IBSTH2P_v27_p03_ota.bin`,
   click *Connect & Flash* and pick `IBSTH2P-XXXXXX`.

Or from Linux:

```bash
python3 tools/fleet_flash_custom.py 38:1F:8D:XX:XX:XX
# another image:
IBS_OTA_IMAGE=path/to/BOOT_xxx_ota.bin python3 tools/fleet_flash_custom.py 38:1F:8D:XX:XX:XX
```

An interrupted transfer is safe. The device marks the staged image valid
only after its full CRC32 checks out, so it keeps running the old firmware
until then.

## Going back to stock / recovery

- `bthome_phy6222/orig/orig.bin` is a full flash dump of a stock unit. You
  can write it back over the PHY6222's UART with `tools/flash_pogo.py` or
  `tools/rdwr_phy62x2.py` (pvvx's tools; see the
  [THB2 docs](https://github.com/pvvx/THB2) for UART wiring and boot-mode
  entry). This needs physical access to the chip's UART pads.
- If a custom unit stops responding, reinsert the battery. The 60 s
  connectable window after reboot is always available, so you can OTA it
  again.

## Repository layout

| Path | Contents |
|---|---|
| `inkbird_fw/` | Release artifacts (V27), the Web Bluetooth flasher, the stock-bundle generator and its SRAM installer source |
| `bthome_phy6222/` | Firmware source (pvvx THB2 tree with the `DEVICE_IBSTH2P` target), SDK, host tests |
| `bthome_phy6222/orig/` | Stock Inkbird firmware dump |
| `tools/` | Python flashers and diagnostics (`bthome_monitor.py` live TUI, `ucap_stats.py` counter readout) |
| `docs/` | Engineering log (per-version notes, the reverse-engineered inter-chip UART protocol, toolchain recipe) and the code reviews |

### Release artifacts (`inkbird_fw/`)

| File | Use |
|---|---|
| `STAGE3_IBSTH2P_v27_p03_stock_bundle_installer.hex16` | Stock to custom (load into `InkbirdOTA.html`) |
| `STAGE3_IBSTH2P_v27_p03_stock_bundle_payload.bin` | Staged payload inside the bundle (reference) |
| `BOOT_IBSTH2P_v27_p03_ota.bin` | Custom to custom OTA |
| `BOOT_IBSTH2P_v27_p03.hex` | Raw image for UART flashing and bundle generation |
| `ibs_thx_b_2p7_48M_phy6222.hex16` | Inkbird's stock updater image, an input to the bundle generator |

## How it works

The IBS-TH2 Plus has two chips. A main MCU runs the sensor and the LCD.
Every ~10.4 s it sends a 13-byte, CRC-16/MODBUS-protected frame over UART
to the PHY6222, which only handles BLE. Before each frame the main MCU
raises GPIO P03. V27 uses that line the same way the stock firmware does:
the PHY6222 sleeps with P03 armed as a wake source, holds the UART awake
for one frame, and releases it on a CRC-good frame or after a 250 ms
timeout. The frame also carries the button state.

Stock-to-custom has three stages:

1. Inkbird's own updater (`PPlusOTA`) receives the bundle. The bundle keeps
   the updater's flash partitions intact, so the updater isn't overwritten
   while it runs.
2. The bundle stages the final image in high flash inside a small `IBI3`
   container, together with an SRAM installer.
3. On reboot the installer validates the container, erases the low-flash
   target, writes the image, verifies it, clears OTA mode and resets into
   the custom firmware.

`docs/ENGINEERING_LOG.md` has the details, including the full UART frame
layout.

## Building

Release images are built with a pinned toolchain: Debian
gcc-arm-none-eabi 14.2.rel1-1, newlib 4.5.0.20241231-1 and binutils 2.42.
Ubuntu's packaged 13.2 cross-compiler generates different code and isn't
validated. To fetch the pinned packages without root, follow the recipe in
`docs/ENGINEERING_LOG.md` (section *Reproducible builds on Ubuntu 24.04*).
`$TC` below is the directory they were extracted to.

```bash
# firmware (the UCAP_P03=1 define is what makes this V27; without it you get a legacy scheduler)
PATH=$TC/root/usr/bin:$PATH make -B -C bthome_phy6222 \
  OBJ_DIR=build_boot_ibsth2p_v27 PROJECT_NAME=BOOT_IBSTH2P \
  PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P -DUCAP_P03=1" BOOT_OTA=1 \
  CROSS_COMPILE="arm-none-eabi-" \
  CC="arm-none-eabi-gcc -B$TC/xbin -B$TC/root/usr/lib/arm-none-eabi/newlib \
      -isystem $TC/root/usr/include/newlib"

# custom-to-custom OTA image
cd bthome_phy6222 && python3 phy62x2_ota.py -w 0x2F00 -f ota_upboot.add \
  build_boot_ibsth2p_v27/BOOT_IBSTH2P.hex

# stock-to-custom bundle from your image (with no --final-hex it rebuilds the
# committed V27 bundle byte-for-byte). Uses the committed SRAM installer in
# inkbird_fw/build/; run `make -C inkbird_fw/stock_bundle_installer` only if you change it.
python3 inkbird_fw/make_stock_stage3_bundle.py \
  --final-hex bthome_phy6222/build_boot_ibsth2p_v27/BOOT_IBSTH2P.hex \
  --output-hex STAGE3_test.hex16 --payload-bin STAGE3_test_payload.bin
```

Host tests for the UART receiver and framing (no hardware needed):

```bash
cd bthome_phy6222/tests
for t in test_ucap_frame test_ucap_p03; do
  gcc -Wall -Wextra -std=gnu11 -fsanitize=address,undefined -o $t $t.c && ./$t
done
```

## Known limitations

- **Stale reading if the main MCU dies.** If the main MCU stops sending
  frames, the PHY6222 keeps advertising the last good reading with fresh
  packet IDs, and Home Assistant won't mark the unit unavailable. This has
  never been seen in the field; details are in `docs/V27_REVIEW.md` (S1).
- **Unauthenticated debug commands.** Debug and config GATT commands
  (memory, register and config read/write) are compiled in without
  authentication. They can only be reached during the ~60 s connect
  windows.
- **Limited testing.** Tested only on "new" IBS-TH2 Plus units with a PHY6222.
  Other Inkbird models and hardware revisions are untested.
  `fleet_flash_stock.py` carries a blocklist for two older units that were
  deliberately left on stock.

`docs/V27_REVIEW.md` lists every known issue, with the owner's triage.
