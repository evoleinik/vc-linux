"""End to end: build/vc in a pseudo-terminal on a real directory.

Every file operation is checked on disk, never only on screen. The screen the
terminal shows must also equal what VC wrote into video memory.
"""
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent / "e2e"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from vcterm import VC, VcSession  # noqa: E402
import vcini  # noqa: E402

DATA = Path(__file__).resolve().parents[1] / "data"

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


def lossy_suffix(host_name: str) -> str:
    """The ~XXXX runtime/dos_fs.c appends to a name DOS cannot spell: FNV-1a
    over the UTF-8 bytes, XOR-folded to 16 bits."""
    h = 2166136261
    for b in host_name.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return "~%04X" % ((h ^ (h >> 16)) & 0xFFFF)


def status(s: VcSession) -> str:
    return s.text().splitlines()[STATUS_ROW]


def select(s: VcSession, name: str) -> None:
    """Move the active panel's cursor onto `name`. The status line shows a
    long name by its last characters after a left arrow, so match the tail.
    Each key waits for the status line to change, so load cannot outrun it."""
    import time
    tail = name[-10:]
    s.send("home")
    s.pump(0.3)
    seen = [status(s)]
    for _ in range(12):
        if tail in seen[-1]:
            return
        s.send("down")
        end = time.time() + 3
        while time.time() < end:
            s.pump(0.05)
            if status(s) != seen[-1]:
                break
        seen.append(status(s))
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


def confirm_until(s: VcSession, done, timeout: float = 10.0) -> None:
    """Press Enter on each confirmation box VC shows until done() holds.
    VC may ask once for the group and again per file, and it zooms each box
    in, so a key sent on a timer can land before the box can take it."""
    import time
    end = time.time() + timeout
    answered = None
    while time.time() < end:
        s.pump(0.1)
        if done():
            return
        box = [line for line in s.text().splitlines() if "Delete" in line and "║" in line]
        if box and box != answered:
            s.pump(0.3)  # let the zoom finish
            s.send("enter")
            answered = box
    raise AssertionError(f"not done after {timeout}s\n{s.text()}")


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


def fake_editor(tmp_path: Path) -> Path:
    editor = tmp_path / "fake-editor"
    editor.write_text('#!/bin/sh\nprintf "%s\\n" "$1" > "$0.args"\necho "edited" >> "$1"\n')
    editor.chmod(0o755)
    return editor


def run_f4(work: Path, tmp_path: Path, name: str, quick_execute: bool = False, shown: str = "") -> Path:
    """Press F4 on `name` with a stub $EDITOR and return what the editor got."""
    editor = fake_editor(tmp_path)
    home = tmp_path / "home"
    config = home / ".config" / "vc-linux"
    config.mkdir(parents=True)
    if quick_execute:  # VC then runs commands through INT 2Eh instead of COMSPEC /C
        ini = vcini.VcIni((DATA / "VC.INI").read_bytes())
        ini.set_main("ExecTyp", 1)
        (config / "VC.INI").write_bytes(ini.data)
    s = VcSession(work, home, extra_env={"EDITOR": str(editor)})
    try:
        s.wait_for("10Quit", timeout=15)
        select(s, shown or name)
        s.send("f4")
        until(s, lambda: "edited" in (work / name).read_text())
        s.wait_for("10Quit", timeout=5)
    finally:
        s.close()
    return Path(Path(str(editor) + ".args").read_text().rstrip("\n"))


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("name", ["a long file name.markdown", "$(touch pwned).txt"])
def test_f4_opens_EDITOR_with_the_exact_file(work, tmp_path, name, quick):
    # VC 4.99.09's own editor is switched off in its source (VCEDIT.INC jumps
    # straight to "Can't find the file"), so F4 goes through VCEDIT.EXT. The
    # file must reach the editor as one argument that no shell has parsed.
    (work / name).write_text("text\n")
    assert run_f4(work, tmp_path, name, quick) == work / name
    assert not (work / "pwned").exists()


