"""Offline provenance, unedited-source build and exact TASM identity for VC 4.05."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile

import pytest

from tools import build_vc405 as builder


ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(scope="module")
def builds(tmp_path_factory):
    directory = tmp_path_factory.mktemp("vc405-build")
    outputs = (directory / "first", directory / "second")
    for output in outputs:
        builder.build(output)
    return outputs


def test_vendor_preserves_all_original_bytes_dates_and_identical_bsd_license():
    manifest = builder.verify_sources()
    assert len(manifest) == 19
    assert set(manifest) == {path.name for path in builder.SOURCE.iterdir()} - {"UPSTREAM", "SOURCES.sha256"}
    upstream = (builder.SOURCE / "UPSTREAM").read_text()
    assert "https://github.com/ddanila/vc" in upstream
    assert "dbbb60578dc3a7e4cbc719f2376a3ad685d99040" in upstream
    assert builder.ARCHIVE_SHA256 in upstream
    assert "2026-10-02" in upstream
    assert b"16.06.2000\r\n" in (builder.SOURCE / "VC.ASM").read_bytes()
    assert b"14.06.2000\r\n" in (builder.SOURCE / "VCSETUP.ASM").read_bytes()
    assert (builder.SOURCE / "VC.ASM").read_bytes().endswith(b"\x1a")
    assert (builder.SOURCE / "LICENSE.TXT").read_bytes() == (ROOT / "asm/LICENSE.TXT").read_bytes()
    # Optional original input is extra evidence, never a fresh-checkout dependency.
    archive = ROOT / "build/vc405-inputs/vc405.zip"
    if archive.is_file():
        assert hashlib.sha256(archive.read_bytes()).hexdigest() == builder.ARCHIVE_SHA256
        with zipfile.ZipFile(archive) as zipped:
            assert set(zipped.namelist()) == set(manifest)
            for name in manifest:
                assert zipped.read(name) == (builder.SOURCE / name).read_bytes(), name


@pytest.mark.parametrize("name", builder.PROGRAMS)
def test_each_source_built_image_is_byte_identical_to_genuine_tasm(builds, name):
    built = (builds[0] / name).read_bytes()
    reference = (builder.REFERENCES / name).read_bytes()
    assert hashlib.sha256(reference).hexdigest() == builder.REFERENCE_HASHES[name]
    assert built == reference
    report = next(item for item in json.loads((builds[0] / "comparison.json").read_text())
                  if item["name"] == name)
    assert report == builder.compare(name, built, reference)
    assert report["identical"] and report["first_difference"] is None


def test_images_listings_and_identity_report_are_reproducible(builds):
    for name in builder.output_names():
        assert (builds[0] / name).read_bytes() == (builds[1] / name).read_bytes(), name


def test_each_assembler_input_is_unedited_and_previous_tool_unchanged(tmp_path, monkeypatch):
    run = builder.subprocess.run
    expected = builder.verify_sources()
    stock = (ROOT / "tools/jwasm/jwasm").read_bytes()
    assembled = []

    def checked_run(command, **kwargs):
        if str(command[0]).endswith("/jwasm"):
            staged = Path(kwargs["cwd"])
            for name in expected:
                assert (staged / name).read_bytes() == (builder.SOURCE / name).read_bytes(), name
            assert set(builder.OPTIONS) <= set(command)
            assembled.append(command[-1])
        return run(command, **kwargs)

    monkeypatch.setattr(builder.subprocess, "run", checked_run)
    builder.build(tmp_path / "checked")
    assert assembled == ["VC.ASM", "VCSETUP.ASM"]
    assert (ROOT / "tools/jwasm/jwasm").read_bytes() == stock


def test_changed_source_is_refused_without_replacing_previous_good_outputs(builds, tmp_path):
    altered = tmp_path / "altered"
    shutil.copytree(builder.SOURCE, altered)
    source = altered / "VC.ASM"
    source.write_bytes(source.read_bytes() + b"; planted change\r\n")
    before = (builds[0] / "VC.COM").read_bytes()
    with pytest.raises(ValueError, match="differs from the unedited archive: VC.ASM"):
        builder.build(builds[0], altered)
    assert (builds[0] / "VC.COM").read_bytes() == before


@pytest.mark.parametrize("name", builder.PROGRAMS)
def test_identity_rejects_changed_byte_and_length(builds, name):
    good = (builds[0] / name).read_bytes()
    damaged = bytearray(good)
    damaged[0x100] ^= 1
    with pytest.raises(ValueError, match="file offset 0x100"):
        builder.verify_image(name, bytes(damaged))
    for changed in (good[:-1], good + b"\0"):
        with pytest.raises(ValueError, match="differs from TASM"):
            builder.verify_image(name, changed)


def test_builder_cannot_overwrite_sources_references_or_their_ancestors():
    for target in (ROOT, builder.SOURCE, builder.SOURCE / "output", builder.REFERENCES):
        with pytest.raises(ValueError, match="separate from the vendored"):
            builder.build(target)


@pytest.mark.parametrize("text,expected", [
    (".model tiny\n.code\norg 100h\nlea di,[si+0fff0h]\ncmp ax,3\nlea bx,slot\nret\nslot dw 0\nend\n",
     "8d7cf03d0300bb0a01c30000"),
    (".model tiny\n.code\norg 100h\nworker proc C NEAR\nUSES DS,SI\n"
     "LOCAL one:BYTE, array:WORD:3\nmov one,al\nret\nworker endp\nend\n",
     "558bec83ec081e568846fe5e1f8be55dc3"),
    (".model tiny\nS struc\nfirst label byte\nvalue dw ?\nS ends\n.code\norg 100h\n"
     "mov ax,item.value\nret\nitem S <1234h>\nreserved S ?\nend\n",
     "a10401c334120000"),
])
def test_tasm_syntax_and_encoding_fixes_are_tool_options_only(tmp_path, text, expected):
    source = tmp_path / "fixture.asm"
    source.write_text(text)
    stock = subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-bin", "-Zm", "-Zg",
                            "-Fo=old.com", str(source)], cwd=tmp_path, capture_output=True)
    subprocess.run([str(builder.ensure_jwasm()), "-q", "-bin", "-Zt", "-Zm", "-Zg",
                    "-Fo=new.com", str(source)], cwd=tmp_path, check=True, capture_output=True)
    assert source.read_text() == text
    assert (tmp_path / "new.com").read_bytes() == bytes.fromhex(expected)
    assert stock.returncode != 0 or (tmp_path / "old.com").read_bytes() != bytes.fromhex(expected)
