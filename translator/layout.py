"""Match listing segments against linked bytes and validate every boundary.

There is deliberately no disassembly sweep. The two exact JWasm source
normalizations and the static-successor closure into declared data are
documented in ``translator/README.md``. Other length discrepancies are errors.
"""

import ast
from bisect import bisect_left
from collections import deque
from dataclasses import dataclass, field, replace
import operator
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_16, CS_GRP_CALL, CS_GRP_JUMP
from capstone.x86_const import X86_OP_IMM

from .image import LoadedImage
from .listing import DataInitializer, Listing, ListingLine, PREFIXES


class LayoutError(ValueError):
    """The listing and linked load module are inconsistent."""


@dataclass
class Chunk:
    name: str
    instructions: list["Instruction"] = field(default_factory=list)
    index: int = 0


@dataclass
class Instruction:
    off: int
    insn: object
    line: ListingLine
    chunk: Chunk | None = None
    return_skip: int = 0
    linked: bool = False
    mutable_offsets: tuple[int, ...] = ()
    variants: tuple["Instruction", ...] = ()


@dataclass
class Layout:
    image: LoadedImage
    listing: Listing
    instructions: list[Instruction]
    chunks: list[Chunk]
    segment_bases: dict[str, int]
    labels: dict[str, int]
    procedures: dict[str, int]
    linked: bool = False
    mutable_offsets: tuple[int, ...] = ()


_JCC = {
    "JO": 0x70, "JNO": 0x71, "JB": 0x72, "JC": 0x72, "JNAE": 0x72,
    "JAE": 0x73, "JNB": 0x73, "JNC": 0x73, "JE": 0x74, "JZ": 0x74,
    "JNE": 0x75, "JNZ": 0x75, "JBE": 0x76, "JNA": 0x76, "JA": 0x77,
    "JNBE": 0x77, "JS": 0x78, "JNS": 0x79, "JP": 0x7A, "JPE": 0x7A,
    "JNP": 0x7B, "JPO": 0x7B, "JL": 0x7C, "JNGE": 0x7C,
    "JGE": 0x7D, "JNL": 0x7D, "JLE": 0x7E, "JNG": 0x7E,
    "JG": 0x7F, "JNLE": 0x7F,
}
_SYMBOL = re.compile(r"(?<![\w])[@?A-Za-z_][\w@?$]*(?:\.[\w@?$]+)*")


def _where(listing: Listing, row: ListingLine) -> str:
    off = f"0x{row.offset:x}" if row.offset is not None else "?"
    return f"{listing.path}:{row.lineno} ({row.segment}:{off}, {row.source.strip()})"


def _relocatable_names(listing: Listing) -> set[str]:
    names = {n.upper() for n in listing.labels}
    names.update(n.upper() for n in listing.procedures)
    names.update(n.upper() for n in listing.segments)
    names.update(seg.group.upper() for seg in listing.segments.values() if seg.group)
    return names


def _has_symbol(expr: str, names: set[str]) -> bool:
    return any(match[0].upper() in names for match in _SYMBOL.finditer(expr))


def _is_uninitialized(expression: str) -> bool:
    return expression == "?" or bool(re.fullmatch(r".+\bDUP\s*\(\s*\?\s*\)", expression, re.I))


def _match_bytes(row: ListingLine, relocatable: set[str]) -> tuple[int | None, ...]:
    values = list(row.bytes)
    for initial in row.initializers:
        if _is_uninitialized(initial.expression):
            # JWasm prints zero placeholders for '?' reservations. COM uses
            # ORG to overlay these reservations with actual executable code.
            end = len(values) if "DUP" in initial.expression.upper() else initial.offset + initial.width
            for i in range(initial.offset, min(end, len(values))):
                values[i] = None
            continue
        if _has_symbol(initial.expression, relocatable):
            for i in range(initial.offset, min(initial.offset + initial.width, len(values))):
                values[i] = None
    return tuple(values)


