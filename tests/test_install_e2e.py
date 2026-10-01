"""VC's own images survive missing files and concurrent installer updates."""
from contextlib import contextmanager
import os
import resource
import shutil
import signal

import pytest

from test_gwbasic_e2e import VcSession, panels, running_vc, until


PROGRAMS = ("VC.COM", "VC.OVL", "GWBASIC.EXE")


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
