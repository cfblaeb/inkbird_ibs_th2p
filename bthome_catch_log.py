#!/usr/bin/env python3
"""Passive 5-minute BTHome capture (decodes 0x09 = V25_P10 window-health %): infer how often each unit's advertised
sensor values refresh (a proxy for main-MCU frame catches) and compare units
side by side. No connection, no flashing. Weaker than ucap_stats.py — the main
MCU only changes its measurement every ~10-30 s, so a healthy unit also repeats
values; judge each suspect AGAINST a healthy neighbour captured in the same run.

Usage:
    python3 bthome_catch_log.py [seconds] [name-or-addr-filter ...]
    python3 bthome_catch_log.py 300 IBSTH2P
Output per device: packets seen, packet-id advance rate, fraction of packets
where temp/humidity changed, longest run of unchanged values (s), and a
timeline of change (#) vs repeat (.) per packet.
"""
import asyncio
import sys
import time
from collections import defaultdict

from bleak import BleakScanner

BTHOME_UUID = "0000fcd2-0000-1000-8000-00805f9b34fb"
OBJ = {0x00: ("pid", 1, False), 0x01: ("batt", 1, False), 0x02: ("temp", 2, True),
       0x03: ("humi", 2, False), 0x09: ("count", 1, False), 0x0C: ("volt", 2, False), 0x3A: ("btn", 1, False),
       0xF2: ("fw", 3, False)}


def decode(data: bytes) -> dict:
    out, i = {}, 1
    while i < len(data):
        oid = data[i]
        if oid not in OBJ:
            break
        name, ln, signed = OBJ[oid]
        out[name] = int.from_bytes(data[i+1:i+1+ln], "little", signed=signed)
        i += 1 + ln
    return out


async def main():
    secs = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 300
    flt = [a.upper() for a in sys.argv[2:]] if len(sys.argv) > 2 else []
    log = defaultdict(list)   # addr -> [(t, pid, temp, humi)]
    names = {}
    last_pid = {}

    def cb(dev, adv):
        sd = adv.service_data.get(BTHOME_UUID)
        if not sd:
            return
        name = adv.local_name or dev.name or ""
        key = dev.address.upper()
        if flt and not any(f in key or f in name.upper() for f in flt):
            return
        d = decode(bytes(sd))
        if "pid" not in d:
            return
        if last_pid.get(key) == d["pid"]:
            return  # same advertisement repeated by the scanner
        last_pid[key] = d["pid"]
        names[key] = name
        log[key].append((time.time(), d["pid"], d.get("temp"), d.get("humi")))

    scanner = BleakScanner(cb, scanning_mode="active")
    await scanner.start()
    t0 = time.time()
    print(f"capturing {secs} s ...")
    while time.time() - t0 < secs:
        await asyncio.sleep(5)
        sys.stdout.write(f"\r  {int(time.time()-t0):3d}s  devices: {len(log)}   ")
        sys.stdout.flush()
    await scanner.stop()
    print("\n")
    print(f"{'device':<34}{'pkts':>5}{'pid/min':>8}{'changed':>9}{'maxrun s':>9}  timeline (#=value changed, .=repeat)")
    for key, rows in sorted(log.items(), key=lambda kv: names.get(kv[0], "")):
        if len(rows) < 3:
            continue
        span = rows[-1][0] - rows[0][0]
        pid_adv = sum(((b[1] - a[1]) & 0xFF) for a, b in zip(rows, rows[1:]))
        changes, tl, run, maxrun, run_t0 = 0, [], 0, 0.0, rows[0][0]
        for a, b in zip(rows, rows[1:]):
            ch = (a[2], a[3]) != (b[2], b[3])
            tl.append("#" if ch else ".")
            if ch:
                changes += 1
                maxrun = max(maxrun, b[0] - run_t0)
                run_t0 = b[0]
        maxrun = max(maxrun, rows[-1][0] - run_t0)
        label = f"{names.get(key,'')[:16]} {key[-8:]}"
        print(f"{label:<34}{len(rows):>5}{60*pid_adv/max(span,1):>8.1f}"
              f"{100*changes/max(len(rows)-1,1):>8.0f}%{maxrun:>9.0f}  {''.join(tl)}")
    print("\nInterpretation: a mis-locked unit (Mode A) refreshes values in a slow regular")
    print("rhythm (every 2nd/3rd packet at best) with long unchanged runs vs its neighbour;")
    print("corruption (Mode B) shows irregular long runs. Healthy neighbours set the baseline.")


if __name__ == "__main__":
    asyncio.run(main())
