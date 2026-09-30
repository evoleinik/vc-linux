"""Run VC's original PutTime machine code on every input and compare both C translations.

The original bytes run on a bare 8086 CPU model (unicorn). No DOS, no screen.
Exit 1 on the first input where a translation differs.
"""
import ctypes, struct, sys
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16
from unicorn.x86_const import *

lib_path = sys.argv[1] if len(sys.argv) > 1 else "./libpt.so"
img = open("harness.com", "rb").read()
country, ent, outbuf, done = struct.unpack_from("<4H", img, 2)
SEG, BASE = 0x1000, 0x10000

uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x100000)
uc.mem_write(BASE + 0x100, img)

def run_original(t, fmt, sep):
    uc.mem_write(BASE + country + 17, bytes([fmt]))
    uc.mem_write(BASE + country + 13, sep.encode())
    uc.mem_write(BASE + ent + 1, struct.pack("<H", t))
    uc.mem_write(BASE + outbuf, b"\xee" * 16)
    for r, v in ((UC_X86_REG_CS, SEG), (UC_X86_REG_DS, SEG), (UC_X86_REG_ES, SEG), (UC_X86_REG_SS, SEG),
                 (UC_X86_REG_SP, 0xFFFE), (UC_X86_REG_AX, 0x1111), (UC_X86_REG_BX, 0x2222),
                 (UC_X86_REG_CX, 0x3333), (UC_X86_REG_DX, 0x4444), (UC_X86_REG_EFLAGS, 0x0002)):
        uc.reg_write(r, v)
    uc.emu_start(BASE + 0x100, BASE + done)
    regs = [uc.reg_read(r) for r in (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX)]
    assert regs == [0x1111, 0x2222, 0x3333, 0x4444], regs
    return bytes(uc.mem_read(BASE + outbuf, 16)), uc.reg_read(UC_X86_REG_DI) - outbuf

lib = ctypes.CDLL(lib_path)
buf = ctypes.create_string_buffer(16)
def run_lib(fn, *args):
    n = fn(*args, buf)
    return buf.raw, n

checked, samples = 0, {}
for fmt in (0, 1):
    for t in range(0x10000):
        want = run_original(t, fmt, ":")
        mech = run_lib(lib.run_mech, t, fmt, b":"[0], country, ent, outbuf)
        plain = run_lib(lib.run_c, t, fmt, b":"[0])
        for name, got in (("instruction-level C", mech), ("plain C", plain)):
            if got != want:
                print(f"FAIL {name}: time=0x{t:04X} fmt={fmt}\n  original {want[0][:want[1]+1]!r} len={want[1]}\n  got      {got[0][:max(got[1],0)+1]!r} len={got[1]}")
                sys.exit(1)
        checked += 1
        if t in (0x0000, 0x6000 | 30 << 5 | 7, 0xA2A5, 0xFFFF):
            samples[(fmt, t)] = want[0][: want[1]].decode()
print(f"OK: {checked} inputs, original machine code == instruction-level C == plain C")
for (fmt, t), s in samples.items():
    print(f"  {'24h' if fmt else '12h'} 0x{t:04X} -> {s!r}")
