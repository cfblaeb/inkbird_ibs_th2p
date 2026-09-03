#!/usr/bin/env python3
"""Read the IBSTH2P UART-capture counters over BLE and estimate the wake-on-RX
frame catch rate — a ~2 minute wireless test for the scheduler mis-lock /
CRC-corruption drain hypothesis (see freezer_battery_data/
firmware_drain_review_2026-09-02.md).

Usage:
    python3 ucap_stats.py 38:1F:8D:17:9B:AF          # Thermo ES
    python3 ucap_stats.py 38:1F:8D:DA:BA:20          # 204-FR2
    python3 ucap_stats.py <MAC> --p10                # also read V25_P10 scheduler counters (GATT ops 5-8)

Connecting: the units advertise every 10 s, which the Linux kernel's 4 s
create-connection limit cannot catch. Either (a) run inside the 60 s fast
window after a battery reinsert (1.5 s adv), or (b) first run
`sudo python3 ble_le_conn_ext.py` (edit ADDR) so BlueZ adopts a link, then run
this script immediately. This script retries bleak connects for up to 90 s.

Reads (GATT char 0xFFF4, same channel frame_probe_monitor.py uses):
    CMD_ID_I2C_SCAN op 0 -> uart_inited, sensor_valid, total_bytes(u32),
                            good_frames(u16), crc_bad(u16), last temp/humi
    CMD_ID_UTC_TIME      -> seconds since boot (clock is zeroed at reset and
                            nothing sets it in the field)
Verdict: expected frames = uptime / 10.39 s. Catch = good/expected.
    catch ~100 %, crc_bad ~0      -> scheduler locked, drain is NOT this
    catch 30-55 %, crc_bad ~0     -> Mode A (period outside ~9.9-10.9 s)
    crc_bad >~10 % of frames seen -> Mode B (corrupt frames)
Caveat: good_frames/crc_bad are uint16 (wrap after ~7.9 days at 100 % catch);
read within the first minute of the connection (the connected 10 s grab loop
inflates good_frames by ~6/min).
"""
import asyncio
import sys
import time

from bleak import BleakClient

CMD_CHAR = "0000fff4-0000-1000-8000-00805f9b34fb"
CMD_I2C_SCAN = 0x03
CMD_UTC_TIME = 0x23
FRAME_PERIOD_S = 10.39


async def query(client, payload, want_id, timeout=5.0):
    fut = asyncio.get_event_loop().create_future()

    def on_notify(_h, data: bytearray):
        if data and data[0] == want_id and not fut.done():
            fut.set_result(bytes(data))

    await client.start_notify(CMD_CHAR, on_notify)
    try:
        await client.write_gatt_char(CMD_CHAR, bytes(payload), response=False)
        return await asyncio.wait_for(fut, timeout)
    finally:
        try:
            await client.stop_notify(CMD_CHAR)
        except Exception:
            pass


def u16(b, i): return b[i] | (b[i+1] << 8)
def u32(b, i): return b[i] | (b[i+1] << 8) | (b[i+2] << 16) | (b[i+3] << 24)

P10_STATES = {0: "WAIT_EDGE", 1: "VERIFY", 2: "WAIT_OPEN", 3: "WINDOW", 4: "SUSPENDED"}
P10_TSRC = {0: "seed", 1: "hit", 2: "edge k=1", 3: "edge k=2"}


