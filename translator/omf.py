"""Read the 16-bit OMF evidence that a compiled-program map refers to.

Only initialized bytes, public symbols and link fixups are needed.  Unsupported
record forms are errors, not a reason to silently stop checking linked bytes.
Library members retain their original OMF records, including CRT members.
"""

from dataclasses import dataclass, field
from pathlib import Path

from .layout import LayoutError


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, count: int) -> bytes:
        end = self.at + count
        if end > len(self.data):
            raise LayoutError("truncated OMF record")
        result, self.at = self.data[self.at:end], end
        return result

    def byte(self) -> int:
        return self.take(1)[0]

    def word(self) -> int:
        return int.from_bytes(self.take(2), "little")

    def index(self) -> int:
        value = self.byte()
        return ((value & 127) << 8) | self.byte() if value & 128 else value

    def name(self) -> str:
        return self.take(self.byte()).decode("latin1")

    def __bool__(self):
        return self.at < len(self.data)


@dataclass(frozen=True)
class Reference:
    method: int
    datum: int = 0


@dataclass(frozen=True)
class Fixup:
    segment: int
    offset: int
    kind: int
    relative: bool
    frame: Reference
    target: Reference
    displacement: int

    @property
    def width(self):
        return {0: 1, 1: 2, 2: 2, 3: 4, 4: 1, 5: 2}[self.kind]


@dataclass
class OmfSegment:
    name: str
    kind: str
    size: int
    data: bytearray = field(default_factory=bytearray)
    initialized: bytearray = field(default_factory=bytearray)


@dataclass
class OmfModule:
    name: str
    raw: bytes
    segments: list[OmfSegment | None] = field(default_factory=lambda: [None])
    groups: list[str] = field(default_factory=lambda: [""])
    externals: list[str] = field(default_factory=lambda: [""])
    publics: dict[str, tuple[int, int, int]] = field(default_factory=dict)
    public_groups: dict[str, int] = field(default_factory=dict)
    fixups: list[Fixup] = field(default_factory=list)
    data_ranges: list[tuple[int, int, int]] = field(default_factory=list)


def records(raw: bytes, start=0):
    at = start
    while at < len(raw):
        if at + 3 > len(raw):
            raise LayoutError(f"truncated OMF record header at 0x{at:x}")
        kind, size = raw[at], int.from_bytes(raw[at + 1:at + 3], "little")
        end = at + 3 + size
        if not size or end > len(raw):
            raise LayoutError(f"truncated OMF record 0x{kind:02x} at 0x{at:x}")
        # A zero checksum denotes an unchecked record in the OMF format.
        if raw[end - 1] and sum(raw[at:end]) & 255:
            raise LayoutError(f"OMF checksum mismatch at 0x{at:x}")
        yield kind, raw[at + 3:end - 1], end
        at = end
        if kind in (0x8a, 0x8b):
            return


