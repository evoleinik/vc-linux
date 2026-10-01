"""VC's own images survive missing files and concurrent installer updates."""
from contextlib import contextmanager
from pathlib import Path
import os
import pty
import resource
import shutil
import signal
import subprocess
import sys
import tempfile

import pytest

from test_gwbasic_e2e import ROOT, VcSession, panels, running_vc, until


MSDOS_PROGRAMS = ("COMMAND.COM", "EDLIN.COM", "DEBUG.COM", "FIND.EXE", "MORE.COM", "SORT.EXE", "FC.EXE")
PROGRAMS = ("VC.COM", "VC.OVL", "GWBASIC.EXE", "BOOTLOGO.COM", "ROGUE.EXE", "VZ.COM", "KERMIT.EXE",
            "DOS2.COM", *(f"DOS2/{name}" for name in MSDOS_PROGRAMS))


@contextmanager
def another_vc(work, config):
    # Share only the installation; each process owns its log and screen dump.
    home = work.parent / "another-home"
    home.mkdir()
    session = VcSession(work, home, extra_env={"XDG_CONFIG_HOME": str(config.parent)})
    try:
        yield session
    finally:
        session.close()


def test_install_bootlogo_uses_its_own_name():
    with running_vc() as (_, _, config):
        assert (config / "BOOTLOGO.COM").is_file(), "bootLogo needs its own DOS PATH name"
        assert (config / "BOOTLOGO.COM").read_bytes() == (ROOT / "build/bootlogo/LOGO.COM").read_bytes()
        assert not (config / "LOGO.COM").exists(), "the bundled interpreter must not shadow host logo"


def test_install_msdos_programs_in_config_without_home_writes():
    with tempfile.TemporaryDirectory(prefix="vcdos-", dir="/tmp") as path:
        root = Path(path)
        work, user_home, config_base = root / "work", root / "home", root / "config"
        work.mkdir()
        user_home.mkdir()
        sentinel = user_home / "OWNED.TXT"
        sentinel.write_bytes(b"the user's own file\n")
        before = sentinel.stat()
        session = VcSession(work, user_home, extra_env={
            "XDG_CONFIG_HOME": str(config_base),
            "XDG_CACHE_HOME": str(root / "cache"),
            "VC_LOG": str(root / "vc.log"),
            "VC_SCREEN_DUMP": str(root / "screen.txt"),
            "VC_FRAME_DUMP": str(root / "frame.pgm"),
        })
        try:
            session.wait_for("10Quit", timeout=15)
            until(session, lambda: panels(session.text()))
            config = config_base / "vc-linux"
            for name in MSDOS_PROGRAMS:
                assert (config / "DOS2" / name).read_bytes() == (ROOT / "build/msdos2" / name).read_bytes()
                assert not (config / name).exists(), "a flat DOS program would capture a host command"
            assert (config / "DOS2.COM").read_bytes() == (ROOT / "build/msdos2/COMMAND.COM").read_bytes()
            assert (config / "DOS.TXT").read_bytes() == (ROOT / "data/DOS.TXT").read_bytes()
            assert (config / "DOSLIC.TXT").read_bytes() == (ROOT / "third_party/msdos2/LICENSE").read_bytes()
            assert not (config / "DOS").exists(), "the browser's DOS directory is not installed on Linux"
            assert not (config / "SRC").exists(), "the browser's source demo must not install on Linux"
            assert not (config / "GAMES").exists(), "the browser's games demo must not install on Linux"
            assert sorted(entry.name for entry in user_home.iterdir()) == ["OWNED.TXT"]
            after = sentinel.stat()
            assert (after.st_ino, after.st_size, after.st_mtime_ns) == (before.st_ino, before.st_size, before.st_mtime_ns)
            assert sentinel.read_bytes() == b"the user's own file\n"
        finally:
            session.close()


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_install_leaves_logo_command_for_host(monkeypatch, quick):
    # Debian's UCBLogo owns the command "logo". A small host executable proves
    # shell routing without requiring UCBLogo to be installed on the test host.
    monkeypatch.setenv("PATH", ".:/usr/bin:/bin")
    with running_vc(quick=quick, files={
            "logo": b"#!/bin/sh\nprintf 'host logo ran' > HOSTLOGO.TXT\n",
    }) as (session, work, _):
        (work / "logo").chmod(0o755)
        result = work / "HOSTLOGO.TXT"
        before = len(session.log.read_text())
        session.send(b"logo", "enter")
        until(session, lambda: result.exists() or "translation LOGO.COM" in session.log.read_text()[before:])
        assert result.exists(), "the bundled LOGO.COM captured the host's logo command"
        assert result.read_text() == "host logo ran"
        until(session, lambda: panels(session.text()))
        assert session.poll() is None


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("command", [
    "find . -name MATCH.TXT", "sort INPUT.TXT", "more INPUT.TXT",
    "fc INPUT.TXT MATCH.TXT", "command -v git",
], ids=["find", "sort", "more", "fc", "command"])
def test_install_preserves_host_shell_commands(monkeypatch, quick, command):
    # No DOS executables in this working directory. Compare real /bin/sh
    # output AND status, including a shell's own missing-command error for
    # fc on hosts that do not provide it. Merely returning to VC is not a
    # pass: all five names were captured by the bundled DOS installation.
    monkeypatch.setenv("PATH", "/usr/bin:/bin")
    with running_vc(quick=quick, files={
            "INPUT.TXT": b"beta\nalpha\n", "MATCH.TXT": b"match\n",
    }) as (session, work, _):
        # MORE also checks whether stdin is a tty when stdout is redirected.
        # Give the reference shell the same terminal/input conditions as VC.
        master, slave = pty.openpty()
        try:
            baseline = subprocess.run(["/bin/sh", "-c", command, "sh"], cwd=work,
                                      env={"PATH": "/usr/bin:/bin", "LANG": "C.UTF-8",
                                           "TERM": "xterm-256color"}, stdin=slave,
                                      stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                      timeout=8)
        finally:
            os.close(slave)
            os.close(master)
        output, status = work / "HOST.TXT", work / "STATUS.TXT"
        script = command + " > HOST.TXT 2>&1; printf '%s' $? > STATUS.TXT"
        assert len(script) < 126
        before = len(session.log.read_text())
        session.send(script.encode(), "enter")
        until(session, lambda: status.exists() or any(
            f"translation {name}" in session.log.read_text()[before:]
            for name in MSDOS_PROGRAMS) or session.poll() is not None)
        log = session.log.read_text()[before:]
        assert status.exists(), f"{command!r} did not reach /bin/sh:\n{log}"
        until(session, lambda: panels(session.text()))
        assert output.read_bytes() == baseline.stdout
        assert int(status.read_text()) == baseline.returncode
        assert f"run: {script} (in {work})" in log
        assert not any(f"translation {name}" in log for name in MSDOS_PROGRAMS)
        assert session.poll() is None


