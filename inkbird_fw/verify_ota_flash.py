#!/usr/bin/env python3
"""
Verify stock-OTA staged flash contents against the current STAGE1 image.

Usage:
    # After the stock OTA attempt you want to diagnose, dump the boot-info sector
    # and, optionally, the staged app-bank area before any erase or reflash.
        python3 flash_pogo.py -p /dev/ttyUSB0 rc 0x11003000 0x100 dump_bootsect.bin
        python3 flash_pogo.py -p /dev/ttyUSB0 rc 0x11011000 0xC000 dump_appbank.bin

    # Equivalent reads with rdwr_phy62x2.py also work.
        python3 rdwr_phy62x2.py -p /dev/ttyUSB0 rc 0x11003000 0x100 dump_bootsect.bin
        python3 rdwr_phy62x2.py -p /dev/ttyUSB0 rc 0x11011000 0xC000 dump_appbank.bin

    # Also dump XIP flash region to verify XIP partition data:
        python3 flash_pogo.py -p /dev/ttyUSB0 rc 0x11020000 0xC000 dump_xip.bin

  # Verify against the current exported stage1 image.
  python3 verify_ota_flash.py dump_bootsect.bin --appbank dump_appbank.bin

  # Verify including XIP flash data:
  python3 verify_ota_flash.py dump_bootsect.bin --appbank dump_appbank.bin --xip dump_xip.bin

  # Or verify against a different Intel HEX image.
  python3 verify_ota_flash.py dump_bootsect.bin --appbank dump_appbank.bin --hex STAGE1.hex16
"""

from __future__ import annotations

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


DEFAULT_HEX = Path(__file__).with_name("STAGE1_IBSTH2P_stock_stage1_mixed.hex16")
BOOTINFO_BUS_ADDR = 0x11003000
OTA_APP_BANK_BUS_ADDR = 0x11011000
OTA_MAX_PART_SIZE = 16 * 1024
EXPECTED_BANK_MODE = 0
STOCK_OTA_SRAM_GAP = 8


def is_xip_addr(addr: int) -> bool:
    return 0x11000000 < addr < 0x11100000


def crc16(data: bytes) -> int:
    crc = 0
    for value in data:
        crc ^= value
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


@dataclass
class HexPartition:
    run_addr: int
    data: bytes


@dataclass
class OtaPartition:
    index: int
    flash_off: int
    run_addr: int
    data: bytes

    @property
    def size(self) -> int:
        return len(self.data)

    @property
    def checksum(self) -> int:
        return crc16(self.data)


@dataclass
class AppBankVerificationResult:
    ok: bool
    complete: bool
    required_size: int


def parse_hex16(path: Path) -> list[HexPartition]:
    parts: list[HexPartition] = []
    high_addr = 0
    current_addr: int | None = None
    current = bytearray()

    def flush() -> None:
        nonlocal current_addr, current
        if current_addr is not None and current:
            parts.append(HexPartition(run_addr=current_addr, data=bytes(current)))
        current_addr = None
        current = bytearray()

    for line in path.read_text().splitlines():
        line = line.strip()
        if not line.startswith(":"):
            continue
        count = int(line[1:3], 16)
        addr = int(line[3:7], 16)
        record_type = int(line[7:9], 16)
        data = bytes.fromhex(line[9:9 + count * 2])

        if record_type == 0x04:
            flush()
            high_addr = int.from_bytes(data, "big") << 16
        elif record_type == 0x00:
            full_addr = high_addr + addr
            if current_addr is None:
                current_addr = full_addr
            elif full_addr != current_addr + len(current):
                flush()
                current_addr = full_addr
            current.extend(data)
        elif record_type in (0x01, 0x05):
            break

    flush()
    return parts


def split_for_stock_ota(parts: list[HexPartition]) -> list[OtaPartition]:
    ota_parts: list[OtaPartition] = []
    flash_off = 0
    index = 0

    for part in parts:
        offset = 0
        while offset < len(part.data):
            raw_chunk = part.data[offset:offset + OTA_MAX_PART_SIZE]
            pad_len = (-len(raw_chunk)) % 4
            chunk = raw_chunk + (b"\xFF" * pad_len)
            run_addr = part.run_addr + offset
            part_flash_off = run_addr if is_xip_addr(run_addr) else flash_off
            ota_parts.append(
                OtaPartition(
                    index=index,
                    flash_off=part_flash_off,
                    run_addr=run_addr,
                    data=chunk,
                )
            )
            if not is_xip_addr(run_addr):
                flash_off += len(chunk) + STOCK_OTA_SRAM_GAP
            index += 1
            offset += len(raw_chunk)

    return ota_parts


