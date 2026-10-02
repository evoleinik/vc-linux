"""VC 4.05's TASM listing dialect, without changing earlier translations.

The private assembler retains the source's LOOP mnemonic on a seven-byte
JUMPS expansion. Its three boundaries are proved from that exact opcode
template and the source label; all linked-byte checks remain in build_layout.
"""

from dataclasses import replace

from .image import LoadedImage
from .layout import Layout, LayoutError, build_layout
from .listing import Listing


_LOOP_OPCODES = {"LOOPNZ": 0xE0, "LOOPNE": 0xE0, "LOOPZ": 0xE1,
                 "LOOPE": 0xE1, "LOOP": 0xE2, "JCXZ": 0xE3}


def build_vc405_layout(image: LoadedImage, listing: Listing) -> Layout:
    if image.is_exe:
        raise LayoutError("VC 4.05 requires a flat COM image")
    rows = []
    labels = {name.upper(): label for name, label in listing.labels.items()}
    for row in listing.lines:
        if not row.is_instruction or row.mnemonic not in _LOOP_OPCODES or row.byte_count == 2:
            rows.append(row)
            continue
        target = labels.get(row.operands.strip().upper())
        raw = row.bytes
        if (row.offset is None or len(raw) != 7 or row.byte_count != 7 or
                raw[:5] != (_LOOP_OPCODES[row.mnemonic], 2, 0xEB, 3, 0xE9) or
                any(byte is None for byte in raw) or target is None or
                target.segment != row.segment or
                (row.offset + 7 + int.from_bytes(bytes(raw[5:]), "little", signed=True)) & 0xFFFF
                != target.offset):
            raise LayoutError(f"{listing.path}:{row.lineno}: unproven TASM JUMPS expansion")
        for begin, end, description in ((0, 2, "conditional branch"),
                                        (2, 4, "fall-through jump"), (4, 7, "near jump")):
            rows.append(replace(row, offset=row.offset + begin, bytes=raw[begin:end],
                                byte_count=end - begin, fixups=(),
                                expansion="TASM JUMPS: " + description))
    return build_layout(image, replace(listing, lines=rows))
