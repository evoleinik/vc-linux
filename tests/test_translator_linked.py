"""Multi-module placement, operand patches and reproducible GW-BASIC build."""

from pathlib import Path
import re
import subprocess

import pytest

from tools.build_gwbasic import modules
from translator.emit import emit_image
from translator.image import load_image
from translator.layout import LayoutError
from translator.linked import build_linked_layout, parse_map
from translator.listing import parse_listing


ROOT = Path(__file__).resolve().parents[1]
GWB = ROOT / "build" / "gwbasic"


@pytest.fixture(scope="module")
def gwbasic_layout():
    subprocess.run(["make", "gwbasic"], cwd=ROOT, check=True, capture_output=True)
    return build_linked_layout(load_image(GWB / "GWBASIC.EXE"),
                               [GWB / (module + ".lst") for module in modules()],
                               GWB / "GWBASIC.MAP")


def test_gwbasic_uses_every_module_at_map_addresses(gwbasic_layout):
    link = parse_map(GWB / "GWBASIC.MAP")
    assert {module for module, _ in link.contributions} == set(modules())
    assert "OEMCBK" in modules()  # omitted by the fork's old MASM link file
    for module in modules():
        listing = parse_listing(GWB / (module + ".lst"), linked=True)
        for name, label in listing.labels.items():
            key = f"{module}::{name}"
            assert gwbasic_layout.labels[key] == link.contributions[(module, label.segment)].address + label.offset
    assert gwbasic_layout.instructions
    assert "rt_fault(" not in emit_image(gwbasic_layout, "GWBASIC.EXE", "image_gwbasic")


def test_link_map_requires_all_and_only_listings(gwbasic_layout):
    with pytest.raises(LayoutError, match="exactly once"):
        build_linked_layout(gwbasic_layout.image, [GWB / (module + ".lst") for module in modules()[:-1]],
                            GWB / "GWBASIC.MAP")


def test_link_map_shifted_module_is_rejected(gwbasic_layout, tmp_path):
    text = (GWB / "GWBASIC.MAP").read_text()
    match = re.search(r"(ADVGRP\.ASM\s+CSEG\s+CODESG\s+)([0-9a-f]+):([0-9a-f]+)", text)
    assert match
    replacement = match[1] + match[2] + f":{int(match[3], 16) + 16:04x}"
    path = tmp_path / "bad.map"
    path.write_text(text[:match.start()] + replacement + text[match.end():])
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        build_linked_layout(gwbasic_layout.image, [GWB / (module + ".lst") for module in modules()], path)


def test_link_map_wrong_contribution_size_is_rejected(gwbasic_layout, tmp_path):
    text = (GWB / "GWBASIC.MAP").read_text()
    text, changes = re.subn(r"(ADVGRP\.ASM\s+CSEG\s+CODESG\s+\w+:\w+\s+)\w+",
                           r"\g<1>00000001", text)
    assert changes == 1
    path = tmp_path / "bad.map"
    path.write_text(text)
    with pytest.raises(LayoutError, match="length disagrees"):
        build_linked_layout(gwbasic_layout.image, [GWB / (module + ".lst") for module in modules()], path)


def test_missing_module_instruction_row_is_rejected(gwbasic_layout, tmp_path):
    original = (GWB / "OEMCBK.lst").read_text()
    damaged, count = re.subn(r"(?m)^0002 B80000[^\n]*\n", "", original)
    assert count == 1
    path = tmp_path / "OEMCBK.lst"
    path.write_text(damaged)
    listings = [path if module == "OEMCBK" else GWB / (module + ".lst") for module in modules()]
    with pytest.raises(LayoutError, match="uncovered byte"):
        build_linked_layout(gwbasic_layout.image, listings, GWB / "GWBASIC.MAP")


