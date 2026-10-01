"""Original Rogue 5.4.4, compiled to DOS and launched by VC's byte-matching EXEC.

Each brief-16 play gate is independently selectable for deliberate-defect
checks. Input follows visible game prompts, never a timer or a host Rogue.
"""
import os
from pathlib import Path
import re
import shutil
import time

import pytest

from test_gwbasic_e2e import ROOT, panels, running_vc, select, until as until_screen


STATUS = re.compile(r"Level:\s*(\d+)\s+Gold:\s*\d+\s+Hp:\s*\d+\(\s*\d+\)")
MOVES = (("h", 0, -1), ("j", 1, 0), ("k", -1, 0), ("l", 0, 1),
         ("y", -1, -1), ("u", -1, 1), ("b", 1, -1), ("n", 1, 1))


def until(session, check, timeout=8):
    try:
        until_screen(session, check, timeout=timeout)
    except AssertionError as error:
        raise AssertionError(f"{error}\n--- VC log ---\n{session.log.read_text()}") from None


def players(text):
    # The first row belongs to messages, not the map (help can mention @).
    return [(row, column) for row, line in enumerate(text.splitlines())
            if row > 0 and not STATUS.search(line)
            for column, char in enumerate(line) if char == "@"]


def dungeon(text, *, first_level=False):
    status = STATUS.search(text)
    return bool(status and (not first_level or status[1] == "1")
                and len(players(text)) == 1 and "." in text)


def game_state(text):
    """All visible dungeon tiles, player position, inventory stats and HP.

    Only the message row changes to 'file name: rogue.sav' on restore.
    Comparing the whole map, after a move, rejects a coincidentally equal
    initial status line from a newly generated game.
    """
    return "\n".join(text.splitlines()[1:])


def revealed_teleport(before, after, row, column):
    """A hidden teleport trap moves @ away and exposes ^ at the chosen tile."""
    old, new = players(before), players(after)
    return (len(old) == len(new) == 1 and old != new
            and before.splitlines()[row][column] != "^"
            and after.splitlines()[row][column] == "^")


def wait_dungeon(session, *, first_level=False):
    until(session, lambda: dungeon(session.text(), first_level=first_level)
          or session.poll() is not None, timeout=20)
    assert session.poll() is None, session.log.read_text()
    assert dungeon(session.text(), first_level=first_level), session.text()
    until(session, lambda: session.text() == session.memory_text())


def start_rogue(session, command=b"rogue", *, first_level=True):
    before = len(session.log.read_text())
    session.send(command, "enter")
    wait_dungeon(session, first_level=first_level)
    assert "translation ROGUE.EXE" in session.log.read_text()[before:]


def move_player(session):
    before = session.text()
    row, column = players(before)[0]
    lines = before.splitlines()
    # Level 1 rooms are lit. A visible empty floor avoids attacking a
    # random monster or walking into a wall, so random room placement is
    # not a timing assumption or an intermittent failure.
    choice = next(((key, row + dy, column + dx)
                   for floor in (".", "*%:!?)=/]") for key, dy, dx in MOVES
                   if lines[row + dy][column + dx] in floor), None)
    assert choice is not None, f"no adjacent unoccupied floor in Rogue's starting room\n{before}"
    key, next_row, next_column = choice
    session.send(key.encode("ascii"))
    # A randomly placed item/trap and a monster can produce two messages
    # on one turn, sometimes before @ moves. Acknowledge only an actual
    # --More-- prompt, and still require movement after at most eight.
    for messages in range(9):
        until(session, lambda: "--More--" in session.text()
              or dungeon(session.text()) and players(session.text()) != [(row, column)])
        if "--More--" not in session.text():
            break
        assert messages < 8, session.text()
        previous = session.text()
        session.send(b" ")
        until(session, lambda: session.text() != previous)
    assert "--More--" not in session.text(), session.text()
    # new_level.c can hide a trap even on level 1. move.c's T_TELEP
    # silently relocates @ and draws ^ at the chosen tile; T_DOOR changes
    # levels. Neither permits an unchanged player or arbitrary movement.
    if not ("you fell into a trap!" in session.text().lower()
            and STATUS.search(session.text())[1] != STATUS.search(before)[1]
            or revealed_teleport(before, session.text(), next_row, next_column)):
        assert players(session.text()) == [(next_row, next_column)], session.text()
    assert session.poll() is None
    until(session, lambda: session.text() == session.memory_text())
    return (row, column), players(session.text())[0]


