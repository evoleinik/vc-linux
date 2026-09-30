"""Small strictness regressions independent of the differential CPU oracle."""

from dataclasses import replace
from pathlib import Path
import struct

import pytest

from translator.image import ImageError, LoadedImage, load_image
from translator.layout import LayoutError, build_layout
from translator.listing import parse_listing


def row(offset, bytefield, source, *, generated=False, macro=False):
    prefix = (f"{offset:04X} {bytefield}" if offset is not None else "").ljust(28)
    return prefix + ("*" if generated else " ") + ("1" if macro else " ") + "  " + source


def listing(tmp_path, rows):
    path = tmp_path / "test.lst"
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    return parse_listing(path)


def com_layout(tmp_path, data, rows):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), *rows])
    return build_layout(LoadedImage(Path("test.com"), data), parsed)


def test_listing_fixup_markers_and_macro_source(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"),
                               row(0x100, "9A 0000o 0000s", "CALL FarProc", macro=True)])
    line = parsed.lines[-1]
    assert line.is_instruction
    assert line.bytes == (0x9A, None, None, None, None)
    assert line.byte_count == 5
    assert line.fixups == ((1, 2, "o"), (3, 2, "s"))


def test_instruction_boundaries_do_not_sweep_data(tmp_path):
    layout = com_layout(tmp_path, b"\x90\xc3", [row(0x100, "90", "CodeByte DB 90h"),
                                                row(0x101, "C3", "RET")])
    assert [ins.off for ins in layout.instructions] == [1]
    assert layout.segment_bases == {"_TEXT": -0x100}


def test_source_macro_expansion_rows_are_instructions(tmp_path):
    layout = com_layout(tmp_path, b"\x40\xc3", [row(0x100, "40", "INC AX", macro=True),
                                                row(0x101, "C3", "RET")])
    assert [ins.off for ins in layout.instructions] == [0, 1]


def test_standalone_prefix_is_joined(tmp_path):
    layout = com_layout(tmp_path, b"\xf3\xa4\xc3", [row(0x100, "F3", "REP"),
                                                     row(0x101, "A4", "MOVSB"),
                                                     row(0x102, "C3", "RET")])
    assert [ins.off for ins in layout.instructions] == [0, 2]
    assert layout.instructions[0].insn.size == 2
    assert layout.instructions[0].insn.mnemonic == "rep movsb"


def test_exact_long_conditional_template(tmp_path):
    layout = com_layout(tmp_path, bytes.fromhex("7403e90000c3"),
                        [row(0x100, "7403E90000", "JNE Destination"),
                         row(0x105, "C3", "Destination: RET")])
    assert [(ins.off, ins.insn.mnemonic) for ins in layout.instructions] == [
        (0, "je"), (2, "jmp"), (5, "ret")]


def test_mismatched_long_conditional_is_not_accepted(tmp_path):
    with pytest.raises(LayoutError, match="instruction length mismatch"):
        com_layout(tmp_path, bytes.fromhex("7503e90000c3"),
                   [row(0x100, "7503E90000", "JNE Destination"),
                    row(0x105, "C3", "Destination: RET")])


def test_same_segment_far_call_has_two_proven_boundaries(tmp_path):
    layout = com_layout(tmp_path, bytes.fromhex("0ee80000cb"),
                        [row(0x100, "0EE80000", "CALL FarProc"),
                         row(0x104, "", "FarProc PROC FAR"),
                         row(0x104, "CB", "RETF"),
                         row(0x105, "", "FarProc ENDP")])
    assert [(ins.off, ins.insn.mnemonic) for ins in layout.instructions] == [
        (0, "push"), (1, "call"), (4, "retf")]
    assert layout.procedures["FarProc"] == 4


def test_unknown_multi_instruction_source_does_not_sweep(tmp_path):
    with pytest.raises(LayoutError, match="instruction length mismatch"):
        com_layout(tmp_path, b"\x90\x90", [row(0x100, "9090", "NOP")])


def test_literal_byte_mismatch_fails(tmp_path):
    with pytest.raises(LayoutError, match="byte mismatch"):
        com_layout(tmp_path, bytes.fromhex("b83412c2"),
                   [row(0x100, "B83412", "MOV AX,1234h"), row(0x103, "C3", "RET")])


def test_word_operand_is_not_misparsed_as_data(tmp_path):
    layout = com_layout(tmp_path, bytes.fromhex("ff36cdab"),
                        [row(0x100, "FF36CDAB", "PUSH WORD PTR DS:[0ABCDh]")])
    assert len(layout.instructions) == 1
    assert layout.instructions[0].insn.mnemonic == "push"


@pytest.mark.parametrize("machine,source,mnemonic", [
    ("2eac", "LODS BYTE PTR CS:[SI]", "lodsb"),
    ("26ad", "LODS WORD PTR ES:[SI]", "lodsw"),
    ("26a4", "MOVS BYTE PTR ES:[SI],ES:[DI]", "movsb"),
    ("aa", "STOS BYTE PTR ES:[DI]", "stosb"),
    ("af", "SCAS WORD PTR ES:[DI]", "scasw"),
    ("a6", "CMPS BYTE PTR DS:[SI],ES:[DI]", "cmpsb"),
    ("0fb606cdab", "MOVZX AX,BYTE PTR [0ABCDh]", "movzx"),
    ("ff36cdab", "UnknownMnemonic WORD PTR DS:[0ABCDh]", "push"),
])
def test_typed_instruction_forms_are_not_data(tmp_path, machine, source, mnemonic):
    layout = com_layout(tmp_path, bytes.fromhex(machine), [row(0x100, machine.upper(), source)])
    assert len(layout.instructions) == 1
    assert layout.instructions[0].insn.mnemonic == mnemonic


