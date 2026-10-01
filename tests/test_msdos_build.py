"""Offline original-source, reproducibility and byte-identity gates for MS-DOS."""

import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

import pytest

from tools import build_msdos as builder
from tools.build_kermit_jwasm import ensure_jwasm as legacy_jwasm
from translator.linked import parse_map
from translator.listing import parse_listing


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def builds(tmp_path_factory):
    directory = tmp_path_factory.mktemp("msdos-build")
    outputs = (directory / "first", directory / "second")
    for output in outputs:
        builder.build(output)
    return outputs


def test_vendor_manifest_preserves_every_upstream_byte_and_license():
    manifest = builder.verify_sources()
    assert len(manifest) == 57
    assert set(manifest) == {p.relative_to(builder.SOURCE).as_posix()
                             for directory in ("source", "bin")
                             for p in (builder.SOURCE / directory).iterdir()} | {"LICENSE", "README.md"}
    assert "MIT License" in (builder.SOURCE / "LICENSE").read_text()
    upstream = (builder.SOURCE / "UPSTREAM").read_text()
    assert "https://github.com/microsoft/MS-DOS" in upstream
    assert "2d04cacc5322951f187bb17e017c12920ac8ebe2" in upstream
    assert "2026-10-02" in upstream
    cache = ROOT / "build/msdos"
    if cache.is_dir():
        for name in manifest:
            origin = cache / ("v2.0/" + name if name.startswith(("source/", "bin/")) else name)
            assert origin.read_bytes() == (builder.SOURCE / name).read_bytes(), name
    # The parity-marked linefeeds and mixed original line endings must survive.
    assert (builder.SOURCE / "source/TDATA.ASM").read_bytes().count(b"\r\x8a") == 4
    assert b"\r\n" not in (builder.SOURCE / "source/EDLIN.ASM").read_bytes()


def test_original_command_comlink_order_and_all_module_contributions(builds):
    original = (builder.SOURCE / "source/COMLINK").read_text().split(";", 1)[0]
    assert re.findall(r"\b[a-z][a-z0-9]*\b", original) == builder.modules("COMMAND.COM")
    assert len(builder.modules()) == 35
    for program in builder.PROGRAMS:
        link = parse_map(builds[0] / (Path(program).stem + ".MAP"))
        assert {module for module, _ in link.contributions} == {
            module.upper() for module in builder.modules(program)
        }


def test_images_maps_objects_listings_and_comparison_are_reproducible(builds):
    for name in builder.output_names():
        assert (builds[0] / name).read_bytes() == (builds[1] / name).read_bytes(), name


def test_per_program_identity_report_is_measured_and_pinned(builds):
    expected = json.loads((builder.SOURCE / "IDENTITY.json").read_text())
    measured = [builder.compare(program, (builds[0] / program).read_bytes(),
                                (builder.SOURCE / "bin" / program).read_bytes())
                for program in builder.PROGRAMS]
    assert measured == expected
    assert json.loads((builds[0] / "comparison.json").read_text()) == measured
    assert [row["first_difference"] for row in measured] == [1, 0, 9, 2, 12, 2, 2]


@pytest.mark.parametrize("built,shipped,identical,first", [
    (b"abc", b"abc", True, None), (b"adc", b"abc", False, 1),
    (b"abc", b"ab", False, 2), (b"ab", b"abc", False, 2), (b"", b"x", False, 0),
])
def test_identity_comparator_includes_length_only_differences(built, shipped, identical, first):
    report = builder.compare("FIXTURE.COM", built, shipped)
    assert report["identical"] == identical
    assert report["first_difference"] == first


def test_each_assembler_input_is_unedited_with_versioned_include_aliases(tmp_path, monkeypatch):
    run = builder.subprocess.run
    assembled = []
    expected = builder.verify_sources()

    def checked_run(command, **kwargs):
        if str(command[0]).endswith("/jwasm"):
            module = Path(command[-1]).stem
            program = next(program for program in builder.PROGRAMS if module in builder.modules(program))
            builder.verify_staged_sources(Path(kwargs["cwd"]), expected, program)
            assert all(option in command for option in ("-Zm", "-Cu", "-Sa", "-Sg", "-Sl"))
            assert "-Fiassembler-options.inc" in command
            assembled.append(module)
        return run(command, **kwargs)

    monkeypatch.setattr(builder.subprocess, "run", checked_run)
    builder.build(tmp_path / "checked")
    assert assembled == builder.modules()


