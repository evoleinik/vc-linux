"""Kermit's ordinary linked-listing path must be complete, exact and oracled."""

from dataclasses import replace
from pathlib import Path
import re
import subprocess

import pytest

from tools.build_kermit import modules, repair_data_listing
from tools.build_kermit_jwasm import ensure_jwasm
from translator.image import load_image
from translator.layout import LayoutError
from translator.linked import build_linked_layout, parse_map
from translator_support.ops_build import load_cases


ROOT = Path(__file__).resolve().parents[1]
DIRECTORY = ROOT / "build/kermit"


@pytest.fixture(scope="module")
def kermit_layout():
    subprocess.run(["make", "kermit"], cwd=ROOT, check=True, capture_output=True)
    return build_linked_layout(load_image(DIRECTORY / "KERMIT.EXE"),
                               [DIRECTORY / (module + ".lst") for module in modules()],
                               DIRECTORY / "KERMIT.MAP")


def test_kermit_maps_all_three_original_code_segments(kermit_layout):
    assert len(kermit_layout.instructions) == 49272
    assert {record.line.segment for record in kermit_layout.instructions} == {"CODE", "CODE1", "CODE2"}
    assert all(record.line.is_instruction for record in kermit_layout.instructions)
    assert kermit_layout.labels["MSSKER::START"] == kermit_layout.image.hdr_cs * 16 + kermit_layout.image.hdr_ip
    assert any(record.line.expansion == "JWasm same-segment far call: PUSH CS"
               for record in kermit_layout.instructions)


def test_kermit_oracle_covers_every_emitted_instruction(kermit_layout):
    covered = {(case.record.off, bytes(case.record.insn.bytes), case.record.mutable_offsets,
                tuple(at - case.record.off for at in case.relocations
                      if case.record.off <= at < case.record.off + case.record.insn.size))
               for case in load_cases()}
    expected = {(record.off, bytes(record.insn.bytes), record.mutable_offsets,
                 tuple(at - record.off for at in kermit_layout.image.relocations
                       if record.off <= at < record.off + record.insn.size))
                for record in kermit_layout.instructions}
    assert expected <= covered


def test_kermit_rejects_changed_linked_instruction(kermit_layout):
    record = kermit_layout.instructions[0]
    data = bytearray(kermit_layout.image.data)
    data[record.off] ^= 1
    damaged = replace(kermit_layout.image, data=bytes(data))
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_linked_layout(damaged, [DIRECTORY / (m + ".lst") for m in modules()], DIRECTORY / "KERMIT.MAP")


def test_kermit_rejects_an_omitted_uart_instruction(kermit_layout, tmp_path):
    module = "msxibm"
    original = (DIRECTORY / (module + ".lst")).read_text()
    damaged, count = re.subn(r"(?m)^\w+\s+EC\s+[^\n]*\bin\s+al,dx[^\n]*\n", "", original, count=1, flags=re.I)
    assert count == 1
    path = tmp_path / (module + ".lst")
    path.write_text(damaged)
    with pytest.raises(LayoutError, match="uncovered byte"):
        build_linked_layout(kermit_layout.image,
                            [path if m == module else DIRECTORY / (m + ".lst") for m in modules()],
                            DIRECTORY / "KERMIT.MAP")


@pytest.fixture
def linked_dialect(tmp_path):
    header = """code segment para public 'kcode'
code ends
data segment para public 'kdata'
data ends
DGROUP group data
info struc
field0 dw ?
field1 dw 0
tagseg dw DGROUP
info ends
"""
    sources = {
        "a": header + """code segment
assume cs:code
public entry
extrn finish:far
entry: call finish
ret
code ends
data segment
flags info <>
buffer db 8 dup (0)
address dw flags.field1
total dw size info
count dw length buffer
pointer dd finish
jumps dw 2 dup (finish), finish
inherited info <1>
data ends
end entry
""",
        "b": header + """code segment
assume cs:code
public finish
finish proc far
inc ax
ret
finish endp
code ends
end
""",
    }
    for name, source in sources.items():
        (tmp_path / (name + ".asm")).write_text(source)
        subprocess.run([str(ensure_jwasm()), "-q", "-Zm", "-Cu", "-Sa", "-Sl",
                        f"-Fo={name}.obj", f"-Fl={name}.lst", name + ".asm"],
                       cwd=tmp_path, check=True, capture_output=True)
        repair_data_listing(tmp_path / (name + ".lst"), tmp_path / (name + ".obj"))
    subprocess.run([str(ROOT / "tools/jwlink/jwlink"), "format", "dos", "file", "a.obj", "file", "b.obj",
                    "name", "test.exe", "option", "map=test.map,verbose,nofarcalls"],
                   cwd=tmp_path, check=True, capture_output=True)
    return load_image(tmp_path / "test.exe"), [tmp_path / (m + ".lst") for m in sources], tmp_path / "test.map"


def test_linked_case_classes_far_extern_and_masm_initializers(linked_dialect):
    image, listings, map_path = linked_dialect
    layout = build_linked_layout(image, listings, map_path)
    assert [record.insn.mnemonic for record in layout.instructions] == ["push", "call", "ret", "inc", "retf"]
    assert layout.labels["B::finish"] == next(record.off for record in layout.instructions if record.insn.mnemonic == "inc")
    assert "CODE" in layout.segment_bases
    assert image.relocations


def test_default_structure_segment_fixup_is_verified(linked_dialect):
    image, listings, map_path = linked_dialect
    layout = build_linked_layout(image, listings, map_path)
    data = bytearray(image.data)
    data[layout.labels["A::flags"] + 4] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_linked_layout(replace(image, data=bytes(data)), listings, map_path)


def test_wrapped_module_path_is_read_from_map(linked_dialect):
    image, listings, map_path = linked_dialect
    expected = parse_map(map_path)
    text = map_path.read_text()
    text, count = re.subn(r"(?m)^(a\.asm)(\s+CODE\s+KCODE\s+)", r"very/long/source/path/\1\n\2", text)
    assert count == 1
    map_path.write_text(text)
    assert parse_map(map_path).contributions == expected.contributions
    assert build_linked_layout(image, listings, map_path).instructions