@pytest.mark.parametrize("old_file", ["installed", "modified", "symlink", "dangling-symlink"])
def test_install_retires_only_unchanged_flat_dos_programs(old_file):
    with running_vc() as (_, work, config):
        before = {}
        for name in MSDOS_PROGRAMS:
            original = (ROOT / "build/msdos2" / name).read_bytes()
            legacy, target = config / name, work / ("USER" + Path(name).suffix)
            if legacy.exists():
                legacy.unlink()
            if old_file in ("symlink", "dangling-symlink"):
                # Use a distinct target for each image, not a shared .COM.
                target = work / ("USER-" + name)
                if old_file == "symlink":
                    target.write_bytes(original)
                legacy.symlink_to(target)
                contents = original
            else:
                contents = original if old_file == "installed" else original[:-1] + bytes([original[-1] ^ 1])
                legacy.write_bytes(contents)
            before[name] = (legacy.lstat(), contents, target)
        with another_vc(work, config) as second:
            second.wait_for("10Quit", timeout=15)
            until(second, lambda: panels(second.text()))
            for name, (previous, contents, target) in before.items():
                legacy = config / name
                if old_file == "installed":
                    assert not legacy.exists(), f"the old {name} still shadows a host command"
                else:
                    now = legacy.lstat()
                    assert (now.st_ino, now.st_mtime_ns, now.st_mode) == (
                        previous.st_ino, previous.st_mtime_ns, previous.st_mode)
                    if old_file in ("symlink", "dangling-symlink"):
                        assert legacy.is_symlink() and legacy.readlink() == target
                        if old_file == "symlink":
                            assert target.read_bytes() == contents
                        else:
                            assert not target.exists()
                    else:
                        assert legacy.read_bytes() == contents
                assert (config / "DOS2" / name).read_bytes() == (ROOT / "build/msdos2" / name).read_bytes()
            assert (config / "DOS2.COM").read_bytes() == (ROOT / "build/msdos2/COMMAND.COM").read_bytes()


