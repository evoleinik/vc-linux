"""Gate 2: every distinct linked instruction, in 32 deterministic random states.

Run all coverage:
    .venv/bin/python -m pytest -q -s tests/test_translator_ops.py
Repeat SBB while planting/restoring a carry bug:
    .venv/bin/python -m pytest -q -s tests/test_translator_ops.py -k sbb

Family parameterization only changes reporting/reproduction: the default suite
walks every bytes/image-offset instruction from VC, GW-BASIC and bootLogo's validated listings.
"""

from __future__ import annotations

import time
from types import SimpleNamespace

import pytest
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

from translator_support import ops_build
from translator_support.ops_build import (
    InstructionCase,
    STATES_PER_INSTRUCTION,
    build_library,
    group_cases,
    load_cases,
)


CASES = load_cases()
GROUPS = group_cases(CASES)


@pytest.fixture(scope="module")
def instruction_library():
    beginning = time.monotonic()
    library = build_library(CASES)
    build_seconds = time.monotonic() - beginning
    yield library
    distinct = len({(bytes(case.record.insn.bytes), case.record.off) for case in CASES})
    print(
        f"\nGate 2: {distinct:,} distinct (bytes, image offset) instructions; "
        f"{len(CASES):,} relocation-aware cases; {STATES_PER_INSTRUCTION} states each; "
        f"{library.ops_checked_states():,} successful states, "
        f"{library.ops_checked_divide_errors():,} #DE cases, "
        f"{library.ops_resampled_states():,} code-overlapping states resampled. "
        f"Build {build_seconds:.2f}s; test execution {time.monotonic() - beginning - build_seconds:.2f}s."
    )


@pytest.mark.parametrize("family", tuple(GROUPS))
def test_every_linked_instruction_against_unicorn(instruction_library, family):
    for case in GROUPS[family]:
        failure = instruction_library.ops_check_instruction(case.index, STATES_PER_INSTRUCTION)
        assert failure is None, failure.decode() if failure else ""


def _oracle_corruption_check(monkeypatch, opcode, before, after):
    """Alter only a test wrapper, proving the oracle notices a known error."""
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instruction = next(decoder.disasm(opcode, 0x1234))
    case = InstructionCase(0, "ORACLE_SELF_TEST", SimpleNamespace(
        off=instruction.address, insn=instruction,
    ), ())
    production_emitter = ops_build.emit_instruction_function

    def deliberately_wrong(record, relocations, symbol):
        original = production_emitter(record, relocations, symbol + "_original")
        return original + (
            f"\nint {symbol}(uint16_t loadseg) {{\n{before}\n"
            f"int result = {symbol}_original(loadseg);\n{after}\nreturn result;\n}}\n"
        )

    monkeypatch.setattr(ops_build, "emit_instruction_function", deliberately_wrong)
    library = build_library((case,))
    failure = library.ops_check_instruction(0, STATES_PER_INSTRUCTION)
    assert failure is not None, "The oracle accepted its deliberately corrupted wrapper"
    return failure.decode()


def test_oracle_detects_corrupted_divide_fault_flags(monkeypatch):
    # Sample zero supplies a zero divisor. Corrupt the *live* carry flag after
    # interrupt entry, leaving the already-written interrupt frame untouched.
    failure = _oracle_corruption_check(monkeypatch, b"\xf7\xf3", "", "cpu.cf ^= 1;")
    assert "FLAGS differs" in failure and "compared_mask=ffff" in failure, failure


@pytest.mark.parametrize("opcode", (b"\xec", b"\xed"), ids=("byte", "word"))
def test_oracle_detects_truncated_input_port(monkeypatch, opcode):
    # Restore DX and the expected input register value after the incorrect IN.
    # Only the I/O trace can now reveal the deliberately wrong port argument.
    destination = "cpu.a.l" if opcode == b"\xec" else "cpu.a.x"
    failure = _oracle_corruption_check(
        monkeypatch, opcode,
        "uint16_t saved_dx = cpu.d.x; cpu.d.x &= 0xff;",
        f"cpu.d.x = saved_dx; {destination} = (uint16_t)(saved_dx * 0x1357u + "
        "((uint32_t)saved_dx >> 8) * 0x5du + 0x246bu);",
    )
    assert "port I/O events differ" in failure and "Unicorn IN port=" in failure, failure


@pytest.mark.parametrize("opcode,mutable", [
    (bytes.fromhex("b80000"), (1, 2)),
    (bytes.fromhex("eaffffffff"), (1, 2, 3, 4)),
    (bytes.fromhex("b8030c"), (1,)),
], ids=("patched-ds", "patched-isr", "bootlogo-color"))
def test_oracle_detects_ignored_patched_operand(monkeypatch, opcode, mutable):
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instruction = next(decoder.disasm(opcode, 0x1234))
    record = SimpleNamespace(off=instruction.address, insn=instruction,
                             mutable_offsets=mutable, linked=True)
    case = InstructionCase(0, "SELF_MODIFYING_ORACLE_TEST", record, ())
    production_emitter = ops_build.emit_instruction_function

    def deliberately_ignore_patch(record, relocations, symbol):
        unpatched = SimpleNamespace(off=record.off, insn=record.insn, linked=True)
        return production_emitter(unpatched, relocations, symbol)

    # The reference receives random operand bytes in memory, just as the
    # actual patch stores supply GW-BASIC's DS/saved ISR or bootLogo's colour.
    monkeypatch.setattr(ops_build, "emit_instruction_function", deliberately_ignore_patch)
    library = build_library((case,))
    failure = library.ops_check_instruction(0, STATES_PER_INSTRUCTION)
    assert failure is not None, "The oracle accepted a deliberately ignored live operand"
    assert "AX differs" in failure.decode() or "CS differs" in failure.decode(), failure.decode()