def _known_runs(row: ListingLine, relocatable: set[str]):
    values = _match_bytes(row, relocatable)
    i = 0
    while i < len(values):
        if values[i] is None:
            i += 1
            continue
        end = i + 1
        while end < len(values) and values[end] is not None:
            end += 1
        yield row.offset + i, bytes(values[i:end])
        i = end


def _segment_bases(image: LoadedImage, listing: Listing, relocatable: set[str]) -> dict[str, int]:
    bases = {}
    for name, segment in listing.segments.items():
        if segment.file_size == 0:
            continue
        rows = [r for r in listing.lines if r.segment == name and r.bytes and r.offset is not None]
        if not rows:
            continue
        runs = sorted((run for row in rows for run in _known_runs(row, relocatable)),
                      key=lambda run: len(run[1]), reverse=True)
        candidates = None
        for relative, pattern in runs:
            if candidates is None:
                candidates = set()
                position = image.data.find(pattern)
                while position != -1:
                    base = position - relative
                    if base >= (-0x100 if not image.is_exe else 0):
                        candidates.add(base)
                    position = image.data.find(pattern, position + 1)
            else:
                candidates = {base for base in candidates
                              if base + relative >= 0 and
                              image.data[base + relative:base + relative + len(pattern)] == pattern}
            if not candidates:
                raise LayoutError(f"{listing.path}: segment {name} has no linked-byte match "
                                  f"for listing offset 0x{relative:x} ({pattern.hex()})")
            if len(candidates) == 1:
                bases[name] = candidates.pop()
                break
        else:
            raise LayoutError(f"{listing.path}: segment {name} has an ambiguous linked-byte base")
    # Uninitialized segments have no bytes to match. Their *memory* addresses
    # are metadata, not instruction-boundary evidence; resolve the listing's
    # group-relative RVA only after that group's real bytes have been matched.
    for name, segment in listing.segments.items():
        if segment.file_size != 0 or segment.map_rva is None:
            continue
        origin = _group_origin(segment.group, bases, listing, image)
        if origin is not None:
            bases[name] = origin + segment.map_rva
    return bases


def _group_origin(group: str | None, bases: dict[str, int], listing: Listing,
                  image: LoadedImage) -> int | None:
    if not image.is_exe:
        return -0x100
    if not group:
        return None
    members = [bases[s.name] for s in listing.segments.values()
               if s.group and s.group.upper() == group.upper() and s.name in bases
               and s.file_size != 0]
    return min(members) & ~15 if members else None


def _symbol_values(image: LoadedImage, listing: Listing, bases: dict[str, int]):
    values = {name.upper(): value for name, value in listing.constants.items()}
    segment_values = {}
    for name, segment in listing.segments.items():
        if name not in bases:
            continue
        origin = _group_origin(segment.group, bases, listing, image)
        frame = origin if origin is not None else bases[name] & ~15
        segment_values[name.upper()] = 0 if not image.is_exe else frame // 16
        if segment.group:
            values[segment.group.upper()] = segment_values[name.upper()]
    values.update(segment_values)
    labels = dict(listing.labels)
    labels.update({name: proc for name, proc in listing.procedures.items()})
    for name, label in labels.items():
        if label.segment not in bases:
            continue
        segment = listing.segments[label.segment]
        origin = _group_origin(segment.group, bases, listing, image)
        frame = origin if origin is not None else bases[label.segment] & ~15
        values[name.upper()] = bases[label.segment] + label.offset - frame
        segment_values[name.upper()] = 0 if not image.is_exe else frame // 16
    return values, segment_values