def test_changed_source_cannot_reuse_an_old_success(builds, tmp_path):
    modified = tmp_path / "changed-source"
    shutil.copytree(builder.SOURCE, modified)
    target = modified / "source/COMMAND.ASM"
    target.write_bytes(target.read_bytes() + b"; planted source change\r\n")
    old = (builds[0] / "COMMAND.COM").read_bytes()
    with pytest.raises(ValueError, match="differs from the unedited upstream: source/COMMAND.ASM"):
        builder.build(builds[0], modified)
    assert (builds[0] / "COMMAND.COM").read_bytes() == old


def test_build_cannot_overwrite_vendor_or_its_ancestors():
    for target in (builder.SOURCE, builder.SOURCE / "new", ROOT):
        with pytest.raises(ValueError, match="separate from the vendored sources"):
            builder.build(target)


def test_listing_data_repair_preserves_instruction_fixup_markers(builds, tmp_path):
    listing = tmp_path / "more.lst"
    shutil.copyfile(builds[0] / "more.lst", listing)
    before = listing.read_bytes()
    assert re.search(rb"BA\s+[0-9A-F]+[os].*MOV\s+DX,OFFSET BADVER", before)
    builder.repair_data_listing(listing, builds[0] / "more.obj")
    assert listing.read_bytes() == before
    assert sum(row.is_instruction for row in parse_listing(listing, linked=True, flat=True).lines) > 70


def test_command_mz_conversion_preserves_third_group_and_rejects_fixups(builds):
    raw = (builds[0] / "COMMAND.MZ").read_bytes()
    com = (builds[0] / "COMMAND.COM").read_bytes()
    assert builder.exe2com(raw) == com
    link = parse_map(builds[0] / "COMMAND.MAP")
    exec_address = link.segments["ZEXEC_CODE"][0]
    # A code signature at the MAP address is a check, never a placement heuristic.
    assert com[exec_address - 0x100:exec_address - 0x100 + 7] == bytes.fromhex("0e1fb80033cd21")
    assert com[exec_address:exec_address + 7] != bytes.fromhex("0e1fb80033cd21")
    for offset, value in ((6, 1), (20, 0), (22, 1), (8, 0)):
        bad = bytearray(raw)
        struct.pack_into("<H", bad, offset, value)
        with pytest.raises(ValueError):
            builder.exe2com(bad)


def test_sort_exemod_bounds_allocation_and_changes_only_loader_metadata(builds):
    raw = (builds[0] / "SORT.MZ").read_bytes()
    final = (builds[0] / "SORT.EXE").read_bytes()
    assert struct.unpack_from("<2H", raw, 10) == (0, 0xFFFF)
    assert struct.unpack_from("<2H", final, 10) == (0, 1)
    assert len(raw) == len(final)
    assert [i for i, (a, b) in enumerate(zip(raw, final)) if a != b] == [12, 13]
    assert raw[:12] + b"\x01\x00" + raw[14:] == final
    assert builder.sort_exemod(raw) == final
    # The linked stack is already initialized in the file, so e_minalloc=0
    # is correct; this policy must not invent an uninitialized stack extent.
    header_size = struct.unpack_from("<H", raw, 8)[0] * 16
    stack_start, stack_size, _, _ = parse_map(builds[0] / "SORT.MAP").segments["CSTACK"]
    assert stack_size == 128 + 96  # original SORT.ASM:58 and :414
    assert raw[header_size + stack_start:header_size + stack_start + stack_size] == bytes(stack_size)
    shipped = (builder.SOURCE / "bin/SORT.EXE").read_bytes()
    assert struct.unpack_from("<2H", shipped, 10) == (1, 1)


def test_sort_exemod_rejects_invalid_headers_and_insufficient_allocation(builds):
    raw = (builds[0] / "SORT.MZ").read_bytes()
    for offset, value in ((0, 0), (2, 512), (4, 0), (8, 0), (8, 0xFFFF), (10, 2), (6, 1)):
        bad = bytearray(raw)
        struct.pack_into("<H", bad, offset, value)
        with pytest.raises(ValueError):
            builder.sort_exemod(bad)
    for bad in (raw[:20], raw[:-1], raw + b"\x00"):
        with pytest.raises(ValueError):
            builder.sort_exemod(bad)


# Every fixture is original syntax assembled without preprocessing. The prior
# Kermit tool is the pinned upstream assembler plus its existing unrelated fixes.
LEGACY_FIXTURES = {
    "escaped_directives": b"maker macro\n&.xcref\ninner macro\ndb 7\n&endm\n&.cref\nendm\n"
                          b".model tiny\n.code\norg 100h\nmaker\ninner\nret\nend\n",
    "percent_concat": b"maker macro target\ntarget: db 7\nendm\n.model tiny\n.code\norg 100h\n"
                      b"n=1\nmaker code&%n\nret\nend\n",
    "empty_segment_alignment": b"code_seg segment public\ncode_seg ends\ncode_seg segment byte public\norg 100h\n"
                               b"db 7\nret\ncode_seg ends\nend\n",
    "parity_linefeed": b".model tiny\n.code\norg 100h\ndb 7\r\x8aret\r\nend\r\n",
    "if2_forward_external": b".model tiny\n.code\norg 100h\nif2\nifndef later\nextrn later:near\n"
                            b"endif\nendif\ncall later\nlater: ret\nend\n",
    "contextual_labels": b".model tiny\n.code\norg 100h\ncall OUT\ncall IFDIF\ncall PAGE\njmp TEST\n"
                         b"out dx,al\nin al,dx\nOUT: ret\nIFDIF: ret\nPAGE: ret\nTEST: test ax,ax\nret\nend\n",
}


