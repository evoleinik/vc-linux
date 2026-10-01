"""DRAW's source DW pointers enter four DB-encoded NEGDE instructions."""

from dataclasses import replace
from pathlib import Path
import subprocess

import pytest

from tools.build_gwbasic import modules
from translator.image import load_image
from translator.layout import LayoutError
from translator.linked import build_linked_layout
from translator.supplement import build_gwbasic_graphics_layout, emit_supplement
from translator_support.ops_build import InstructionCase, STATES_PER_INSTRUCTION, build_library, load_cases


ROOT = Path(__file__).resolve().parents[1]
GWB = ROOT / "build" / "gwbasic"


@pytest.fixture(scope="module")
def base_layout():
    subprocess.run(["make", "gwbasic"], cwd=ROOT, check=True, capture_output=True)
    return build_linked_layout(load_image(GWB / "GWBASIC.EXE"),
                               [GWB / (module + ".lst") for module in modules()],
                               GWB / "GWBASIC.MAP")


def test_draw_source_table_supplies_all_four_missing_entries(base_layout):
    extra = build_gwbasic_graphics_layout(base_layout)
    expected = {base_layout.labels["ADVGRP::" + name]
                for name in ("DRUP", "DRLEFT", "DRWHHH", "DRWGGG")}
    assert expected == {0xbe5, 0xbec, 0xbf5, 0xc01}
    assert {record.off for record in extra.instructions} == expected
    assert expected.isdisjoint(record.off for record in base_layout.instructions)
    for record in extra.instructions:
        assert record.insn.mnemonic == "neg" and record.insn.op_str == "dx"
        assert bytes(record.insn.bytes) == base_layout.image.data[record.off:record.off + 2]
        assert record.line.expansion == "DRAW DW entry into NEGDE / INS86"


def test_draw_supplement_every_instruction_against_unicorn(base_layout):
    extra = build_gwbasic_graphics_layout(base_layout)
    cases = tuple(InstructionCase(index, "GWBASIC DRAW supplement", record, ())
                  for index, record in enumerate(extra.instructions))
    library = build_library(cases)
    for case in cases:
        failure = library.ops_check_instruction(case.index, STATES_PER_INSTRUCTION)
        assert failure is None, failure.decode() if failure else ""
    assert library.ops_checked_states() == 4 * STATES_PER_INSTRUCTION


def test_draw_supplement_is_in_all_image_unicorn_coverage(base_layout):
    extra = build_gwbasic_graphics_layout(base_layout)
    covered = {(case.record.off, bytes(case.record.insn.bytes)) for case in load_cases()}
    assert {(record.off, bytes(record.insn.bytes)) for record in extra.instructions} <= covered


def test_draw_supplement_emits_only_exact_start_native_steps(base_layout, tmp_path):
    extra = build_gwbasic_graphics_layout(base_layout)
    generated = emit_supplement(extra, "run_gwbasic_graphics")
    assert "rt_fault(" not in generated
    assert "image_bytes[]" not in generated  # the existing image owns the bytes
    assert generated.count("case 0x") == 4
    path = tmp_path / "gwbasic_graphics.c"
    path.write_text(generated)
    subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-fsyntax-only", "-Iruntime", str(path)],
                   cwd=ROOT, check=True, capture_output=True)


def test_draw_table_changed_target_is_rejected(base_layout):
    data = bytearray(base_layout.image.data)
    table = base_layout.labels["ADVGRP::DRWTAB"]
    data[table + 1] ^= 1
    with pytest.raises(LayoutError, match="disagrees with its linked source label"):
        build_gwbasic_graphics_layout(replace(base_layout, image=replace(base_layout.image, data=bytes(data))))


@pytest.mark.parametrize("damage", ("macro", "db-byte", "next-boundary"))
def test_draw_entry_requires_source_macro_bytes_and_next_boundary(base_layout, damage):
    target = base_layout.labels["ADVGRP::DRLEFT"]
    rows = list(base_layout.listing.lines)
    instructions = list(base_layout.instructions)
    if damage == "macro":
        rows = [row for row in rows if not (row.segment == "CSEG" and row.offset == target
                                            and row.mnemonic == "NEGDE")]
    elif damage == "db-byte":
        rows = [row for row in rows if not (row.segment == "CSEG" and row.offset == target + 1
                                            and row.mnemonic == "DB")]
    else:
        instructions = [record for record in instructions if record.off != target + 2]
    damaged = replace(base_layout, listing=replace(base_layout.listing, lines=rows),
                      instructions=instructions)
    with pytest.raises(LayoutError, match="unproved indirect DRAW instruction"):
        build_gwbasic_graphics_layout(damaged)
