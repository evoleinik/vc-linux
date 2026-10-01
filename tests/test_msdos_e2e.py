"""Microsoft's unedited DOS shell/utilities running through VC in a Linux pty.

Every filesystem path is private /tmp state. Screen conditions, executable
load logs, and byte-for-byte file readback are the gates, never elapsed sleeps.
"""
import re
import shutil

import pytest

from test_gwbasic_e2e import ROOT, panels, running_vc
from test_e2e import until


PROMPT = re.compile(r"(?m)^[CH]:\\[^\n>]*> *$")


def shell_ready(session):
    return not panels(session.text()) and PROMPT.search(session.text()) is not None


def start_command(session, command=b"dos2"):
    before = len(session.log.read_text())
    session.send(command, "enter")
    until(session, lambda: "translation COMMAND.COM" in session.log.read_text()[before:]
          and shell_ready(session), timeout=15)
    assert session.poll() is None
    assert "No translated code" not in session.log.read_text()[before:]
    return before


def line(session, command, expected=None):
    # An echoed input is not evidence of command output. Match a separate
    # line after the submitted command and the shell's subsequent prompt.
    session.send(command.encode(), "enter")
    until(session, lambda: shell_ready(session) and (expected is None or
          expected(session.text().rpartition(command)[2])), timeout=12)
    assert session.poll() is None


def back_to_vc(session):
    session.send(b"exit", "enter")
    until(session, lambda: panels(session.text()), timeout=12)
    assert session.poll() is None
    assert session.text() == session.memory_text()


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_command_dir_echo_type_copy_batch_exit(quick):
    files = {"README.TXT": b"original DOS file\r\n", "TWO.BAT":
             b"echo BATCH FIRST LINE\r\necho BATCH SECOND LINE\r\n"}
    with running_vc(quick=quick, files=files) as (session, work, config):
        assert (config / "DOS2.COM").read_bytes() == (ROOT / "build/msdos2/COMMAND.COM").read_bytes()
        before = start_command(session)
        line(session, "dir", lambda text: "README" in text and "TWO" in text)
        line(session, "echo hello", lambda text: re.search(r"(?m)^hello\s*$", text))
        line(session, "type README.TXT", lambda text: "original DOS file" in text)
        line(session, "copy README.TXT COPIED.TXT", lambda text: "copied" in text.lower())
        assert (work / "COPIED.TXT").read_bytes() == files["README.TXT"]
        line(session, "TWO", lambda text: "BATCH FIRST LINE" in text and "BATCH SECOND LINE" in text)
        line(session, "ver", lambda text: "2.00" in text)
        back_to_vc(session)
        assert "No translated code" not in session.log.read_text()[before:]
        # In particular the host's COMMAND builtin must stay native after
        # EXIT: leaking the child's PATH would find DOS2/COMMAND.COM again.
        session.send(b"command printf HOST-SHELL > HOST.TXT", "enter")
        until(session, lambda: (work / "HOST.TXT").exists() and panels(session.text()))
        assert (work / "HOST.TXT").read_text() == "HOST-SHELL"


def test_command_renamed_and_single_command():
    with running_vc(files={"NOTE.TXT": b"renamed works\r\n"}) as (session, work, config):
        shutil.copyfile(config / "DOS2/COMMAND.COM", work / "SHELL.COM")
        start_command(session, b"shell")
        line(session, "type NOTE.TXT", lambda text: "renamed works" in text)
        # A renamed shell still receives the installation's private utility
        # directory, not a guessed DOS2 beside the renamed executable.
        line(session, "sort < NOTE.TXT > SORTED.TXT")
        until(session, lambda: (work / "SORTED.TXT").exists() and shell_ready(session))
        assert (work / "SORTED.TXT").read_bytes() == b"renamed works\r\n"
        back_to_vc(session)
        before = session.log.read_text().count("translation COMMAND.COM")
        session.send(b"dos2 /c echo SINGLE-RUN > SINGLE.TXT", "enter")
        until(session, lambda: (work / "SINGLE.TXT").exists() and panels(session.text()))
        # DOS ECHO preserves the space immediately before the redirection.
        assert (work / "SINGLE.TXT").read_bytes() == b"SINGLE-RUN \r\n"
        assert session.log.read_text().count("translation COMMAND.COM") == before + 1


def test_edlin_edits_saves_and_returns():
    with running_vc(files={"EDIT.TXT": b"old line\r\n"}) as (session, work, _):
        start_command(session)
        session.send(b"edlin EDIT.TXT", "enter")
        until(session, lambda: "translation EDLIN.COM" in session.log.read_text()
              and not panels(session.text()) and re.search(r"(?m)^\*\s*$", session.text()))
        session.send(b"1i", "enter")
        until(session, lambda: re.search(r"(?m)^\s*1:\*?\s*$", session.text()))
        session.send(b"new line", "enter")
        until(session, lambda: re.search(r"(?m)^\s*2:\*?\s*$", session.text()))
        session.send(b"\x1a", "enter")
        until(session, lambda: re.search(r"(?m)^\*\s*$", session.text()))
        session.send(b"e", "enter")
        until(session, lambda: shell_ready(session))
        assert (work / "EDIT.TXT").read_bytes().rstrip(b"\x1a") == b"new line\r\nold line\r\n"
        assert (work / "EDIT.BAK").read_bytes() == b"old line\r\n"
        back_to_vc(session)


@pytest.mark.parametrize("program,command,filename,expected", [
    ("FIND.EXE", 'find "x" INPUT.TXT > FOUND.TXT', "FOUND.TXT", b"x-line\r\n"),
    # MORE.ASM:38 writes CRLFTXT once to initialize the cursor, even when redirected.
    ("MORE.COM", "more < INPUT.TXT > MOREOUT.TXT", "MOREOUT.TXT", b"\r\nbeta\r\nalpha\r\nx-line\r\n"),
    ("SORT.EXE", "sort < INPUT.TXT > SORTOUT.TXT", "SORTOUT.TXT", b"alpha\r\nbeta\r\nx-line\r\n"),
    ("SORT.EXE", "sort /r < INPUT.TXT > SORTOUT.TXT", "SORTOUT.TXT", b"x-line\r\nbeta\r\nalpha\r\n"),
    ("FC.EXE", "fc /b INPUT.TXT ALTER.TXT > FCOUT.TXT", "FCOUT.TXT", b"00000000"),
])
def test_command_original_utilities_and_redirection(program, command, filename, expected):
    with running_vc(files={"INPUT.TXT": b"beta\r\nalpha\r\nx-line\r\n",
                           "ALTER.TXT": b"zeta\r\nalpha\r\nx-line\r\n"}) as (session, work, _):
        before = start_command(session)
        line(session, command)
        until(session, lambda: (work / filename).exists() and shell_ready(session))
        result = (work / filename).read_bytes()
        if program in ("FIND.EXE", "FC.EXE"):
            assert expected in result
            if program == "FIND.EXE":
                assert b"beta\r\n" not in result and b"alpha\r\n" not in result
        else:
            assert result == expected
        log = session.log.read_text()[before:]
        assert f"translation {program}" in log
        assert "No translated code" not in log
        back_to_vc(session)


def test_debug_prompt_quit_and_panel_redraw():
    with running_vc() as (session, _, _):
        start_command(session)
        session.send(b"debug", "enter")
        until(session, lambda: "translation DEBUG.COM" in session.log.read_text()
              and not panels(session.text()) and re.search(r"(?m)^-\s*$", session.text()))
        session.send(b"q", "enter")
        until(session, lambda: shell_ready(session))
        back_to_vc(session)
