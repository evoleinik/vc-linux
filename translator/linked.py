"""Place independently assembled JWasm listings using JWlink's verbose map.

The map's Module Segments table is the authority for each contribution's
address, including modules without public symbols and zero-length segments.
No signature search or guessed module ordering is used.
"""

from dataclasses import dataclass, replace
from pathlib import Path
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_CS

from .image import LoadedImage
from .layout import (Chunk, Instruction, Layout, LayoutError, _decode_static_successors,
                     _evaluate, _is_uninitialized, _normalized_rows, _verify_relocations)
from .listing import Listing, Segment, parse_listing


@dataclass(frozen=True)
class Contribution:
    module: str
    segment: str
    kind: str
    address: int
    size: int


@dataclass
class LinkMap:
    path: Path
    segments: dict[str, tuple[int, int, str, str]]
    contributions: dict[tuple[str, str], Contribution]
    symbols: dict[str, tuple[int, int]]


def parse_map(path: str | Path) -> LinkMap:
    path = Path(path)
    text = path.read_text()
    if "|   Module Segments   |" not in text:
        raise LayoutError(f"{path}: require JWlink option verbose for module segment addresses")
    segments, contributions, symbols = {}, {}, {}
    table = text.split("|   Segments   |", 1)[1].split("|   Memory Map   |", 1)[0]
    for line in table.splitlines():
        m = re.fullmatch(r"(\S+)\s+(\S+)\s+(\S+)\s+([\da-fA-F]{4}):([\da-fA-F]{4})\s+([\da-fA-F]+)", line.strip())
        if m:
            segments[m[1]] = (int(m[4], 16) * 16 + int(m[5], 16), int(m[6], 16), m[2], m[3])
    table = text.split("|   Module Segments   |", 1)[1]
    module = None
    for line in table.splitlines():
        m = re.fullmatch(r"(?:(\S+\.ASM)\s+)?\s*(\S+)\s+(\S+)\s+([\da-fA-F]{4}):([\da-fA-F]{4})\s+([\da-fA-F]+)", line, re.I)
        if not m:
            continue
        if m[1]:
            module = Path(m[1]).stem.upper()
        if module is None:
            raise LayoutError(f"{path}: segment contribution without a module")
        key = module, m[2]
        if key in contributions:
            raise LayoutError(f"{path}: duplicate segment contribution {key}")
        contributions[key] = Contribution(module, m[2], m[3], int(m[4], 16) * 16 + int(m[5], 16), int(m[6], 16))
    table = text.split("|   Memory Map   |", 1)[1].split("|   Module Segments   |", 1)[0]
    for line in table.splitlines():
        m = re.fullmatch(r"([\da-fA-F]{4}):([\da-fA-F]{4})[*+]?\s+(\S+)", line.strip())
        if m:
            value = (int(m[1], 16), int(m[2], 16))
            name = m[3].strip("`").upper()
            if name in symbols and symbols[name] != value:
                raise LayoutError(f"{path}: conflicting public symbol {name}")
            symbols[name] = value
    if not contributions or not segments:
        raise LayoutError(f"{path}: missing segment addresses")
    return LinkMap(path, segments, contributions, symbols)


