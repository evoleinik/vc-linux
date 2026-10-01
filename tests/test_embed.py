"""Embedded bytes are stored once/packed on the web, but remain real DOS files.

Compile both generated branches with a tiny Image fixture. That checks the
actual initialized bytes and BSS symbols, not merely the generator's text.
"""

from pathlib import Path
import ctypes
import lzma
import random
import re
import struct
import subprocess
import sys
import zlib

import pytest

from translator.image import load_image


ROOT = Path(__file__).resolve().parents[1]
LZMA_FILTER = {"id": lzma.FILTER_LZMA1, "dict_size": 1 << 20,
               "lc": 0, "lp": 0, "pb": 0, "mode": lzma.MODE_NORMAL,
               "nice_len": 273, "mf": lzma.MF_BT4}


def mz(payload: bytes, tail: bytes = b"", magic: bytes = b"MZ") -> bytes:
    header_size = 80
    file_size = header_size + len(payload)
    header = bytearray((i * 19 + 7) & 255 for i in range(header_size))
    struct.pack_into("<14H", header, 0, int.from_bytes(magic, "little"),
                     file_size % 512, (file_size + 511) // 512, 2,
                     header_size // 16, 0x31, 0xffff, 0x42, 0x1234, 0xabcd,
                     3, 0, 48, 0)
    struct.pack_into("<HH", header, 48, 2, 0)
    struct.pack_into("<HH", header, 52, (len(payload) - 2) % 16,
                     (len(payload) - 2) // 16)
    return bytes(header) + payload + tail


def generate(tmp_path, pairs, *, check=True):
    output = tmp_path / "files.c"
    command = [sys.executable, str(ROOT / "tools/embed.py"), str(output)]
    for index, (name, data) in enumerate(pairs):
        source = tmp_path / f"input-{index}.bin"
        source.write_bytes(data)
        command.append(f"{name}={source}")
    result = subprocess.run(command, capture_output=True, text=True)
    if check:
        assert result.returncode == 0, result.stderr
    return output, result


def c_bytes(data):
    return ",".join(str(byte) for byte in data) or "0"


def compile_fixture(tmp_path, source, pairs, *, web, wrong_size=False):
    com, overlay = pairs[0][1], pairs[1][1]
    header_size = struct.unpack_from("<H", overlay, 8)[0] * 16
    last, pages = struct.unpack_from("<HH", overlay, 2)
    file_size = (pages - 1) * 512 + (last or 512)
    module = overlay[header_size:file_size]
    harness = tmp_path / "fixture.c"
    harness.write_text(f'''#include "rt.h"
#include <stdio.h>
static const uint8_t com[] = {{{c_bytes(com)}}};
static const uint8_t ovl[] = {{{c_bytes(module)}}};
const Image image_vc_com = {{.bytes = com, .size = sizeof com}};
const Image image_vc_ovl = {{.is_exe = 1, .bytes = ovl,
    .size = sizeof ovl {"- 1" if wrong_size else ""}}};
void embedded_files_init(void);
int main(void) {{
    embedded_files_init();
    embedded_files_init();
    for (int i = 0; i < embedded_file_count; ++i)
        if (fwrite(embedded_files[i].data, 1, embedded_files[i].size, stdout)
                != embedded_files[i].size) return 2;
    return 0;
}}
''')
    flags = ["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "runtime")]
    if web:
        flags.append("-D__EMSCRIPTEN__")
    obj = tmp_path / "files.o"
    exe = tmp_path / "embedded"
    subprocess.run([*flags, "-c", str(source), "-o", str(obj)], check=True, capture_output=True)
    decoder = [str(ROOT / "runtime/embed_lzma.c")] if web else []
    subprocess.run([*flags, str(obj), str(harness), *decoder, "-o", str(exe)],
                   check=True, capture_output=True)
    return obj, subprocess.run([str(exe)], capture_output=True)


@pytest.mark.parametrize("web", [False, True], ids=["native", "web"])
@pytest.mark.parametrize("magic", [b"MZ", b"ZM"])
@pytest.mark.parametrize("module_size", [1137, 1024 - 80], ids=["partial-page", "full-page"])
def test_complete_files_and_single_web_payload(tmp_path, web, magic, module_size):
    com = b"\xeb\0\xc3" + bytes((i * 37 + 11) & 255 for i in range(513))
    module = bytes((i * 71 + 19) & 255 for i in range(module_size))
    overlay = mz(module, b"\0trailing overlay/debug records\xff", magic)
    pairs = [("VC.COM", com), ("VC.OVL", overlay), ("EMPTY.TXT", b""),
             ("OTHER.EXE", b"MZopaque non-VC bytes\0\xff")]
    source, _ = generate(tmp_path, pairs)
    obj, result = compile_fixture(tmp_path, source, pairs, web=web)
    assert result.returncode == 0, result.stderr
    assert result.stdout == b"".join(data for _, data in pairs)
    symbols = subprocess.check_output(["nm", "-S", "--defined-only", str(obj)], text=True)
    for index, size in [(0, len(com)), (1, len(overlay))]:
        match = re.search(rf"^[0-9a-f]+ ([0-9a-f]+) (\w) f{index}$", symbols, re.M)
        assert match, symbols
        assert int(match[1], 16) == size
        assert match[2].lower() == ("b" if web else "r"), (
            "web VC raw files must live in zero BSS, not duplicate downloaded data", symbols)


def test_image_size_mismatch_is_refused_before_copy(tmp_path):
    pairs = [("VC.COM", b"\xc3"), ("VC.OVL", mz(bytes(range(200))))]
    source, _ = generate(tmp_path, pairs)
    _, result = compile_fixture(tmp_path, source, pairs, web=True, wrong_size=True)
    assert result.returncode != 0


@pytest.mark.parametrize("name,data,message", [
    ("VC.COM", mz(bytes(range(200))), "VC.COM must be a COM"),
    ("VC.OVL", b"not an executable", "VC.OVL must be an MZ"),
    ("VC.OVL", b"MZtruncated", "truncated MZ header"),
], ids=["com-is-exe", "ovl-not-exe", "truncated-mz"])
def test_incompatible_source_is_rejected_without_overwriting(tmp_path, name, data, message):
    output = tmp_path / "files.c"
    output.write_text("keep previous generated file\n")
    _, result = generate(tmp_path, [(name, data)], check=False)
    assert result.returncode != 0
    assert message in result.stderr
    assert output.read_text() == "keep previous generated file\n"


@pytest.mark.parametrize("corruption", ["size", "header", "relocation", "duplicate"])
def test_malformed_mz_is_rejected_at_build_time(tmp_path, corruption):
    data = bytearray(mz(bytes(range(200))))
    if corruption == "size":
        struct.pack_into("<H", data, 4, 0)
    elif corruption == "header":
        struct.pack_into("<H", data, 8, 0xffff)
    elif corruption == "relocation":
        struct.pack_into("<HH", data, 48, 0xffff, 0xffff)
    else:
        data[52:56] = data[48:52]
    _, result = generate(tmp_path, [("VC.OVL", data)], check=False)
    assert result.returncode != 0
    assert "MZ" in result.stderr or "relocation" in result.stderr


def test_real_vc_prefix_and_tail_match_translator(tmp_path):
    pairs = [(name, (ROOT / "build" / name).read_bytes()) for name in ("VC.COM", "VC.OVL")]
    source, _ = generate(tmp_path, pairs)
    for index, name in enumerate(("VC.COM", "VC.OVL")):
        loaded = load_image(ROOT / "build" / name)
        assert loaded.data
        assert len(loaded.data) <= len(pairs[index][1])
    _, result = compile_fixture(tmp_path, source, pairs, web=True)
    assert result.returncode == 0
    assert result.stdout == b"".join(data for _, data in pairs)


@pytest.fixture(scope="module")
def decoder(tmp_path_factory):
    """Exercise the actual C decoder against Python's independent encoder."""
    shared = tmp_path_factory.mktemp("embed-lzma") / "decoder.so"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                    "-O2", "-fPIC", "-shared", str(ROOT / "runtime/embed_lzma.c"),
                    "-o", str(shared)], check=True, capture_output=True)
    library = ctypes.CDLL(str(shared))
    decode = library.embed_lzma_decode
    decode.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    decode.restype = ctypes.c_int
    checksum = library.embed_adler32
    checksum.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    checksum.restype = ctypes.c_uint32

    def run(packed, size):
        output = ctypes.create_string_buffer(b"\xa5" * (size + 16), size + 16)
        result = decode(output, size, packed, len(packed))
        assert output.raw[size:] == b"\xa5" * 16, "decoder wrote beyond the output bound"
        value = output.raw[:size]
        assert checksum(output, size) == zlib.adler32(value)
        return result, value

    return run


