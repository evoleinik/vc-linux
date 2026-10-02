"""Reproducible Hack bytes and DOS-specific behavior in the real 8086 image."""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

import pytest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE, UC_HOOK_INTR
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_EFLAGS,
)

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/hack"
EXE_SHA256 = "b6f6ca8667fc8d1e37eb81fbd1c469a371312e4a39c53052985d553aaac345ee"
EXE_SIZE = 243478
LOAD = 0x1000
STOP = 0xF0100


def load_port():
    spec = importlib.util.spec_from_file_location("hack_port", ROOT / "tools/hack_port.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_builder(monkeypatch):
    monkeypatch.syspath_prepend(str(ROOT / "tools"))
    spec = importlib.util.spec_from_file_location("build_hack", ROOT / "tools/build_hack.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def hack_files():
    subprocess.run(["make", "hack"], cwd=ROOT, check=True, capture_output=True)
    return BUILD / "HACK.EXE", BUILD / "HACK.MAP"


def vendor_digest():
    paths = sorted(path for path in (ROOT / "third_party/hack").iterdir()
                   if path.name != "UPSTREAM")
    assert len(paths) == 92 and all(path.is_file() for path in paths)
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode() + b"\0" + hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def test_hack_vendor_is_exact_92_file_snapshot():
    assert vendor_digest() == "51e05f313639e8bd784496ed4c37fe8ff888f08679ca6dc10d56cbe37fde11db"
    manifest = (ROOT / "runtime/hack_dos/UPSTREAM.sha256").read_text().splitlines()
    assert len(manifest) == 92
    for line in manifest:
        digest, name = line.split("  ", 1)
        assert hashlib.sha256((ROOT / "third_party/hack" / name).read_bytes()).hexdigest() == digest
    upstream = (ROOT / "third_party/hack/UPSTREAM").read_text()
    assert "https://github.com/NetBSD/src" in upstream
    assert "games/hack" in upstream
    assert "f037b5fcaa6db302271bbb445039a3a513b2e28c" in upstream
    assert "2026-08-14" in upstream


@pytest.mark.parametrize("state", ("unset", "missing"))
def test_hack_build_missing_watcom_explains_install(tmp_path, state):
    env = os.environ.copy()
    env.pop("WATCOM", None)
    if state == "missing":
        env["WATCOM"] = str(tmp_path / "missing-watcom")
    result = subprocess.run([sys.executable, "tools/build_hack.py", "--out", str(tmp_path / "out")],
                            cwd=ROOT, env=env, capture_output=True, text=True, timeout=10)
    assert result.returncode
    assert ("tools/fetch-openwatcom.sh build/openwatcom && "
            "export WATCOM=$PWD/build/openwatcom") in result.stderr
    assert not (tmp_path / "out").exists()


def identity_fixture():
    image = bytearray(128)
    struct.pack_into("<14H", image, 0, 0x5a4d, 128, 1, 0, 4, 0, 0, 0, 0, 0, 0, 0, 28, 0)
    image[96:100] = b"HSTP"
    return image, "0002:0000      _hack_build_stamp\n"


def test_hack_build_identity_is_a_digest_of_all_linked_bytes(monkeypatch):
    builder = load_builder(monkeypatch)
    image, map_text = identity_fixture()
    at, stamp = builder.build_identity(image, map_text)
    assert at == 96
    assert stamp == int.from_bytes(hashlib.sha256(image).digest()[:4], "little")
    image[-1] ^= 1
    assert builder.build_identity(image, map_text)[1] != stamp


@pytest.mark.parametrize("defect", ("MZ", "missing", "duplicate", "placeholder", "table", "fixup"))
def test_hack_build_identity_rejects_unsafe_stamp_slots(monkeypatch, defect):
    builder = load_builder(monkeypatch)
    image, map_text = identity_fixture()
    if defect == "MZ": image[0] = 0
    if defect == "missing": map_text = ""
    if defect == "duplicate": map_text *= 2
    if defect == "placeholder": image[96] ^= 1
    if defect == "table": struct.pack_into("<H", image, 6, 10)
    if defect == "fixup":
        struct.pack_into("<H", image, 6, 1)
        struct.pack_into("<HH", image, 28, 1, 2)
    with pytest.raises(ValueError, match="Hack build identity"):
        builder.build_identity(image, map_text)


def verified_image(raw):
    assert len(raw) == EXE_SIZE, "verified HACK.EXE size mismatch"
    assert hashlib.sha256(raw).hexdigest() == EXE_SHA256, "verified HACK.EXE SHA-256 mismatch"


def test_hack_build_is_byte_reproducible(hack_files, tmp_path):
    before = hack_files[0].read_bytes()
    vendor = vendor_digest()
    (tmp_path / "src").mkdir()
    (tmp_path / "src/stale.c").write_text("#error stale file must not be compiled\n")
    env = os.environ | {"WATCOM": os.environ.get("WATCOM", str(ROOT / "build/openwatcom"))}
    subprocess.run([sys.executable, "tools/build_hack.py", "--out", str(tmp_path)],
                   cwd=ROOT, env=env, capture_output=True, check=True)
    assert (tmp_path / "HACK.EXE").read_bytes() == before
    verified_image(before)
    assert vendor_digest() == vendor
    for name in ("data", "help", "hh", "rumors", "COPYRIGHT", "COPYRIGHT-JF"):
        assert (tmp_path / name).read_bytes() == (ROOT / "third_party/hack" / name).read_bytes()
    for name in ("record", "perm"):
        assert (tmp_path / name).read_bytes() == b""
    assert b"OpenWatcom runtime source" in (tmp_path / "OWLIC.TXT").read_bytes()
    assert b"The NetBSD Foundation, Inc." in (tmp_path / "OWLIC.TXT").read_bytes()
    for name in load_port().REPLACED:
        assert not (tmp_path / "obj" / (Path(name).stem + ".obj")).exists()


def test_hack_reproducibility_rejects_unverified_bytes(hack_files):
    raw = bytearray(hack_files[0].read_bytes())
    # Plant a deterministic same-size defect in the otherwise valid image.
    raw[-1] ^= 1
    with pytest.raises(AssertionError, match="HACK.EXE SHA-256"):
        verified_image(raw)


@pytest.mark.parametrize("changed", ("missing", "duplicate", "clock"))
def test_hack_port_refuses_changed_upstream(tmp_path, changed):
    source = tmp_path / "vendor"
    shutil.copytree(ROOT / "third_party/hack", source)
    if changed == "missing":
        (source / "hack.main.c").unlink()
        match = "missing adapted Hack"
    elif changed == "duplicate":
        path = source / "hack.main.c"
        path.write_text(path.read_text() + '\n/* main(int argc, char *argv[]) */\n')
        match = "expected 1 instances"
    else:
        path = source / "date.h"
        path.write_text(path.read_text() + '\nconst char *clock = __TIME__;\n')
        match = "unpinned compiler clock"
    with pytest.raises(ValueError, match=match):
        load_port().prepare(source, tmp_path / "out")


class DosRoutine:
    """Execute actual Watcom routines, stubbing only I/O and stack probing."""

    def __init__(self, files):
        raw = files[0].read_bytes()
        header = struct.unpack_from("<14H", raw)
        assert raw[:2] == b"MZ"
        body = bytearray(raw[header[4] * 16:])
        for index in range(header[3]):
            offset, segment = struct.unpack_from("<HH", raw, header[12] + 4 * index)
            at = segment * 16 + offset
            value, = struct.unpack_from("<H", body, at)
            struct.pack_into("<H", body, at, (value + LOAD) & 0xffff)
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(LOAD * 16, bytes(body))
        text = files[1].read_text()
        self.symbols = {
            name: (LOAD + int(segment, 16), int(offset, 16))
            for segment, offset, name in re.findall(
                r"^([0-9a-f]{4}):([0-9a-f]{4})[+* ]+\s+(\S+)\s*$", text, re.M | re.I)
        }
        match = re.search(r"^DGROUP\s+([0-9a-f]+):([0-9a-f]+)", text, re.M | re.I)
        self.ds = LOAD + int(match[1], 16)
        self.stack = (header[7] - (self.ds - LOAD)) * 16 + header[8] - 16
        self.returned = False
        self.stubs = {}
        self.written = []
        self.incoming = b""
        self.file = bytearray()
        self.position = 0
        self.stub("__STK")
        self.uc.hook_add(UC_HOOK_CODE, self.hook)

    def address(self, name):
        segment, offset = self.symbols[name]
        return segment * 16 + offset

    def stub(self, name, callback=None):
        address = self.address(name)
        self.uc.mem_write(address, b"\xcb")
        self.stubs[address] = callback

    def hook(self, uc, address, size, opaque):
        if address == STOP:
            self.returned = True
            uc.emu_stop()
        elif address in self.stubs and self.stubs[address]:
            self.stubs[address](self)

    def call(self, name, *, ax=0, bx=0, cx=0, dx=0):
        self.returned = False
        self.uc.mem_write(self.ds * 16 + self.stack - 4096, b"\xa5" * 4096)
        for register, value in {
            UC_X86_REG_AX: ax, UC_X86_REG_BX: bx, UC_X86_REG_CX: cx, UC_X86_REG_DX: dx,
            UC_X86_REG_SI: 0x3210, UC_X86_REG_DI: 0x4567, UC_X86_REG_BP: 0x7654,
            UC_X86_REG_DS: self.ds, UC_X86_REG_SS: self.ds, UC_X86_REG_ES: self.ds,
            UC_X86_REG_SP: self.stack - 4, UC_X86_REG_EFLAGS: 2,
        }.items():
            self.uc.reg_write(register, value & 0xffff)
        self.uc.mem_write(self.ds * 16 + self.stack - 4, struct.pack("<HH", 0x100, 0xf000))
        self.uc.reg_write(UC_X86_REG_CS, self.symbols[name][0])
        self.uc.emu_start(self.address(name), 0, count=2000000)
        assert self.returned, f"{name} did not return"
        assert self.uc.reg_read(UC_X86_REG_SP) == self.stack
        assert self.uc.reg_read(UC_X86_REG_BP) == 0x7654
        return self.uc.reg_read(UC_X86_REG_AX)

    def pointer(self):
        return self.uc.reg_read(UC_X86_REG_CX) * 16 + self.uc.reg_read(UC_X86_REG_BX)

    def streams(self):
        def write(machine):
            count = machine.uc.reg_read(UC_X86_REG_DX)
            machine.written.append(bytes(machine.uc.mem_read(machine.pointer(), count)))

        def read(machine):
            count = machine.uc.reg_read(UC_X86_REG_DX)
            assert len(machine.incoming) >= count
            machine.uc.mem_write(machine.pointer(), machine.incoming[:count])
            machine.incoming = machine.incoming[count:]

        self.stub("bwrite_", write)
        self.stub("mread_", read)

    def file_io(self):
        def length(machine):
            machine.uc.reg_write(UC_X86_REG_AX, len(machine.file) & 0xffff)
            machine.uc.reg_write(UC_X86_REG_DX, len(machine.file) >> 16)

        def seek(machine):
            offset = machine.uc.reg_read(UC_X86_REG_BX) | machine.uc.reg_read(UC_X86_REG_CX) << 16
            origin = machine.uc.reg_read(UC_X86_REG_DX)
            assert origin == 0
            machine.position = offset
            machine.uc.reg_write(UC_X86_REG_AX, offset & 0xffff)
            machine.uc.reg_write(UC_X86_REG_DX, offset >> 16)

        def read(machine):
            count = machine.uc.reg_read(UC_X86_REG_DX)
            data = machine.file[machine.position:machine.position + count]
            if data:
                machine.uc.mem_write(machine.pointer(), bytes(data))
            machine.position += len(data)
            machine.uc.reg_write(UC_X86_REG_AX, len(data))

        def write(machine):
            count = machine.uc.reg_read(UC_X86_REG_DX)
            data = machine.uc.mem_read(machine.pointer(), count)
            machine.file[machine.position:machine.position + count] = data
            machine.position += count
            machine.uc.reg_write(UC_X86_REG_AX, count)

        for name, callback in (("filelength_", length), ("lseek_", seek), ("read_", read), ("write_", write)):
            self.stub(name, callback)


def test_hack_dos_random_uses_full_bsd_sequence(hack_files):
    machine = DosRoutine(hack_files)
    machine.call("srandom_", ax=1)
    values = []
    for _ in range(5):
        low = machine.call("random_")
        values.append(low | machine.uc.reg_read(UC_X86_REG_DX) << 16)
    assert values == [1804289383, 846930886, 1681692777, 1714636915, 1957747793]


@pytest.mark.parametrize("kind,char,expected", ((1, "-", 196), (2, "|", 179),
                                               (9, "-", 45), (1, "@", 64), (2, "!", 33)))
def test_hack_dos_wall_glyphs_do_not_change_items(hack_files, kind, char, expected):
    machine = DosRoutine(hack_files)
    # Watcom struct rm: one char, one pad byte, two-byte bitfield; [80][22].
    machine.uc.mem_write(machine.address("_levl") + (10 * 22 + 5) * 4,
                         struct.pack("<BBH", ord(char), 0, kind))
    assert machine.call("hack_mapchar_", ax=10, dx=5, bx=ord(char)) == expected


@pytest.mark.parametrize("relocation", (-0x300, 0, 0x500))
def test_hack_dos_saved_monsters_rebase_full_far_pointer(hack_files, relocation):
    machine = DosRoutine(hack_files)
    base_segment, base_offset = machine.symbols["_mons"]
    old_segment = base_segment - relocation
    # Cover mons itself and static dog/wizard records in a different segment.
    for saved_segment, saved_offset in ((old_segment, base_offset + 20),
                                         (old_segment - 0x120, 0xfabc)):
        assert machine.call("hack_monster_rebase_", ax=saved_offset, dx=saved_segment,
                            bx=base_offset, cx=old_segment) == saved_offset
        assert machine.uc.reg_read(UC_X86_REG_DX) == (saved_segment + relocation) & 0xffff


def test_hack_dos_save_sickness_text_and_rebase_timeout(hack_files):
    machine = DosRoutine(hack_files)
    machine.streams()
    player = machine.address("_u")
    # Verified Watcom struct you offsets: sickness pointer +0x16; properties
    # +0x1a, eight bytes each; levitation index 6. Test the actual linked ABI.
    machine.uc.mem_write(0xe0100, b"poisonous corpse\0")
    machine.uc.mem_write(player + 0x16, struct.pack("<HH", 0x100, 0xe000))
    machine.call("hack_save_you_", ax=4)
    assert machine.written == [struct.pack("<H", 17), b"poisonous corpse\0"]

    machine.incoming = b"\0\0"
    machine.uc.mem_write(player + 0x1a, b"\xa5" * (29 * 8))
    machine.uc.mem_write(player + 0x4a, struct.pack("<I", 12))
    machine.call("hack_restore_you_", ax=4)
    assert bytes(machine.uc.mem_read(player + 0x16, 4)) == b"\0" * 4
    for prop in range(29):
        value = bytes(machine.uc.mem_read(player + 0x1e + prop * 8, 4))
        segment, offset = machine.symbols["float_down_"]
        assert value == (struct.pack("<HH", offset, segment) if prop == 6 else b"\0" * 4)


@pytest.mark.parametrize("defect", (None, "body", "length", "version", "truncate", "append"))
def test_hack_dos_persistent_file_integrity(hack_files, defect):
    machine = DosRoutine(hack_files)
    machine.file_io()
    body = b"Hack map, inventory, monsters and 32-bit gold\0" * 200
    machine.file = bytearray(16) + body
    assert machine.call("hack_finish_file_", ax=4) == 1
    magic, stamp, length, checksum = struct.unpack("<4sIII", machine.file[:16])
    assert magic == b"HACK" and length == len(machine.file)
    assert machine.file[4:8] == machine.uc.mem_read(machine.address("_hack_build_stamp"), 4)
    expected = 2166136261
    for byte in body:
        expected = ((expected ^ byte) * 16777619) & 0xffffffff
    assert checksum == expected
    if defect == "body": machine.file[24] ^= 1
    if defect == "length": machine.file[8] ^= 1
    if defect == "version": machine.file[7] ^= 1
    if defect == "truncate": del machine.file[-1:]
    if defect == "append": machine.file += b"x"
    machine.stub("pline_")  # Outdated headers use the original Hack message.
    assert machine.call("uptodate_", ax=4) == (defect is None)
    assert machine.position == 0


def test_hack_dos_header_identifies_linked_build(hack_files):
    machine = DosRoutine(hack_files)
    assert "_hack_build_stamp" in machine.symbols, "saves need a linked-image build identity"
    machine.streams()
    machine.call("hack_write_header_", ax=4)
    magic, stamp, length, checksum = struct.unpack("<4sIII", b"".join(machine.written))
    assert magic == b"HACK" and length == checksum == 0
    image = bytearray(hack_files[0].read_bytes())
    segment, offset = machine.symbols["_hack_build_stamp"]
    header_size = struct.unpack_from("<H", image, 8)[0] * 16
    at = header_size + (segment - LOAD) * 16 + offset
    assert image[at:at + 4] == struct.pack("<I", stamp)
    # The build fills exactly this data slot after linking. Normalize it to
    # the source placeholder to independently reconstruct the build identity.
    image[at:at + 4] = b"HSTP"
    assert stamp == int.from_bytes(hashlib.sha256(image).digest()[:4], "little")


def test_hack_build_stamp_is_data_with_complete_omf_byte_proof(hack_files):
    from dataclasses import replace
    from translator.compiled import load_modules, parse_compiled_map, verify_linked_bytes
    from translator.image import load_image
    from translator.layout import LayoutError

    link = parse_compiled_map(hack_files[1])
    modules = load_modules(link)
    image = load_image(hack_files[0])
    at = sum(link.symbols["_hack_build_stamp"])
    owner = next(key for key in modules if key.endswith("/build_stamp.obj"))
    stamp = link.pieces[owner, "_DATA"]
    assert (stamp.kind, stamp.address, stamp.size) == ("DATA", at, 4)
    verify_linked_bytes(image, link, modules)
    changed = bytearray(image.data)
    changed[at] ^= 1
    with pytest.raises(LayoutError, match="linked byte mismatch"):
        verify_linked_bytes(replace(image, data=bytes(changed)), link, modules)


@pytest.mark.parametrize("consumer,wizard", (("save", False), ("bones", False), ("bones", True)),
                         ids=("save", "bones", "wizard-bones"))
@pytest.mark.parametrize("old_header", ("other-build", "legacy"))
def test_hack_dos_outdated_save_and_bones_message_then_delete(hack_files, consumer, wizard, old_header):
    machine = DosRoutine(hack_files)
    machine.file_io()
    machine.file = bytearray(16) + b"Old map with valid length and checksum.\0" * 250
    assert machine.call("hack_finish_file_", ax=4) == 1
    if old_header == "legacy":
        machine.file[:8] = b"HACK103\1"
    else:
        machine.file[4] ^= 1  # Change only build identity, not payload integrity.
    events = []

    def string(pointer):
        return bytes(machine.uc.mem_read(pointer, 256)).split(b"\0", 1)[0].decode("ascii")

    def argument():
        return machine.uc.reg_read(UC_X86_REG_DX) * 16 + machine.uc.reg_read(UC_X86_REG_AX)

    def variadic_argument():
        at = machine.uc.reg_read(UC_X86_REG_SS) * 16 + machine.uc.reg_read(UC_X86_REG_SP) + 4
        offset, segment = struct.unpack("<HH", machine.uc.mem_read(at, 4))
        return segment * 16 + offset

    def message(current):
        events.append(("message", string(variadic_argument())))

    def close(current):
        events.append(("close", machine.uc.reg_read(UC_X86_REG_AX)))
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    def unlink(current):
        events.append(("unlink", string(argument())))
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    def rename(current):
        events.append(("rename", string(argument())))
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    def restore(current):
        raise AssertionError("outdated pointer-bearing level must not reach getlev")

    machine.stub("pline_", message)
    machine.stub("hack_printf_", message)
    machine.stub("close_", close)
    machine.stub("unlink_", unlink)
    if "rename_" in machine.symbols:
        machine.stub("rename_", rename)
    machine.stub("getlev_", restore)
    if consumer == "save":
        result = machine.call("hack_resume_valid_", ax=4)
        expected_name = "HACK.SAV"
    else:
        machine.stub("rn2_", lambda m: m.uc.reg_write(UC_X86_REG_AX, 0))
        machine.stub("open_", lambda m: m.uc.reg_write(UC_X86_REG_AX, 4))
        # wizard is flags.debug: the low bit after the 16-bit ident member.
        machine.uc.mem_write(machine.address("_flags") + 2, struct.pack("<H", wizard))
        machine.uc.mem_write(machine.address("_dlevel"), struct.pack("<H", 7))
        result = machine.call("getbones_")
        expected_name = "bones_07"
    assert result == 0
    assert events == [("message", "Saved level is out of date. "),
                      ("close", 4), ("unlink", expected_name)]


@pytest.mark.parametrize("wizard", (False, True), ids=("normal", "wizard"))
def test_hack_dos_current_bones_preserve_original_wizard_behavior(hack_files, wizard):
    machine = DosRoutine(hack_files)
    machine.file_io()
    machine.file = bytearray(16) + b"Current build bones payload.\0" * 300
    assert machine.call("hack_finish_file_", ax=4) == 1
    events = []

    def event(name):
        def called(current):
            events.append(name)
            machine.uc.reg_write(UC_X86_REG_AX, 0)
        return called

    machine.stub("rn2_", lambda m: m.uc.reg_write(UC_X86_REG_AX, 0))
    machine.stub("open_", lambda m: m.uc.reg_write(UC_X86_REG_AX, 4))
    machine.stub("getlev_", event("restore"))
    machine.stub("close_", event("close"))
    machine.stub("unlink_", event("unlink"))
    machine.stub("pline_", event("unexpected message"))
    machine.uc.mem_write(machine.address("_flags") + 2, struct.pack("<H", wizard))
    machine.uc.mem_write(machine.address("_dlevel"), struct.pack("<H", 7))
    assert machine.call("getbones_") == 1
    assert events == ["restore", "close"] + ([] if wizard else ["unlink"])


@pytest.mark.parametrize("typed,expected,erases", (
    (b"old\x7freplacement\n", b"replacement", 3),
    (b"old\bnew\n", b"olnew", 1),
    (b"old\x15replacement\n", b"replacement", 3),
    (b"\x7fold\x7f\x7fnew\n", b"new", 3),
), ids=("DEL-whole-line", "BS-one-character", "Ctrl-U-whole-line", "empty-DEL"))
def test_hack_dos_getlin_matches_upstream_erase_semantics(hack_files, typed, expected, erases):
    machine = DosRoutine(hack_files)
    keys = iter(typed)
    erased = []

    def erase(current):
        pointer = machine.uc.reg_read(UC_X86_REG_DX) * 16 + machine.uc.reg_read(UC_X86_REG_AX)
        erased.append(bytes(machine.uc.mem_read(pointer, 4)))

    machine.stub("hack_getchar_", lambda m: m.uc.reg_write(UC_X86_REG_AX, next(keys)))
    machine.stub("putstr_", erase)
    machine.stub("putsym_")
    machine.call("getlin_", ax=0x100, dx=0xe000)
    line = bytes(machine.uc.mem_read(0xe0100, 80)).split(b"\0", 1)[0]
    assert line == expected
    assert erased == [b"\b \b\0"] * erases


@pytest.mark.parametrize("caller_drive", (3, 8), ids=("other-drive", "same-drive"))
@pytest.mark.parametrize("lfn", ("supported", "unsupported-7100", "unsupported-1"))
def test_hack_dos_exit_restores_all_changed_directories(hack_files, caller_drive, lfn):
    machine = DosRoutine(hack_files)
    directories = {3: "C:\\Long caller directory", 8: "H:\\Before Hack directory"} if lfn == "supported" else {
        3: "C:\\WORK", 8: "H:\\BEFORE"}
    original = directories.copy()
    active = [caller_drive]
    callback = []
    classic = []
    lfn_calls = []

    def string(pointer):
        return bytes(machine.uc.mem_read(pointer, 263)).split(b"\0", 1)[0].decode("ascii")

    def first_pointer():
        return machine.uc.reg_read(UC_X86_REG_DX) * 16 + machine.uc.reg_read(UC_X86_REG_AX)

    def getdcwd(current):
        classic.append("getcwd")
        drive = machine.uc.reg_read(UC_X86_REG_AX)
        pointer = machine.pointer()
        machine.uc.mem_write(pointer, directories[drive].encode() + b"\0")
        machine.uc.reg_write(UC_X86_REG_AX, machine.uc.reg_read(UC_X86_REG_BX))
        machine.uc.reg_write(UC_X86_REG_DX, machine.uc.reg_read(UC_X86_REG_CX))

    def chdrive(current):
        active[0] = machine.uc.reg_read(UC_X86_REG_AX)
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    def chdir(current):
        classic.append("chdir")
        path = string(first_pointer())
        drive = ord(path[0].upper()) - ord("A") + 1 if path[1:2] == ":" else active[0]
        directories[drive] = path
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    def dos(uc, number, opaque):
        assert number == 0x21
        function = uc.reg_read(UC_X86_REG_AX)
        assert function in (0x7147, 0x713b)
        lfn_calls.append(function)
        if lfn != "supported":
            uc.reg_write(UC_X86_REG_AX, 0x7100 if lfn == "unsupported-7100" else 1)
            uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) | 1)
            return
        if function == 0x7147:
            drive = uc.reg_read(UC_X86_REG_DX) & 0xff
            pointer = uc.reg_read(UC_X86_REG_DS) * 16 + uc.reg_read(UC_X86_REG_SI)
            uc.mem_write(pointer, directories[drive][3:].encode() + b"\0")
            uc.reg_write(UC_X86_REG_AX, 0x100)
        else:
            pointer = uc.reg_read(UC_X86_REG_DS) * 16 + uc.reg_read(UC_X86_REG_DX)
            path = string(pointer)
            directories[ord(path[0]) - ord("A") + 1] = path
        uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) & ~1)

    def register(current):
        callback.append((machine.uc.reg_read(UC_X86_REG_DX), machine.uc.reg_read(UC_X86_REG_AX)))
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    def no_file(current):
        machine.uc.reg_write(UC_X86_REG_AX, 0)
        machine.uc.reg_write(UC_X86_REG_DX, 0)

    def game(current):
        assert active[0] == 8
        assert directories[8] == "H:\\GAMES\\HACK"
        machine.uc.reg_write(UC_X86_REG_AX, 42)

    machine.stub("_getdrive_", lambda m: m.uc.reg_write(UC_X86_REG_AX, caller_drive))
    machine.stub("_getdcwd_", getdcwd)
    machine.stub("_chdrive_", chdrive)
    machine.stub("chdir_", chdir)
    machine.stub("atexit_", register)
    machine.stub("access_", lambda m: m.uc.reg_write(UC_X86_REG_AX, 0))
    machine.stub("fopen_", no_file)
    machine.stub("hack_main_", game)
    machine.uc.hook_add(UC_HOOK_INTR, dos)
    machine.uc.mem_write(0xe0100, struct.pack("<4H", 0x200, 0xe000, 0, 0))
    machine.uc.mem_write(0xe0200, b"H:\\GAMES\\HACK\\HACK.EXE\0")
    assert machine.call("main_", ax=1, bx=0x100, cx=0xe000) == 42
    assert len(callback) == 1, "startup must register an exit handler"
    machine.symbols["restore_directory"] = callback[0]
    machine.call("restore_directory")
    assert active[0] == caller_drive
    assert directories == original
    assert lfn_calls.count(0x7147) == (2 if caller_drive == 3 else 1)
    assert lfn_calls.count(0x713b) == (3 if caller_drive == 3 else 2)
    assert (classic == []) == (lfn == "supported"), "LFN paths must never become numbered aliases"


