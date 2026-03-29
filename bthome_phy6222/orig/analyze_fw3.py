#!/usr/bin/env python3
"""
Focused analysis: Find P17/UART1 config, trace TX path, find app-level UART calls.
"""
import struct
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB

FW = open("orig.bin", "rb").read()

# APP SRAM segments: (run_addr, size, file_offset)
APP_SRAM_SEGS = [
    (0x1fff0000, 0x040c, 0x11000),
    (0x1fff1838, 0x4000, 0x11414),
    (0x1fff5838, 0x2e0c, 0x1541c),
]
# APP Flash (XIP): run at 0x11020000, file at 0x20000
APP_FLASH_BASE = 0x11020000
APP_FLASH_OFF  = 0x20000
APP_FLASH_SIZE = 0x8f00

def addr_to_file(addr):
    """Convert runtime address to file offset."""
    for run, sz, foff in APP_SRAM_SEGS:
        if run <= addr < run + sz:
            return foff + (addr - run)
    if APP_FLASH_BASE <= addr < APP_FLASH_BASE + APP_FLASH_SIZE:
        return APP_FLASH_OFF + (addr - APP_FLASH_BASE)
    return None

def file_to_addr(foff):
    """Convert file offset to runtime address."""
    for run, sz, fo in APP_SRAM_SEGS:
        if fo <= foff < fo + sz:
            return run + (foff - fo)
    if APP_FLASH_OFF <= foff < APP_FLASH_OFF + APP_FLASH_SIZE:
        return APP_FLASH_BASE + (foff - APP_FLASH_OFF)
    return None

def disasm_at(addr, count=40):
    """Disassemble 'count' instructions starting at addr."""
    foff = addr_to_file(addr & ~1)
    if foff is None:
        return f"  Cannot map 0x{addr:08x} to file"
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    md.detail = False
    code = FW[foff:foff + count * 4]
    lines = []
    for insn in md.disasm(code, addr & ~1):
        # Annotate literal pool loads
        extra = ""
        if insn.mnemonic == "ldr" and "pc" in insn.op_str:
            pc_val = (insn.address + 4) & ~3
            try:
                off_str = insn.op_str.split("#")[1].rstrip("])")
                offset = int(off_str, 16) if "0x" in off_str else int(off_str)
                target = pc_val + offset
                tfoff = addr_to_file(target)
                if tfoff and tfoff + 4 <= len(FW):
                    val = struct.unpack_little_endian("<I", FW[tfoff:tfoff+4])[0] if False else int.from_bytes(FW[tfoff:tfoff+4], 'little')
                    extra = f"  ; =0x{val:08x}"
            except:
                pass
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                extra = f"  ; -> {resolve_target(target)}"
            except:
                pass
        lines.append(f"  0x{insn.address:08x}: {insn.mnemonic:<8s} {insn.op_str}{extra}")
        if insn.mnemonic in ("pop", "bx") and "pc" in insn.op_str:
            break
    return "\n".join(lines)

# Parse trampolines for resolution
TRAMPOLINES = {}
tramp_base = APP_FLASH_BASE
tramp_off = APP_FLASH_OFF
while tramp_off + 12 <= APP_FLASH_OFF + APP_FLASH_SIZE:
    code8 = FW[tramp_off:tramp_off+8]
    target = int.from_bytes(FW[tramp_off+8:tramp_off+12], 'little')
    if code8[0:2] == b'\x00\xbf' or code8 == bytes(8):
        break
    # Check if it's asm trampoline pattern (ldr + bx)
    if (code8[1] & 0xF8) == 0x48 and code8[3] == 0x47:  # ldr rX, [pc,...] + bx rX
        TRAMPOLINES[tramp_base] = target
    tramp_base += 12
    tramp_off += 12

def resolve_target(addr):
    if addr in TRAMPOLINES:
        return f"tramp -> 0x{TRAMPOLINES[addr]:08x}"
    return f"0x{addr:08x}"

print("=" * 70)
print("1. SEARCH FOR ALL hal_uart_init CALLS (looking for UART1 init)")
print("=" * 70)

# hal_uart_init lives at 0x1fff426c (from trampoline at 0x1102045c -> 0x1fff426d)
# The trampoline address is 0x1102045c
# Search for BL instructions targeting 0x1102045c in both SRAM and flash
UART_INIT_TRAMP = 0x1102045c

# Search flash code for BL to uart_init trampoline
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = False
flash_code = FW[APP_FLASH_OFF:APP_FLASH_OFF + APP_FLASH_SIZE]
for insn in md.disasm(flash_code, APP_FLASH_BASE):
    if insn.mnemonic == "bl":
        try:
            target = int(insn.op_str.lstrip("#"), 16)
            if target == UART_INIT_TRAMP:
                print(f"  FLASH: BL to uart_init at 0x{insn.address:08x}")
        except:
            pass