@pytest.mark.parametrize("old_tile,new_tile,new_player,expected", [
    (".", "^", (2, 4), True),
    (".", "^", (1, 1), False),  # dropped key: @ must change
    (".", ".", (2, 4), False),  # arbitrary relocation without a trap
    ("^", "^", (2, 4), False),  # the trap must be newly revealed
    (".", "^", (1, 2), False),  # ordinary move: @ covers the target
    (".", "^", None, False),    # losing @ is not teleportation
])
def test_rogue_teleport_requires_new_trap_and_moved_player(old_tile, new_tile, new_player, expected):
    def screen(tile, player):
        rows = [list(" " * 8) for _ in range(3)]
        rows[1][2] = tile
        if player is not None:
            rows[player[0]][player[1]] = "@"
        return "\n".join("".join(row) for row in rows)

    assert revealed_teleport(screen(old_tile, (1, 1)), screen(new_tile, new_player), 1, 2) is expected


def test_rogue_movement_acknowledges_more_before_player_moves(monkeypatch):
    # T_RUST can ask --More-- between its two messages, before do_move
    # reaches move_stuff. A visible prompt must be handled before waiting
    # for @ to change; otherwise this ordinary turn would deadlock.
    frames = [
        "\n-----\n|.@.|\n-----\nLevel: 1  Gold: 0  Hp: 12(12)",
        "A gush of water hits you on the head--More--\n-----\n|.@.|\n-----\nLevel: 1  Gold: 0  Hp: 12(12)",
        "Your armor weakens\n-----\n|@..|\n-----\nLevel: 1  Gold: 0  Hp: 12(12)",
    ]

    class Session:
        frame = 0

        def text(self):
            return frames[self.frame]

        memory_text = text

        def send(self, key):
            assert key == (b"h", b" ")[self.frame]
            self.frame += 1

        def poll(self):
            return None

    def visible_state(session, check, timeout=8):
        assert check(), session.text()

    monkeypatch.setitem(globals(), "until", visible_state)
    session = Session()
    assert move_player(session) == ((2, 2), (2, 1))
    assert session.frame == 2


def quit_rogue(session):
    before = len(session.log.read_text())
    session.send(b"Q")
    until(session, lambda: "really quit?" in session.text().lower())
    assert not panels(session.text()), "Q must ask before ending the game"
    session.send(b"y")
    until(session, lambda: panels(session.text()) or session.poll() is not None, timeout=15)
    assert session.poll() is None, session.log.read_text()
    assert panels(session.text()), session.text()
    until(session, lambda: session.text() == session.memory_text())
    log = session.log.read_text()[before:]
    assert "no translated code" not in log.lower(), log
    exits = re.findall(r"terminate psp [0-9A-F]+ code (\d+)\b", log)
    assert exits and all(code == "0" for code in exits), log


def save_rogue(session):
    session.send(b"S")
    until(session, lambda: "save file (rogue.sav)?" in session.text().lower())
    session.send(b"y")
    until(session, lambda: panels(session.text()))
    assert session.poll() is None, session.log.read_text()


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("command", [b"rogue", b"ROGUE.EXE"])
def test_rogue_command_dungeon_and_status(quick, command):
    with running_vc(quick=quick) as (session, work, config):
        image = (ROOT / "build/rogue/ROGUE.EXE").read_bytes()
        assert image[:2] == b"MZ", "Rogue must be a real linked DOS EXE"
        assert (config / "ROGUE.EXE").read_bytes() == image
        assert not (work / "ROGUE.EXE").exists(), "typed rogue must find the DOS PATH image"
        start_rogue(session, command)


