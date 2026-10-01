"""DOS 2 source boundaries, separate COM groups and fixed opcode variants."""

from dataclasses import replace
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

from tools.build_kermit_jwasm import ensure_jwasm
from tools.build_msdos import PROGRAMS
from translator.image import load_image
from translator.layout import LayoutError
from translator.linked import build_linked_layout, parse_map
from translator.msdos import build_msdos_layout, prove_sort_variants
from translator_support.ops_build import InstructionCase, build_library, load_cases


ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "build/msdos2"
PROGRAM_COUNTS = {"COMMAND.COM": 5445, "EDLIN.COM": 1433, "DEBUG.COM": 3242,
                  "FIND.EXE": 351, "MORE.COM": 90, "SORT.EXE": 239, "FC.EXE": 726}


@pytest.fixture(scope="module")
def msdos_layouts():
    built = subprocess.run(["make", "msdos2"], cwd=ROOT, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    assert built.returncode == 0, built.stdout
    return {name: build_msdos_layout(load_image(DIRECTORY / name),
                                     [DIRECTORY / (module + ".lst") for module in modules],
                                     DIRECTORY / (Path(name).stem + ".MAP"))
            for name, modules in PROGRAMS.items()}


@pytest.mark.parametrize("name", tuple(PROGRAM_COUNTS))
def test_msdos_covers_the_complete_original_source_program(msdos_layouts, name):
    layout = msdos_layouts[name]
    assert len(layout.instructions) == PROGRAM_COUNTS[name]
    assert layout.instructions
    if name == "COMMAND.COM":
        assert {record.line.segment for record in layout.instructions} == {"CODERES", "INIT", "TRANCODE", "ZEXEC_CODE"}
        assert layout.labels["COMMAND::$EXEC"] in {record.off for record in layout.instructions}
    if name == "SORT.EXE":
        assert [record.off for record in layout.instructions if record.variants] == [layout.labels["SORT::CODE_PATCH"]]


def emitted_form_key(record, relocations):
    return (record.off, bytes(record.insn.bytes), record.mutable_offsets,
            tuple(at - record.off for at in relocations
                  if record.off <= at < record.off + record.insn.size),
            tuple(bytes(form.insn.bytes) for form in record.variants))


@pytest.fixture(scope="module")
def oracle_forms():
    return {emitted_form_key(case.record, case.relocations) for case in load_cases()}


@pytest.mark.parametrize("name", tuple(PROGRAM_COUNTS))
def test_msdos_oracle_covers_every_emitted_form(msdos_layouts, oracle_forms, name):
    layout = msdos_layouts[name]
    expected = {emitted_form_key(replace(form, variants=record.variants) if record.variants else form,
                                 layout.image.relocations)
                for record in layout.instructions for form in (record.variants or (record,))}
    assert expected <= oracle_forms, f"{name}: {len(expected - oracle_forms)} emitted forms absent from Unicorn"


@pytest.mark.parametrize("name", tuple(PROGRAM_COUNTS))
def test_msdos_rejects_corrupted_linked_instruction(msdos_layouts, name):
    layout = msdos_layouts[name]
    record = next(record for record in layout.instructions if record.line.is_instruction)
    data = bytearray(layout.image.data)
    data[record.off] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_msdos_layout(replace(layout.image, data=bytes(data)),
                           [DIRECTORY / (module + ".lst") for module in PROGRAMS[name]],
                           DIRECTORY / (Path(name).stem + ".MAP"))


def assemble(directory, text, *, com=True, module="fixture"):
    (directory / (module + ".asm")).write_text(text)
    subprocess.run([str(ensure_jwasm()), "-q", "-Zm", "-Cu", "-Sa", "-Sg",
                    f"-Fl={module}.lst", f"-Fo={module}.obj", module + ".asm"],
                   cwd=directory, check=True, capture_output=True)
    command = [str(ROOT / "tools/jwlink/jwlink"), "format", "dos"]
    if com:
        command.append("com")
    name = "FIXTURE.COM" if com else "FIXTURE.EXE"
    subprocess.run(command + ["file", module + ".obj", "name", name,
                              "option", "map=FIXTURE.MAP,verbose,nofarcalls"],
                   cwd=directory, check=True, capture_output=True)
    return load_image(directory / name), [directory / (module + ".lst")], directory / "FIXTURE.MAP"


@pytest.fixture
def grouped_com(tmp_path):
    return assemble(tmp_path, """RSEG SEGMENT PARA PUBLIC
RSEG ENDS
DSEG SEGMENT PARA PUBLIC
DSEG ENDS
TSEG SEGMENT PARA PUBLIC
TSEG ENDS
RGRP GROUP RSEG,DSEG
TGRP GROUP TSEG
RSEG SEGMENT
ASSUME CS:RGRP,DS:RGRP
ORG 100h
entry: CALL init
RET
RSEG ENDS
DSEG SEGMENT
init: INC AX
RET
group_pointer DW OFFSET RGRP:message
segment_pointer DW OFFSET DSEG:message
transient_pointer DW OFFSET TGRP:transient
message DB 'resident',0
DSEG ENDS
TSEG SEGMENT
ASSUME CS:TGRP,DS:TGRP
transient: MOV AX,OFFSET TGRP:transient
DB 0EBh,0
RET
TSEG ENDS
END entry
""")


def test_multi_group_com_preserves_explicit_segment_and_group_offsets(grouped_com):
    image, listings, map_path = grouped_com
    layout = build_msdos_layout(image, listings, map_path)
    link = parse_map(map_path)
    labels = {name.upper(): value for name, value in layout.labels.items()}
    message = labels["FIXTURE::MESSAGE"] + 0x100
    dseg = link.segments["DSEG"][0]
    expected = {"GROUP_POINTER": message, "SEGMENT_POINTER": message - dseg,
                "TRANSIENT_POINTER": 0}
    for name, value in expected.items():
        off = labels["FIXTURE::" + name]
        assert int.from_bytes(image.data[off:off + 2], "little") == value
    assert {record.line.segment for record in layout.instructions} == {"RSEG", "DSEG", "TSEG"}
    assert [record.insn.mnemonic for record in layout.instructions] == [
        "call", "ret", "inc", "ret", "mov", "jmp", "ret"]
    # The older front end remains deliberately unchanged.
    with pytest.raises(LayoutError, match="one flat COM group"):
        build_linked_layout(image, listings, map_path)


def test_multi_group_com_rejects_changed_group_relative_data(grouped_com):
    image, listings, map_path = grouped_com
    layout = build_msdos_layout(image, listings, map_path)
    off = next(value for name, value in layout.labels.items() if name.upper().endswith("::SEGMENT_POINTER"))
    data = bytearray(image.data)
    data[off] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_msdos_layout(replace(image, data=bytes(data)), listings, map_path)


def test_multi_group_com_rejects_an_omitted_source_instruction(grouped_com):
    image, listings, map_path = grouped_com
    original = listings[0].read_text()
    rows = original.splitlines(keepends=True)
    damaged = "".join(row for row in rows if "init: INC AX" not in row)
    assert len(damaged) < len(original)
    listings[0].write_text(damaged)
    with pytest.raises(LayoutError, match="uncovered byte"):
        build_msdos_layout(image, listings, map_path)


@pytest.mark.parametrize("com", (True, False), ids=("ungrouped-com", "unclassified-exe"))
def test_code_without_masm_class_or_group_is_source_directed(tmp_path, com):
    image, listings, map_path = assemble(tmp_path, """CODE SEGMENT PARA PUBLIC
ASSUME CS:CODE
""" + ("ORG 100h\n" if com else "") + """entry: MOV AX,1234h
INC AX
RET
CODE ENDS
END entry
""", com=com)
    layout = build_msdos_layout(image, listings, map_path)
    assert [record.insn.mnemonic for record in layout.instructions] == ["mov", "inc", "ret"]
    library = build_library(tuple(InstructionCase(index, "MSDOS_FIXTURE", record, ())
                                  for index, record in enumerate(layout.instructions)))
    for index in range(len(layout.instructions)):
        failure = library.ops_check_instruction(index, 32)
        assert failure is None, failure.decode() if failure else ""


def test_masm_repeated_uninitialized_words_need_no_file_bytes(tmp_path):
    image, listings, map_path = assemble(tmp_path, """CODE SEGMENT
ASSUME CS:CODE
entry: INC AX
RET
CODE ENDS
STACK SEGMENT STACK
DW 64 DUP(?,?)
STACK ENDS
END entry
""", com=False)
    layout = build_msdos_layout(image, listings, map_path)
    assert [record.insn.mnemonic for record in layout.instructions] == ["inc", "ret"]


def test_capstone_enum_sentinel_is_a_valid_masm_data_label(tmp_path):
    image, listings, map_path = assemble(tmp_path, """CODE SEGMENT
ASSUME CS:CODE
entry: RET
CODE ENDS
DATA SEGMENT
ENDING DB 1 DUP (?)
DATA ENDS
END entry
""", com=False)
    layout = build_msdos_layout(image, listings, map_path)
    assert [record.insn.mnemonic for record in layout.instructions] == ["ret"]


@pytest.mark.parametrize("function,expected", [(0x4c, ["mov", "int"]),
                                              (9, ["mov", "int", "nop", "ret"])])
def test_only_proved_terminating_dos_calls_stop_data_successors(tmp_path, function, expected):
    image, listings, map_path = assemble(tmp_path, f"""CODE SEGMENT
ASSUME CS:CODE
entry: MOV AH,{function}
INT 21h
DB 90h,0c3h
CODE ENDS
END entry
""", com=False)
    layout = build_msdos_layout(image, listings, map_path)
    assert [record.insn.mnemonic for record in layout.instructions] == expected


@pytest.fixture
def sort_branch(tmp_path):
    image, listings, map_path = assemble(tmp_path, """CODE SEGMENT PARA PUBLIC
ASSUME CS:CODE
entry: JMP SWITCH_LOOP
SWITCH_LOOP:
OR AL,20h
CMP AL,'r'
JNZ SWITCH_LOOP
MOV CS:CODE_PATCH,72h
JMP SWITCH_LOOP
INNER_SORT_LOOP:
INC AX
TESTED_NOT_EQUAL:
CODE_PATCH LABEL BYTE
JAE INNER_SORT_LOOP
MOV BX,SI
JMP INNER_SORT_LOOP
CODE ENDS
END entry
""", com=False, module="sort")
    return build_msdos_layout(image, listings, map_path)


def test_sort_reverse_branch_has_only_two_source_proved_forms(sort_branch):
    site = next(record for record in sort_branch.instructions if record.variants)
    assert [record.insn.mnemonic for record in site.variants] == ["jae", "jb"]
    assert sort_branch.mutable_offsets == (site.off,)
    cases = tuple(InstructionCase(index, "SORT_BRANCH", replace(variant, variants=site.variants), ())
                  for index, variant in enumerate(site.variants))
    library = build_library(cases)
    for index in range(len(cases)):
        failure = library.ops_check_instruction(index, 1024)
        assert failure is None, failure.decode() if failure else ""


def test_sort_reverse_branch_rejects_damaged_source_proof(sort_branch):
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    data = bytearray(sort_branch.image.data)
    data[sort_branch.labels["SORT::SWITCH_LOOP"] + 1] ^= 1
    with pytest.raises(LayoutError, match="unproven SORT reverse-branch"):
        prove_sort_variants(replace(sort_branch.image, data=bytes(data)), sort_branch.instructions,
                            sort_branch.labels, decoder, {"CODE": 0})


def test_sort_reverse_branch_guard_refuses_an_unproved_opcode(sort_branch):
    site = next(record for record in sort_branch.instructions if record.variants)
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    invalid = next(decoder.disasm(bytes((0x74, site.insn.bytes[1])), site.off))
    case = InstructionCase(0, "SORT_BAD_BRANCH", replace(site, insn=invalid, mutable_offsets=()), ())
    library = build_library((case,))
    failure = library.ops_check_instruction(0, 32)
    assert failure and b"unsupported bytes in source-proved instruction variant" in failure


def test_msdos_daa_against_unicorn():
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    insn = next(decoder.disasm(b"\x27", 0x1234))
    case = InstructionCase(0, "MSDOS_DAA", SimpleNamespace(off=insn.address, insn=insn), ())
    library = build_library((case,))
    failure = library.ops_check_instruction(0, 4096)
    assert failure is None, failure.decode() if failure else ""
