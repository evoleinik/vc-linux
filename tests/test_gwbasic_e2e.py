"""The real GW-BASIC image, launched by VC in a pty (never a host BASIC).

Short /tmp roots leave space in DOS's 126-byte command/environment limits.
Every shipped game is parameterized here from the same manifest as the drive.
"""
from contextlib import contextmanager
from pathlib import Path
import re
import shutil
import sys
import tempfile

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tests/e2e"))
from basic_games import GAMES, game_files  # noqa: E402
import vcini  # noqa: E402
from vcterm import VcSession  # noqa: E402
from test_e2e import select, until  # noqa: E402


BASIC_ERRORS = re.compile(
    r"Syntax error|Type mismatch|Illegal function call|Out of (?:memory|string space|data)|"
    r"Undefined (?:line number|user function)|Bad file (?:name|number)|File not found|"
    r"Input past end|Device I/O error|Subscript out of range|Division by zero|"
    r"Overflow|NEXT without FOR|RETURN without GOSUB|FOR without NEXT|Missing operand|"
    r"String too long|Device unavailable|Disk full|Direct statement in file",
    re.I,
)


def panels(text: str) -> bool:
    lines = text.splitlines()
    return len(lines) == 25 and text.startswith("╔") and "10Quit" in lines[24]


def ready(text: str) -> bool:
    return "GW-BASIC" in text and re.search(r"(?m)^Ok\s*$", text) is not None


@contextmanager
def running_vc(*, quick: bool = False, files: dict[str, bytes] | None = None,
               old_default: bool = False):
    with tempfile.TemporaryDirectory(prefix="vc-bas-", dir="/tmp") as path:
        root = Path(path)
        work, home = root / "work", root / "home"
        work.mkdir()
        config = home / ".config/vc-linux"
        config.mkdir(parents=True)
        for name, data in (files or {}).items():
            (work / name).write_bytes(data)
        if quick:
            ini = vcini.VcIni((ROOT / "data/VC.INI").read_bytes())
            ini.set_main("ExecTyp", 1)
            (config / "VC.INI").write_bytes(ini.data)
        if old_default:
            (config / "VC.EXT").write_bytes(b"")
        session = VcSession(work, home)
        try:
            session.wait_for("10Quit", timeout=15)
            until(session, lambda: panels(session.text()))
            yield session, work, config
        finally:
            session.close()


def start_basic(session: VcSession, command: bytes = b"gwbasic") -> None:
    session.send(command, "enter")
    session.wait_for("GW-BASIC", timeout=10)
    until(session, lambda: ready(session.text()))
    assert session.poll() is None
    assert not BASIC_ERRORS.search(session.text()), session.text()


def return_to_vc(session: VcSession) -> None:
    session.send(b"SYSTEM", "enter")
    until(session, lambda: panels(session.text()))
    assert session.text() == session.memory_text()
    assert session.poll() is None


@pytest.mark.parametrize("command", [b"gwbasic", b"GWBASIC.EXE"])
def test_gwbasic_command_banner_and_ok(command):
    with running_vc() as (session, work, config):
        assert (config / "GWBASIC.EXE").read_bytes() == (ROOT / "build/gwbasic/GWBASIC.EXE").read_bytes()
        assert not (work / "GAMES").exists(), "Linux installs the interpreter, not the games"
        start_basic(session, command)


def test_gwbasic_print_arithmetic():
    with running_vc() as (session, _, _):
        start_basic(session)
        session.send(b"PRINT 2+2", "enter")
        until(session, lambda: re.search(r"(?m)^\s*4\s*$", session.text()))
        assert not BASIC_ERRORS.search(session.text()), session.text()


def test_gwbasic_data_encoded_inp_out():
    # FNINP/FNOUT are INS86 data macros in the listing, not mnemonic rows.
    with running_vc() as (session, _, _):
        start_basic(session)
        for value in (16, 0):
            session.send(f"OUT &H61,{value}".encode(), "enter")
            session.send(b"PRINT INP(&H61)", "enter")
            until(session, lambda: re.search(rf"(?m)^\s*{value}\s*$", session.text()))
            assert not BASIC_ERRORS.search(session.text()), session.text()
        return_to_vc(session)


