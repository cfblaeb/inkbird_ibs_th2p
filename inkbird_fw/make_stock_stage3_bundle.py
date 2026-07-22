#!/usr/bin/env python3
"""Build a stock-OTA image that carries a tiny SRAM installer plus final payload.

This is the "option #3" migration path:

  stock Inkbird OTA -> SRAM installer -> copy staged final image -> reset

The final image is converted into the same flash records that `rdwr_phy62x2.py
wh` would write over UART, then those records are staged at high flash in a
small `IBI3` container. The stock OTA image preserves the official stock XIP
partitions so the live updater is not overwritten during upload.
"""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path

from verify_ota_flash import HexPartition, crc16, is_xip_addr, parse_hex16, split_for_stock_ota


FLASH_BASE = 0x11000000
FLASH_SIZE = 0x80000
FLASH_END = FLASH_BASE + FLASH_SIZE
DEFAULT_STAGING_ADDR = 0x1106E000
DEFAULT_TRACE_SENTINEL_ADDR = 0x1107B000
DEFAULT_START_ADDR = 0x1FFF1838
DEFAULT_WRITE_ADDR = 0x5000
BOOT_TABLE_OFFSET = 0x2000
HEX_RECORD_SIZE = 16
IBI3_MAGIC = 0x33494249  # "IBI3"
IBI3_VERSION = 1
TRACE_SENTINEL_DATA = b"".join(
    struct.pack("<I", value)
    for value in (0x13579BDF, 0x2468ACE0, 0x55AA55AA, 0xAA55AA55)
)

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_STOCK_HEX = HERE / "ibs_thx_b_2p7_48M_phy6222.hex16"
DEFAULT_INSTALLER_HEX = HERE / "build" / "stock_bundle_installer" / "STOCK_BUNDLE_INSTALLER.hex"
# Default to the current hardware-validated image. Callers can still override this
# with --final-hex when testing a fresh source build.
DEFAULT_FINAL_HEX = HERE / "BOOT_IBSTH2P_v22.hex"
DEFAULT_OUTPUT_HEX = HERE / "STAGE3_IBSTH2P_stock_bundle_installer.hex16"
DEFAULT_PAYLOAD_BIN = HERE / "STAGE3_IBSTH2P_stock_bundle_payload.bin"


@dataclass(frozen=True)
class InstallRecord:
    target_addr: int
    data: bytes


def is_sram_addr(addr: int) -> bool:
    return (
        0x1FFF0000 <= addr < 0x20000000
        or 0x20000000 <= addr < 0x20010000
        or 0x20010000 <= addr < 0x20012000
        or 0x20012000 <= addr < 0x20012800
    )


def flash_offset_for_bus_addr(addr: int) -> int:
    if not (FLASH_BASE <= addr < FLASH_END):
        raise ValueError(f"not a 512 KiB PHY6222 flash bus address: 0x{addr:08X}")
    return addr - FLASH_BASE


def align(value: int, boundary: int) -> int:
    return (value + boundary - 1) & ~(boundary - 1)


def insert_bytes(memory: dict[int, int], base_addr: int, data: bytes, label: str) -> None:
    for offset, value in enumerate(data):
        addr = base_addr + offset
        previous = memory.get(addr)
        if previous is not None and previous != value:
            raise ValueError(
                f"address overlap at 0x{addr:08X}: {label} wants 0x{value:02X}, existing 0x{previous:02X}"
            )
        memory[addr] = value


def collapse_memory_map(memory: dict[int, int]) -> list[tuple[int, bytes]]:
    if not memory:
        return []

    ordered = sorted(memory.items())
    start_addr = ordered[0][0]
    current_addr = start_addr
    current = bytearray([ordered[0][1]])
    segments: list[tuple[int, bytes]] = []

    for addr, value in ordered[1:]:
        if addr == current_addr + 1:
            current.append(value)
        else:
            segments.append((start_addr, bytes(current)))
            start_addr = addr
            current = bytearray([value])
        current_addr = addr

    segments.append((start_addr, bytes(current)))
    return segments


def hex_checksum(record: bytes) -> int:
    return (-sum(record)) & 0xFF


def format_hex_record(record_type: int, address: int, payload: bytes) -> str:
    header = bytes([len(payload), (address >> 8) & 0xFF, address & 0xFF, record_type])
    return ":" + (header + payload + bytes([hex_checksum(header + payload)])).hex().upper()


