"""Small strictness regressions independent of the differential CPU oracle."""

from dataclasses import replace
from pathlib import Path
import struct
import subprocess

import pytest
from capstone.x86_const import X86_OP_IMM

from translator.image import ImageError, LoadedImage, load_image
from translator.layout import LayoutError, build_layout
from translator.listing import parse_listing


ROOT = Path(__file__).resolve().parents[1]


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
    layout = com_layout(tmp_path, b"\xc3\x90\xc3", [row(0x100, "C3", "RET"),
                        row(0x101, "90", "CodeByte DB 90h"), row(0x102, "C3", "RET")])
    assert [ins.off for ins in layout.instructions] == [0, 2]
    assert layout.segment_bases == {"_TEXT": -0x100}


def test_entry_data_follows_fallthrough_to_listed_code(tmp_path):
    layout = com_layout(tmp_path, b"\x50\x40\xc3", [row(0x100, "5040", "DB 50h,40h"),
                                                row(0x102, "C3", "RET")])
    assert [(ins.off, ins.insn.mnemonic) for ins in layout.instructions] == [
        (0, "push"), (1, "inc"), (2, "ret")]


def test_undecodable_entry_data_fails(tmp_path):
    with pytest.raises(LayoutError, match="undecodable reachable bytes.*0x0"):
        com_layout(tmp_path, b"\x0f", [row(0x100, "0F", "DB 0fh")])


def test_entry_outside_image_fails(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, "C3", "RET")])
    image = LoadedImage(Path("test.exe"), b"\xc3", is_exe=True, hdr_ip=1)
    with pytest.raises(LayoutError, match="entry point 0x1 is outside the image"):
        build_layout(image, parsed)


def test_mz_entry_uses_both_cs_and_ip(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, "C3", "RET"),
                     row(19, "90", "DB 90h"), row(20, "C3", "RET")])
    image = LoadedImage(Path("test.exe"), b"\xc3" + b"\xff" * 18 + b"\x90\xc3",
                        is_exe=True, hdr_cs=1, hdr_ip=3)
    assert [ins.off for ins in build_layout(image, parsed).instructions] == [0, 19, 20]


@pytest.mark.parametrize("machine,source", [
    ("eb04", "JMP Target"), ("e90400", "JMP Target"), ("e80400", "CALL Target"),
    ("7004", "JO Target"), ("7104", "JNO Target"), ("7204", "JB Target"),
    ("7304", "JAE Target"), ("7404", "JE Target"), ("7504", "JNE Target"),
    ("7604", "JBE Target"), ("7704", "JA Target"), ("7804", "JS Target"),
    ("7904", "JNS Target"), ("7a04", "JP Target"), ("7b04", "JNP Target"),
    ("7c04", "JL Target"), ("7d04", "JGE Target"), ("7e04", "JLE Target"),
    ("7f04", "JG Target"), ("e004", "LOOPNE Target"), ("e104", "LOOPE Target"),
    ("e204", "LOOP Target"), ("e304", "JCXZ Target"),
])
def test_direct_targets_and_return_sites_decode_data(tmp_path, machine, source):
    branch = bytes.fromhex(machine)
    after = len(branch)
    layout = com_layout(tmp_path, branch + bytes.fromhex("40c3ffff48c3"),
                        [row(0x100, machine.upper(), source),
                         row(0x100 + after, "40C3FFFF", "DB 40h,0c3h,0ffh,0ffh"),
                         row(0x104 + after, "48C3", "Target DB 48h,0c3h")])
    expected = [0, after + 4, after + 5]
    if not source.startswith("JMP"):
        expected[1:1] = [after, after + 1]
    assert [ins.off for ins in layout.instructions] == expected


