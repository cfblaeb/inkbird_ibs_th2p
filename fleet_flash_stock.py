#!/usr/bin/env python3
"""Stock-path fleet flasher: upgrade a stock Inkbird IBS-TH2P to custom fw.

Python port of the stock SHB OTA path from inkbird_fw/InkbirdOTA.html
(hispeed burst mode, protocol of the stock otam_protocol.c), reusing the
hex16 parsing / partition splitting / trampoline logic that already lives
in inkbird_fw/verify_ota_flash.py.

Flow:
  1. connect to the stock device (its 49:xx... address, connectable anytime)
  2. app mode -> mode-switch cmd 0x0102 -> device reboots as "PPlusOTA"
  3. reconnect, upload the STAGE3 bundle partitions, send finish
  4. device reboots: SRAM installer installs the custom image, then the
     custom firmware boots with the chip's REAL MAC (38:1F:8D:...) — a
     different address than the stock one, which is why step 5 exists
  5. watch for a new 38:1F:8D:* BTHome advertiser (fast window), read its
     Software Revision, and record the stock->new address mapping so
     downstream consumers can carry device identity across the change

Usage:
    python3 fleet_flash_stock.py 49:24:06:18:12:33 [STAGE3_...hex16]

NEVER flash 49:23:09:15:12:B2 or 49:23:09:15:14:D7 (older hardware that
works well on stock — hard blocklist below).
"""
import asyncio
import json
import sys
import time
from pathlib import Path

from bleak import BleakClient, BleakScanner

sys.path.insert(0, str(Path(__file__).parent / "inkbird_fw"))
from verify_ota_flash import apply_trampoline, parse_hex16, split_for_stock_ota

BLOCKLIST = {"49:23:09:15:12:B2", "49:23:09:15:14:D7"}

SHB_SERVICE = "5833ff01-9b8b-5191-6142-22a4536ef123"
SHB_CMD = "5833ff02-9b8b-5191-6142-22a4536ef123"
SHB_NTF = "5833ff03-9b8b-5191-6142-22a4536ef123"
SHB_DATA = "5833ff04-9b8b-5191-6142-22a4536ef123"
PHY_SERVICE = "0000fcd2-0000-1000-8000-00805f9b34fb"
SW_REV = "00002a28-0000-1000-8000-00805f9b34fb"

DEFAULT_BUNDLE = Path(__file__).parent / "inkbird_fw" / \
    "STAGE3_IBSTH2P_v22_stock_bundle_installer.hex16"

ERR_NAMES = {  # pplusErrorName() in InkbirdOTA.html
    0x01: "verify error", 0x02: "unknown command", 0x03: "not in OTA mode",
    0x04: "busy", 0x05: "no partition info", 0x06: "wrong packet order",
    0x07: "crc error", 0x08: "flash write error", 0x09: "overflow",
}

t0 = time.monotonic()


def log(msg):
    print(f"[{time.monotonic()-t0:6.1f}s] {msg}", flush=True)


class Notifier:
    """FF03 notification queue."""

    def __init__(self):
        self.q = asyncio.Queue()

    def cb(self, _h, data: bytearray):
        self.q.put_nowait(bytes(data))

    async def expect(self, what, timeout=15):
        resp = await asyncio.wait_for(self.q.get(), timeout)
        log(f"  notify: {resp.hex()}")
        if len(resp) == 1:
            raise RuntimeError(
                f"{what}: device error 0x{resp[0]:02x} "
                f"({ERR_NAMES.get(resp[0], 'unknown')})")
        return resp


async def find_device(match, timeout=20):
    """Return (address, name) of the first advertiser matching match(dev, adv)."""
    fut = asyncio.get_event_loop().create_future()

    def cb(dev, adv):
        if not fut.done() and match(dev, adv):
            fut.set_result((dev.address, adv.local_name or dev.name or ""))

    async with BleakScanner(cb):
        return await asyncio.wait_for(fut, timeout)


async def mode_switch(stock_addr):
    """Connect to the app-mode stock device and reboot it into PPlusOTA.
    Returns the OTA device's address (may equal the stock address)."""
    log(f"connecting to stock device {stock_addr} ...")
    disconnected = asyncio.Event()
    client = BleakClient(stock_addr, timeout=25,
                         disconnected_callback=lambda c: disconnected.set())
    await client.connect()
    try:
        uuids = [s.uuid for s in client.services]
        if PHY_SERVICE in uuids:
            raise RuntimeError("device runs CUSTOM firmware — use the pvvx "
                               "path (flash_v22 OTA bin), not the stock path")
        if SHB_SERVICE not in uuids:
            raise RuntimeError("no stock SHB OTA service — wrong device?")
        chars = [c.uuid for s in client.services if s.uuid == SHB_SERVICE
                 for c in s.characteristics]
        if SHB_DATA in chars:
            log("already in OTA mode (FF04 present); no mode switch needed")
            return stock_addr
        log("app mode: sending mode switch 0x0102 (disconnect is expected)")
        try:
            await client.write_gatt_char(SHB_CMD, bytes([0x01, 0x02]),
                                         response=True)
        except Exception:
            pass  # device reboots before the write response — expected
        try:
            await asyncio.wait_for(disconnected.wait(), 8)
        except asyncio.TimeoutError:
            pass
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass
    log("waiting for PPlusOTA advertiser ...")
    addr, name = await find_device(
        lambda d, a: (a.local_name or "").startswith("PPlusOTA"), 30)
    log(f"found {name} at {addr}")
    return addr