@pytest.mark.parametrize("old_file", ["installed", "modified", "symlink"])
def test_install_retires_only_unchanged_logo_image(old_file):
    with running_vc() as (_, work, config):
        original = (ROOT / "build/bootlogo/LOGO.COM").read_bytes()
        legacy = config / "LOGO.COM"
        if legacy.exists():
            legacy.unlink()
        if old_file == "symlink":
            target = work / "USER.COM"
            target.write_bytes(original)
            legacy.symlink_to(target)
        else:
            contents = original if old_file == "installed" else original[:-1] + bytes([original[-1] ^ 1])
            legacy.write_bytes(contents)
        with another_vc(work, config) as second:
            second.wait_for("10Quit", timeout=15)
            until(second, lambda: panels(second.text()))
            if old_file == "installed":
                assert not legacy.exists(), "an earlier bundled LOGO.COM still shadows the host command"
            elif old_file == "modified":
                assert legacy.read_bytes() == contents, "a user-modified old image must be preserved"
            else:
                assert legacy.is_symlink(), "a user-created link is not an installed default"
                assert target.read_bytes() == original
            assert (config / "BOOTLOGO.COM").read_bytes() == original


@pytest.mark.parametrize("previous", ["installed", "customized", "current", "symlink", "dangling-symlink"])
def test_install_upgrades_only_unmodified_vz_backup_default(previous):
    original = (ROOT / "third_party/vzeditor/VZ-IBM/VZIBM.DEF").read_bytes()
    option = b"\r\nEb-\t\t\t;make backup\r\n"
    assert original.count(option) == 1
    current = original.replace(option, option.replace(b"Eb-", b"Eb+"))
    with running_vc() as (_, work, config):
        definition = config / "VZ.DEF"
        definition.unlink()
        target = work / "USER.DEF"
        if previous in ("symlink", "dangling-symlink"):
            if previous == "symlink":
                target.write_bytes(original)
            definition.symlink_to(target)
        else:
            contents = current if previous == "current" else original
            if previous == "customized":
                # One unrelated setting, with exactly the same file size and
                # backup still off: the migration must compare every byte.
                contents = original.replace(b"\r\nEi+\t", b"\r\nEi-\t", 1)
                assert contents != original and len(contents) == len(original)
            definition.write_bytes(contents)
            definition.chmod(0o640)
            os.utime(definition, ns=(1_000_000_000, 1_000_000_000))
        before = definition.lstat()
        with another_vc(work, config) as second:
            second.wait_for("10Quit", timeout=15)
            until(second, lambda: panels(second.text()))
            if previous == "installed":
                assert definition.read_bytes() == current, "the unchanged old default must enable backups"
                assert definition.stat().st_ino != before.st_ino, "publish the upgrade atomically"
                assert definition.stat().st_mode & 0o777 == 0o640
            else:
                after = definition.lstat()
                assert (after.st_ino, after.st_mtime_ns, after.st_mode) == (
                    before.st_ino, before.st_mtime_ns, before.st_mode), "user settings must not be rewritten"
                if previous in ("symlink", "dangling-symlink"):
                    assert definition.is_symlink(), "a user-created link is not an installed default"
                    assert definition.readlink() == target
                    if previous == "symlink":
                        assert target.read_bytes() == original
                    else:
                        assert not target.exists()
                else:
                    assert definition.read_bytes() == contents


def web_demo(destination):
    return subprocess.run([
        sys.executable, str(ROOT / "tools/web_demo.py"), str(destination),
        str(ROOT / "build/gwbasic/GWBASIC.EXE"), str(ROOT / "build/games"),
        str(ROOT / "build/bootlogo/LOGO.COM"),
        str(ROOT / "build/rogue/ROGUE.EXE"),
        str(ROOT / "build/vz/VZ.COM"),
        str(ROOT / "build/kermit/KERMIT.EXE"),
        str(ROOT / "build/msdos2"),
    ], text=True, capture_output=True)


def legacy_logo_guide():
    # The only change to this guide is the command/file rename. Seed the
    # actual previous generated CRLF file, not a made-up stale-file sample.
    source = ROOT / "web/BOOTLOGO.TXT"
    if not source.exists():
        source = ROOT / "web/LOGO.TXT"
    return source.read_text(encoding="ascii").replace("BOOTLOGO", "LOGO").replace("\n", "\r\n").encode("ascii")