@pytest.mark.parametrize("operation", ("remember", "return"))
def test_hack_lfn_path_errors_never_fall_back_to_short_aliases(hack_files, operation):
    machine = DosRoutine(hack_files)
    calls = []

    def forbidden(current):
        raise AssertionError("a path error must not fall back to classic DOS")

    def dos(uc, number, opaque):
        assert number == 0x21
        calls.append(uc.reg_read(UC_X86_REG_AX))
        uc.reg_write(UC_X86_REG_AX, 3)  # path not found, not missing LFN support
        uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) | 1)

    machine.stub("_getdcwd_", forbidden)
    machine.stub("chdir_", forbidden)
    machine.uc.hook_add(UC_HOOK_INTR, dos)
    machine.uc.mem_write(0xe0100, b"C:\\Caller\0")
    if operation == "remember":
        assert machine.call("hack_remember_directory_", ax=3, bx=0x100, cx=0xe000) == 0
        assert calls == [0x7147]
    else:
        assert machine.call("hack_return_directory_", ax=0x100, dx=0xe000) == 0xffff
        assert calls == [0x713b]


def test_hack_lfn_directory_capture_has_room_for_drive_prefix(hack_files):
    machine = DosRoutine(hack_files)
    directory = b"a" * 259 + b"\0"

    def dos(uc, number, opaque):
        assert number == 0x21 and uc.reg_read(UC_X86_REG_AX) == 0x7147
        pointer = uc.reg_read(UC_X86_REG_DS) * 16 + uc.reg_read(UC_X86_REG_SI)
        uc.mem_write(pointer, directory)
        uc.reg_write(UC_X86_REG_EFLAGS, uc.reg_read(UC_X86_REG_EFLAGS) & ~1)

    machine.uc.hook_add(UC_HOOK_INTR, dos)
    machine.uc.mem_write(0xe0100, b"\xa5" * 263 + b"GUARD")
    assert machine.call("hack_remember_directory_", ax=3, bx=0x100, cx=0xe000) == 1
    assert bytes(machine.uc.mem_read(0xe0100, 268)) == b"C:\\" + directory + b"GUARD"


