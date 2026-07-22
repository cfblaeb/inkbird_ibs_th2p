#!/usr/bin/env python3
"""BTHome TUI monitor — one live-updating row per detected device.

Usage:
    python3 bthome_monitor.py                  # all BTHome devices
    python3 bthome_monitor.py IBSTH2P          # only names containing this
    python3 bthome_monitor.py AA:BB:CC:DD:EE:FF  # only this address

Keys: q quits.

Columns:
    NEW    time since the payload last changed (a fresh packet arrived —
           every ~10 s on V21+, so this doubles as a liveness indicator)
    BUTTON time since the last button-press event (BTHome object 0x3A)

Sensor values (temp/hum/batt/volt) turn red for ~12 s after they change,
so a moving quantity stays red while static ones stay plain. Note the
main MCU only refreshes its measurement every ~10-30 s, so values may
legitimately repeat across a few packets.
"""
import asyncio
import curses
import sys
import time

from bleak import BleakScanner

BTHOME_UUID = "0000fcd2-0000-1000-8000-00805f9b34fb"

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
    0xF2: ("fw_version",     3, False, 1),
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
        if obj == 0xF2:
            # uint24 firmware version: bytes are patch, minor, major (LE)
            out[name] = f"{(raw >> 16) & 0xFF}.{(raw >> 8) & 0xFF}.{raw & 0xFF}"
        else:
            out[name] = round(raw * scale, 3)
    return out


def fmt_age(t_event, now):
    """Compact age like 8s / 4m32s / 1h05m / 2d03h; '-' if never."""
    if t_event is None:
        return "-"
    s = int(now - t_event)
    if s < 0:
        s = 0
    if s < 60:
        return f"{s}s"
    if s < 3600:
        return f"{s // 60}m{s % 60:02d}s"
    if s < 86400:
        return f"{s // 3600}h{(s % 3600) // 60:02d}m"
    return f"{s // 86400}d{(s % 86400) // 3600:02d}h"


# Sensor fields that get the red change highlight, and how long it lasts.
# 12 s is just over one V21 packet interval: a quantity changing every
# packet stays continuously red, a static one never lights up.
SENSOR_KEYS = ("temperature_C", "humidity_%", "battery_%", "voltage_V")
CHANGE_HL_S = 12


class DevState:
    __slots__ = ("addr", "name", "rssi", "payload", "values",
                 "last_new", "last_button", "fw_version", "changed")

    def __init__(self, addr):
        self.addr = addr
        self.name = ""
        self.rssi = 0
        self.payload = None
        self.values = {}
        self.last_new = None
        self.last_button = None
        self.fw_version = None
        self.changed = {}  # sensor key -> monotonic time it last changed


devices = {}   # addr -> DevState
dev_order = []  # first-seen ordering


def make_callback(flt):
    flt_l = flt.lower() if flt else None

    def cb(device, adv):
        sd = adv.service_data.get(BTHOME_UUID)
        if not sd:
            return
        name = adv.local_name or device.name or ""
        if flt_l and flt_l not in name.lower() and flt_l != device.address.lower():
            return
        now = time.monotonic()
        d = devices.get(device.address)
        if d is None:
            d = devices[device.address] = DevState(device.address)
            dev_order.append(device.address)
        d.rssi = adv.rssi
        if name:
            d.name = name
        payload = bytes(sd)
        if payload != d.payload:
            new_values = decode_bthome(payload)
            if d.payload is not None:  # don't flag the very first sighting
                for k in SENSOR_KEYS:
                    if k in new_values and new_values.get(k) != d.values.get(k):
                        d.changed[k] = now
            d.payload = payload
            d.values = new_values
            d.last_new = now
            if "button_event" in d.values:
                d.last_button = now

    return cb


HEADER = (f"{'ADDRESS':<18}{'NAME':<16}{'RSSI':>5} {'PID':>4} {'TEMP°C':>7} "
          f"{'HUM%':>6} {'BATT':>5} {'VOLT':>6} {'FW':>7} {'NEW':>7} "
          f"{'BUTTON':>8}")


