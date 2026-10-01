"""The US VZ editor runs as a real DOS child, including F4 without $EDITOR.

Wait on the editor's own English prompts and inspect saved host bytes. The
shipped VZIBM.DEF binds Esc S to Save As and Esc Q to Quit; both ask before
committing their action. No test writes the edited text behind VZ's back.
"""
from contextlib import contextmanager
from pathlib import Path
import os
import resource
import shutil
import signal
import tempfile

import pytest

from test_gwbasic_e2e import ROOT, VcSession, panels, running_vc, select, until


VENDOR = ROOT / "third_party/vzeditor/VZ-IBM"
ORIGINAL = b"The first VZ line.\r\nThe second VZ line.\r\n"
ADDED = b"Written by VZ."
DEFAULT_DEFINITION = (VENDOR / "VZIBM.DEF").read_bytes().replace(b"\r\nEb-", b"\r\nEb+")


@pytest.fixture(autouse=True)
def no_host_editor(monkeypatch):
    # VcSession normally preserves the caller's EDITOR. These gates must
    # exercise the actual unset case even on a developer's configured tty.
    monkeypatch.delenv("EDITOR", raising=False)


@contextmanager
def configured_vc(*, work_name="work", config_name="config", tmp_name="tmp", files=None):
    """Exercise host path spelling without exceeding VC's command limit."""
    with tempfile.TemporaryDirectory(prefix="vz-", dir="/tmp") as directory:
        root = Path(directory)
        work, home, config, temporary = (root / name for name in
                                         (work_name, "home", config_name, tmp_name))
        for path in (work, home, config, temporary):
            path.mkdir()
        for name, contents in (files or {}).items():
            (work / name).write_bytes(contents)
        session = VcSession(work, home, extra_env={"XDG_CONFIG_HOME": str(config),
                                                  "TMPDIR": str(temporary)})
        try:
            session.wait_for("10Quit", timeout=15)
            until(session, lambda: panels(session.text()))
            yield session, work, config / "vc-linux", temporary
        finally:
            session.close()


def in_editor(session, first_line: str) -> None:
    try:
        until(session, lambda: first_line in session.text() and not panels(session.text())
              or session.poll() is not None, timeout=15)
    except AssertionError as error:
        raise AssertionError(f"{error}\n--- VZ log ---\n{session.log.read_text()}") from error
    assert session.poll() is None, session.log.read_text()
    assert "translation VZ.COM" in session.log.read_text(), session.log.read_text()
    assert first_line in session.text()
    assert "Customization error" not in session.text()


def new_vz_buffer(session, command=b"vz NEW.TXT") -> None:
    session.send(command, "enter")
    session.wait_for("not found. New file? (Y/N)")
    session.send(b"Y")
    until(session, lambda: not panels(session.text()) and "NEW.TXT" in session.text().upper()
          and "(Y/N)" not in session.text())
    assert session.poll() is None
    assert "translation VZ.COM" in session.log.read_text()


def type_line(session, line: bytes) -> None:
    # Keep each chunk below the original DOS keyboard buffer's capacity.
    start_row = session.screen.cursor.y
    for start in range(0, len(line), 8):
        session.send(line[start:start + 8])
        session.wait_for(line[:start + 8].decode("ascii"))
    session.send("enter")
    until(session, lambda: session.screen.cursor.y > start_row)


def save_vz(session, path, expected: bytes) -> None:
    session.send("esc", b"S")
    session.wait_for("Save As:")
    session.send("enter")
    try:
        until(session, lambda: path.exists() and path.read_bytes() == expected)
    except AssertionError as error:
        files = {entry.name: entry.read_bytes()[:160] for entry in path.parent.iterdir()
                 if entry.is_file()}
        raise AssertionError(f"{error}\nexpected {path.name}: {expected!r}\n"
                             f"actual host files: {files!r}\n--- VZ log ---\n"
                             f"{session.log.read_text()}") from error
    assert "Save As:" not in session.text()
    assert session.poll() is None