# Search SRAM code for BL to 0x1fff426c
for run, sz, foff in APP_SRAM_SEGS:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, run):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (0x1fff426c, 0x1fff426d):
                    print(f"  SRAM: BL to uart_init at 0x{insn.address:08x}")
            except:
                pass

print()
print("=" * 70)
print("2. SEARCH FOR hal_uart_init IN SRAM (the actual function)")
print("=" * 70)
# The function at 0x1fff426c takes 5 args: r0=tx, r1=rx, r2=rts, r3=cts, [sp]=port
# Let's disassemble more of it to understand what it does
print(disasm_at(0x1fff426c, 80))

print()
print("=" * 70)
print("3. SEARCH FOR GPIO P17 (enum 11) CONFIGURATION IN CODE")
print("=" * 70)
# Look for movs rX, #11 or #0xb near GPIO config calls
# Key functions: hal_gpio_pin_init, hal_gpio_pull_set, hal_gpio_fmux_set
# These are called with pin enum as first arg

# Search for "movs rX, #0xb" followed by bl (function call)
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM0", APP_SRAM_SEGS[0][0], APP_SRAM_SEGS[0][2], APP_SRAM_SEGS[0][1]),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    prev_insn = None
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl" and prev_insn:
            if prev_insn.mnemonic == "movs" and "#0xb" in prev_insn.op_str:
                target = int(insn.op_str.lstrip("#"), 16)
                print(f"  {region_name}: movs+bl at 0x{prev_insn.address:08x}: movs {prev_insn.op_str} then bl {resolve_target(target)}")
            # Also check 2 instructions back for r0=#0xb
            if hasattr(prev_insn, '_prev') and prev_insn._prev and prev_insn._prev.mnemonic == "movs" and "#0xb" in prev_insn._prev.op_str:
                target = int(insn.op_str.lstrip("#"), 16)
                print(f"  {region_name}: movs+X+bl at 0x{prev_insn._prev.address:08x}")
        if prev_insn:
            insn._prev = prev_insn
        else:
            insn._prev = None
        prev_insn = insn

print()
print("=" * 70)
print("4. FIND ALL BL CALLS TO hal_uart_send_buff (0x1fff6ab8)")
print("=" * 70)
# 0x1fff6ab8 is the multi-byte UART send function
UART_SEND = 0x1fff6ab8
UART_SEND_BYTE = 0x1fff42f0  # single byte send

# First find trampolines to these
send_tramps = []
send_byte_tramps = []
for addr, target in TRAMPOLINES.items():
    if target == UART_SEND + 1 or target == UART_SEND:
        send_tramps.append(addr)
        print(f"  Trampoline to uart_send_buff: 0x{addr:08x} -> 0x{target:08x}")
    if target == UART_SEND_BYTE + 1 or target == UART_SEND_BYTE:
        send_byte_tramps.append(addr)
        print(f"  Trampoline to uart_send_byte: 0x{addr:08x} -> 0x{target:08x}")

# Search for direct BL calls in all code regions
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (UART_SEND, UART_SEND + 1):
                    print(f"  {region_name}: BL to uart_send_buff at 0x{insn.address:08x}")
                if target in (UART_SEND_BYTE, UART_SEND_BYTE + 1):
                    print(f"  {region_name}: BL to uart_send_byte at 0x{insn.address:08x}")
                if target in send_tramps:
                    print(f"  {region_name}: BL to send_tramp at 0x{insn.address:08x} (-> uart_send_buff)")
                if target in send_byte_tramps:
                    print(f"  {region_name}: BL to send_byte_tramp at 0x{insn.address:08x} (-> uart_send_byte)")
            except:
                pass

print()
print("=" * 70)
print("5. SEARCH FOR SECOND UART CONFIG (UART port 1)")
print("=" * 70)
# The hal_uart_init at 0x1fff426c checks port number (5th arg on stack)
# When port=1, it uses UART1 (0x40009000)
# Look for code that pushes 1 on stack then calls uart_init
# Also look for uart_config structures with different pins

# Search for any 9600 baud rate references (0x2580 = 9600)
baud_bytes = struct.pack('<I', 9600)
pos = 0
while True:
    pos = FW.find(baud_bytes, pos)
    if pos < 0:
        break
    # Check if this could be a UART config (check surrounding bytes)
    if pos >= 4 and pos + 20 <= len(FW):
        # UART config: tx(1), rx(1), rts(1), cts(1), baud(4), ...
        # baud is at offset 4 in the config
        cfg_start = pos - 4
        tx = FW[cfg_start]
        rx = FW[cfg_start + 1]
        rts = FW[cfg_start + 2]
        cts = FW[cfg_start + 3]
        # Valid GPIO enums are 0-23 or 0xFF
        if tx <= 23 or tx == 0xFF:
            if rx <= 23 or rx == 0xFF:
                addr = file_to_addr(cfg_start)
                addr_str = f"0x{addr:08x}" if addr else "unknown"
                print(f"  Possible UART config at file 0x{cfg_start:05x} ({addr_str}): tx={tx} rx={rx} rts={rts} cts={cts} baud=9600")
    pos += 1