def _evaluate(expression: str, values: dict[str, int], segment_values: dict[str, int]) -> int:
    """Evaluate a restricted MASM integer expression; never execute its text."""
    expr = re.sub(r"\bSEG\s+([\w@?$]+)",
                  lambda m: str(segment_values[m[1].upper()]), expression, flags=re.I)
    expr = re.sub(r"\b(?:OFFSET|BYTE|WORD|DWORD|FAR|NEAR|PTR)\b", "", expr, flags=re.I)
    expr = re.sub(r"\b([0-9][0-9A-F]*)h\b", lambda m: str(int(m[1], 16)), expr, flags=re.I)
    expr = re.sub(r"\b([01]+)b\b", lambda m: str(int(m[1], 2)), expr, flags=re.I)
    for masm, py in (("SHL", "<<"), ("SHR", ">>"), ("AND", "&"),
                     ("OR", "|"), ("XOR", "^"), ("NOT", "~"), ("MOD", "%")):
        expr = re.sub(rf"\b{masm}\b", py, expr, flags=re.I)
    expr = _SYMBOL.sub(lambda m: str(values[m[0].upper()]), expr)
    operators = {ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul,
                 ast.Div: operator.floordiv, ast.FloorDiv: operator.floordiv,
                 ast.Mod: operator.mod, ast.LShift: operator.lshift,
                 ast.RShift: operator.rshift, ast.BitOr: operator.or_,
                 ast.BitAnd: operator.and_, ast.BitXor: operator.xor}

    def visit(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in operators:
            return operators[type(node.op)](visit(node.left), visit(node.right))
        if isinstance(node, ast.UnaryOp) and isinstance(node.op, (ast.USub, ast.UAdd, ast.Invert)):
            value = visit(node.operand)
            return -value if isinstance(node.op, ast.USub) else ~value if isinstance(node.op, ast.Invert) else value
        raise ValueError(f"unsupported data expression {expression!r}")

    return visit(ast.parse(expr.strip(), mode="eval").body)


def _verify_bytes(image: LoadedImage, listing: Listing, bases: dict[str, int],
                  relocatable: set[str]) -> None:
    values, segment_values = _symbol_values(image, listing, bases)
    for row in listing.lines:
        if not row.bytes or row.segment not in bases:
            continue
        if listing.segments[row.segment].file_size == 0:
            continue
        off = bases[row.segment] + row.offset
        expected = _match_bytes(row, relocatable)
        if off < 0 or off + len(expected) > len(image.data):
            raise LayoutError(f"{_where(listing, row)}: bytes fall outside image at 0x{off:x}")
        for i, byte in enumerate(expected):
            if byte is not None and image.data[off + i] != byte:
                raise LayoutError(f"{_where(listing, row)}: byte mismatch at image 0x{off+i:x}: "
                                  f"listing {byte:02x}, image {image.data[off+i]:02x}")
        for initial in row.initializers:
            if not _has_symbol(initial.expression, relocatable):
                continue
            try:
                value = _evaluate(initial.expression, values, segment_values)
            except (KeyError, SyntaxError, ValueError, TypeError) as exc:
                raise LayoutError(f"{_where(listing, row)}: cannot resolve data fixup "
                                  f"{initial.expression!r}: {exc}") from exc
            field_off = off + initial.offset
            wanted = (value & ((1 << (initial.width * 8)) - 1)).to_bytes(initial.width, "little")
            actual = image.data[field_off:field_off + initial.width]
            if actual != wanted:
                raise LayoutError(f"{_where(listing, row)}: resolved data fixup mismatch at "
                                  f"image 0x{field_off:x}: {initial.expression} expected "
                                  f"{wanted.hex()}, image {actual.hex()}")


def _normalized_rows(listing: Listing):
    pending = None
    for row in listing.lines:
        if not row.is_instruction:
            continue
        if row.mnemonic in PREFIXES and not row.operands:
            if pending is None:
                pending = row
            else:
                if pending.segment != row.segment or pending.offset + pending.byte_count != row.offset:
                    raise LayoutError(f"{_where(listing, row)}: discontiguous prefix lines")
                pending = replace(pending, bytes=pending.bytes + row.bytes,
                                  byte_count=pending.byte_count + row.byte_count)
            continue
        if pending:
            if pending.segment != row.segment or pending.offset + pending.byte_count != row.offset:
                raise LayoutError(f"{_where(listing, row)}: prefix is not adjacent to its instruction")
            row = replace(row, offset=pending.offset, bytes=pending.bytes + row.bytes,
                          byte_count=pending.byte_count + row.byte_count,
                          source=pending.source.strip() + " " + row.source.strip(),
                          expansion="joined standalone prefix")
            pending = None
        data = row.bytes
        if (row.mnemonic in _JCC and len(data) == 5 and
                data[0] == (_JCC[row.mnemonic] ^ 1) and data[1:3] == (3, 0xE9)):
            yield replace(row, bytes=data[:2], byte_count=2,
                          expansion="JWasm long conditional: inverted short branch")
            yield replace(row, offset=row.offset + 2, bytes=data[2:], byte_count=3,
                          expansion="JWasm long conditional: near jump")
        elif row.mnemonic == "CALL" and len(data) == 4 and data[:2] == (0x0E, 0xE8):
            target = row.operands.strip().upper()
            procs = {name.upper(): proc for name, proc in listing.procedures.items()}
            if target not in procs or not procs[target].far or procs[target].segment != row.segment:
                raise LayoutError(f"{_where(listing, row)}: unproven PUSH CS / CALL expansion")
            yield replace(row, bytes=data[:1], byte_count=1,
                          expansion="JWasm same-segment far call: PUSH CS")
            yield replace(row, offset=row.offset + 1, bytes=data[1:], byte_count=3,
                          expansion="JWasm same-segment far call: near CALL")
        else:
            yield row
    if pending:
        raise LayoutError(f"{_where(listing, pending)}: dangling standalone prefix")


def _verify_relocations(image: LoadedImage, record: Instruction) -> None:
    insn = record.insn
    for reloc in image.relocations[bisect_left(image.relocations, record.off - 1):
                                   bisect_left(image.relocations, record.off + insn.size)]:
        relative = reloc - record.off
        raw = bytes(insn.bytes)
        # Capstone's far-pointer imm_size metadata is inconsistent across
        # versions; the architectural segment field is unambiguous here.
        opcode_index = 0
        while opcode_index < len(raw) and raw[opcode_index] in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65, 0x66, 0x67, 0xF0, 0xF2, 0xF3):
            opcode_index += 1
        is_far = opcode_index < len(raw) and raw[opcode_index] in (0x9A, 0xEA)
        far_segment = (is_far
                       and relative == insn.size - 2)
        enter_allocation = (opcode_index < len(raw) and raw[opcode_index] == 0xC8
                            and relative == opcode_index + 1)
        immediate = (not is_far and insn.imm_size and 0 <= relative and
                     insn.imm_offset <= relative and relative + 2 <= insn.size and
                     relative + 2 <= insn.imm_offset + insn.imm_size)
        if not (far_segment or enter_allocation or immediate):
            raise LayoutError(f"{image.path}: relocation 0x{reloc:x} falls outside an immediate "
                              f"or far segment at instruction 0x{record.off:x} "
                              f"({insn.mnemonic} {insn.op_str})")