def test_proc_entry_must_have_an_instruction_boundary(tmp_path):
    with pytest.raises(LayoutError, match="entry.*missing an instruction boundary"):
        com_layout(tmp_path, b"\x50\xc3", [row(0x100, "", "Fn PROC NEAR"),
                   row(0x101, "C3", "RET", generated=True), row(0x102, "", "Fn ENDP")])


def test_known_code_segment_cannot_have_undocumented_byte_gaps(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"),
                               row(0x100, "B83412", "MOV AX,1234h"),
                               row(0x104, "C3", "RET")])
    parsed.segments["_TEXT"].length = 0x105
    with pytest.raises(LayoutError, match="uncovered byte.*0x103"):
        build_layout(LoadedImage(Path("test.com"), bytes.fromhex("b8341240c3")), parsed)


def test_omitted_generated_prologue_fails_clearly(tmp_path):
    with pytest.raises(LayoutError, match="reassemble with -Sg"):
        com_layout(tmp_path, b"\x50\xc3", [row(0x100, "", "Fn PROC NEAR USES AX"),
                                                row(0x101, "C3", "RET"),
                                                row(0x102, "", "Fn ENDP")])


def data_fixup_layout(tmp_path, value):
    return com_layout(tmp_path, b"\xc3ABC" + value,
                      [row(0x100, "C3", "Destination: RET"),
                       row(0, "", ".DATA"), row(0, "414243", "DB 'ABC'"),
                       row(3, "0000", "DW Destination")])


def test_data_fixup_is_source_resolved_not_unchecked(tmp_path):
    layout = data_fixup_layout(tmp_path, b"\x00\x01")
    assert layout.segment_bases["_DATA"] == 1
    assert layout.labels["Destination"] == 0
    with pytest.raises(LayoutError, match="resolved data fixup mismatch"):
        data_fixup_layout(tmp_path, b"\xff\x01")


def test_unknown_data_symbol_is_not_a_blanket_byte_wildcard(tmp_path):
    with pytest.raises(LayoutError, match="byte mismatch"):
        com_layout(tmp_path, b"ABC\xff\xff\xc3", [row(0x100, "414243", "DB 'ABC'"),
                 row(0x103, "0000", "DW Unknown"), row(0x105, "C3", "RET")])


def test_comment_block_cannot_create_code_or_change_segment(tmp_path):
    layout = com_layout(tmp_path, b"\x40\xc3", [row(0x100, "40", "INC AX"),
        row(None, "", "COMMENT |"), row(None, "", ".DATA"),
        row(None, "", "Fake: RET"), row(None, "", "|"), row(0x101, "C3", "RET")])
    assert [ins.off for ins in layout.instructions] == [0, 1]
    assert "Fake" not in layout.labels


def test_org_overlaid_uninitialized_reservation_is_not_zero_data(tmp_path):
    layout = com_layout(tmp_path, b"\x40\xc3", [row(0x100, "0000", "Scratch DW ?"),
        row(None, "", "ORG Scratch"), row(0x100, "40", "INC AX"), row(0x101, "C3", "RET")])
    assert [ins.off for ins in layout.instructions] == [0, 1]


def test_relocations_only_cover_immediates_or_far_segments(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, "B80000", "MOV AX,0")])
    image = LoadedImage(Path("test.exe"), bytes.fromhex("b80000"), [1], True)
    assert len(build_layout(image, parsed).instructions) == 1
    bad = listing(tmp_path, [row(0, "", ".CODE"), row(0, "8B060000", "MOV AX,[0]")])
    image = replace(image, data=bytes.fromhex("8b060000"), relocations=[2])
    with pytest.raises(LayoutError, match="outside an immediate"):
        build_layout(image, bad)


@pytest.mark.parametrize("machine,source", [
    ("e80000", "CALL 0"), ("c20800", "RET 8"),
    ("ca0800", "RETF 8"), ("c8060000", "ENTER 6,0"),
])
def test_relocated_control_instruction_immediates(tmp_path, machine, source):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, machine.upper(), source)])
    image = LoadedImage(Path("test.exe"), bytes.fromhex(machine), [1], True)
    assert len(build_layout(image, parsed).instructions) == 1


def mz_bytes(*, relocation=0, pages=1, last_page=36):
    header = struct.pack("<14H", 0x5A4D, last_page, pages, 1, 2, 0, 0xffff,
                         0, 0xfffe, 0, 0, 0, 28, 0)
    return header + struct.pack("<HH", relocation, 0) + b"ABCD"


def test_mz_load_module_and_relocation_are_unmodified(tmp_path):
    path = tmp_path / "test.exe"
    path.write_bytes(mz_bytes(relocation=1))
    image = load_image(path)
    assert image.is_exe and image.data == b"ABCD"
    assert image.relocations == [1]
    assert image.hdr_sp == 0xfffe and image.max_alloc == 0xffff
    assert image.header["header_size"] == 32


def test_mz_without_relocations_may_have_no_table(tmp_path):
    data = bytearray(mz_bytes())
    struct.pack_into("<H", data, 6, 0)
    struct.pack_into("<H", data, 24, 0)
    path = tmp_path / "no_relocations.exe"
    path.write_bytes(data)
    assert load_image(path).relocations == []


@pytest.mark.parametrize("data,message", [(b"MZ", "truncated MZ header"),
    (mz_bytes(relocation=3), "outside the load module"),
    (mz_bytes(last_page=37), "truncated or inconsistent")])
def test_malformed_mz_fails(tmp_path, data, message):
    path = tmp_path / "bad.exe"
    path.write_bytes(data)
    with pytest.raises(ImageError, match=message):
        load_image(path)