@pytest.mark.parametrize("operation", ("remember", "return"))
@pytest.mark.parametrize("unsupported", (0x7100, 1), ids=("unsupported-7100", "unsupported-1"))
def test_hack_lfn_presets_real_carry_before_unsupported_dos(hack_files, operation, unsupported):
    machine = DosRoutine(hack_files)
    incoming_carry = []
    classic = []
    directory = b"C:\\WORK\0"

    def dos(uc, number, opaque):
        assert number == 0x21
        assert uc.reg_read(UC_X86_REG_AX) == (0x7147 if operation == "remember" else 0x713b)
        incoming_carry.append(uc.reg_read(UC_X86_REG_EFLAGS) & 1)
        # Older DOS handlers may report an unsupported LFN function without
        # changing carry. Setting union REGS.cflag is not enough: Watcom's
        # int86x treats it as output-only and enters the actual INT with CF=0.
        uc.reg_write(UC_X86_REG_AX, unsupported)

    def getdcwd(current):
        classic.append("remember")
        assert machine.uc.reg_read(UC_X86_REG_AX) == 3
        machine.uc.mem_write(machine.pointer(), directory)
        machine.uc.reg_write(UC_X86_REG_AX, machine.uc.reg_read(UC_X86_REG_BX))
        machine.uc.reg_write(UC_X86_REG_DX, machine.uc.reg_read(UC_X86_REG_CX))

    def chdir(current):
        classic.append("return")
        pointer = machine.uc.reg_read(UC_X86_REG_DX) * 16 + machine.uc.reg_read(UC_X86_REG_AX)
        assert bytes(machine.uc.mem_read(pointer, len(directory))) == directory
        machine.uc.reg_write(UC_X86_REG_AX, 0)

    machine.stub("_getdcwd_", getdcwd)
    machine.stub("chdir_", chdir)
    machine.uc.hook_add(UC_HOOK_INTR, dos)
    machine.uc.mem_write(0xe0100, directory)
    if operation == "remember":
        result = machine.call("hack_remember_directory_", ax=3, bx=0x100, cx=0xe000)
    else:
        result = machine.call("hack_return_directory_", ax=0x100, dx=0xe000)
    assert incoming_carry == [1], "the real INT 21h must enter with CF set, not just REGS.cflag"
    assert classic == [operation], "unsupported LFN calls with unchanged CF must use classic DOS"
    assert result == (1 if operation == "remember" else 0)
    assert bytes(machine.uc.mem_read(0xe0100, len(directory))) == directory
