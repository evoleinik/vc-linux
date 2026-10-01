"""Map-directed AOT front end for OpenWatcom 16-bit C and its OMF libraries.

The verbose map chooses every input contribution, including library members.
OMF bytes and fully resolved FIXUPP records independently verify the linked
image. WDIS's OMF-aware instruction/data listings provide boundaries; they are
not reassembled (JWasm chooses different direction-bit opcode encodings).
No instruction decoding is performed by the generated program.
"""

from dataclasses import dataclass, field
from pathlib import Path
from collections import deque
import hashlib
import os
import re
import subprocess

from capstone import Cs, CS_ARCH_X86, CS_MODE_16, CS_GRP_JUMP, CS_GRP_CALL
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_CS

from .image import LoadedImage
from .emit import EmissionError, UnsupportedInstruction, _InstructionEmitter
from .layout import (Chunk, Instruction, Layout, LayoutError, _static_successors,
                     _verify_relocations)
from .listing import Listing, ListingLine, Segment
from .omf import OmfModule, Reference, library_members, parse_module


@dataclass(frozen=True)
class Piece:
    module: str
    name: str
    kind: str
    frame: int
    address: int
    size: int


@dataclass
class CompiledMap:
    path: Path
    modules: dict[str, tuple[Path, str | None]]
    pieces: dict[tuple[str, str], Piece]
    segments: dict[str, Piece]
    groups: dict[str, int]
    symbols: dict[str, tuple[int, int]]
    entry: tuple[int, int]


@dataclass
class CompiledLayout(Layout):
    compiled_fixup_targets: tuple[tuple[int, int, int], ...] = ()
    compiled_modules: tuple[str, ...] = ()
    compiled_frames: dict[str, int] = field(default_factory=dict)
    compiled_indirect_targets: tuple[int, ...] = ()
    compiled_interrupt_targets: tuple[int, ...] = ()


def parse_compiled_map(path: str | Path) -> CompiledMap:
    path = Path(path)
    text = path.read_text()
    for table in ("Groups", "Segments", "Memory Map", "Module Segments"):
        if f"|   {table}   |" not in text:
            raise LayoutError(f"{path}: require a verbose Watcom DOS map ({table})")
    if "creating a DOS executable" not in text:
        raise LayoutError(f"{path}: require a DOS executable, not a protected-mode map")
    groups, segments, pieces, symbols, modules = {}, {}, {}, {}, {}
    table = text.split("|   Groups   |", 1)[1].split("|   Segments   |", 1)[0]
    for line in table.splitlines():
        match = re.fullmatch(r"(\S+)\s+([\da-f]{4}):([\da-f]{4})\s+[\da-f]+", line.strip(), re.I)
        if match:
            groups[match[1]] = int(match[2], 16) * 16 + int(match[3], 16)
    table = text.split("|   Segments   |", 1)[1].split("|   Memory Map   |", 1)[0]
    for line in table.splitlines():
        match = re.fullmatch(r"(\S+)\s+(\S+)\s+(\S+)\s+([\da-f]{4}):([\da-f]{4})\s+([\da-f]+)", line.strip(), re.I)
        if match:
            name, kind, group = match[1], match[2], match[3]
            frame = int(match[4], 16) * 16
            segments[name] = Piece("", name, kind, frame, frame + int(match[5], 16), int(match[6], 16))
    table = text.split("|   Memory Map   |", 1)[1].split("|   Module Segments   |", 1)[0]
    for line in table.splitlines():
        match = re.fullmatch(r"Module: (.+)\((.+)\)", line)
        if match:
            obj, member = Path(match[1]), match[2]
            key = member
            if key in modules:
                raise LayoutError(f"{path}: ambiguous duplicate map module {key}")
            modules[key] = (obj, member if obj.suffix.lower() == ".lib" else None)
        match = re.fullmatch(r"([\da-f]{4}):([\da-f]{4})[*+]?\s+(\S+)", line.strip(), re.I)
        if match:
            value = (int(match[1], 16) * 16, int(match[2], 16))
            if match[3] in symbols and symbols[match[3]] != value:
                # File-local names may repeat. They are resolved through the
                # owning object's PUBDEF, never by this global dictionary.
                continue
            symbols[match[3]] = value
    table = text.split("|   Module Segments   |", 1)[1].split("+--------------------+", 1)[0]
    owner = None
    for line in table.splitlines():
        if line.strip() in modules:
            owner = line.strip()
            continue
        match = re.fullmatch(r"(?:(\S+)\s+)?\s+(\S+)\s+(\S+)\s+([\da-f]{4}):([\da-f]{4})\s+([\da-f]+)", line, re.I)
        if not match:
            continue
        if match[1]:
            owner = match[1]
        if owner not in modules:
            raise LayoutError(f"{path}: contribution without a mapped input module {owner}")
        frame = int(match[4], 16) * 16
        piece = Piece(owner, match[2], match[3], frame, frame + int(match[5], 16), int(match[6], 16))
        key = owner, piece.name
        if key in pieces:
            raise LayoutError(f"{path}: duplicate map contribution {key}")
        pieces[key] = piece
    if not pieces or set(modules) != {owner for owner, _ in pieces}:
        raise LayoutError(f"{path}: every input module must have contribution metadata")
    # DOSSEG synthesizes these two traditional linker symbols without printing
    # them in Memory Map. Their definition is the beginning/end of _BSS.
    if "_BSS" in segments and "DGROUP" in groups:
        bss = segments["_BSS"]
        frame = groups["DGROUP"]
        symbols.setdefault("_edata", (frame, bss.address - frame))
        symbols.setdefault("_end", (frame, bss.address + bss.size - frame))
    entries = re.findall(r"(?m)^Entry point address:\s+([\da-f]{4}):([\da-f]{4})\s*$", text, re.I)
    if len(entries) != 1:
        raise LayoutError(f"{path}: require exactly one entry point in the link map")
    entry = tuple(int(part, 16) for part in entries[0])
    return CompiledMap(path, modules, pieces, segments, groups, symbols, entry)