async def shb_upload(ota_addr, parts):
    log(f"connecting to OTA device {ota_addr} ...")
    async with BleakClient(ota_addr, timeout=25) as client:
        ntf = Notifier()
        await client.start_notify(SHB_NTF, ntf.cb)

        log(f"partition count: {len(parts)} (hispeed burst mode)")
        await client.write_gatt_char(
            SHB_CMD, bytes([0x01, len(parts), 0xFF]), response=True)
        resp = await ntf.expect("partition count", 10)
        if resp[:2] != b"\x00\x81":
            raise RuntimeError(f"unexpected partition-count response {resp.hex()}")

        total = sum(p.size for p in parts)
        sent = 0
        resp = b""
        for p in parts:
            info = bytes([0x02, p.index]) \
                + p.flash_off.to_bytes(4, "little") \
                + p.run_addr.to_bytes(4, "little") \
                + p.size.to_bytes(4, "little") \
                + p.checksum.to_bytes(2, "little")
            log(f"partition {p.index}: run=0x{p.run_addr:08X} "
                f"flash=0x{p.flash_off:08X} size={p.size} crc=0x{p.checksum:04X}")
            await client.write_gatt_char(SHB_CMD, info, response=True)
            resp = await ntf.expect("partition info", 10)
            if resp[:2] == b"\x00\x89":  # retry request
                log("  got 0089 retry, resending partition info")
                await client.write_gatt_char(SHB_CMD, info, response=True)
                resp = await ntf.expect("partition info retry", 10)
            if resp[:2] != b"\x00\x84":
                raise RuntimeError(f"partition info rejected: {resp.hex()}")
            await asyncio.sleep(0.1)

            data = p.data
            for off in range(0, len(data), 20):
                await client.write_gatt_char(SHB_DATA, data[off:off + 20],
                                             response=False)
                sent += min(20, len(data) - off)
                if off % 4000 == 0:
                    log(f"  ... {sent}/{total} bytes ({100*sent//total}%)")
            log("  all writes sent, waiting for partition completion")
            resp = await ntf.expect("partition completion", 90)
            if resp[:2] == b"\x00\x83":
                log("  all partitions complete (early)")
                break
            if resp[:2] != b"\x00\x85":
                raise RuntimeError(f"partition failed: {resp.hex()}")
            log(f"  partition {p.index} complete")

        if resp[:2] != b"\x00\x83":
            log("waiting for all-partitions-complete ...")
            resp = await ntf.expect("all complete", 20)
            if resp[:2] != b"\x00\x83":
                raise RuntimeError(f"expected 0083, got {resp.hex()}")
        log("all partitions complete — sending finish (0x04), device reboots")
        try:
            await client.write_gatt_char(SHB_CMD, bytes([0x04]), response=True)
        except Exception:
            pass  # reboot races the write response


async def watch_new_custom(known, timeout=120):
    """Wait for a 38:1F:8D:* BTHome advertiser not in `known`; verify revision."""
    log("watching for the new custom-firmware identity (38:1F:8D:*) ...")

    def match(dev, adv):
        return (dev.address.upper().startswith("38:1F:8D")
                and dev.address.upper() not in known
                and PHY_SERVICE in (adv.service_data or {}))

    addr, name = await find_device(match, timeout)
    log(f"new device: {name} at {addr}; reading Software Revision ...")
    rev = "?"
    for _ in range(3):  # fast window is active — connect should work
        try:
            async with BleakClient(addr, timeout=15) as c:
                rev = (await c.read_gatt_char(SW_REV)).decode()
            break
        except Exception as e:
            log(f"  revision read retry: {type(e).__name__}")
            await asyncio.sleep(2)
    return addr, name, rev


async def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    stock_addr = sys.argv[1].upper()
    bundle = Path(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_BUNDLE
    if stock_addr in BLOCKLIST:
        print(f"REFUSING: {stock_addr} is blocklisted (older hardware, "
              "stays on stock firmware)")
        return 1

    parts = apply_trampoline(split_for_stock_ota(parse_hex16(bundle)))
    log(f"loaded {bundle.name}: {len(parts)} partitions, "
        f"{sum(p.size for p in parts)} bytes")

    # Snapshot the custom devices already on air so the new one stands out.
    known = set()

    def note(dev, adv):
        if dev.address.upper().startswith("38:1F:8D"):
            known.add(dev.address.upper())

    async with BleakScanner(note):
        await asyncio.sleep(10)
    log(f"pre-flash custom devices in range: {sorted(known) or 'none'}")

    ota_addr = await mode_switch(stock_addr)
    await shb_upload(ota_addr, parts)

    addr, name, rev = await watch_new_custom(known)
    ok = "V22" in rev
    log(f"=== {'SUCCESS' if ok else 'CHECK NEEDED'}: {stock_addr} -> {addr} "
        f"({name}), revision {rev} ===")
    mapping = {"stock_addr": stock_addr, "new_addr": addr, "new_name": name,
               "revision": rev, "utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())}
    map_file = Path(__file__).parent / "fleet_flash_mapping.jsonl"
    with open(map_file, "a") as f:
        f.write(json.dumps(mapping) + "\n")
    log(f"mapping appended to {map_file}")
    return 0 if ok else 2


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        pass