@pytest.mark.parametrize("previous", [False, True], ids=["fresh", "upgrade"])
def test_web_demo_installs_bootlogo_only(tmp_path, previous):
    original = (ROOT / "build/bootlogo/LOGO.COM").read_bytes()
    if previous:
        (tmp_path / "LOGO.COM").write_bytes(original)
        (tmp_path / "LOGO.TXT").write_bytes(legacy_logo_guide())
    result = web_demo(tmp_path)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "BOOTLOGO.COM").is_file(), "web H: must install BOOTLOGO.COM"
    assert (tmp_path / "BOOTLOGO.COM").read_bytes() == original
    assert (tmp_path / "BOOTLOGO.TXT").is_file()
    assert not (tmp_path / "LOGO.COM").exists()
    assert not (tmp_path / "LOGO.TXT").exists()


def test_web_demo_installs_real_rogue_and_licenses_in_games(tmp_path):
    result = web_demo(tmp_path)
    assert result.returncode == 0, result.stderr
    assert (tmp_path / "GAMES/ROGUE.EXE").read_bytes() == (ROOT / "build/rogue/ROGUE.EXE").read_bytes()
    assert not (tmp_path / "ROGUE.EXE").exists(), "one real Rogue file belongs in H:\\GAMES"
    assert (tmp_path / "GAMES/ROGUELIC.TXT").read_bytes() == (ROOT / "third_party/rogue/LICENSE.TXT").read_bytes()
    assert b"public domain" in (tmp_path / "GAMES/PDCLIC.TXT").read_bytes()
    assert (tmp_path / "GAMES/OWLIC.TXT").read_bytes() == (ROOT / "build/rogue/OWLIC.TXT").read_bytes()
    readme = (tmp_path / "README.TXT").read_text()
    assert all(text in readme for text in ("ROGUE.EXE", "Q then y", "S then y", "h (left)"))


def test_web_demo_installs_source_built_dos_shell_and_utilities(tmp_path):
    result = web_demo(tmp_path)
    assert result.returncode == 0, result.stderr
    for name in MSDOS_PROGRAMS:
        target = tmp_path / name if name == "COMMAND.COM" else tmp_path / "DOS" / name
        assert target.read_bytes() == (ROOT / "build/msdos2" / name).read_bytes()
        if name != "COMMAND.COM":
            assert not (tmp_path / name).exists(), "utilities belong in H:\\DOS, not H:'s root"
    assert (tmp_path / "DOS/DOS.TXT").read_bytes() == (ROOT / "data/DOS.TXT").read_bytes()
    assert (tmp_path / "DOS/DOSLIC.TXT").read_bytes() == (ROOT / "third_party/msdos2/LICENSE").read_bytes()
    guide = (tmp_path / "DOS/DOS.TXT").read_text()
    assert all(word in guide for word in (*MSDOS_PROGRAMS, "Microsoft", "MIT", "github.com/microsoft/MS-DOS"))
    assert "1." in guide and "2." in guide and "3." in guide


def test_web_demo_installs_vz_and_all_definitions_alongside_rogue(tmp_path):
    result = web_demo(tmp_path)
    assert result.returncode == 0, result.stderr
    vendor = ROOT / "third_party/vzeditor"
    assert (tmp_path / "VZ.COM").read_bytes() == (vendor / "VZ-IBM/US/VZUS.COM").read_bytes()
    assert (tmp_path / "GAMES/ROGUE.EXE").read_bytes() == (ROOT / "build/rogue/ROGUE.EXE").read_bytes()
    assert (tmp_path / "VZLIC.TXT").read_bytes() == (vendor / "LICENSE").read_bytes()
    for installed, source in (("VZ.DEF", "VZIBM.DEF"), ("VZFLE.DEF", "VZFLE.DEF"),
                              ("HELPE.DEF", "HELPE.DEF"), ("BLOCK.DEF", "BLOCK.DEF"),
                              ("PALET.DEF", "PALET.DEF"), ("BW.DEF", "BW.DEF")):
        expected = (vendor / "VZ-IBM" / source).read_bytes()
        if installed == "VZ.DEF":
            option = b"\r\nEb-\t\t\t;make backup\r\n"
            assert expected.count(option) == 1
            expected = expected.replace(option, option.replace(b"Eb-", b"Eb+"))
        assert (tmp_path / installed).read_bytes() == expected
    readme = (tmp_path / "README.TXT").read_text()
    assert all(text in readme for text in ("VZ Editor", "VZ NEW.TXT", "Alt-S", "Alt-Q"))


