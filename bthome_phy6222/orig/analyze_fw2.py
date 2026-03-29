#!/usr/bin/env python3
"""
Analyze original Inkbird IBS-TH2P PHY6222 firmware - FIXED SRAM mapping.
The APP firmware loads SRAM segments from file offset 0x11000, not 0x5000.
"""
import struct
from capstone import *

data = open('orig.bin', 'rb').read()

# PHY6222 Memory Map:
# ROM:   0x00000000 - 0x0001FFFF
# Flash: 0x11000000 + file_offset
# SRAM:  0x1FFF0000 - 0x20008000

# GPIO enum: P00=0,P01=1,P02=2,P03=3,P07=4,P09=5,P10=6,P11=7,
#            P14=8,P15=9,P16=10,P17=11,P18=12,P20=13,...
GPIO_NAMES = ['P00','P01','P02','P03','P07','P09','P10','P11',
              'P14','P15','P16','P17','P18','P20','P23','P24',
              'P25','P26','P27','P31','P32','P33','P34']

def gpio_name(val):
    if val < len(GPIO_NAMES):
        return GPIO_NAMES[val]
    return f"?{val}"

# APP firmware SRAM segments (loaded from file 0x11000 + src_off):
APP_SRAM = [
    (0x1fff0000, 0x040c, 0x11000),    # vectors + init
    (0x1fff1838, 0x4000, 0x11414),    # main SRAM code
    (0x1fff5838, 0x2e0c, 0x1541c),    # more SRAM code
]

# APP flash XIP segments:
APP_FLASH = [
    (0x11020000, 0x4000, 0x20000),   # trampolines
    (0x11024000, 0x4000, 0x24000),   # code
    (0x11028000, 0x0ea0, 0x28000),   # code + data
]

def parse_trampolines(file_start, flash_base):
    t = {}
    off = file_start
    while off < len(data) - 12:
        code = data[off:off+8]
        if code != bytes.fromhex('03b40148019001bd'):
            break
        target = struct.unpack_from('<I', data, off+8)[0]
        tramp_addr = flash_base + (off - file_start)
        t[tramp_addr] = target
        off += 12
    return t

app_trampolines = parse_trampolines(0x20000, 0x11020000)

def addr_to_file(addr):
    """Convert any address to file offset."""
    a = addr & ~1
    # SRAM
    for run_addr, size, file_off in APP_SRAM:
        if run_addr <= a < run_addr + size:
            return file_off + (a - run_addr), "SRAM"
    # Flash
    for flash_addr, size, file_off in APP_FLASH:
        if flash_addr <= a < flash_addr + size:
            return file_off + (a - flash_addr), "FLASH"
    if a < 0x20000:
        return None, "ROM"
    return None, "???"

md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
md.detail = True

def disasm_func(addr, max_insns=100, label="", show=True):
    """Disassemble a function."""
    foff, loc = addr_to_file(addr)
    if foff is None:
        if show:
            print(f"  Cannot resolve 0x{addr:08x} ({loc})")
        return []
    
    code = data[foff:foff+max_insns*4]
    insns = list(md.disasm(code, addr & ~1, max_insns))
    
    if show and label:
        print(f"\n--- {label} @ 0x{addr:08x} ({loc}, file 0x{foff:05x}) ---")
    
    result = []
    for i in insns:
        line = f"  0x{i.address:08x}: {i.mnemonic:8s} {i.op_str}"
        
        # Annotate trampoline calls
        if i.mnemonic in ('bl', 'b.w'):
            try:
                tgt = int(i.op_str.lstrip('#'), 0)
                if tgt in app_trampolines:
                    final = app_trampolines[tgt]
                    _, floc = addr_to_file(final)
                    line += f"  ; -> 0x{final:08x} ({floc})"
            except:
                pass
        
        # Annotate literal pool loads
        if i.mnemonic == 'ldr' and '[pc' in i.op_str:
            # Calculate literal pool address
            pc_aligned = (i.address + 4) & ~3
            # Extract immediate from op_str like "r3, [pc, #0xc]"
            try:
                parts = i.op_str.split('#')
                if len(parts) == 2:
                    imm = int(parts[1].rstrip(']'), 0)
                    lit_addr = pc_aligned + imm
                    lit_foff, _ = addr_to_file(lit_addr)
                    if lit_foff is not None and lit_foff + 4 <= len(data):
                        val = struct.unpack_from('<I', data, lit_foff)[0]
                        line += f"  ; =0x{val:08x}"
            except:
                pass
        
        result.append(i)
        if show:
            print(line)
        
        # Stop at return-like instruction
        if i.mnemonic == 'pop' and 'pc' in i.op_str:
            break
        if i.mnemonic == 'bx' and 'lr' in i.op_str:
            break
    
    return result

