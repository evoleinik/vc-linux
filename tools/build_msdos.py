"""Reproducibly build the unedited Microsoft MS-DOS 2 source programs offline.

JWasm receives exact source bytes, including their mixed line endings and
historical EOF padding. Dialect adjustments belong to assembler options/tools.
The Microsoft release files are comparison evidence, never build inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/msdos2"
PROGRAMS = {
    "COMMAND.COM": ("command", "rucode", "rdata", "init", "uinit", "tcode", "tcode2",
                    "tcode3", "tcode4", "tcode5", "tucode", "copy", "copyproc", "cparse", "tdata", "tspc"),
    "EDLIN.COM": ("edlin", "edlproc", "edlmes"),
    "DEBUG.COM": ("debug", "debcom1", "debcom2", "debasm", "debuasm", "debconst", "debmes", "debdata"),
    "FIND.EXE": ("find", "findmes"),
    "MORE.COM": ("more", "moremes"),
    "SORT.EXE": ("sort", "sortmes"),
    "FC.EXE": ("fc", "fcmes"),
}
sys.path.insert(0, str(ROOT))

from tools.build_msdos_jwasm import ensure_jwasm
from translator.listing import parse_listing
from translator.omf import parse_module


def repair_data_listing(listing_path: Path, object_path: Path) -> None:
    """Repair only data byte columns: MS-DOS code segments have no CODE class."""
    listing = parse_listing(listing_path, linked=True, flat=True)
    module = parse_module(object_path.read_bytes())
    segments = {segment.name.upper(): segment for segment in module.segments[1:]}
    lines = listing_path.read_text(encoding="latin1").splitlines()
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
    text = re.sub(r", \d+ ms,", ", elapsed omitted,", "\n".join(lines) + "\n")
    listing_path.write_text(text, encoding="latin1")


def modules(program: str | None = None) -> list[str]:
    return list(PROGRAMS[program]) if program else [m for values in PROGRAMS.values() for m in values]


def verify_sources(source: Path = SOURCE) -> dict[str, str]:
    """Refuse edited sources and releases, even if a previous build succeeded."""
    expected = {}
    for line in (SOURCE / "SOURCES.sha256").read_text().splitlines():
        digest, name = line.split()
        if not re.fullmatch(r"(?:source/[A-Za-z0-9_.]+|bin/[A-Z0-9.]+|LICENSE|README.md)", name):
            raise ValueError(f"invalid pinned MS-DOS source name {name!r}")
        if name in expected:
            raise ValueError(f"duplicate MS-DOS source manifest member {name}")
        expected[name] = digest
        path = source / name
        if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"MS-DOS member differs from the unedited upstream: {name}")
    if len(expected) != 57:
        raise ValueError("the MS-DOS manifest must pin all 57 upstream members")
    return expected


def verify_staged_sources(stage: Path, expected: dict[str, str], program: str) -> None:
    """Check both filename spellings and the explicit versioned include aliases."""
    for name, digest in expected.items():
        if not name.startswith("source/"):
            continue
        if name in ("source/DOSSYM.ASM", "source/DOSMAC.ASM") and program == "COMMAND.COM":
            digest = expected[name.replace(".ASM", "_v211.ASM")]
        original = Path(name)
        names = {original.name}
        if original.suffix.upper() == ".ASM":
            names.add(original.name.lower())
        for alias in names:
            path = stage / alias
            if not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise ValueError(f"assembler input was edited: {name} ({alias})")


def compare(program: str, built: bytes, shipped: bytes) -> dict:
    first = next((i for i, (a, b) in enumerate(zip(built, shipped)) if a != b), None)
    if first is None and len(built) != len(shipped):
        first = min(len(built), len(shipped))
    return {"program": program, "identical": built == shipped, "first_difference": first,
            "built_size": len(built), "shipped_size": len(shipped),
            "built_sha256": hashlib.sha256(built).hexdigest(),
            "shipped_sha256": hashlib.sha256(shipped).hexdigest(),
            "built_byte": built[first] if first is not None and first < len(built) else None,
            "shipped_byte": shipped[first] if first is not None and first < len(shipped) else None}


def output_names(programs=None) -> list[str]:
    selected = programs or PROGRAMS
    return [name for program in selected for name in (program, Path(program).stem + ".MAP")] + [
        f"{module}.{extension}" for program in selected for module in modules(program) for extension in ("obj", "lst")
    ] + [Path(program).stem + ".MZ" for program in ("COMMAND.COM", "SORT.EXE")
         if program in selected] + ["comparison.json"]


def exe2com(raw: bytes) -> bytes:
    """DOS EXE2BIN's COM case: no fixups, CS=0/IP=100h, strip PSP padding.

