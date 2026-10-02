"""Publish lazy, content-addressed original-source maps for the browser build.

The translator's validated layouts own instruction boundaries and image-relative
addresses. JWasm's source column owns provenance: ordinary rows advance through
the matching source/include, macro expansions retain their invocation, and -Sg
prologues/epilogues retain the PROC/RET which generated them. Never disassemble an
arbitrary range or invent a source line for a missing listing row.

Usage: .venv/bin/python tools/source_maps.py --build build --out build/web
       --work build/web-work
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from hashlib import sha256
import json
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools.build_gwbasic import modules as basic_modules
from tools.build_vz import modules as vz_modules, preprocess as vz_preprocess
from tools.web_modules import update
from translator.compiled import build_compiled_layout, parse_compiled_map
from translator.image import load_image
from translator.layout import build_layout
from translator.linked import build_linked_layout, parse_map
from translator.listing import Listing, parse_listing
from translator.nasm import build_nasm_layout, parse_nasm_listing
from translator.supplement import build_gwbasic_graphics_layout
from translator.vc405 import build_vc405_layout


def physical_lines(data: bytes) -> list[bytes]:
    """DOS physical lines, not Python's broader Unicode line separators."""
    return [line.removesuffix(b"\r") for line in data.split(b"\n")]


def decode_source(data: bytes, legacy_encoding: str = "cp866") -> tuple[str, str]:
    # Several files were already converted to UTF-8 in the vendored tree.
    # Keep those glyphs intact; genuinely DOS-encoded files use CP866.
    try:
        return data.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        # VZ is a Japanese DOS program: its few unconverted comments are
        # Windows-31J/Shift-JIS, not Cyrillic. The original file wins over a
        # blanket code-page assumption; publish the decoder in file metadata.
        return data.decode(legacy_encoding), "shift_jis" if legacy_encoding == "cp932" else "ibm866"


@dataclass(frozen=True)
class Origin:
    path: Path
    line: int
    via: tuple[tuple[Path, int], ...] = ()


@dataclass
class Source:
    path: Path
    lines: list[bytes]
    original_lines: list[int]


class Sources:
    """Connect staged compatibility sources to their untouched vendored inputs."""

    def __init__(self, original: Path, staged: Path | None = None, dialect: str = "vc"):
        self.original = original
        self.staged = staged or original
        self.dialect = dialect
        self.paths = {path.name.upper(): path for path in original.iterdir() if path.is_file()}
        self.cache: dict[str, Source] = {}

    def get(self, name: str) -> Source:
        key = name.upper()
        if key in self.cache:
            return self.cache[key]
        if key not in self.paths:
            raise ValueError(f"{self.original}: source include not found: {name}")
        original = self.paths[key]
        data = original.read_bytes()
        if self.dialect == "basic":
            # Prepend a record marker to the ORIGINAL awk program. Every line
            # emitted by it now has its input NR, including inserted checks and
            # rewritten instructions. Removing markers must reproduce the exact
            # staged file; no fuzzy matching across preprocessing edits.
            marker = b"@VC_SOURCE_INPUT@"
            result = subprocess.run(
                ["awk", "-f", "/dev/stdin", "-f", str(self.original / "jwasmify.awk"), str(original)],
                input=b'{ print "@VC_SOURCE_INPUT@" NR }\n', stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, check=True).stdout
            line_numbers, output, current = [], [], 0
            for line in physical_lines(result)[:-1]:
                if line.startswith(marker):
                    current = int(line[len(marker):])
                else:
                    output.append(line)
                    line_numbers.append(current)
            version = (self.original / "UPSTREAM").read_text().split()[1]
            output[0] = f"; [ Munged by jwasmify.awk; UPSTREAM {version} ]".encode()
            reproduced = b"\n".join(output) + b"\n"
            reproduced = re.sub(rb"(?im)^(\s*)\.SALL\b", rb"\1.LALL", reproduced)
            reproduced = re.sub(rb"(?im)^(\s*)\.XLIST\b", rb"\1.LIST", reproduced)
            staged_data = (self.staged / original.name).read_bytes()
            if reproduced != staged_data:
                raise ValueError(f"{original}: preprocessor provenance no longer reproduces staged source")
            source = Source(original, physical_lines(staged_data), line_numbers + [0])
        elif self.dialect == "vz":
            staged_data = (self.staged / key).read_bytes()
            if staged_data != vz_preprocess(original.name, data):
                raise ValueError(f"{original}: staged VZ source differs from its compatibility transform")
            lines = physical_lines(staged_data)
            if len(lines) != len(physical_lines(data.rstrip(b"\x1a"))):
                raise ValueError(f"{original}: VZ preprocessing changed physical line count")
            source = Source(original, lines, list(range(1, len(lines) + 1)))
        elif self.dialect == "vc405":
            # JWasm prints TASM backslash continuations as one logical row.
            # Reproduce only that documented join, retaining the first actual
            # physical line as provenance; the vendor bytes stay untouched.
            physical = physical_lines(data)
            lines, numbers = [], []
            index = 0
            while index < len(physical):
                number, line = index + 1, physical[index]
                while line.rstrip().endswith(b"\\") and b";" not in line:
                    index += 1
                    if index == len(physical):
                        raise ValueError(f"{original}:{number}: unterminated source continuation")
                    line = line.rstrip()[:-1] + b" " + physical[index].lstrip()
                lines.append(line)
                numbers.append(number)
                index += 1
            source = Source(original, lines, numbers)
        else:
            lines = physical_lines(data)
            source = Source(original, lines, list(range(1, len(lines) + 1)))
        self.cache[key] = source
        return source


