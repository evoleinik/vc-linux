"""Unedited VC 4.05 and its setup, nested inside 4.99 in real Linux PTYs."""
import shutil

import pytest

from test_gwbasic_e2e import ROOT, panels, running_vc, select, until
from test_door_e2e import paths, running, select_file
from test_msdos_e2e import back_to_vc, shell_ready, start_command


VIEW_TEXT = "The first line viewed by VC 4.05."


def old_panels(text):
    lines = text.splitlines()
    return len(lines) == 25 and "╔" in lines[0] and "10Quit" in lines[24]


def start_vc405(session, command=b"vc405"):
    before = len(session.log.read_text())
    session.send(command, "enter")
    until(session, lambda: "translation VC405.COM" in session.log.read_text()[before:]
          or session.poll() is not None, timeout=15)
    assert session.poll() is None, session.log.read_text()
    until(session, lambda: old_panels(session.text()) and not session.screen.cursor.hidden)
    # A familiar panel alone could be the parent redrawing itself. 4.05's
    # own user-screen banner and the byte-matched load both prove identity.
    session.send(b"\x0f")
    session.wait_for("Version 4.05")
    session.send(b"\x0f")
    until(session, lambda: old_panels(session.text()))
    if not panels(session.text()):
        # Source defaults deliberately hide the left/inactive panel
        # (VC.ASM:1451). Use 4.05's own Ctrl-P to show both for this gate.
        session.send(b"\x10")
        until(session, lambda: panels(session.text()))


def return_to_modern_vc(session):
    before = len(session.log.read_text())
    session.send("f10")
    until(session, lambda: "Do you want to quit" in session.text()
          or "translation VC.OVL" in session.log.read_text()[before:]
          or session.poll() is not None)
    assert session.poll() is None, session.log.read_text()
    if "Do you want to quit" in session.text():
        session.send("enter")
    until(session, lambda: panels(session.text()) or "Press ENTER" in session.text()
          or session.poll() is not None)
    assert session.poll() is None, session.log.read_text()
    if "Press ENTER" in session.text():
        session.send("enter")
    until(session, lambda: panels(session.text()))
    until(session, lambda: "translation VC.OVL" in session.log.read_text()[before:])
    assert session.text() == session.memory_text()


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_vc405_command_panels_f3_and_f10(quick):
    with running_vc(quick=quick, files={
        "VIEW.TXT": (VIEW_TEXT + "\r\nSecond line.\r\n").encode(),
        "SECOND.TXT": b"Another file.\r\n",
    }) as (session, _, config):
        assert (config / "VC405.COM").read_bytes() == (ROOT / "build/vc405/VC.COM").read_bytes()
        start_vc405(session)
        assert "VIEW" in session.text().upper() and "SECOND" in session.text().upper()
        select(session, "view.txt")
        session.send("f3")
        session.wait_for(VIEW_TEXT)
        session.send("esc")
        until(session, lambda: panels(session.text()))
        return_to_modern_vc(session)


@pytest.mark.parametrize("name", ["OLD.EXE", "VC.COM"])
def test_vc405_dos2_complete_identical_renamed_copy(name):
    with running_vc() as (session, work, _):
        shutil.copyfile(ROOT / "build/vc405/VC.COM", work / name)
        # Native typed names retain their pre-4.05 host meaning. DOS2's
        # own EXEC still accepts an identical image under any filename.
        start_command(session)
        start_vc405(session, name.encode())
        session.send("f10")
        session.wait_for("Do you want to quit")
        session.send("enter")
        until(session, lambda: shell_ready(session))
        back_to_vc(session)


