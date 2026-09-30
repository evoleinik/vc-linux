"""The default VC.INI shipped in data/ must load in VC and suit Linux."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import vcini  # noqa: E402


def load() -> vcini.VcIni:
    return vcini.VcIni((ROOT / "data" / "VC.INI").read_bytes())


def test_checksum_valid():
    # VC rejects the whole file with "Incorrect data in the setup file" otherwise.
    assert load().checksum_ok


def test_lowercase_short_names_off():
    # With ConvCase on, VC uppercases every 8.3 target name before creating it,
    # so copying or renaming to hello.txt produced HELLO.TXT on Linux.
    assert load().main("ConvCase") == 0


def test_both_panels_with_long_names():
    ini = load()
    assert [ini.panel(p, "Visible") for p in (0, 1)] == [1, 1]
    assert [ini.panel(p, "LongNames") for p in (0, 1)] == [1, 1]
