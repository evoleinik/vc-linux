"""Reproduce unedited MS-DOS Kermit 3.15's pure-assembly no_network build.

Source files are copied byte-for-byte, never preprocessed. All compatibility
changes are assembler options or explicit tool fixes. Completed outputs are
published only after the entire fresh build succeeds.
"""

from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/mskermit"
MODULES = ("msscmd", "msscom", "mssfil", "mssker", "mssrcv", "mssscp", "msssen", "mssser",
           "mssset", "msssho", "msster", "msuibm", "msgibm", "msxibm", "msyibm", "mszibm")
sys.path.insert(0, str(ROOT))

from tools.build_kermit_jwasm import ensure_jwasm
from translator.listing import parse_listing
from translator.omf import parse_module


def modules() -> list[str]:
    """Keep the assembly module order in the supplied msk315.mak."""
    return list(MODULES)


def verify_sources(source: Path = SOURCE) -> dict[str, str]:
    expected = {}
    for line in (SOURCE / "SOURCES.sha256").read_text().splitlines():
        digest, name = line.split()
        if not re.fullmatch(r"[a-z0-9]+\.(?:asm|c|h|mak)", name):
            raise ValueError(f"invalid pinned Kermit source name {name!r}")
        expected[name] = digest
        path = source / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"Kermit source differs from the unedited archive: {name}")
    if len(expected) != 32:
        raise ValueError("the Kermit source manifest must cover all 32 archive members")
    return expected


def repair_data_listing(listing_path: Path, object_path: Path) -> None:
    """Fix JWasm's stale-buffer rendering of data rows containing STRUC '?'s.

    OMF's initialized mask and exact pre-link data are authoritative here.
    Neither source text nor instruction byte columns/boundaries are changed.
    The linked front end still resolves initializers independently and checks
    these pre-link bytes against the final EXE.
    """
    listing = parse_listing(listing_path, linked=True)
    module = parse_module(object_path.read_bytes())
    segments = {segment.name.upper(): segment for segment in module.segments[1:]}
    text = listing_path.read_text(encoding="latin1")
    lines = text.splitlines()
    for row in listing.lines:
        if not row.bytes or row.segment is None or row.is_instruction:
            continue
        segment = segments[row.segment.upper()]
        raw = bytes(segment.data[row.offset:row.offset + len(row.bytes)])
        if len(raw) != len(row.bytes):
            raise ValueError(f"{listing_path}:{row.lineno}: data listing exceeds OMF segment")
        line = lines[row.lineno - 1]
        prefix = re.match(r"^[0-9A-F]{4,8} ", line)
        if prefix is None:
            raise ValueError("a listed data row has no byte-column offset")
        lines[row.lineno - 1] = (line[:prefix.end()] + raw.hex().upper()).ljust(28) + line[28:]
    text = "\n".join(lines) + "\n"
    text = re.sub(r", \d+ ms,", ", elapsed omitted,", text)
    listing_path.write_text(text, encoding="latin1")


def build(output: Path, source: Path = SOURCE) -> None:
    output, source = output.resolve(), source.resolve()
    if any(output == protected or protected in output.parents or output in protected.parents
           for protected in (SOURCE, source)):
        raise ValueError("Kermit output must be separate from the vendored sources")
    expected = verify_sources(source)
    assembler = ensure_jwasm()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".kermit-", dir=output.parent) as temporary:
        stage = Path(temporary)
        for name in expected:
            shutil.copyfile(source / name, stage / name)
        shutil.copyfile(ROOT / "tools/kermit-options.inc", stage / "assembler-options.inc")
        objects = []
        for module in modules():
            subprocess.run([str(assembler), "-q", "-Zm", "-Cu", "-Dno_network", "-Sg", "-Sa", "-Sl",
                            "-Fiassembler-options.inc", f"-Fl={module}.lst", f"-Fo={module}.obj",
                            f"-Fw={module}.err", module + ".asm"], cwd=stage, check=True)
            repair_data_listing(stage / (module + ".lst"), stage / (module + ".obj"))
            objects.extend(("file", module + ".obj"))
        subprocess.run([str(ROOT / "tools/jwlink/jwlink"), "format", "dos", *objects,
                        "name", "KERMIT.EXE", "option", "map=KERMIT.MAP,verbose,nofarcalls"],
                       cwd=stage, check=True)
        map_file = stage / "KERMIT.MAP"
        text = re.sub(r"(?m)^Created on:.*$", "Created from MS-DOS Kermit 3.15, 15 Sept 1997", map_file.read_text())
        text = re.sub(r"(?m)^Link time:.*$", "Link time: omitted for reproducibility", text)
        map_file.write_text(text)
        verify_sources(stage)
        output.mkdir(parents=True, exist_ok=True)
        for name in ("KERMIT.EXE", "KERMIT.MAP", *[f"{m}.{ext}" for m in modules() for ext in ("lst", "obj")]):
            (stage / name).replace(output / name)
    print(f"Kermit 3.15 no_network: {len(modules())} modules, "
          f"{(output / 'KERMIT.EXE').stat().st_size:,} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--modules", action="store_true")
    arguments = parser.parse_args()
    if arguments.modules:
        print(" ".join(modules()))
    elif arguments.output:
        build(arguments.output)
    else:
        parser.error("an output directory or --modules is required")
