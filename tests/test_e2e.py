"""End to end: build/vc in a pseudo-terminal on a real directory."""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent / "e2e"))
from vcterm import VC, VcSession  # noqa: E402

pytestmark = pytest.mark.skipif(not VC.exists(), reason="build/vc not built; run make")


@pytest.fixture
def session(tmp_path):
    work = tmp_path / "work"
    (work / "subdir").mkdir(parents=True)
    (work / "hello.txt").write_text("hello from linux\n")
    (work / "a long file name.markdown").write_text("# long\n")
    (work / "Кириллица.txt").write_text("привет\n")
    (work / ".hidden").write_text("secret\n")
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home)
    yield s
    s.close()


def test_panels_show_real_files(session):
    screen = session.wait_for("hello", timeout=15)
    assert "subdir" in screen.lower()
    assert "Кириллица" in screen


def test_f10_quits(session):
    session.wait_for("hello", timeout=15)
    session.send("f10")
    session.pump(0.3)
    session.send("enter")
    assert session.wait_exit() == 0