def write_hex16(segments: list[tuple[int, bytes]], output_path: Path) -> None:
    lines: list[str] = []
    current_ela: int | None = None

    for base_addr, data in segments:
        offset = 0
        while offset < len(data):
            full_addr = base_addr + offset
            ela = (full_addr >> 16) & 0xFFFF
            if ela != current_ela:
                lines.append(format_hex_record(0x04, 0, ela.to_bytes(2, "big")))
                current_ela = ela

            chunk = data[offset:offset + HEX_RECORD_SIZE]
            lines.append(format_hex_record(0x00, full_addr & 0xFFFF, chunk))
            offset += len(chunk)

    lines.append(":00000001FF")
    output_path.write_text("\n".join(lines) + "\n")


def build_uart_wh_records(final_hex: Path, *, start_addr: int, write_addr: int) -> list[InstallRecord]:
    parts = parse_hex16(final_hex)
    if not parts:
        raise ValueError(f"no data records in {final_hex}")

    faddr_min = FLASH_SIZE - 1
    faddr_max = 0
    sram_size = 0
    for part in parts:
        if is_sram_addr(part.run_addr):
            sram_size += len(part.data)
        elif is_xip_addr(part.run_addr):
            offset = flash_offset_for_bus_addr(part.run_addr)
            faddr_min = min(faddr_min, offset)
            faddr_max = max(faddr_max, offset + len(part.data))
        else:
            raise ValueError(f"unsupported final HEX address: 0x{part.run_addr:08X}")

    ram_write_addr = write_addr
    if ram_write_addr + sram_size >= faddr_min:
        ram_write_addr = align(faddr_max, 4)

    table = bytearray(b"\xFF" * 0x100)
    table[0:4] = struct.pack("<I", len(parts))
    table[8:12] = struct.pack("<I", start_addr)

    records: list[InstallRecord] = []
    segment_entries = bytearray()

    for part in parts:
        if is_sram_addr(part.run_addr):
            faddr = ram_write_addr
            ram_write_addr = align(ram_write_addr + len(part.data), 4)
        else:
            faddr = flash_offset_for_bus_addr(part.run_addr)

        segment_entries.extend(struct.pack("<IIII", faddr, len(part.data), part.run_addr, 0xFFFFFFFF))
        records.append(InstallRecord(target_addr=FLASH_BASE + faddr, data=part.data))

    table.extend(segment_entries)
    records.insert(0, InstallRecord(target_addr=FLASH_BASE + BOOT_TABLE_OFFSET, data=bytes(table)))
    records.sort(key=lambda rec: rec.target_addr)
    return records


def build_ibi3_payload(records: list[InstallRecord], staging_addr: int) -> bytes:
    record_structs = bytearray()
    data = bytearray()
    header_size = align(32 + 16 * len(records), 16)
    cursor = header_size

    for record in records:
        if record.target_addr < 0x11002000 or record.target_addr + len(record.data) > staging_addr:
            raise ValueError(
                f"target record 0x{record.target_addr:08X}+0x{len(record.data):X} is outside the guarded low-flash install range"
            )
        record_structs.extend(
            struct.pack(
                "<IIII",
                record.target_addr,
                cursor,
                len(record.data),
                crc16(record.data),
            )
        )
        data.extend(record.data)
        pad = (-len(data)) % 4
        if pad:
            data.extend(b"\xFF" * pad)
        cursor = header_size + len(data)

    image_size = header_size + len(data)
    records_crc = crc16(record_structs)
    header = struct.pack(
        "<IIIIIIII",
        IBI3_MAGIC,
        IBI3_VERSION,
        len(records),
        header_size,
        image_size,
        records_crc,
        0,
        0,
    )

    payload = bytearray(header)
    payload.extend(record_structs)
    payload.extend(b"\xFF" * (header_size - len(payload)))
    payload.extend(data)
    return bytes(payload)


