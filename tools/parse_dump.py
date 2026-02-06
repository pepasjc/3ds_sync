import struct
import sys

EXCEPTION_TYPES = {
    0: "Prefetch Abort",
    1: "Data Abort",
    2: "Undefined Instruction",
    3: "VFP Exception",
}

REG_NAMES = [
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
    "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc",
    "cpsr", "dfsr", "ifsr", "far",
]

with open(sys.argv[1], "rb") as f:
    data = f.read()

# Header
magic1, magic2 = struct.unpack_from("<II", data, 0)
print(f"Magic: {magic1:#010x} {magic2:#010x}")
assert magic1 == 0xDEADC0DE and magic2 == 0xDEADCAFE, "Not a Luma3DS crash dump"

ver_major, ver_minor = struct.unpack_from("<HH", data, 8)
processor, core_id = struct.unpack_from("<HH", data, 12)
print(f"Luma3DS exception dump v{ver_major}.{ver_minor}, ARM{processor}, core {core_id}")

# Read the exception info
# The layout after the initial header varies. Let me try different offsets.
# Based on Luma3DS source: after the 16-byte header comes:
# nbRegisters(u32), codeDumpSize(u32), stackDumpSize(u32), additionalDataSize(u32)
nb_regs, code_size, stack_size, additional_size = struct.unpack_from("<IIII", data, 16)
print(f"Registers: {nb_regs}, Code dump: {code_size}B, Stack dump: {stack_size}B, Additional: {additional_size}B")

# Exception type might be stored differently. Let's look at what nb_regs=3 means...
# Actually nb_regs=3 seems wrong for register count. This might be the exception type.
# Let me try: offset 16 = exceptionType, offset 20 = codeDumpSize, etc.
exc_type = nb_regs
code_size_2 = code_size
stack_size_2 = stack_size
additional_size_2 = additional_size

print(f"\nException type: {exc_type} ({EXCEPTION_TYPES.get(exc_type, 'Unknown')})")
print(f"Code dump: {code_size_2} bytes")
print(f"Stack dump: {stack_size_2} bytes")
print(f"Additional data: {additional_size_2} bytes")

# Registers start after header. Let me try offset 0x28 (40 bytes header)
# based on hex inspection showing register-like values there
reg_offset = 0x28
print(f"\n--- Registers (from offset 0x{reg_offset:02x}) ---")
for i in range(min(20, (len(data) - reg_offset) // 4)):
    val = struct.unpack_from("<I", data, reg_offset + i * 4)[0]
    name = REG_NAMES[i] if i < len(REG_NAMES) else f"r{i}"
    print(f"  {name:5s} = 0x{val:08X}")
    if name == "pc":
        pc = val
    if name == "lr":
        lr = val

print(f"\n--- Key Addresses ---")
print(f"  PC (crash location): 0x{pc:08X}")
print(f"  LR (return address): 0x{lr:08X}")

# Code dump follows registers
code_offset = reg_offset + 20 * 4  # after ~20 registers
print(f"\n--- Code dump at offset 0x{code_offset:02x} (first 64 bytes) ---")
for i in range(0, min(64, len(data) - code_offset), 4):
    val = struct.unpack_from("<I", data, code_offset + i)[0]
    addr_guess = pc - code_size_2 // 2 + i if code_size_2 > 0 else i
    print(f"  {addr_guess:#010x}: {val:08x}")
