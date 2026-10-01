"""Compiled-C gate: OMF byte/relocation proof and complete computed entries."""

from dataclasses import replace
from pathlib import Path
import os
import re
import subprocess

import pytest

from translator.compiled import (build_compiled_layout, disassemble_modules,
                                 load_modules, parse_compiled_map,
                                 validate_compiled_targets, verify_linked_bytes)
from translator.emit import emit_image
from translator.image import load_image
from translator.layout import LayoutError
from translator.omf import parse_module


ROOT = Path(__file__).resolve().parents[1]
ROGUE = ROOT / "build/rogue"
WATCOM = Path(os.environ.get("WATCOM", Path.home() / "src/vc-linux-wt/tools-cache/openwatcom"))


@pytest.fixture(scope="module")
def compiled_inputs():
    subprocess.run(["make", "rogue"], cwd=ROOT, check=True, capture_output=True)
    image = load_image(ROGUE / "ROGUE.EXE")
    link = parse_compiled_map(ROGUE / "ROGUE.MAP")
    modules = load_modules(link)
    listings = disassemble_modules(link, modules, ROGUE / "disasm", WATCOM)
    return image, link, modules, listings


@pytest.fixture(scope="module")
def rogue_layout(compiled_inputs):
    image, link, modules, listings = compiled_inputs
    return build_compiled_layout(image, link.path, listings=listings)


def test_compiled_map_covers_rogue_pdcurses_and_every_selected_crt_member(compiled_inputs, rogue_layout):
    image, link, modules, listings = compiled_inputs
    assert set(modules) == set(listings) == set(rogue_layout.compiled_modules)
    assert any(Path(path).name == "pdcurses.lib" for path, _ in link.modules.values())
    assert any(Path(path).name == "clibl.lib" for path, _ in link.modules.values())
    assert "cstart" in modules and "dointr" in modules
    assert len(rogue_layout.instructions) > 50000
    assert len(image.relocations) > 3000
    assert "rt_fault(" not in emit_image(rogue_layout, "ROGUE.EXE", "image_rogue")


@pytest.mark.parametrize("in_fixup", (False, True), ids=("opcode", "resolved-fixup"))
def test_compiled_linked_byte_corruption_is_rejected(compiled_inputs, in_fixup):
    image, link, modules, _ = compiled_inputs
    at = image.relocations[0] if in_fixup else 0
    damaged = bytearray(image.data)
    damaged[at] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        verify_linked_bytes(replace(image, data=bytes(damaged)), link, modules)


def test_compiled_mz_relocation_omission_is_rejected(compiled_inputs):
    image, link, modules, _ = compiled_inputs
    with pytest.raises(LayoutError, match="MZ relocation table differs"):
        verify_linked_bytes(replace(image, relocations=image.relocations[1:]), link, modules)


def test_compiled_mz_entry_must_match_link_map_not_just_any_code_start(compiled_inputs):
    image, link, modules, _ = compiled_inputs
    # 0000:0000 is wear_, a valid translated procedure but not the CRT entry.
    assert (image.hdr_cs, image.hdr_ip) != (0, 0)
    with pytest.raises(LayoutError, match="entry point differs"):
        verify_linked_bytes(replace(image, hdr_cs=0, hdr_ip=0), link, modules)


def test_compiled_shifted_module_is_rejected(compiled_inputs):
    image, link, modules, _ = compiled_inputs
    owner = next(key for key in modules if key.endswith("/armor.obj"))
    piece = link.pieces[owner, "armor_TEXT"]
    damaged = replace(link, pieces={**link.pieces, (owner, "armor_TEXT"): replace(piece, address=piece.address + 1)})
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        verify_linked_bytes(image, damaged, modules)


def test_compiled_missing_library_listing_is_rejected(compiled_inputs):
    image, link, modules, listings = compiled_inputs
    damaged = dict(listings)
    del damaged["dointr"]
    with pytest.raises(LayoutError, match="every linked module"):
        build_compiled_layout(image, link.path, listings=damaged)


