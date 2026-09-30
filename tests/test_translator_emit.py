"""Small emitter/runtime contract tests in addition to the Unicorn gates."""

from __future__ import annotations

import ctypes
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from translator.emit import (  # noqa: E402
    EmissionError, emit_image, emit_instruction_function, emit_prelude,
)
from translator.image import LoadedImage  # noqa: E402


def instruction(raw: str, off: int = 0):
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    data = bytes.fromhex(raw)
    decoded = next(decoder.disasm(data, off))
    assert decoded.size == len(data)
    return SimpleNamespace(off=off, insn=decoded, line=None)


def synthetic_layout():
    # Source-supplied boundaries are explicit, including an entry inside a PROC.
    sequences = [
        [(0, "b80100"), (3, "e80300"), (6, "40"), (7, "c3"),
         (8, "90"), (9, "83c002"), (12, "c3"), (64, "e9c6ff")],
        [(15, "90"), (16, "b90200"), (19, "49"), (20, "75fd"), (22, "c3")],
    ]
    chunks, records, module = [], [], bytearray(67)
    for index, sequence in enumerate(sequences):
        chunk = SimpleNamespace(index=index, name=f"contract_{index}", instructions=[])
        chunks.append(chunk)
        for off, raw in sequence:
            record = instruction(raw, off)
            record.chunk = chunk
            chunk.instructions.append(record)
            records.append(record)
            module[off:off + record.insn.size] = record.insn.bytes
    return SimpleNamespace(
        image=LoadedImage(Path("synthetic.com"), bytes(module)),
        chunks=chunks, instructions=records,
    )


def test_relocation_immediates_and_rejection():
    normal = emit_instruction_function(instruction("b83412", 100), [101], "mov_segment")
    assert "(uint16_t)(0x1234 + loadseg)" in normal
    unsorted = emit_instruction_function(instruction("b83412", 100), (900, 101), "unordered")
    assert "(uint16_t)(0x1234 + loadseg)" in unsorted
    far = emit_instruction_function(instruction("9a34127856", 100), [103], "far_call")
    assert "(uint16_t)(0x5678 + loadseg)" in far
    assert "target_ip = 0x1234u" in far
    relative = emit_instruction_function(instruction("e81200", 100), [101], "relocated_call")
    assert "displacement = (uint16_t)(0x0012 + loadseg)" in relative
    assert "cpu.ip = target_ip;" in relative
    # Address displacements, opcodes and partial words are never silently
    # mistaken for segment-bearing immediates.
    for raw, relocation in (("a13412", 101), ("b83412", 100),
                            ("b83412", 102), ("b83412", 99),
                            ("9a34127856", 101)):
        with pytest.raises(EmissionError, match="relocation at image offset"):
            emit_instruction_function(instruction(raw, 100), [relocation], "bad")


@pytest.mark.parametrize("raw", ["f4", "0f0b", "6689c0"])
def test_unsupported_bytes_have_explicit_offset_fault(raw):
    source = emit_instruction_function(instruction(raw, 0x123), [], "unsupported")
    assert 'rt_fault("image offset 0x123:' in source
    assert bytes.fromhex(raw).hex(" ") in source


def test_codegen_exact_entries_and_static_calls():
    layout = synthetic_layout()
    source = emit_image(layout, "synthetic.com", "contract_image")
    for record in layout.instructions:
        assert f"case 0x{record.off:x}u: goto L_{record.off:x};" in source
    assert "goto L_9;" in source
    assert "size_t mid = lo + (hi - lo) / 2;" in source
    assert "starts[lo].off != off) return -1;" in source