def _static_successors(record: Instruction, relocations: set[int], frame: int):
    """Image-relative fall-through/return sites and statically known targets."""
    insn = record.insn
    mnemonic = insn.mnemonic.split()[-1]
    if mnemonic not in ("jmp", "ljmp", "ret", "retf", "iret", "iretd"):
        # CALL/INT can return, including INT 20h if its vector was hooked.
        yield frame + ((record.off + insn.size + getattr(record, "return_skip", 0) - frame) & 0xffff)
    if not (insn.group(CS_GRP_JUMP) or insn.group(CS_GRP_CALL) or
            mnemonic in ("loop", "loope", "loopne")):
        return
    if not insn.operands or insn.operands[0].type != X86_OP_IMM:
        return
    raw = bytes(insn.bytes)
    if mnemonic in ("lcall", "ljmp"):
        # Only a relocated far segment is relative to this image's load base.
        # An absolute far pointer is a physical address, not an image offset.
        if record.off + insn.size - 2 in relocations:
            yield int.from_bytes(raw[-2:], "little") * 16 + int.from_bytes(raw[-4:-2], "little")
    elif record.off + insn.imm_offset not in relocations:
        # Disassemble image offsets, but wrap IP in the listing's CS frame.
        # A near jump across half a segment may encode the opposite signed
        # displacement; its unwrapped image address is not the actual target.
        displacement = int.from_bytes(raw[insn.imm_offset:insn.imm_offset + insn.imm_size],
                                      "little", signed=True)
        yield frame + ((record.off + insn.size + displacement - frame) & 0xffff)


