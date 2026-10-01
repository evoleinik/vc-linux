"""Finite, source-proved instruction variants in VZ's macro interrupt call.

MACRO.ASM's &i macro writes either CD nn (INT nn) or EB 22 (JMP to_int)
to a two-byte DW slot. These two templates are compiled ahead of time; this
is not a decoder for arbitrary macro-created machine code.
"""

from .layout import Instruction, LayoutError


def prove_macro_variants(image, instructions, labels, decoder):
    """Attach the two variants only after proving the entire writer path."""
    if "MACRO::opcode" not in labels:
        return {}
    required = ("MACRO::smac_int", "MACRO::opcode", "MACRO::from_int", "MACRO::to_int")
    if any(name not in labels for name in required):
        raise LayoutError(f"{image.path}: incomplete VZ macro interrupt labels")
    beginning, site, continuation, target = (labels[name] for name in required)
    # readparm -> save BX/SI/BP -> load intregs pointer precedes this exact
    # branch. AH==0 selects INT AL; otherwise BP gets the far-call offset
    # and the fixed EB22 jumps to the source's far-call trampoline.
    expected = (bytes.fromhex("8bc20ae475068ae0b0cdeb058be8b8eb222ea3")
                + (site + 0x100).to_bytes(2, "little")
                + bytes.fromhex("eb008b058b5d028b4d048b55068b75088b7d0a1e060000"))
    by_offset = {record.off: record for record in instructions}
    record = by_offset.get(site)
    writer = site - 25
    if (record is None or record.line.mnemonic != "DW" or record.line.byte_count != 2
            or record.line.operands.strip() != "0" or continuation != site + 2
            or target != site + 2 + 0x22 or beginning + 9 + len(expected) != continuation
            or image.data[beginning + 9:continuation] != expected
            or writer not in by_offset or not by_offset[writer].line.is_instruction):
        raise LayoutError(f"{image.path}: unproven VZ macro interrupt instruction variants")
    variants = []
    for raw, mutable in ((b"\xcd\0", (1,)), (b"\xeb\x22", ())):
        insn = next(decoder.disasm(raw, site, count=1), None)
        if insn is None or insn.size != 2:
            raise LayoutError(f"{image.path}: cannot decode proved VZ macro variant")
        variants.append(Instruction(site, insn, record.line, linked=True, mutable_offsets=mutable))
    record.variants = tuple(variants)
    return {site: writer}
