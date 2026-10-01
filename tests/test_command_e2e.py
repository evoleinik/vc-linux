"""Typed Linux commands must not be captured by unrelated DOS executables."""
import os

import pytest

from test_gwbasic_e2e import panels, running_vc
from test_e2e import until
from vcterm import VC


def shell_result(session, command: bytes, result, expected: str) -> None:
    """Wait for command routing, then require the host's observable result."""
    before = len(session.log.read_text())
    session.send(command, "enter")

    def routed():
        log = session.log.read_text()[before:]
        return (result.exists() and expected in result.read_text()
                or "DOS command error" in log or "translation VC.COM" in log)

    until(session, routed)
    log = session.log.read_text()[before:]
    assert result.exists(), f"native command produced no result\n{log}\n{session.text()}"
    assert expected in result.read_text()
    assert f"run: {command.decode()}" in log
    until(session, lambda: panels(session.text()))
    assert session.poll() is None
    assert session.text() == session.memory_text()


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_typed_vc_reaches_native_linux_command(monkeypatch, quick):
    # Resolve the actual native binary through the shell's PATH. --help
    # exits without creating another interactive VC or touching its files.
    monkeypatch.setenv("PATH", f"{VC.parent}:/usr/bin:/bin")
    with running_vc(quick=quick) as (session, work, config):
        assert (config / "VC.COM").exists()
        shell_result(session, b"vc --help > NATIVE.TXT", work / "NATIVE.TXT",
                     "usage: vc [DIRECTORY]")


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_find_ignores_untranslated_dos_executable(monkeypatch, quick):
    monkeypatch.setenv("PATH", "/usr/bin:/bin")
    with running_vc(quick=quick, files={"FIND.EXE": b"not a translated image\n"}) as (session, work, _):
        shell_result(session, b"find . > FOUND.TXT", work / "FOUND.TXT", "./FIND.EXE")


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_command_in_execute_only_directory(quick):
    with running_vc(quick=quick) as (session, work, _):
        locked = work / "locked"
        locked.mkdir()
        # These are the effective permissions a non-owner gets from 0711.
        # The test owns its temporary directory, so remove the owner's read
        # and write bits too; chmod(0711) alone would not exercise EACCES.
        locked.chmod(0o111)
        try:
            with pytest.raises(PermissionError):
                os.listdir(locked)
            session.send(b"cd locked", "enter")
            until(session, lambda: "cd locked: ok" in session.log.read_text())
            until(session, lambda: panels(session.text()) and "\\locked>" in session.text())
            shell_result(session, b"printf accessible > ../ACCESS.TXT",
                         work / "ACCESS.TXT", "accessible")
        finally:
            locked.chmod(0o700)