@pytest.mark.parametrize("name", LEGACY_FIXTURES)
def test_legacy_masm_tool_fixes_are_required_without_source_edits(name, tmp_path):
    source = tmp_path / (name + ".asm")
    original = LEGACY_FIXTURES[name]
    source.write_bytes(original)
    old = subprocess.run([str(legacy_jwasm()), "-q", "-Zm", "-bin", "-Fo=old.bin", str(source)],
                         cwd=tmp_path, capture_output=True)
    assert old.returncode != 0, old.stdout.decode(errors="replace")
    result = subprocess.run([str(builder.ensure_jwasm()), "-q", "-Zm", "-bin", "-Fo=new.bin", str(source)],
                            cwd=tmp_path, capture_output=True)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    assert source.read_bytes() == original
    if name == "if2_forward_external":
        assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("e80000c3")
    elif name == "contextual_labels":
        assert (tmp_path / "new.bin").read_bytes().endswith(bytes.fromhex("ee ecc3c3c385c0c3"))
    else:
        assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("07c3")


def test_force_include_resolves_keyword_collisions_without_vendor_edits(tmp_path):
    source = tmp_path / "keywords.asm"
    source.write_text("WAIT equ 77\nFSAVE equ 3\n.model tiny\n.code\norg 100h\n"
                      "SYSCALL:\nmov ah,WAIT\nmov al,FSAVE\nret\nend\n")
    original = source.read_bytes()
    old = subprocess.run([str(builder.ensure_jwasm()), "-q", "-Zm", "-bin", "-Fo=old.bin", str(source)],
                         cwd=tmp_path, capture_output=True)
    assert old.returncode != 0
    subprocess.run([str(builder.ensure_jwasm()), "-q", "-Zm", "-bin", "-Fo=new.bin",
                    "-Fi" + str(ROOT / "tools/msdos-options.inc"), str(source)], cwd=tmp_path, check=True)
    assert source.read_bytes() == original
    assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("b44db003c3")


def test_if1_if2_are_reexecuted_on_later_passes(tmp_path):
    source = tmp_path / "real_passes.asm"
    source.write_text(".model tiny\n.code\norg 100h\nif1\ndb 1\nendif\nif2\ndb 2\nendif\nret\nend\n")
    for assembler, name in ((legacy_jwasm(), "old"), (builder.ensure_jwasm(), "new")):
        subprocess.run([str(assembler), "-q", "-Zm", "-bin", f"-Fo={name}.bin", str(source)],
                       cwd=tmp_path, check=True, capture_output=True)
    assert (tmp_path / "old.bin").read_bytes() == bytes.fromhex("0102c3")
    assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("02c3")


def test_reassigned_relocatable_return_alias_uses_current_pass_value(tmp_path):
    # DOSMAC_v211's condret remembers its latest RET in an assembly-time
    # relocatable variable. Treating that assignment as a stale forward label
    # silently emits JZ $ (74FE), hanging COMMAND's redirection check.
    source = tmp_path / "return_alias.asm"
    original = (b".model tiny\n.code\norg 100h\nfirst: ret\nret_l=first\n"
                b"cmp byte ptr ds:[0],0\njz ret_l\nnop\nsecond: ret\nret_l=second\n"
                b"cmp byte ptr ds:[1],0\njnz ret_l\nret\nend\n")
    source.write_bytes(original)
    for assembler, name in ((legacy_jwasm(), "old"), (builder.ensure_jwasm(), "new")):
        subprocess.run([str(assembler), "-q", "-Zm", "-bin", f"-Fo={name}.bin", str(source)],
                       cwd=tmp_path, check=True, capture_output=True)
    assert source.read_bytes() == original
    assert (tmp_path / "old.bin").read_bytes() == bytes.fromhex("c3803e00000074fe90c3803e01000075fec3")
    # Both -8 displacements land on their immediately preceding RET. These
    # independently calculated bytes are not obtained from any generated map.
    assert (tmp_path / "new.bin").read_bytes() == bytes.fromhex("c3803e00000074f890c3803e01000075f8c3")
