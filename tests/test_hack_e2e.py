"""Hack's real 8086 EXE, launched by VC in a pty and observed through pyte.

The gates follow visible prompts, use unoccupied map cells for movement, and
compare a moved game's complete map/status across DOS process lifetimes.
"""

from contextlib import ExitStack, contextmanager
from pathlib import Path
import os
import re
import shutil
import signal
import tempfile

import pytest

from test_gwbasic_e2e import ROOT, panels, running_vc, select, until as until_screen
from vcterm import VcSession


STATUS = re.compile(r"Level\s+(\d+)\s+Gold\s+\d+\s+Hp\s+\d+\(\d+\)\s+Ac\s+-?\d+\s+Str\s+\S+\s+Exp\s+\d+")
MOVES = (("h", 0, -1), ("j", 1, 0), ("k", -1, 0), ("l", 0, 1),
         ("y", -1, -1), ("u", -1, 1), ("b", 1, -1), ("n", 1, 1))


def until(session, check, timeout=15):
    try:
        until_screen(session, check, timeout=timeout)
    except AssertionError as error:
        raise AssertionError(f"{error}\n--- VC log ---\n{session.log.read_text()}") from None


def players(text):
    return [(row, column) for row, line in enumerate(text.splitlines())
            if 1 <= row <= 22
            for column, char in enumerate(line) if char == "@"]


def dungeon(text, *, first_level=False):
    status = STATUS.search(text)
    return bool(status and (not first_level or status[1] == "1")
                and len(players(text)) == 1
                and any(char in ".#+-|│─┌┐└┘" for line in text.splitlines()[1:23]
                        for char in line))


def wait_dungeon(session, *, first_level=False):
    # Original Hack asks about experience, then class, on a new plain launch.
    # A command-line -C and a successful restore skip these questions.
    for _ in range(8):
        until(session, lambda: dungeon(session.text(), first_level=first_level)
              or "--More--" in session.text()
              or "experienced player?" in session.text()
              or "what kind of character" in session.text()
              or session.poll() is not None, timeout=25)
        assert session.poll() is None, session.log.read_text()
        text = session.text()
        if "--More--" in text:
            session.send(b" ")
        elif dungeon(text, first_level=first_level):
            until(session, lambda: session.text() == session.memory_text())
            return
        elif "what kind of character" in text:
            session.send(b"C")
        elif "experienced player?" in text:
            session.send(b"y")
    raise AssertionError(f"Hack did not finish its startup prompts\n{session.text()}")


def start_hack(session, command=b"hack103 -C", *, first_level=True):
    before = len(session.log.read_text())
    session.send(command, "enter")
    until(session, lambda: "translation HACK.EXE" in session.log.read_text()[before:]
          or "run: " in session.log.read_text()[before:]
          or "DOS command error" in session.log.read_text()[before:]
          or session.poll() is not None)
    assert "translation HACK.EXE" in session.log.read_text()[before:]
    wait_dungeon(session, first_level=first_level)
    assert "translation HACK.EXE" in session.log.read_text()[before:]


def game_state(text):
    # Messages on row 0 differ on restore; every map row and bottom status
    # row must match, including the pet, visible objects, HP and experience.
    return "\n".join(text.splitlines()[1:])