def load_modules(link: CompiledMap) -> dict[str, OmfModule]:
    libraries, result = {}, {}
    for key, (path, member) in link.modules.items():
        if member is None:
            raw = path.read_bytes()
        else:
            if path not in libraries:
                libraries[path] = library_members(path)
            if member not in libraries[path]:
                raise LayoutError(f"{path}: mapped library member {member!r} is missing")
            raw = libraries[path][member]
        module = parse_module(raw)
        expected = {name for owner, name in link.pieces if owner == key}
        actual = {segment.name for segment in module.segments[1:]}
        if expected != actual:
            raise LayoutError(f"{key}: OMF segments differ from map: {expected ^ actual}")
        for segment in module.segments[1:]:
            piece = link.pieces[key, segment.name]
            if (piece.size, piece.kind) != (segment.size, segment.kind):
                raise LayoutError(f"{key}: OMF segment {segment.name} size/class differs from map")
        result[key] = module
    return result


def _reference(link, key, module, ref: Reference):
    if ref.method == 0:
        segment = module.segments[ref.datum]
        piece = link.pieces[key, segment.name]
        # The map expresses grouped data contributions as normalized segment:
        # offsets. An explicit SEGDEF frame refers to its output segment.
        outer = link.segments[segment.name]
        return piece.address, outer.frame
    if ref.method == 1:
        frame = link.groups[module.groups[ref.datum]]
        return frame, frame
    if ref.method == 2:
        name = module.externals[ref.datum]
        if name in module.publics:
            segment, offset, absolute = module.publics[name]
            if segment:
                piece = link.pieces[key, module.segments[segment].name]
                group = module.public_groups[name]
                frame = (link.groups[module.groups[group]] if group else link.segments[piece.name].frame)
                return piece.address + offset, frame
            return absolute * 16 + offset, absolute * 16
        if name not in link.symbols:
            raise LayoutError(f"{key}: unresolved OMF external {name}")
        frame, offset = link.symbols[name]
        return frame + offset, frame
    if ref.method == 3:
        return ref.datum * 16, ref.datum * 16
    raise LayoutError(f"{key}: unsupported OMF target method {ref.method}")


