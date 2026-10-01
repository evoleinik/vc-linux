"""The DOS port's widths are checked in compiled 16-bit code, not host C.

Unicorn executes the real EXE routines. Stack checks, external curses/stream
calls and irrelevant collections are stubbed; arithmetic and score formatting
use Watcom CRT, and all tested scalar/cell serialization remains real DOS code.
"""
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tomllib

import pytest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_EFLAGS,
)

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/rogue"
LOAD = 0x1000
STOP = 0xF0100


@pytest.mark.parametrize("watcom_state", ("unset", "missing"))
def test_rogue_build_missing_watcom_explains_install(tmp_path, watcom_state):
    env = os.environ.copy()
    env.pop("WATCOM", None)
    if watcom_state == "missing":
        env["WATCOM"] = str(tmp_path / "missing-openwatcom")
    out = tmp_path / "rogue"
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools/build_rogue.py"), "--out", str(out)],
        cwd=ROOT, env=env, capture_output=True, text=True, timeout=10,
    )
    assert result.returncode != 0
    assert ("tools/fetch-openwatcom.sh build/openwatcom && "
            "export WATCOM=$PWD/build/openwatcom") in result.stderr
    assert not out.exists()


@pytest.fixture(scope="module")
def rogue_files():
    subprocess.run(["make", "rogue"], cwd=ROOT, check=True, capture_output=True)
    return BUILD / "ROGUE.EXE", BUILD / "ROGUE.MAP"


class DosRoutine:
    def __init__(self, files):
        exe, mapfile = files
        raw = exe.read_bytes()
        assert raw[:2] == b"MZ"
        header = struct.unpack_from("<14H", raw)
        body = bytearray(raw[header[4] * 16:])
        for index in range(header[3]):
            offset, segment = struct.unpack_from("<HH", raw, header[12] + 4 * index)
            at = 16 * segment + offset
            old, = struct.unpack_from("<H", body, at)
            struct.pack_into("<H", body, at, (old + LOAD) & 0xffff)
        self.uc = Uc(UC_ARCH_X86, UC_MODE_16)
        self.uc.mem_map(0, 0x100000)
        self.uc.mem_write(LOAD * 16, bytes(body))
        text = mapfile.read_text()
        self.symbols = {
            name: (LOAD + int(segment, 16), int(offset, 16))
            for segment, offset, name in re.findall(
                r"^([0-9a-f]{4}):([0-9a-f]{4})[+* ]+\s+(\S+)\s*$", text, re.M | re.I)
        }
        match = re.search(r"^DGROUP\s+([0-9a-f]+):([0-9a-f]+)", text, re.M | re.I)
        assert match
        self.ds = LOAD + int(match[1], 16)
        self.stack = (header[7] - (self.ds - LOAD)) * 16 + header[8] - 16
        self.written = []
        self.incoming = b""
        self.returned = False
        self.stubs = {}
        # __STK is not relevant to a leaf-routine width test. Its far-call
        # frame remains real, and all tested helpers must balance the stack.
        self.uc.mem_write(self.address("__STK"), b"\xcb")
        for symbol in ("rs_write_", "rs_read_"):
            self.uc.mem_write(self.address(symbol), b"\xca\x02\x00")
        self.uc.hook_add(UC_HOOK_CODE, self._hook)

    def address(self, symbol):
        segment, offset = self.symbols[symbol]
        return segment * 16 + offset

    def stub(self, symbol, result=0, *, pop=0):
        address = self.address(symbol)
        self.stubs[address] = {"name": symbol, "result": result, "calls": 0}
        self.uc.mem_write(address, b"\xca" + struct.pack("<H", pop) if pop else b"\xcb")

    def calls(self, symbol):
        return self.stubs[self.address(symbol)]["calls"]

    def _hook(self, uc, address, size, _):
        if address == STOP:
            self.returned = True
            uc.emu_stop()
        elif address in self.stubs:
            stub = self.stubs[address]
            stub["calls"] += 1
            uc.reg_write(UC_X86_REG_AX, stub["result"] & 0xffff)
        elif address in (self.address("rs_write_"), self.address("rs_read_")):
            pointer = uc.reg_read(UC_X86_REG_CX) * 16 + uc.reg_read(UC_X86_REG_BX)
            stack = uc.reg_read(UC_X86_REG_SS) * 16 + uc.reg_read(UC_X86_REG_SP)
            count, = struct.unpack("<H", uc.mem_read(stack + 4, 2))
            if address == self.address("rs_write_"):
                self.written.append(bytes(uc.mem_read(pointer, count)))
            else:
                assert len(self.incoming) >= count
                uc.mem_write(pointer, self.incoming[:count])
                self.incoming = self.incoming[count:]
            uc.reg_write(UC_X86_REG_AX, 0)

    def call(self, symbol, *, ax=0, bx=0, cx=0, dx=0, instruction_limit=10000):
        uc = self.uc
        self.returned = False
        # Poison otherwise-unused stack bytes to expose two-byte values that
        # were wrongly serialized as four-byte Unix ints.
        uc.mem_write(self.ds * 16 + self.stack - 1024, b"\xa5" * 1024)
        regs = {
            UC_X86_REG_AX: ax, UC_X86_REG_BX: bx, UC_X86_REG_CX: cx, UC_X86_REG_DX: dx,
            UC_X86_REG_SI: 0x3210, UC_X86_REG_DI: 0x4567, UC_X86_REG_BP: 0x7654,
            UC_X86_REG_DS: self.ds, UC_X86_REG_SS: self.ds, UC_X86_REG_ES: self.ds,
            UC_X86_REG_SP: self.stack - 4, UC_X86_REG_EFLAGS: 2,
        }
        for register, value in regs.items():
            uc.reg_write(register, value & 0xffff)
        uc.mem_write(self.ds * 16 + self.stack - 4, struct.pack("<HH", 0x100, 0xF000))
        segment, offset = self.symbols[symbol]
        uc.reg_write(UC_X86_REG_CS, segment)
        uc.emu_start(segment * 16 + offset, 0, count=instruction_limit)
        assert self.returned, f"{symbol} did not return"
        assert uc.reg_read(UC_X86_REG_SP) == self.stack
        assert uc.reg_read(UC_X86_REG_BP) == 0x7654
        return uc.reg_read(UC_X86_REG_AX)