# Legacy staged-bank patch point used by earlier APP-bridge experiments.
# Direct runtime-address disassembly now shows stock otaProtocol_BootMode()
# branches to runtime 0x1FFFF4E0 instead, so bank + 0x20BC should be treated as
# historical compatibility logic rather than the current boot-flow model.
# The mixed stage1 path preserves the stock helper bytes there, so the verifier
# must not unconditionally overwrite them when reconstructing the expected
# staged bank image.
TRAMPOLINE_BANK_OFFSET = 0x20BC
TRAMPOLINE_BYTES = bytes([0x00, 0x48, 0x00, 0x47, 0xC5, 0xA2, 0x00, 0x11])
STOCK_RUNAPP_BYTES = bytes([0xC0, 0x0B, 0x08, 0x22, 0x91, 0x43, 0xE1, 0x61])


def apply_trampoline(parts: list[OtaPartition]) -> list[OtaPartition]:
    """Patch only legacy staged-bank images that do not already preserve stock bytes."""
    result: list[OtaPartition] = []
    bank_off = 0
    for p in parts:
        if is_xip_addr(p.run_addr):
            result.append(p)
            continue
        bank_end = bank_off + p.size
        if bank_off <= TRAMPOLINE_BANK_OFFSET < bank_end and TRAMPOLINE_BANK_OFFSET + len(TRAMPOLINE_BYTES) <= bank_end:
            local = TRAMPOLINE_BANK_OFFSET - bank_off
            current = p.data[local:local + len(TRAMPOLINE_BYTES)]
            if current == STOCK_RUNAPP_BYTES or current == TRAMPOLINE_BYTES:
                result.append(p)
            else:
                patched = bytearray(p.data)
                patched[local:local + len(TRAMPOLINE_BYTES)] = TRAMPOLINE_BYTES
                result.append(OtaPartition(
                    index=p.index, flash_off=p.flash_off, run_addr=p.run_addr, data=bytes(patched)
                ))
        else:
            result.append(p)
        bank_off += p.size + STOCK_OTA_SRAM_GAP
    return result


def read_boot_entries(data: bytes) -> tuple[int, int, list[tuple[int, int, int, int]]]:
    if len(data) < 8:
        raise ValueError("boot sector dump is too small")

    part_count = struct.unpack_from("<I", data, 0)[0]
    bank_mode = struct.unpack_from("<I", data, 4)[0]
    entries = []
    for index in range(part_count):
        offset = 16 * (index + 1)
        if offset + 16 > len(data):
            break
        entries.append(struct.unpack_from("<4I", data, offset))
    return part_count, bank_mode, entries


def verify_boot_sector(boot_data: bytes, expected_parts: list[OtaPartition]) -> bool:
    ok = True
    part_count, bank_mode, entries = read_boot_entries(boot_data)

    print(f"=== Boot sector ({len(boot_data)} bytes) ===")
    print(f"Partition count: {part_count}")
    print(f"Bank mode: {bank_mode} ({'SINGLE' if bank_mode == 0 else 'OTHER'})")

    if part_count == 0xFFFFFFFF:
        print("ERROR: Boot sector is erased (all 0xFF) - no staged app metadata.")
        return False

    if bank_mode != EXPECTED_BANK_MODE:
        print(f"ERROR: Expected bank mode {EXPECTED_BANK_MODE}, got {bank_mode}.")
        ok = False

    if part_count != len(expected_parts):
        print(f"ERROR: Expected {len(expected_parts)} partitions, got {part_count}.")
        ok = False

    print("\n=== Partition entries ===")
    for expected in expected_parts:
        actual = entries[expected.index] if expected.index < len(entries) else None
        print(
            f"Part {expected.index}: expected flash_off=0x{expected.flash_off:08X} "
            f"run=0x{expected.run_addr:08X} size=0x{expected.size:X} crc=0x{expected.checksum:04X}"
        )
        if actual is None:
            print("  MISSING entry in dump")
            ok = False
            continue
        flash_off, run_addr, size, checksum = actual
        print(
            f"  actual   flash_off=0x{flash_off:08X} run=0x{run_addr:08X} "
            f"size=0x{size:X} crc=0x{checksum:04X}"
        )
        if (flash_off, run_addr, size, checksum) != (
            expected.flash_off,
            expected.run_addr,
            expected.size,
            expected.checksum,
        ):
            print("  MISMATCH")
            ok = False
        else:
            print("  MATCH")

    return ok


