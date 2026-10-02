"""Reproduce VC 4.05's two TASM binaries from the unedited, offline sources."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "asm405"
REFERENCES = ROOT / "tests/fixtures/vc405-tasm"
ARCHIVE_SHA256 = "c4ede94b833ea81da24483231de157bd64b67cf1ec66a77be383490046889c3e"
PROGRAMS = ("VC.COM", "VCSETUP.COM")
REFERENCE_HASHES = {
    "VC.COM": "b408f14da5bcba174f5e86107437b22b2863ee6ec72f79bdadf1b812607405fb",
    "VCSETUP.COM": "64f629207d16c4025c1e824fe11445dec8d9ea584d55c55b374a977ff5bbf703",
}
OPTIONS = ("-q", "-bin", "-Zt", "-Zm", "-Zg", "-Sg", "-Sa", "-Fiassembler-options.inc")
sys.path.insert(0, str(ROOT))

from tools.build_vc405_jwasm import ensure_jwasm


def verify_sources(source: Path = SOURCE) -> dict[str, str]:
    expected = {}
    for line in (SOURCE / "SOURCES.sha256").read_text().splitlines():
        digest, name = line.split()
        if not re.fullmatch(r"[A-Z0-9]+\.(?:ASM|INC|EXT|BAT|TXT)", name):
            raise ValueError(f"invalid pinned VC 4.05 source name {name!r}")
        path = source / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"VC 4.05 source differs from the unedited archive: {name}")
        expected[name] = digest
    if len(expected) != 19:
        raise ValueError("the VC 4.05 manifest must cover all 19 archive members")
    if (source / "LICENSE.TXT").read_bytes() != (ROOT / "asm/LICENSE.TXT").read_bytes():
        raise ValueError("VC 4.05 and VC 4.99.09 must carry byte-identical BSD licences")
    return expected


def compare(name: str, actual: bytes, reference: bytes) -> dict:
    first = next((i for i, pair in enumerate(zip(actual, reference)) if pair[0] != pair[1]),
                 min(len(actual), len(reference)) if len(actual) != len(reference) else None)
    return {"name": name, "identical": actual == reference, "first_difference": first,
            "built_size": len(actual), "tasm_size": len(reference),
            "built_sha256": hashlib.sha256(actual).hexdigest(),
            "tasm_sha256": hashlib.sha256(reference).hexdigest()}


def verify_image(name: str, actual: bytes) -> dict:
    reference = (REFERENCES / name).read_bytes()
    if hashlib.sha256(reference).hexdigest() != REFERENCE_HASHES[name]:
        raise ValueError(f"the pinned TASM reference SHA-256 does not match: {name}")
    result = compare(name, actual, reference)
    if not result["identical"]:
        raise ValueError(f"{name} differs from TASM at file offset 0x{result['first_difference']:x} "
                         f"(built {len(actual)} bytes, TASM {len(reference)} bytes)")
    return result


def output_names() -> tuple[str, ...]:
    return (*PROGRAMS, *(name + ".lst" for name in PROGRAMS), "comparison.json")


def build(output: Path, source: Path = SOURCE) -> None:
    output, source = output.resolve(), source.resolve()
    if any(output == protected or protected in output.parents or output in protected.parents
           for protected in (SOURCE, source, REFERENCES)):
        raise ValueError("VC 4.05 output must be separate from the vendored sources and references")
    expected = verify_sources(source)
    assembler = ensure_jwasm()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".vc405-", dir=output.parent) as temporary:
        stage = Path(temporary)
        for name in expected:
            shutil.copyfile(source / name, stage / name)
        shutil.copyfile(ROOT / "tools/vc405-options.inc", stage / "assembler-options.inc")
        results = []
        for name in PROGRAMS:
            subprocess.run([str(assembler), *OPTIONS, f"-Fl={name}.lst", f"-Fo={name}",
                            f"-Fw={name}.err", Path(name).stem + ".ASM"],
                           cwd=stage, check=True, capture_output=True)
            listing = stage / (name + ".lst")
            listing.write_bytes(re.sub(rb", \d+ ms,", b", elapsed omitted,", listing.read_bytes()))
            results.append(verify_image(name, (stage / name).read_bytes()))
        (stage / "comparison.json").write_text(json.dumps(results, indent=2) + "\n")
        output.mkdir(parents=True, exist_ok=True)
        for name in output_names():
            (stage / name).replace(output / name)
    for result in results:
        print(f"VC 4.05 {result['name']}: {result['built_size']:,} bytes, identical to TASM 4.1/TLINK 7.1")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    build(parser.parse_args().output)
