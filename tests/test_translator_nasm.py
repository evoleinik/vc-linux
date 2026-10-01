"""NASM's independent boundaries/linked bytes and every bootLogo instruction."""

from dataclasses import replace
from pathlib import Path
import re
import subprocess

import pytest

from translator.emit import emit_image
from translator.image import load_image
from translator.layout import LayoutError
from translator.listing import ListingError
from translator.nasm import build_nasm_layout, parse_nasm_listing
from translator_support.bootlogo_oracle import run_logo


ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / "build" / "bootlogo"
NASM = ROOT / "tools" / "nasm" / "nasm"


def assemble(tmp_path, source):
    path = tmp_path / "sample.asm"
    path.write_text(source)
    subprocess.run([str(NASM), "-f", "bin", "-LefFt", "-l", "sample.lst",
                    "-o", "sample.com", path.name], cwd=tmp_path, check=True,
                   capture_output=True)
    return load_image(tmp_path / "sample.com"), parse_nasm_listing(tmp_path / "sample.lst")


@pytest.fixture(scope="module")
def logo_layout():
    subprocess.run(["make", "bootlogo"], cwd=ROOT, check=True, capture_output=True)
    return build_nasm_layout(load_image(BOOT / "LOGO.COM"), parse_nasm_listing(BOOT / "LOGO.lst"))


def test_nasm_bootlogo_exact_boundaries_and_linked_bytes(logo_layout):
    assert len(logo_layout.image.data) == 503
    assert len(logo_layout.instructions) == 217
    assert logo_layout.labels["start"] == 0
    assert logo_layout.labels["command_fd"] == 0x176
    assert logo_layout.labels["sin_table"] == 0x1d7
    for record in logo_layout.instructions:
        assert bytes(record.insn.bytes) == logo_layout.image.data[record.off:record.off + record.insn.size]
    generated = emit_image(logo_layout, "LOGO.COM", "image_bootlogo")
    assert "rt_fault(" not in generated
    assert ".name = \"LOGO.COM\", .is_exe = 0" in generated


def test_nasm_recovers_both_overlapping_skip_instructions(logo_layout):
    recovered = [record for record in logo_layout.instructions if not record.line.is_instruction]
    assert [(record.off, bytes(record.insn.bytes)) for record in recovered] == [
        (0x175, bytes.fromhex("ba31c0")), (0x187, bytes.fromhex("bab001")),
    ]
    starts = {record.off for record in logo_layout.instructions}
    assert {0x175, 0x176, 0x187, 0x188} <= starts
    assert not starts.intersection(range(0x120, 0x141))  # command table remains data
    assert not starts.intersection(range(0x1d7, 0x1f7))  # sine table remains data


def test_nasm_color_patch_is_live_and_precisely_masked(logo_layout):
    patches = [record for record in logo_layout.instructions if record.mutable_offsets]
    assert len(patches) == 1
    assert patches[0].off == 0x1ae
    assert patches[0].mutable_offsets == (1,)
    assert logo_layout.mutable_offsets == (0x1af,)


def test_nasm_rebuild_is_byte_reproducible(logo_layout, tmp_path):
    subprocess.run([str(NASM), "-Dcom_file=1", "-f", "bin", "-LefFt",
                    "-l", str(tmp_path / "LOGO.lst"), "-o", str(tmp_path / "LOGO.COM"),
                    "third_party/bootlogo/bootlogo.asm"], cwd=ROOT, check=True, capture_output=True)
    assert (tmp_path / "LOGO.COM").read_bytes() == logo_layout.image.data
    assert (tmp_path / "LOGO.lst").read_bytes() == (BOOT / "LOGO.lst").read_bytes()


def test_nasm_active_conditionals_macros_and_long_data(tmp_path):
    image, listing = assemble(tmp_path, """cpu 8086
%define com_file 1
%if com_file
org 0x100
%else
org 0x7c00
mov ax,0xffff
%endif
%macro LOAD 1
mov ax,%1
%endmacro
start: LOAD tail
ret
tail: db 'a;this is not a comment;z'
times 17 db 0xab
dw start,tail
""")
    layout = build_nasm_layout(image, listing)
    assert [(record.off, record.insn.mnemonic) for record in layout.instructions] == [(0, "mov"), (3, "ret")]
    assert layout.instructions[0].insn.operands[1].imm == 0x104
    assert b"a;this is not a comment;z" in image.data
    assert all(bytes(row.bytes) == image.data[row.offset - 0x100:row.offset - 0x100 + row.byte_count]
               for row in listing.lines)


