"""Embed files into the binary as C arrays: python3 tools/embed.py OUT.c NAME=PATH ...

--web-only=NAME=PATH puts a startup file in the browser's packed table only.
It neither installs nor adds the file's bytes to the native binary.
--web-lazy=NAME=PATH retains the native bytes but makes a browser metadata-only
file. --web-only-lazy does the same without a native installation. Their exact
content-derived URLs go in C; the sibling .web.json manifest lets web_files.py
publish the bytes separately, with no dependency on the page's version hash.
--native-only=NAME=PATH installs a native file without adding its bytes or name
to the browser. A separate --web-only-lazy entry may expose the same file under
the browser's H: path without creating duplicate references.

Writes the `embedded_files` table declared in runtime/rt.h. Native arrays stay
unchanged. The browser reconstructs VC from its already-linked, unrelocated
image and exact MZ header/trailer, and unpacks the explicitly eager files before
installation. Lazy references become available through the DOS open/EXEC path.

Before raw LZMA1, a reversible byte filter turns each E8/E9-following 16-bit
word into an absolute stream offset modulo 65536. This is compression, not
instruction decoding: opcode-looking data is restored identically too.
LZMA1 keeps frozen lc=lp=pb=0, a 1 MiB dictionary and an end marker.
runtime/embed_lzma.c implements that bounded profile and reverses the filter.
An Adler-32 over the original bytes catches accidental corruption; it is not
an authenticity mechanism.
"""
from pathlib import Path
from hashlib import sha256
import json
import lzma
import sys
import zlib


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from translator.image import load_image
from tools.web_modules import update


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


def x86_16_filter(data: bytes) -> bytes:
    """Normalize near relative operands; skip their bytes in both directions."""
    result = bytearray(data)
    at = 0
    while at + 2 < len(result):
        if result[at] in (0xE8, 0xE9):
            relative = result[at + 1] | (result[at + 2] << 8)
            absolute = (relative + at + 3) & 0xFFFF
            result[at + 1] = absolute & 0xFF
            result[at + 2] = absolute >> 8
            at += 3
        else:
            at += 1
    return bytes(result)


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
    lazy_files = []
    names = set()
    # Validate every input before replacing a previously generated source.
    for pair in pairs:
        web_only = pair.startswith(("--web-only=", "--web-only-lazy="))
        lazy = pair.startswith(("--web-lazy=", "--web-only-lazy="))
        native_only = pair.startswith("--native-only=")
        if web_only or lazy or native_only:
            pair = pair.split("=", 1)[1]
        name, path = pair.split("=", 1)
        if (web_only or native_only) and name in ("VC.COM", "VC.OVL"):
            raise ValueError("VC's built-in files must exist in both builds")
        if lazy and name in ("VC.COM", "VC.OVL", "VC.INI", "VC.EXT", "VCEDIT.EXT"):
            raise ValueError(f"{name}: VC's startup files must remain eager")
        if name in names or not name or name.startswith("/") or any(
                part in ("", ".", "..") for part in name.split("/")):
            raise ValueError(f"Duplicate or unsafe embedded name: {name}")
        names.add(name)
        data = Path(path).read_bytes()
        parts = vc_parts(name, path, data) if name in ("VC.COM", "VC.OVL") else None
        entries.append((name, data, parts, len(payload), web_only, lazy, native_only))
        if lazy:
            digest = sha256(data).hexdigest()
            lazy_files.append({"name": name, "source": str(Path(path).resolve()),
                               "asset": f"file.{digest[:12]}.bin", "sha256": digest,
                               "size": len(data), "checksum": zlib.adler32(data)})
        elif parts is None and not native_only:
            payload.extend(data)

    lines = ['#include "rt.h"', '#ifdef __EMSCRIPTEN__', '#include "embed_lzma.h"',
             '#include "web_files.h"',
             '#include <stdlib.h>', '#include <string.h>', '#endif', ""]
    table = []
    initialize = []
    if payload:
        filtered = x86_16_filter(payload)
        packed = lzma.compress(filtered, format=lzma.FORMAT_RAW, filters=[WEB_LZMA_FILTER])
        if lzma.decompress(packed, format=lzma.FORMAT_RAW, filters=[WEB_LZMA_FILTER]) != filtered:
            raise ValueError("embedded DOS file compression did not round-trip")
        lines += ["#ifdef __EMSCRIPTEN__",
                  f"static uint8_t embedded_unpacked[{len(payload)}];",
                  *array("embedded_packed", packed), "#endif", ""]
        initialize += [
            "  if (embed_lzma_decode(embedded_unpacked, sizeof embedded_unpacked,",
            "                        embedded_packed, sizeof embedded_packed) != 0) abort();",
            "  embed_x86_16_restore(embedded_unpacked, sizeof embedded_unpacked);",
            "  if (embed_adler32(embedded_unpacked, sizeof embedded_unpacked) !=",
            f"          {zlib.adler32(payload)}u) abort();",
        ]
    for i, (name, data, parts, offset, web_only, lazy, native_only) in enumerate(entries):
        if lazy:
            lines += ["#ifdef __EMSCRIPTEN__", f"#define f{i} NULL"]
            if not web_only:
                lines += ["#else", *array(f"f{i}", data)]
            lines.append("#endif")
        elif native_only:
            lines += ["#ifndef __EMSCRIPTEN__", *array(f"f{i}", data), "#endif"]
        elif parts is not None:
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
                      f"#define f{i} (embedded_unpacked + {offset}u)"]
            if not web_only:
                lines += ["#else", *array(f"f{i}", data)]
            lines.append("#endif")
        else:
            if web_only:
                lines.append("#ifdef __EMSCRIPTEN__")
            lines += array(f"f{i}", data)
            if web_only:
                lines.append("#endif")
        if web_only:
            table.append("#ifdef __EMSCRIPTEN__")
        elif native_only:
            table.append("#ifndef __EMSCRIPTEN__")
        table.append(f'  {{{json.dumps(name, ensure_ascii=False)}, f{i}, {len(data)}}},')
        if web_only or native_only:
            table.append("#endif")
    lines += ["", "const EmbeddedFile embedded_files[] = {", *table, "};"]
    lines.append("const int embedded_file_count = sizeof embedded_files / sizeof embedded_files[0];")
    lines += ["", "#ifdef __EMSCRIPTEN__", "WebFileReference web_file_references[] = {"]
    for entry in lazy_files:
        lines.append(f'  {{{json.dumps(entry["name"], ensure_ascii=False)}, '
                     f'"{entry["asset"]}", "{entry["sha256"]}", '
                     f'{entry["size"]}u, {entry["checksum"]}u, NULL}},')
    if not lazy_files:
        lines.append("  {0},")
    lines += ["};", f"const size_t web_file_reference_count = {len(lazy_files)}u;", "#endif"]
    lines += ["", "/* Called before installation and before any DOS EXEC byte match. */",
              "void embedded_files_init(void) {", "#ifdef __EMSCRIPTEN__",
              *initialize, "#endif", "}"]
    update(Path(out).with_suffix(".web.json"),
           (json.dumps({"format": 1, "files": lazy_files}, ensure_ascii=False, indent=2) + "\n").encode("utf-8"))
    update(Path(out), ("\n".join(lines) + "\n").encode("utf-8"))


if __name__ == "__main__":
    main()