print()
print("=" * 70)
print("6. P17 IN BOOT FIRMWARE")
print("=" * 70)
# Maybe P17 is configured by the BOOT firmware, not APP
# Boot SRAM: file 0x5000, vectors
# Boot trampolines: file 0xb000

# Boot SRAM segments from boot partition table
# Let's check the boot header at 0x1000
boot_hdr = FW[0x1000:0x1040]
print(f"  Boot header: {boot_hdr[:16].hex()}")

# Search boot SRAM region (0x5000-0xa600) for P17 references
boot_sram = FW[0x5000:0xa600]
# Look for 9600 baud in boot
pos = 0
while True:
    pos = boot_sram.find(baud_bytes, pos)
    if pos < 0:
        break
    cfg_start = pos - 4
    if cfg_start >= 0:
        tx = boot_sram[cfg_start]
        rx = boot_sram[cfg_start + 1]
        rts = boot_sram[cfg_start + 2]
        cts = boot_sram[cfg_start + 3]
        if tx <= 23 or tx == 0xFF:
            if rx <= 23 or rx == 0xFF:
                print(f"  Boot SRAM: Possible UART config at file 0x{0x5000+cfg_start:05x}: tx={tx} rx={rx} rts={rts} cts={cts} baud=9600")
    pos += 1

# Search boot flash/trampoline region
boot_flash = FW[0xb000:0x10c00]
pos = 0
while True:
    pos = boot_flash.find(baud_bytes, pos)
    if pos < 0:
        break
    cfg_start = pos - 4
    if cfg_start >= 0:
        tx = boot_flash[cfg_start]
        rx = boot_flash[cfg_start + 1]
        rts = boot_flash[cfg_start + 2]
        cts = boot_flash[cfg_start + 3]
        if tx <= 23 or tx == 0xFF:
            if rx <= 23 or rx == 0xFF:
                print(f"  Boot flash: Possible UART config at file 0x{0xb000+cfg_start:05x}: tx={tx} rx={rx} rts={rts} cts={cts} baud=9600")
    pos += 1

print()
print("=" * 70)
print("7. DUMP FUNCTION AROUND THE APP UART INIT WRAPPER CALLER")
print("=" * 70)
# The uart_init_wrapper is at 0x11023a0c
# And the function at 0x11022164 calls 0x11023a6c (another UART-adjacent func)
# Let's disassemble more context around these

# First, find what calls 0x11023a0c (the wrapper)
print("Callers of uart_init_wrapper (0x11023a0c):")
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (0x11023a0c, 0x11023a0d):
                    print(f"  {region_name}: BL at 0x{insn.address:08x}")
            except:
                pass

# And 0x11023a6c
print("\nCallers of 0x11023a6c:")
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (0x11023a6c, 0x11023a6d):
                    print(f"  {region_name}: BL at 0x{insn.address:08x}")
            except:
                pass

print("\n--- Disasm of 0x11023a6c ---")
print(disasm_at(0x11023a6c, 40))

print()
print("=" * 70)
print("8. TRACE THE UART EVENT HANDLER (0x1102583d) IN DETAIL")
print("=" * 70)
# This is the callback function registered in the UART config
# offset 24 of the config struct = 0x1102583d
# Let's get the full function
print("--- uart_evt_handler full (0x1102583c) ---")
print(disasm_at(0x1102583c, 80))

print()
print("=" * 70)
print("9. TRACE FUNCTION 0x11025959 MORE (the data handler)")
print("=" * 70)
# case 0 -> 0x11025992
# case 1 -> 0x110259d6
# case 2 -> 0x11025a18
# case 3 -> 0x11025a04
print("--- func_25959 full (0x11025958) ---")
print(disasm_at(0x11025958, 120))

print()
print("=" * 70)
print("10. FIND ALL FUNCTIONS THAT WRITE TO UART THR (reg offset 0x00)")
print("=" * 70)
# UART THR is at base+0. Writing: strb rX, [rY] where rY=UART base
# The function at 0x1fff42f0 does: strb r1, [r2] where r2=UART base
# And 0x1fff6ab8 does: strb r5, [r3] where r3=UART base

