#!/usr/bin/env python3
"""pvvx-path flasher: upgrade a custom-firmware IBSTH2P to the pinned OTA
image (IMAGES below; default v27, or IBS_OTA_IMAGE=<key> / a path to any
PHY6 _ota.bin).

For devices already on custom firmware (38:1F:8D:* BTHome). Their address
does NOT change across the flash, so HA identity is untouched.

V27 is non-connectable between button presses: press the device button
(or pull and reinsert the battery) when prompted; either opens a ~60 s
connectable window. V18-V26: power-cycle. V15-V17 have no fast window and
need a raw-HCI LE connection helper (ble_le_conn_ext.py, in git history).

Usage: python3 tools/fleet_flash_custom.py 38:1F:8D:XX:XX:XX
       IBS_OTA_IMAGE=path/to/BOOT_xxx_ota.bin python3 tools/fleet_flash_custom.py 38:1F:8D:XX:XX:XX
"""
import asyncio
import sys
import time
from pathlib import Path

from bleak import BleakClient

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
import os, re
IMAGES = {
    "v27": "BOOT_IBSTH2P_v27_p03_ota.bin",    # V27: P03 wake-line receiver, non-connectable steady state (IBS-W27)
}
IMAGE = os.environ.get("IBS_OTA_IMAGE", "v27")
if IMAGE in IMAGES:
    OTA_BIN = REPO / "inkbird_fw" / IMAGES[IMAGE]
elif Path(IMAGE).is_file():
    OTA_BIN = Path(IMAGE)
else:
    sys.exit(f"IBS_OTA_IMAGE must be one of {sorted(IMAGES)} or a path to an _ota.bin")
SW_REV_CHAR = "00002a28-0000-1000-8000-00805f9b34fb"

ADDR = sys.argv[1].upper() if len(sys.argv) > 1 else ""
if not ADDR.startswith("38:1F:8D"):
    print(__doc__)
    sys.exit(1)

sys.argv = ["ble_phy_ota_flash.py", str(OTA_BIN), ADDR]
src = open(HERE / "ble_phy_ota_flash.py").read()
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
    print(f"PRESS THE BUTTON on {ADDR} (or power-cycle it) now — retrying "
          "connect for up to 10 min.", flush=True)
    client = await connect_retry(time.monotonic() + 600, "waiting for window")
    if client is None:
        print("never connected — rerun and press the button again "
              "(pre-V18 firmware needs a raw-HCI connect helper).", flush=True)
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
        m = re.search(rb"IBS-[VXPW]\d\d", open(OTA_BIN, "rb").read())
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
