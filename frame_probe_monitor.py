#!/usr/bin/env python3
"""Frame-period probe client for the IBS-X20 experimental build.

Answers the two V21 questions from IBSTH2P_PROJECT_PLAN.md:
  (a) the main MCU's real inter-frame period, and
  (b) whether a button press emits an immediate extra frame.

The probe firmware (branch v21-frame-probe, revision "IBS-X20") keeps the
inter-chip UART permanently open, timestamps every valid frame (24-bit RTC
@32768 Hz), keeps the last 64 in a ring, and — while a client has
notifications enabled on the CMD characteristic 0xFFF4 — streams one
notification per frame.

Usage:
    python3 frame_probe_monitor.py <ADDR>            # live stream (default)
    python3 frame_probe_monitor.py <ADDR> --dump     # fetch ring + stats

Live mode: sit and watch. Each received frame prints its delta to the
previous frame; press the device button and see whether a frame with a
flipped BTN byte arrives immediately (option 2 viable) or only at the next
periodic frame (frame-period latency is the floor).

Note: connecting is easiest during the 60 s post-reset fast-advertising
window (pull battery), same as flashing.
"""
import asyncio
import statistics
import sys
import time

from bleak import BleakClient

U = lambda s: f"0000{s}-0000-1000-8000-00805f9b34fb"
CMD_CHAR = U("fff4")
CMD_ID = 0x03          # CMD_ID_I2C_SCAN, repurposed as IBSTH2P debug channel
MARK_DUMP = 0x50
MARK_LIVE = 0x51
RTC_HZ = 32768
RTC_MASK = 0xFFFFFF    # 24-bit counter, wraps every 512 s


def ticks_to_s(dt):
    return (dt & RTC_MASK) / RTC_HZ


class Live:
    def __init__(self):
        self.last_flags8 = None
        self.t0 = time.monotonic()

    def on_notify(self, _h, data: bytearray):
        if len(data) < 12 or data[0] != CMD_ID or data[1] != MARK_LIVE:
            return
        cnt = data[2] | (data[3] << 8)
        tik = data[4] | (data[5] << 8) | (data[6] << 16)
        type7 = data[8]
        flags8 = data[9]
        dt_ms = data[10] | (data[11] << 8)
        dt = "   ?  " if dt_ms == 0xFFFF else f"{dt_ms/1000:6.2f}"
        btn = ""
        if self.last_flags8 is not None and flags8 != self.last_flags8:
            btn = "  <-- BTN byte changed!"
        self.last_flags8 = flags8
        print(f"[{time.monotonic()-self.t0:8.2f}s] frame #{cnt:<5d} "
              f"dt={dt}s  type={type7}  btn_byte=0x{flags8:02x}{btn}")


async def live(client):
    lv = Live()
    await client.start_notify(CMD_CHAR, lv.on_notify)
    print("Streaming frames — press the device button to test (b). Ctrl-C to stop.")
    while True:
        await asyncio.sleep(1)


async def dump(client):
    got = {}
    done = asyncio.Event()
    total = 0

    def on_notify(_h, data: bytearray):
        nonlocal total
        if len(data) < 6 or data[0] != CMD_ID or data[1] != MARK_DUMP:
            return
        total = data[2] | (data[3] << 8)
        idx = data[4] | (data[5] << 8)
        p = 6
        while p + 6 <= len(data):
            tik = data[p] | (data[p+1] << 8) | (data[p+2] << 16)
            got[idx] = (tik, data[p+4], data[p+5])  # tik, type7, flags8
            idx += 1
            p += 6
        done.set()

    await client.start_notify(CMD_CHAR, on_notify)
    # first request tells us the total count
    done.clear()
    await client.write_gatt_char(CMD_CHAR, bytes([CMD_ID, 4, 0, 0]), response=False)
    await asyncio.wait_for(done.wait(), 5)
    first = max(0, total - 64)
    idx = first
    while idx < total:
        if idx not in got:
            done.clear()
            await client.write_gatt_char(
                CMD_CHAR, bytes([CMD_ID, 4, idx & 0xFF, idx >> 8]), response=False)
            await asyncio.wait_for(done.wait(), 5)
        idx += 2

    recs = [(i, *got[i]) for i in sorted(got) if first <= i < total]
    print(f"\ntotal frames since boot: {total}; showing {len(recs)} "
          f"(#{first}..#{total-1})\n")
    print(f"{'frame':>6} {'dt (s)':>8} {'type':>5} {'btn':>5}")
    deltas = []
    prev = None
    prev_flags = None
    for i, tik, type7, flags8 in recs:
        mark = ""
        if prev_flags is not None and flags8 != prev_flags:
            mark = "  <-- BTN byte changed"
        prev_flags = flags8
        if prev is None:
            print(f"{i:>6} {'-':>8} {type7:>5} {flags8:#05x}{mark}")
        else:
            d = ticks_to_s(tik - prev)
            deltas.append(d)
            print(f"{i:>6} {d:>8.3f} {type7:>5} {flags8:#05x}{mark}")
        prev = tik
    if deltas:
        print(f"\nframe period: min {min(deltas):.3f} s, "
              f"median {statistics.median(deltas):.3f} s, "
              f"max {max(deltas):.3f} s over {len(deltas)} intervals")
        print("(a 'max' outlier well above the median usually brackets a "
              "24-bit RTC wrap, 512 s — ignore single outliers)")


async def main():
    if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
        print(__doc__)
        sys.exit(1)
    addr = sys.argv[1]
    mode = dump if "--dump" in sys.argv[2:] else live
    print(f"Connecting to {addr} ...")
    async with BleakClient(addr, timeout=30) as client:
        print("Connected. Software revision should read IBS-X20.")
        await mode(client)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