def test_gwbasic_reachable_data_and_live_operands_are_translated(gwbasic_layout):
    recovered = [record for record in gwbasic_layout.instructions if not record.line.is_instruction]
    assert len(recovered) > 100
    patches = [record for record in gwbasic_layout.instructions if record.mutable_offsets]
    assert len(patches) == 6
    assert [record.insn.mnemonic for record in patches].count("mov") == 4
    assert [record.insn.mnemonic for record in patches].count("ljmp") == 2
    assert len(gwbasic_layout.mutable_offsets) == 16
    by_offset = {record.off: record for record in gwbasic_layout.instructions}
    for name in ("OEMCBK::CBKDS", "OEMCBK::IMDS", "OEMEV::ITICDS", "OEMSND::IRQ0DS"):
        record = by_offset[gwbasic_layout.labels[name]]
        assert record.mutable_offsets == (1, 2)
    # Initialization begins with the old INS86 DB macro, not a source MOV.
    assert not by_offset[gwbasic_layout.labels["GWINIT::INIT"]].line.is_instruction
    # The inline token after SYNCHR must not become a bogus instruction.
    assert any(getattr(record, "return_skip", 0) == 1 for record in gwbasic_layout.instructions)


def test_gwbasic_rebuild_is_byte_reproducible(gwbasic_layout, tmp_path):
    destination = tmp_path / "rebuild"
    subprocess.run([str(ROOT / ".venv/bin/python"), str(ROOT / "tools/build_gwbasic.py"), str(destination)],
                   cwd=ROOT, check=True, capture_output=True)
    original = (GWB / "GWBASIC.EXE").read_bytes()
    assert (destination / "GWBASIC.EXE").read_bytes() == original
    assert (destination / "GWBASIC.MAP").read_bytes() == (GWB / "GWBASIC.MAP").read_bytes()
    version = (ROOT / "third_party/gwbasic/UPSTREAM").read_text().split()[1]
    assert ("GW-BASIC " + version + " version").encode() in original
    for module in modules():
        assert (destination / (module + ".lst")).read_bytes() == (GWB / (module + ".lst")).read_bytes()


def test_two_module_exe_places_code_data_and_mz_relocations(tmp_path):
    sources = {
        "A": """CSEG SEGMENT PARA PUBLIC 'CODE'
ASSUME CS:CSEG
EXTRN finish:NEAR
PUBLIC entry
entry: MOV AX,1234h
MOV DX,SEG first
CALL finish
RET
CSEG ENDS
DGROUP GROUP DSEG
DSEG SEGMENT PARA PUBLIC 'DATA'
PUBLIC first
first DW 203h
DSEG ENDS
END entry
""",
        "B": """CSEG SEGMENT PARA PUBLIC 'CODE'
ASSUME CS:CSEG
PUBLIC finish
finish: INC AX
RET
CSEG ENDS
DGROUP GROUP DSEG
DSEG SEGMENT PARA PUBLIC 'DATA'
second DW OFFSET finish
DSEG ENDS
END
""",
    }
    for module, source in sources.items():
        (tmp_path / (module + ".ASM")).write_text(source)
        subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-Zm", "-Sg",
                        f"-Fl={module}.lst", f"-Fo={module}.OBJ", module + ".ASM"],
                       cwd=tmp_path, check=True, capture_output=True)
    subprocess.run([str(ROOT / "tools/jwlink/jwlink"), "format", "dos", "file", "A.OBJ", "file", "B.OBJ",
                    "name", "SMALL.EXE", "option", "dosseg,map=SMALL.MAP,verbose"],
                   cwd=tmp_path, check=True, capture_output=True)
    loaded = load_image(tmp_path / "SMALL.EXE")
    layout = build_linked_layout(loaded, [tmp_path / "A.lst", tmp_path / "B.lst"], tmp_path / "SMALL.MAP")
    link = parse_map(tmp_path / "SMALL.MAP")
    assert layout.labels["B::finish"] == link.contributions[("B", "CSEG")].address
    assert layout.labels["B::second"] == link.contributions[("B", "DSEG")].address
    assert len(loaded.relocations) == 1
    assert [record.insn.mnemonic for record in layout.instructions] == ["mov", "mov", "call", "ret", "inc", "ret"]
    assert not layout.mutable_offsets
    generated = tmp_path / "small.c"
    generated.write_text(emit_image(layout, "SMALL.EXE", "small_image"))
    subprocess.run(["cc", "-fsyntax-only", "-I" + str(ROOT / "runtime"), str(generated)], check=True,
                   capture_output=True)