@dataclass
class Frame:
    source: Source
    cursor: int = 0
    via: tuple[tuple[Path, int], ...] = ()


def jwasm_origins(path: Path, sources: Sources, listing: Listing | None = None) -> dict[int, Origin]:
    """Track includes and original physical lines through JWasm's source column.

    False conditional lines may be omitted in VC's established listing. Matching
    is strictly forward in the current source, with include-stack transitions;
    no whole-repository text search and no nearest-line fallback is permitted.
    """
    if listing is None:
        listing = parse_listing(path, linked=sources.dialect != "vc", flat=sources.dialect == "vz")
    statements = {row.lineno: row for row in listing.lines}
    rows = physical_lines(path.read_bytes())
    stack = [Frame(sources.get(rows[1].decode("ascii")))]
    origins: dict[int, Origin] = {}
    last: Origin | None = None
    prologue: Origin | None = None
    pending: Frame | None = None
    text_macros: dict[bytes, bytes] = {}
    forced_options = set()
    if sources.dialect == "vc405":
        forced_options = {line.rstrip() for line in physical_lines(
            (ROOT / "tools/vc405-options.inc").read_bytes())}
    if sources.dialect == "basic":
        text_macros[b"OEMVER"] = (sources.original / "UPSTREAM").read_bytes().split()[1]

    def remember_text(raw: bytes) -> None:
        if not raw.startswith(b" = ") or raw[28:30].strip():
            return
        definition = re.match(rb"\s*(\S+)\s+(?:EQU|TEXTEQU)\s+(.+?)(?:\s*;.*)?$", raw[32:], re.I)
        if definition:
            value = definition[2].strip()
            if (value.startswith((b"'", b'"', b"<")) or
                    not re.fullmatch(rb"[0-9A-F]+", raw[3:28].strip())):
                text_macros[definition[1].upper()] = value[1:-1] if value.startswith(b"<") else value

    # A second-pass listing may already substitute a forward text EQU.
    for row in rows:
        remember_text(row)

    def expanded(line: bytes) -> bytes:
        # JWasm substitutes text EQU/TEXTEQU values in the ordinary source
        # column as well as its byte field. Preserve strings and comments.
        tokens = rb"'(?:[^']|'')*'|\"(?:[^\"]|\"\")*\"|;.*|[A-Za-z_@$?][\w@$?.]*"
        for _ in range(16):
            replacement = re.sub(tokens, lambda match: text_macros.get(match[0].upper(), match[0]), line)
            if replacement == line:
                return line
            line = replacement
        raise ValueError(f"{path}: recursive text macro expansion")

    for lineno, raw in enumerate(rows, 1):
        if raw in (b"Binary Map:", b"Macros:"):
            break
        if lineno <= 2:
            continue
        if raw.startswith((b"Warning ", b"Error ")):
            continue
        if len(raw) < 32:
            # JWasm omits the padded source column for ordinary blank lines.
            if not raw.strip() and stack[-1].cursor < len(stack[-1].source.lines):
                if not stack[-1].source.lines[stack[-1].cursor].strip():
                    stack[-1].cursor += 1
            continue
        source = raw[32:].rstrip()
        generated = raw[28:29] == b"*" or raw[28:30].strip().isdigit()
        if generated:
            origin = prologue if raw[28:29] == b"*" and prologue is not None else last
            if origin is not None:
                origins[lineno] = origin
            continue
        included = raw[30:31] == b"C"
        # -Fi supplies only assembler options before the original VC source.
        # Match those exact tool-owned lines, never assign them a vendor line.
        if included and not stack[0].cursor and source in forced_options:
            continue
        if pending is not None:
            if included and source == pending.source.lines[0].rstrip():
                stack.append(pending)
            pending = None
        if not included:
            stack = stack[:1]
        matched = None
        for depth in range(len(stack) - 1, -1, -1):
            if included and depth == 0:
                continue
            frame = stack[depth]
            for index in range(frame.cursor, len(frame.source.lines)):
                original_text = frame.source.lines[index].rstrip()
                if original_text == source or expanded(original_text) == source:
                    matched = depth, index
                    break
            if matched is not None:
                break
        if matched is None:
            # Blank padding and end-of-module summaries are not source rows.
            if not source or raw.startswith(b" ") and b"lines, " in raw:
                continue
            locations = ", ".join(f"{f.source.path.name}:{f.cursor + 1}" for f in stack)
            raise ValueError(f"{path}:{lineno}: source row has no forward provenance ({locations}): {source!r}")
        depth, index = matched
        stack = stack[:depth + 1]
        frame = stack[-1]
        frame.cursor = index + 1
        remember_text(raw)
        original_line = frame.source.original_lines[index]
        current = Origin(frame.source.path, original_line, frame.via) if original_line else None
        if not raw.lstrip().startswith(b">"):
            last = current
            statement = statements.get(lineno)
            if statement is not None:
                if statement.mnemonic == "PROC" and statement.offset is not None:
                    # JWasm delays USES/LOCAL setup until the first instruction;
                    # intervening declarations, comments and IFs are not its origin.
                    prologue = current
                elif statement.is_instruction or statement.mnemonic in {"ENDP", "RET", "RETF", "RETN"}:
                    # RET source rows may have no bytes: their generated restore
                    # instructions and return belong to RET, even in an empty PROC.
                    prologue = None
        if current is not None:
            origins[lineno] = current
        include = re.match(rb"\s*include\s+([^\s;]+)", source, re.I)
        if include and current is not None:
            name = include[1].decode("ascii").strip("\"'<>")
            pending = Frame(sources.get(name), via=frame.via + ((current.path, current.line),))
    return origins


