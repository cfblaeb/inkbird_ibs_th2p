#!/usr/bin/env python3
"""
Analyze original Inkbird IBS-TH2P PHY6222 firmware.
Focus on UART communication with the main chip.
"""
import struct
from capstone import *

data = open('orig.bin', 'rb').read()

# PHY6222 Memory Map:
# ROM:   0x00000000 - 0x0001FFFF
# Flash: 0x11000000 + file_offset
# SRAM:  0x1FFF0000 - 0x20008000

# Firmware regions in flash dump:
# 0x5000-0xa600:  Boot firmware (copied to SRAM)
# 0xb000-0x10c00: Boot trampolines (XIP from flash 0x1100b000)
# 0x11000-0x18300: App vector table + SRAM-loaded code
# 0x20000-0x28f00: App trampolines + code (XIP from flash 0x11020000)

# GPIO enum: P10=6, P17=11, P09=5, P11=7

# === Parse the partition/segment table at 0x3000 ===
print("=" * 60)
print("PARTITION TABLE (0x3000)")
print("=" * 60)
ptable = data[0x3000:0x3080]
# First word: 0x06 (6 segments)
nseg = struct.unpack_from('<I', ptable, 0)[0]
print(f"Number of segments: {nseg}")
print()

# Flash-executed (XIP) segments: 3 entries at offset 0x10
print("Flash XIP segments:")
for i in range(3):
    off = 0x10 + i * 16
    flash_addr, load_addr, size, cksum = struct.unpack_from('<IIII', ptable, off)
    foff = flash_addr - 0x11000000
    print(f"  Seg {i}: flash=0x{flash_addr:08x} (file 0x{foff:05x}) load=0x{load_addr:08x} size=0x{size:04x} ({size} bytes)")

# RAM-loaded segments: 3 entries at offset 0x40
print("\nRAM-loaded segments:")
for i in range(3):
    off = 0x40 + i * 16
    src_off, run_addr, size, cksum = struct.unpack_from('<IIII', ptable, off)
    print(f"  Seg {i+3}: src_off=0x{src_off:05x} run=0x{run_addr:08x} size=0x{size:04x} ({size} bytes)")

# === Parse all trampolines in both banks ===
def parse_trampolines(file_start, flash_base, label):
    """Parse 12-byte jump trampolines."""
    trampolines = {}
    off = file_start
    idx = 0
    while off < len(data) - 12:
        code = data[off:off+8]
        if code != bytes.fromhex('03b40148019001bd'):
            break
        target = struct.unpack_from('<I', data, off+8)[0]
        tramp_addr = flash_base + (off - file_start)
        trampolines[tramp_addr] = target
        off += 12
        idx += 1
    return trampolines

boot_trampolines = parse_trampolines(0xb000, 0x1100b000, "Boot")
app_trampolines = parse_trampolines(0x20000, 0x11020000, "App")

print(f"\n{'='*60}")
print(f"TRAMPOLINES: Boot={len(boot_trampolines)}, App={len(app_trampolines)}")
print(f"{'='*60}")

# === Build SRAM segment mapping ===
# Boot SRAM: loaded from file 0x5000
# Seg3: src_off=0x0000, run=0x1fff0000, size=0x040c -> file 0x5000+0x0000=0x5000
# Seg4: src_off=0x0414, run=0x1fff1838, size=0x4000 -> file 0x5000+0x0414=0x5414
# Seg5: src_off=0x441c, run=0x1fff5838, size=0x2e0c -> file 0x5000+0x441c=0x941c

sram_segments = [
    (0x1fff0000, 0x040c, 0x5000),    # vectors + init
    (0x1fff1838, 0x4000, 0x5414),    # main SRAM code
    (0x1fff5838, 0x2e0c, 0x941c),    # more SRAM code
]

def sram_to_file(addr):
    """Convert SRAM address to file offset."""
    addr = addr & ~1  # Clear Thumb bit
    for run_addr, size, file_off in sram_segments:
        if run_addr <= addr < run_addr + size:
            return file_off + (addr - run_addr)
    return None

def flash_to_file(addr):
    """Convert flash address to file offset."""
    addr = addr & ~1
    if 0x11000000 <= addr < 0x11080000:
        return addr - 0x11000000
    return None

def resolve_addr(addr):
    """Try to get file offset for any address."""
    a = addr & ~1
    foff = sram_to_file(a)
    if foff is not None:
        return foff, "SRAM"
    foff = flash_to_file(a)
    if foff is not None:
        return foff, "FLASH"
    if a < 0x20000:
        return None, "ROM"
    return None, "???"