def test_f4_keeps_a_trailing_space(work, tmp_path):
    # DOS drops trailing spaces, so "note.txt " shows as note~XXXX.txt■ and
    # must still open itself, never note.txt.
    (work / "note.txt").write_text("plain\n")
    (work / "note.txt ").write_text("spaced\n")
    assert run_f4(work, tmp_path, "note.txt ", shown=".txt■") == work / "note.txt "
    assert (work / "note.txt").read_text() == "plain\n"


def test_f4_refuses_a_command_that_may_have_been_cut(work, tmp_path):
    # The review's case: "/C vc-edit " plus a 116-byte name passes DOS's 126
    # bytes, VC cuts it, and the cut name matched a different file.
    stem = "a" * 106
    (work / (stem + ".txt ")).write_text("the one pressed\n")   # shows as <stem>~XXXX.txt■
    victim = work / (stem + lossy_suffix(stem + ".txt ") + ".txt")
    victim.write_text("must stay untouched\n")
    editor = fake_editor(tmp_path)
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home, extra_env={"EDITOR": str(editor)})
    try:
        s.wait_for("10Quit", timeout=15)
        select(s, ".txt■")
        s.send("f4")
        s.wait_for("10Quit", timeout=5)
        s.pump(1.5)
    finally:
        s.close()
    assert not Path(str(editor) + ".args").exists()
    assert victim.read_text() == "must stay untouched\n"


def test_retired_unsafe_defaults_are_replaced(work, tmp_path):
    home = tmp_path / "home"
    config = home / ".config" / "vc-linux"
    config.mkdir(parents=True)
    (config / "VCEDIT.EXT").write_bytes(b'*: ${EDITOR:-vi} "!.!"\r\n')  # shipped by an early build
    (config / "VC.EXT").write_bytes(b"zip: my own entry\r\n")          # edited by the user
    s = VcSession(work, home)
    try:
        s.wait_for("10Quit", timeout=15)
    finally:
        s.close()
    assert (config / "VCEDIT.EXT").read_bytes() == (DATA / "VCEDIT.EXT").read_bytes()
    assert (config / "VC.EXT").read_bytes() == b"zip: my own entry\r\n"


def test_cd_accepts_a_quoted_path(vc, work):
    (work / "sub dir").mkdir()
    vc.send(b'cd "sub dir"', "enter")
    vc.wait_for("sub dir>", timeout=5)


def test_cd_to_an_oversized_path_leaves_vc_working(work, tmp_path):
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home, extra_env={"HOME": "/" + "h" * 300})
    try:
        s.wait_for("10Quit", timeout=15)
        s.send(b"cd", "enter")
        s.wait_for("10Quit", timeout=5)
        s.send("f7")
        s.wait_for("Make directory", timeout=5)
        s.send(b"after", "enter")
        until(s, lambda: (work / "after").is_dir())
    finally:
        s.close()


def test_config_dir_with_cyrillic_name(work, tmp_path):
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home, extra_env={"XDG_CONFIG_HOME": str(tmp_path / "конфиг")})
    try:
        s.wait_for("10Quit", timeout=15)
        assert "hello.txt" in s.text()
    finally:
        s.close()


def test_home_is_drive_h(tmp_path):
    home = tmp_path / "home"
    (home / "projects" / "demo").mkdir(parents=True)
    (home / "projects" / "demo" / "readme.txt").write_text("in home\n")
    s = VcSession(home / "projects" / "demo", home)
    try:
        s.wait_for("10Quit", timeout=15)
        s.wait_for("H:\\projects\\demo>", timeout=5)   # started under $HOME: drive H:
        assert "readme.txt" in s.text()
        s.send(b"cd /", "enter")
        s.wait_for("C:\\>", timeout=5)                     # / is drive C:
        s.send(b"cd ~", "enter")
        s.wait_for("H:\\>", timeout=5)                     # ~ is the root of H:
    finally:
        s.close()