def test_rogue_movement_moves_player():
    with running_vc() as (session, _, _):
        start_rogue(session)
        old, new = move_player(session)
        assert old != new


def test_rogue_options_indirect_callbacks():
    with running_vc() as (session, _, _):
        start_rogue(session)
        before = game_state(session.text())
        session.send(b"o")
        # optlist holds far pointers for seven different input/output
        # routines. Visit every option so all seven execute, including
        # string and inventory functions not reached by simple movement.
        until(session, lambda: 'Terse output ("terse")' in session.text()
              and 'Save file ("file")' in session.text())
        # All ten getters accept Enter without changing a value or flushing
        # typeahead. The ten-byte queue fits the 15-key BIOS buffer; the
        # completion prompt acknowledges them all. PDCurses hides the
        # terminal cursor, so its position cannot acknowledge each option.
        session.send(b"\r" * 10)
        session.wait_for("--Press space to continue--")
        session.send(b" ")
        wait_dungeon(session)
        assert game_state(session.text()) == before, "visiting options does not advance the game"
        quit_rogue(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_rogue_quit_restores_panels(quick):
    with running_vc(quick=quick, files={"KEPT.TXT": b"parent survived\r\n"}) as (session, work, config):
        start_rogue(session)
        quit_rogue(session)
        assert "KEPT" in session.text()
        assert (work / "KEPT.TXT").read_bytes() == b"parent survived\r\n"
        assert (work / "rogue.scr").is_file(), "the score file belongs to the current directory"
        assert not (config / "rogue.scr").exists()
        # A fresh child also proves parent PSP/stack and curses state were
        # restored, not just that a cached panel frame was displayed.
        start_rogue(session)
        quit_rogue(session)


def test_rogue_save_restores_same_game_in_current_directory():
    with running_vc() as (session, work, config):
        save_dir = work / "SAVELOC"
        save_dir.mkdir()
        session.send(b"cd SAVELOC", "enter")
        until(session, lambda: panels(session.text()) and "SAVELOC>" in session.text().splitlines()[23])
        start_rogue(session)
        move_player(session)
        before = game_state(session.text())
        save_rogue(session)
        saved = save_dir / "rogue.sav"
        assert saved.is_file() and saved.stat().st_size > 4096, "S must serialize the actual game"
        assert (save_dir / "rogue.scr").is_file()
        for elsewhere in (work, config, config.parent.parent):
            assert not (elsewhere / "rogue.sav").exists()
            assert not (elsewhere / "rogue.scr").exists()
        start_rogue(session, first_level=False)
        until(session, lambda: game_state(session.text()) == before)
        assert not saved.exists(), "Rogue consumes its save on a successful restore"
        quit_rogue(session)


def test_rogue_explicit_truncated_restore_preserves_saved_file():
    with running_vc() as (session, work, _):
        start_rogue(session)
        save_rogue(session)
        saved = work / "rogue.sav"
        data = saved.read_bytes()
        assert len(data) > 4096
        # Keep the encrypted version/dimensions and all main game state;
        # fail near the end, in rs_read_window. A mere wrong-version test
        # never reaches the formerly ignored rs_restore_file return value.
        truncated = data[:-64]
        saved.write_bytes(truncated)
        before = len(session.log.read_text())
        session.send(b"rogue rogue.sav", "enter")
        until(session, lambda: "terminate psp" in session.log.read_text()[before:]
              or not saved.exists() or session.poll() is not None, timeout=20)
        assert saved.is_file(), "a rejected restore must not consume the user's saved game"
        assert saved.read_bytes() == truncated, "restore must not overwrite a rejected saved game"
        assert session.poll() is None, session.log.read_text()
        log = session.log.read_text()[before:]
        assert re.search(r"terminate psp [0-9A-F]+ code 1\b", log), log
        assert "no translated code" not in log.lower(), log
        until(session, lambda: panels(session.text()))
        saved.unlink()
        start_rogue(session)
        quit_rogue(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("damage", ["truncated", "old-version"])
def test_rogue_failed_auto_resume_quarantines_save_and_starts_fresh(quick, damage):
    with running_vc(quick=quick) as (session, work, _):
        start_rogue(session)
        # A starting game already serializes the complete player and pack.
        # Recovery must not depend on a random room having a safe move.
        save_rogue(session)
        saved, rejected = work / "rogue.sav", work / "rogue.bad"
        data = saved.read_bytes()
        assert len(data) > 4096
        # A late truncation has already deserialized the old player and
        # inventory. Recovery must start clean, not reuse that partial state.
        damaged = data[:-64] if damage == "truncated" else bytes([data[0] ^ 1]) + data[1:]
        saved.write_bytes(damaged)
        session.send(b"rogue", "enter")
        until(session, lambda: dungeon(session.text(), first_level=True)
              or session.poll() is not None, timeout=20)
        assert dungeon(session.text(), first_level=True), "rejected auto-resume must start a new game"
        until(session, lambda: session.text() == session.memory_text())
        assert session.poll() is None, session.log.read_text()
        assert not saved.exists(), "plain rogue must not retry the same rejected save forever"
        assert rejected.read_bytes() == damaged, "quarantine must preserve the rejected save bytes"
        assert "renamed to rogue.bad" in session.text().splitlines()[0].lower(), session.text()
        assert "starting a new game" in session.text().splitlines()[0].lower(), session.text()
        # Acknowledge the notice without advancing a turn, and let Rogue
        # render the cleared message row before opening its inventory window.
        session.send("esc")
        until(session, lambda: dungeon(session.text(), first_level=True)
              and not session.text().splitlines()[0].strip())
        session.send(b"i")
        until(session, lambda: "--Press space to continue--" in session.text())
        assert re.findall(r"(?m)^([a-z])\)", session.text()) == list("abcde"), (
            "fresh inventory must not include the partially restored pack\n" + session.text())
        session.send(b" ")
        wait_dungeon(session, first_level=True)
        quit_rogue(session)
        start_rogue(session)
        assert rejected.read_bytes() == damaged, "a later plain launch must leave rogue.bad alone"
        quit_rogue(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_rogue_enter_on_exe(quick):
    image = (ROOT / "build/rogue/ROGUE.EXE").read_bytes()
    with running_vc(quick=quick, files={"ROGUE.EXE": image}) as (session, _, _):
        select(session, "ROGUE.EXE")
        session.send("enter")
        wait_dungeon(session, first_level=True)
        assert "translation ROGUE.EXE" in session.log.read_text()
        quit_rogue(session)


def test_rogue_renamed_image_runs_by_complete_bytes():
    with running_vc() as (session, work, config):
        shutil.copyfile(config / "ROGUE.EXE", work / "DUNGEON.EXE")
        (config / "ROGUE.EXE").unlink()
        start_rogue(session, b"dungeon")
        move_player(session)
        before = game_state(session.text())
        save_rogue(session)
        saved = work / "rogue.sav"
        assert saved.is_file()
        # Automatic resume launches another DOS copy of argv[0]. It must
        # use the renamed executable, not the removed installed ROGUE.EXE.
        start_rogue(session, b"dungeon", first_level=False)
        until(session, lambda: game_state(session.text()) == before)
        assert not saved.exists(), "successful resume must consume the renamed game's save"
        quit_rogue(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_rogue_changed_image_is_refused_and_parent_survives(quick):
    changed = bytearray((ROOT / "build/rogue/ROGUE.EXE").read_bytes())
    changed[-1] ^= 1  # Do not merely test the header or startup bytes.
    with running_vc(quick=quick, files={"ROGUE.EXE": bytes(changed)}) as (session, work, _):
        select(session, "ROGUE.EXE")
        before = len(session.log.read_text())
        # Enter is DOS EXEC, which must reject changed executables. A
        # generic typed command may intentionally fall back to Linux's
        # shell when it finds no translated program, as before Rogue.
        session.send("enter")
        until(session, lambda: "no matching translation" in session.log.read_text()[before:]
              or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        assert "unsupported executable" in session.log.read_text()[before:].lower()
        assert "translation ROGUE.EXE" not in session.log.read_text()[before:]
        until(session, lambda: panels(session.text()))
        assert not dungeon(session.text())
        (work / "ROGUE.EXE").unlink()
        start_rogue(session)
        quit_rogue(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_rogue_read_only_directory_does_not_delay_qualifying_score(quick):
    # C:\ is /, normally unwritable. Root can bypass its mode bits, so use
    # the read-only sysfs mount in that case rather than a chmod-only fixture.
    readonly = Path("/sys") if os.access("/", os.W_OK) else Path("/")
    assert readonly.is_dir() and not os.access(readonly, os.W_OK)
    score, lock = readonly / "rogue.scr", readonly / "rogue.lck"
    assert not score.exists() and not lock.exists(), "need an empty, unwritable scoreboard"
    dos_path = "C:" + str(readonly).replace("/", "\\")
    with running_vc(quick=quick) as (session, _, _):
        session.send(f"cd {dos_path}".encode("ascii"), "enter")
        until(session, lambda: panels(session.text())
              and session.text().splitlines()[23].rstrip().endswith(f"{dos_path}>"))
        before = len(session.log.read_text())
        # Upstream's -d death demonstration sets purse = rnd(100) + 1 and
        # calls the real death/score path. Every seed gives positive gold:
        # no wandering through a random dungeon or edited save is necessary.
        session.send(b"rogue -d", "enter")
        until(session, lambda: "[Press return to continue]" in session.text()
              and re.search(r"\b[1-9]\d* Au\b", session.text()), timeout=20)
        gold = int(re.search(r"\b([1-9]\d*) Au\b", session.text())[1])
        assert "translation ROGUE.EXE" in session.log.read_text()[before:]
        assert not panels(session.text())
        started = time.monotonic()
        session.send("enter")
        until(session, lambda: panels(session.text()) or session.poll() is not None, timeout=12)
        elapsed = time.monotonic() - started
        assert session.poll() is None, session.log.read_text()
        assert panels(session.text()), session.text()
        # Without the lock delay the score table can disappear between tty
        # reads: the final stdio prompt currently gets EOF from DOS CON.
        # VC's saved DOS user screen retains the actual qualifying score.
        session.send(b"\x0f")  # Ctrl-O
        until(session, lambda: not panels(session.text()) and "Top " in session.text())
        assert re.search(rf"\b1\s+{gold}\s+Rogue:", session.text()), session.text()
        session.send(b"\x0f")
        until(session, lambda: panels(session.text()))
        assert elapsed < 4.0, f"read-only score handling stalled for {elapsed:.2f}s"
        until(session, lambda: session.text() == session.memory_text())
        assert not score.exists() and not lock.exists()


def test_rogue_failed_auto_resume_preserves_existing_quarantine():
    older = b"an earlier rejected save that must be preserved\r\n"
    with running_vc(files={"rogue.bad": older}) as (session, work, _):
        start_rogue(session)
        save_rogue(session)
        saved, rejected = work / "rogue.sav", work / "rogue.bad"
        data = saved.read_bytes()
        assert len(data) > 4096
        damaged = data[:-64]
        saved.write_bytes(damaged)
        # DOS rename refuses to replace an existing file. The failed
        # quarantine must preserve both saves without locking out play.
        for _ in range(2):
            start_rogue(session)
            notice = session.text().splitlines()[0].lower()
            assert "cannot rename rogue.sav to rogue.bad" in notice, session.text()
            assert "starting a new game" in notice, session.text()
            assert saved.read_bytes() == damaged
            assert rejected.read_bytes() == older
            quit_rogue(session)