def test_gwbasic_system_redraws_panels():
    with running_vc(files={"PANEL.TXT": b"still here\r\n"}) as (session, _, _):
        start_basic(session)
        return_to_vc(session)
        assert "PANEL" in session.text()
        # A second launch proves SYSTEM restored the parent PSP/stack too.
        start_basic(session)
        return_to_vc(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("setup,command,address", [
    ([b"DEF SEG=0", b"A=0"], b"CALL A", "0000:0000"),
    ([b"DEF SEG=&HF000", b"A=&H200"], b"CALL A", "F000:0200"),
    # MOV AX,1234h; RETF is new machine code, not a copied translated routine.
    ([b"DEF SEG=&H9000", b"POKE 0,184: POKE 1,52", b"POKE 2,18: POKE 3,203", b"A=0"],
     b"CALL A", "9000:0000"),
    ([b"DEF SEG=&H9000", b"POKE 0,184: POKE 1,52", b"POKE 2,18: POKE 3,203", b"DEF USR=0"],
     b"PRINT USR(0)", "9000:0000"),
], ids=["call-zero", "rom-hole", "poked-call", "poked-usr"])
def test_gwbasic_untranslated_call_stops_only_child(quick, setup, command, address):
    with running_vc(quick=quick, files={"PANEL.TXT": b"still here\r\n"}) as (session, _, _):
        start_basic(session)
        # Short, acknowledged lines do not overflow BASIC's keyboard queue.
        for line in setup:
            session.send(line, "enter")
            until(session, lambda: re.search(r"(?m)^Ok\s*$", session.text().partition(line.decode())[2]))
            assert not BASIC_ERRORS.search(session.text()), session.text()
        # Keep every rendered frame: VC can restore its panels in the same
        # pty read as the one-line diagnostic on the DOS user screen.
        frames = []
        feed = session.stream.feed

        def observe(data):
            for byte in data:
                feed(bytes([byte]))
                if byte == ord("."):
                    frames.append(session.text())

        session.stream.feed = observe
        # This interpreter's CALL grammar requires an address variable.
        session.send(command, "enter")
        until(session, lambda: panels(session.text()) or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        assert any(f"No translated code at {address}. GWBASIC.EXE stopped." in text
                   for text in frames), session.text()
        assert "PANEL" in session.text()
        assert session.text() == session.memory_text()
        session.stream.feed = feed
        # A second child exercises restored parent state and timer hooks.
        start_basic(session)
        session.send(b"PRINT 2+2", "enter")
        until(session, lambda: re.search(r"(?m)^\s*4\s*$", session.text()))
        return_to_vc(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_gwbasic_enter_runs_bas_and_migrates_empty_default(quick):
    with running_vc(quick=quick, old_default=True,
                    files={"HELLO.BAS": b'10 PRINT "BASIC FILE EXECUTED"\r\n20 END\r\n'}) as (session, _, config):
        assert (config / "VC.EXT").read_bytes() == (ROOT / "data/VC.EXT").read_bytes()
        select(session, "HELLO.BAS")
        session.send("enter")
        session.wait_for("BASIC FILE EXECUTED")
        until(session, lambda: re.search(r"(?m)^Ok\s*$", session.text()))
        assert not BASIC_ERRORS.search(session.text()), session.text()
        return_to_vc(session)


@pytest.mark.parametrize("name,command", [("BASIC.EXE", b"basic"), ("BASIC.EXE", b"bAsIc.ExE"),
                                          ("BASIC.COM", b"basic.com")])
def test_gwbasic_renamed_image_runs_by_bytes(name, command):
    with running_vc() as (session, work, config):
        shutil.copyfile(config / "GWBASIC.EXE", work / name)
        (config / "GWBASIC.EXE").unlink()
        start_basic(session, command)
        return_to_vc(session)


@pytest.mark.parametrize("replacement", ["changed", "VC.COM", "VC.OVL"])
def test_gwbasic_changed_image_is_refused_and_vc_survives(replacement):
    with running_vc() as (session, work, config):
        if replacement == "changed":
            data = bytearray((config / "GWBASIC.EXE").read_bytes())
            data[-1] ^= 1  # Even a change outside the startup code must be refused.
        else:
            # A different valid translation is still a changed interpreter.
            # In particular, VC must not parse an association's file tail.
            data = (config / replacement).read_bytes()
        (work / "GWBASIC.EXE").write_bytes(data)
        before = len(session.log.read_text())
        session.send(b"gwbasic", "enter")
        until(session, lambda: "DOS command error 11" in session.log.read_text()[before:]
              or "translation VC." in session.log.read_text()[before:]
              or session.poll() is not None)
        assert "DOS command error 11" in session.log.read_text()[before:], session.log.read_text()[before:]
        until(session, lambda: panels(session.text()))
        log = session.log.read_text()
        assert "unsupported executable" in log.lower(), log
        assert "GW-BASIC" not in session.text()
        # Removing the bad current-directory image permits the PATH image.
        (work / "GWBASIC.EXE").unlink()
        start_basic(session)
        return_to_vc(session)


@pytest.mark.parametrize("missing", [False, True], ids=["installed", "deleted"])
@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_gwbasic_filename_never_reaches_shell(missing, quick):
    name = "safe;touch PWNED;.bas"
    with running_vc(quick=quick, files={name: b'10 PRINT "SAFE FILENAME"\r\n20 END\r\n'}) as (session, work, config):
        if missing:
            (config / "GWBASIC.EXE").unlink()
        select(session, name)
        session.send("enter")
        if missing:
            until(session, lambda: "DOS command error 2" in session.log.read_text())
            until(session, lambda: panels(session.text()))
            assert "GW-BASIC" not in session.text()
        else:
            until(session, lambda: "translation GWBASIC.EXE" in session.log.read_text())
            # GW-BASIC can reject DOS-forbidden or long filenames; it must
            # never turn their punctuation into host-shell operators.
            until(session, lambda: re.search(r"(?m)^Ok\s*$", session.text()) or (
                "terminate psp" in session.log.read_text().rpartition("translation GWBASIC.EXE")[2]
                and panels(session.text())))
            if not panels(session.text()):
                return_to_vc(session)
        assert session.poll() is None
        assert not (work / "PWNED").exists()
        assert not (config / "PWNED").exists()
        assert (work / name).exists()


@pytest.mark.parametrize("key", [b"\x1b[57362;5u", b"\x1b[98;6u"], ids=["ctrl-pause", "ctrl-shift-b"])
def test_gwbasic_ctrl_break_stops_infinite_program(key):
    with running_vc() as (session, _, _):
        start_basic(session)
        session.send(b"10 GOTO 10")
        session.wait_for("10 GOTO 10")
        line_row = session.screen.cursor.y
        session.send("enter")
        until(session, lambda: session.screen.cursor.y > line_row)
        session.send(b"RUN")
        session.wait_for("RUN")
        run_row = session.screen.cursor.y
        session.send("enter")
        until(session, lambda: session.screen.cursor.y > run_row)
        session.send(key)
        session.wait_for("Break in 10", timeout=10)
        session.send(b"\x1b[57442;1:3u\x1b[57441;1:3u")
        return_to_vc(session)


@pytest.mark.parametrize("game", GAMES, ids=lambda game: game.name)
def test_shipped_game_reaches_first_prompt(game):
    source = ROOT / "build/games" / game.name
    assert source.read_bytes() == game_files()[game.name]
    assert re.fullmatch(r"[A-Z0-9]{1,8}\.BAS", game.name)
    assert b"\n" not in source.read_bytes().replace(b"\r\n", b"")
    with running_vc(files={game.name: source.read_bytes()}) as (session, _, _):
        select(session, game.name)
        session.send("enter")
        # Stop promptly on an interpreter error/empty-program Ok, too. A
        # broken file loader must not look like a slow game initialization.
        until(session, lambda: game.prompt in session.text()
              or BASIC_ERRORS.search(session.text())
              or re.search(r"(?m)^Ok\s*$", session.text()), timeout=15)
        assert game.prompt in session.text(), session.text()
        until(session, lambda: "?" in session.text())
        assert not BASIC_ERRORS.search(session.text()), session.text()
        assert session.poll() is None
