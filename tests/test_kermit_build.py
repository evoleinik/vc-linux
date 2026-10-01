"""An offline, unedited MS-DOS Kermit build, including its tool-side fixes."""

import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

import pytest

from tools import build_kermit as builder
from tools.build_kermit_jwasm import ensure_jwasm
from translator.image import load_image
from translator.linked import parse_map


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def builds(tmp_path_factory):
    directory = tmp_path_factory.mktemp("kermit-build")
    outputs = (directory / "first", directory / "second")
    for output in outputs:
        builder.build(output)
    return outputs


def test_vendor_archive_members_and_license_are_preserved():
    manifest = builder.verify_sources()
    assert len(manifest) == 32
    assert all(b"\n" not in (builder.SOURCE / name).read_bytes().replace(b"\r\n", b"") for name in manifest)
    upstream = (builder.SOURCE / "UPSTREAM").read_text()
    assert "1d897b06f1a6fa6a14bac401c20cc1b12b04e9f330ff4dc45f8d826d2aa60e72" in upstream
    assert "435aa3db8275fdbf2462e6bb6d6e115815d4769217cda0a7f6466d917b6d47b2" in upstream
    assert "Wayback Machine" in upstream and "1998-05-28" in upstream
    license_text = (builder.SOURCE / "LICENSE").read_text()
    assert "Copyright (C) 1982, 1997, Trustees of Columbia University in the City of New York" in license_text
    evidence = (builder.SOURCE / "licensing.html").read_text()
    assert "As of 20 July 2011" in evidence and "27 September 2011" in evidence
    assert "holds the copyright should be considered to have the Revised 3-Clause BSD" in evidence
    assert 'href="mskermit.html">MS-DOS Kermit</a>' in evidence
    # Check the supplied cache independently when present; CI only needs the
    # vendored source members and their complete pinned hash manifest.
    archive = ROOT / "build/mskermit/msk315src.zip"
    if archive.exists():
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == "1d897b06f1a6fa6a14bac401c20cc1b12b04e9f330ff4dc45f8d826d2aa60e72"
        with zipfile.ZipFile(archive) as source:
            assert set(source.namelist()) == set(manifest)
            for name in manifest:
                assert source.read(name) == (builder.SOURCE / name).read_bytes()


def test_pure_assembly_original_module_order(builds):
    original = (builder.SOURCE / "msk315.mak").read_text()
    link_order = re.search(r"kermit.exe:\s*(.*?)\n\s*LINK", original, re.S)[1]
    assert builder.modules() == re.findall(r"(\w+)\.obj", link_order)[:16]
    link = parse_map(builds[0] / "KERMIT.MAP")
    assert {module for module, _ in link.contributions} == {module.upper() for module in builder.modules()}
    assert len(builder.modules()) == 16
    assert not any(module.startswith("MSN") for module, _ in link.contributions)
    image = load_image(builds[0] / "KERMIT.EXE")
    assert b" MS-DOS Kermit: 3.15 15 Sept 1997" in image.data
    assert len(image.relocations) == 1037


def test_images_maps_objects_and_listings_are_reproducible(builds):
    names = ["KERMIT.EXE", "KERMIT.MAP"] + [f"{m}.{ext}" for m in builder.modules() for ext in ("lst", "obj")]
    for name in names:
        assert (builds[0] / name).read_bytes() == (builds[1] / name).read_bytes(), name
    assert (builds[0] / "KERMIT.EXE").stat().st_size == 228476
    builder.verify_sources()


def test_assembler_receives_original_bytes_and_command_line_no_network(tmp_path, monkeypatch):
    original_run = builder.subprocess.run
    assembled = []

    def checked_run(command, **kwargs):
        if str(command[0]).endswith("/jwasm"):
            builder.verify_sources(Path(kwargs["cwd"]))
            assert "-Dno_network" in command and "-Zm" in command
            assert command[-1].endswith(".asm") and not command[-1].startswith("msn")
            assembled.append(command[-1])
        return original_run(command, **kwargs)

    monkeypatch.setattr(builder.subprocess, "run", checked_run)
    builder.build(tmp_path / "checked")
    assert assembled == [m + ".asm" for m in builder.modules()]


def test_changed_source_cannot_reuse_an_old_success(builds, tmp_path):
    staged_source = tmp_path / "changed-source"
    shutil.copytree(builder.SOURCE, staged_source)
    source = staged_source / "mssker.asm"
    source.write_bytes(source.read_bytes() + b"; changed\r\n")
    old_executable = (builds[0] / "KERMIT.EXE").read_bytes()
    with pytest.raises(ValueError, match="source differs.*mssker.asm"):
        builder.build(builds[0], staged_source)
    assert (builds[0] / "KERMIT.EXE").read_bytes() == old_executable


def test_build_cannot_overwrite_vendor():
    for target in (builder.SOURCE, builder.SOURCE / "generated", ROOT):
        with pytest.raises(ValueError, match="separate from the vendored sources"):
            builder.build(target)


def test_masm_16_bit_constant_wrapping_is_tool_side(tmp_path):
    source = tmp_path / "wrap.asm"
    source.write_text(".model tiny\n.code\norg 100h\nmov al,0ffffh\nmov ax,159091\nret\nend\n")
    unmodified = source.read_bytes()
    stock = subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-Zm", "-bin", "-Fo=stock.bin", str(source)],
                           cwd=tmp_path, capture_output=True)
    assert stock.returncode != 0  # Reproduce the old tool's actual rejection.
    subprocess.run([str(ensure_jwasm()), "-q", "-Zm", "-bin", "-Fo=wrapped.bin", str(source)],
                   cwd=tmp_path, check=True, capture_output=True)
    assert (tmp_path / "wrapped.bin").read_bytes() == bytes.fromhex("b0ffb8736dc3")
    assert source.read_bytes() == unmodified


def test_force_listing_and_ascii_fs_gs_are_options_not_source_edits(tmp_path):
    source = tmp_path / "controls.asm"
    source.write_text(".model tiny\n.code\norg 100h\n.xlist\n.sall\nFS equ 1ch\nGS equ 1dh\n"
                      "emit_control macro\nmov al,FS\nmov ah,GS\nendm\nemit_control\nret\nend\n")
    unmodified = source.read_bytes()
    subprocess.run([str(ensure_jwasm()), "-q", "-Zm", "-Sa", "-Sl", "-bin",
                    "-Fi" + str(ROOT / "tools/kermit-options.inc"), "-Fl=controls.lst", "-Fo=controls.bin", str(source)],
                   cwd=tmp_path, check=True, capture_output=True)
    assert (tmp_path / "controls.bin").read_bytes() == bytes.fromhex("b01cb41dc3")
    listing = (tmp_path / "controls.lst").read_text()
    assert re.search(r"B01C\s+1\s+mov al,FS", listing)
    assert re.search(r"B41D\s+1\s+mov ah,GS", listing)
    assert source.read_bytes() == unmodified


def test_data_listing_repair_retains_instruction_evidence(builds, tmp_path):
    listing = tmp_path / "msssho.lst"
    original = (builds[0] / listing.name).read_text()
    match = re.search(r"(?m)^(\w+ )(\w+)(\s+.*stent\s+<stmsg,lsesmsg>.*)$", original)
    assert match
    damaged = original[:match.start(2)] + "FF" * (len(match[2]) // 2) + original[match.end(2):]
    listing.write_text(damaged)
    builder.repair_data_listing(listing, builds[0] / "msssho.obj")
    assert listing.read_text() == original
