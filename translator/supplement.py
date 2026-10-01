"""Source-proved indirect graphics entries, without changing existing images.

The original GW-BASIC translation predates graphics and follows only direct
control-flow edges into DB-encoded instructions. DRAW's U/L/G/H handlers are
entered through a source DW table and start with an INS86 NEGDE macro. Keep
the original generated C byte-identical and emit these four starts separately.
"""

from dataclasses import replace
import re

from capstone import Cs, CS_ARCH_X86, CS_MODE_16

from .emit import emit_instruction_function, emit_prelude
from .layout import Chunk, Instruction, Layout, LayoutError


def build_gwbasic_graphics_layout(base: Layout) -> Layout:
    """Validate DRAW's source pointer table and its previously unseeded macros."""
    table = base.labels.get("ADVGRP::DRWTAB")
    if table is None or not base.linked:
        raise LayoutError("GW-BASIC graphics supplement requires ADVGRP::DRWTAB")
    rows = {}
    for row in base.listing.lines:
        if row.bytes and row.segment in base.segment_bases and row.offset is not None:
            rows[base.segment_bases[row.segment] + row.offset] = row
    known = {record.off for record in base.instructions}
    decoder = Cs(CS_ARCH_X86, CS_MODE_16)
    decoder.detail = True
    instructions = []
    cursor = table
    commands = set()
    while True:
        command = rows.get(cursor)
        if command is None or command.mnemonic != "DB" or command.byte_count != 1:
            raise LayoutError(f"DRAW table lacks an explicit DB command at 0x{cursor:x}")
        value = base.image.data[cursor]
        if not value:
            break
        letter = value & 0x7f
        if not ord("A") <= letter <= ord("Z") or letter in commands:
            raise LayoutError("DRAW table contains an invalid or repeated command")
        commands.add(letter)
        pointer = rows.get(cursor + 1)
        if (pointer is None or pointer.mnemonic != "DW" or pointer.byte_count != 2
                or len(pointer.initializers) != 1):
            raise LayoutError(f"DRAW table lacks a source DW target at 0x{cursor + 1:x}")
        symbol = pointer.initializers[0].expression.strip(" `")
        if not re.fullmatch(r"[A-Za-z_][\w$]*", symbol):
            raise LayoutError(f"DRAW pointer is not a source label: {symbol!r}")
        local = "ADVGRP::" + symbol
        targets = ([base.labels[local]] if local in base.labels else
                   [off for name, off in base.labels.items() if name.endswith("::" + symbol)])
        target = int.from_bytes(base.image.data[cursor + 1:cursor + 3], "little")
        if target not in targets:
            raise LayoutError(f"DRAW pointer for {symbol} disagrees with its linked source label")
        if target not in known:
            # This exact two-byte source-qualified macro, rather than the
            # opcode alone, proves the instruction start and its boundary.
            macro = next((row for row in base.listing.lines
                          if row.mnemonic == "NEGDE" and not row.bytes
                          and row.segment in base.segment_bases and row.offset is not None
                          and base.segment_bases[row.segment] + row.offset == target), None)
            first, second = rows.get(target), rows.get(target + 1)
            if (macro is None or first is None or second is None
                    or first.mnemonic != "DB" or second.mnemonic != "DB"
                    or first.byte_count != 1 or second.byte_count != 1
                    or bytes(first.bytes + second.bytes) != b"\xf7\xda"
                    or base.image.data[target:target + 2] != b"\xf7\xda"
                    or target + 2 not in known):
                raise LayoutError(f"unproved indirect DRAW instruction at 0x{target:x} ({symbol})")
            instruction = next(decoder.disasm(base.image.data[target:target + 2], target, count=1))
            if instruction.size != 2:
                raise LayoutError(f"DRAW macro has an inconsistent boundary at 0x{target:x}")
            instructions.append(Instruction(target, instruction, replace(
                first, byte_count=2, bytes=first.bytes + second.bytes,
                expansion="DRAW DW entry into NEGDE / INS86"), linked=True))
        cursor += 3
    if commands != set(b"UDLRMEFGHABNXCS"):
        raise LayoutError("incomplete GW-BASIC DRAW source dispatch table")
    instructions.sort(key=lambda record: record.off)
    chunk = Chunk("gwbasic_graphics", instructions)
    for record in instructions:
        record.chunk = chunk
    return Layout(base.image, base.listing, instructions, [chunk],
                  base.segment_bases, base.labels, {}, linked=True)


def emit_supplement(layout: Layout, symbol: str) -> str:
    """Emit exact-start single steps for the runtime's normal image matcher."""
    if not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", symbol):
        raise ValueError(f"invalid supplement symbol {symbol!r}")
    output = [emit_prelude()]
    for record in layout.instructions:
        name = f"{symbol}_{record.off:x}"
        function = emit_instruction_function(record, layout.image.relocations, name)
        output.append(function.replace(f"int {name}(", f"static int {name}(", 1))
    output.append(f"int {symbol}(uint32_t off, uint16_t loadseg) {{\n    switch (off) {{")
    for record in layout.instructions:
        output.append(f"    case 0x{record.off:x}u: return {symbol}_{record.off:x}(loadseg);")
    output.append("    default: return -1;\n    }\n}\n")
    return "\n".join(output)