def test_compiled_missing_instruction_row_is_rejected(compiled_inputs, tmp_path):
    image, link, modules, listings = compiled_inputs
    owner = next(key for key in modules if key.endswith("/armor.obj"))
    text = listings[owner].read_text()
    damaged, count = re.subn(r"(?m)^0000  [^\n]+\n", "", text, count=1)
    assert count == 1
    path = tmp_path / "armor.dis"
    path.write_text(damaged)
    with pytest.raises(LayoutError, match="uncovered initialized CODE byte"):
        build_compiled_layout(image, link.path, listings={**listings, owner: path})


def test_compiled_callback_reclassified_as_data_is_rejected(compiled_inputs, tmp_path):
    image, link, modules, listings = compiled_inputs
    owner = next(key for key in modules if key.endswith("/daemons.obj"))
    segment, offset, _ = modules[owner].publics["doctor_"]
    lines = listings[owner].read_text().splitlines()
    prefix = f"{offset:04X}  "
    index = next(index for index, line in enumerate(lines) if line.startswith(prefix))
    raw = lines[index][6:].split("\t", 1)[0]
    # Preserve bytes and coverage while lying only about CODE-vs-DATA. The
    # original OMF scan/fixup evidence must catch this, independently of WDIS.
    lines[index] = prefix + f"{raw:<48}" + "forged data"
    path = tmp_path / "daemons.dis"
    path.write_text("\n".join(lines) + "\n")
    with pytest.raises(LayoutError, match="CODE data lacks OMF"):
        build_compiled_layout(image, link.path, listings={**listings, owner: path})


def test_compiled_omf_checksum_corruption_is_rejected(compiled_inputs):
    _, _, modules, _ = compiled_inputs
    original = modules["dointr"].raw
    # THEADR has a real checksum in the supplied toolchain's library.
    assert original[3 + int.from_bytes(original[1:3], "little") - 1]
    damaged = original[:4] + bytes([original[4] ^ 1]) + original[5:]
    with pytest.raises(LayoutError, match="OMF checksum mismatch"):
        parse_module(damaged)


def _procedure(layout, name):
    matches = [off for symbol, off in layout.procedures.items() if symbol.endswith("::" + name)]
    assert len(matches) == 1, (name, matches)
    return matches[0]


def test_compiled_callbacks_include_daemons_options_and_local_crt_functions(rogue_layout):
    targets = set(rogue_layout.compiled_indirect_targets)
    for name in ("runners_", "doctor_", "stomach_", "swander_", "rollwand_",
                 "unconfuse_", "nohaste_", "turn_see_", "visuals_", "come_down_",
                 "put_bool_", "put_str_", "get_bool_", "get_str_", "charge_str_", "ring_num_"):
        assert _procedure(rogue_layout, name) in targets
    # These CRT callbacks are local, so they have no map public symbols.
    local = [row for row in rogue_layout.listing.lines if not row.is_instruction and "offset" in row.source]
    assert local, "WDIS must retain data-in-code table metadata"
    assert len(targets) > 1000


@pytest.mark.parametrize("callback", ("runners_", "come_down_", "put_bool_"))
def test_missing_computed_callback_target_fails_before_play(rogue_layout, callback):
    target = _procedure(rogue_layout, callback)
    damaged = replace(rogue_layout, instructions=[record for record in rogue_layout.instructions if record.off != target])
    with pytest.raises(LayoutError, match="indirect/procedure target.*lacks a translated instruction"):
        validate_compiled_targets(damaged)


def test_compiled_dointr_checks_every_computed_retf_target(rogue_layout):
    table = rogue_layout.compiled_interrupt_targets
    assert len(table) == 256 and table == tuple(table[0] + vector * 3 for vector in range(256))
    starts = {record.off for record in rogue_layout.instructions}
    assert set(table) <= starts
    # This entry is not a public/relocation target; only the proven 3*n table
    # arithmetic reveals it. Removing it must fail the build-time target gate.
    target = table[0x16]
    damaged = replace(rogue_layout, instructions=[record for record in rogue_layout.instructions if record.off != target])
    with pytest.raises(LayoutError, match="indirect/procedure target.*lacks a translated instruction"):
        validate_compiled_targets(damaged)


