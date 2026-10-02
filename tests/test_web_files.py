"""Immutable DOS-file publication uses the exact references compiled in main."""
from hashlib import sha256
import json
from pathlib import Path

import pytest

from tools.web_files import publish


def manifest(tmp_path, contents):
    entries = []
    for index, (name, data) in enumerate(contents):
        source = tmp_path / f"input-{index}"
        source.write_bytes(data)
        digest = sha256(data).hexdigest()
        entries.append({"name": name, "source": str(source), "size": len(data),
                        "sha256": digest, "asset": f"file.{digest[:12]}.bin"})
    path = tmp_path / "files.web.json"
    path.write_text(json.dumps({"format": 1, "files": entries}))
    return path, entries


def test_exact_hashed_publication_and_repeat_preserve_mtime(tmp_path):
    source, entries = manifest(tmp_path, [("SRC/VC.ASM", b"original\r\n"),
                                          ("COPY.ASM", b"original\r\n"), ("EMPTY.TXT", b"")])
    site, work = tmp_path / "site", tmp_path / "work"
    assert publish(source, site, work) == (2, 10)
    before = {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in site.iterdir()}
    assert set(before) == {entry["asset"] for entry in entries}
    assert publish(source, site, work) == (2, 10)
    assert before == {path.name: (path.read_bytes(), path.stat().st_mtime_ns) for path in site.iterdir()}
    for entry in entries:
        assert sha256((site / entry["asset"]).read_bytes()).hexdigest() == entry["sha256"]


@pytest.mark.parametrize("corruption", ["size", "sha256", "asset", "source"])
def test_changed_inputs_are_refused_before_publication(tmp_path, corruption):
    source, entries = manifest(tmp_path, [("FIRST.TXT", b"first"), ("OTHER.TXT", b"second")])
    if corruption == "source":
        Path(entries[1]["source"]).write_bytes(b"edited")
    else:
        entries[1][corruption] = 123 if corruption == "size" else "wrong"
        source.write_text(json.dumps({"format": 1, "files": entries}))
    site, work = tmp_path / "site", tmp_path / "work"
    with pytest.raises(ValueError, match="changed since the main"):
        publish(source, site, work)
    assert not site.exists(), "a stale main must not silently receive newer content"


def test_old_urls_are_retired_recoverably_and_missing_files_are_repaired(tmp_path):
    source, entries = manifest(tmp_path, [("FILE.TXT", b"before")])
    site, work = tmp_path / "site", tmp_path / "work"
    publish(source, site, work)
    old_name = entries[0]["asset"]
    unrelated = site / "user.bin"
    unrelated.write_bytes(b"keep")
    source, entries = manifest(tmp_path, [("FILE.TXT", b"after")])
    publish(source, site, work)
    assert not (site / old_name).exists()
    assert [path.read_bytes() for path in (work / "retired-files").iterdir()] == [b"before"]
    current = site / entries[0]["asset"]
    current.unlink()
    publish(source, site, work)
    assert current.read_bytes() == b"after"
    assert unrelated.read_bytes() == b"keep"


def test_generated_symlink_is_not_replaced(tmp_path):
    source, entries = manifest(tmp_path, [("FILE.TXT", b"original")])
    site, work = tmp_path / "site", tmp_path / "work"
    site.mkdir()
    victim = tmp_path / "untouched"
    victim.write_bytes(b"user file")
    (site / entries[0]["asset"]).symlink_to(victim)
    with pytest.raises(ValueError, match="symlink"):
        publish(source, site, work)
    assert victim.read_bytes() == b"user file"