def quit_vz(session) -> None:
    session.send("esc", b"Q")
    session.wait_for("Quit from editor? (Y/N)")
    # An unexpected save prompt would mean the explicit save gate failed.
    assert "Save modified files" not in session.text()
    session.send(b"Y")
    until(session, lambda: panels(session.text()))
    assert session.text() == session.memory_text()
    assert session.poll() is None


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("editor", [None, ""], ids=["unset", "empty"])
def test_vz_f4_opens_first_line(editor, quick, monkeypatch):
    if editor is not None:
        monkeypatch.setenv("EDITOR", editor)
    with running_vc(quick=quick, files={"NOTE.TXT": ORIGINAL}) as (session, _, _):
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        quit_vz(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("editor", [None, ""], ids=["unset", "empty"])
def test_vz_f4_saves_and_quits(editor, quick, monkeypatch):
    if editor is not None:
        monkeypatch.setenv("EDITOR", editor)
    with running_vc(quick=quick, files={"NOTE.TXT": ORIGINAL}) as (session, work, _):
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        type_line(session, ADDED)
        # Ez+ in the unmodified upstream DEF writes the DOS EOF byte.
        expected = ADDED + b"\r\n" + ORIGINAL + b"\x1a"
        save_vz(session, work / "NOTE.TXT", expected)
        quit_vz(session)
        assert (work / "NOTE.TXT").read_bytes() == expected
        # A second EXEC proves VZ restored the parent's keyboard/vectors,
        # not just that the final screen happened to resemble the panels.
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, ADDED.decode())
        quit_vz(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("command", [b"vz NEW.TXT", b"VZ.COM NEW.TXT"])
def test_vz_typed_command_creates_new_file(quick, command):
    with running_vc(quick=quick) as (session, work, config):
        assert not (work / "VZ.COM").exists(), "the DOS PATH must find installed VZ"
        assert (config / "VZ.COM").read_bytes() == (VENDOR / "US/VZUS.COM").read_bytes()
        new_vz_buffer(session, command)
        assert not any(path.name.casefold() == "new.txt" for path in work.iterdir()), (
            "an open buffer is not yet a saved file")
        type_line(session, ADDED)
        expected = ADDED + b"\r\n\x1a"
        # The original Dp+ option lowercases VZ's own path before creation.
        save_vz(session, work / "new.txt", expected)
        quit_vz(session)
        assert (work / "new.txt").read_bytes() == expected


def test_vz_installed_def_files_and_menus_are_english():
    with running_vc(files={"NOTE.TXT": ORIGINAL}) as (session, _, config):
        for target, source in (("VZ.DEF", "VZIBM.DEF"), ("VZFLE.DEF", "VZFLE.DEF"),
                               ("HELPE.DEF", "HELPE.DEF")):
            expected = DEFAULT_DEFINITION if target == "VZ.DEF" else (VENDOR / source).read_bytes()
            assert (config / target).read_bytes() == expected
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        session.send("f1")
        session.wait_for("Save as")
        assert "Quit" in session.text() and "Open Files" in session.text()
        session.send("esc")
        until(session, lambda: "Save as" not in session.text())
        in_editor(session, "The first VZ line.")
        session.send(b"\x1b[24~")  # F12: the unmodified HELPE.DEF macro.
        session.wait_for("Screen Edit")
        assert "Cursor Move" in session.text() and "File/Window" in session.text()
        session.send("esc")
        until(session, lambda: "Screen Edit" not in session.text())
        quit_vz(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("missing", [False, True], ids=["installed", "missing"])
def test_vz_f4_filename_never_reaches_shell(quick, missing):
    name = "safe;touch PWNED;.txt"
    with running_vc(quick=quick, files={name: ORIGINAL}) as (session, work, config):
        expected = ORIGINAL
        if missing:
            (config / "VZ.COM").unlink()
        select(session, name)
        session.send("f4")
        if missing:
            until(session, lambda: "DOS command error 2" in session.log.read_text())
            until(session, lambda: panels(session.text()))
            assert "translation VZ.COM" not in session.log.read_text()
        else:
            in_editor(session, "The first VZ line.")
            type_line(session, ADDED)
            expected = ADDED + b"\r\n" + ORIGINAL + b"\x1a"
            save_vz(session, work / name, expected)
            quit_vz(session)
        assert session.poll() is None
        assert not (work / "PWNED").exists()
        assert not (config / "PWNED").exists()
        assert (work / name).read_bytes() == expected


@pytest.mark.parametrize("name,command", [("EDIT.COM", b"edit NOTE.TXT"),
                                          ("EDIT.EXE", b"eDiT.ExE NOTE.TXT")])
def test_vz_renamed_image_matches_by_bytes(name, command):
    with running_vc(files={"NOTE.TXT": ORIGINAL}) as (session, work, config):
        shutil.copyfile(config / "VZ.COM", work / name)
        # VZ derives its default DEF basename and directory from its own
        # DOS environment tail. A renamed distribution includes that DEF.
        for source in config.glob("*.DEF"):
            shutil.copyfile(source, work / ("EDIT.DEF" if source.name == "VZ.DEF" else source.name))
        (config / "VZ.COM").unlink()
        session.send(command, "enter")
        in_editor(session, "The first VZ line.")
        quit_vz(session)


@pytest.mark.parametrize("replacement", ["changed", "VC.COM", "BOOTLOGO.COM"])
def test_vz_changed_image_is_refused_and_vc_survives(replacement):
    with running_vc(files={"NOTE.TXT": ORIGINAL}) as (session, work, config):
        if replacement == "changed":
            data = bytearray((config / "VZ.COM").read_bytes())
            data[-1] ^= 1
        else:
            data = (config / replacement).read_bytes()
        (work / "VZ.COM").write_bytes(data)
        # F4's helper requires the VZ translation, not just any DOS child.
        # A typed command remains the generic byte-matching EXEC route.
        select(session, "NOTE.TXT")
        session.send("f4")
        until(session, lambda: "DOS command error 11" in session.log.read_text()
              or session.poll() is not None)
        assert session.poll() is None
        until(session, lambda: panels(session.text()))
        if replacement == "changed":
            assert "unsupported executable" in session.log.read_text().lower()
        assert "translation LOGO.COM" not in session.log.read_text()
        assert "translation VZ.COM" not in session.log.read_text()
        (work / "VZ.COM").unlink()
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        quit_vz(session)


def test_vz_f4_handles_configuration_path_with_dos_delimiters():
    # VZ's own path parser treats space, plus, and comma as separators.
    # Its COM/DEF path must be safe even when XDG_CONFIG_HOME contains all
    # three; altering just the user's text-file argument is insufficient.
    with configured_vc(config_name="settings +, with spaces", files={"NOTE.TXT": ORIGINAL}) as (
            session, work, config, _):
        assert (config / "VZ.DEF").read_bytes() == DEFAULT_DEFINITION
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        type_line(session, ADDED)
        save_vz(session, work / "NOTE.TXT", ADDED + b"\r\n" + ORIGINAL + b"\x1a")
        quit_vz(session)


def test_vz_f4_long_host_tmpdir_does_not_corrupt_dos_startup():
    # VZ has a 32-byte temporary-name buffer (not the usual DOS 128-byte
    # maximum). A legal long host TMPDIR must never be copied into it.
    with configured_vc(tmp_name="long-" + "t" * 80, files={"NOTE.TXT": ORIGINAL}) as (
            session, work, _, temporary):
        assert len(str(temporary)) > 80
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        type_line(session, ADDED)
        save_vz(session, work / "NOTE.TXT", ADDED + b"\r\n" + ORIGINAL + b"\x1a")
        quit_vz(session)


@pytest.mark.parametrize("change", ["insert", "delete"])
def test_vz_f4_short_alias_stays_bound_during_neighbor_changes(change):
    target, neighbor = "sameprefix-b.txt", "sameprefix-a.txt"
    other = b"The neighboring file must not be edited.\r\n"
    with running_vc(files={target: ORIGINAL, neighbor: other}) as (session, work, _):
        select(session, target)
        session.send("f4")
        in_editor(session, "The first VZ line.")
        # Both long names share SAMEPR~n.TXT. Changing the lexically first
        # sibling used to reassign the alias VZ already held in its buffer.
        if change == "insert":
            (work / "sameprefix-0.txt").write_bytes(other)
        else:
            (work / neighbor).unlink()
        type_line(session, ADDED)
        expected = ADDED + b"\r\n" + ORIGINAL + b"\x1a"
        save_vz(session, work / target, expected)
        quit_vz(session)
        assert (work / target).read_bytes() == expected
        if change == "insert":
            assert (work / neighbor).read_bytes() == other
            assert (work / "sameprefix-0.txt").read_bytes() == other
        else:
            assert not (work / neighbor).exists()
        assert not any(path.name.casefold().startswith("samepr~") and path.suffix.lower() == ".txt"
                       for path in work.iterdir()), (
            "saving a stale DOS alias must not create a second edited file")


def test_vz_f4_short_alias_stays_bound_when_parent_alias_changes():
    with configured_vc(work_name="sameprefix-b", files={"NOTE.TXT": ORIGINAL}) as (
            session, work, _, _):
        neighbor = work.parent / "sameprefix-a"
        neighbor.mkdir()
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        # The long parent directory, not the file's already-short basename,
        # changes from SAMEPR~2 to SAMEPR~1 while the editor holds the path.
        neighbor.rmdir()
        type_line(session, ADDED)
        expected = ADDED + b"\r\n" + ORIGINAL + b"\x1a"
        save_vz(session, work / "NOTE.TXT", expected)
        quit_vz(session)
        assert (work / "NOTE.TXT").read_bytes() == expected


@pytest.mark.parametrize("change", ["insert", "delete"])
def test_vz_typed_new_file_keeps_cwd_when_parent_alias_changes(change):
    with configured_vc(work_name="sameprefix-b") as (session, work, _, _):
        neighbor = work.parent / "sameprefix-a"
        neighbor.mkdir()
        new_vz_buffer(session)
        assert not (work / "new.txt").exists()
        if change == "insert":
            (work.parent / "sameprefix-0").mkdir()
        else:
            neighbor.rmdir()
        # There is no selected existing file to lease. VZ has already
        # expanded NEW.TXT using a short CWD; that directory must keep its
        # meaning even when its ~n alias would otherwise move to a sibling.
        type_line(session, ADDED)
        expected = ADDED + b"\r\n\x1a"
        save_vz(session, work / "new.txt", expected)
        quit_vz(session)
        assert (work / "new.txt").read_bytes() == expected
        assert not (neighbor / "new.txt").exists()
        assert not (work.parent / "sameprefix-0/new.txt").exists()


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("name", ["NOTE.TXT", "sameprefix-b.txt"])
def test_vz_backup_save_keeps_original_and_edited_file(quick, name):
    neighbor = b"Do not change the neighboring long filename.\r\n"
    with running_vc(quick=quick, files={name: ORIGINAL, "sameprefix-a.txt": neighbor}) as (
            session, work, config):
        # Explicitly enable backups so this exercises the lease repair even
        # against a binary whose installed default still has Eb-.
        definition = (config / "VZ.DEF").read_bytes()
        (config / "VZ.DEF").write_bytes(definition.replace(b"\r\nEb-", b"\r\nEb+"))
        select(session, name)
        session.send("f4")
        in_editor(session, "The first VZ line.")
        type_line(session, ADDED)
        expected = ADDED + b"\r\n" + ORIGINAL + b"\x1a"
        save_vz(session, work / name, expected)
        backups = [path for path in work.iterdir() if path.suffix.lower() == ".bak"]
        assert len(backups) == 1
        assert backups[0].read_bytes() == ORIGINAL
        if name == "NOTE.TXT":
            assert backups[0].name.upper() == "NOTE.BAK"
        assert (work / "sameprefix-a.txt").read_bytes() == neighbor
        assert not any("~" in path.name and path.suffix.lower() == ".txt"
                       for path in work.iterdir()), "the save must retain the long host basename"
        quit_vz(session)
        assert (work / name).read_bytes() == expected


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("name", ["NOTE.TXT", "sameprefix-b.txt"])
def test_vz_external_replacement_still_saves(quick, name):
    replacement = b"A newer version from an external atomic replacement.\r\n"
    with running_vc(quick=quick, files={name: ORIGINAL}) as (session, work, _):
        select(session, name)
        session.send("f4")
        in_editor(session, "The first VZ line.")
        path = work / name
        inode = path.stat().st_ino
        staging = work / "replacement.tmp"
        staging.write_bytes(replacement)
        staging.replace(path)
        assert path.stat().st_ino != inode
        type_line(session, ADDED)
        expected = ADDED + b"\r\n" + ORIGINAL + b"\x1a"
        save_vz(session, path, expected)
        backups = [entry for entry in work.iterdir() if entry.suffix.lower() == ".bak"]
        assert len(backups) == 1
        assert backups[0].read_bytes() == replacement, "the external version must remain recoverable"
        quit_vz(session)
        assert path.read_bytes() == expected


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("command", [b"vz NOTES.TXT", b"vz", None], ids=["typed-file", "typed-empty", "f4"])
def test_vz_deep_current_directory_refused_before_editor(quick, command):
    with running_vc(quick=quick) as (session, _, _):
        # H:\ plus six native 8.3 directory components is 56 bytes. Even a
        # short file argument can overflow makefulpath's 64-byte destination.
        deep = session.log.parent.joinpath(*(["DEPTH000"] * 6))
        deep.mkdir(parents=True)
        note = deep / ("AA.TXT" if command is None else "NOTES.TXT")
        note.write_bytes(ORIGINAL)
        session.send(("cd " + str(deep)).encode(), "enter")
        until(session, lambda: panels(session.text()) and note.stem in session.text())
        if command is None:
            select(session, note.name)
        frames = []
        feed = session.stream.feed

        def observe(data):
            for byte in data:
                feed(bytes([byte]))
                if byte == ord("Z"):
                    frames.append(session.text())

        session.stream.feed = observe
        try:
            if command is None:
                session.send("f4")
            else:
                session.send(command, "enter")
            message = "Current directory too long for VZ"
            until(session, lambda: any(message in frame for frame in frames)
                  or "translation VZ.COM" in session.log.read_text()
                  or session.poll() is not None)
            assert any(message in frame for frame in frames), session.text()
            assert "translation VZ.COM" not in session.log.read_text()
            until(session, lambda: panels(session.text()))
            assert session.poll() is None
            assert note.read_bytes() == ORIGINAL
        finally:
            session.stream.feed = feed


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_vz_failed_partial_save_keeps_original_backup(quick, monkeypatch):
    # Ignore SIGXFSZ only in the forked VC, allowing the real host write to
    # return short/EFBIG. Apply the quota after installation and editing, so
    # this cannot accidentally test a failed config install or VZ read.
    original_execve = os.execve

    def ignore_file_size_signal(*args):
        signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
        original_execve(*args)

    monkeypatch.setattr(os, "execve", ignore_file_size_signal)
    original = ORIGINAL + b"Untouched original tail.\r\n" * 3000
    expected = ADDED + b"\r\n" + original + b"\x1a"
    limit = 32_768
    assert len(original) > limit
    with running_vc(quick=quick, files={"NOTE.TXT": original}) as (session, work, _):
        select(session, "NOTE.TXT")
        session.send("f4")
        in_editor(session, "The first VZ line.")
        type_line(session, ADDED)
        previous_limit = resource.prlimit(session.pid, resource.RLIMIT_FSIZE)
        resource.prlimit(session.pid, resource.RLIMIT_FSIZE, (limit, previous_limit[1]))
        try:
            session.send("esc", b"S")
            session.wait_for("Save As:")
            session.send("enter")
            session.wait_for("Disk full.")
            assert session.poll() is None
            actual = (work / "NOTE.TXT").read_bytes()
            assert actual[:limit] == expected[:limit], "the quota must fail after a real partial write"
            backups = [path for path in work.iterdir() if path.name.upper() == "NOTE.BAK"]
            assert len(backups) == 1, "a failed F4 save must retain NOTE.BAK"
            assert backups[0].read_bytes() == original
            assert len(actual) == limit, "the failed new file must not retain an old suffix"
        finally:
            if session.poll() is None:
                resource.prlimit(session.pid, resource.RLIMIT_FSIZE, previous_limit)