def library_members(path: str | Path) -> dict[str, bytes]:
    """Return original member records, not wlib's rewritten object files."""
    path = Path(path)
    raw = path.read_bytes()
    if raw[:1] != b"\xf0":
        raise LayoutError(f"{path}: not an OMF library")
    page = int.from_bytes(raw[1:3], "little") + 3
    if page < 16 or page & (page - 1):
        raise LayoutError(f"{path}: unsupported OMF library page size {page}")
    at, result = page, {}
    while at < len(raw) and raw[at] in (0x80, 0x82):
        begin = at
        parsed = list(records(raw, at))
        if not parsed or parsed[-1][0] not in (0x8a, 0x8b):
            raise LayoutError(f"{path}: library member without MODEND")
        name = Reader(parsed[0][1]).name()
        if name in result:
            raise LayoutError(f"{path}: duplicate OMF library member {name}")
        at = parsed[-1][2]
        result[name] = raw[begin:at]
        at = ((at + page - 1) // page) * page
    if not result:
        raise LayoutError(f"{path}: empty OMF library")
    return result


def _iterated(reader: Reader) -> bytes:
    count, children = reader.word(), reader.word()
    if children:
        body = b"".join(_iterated(reader) for _ in range(children))
    else:
        body = reader.take(reader.byte())
    if count * len(body) > 65536:
        raise LayoutError("OMF iterated data exceeds a 16-bit segment")
    return body * count


def parse_module(raw: bytes) -> OmfModule:
    parsed = list(records(raw))
    if not parsed or parsed[0][0] not in (0x80, 0x82) or parsed[-1][0] != 0x8a:
        raise LayoutError("require one complete 16-bit OMF module")
    module = OmfModule(Reader(parsed[0][1]).name(), raw)
    names = [""]
    threads = [{}, {}]
    data_segment, data_offset, iterated = None, 0, False
    for kind, payload, _ in parsed[1:]:
        reader = Reader(payload)
        if kind == 0x96:  # LNAMES
            while reader:
                names.append(reader.name())
        elif kind == 0x98:  # SEGDEF
            attributes = reader.byte()
            if attributes & 1:
                raise LayoutError("32-bit OMF segment is not supported")
            if not attributes >> 5:
                raise LayoutError("absolute OMF segment is not supported")
            size = reader.word()
            if attributes & 2:
                size = 65536
            name, category = names[reader.index()], names[reader.index()]
            reader.index()  # overlay name
            module.segments.append(OmfSegment(name, category, size,
                                              bytearray(size), bytearray(size)))
        elif kind == 0x9a:  # GRPDEF
            module.groups.append(names[reader.index()])
            while reader:
                if reader.byte() != 255:
                    raise LayoutError("unsupported OMF group component")
                reader.index()
        elif kind in (0x8c, 0xb4):  # EXTDEF, LEXTDEF
            while reader:
                module.externals.append(reader.name())
                reader.index()
        elif kind in (0x90, 0xb6):  # PUBDEF, LPUBDEF
            group, segment = reader.index(), reader.index()
            absolute = reader.word() if not segment else 0
            while reader:
                name, offset = reader.name(), reader.word()
                reader.index()
                if name in module.publics:
                    raise LayoutError(f"{module.name}: duplicate OMF public {name}")
                module.publics[name] = (segment, offset, absolute)
                module.public_groups[name] = group
        elif kind in (0xa0, 0xa2):  # LEDATA, LIDATA
            data_segment, data_offset = reader.index(), reader.word()
            iterated = kind == 0xa2
            if iterated:
                pieces = []
                while reader:
                    pieces.append(_iterated(reader))
                data = b"".join(pieces)
            else:
                data = reader.take(len(payload) - reader.at)
            segment = module.segments[data_segment]
            end = data_offset + len(data)
            if end > segment.size or any(segment.initialized[data_offset:end]):
                raise LayoutError(f"{module.name}: overlapping/out-of-range OMF data")
            segment.data[data_offset:end] = data
            segment.initialized[data_offset:end] = b"\1" * len(data)
        elif kind == 0x9c:  # FIXUPP
            while reader:
                first = reader.byte()
                if first & 128:
                    if data_segment is None or iterated:
                        raise LayoutError("FIXUPP without flat LEDATA is unsupported")
                    offset = data_offset + ((first & 3) << 8) + reader.byte()
                    location = (first >> 2) & 15
                    if location not in (0, 1, 2, 3, 4, 5):
                        raise LayoutError(f"unsupported OMF fixup location {location}")
                    spec = reader.byte()
                    if spec & 128:
                        frame = threads[1].get((spec >> 4) & 3)
                    else:
                        method = (spec >> 4) & 7
                        frame = Reference(method, reader.index() if method <= 2 else
                                          reader.word() if method == 3 else 0)
                    if spec & 8:
                        target = threads[0].get(spec & 3)
                    else:
                        method = spec & 3
                        target = Reference(method, reader.word() if method == 3 else reader.index())
                    if frame is None or target is None:
                        raise LayoutError("OMF fixup uses an undefined thread")
                    displacement = 0 if spec & 4 else reader.word()
                    fixup = Fixup(data_segment, offset, location, not bool(first & 64),
                                  frame, target, displacement)
                    if offset + fixup.width > module.segments[data_segment].size:
                        raise LayoutError("OMF fixup extends beyond its segment")
                    module.fixups.append(fixup)
                else:
                    frame = (first >> 6) & 1
                    method = (first >> 2) & (7 if frame else 3)
                    datum = reader.index() if method <= 2 else reader.word() if method == 3 else 0
                    threads[frame][first & 3] = Reference(method, datum)
        elif kind == 0x88:  # COMENT, including Watcom's data-in-code metadata.
            reader.byte()  # attribute flags
            category = reader.byte()
            if category == 0xfd:
                if reader.byte() != ord("s"):
                    raise LayoutError(f"{module.name}: unsupported OMF disassembly directive")
                segment, begin, end = reader.index(), reader.word(), reader.word()
                if reader or not 0 <= begin <= end <= module.segments[segment].size:
                    raise LayoutError(f"{module.name}: invalid OMF data-in-code range")
                module.data_ranges.append((segment, begin, end))
        elif kind in (0x8a, 0x94, 0x9e):
            # MODEND, LINNUM and TYPDEF do not initialize linked bytes.
            pass
        else:
            raise LayoutError(f"{module.name}: unsupported OMF record 0x{kind:02x}")
    return module