# === KEY ANALYSIS ===

print("=" * 70)
print("UART CONFIG at 0x11028d28")
print("=" * 70)
cfg = data[0x28d28:0x28d60]
print(f"  tx_pin  = {cfg[0]} ({gpio_name(cfg[0])})")
print(f"  rx_pin  = {cfg[1]} ({gpio_name(cfg[1])})")
print(f"  rts_pin = {cfg[2]} ({'none' if cfg[2]==0xFF else gpio_name(cfg[2])})")
print(f"  cts_pin = {cfg[3]} ({'none' if cfg[3]==0xFF else gpio_name(cfg[3])})")
baud = struct.unpack_from('<I', cfg, 4)[0]
print(f"  baud    = {baud}")
use_fifo = struct.unpack_from('<I', cfg, 8)[0]
hw_fwctrl = struct.unpack_from('<I', cfg, 12)[0]
print(f"  use_fifo   = {use_fifo}")
print(f"  hw_fwctrl  = {hw_fwctrl}")

# The next fields depend on struct packing
# After use_fifo(4) + hw_fwctrl(4), we have use_tx_buf and parity
# But since bools might be packed as 4-byte ints...
# Word at offset 16: 0x00000002 — might be uart_index, not part of cfg
# Words at offset 20: 0x11028dfc, 0x1102583d, 0x11025959
# These look like: evt_handler, and extra function pointers

print(f"\n  After main config:")
for i in range(16, 48, 4):
    val = struct.unpack_from('<I', cfg, i)[0]
    print(f"    offset {i}: 0x{val:08x}")

# Config interpretation:
# The struct uart_Cfg_t has 4 gpio_pin_e (each 4 bytes as enum=int),
# but maybe the compiler packs them as bytes since they fit in uint8.
# Let me check: tx=P09(5), rx=P10(6) — but from our hardware scan, 
# P09 was FLOATING and P10 is IDLE-HIGH (TX line from PHY).
# This is BACKWARDS from what we expected!
# UNLESS: P09 IS connected but was floating because our custom firmware
# didn't initialize it! The original firmware uses P09 as TX and P10 as RX.

print("\n" + "=" * 70)
print("!!! KEY FINDING: Original uses P09 as TX, P10 as RX !!!")
print("!!! We saw P10 as idle-HIGH because it has an external pull-up !!!")
print("!!! P09 appeared floating because our FW never drove it       !!!")
print("=" * 70)

# But wait - in our scan, P10 was class 3 (strongly driven HIGH, resists pull-down)
# ...and P17 was class 0 with 2 transitions (UART RX from main chip).
# If P10 is the UART RX from main chip, why class 3?
# Unless there are TWO UART channels: one for each direction.

# Let me look for a SECOND UART config with P17

print("\n" + "=" * 70)
print("SEARCHING FOR ALL UART-LIKE CONFIGS IN DATA REGION")
print("=" * 70)

# Search for patterns: byte byte ff ff followed by 00002580
for i in range(0x28000, 0x28f00-8):
    val = struct.unpack_from('<I', data, i+4)[0]
    if val == 9600 and data[i+2] == 0xFF and data[i+3] == 0xFF:
        print(f"  UART config at 0x{i:06x}: tx={gpio_name(data[i])} rx={gpio_name(data[i+1])} baud=9600")

