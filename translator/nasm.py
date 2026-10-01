"""Read NASM's expanded flat-COM listing, without sweeping data as code.

NASM's -Le rows (marked ;;;) identify the source that was actually assembled;
ordinary rows also include inactive %if branches and unexpanded macro calls.
The -Lf/-LF/-Lt options expose hidden/repeated bytes. Offsets in the listing
are section-relative, and bracketed fields still need the section's ORG.
Instructions are decoded only at listed boundaries or static data successors.
"""

from dataclasses import replace
from pathlib import Path
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_16
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_CS, X86_REG_DS

from .image import LoadedImage
from .layout import Chunk, Instruction, Layout, LayoutError, _JCC, _decode_static_successors
from .listing import (DATA_WIDTHS, PREFIXES, Label, Listing, ListingError,
                      ListingLine, Segment, _INSTRUCTION_HEADS, strip_comment)


_SECTION = "NASM_COM"
_ACTIVE = re.compile(r"\s*(?:<\d+>\s*)?;;;\s?(.*)$")
_LABEL = re.compile(r"^([\w.$?@~#]+):\s*(.*)$")


def _bytes(column: str, origin: int, where: str) -> tuple[int, ...]:
    result = bytearray()
    cursor = 0
    for match in re.finditer(r"\[([0-9A-Fa-f]+)\]|([0-9A-Fa-f]+)", column):
        if match.start() != cursor:
            raise ListingError(f"{where}: unsupported NASM byte field {column!r}")
        raw = bytes.fromhex(match[1] or match[2])
        if match[1]:
            # Flat binary has no loader relocations: these displayed values
            # are section offsets, adjusted by NASM's final binary backend.
            value = int.from_bytes(raw, "little") + origin
            raw = (value & ((1 << (8 * len(raw))) - 1)).to_bytes(len(raw), "little")
        result.extend(raw)
        cursor = match.end()
    if cursor != len(column):
        raise ListingError(f"{where}: unsupported NASM byte field {column!r}")
    return tuple(result)


def parse_nasm_listing(path: str | Path) -> Listing:
    """Parse -LefFt output for one 16-bit COM section with ORG 100h.

    Reject unproved formats instead of inferring instruction boundaries from
    next-line addresses. This reader intentionally does not support multiple
    NASM sections, INCBIN, or non-COM origins.
    """
    path = Path(path)
    rows, labels = [], {}
    listing = Listing(path, rows, {}, {}, labels, generated_listing=True)
    origin = None
    current = 0
    global_label = ""
    continuation = None
    active_seen = False
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if len(raw) < 35 or not raw[:7].strip().isdigit():
            continue
        active = _ACTIVE.fullmatch(raw[35:])
        address = raw[7:15].strip()
        column = raw[16:35].strip()
        if continuation is not None:
            if active or not address or not column:
                raise ListingError(f"{path}:{lineno}: missing NASM continuation row")
            previous = rows[-1]
            if int(address, 16) != current:
                raise ListingError(f"{path}:{lineno}: discontiguous NASM continuation")
            values = _bytes(column.removesuffix("-"), origin, f"{path}:{lineno}")
            rows[-1] = replace(previous, bytes=previous.bytes + values,
                               byte_count=previous.byte_count + len(values))
            current += len(values)
            continuation = lineno if column.endswith("-") else None
            continue
        if not active:
            if address and column:
                raise ListingError(f"{path}:{lineno}: require NASM -Le active-source listing")
            continue
        active_seen = True
        source = strip_comment(active[1]).strip()
        label = _LABEL.fullmatch(source)
        if label:
            name, source = label.groups()
            # EQU defines a value, not an executable/data address.
            if not source.upper().startswith("EQU "):
                if name.startswith("."):
                    name = global_label + name
                else:
                    global_label = name
                if name in labels:
                    raise ListingError(f"{path}:{lineno}: duplicate NASM label {name}")
                labels[name] = Label(name, _SECTION, 0x100 + current)
        directive = source.strip("[]").split(None, 1)
        mnemonic = directive[0].upper() if directive else ""
        operands = directive[1] if len(directive) > 1 else ""
        if mnemonic == "ORG":
            if origin is not None or current:
                raise ListingError(f"{path}:{lineno}: require one leading NASM ORG")
            try:
                origin = int(operands, 0)
            except ValueError as exc:
                raise ListingError(f"{path}:{lineno}: require literal ORG 0x100") from exc
            if origin != 0x100:
                raise ListingError(f"{path}:{lineno}: COM origin must be 0x100")
        if mnemonic in ("SECTION", "SEGMENT", "ABSOLUTE", "INCBIN"):
            raise ListingError(f"{path}:{lineno}: unsupported NASM {mnemonic}")
        if mnemonic == "BITS" and operands != "16":
            raise ListingError(f"{path}:{lineno}: require NASM BITS 16")
        if not column:
            continue
        if origin is None or not re.fullmatch(r"[0-9A-Fa-f]{8}", address):
            raise ListingError(f"{path}:{lineno}: byte row before ORG or without an address")
        offset = int(address, 16)
        if offset != current:
            raise ListingError(f"{path}:{lineno}: uncovered byte or overlapping NASM row at 0x{current:x}")
        # TIMES can abbreviate multiple instructions. Data repetitions have
        # exact bytes with -Lt; repeated instructions need separate boundaries.
        if mnemonic == "TIMES":
            repeat = re.fullmatch(r"\S+\s+(D[BWDQT])\s+(.+)", operands, re.I)
            if not repeat:
                raise ListingError(f"{path}:{lineno}: only data TIMES is supported")
            mnemonic, operands = repeat[1].upper(), repeat[2]
        is_instruction = (mnemonic in _INSTRUCTION_HEADS or mnemonic in PREFIXES
                          or mnemonic in _JCC or mnemonic == "XLAT")
        if not is_instruction and mnemonic not in DATA_WIDTHS and mnemonic not in ("ALIGN", "ALIGNB"):
            raise ListingError(f"{path}:{lineno}: unknown byte-emitting NASM statement {source!r}")
        values = _bytes(column.removesuffix("-"), origin, f"{path}:{lineno}")
        rows.append(ListingLine(lineno, _SECTION, origin + offset, values, len(values),
                                source, is_instruction=is_instruction,
                                mnemonic=mnemonic, operands=operands, generated=True))
        current += len(values)
        continuation = lineno if column.endswith("-") else None
    if not active_seen or origin is None or continuation is not None or not rows:
        raise ListingError(f"{path}: incomplete NASM listing; assemble with -LefFt")
    listing.segments[_SECTION] = Segment(_SECTION, True, origin + current, file_size=current)
    return listing