def test_dos_rng_keeps_all_32_bits(rogue_files):
    machine = DosRoutine(rogue_files)
    for seed in (0, 1, 42, 0x7fff, 0x8000, 0xffff, 0x12345678, 0x80000000, 0xffffffff):
        for span in (0, 1, 6, 100, 255, 1000, 32767):
            machine.uc.mem_write(machine.address("_seed"), struct.pack("<I", seed))
            next_seed = (seed * 11109 + 13849) & 0xffffffff if span else seed
            expected = ((next_seed >> 16) % span) if span else 0
            assert machine.call("rnd_", ax=span) == expected
            actual, = struct.unpack("<I", machine.uc.mem_read(machine.address("_seed"), 4))
            assert actual == next_seed


def test_dos_experience_thresholds_do_not_truncate(rogue_files):
    machine = DosRoutine(rogue_files)
    levels = struct.unpack("<21i", machine.uc.mem_read(machine.address("_e_levels"), 84))
    assert levels == (10, 20, 40, 80, 160, 320, 640, 1300, 2600, 5200, 13000,
                      26000, 50000, 100000, 200000, 400000, 800000, 2000000, 4000000,
                      8000000, 0)


@pytest.mark.parametrize("value", (-32768, -1, 0, 1, 32767))
def test_dos_save_int_sign_extends_four_bytes(rogue_files, value):
    machine = DosRoutine(rogue_files)
    assert machine.call("rs_write_int_", bx=value) == 0
    assert machine.written == [struct.pack("<i", value)]


@pytest.mark.parametrize("value", (0, 32767, 32768, 65535))
def test_dos_save_uint_zero_extends_four_bytes(rogue_files, value):
    machine = DosRoutine(rogue_files)
    assert machine.call("rs_write_uint_", bx=value) == 0
    assert machine.written == [struct.pack("<I", value)]


@pytest.mark.parametrize("symbol", ("rs_read_int_", "rs_read_uint_"))
@pytest.mark.parametrize("value", (0, 1, 0x7fff, 0x8000, 0xffff))
def test_dos_restore_int_does_not_overwrite_neighbours(rogue_files, symbol, value):
    machine = DosRoutine(rogue_files)
    machine.incoming = struct.pack("<I", value)
    machine.uc.mem_write(0xe0100, b"LEFT\0\0RIGHT")
    assert machine.call(symbol, bx=0x104, cx=0xe000) == 0
    assert bytes(machine.uc.mem_read(0xe0100, 11)) == b"LEFT" + struct.pack("<H", value) + b"RIGHT"
    assert not machine.incoming


