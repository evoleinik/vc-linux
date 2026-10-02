"""Parse JWasm's fixed-column listing, including generated ``-Sg`` rows.

The byte column is a *pre-link* view. Instruction fixups carry ``o``/``s``
markers; JWasm unfortunately omits those markers on data initializers. We
retain those initializers separately so layout can resolve and check them,
instead of accepting arbitrary discrepancies in the linked data.
"""

from dataclasses import dataclass, field
from pathlib import Path
import re

from capstone import x86_const


class ListingError(ValueError):
    """A listing is not complete enough to translate safely."""


DATA_WIDTHS = {"DB": 1, "SBYTE": 1, "BYTE": 1, "DW": 2, "SWORD": 2,
               "WORD": 2, "DD": 4, "SDWORD": 4, "DWORD": 4, "DF": 6,
               "FWORD": 6, "DQ": 8, "QWORD": 8, "DT": 10, "TBYTE": 10,
               "REAL4": 4, "REAL8": 8, "REAL10": 10}
PREFIXES = {"REP", "REPE", "REPZ", "REPNE", "REPNZ", "LOCK",
            "CS:", "DS:", "ES:", "SS:", "FS:", "GS:"}
NON_INSTRUCTIONS = {"ALIGN", "EVEN", "ORG", "LABEL", "PROC", "ENDP", "ENDS",
                    "SEGMENT", "EQU", "=", "INCLUDE", "END", "MACRO", "ENDM"}
_INSTRUCTION_HEADS = {name.removeprefix("X86_INS_") for name in vars(x86_const)
                      if name.startswith("X86_INS_") and name not in {"X86_INS_INVALID", "X86_INS_ENDING"}}
_INSTRUCTION_HEADS.update("LODS MOVS STOS SCAS CMPS INS OUTS RETN".split())


@dataclass
class DataInitializer:
    offset: int
    width: int
    expression: str


@dataclass
class ListingLine:
    lineno: int
    segment: str | None
    offset: int | None
    bytes: tuple[int | None, ...]
    byte_count: int
    source: str
    procedure: str | None = None
    is_instruction: bool = False
    mnemonic: str = ""
    operands: str = ""
    fixups: tuple[tuple[int, int, str], ...] = ()
    initializers: list[DataInitializer] = field(default_factory=list)
    generated: bool = False
    expansion: str | None = None


@dataclass
class Segment:
    name: str
    is_code: bool
    length: int = 0
    group: str | None = None
    file_size: int | None = None
    map_rva: int | None = None


@dataclass
class Procedure:
    name: str
    segment: str
    offset: int
    end: int | None = None
    far: bool = False
    uses: tuple[str, ...] = ()


@dataclass
class Label:
    name: str
    segment: str
    offset: int
    procedure: str | None = None


@dataclass
class Structure:
    name: str
    fields: list[tuple[int, str]] = field(default_factory=list)
    size: int = 0
    defaults: dict[int, str] = field(default_factory=dict)


@dataclass
class Listing:
    path: Path
    lines: list[ListingLine]
    segments: dict[str, Segment]
    procedures: dict[str, Procedure]
    labels: dict[str, Label]
    constants: dict[str, int] = field(default_factory=dict)
    structures: dict[str, Structure] = field(default_factory=dict)
    generated_listing: bool = False
    external_segments: dict[str, str] = field(default_factory=dict)


def strip_comment(source: str) -> str:
    quote = None
    i = 0
    while i < len(source):
        char = source[i]
        if quote:
            if char == quote:
                if i + 1 < len(source) and source[i + 1] == quote:
                    i += 2
                    continue
                quote = None
        elif char in "\"'":
            quote = char
        elif char == ";":
            return source[:i]
        i += 1
    return source


def split_operands(text: str) -> list[str]:
    """Split MASM initializers without splitting strings, DUPs or structures."""
    result, start, depth, quote = [], 0, 0, None
    i = 0
    while i < len(text):
        char = text[i]
        if quote:
            if char == quote:
                if i + 1 < len(text) and text[i + 1] == quote:
                    i += 2
                    continue
                quote = None
        elif char in "\"'":
            quote = char
        elif char in "([<":
            depth += 1
        elif char in ")]>":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:i].strip())
            start = i + 1
        i += 1
    result.append(text[start:].strip())
    return result