def test_vc405_save_uses_private_ini_and_setup_reads_it():
    with running_vc(files={"VIEW.TXT": (VIEW_TEXT + "\r\n").encode()}) as (session, work, config):
        modern = config / "VC.INI"
        original = modern.read_bytes()
        old = config / "VC405/VC.INI"
        assert not old.exists()
        start_vc405(session)
        session.send(b"\x1b[20;2~")  # Shift-F9: the original Save setup dialog.
        session.wait_for("Do you wish to save")
        session.send("enter")
        until(session, lambda: old.exists() and panels(session.text()))
        assert modern.read_bytes() == original, "4.05 replaced 4.99's incompatible VC.INI"
        saved = old.read_bytes()
        assert saved.startswith(b"VVV") and saved != original
        assert not (work / "VC.INI").exists(), "4.05 leaked config into the panel directory"
        return_to_modern_vc(session)

        # Setup is intentionally not a new flat PATH command on Linux.
        # Its real bytes also work under another filename, with the same VC=.
        shutil.copyfile(config / "VC405/VCSETUP.COM", work / "SETOLD.COM")
        start_command(session)
        before = len(session.log.read_text())
        session.send(b"SETOLD.COM", "enter")
        session.wait_for("F2   Configuration")
        assert "translation VCSETUP.COM" in session.log.read_text()[before:]
        assert "not found" not in session.text().lower(), session.text()
        session.send("f2")
        session.wait_for("Auto menus")
        option = next(row for row in session.text().splitlines() if "Auto menus" in row)
        session.send(b" ")
        until(session, lambda: next(row for row in session.text().splitlines()
                                   if "Auto menus" in row) != option)
        session.send("enter")
        session.wait_for("F2   Configuration")
        session.send("f10")
        session.wait_for("Do you wish to save")
        session.send("enter")
        until(session, lambda: shell_ready(session))
        updated = old.read_bytes()
        assert updated != saved and updated.startswith(b"VVV")
        assert sum(updated[:-2]) & 0xFFFF == int.from_bytes(updated[-2:], "little")
        assert modern.read_bytes() == original
        assert session.poll() is None
        back_to_vc(session)


def test_vc405_dos2_loader_environment_and_return():
    with running_vc(files={"VIEW.TXT": (VIEW_TEXT + "\r\n").encode()}) as (session, _, config):
        modern = (config / "VC.INI").read_bytes()
        start_command(session)
        before = len(session.log.read_text())
        start_vc405(session)
        assert "translation VC405.COM (DOS-hosted loader)" in session.log.read_text()[before:]
        select(session, "view.txt")
        session.send("f3")
        session.wait_for(VIEW_TEXT)
        session.send("esc")
        until(session, lambda: panels(session.text()))
        session.send("f10")
        session.wait_for("Do you want to quit")
        session.send("enter")
        until(session, lambda: shell_ready(session))
        assert (config / "VC405/VC.INI").read_bytes().startswith(b"VVV")
        assert (config / "VC.INI").read_bytes() == modern
        assert "No translated code" not in session.log.read_text()[before:]
        back_to_vc(session)


@pytest.mark.parametrize("command", ["vc", "vcsetup"])
def test_vc405_install_does_not_capture_other_host_commands(monkeypatch, command):
    monkeypatch.setenv("PATH", ".:/usr/bin:/bin")
    with running_vc(files={command: b"#!/bin/sh\nprintf 'host command' > HOST.TXT\n"}) as (session, work, _):
        (work / command).chmod(0o755)
        session.send(command.encode(), "enter")
        until(session, lambda: (work / "HOST.TXT").exists())
        assert (work / "HOST.TXT").read_text() == "host command"
        until(session, lambda: panels(session.text()))
        assert session.poll() is None


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("arguments", [False, True], ids=["bare", "argv"])
@pytest.mark.parametrize("command,filename,source,translation", [
    ("vc", "VC.COM", "VC.COM", "VC.COM"),
    ("vC", "VC.COM", "VC.COM", "VC.COM"),
    ("vc", "VC.COM", "VC405.COM", "VC405.COM"),
    ("vcsetup", "VCSETUP.COM", "VC405/VCSETUP.COM", "VCSETUP.COM"),
    ("old", "OLD.EXE", "VC405.COM", "VC405.COM"),
    ("oldset", "OLDSET.COM", "VC405/VCSETUP.COM", "VCSETUP.COM"),
    ("vc405.com", "VC405.COM", "VC405.COM", "VC405.COM"),
    ("vc405.exe", "VC405.EXE", "VC405.COM", "VC405.COM"),
], ids=["vc499", "vc499-mixed-case", "vc405-vc", "vcsetup", "renamed-vc405",
        "renamed-setup", "vc405-com", "vc405-exe"])