def content_file(destination: Path, stem: str, suffix: str, data: bytes) -> str:
    name = f"{stem}.{sha256(data).hexdigest()[:12]}.{suffix}"
    update(destination / name, data)
    return name


def json_bytes(value: object) -> bytes:
    return (json.dumps(value, ensure_ascii=False, separators=(",", ":")) + "\n").encode("utf-8")


def call_rows(layout) -> list[list]:
    # Only a CALL present in the listing qualifies, never an arbitrary byte
    # decoding that happens to look like one. The return address is the actual
    # end, not GW-BASIC's occasionally adjusted inline-token continuation.
    calls = [[record.off + record.insn.size, record.off,
              "far" if record.insn.mnemonic == "lcall" or
              record.line.expansion == "JWasm same-segment far call: near CALL" else "near"]
             for record in layout.instructions
             if record.line.mnemonic.upper() in ("CALL", "LCALL")
             and record.insn.mnemonic in ("call", "lcall")]
    calls.sort(key=lambda row: row[0])
    if len({row[0] for row in calls}) != len(calls):
        raise ValueError(f"{layout.image.path}: ambiguous listing CALL return address")
    return calls


def stack_metadata(image) -> dict:
    """The initial MZ stack is bounded only while its load-relative SS is live."""
    if not image.is_exe:
        return {}
    return {"stack": {"segment": image.hdr_ss, "top": image.hdr_sp or 0x10000}}


def assembly_map(name: str, layout, origin_at, destination: Path) -> dict:
    result = {"version": 1, "image": name, "kind": "asm", "files": [],
              "lines": [], "calls": call_rows(layout), **stack_metadata(layout.image)}
    file_ids = {}
    source_data = {}
    seen = set()
    for record in sorted(layout.instructions, key=lambda row: row.off):
        origin = origin_at(record)
        if origin is None or origin.line < 1:
            raise ValueError(f"{name}+{record.off:x}: instruction lacks original-source provenance")
        if record.off in seen:
            raise ValueError(f"{name}+{record.off:x}: duplicate instruction mapping")
        seen.add(record.off)
        # A common include may have distinct roots in a linked program. Keep
        # the include path in the file-table key so breadcrumbs stay truthful.
        key = origin.path, origin.via
        if origin.path not in source_data:
            data = origin.path.read_bytes()
            text, encoding = decode_source(data, "cp932" if name == "VZ.COM" else "cp866")
            url = content_file(destination, "source-" + origin.path.stem.lower(), "txt", text.encode("utf-8"))
            source_data[origin.path] = text, encoding, url, len(physical_lines(data))
        text, encoding, url, count = source_data[origin.path]
        if key not in file_ids:
            item = {"name": origin.path.name, "path": origin.path.relative_to(ROOT).as_posix(),
                    "url": url, "encoding": encoding}
            if origin.via:
                item["via"] = [{"name": path.name, "path": path.relative_to(ROOT).as_posix(), "line": line}
                               for path, line in origin.via]
            file_ids[key] = len(result["files"])
            result["files"].append(item)
        if origin.line > count:
            raise ValueError(f"{origin.path}:{origin.line}: source line outside file")
        result["lines"].append([record.off, file_ids[key], origin.line, record.insn.size])
    return result


