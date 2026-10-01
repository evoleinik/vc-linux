"""Embed files into the binary as C arrays: python3 tools/embed.py OUT.c NAME=PATH ...

Writes the `embedded_files` table declared in runtime/rt.h. Native arrays stay
unchanged. The browser reconstructs VC from its already-linked, unrelocated
image and exact MZ header/trailer, and unpacks the other files before any DOS
installation/EXEC comparison. Nothing is removed, deferred, or network-fetched.

The packed stream is raw LZMA1 with frozen lc=lp=pb=0, a 1 MiB dictionary and
an end marker. runtime/embed_lzma.c implements only that bounded profile. An
Adler-32 catches accidental corruption; it is not an authenticity mechanism.
"""
from pathlib import Path
import lzma
import sys
import zlib


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from translator.image import load_image


WEB_LZMA_FILTER = {
    "id": lzma.FILTER_LZMA1, "dict_size": 1 << 20,
    "lc": 0, "lp": 0, "pb": 0, "mode": lzma.MODE_NORMAL,
    "nice_len": 273, "mf": lzma.MF_BT4,
}


def array(symbol: str, data: bytes) -> list[str]:
    lines = [f"static const uint8_t {symbol}[{max(len(data), 1)}] = {{"]
    for j in range(0, len(data), 20):
        lines.append("  " + ",".join(str(b) for b in data[j:j + 20]) + ",")
    if not data:
        lines.append("  0,")
    return [*lines, "};"]


def vc_parts(name: str, path: str, raw: bytes) -> tuple[str, bytes, bytes, int]:
    """Use the translator's validated MZ boundaries, including trailing data.

    Never infer the load module as 'everything after the header': an EXE may
    have overlay/debug bytes past its declared final page. Keep those too.
    """
    image = load_image(path)
    if name == "VC.COM" and image.is_exe:
        raise ValueError("VC.COM must be a COM load module for browser deduplication")
    if name == "VC.OVL" and not image.is_exe:
        raise ValueError("VC.OVL must be an MZ load module for browser deduplication")
    start = image.header["header_size"] if image.is_exe else 0
    end = image.header["file_size"] if image.is_exe else len(raw)
    prefix, suffix = raw[:start], raw[end:]
    if not image.data or prefix + image.data + suffix != raw:
        raise ValueError(f"{name}: input changed or contains an empty load module")
    return ("image_vc_ovl" if image.is_exe else "image_vc_com",
            prefix, suffix, len(image.data))


def main() -> None:
    out, pairs = sys.argv[1], sys.argv[2:]
    entries = []
    payload = bytearray()
    # Validate every input before replacing a previously generated source.
    for pair in pairs:
        name, path = pair.split("=", 1)
        data = Path(path).read_bytes()
        parts = vc_parts(name, path, data) if name in ("VC.COM", "VC.OVL") else None
        entries.append((name, data, parts, len(payload)))
        if parts is None:
            payload.extend(data)

    lines = ['#include "rt.h"', '#ifdef __EMSCRIPTEN__', '#include "embed_lzma.h"',
             '#include <stdlib.h>', '#include <string.h>', '#endif', ""]
    table = []
    initialize = []
    if payload:
        packed = lzma.compress(payload, format=lzma.FORMAT_RAW, filters=[WEB_LZMA_FILTER])
        if lzma.decompress(packed, format=lzma.FORMAT_RAW, filters=[WEB_LZMA_FILTER]) != payload:
            raise ValueError("embedded DOS file compression did not round-trip")
        lines += ["#ifdef __EMSCRIPTEN__",
                  f"static uint8_t embedded_unpacked[{len(payload)}];",
                  *array("embedded_packed", packed), "#endif", ""]
        initialize += [
            "  if (embed_lzma_decode(embedded_unpacked, sizeof embedded_unpacked,",
            "                        embedded_packed, sizeof embedded_packed) != 0 ||",
            "      embed_adler32(embedded_unpacked, sizeof embedded_unpacked) !=",
            f"          {zlib.adler32(payload)}u) abort();",
        ]
    for i, (name, data, parts, offset) in enumerate(entries):
        if parts is not None:
            image, prefix, suffix, size = parts
            lines += ["#ifdef __EMSCRIPTEN__", f"static uint8_t f{i}[{len(data)}];"]
            if prefix:
                lines += array(f"f{i}_prefix", prefix)
            if suffix:
                lines += array(f"f{i}_suffix", suffix)
            lines.append("#else")
            # Native builds retain exactly their ordinary const file arrays.
            lines += array(f"f{i}", data)
            lines.append("#endif")
            initialize.append(f"  if ({image}.size != {size}u || "
                              f"{image}.is_exe != {int(name == 'VC.OVL')}) abort();")
            if prefix:
                initialize.append(f"  memcpy(f{i}, f{i}_prefix, {len(prefix)}u);")
            initialize.append(f"  memcpy(f{i} + {len(prefix)}u, {image}.bytes, {size}u);")
            if suffix:
                initialize.append(f"  memcpy(f{i} + {len(prefix) + size}u, "
                                  f"f{i}_suffix, {len(suffix)}u);")
        elif payload:
            lines += ["#ifdef __EMSCRIPTEN__",
                      f"#define f{i} (embedded_unpacked + {offset}u)", "#else",
                      *array(f"f{i}", data), "#endif"]
        else:
            lines += array(f"f{i}", data)
        table.append(f'  {{"{name}", f{i}, {len(data)}}},')
    lines += ["", "const EmbeddedFile embedded_files[] = {", *table, "};"]
    lines.append(f"const int embedded_file_count = {len(pairs)};")
    lines += ["", "/* Called before installation and before any DOS EXEC byte match. */",
              "void embedded_files_init(void) {", "#ifdef __EMSCRIPTEN__",
              *initialize, "#endif", "}"]
    Path(out).write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
