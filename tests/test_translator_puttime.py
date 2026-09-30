"""Gate 3: generated VC.OVL PutTime/BinDec against original linked machine code.

The two exhaustive cases cover every 16-bit DOS time in both formats.  The
smaller matrix additionally moves the image, aliases CS, uses other separators,
and exercises DF=1.  No code or expected output is taken from the hand-written
spike: it remains an unchanged reference for the routine's calling convention.
"""

from __future__ import annotations

import ctypes
from pathlib import Path
import struct
import subprocess
import sys

import pytest
import unicorn
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_OP_REG, X86_REG_AL, X86_REG_ES


ROOT = Path(__file__).resolve().parents[1]
# pytest's console entry point need not place the project root on sys.path.
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))


class PutTimeConfig(ctypes.Structure):
    _fields_ = [
        (name, ctypes.c_uint32)
        for name in (
            "puttime_off", "bindec_off", "subs_base", "format_off", "separator_off",
            "loadseg", "cs_delta", "format", "exhaustive", "reverse",
        )
    ]


def original_mz_at(loadseg: int) -> bytes:
    """Read/relocate the reference without using translator.image or C metadata."""
    image = (ROOT / "build/VC.OVL").read_bytes()
    assert image[:2] == b"MZ"
    last_size, pages, nrelocs, header_paras = struct.unpack_from("<4H", image, 2)
    declared_size = (pages - 1) * 512 + (last_size or 512)
    assert declared_size == len(image), "VC.OVL contains missing or trailing bytes"
    header_size = header_paras * 16
    module = bytearray(image[header_size:declared_size])
    reloc_table = struct.unpack_from("<H", image, 24)[0]
    assert reloc_table + nrelocs * 4 <= header_size
    for index in range(nrelocs):
        offset, segment = struct.unpack_from("<HH", image, reloc_table + index * 4)
        address = segment * 16 + offset
        assert address + 2 <= len(module)
        value = struct.unpack_from("<H", module, address)[0]
        struct.pack_into("<H", module, address, (value + loadseg) & 0xFFFF)
    return bytes(module)


@pytest.fixture(scope="module")
def generated_puttime(tmp_path_factory):
    """Compile the actual complete generated image, not per-test translations."""
    generated = subprocess.run(
        ["make", "gen"], cwd=ROOT, capture_output=True, text=True, timeout=300,
    )
    assert generated.returncode == 0, generated.stdout + generated.stderr

    from translator.image import load_image
    from translator.layout import build_layout
    from translator.listing import parse_listing

    layout = build_layout(
        load_image(ROOT / "build/VC.OVL"),
        parse_listing(ROOT / "build/gen/VC.OVL.lst"),
    )
    puttime = layout.procedures["PutTime"]
    bindec = layout.procedures["BinDec"]
    chunk = next(c for c in layout.chunks if any(i.off == puttime for i in c.instructions))
    instructions = [i.insn for i in chunk.instructions]

    # Country is a member of a runtime-allocated STRUC, not an image data label.
    # Discover its two actual ES-relative displacements from the linked code.
    def direct_es(operand):
        return (
            operand.type == X86_OP_MEM
            and operand.mem.segment == X86_REG_ES
            and operand.mem.base == 0
            and operand.mem.index == 0
        )

    format_offsets = {
        i.operands[0].mem.disp & 0xFFFF for i in instructions
        if i.mnemonic == "cmp" and len(i.operands) == 2
        and direct_es(i.operands[0])
        and i.operands[1].type == X86_OP_IMM and i.operands[1].imm == 0
    }
    separator_offsets = {
        i.operands[1].mem.disp & 0xFFFF for i in instructions
        if i.mnemonic == "mov" and len(i.operands) == 2
        and i.operands[0].type == X86_OP_REG and i.operands[0].reg == X86_REG_AL
        and direct_es(i.operands[1])
    }
    assert len(format_offsets) == len(separator_offsets) == 1
    format_off = format_offsets.pop()
    separator_off = separator_offsets.pop()
    assert format_off - separator_off == 4, "CNTRY.TimeFmt/TimeSep layout changed"
    bases = {name.casefold(): base for name, base in layout.segment_bases.items()}
    subs_base = bases["subs"]

    package = Path(unicorn.__file__).resolve().parent
    unicorn_library = package / "lib/libunicorn.so.2"
    assert unicorn_library.is_file(), "The preinstalled Unicorn C library is required"
    build_dir = tmp_path_factory.mktemp("translator_puttime")
    library = build_dir / "puttime.so"
    command = [
        "gcc", "-O1", "-Wall", "-Wno-unused-label", "-fPIC", "-shared",
        "-DCPU_TRACE_WRITES", "-I", str(ROOT / "runtime"), "-I", str(ROOT),
        "-I", str(package / "include"),
        str(ROOT / "tests/translator_support/puttime_driver.c"),
        str(ROOT / "runtime/cpu.c"), str(ROOT / "build/gen/vc_ovl.c"),
        str(unicorn_library), f"-Wl,-rpath,{unicorn_library.parent}",
        "-o", str(library),
    ]
    compiled = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=300)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    lib = ctypes.CDLL(str(library))
    lib.puttime_check.argtypes = [
        ctypes.POINTER(ctypes.c_uint8), ctypes.c_uint32,
        ctypes.POINTER(PutTimeConfig), ctypes.c_char_p, ctypes.c_uint32,
    ]
    lib.puttime_check.restype = ctypes.c_int
    return lib, (puttime, bindec, subs_base, format_off, separator_off)


def compare_cases(generated_puttime, *, time_format, exhaustive, reverse=0):
    lib, offsets = generated_puttime
    loadseg = 0x1800 if exhaustive else 0x2300
    config = PutTimeConfig(
        *offsets, loadseg, 0 if exhaustive else 0x37, time_format, exhaustive, reverse,
    )
    reference = original_mz_at(loadseg)
    reference_buffer = (ctypes.c_uint8 * len(reference)).from_buffer_copy(reference)
    message = ctypes.create_string_buffer(2048)
    count = lib.puttime_check(
        reference_buffer, len(reference), ctypes.byref(config), message, len(message),
    )
    assert count == (65536 if exhaustive else 256), message.value.decode()
    print(message.value.decode())


@pytest.mark.parametrize("time_format", [0, 1], ids=["12-hour", "24-hour"])
def test_puttime_every_dos_time(generated_puttime, time_format):
    compare_cases(generated_puttime, time_format=time_format, exhaustive=1)


@pytest.mark.parametrize("time_format", [0, 1], ids=["12-hour", "24-hour"])
@pytest.mark.parametrize("reverse", [0, 1], ids=["DF-clear", "DF-set"])
def test_puttime_relocated_cs_aliases(generated_puttime, time_format, reverse):
    compare_cases(
        generated_puttime, time_format=time_format, exhaustive=0, reverse=reverse,
    )
