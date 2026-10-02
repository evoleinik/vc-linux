"""VC 4.05 source boundaries and complete coverage by the Unicorn gate."""

from dataclasses import replace
from pathlib import Path
import subprocess

import pytest

from translator.image import load_image
from translator.layout import LayoutError, build_layout
from translator.listing import parse_listing
from translator.vc405 import build_vc405_layout
from translator_support.ops_build import InstructionCase, STATES_PER_INSTRUCTION, build_library, load_cases


ROOT = Path(__file__).resolve().parents[1]
COUNTS = {"VC.COM": 22692, "VCSETUP.COM": 4879}


@pytest.fixture(scope="module")
def layouts():
    built = subprocess.run(["make", "vc405"], cwd=ROOT, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert built.returncode == 0, built.stdout
    return {name: build_vc405_layout(load_image(ROOT / "build/vc405" / name),
                                     parse_listing(ROOT / "build/vc405" / (name + ".lst"),
                                                   physical_lines=True))
            for name in COUNTS}


@pytest.mark.parametrize("name", COUNTS)
def test_complete_program_has_listing_proved_instruction_boundaries(layouts, name):
    layout = layouts[name]
    assert len(layout.instructions) == COUNTS[name]
    for record in layout.instructions:
        assert bytes(record.insn.bytes) == layout.image.data[record.off:record.off + record.insn.size]
    assert layout.image.relocations == []


@pytest.mark.parametrize("name", COUNTS)
def test_every_vc405_instruction_is_present_in_the_unicorn_gate(layouts, name):
    covered = {(case.record.off, bytes(case.record.insn.bytes), case.record.mutable_offsets)
               for case in load_cases()}
    expected = {(record.off, bytes(record.insn.bytes), record.mutable_offsets)
                for record in layouts[name].instructions}
    assert expected <= covered, f"{name}: {len(expected - covered)} instructions absent from Unicorn"


def test_loop_expansion_preserves_flags_and_both_control_paths(layouts):
    layout = layouts["VC.COM"]
    expanded = [record for record in layout.instructions
                if (record.line.expansion or "").startswith("TASM JUMPS")]
    assert len(expanded) == 3
    loop, untaken, taken = expanded
    assert loop.insn.mnemonic == "loop"
    assert untaken.insn.mnemonic == taken.insn.mnemonic == "jmp"
    assert loop.insn.operands[0].imm == taken.off
    assert untaken.insn.operands[0].imm == taken.off + taken.insn.size
    assert taken.insn.operands[0].imm == layout.labels["F39_52"]
    assert not any(record.insn.eflags for record in expanded)


def test_loop_expansion_each_instruction_matches_unicorn(layouts):
    expanded = [record for record in layouts["VC.COM"].instructions
                if (record.line.expansion or "").startswith("TASM JUMPS")]
    cases = tuple(InstructionCase(index, "VC405.COM", record, ())
                  for index, record in enumerate(expanded))
    library = build_library(cases)
    for case in cases:
        failure = library.ops_check_instruction(case.index, STATES_PER_INSTRUCTION)
        assert failure is None, failure.decode() if failure else ""


def test_tasm_extension_requires_its_exact_bytes_and_source_target(layouts):
    image = layouts["VC.COM"].image
    listing = parse_listing(ROOT / "build/vc405/VC.COM.lst", physical_lines=True)
    row = next(row for row in listing.lines if row.mnemonic == "LOOP" and row.byte_count == 7)
    with pytest.raises(LayoutError, match="instruction length mismatch"):
        build_layout(image, listing)
    for index in range(7):
        raw = list(row.bytes)
        raw[index] ^= 1
        changed = replace(row, bytes=tuple(raw))
        bad = replace(listing, lines=[changed if item is row else item for item in listing.lines])
        with pytest.raises(LayoutError, match="unproven TASM JUMPS"):
            build_vc405_layout(image, bad)
    changed = replace(row, operands="Exec")
    bad = replace(listing, lines=[changed if item is row else item for item in listing.lines])
    with pytest.raises(LayoutError, match="unproven TASM JUMPS"):
        build_vc405_layout(image, bad)


@pytest.mark.parametrize("name", COUNTS)
def test_changed_image_cannot_bypass_listing_checks(layouts, name):
    layout = layouts[name]
    raw = bytearray(layout.image.data)
    row = next(record for record in layout.instructions if record.line.is_instruction and record.off > 32)
    raw[row.off] ^= 1
    with pytest.raises(LayoutError, match="byte mismatch|linked-byte match"):
        build_vc405_layout(replace(layout.image, data=bytes(raw)),
                           parse_listing(ROOT / "build/vc405" / (name + ".lst"), physical_lines=True))


def test_vc405_line_numbers_are_physical_listing_lines(layouts):
    path = ROOT / "build/vc405/VC.COM.lst"
    physical = path.read_bytes().decode("utf-8", errors="replace").split("\n")
    listing = parse_listing(path, physical_lines=True)
    for row in listing.lines:
        assert physical[row.lineno - 1].removesuffix("\r")[32:] == row.source
    # The original DOS control characters occur inside quoted source data;
    # they must not move source locations for following RETF instructions.
    assert any(len(line.splitlines()) > 1 for line in physical)