def _statement(source: str, structures: dict[str, Structure]) -> tuple[str | None, str, str]:
    text = strip_comment(source).strip()
    label = None
    match = re.match(r"^([\w@?$\.]+)::?\s*(.*)$", text)
    if match and match[1].upper() not in {"CS", "DS", "ES", "SS", "FS", "GS"}:
        label, text = match[1], match[2]
    words = text.split(None, 2)
    operand_type = len(words) > 2 and words[2].upper().startswith("PTR ")
    if len(words) > 1 and not operand_type and words[0].upper() not in _INSTRUCTION_HEADS and words[1].upper() in (
            set(DATA_WIDTHS) | set(structures) | NON_INSTRUCTIONS | {"STRUC", "STRUCT"}):
        label, mnemonic = words[:2]
        operands = words[2] if len(words) > 2 else ""
        return label, mnemonic.upper(), operands
    words = text.split(None, 1)
    return label, words[0].upper() if words else "", words[1] if len(words) > 1 else ""


def _parse_byte_field(field: str, path: Path, lineno: int):
    values, fixups = [], []
    for token in field.split():
        match = re.fullmatch(r"([0-9A-Fa-f]+)([osri])?", token)
        if not match or len(match[1]) % 2:
            raise ListingError(f"{path}:{lineno}: unsupported byte-column token {token!r}")
        count = len(match[1]) // 2
        if match[2]:
            fixups.append((len(values), count, match[2]))
            values.extend([None] * count)
        else:
            values.extend(bytes.fromhex(match[1]))
    return tuple(values), tuple(fixups)


def _metadata(text: str) -> tuple[dict[str, Segment], dict[str, Label], dict[str, int]]:
    segments, labels, constants = {}, {}, {}
    if "Segments and Groups:" in text:
        table = text.split("Segments and Groups:", 1)[1].split("Procedures,", 1)[0]
        group = None
        for row in table.splitlines():
            gm = re.match(r"^(\S+)\s+(?:\.\s+)*GROUP\s*$", row)
            if gm:
                group = gm[1]
            sm = re.match(r"^(\S+)\s+(?:\.\s+)*16 Bit\s+([0-9A-F]+)\s+\S+\s+\S+\s+'([^']*)'", row)
            if sm:
                segments[sm[1]] = Segment(sm[1], sm[3].upper() == "CODE", int(sm[2], 16), group)
    if "Binary Map:" in text:
        table = text.split("Binary Map:", 1)[1].split("Macros:", 1)[0]
        for row in table.splitlines():
            match = re.match(r"^(\S+)\s+([0-9A-F]+)\s+([0-9A-F]+)\s+([0-9A-F]+)\s+([0-9A-F]+)\s*$", row)
            if match and match[1] in segments:
                seg = segments[match[1]]
                seg.file_size, seg.map_rva = int(match[4], 16), int(match[3], 16)
    if "Symbols:" in text:
        for row in text.split("Symbols:", 1)[1].splitlines():
            match = re.match(r"^(\S+)\s+(?:\.\s+)*(.+?)\s+([0-9A-F]+)h(?:\s+(\S+))?", row)
            if not match:
                continue
            name, _, value, seg = match.groups()
            if seg in segments:
                labels[name] = Label(name, seg, int(value, 16))
            else:
                constants[name] = int(value, 16)
    return segments, labels, constants