def test_dos_save_markers_preserve_their_high_word(rogue_files):
    machine = DosRoutine(rogue_files)
    assert machine.call("rs_write_marker_", bx=0x0017, cx=0xabcd) == 0
    assert machine.written == [bytes.fromhex("1700cdab")]
    machine.incoming = bytes.fromhex("1700cdab")
    assert machine.call("rs_read_marker_", bx=0x0017, cx=0xabcd) == 0
    machine.incoming = bytes.fromhex("1700ceab")
    assert machine.call("rs_read_marker_", bx=0x0017, cx=0xabcd) == 1


WINDOW_CELLS = (0x00200041, 0x00800041, 0xa5000041, 0xa5a00041)


@pytest.mark.parametrize("cell", WINDOW_CELLS, ids=("reverse", "bold", "colour", "combined"))
def test_dos_save_window_preserves_32bit_attributes(rogue_files, cell):
    machine = DosRoutine(rogue_files)
    machine.stub("getmaxx_", 1)
    machine.stub("getmaxy_", 1)
    machine.stub("mvwinch_", cell & 0xffff)

    def return_cell(uc, address, size, opaque):
        if address == machine.address("mvwinch_"):
            uc.reg_write(UC_X86_REG_DX, cell >> 16)

    machine.uc.hook_add(UC_HOOK_CODE, return_cell)
    assert machine.call("rs_write_window_", bx=0x100, cx=0xe000) == 0
    assert machine.written == [struct.pack("<I", value)
                               for value in (0xabcd000d, 1, 1, cell)]


@pytest.mark.parametrize("cell", WINDOW_CELLS, ids=("reverse", "bold", "colour", "combined"))
def test_dos_restore_window_preserves_32bit_attributes(rogue_files, cell):
    machine = DosRoutine(rogue_files)
    machine.stub("getmaxx_", 1)
    machine.stub("getmaxy_", 1)
    # mvwaddch's final chtype argument occupies four callee-popped stack bytes.
    machine.uc.mem_write(machine.address("mvwaddch_"), b"\xca\x04\x00")
    drawn = []

    def draw_cell(uc, address, size, opaque):
        if address == machine.address("mvwaddch_"):
            stack = uc.reg_read(UC_X86_REG_SS) * 16 + uc.reg_read(UC_X86_REG_SP)
            drawn.append(struct.unpack("<I", uc.mem_read(stack + 4, 4))[0])
            assert uc.reg_read(UC_X86_REG_BX) == uc.reg_read(UC_X86_REG_CX) == 0
            uc.reg_write(UC_X86_REG_AX, 0)

    machine.uc.hook_add(UC_HOOK_CODE, draw_cell)
    machine.incoming = struct.pack("<4I", 0xabcd000d, 1, 1, cell)
    assert machine.call("rs_read_window_", bx=0x100, cx=0xe000) == 0
    assert drawn == [cell]
    assert not machine.incoming


def test_dos_winning_gold_crosses_signed_16bit_boundary(rogue_files):
    machine = DosRoutine(rogue_files)
    for name in ("clear_", "standout_", "addstr_", "standend_", "mvaddstr_",
                 "refresh_", "wait_for_", "printw_", "inv_name_", "my_exit_"):
        machine.stub(name)
    # This is the actual large-model THING layout: the pack is at player+54
    # and an object's type is at +8. A single amulet is worth exactly 1000.
    machine.uc.mem_write(machine.address("_player") + 54, struct.pack("<HH", 0x100, 0xe000))
    machine.uc.mem_write(0xe0100, b"\0" * 64)
    machine.uc.mem_write(0xe0108, struct.pack("<H", ord(',')))
    machine.uc.mem_write(machine.address("_purse"), struct.pack("<i", 32000))
    machine.uc.mem_write(machine.address("score_"), b"\xcb")
    scored = []

    def collect_score(uc, address, size, opaque):
        if address == machine.address("score_"):
            scored.append((uc.reg_read(UC_X86_REG_DX) << 16) | uc.reg_read(UC_X86_REG_AX))
            assert uc.reg_read(UC_X86_REG_BX) == 2  # total-winner score
            assert uc.reg_read(UC_X86_REG_CX) & 0xff == ord(' ')

    machine.uc.hook_add(UC_HOOK_CODE, collect_score)
    machine.call("total_winner_")
    assert struct.unpack("<i", machine.uc.mem_read(machine.address("_purse"), 4))[0] == 33000
    assert scored == [33000]