def verify_appbank(appbank: bytes, expected_parts: list[OtaPartition]) -> AppBankVerificationResult:
    ok = True
    complete = True
    print(f"\n=== App bank staging dump ({len(appbank)} bytes from 0x{OTA_APP_BANK_BUS_ADDR:08X}) ===")
    staged_parts = [part for part in expected_parts if not is_xip_addr(part.run_addr)]
    required = max(part.flash_off + part.size for part in staged_parts)
    print(f"Required bytes to cover all staged partitions: 0x{required:X}")

    if len(appbank) < required:
        print(
            "ERROR: App-bank dump is shorter than the staged image range "
            f"(need 0x{required:X}, have 0x{len(appbank):X})."
        )
        ok = False
        complete = False

    for expected in staged_parts:
        start = expected.flash_off
        end = start + expected.size
        if end > len(appbank):
            print(f"Part {expected.index}: missing data in dump (need 0x{end:X}, have 0x{len(appbank):X})")
            ok = False
            continue

        actual = appbank[start:end]
        actual_crc = crc16(actual)
        print(
            f"Part {expected.index}: file_off=0x{start:05X} size=0x{expected.size:X} "
            f"crc=0x{actual_crc:04X} expected=0x{expected.checksum:04X}"
        )
        if actual != expected.data:
            mismatch_at = next(i for i, (a, b) in enumerate(zip(actual, expected.data)) if a != b)
            print(
                f"  DATA MISMATCH at +0x{mismatch_at:X}: got 0x{actual[mismatch_at]:02X}, "
                f"expected 0x{expected.data[mismatch_at]:02X}"
            )
            ok = False
        else:
            print("  MATCH")

    return AppBankVerificationResult(ok=ok, complete=complete, required_size=required)


def print_expected_layout(expected_parts: list[OtaPartition]) -> None:
    print("=== Expected stock OTA layout for current image ===")
    for part in expected_parts:
        print(
            f"Part {part.index}: flash_off=0x{part.flash_off:08X} "
            f"run=0x{part.run_addr:08X} size=0x{part.size:X} crc=0x{part.checksum:04X}"
        )
    staged_parts = [part for part in expected_parts if not is_xip_addr(part.run_addr)]
    if staged_parts:
        required = max(part.flash_off + part.size for part in staged_parts)
        print(f"Staged bytes required in app bank: 0x{required:X}")
    xip_parts = [part for part in expected_parts if is_xip_addr(part.run_addr)]
    if xip_parts:
        xip_start = min(p.run_addr for p in xip_parts)
        xip_end = max(p.run_addr + p.size for p in xip_parts)
        print(f"XIP flash range: 0x{xip_start:08X}-0x{xip_end:08X} ({xip_end - xip_start} bytes)")


XIP_FLASH_BASE = 0x11020000