def _decode_static_successors(image: LoadedImage, listing: Listing, bases: dict[str, int],
                              decoder, instructions: list[Instruction], *,
                              allow_data_overlaps: bool = False,
                              return_skips: dict[int, int] | None = None) -> None:
    """Close listed code over static successors and the program entry point.

    DB bytes may be executable: VC.COM's RESIDENT banner runs at entry, and
    PutTree's _JCXZ macro emits an instruction as two separate DB rows. Decode
    only reachable starts, keeping both listed and recovered boundaries strict.
    """
    entry = image.hdr_cs * 16 + image.hdr_ip if image.is_exe else 0
    if not 0 <= entry < len(image.data):
        raise LayoutError(f"{listing.path}: entry point 0x{entry:x} is outside the image")
    starts = {record.off for record in instructions}
    owners: list[Instruction | None] = [None] * len(image.data)
    for record in instructions:
        owners[record.off:record.off + record.insn.size] = [record] * record.insn.size
    source_rows: list[ListingLine | None] = [None] * len(image.data)
    for row in listing.lines:
        if row.segment not in bases or not row.bytes or row.offset is None:
            continue
        if row.initializers and all(_is_uninitialized(i.expression) for i in row.initializers):
            continue
        begin = max(0, bases[row.segment] + row.offset)
        end = min(len(image.data), bases[row.segment] + row.offset + row.byte_count)
        if begin < end:
            source_rows[begin:end] = [row] * (end - begin)
    relocations = set(image.relocations)
    frames = {}
    for name, base in bases.items():
        origin = _group_origin(listing.segments[name].group, bases, listing, image)
        frames[name] = origin if origin is not None else base & ~15
    pending = deque([entry])
    for record in instructions:
        pending.extend(_static_successors(record, relocations, frames[record.line.segment]))
    while pending:
        off = pending.popleft()
        if off in starts or not 0 <= off < len(image.data):
            continue
        insn = next(decoder.disasm(image.data[off:off + 15], off, count=1), None)
        row = source_rows[off]
        overlap = owners[off]
        if overlap is None and insn is not None:
            overlap = next((owner for owner in owners[off:off + insn.size]
                            if owner is not None), None)
        # 8080-derived GW-BASIC deliberately executes DB B0..BF/3D as a
        # MOV/CMP that consumes the following instruction as its immediate,
        # and emits standalone segment prefixes as DB. Both entry paths
        # remain ahead-of-time translations with independently tested bytes.
        data_overlay = (allow_data_overlaps and row is not None and not row.is_instruction
                        and bases[row.segment] + row.offset == off and insn is not None
                        and (0xb0 <= image.data[off] <= 0xbf or image.data[off] == 0x3d
                             or ("SKIP" in row.source.upper() and image.data[off] in (0x04, 0x05, 0x0c, 0x0d, 0x14, 0x15, 0x1c, 0x1d, 0x24, 0x25, 0x2c, 0x2d, 0x34, 0x35, 0x3c))
                             or image.data[off] in (0x26, 0x2e, 0x36, 0x3e)))
        if overlap is not None and not data_overlay:
            kind = "listed" if overlap.line.is_instruction else "decoded"
            raise LayoutError(f"{listing.path}: static successor at image 0x{off:x} overlaps a "
                              f"{kind} instruction at image 0x{overlap.off:x}")
        if insn is None:
            raise LayoutError(f"{listing.path}: undecodable reachable bytes at image 0x{off:x}")
        if row is None:
            raise LayoutError(f"{listing.path}: reachable image offset 0x{off:x} is outside every "
                              "listing row; missing source/generated listing row")
        record = Instruction(off, insn, row)
        if return_skips and insn.mnemonic == "call" and insn.operands[0].type == X86_OP_IMM:
            frame = frames[row.segment]
            record.return_skip = return_skips.get(frame + ((insn.operands[0].imm - frame) & 0xffff), 0)
        _verify_relocations(image, record)
        instructions.append(record)
        starts.add(off)
        owners[off:off + insn.size] = [record] * insn.size
        pending.extend(_static_successors(record, relocations, frames[row.segment]))
    instructions.sort(key=lambda record: record.off)