@pytest.mark.parametrize("gold", (33000, 1000000))
def test_dos_score_text_roundtrip_keeps_32bit_gold(rogue_files, gold):
    machine = DosRoutine(rogue_files)
    machine.stub("rewind_")
    # The 32-bit score changes sizeof(SCORE) to 1038; its score is at +2.
    record = struct.pack("<HiHH1024sHH", 42, gold, 1, 0, b"Rogue", 7, 0)
    machine.uc.mem_write(0xe0100, record)
    machine.uc.mem_write(machine.address("_numscores"), struct.pack("<H", 1))
    machine.uc.mem_write(machine.address("_scoreboard"), struct.pack("<HH", 0x100, 0xd000))
    for name in ("encwrite_", "encread_"):
        machine.uc.mem_write(machine.address(name), b"\xca\x04\x00")
    saved, restoring = [], []

    def stream(uc, address, size, opaque):
        if address not in (machine.address("encwrite_"), machine.address("encread_")):
            return
        pointer = uc.reg_read(UC_X86_REG_DX) * 16 + uc.reg_read(UC_X86_REG_AX)
        count = uc.reg_read(UC_X86_REG_BX)
        if address == machine.address("encwrite_"):
            saved.append(bytes(uc.mem_read(pointer, count)))
        else:
            block = restoring.pop(0)
            assert len(block) == count
            uc.mem_write(pointer, block)
        uc.reg_write(UC_X86_REG_AX, count)

    machine.uc.hook_add(UC_HOOK_CODE, stream)
    machine.call("wr_score_", ax=0x100, dx=0xe000, instruction_limit=100000)
    assert saved[0] == record[10:1034]
    assert saved[1].split(b"\0", 1)[0] == f" 42 {gold} 1 0 7 0 \n".encode()
    restoring[:] = saved
    machine.uc.mem_write(0xe0100, b"\0" * len(record))
    machine.call("rd_score_", ax=0x100, dx=0xe000, instruction_limit=100000)
    assert bytes(machine.uc.mem_read(0xe0100, len(record))) == record
    assert not restoring


@pytest.mark.parametrize("gold", (33000, 1000000))
def test_dos_saved_game_scalar_roundtrip_keeps_32bit_gold(rogue_files, gold):
    machine = DosRoutine(rogue_files)
    # Keep all real scalar save/read calls. Complex collections, pointers and
    # curses are independent of the gold-width regression and are stubbed in
    # matching write/read pairs. Counts beyond the four register words are
    # callee-popped; the small by-value coord fits the remaining registers.
    for suffix in ("booleans", "chars", "strings", "ints", "longs", "places",
                   "rooms", "monsters", "obj_info", "daemons"):
        for mode in ("write", "read"):
            if mode == "read" and suffix == "strings":
                name = "rs_read_new_strings_"
            else:
                name = f"rs_{mode}_{suffix}_"
            machine.stub(name, pop=2)
    for suffix in ("potions", "rings", "scrolls", "sticks", "thing", "object_list",
                   "thing_list", "stats", "room_reference", "window"):
        for mode in ("write", "read"):
            machine.stub(f"rs_{mode}_{suffix}_")
    machine.stub("rs_write_string_")
    machine.stub("rs_read_new_string_")
    machine.stub("rs_write_coord_")
    machine.stub("rs_read_coord_")
    machine.stub("rs_write_object_reference_", pop=4)
    machine.stub("rs_read_object_reference_", pop=4)
    machine.stub("rs_fix_thing_")
    machine.stub("rs_fix_thing_list_")
    for name, value in (("_purse", gold), ("_lastscore", gold - 1)):
        machine.uc.mem_write(machine.address(name), struct.pack("<i", value))
    assert machine.call("rs_save_file_") == 0
    assert struct.pack("<i", gold) in machine.written
    assert struct.pack("<i", gold - 1) in machine.written
    machine.incoming = b"".join(machine.written)
    for name in ("_purse", "_lastscore"):
        machine.uc.mem_write(machine.address(name), b"\0" * 4)
    assert machine.call("rs_restore_file_") == 0
    for name, value in (("_purse", gold), ("_lastscore", gold - 1)):
        assert bytes(machine.uc.mem_read(machine.address(name), 4)) == struct.pack("<i", value)
    assert not machine.incoming


