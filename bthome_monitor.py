#!/usr/bin/env python3
"""Monitor unencrypted BTHome v2 broadcasts from an Inkbird IBS-TH2P.

Usage:
    python3 bthome_monitor.py                 # match by name (default below)
    python3 bthome_monitor.py IBSTH2P-CF776F  # match by name substring
    python3 bthome_monitor.py AA:BB:CC:...    # match by MAC address
"""
import sys
import struct
import asyncio
from datetime import datetime
from bleak import BleakScanner

BTHOME_UUID = "0000fcd2-0000-1000-8000-00805f9b34fb"
DEFAULT_MATCH = "IBSTH2P"

# object_id -> (name, byte length, signed?, scale factor)
OBJECTS = {
    0x00: ("packet_id",      1, False, 1),
    0x01: ("battery_%",      1, False, 1),
    0x02: ("temperature_C",  2, True,  0.01),
    0x03: ("humidity_%",     2, False, 0.01),
    0x04: ("pressure_hPa",   3, False, 0.01),
    0x0C: ("voltage_V",      2, False, 0.001),
    0x12: ("co2_ppm",        2, False, 1),
    0x2E: ("humidity_%",     1, False, 1),
    0x2F: ("temperature_C",  1, True,  1),
    0x3A: ("button_event",   1, False, 1),
}


def decode_bthome(data: bytes) -> dict:
    """Decode a BTHome v2 service-data payload (device-info byte + objects)."""
    out = {}
    i = 1  # skip device-info flags byte
    while i < len(data):
        obj = data[i]
        i += 1
        if obj not in OBJECTS:
            # unknown object; can't know its length — show the rest raw
            out[f"unknown_0x{obj:02x}"] = data[i:].hex() or "-"
            break
        name, length, signed, scale = OBJECTS[obj]
        raw = int.from_bytes(data[i:i + length], "little", signed=signed)
        i += length
        out[name] = round(raw * scale, 3)
    return out


# De-duplication state: only payload changes start a new line; identical
# re-transmissions bump a counter updated in place on the current line.
_last_key = None
_line = ""
_repeats = 0


def callback(device, adv):
    global _last_key, _line, _repeats
    sd = adv.service_data.get(BTHOME_UUID)
    if not sd:
        return
    key = (device.address, bytes(sd))
    if key == _last_key:
        _repeats += 1
        print(f"\r{_line}  (x{_repeats})", end="", flush=True)
        return
    if _last_key is not None:
        print()  # finalize the previous line
    values = decode_bthome(sd)
    fields = "  ".join(f"{k}={v}" for k, v in values.items())
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    _last_key = key
    _repeats = 1
    _line = f"{ts}  {device.address}  RSSI={adv.rssi:>4}  {fields}"
    print(_line, end="", flush=True)


async def main():
    match = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_MATCH
    is_mac = ":" in match
    match_l = match.lower()

    def detection(device, adv):
        name = (adv.local_name or device.name or "")
        if is_mac:
            if device.address.lower() != match_l:
                return
        elif match_l not in name.lower():
            return
        callback(device, adv)

    print(f"Scanning for '{match}'  (Ctrl-C to stop)...")
    scanner = BleakScanner(detection_callback=detection)
    await scanner.start()
    try:
        while True:
            await asyncio.sleep(1)
    finally:
        await scanner.stop()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print()  # finalize the in-place line before exiting