def build_stock_ota_memory(
    stock_hex: Path,
    installer_hex: Path,
    payload: bytes,
    staging_addr: int,
    *,
    trace_sentinel: bool = False,
    trace_addr: int = DEFAULT_TRACE_SENTINEL_ADDR,
) -> dict[int, int]:
    memory: dict[int, int] = {}

    for part in parse_hex16(stock_hex):
        if is_xip_addr(part.run_addr):
            insert_bytes(memory, part.run_addr, part.data, f"stock XIP @ 0x{part.run_addr:08X}")
        elif part.run_addr < 0x1FFF1838:
            insert_bytes(memory, part.run_addr, part.data, f"stock low SRAM @ 0x{part.run_addr:08X}")

    for part in parse_hex16(installer_hex):
        if is_xip_addr(part.run_addr):
            raise ValueError(f"installer must be SRAM-only; got XIP data at 0x{part.run_addr:08X}")
        insert_bytes(memory, part.run_addr, part.data, f"installer SRAM @ 0x{part.run_addr:08X}")

    if staging_addr + len(payload) > FLASH_END:
        raise ValueError(
            f"payload does not fit: 0x{staging_addr:08X}+0x{len(payload):X} exceeds 0x{FLASH_END:08X}"
        )
    insert_bytes(memory, staging_addr, payload, f"IBI3 payload @ 0x{staging_addr:08X}")

    if trace_sentinel:
        if staging_addr + len(payload) > trace_addr:
            raise ValueError(
                f"trace sentinel at 0x{trace_addr:08X} overlaps payload ending at "
                f"0x{staging_addr + len(payload):08X}"
            )
        insert_bytes(
            memory,
            trace_addr,
            TRACE_SENTINEL_DATA,
            f"trace sentinel @ 0x{trace_addr:08X}",
        )
    return memory


def print_records(records: list[InstallRecord]) -> None:
    print("=== Installer flash records ===")
    for index, record in enumerate(records):
        print(
            f"{index:02d}: target=0x{record.target_addr:08X} "
            f"size=0x{len(record.data):X} crc16=0x{crc16(record.data):04X}"
        )


def print_stock_ota_parts(hex_path: Path) -> None:
    print("\n=== Stock OTA partitions ===")
    for part in split_for_stock_ota(parse_hex16(hex_path)):
        kind = "XIP" if is_xip_addr(part.run_addr) else "SRAM"
        print(
            f"{part.index:02d}: {kind} flash=0x{part.flash_off:08X} "
            f"run=0x{part.run_addr:08X} size=0x{part.size:X} crc16=0x{part.checksum:04X}"
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stock-hex", type=Path, default=DEFAULT_STOCK_HEX)
    parser.add_argument("--installer-hex", type=Path, default=DEFAULT_INSTALLER_HEX)
    parser.add_argument(
        "--final-hex",
        type=Path,
        default=DEFAULT_FINAL_HEX,
        help="final low-flash image to install (default: current hardware-validated hex)",
    )
    parser.add_argument("--output-hex", type=Path, default=DEFAULT_OUTPUT_HEX)
    parser.add_argument("--payload-bin", type=Path, default=DEFAULT_PAYLOAD_BIN)
    parser.add_argument("--staging-addr", type=lambda value: int(value, 0), default=DEFAULT_STAGING_ADDR)
    parser.add_argument("--trace-addr", type=lambda value: int(value, 0), default=DEFAULT_TRACE_SENTINEL_ADDR)
    parser.add_argument("--start-addr", type=lambda value: int(value, 0), default=DEFAULT_START_ADDR)
    parser.add_argument("--write-addr", type=lambda value: int(value, 0), default=DEFAULT_WRITE_ADDR)
    parser.add_argument(
        "--trace-sentinel",
        action="store_true",
        help=f"stage a known marker at 0x{DEFAULT_TRACE_SENTINEL_ADDR:08X} for post-run installer trace dumps",
    )
    args = parser.parse_args()

    for path in (args.stock_hex, args.installer_hex, args.final_hex):
        if not path.exists():
            raise SystemExit(f"ERROR: missing input file: {path}")

    records = build_uart_wh_records(args.final_hex, start_addr=args.start_addr, write_addr=args.write_addr)
    print_records(records)

    payload = build_ibi3_payload(records, args.staging_addr)
    args.payload_bin.write_bytes(payload)
    print(
        f"\nIBI3 payload: {args.payload_bin} "
        f"staged at 0x{args.staging_addr:08X}, size=0x{len(payload):X}, crc16=0x{crc16(payload):04X}"
    )

    memory = build_stock_ota_memory(
        args.stock_hex,
        args.installer_hex,
        payload,
        args.staging_addr,
        trace_sentinel=args.trace_sentinel,
        trace_addr=args.trace_addr,
    )
    segments = collapse_memory_map(memory)
    write_hex16(segments, args.output_hex)
    print(f"Wrote stock-OTA bundle: {args.output_hex}")
    print_stock_ota_parts(args.output_hex)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