# === Disassemble a function ===
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True

def disasm_at(file_offset, flash_or_sram_addr, max_insns=80):
    """Disassemble Thumb code starting at given file offset."""
    code = data[file_offset:file_offset+max_insns*4]
    insns = list(md.disasm(code, flash_or_sram_addr & ~1))
    return insns

def disasm_function(addr, max_insns=120, label=""):
    """Disassemble a function starting at addr."""
    foff, loc = resolve_addr(addr)
    if foff is None:
        print(f"  Cannot resolve 0x{addr:08x} ({loc})")
        return []
    
    insns = disasm_at(foff, addr & ~1, max_insns)
    if label:
        print(f"\n--- {label} at 0x{addr:08x} ({loc}, file 0x{foff:05x}) ---")
    
    result = []
    for i in insns:
        line = f"  0x{i.address:08x}: {i.mnemonic:8s} {i.op_str}"
        
        # Annotate known addresses
        if i.mnemonic in ('bl', 'b', 'b.n', 'b.w'):
            try:
                target = int(i.op_str.replace('#', ''), 0) if i.op_str.startswith('#') else int(i.op_str, 0)
            except:
                target = None
            if target:
                if target in app_trampolines:
                    final = app_trampolines[target]
                    _, floc = resolve_addr(final)
                    line += f"  ; tramp -> 0x{final:08x} ({floc})"
                elif target in boot_trampolines:
                    final = boot_trampolines[target]
                    _, floc = resolve_addr(final)
                    line += f"  ; tramp -> 0x{final:08x} ({floc})"
        
        # Annotate literal pool loads
        if i.mnemonic == 'ldr' and '[pc' in i.op_str:
            # Extract the effective address
            # The literal pool address calculation for Thumb:
            # PC = current_instruction_address + 4, aligned to 4
            # But capstone might give us the operand directly
            pass
        
        result.append(i)
        print(line)
        
        # Stop at return
        if i.mnemonic in ('bx', 'pop') and ('pc' in i.op_str or 'lr' in i.op_str):
            if i.mnemonic == 'pop' and 'pc' in i.op_str:
                break
            if i.mnemonic == 'bx' and 'lr' in i.op_str:
                break
    
    return result

# === MAIN ANALYSIS ===

# 1. The UART init function at 0x11023a0c
print(f"\n{'='*60}")
print("UART INIT FUNCTION")
print(f"{'='*60}")

# First, decode what the config struct at 0x11028d28 contains
print("\nUART config data at 0x11028d28:")
config_data = data[0x28d28:0x28d60]
print(f"  Raw: {config_data[:20].hex()}")

# uart_Cfg_t as passed to hal_uart_init:
# The function loads 4 words and passes them as args
# Word 0 (r0): tx_pin + rx_pin + rts_pin + cts_pin (packed as bytes? or as ints?)
w0, w1, w2, w3, w4 = struct.unpack_from('<IIIII', config_data, 0)
print(f"  Word 0: 0x{w0:08x}  (bytes: {config_data[0]:02x} {config_data[1]:02x} {config_data[2]:02x} {config_data[3]:02x})")
print(f"  Word 1: 0x{w1:08x}  = {w1} (baud rate)")
print(f"  Word 2: 0x{w2:08x}")
print(f"  Word 3: 0x{w3:08x}")
print(f"  Word 4: 0x{w4:08x}")

# Byte 0=0x05=GPIO_P09, Byte 1=0x06=GPIO_P10, Byte 2=0xFF, Byte 3=0xFF
# Maybe: tx=P09(5), rx=P10(6), rts=0xFF(none), cts=0xFF(none)
# OR:    tx=P10(6), rx=P09(5), rts=0xFF(none), cts=0xFF(none)  
# But we know from probing: P10=TX (idle-high), P17=RX (active)
# So this UART config might be for a DIFFERENT UART channel!
# P09 is enum 5, P10 is enum 6
# Perhaps this is NOT the inter-chip UART? Or P09 is used in original firmware?
print()
print("  Interpretation:")
print(f"    tx_pin = {config_data[0]} = GPIO_P{['00','01','02','03','07','09','10','11','14','15','16','17','18','20','23','24','25','26','27','31','32','33','34'][config_data[0]] if config_data[0] < 23 else '??'}")
print(f"    rx_pin = {config_data[1]} = GPIO_P{['00','01','02','03','07','09','10','11','14','15','16','17','18','20','23','24','25','26','27','31','32','33','34'][config_data[1]] if config_data[1] < 23 else '??'}")
print(f"    rts    = {config_data[2]} (0xFF=none)")
print(f"    cts    = {config_data[3]} (0xFF=none)")
print(f"    baud   = {w1}")