def test_compiled_unicorn_gate_contains_every_distinct_rogue_instruction(rogue_layout):
    from translator_support.ops_build import load_cases

    def key(record, relocations):
        return (bytes(record.insn.bytes), record.off,
                tuple(at - record.off for at in relocations if record.off <= at < record.off + record.insn.size))

    tested = {key(case.record, case.relocations) for case in load_cases()}
    expected = {key(record, rogue_layout.image.relocations) for record in rogue_layout.instructions}
    assert expected <= tested, f"{len(expected - tested)} Rogue instructions missing from Unicorn gate"


def test_wdis_jwasm_jwlink_roundtrip_really_changes_unrelocated_instruction_bytes(tmp_path):
    """Executable evidence for choosing the map front end, not a guessed excuse."""
    source = tmp_path / "probe.c"
    source.write_text("int probe(int a) { return a * 7 + 1; }\nint main(void) { return probe(2); }\n")
    # Do not copy unrelated credentials into subprocess kwargs: pytest prints
    # those kwargs when a tool exits unsuccessfully.
    env = {"PATH": os.defpath, "WATCOM": str(WATCOM), "INCLUDE": str(WATCOM / "h")}
    obj, asm, reassembled = (tmp_path / name for name in ("probe.obj", "probe.asm", "jwasm.obj"))
    subprocess.run([str(WATCOM / "binl64/wcc"), "-bt=dos", "-0", "-ml", "-zq", f"-fo={obj}", str(source)],
                   env=env, check=True, capture_output=True)
    subprocess.run([str(WATCOM / "binl64/wdis"), "-a", f"-l={asm}", str(obj)], check=True, capture_output=True)
    subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-Cp", "-Zg", "-Sg",
                    f"-Fl={tmp_path / 'probe.lst'}", f"-Fo={reassembled}", str(asm)], check=True, capture_output=True)
    clib = WATCOM / "lib286/dos/clibl.lib"
    original_exe, original_map = tmp_path / "watcom.exe", tmp_path / "watcom.map"
    subprocess.run([str(WATCOM / "binl64/wlink"), "format", "dos", "name", str(original_exe),
                    "file", str(obj), "option", f"dosseg,nofarcalls,verbose,map={original_map}",
                    "library", str(clib)], env=env, check=True, capture_output=True)
    # JWlink predates this Watcom library's dictionary attribute. Pass every
    # selected CRT member as an original object, bypassing that separate issue.
    link = parse_compiled_map(original_map)
    command = [str(ROOT / "tools/jwlink/jwlink"), "format", "dos", "name", str(tmp_path / "jwasm.exe"),
               "file", str(reassembled), "option", "dosseg,nofarcalls,nodefaultlibs"]
    for index, (name, module) in enumerate(load_modules(link).items()):
        if link.modules[name][1] is None:
            continue
        extracted = tmp_path / f"crt{index}.obj"
        extracted.write_bytes(module.raw)
        command.extend(("file", str(extracted)))
    subprocess.run(command, env=env, check=True, capture_output=True)
    before, after = load_image(original_exe), load_image(tmp_path / "jwasm.exe")
    assert before.data != after.data
    # The lost opcode direction bits are not fixups or linker header fields.
    assert before.data[12:14] == bytes.fromhex("89c2")   # MOV DX,AX
    assert after.data[12:14] == bytes.fromhex("8bd0")
    assert before.data[18:20] == bytes.fromhex("29d0")   # SUB AX,DX
    assert after.data[18:20] == bytes.fromhex("2bc2")
    assert not set(before.relocations) & {12, 13, 18, 19}