@pytest.mark.parametrize("offset", (0, 0x9, 0x122, 0x1df), ids=("opcode", "origin-fixup", "command-table", "sine-table"))
def test_nasm_rejects_changed_linked_bytes(logo_layout, offset):
    raw = bytearray(logo_layout.image.data)
    raw[offset] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_nasm_layout(replace(logo_layout.image, data=bytes(raw)), logo_layout.listing)


def test_nasm_rejects_missing_instruction_row(logo_layout, tmp_path):
    text = (BOOT / "LOGO.lst").read_text()
    damaged, count = re.subn(r"(?m)^\s*85 00000001[^\n]*\n", "", text)
    assert count == 1
    path = tmp_path / "damaged.lst"
    path.write_text(damaged)
    with pytest.raises(ListingError, match="uncovered byte"):
        parse_nasm_listing(path)


def test_nasm_rejects_unexpanded_listing(logo_layout, tmp_path):
    subprocess.run([str(NASM), "-Dcom_file=1", "-f", "bin", "-l", str(tmp_path / "plain.lst"),
                    "-o", str(tmp_path / "plain.com"), "third_party/bootlogo/bootlogo.asm"],
                   cwd=ROOT, check=True, capture_output=True)
    with pytest.raises(ListingError, match="require NASM -Le"):
        parse_nasm_listing(tmp_path / "plain.lst")


def test_nasm_rejects_unlisted_tail(logo_layout):
    with pytest.raises(LayoutError, match="sizes disagree"):
        build_nasm_layout(replace(logo_layout.image, data=logo_layout.image.data + b"\x90"), logo_layout.listing)


def test_nasm_rejects_opcode_patch(tmp_path):
    image, listing = assemble(tmp_path, """org 0x100
mov byte [target],0x90
ret
target: mov ax,3
ret
""")
    with pytest.raises(LayoutError, match="unsupported opcode/operand"):
        build_nasm_layout(image, listing)


def test_nasm_rejects_changed_instruction_boundaries(logo_layout):
    listing = replace(logo_layout.listing, lines=list(logo_layout.listing.lines))
    first, second = listing.lines[:2]
    listing.lines[:2] = [replace(first, bytes=first.bytes + second.bytes, byte_count=2)]
    with pytest.raises(LayoutError, match="instruction length mismatch"):
        build_nasm_layout(logo_layout.image, listing)


def test_every_bootlogo_instruction_is_in_the_unicorn_gate(logo_layout):
    from translator_support.ops_build import load_cases
    cases = load_cases()
    included = {(case.record.off, bytes(case.record.insn.bytes), case.record.mutable_offsets)
                for case in cases}
    expected = {(record.off, bytes(record.insn.bytes), record.mutable_offsets)
                for record in logo_layout.instructions}
    assert expected <= included
    assert sum(case.image_name == "LOGO.COM" for case in cases) > 200


def test_bootlogo_instructions_against_unicorn(logo_layout):
    from translator_support.ops_build import (InstructionCase, STATES_PER_INSTRUCTION,
                                               build_library)
    cases = tuple(InstructionCase(index, "LOGO.COM", record, ())
                  for index, record in enumerate(logo_layout.instructions))
    library = build_library(cases)
    for case in cases:
        failure = library.ops_check_instruction(case.index, STATES_PER_INSTRUCTION)
        assert failure is None, failure.decode() if failure else ""
    assert library.ops_checked_states() == len(cases) * STATES_PER_INSTRUCTION


def test_bootlogo_square_pixels_from_unicorn_and_fixed_point_arithmetic(logo_layout):
    result = run_logo(logo_layout.image.data, "REPEAT 4 [FD 50 RT 90]\r")
    # 9.7 fixed point; cardinal sin-table entries are exactly 0,+128,0,-128.
    # Each FD plots before advancing, so turns plot the other segment's end.
    corners = ((160, 100), (160, 50), (210, 50), (210, 100))
    assert all(result.pixels[y * 320 + x] == 3 for x, y in corners)
    assert result.coordinates == (160 << 7, 100 << 7)
    assert result.visited <= {record.off for record in logo_layout.instructions}
    assert not result.exited


def test_bootlogo_pen_skip_paths_color_and_quit_against_unicorn(logo_layout):
    result = run_logo(logo_layout.image.data, "SETCOLOR 2\rREPEAT 1 [PU FD 10 PD BK 10]\rQUIT\r")
    assert result.exited
    assert result.color == 2
    assert result.coordinates == (160 << 7, 100 << 7)
    assert result.pixels[90 * 320 + 160] == 2
    assert result.visited <= {record.off for record in logo_layout.instructions}
    assert {0x175, 0x176, 0x187, 0x188} <= result.visited