@pytest.mark.parametrize("name", ["LOGO.COM", "LOGO.TXT"])
def test_web_demo_preserves_modified_retired_files(tmp_path, name):
    old = tmp_path / name
    old.write_bytes(b"my own file\n")
    result = web_demo(tmp_path)
    assert result.returncode != 0, "an unknown stale file must stop the build, not be overwritten"
    assert name in result.stderr
    assert old.read_bytes() == b"my own file\n"


@pytest.mark.parametrize("damage", ["deleted", "truncated", "changed", "config-deleted"])
def test_vc_overlay_does_not_depend_on_installed_file(damage):
    with running_vc() as (session, work, config):
        overlay = config / "VC.OVL"
        if damage == "config-deleted":
            shutil.rmtree(config)
        elif damage == "deleted":
            overlay.unlink()
        elif damage == "truncated":
            overlay.write_bytes(b"")
        else:
            contents = bytearray(overlay.read_bytes())
            contents[-1] ^= 1
            overlay.write_bytes(contents)

        # COMSPEC commands return through VC.COM, which EXECs VC.OVL again.
        # A real side effect and fresh panel entry exclude the old idle screen.
        for name in ("FIRST.TXT", "SECOND.TXT"):
            session.send(f"printf alive > {name}".encode(), "enter")
            until(session, lambda: (work / name).exists() or session.poll() is not None)
            until(session, lambda: session.poll() is not None or (
                panels(session.text()) and name.split(".")[0] in session.text()))
            assert session.poll() is None, session.text()
            assert (work / name).read_text() == "alive"
            assert session.text() == session.memory_text()


def test_install_skips_matching_program_images():
    with running_vc() as (_, work, config):
        expected = {}
        for name in PROGRAMS:
            path = config / name
            path.chmod(0o640)
            # Pin mtime so a fast second start cannot hide a rewrite.
            os.utime(path, ns=(1_000_000_000, 1_000_000_000))
            info = path.stat()
            expected[name] = (info.st_ino, info.st_mtime_ns, info.st_mode & 0o777)
        with another_vc(work, config) as second:
            second.wait_for("10Quit", timeout=15)
            until(second, lambda: panels(second.text()))
            actual = {name: ((config / name).stat().st_ino,
                             (config / name).stat().st_mtime_ns,
                             (config / name).stat().st_mode & 0o777) for name in PROGRAMS}
            assert actual == expected


@pytest.mark.parametrize("name", PROGRAMS)
def test_install_replaces_changed_image_atomically(name):
    with running_vc() as (_, work, config):
        path = config / name
        expected = path.read_bytes()
        old_contents = b"previous installed image\n"
        path.write_bytes(old_contents)
        path.chmod(0o640)
        with path.open("rb") as old:
            old_inode = os.fstat(old.fileno()).st_ino
            with another_vc(work, config) as second:
                second.wait_for("10Quit", timeout=15)
                until(second, lambda: panels(second.text()))
                assert path.read_bytes() == expected
                assert path.stat().st_mode & 0o777 == 0o640
                # A reader holding the old inode never sees a partial rewrite.
                assert old.read() == old_contents
                assert path.stat().st_ino != old_inode


def test_failed_install_preserves_previous_image(monkeypatch):
    with running_vc() as (_, work, config):
        path = config / "VC.COM"
        old_contents = b"previous installed image\n"
        path.write_bytes(old_contents)
        old_inode = path.stat().st_ino
        original_execve = os.execve

        def exec_with_small_file_limit(*args):
            # VcSession calls execve only in its forked child. Limit that
            # process, never pytest, and turn SIGXFSZ into an EFBIG write error.
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            _, hard = resource.getrlimit(resource.RLIMIT_FSIZE)
            resource.setrlimit(resource.RLIMIT_FSIZE, (4096, hard))
            original_execve(*args)

        monkeypatch.setattr(os, "execve", exec_with_small_file_limit)
        before = set(config.iterdir())
        with another_vc(work, config) as second:
            assert second.wait_exit() == 1
            assert "cannot write" in second.text()
        assert path.read_bytes() == old_contents
        assert path.stat().st_ino == old_inode
        assert set(config.iterdir()) == before, "failed install left a temporary file"
