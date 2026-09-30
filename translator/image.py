"""Read DOS load modules without applying their run-time relocations."""

from dataclasses import dataclass, field
from pathlib import Path
import struct


class ImageError(ValueError):
    """An input image is malformed or truncated."""


@dataclass
class LoadedImage:
    path: Path
    data: bytes
    relocations: list[int] = field(default_factory=list)
    is_exe: bool = False
    hdr_cs: int = 0
    hdr_ip: int = 0
    hdr_ss: int = 0
    hdr_sp: int = 0
    min_alloc: int = 0
    max_alloc: int = 0
    header: dict[str, int] = field(default_factory=dict)


def load_image(path: str | Path) -> LoadedImage:
    path = Path(path)
    raw = path.read_bytes()
    if raw[:2] not in (b"MZ", b"ZM"):
        return LoadedImage(path, raw)
    if len(raw) < 28:
        raise ImageError(f"{path}: truncated MZ header")
    names = ("magic", "last_page", "pages", "nrelocs", "header_paragraphs",
             "min_alloc", "max_alloc", "ss", "sp", "checksum", "ip", "cs",
             "reloc_table", "overlay")
    header = dict(zip(names, struct.unpack_from("<14H", raw)))
    header_size = header["header_paragraphs"] * 16
    if not header["pages"] or header["last_page"] > 511:
        raise ImageError(f"{path}: invalid MZ file size")
    file_size = (header["pages"] - 1) * 512 + (header["last_page"] or 512)
    if header_size < 28 or header_size > file_size or file_size > len(raw):
        raise ImageError(f"{path}: truncated or inconsistent MZ load module")
    table = header["reloc_table"]
    if header["nrelocs"] and (table < 28 or table + 4 * header["nrelocs"] > header_size):
        raise ImageError(f"{path}: MZ relocation table lies outside the header")
    data = raw[header_size:file_size]
    relocs = []
    for i in range(header["nrelocs"]):
        off, seg = struct.unpack_from("<HH", raw, table + i * 4)
        address = seg * 16 + off
        if address + 2 > len(data):
            raise ImageError(f"{path}: relocation 0x{address:x} lies outside the load module")
        relocs.append(address)
    if len(set(relocs)) != len(relocs):
        raise ImageError(f"{path}: duplicate MZ relocation words")
    header["header_size"] = header_size
    header["file_size"] = file_size
    return LoadedImage(path, data, sorted(relocs), True,
                       header["cs"], header["ip"], header["ss"], header["sp"],
                       header["min_alloc"], header["max_alloc"], header)