def test_refuses_to_run_without_a_terminal(work):
    import subprocess
    r = subprocess.run([str(VC)], cwd=work, stdin=subprocess.DEVNULL, capture_output=True, timeout=10)
    assert r.returncode == 2
    assert b"needs a terminal" in r.stderr


def test_delete_directory_symlink_keeps_the_target(work, tmp_path):
    (work / "real").mkdir()
    (work / "real" / "precious.txt").write_text("keep me\n")
    (work / "linkdir").symlink_to("real")
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home)
    try:
        s.wait_for("10Quit", timeout=15)
        select(s, "linkdir")
        s.send("f8")
        s.wait_for("Delete", timeout=5)
        s.send("enter")
        until(s, lambda: not (work / "linkdir").is_symlink())
        s.pump(1.0)
    finally:
        s.close()
    assert (work / "real" / "precious.txt").read_text() == "keep me\n"


def test_delete_second_of_two_emoji_names(work, tmp_path):
    (work / "face-😀.txt").write_text("first\n")
    (work / "face-😃.txt").write_text("second\n")
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home)
    try:
        s.wait_for("10Quit", timeout=15)
        # each shows as face-■~XXXX.txt, XXXX from a hash of its own name
        select(s, lossy_suffix("face-😃.txt") + ".txt")
        s.send("f8")
        s.wait_for("Delete", timeout=5)
        s.send("enter")
        until(s, lambda: len(list(work.glob("face-*.txt"))) == 1)
    finally:
        s.close()
    assert [p.read_text() for p in work.glob("face-*.txt")] == ["first\n"]


def test_group_delete_of_emoji_names_spares_the_unselected(work, tmp_path):
    # The review's scenario: numbering by position made the third file take the
    # second one's name mid-delete, so the unselected file went.
    names = ["face-😀.txt", "face-😃.txt", "face-😄.txt"]
    for n in names:
        (work / n).write_text(n + "\n")
    by_suffix = {lossy_suffix(n): n for n in names}
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home)
    try:
        s.wait_for("10Quit", timeout=15)
        shown = [suf for line in s.text().splitlines() for suf in by_suffix if suf in line[40:]]
        assert len(shown) == 3, s.text()
        select(s, shown[0] + ".txt")
        s.send("ins", "ins")  # Ins selects and moves down: the first two shown
        s.pump(0.3)
        s.send("f8")
        confirm_until(s, lambda: len(list(work.glob("face-*.txt"))) == 1)
    finally:
        s.close()
    assert [p.name for p in work.glob("face-*.txt")] == [by_suffix[shown[2]]]


def test_delete_symlink_to_ancestor(work, tmp_path):
    (work / "up").symlink_to("..")
    home = tmp_path / "home"
    home.mkdir()
    s = VcSession(work, home)
    try:
        s.wait_for("10Quit", timeout=15)
        select(s, "up")
        s.send("f8")
        s.wait_for("Delete", timeout=5)
        s.send("enter")
        until(s, lambda: not (work / "up").is_symlink())
    finally:
        s.close()
    assert (work / "hello.txt").exists()


def test_mouse_click_moves_the_cursor(vc):
    vc.pump(1.0)  # VC resets the mouse late in start-up; an earlier click is lost, as in DOS
    lines = vc.text().splitlines()
    y = next(i for i, line in enumerate(lines) if "subdir" in line[40:])
    x = lines[y].index("subdir", 40)
    vc.send(f"\x1b[<0;{x + 2};{y + 1}M\x1b[<0;{x + 2};{y + 1}m".encode())  # SGR press, release
    until(vc, lambda: "subdir" in status(vc)[40:])


def test_f10_quits(vc):
    vc.send("f10")
    vc.pump(0.3)
    vc.send("enter")
    assert vc.wait_exit() == 0