def parse_listing(path: str | Path, *, linked: bool = False, flat: bool = False,
                  physical_lines: bool = False) -> Listing:
    path = Path(path)
    # VC 4.05 quotes DOS control characters in DB strings. Python splitlines()
    # treats some as line breaks; its default text reader also rewrites bare
    # CRs. Opt in to actual LF-delimited listing lines, retaining all earlier
    # programs' established parsing and generated C byte-for-byte.
    text = (path.read_bytes().decode("utf-8", errors="replace") if physical_lines else
            path.read_text(encoding="utf-8", errors="replace"))
    segments, labels, constants = _metadata(text)
    if flat:
        # A flat COM may put executable source in WORK/BASE/INIT as well as
        # CODE. Treat the whole address space as eligible, but still take
        # instruction starts exclusively from active source listing rows.
        for segment in segments.values():
            segment.is_code = True
    external_segments = {}
    if linked:
        # GW-BASIC's historical segment class is CODESG, not MASM's newer
        # conventional CODE. This opt-in leaves the single-image path alone.
        for match in re.finditer(r"^(\S+).*16 Bit.*'(?:CODESG|KCODE)'", text, re.M | re.I):
            segments[match[1]].is_code = True
        for match in re.finditer(r"^(\S+).*\bExternal\b", text, re.M):
            label = labels.pop(match[1], None)
            if label:
                external_segments[match[1].strip("`").upper()] = label.segment
    source_text = text.split("Binary Map:", 1)[0]
    if linked:
        source_text = source_text.split("\nMacros:", 1)[0]
    default_code = next((s for s in segments if s.upper().endswith("_TEXT")), "_TEXT")
    segment = None
    segment_stack = []
    active_procs: dict[str, str | None] = {}
    procedures, structures, rows = {}, {}, []
    structure = None
    generated = False
    comment_delimiter = None
    pending_labels: list[tuple[str, str, str | None]] = []
    source_lines = source_text.split("\n") if physical_lines else source_text.splitlines()
    for lineno, raw in enumerate(source_lines, 1):
        if physical_lines:
            raw = raw.removesuffix("\r")
        if len(raw) < 32:
            continue
        source = raw[32:]
        if linked:
            source = source.replace("`", "")
            source = re.sub(r"\b(D[BWD])(?=[\"'])", r"\1 ", source, flags=re.I)
        if comment_delimiter:
            if comment_delimiter in source:
                comment_delimiter = None
            rows.append(ListingLine(lineno, None, None, (), 0, source))
            continue
        label, mnemonic, operands = _statement(source, structures)
        if linked and mnemonic in {"SEGMENT", "ENDS"} and label not in segments:
            # CASEMAP:ALL puts uppercase identifiers in the symbol table,
            # while source rows retain their original case. Do not conflate
            # genuinely distinct CASEMAP:NONE segments.
            matches = [name for name in segments if name.upper() == (label or "").upper()]
            if len(matches) == 1:
                label = matches[0]
        if mnemonic == "COMMENT" and operands:
            comment_delimiter = operands[0]
            rows.append(ListingLine(lineno, None, None, (), 0, source))
            continue
        address = re.match(r"^([0-9A-F]{4,8}) ", raw)
        offset = int(address[1], 16) if address else None
        if flat and mnemonic in ("SEGMENT", "ENDS") and offset is None:
            # -Sa also prints inactive IFDEF branches. Their declarations
            # have no listing address and must not change the active segment.
            rows.append(ListingLine(lineno, None, None, (), 0, source))
            continue
        byte_values, fixups = (), ()
        if address:
            field = raw[address.end():28].strip()
            if not field.startswith("="):
                byte_values, fixups = _parse_byte_field(field, path, lineno)
        is_generated = raw[28:29] == "*"
        generated |= is_generated
        if raw.lstrip().startswith(">"):
            rows.append(ListingLine(lineno, None, offset, (), 0, source))
            continue
        if mnemonic in {"STRUC", "STRUCT"}:
            if not label:
                raise ListingError(f"{path}:{lineno}: unnamed structure")
            structure = Structure(label)
            structures[label.upper()] = structure
        if structure:
            if mnemonic == "ENDS":
                structure.size = offset or 0
                constants[structure.name] = structure.size
                structure = None
            else:
                if label and offset is not None:
                    constants[label] = offset
                if mnemonic in DATA_WIDTHS or mnemonic in structures:
                    structure.fields.append((offset or 0, mnemonic))
                    if linked:
                        structure.defaults[offset or 0] = operands
            rows.append(ListingLine(lineno, None, offset, byte_values, len(byte_values), source))
            continue
        if raw.startswith(" = ") and label:
            eq = re.match(r" = ([0-9A-F]+)\s", raw)
            if eq:
                constants[label] = int(eq[1], 16)
        if mnemonic == ".CODE":
            segment = operands.split()[0] if operands else default_code
            segments.setdefault(segment, Segment(segment, True))
        elif mnemonic in {".DATA", ".DATA?", ".CONST", ".FARDATA", ".FARDATA?"}:
            defaults = {".DATA": "_DATA", ".DATA?": "_BSS", ".CONST": "CONST",
                        ".FARDATA": "FAR_DATA", ".FARDATA?": "FAR_BSS"}
            segment = operands.split()[0] if operands else defaults[mnemonic]
            segments.setdefault(segment, Segment(segment, False))
        elif mnemonic == "SEGMENT" and label:
            if linked:
                segment_stack.append(segment)
            segment = label
            segments.setdefault(segment, Segment(segment, "'CODE'" in operands.upper()))
        elif mnemonic == "ENDS" and label == segment:
            segment = segment_stack.pop() if linked and segment_stack else None
        proc = active_procs.get(segment) if segment else None
        if mnemonic == "PROC" and segment and offset is not None and label:
            uses = re.search(r"\bUSES\s+(.+)", operands, re.I)
            used_regs = tuple(uses[1].split()) if uses else ()
            procedures[label] = Procedure(label, segment, offset, far="FAR" in operands.upper(), uses=used_regs)
            active_procs[segment] = proc = label
        if (label and segment and mnemonic not in {"EQU", "=", "SEGMENT", "ENDS"}
                and not (linked and mnemonic == "ENDP")):
            if offset is not None:
                key = f"{proc}::{label}" if label.startswith("@@") and proc else label
                labels[key] = Label(label, segment, offset, proc)
            elif mnemonic in {"RET", "RETF", "RETN"}:
                pending_labels.append((label, segment, proc))
        if byte_values and offset is not None and pending_labels:
            for name, label_seg, label_proc in pending_labels:
                if label_seg != segment:
                    raise ListingError(f"{path}:{lineno}: label crosses segments before generated code")
                key = f"{label_proc}::{name}" if name.startswith("@@") and label_proc else name
                labels[key] = Label(name, segment, offset, label_proc)
            pending_labels.clear()
        is_instruction = bool(byte_values and segment and segments[segment].is_code
                              and mnemonic not in DATA_WIDTHS and mnemonic not in structures
                              and mnemonic not in NON_INSTRUCTIONS and not mnemonic.startswith("."))
        row = ListingLine(lineno, segment, offset, byte_values, len(byte_values), source, proc,
                          is_instruction, mnemonic, operands, fixups, generated=is_generated)
        if linked and mnemonic == "ENDS":
            # A nested DSEG ENDS restores CSEG for the following source;
            # its printed DSEG offset must not truncate a preceding CSEG DB.
            row.segment = None
        rows.append(row)
        if mnemonic == "ENDP" and segment and label in procedures:
            procedures[label].end = offset
            active_procs[segment] = None
    # A long DB/DW listing is truncated after eleven displayed bytes. Infer its
    # full allocation length from the next listing offset in that same segment.
    next_offsets = {name: seg.length for name, seg in segments.items() if seg.length}
    for row in reversed(rows):
        if row.segment is None or row.offset is None:
            continue
        end = next_offsets.get(row.segment)
        if (len(row.bytes) == 11 and not row.is_instruction and
                end is not None and end > row.offset + len(row.bytes)):
            row.byte_count = end - row.offset
        next_offsets[row.segment] = row.offset
    listing = Listing(path, rows, segments, procedures, labels, constants, structures, generated)
    listing.external_segments = external_segments
    for row in rows:
        if row.bytes and not row.is_instruction and row.segment:
            row.initializers = _initializers(row.mnemonic, row.operands, listing)
    return listing


