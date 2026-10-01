"""Gate 2: every distinct linked instruction, in 32 deterministic random states.

Run all coverage:
    .venv/bin/python -m pytest -q -s tests/test_translator_ops.py
Repeat SBB while planting/restoring a carry bug:
    .venv/bin/python -m pytest -q -s tests/test_translator_ops.py -k sbb

Family parameterization only changes reporting/reproduction: the default suite
walks every bytes/image-offset instruction from VC, GW-BASIC, bootLogo, Rogue
and VZ's validated listing/map front ends (including Rogue's complete CRT).
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


def test_oracle_executes_every_software_interrupt_vector():
    """DoINTR adds reserved vectors too, notably Unicorn's INT 6 special case."""
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    cases = []
    for vector in range(256):
        insn = next(decoder.disasm(bytes([0xcd, vector]), 0x1234))
        cases.append(InstructionCase(vector, "SOFTWARE_INTERRUPT_ORACLE_TEST", SimpleNamespace(
            off=insn.address, insn=insn,
        ), ()))
    library = build_library(tuple(cases))
    for vector in range(256):
        failure = library.ops_check_instruction(vector, STATES_PER_INSTRUCTION)
        assert failure is None, failure.decode() if failure else ""


def test_oracle_executes_patched_software_interrupt_vectors():
    """VZ's live vector must also reach Rogue's reserved INT 6 adapter."""
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instruction = next(decoder.disasm(b"\xcd\x00", 0x1234))
    record = SimpleNamespace(off=instruction.address, insn=instruction,
                             mutable_offsets=(1,), linked=True)
    library = build_library((InstructionCase(0, "PATCHED_INTERRUPT_ORACLE_TEST", record, ()),))
    # The deterministic random stream reaches live CD 06 at sample 54, even
    # though the original template and its metadata specify INT 0.
    failure = library.ops_check_instruction(0, 1024)
    assert failure is None, failure.decode() if failure else ""


def test_vz_oracle_includes_both_macro_variants():
    cases = [case for case in CASES if case.image_name == "VZ.COM"]
    assert len(cases) > 20000
    variants = [case for case in cases if case.record.variants]
    assert [case.name for case in variants] == ["int", "jmp"]
    # A zero DW is not a real third execution path; &i patches it first.
    assert all(bytes(case.record.insn.bytes) != b"\0\0" for case in variants)
    small = tuple(InstructionCase(index, case.image_name, case.record, case.relocations)
                  for index, case in enumerate(variants))
    library = build_library(small)
    for case in small:
        failure = library.ops_check_instruction(case.index, STATES_PER_INSTRUCTION)
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


def test_oracle_detects_corrupted_software_int6_flags(monkeypatch):
    failure = _oracle_corruption_check(monkeypatch, b"\xcd\x06", "", "cpu.cf ^= 1;")
    assert "FLAGS differs" in failure and "compared_mask=ffff" in failure, failure


def test_int6_oracle_adapter_does_not_accept_an_actual_invalid_opcode(monkeypatch):
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    insn = next(decoder.disasm(b"\x0f\x0b", 0x1234))  # UD2, genuinely invalid.
    case = InstructionCase(0, "INVALID_OPCODE_ORACLE_TEST", SimpleNamespace(off=insn.address, insn=insn), ())
    original_metadata = ops_build._metadata

    def mislabeled_interrupt(case):
        metadata = original_metadata(case)
        assert ".special=0, .vector=0" in metadata
        return metadata.replace(".special=0, .vector=0", ".special=OPS_INT, .vector=6")

    # A deliberately incorrect generated no-op makes the reference execute.
    # Even mislabeled metadata must not turn UD2 into the INT 6 workaround.
    monkeypatch.setattr(ops_build, "_metadata", mislabeled_interrupt)
    monkeypatch.setattr(ops_build, "emit_instruction_function",
                        lambda record, relocations, symbol: f"int {symbol}(uint16_t loadseg) {{ return 0; }}\n")
    library = build_library((case,))
    failure = library.ops_check_instruction(0, STATES_PER_INSTRUCTION)
    assert failure is not None and b"UC_ERR_INSN_INVALID" in failure, failure


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
    (bytes.fromhex("cd00"), (1,)),
], ids=("patched-ds", "patched-isr", "bootlogo-color", "vz-interrupt"))
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
