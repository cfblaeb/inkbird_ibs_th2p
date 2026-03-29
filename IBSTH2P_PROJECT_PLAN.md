# Inkbird IBS-TH2 Plus — Custom BLE Firmware Project

## Goal

Replace the stock Bluetooth firmware on the Inkbird IBS-TH2 Plus temperature/humidity logger's PHY6222 chip with custom firmware that:

1. **Listens for sensor data** from the primary (main) chip via the inter-chip communication bus.
2. **Advertises via BLE** in [BTHome v2 format](https://bthome.io/) at a low duty cycle (~every 30 minutes).
3. **Integrates with Home Assistant** for passive, battery-friendly environmental monitoring.

The device has two chips:
- **Primary chip** — reads the onboard temperature/humidity sensor, drives the LCD, manages buttons and power. This chip is NOT being reflashed.
- **PHY6222 (Phyplus)** — handles Bluetooth Low Energy. Stock firmware only advertises in Inkbird's proprietary format. **This is the chip we are reflashing** using [pvvx/ATC_MiThermometer](https://github.com/pvvx/ATC_MiThermometer) (fork: bthome_phy6222).

---

## Repository & Tooling

| Item | Detail |
|------|--------|
| Repo | Fork of pvvx/ATC_MiThermometer with `DEVICE_IBSTH2P` target added |
| Build | `make -C bthome_phy6222 PROJECT_NAME=BOOT_IBSTH2P PROJECT_DEF="-DDEVICE=DEVICE_IBSTH2P" BOOT_OTA=1` |
| Flash | `rdwr_phy62x2.py` over USB-UART |
| OTA | `web/PHY62x2BTHome.html` via Web Bluetooth, device MAC suffix `8EE69D` |
| Firmware variant | Building **Boot** (BOOT_OTA=1) — stays in boot mode permanently |

---

## What We Know (Established Facts)

### Hardware

- **2-chip architecture:** Primary MCU (unknown) reads sensor, drives LCD, manages buttons. PHY6222 handles BLE only.
- **Inter-chip link:** UART at 9600 baud — P17 (RX from main chip), P10 (TX to main chip).
- **P11:** Battery ADC voltage divider (`VBAT_ADC_P11`), external pull-up. NOT I2C.
- **P03:** Tied to GND.
- **All other 20 GPIOs:** Unconnected/floating (high-impedance, follow internal pulls).
- **I2C:** Conclusively ruled out — 8 pin pairs scanned, zero devices found.

### UART Stream (P17 RX from main chip)

- **Constant 18-byte heartbeat frame** repeated at ~46 frames/sec:
  ```
  a5 00 40 25 00 40 25 00 50 05 00 52 01 00 52 01 00 00
  ```
- Frame marker: first byte >= 0xA0 (typically 0xA5, occasionally 0xA4 for fragments).
- **Noise positions:** Byte 1 (00↔02), byte 9 (05↔25), byte 12 (01↔05) — frame-to-frame jitter, NOT data.
- **Stream is invariant:** Identical across 7s and 15s observation windows. Does NOT change with actual temperature (21.5°C vs 32°C differential test showed identical frames).

### What Has NOT Worked

| Approach | Result |
|----------|--------|
| Passive listen (7s, 15s) | Stream constant — no temperature data visible |
| Differential capture (21.5°C vs 32°C) | Frames identical — temp NOT encoded in heartbeat |
| 8 arbitrary TX probes on P10 | No effect on P17 stream |
| Modbus RTU (4 standard probes) | No response — main chip doesn't speak Modbus |
| Echo (replay captured frame) | No effect |
| Frame-synchronized timed probes | **NOT YET TESTED** (V7 binary failed to flash) |

### Key Hypothesis

The main chip's heartbeat is a "keep-alive" or status frame. The **original PHY6222 firmware** likely sends a specific response (within a tight timing window after each heartbeat) that triggers the main chip to include sensor data in subsequent frames. We have not yet tested this hypothesis because the V7 firmware (which implements frame-synchronized response probing) was never successfully OTA'd.

---

## Progress & Status

### Phase 1: BTHome Advertising — DONE ✅

- Added `DEVICE_IBSTH2P` blocks to `config.h`, `main.c`, and build system.
- Firmware advertises in BTHome format, forced to BOOT mode.

### Phase 2: Identify Inter-Chip Communication — DONE ✅

- GPIO scan (23 pins) → UART on P10/P17
- I2C ruled out
- Baud rate: 9600
- P11 = battery ADC (not sensor I2C)

### Phase 3: UART Protocol Decode — IN PROGRESS 🔄

**Objective:** Determine how to get the main chip to send temperature/humidity data.

#### Completed experiments (V1–V6):
- [x] V1–V3: GPIO scan, I2C scan, baud detection, raw UART capture
- [x] V4: Differential capture (21.5°C vs 32°C) — frames identical, temp NOT in heartbeat
- [x] V5: 8 TX probes on P10 — no effect
- [x] V6: 7s passive listen, 4 Modbus RTU probes, echo probe — all no effect
- [x] V6 re-run with 15s listen — confirmed stream constant (692 frames, only noise diffs)

#### Current state — V7 (built, NOT yet flashed):
- [x] V7 firmware written with frame-synchronized timed response probing (12 patterns)
- [x] V7 binary compiled and verified via disassembly (49,940 bytes, types 16/17 confirmed in ELF)
- [x] Web UI updated for V7 record types 16/17
- [ ] **V7 OTA flash** — previous flash attempt did NOT update the device (device still ran V6)
- [ ] **V7 test** — run scan, analyze timed probe results

---

## Current Firmware (V7) — What It Does

Source: `bthome_phy6222/source/cmd_parser.c`
Binary: `bthome_phy6222/build/BOOT_IBSTH2P.bin` (49,940 bytes)

**Phase A — 15-second passive listen:**
- Records reference frame + up to 4 diff frames (record types 10/11)

**Phase B — 12 frame-synchronized response probes:**
- ISR detects frame completion via `frame_ready` flag
- Main loop immediately sends TX response on P10 within ~32μs of frame end
- Listens 300ms after each probe for changed frames
- Record types 16 (probe result) / 17 (probe diff)

**12 response patterns tested:**

| # | TX Data | Rationale |
|---|---------|-----------|
| 0 | `01` | Simple ACK byte |
| 1 | `06` | ACK/NAK byte |
| 2 | `A5 00` | Marker + null |
| 3 | `A5 01` | Marker + command 1 |
| 4 | `A4 00` | Alt marker + null |
| 5 | `A5 03` | Marker + FC3-like |
| 6 | `A5 04` | Marker + FC4-like |
| 7 | `A5 01 00 00 00 00 00 00` | Longer command |
| 8 | `A4 01 00 00 00 00 00 00` | Alt marker longer |
| 9 | (echo reference frame) | Mirror heartbeat back |
| 10 | (ref with byte[1]=0x01) | Modified heartbeat |
| 11 | `A5 05 01` | Possible "read data" command |

---

## Next Steps (Priority Order)

### 1. Successfully flash V7 and test timed probes

The V7 binary on disk is confirmed correct (disassembly shows types 16/17). The previous OTA attempt failed silently — the device kept running V6. Try again:
- Ensure the correct `.bin` file is selected in `PHY62x2BTHome.html`
- Verify the OTA completes (watch for success/error messages)
- After flash, run "Scan IO" — expect ~20 second wait
- If type 16/17 records appear → V7 is running

### 2. If V7 timed probes all show only noise diffs

Escalation approaches (try in order):
- **Continuous response mode:** Send responses after EVERY heartbeat for 10+ seconds continuously (not just one-shot probes)
- **Different baud rate TX:** Main chip might expect P10 TX at a different baud than P17 RX
- **Init sequence:** Original firmware might send a multi-step handshake at boot time
- **Flash dump of original firmware:** Use `CMD_ID_MEM_RW` to read flash regions that might contain the stock PHY6222 firmware (could be in an app bank) — disassemble to find the UART TX handler
- **Longer passive listen (60s+):** Catch very infrequent data frames (if main chip only sends sensor data periodically)
- **Sniff original firmware traffic:** If stock firmware can be restored, use a logic analyzer on P10/P17 to capture the actual protocol

### 3. Phase 4: Implement Sensor Data Reception

Once protocol is decoded:
- Write persistent UART listener (P17, 9600 baud)
- Parse temperature/humidity from decoded format
- Store latest readings in RAM

### 4. Phase 5: Low-Power BTHome Advertising

- Configure ~30 minute advertising interval
- Pack temp + humidity into BTHome v2 payload
- Deep sleep between advertisements
- Test Home Assistant discovery

### 5. Phase 6: OTA Support

- Verify OTA works reliably (current OTA flash issue needs investigation)
- Ensure Boot/App firmware split is functional

---

## Hardware Reference

### PHY6222 GPIO Mapping

| Enum | Pin | Function |
|------|-----|----------|
| 3 | GPIO_P03 | Tied to GND |
| 6 | GPIO_P10 | UART TX to main chip (idle-HIGH) |
| 7 | GPIO_P11 | Battery ADC (voltage divider pull-up) |
| 11 | GPIO_P17 | UART RX from main chip (active — heartbeat stream) |
| All others | — | Unconnected (high-impedance) |

### Key Source Files

| File | Purpose |
|------|---------|
| `bthome_phy6222/source/cmd_parser.c` | UART scan/probe logic (V7 timed probes) |
| `bthome_phy6222/source/config.h` | DEVICE_IBSTH2P pin/feature definitions |
| `bthome_phy6222/source/main.c` | GPIO initialization |
| `bthome_phy6222/source/bthome_beacon.c` | BTHome advertisement formatting |
| `bthome_phy6222/web/PHY62x2BTHome.html` | Web Bluetooth OTA, config, and scan UI |

### Inkbird BLE Advertising Format (stock firmware)

```python
(temp, hum, probe, modbus, bat) = struct.unpack("<hHBHB", xvalue[0:8])
```
The "modbus" field name in the stock BLE format was a clue we investigated — Modbus RTU probes showed no response from the main chip.