def verify_linked_bytes(image: LoadedImage, link: CompiledMap, modules):
    """Resolve every fixup, compare every initialized byte, and all MZ relocs."""
    if (image.hdr_cs, image.hdr_ip) != link.entry:
        raise LayoutError(f"MZ entry point differs from link map: "
                          f"{image.hdr_cs:04x}:{image.hdr_ip:04x} != {link.entry[0]:04x}:{link.entry[1]:04x}")
    relocations, targets = set(), []
    occupied = bytearray(len(image.data))
    for key, module in modules.items():
        expected = {index: bytearray(segment.data) for index, segment in enumerate(module.segments) if index}
        for fixup in module.fixups:
            segment = module.segments[fixup.segment]
            piece = link.pieces[key, segment.name]
            at = piece.address + fixup.offset
            target, target_frame = _reference(link, key, module, fixup.target)
            if fixup.frame.method <= 3:
                _, frame = _reference(link, key, module, fixup.frame)
            elif fixup.frame.method == 4:
                frame = link.segments[segment.name].frame
            elif fixup.frame.method == 5:
                frame = target_frame
            else:
                raise LayoutError(f"{key}: unsupported OMF frame method {fixup.frame.method}")
            data = expected[fixup.segment]
            previous = int.from_bytes(data[fixup.offset:fixup.offset + min(2, fixup.width)], "little")
            if fixup.kind == 2:
                if fixup.relative:
                    raise LayoutError("self-relative OMF segment fixup is unsupported")
                value = (frame // 16 + previous) & 65535
                replacement = value.to_bytes(2, "little")
                relocations.add(at)
            else:
                destination = target + fixup.displacement + previous
                value = destination - (at + fixup.width if fixup.relative else frame)
                if fixup.kind == 3:
                    old_segment = int.from_bytes(data[fixup.offset + 2:fixup.offset + 4], "little")
                    replacement = (value & 65535).to_bytes(2, "little") + ((frame // 16 + old_segment) & 65535).to_bytes(2, "little")
                    relocations.add(at + 2)
                elif fixup.kind in (1, 5):
                    replacement = (value & 65535).to_bytes(2, "little")
                elif fixup.kind in (0, 4):
                    replacement = bytes([(value >> (8 if fixup.kind == 4 else 0)) & 255])
                targets.append((at, destination, fixup.kind))
            data[fixup.offset:fixup.offset + fixup.width] = replacement
        for index, segment in enumerate(module.segments[1:], 1):
            piece = link.pieces[key, segment.name]
            for relative, initialized in enumerate(segment.initialized):
                if not initialized:
                    continue
                at = piece.address + relative
                if at >= len(image.data) or occupied[at]:
                    raise LayoutError(f"{key}: initialized OMF byte overlaps or lies outside image at 0x{at:x}")
                occupied[at] = 1
                if image.data[at] != expected[index][relative]:
                    raise LayoutError(f"{key}:{segment.name}+0x{relative:x}: linked byte mismatch at 0x{at:x}: "
                                      f"resolved OMF {expected[index][relative]:02x}, EXE {image.data[at]:02x}")
    if relocations != set(image.relocations):
        raise LayoutError(f"MZ relocation table differs from resolved OMF FIXUPP: "
                          f"missing {sorted(relocations - set(image.relocations))[:8]}, "
                          f"extra {sorted(set(image.relocations) - relocations)[:8]}")
    return targets


def disassemble_modules(link, modules, directory: Path, watcom: str | Path | None):
    directory.mkdir(parents=True, exist_ok=True)
    root = Path(watcom or os.environ.get("WATCOM", Path.home() / "src/vc-linux-wt/tools-cache/openwatcom"))
    wdis = root / "binl64/wdis"
    if not wdis.is_file():
        raise LayoutError(f"WDIS not found at {wdis}; set WATCOM")
    result = {}
    for key, module in modules.items():
        digest = hashlib.sha256(module.raw).hexdigest()[:20]
        stem = re.sub(r"[^A-Za-z0-9_-]", "_", Path(key).name)
        obj, listing = directory / f"{stem}-{digest}.obj", directory / f"{stem}-{digest}.dis"
        if not listing.exists():
            obj.write_bytes(module.raw)
            completed = subprocess.run([str(wdis), "-p", "-e", str(obj)], text=True,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if completed.returncode or "No disassembly errors" not in completed.stdout:
                raise LayoutError(f"{key}: WDIS failed: {completed.stderr or completed.stdout}")
            listing.write_text(completed.stdout)
        result[key] = listing
    return result


def _listing_rows(path, key, module, link):
    rows, sizes, coverage = [], {}, {}
    # An instruction must not disappear merely because a corrupted listing
    # calls its bytes data. OMF's FD/s ranges identify literal CODE data; an
    # entire offset/pointer fixup identifies a switch-table scalar. Opcode
    # bytes themselves are never fixups, so reclassifying a callback's first
    # instruction cannot pass this independent data-provenance check.
    proven_data = {segment.name: bytearray(segment.size) for segment in module.segments[1:]}
    for segment, begin, end in module.data_ranges:
        proven_data[module.segments[segment].name][begin:end] = b"\1" * (end - begin)
    for fixup in module.fixups:
        proven_data[module.segments[fixup.segment].name][fixup.offset:fixup.offset + fixup.width] = b"\1" * fixup.width
    name = None
    for lineno, text in enumerate(path.read_text().splitlines(), 1):
        match = re.fullmatch(r"Segment: (\S+) \S+ USE16 ([\da-fA-F]+) bytes", text)
        if match:
            name = match[1]
            sizes[name] = int(match[2], 16)
            coverage[name] = bytearray(sizes[name])
            continue
        match = re.match(r"^([\da-fA-F]{4})  ", text)
        if not match or name is None:
            continue
        relative = int(match[1], 16)
        instruction = 6 <= text.find("\t", 6) < 54
        byte_field, tail = text[6:].split("\t", 1) if instruction else (text[6:54], text[54:])
        raw = bytes.fromhex(byte_field)
        segment = next(segment for segment in module.segments[1:] if segment.name == name)
        if relative + len(raw) > sizes[name] or bytes(segment.data[relative:relative + len(raw)]) != raw:
            raise LayoutError(f"{path}:{lineno}: WDIS bytes differ from OMF")
        if instruction and not all(segment.initialized[relative:relative + len(raw)]):
            raise LayoutError(f"{path}:{lineno}: WDIS instruction contains uninitialized OMF bytes")
        if (not instruction and segment.kind == "CODE" and
                not all(proven_data[name][relative:relative + len(raw)])):
            raise LayoutError(f"{path}:{lineno}: CODE data lacks OMF scan/fixup evidence")
        coverage[name][relative:relative + len(raw)] = b"\1" * len(raw)
        # WDIS's instructions have a TAB separating the byte and asm fields;
        # data has a padded ASCII rendering instead, even inside CODE segments.
        source = tail.strip()
        if instruction:
            parts = source.split(None, 1)
            mnemonic, operands = parts[0], parts[1] if len(parts) > 1 else ""
        else:
            mnemonic, operands = "DB", ""
        piece = link.pieces[key, name]
        if piece.kind == "CODE":
            rows.append(ListingLine(lineno, name, piece.address - link.segments[name].address + relative,
                                    tuple(raw), len(raw), source, key, instruction,
                                    mnemonic.upper(), operands))
    if sizes != {segment.name: segment.size for segment in module.segments[1:]}:
        raise LayoutError(f"{path}: WDIS segments differ from OMF/map")
    for segment in module.segments[1:]:
        if segment.kind == "CODE":
            for offset, initialized in enumerate(segment.initialized):
                if initialized and not coverage[segment.name][offset]:
                    raise LayoutError(f"{path}: uncovered initialized CODE byte at {segment.name}+0x{offset:x}")
    return rows


def validate_compiled_targets(layout: CompiledLayout) -> None:
    """Fail at build/test time for missing callbacks, switches or return targets.

    Compiler tables use OMF offset/pointer fixups, including local symbols that
    the map does not publish. The CRT's DoINTR table is the one computed-return
    exception: its complete 256-slot shape is proved here, not inferred by play.
    """
    by_offset = {record.off: record for record in layout.instructions}
    declared_data = {layout.segment_bases[row.segment] + row.offset + index
                     for row in layout.listing.lines if not row.is_instruction
                     for index in range(row.byte_count)}
    instruction_bytes = {record.off + index for record in layout.instructions
                         for index in range(record.insn.size)}
    for record in layout.instructions:
        for operand in record.insn.operands:
            if (operand.type != X86_OP_MEM or not operand.access & 2
                    or operand.mem.segment != X86_REG_CS):
                continue
            start = layout.compiled_frames[record.line.segment] + (operand.mem.disp & 65535)
            if (operand.mem.base or operand.mem.index or
                    any(start + index in instruction_bytes for index in range(operand.size))):
                raise LayoutError(f"compiled CS store at 0x{record.off:x} requires an explicit code-patch proof")
    code = [(layout.segment_bases[name], layout.segment_bases[name] + segment.length)
            for name, segment in layout.listing.segments.items() if segment.is_code]
    expected = set(layout.procedures.values())
    for source, target, kind in layout.compiled_fixup_targets:
        if not any(begin <= target < end for begin, end in code):
            continue
        if target in declared_data:
            if source not in instruction_bytes:
                raise LayoutError(f"compiled code-pointer initializer at 0x{source:x} targets data at 0x{target:x}")
            # A code instruction may load the address of a switch's table or
            # of a CS-resident constant. Neither is a callable entry.
            continue
        expected.add(target)
    interrupt_targets = []
    for name, base in layout.procedures.items():
        if not name.endswith("::_DoINTR_"):
            continue
        data = layout.image.data
        # MOV CX,AX; SHL AX,1; ADD AX,CX; ADD AX,offset inttable;
        # PUSH CS; PUSH AX; ...; RETF. Offsets are guarded by exact bytes.
        if (data[base + 0x37:base + 0x3e] != bytes.fromhex("8bc8d1e003c105")
                or data[base + 0x40:base + 0x42] != b"\x0e\x50"
                or data[base + 0x60] != 0xcb):
            raise LayoutError("unrecognized Watcom DoINTR computed-return dispatcher")
        table = base + 0x61
        record = by_offset.get(base + 0x3d)
        if record is None:
            raise LayoutError("DoINTR table-address instruction is missing")
        frame = layout.compiled_frames[record.line.segment]
        if int.from_bytes(data[base + 0x3e:base + 0x40], "little") != table - frame:
            raise LayoutError("DoINTR computed table address is incorrect")
        for vector in range(256):
            off = table + 3 * vector
            raw = data[off:off + 3]
            if vector == 3:
                valid = raw == b"\xcc\x90\xc3"
            elif vector in (0x25, 0x26):
                target = off + 3 + int.from_bytes(raw[1:], "little", signed=True)
                valid = raw[0] == 0xe9 and data[target:target + 2] == bytes([0xcd, vector])
            else:
                valid = raw == bytes([0xcd, vector, 0xc3])
            if not valid:
                raise LayoutError(f"DoINTR table slot {vector:02x} is not proved executable code")
            interrupt_targets.append(off)
            expected.add(off)
    entry = layout.image.hdr_cs * 16 + layout.image.hdr_ip
    expected.add(entry)
    for target in sorted(expected):
        if target not in by_offset:
            raise LayoutError(f"compiled indirect/procedure target 0x{target:x} lacks a translated instruction")
    pending, reached = deque(sorted(expected)), set()
    relocations = set(layout.image.relocations)
    while pending:
        off = pending.popleft()
        if off in reached:
            continue
        reached.add(off)
        if off not in by_offset:
            raise LayoutError(f"compiled reachable successor 0x{off:x} lacks a translated instruction")
        record = by_offset[off]
        raw = bytes(record.insn.bytes)
        # DOS termination has no return site. In Watcom's startup, the byte
        # immediately after INT 21h/AH=4Ch is its literal copyright string.
        exit_interrupt = (raw == b"\xcd\x20" or (raw == b"\xcd\x21" and
                          layout.image.data[max(0, off - 2):off] == b"\xb4\x4c"))
        if not exit_interrupt:
            pending.extend(_static_successors(record, relocations,
                           layout.compiled_frames[record.line.segment]))
    layout.compiled_indirect_targets = tuple(sorted(expected))
    layout.compiled_interrupt_targets = tuple(interrupt_targets)


def build_compiled_layout(image: LoadedImage, map_path: str | Path, *,
                          watcom: str | Path | None = None,
                          listings: dict[str, Path] | None = None) -> Layout:
    if not image.is_exe:
        raise LayoutError("compiled OMF input requires an MZ EXE")
    link = parse_compiled_map(map_path)
    modules = load_modules(link)
    fixup_targets = verify_linked_bytes(image, link, modules)
    if listings is None:
        listings = disassemble_modules(link, modules, image.path.parent / "disasm", watcom)
    if set(listings) != set(modules):
        raise LayoutError("compiled listings must cover every linked module, CRT members included")
    rows = [row for key, module in modules.items()
            for row in _listing_rows(Path(listings[key]), key, module, link)]
    segments = {name: Segment(name, piece.kind == "CODE", piece.size, file_size=piece.size)
                for name, piece in link.segments.items()}
    bases = {name: piece.address for name, piece in link.segments.items()}
    listing = Listing(Path(map_path), rows, segments, {}, {})
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instructions, owners = [], {}
    for row in rows:
        if not row.is_instruction:
            continue
        off = bases[row.segment] + row.offset
        insn = next(decoder.disasm(image.data[off:off + row.byte_count], off, count=1), None)
        if insn is None or insn.size != row.byte_count:
            raise LayoutError(f"{row.procedure}:{row.segment}+0x{row.offset:x}: WDIS/Capstone instruction length mismatch")
        if any(byte in owners for byte in range(off, off + insn.size)):
            raise LayoutError(f"overlapping compiled instruction at 0x{off:x}")
        record = Instruction(off, insn, row, linked=True)
        _verify_relocations(image, record)
        instructions.append(record)
        for byte in range(off, off + insn.size):
            owners[byte] = record
    starts = {record.off for record in instructions}
    declared_data = {bases[row.segment] + row.offset + index for row in rows
                     if not row.is_instruction for index in range(row.byte_count)}
    procedures = {}
    for key, module in modules.items():
        for name, (segment, relative, _) in module.publics.items():
            if segment and module.segments[segment].kind == "CODE":
                piece = link.pieces[key, module.segments[segment].name]
                off = piece.address + relative
                # End-of-segment model sentinels (e.g. _big_code_) can name
                # the first byte of another routine; they do not add code.
                if relative < piece.size and off not in declared_data:
                    procedures[f"{key}::{name}"] = off
    entry = image.hdr_cs * 16 + image.hdr_ip
    for name, off in {"EXE entry": entry, **procedures}.items():
        if off not in starts:
            raise LayoutError(f"{name}: procedure/entry lacks a compiled instruction boundary at 0x{off:x}")
    # Every statically encoded transfer, including a computed-switch table
    # fixup into code, must land on a translated start. Pure code-segment data
    # references remain data, as the OMF-aware disassembler declared them.
    for record in instructions:
        insn = record.insn
        if (insn.group(CS_GRP_CALL) or insn.group(CS_GRP_JUMP)) and insn.operands and insn.operands[0].type == X86_OP_IMM:
            frame = link.segments[record.line.segment].frame
            successors = list(_static_successors(record, set(image.relocations), frame))
            if insn.mnemonic not in ("jmp", "ljmp"):
                successors = successors[1:]
            for target in successors:
                if target not in starts:
                    raise LayoutError(f"static compiled transfer at 0x{record.off:x} misses target 0x{target:x}")
    chunks = []
    instructions.sort(key=lambda record: record.off)
    for record in instructions:
        # Smaller functions bound the C compiler's CFG/SSA work for large C
        # programs. This is deliberately separate from all older front ends.
        if not chunks or len(chunks[-1].instructions) >= 64:
            chunks.append(Chunk(f"compiled_{record.off:x}", index=len(chunks)))
        record.chunk = chunks[-1]
        record.chunk.instructions.append(record)
    layout = CompiledLayout(image, listing, instructions, chunks, bases, {}, procedures, linked=True,
                            compiled_fixup_targets=tuple(fixup_targets), compiled_modules=tuple(modules),
                            compiled_frames={name: piece.frame for name, piece in link.segments.items()})
    validate_compiled_targets(layout)
    # The older ASM front ends retain explicit runtime faults for their known
    # unreachable opcodes. A new compiled input has no such exception: adding
    # an instruction requires extending the emitter and its Unicorn gate.
    for record in instructions:
        try:
            _InstructionEmitter(record, image.relocations).semantics()
        except UnsupportedInstruction as exc:
            raise EmissionError(f"compiled instruction 0x{record.off:x}: {exc}") from exc
    return layout