def test_native_host_commands_win_over_vc_images(monkeypatch, tmp_path, quick, arguments, command,
                                                filename, source, translation):
    # A byte-identical DOS image must not capture a formerly native command.
    # In particular VC.COM can be left in cwd by another XDG installation.
    # Keep host stubs off the DOS search path, including suffixed commands
    # whose host filename would otherwise mask the DOS image in cwd.
    host_bin = tmp_path / "bin"
    host_bin.mkdir()
    script = host_bin / command
    script.write_bytes(b'#!/bin/sh\nprintf "%s\\n" "$#" "$@" > HOST.TXT\npwd > CWD.TXT\n')
    script.chmod(0o755)
    monkeypatch.setenv("PATH", f"{host_bin}:/usr/bin:/bin")
    with running_vc(quick=quick) as (session, work, config):
        shutil.copyfile(config / source, work / filename)
        assert (work / filename).read_bytes() == (config / source).read_bytes()
        original = (config / "VC.INI").read_bytes()
        before = len(session.log.read_text())
        session.send(command.encode() + (b" one 'two words'" if arguments else b""), "enter")
        until(session, lambda: (work / "HOST.TXT").exists()
              or f"translation {translation}" in session.log.read_text()[before:]
              or session.poll() is not None)
        assert (work / "HOST.TXT").exists(), (
            f"native {command!r} was captured by byte-identical {filename}:\n"
            + session.log.read_text()[before:])
        until(session, lambda: (work / "CWD.TXT").exists() and panels(session.text()))
        assert (work / "HOST.TXT").read_text() == ("2\none\ntwo words\n" if arguments else "0\n")
        assert (work / "CWD.TXT").read_text() == str(work) + "\n"
        assert f"translation {translation}" not in session.log.read_text()[before:]
        assert (config / "VC.INI").read_bytes() == original
        assert not (work / "VC.INI").exists()
        assert session.poll() is None


def test_door_vc405_panels_f3_f10_and_config_isolation(paths):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        modern = drive / ".VC/VC.INI"
        original = modern.read_bytes()
        session.type_line(r"cd H:\VC405")
        session.until(lambda: panels(session.text()) and "H:\\VC405>" in session.text())
        select_file(session, "VC.COM")
        session.send("enter")
        session.until(lambda: old_panels(session.text()) and "Version 4.05" in session.text())
        assert not panels(session.text()), "4.05's original right-only defaults were not shown"
        select_file(session, "LICENSE.TXT")
        session.send("f3")
        session.wait_for("Copyright 1991-2000 Vsevolod V. Volkov")
        session.send("esc")
        session.until(lambda: old_panels(session.text()) and not session.screen.cursor.hidden)
        session.send(b"\x1b[20;2~")
        session.wait_for("Do you wish to save")
        session.send("enter")
        session.until(lambda: (drive / "VC405/VC.INI").exists() and old_panels(session.text()))
        session.send("f10")
        session.wait_for("Do you want to quit")
        session.send("enter")
        session.until(lambda: panels(session.text()) or "Press ENTER" in session.text())
        if "Press ENTER" in session.text():
            session.send("enter")
        # Only the original 4.99 parent has both panels visible in this
        # session; the child retained its source-default right-only layout.
        session.ready()
        assert modern.read_bytes() == original
        assert (drive / "VC405/VC.INI").read_bytes().startswith(b"VVV")
        assert session.poll() is None
