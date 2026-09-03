#!/usr/bin/env python3
"""pvvx-path fleet flasher: upgrade a custom-firmware IBSTH2P to the
pinned OTA image (IMAGES below; default v24 = fleet release, or
IBS_OTA_IMAGE=p10 for the V25 "P10 alone" experiment image, IBS-P25).

For devices already on custom firmware (38:1F:8D:* BTHome). Their address
does NOT change across the flash, so HA identity is untouched.

V18+ firmware: power-cycle the device when prompted (60 s fast window).
V15-V17 firmware (no fast window): run the sudo raw-HCI helper in another
terminal instead; this script's retry loop will attach to that link.

Usage: python3 fleet_flash_custom.py 38:1F:8D:XX:XX:XX
       IBS_OTA_IMAGE=p10 python3 fleet_flash_custom.py 38:1F:8D:XX:XX:XX
"""
import asyncio
import sys
import time
from pathlib import Path

from bleak import BleakClient

REPO = Path(__file__).parent
import os, re
IMAGES = {
    "v24": "BOOT_IBSTH2P_v24_ota.bin",        # default, fleet release
    "p10": "BOOT_IBSTH2P_v25_p10_ota.bin",    # V25 "P10 alone" experiment (IBS-P25)
}
IMAGE = os.environ.get("IBS_OTA_IMAGE", "v24")
if IMAGE not in IMAGES:
    sys.exit(f"IBS_OTA_IMAGE must be one of {sorted(IMAGES)}")
OTA_BIN = REPO / "inkbird_fw" / IMAGES[IMAGE]
SW_REV_CHAR = "00002a28-0000-1000-8000-00805f9b34fb"

ADDR = sys.argv[1].upper() if len(sys.argv) > 1 else ""
if not ADDR.startswith("38:1F:8D"):
    print(__doc__)
    sys.exit(1)

sys.argv = ["ble_phy_ota_flash.py", str(OTA_BIN), ADDR]
src = open(REPO / "ble_phy_ota_flash.py").read()
entry = "sys.exit(asyncio.run(main()))"
assert entry in src
ns = {}
exec(compile(src.replace(entry, ""), "ble_phy_ota_flash.py", "exec"), ns)


async def connect_retry(deadline, why):
    while time.monotonic() < deadline:
        try:
            client = BleakClient(ADDR, timeout=15)
            await client.connect()
            return client
        except Exception as e:
            print(f"connect attempt failed ({why}): {type(e).__name__}",
                  flush=True)
            await asyncio.sleep(2)
    return None


async def main():
    img = ns["load_image"](str(OTA_BIN))
    print(f"POWER-CYCLE {ADDR} now — retrying connect for up to 10 min. "
          "(No fast window on V15-V17: run the sudo raw-HCI helper instead.)",
          flush=True)
    client = await connect_retry(time.monotonic() + 600, "waiting for window")
    if client is None:
        print("never connected — rerun, or the device is pre-V18 (sudo helper).",
              flush=True)
        return 1
    try:
        rev = (await client.read_gatt_char(SW_REV_CHAR)).decode()
        print(f"connected; current Software Revision: {rev}", flush=True)
        await ns["flash"](client, img)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    print("waiting 20 s for reboot + boot-updater install...", flush=True)
    await asyncio.sleep(20)
    client = await connect_retry(time.monotonic() + 90, "post-reboot verify")
    if client is None:
        print("flash done but could not reconnect to verify — check 0xF2 "
              "via passive scan.", flush=True)
        return 0
    try:
        rev = (await client.read_gatt_char(SW_REV_CHAR)).decode()
        m = re.search(rb"IBS-[VXP]\d\d", open(OTA_BIN, "rb").read())
        expect = m.group().decode() if m else OTA_BIN.stem
        print(f"post-flash Software Revision: {rev} "
              f"{f'— {expect} CONFIRMED' if expect in rev else '— UNEXPECTED!'}",
              flush=True)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    return 0


sys.exit(asyncio.run(main()))