# Disassemble the init function
disasm_function(0x11023a0d, label="uart_init_wrapper")

# 2. Now find the function that's called (trampoline target)
tramp_target = app_trampolines.get(0x1102045c)
if tramp_target:
    print(f"\nTrampoline 0x1102045c -> 0x{tramp_target:08x}")
    disasm_function(tramp_target, label="hal_uart_init (via trampoline)")

# 3. Now search for hal_uart_send_buff calls - look for trampoline to known ROM addresses
# In the build, hal_uart_send_buff has a specific ROM address
# Let me search for all trampolines that might be uart functions
print(f"\n{'='*60}")
print("SEARCHING FOR UART SEND FUNCTIONS")
print(f"{'='*60}")

# Known ROM UART functions from typical PHY6222 ROM:
# Let me look at our own build's linker map or symbols for reference
# For now, let me find functions in the app region that call the UART init trampoline

# Search for 'bl 0x1102045c' in the app code
# In Thumb2, bl is a 4-byte instruction
# Let me search for all BL targets in the app code region
app_code = data[0x20000:0x28f00]
app_base = 0x11020000

# Better approach: search for functions that reference UART-related addresses
# Find all functions that reference the UART config or nearby data

# Let me look at function pointers stored in the config area
# At 0x028d38: value 2 (could be UART_INDEX = UART1 = 1, wait no, 2?)
# At 0x028d3c: 0x11028dfc (table pointer)
# At 0x028d40: 0x1102583d (function pointer)
# At 0x028d44: 0x11025959 (function pointer)

print("\nFunction pointer 0x1102583d:")
disasm_function(0x1102583d, label="config_func_1 (evt_handler?)")

print("\nFunction pointer 0x11025959:")
disasm_function(0x11025959, max_insns=80, label="config_func_2")

# 4. Search for the string "a5" or heartbeat-related byte patterns
print(f"\n{'='*60}")
print("SEARCHING FOR HEARTBEAT FRAME PATTERNS")
print(f"{'='*60}")

# Search for the constant 0xA5 used as frame markers
# Look in code regions for immediate values or byte constants
heartbeat_ref = bytes([0xa5, 0x00, 0x40, 0x25])
for i in range(0, len(data), 1):
    if data[i:i+4] == heartbeat_ref:
        if not all(b == 0xFF for b in data[max(0,i-4):i]):
            print(f"  Heartbeat ref 'a5 00 40 25' at file 0x{i:06x}")

# Search for 0xA0 threshold
for i in range(0, len(data)-4, 2):
    val = struct.unpack_from('<I', data, i)[0]
    if val == 0xa0 or val == 0xa5:
        # Check if in code region
        if (0x5000 <= i <= 0xa600) or (0x20000 <= i <= 0x28f00):
            print(f"  Value 0x{val:02x} in code at file 0x{i:06x}")

# 5. Look for the second UART config structure (maybe for UART1 with P17)
print(f"\n{'='*60}")
print("SEARCHING FOR SECOND UART CONFIG (P17)")
print(f"{'='*60}")

# Search for byte value 11 (GPIO_P17) followed by nearby patterns
# In the config area
for i in range(0x28000, 0x28f00):
    # Look for byte 11 (P17) near byte 6 (P10) 
    if data[i] == 6 and data[i+1] == 11:
        print(f"  P10(6), P17(11) at file 0x{i:06x}: {data[i:i+16].hex()}")
    if data[i] == 11 and data[i+1] == 6:
        print(f"  P17(11), P10(6) at file 0x{i:06x}: {data[i:i+16].hex()}")

# Also search entire binary for the pair
print("\n  Searching full binary for P10/P17 byte pairs:")
for i in range(0, len(data)-8):
    if all(b == 0xFF for b in data[max(0,i-2):i]):
        continue  # Skip erased flash
    if data[i] == 6 and data[i+1] == 11 and data[i+2] == 0xFF and data[i+3] == 0xFF:
        # tx=P10(6), rx=P17(11), rts=none, cts=none - IDEAL MATCH
        baud = struct.unpack_from('<I', data, i+4)[0]
        print(f"  *** MATCH: tx=P10, rx=P17, rts=FF, cts=FF at 0x{i:06x}, next word=0x{baud:08x} ({baud})")