def single_vc(build: Path, destination: Path, name: str) -> dict:
    path = build / "gen" / (name + ".lst")
    listing = parse_listing(path)
    origins = jwasm_origins(path, Sources(ROOT / "asm"), listing)
    layout = build_layout(load_image(build / name), listing)
    return assembly_map(name, layout, lambda record: origins.get(record.line.lineno), destination)


def single_vc405(build: Path, destination: Path, name: str) -> dict:
    image_name = "VC405.COM" if name == "VC.COM" else "VCSETUP.COM"
    path = build / "vc405" / (name + ".lst")
    listing = parse_listing(path, physical_lines=True)
    origins = jwasm_origins(path, Sources(ROOT / "asm405", dialect="vc405"), listing)
    layout = build_vc405_layout(load_image(build / "vc405" / name), listing)
    return assembly_map(image_name, layout, lambda record: origins.get(record.line.lineno), destination)


def linked_assembly(build: Path, destination: Path, dialect: str) -> dict:
    basic = dialect == "basic"
    directory = build / ("gwbasic" if basic else "vz")
    name = "GWBASIC.EXE" if basic else "VZ.COM"
    map_path = directory / ("GWBASIC.MAP" if basic else "VZ.MAP")
    original = ROOT / ("third_party/gwbasic" if basic else "third_party/vzeditor/SRC")
    sources = Sources(original, directory, dialect)
    paths = [directory / f"{module}.lst" for module in (basic_modules() if basic else vz_modules())]
    link = parse_map(map_path)
    origin_offset = 0 if basic else 0x100
    provenance = {}
    for path in paths:
        listing = parse_listing(path, linked=True, flat=not basic)
        origins = jwasm_origins(path, sources, listing)
        for row in listing.lines:
            if row.segment is None or row.offset is None or row.lineno not in origins:
                continue
            contribution = link.contributions[path.stem.upper(), row.segment]
            offset = contribution.address - origin_offset + row.offset
            provenance[(offset, row.lineno, row.segment)] = origins[row.lineno]
    layout = build_linked_layout(load_image(directory / name), paths, map_path)
    if basic:
        supplement = build_gwbasic_graphics_layout(layout)
        existing = {record.off for record in layout.instructions}
        layout.instructions.extend(record for record in supplement.instructions if record.off not in existing)

    def origin_at(record):
        # Normalized long jumps and reachable DB instructions retain the row's
        # ORIGINAL offset even when their decoded instruction begins later.
        offset = layout.segment_bases[record.line.segment] + record.line.offset
        return provenance.get((offset, record.line.lineno, record.line.segment))

    return assembly_map(name, layout, origin_at, destination)


def bootlogo(build: Path, destination: Path) -> dict:
    path = build / "bootlogo/LOGO.lst"
    layout = build_nasm_layout(load_image(build / "bootlogo/LOGO.COM"), parse_nasm_listing(path))
    original = ROOT / "third_party/bootlogo/bootlogo.asm"
    origins = {}
    previous = 0
    for lineno, raw in enumerate(physical_lines(path.read_bytes()), 1):
        if raw[:7].strip().isdigit():
            number = int(raw[:7])
            if number:
                previous = number
            origins[lineno] = Origin(original, previous)
    return assembly_map("LOGO.COM", layout, lambda record: origins.get(record.line.lineno), destination)


def compiled_map(build: Path, program: str) -> dict:
    """Publish real Watcom CODE symbols, not inferred C source lines."""
    image_name = program.upper() + ".EXE"
    path = build / program / (program.upper() + ".MAP")
    link = parse_compiled_map(path)
    layout = build_compiled_layout(load_image(build / program / image_name), path)
    starts = {record.off for record in layout.instructions}
    functions = []
    for piece in link.segments.values():
        if piece.kind != "CODE":
            continue
        symbols = sorted((frame + offset, name) for name, (frame, offset) in link.symbols.items()
                         if frame + offset in starts and piece.address <= frame + offset < piece.address + piece.size)
        # Map aliases share one start. Choose a deterministic published name;
        # the extent stops at the next mapped function or this code segment.
        grouped = {}
        for address, name in symbols:
            grouped.setdefault(address, name)
        addresses = sorted(grouped)
        for index, address in enumerate(addresses):
            end = addresses[index + 1] if index + 1 < len(addresses) else piece.address + piece.size
            functions.append([address, end, grouped[address]])
    return {"version": 1, "image": image_name, "kind": "functions",
            "functions": sorted(functions), "calls": call_rows(layout), **stack_metadata(layout.image)}