def build_layout(image: LoadedImage, listing: Listing) -> Layout:
    if not listing.generated_listing and any(
            (proc.uses for proc in listing.procedures.values())):
        raise LayoutError(f"{listing.path}: listing hides assembler-generated PROC/RET instructions; "
                          "reassemble with -Sg (make gen creates build/gen/VC.*.lst)")
    relocatable = _relocatable_names(listing)
    bases = _segment_bases(image, listing, relocatable)
    _verify_bytes(image, listing, bases, relocatable)
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instructions = []
    for row in _normalized_rows(listing):
        if row.segment not in bases:
            raise LayoutError(f"{_where(listing, row)}: instruction segment has no linked-byte base")
        off = bases[row.segment] + row.offset
        raw = image.data[off:off + row.byte_count]
        insn = next(decoder.disasm(raw, off, count=1), None)
        if insn is None or insn.size != row.byte_count:
            actual = "undecodable" if insn is None else str(insn.size)
            raise LayoutError(f"{_where(listing, row)}: instruction length mismatch at image "
                              f"0x{off:x}: listing {row.byte_count}, capstone {actual}, bytes {raw.hex()}")
        record = Instruction(off, insn, row)
        _verify_relocations(image, record)
        instructions.append(record)
    instructions.sort(key=lambda record: record.off)
    for first, second in zip(instructions, instructions[1:]):
        if first.off + first.insn.size > second.off:
            raise LayoutError(f"{listing.path}: overlapping instruction starts at image "
                              f"0x{first.off:x} and 0x{second.off:x}")
    # A second, independent completeness check catches omitted generated rows:
    # each byte of every known code segment must belong to a decoded source
    # instruction or an explicit initialized data/alignment declaration.
    for segment in listing.segments.values():
        if not segment.is_code or not segment.length or segment.file_size == 0:
            continue
        covered = bytearray(segment.length)
        for row in listing.lines:
            if row.segment != segment.name or not row.bytes:
                continue
            if row.initializers and all(_is_uninitialized(i.expression) for i in row.initializers):
                continue
            end = min(row.offset + row.byte_count, segment.length)
            covered[row.offset:end] = b"\x01" * (end - row.offset)
        begin = max(0, -bases[segment.name])
        missing = covered.find(b"\x00", begin)
        if missing >= 0:
            raise LayoutError(f"{listing.path}: uncovered byte in code segment {segment.name} "
                              f"at listing offset 0x{missing:x}; missing source/generated listing row")
    _decode_static_successors(image, listing, bases, decoder, instructions)
    starts = {record.off for record in instructions}
    for proc in listing.procedures.values():
        if proc.end != proc.offset and bases[proc.segment] + proc.offset not in starts:
            raise LayoutError(f"{listing.path}: PROC {proc.name} entry at "
                              f"{proc.segment}:0x{proc.offset:x} is missing an instruction boundary")
    chunks, proc_chunks = [], {}
    previous = None
    for record in instructions:
        row = record.line
        if row.procedure:
            key = (row.segment, row.procedure)
            if key not in proc_chunks:
                proc_chunks[key] = Chunk(row.procedure, index=len(chunks))
                chunks.append(proc_chunks[key])
            chunk = proc_chunks[key]
        elif (previous is not None and not previous.line.procedure and
              previous.line.segment == row.segment and previous.off + previous.insn.size == record.off):
            chunk = previous.chunk
        else:
            chunk = Chunk(f"{row.segment}_run_{record.off:x}", index=len(chunks))
            chunks.append(chunk)
        record.chunk = chunk
        chunk.instructions.append(record)
        previous = record
    labels = {name: bases[label.segment] + label.offset for name, label in listing.labels.items()
              if label.segment in bases}
    procedures = {name: bases[proc.segment] + proc.offset for name, proc in listing.procedures.items()
                  if proc.segment in bases}
    return Layout(image, listing, instructions, chunks, bases, labels, procedures)
