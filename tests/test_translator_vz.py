"""Linked COM placement and VZ's source-proved instruction boundaries."""

from dataclasses import replace
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

from translator.emit import emit_image
from translator.image import load_image
from translator.layout import LayoutError
from translator.linked import build_linked_layout, parse_map
from translator_support.ops_build import InstructionCase, build_library, load_cases


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def vz_layout():
    from tools.build_vz import modules
    subprocess.run(["make", "vz"], cwd=ROOT, check=True, capture_output=True)
    directory = ROOT / "build/vz"
    return build_linked_layout(load_image(directory / "VZ.COM"),
                               [directory / (module + ".lst") for module in modules()],
                               directory / "VZ.MAP")


def test_vz_proves_dynamic_macro_instruction_variants(vz_layout):
    by_offset = {record.off: record for record in vz_layout.instructions}
    site = by_offset[vz_layout.labels["MACRO::opcode"]]
    assert bytes(site.insn.bytes) == b"\0\0"
    assert [record.insn.mnemonic for record in site.variants] == ["int", "jmp"]
    assert site.variants[0].mutable_offsets == (1,)
    assert site.variants[1].insn.operands[0].imm == vz_layout.labels["MACRO::to_int"]
    assert set(range(site.off, site.off + 2)) <= set(vz_layout.mutable_offsets)
    assert len(vz_layout.instructions) > 15000


def test_vz_oracle_covers_every_emitted_start(vz_layout):
    covered = {(case.record.off, bytes(case.record.insn.bytes), case.record.mutable_offsets)
               for case in load_cases()}
    expected = {(form.off, bytes(form.insn.bytes), form.mutable_offsets)
                for record in vz_layout.instructions for form in (record.variants or (record,))}
    assert expected <= covered


def test_vz_macro_variants_reject_changed_writer_proof(vz_layout):
    from translator.vz import prove_macro_variants
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    data = bytearray(vz_layout.image.data)
    data[vz_layout.labels["MACRO::smac_int"] + 9] ^= 1
    damaged = replace(vz_layout.image, data=bytes(data))
    with pytest.raises(LayoutError, match="unproven VZ macro"):
        prove_macro_variants(damaged, vz_layout.instructions, vz_layout.labels, decoder)


def test_vz_macro_variant_guard_rejects_unpatched_zero_word(vz_layout):
    site = next(record for record in vz_layout.instructions if record.variants)
    # Keep the on-disk zero DW intact. It is neither permitted runtime form.
    invalid = replace(site, mutable_offsets=())
    library = build_library((InstructionCase(0, "VZ_BAD_VARIANT", invalid, ()),))
    failure = library.ops_check_instruction(0, 32)
    assert failure and "unsupported bytes in source-proved instruction variant" in failure.decode()


def test_vz_das_matches_unicorn():
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instruction = next(decoder.disasm(b"\x2f", 0x1234))
    record = SimpleNamespace(off=instruction.address, insn=instruction)
    library = build_library((InstructionCase(0, "VZ_DAS", record, ()),))
    failure = library.ops_check_instruction(0, 1024)
    assert failure is None, failure.decode() if failure else ""


def _assemble_com(directory, sources):
    for module, source in sources.items():
        (directory / (module + ".ASM")).write_text(source)
        subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-Zm", "-Sg", "-Sa",
                        f"-Fl={module}.lst", f"-Fo={module}.OBJ", module + ".ASM"],
                       cwd=directory, check=True, capture_output=True)
    command = [str(ROOT / "tools/jwlink/jwlink"), "format", "dos", "com"]
    for module in sources:
        command.extend(("file", module + ".OBJ"))
    subprocess.run(command + ["name", "SMALL.COM", "option", "map=SMALL.MAP,verbose"],
                   cwd=directory, check=True, capture_output=True)
    return (load_image(directory / "SMALL.COM"),
            [directory / (module + ".lst") for module in sources], directory / "SMALL.MAP")


@pytest.fixture
def linked_com(tmp_path):
    headers = """WSEG SEGMENT WORD PUBLIC 'WORK'
WSEG ENDS
DSEG SEGMENT WORD PUBLIC 'DATA'
DSEG ENDS
CSEG SEGMENT BYTE PUBLIC 'CODE'
CSEG ENDS
FLATGROUP GROUP WSEG,DSEG,CSEG
ASSUME CS:FLATGROUP,DS:FLATGROUP
IFDEF NEVER
ABSENT SEGMENT BYTE PUBLIC 'CODE'
ABSENT ENDS
ENDIF
"""
    return _assemble_com(tmp_path, {
        "A": headers + """WSEG SEGMENT
ORG 100h
PUBLIC entry
EXTRN finish:NEAR
entry: MOV AX,OFFSET FLATGROUP:first
CALL finish
RET
WSEG ENDS
DSEG SEGMENT
PUBLIC first
first DW OFFSET FLATGROUP:finish
DB 7
DB (($ - first) AND 1) DUP (90h)
DSEG ENDS
END entry
""",
        "B": headers + """CSEG SEGMENT
PUBLIC finish
finish PROC NEAR
INC AX
RET
finish ENDP
CSEG ENDS
END
""",
    })


def test_linked_com_places_all_classes_from_psp_origin(linked_com, tmp_path):
    image, listings, map_path = linked_com
    layout = build_linked_layout(image, listings, map_path)
    link = parse_map(map_path)
    assert not image.is_exe
    assert layout.labels["A::entry"] == 0
    assert layout.labels["A::first"] == link.contributions[("A", "DSEG")].address - 0x100
    assert layout.labels["B::finish"] == link.contributions[("B", "CSEG")].address - 0x100
    assert [record.insn.mnemonic for record in layout.instructions] == ["mov", "call", "ret", "inc", "ret"]
    assert layout.instructions[1].insn.operands[0].imm == layout.labels["B::finish"]
    generated = tmp_path / "small.c"
    generated.write_text(emit_image(layout, "SMALL.COM", "small_image"))
    subprocess.run(["cc", "-fsyntax-only", "-I" + str(ROOT / "runtime"), str(generated)],
                   check=True, capture_output=True)


def test_linked_com_rejects_wrong_group_data_fixup(linked_com):
    image, listings, map_path = linked_com
    link = parse_map(map_path)
    data = bytearray(image.data)
    data[link.contributions[("A", "DSEG")].address - 0x100] ^= 1
    image.data = bytes(data)
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_linked_layout(image, listings, map_path)


def test_linked_com_rejects_missing_non_code_instruction(linked_com):
    image, listings, map_path = linked_com
    original = listings[0].read_text()
    lines = [line for line in original.splitlines(keepends=True) if "entry: MOV AX" not in line]
    assert len(lines) + 1 == len(original.splitlines())
    listings[0].write_text("".join(lines))
    with pytest.raises(LayoutError, match="uncovered byte"):
        build_linked_layout(image, listings, map_path)