JWlink's direct COM writer mishandles COMMAND's later group after a BSS hole.
The MZ writer keeps the authoritative map layout; no instruction is patched.
"""
    if len(raw) < 28 or raw[:2] != b"MZ":
        raise ValueError("COMMAND intermediate must be MZ")
    last, pages, relocations, paragraphs = struct.unpack_from("<4H", raw, 2)
    ip, cs = struct.unpack_from("<2H", raw, 20)
    file_size = (pages - 1) * 512 + (last or 512)
    header_size = paragraphs * 16
    if (relocations or (cs, ip) != (0, 0x100) or file_size != len(raw)
            or header_size < 28 or header_size > len(raw)):
        raise ValueError("COMMAND EXE2BIN needs an exact no-relocation CS:IP=0000:0100 image")
    body = raw[header_size:]
    if len(body) < 0x100 or any(body[:0x100]):
        raise ValueError("COMMAND EXE2BIN PSP reservation must contain only zeros")
    return body[0x100:]


def sort_exemod(raw: bytes) -> bytes:
    """Set SORT's explicit DOS allocation policy, changing only e_maxalloc.

    JWlink hardcodes FFFFh for DOS; SORT never shrinks that allocation before
    requesting its separate sorting buffer. Like historical EXEMOD, bound the
    loader metadata to one extra paragraph. Zero means load-high on DOS 2 and
    would still consume all memory. Keep raw SORT.MZ for byte-by-byte auditing.
    """
    if len(raw) < 28 or raw[:2] != b"MZ":
        raise ValueError("SORT intermediate must be MZ")
    last, pages, relocations, paragraphs, minimum = struct.unpack_from("<5H", raw, 2)
    maximum = 1
    header_size = paragraphs * 16
    relocation_offset = struct.unpack_from("<H", raw, 24)[0]
    file_size = (pages - 1) * 512 + (last or 512)
    if (not pages or last > 511 or file_size != len(raw)
            or header_size < 28 or header_size > len(raw)
            or (relocations and (relocation_offset < 28
                                 or relocation_offset + 4 * relocations > header_size))):
        raise ValueError("SORT EXEMOD needs an exact, valid MZ image")
    if minimum > maximum:
        raise ValueError("SORT maximum extra allocation is below the linked minimum")
    # Only this two-byte loader field is finalized. Code, initialized data,
    # relocations, entry/stack addresses and all other header bytes stay exact.
    return raw[:12] + struct.pack("<H", maximum) + raw[14:]


def build(output: Path, source: Path = SOURCE, programs=None) -> None:
    output, source = output.resolve(), source.resolve()
    if any(output == protected or protected in output.parents or output in protected.parents
           for protected in (SOURCE, source)):
        raise ValueError("MS-DOS output must be separate from the vendored sources")
    expected = verify_sources(source)
    assembler = ensure_jwasm()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".msdos2-", dir=output.parent) as temporary:
        stage = Path(temporary)
        for name in expected:
            if name.startswith("source/"):
                original = source / name
                shutil.copyfile(original, stage / original.name)
                if original.suffix.upper() == ".ASM":
                    shutil.copyfile(original, stage / original.name.lower())
        shutil.copyfile(ROOT / "tools/msdos-options.inc", stage / "assembler-options.inc")
        reports = []
        for program in programs or PROGRAMS:
            program_modules = PROGRAMS[program]
            for include in ("DOSSYM", "DOSMAC"):
                suffix = "_v211" if program == "COMMAND.COM" else ""
                original = source / "source" / (include + suffix + ".ASM")
                for alias in (include + ".ASM", include.lower() + ".asm"):
                    shutil.copyfile(original, stage / alias)
            verify_staged_sources(stage, expected, program)
            objects = []
            for module in program_modules:
                subprocess.run([str(assembler), "-q", "-Zm", "-Cu", "-Sg", "-Sa", "-Sl",
                                "-Fiassembler-options.inc", f"-Fl={module}.lst", f"-Fo={module}.obj",
                                f"-Fw={module}.err", module + ".asm"], cwd=stage, check=True)
                repair_data_listing(stage / (module + ".lst"), stage / (module + ".obj"))
                objects.extend(("file", module + ".obj"))
            verify_staged_sources(stage, expected, program)
            map_name = Path(program).stem + ".MAP"
            mz_bridge = program == "COMMAND.COM"
            bounded_allocation = program == "SORT.EXE"
            linked_name = Path(program).stem + ".MZ" if mz_bridge or bounded_allocation else program
            target = ["dos", "com"] if program.endswith(".COM") and not mz_bridge else ["dos"]
            subprocess.run([str(ROOT / "tools/jwlink/jwlink"), "format", *target, *objects,
                            "name", linked_name, "option", f"map={map_name},verbose,nofarcalls"],
                           cwd=stage, check=True)
            if mz_bridge:
                (stage / program).write_bytes(exe2com((stage / linked_name).read_bytes()))
            elif bounded_allocation:
                (stage / program).write_bytes(sort_exemod((stage / linked_name).read_bytes()))
            map_file = stage / map_name
            text = re.sub(r"(?m)^Created on:.*$", "Created from pinned MS-DOS 2 source", map_file.read_text())
            text = re.sub(r"(?m)^Link time:.*$", "Link time: omitted for reproducibility", text)
            map_file.write_text(text)
            reports.append(compare(program, (stage / program).read_bytes(), (source / "bin" / program).read_bytes()))
        (stage / "comparison.json").write_text(json.dumps(reports, indent=2) + "\n")
        output.mkdir(parents=True, exist_ok=True)
        for name in output_names(programs):
            (stage / name).replace(output / name)
    for report in reports:
        status = "byte-identical" if report["identical"] else f"first difference 0x{report['first_difference']:x}"
        print(f"{report['program']}: {report['built_size']:,} bytes, {status}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--modules", nargs="?", const="all", choices=["all", *PROGRAMS])
    parser.add_argument("--program", action="append", choices=PROGRAMS)
    arguments = parser.parse_args()
    if arguments.modules:
        print(" ".join(modules(None if arguments.modules == "all" else arguments.modules)))
    elif arguments.output:
        build(arguments.output, programs=arguments.program)
    else:
        parser.error("an output directory or --modules is required")