# More importantly, let's find ALL functions that call 0x1fff6ab8 (uart_send_buff)
print("All callers of uart_send_buff (0x1fff6ab8):")
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM0", APP_SRAM_SEGS[0][0], APP_SRAM_SEGS[0][2], APP_SRAM_SEGS[0][1]),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (UART_SEND, UART_SEND + 1):
                    print(f"  {region_name}: BL at 0x{insn.address:08x}")
            except:
                pass

print("\nAll callers of uart_send_byte (0x1fff42f0):")
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM0", APP_SRAM_SEGS[0][0], APP_SRAM_SEGS[0][2], APP_SRAM_SEGS[0][1]),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (UART_SEND_BYTE, UART_SEND_BYTE + 1):
                    print(f"  {region_name}: BL at 0x{insn.address:08x}")
            except:
                pass

print()
print("=" * 70)
print("11. KEY DATA STRUCTURE AT 0x1fff76d8")
print("=" * 70)
# This address appears in multiple UART-related functions
# Let's dump its contents
foff = addr_to_file(0x1fff76d8)
if foff:
    data = FW[foff:foff+64]
    print(f"  File offset: 0x{foff:05x}")
    for i in range(0, 64, 4):
        val = int.from_bytes(data[i:i+4], 'little')
        print(f"  +{i:3d} (0x{0x1fff76d8+i:08x}): 0x{val:08x}")
else:
    print("  Cannot map to file")

print()
print("=" * 70)
print("12. KEY DATA STRUCTURE AT 0x1fff89b4 (UART context array)")
print("=" * 70)
# Referenced by hal_uart_init and many UART functions
# Indexed by port << 5 (32-byte entries per UART port)
foff = addr_to_file(0x1fff89b4)
if foff:
    print(f"  File offset: 0x{foff:05x}")
    # Dump 64 bytes (2 ports x 32 bytes each)
    data = FW[foff:foff+64]
    for port in range(2):
        print(f"\n  UART{port} context (offset {port*32}):")
        entry = data[port*32:(port+1)*32]
        for i in range(0, 32, 4):
            val = int.from_bytes(entry[i:i+4], 'little')
            print(f"    +{i:3d}: 0x{val:08x}")
else:
    print("  Cannot map to file (in uninitialized BSS?)")
    print("  This is a runtime-only data structure at 0x1fff89b4")
    # Check: 0x1fff89b4 - is it past the end of loaded segments?
    max_addr = max(run + sz for run, sz, _ in APP_SRAM_SEGS)
    print(f"  Max loaded SRAM: 0x{max_addr:08x}")
    print(f"  0x1fff89b4 is {'within' if 0x1fff89b4 < max_addr else 'BEYOND'} loaded segments -> BSS/heap")

print()
print("=" * 70)
print("13. SEARCH FOR GPIO_P17 IN FUNCTION INIT TABLE (0x1fff1f84)")
print("=" * 70)
# 0x1fff1f84 was called from hal_uart_deinit (0x1fff6be4) with pin args
# It's likely hal_gpio_fmux_set or hal_gpio_pin_init
# Search for calls to it with r0=0xb (P17)
print("--- func at 0x1fff1f84 ---")
print(disasm_at(0x1fff1f84, 30))

print()
print("=" * 70)
print("14. COMPLETE FUNCTION CALL GRAPH FROM APP INIT")
print("=" * 70)
# The app's reset vector is at 0x1fff55f5 (from vector table at 0x11000+4)
# Let's trace the initialization path to find UART init
vec_table = FW[0x11000:0x11010]
sp_init = int.from_bytes(vec_table[0:4], 'little')
reset_vec = int.from_bytes(vec_table[4:8], 'little')
print(f"  SP init: 0x{sp_init:08x}")
print(f"  Reset vector: 0x{reset_vec:08x}")

print("\n--- Reset handler ---")
print(disasm_at(reset_vec, 60))

# Find the OSAL init / app init functions
# Look for the function that registers tasks
print()
print("=" * 70)
print("15. SEARCH FOR FUNCTION 0x11022164 CALLERS (the UART setup func)")
print("=" * 70)
for region_name, base, foff, sz in [
    ("FLASH", APP_FLASH_BASE, APP_FLASH_OFF, APP_FLASH_SIZE),
    ("SRAM1", APP_SRAM_SEGS[1][0], APP_SRAM_SEGS[1][2], APP_SRAM_SEGS[1][1]),
    ("SRAM2", APP_SRAM_SEGS[2][0], APP_SRAM_SEGS[2][2], APP_SRAM_SEGS[2][1]),
]:
    code = FW[foff:foff + sz]
    for insn in md.disasm(code, base):
        if insn.mnemonic == "bl":
            try:
                target = int(insn.op_str.lstrip("#"), 16)
                if target in (0x11022164, 0x11022165):
                    print(f"  {region_name}: BL at 0x{insn.address:08x}")
            except:
                pass