@pytest.fixture(scope="module")
def contract_library(tmp_path_factory):
    directory = tmp_path_factory.mktemp("translator_contract")
    generated = emit_image(synthetic_layout(), "synthetic.com", "contract_image")
    for index, raw in enumerate(("e200", "e100", "e000", "e300")):
        generated += emit_instruction_function(instruction(raw, 32), [], f"loop_{index}")
    for name, raw in (("jump", "e91000"), ("call", "e81000"),
                      ("ret", "c20800"), ("enter", "c8060000")):
        generated += emit_instruction_function(instruction(raw, 0x100), [0x101], f"rel_{name}")
    generated += r'''
#include <stdlib.h>
#include <string.h>
static unsigned yields;
static uint32_t noted[16];
static unsigned notes;
void cpu_trace_write(uint32_t address) { if (notes < 16) noted[notes++] = address; }
void rt_yield(void) { yields++; rt_budget = 100; }
_Noreturn void rt_fault(const char *fmt, ...) { (void)fmt; abort(); }
uint8_t port_in8(uint16_t p) { return (uint8_t)p; }
uint16_t port_in16(uint16_t p) { return p; }
void port_out8(uint16_t p, uint8_t v) { (void)p; (void)v; }
void port_out16(uint16_t p, uint16_t v) { (void)p; (void)v; }

#define CHECK(c) do { if (!(c)) return "failed: " #c; } while (0)
const char *contract_control(void) {
    memset(&cpu, 0, sizeof cpu);
    cpu.cs = 0x2300; cpu.ss = 0x6000; cpu.sp = 0x8000;
    tr_wr16(cpu.ss, cpu.sp, 0x7788);
    cpu.ip = 0x400;
    rt_budget = 100;
    CHECK(contract_image.run(0, 0x2340) == 0);
    CHECK(cpu.a.x == 3 && cpu.ip == 0x406 && cpu.sp == 0x8000);
    CHECK(contract_image.run(6, 0x2340) == 0);
    CHECK(cpu.a.x == 4 && cpu.ip == 0x7788 && cpu.sp == 0x8002);
    Cpu saved = cpu;
    int32_t old_budget = rt_budget;
    unsigned old_notes = notes;
    CHECK(contract_image.run(1, 0x9876) == -1);
    CHECK(contract_image.run(23, 0x9876) == -1);
    CHECK(contract_image.run(0xffffffffu, 0x9876) == -1);
    CHECK(!memcmp(&cpu, &saved, sizeof cpu));
    CHECK(rt_budget == old_budget && notes == old_notes);
    cpu.sp = 0x8000; rt_budget = 0; yields = 0;
    CHECK(contract_image.run(16, 0x2340) == 0);
    CHECK(cpu.c.x == 0 && cpu.ip == 0x7788 && yields == 1);
    /* Paragraph aliases can force IP wrap even for an in-chunk target. */
    cpu.cs = 0x2344; cpu.ip = 0; cpu.sp = 0x8000; cpu.a.x = 0x1234;
    CHECK(contract_image.run(64, 0x2340) == 0);
    CHECK(cpu.ip == 0xffc9 && cpu.a.x == 0x1234 && cpu.sp == 0x8000);
    cpu.cs = 0x1341; cpu.ip = 0xffff; cpu.c.x = 0x1234;
    CHECK(contract_image.run(15, 0x2340) == 0);
    CHECK(cpu.ip == 0 && cpu.c.x == 0x1234 && cpu.sp == 0x8000);
    cpu.cs = 0x2300;
    int (*loops[])(uint16_t) = {loop_0, loop_1, loop_2, loop_3};
    for (unsigned i = 0; i < 4; ++i) {
        cpu.c.x = 1; cpu.zf = 0; rt_budget = 0; yields = 0;
        CHECK(loops[i](0x2340) == 0);
        CHECK(yields == 1 && cpu.c.x == (i == 3 ? 1 : 0));
        CHECK(cpu.zf == 0 && cpu.ip == 0x422);
    }
    CHECK(rel_jump(0x2340) == 0 && cpu.ip == 0x2853);
    CHECK(rel_call(0x2340) == 0 && cpu.ip == 0x2853 && cpu.sp == 0x7ffe);
    CHECK(tr_rd16(cpu.ss, 0x7ffe) == 0x503);
    cpu.sp = 0x8000; tr_wr16(cpu.ss, cpu.sp, 0x7777);
    CHECK(rel_ret(0x2340) == 0 && cpu.ip == 0x7777 && cpu.sp == 0xa34a);
    cpu.sp = 0x8000; cpu.bp = 0x1234;
    CHECK(rel_enter(0x2340) == 0 && cpu.bp == 0x7ffe && cpu.sp == 0x5cb8);
    CHECK(tr_rd16(cpu.ss, 0x7ffe) == 0x1234 && cpu.ip == 0x504);
    return NULL;
}

const char *contract_flags_and_interrupt(void) {
    for (unsigned f = 0; f < 0x10000; ++f) {
        flags_set((uint16_t)f);
        CHECK(flags_get() == ((f & 0x7fd5u) | 2u));
    }
    memset(&cpu, 0, sizeof cpu);
    cpu.cs = 0x2345; cpu.ss = 0x5000; cpu.sp = 1;
    flags_set(0x7fd7);
    mem[0x80] = 0x78; mem[0x81] = 0x56;
    mem[0x82] = 0x34; mem[0x83] = 0x12;
    mem[0x50000] = 0xa5;
    notes = 0;
    cpu_int(0x20, 0xabcd);
    CHECK(cpu.cs == 0x1234 && cpu.ip == 0x5678 && cpu.sp == 0xfffb);
    CHECK(cpu.ifl == 0 && cpu.tf == 0 && flags_get() == 0x7cd7);
    CHECK(mem[0x5ffff] == 0xd7 && mem[0x60000] == 0x7f);
    CHECK(mem[0x50000] == 0xa5);
    CHECK(tr_rd16(cpu.ss, 0xfffd) == 0x2345);
    CHECK(tr_rd16(cpu.ss, 0xfffb) == 0xabcd);
    CHECK(notes == 6 && noted[0] == 0x5ffff && noted[1] == 0x60000);
    return NULL;
}
'''
    source = directory / "contract.c"
    source.write_text(generated)
    library = directory / "contract.so"
    result = subprocess.run(
        ["gcc", "-O1", "-Wall", "-Werror", "-shared", "-fPIC",
         "-DCPU_TRACE_WRITES", "-I", str(ROOT / "runtime"),
         str(source), str(ROOT / "runtime/cpu.c"), "-o", str(library)],
        text=True, capture_output=True, timeout=60,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return ctypes.CDLL(str(library))


@pytest.mark.parametrize("function", ["contract_control", "contract_flags_and_interrupt"])
def test_compiled_runtime_contract(contract_library, function):
    probe = getattr(contract_library, function)
    probe.restype = ctypes.c_char_p
    message = probe()
    assert message is None, message.decode()