def _values(listing: Listing, link: LinkMap):
    values = {name.strip("`").upper(): value for name, value in listing.constants.items()}
    values.update({name: offset for name, (_, offset) in link.symbols.items()})
    segvalues = {name: segment for name, (segment, _) in link.symbols.items()}
    for name, declared_segment in listing.external_segments.items():
        if name in link.symbols:
            segment, offset = link.symbols[name]
            values[name] = segment * 16 + offset - link.segments[declared_segment][0]
    for name, (address, _, _, group) in link.segments.items():
        values[name] = segvalues[name] = address // 16
        if group != "AUTO":
            values[group] = segvalues[group] = min(segvalues.get(group, address // 16), address // 16)
    for name, label in listing.labels.items():
        piece = link.contributions[(listing.path.stem.upper(), label.segment)]
        base = link.segments[label.segment][0]
        values[name.strip("`").upper()] = piece.address + label.offset - base
        segvalues[name.strip("`").upper()] = base // 16
    return values, segvalues


def _expression(expression: str, values, segvalues, location: int) -> int:
    expr = expression.replace("`", "")
    expr = re.sub(r"\b(?:CSEG|DSEG|CS|DS):", "", expr, flags=re.I)
    expr = re.sub(r"\b([0-9]+)[dD]\b", r"\1", expr)
    expr = re.sub(r"\b([0-7]+)[oOqQ]\b", lambda m: str(int(m[1], 8)), expr)
    expr = re.sub(r"(?<![\w$])\$(?![\w$])", str(location), expr)
    # LOW/HIGH bind to the remaining scalar initializer in this dialect.
    transform = None
    match = re.match(r"\s*(LOW|HIGH)\s+(.+)", expr, re.I)
    if match:
        transform, expr = match[1].upper(), match[2]
    expr = re.sub(r"'([^']{1,2})'", lambda m: str(int.from_bytes(m[1].encode("ascii"), "little")), expr)
    # The single-listing expression reader predates MASM's '$'-prefixed names.
    renamed = dict(values)
    renamed_seg = dict(segvalues)
    for index, symbol in enumerate(sorted(set(re.findall(r"[$?@A-Za-z_][\w$?@]*", expr)), key=len, reverse=True)):
        if symbol.upper() in values and ("$" in symbol or "?" in symbol or "@" in symbol):
            alias = f"LINKSYMBOL{index}"
            renamed[alias] = values[symbol.upper()]
            if symbol.upper() in segvalues:
                renamed_seg[alias] = segvalues[symbol.upper()]
            expr = expr.replace(symbol, alias)
    value = _evaluate(expr, renamed, renamed_seg)
    return value & 255 if transform == "LOW" else (value >> 8) & 255 if transform == "HIGH" else value


def _verify_module(image: LoadedImage, listing: Listing, link: LinkMap):
    values, segvalues = _values(listing, link)
    # Plain assembler constants are already fully represented by the byte
    # column and can change within a macro expansion (NUM=NUM-1). Only
    # relocatable symbols need link-time reevaluation.
    symbols = set(link.symbols) | {name.strip("`").upper() for name in listing.labels} | set(link.segments)
    owners = {}
    for row in listing.lines:
        if row.bytes and row.segment is not None:
            for index in range(len(row.bytes)):
                owners[(row.segment, row.offset + index)] = row
    for row in listing.lines:
        if not row.bytes or row.segment is None:
            continue
        piece = link.contributions[(listing.path.stem.upper(), row.segment)]
        off = piece.address + row.offset
        expected = list(row.bytes)
        for initial in row.initializers:
            expression = initial.expression.replace("`", "")
            if _is_uninitialized(expression):
                end = len(expected) if "DUP" in expression.upper() else initial.offset + initial.width
                expected[initial.offset:end] = [None] * max(0, min(end, len(expected)) - initial.offset)
                continue
            names = re.findall(r"[$?@A-Za-z_][\w$?@]*", expression)
            if not any(name.upper() in symbols for name in names):
                continue
            try:
                value = _expression(expression, values, segvalues,
                                    off - link.segments[row.segment][0] + initial.offset)
            except (KeyError, SyntaxError, TypeError, ValueError) as exc:
                raise LayoutError(f"{listing.path}:{row.lineno}: cannot resolve linked data {expression!r}: {exc}") from exc
            for i in range(initial.width):
                if initial.offset + i < len(expected):
                    expected[initial.offset + i] = (value >> (8 * i)) & 255
        if any(b is not None for b in expected) and off + len(expected) > len(image.data):
            raise LayoutError(f"{listing.path}:{row.lineno}: initialized bytes outside load module")
        for index, byte in enumerate(expected):
            # MASM ORG can deliberately overwrite an earlier DB, e.g. the
            # last DERMAK skip byte is replaced by JMP SHORT ERROR.
            if owners[(row.segment, row.offset + index)] is not row:
                continue
            if byte is not None and image.data[off + index] != byte:
                raise LayoutError(f"{listing.path}:{row.lineno}: linked byte mismatch at 0x{off + index:x}: "
                                  f"listing {byte:02x}, image {image.data[off + index]:02x}; {row.source.strip()}")
    for segment in listing.segments.values():
        if not segment.is_code or not segment.length:
            continue
        covered = bytearray(segment.length)
        cursor = 0
        for row in listing.lines:
            if row.segment != segment.name:
                continue
            if row.mnemonic == "ORG":
                try:
                    target = _expression(row.operands, values, segvalues, cursor)
                except (KeyError, SyntaxError, TypeError, ValueError) as exc:
                    raise LayoutError(f"{listing.path}:{row.lineno}: cannot resolve ORG") from exc
                if target > cursor:
                    covered[cursor:min(target, len(covered))] = b"\1" * (min(target, len(covered)) - cursor)
                cursor = target
            elif row.bytes:
                end = min(row.offset + row.byte_count, len(covered))
                covered[row.offset:end] = b"\1" * (end - row.offset)
                cursor = row.offset + row.byte_count
        missing = covered.find(b"\0")
        if missing >= 0:
            raise LayoutError(f"{listing.path}: uncovered byte in code segment {segment.name} "
                              f"at offset 0x{missing:x}; missing source/generated listing row")


def build_linked_layout(image: LoadedImage, listings, map_path: str | Path) -> Layout:
    if not image.is_exe:
        raise LayoutError("a link map requires an MZ executable")
    link = parse_map(map_path)
    parsed = [parse_listing(path, linked=True) for path in listings]
    modules = [listing.path.stem.upper() for listing in parsed]
    expected_modules = {module for module, _ in link.contributions}
    if len(set(modules)) != len(modules) or set(modules) != expected_modules:
        raise LayoutError(f"{link.path}: listings must cover each module exactly once; "
                          f"missing {sorted(expected_modules - set(modules))}, extra {sorted(set(modules) - expected_modules)}")
    segments = {name: Segment(name, kind in ("CODE", "CODESG"), size,
                              None if group == "AUTO" else group, size)
                for name, (address, size, kind, group) in link.segments.items()}
    bases = {name: address for name, (address, _, _, _) in link.segments.items()}
    merged = Listing(link.path, [], segments, {}, {})
    normalized = []
    for listing in parsed:
        module = listing.path.stem.upper()
        for name, segment in listing.segments.items():
            piece = link.contributions.get((module, name))
            if piece is None or piece.size != segment.length:
                raise LayoutError(f"{listing.path}: segment {name} length disagrees with map")
        _verify_module(image, listing, link)
        def placed(row):
            if row.segment is None or row.offset is None:
                return row
            piece = link.contributions[(module, row.segment)]
            return replace(row, offset=piece.address - bases[row.segment] + row.offset,
                           procedure=f"{module}::{row.procedure}" if row.procedure else None)
        merged.lines.extend(placed(row) for row in listing.lines)
        normalized.extend(placed(row) for row in _normalized_rows(listing))
        for name, label in listing.labels.items():
            piece = link.contributions[(module, label.segment)]
            merged.labels[f"{module}::{name}"] = replace(label, offset=piece.address - bases[label.segment] + label.offset)
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instructions = []
    for row in normalized:
        off = bases[row.segment] + row.offset
        raw = image.data[off:off + row.byte_count]
        insn = next(decoder.disasm(raw, off, count=1), None)
        if insn is None or insn.size != row.byte_count:
            raise LayoutError(f"{link.path}: instruction length mismatch at image 0x{off:x}: "
                              f"listing {row.byte_count}, capstone {insn.size if insn else None}; {row.source.strip()}")
        record = Instruction(off, insn, row)
        _verify_relocations(image, record)
        instructions.append(record)
    instructions.sort(key=lambda record: record.off)
    for first, second in zip(instructions, instructions[1:]):
        if first.off + first.insn.size > second.off:
            raise LayoutError(f"{link.path}: overlapping listed instructions at 0x{first.off:x} and 0x{second.off:x}")
    # SYNCHR is a real machine-code subroutine that POPs its return address
    # into SI, compares CS:[SI] with ES:[DI] using CMPSB, then PUSHes the
    # incremented SI. Its caller's one-byte token is data, not executable.
    # Recognize the complete fixed prefix, never merely its symbol's name.
    inline_prefix = bytes.fromhex("5e8bfbfc2ea6568bdf")
    return_skips = {segment * 16 + offset: 1 for segment, offset in link.symbols.values()
                    if image.data[segment * 16 + offset:segment * 16 + offset + len(inline_prefix)] == inline_prefix}
    for record in instructions:
        if record.insn.mnemonic == "call" and record.insn.operands[0].type == X86_OP_IMM:
            frame = bases[record.line.segment]
            record.return_skip = return_skips.get(frame + ((record.insn.operands[0].imm - frame) & 0xffff), 0)
    _decode_static_successors(image, merged, bases, decoder, instructions,
                              allow_data_overlaps=True, return_skips=return_skips)
    # Direct CS-relative stores in the sources reveal patched operand fields.
    # Keep the opcode and instruction boundaries static; read only those
    # immediate bytes live. Patches that change the instruction itself fail.
    mutable = set()
    for writer in instructions:
        for operand in writer.insn.operands:
            if (operand.type != X86_OP_MEM or not operand.access & 2
                    or operand.mem.segment != X86_REG_CS or operand.mem.base or operand.mem.index):
                continue
            start = bases[writer.line.segment] + (operand.mem.disp & 0xffff)
            for target in instructions:
                changed = set(range(start, start + operand.size)) & set(range(target.off, target.off + target.insn.size))
                if not changed:
                    continue
                if target.insn.mnemonic in ("ljmp", "lcall") and target.insn.operands[0].type == X86_OP_IMM:
                    allowed = set(range(target.off + target.insn.size - 4, target.off + target.insn.size))
                else:
                    allowed = set(range(target.off + target.insn.imm_offset,
                                        target.off + target.insn.imm_offset + target.insn.imm_size))
                if not changed <= allowed:
                    raise LayoutError(f"{link.path}: code store at 0x{writer.off:x} changes opcode/boundary at 0x{target.off:x}")
                mutable.update(changed)
    for record in instructions:
        record.linked = True
        record.mutable_offsets = tuple(off - record.off for off in sorted(mutable)
                                       if record.off <= off < record.off + record.insn.size)
    chunks = []
    for record in instructions:
        if not chunks or len(chunks[-1].instructions) >= 256:
            chunks.append(Chunk(f"linked_{record.off:x}", index=len(chunks)))
        record.chunk = chunks[-1]
        record.chunk.instructions.append(record)
    labels = {name: bases[label.segment] + label.offset for name, label in merged.labels.items()}
    layout = Layout(image, merged, instructions, chunks, bases, labels, {})
    layout.linked = True
    layout.mutable_offsets = tuple(sorted(mutable))
    return layout
