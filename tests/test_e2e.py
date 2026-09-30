"""End to end: build/vc in a pseudo-terminal on a real directory.

Every file operation is checked on disk, never only on screen. The screen the
terminal shows must also equal what VC wrote into video memory.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent / "e2e"))
from vcterm import VC, VcSession  # noqa: E402

pytestmark = pytest.mark.skipif(not VC.exists(), reason="build/vc not built; run make")

STATUS_ROW = 21  # the panel line under the file list that names the current file


@pytest.fixture
def work(tmp_path):
    w = tmp_path / "work"
    (w / "subdir").mkdir(parents=True)
    (w / "target").mkdir()
    (w / "hello.txt").write_text("hello from linux\n")
    (w / "a long file name.markdown").write_text("# long\n")
    (w / "Кириллица.txt").write_text("привет\n")
    (w / ".hidden").write_text("secret\n")
    return w


@pytest.fixture
def vc(work, tmp_path):
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home)
    s.wait_for("10Quit", timeout=15)
    yield s
    s.close()


def status(s: VcSession) -> str:
    return s.text().splitlines()[STATUS_ROW]


def select(s: VcSession, name: str) -> None:
    """Move the active panel's cursor onto `name`. The status line shows a
    long name by its last characters after a left arrow, so match the tail."""
    tail = name[-10:]
    s.send("home")
    seen = []
    for _ in range(12):
        s.pump(0.3)
        seen.append(status(s))
        if tail in seen[-1]:
            return
        s.send("down")
    raise AssertionError(f"{name!r} not reachable\n" + "\n".join(seen) + "\n" + s.text())


def until(s: VcSession, check, timeout: float = 8.0) -> None:
    """Pump the terminal until check() holds. VC animates its boxes, so fixed
    sleeps are either slow or flaky."""
    import time
    end = time.time() + timeout
    while time.time() < end:
        s.pump(0.1)
        if check():
            return
    raise AssertionError(f"condition not met after {timeout}s\n{s.text()}")


def other_panel_to(s: VcSession, path: Path) -> None:
    s.send("tab")
    s.send(f"cd {path}".encode(), "enter")
    s.wait_for(f"{path.name}>", timeout=5)
    s.send("tab")
    s.pump(0.5)


def test_panels_show_real_files(vc):
    screen = vc.text()
    for name in ("hello.txt", "subdir", "Кириллица", "a long file"):
        assert name in screen


def test_terminal_matches_video_memory(vc):
    vc.pump(0.5)
    assert vc.text() == vc.memory_text()


def test_shell_command_with_redirect(vc, work):
    vc.send(b"echo made by vc > made.txt", "enter")
    vc.wait_for("10Quit", timeout=10)
    assert (work / "made.txt").read_text() == "made by vc\n"


def test_cd_moves_the_panel(vc, work):
    vc.send(b"cd subdir", "enter")
    vc.wait_for("subdir>", timeout=5)


def test_view_file(vc):
    select(vc, "hello.txt")
    vc.send("f3")
    vc.wait_for("hello from linux", timeout=5)
    vc.send("esc")
    vc.wait_for("10Quit", timeout=5)


def test_make_directory(vc, work):
    vc.send("f7")
    vc.wait_for("Make directory", timeout=5)
    vc.send(b"newdir", "enter")
    vc.wait_for("newdir", timeout=5)
    assert (work / "newdir").is_dir()


def test_copy_to_other_panel(vc, work):
    other_panel_to(vc, work / "target")
    select(vc, "hello.txt")
    vc.send("f5")
    vc.wait_for('Copy "hello.txt" to', timeout=5)
    vc.send("enter")
    until(vc, lambda: (work / "target" / "hello.txt").exists())
    assert sorted(p.name for p in (work / "target").iterdir()) == ["hello.txt"]
    assert (work / "target" / "hello.txt").read_text() == "hello from linux\n"
    assert (work / "hello.txt").exists()


def test_copy_keeps_long_and_cyrillic_names(vc, work):
    other_panel_to(vc, work / "target")
    for name in ("a long file name.markdown", "Кириллица.txt"):
        select(vc, name)
        vc.send("f5")
        vc.wait_for("Copy", timeout=5)
        vc.send("enter")
        until(vc, lambda: (work / "target" / name).exists())
        vc.wait_for("10Quit", timeout=5)
        assert (work / "target" / name).read_text() == (work / name).read_text()


def test_rename(vc, work):
    select(vc, "hello.txt")
    vc.send("f6")
    vc.wait_for("Rename or move", timeout=5)
    vc.send(b"renamed.txt", "enter")  # the first key replaces the proposed path
    until(vc, lambda: not (work / "hello.txt").exists())
    assert sorted(p.name for p in work.iterdir() if p.suffix == ".txt" and p.name[0].isascii()) == ["renamed.txt"], vc.text()
    assert (work / "renamed.txt").read_text() == "hello from linux\n"
    assert not (work / "hello.txt").exists()


def test_delete(vc, work):
    select(vc, "hello.txt")
    vc.send("f8")
    vc.wait_for("Delete", timeout=5)
    vc.send("enter")
    until(vc, lambda: not (work / "hello.txt").exists())
    assert not (work / "hello.txt").exists()


def test_f10_quits(vc):
    vc.send("f10")
    vc.pump(0.3)
    vc.send("enter")
    assert vc.wait_exit() == 0