def test_newly_decoded_branches_add_successors_until_closed(tmp_path):
    layout = com_layout(tmp_path, bytes.fromhex("eb03ffffffe30340c3ff48c3"),
                        [row(0x100, "EB03", "JMP Hidden"),
                         row(0x102, "FFFFFF", "DB 0ffh,0ffh,0ffh"),
                         row(0x105, "E3", "Hidden DB 0e3h"), row(0x106, "03", "DB 3"),
                         row(0x107, "40C3FF48C3", "DB 40h,0c3h,0ffh,48h,0c3h")])
    assert [(ins.off, ins.insn.mnemonic) for ins in layout.instructions] == [
        (0, "jmp"), (5, "jcxz"), (7, "inc"), (8, "ret"), (10, "dec"), (11, "ret")]


@pytest.mark.parametrize("machine,source", [
    ("ffd0", "CALL AX"), ("ff1e0000", "CALL FAR PTR DS:[0]"),
    ("cd20", "INT 20h"), ("cd21", "INT 21h"), ("cc", "INT 3"), ("ce", "INTO"),
])
def test_calls_and_interrupts_keep_their_return_sites(tmp_path, machine, source):
    raw = bytes.fromhex(machine)
    layout = com_layout(tmp_path, raw + b"\x40\xc3", [row(0x100, machine.upper(), source),
                         row(0x100 + len(raw), "40C3", "DB 40h,0c3h")])
    assert [ins.off for ins in layout.instructions] == [0, len(raw), len(raw) + 1]


@pytest.mark.parametrize("machine,source", [
    ("ffe0", "JMP AX"), ("ff2e0000", "JMP FAR PTR DS:[0]"),
    ("c3", "RET"), ("cb", "RETF"), ("cf", "IRET"),
    ("ebfd", "JMP Outside"), ("eb7f", "JMP Outside"), ("ebfe", "JMP Self"),
])
def test_unconditional_transfers_do_not_decode_fallthrough(tmp_path, machine, source):
    raw = bytes.fromhex(machine)
    layout = com_layout(tmp_path, raw + b"\x0f", [row(0x100, machine.upper(), source),
                                              row(0x100 + len(raw), "0F", "DB 0fh")])
    assert [ins.off for ins in layout.instructions] == [0]


def test_relative_target_above_64k_is_not_truncated(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE Low"), row(0, "B80100", "MOV AX,1"),
                     row(3, "C3", "RET"), row(0, "", ".CODE High"),
                     row(0, "EB00", "JMP Target"), row(2, "90C3", "Target DB 90h,0c3h")])
    image = LoadedImage(Path("test.exe"), bytes.fromhex("b80100c3") + b"\xff" * 0xfffc +
                        bytes.fromhex("eb0090c3"), is_exe=True)
    assert [ins.off for ins in build_layout(image, parsed).instructions] == [
        0, 3, 0x10000, 0x10002, 0x10003]


@pytest.mark.parametrize("forward", [False, True])
def test_near_displacements_wrap_within_the_source_cs_frame(tmp_path, forward):
    if forward:
        raw = b"\xc3\xff\xff\xff\xff\x90\xc3" + b"\xff" * (0x9000 - 7) + bytes.fromhex("e90270")
        rows = [row(0, "C3", "RET"), row(5, "90C3", "Target DB 90h,0c3h"),
                row(0x9000, "E90270", "JMP Target")]
        expected = [0, 5, 6, 0x9000]
    else:
        raw = bytes.fromhex("e9fd8f") + b"\xff" * (0x9000 - 3) + b"\x90\xc3"
        rows = [row(0, "E9FD8F", "JMP Target"), row(0x9000, "90C3", "Target DB 90h,0c3h")]
        expected = [0, 0x9000, 0x9001]
    parsed = listing(tmp_path, [row(0, "", ".CODE"), *rows])
    image = LoadedImage(Path("test.exe"), raw, is_exe=True)
    assert [ins.off for ins in build_layout(image, parsed).instructions] == expected