def move_player(session):
    before = session.text()
    row, column = players(before)[0]
    lines = before.splitlines()
    choice = next(((key, row + dy, column + dx)
                   for terrain in (".", "#<>", ")!?[/%=*")
                   for key, dy, dx in MOVES
                   if 1 <= row + dy <= 22 and 0 <= column + dx < len(lines[row + dy])
                   and lines[row + dy][column + dx] in terrain), None)
    assert choice is not None, f"Hack has no visible unoccupied neighboring tile\n{before}"
    key, next_row, next_column = choice
    session.send(key.encode("ascii"))
    for _ in range(8):
        until(session, lambda: "--More--" in session.text()
              or dungeon(session.text()) and players(session.text()) != [(row, column)]
              or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        if "--More--" not in session.text():
            break
        session.send(b" ")
    assert players(session.text()) != [(row, column)], session.text()
    # A hidden teleport trap is the only legitimate different destination.
    if players(session.text()) != [(next_row, next_column)]:
        assert (session.text().splitlines()[next_row][next_column] == "^"
                or STATUS.search(session.text())[1] != STATUS.search(before)[1]), session.text()
    until(session, lambda: session.text() == session.memory_text())
    return (row, column), players(session.text())[0]


def quit_hack(session):
    before = len(session.log.read_text())
    session.send(b"Q")
    until(session, lambda: "Really quit?" in session.text())
    assert not panels(session.text())
    session.send(b"y")
    for _ in range(8):
        until(session, lambda: panels(session.text()) or "--More--" in session.text()
              or "Hit space" in session.text() or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        if panels(session.text()):
            break
        session.send(b" ")
    assert panels(session.text()), session.text()
    until(session, lambda: session.text() == session.memory_text())
    log = session.log.read_text()[before:]
    assert "no translated code" not in log.lower(), log
    exits = re.findall(r"terminate psp [0-9A-F]+ code (\d+)\b", log)
    assert exits and all(code == "0" for code in exits), log


def save_hack(session):
    session.send(b"S")
    until(session, lambda: panels(session.text()) or session.poll() is not None)
    assert session.poll() is None, session.log.read_text()
    assert panels(session.text()), session.text()


@contextmanager
def shared_installation(*, work_name="work", config_name="cfg"):
    """Real simultaneous PTYs, one XDG install, and a HOME that stays untouched."""
    with tempfile.TemporaryDirectory(prefix="vch-", dir="/tmp") as temporary, ExitStack() as cleanup:
        root = Path(temporary)
        work, user_home, config = root / work_name, root / "home", root / config_name / "vc-linux"
        work.mkdir()
        user_home.mkdir()
        sentinel = user_home / "OWNED.TXT"
        sentinel.write_bytes(b"the user's file\n")
        before = sentinel.stat()
        count = 0

        def launch():
            nonlocal count
            count += 1
            session = VcSession(work, user_home, extra_env={
                "XDG_CONFIG_HOME": str(config.parent),
                "XDG_CACHE_HOME": str(root / "cache"),
                "VC_LOG": str(root / f"vc{count}.log"),
                "VC_SCREEN_DUMP": str(root / f"screen{count}.txt"),
                "VC_FRAME_DUMP": str(root / f"frame{count}.pgm"),
            })
            cleanup.callback(session.close)
            session.wait_for("10Quit", timeout=15)
            until(session, lambda: panels(session.text()))
            return session

        yield launch, work, config
        assert sorted(path.name for path in user_home.iterdir()) == ["OWNED.TXT"]
        after = sentinel.stat()
        assert (after.st_ino, after.st_size, after.st_mtime_ns) == (
            before.st_ino, before.st_size, before.st_mtime_ns)
        assert sentinel.read_bytes() == b"the user's file\n"


@pytest.mark.parametrize("loader", (False, True), ids=("native", "dos2-loader"))
def test_hack_concurrent_processes_have_independent_playgrounds_and_saves(loader):
    with shared_installation() as (launch, _, config):
        first, second = launch(), launch()
        if loader:
            # COMMAND.COM's own loader takes classic DOS paths. Changing
            # directory through VC avoids feeding it long host ancestors.
            for session in (first, second):
                session.send(b"cd " + str(config / "HACK").encode(), "enter")
                until(session, lambda: panels(session.text())
                      and "\\HACK>" in session.text().splitlines()[23])
        command = b"dos2 /c HACK.EXE" if loader else b"hack103"
        start_hack(first, command + b" -C")
        move_player(first)
        first_state = game_state(first.text())
        start_hack(second, command + b" -C")
        move_player(second)
        move_player(second)
        second_state = game_state(second.text())
        assert first_state != second_state
        save_hack(first)
        first_save = config / "HACK/HACK.SAV"
        saved_bytes = first_save.read_bytes()
        save_hack(second)
        saves = sorted((config / "HACK").rglob("HACK.SAV"))
        assert len(saves) == 2, "simultaneous Hack games must not overwrite one shared save"
        assert first_save.read_bytes() == saved_bytes
        for save in saves:
            assert save.stat().st_size > 4096
            assert (save.parent / "HACK.EXE").read_bytes() == (ROOT / "build/hack/HACK.EXE").read_bytes()
        start_hack(first, command, first_level=False)
        until(first, lambda: game_state(first.text()) == first_state)
        start_hack(second, command, first_level=False)
        until(second, lambda: game_state(second.text()) == second_state)
        assert not list((config / "HACK").rglob("HACK.SAV"))
        quit_hack(first)
        quit_hack(second)


def test_hack_concurrent_slot_save_survives_process_death_and_is_reused():
    with shared_installation() as (launch, _, config):
        first, second = launch(), launch()
        start_hack(first)
        first_state = game_state(first.text())
        start_hack(second)
        move_player(second)
        second_state = game_state(second.text())
        save_hack(second)
        saved = list((config / "HACK").rglob("HACK.SAV"))
        assert len(saved) == 1 and saved[0].parent != config / "HACK"
        saved_bytes = saved[0].read_bytes()
        os.kill(second.pid, signal.SIGKILL)
        assert second.wait_exit() == -signal.SIGKILL
        replacement = launch()
        assert saved[0].read_bytes() == saved_bytes, "installation must preserve secondary saves"
        start_hack(replacement, b"hack103", first_level=False)
        until(replacement, lambda: game_state(replacement.text()) == second_state)
        assert not saved[0].exists()
        assert game_state(first.text()) == first_state
        quit_hack(replacement)
        save_hack(first)
        os.kill(first.pid, signal.SIGKILL)
        assert first.wait_exit() == -signal.SIGKILL
        primary = launch()
        start_hack(primary, b"hack103", first_level=False)
        until(primary, lambda: game_state(primary.text()) == first_state)
        quit_hack(primary)


def test_hack_concurrent_playgrounds_keep_drive_and_long_config_aliases():
    with shared_installation(config_name="c" * 88) as (launch, _, config):
        assert len("C:" + str(config / "VC.COM")) < 128
        assert len("C:" + str(config / "HACK/HACK.EXE")) >= 128
        first, second = launch(), launch()
        for session in (first, second):
            session.send(b"cd H:\\", "enter")
            until(session, lambda: panels(session.text()) and "H:\\>" in session.text().splitlines()[23])
            start_hack(session)
        for session in (first, second):
            save_hack(session)
            until(session, lambda: "H:\\>" in session.text().splitlines()[23])
        assert len(list((config / "HACK").rglob("HACK.SAV"))) == 2
        for session in (first, second):
            start_hack(session, b"hack103", first_level=False)
            quit_hack(session)
            assert "H:\\>" in session.text().splitlines()[23]


def test_hack_concurrent_renamed_executables_use_their_own_unicode_playground():
    with shared_installation(work_name="playground-" + "long" * 30 + "🙂") as (launch, work, config):
        first, second = launch(), launch()
        for path in (config / "HACK").iterdir():
            shutil.copyfile(path, work / ("DUNGEON.EXE" if path.name == "HACK.EXE" else path.name))
        start_hack(first, b"dos2 /c dungeon -C")
        start_hack(second, b"dos2 /c dungeon -C")
        for session in (first, second):
            move_player(session)
            before = game_state(session.text())
            save_hack(session)
            start_hack(session, b"dos2 /c dungeon", first_level=False)
            until(session, lambda: game_state(session.text()) == before)
            save_hack(session)
        assert len(list(work.rglob("HACK.SAV"))) == 2
        assert not list((config / "HACK").rglob("HACK.SAV"))


def test_hack_concurrent_allocator_preserves_unowned_files_and_symlinks():
    with shared_installation() as (launch, work, config):
        first, second = launch(), launch()
        start_hack(first)
        directory = config / "HACK"
        (directory / "PLAY0001").mkdir()
        sentinel = directory / "PLAY0001/OWNED.TXT"
        sentinel.write_bytes(b"unrelated existing directory\n")
        (directory / "PLAY0002").symlink_to(work, target_is_directory=True)
        start_hack(second)
        save_hack(second)
        assert (directory / "PLAY0003/HACK.SAV").is_file()
        assert sentinel.read_bytes() == b"unrelated existing directory\n"
        assert sorted(path.name for path in sentinel.parent.iterdir()) == ["OWNED.TXT"]
        assert (directory / "PLAY0002").is_symlink()
        assert not list(work.iterdir())
        quit_hack(first)


def test_hack_replaced_playground_cannot_overwrite_another_process_save():
    with shared_installation() as (launch, _, config):
        first = launch()
        start_hack(first)
        move_player(first)
        first_state = game_state(first.text())
        original, moved = config / "HACK", config / "HACKOLD"
        original.rename(moved)
        second = launch()  # A fresh installation at the old pathname.
        start_hack(second)
        save_hack(second)
        second_bytes = (original / "HACK.SAV").read_bytes()
        first.send(b"S")
        until(first, lambda: "Cannot open save file" in first.text() or panels(first.text()))
        assert "Cannot open save file" in first.text()
        assert (original / "HACK.SAV").read_bytes() == second_bytes
        replacement = config / "HACKNEW"
        original.rename(replacement)
        moved.rename(original)
        save_hack(first)
        assert (replacement / "HACK.SAV").read_bytes() == second_bytes
        start_hack(first, b"hack103", first_level=False)
        until(first, lambda: game_state(first.text()) == first_state)
        quit_hack(first)


@pytest.mark.parametrize("quick", (False, True), ids=("comspec", "int2e"))
@pytest.mark.parametrize("command", (b"hack103", b"HaCk103"))
def test_hack_command_starts_first_level_with_player_and_status(quick, command):
    with running_vc(quick=quick) as (session, work, config):
        assert (config / "HACK/HACK.EXE").read_bytes() == (ROOT / "build/hack/HACK.EXE").read_bytes()
        assert not (work / "HACK.EXE").exists(), "the native alias must find the installed playground"
        start_hack(session, command)
        assert any(char in session.text() for char in "│─┌┐└┘"), "walls use IBM PC line drawing"
        quit_hack(session)


@pytest.mark.parametrize("quick", (False, True), ids=("comspec", "int2e"))
@pytest.mark.parametrize("command,filename", (
    ("hack", None),
    ("hack", "HACK.EXE"),
    ("HaCk", "HACK.EXE"),
    ("hack.exe", "HACK.EXE"),
    ("dungeon", "DUNGEON.EXE"),
    ("hack103.exe", "HACK103.EXE"),
    ("hack103.com", "HACK103.COM"),
), ids=("installed", "cwd", "mixed-case", "exe", "renamed", "alias-exe", "alias-com"))
def test_hack_native_host_commands_are_not_captured(monkeypatch, tmp_path, quick, command, filename):
    # Like /usr/games/hack from bsdgames, this host command is on the real
    # shell PATH, outside DOS's search directories. Even a byte-identical
    # DOS copy in cwd must not capture anything except the word hack103.
    host_games = tmp_path / "games"
    host_games.mkdir()
    script = host_games / command
    script.write_bytes(b'#!/bin/sh\nprintf "%s\\n" "$#" "$@" > HOST.TXT\npwd > CWD.TXT\n')
    script.chmod(0o755)
    monkeypatch.setenv("PATH", f"{host_games}:/usr/bin:/bin")
    with running_vc(quick=quick) as (session, work, config):
        if filename:
            shutil.copyfile(config / "HACK/HACK.EXE", work / filename)
        before = len(session.log.read_text())
        session.send(command.encode() + b" one 'two words'", "enter")
        until(session, lambda: (work / "CWD.TXT").exists()
              or "translation HACK.EXE" in session.log.read_text()[before:]
              or session.poll() is not None)
        log = session.log.read_text()[before:]
        assert (work / "HOST.TXT").exists(), f"host command {command!r} was captured by Hack:\n{log}"
        until(session, lambda: (work / "CWD.TXT").exists() and panels(session.text()))
        assert (work / "HOST.TXT").read_text() == "2\none\ntwo words\n"
        assert (work / "CWD.TXT").read_text() == str(work) + "\n"
        assert f"run: {command} one 'two words'" in log
        assert "translation HACK.EXE" not in log
        assert session.poll() is None


@pytest.mark.parametrize("quick", (False, True), ids=("comspec", "int2e"))
def test_hack_directory_is_not_on_native_dos_path(quick):
    with running_vc(quick=quick) as (session, work, _):
        session.send(b"dos2 /c set > ENV.TXT", "enter")
        until(session, lambda: (work / "ENV.TXT").exists()
              and b"PATH=" in (work / "ENV.TXT").read_bytes() and panels(session.text()))
        path = next(line for line in (work / "ENV.TXT").read_bytes().upper().splitlines()
                    if line.startswith(b"PATH="))
        assert b"\\HACK" not in path, f"Hack must not enter native DOS PATH: {path!r}"


def test_hack_movement_moves_player():
    with running_vc() as (session, _, _):
        start_hack(session)
        old, new = move_player(session)
        assert old != new
        quit_hack(session)


@pytest.mark.parametrize("quick", (False, True), ids=("comspec", "int2e"))
def test_hack_quit_returns_to_live_vc(quick):
    with running_vc(quick=quick, files={"KEPT.TXT": b"parent survived\r\n"}) as (session, work, _):
        start_hack(session)
        quit_hack(session)
        assert "KEPT" in session.text()
        assert (work / "KEPT.TXT").read_bytes() == b"parent survived\r\n"
        # A second child proves the parent PSP/stack were restored too.
        start_hack(session)
        quit_hack(session)


def test_hack_save_restores_same_game_beside_executable():
    with running_vc() as (session, work, config):
        start_hack(session)
        move_player(session)
        before = game_state(session.text())
        save_hack(session)
        saved = config / "HACK/HACK.SAV"
        assert saved.is_file() and saved.stat().st_size > 4096, "S must serialize a real game"
        assert not (work / "HACK.SAV").exists()
        assert not (config / "HACK.SAV").exists()
        assert not (config.parent.parent / "HACK.SAV").exists()
        # Changing VC's current directory must not change save discovery.
        (work / "OTHER").mkdir()
        session.send(b"cd OTHER", "enter")
        until(session, lambda: panels(session.text()) and "OTHER>" in session.text().splitlines()[23])
        start_hack(session, b"hack103", first_level=False)
        until(session, lambda: game_state(session.text()) == before)
        assert not saved.exists(), "successful original Hack restore consumes its save"
        quit_hack(session)
        assert not any(path.name.upper().startswith(("HACK.", "BONES_", "BONES."))
                       for path in work.rglob("*")), "mutable Hack files escaped its directory"


@pytest.mark.parametrize("old_header", ("other-build", "legacy"))
def test_hack_outdated_save_shows_original_message_and_starts_new_game(old_header):
    with running_vc() as (session, _, config):
        start_hack(session)
        save_hack(session)
        saved = config / "HACK/HACK.SAV"
        original = bytearray(saved.read_bytes())
        if old_header == "legacy":
            original[:8] = b"HACK103\1"
        else:
            original[4] ^= 1  # Only the build stamp changes; the payload is intact.
        saved.write_bytes(original)
        session.send(b"hack103", "enter")
        until(session, lambda: "Saved level is out of date." in session.text())
        wait_dungeon(session, first_level=True)
        assert not saved.exists(), "an outdated save must be discarded, not retried"
        assert not (config / "HACK/HACK.BAD").exists(), "only corruption is quarantined"
        quit_hack(session)


@pytest.mark.parametrize("damage", ("truncated", "modified"))
def test_hack_invalid_save_is_quarantined_without_lockout(damage):
    with running_vc() as (session, _, config):
        start_hack(session)
        save_hack(session)
        saved, rejected = config / "HACK/HACK.SAV", config / "HACK/HACK.BAD"
        original = saved.read_bytes()
        assert len(original) > 4096
        damaged = original[:-64] if damage == "truncated" else original[:-1] + bytes([original[-1] ^ 1])
        saved.write_bytes(damaged)
        start_hack(session)
        assert not saved.exists(), "automatic restore must not keep retrying a bad save"
        assert rejected.read_bytes() == damaged, "the rejected save must be recoverable"
        quit_hack(session)
        start_hack(session)
        assert rejected.read_bytes() == damaged
        quit_hack(session)


def test_hack_invalid_save_does_not_overwrite_earlier_quarantine():
    with running_vc() as (session, _, config):
        start_hack(session)
        save_hack(session)
        saved, rejected = config / "HACK/HACK.SAV", config / "HACK/HACK.BAD"
        damaged = saved.read_bytes()[:-64]
        saved.write_bytes(damaged)
        rejected.write_bytes(b"older save, keep me")
        start_hack(session)
        assert saved.read_bytes() == damaged
        assert rejected.read_bytes() == b"older save, keep me"
        quit_hack(session)


@pytest.mark.parametrize("quick", (False, True), ids=("comspec", "int2e"))
def test_hack_native_enter_keeps_host_meaning_and_dos2_exec_uses_bytes(monkeypatch, tmp_path, quick):
    host_games = tmp_path / "games"
    host_games.mkdir()
    script = host_games / "HACK.EXE"
    script.write_bytes(b'#!/bin/sh\nprintf "host Enter command\\n" > HOST.TXT\n')
    script.chmod(0o755)
    monkeypatch.setenv("PATH", f"{host_games}:/usr/bin:/bin")
    with running_vc(quick=quick) as (session, work, config):
        # Work's own copy has its own playground, exactly as DOS expected.
        for path in (config / "HACK").iterdir():
            shutil.copyfile(path, work / path.name)
        session.send(b"cd .", "enter")
        until(session, lambda: panels(session.text()) and "HACK" in session.text())
        select(session, "HACK.EXE")
        before = len(session.log.read_text())
        session.send("enter")
        # Like vc405, native Enter produces a shell command, not direct EXEC.
        until(session, lambda: (work / "HOST.TXT").exists()
              or "translation HACK.EXE" in session.log.read_text()[before:])
        assert (work / "HOST.TXT").is_file(), "native Enter must retain the command's host meaning"
        until(session, lambda: panels(session.text()))
        assert (work / "HOST.TXT").read_text() == "host Enter command\n"
        log = session.log.read_text()[before:]
        assert "run: HACK.EXE" in log
        assert "translation HACK.EXE" not in log
        start_hack(session, b"dos2 /c HACK.EXE -C")
        assert "translation HACK.EXE" in session.log.read_text()
        quit_hack(session)


def test_hack_renamed_executable_runs_by_complete_bytes():
    with running_vc() as (session, work, config):
        for path in (config / "HACK").iterdir():
            shutil.copyfile(path, work / ("DUNGEON.EXE" if path.name == "HACK.EXE" else path.name))
        start_hack(session, b"dos2 /c dungeon -C")
        save_hack(session)
        assert (work / "HACK.SAV").is_file()
        assert not (config / "HACK/HACK.SAV").exists()
        start_hack(session, b"dos2 /c dungeon", first_level=False)
        assert not (work / "HACK.SAV").exists()
        quit_hack(session)


def test_hack_modified_executable_is_rejected():
    changed = bytearray((ROOT / "build/hack/HACK.EXE").read_bytes())
    changed[-1] ^= 1
    with running_vc(files={"HACK.EXE": bytes(changed)}) as (session, work, _):
        select(session, "HACK.EXE")
        before = len(session.log.read_text())
        session.send("enter")
        until(session, lambda: "no matching translation" in session.log.read_text()[before:]
              or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        assert "translation HACK.EXE" not in session.log.read_text()[before:]
        until(session, lambda: panels(session.text()))
        (work / "HACK.EXE").unlink()
        start_hack(session)
        quit_hack(session)


@pytest.mark.parametrize("quick", (False, True), ids=("comspec", "int2e"))
@pytest.mark.parametrize("leave", ("quit", "save"))
def test_hack_restores_exact_cwd_when_short_aliases_change(quick, leave):
    from pathlib import Path
    import tempfile

    with running_vc(quick=quick, files={"KEPT.TXT": b"original directory\r\n"}) as (session, work, _):
        until(session, lambda: session.text() == session.memory_text())
        original_prompt = session.text().splitlines()[23]
        start_hack(session)
        # This owned sibling sorts immediately before our long temporary
        # root, and shares its VC-BAS~n short-name prefix. Creating it while
        # Hack runs renumbers the unleased DOS aliases returned by AH=47h.
        # A remembered long cwd must still restore the actual original root.
        prefix = work.parent.name[:-1] + "-"
        with tempfile.TemporaryDirectory(prefix=prefix, dir="/tmp") as sibling:
            sibling_work = Path(sibling) / "work"
            sibling_work.mkdir()
            assert Path(sibling).name < work.parent.name
            (quit_hack if leave == "quit" else save_hack)(session)
            until(session, lambda: session.text() == session.memory_text())
            assert panels(session.text()), session.text()
            assert session.text().splitlines()[23] == original_prompt, session.text()
            assert "KEPT" in session.text()
        # If an unstable short alias changed into the sibling, its removal
        # leaves an invalid cwd and the next launch exits during startup.
        start_hack(session, first_level=leave == "quit")
        quit_hack(session)
        assert session.text().splitlines()[23] == original_prompt, session.text()
