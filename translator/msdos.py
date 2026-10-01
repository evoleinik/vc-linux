"""Listing-directed front end for Microsoft's multi-frame DOS 2 programs.

COMMAND's resident, transient and EXEC groups are separate address frames in
one flat COM. The other tools also predate the conventional CODE class. This
opt-in uses their explicit source instructions and linked group addresses;
it never obtains boundaries from a linear disassembly of the image.
"""

from capstone import CS_GRP_CALL, CS_GRP_JUMP
from capstone.x86_const import X86_OP_IMM, X86_OP_MEM, X86_REG_CS

from .layout import Instruction, LayoutError
from .linked import build_linked_layout


def build_msdos_layout(image, listings, map_path):
    return build_linked_layout(image, listings, map_path, source_frames=True)


def terminating_dos_calls(image, instructions, labels):
    """Do not invent an INT 21h/4Ch return path into following message data.

    The immediately preceding instruction must set AH=4Ch, with no alternate
    labeled/direct entry to the INT itself. This only bounds static closure;
    emitted instructions still make the ordinary interrupt/dispatcher call.
    """
    entries = set(labels.values())
    for record in instructions:
        if record.insn.group(CS_GRP_CALL) or record.insn.group(CS_GRP_JUMP):
            entries.update(op.imm for op in record.insn.operands if op.type == X86_OP_IMM)
    terminals = set()
    for previous, record in zip(instructions, instructions[1:]):
        raw = bytes(previous.insn.bytes)
        if (record.off not in entries and bytes(record.insn.bytes) == b"\xcd\x21"
                and previous.off + len(raw) == record.off
                and (raw == b"\xb4\x4c" or (len(raw) == 3 and raw[0] == 0xb8 and raw[2] == 0x4c))
                and previous.off + previous.insn.imm_offset not in image.relocations):
            terminals.add(record.off)
    return terminals


def prove_sort_variants(image, instructions, labels, decoder, frames):
    """Compile SORT's original JAE and its single source-written JB form.

    /R writes literal 72h over CODE_PATCH's first byte. Both two-byte
    instruction templates are fixed at build time; neither the displacement
    nor any other opcode is writable under this proof.
    """
    labels = {name.upper(): value for name, value in labels.items()}
    if "SORT::CODE_PATCH" not in labels:
        return {}
    required = ("SORT::CODE_PATCH", "SORT::TESTED_NOT_EQUAL", "SORT::INNER_SORT_LOOP",
                "SORT::SWITCH_LOOP")
    if any(name not in labels for name in required):
        raise LayoutError(f"{image.path}: incomplete SORT reverse-branch labels")
    site, equal, target, switch = (labels[name] for name in required)
    by_offset = {record.off: record for record in instructions}
    record = by_offset.get(site)
    writers = []
    for candidate in instructions:
        operands = candidate.insn.operands
        if (candidate.insn.mnemonic == "mov" and len(operands) == 2
                and operands[0].type == X86_OP_MEM and operands[0].size == 1
                and operands[0].mem.segment == X86_REG_CS
                and not operands[0].mem.base and not operands[0].mem.index
                and frames[candidate.line.segment] + (operands[0].mem.disp & 0xffff) == site
                and operands[1].type == X86_OP_IMM and operands[1].imm == 0x72):
            writers.append(candidate)
    if record is None or len(writers) != 1:
        raise LayoutError(f"{image.path}: unproven SORT reverse-branch writer")
    writer = writers[0]
    relative = site - frames[writer.line.segment]
    expected_writer = b"\x2e\xc6\x06" + relative.to_bytes(2, "little") + b"\x72"
    prefix = bytes((0x0c, 0x20, 0x3c, 0x72, 0x75, (switch - writer.off) & 255))
    suffix = bytes((0xeb, (switch - writer.off - 8) & 255))
    raw = bytes(record.insn.bytes)
    if (equal != site or record.line.mnemonic != "JAE" or len(raw) != 2 or raw[0] != 0x73
            or record.insn.operands[0].imm != target or site + 2 not in by_offset
            or image.data[site + 2:site + 4] != b"\x8b\xde"
            or bytes(writer.insn.bytes) != expected_writer
            or image.data[writer.off - 6:writer.off + 8] != prefix + expected_writer + suffix
            or not all(off in by_offset for off in (writer.off - 6, writer.off - 4,
                                                    writer.off - 2, writer.off + 6))):
        raise LayoutError(f"{image.path}: unproven SORT reverse-branch instruction variants")
    variants = []
    for opcode in (0x73, 0x72):
        insn = next(decoder.disasm(bytes((opcode, raw[1])), site, count=1), None)
        if insn is None or insn.size != 2 or insn.operands[0].imm != target:
            raise LayoutError(f"{image.path}: cannot decode proved SORT reverse-branch variant")
        variants.append(Instruction(site, insn, record.line, linked=True))
    record.variants = tuple(variants)
    return {site: writer.off}