def draw(stdscr):
    now = time.monotonic()
    h, w = stdscr.getmaxyx()
    stdscr.erase()
    clock = time.strftime("%H:%M:%S")
    title = f" BTHome monitor — {len(devices)} device(s) — q to quit"
    stdscr.addnstr(0, 0, title.ljust(w - len(clock) - 1) + clock, w - 1,
                   curses.A_BOLD)
    stdscr.addnstr(1, 0, HEADER, w - 1, curses.A_UNDERLINE)
    row = 2
    for addr in dev_order:
        if row >= h:
            break
        d = devices[addr]
        v = d.values
        pid = v.get("packet_id", "-")
        temp = v.get("temperature_C")
        humi = v.get("humidity_%")
        batt = v.get("battery_%")
        volt = v.get("voltage_V")
        # fw_version leaves the advertisement during a button burst; latch
        # the last seen value so the column doesn't flicker to '-'.
        if "fw_version" in v:
            d.fw_version = v["fw_version"]
        # cell text + the sensor key driving its red change-highlight
        cells = (
            (f"{d.addr:<18}", None),
            (f"{d.name[:15]:<16}", None),
            (f"{d.rssi:>5} ", None),
            (f"{pid!s:>4} ", None),
            (f"{f'{temp:.2f}' if temp is not None else '-':>7} ", "temperature_C"),
            (f"{f'{humi:.2f}' if humi is not None else '-':>6} ", "humidity_%"),
            (f"{str(batt) + '%' if batt is not None else '-':>5} ", "battery_%"),
            (f"{f'{volt:.3f}' if volt is not None else '-':>6} ", "voltage_V"),
            (f"{d.fw_version or '-':>7} ", None),
            (f"{fmt_age(d.last_new, now):>7} ", None),
        )
        base = curses.A_NORMAL
        if d.last_new is not None and now - d.last_new < 3:
            base |= curses.A_BOLD | curses.color_pair(1)   # fresh data
        x = 0
        for text, key in cells:
            attr = base
            if (key and key in d.changed
                    and now - d.changed[key] < CHANGE_HL_S):
                attr = curses.color_pair(3) | curses.A_BOLD  # value changed
            if x < w - 1:
                stdscr.addnstr(row, x, text, w - 1 - x, attr)
            x += len(text)
        # button column: timestamp is latched at each press and keeps
        # counting up after the 0x3A object leaves the advertisement;
        # highlighted while recent, dim only if never pressed.
        btn = f"{fmt_age(d.last_button, now):>8}"
        if d.last_button is None:
            battr = curses.A_DIM
        elif now - d.last_button < 120:
            battr = curses.color_pair(2) | curses.A_BOLD
        else:
            battr = curses.A_NORMAL
        if x < w - 1:
            stdscr.addnstr(row, x, btn, w - 1 - x, battr)
        row += 1
    if not devices:
        stdscr.addnstr(3, 2, "scanning...", w - 3, curses.A_DIM)
    stdscr.refresh()


async def run(stdscr, flt):
    curses.curs_set(0)
    stdscr.nodelay(True)
    curses.start_color()
    curses.use_default_colors()
    curses.init_pair(1, curses.COLOR_GREEN, -1)
    curses.init_pair(2, curses.COLOR_YELLOW, -1)
    curses.init_pair(3, curses.COLOR_RED, -1)

    scanner = BleakScanner(detection_callback=make_callback(flt))
    await scanner.start()
    try:
        while True:
            draw(stdscr)
            if stdscr.getch() in (ord("q"), ord("Q")):
                break
            await asyncio.sleep(0.25)
    finally:
        await scanner.stop()


def main():
    flt = sys.argv[1] if len(sys.argv) > 1 else None
    stdscr = curses.initscr()
    try:
        curses.noecho()
        curses.cbreak()
        stdscr.keypad(True)
        asyncio.run(run(stdscr, flt))
    finally:
        stdscr.keypad(False)
        curses.nocbreak()
        curses.echo()
        curses.endwin()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