@pytest.mark.parametrize("error", ("none", "state", "stream", "flush", "close"))
def test_dos_failed_save_resumes_game_and_closes_stream(rogue_files, error):
    machine = DosRoutine(rogue_files)
    for name in ("fflush_", "fclose_", "md_unlink_", "setup_", "playltchars_",
                 "clearok_", "wrefresh_", "msg_"):
        result = -1 if ((name == "fflush_" and error == "flush") or
                        (name == "fclose_" and error == "close")) else 0
        machine.stub(name, result)
    # Watcom large-model FILE::_flag is at +10; _SFERR is bit 0x20.
    # This actual binary layout is intentionally part of this DOS-only test.
    machine.uc.mem_write(0xe0200, b"\0" * 24)
    if error == "stream":
        machine.uc.mem_write(0xe020a, b"\x20\0")
    assert machine.call("rogue_finish_save_", ax=0x200, dx=0xe000,
                        bx=int(error == "state")) == int(error == "none")
    assert machine.calls("fflush_") == machine.calls("fclose_") == 1
    for name in ("md_unlink_", "setup_", "playltchars_", "clearok_", "wrefresh_", "msg_"):
        assert machine.calls(name) == int(error != "none")


def _build_pre18_rogue_probe(probe_root):
    # Compile the historical sources, rather than trusting an archived EXE or
    # allowing its old hash for today's production build. All edits are copies.
    (probe_root / "tools").mkdir(parents=True)
    (probe_root / "third_party").symlink_to(ROOT / "third_party", target_is_directory=True)
    for name in ("build_rogue.py", "rogue_port.py"):
        shutil.copyfile(ROOT / "tools" / name, probe_root / "tools" / name)
    shutil.copytree(ROOT / "runtime/rogue_dos", probe_root / "runtime/rogue_dos")
    edits = tomllib.loads((ROOT / "tests/fixtures/rogue-pre18.toml").read_text())["replace"]
    for edit in edits:
        path = probe_root / edit["path"]
        content = path.read_text()
        assert content.count(edit["before"]) == 1, (
            f"historical Rogue probe no longer matches {edit['path']}")
        path.write_text(content.replace(edit["before"], edit["after"]))
    output = probe_root / "build"
    subprocess.run([sys.executable, "tools/build_rogue.py", "--out", str(output)],
                   cwd=probe_root, env=os.environ, check=True, capture_output=True)
    return (output / "ROGUE.EXE").read_bytes()