def build_nasm_layout(image: LoadedImage, listing: Listing) -> Layout:
    """Validate flat linked bytes and retain bootLogo's proved operand patch."""
    if image.is_exe or image.relocations:
        raise LayoutError(f"{listing.path}: NASM reader requires a flat COM image")
    if listing.segments[_SECTION].file_size != len(image.data):
        raise LayoutError(f"{listing.path}: NASM listing and COM image sizes disagree")
    bases = {_SECTION: -0x100}
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instructions = []
    for row in listing.lines:
        off = row.offset - 0x100
        raw = image.data[off:off + row.byte_count]
        if raw != bytes(row.bytes):
            raise LayoutError(f"{listing.path}:{row.lineno}: linked byte mismatch at 0x{off:x}")
        if not row.is_instruction:
            continue
        insn = next(decoder.disasm(raw, off, count=1), None)
        if insn is None or insn.size != row.byte_count:
            raise LayoutError(f"{listing.path}:{row.lineno}: NASM instruction length mismatch at 0x{off:x}")
        instructions.append(Instruction(off, insn, row))
    # bootLogo's two DB BAh bytes execute MOV DX,imm16, consuming the next
    # two-byte instruction. Preserve both starts using the same checked
    # reachable-data rule as the GW-BASIC frontend, not a linear sweep.
    _decode_static_successors(image, listing, bases, decoder, instructions,
                              allow_data_overlaps=True)
    mutable = set()
    for writer in instructions:
        for operand in writer.insn.operands:
            if (operand.type != X86_OP_MEM or not operand.access & 2
                    or operand.mem.base or operand.mem.index
                    or operand.mem.segment not in (0, X86_REG_CS, X86_REG_DS)):
                continue
            # COM code/data share the PSP frame at entry. Conservatively
            # treating absolute DS stores as possible code patches is safe
            # even if a program later changes DS: reads still use live CS.
            start = (operand.mem.disp & 0xffff) - 0x100
            for target in instructions:
                changed = set(range(start, start + operand.size)) & set(range(target.off, target.off + target.insn.size))
                if not changed:
                    continue
                # The emitter reads live MOV immediates; no support is
                # claimed for patching opcodes, displacements or branches.
                allowed = set()
                if (target.insn.mnemonic == "mov" and len(target.insn.operands) == 2
                        and target.insn.operands[1].type == X86_OP_IMM):
                    allowed = set(range(target.off + target.insn.imm_offset,
                                        target.off + target.insn.imm_offset + target.insn.imm_size))
                if not changed <= allowed:
                    raise LayoutError(f"{listing.path}: code store at 0x{writer.off:x} changes an unsupported opcode/operand at 0x{target.off:x}")
                # Even a high-byte-only store needs the emitter to read the
                # whole immediate. Copy matching ignores only changed bytes.
                mutable.update(changed)
    chunks = []
    for record in instructions:
        if not chunks or len(chunks[-1].instructions) >= 256:
            chunks.append(Chunk(f"nasm_{record.off:x}", index=len(chunks)))
        record.chunk = chunks[-1]
        chunks[-1].instructions.append(record)
        record.linked = True
        changed = tuple(off - record.off for off in sorted(mutable)
                        if record.off <= off < record.off + record.insn.size)
        record.mutable_offsets = ((record.insn.imm_offset,) + changed
                                  if changed and record.insn.imm_offset not in changed else changed)
    labels = {name: label.offset - 0x100 for name, label in listing.labels.items()}
    return Layout(image, listing, instructions, chunks, bases, labels, {},
                  linked=True, mutable_offsets=tuple(sorted(mutable)))
