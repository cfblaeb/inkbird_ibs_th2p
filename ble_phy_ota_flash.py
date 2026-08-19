#!/usr/bin/env python3
"""pvvx BTHome OTA flasher for the IBSTH2P (PHY6222).

Protocol mirror of bthome_phy6222/source/ble_ota.c (same as the pvvx web
flasher and the new pvvx path in InkbirdOTA.html):
  CMD_OTA_START (0xFF00) -> CMD_OTA_SET (0xFF01) -> 16-byte blocks
  [idxLE | data16 | crc16(init 0xFFFF, poly 0xA001) LE] -> final status must
  be 0xFF (OTA end) -> CMD_OTA_END (0xFF02) reboots the device.

Safety: the device blanks the PHY6 magic of the staged image on block 0 and
only writes it back after verifying the full-image CRC32 in flash, so an
interrupted transfer leaves the current firmware untouched.

Usage: waits for an existing ACL link (created via sudo le_conn_ext.py),
attaches, validates, flashes, reboots.
"""
import asyncio
import struct
import sys
import time

from bleak import BleakClient
from dbus_fast.aio import MessageBus
from dbus_fast import BusType, Message

ADDR = sys.argv[2] if len(sys.argv) > 2 else "38:1F:8D:CF:77:6F"
DEVPATH = "/org/bluez/hci0/dev_" + ADDR.replace(":", "_")
OTA_BIN = sys.argv[1] if len(sys.argv) > 1 else \
    "/home/laeb/WORK/inkbird_ibs_th2p/inkbird_fw/BOOT_IBSTH2P_v16_ota.bin"
U = lambda s: f"0000{s}-0000-1000-8000-00805f9b34fb"
OTA_CHAR = U("fff3")
PHY6_MAGIC = 0x36594850

ERRS = {0: "OK", 1: "wrong command", 2: "start not set", 3: "params not set",
        4: "wrong params", 5: "wrong packet size", 6: "packet CRC error",
        7: "packet loss", 8: "flash write error", 9: "overflow",
        10: "fw id error", 11: "program CRC32 error", 0xFF: "OTA end"}

t0 = time.monotonic()
def stamp(): return f"[{time.monotonic()-t0:6.1f}s]"


def crc16m(data, length):
    crc = 0xFFFF
    for i in range(length):
        crc ^= data[i]
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def load_image(path):
    raw = open(path, "rb").read()
    if len(raw) < 20 or struct.unpack("<I", raw[:4])[0] != PHY6_MAGIC:
        raise SystemExit("not a PHY6 OTA image")
    segs, start, size = struct.unpack_from("<III", raw, 4)
    if size + 4 != len(raw):
        raise SystemExit("PHY6 header size mismatch")
    import zlib
    stored = struct.unpack("<I", raw[size:size + 4])[0]
    if stored != (0xFFFFFFFF - zlib.crc32(raw[:size])) & 0xFFFFFFFF:
        raise SystemExit("PHY6 image CRC32 mismatch")
    pad = (-len(raw)) % 16
    img = raw + b"\xFF" * pad
    print(stamp(), f"image OK: {len(raw)} bytes, {segs} segs, start 0x{start:X}, "
          f"{len(img)//16} blocks")
    return img


async def read_status(client):
    v = await client.read_gatt_char(OTA_CHAR)
    if len(v) < 20:
        raise RuntimeError(f"short OTA status ({len(v)} bytes)")
    err, ver, start_flag = v[0], v[1], v[2]
    pkt_idx = struct.unpack_from("<H", v, 8)[0]
    return err, ver, start_flag, pkt_idx


async def flash(client, img):
    nblocks = len(img) // 16

    err, ver, start_flag, _ = await read_status(client)
    print(stamp(), f"device OTA ver {ver}, err={err}, start_flag={start_flag}")

    print(stamp(), "CMD_OTA_START")
    await client.write_gatt_char(OTA_CHAR, bytes([0x00, 0xFF]), response=False)
    err, *_ = await read_status(client)
    if err:
        raise RuntimeError(f"OTA start rejected: {ERRS.get(err, err)}")

    print(stamp(), "CMD_OTA_SET")
    await client.write_gatt_char(OTA_CHAR, bytes([0x01, 0xFF]), response=False)
    err, _, start_flag, _ = await read_status(client)
    if err or start_flag != 1:
        raise RuntimeError(f"OTA set rejected: {ERRS.get(err, err)}")

    print(stamp(), f"streaming {nblocks} blocks...")
    pkt = bytearray(20)
    for i in range(nblocks):
        pkt[0] = i & 0xFF
        pkt[1] = (i >> 8) & 0xFF
        pkt[2:18] = img[i * 16:i * 16 + 16]
        crc = crc16m(pkt, 18)
        pkt[18] = crc & 0xFF
        pkt[19] = (crc >> 8) & 0xFF
        await client.write_gatt_char(OTA_CHAR, bytes(pkt), response=False)
        if (i + 1) % 16 == 0 and i != nblocks - 1:
            err, _, _, idx = await read_status(client)
            if err:
                raise RuntimeError(f"device error at block {i}: {ERRS.get(err, err)} (idx={idx})")
        if (i + 1) % 256 == 0:
            print(stamp(), f"  {i+1}/{nblocks} ({(i+1)*100//nblocks}%)")

    print(stamp(), "all blocks sent, reading final status...")
    err, *_ = await read_status(client)
    if err != 0xFF:
        raise RuntimeError(f"final check failed: {ERRS.get(err, err)} (expected OTA end)")
    print(stamp(), "image verified by device (OTA end). Sending CMD_OTA_END (reboot)...")
    try:
        await client.write_gatt_char(OTA_CHAR, bytes([0x02, 0xFF]), response=False)
    except Exception:
        pass  # device drops the link on reset — expected
    print(stamp(), "=== FLASH COMPLETE — device rebooting into the new image ===")


async def get_connected(bus):
    try:
        reply = await bus.call(Message(destination="org.bluez", path=DEVPATH,
            interface="org.freedesktop.DBus.Properties", member="Get",
            signature="ss", body=["org.bluez.Device1", "Connected"]))
        return reply.body[0].value
    except Exception:
        return False


async def main():
    img = load_image(OTA_BIN)
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    print(stamp(), f"waiting for ACL link to {ADDR} (run: sudo le_conn_ext.py)...")
    deadline = time.monotonic() + 1800
    while time.monotonic() < deadline:
        if await get_connected(bus):
            print(stamp(), "link detected, attaching...")
            try:
                async with BleakClient(ADDR, timeout=30) as client:
                    await flash(client, img)
                return 0
            except Exception as e:
                print(stamp(), f"ERROR: {e!r}")
                print(stamp(), "Aborted safely — running firmware is untouched until final CRC passes.")
                return 1
        await asyncio.sleep(0.2)
    print(stamp(), "timed out waiting for link")
    return 1

sys.exit(asyncio.run(main()))
