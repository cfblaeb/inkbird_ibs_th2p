#!/usr/bin/env python3
"""Round-robin pvvx-path flasher: flash V22 onto any of the listed custom
units as their operator battery-pulls them one at a time.

Cycles connect attempts across all remaining targets; whichever device is
in its post-reset fast window gets flashed and verified, then the loop
continues with the rest. Custom->custom flashes keep the MAC, so HA
identity is untouched.

Usage: python3 fleet_flash_custom_any.py MAC1 [MAC2 ...]
"""
import asyncio
import sys
import time
from pathlib import Path

from bleak import BleakClient

REPO = Path(__file__).parent
OTA_BIN = REPO / "inkbird_fw" / "BOOT_IBSTH2P_v22_ota.bin"
SW_REV_CHAR = "00002a28-0000-1000-8000-00805f9b34fb"

TARGETS = [a.upper() for a in sys.argv[1:]]
if not TARGETS or not all(a.startswith("38:1F:8D") for a in TARGETS):
    print(__doc__)
    sys.exit(1)

sys.argv = ["ble_phy_ota_flash.py", str(OTA_BIN)]
src = open(REPO / "ble_phy_ota_flash.py").read()
entry = "sys.exit(asyncio.run(main()))"
assert entry in src
ns = {}
exec(compile(src.replace(entry, ""), "ble_phy_ota_flash.py", "exec"), ns)


async def try_connect(addr):
    client = BleakClient(addr, timeout=8)
    try:
        await client.connect()
        return client
    except Exception:
        return None


async def flash_one(addr, client, img):
    try:
        # Audit #40: the revision read must sit inside the try/finally —
        # a GATT failure here used to leak the BLE connection.
        rev = (await client.read_gatt_char(SW_REV_CHAR)).decode()
        print(f"[{addr}] connected; current revision: {rev}", flush=True)
        await ns["flash"](client, img)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    print(f"[{addr}] flashed; waiting 20 s for boot-updater install", flush=True)
    await asyncio.sleep(20)
    deadline = time.monotonic() + 90
    while time.monotonic() < deadline:
        c = await try_connect(addr)
        if c:
            try:
                rev = (await c.read_gatt_char(SW_REV_CHAR)).decode()
            finally:
                try:
                    await c.disconnect()
                except Exception:
                    pass
            ok = "V22" in rev
            print(f"[{addr}] post-flash revision: {rev} "
                  f"{'— V22 CONFIRMED' if ok else '— UNEXPECTED!'}", flush=True)
            return ok
        await asyncio.sleep(2)
    print(f"[{addr}] flashed but could not reconnect to verify — check via "
          "passive 0xF2 scan", flush=True)
    return True


async def main():
    img = ns["load_image"](str(OTA_BIN))
    remaining = list(TARGETS)
    print(f"watching for fast windows on: {', '.join(remaining)}", flush=True)
    print("OPERATOR: pull+reseat the battery on ONE device at a time; "
          "wait for its DONE line before the next.", flush=True)
    while remaining:
        for addr in list(remaining):
            client = await try_connect(addr)
            if client is None:
                continue
            print(f"=== {addr} woke up — flashing ===", flush=True)
            try:
                ok = await flash_one(addr, client, img)
            except Exception as e:
                print(f"[{addr}] FAILED: {type(e).__name__}: {e} — device "
                      "keeps its old firmware; battery-pull to retry",
                      flush=True)
                continue
            if ok:
                remaining.remove(addr)
                print(f"=== {addr} DONE ({len(TARGETS)-len(remaining)}/"
                      f"{len(TARGETS)}) — next battery pull when ready ===",
                      flush=True)
    print("=== ALL TARGETS FLASHED ===", flush=True)


try:
    asyncio.run(main())
except KeyboardInterrupt:
    pass
