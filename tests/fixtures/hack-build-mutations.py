"""Private mutation probes: every failure is an intentionally broken gate."""
import os
from pathlib import Path
import shutil
import sys
import types

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests"))
import test_hack_build as gate

FILES = (ROOT / "build/hack/HACK.EXE", ROOT / "build/hack/HACK.MAP")
MUTATE = os.environ.get("HACK_MUTANT", "0") == "1"

def broken_routine(monkeypatch, symbol, code):
    if not MUTATE:
        return
    original = gate.DosRoutine
    class Broken(original):
        def __init__(self, files):
            super().__init__(files)
            self.uc.mem_write(self.address(symbol), code)
    monkeypatch.setattr(gate, "DosRoutine", Broken)

def test_full_width_rng(monkeypatch):
    broken_routine(monkeypatch, "random_", bytes.fromhex("31d2cb"))
    gate.test_hack_dos_random_uses_full_bsd_sequence(FILES)

def test_ibm_wall_glyph(monkeypatch):
    broken_routine(monkeypatch, "hack_mapchar_", bytes.fromhex("89d8cb"))
    gate.test_hack_dos_wall_glyphs_do_not_change_items(FILES, 1, "-", 196)

def test_saved_monster_segments(monkeypatch):
    broken_routine(monkeypatch, "hack_monster_rebase_", bytes.fromhex("cb"))
    gate.test_hack_dos_saved_monsters_rebase_full_far_pointer(FILES, 0x500)

def test_saved_timeout_callback(monkeypatch):
    broken_routine(monkeypatch, "hack_restore_you_", bytes.fromhex("cb"))
    gate.test_hack_dos_save_sickness_text_and_rebase_timeout(FILES)

def test_corrupt_save_validation(monkeypatch):
    broken_routine(monkeypatch, "uptodate_", bytes.fromhex("b80100cb"))
    gate.test_hack_dos_persistent_file_integrity(FILES, "body")

def test_pinned_executable():
    raw = bytearray(FILES[0].read_bytes())
    if MUTATE:
        raw[-1] ^= 1
    gate.verified_image(raw)

def test_exact_original_sources(tmp_path, monkeypatch):
    shutil.copytree(ROOT / "third_party/hack", tmp_path / "third_party/hack")
    (tmp_path / "runtime/hack_dos").mkdir(parents=True)
    shutil.copyfile(ROOT / "runtime/hack_dos/UPSTREAM.sha256",
                    tmp_path / "runtime/hack_dos/UPSTREAM.sha256")
    if MUTATE:
        source = tmp_path / "third_party/hack/data"
        source.write_bytes(source.read_bytes() + b"\nplanted mutation\n")
    monkeypatch.setattr(gate, "ROOT", tmp_path)
    gate.test_hack_vendor_is_exact_92_file_snapshot()

def test_checked_patch_guards(tmp_path, monkeypatch):
    if MUTATE:
        monkeypatch.setattr(gate, "load_port",
            lambda: types.SimpleNamespace(prepare=lambda *args: None))
    gate.test_hack_port_refuses_changed_upstream(tmp_path, "duplicate")

def test_reject_compiler_clock(tmp_path, monkeypatch):
    if MUTATE:
        monkeypatch.setattr(gate, "load_port",
            lambda: types.SimpleNamespace(prepare=lambda *args: None))
    gate.test_hack_port_refuses_changed_upstream(tmp_path, "clock")

def test_restore_parent_drive_and_directories(monkeypatch):
    if MUTATE:
        original = gate.DosRoutine.call
        def broken_call(machine, name, **kwargs):
            if name == "restore_directory":
                machine.uc.mem_write(machine.address(name), b"\xcb")
            return original(machine, name, **kwargs)
        monkeypatch.setattr(gate.DosRoutine, "call", broken_call)
    gate.test_hack_dos_exit_restores_all_changed_directories(FILES, 3, "supported")