# Also search for ANY occurrence of byte 11 (P17) in the data region near the UART config
print("\nAll occurrences of GPIO_P17 (enum 11) in 0x28c00-0x28f00:")
for i in range(0x28c00, 0x28f00):
    if data[i] == 11:
        ctx = data[max(0,i-4):i+8]
        print(f"  byte 11 at 0x{i:06x}: ctx={ctx.hex()}")

# =============================================================
# Now disassemble the UART init wrapper with correct SRAM mapping
print("\n" + "=" * 70)
print("UART INIT WRAPPER (0x11023a0c)")
print("=" * 70)
disasm_func(0x11023a0d, label="uart_init_wrapper")

# Follow the trampoline call to 0x1fff426d with CORRECT mapping
tramp_target = app_trampolines.get(0x1102045c)
if tramp_target:
    print(f"\nTrampoline 0x1102045c -> 0x{tramp_target:08x}")
    disasm_func(tramp_target, max_insns=60, label="hall_uart_init")

# =============================================================
# Now find the UART callback/event handler
# The config at 0x11028d40 has a function ptr 0x1102583d
print("\n" + "=" * 70)
print("UART EVENT HANDLER? (0x1102583d)")
print("=" * 70)
disasm_func(0x1102583d, max_insns=150, label="uart_evt_handler")

# And the other function pointer
print("\n" + "=" * 70)
print("FUNCTION at 0x11025959")
print("=" * 70)
disasm_func(0x11025959, max_insns=80, label="func_25959")

# =============================================================
# Search for hal_uart_send_buff calls
# In the ROM, hal_uart_send_buff might be at a specific address
# Let me find it by searching for trampolines that point to ROM UART functions
print("\n" + "=" * 70)
print("ALL APP TRAMPOLINES (to identify UART functions)")
print("=" * 70)
# Known ROM functions for PHY6222:
# hal_uart_init: 0x00008961? (from trampoline 6)
# Let's just list SRAM trampolines - those are app functions loaded from flash

uart_tramps = []
for addr, target in sorted(app_trampolines.items()):
    # Look for trampolines to SRAM addresses near the UART init
    pass

# Let me look at the function that CALLS uart_init_wrapper
# We need to find who calls 0x11023a0c
print("\n" + "=" * 70)
print("FINDING CALLERS OF UART INIT")
print("=" * 70)

# Search for BL instructions targeting 0x11023a0c
# In Thumb2, BL is F000 Fxxx or F7xx Fxxx (4 bytes)
# The encoding depends on the offset from the call site
# Easier: search the disassembly output

# Let me search the app code for all BL targets
app_code = data[0x20000:0x28f00]
app_base = 0x11020000

# Disassemble the entire app flash region and find BL targets
all_insns = list(md.disasm(data[0x24000:0x28ea0], 0x11024000))
callers_of_uart_init = []
callers_of_handler = []
callers_of_send = []

for insn in all_insns:
    if insn.mnemonic == 'bl':
        try:
            target = int(insn.op_str.lstrip('#'), 0)
            if target == 0x11023a0c or target == 0x11023a0d:
                callers_of_uart_init.append(insn.address)
            # Also check for calls to function at 0x11023a20 (nearby bl target)
        except:
            pass

print(f"  Callers of uart_init_wrapper (0x11023a0c): {[f'0x{a:08x}' for a in callers_of_uart_init]}")

# =============================================================
# Let me also disassemble around 0x11022194 which calls 0x11023a6c
# That was one of the callers we found earlier
print("\n" + "=" * 70)
print("FUNCTION CALLING 0x11023a6c (possibly UART-related init)")
print("=" * 70)