def encode(data, **properties):
    return lzma.compress(data, format=lzma.FORMAT_RAW,
                         filters=[{**LZMA_FILTER, **properties}])


@pytest.mark.parametrize("kind", ["empty", "one", "alphabet", "random-65535",
                                  "random-65536", "random-65537", "overlap",
                                  "opcode-looking", "mixed", "dictionary-boundary"])
def test_lzma_fixed_profile_exact_bytes_and_checksum(decoder, kind):
    rng = random.Random(20261002)
    if kind == "empty":
        data = b""
    elif kind == "one":
        data = b"\xff"
    elif kind == "alphabet":
        data = bytes(range(256)) * 4096
    elif kind.startswith("random-"):
        data = rng.randbytes(int(kind.split("-")[1]))
    elif kind == "overlap":
        data = b"x" * 1_000_000 + b"y" * 1_000_000
    elif kind == "opcode-looking":
        data = b"\xe8\xff\xff\xe9\0\0\xe8\0\x80\xe9\xfe\xff" * 16384 + b"\xe8\xe9"
    elif kind == "dictionary-boundary":
        block = rng.randbytes((1 << 20) + 1)
        data = block + block[:65536] + block[-65536:]
    else:
        blocks = [rng.randbytes(997 + n) for n in range(5)]
        data = b"".join(blocks[n % 5] + blocks[(n // 2) % 5] for n in range(200))
    packed = encode(data)
    assert lzma.decompress(packed, format=lzma.FORMAT_RAW, filters=[LZMA_FILTER]) == data
    result, output = decoder(packed, len(data))
    assert result == 0
    assert output == data


def test_lzma_rejects_truncation_trailing_data_and_wrong_output_bound(decoder):
    data = bytes(range(256)) * 20 + b"\xff" * 10000
    packed = encode(data)
    for size in range(len(packed)):
        assert decoder(packed[:size], len(data))[0] != 0, size
    for tail in (b"\0", b"\xff", packed):
        assert decoder(packed + tail, len(data))[0] != 0
    for size in (0, 1, len(data) - 1, len(data) + 1):
        assert decoder(packed, size)[0] != 0
    assert decoder(b"\x01" + packed[1:], len(data))[0] != 0


def test_lzma_corruption_never_publishes_wrong_bytes_with_the_expected_checksum(decoder):
    rng = random.Random(20261002)
    data = rng.randbytes(65537) + bytes(range(256)) * 200
    packed = encode(data)
    expected = zlib.adler32(data)
    for _ in range(128):
        corrupt = bytearray(packed)
        corrupt[rng.randrange(len(corrupt))] ^= 1 << rng.randrange(8)
        result, output = decoder(bytes(corrupt), len(data))
        assert result != 0 or zlib.adler32(output) != expected
    for _ in range(512):
        size = rng.randrange(256)
        result, output = decoder(b"\0" + rng.randbytes(size), rng.randrange(2048))
        assert result != 0


def test_lzma_dictionary_limit_and_profile_are_not_silently_relaxed(decoder):
    rng = random.Random(20261002)
    block = rng.randbytes((1 << 20) + 512)
    data = block + block[:65536]
    assert decoder(encode(data, dict_size=1 << 21), len(data))[0] != 0
    data = bytes(range(256)) * 257
    result, output = decoder(encode(data, lc=3, pb=2), len(data))
    assert result != 0 or output != data


@pytest.mark.parametrize("size", [0, 1, 5551, 5552, 5553, 11104, 65535, 65536])
def test_adler32_worst_case_chunk_boundaries(decoder, size):
    data = b"\xff" * size
    result, output = decoder(encode(data), size)
    assert result == 0 and output == data


@pytest.mark.parametrize("dictionary", [4096, 65536, 1 << 20])
def test_lzma_accepts_fast_encoder_and_smaller_dictionaries(decoder, dictionary):
    data = bytes(range(256)) * 1000 + random.Random(20261002).randbytes(65537)
    result, output = decoder(encode(data, mode=lzma.MODE_FAST, dict_size=dictionary), len(data))
    assert result == 0 and output == data


@pytest.mark.parametrize("corruption", ["stream", "checksum"])
@pytest.mark.parametrize("web", [False, True], ids=["native-unaffected", "web-refused"])
def test_generated_initializer_refuses_corrupt_packed_files(tmp_path, corruption, web):
    payload = bytes(range(256)) * 100 + b"\xe8\xff\xff\xe9\0\0"
    pairs = [("VC.COM", b"\xc3"), ("VC.OVL", mz(bytes(range(200)))),
             ("PAYLOAD.EXE", payload), ("EMPTY.TXT", b"")]
    source, _ = generate(tmp_path, pairs)
    text = source.read_text()
    if corruption == "stream":
        text, count = re.subn(r"(static const uint8_t embedded_packed\[\d+\] = \{\s*)0,",
                             r"\g<1>1,", text, count=1)
        assert count == 1
    else:
        expected = zlib.adler32(payload)
        assert f"{expected}u) abort();" in text
        text = text.replace(f"{expected}u) abort();", f"{expected ^ 1}u) abort();")
    source.write_text(text)
    _, result = compile_fixture(tmp_path, source, pairs, web=web)
    if web:
        assert result.returncode != 0
        assert result.stdout == b"", "corrupt files must not be published"
    else:
        assert result.returncode == 0
        assert result.stdout == b"".join(data for _, data in pairs)


@pytest.mark.parametrize("web", [False, True], ids=["native", "web"])
def test_every_real_embedded_file_round_trips_byte_for_byte(tmp_path, web):
    paths = [
        ("VC.COM", "build/VC.COM"), ("VC.OVL", "build/VC.OVL"),
        ("GWBASIC.EXE", "build/gwbasic/GWBASIC.EXE"),
        ("BOOTLOGO.COM", "build/bootlogo/LOGO.COM"),
        ("ROGUE.EXE", "build/rogue/ROGUE.EXE"),
        ("ROGUELIC.TXT", "third_party/rogue/LICENSE.TXT"),
        ("PDCLIC.TXT", "third_party/pdcurses/README.md"),
        ("OWLIC.TXT", "build/rogue/OWLIC.TXT"),
        ("VZ.COM", "build/vz/VZ.COM"), ("VZ.DEF", "build/vz/VZ.DEF"),
        *((name, f"third_party/vzeditor/VZ-IBM/{name}")
          for name in ("VZFLE.DEF", "HELPE.DEF", "BLOCK.DEF", "PALET.DEF", "BW.DEF")),
        ("VZLIC.TXT", "third_party/vzeditor/LICENSE"),
        ("KERMIT.EXE", "build/kermit/KERMIT.EXE"), ("BBS.TAK", "data/BBS.TAK"),
        ("KERMIT.TXT", "data/KERMIT.TXT"), ("KERMLIC.TXT", "third_party/mskermit/LICENSE"),
        *((name, f"data/{name}") for name in ("VC.INI", "VC.EXT", "VCEDIT.EXT", "VC.HLP")),
    ]
    pairs = [(name, (ROOT / path).read_bytes()) for name, path in paths]
    source, _ = generate(tmp_path, pairs)
    if web:
        text = source.read_text()
        match = re.search(r"static const uint8_t embedded_packed\[\d+\] = \{([\s\S]*?)\n\};", text)
        assert match
        packed = bytes(map(int, re.findall(r"\d+", match[1])))
        assert lzma.decompress(packed, format=lzma.FORMAT_RAW, filters=[LZMA_FILTER]) == b"".join(
            data for name, data in pairs if name not in ("VC.COM", "VC.OVL"))
    _, result = compile_fixture(tmp_path, source, pairs, web=web)
    assert result.returncode == 0, result.stderr
    assert result.stdout == b"".join(data for _, data in pairs)