async def read_p10(client, uptime):
    """V25_P10 telemetry: CMD_ID_I2C_SCAN ops 5-8 (layouts per SPEC_V25_P10.md 8.2)."""
    r5 = await query(client, [CMD_I2C_SCAN, 5], CMD_I2C_SCAN)
    r6 = await query(client, [CMD_I2C_SCAN, 6], CMD_I2C_SCAN)
    r7 = await query(client, [CMD_I2C_SCAN, 7], CMD_I2C_SCAN)
    r8 = await query(client, [CMD_I2C_SCAN, 8], CMD_I2C_SCAN)
    if r5[1] != 0x5A or r6[1] != 0x5B or r7[1] != 0x5C or r8[1] != 0x5D:
        print("P10 ops not recognised (not a V25_P10 image?):", r5[:2].hex(), r6[:2].hex(), r7[:2].hex(), r8[:2].hex())
        return
    st, tsrc, t_est = r5[2], r5[3], u16(r5, 4)
    edges, glitches, windows, hits, misses, strays = (u16(r5, 6), u16(r5, 8), u16(r5, 10), u16(r5, 12), u16(r5, 14), u16(r5, 16))
    health, miss_streak = r5[18], r5[19]
    hits_bad, stale, aborted = u16(r6, 2), u16(r6, 4), u16(r6, 6)
    connects, resumes, recovers, bad_sleep, bad_wake, uart_fail, gpio_fail = r6[8], r6[9], r6[10], r6[11], r6[12], r6[13], r6[14]
    wakes_io, last_hit_pos, guard = u16(r6, 15), u16(r6, 17), r6[19] * 10
    irq_total, wake_src_raw, hist, hist_n = u32(r7, 2), u32(r7, 6), u32(r7, 10), r7[14]
    last_dt, rc_ct, sanity_rec = u16(r7, 15), u16(r7, 17), r7[19]
    frame_oos, partial_close, anchor_valid = u16(r8, 2), u16(r8, 4), r8[14]
    print()
    print("=== V25_P10 scheduler ===")
    print(f"state {P10_STATES.get(st, st)}   T_est {t_est} ms ({P10_TSRC.get(tsrc, tsrc)})   guard {guard} ms   miss_streak {miss_streak}")
    print(f"edges {edges}  glitches {glitches}  strays {strays}  windows {windows}  hits {hits} (CRC-bad {hits_bad})  misses {misses}")
    print(f"health {health} % over last {hist_n} windows (hist 0x{hist:08x})   last_hit_pos {last_hit_pos} ms (ideal ~ guard+18)   last_dt {last_dt} ms")
    print(f"wakes_io {wakes_io}  irq_total {irq_total}  wake_src_raw 0x{wake_src_raw:08x}  rc_ct {rc_ct} (nominal 7812; 7421..8203 = corrected)")
    print(f"diag: stale {stale} frame_oos {frame_oos} partial_at_close {partial_close} win_aborted {aborted} connects {connects} resumes {resumes} "
          f"recovers {recovers} sanity_recovers {sanity_rec} bad_sleep {bad_sleep} bad_wake {bad_wake} uart_fail {uart_fail} gpio_fail {gpio_fail} anchor_valid {anchor_valid}")
    if uptime and t_est:
        exp_edges = uptime / (2 * t_est / 1000.0)
        print(f"expected edges at uptime {uptime} s: ~{exp_edges:.0f} (one per 2 periods); observed {edges}")
    if windows:
        print(f"HIT RATIO {100*hits/windows:.0f} %   GLITCH RATIO {100*glitches/max(edges+glitches,1):.0f} % of bursts")
    bad = bad_sleep or bad_wake or uart_fail or gpio_fail or recovers or sanity_rec
    print("VERDICT:", "diag counters non-zero -> read the plan's acceptance section" if bad else
          ("healthy P10 operation" if windows and hits / windows >= 0.9 else "low hit ratio -> check T_est/guard/rc_ct/last_hit_pos"))


async def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    addr = sys.argv[1]
    deadline = time.time() + 90
    client = None
    while time.time() < deadline:
        try:
            c = BleakClient(addr, timeout=20.0)
            await c.connect()
            client = c
            break
        except Exception as e:  # noqa: BLE001
            print(f"connect attempt failed ({e.__class__.__name__}); retrying ...")
            await asyncio.sleep(1)
    if client is None:
        print("could not connect within 90 s — use the fast window or ble_le_conn_ext.py")
        sys.exit(2)
    print(f"connected to {addr}")
    try:
        utc = await query(client, [CMD_UTC_TIME], CMD_UTC_TIME)
        uptime = int.from_bytes(utc[1:5], "little")
        st = await query(client, [CMD_I2C_SCAN, 0], CMD_I2C_SCAN)
        if len(st) < 16 or st[1] != 0x0C:
            print("unexpected stats reply:", st.hex())
            sys.exit(3)
        uart_inited, sensor_valid = st[2], st[3]
        total_bytes = int.from_bytes(st[4:8], "little")
        good = int.from_bytes(st[8:10], "little")
        crc_bad = int.from_bytes(st[10:12], "little")
        temp = int.from_bytes(st[12:14], "little", signed=True) / 100
        humi = int.from_bytes(st[14:16], "little") / 100
        p10 = "--p10" in sys.argv[2:]
        if p10:
            await read_p10(client, uptime)
    finally:
        await client.disconnect()

    expected = uptime / FRAME_PERIOD_S if uptime else 0
    seen = good + crc_bad
    print()
    print(f"uptime since boot   : {uptime} s ({uptime/3600:.1f} h)")
    print(f"uart_inited/sensor_valid: {uart_inited}/{sensor_valid}")
    print(f"total UART bytes    : {total_bytes}  (~{total_bytes/13:.0f} frame-equivalents)")
    print(f"good frames         : {good}")
    print(f"CRC-bad buffers     : {crc_bad}")
    print(f"last temp/humi      : {temp:.2f} C / {humi:.2f} %")
    if expected:
        print(f"expected frames     : {expected:.0f}  (uptime / {FRAME_PERIOD_S} s)")
        print(f"CATCH RATE          : {100*good/expected:.0f} % of expected frames")
    if seen:
        print(f"CORRUPTION          : {100*crc_bad/seen:.0f} % of frames seen in windows")
    print()
    if uptime > 7.5 * 86400:
        print("NOTE: uptime > 7.5 d — uint16 good_frames may have wrapped; catch rate unreliable.")
    if expected and good / expected > 0.85 and (not seen or crc_bad / seen < 0.05):
        print("VERDICT: scheduler is locked and frames are clean -> drain is NOT the wake-on-RX mechanism.")
    elif seen and crc_bad / seen >= 0.10:
        print("VERDICT: Mode B — significant CRC corruption on the inter-chip UART.")
    elif expected and good / expected < 0.6:
        print("VERDICT: Mode A — low catch rate with clean frames: frame period outside the 9.9-10.9 s lock range.")
    else:
        print("VERDICT: inconclusive — see numbers above.")


if __name__ == "__main__":
    asyncio.run(main())