def _initializers(kind: str, operands: str, listing: Listing, base: int = 0) -> list[DataInitializer]:
    """Locate individual data fields; offsets come from the listing's STRUCs."""
    if kind in listing.structures:
        structure = listing.structures[kind]
        if not (operands.startswith("<") and operands.endswith(">")):
            return []
        values = split_operands(operands[1:-1])
        result = []
        for index, (offset, member_type) in enumerate(structure.fields):
            value = values[index] if index < len(values) and values[index] else structure.defaults.get(offset, "")
            if value:
                result.extend(_initializers(member_type, value, listing, base + offset))
        return result
    if kind not in DATA_WIDTHS:
        return []
    width = DATA_WIDTHS[kind]
    result, cursor = [], base
    values = split_operands(operands)
    for index, value in enumerate(values):
        if not value:
            continue
        if re.search(r"\bDUP\s*\(", value, re.I):
            if index + 1 < len(values):
                repeat = re.fullmatch(r"(.+)\s+DUP\s*\(([^()]*)\)", value, re.I)
                count = None
                if repeat:
                    from .layout import _evaluate
                    try:
                        count = _evaluate(repeat[1], {k.upper(): v for k, v in listing.constants.items()}, {})
                    except (KeyError, SyntaxError, TypeError, ValueError):
                        pass
                if count is None or not 0 <= count <= 65536:
                    raise ListingError(f"{listing.path}: cannot place initializer after {value!r}")
                for _ in range(count):
                    result.extend(_initializers(kind, repeat[2], listing, cursor))
                    cursor += width * len(split_operands(repeat[2]))
                continue
            # DUP arrays in these images contain only literals/uninitialized
            # storage. Symbolic DUP is rejected if layout needs to resolve it.
            result.append(DataInitializer(cursor, width, value))
            break
        if len(value) >= 2 and value[0] in "\"'" and value[-1] == value[0]:
            size = len(value[1:-1].replace(value[0] * 2, value[0])) if width == 1 else width
            cursor += size
            continue
        result.append(DataInitializer(cursor, width, value))
        cursor += width
    return result