def verify_xip(xip_data: bytes, xip_base: int, expected_parts: list[OtaPartition]) -> bool:
    ok = True
    xip_parts = [part for part in expected_parts if is_xip_addr(part.run_addr)]
    if not xip_parts:
        print("\n=== XIP verification: no XIP partitions in image ===")
        return True

    print(f"\n=== XIP flash dump ({len(xip_data)} bytes from 0x{xip_base:08X}) ===")

    for expected in xip_parts:
        offset = expected.run_addr - xip_base
        end_offset = offset + expected.size
        if offset < 0 or end_offset > len(xip_data):
            print(
                f"Part {expected.index}: XIP data at 0x{expected.run_addr:08X} not covered by dump "
                f"(need offset 0x{max(0,offset):X}-0x{end_offset:X}, dump is {len(xip_data)} bytes)"
            )
            ok = False
            continue

        actual = xip_data[offset:end_offset]
        actual_crc = crc16(actual)
        match = actual == expected.data
        print(
            f"Part {expected.index}: addr=0x{expected.run_addr:08X} size=0x{expected.size:X} "
            f"crc_flash=0x{actual_crc:04X} crc_expected=0x{expected.checksum:04X} "
            f"{'MATCH' if match else 'MISMATCH'}"
        )
        if not match:
            if actual_crc != expected.checksum:
                print(f"  CRC MISMATCH — flash data does not match image. "
                      f"Boot CRC check WILL FAIL → PPlusOTA!")
            mismatch_at = next(
                (i for i, (a, b) in enumerate(zip(actual, expected.data)) if a != b),
                None,
            )
            if mismatch_at is not None:
                print(
                    f"  First byte diff at +0x{mismatch_at:X} (0x{expected.run_addr + mismatch_at:08X}): "
                    f"flash=0x{actual[mismatch_at]:02X} expected=0x{expected.data[mismatch_at]:02X}"
                )
            ok = False

    return ok


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "bootsect",
        help="dump of chip bus address 0x11003000, e.g. dump_bootsect.bin",
    )
    parser.add_argument(
        "--appbank",
        help="dump of chip bus address 0x11011000 covering the staged app-bank data, e.g. dump_appbank.bin",
    )
    parser.add_argument(
        "--hex",
        default=str(DEFAULT_HEX),
        help="Intel HEX image to compare against (default: current exported stage1 image)",
    )
    parser.add_argument(
        "--xip",
        help="dump of XIP flash region starting at 0x11020000, e.g. dump_xip.bin",
    )
    args = parser.parse_args()

    hex_path = Path(args.hex)
    if not hex_path.exists():
        print(f"ERROR: hex image not found: {hex_path}")
        return 1

    boot_path = Path(args.bootsect)
    if not boot_path.exists():
        print(f"ERROR: boot sector dump not found: {boot_path}")
        return 1

    expected_parts = apply_trampoline(split_for_stock_ota(parse_hex16(hex_path)))
    print_expected_layout(expected_parts)

    boot_ok = verify_boot_sector(boot_path.read_bytes(), expected_parts)
    appbank_result = AppBankVerificationResult(ok=True, complete=True, required_size=0)
    if args.appbank:
        app_path = Path(args.appbank)
        if not app_path.exists():
            print(f"ERROR: app-bank dump not found: {app_path}")
            return 1
        appbank_result = verify_appbank(app_path.read_bytes(), expected_parts)

    xip_ok = True
    if args.xip:
        xip_path = Path(args.xip)
        if not xip_path.exists():
            print(f"ERROR: XIP dump not found: {xip_path}")
            return 1
        xip_ok = verify_xip(xip_path.read_bytes(), XIP_FLASH_BASE, expected_parts)

    print("\n=== Diagnosis ===")
    if not boot_ok:
        print("Boot sector metadata does not match the expected staged stage1 image.")
        print("This explains why the device can fall back to PPlusOTA without ever reaching UART diagnostics.")
    elif args.appbank and not appbank_result.complete:
        suggested = (appbank_result.required_size + 0xFFF) & ~0xFFF
        print("Boot sector metadata and the checked app-bank bytes match so far, but the app-bank dump is incomplete.")
        print(
            f"Redump 0x11011000 for at least 0x{appbank_result.required_size:X} bytes "
            f"(use 0x{suggested:X} for alignment) before concluding OTA corruption."
        )
    elif args.appbank and not appbank_result.ok:
        print("Boot sector metadata matches, but staged app-bank bytes do not.")
        print("This points to OTA write corruption or a wrong image being flashed.")
    elif args.xip and not xip_ok:
        print("XIP flash data does NOT match the expected image.")
        print("The boot loader CRC-checks XIP partitions in-place at 0x11020000+.")
        print("If the XIP data was not written correctly by OTA (e.g. flash sector protection),")
        print("this CRC mismatch is the ROOT CAUSE of PPlusOTA fallback.")
        print("WORKAROUND: Use a SRAM-only hex16 (no XIP partitions) to bypass this check.")
    elif args.appbank:
        print("Boot sector and staged app-bank bytes both match the current stage1 image.")
        if not args.xip:
            print("WARNING: XIP flash data was NOT verified. Dump 0x11020000 (0xC000 bytes) to check.")
        print("If the device still returns to PPlusOTA, the next suspect is runtime failure after load.")
    else:
        print("Boot sector metadata matches the current stage1 image.")
        print("Dump the app-bank staging area too if you want to distinguish metadata failure from payload corruption.")

    return 0 if (boot_ok and appbank_result.ok and xip_ok) else 2


if __name__ == "__main__":
    sys.exit(main())