def test_fallthrough_wraps_within_the_source_cs_frame(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, "C3", "RET"),
                     row(0xffff, "90", "NOP"), row(0, "", ".DATA"), row(0, "0F", "DB 0fh")])
    image = LoadedImage(Path("test.exe"), b"\xc3" + b"\xff" * 0xfffe + b"\x90\x0f", is_exe=True)
    assert [ins.off for ins in build_layout(image, parsed).instructions] == [0, 0xffff]


@pytest.mark.parametrize("opcode,mnemonic", [("9a", "CALL"), ("ea", "JMP")])
@pytest.mark.parametrize("relocated", [False, True])
def test_far_targets_are_image_relative_only_with_segment_relocation(tmp_path, opcode, mnemonic,
                                                                    relocated):
    parsed = listing(tmp_path, [row(0, "", ".CODE"),
                     row(0, opcode.upper() + "08000000", mnemonic + " FAR PTR Target"),
                     row(5, "C3", "RET"), row(6, "FFFF", "DB 0ffh,0ffh"),
                     row(8, "40C3", "Target DB 40h,0c3h")])
    image = LoadedImage(Path("test.exe"), bytes.fromhex(opcode + "08000000c3ffff40c3"),
                        [3] if relocated else [], is_exe=True)
    expected = [0, 5, 8, 9] if relocated else [0, 5]
    assert [ins.off for ins in build_layout(image, parsed).instructions] == expected


def test_relocated_near_displacement_does_not_guess_a_static_target(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, "E80100", "CALL Dynamic"),
                     row(3, "C3", "RET"), row(4, "0F", "DB 0fh")])
    image = LoadedImage(Path("test.exe"), bytes.fromhex("e80100c30f"), [1], is_exe=True)
    assert [ins.off for ins in build_layout(image, parsed).instructions] == [0, 3]


@pytest.mark.parametrize("data,rows,offset,kind,owner", [
    ("eb01b80100c3", [(0x100, "EB01", "JMP Middle"), (0x102, "B80100", "MOV AX,1"),
                     (0x105, "C3", "RET")], 3, "listed", 2),
    ("90b890c3", [(0x100, "90", "NOP"), (0x101, "B8", "DB 0b8h"),
                 (0x102, "90", "NOP"), (0x103, "C3", "RET")], 1, "listed", 2),
    ("eb00b80000ebfc", [(0x100, "EB00", "JMP Hidden"),
                       (0x102, "B80000EBFC", "Hidden DB 0b8h,0,0,0ebh,0fch")], 3, "decoded", 2),
])
def test_static_successors_reject_instruction_overlap(tmp_path, data, rows, offset, kind, owner):
    with pytest.raises(LayoutError, match=rf"0x{offset:x} overlaps a {kind} instruction.*0x{owner:x}"):
        com_layout(tmp_path, bytes.fromhex(data), [row(*fields) for fields in rows])


def test_static_successors_reject_undecodable_bytes(tmp_path):
    with pytest.raises(LayoutError, match="undecodable reachable bytes.*0x1"):
        com_layout(tmp_path, b"\x90\x0f", [row(0x100, "90", "NOP"), row(0x101, "0F", "DB 0fh")])


def test_recovered_instructions_still_validate_relocations(tmp_path):
    parsed = listing(tmp_path, [row(0, "", ".CODE"), row(0, "90", "NOP"),
                     row(1, "8B060000C3", "DB 8bh,6,0,0,0c3h")])
    image = LoadedImage(Path("test.exe"), bytes.fromhex("908b060000c3"), [3], is_exe=True)
    with pytest.raises(LayoutError, match="relocation 0x3 falls outside an immediate"):
        build_layout(image, parsed)


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
        com_layout(tmp_path, b"\xc3\x50\xc3", [row(0x100, "C3", "RET"),
                   row(0x101, "", "Fn PROC NEAR"), row(0x102, "C3", "RET", generated=True),
                   row(0x103, "", "Fn ENDP")])


