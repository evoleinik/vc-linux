"""COMMAND's real console, NUL, pipe and wildcard paths, through a Linux pty."""
import re

import pytest

from test_e2e import until
from test_gwbasic_e2e import running_vc
from test_msdos_e2e import back_to_vc, line, shell_ready, start_command


def test_command_wide_directory_uses_tab_stops():
    files = {"ONE.TXT": b"one", "TWO.TXT": b"two", "THREE.TXT": b"three"}
    with running_vc(files=files) as (session, _, _):
        start_command(session)
        line(session, "dir /w", lambda text: all(name in text for name in ("ONE", "TWO", "THREE")))
        output = session.text().rpartition("dir /w")[2]
        assert "\u25cb" not in output, "TAB must not render as the CP437 white-circle glyph"
        assert any("ONE" in row and "TWO" in row for row in output.splitlines())
        back_to_vc(session)


def test_command_copy_con_reads_lines_echoes_and_stops_at_ctrl_z():
    with running_vc() as (session, work, _):
        start_command(session)
        for name, text in (("NOTE.TXT", "first typed line"), ("AGAIN.TXT", "second copy")):
            command = f"copy con {name}"
            session.send(command.encode(), "enter")
            # COMMAND buffers the source before it creates the destination.
            until(session, lambda: command in session.text() and not shell_ready(session))
            session.send(text.encode(), "enter")
            until(session, lambda: re.search(rf"(?m)^{re.escape(text)}\s*$", session.text()))
            session.send(b"last line", "enter")
            until(session, lambda: re.search(r"(?m)^last line\s*$", session.text()))
            session.send(b"\x1a", "enter")
            until(session, lambda: shell_ready(session) and "copied" in session.text().lower())
            assert (work / name).read_bytes() == f"{text}\r\nlast line\r\n".encode()
            assert "^Z" in session.text()
            assert session.poll() is None
        back_to_vc(session)


def test_command_nul_discards_output_without_touching_a_host_file():
    files = {"UNIQUE.TXT": b"keep", "nul": b"host sentinel"}
    with running_vc(files=files) as (session, work, _):
        start_command(session)
        line(session, "dir > nul")
        assert (work / "nul").read_bytes() == b"host sentinel"
        output = session.text().rpartition("dir > nul")[2]
        assert "UNIQUE" not in output
        assert not (work / "NUL").exists()
        back_to_vc(session)


@pytest.mark.parametrize("drive", ["C", "H"])
def test_command_directory_pipe_works_on_every_drive_without_home_writes(drive):
    with running_vc(files={"PIPEA.TXT": b"a", "PIPEB.TXT": b"b"}) as (session, work, config):
        home = config.parents[1]
        sentinels = [home / "%PIPE1.$$$", home / "%PIPE2.$$$"]
        for sentinel in sentinels:
            sentinel.write_bytes(b"private HOME sentinel")
        target = work
        if drive == "H":
            target = home / "PIPEWORK"
            target.mkdir()
            (target / "PIPEA.TXT").write_bytes(b"a")
            (target / "PIPEB.TXT").write_bytes(b"b")
        before = start_command(session)
        if drive == "H":
            line(session, "h:")
            line(session, "cd PIPEWORK")
        line(session, "dir | sort > SORTED.TXT")
        until(session, lambda: (target / "SORTED.TXT").exists() and shell_ready(session))
        output = (target / "SORTED.TXT").read_bytes()
        assert b"PIPEA" in output and b"PIPEB" in output
        assert "translation SORT.EXE" in session.log.read_text()[before:]
        for sentinel in sentinels:
            assert sentinel.read_bytes() == b"private HOME sentinel"
        assert session.poll() is None
        back_to_vc(session)


def test_command_fcb_wildcard_rename_and_delete():
    files = {"RA.TXT": b"blank", "RAB.TXT": b"character", "RABC.TXT": b"not matched"}
    with running_vc(files=files) as (session, work, _):
        start_command(session)
        line(session, "ren RA?.TXT Q??.BAK")
        until(session, lambda: (work / "QA.BAK").exists() and (work / "QAB.BAK").exists())
        assert (work / "QA.BAK").read_bytes() == b"blank"
        assert (work / "QAB.BAK").read_bytes() == b"character"
        assert not (work / "RA.TXT").exists() and not (work / "RAB.TXT").exists()
        line(session, "del Q??.BAK")
        until(session, lambda: not (work / "QA.BAK").exists() and not (work / "QAB.BAK").exists())
        assert (work / "RABC.TXT").read_bytes() == b"not matched"
        back_to_vc(session)
