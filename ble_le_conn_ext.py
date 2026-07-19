#!/usr/bin/env python3
"""Establish an LE connection to a slow advertiser (10 s interval) that the
kernel's fixed 4 s create-connection timeout can never reach.

Sends LE Extended Create Connection (0x2043) on a raw HCI socket with a
continuous initiator scan window and waits up to 40 s for the connection
complete event. The kernel sees the same HCI events and creates its own
connection object, so BlueZ adopts the link and normal GATT tools work on it.

Run with sudo:  sudo python3 le_conn_ext.py
"""
import socket
import struct
import sys
import time

ADDR = "38:1F:8D:CF:77:6F"
TIMEOUT_S = 40

OGF_LE = 0x08
OCF_EXT_CREATE_CONN = 0x0043
OCF_CREATE_CONN_CANCEL = 0x000E
OCF_SET_EXT_SCAN_ENABLE = 0x0042

def opcode(ogf, ocf):
    return (ogf << 10) | ocf

def cmd_pkt(op, params=b""):
    return struct.pack("<BHB", 0x01, op, len(params)) + params

def main():
    try:
        s = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_RAW, socket.BTPROTO_HCI)
        s.bind((0,))
    except PermissionError:
        print("Needs root: sudo python3 le_conn_ext.py")
        return 1

    # HCI filter: all events
    flt = struct.pack("<LLLH2x", 1 << 0x04, 0xFFFFFFFF, 0xFFFFFFFF, 0)
    s.setsockopt(0, 2, flt)  # SOL_HCI, HCI_FILTER

    peer = bytes.fromhex(ADDR.replace(":", ""))[::-1]  # little-endian
    params = struct.pack("<BBB6sB", 0x00, 0x00, 0x00, peer, 0x01)  # filter/own/peer type/addr/PHY=1M
    params += struct.pack("<8H",
        0x0800, 0x0800,   # scan interval = window = 1.28 s -> continuous
        0x0018, 0x0028,   # conn interval 30-50 ms
        0x0000,           # latency
        0x002A,           # supervision timeout 420 ms
        0x0000, 0x0000)   # CE length min/max

    def send(op, p=b""):
        s.send(cmd_pkt(op, p))

    def wait_event(matcher, timeout):
        end = time.time() + timeout
        while time.time() < end:
            s.settimeout(max(0.1, end - time.time()))
            try:
                pkt = s.recv(300)
            except socket.timeout:
                break
            if len(pkt) < 3 or pkt[0] != 0x04:
                continue
            evt, plen = pkt[1], pkt[2]
            body = pkt[3:3 + plen]
            r = matcher(evt, body)
            if r is not None:
                return r
        return None

    print(f"Issuing LE Extended Create Connection to {ADDR} "
          f"(continuous initiator, {TIMEOUT_S} s budget)...")
    send(opcode(OGF_LE, OCF_EXT_CREATE_CONN), params)

    def match_status(evt, body):
        # Command Status for our opcode
        if evt == 0x0F and len(body) >= 4:
            st, _ncmd, op = struct.unpack("<BBH", body[:4])
            if op == opcode(OGF_LE, OCF_EXT_CREATE_CONN):
                return st
        return None

    st = wait_event(match_status, 3)
    if st is None:
        print("No Command Status received?!")
    elif st == 0x0C:
        print("Controller says Command Disallowed — a scan/initiator is already active.")
        print("Retrying once after disabling extended scan...")
        send(opcode(OGF_LE, OCF_SET_EXT_SCAN_ENABLE), struct.pack("<BBHH", 0, 0, 0, 0))
        time.sleep(0.3)
        send(opcode(OGF_LE, OCF_EXT_CREATE_CONN), params)
        st = wait_event(match_status, 3)
    if st not in (0x00, None):
        print(f"Create Connection rejected, HCI status 0x{st:02X}")
        return 1

    def match_conn_complete(evt, body):
        if evt == 0x3E and len(body) >= 1 and body[0] in (0x01, 0x0A):
            status = body[1]
            handle = struct.unpack("<H", body[2:4])[0]
            return (status, handle)
        return None

    print("Waiting for the device's next advertisement (up to 10 s away)...")
    res = wait_event(match_conn_complete, TIMEOUT_S)
    if res is None:
        print("No connection within budget — cancelling initiator.")
        send(opcode(OGF_LE, OCF_CREATE_CONN_CANCEL))
        time.sleep(0.5)
        return 1
    status, handle = res
    if status == 0x00:
        print(f"SUCCESS: connected, handle {handle}. BlueZ now owns the link.")
        return 0
    print(f"Connection complete with error status 0x{status:02X}")
    return 1

sys.exit(main())