def test_proc_entry_in_reachable_data_is_a_valid_boundary(tmp_path):
    layout = com_layout(tmp_path, b"\x40\xc3", [row(0x100, "", "Fn PROC NEAR"),
                        row(0x100, "40", "DB 40h"), row(0x101, "C3", "RET"),
                        row(0x102, "", "Fn ENDP")])
    assert layout.procedures["Fn"] == 0
    assert layout.instructions[0].chunk.name == "Fn"


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


@pytest.fixture(scope="module")
def vc_layouts():
    generated = subprocess.run(["make", "gen"], cwd=ROOT, text=True, capture_output=True)
    assert generated.returncode == 0, generated.stdout + generated.stderr
    layouts = {name: build_layout(load_image(ROOT / "build" / name),
                                 parse_listing(ROOT / "build" / "gen" / (name + ".lst")))
               for name in ("VC.COM", "VC.OVL")}
    # A failed check must not repr tens of thousands of cyclic instruction /
    # chunk records while pytest renders its fixture arguments.
    return layouts.__getitem__


@pytest.mark.parametrize("name", ["VC.COM", "VC.OVL"])
def test_vc_static_successors_are_instruction_starts(vc_layouts, name):
    """Check closure independently of the production successor enumerator."""
    layout = vc_layouts(name)
    starts = {ins.off for ins in layout.instructions}
    image_size = len(layout.image.data)
    entry = layout.image.hdr_cs * 16 + layout.image.hdr_ip if layout.image.is_exe else 0
    assert entry in starts, f"{name}: entry 0x{entry:x} is not translated"
    for record in layout.instructions:
        insn = record.insn
        mnemonic = insn.mnemonic.split()[-1]
        after = record.off + insn.size
        segment = layout.listing.segments[record.line.segment]
        if not layout.image.is_exe:
            frame = -0x100
        else:
            frame = min(base for name, base in layout.segment_bases.items()
                        if name == segment.name or segment.group and
                        layout.listing.segments[name].group == segment.group and
                        layout.listing.segments[name].file_size != 0) & ~15
        successors = []
        if mnemonic not in {"jmp", "ljmp", "ret", "retf", "iret", "iretd"}:
            successors.append(frame + ((after - frame) & 0xffff))
        if insn.operands and insn.operands[0].type == X86_OP_IMM:
            if mnemonic in {"ljmp", "lcall"}:
                if after - 2 in layout.image.relocations:
                    successors.append(insn.operands[0].imm * 16 + insn.operands[1].imm)
            elif mnemonic.startswith(("j", "loop")) or mnemonic == "call":
                if record.off + insn.imm_offset not in layout.image.relocations:
                    displacement = bytes(insn.bytes)[insn.imm_offset:insn.imm_offset + insn.imm_size]
                    target_ip = (after - frame + int.from_bytes(displacement, "little", signed=True)) & 0xffff
                    successors.append(frame + target_ip)
        for successor in successors:
            covered = successor in starts or not 0 <= successor < image_size
            assert covered, (
                f"{name}: {mnemonic} at 0x{record.off:x} has untranslated successor 0x{successor:x}")


def test_vc_puttree_jcxz_sites_are_instructions(vc_layouts):
    layout = vc_layouts("VC.OVL")
    by_offset = {ins.off: ins for ins in layout.instructions}
    # This source tree has one _JCXZ expansion (two DB rows), followed by DEC CX
    # and a normal JCXZ. Check both JCXZ instructions, not the displacement DB.
    macro_rows = [line for line in layout.listing.lines
                  if line.procedure == "PutTree" and line.mnemonic == "DB" and line.bytes == (0xe3,)]
    assert len(macro_rows) == 1
    macro = macro_rows[0]
    macro_off = layout.segment_bases[macro.segment] + macro.offset
    for off, encoded in ((macro_off, b"\xe3\x57"), (macro_off + 3, b"\xe3\x19")):
        translated = off in by_offset
        assert translated, f"PutTree _JCXZ/JCXZ site at image 0x{off:x} is not translated"
        instruction = by_offset[off].insn
        assert instruction.mnemonic == "jcxz"
        assert bytes(instruction.bytes) == encoded
