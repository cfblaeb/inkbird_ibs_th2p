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
    python3 frame_probe_monitor.py <ADDR> --guided   # step-by-step experiment
    python3 frame_probe_monitor.py <ADDR> --dump     # fetch ring + stats

Live mode: sit and watch. Each received frame prints its delta to the
previous frame; press the device button and see whether a frame with a
flipped BTN byte arrives immediately (option 2 viable) or only at the next
periodic frame (frame-period latency is the floor).

Guided mode runs the whole V21 experiment interactively: phase 1 measures
the steady cadence (just wait), phase 2 prompts three button presses and
classifies each as immediate or cadence-bound, then prints a summary with
answers (a) and (b). SPACE redoes a missed press, 's' skips ahead.

Note: connecting is easiest during the 60 s post-reset fast-advertising
window (pull battery), same as flashing. The on-device ring resets on
every boot, so --dump right after a battery pull only shows the boot
burst — use live/guided mode (which stays connected) to measure the
steady period.
"""
import asyncio
import contextlib
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


class Feed:
    """Live-streamed frames with host arrival timestamps."""

    def __init__(self):
        self.frames = []  # (t_host, cnt, type7, flags8)
        self.new_frame = asyncio.Event()

    def on_notify(self, _h, data: bytearray):
        if len(data) < 12 or data[0] != CMD_ID or data[1] != MARK_LIVE:
            return
        cnt = data[2] | (data[3] << 8)
        self.frames.append((time.monotonic(), cnt, data[8], data[9]))
        self.new_frame.set()


@contextlib.contextmanager
def raw_keys(queue):
    """Feed single keypresses into queue while inside the context (tty only)."""
    if not sys.stdin.isatty():
        yield False
        return
    import termios
    import tty
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    loop = asyncio.get_event_loop()
    tty.setcbreak(fd)
    loop.add_reader(fd, lambda: queue.put_nowait(sys.stdin.read(1)))
    try:
        yield True
    finally:
        loop.remove_reader(fd)
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


async def wait_frame_or_key(feed, keys, timeout, cursor):
    """Return ('frame', feed.frames[cursor]), ('key', ch) or ('timeout', None).

    The caller advances cursor after consuming a frame, so frames arriving
    back-to-back (periodic + immediate press frame) are never skipped.
    """
    if cursor < len(feed.frames):
        return "frame", feed.frames[cursor]
    feed.new_frame.clear()
    waiter = asyncio.ensure_future(feed.new_frame.wait())
    getter = asyncio.ensure_future(keys.get())
    try:
        done, _ = await asyncio.wait(
            [waiter, getter], timeout=timeout,
            return_when=asyncio.FIRST_COMPLETED)
        if getter in done:
            return "key", getter.result()
        if cursor < len(feed.frames):
            return "frame", feed.frames[cursor]
        return "timeout", None
    finally:
        for t in (waiter, getter):
            t.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await t


CADENCE_INTERVALS = 5
PRESSES = 3
IMMEDIATE_S = 3.0  # prompt-to-frame latency below this = immediate push


async def guided(client):
    feed = Feed()
    keys = asyncio.Queue()
    await client.start_notify(CMD_CHAR, feed.on_notify)

    with raw_keys(keys) as tty_ok:
        if not tty_ok:
            print("(stdin is not a tty — SPACE/'s' keys unavailable)")

        # ---- Phase 1: steady cadence -------------------------------------
        print(f"\n=== Phase 1/2: steady frame cadence ===\n"
              f"Leave the device alone — do NOT press the button.\n"
              f"Collecting until {CADENCE_INTERVALS} intervals are seen "
              f"('s' = enough data, move on).\n")
        t_start = time.monotonic()
        cursor = 0
        deltas = []
        last_t = None
        last_note = t_start
        while len(deltas) < CADENCE_INTERVALS:
            kind, val = await wait_frame_or_key(feed, keys, 30, cursor)
            now = time.monotonic()
            if kind == "key" and val in "sS":
                print("  (skipping ahead)")
                break
            if kind == "frame":
                cursor += 1
                t, cnt, type7, flags8 = val
                if last_t is None:
                    print(f"  frame #{cnt}: first frame seen, measuring from here")
                else:
                    deltas.append(t - last_t)
                    print(f"  frame #{cnt}: dt={t - last_t:6.2f} s  "
                          f"({len(deltas)}/{CADENCE_INTERVALS} intervals)")
                last_t = t
                last_note = now
            elif now - last_note > 25:
                print(f"  ... still waiting ({now - t_start:.0f} s elapsed, "
                      f"{len(deltas)} intervals so far)")
                last_note = now
        period = statistics.median(deltas) if deltas else None

        # ---- Phase 2: button presses -------------------------------------
        press_timeout = max(10.0, 1.5 * period + 5) if period else 30.0
        print(f"\n=== Phase 2/2: button press test ({PRESSES} presses) ===\n"
              f"Watching {press_timeout:.0f} s per press for a frame with a "
              f"flipped btn byte.\n")
        results = []  # latency in s, or None = no immediate frame
        press = 1
        while press <= PRESSES:
            base = feed.frames[-1][3] if feed.frames else None
            print(f">>> PRESS THE BUTTON NOW  (press {press}/{PRESSES}; "
                  f"SPACE = missed it, redo)")
            t0 = time.monotonic()
            outcome = None  # 'redo' | latency | None(timeout)
            while outcome is None:
                left = press_timeout - (time.monotonic() - t0)
                if left <= 0:
                    break
                kind, val = await wait_frame_or_key(feed, keys, left, cursor)
                if kind == "key" and val == " ":
                    outcome = "redo"
                elif kind == "frame":
                    cursor += 1
                    t, cnt, type7, flags8 = val
                    if base is None or flags8 != base:
                        outcome = t - t0
                    else:
                        print(f"    (periodic frame #{cnt}, btn byte "
                              f"unchanged — keep waiting)")
            if outcome == "redo":
                print("    redoing this press\n")
                continue
            if outcome is None:
                print(f"    no btn-byte change within {press_timeout:.0f} s "
                      f"-> press NOT pushed immediately\n")
                results.append(None)
            else:
                verdict = ("immediate" if outcome <= IMMEDIATE_S
                           else "only at next periodic frame?")
                print(f"    btn byte flipped {outcome:.2f} s after prompt "
                      f"-> {verdict}\n")
                results.append(outcome)
            press += 1
            await asyncio.sleep(1)

    # ---- Summary ---------------------------------------------------------
    print("=== Summary (record in IBSTH2P_PROJECT_PLAN.md) ===")
    if deltas:
        print(f"(a) steady frame period: min {min(deltas):.2f} / median "
              f"{statistics.median(deltas):.2f} / max {max(deltas):.2f} s "
              f"over {len(deltas)} intervals")
    else:
        print("(a) steady frame period: no intervals captured — rerun and "
              "let phase 1 finish")
    hits = [r for r in results if r is not None and r <= IMMEDIATE_S]
    print(f"(b) button press: {len(hits)}/{len(results)} presses produced a "
          f"btn-byte frame within {IMMEDIATE_S:.0f} s of the prompt")
    if results and len(hits) == len(results):
        print("    -> main MCU pushes presses immediately; wake-on-RX "
              "(V21 option 2) is viable")
    elif results and not hits:
        print("    -> presses only surface on the periodic frame; frame "
              "period is the latency floor (V21 option 1 territory)")
    elif results:
        print("    -> mixed results — rerun to confirm")
    print("\nReminder: reflash the release V20 image when done "
          "(probe build keeps UART RX powered, ~1-2 mA).")


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
    if "--dump" in sys.argv[2:]:
        mode = dump
    elif "--guided" in sys.argv[2:]:
        mode = guided
    else:
        mode = live
    print(f"Connecting to {addr} ...")
    async with BleakClient(addr, timeout=30) as client:
        print("Connected. Software revision should read IBS-X20.")
        await mode(client)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