GENERATED_ASSET = re.compile(r"(?:source-index|source-map-[a-z0-9-]+|source-[a-z0-9_-]+)\.([0-9a-f]{12})\.(?:json|txt)")


def asset_bytes(path: Path) -> bytes:
    match = GENERATED_ASSET.fullmatch(path.name)
    if match is None or path.is_symlink() or not path.is_file():
        raise ValueError(f"Not a regular generated source asset: {path}")
    data = path.read_bytes()
    if sha256(data).hexdigest()[:12] != match[1]:
        raise ValueError(f"Generated source asset hash mismatch: {path}")
    return data


def restore(destination: Path, work: Path) -> str:
    """Restore a deleted publish directory without rebuilding validated maps.

    Keep current and retired immutable bytes in the work cache. Only our exact
    content-verified generated names can be retired from the published site.
    """
    inventory = json.loads((work / "source-assets.json").read_bytes())
    names = inventory["files"]
    if inventory["version"] != 1 or inventory["index"] not in names:
        raise ValueError("Invalid source-asset cache inventory")
    destination.mkdir(parents=True, exist_ok=True)
    cache = work / "source-assets"
    for name in names:
        if Path(name).name != name:
            raise ValueError("Source-asset cache name is not a basename")
        update(destination / name, asset_bytes(cache / name))
    retired = 0
    for path in sorted(destination.iterdir()):
        if path.name in names or not GENERATED_ASSET.fullmatch(path.name):
            continue
        data = asset_bytes(path)
        update(cache / path.name, data)
        path.unlink()  # exact immutable bytes are recoverable from the work cache
        retired += 1
    if retired:
        print(f"source maps: retired {retired} obsolete published assets; recoverable in {cache}", flush=True)
    return inventory["index"]


def publish(build: Path, destination: Path, work: Path) -> str:
    destination.mkdir(parents=True, exist_ok=True)
    index = {"version": 1, "images": {}}
    names = set()
    builders = (("VC.COM", lambda: single_vc(build, destination, "VC.COM")),
                ("VC.OVL", lambda: single_vc(build, destination, "VC.OVL")),
                ("GWBASIC.EXE", lambda: linked_assembly(build, destination, "basic")),
                ("LOGO.COM", lambda: bootlogo(build, destination)),
                ("ROGUE.EXE", lambda: compiled_map(build, "rogue")),
                ("HACK.EXE", lambda: compiled_map(build, "hack")),
                ("VZ.COM", lambda: linked_assembly(build, destination, "vz")),
                ("VC405.COM", lambda: single_vc405(build, destination, "VC.COM")),
                ("VCSETUP.COM", lambda: single_vc405(build, destination, "VCSETUP.COM")))
    for name, builder in builders:
        mapping = builder()
        stem = "source-map-" + name.lower().replace(".", "-")
        url = content_file(destination, stem, "json", json_bytes(mapping))
        index["images"][name] = {"url": url}
        names.add(url)
        names.update(file["url"] for file in mapping.get("files", []))
        count = len(mapping.get("lines", mapping.get("functions", [])))
        print(f"source maps: {name}: {count:,} {mapping['kind']} entries, {len(mapping['calls']):,} calls", flush=True)
    filename = content_file(destination, "source-index", "json", json_bytes(index))
    names.add(filename)
    for name in sorted(names):
        update(work / "source-assets" / name, asset_bytes(destination / name))
    update(work / "source-assets.json", json_bytes({"version": 1, "index": filename, "files": sorted(names)}))
    update(work / "source-index-name.txt", (filename + "\n").encode("ascii"))
    restore(destination, work)
    return filename


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build"))
    parser.add_argument("--out", type=Path, default=Path("build/web"))
    parser.add_argument("--work", type=Path, default=Path("build/web-work"))
    parser.add_argument("--restore", action="store_true", help="restore/verify cached source assets without rebuilding maps")
    args = parser.parse_args()
    filename = restore(args.out, args.work) if args.restore else publish(args.build, args.out, args.work)
    print(f"source maps: published {filename}")


if __name__ == "__main__":
    main()