# Find beginning of function containing 0x11022194
# Search backwards for push instruction
foff = 0x22194 - 0x20000 + 0x20000  # file offset
for back in range(0, 200, 2):
    test_off = 0x22194 - back
    word = struct.unpack_from('<H', data, test_off)[0]
    if word & 0xFF00 == 0xB500:  # push {... lr}
        print(f"  Function likely starts at 0x{0x11020000 + test_off - 0x20000:08x}")
        disasm_func(0x11020000 + test_off - 0x20000, max_insns=80, label="caller_of_uart_func")
        break

# =============================================================
# The KEY question: what does the original firmware SEND on UART TX?
# Let me search for hal_uart_send_buff calls
print("\n" + "=" * 70)  
print("SEARCHING FOR hal_uart_send_buff CALLS")
print("=" * 70)

# Look through all trampolines for the one that calls the ROM hal_uart_send_buff
# In PHY6222 ROM, hal_uart_send_buff is typically at a fixed address
# Let me check our own compiled firmware for the address

# From known PHY6222 ROM symbol table:
# hal_uart_send_buff might be 0x00008xxx
# Let me search for trampoline targets that are ROM functions and try them

# Actually, let's search for ALL code paths that write to UART THR register
# UART0 THR = 0x40004000, UART1 THR = 0x40009000
# A write to this address means UART TX

# Search for literal pool entries with UART base addresses
print("\nLiteral pools with UART addresses in SRAM code:")
for seg_run, seg_size, seg_file in APP_SRAM:
    for i in range(seg_file, seg_file + seg_size, 4):
        if i + 4 > len(data):
            break
        val = struct.unpack_from('<I', data, i)[0]
        if val in (0x40004000, 0x40009000):
            sram_addr = seg_run + (i - seg_file)
            uart_idx = "UART0" if val == 0x40004000 else "UART1"
            print(f"  {uart_idx} base at SRAM 0x{sram_addr:08x} (file 0x{i:05x})")

print("\nLiteral pools with UART addresses in FLASH code:")
for seg_flash, seg_size, seg_file in APP_FLASH:
    for i in range(seg_file, seg_file + seg_size, 4):
        if i + 4 > len(data):
            break
        val = struct.unpack_from('<I', data, i)[0]
        if val in (0x40004000, 0x40009000):
            flash_addr = seg_flash + (i - seg_file)
            uart_idx = "UART0" if val == 0x40004000 else "UART1"
            print(f"  {uart_idx} base at FLASH 0x{flash_addr:08x} (file 0x{i:05x})")

# Now disassemble around each UART register reference in SRAM to find TX code
print("\n" + "=" * 70)
print("UART REGISTER ACCESS FUNCTIONS (SRAM)")
print("=" * 70)
for seg_run, seg_size, seg_file in APP_SRAM:
    for i in range(seg_file, seg_file + seg_size, 4):
        if i + 4 > len(data):
            break
        val = struct.unpack_from('<I', data, i)[0]
        if val in (0x40004000, 0x40009000):
            sram_addr = seg_run + (i - seg_file)
            uart_idx = "UART0" if val == 0x40004000 else "UART1"
            # Find the function containing this literal pool entry
            # Search backwards for the function that uses this pool
            # The LDR instruction that loads this pool entry is typically within 
            # ~1024 bytes before the pool entry
            func_start = None
            for back in range(4, 1024, 2):
                test_addr = sram_addr - back
                test_foff, _ = addr_to_file(test_addr)
                if test_foff and test_foff + 2 <= len(data):
                    word = struct.unpack_from('<H', data, test_foff)[0]
                    # Look for push {... lr} as function start
                    if (word & 0xFF00) in (0xB500, 0xB400) or \
                       (word & 0xFFF0) == 0xE920:  # stmdb / push.w
                        func_start = test_addr
                        break
            
            if func_start:
                print(f"\n  Function using {uart_idx} near 0x{sram_addr:08x}:")
                disasm_func(func_start, max_insns=60, label=f"{uart_idx}_user @ 0x{func_start:08x}")
