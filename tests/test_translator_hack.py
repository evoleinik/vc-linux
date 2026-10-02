"""Hack 1.0.3 uses the same checked OMF/map front end as Rogue.

No guessed function-only disassembly: all linked CRT code, command-table
callbacks, switch branches and every distinct instruction reach the gate.
"""

from dataclasses import replace
from pathlib import Path
import subprocess

import pytest

from translator.compiled import (build_compiled_layout, load_modules,
                                 parse_compiled_map, validate_compiled_targets,
                                 verify_linked_bytes)
from translator.emit import emit_image
from translator.image import load_image
from translator.layout import LayoutError


ROOT = Path(__file__).resolve().parents[1]
HACK = ROOT / "build/hack"


@pytest.fixture(scope="module")
def hack_layout():
    subprocess.run(["make", "hack"], cwd=ROOT, check=True, capture_output=True)
    return build_compiled_layout(load_image(HACK / "HACK.EXE"), HACK / "HACK.MAP")


def test_hack_map_covers_game_shims_and_linked_crt(hack_layout):
    link = parse_compiled_map(HACK / "HACK.MAP")
    modules = load_modules(link)
    assert set(modules) == set(hack_layout.compiled_modules)
    assert any(Path(path).name == "clibl.lib" for path, _ in link.modules.values())
    assert "cstart" in modules
    assert len(hack_layout.instructions) > 30000
    assert len(hack_layout.image.relocations) > 1000
    emitted = emit_image(hack_layout, "HACK.EXE", "image_hack")
    assert "rt_fault(" not in emitted
    assert (ROOT / "build/gen/hack.c").read_text() == emitted


def procedure(layout, name):
    matches = [offset for symbol, offset in layout.procedures.items()
               if symbol.endswith("::" + name)]
    assert len(matches) == 1, (name, matches)
    return matches[0]


def test_hack_command_and_occupation_callbacks_are_translated(hack_layout):
    expected = set(hack_layout.compiled_indirect_targets)
    # Command-table far pointers, including save/quit and the tin-opening
    # command that installs an occupation callback for later turns.
    for name in ("dosave_", "dodone_", "dohelp_", "ddoinv_", "doeat_"):
        assert procedure(hack_layout, name) in expected


@pytest.mark.parametrize("callback", ("dosave_", "dodone_", "dohelp_"))
def test_hack_missing_callback_is_rejected_before_play(hack_layout, callback):
    target = procedure(hack_layout, callback)
    damaged = replace(hack_layout, instructions=[record for record in hack_layout.instructions
                                                if record.off != target])
    with pytest.raises(LayoutError, match="indirect/procedure target.*lacks a translated instruction"):
        validate_compiled_targets(damaged)


@pytest.mark.parametrize("relocation", (False, True), ids=("opcode", "fixup"))
def test_hack_linked_byte_mutation_fails_independent_omf_proof(hack_layout, relocation):
    link = parse_compiled_map(HACK / "HACK.MAP")
    modules = load_modules(link)
    image = hack_layout.image
    at = image.relocations[0] if relocation else hack_layout.instructions[0].off
    data = bytearray(image.data)
    data[at] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        verify_linked_bytes(replace(image, data=bytes(data)), link, modules)


def test_hack_every_distinct_instruction_is_in_unicorn_gate(hack_layout):
    from translator_support.ops_build import load_cases

    def key(record, relocations):
        return (bytes(record.insn.bytes), record.off,
                tuple(at - record.off for at in relocations
                      if record.off <= at < record.off + record.insn.size))

    tested = {key(case.record, case.relocations) for case in load_cases()}
    expected = {key(record, hack_layout.image.relocations)
                for record in hack_layout.instructions}
    assert expected <= tested, f"{len(expected - tested)} Hack instructions missing from Unicorn gate"