def test_rogue_build_pins_banner_before_compilation(tmp_path, monkeypatch):
    # This fails on the old builder even on the same day as the verified
    # image: a live compiler date must never reach the banner translation.
    monkeypatch.syspath_prepend(str(ROOT / "tools"))
    spec = importlib.util.spec_from_file_location("build_rogue", ROOT / "tools/build_rogue.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    watcom = Path(os.environ.get("WATCOM", ROOT / "build/openwatcom")).resolve()
    run = subprocess.run
    compiled_notices = []
    vendor = ROOT / "third_party/pdcurses/pdcurses/initscr.c"
    original = vendor.read_bytes()

    def compile_checked_notice(args, **kwargs):
        if Path(args[0]).name == "wcc" and Path(args[-1]).name == "initscr.c":
            source = Path(args[-1])
            content = source.read_text()
            assert "__DATE__" not in content, "PDCurses banner still depends on the compiler date"
            assert "__TIME__" not in content and "__TIMESTAMP__" not in content
            assert '"Oct  1 2026"' in content
            assert source == tmp_path / "pdcsrc/initscr.c"
            compiled_notices.append(source)
        return run(args, **kwargs)

    monkeypatch.setattr(subprocess, "run", compile_checked_notice)
    builder.build(ROOT, watcom, tmp_path, 2)
    assert compiled_notices == [tmp_path / "pdcsrc/initscr.c"]
    assert vendor.read_bytes() == original
    assert hashlib.sha256((tmp_path / "ROGUE.EXE").read_bytes()).hexdigest() == (
        "b6350f98553e96ae5454383ec377d9feb63834cc99fc3411f7ff016321dd61a7"
    )


@pytest.mark.parametrize("changed", ("missing", "duplicate", "date", "time", "timestamp"))
def test_rogue_build_refuses_changed_clock_notice(monkeypatch, changed):
    monkeypatch.syspath_prepend(str(ROOT / "tools"))
    spec = importlib.util.spec_from_file_location("build_rogue", ROOT / "tools/build_rogue.py")
    builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(builder)
    source = (ROOT / "third_party/pdcurses/pdcurses/initscr.c").read_text()
    if changed == "missing":
        source = source.replace("__DATE__", '"upstream date changed"')
    elif changed == "duplicate":
        source += source
    else:
        source += "\nconst char *another_clock = __" + changed.upper() + "__;\n"
    with pytest.raises(ValueError, match="PDCurses.*(?:notice|clock)"):
        builder.pin_pdcurses_notice(source)


def test_rogue_build_is_byte_reproducible(rogue_files, tmp_path):
    before = rogue_files[0].read_bytes()
    def vendor_digests():
        return {path: hashlib.sha256(path.read_bytes()).digest()
                for vendor in ("rogue", "pdcurses")
                for path in (ROOT / "third_party" / vendor).rglob("*") if path.is_file()}
    digests = vendor_digests()
    # Removed/renamed upstream inputs must not survive as accidentally linked
    # stale build copies; only the current vendor source list is authoritative.
    (tmp_path / "src").mkdir()
    (tmp_path / "src/stale.c").write_text("#error obsolete build copy must not be compiled\n")
    subprocess.run([sys.executable, "tools/build_rogue.py", "--out", str(tmp_path)],
                   cwd=ROOT, env=os.environ, check=True, capture_output=True)
    assert (tmp_path / "ROGUE.EXE").read_bytes() == before
    assert vendor_digests() == digests
    assert hashlib.sha256(before).hexdigest() == (
        "b6350f98553e96ae5454383ec377d9feb63834cc99fc3411f7ff016321dd61a7"
    ), "verified ROGUE.EXE SHA-256 mismatch"
    # Brief 18 quotes the pre-fix source identity. Rebuild that historical
    # variant independently; its hash must never admit an unfixed shipped EXE.
    historical = _build_pre18_rogue_probe(tmp_path / "pre18")
    assert hashlib.sha256(historical).hexdigest() == (
        "8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee"
    ), "pre-brief-18 ROGUE.EXE SHA-256 mismatch"
    assert vendor_digests() == digests


def test_rogue_reproducibility_rejects_matching_unverified_bytes(rogue_files, tmp_path,
                                                               monkeypatch):
    # Equality alone accepts a deterministic but unverified build. Append the
    # same harmless DOS overlay to two isolated real builds, never the fixture.
    overlay = b"\0unverified-build-regression\0"
    unverified = tmp_path / "ROGUE.EXE"
    unverified.write_bytes(rogue_files[0].read_bytes() + overlay)
    rebuilt = tmp_path / "rebuilt"
    rebuilt.mkdir()
    run = subprocess.run

    def build_with_overlay(args, **kwargs):
        result = run(args, **kwargs)
        exe = rebuilt / "ROGUE.EXE"
        exe.write_bytes(exe.read_bytes() + overlay)
        return result

    monkeypatch.setattr(subprocess, "run", build_with_overlay)
    with pytest.raises(AssertionError, match="verified ROGUE.EXE SHA-256"):
        test_rogue_build_is_byte_reproducible((unverified, rogue_files[1]), rebuilt)


def test_rogue_port_refuses_changed_upstream_source(tmp_path):
    spec = importlib.util.spec_from_file_location("rogue_port", ROOT / "tools/rogue_port.py")
    port = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(port)
    source = tmp_path / "input"
    source.mkdir()
    (source / "main.c").write_text("/* a changed upstream entry point */")
    with pytest.raises(ValueError, match="expected 1 instances"):
        port.prepare(source, tmp_path / "output")

