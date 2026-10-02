"""Native Hack slots refresh installed assets and use NFS-compatible locks."""

from pathlib import Path
import errno
import fcntl
import os
import signal
import stat
import subprocess

import pytest

import test_hack_e2e as hack_e2e
from test_hack_e2e import (
    game_state, move_player, panels, save_hack, shared_installation, until,
    wait_dungeon,
)


def attempt_hack(session, config):
    # VC's native cd accepts the long host path; the original DOS shell
    # needs its short current directory and an 8.3 program name.
    session.send(b"cd " + str(config / "HACK").encode(), "enter")
    until(session, lambda: panels(session.text()) and "HACK" in session.text())
    before = len(session.log.read_text())
    session.send(b"dos2 /c HACK.EXE", "enter")
    until(session, lambda: "native Hack playground:" in session.log.read_text()[before:]
          or "unsupported executable" in session.log.read_text()[before:].lower()
          or (panels(session.text()) and "terminate psp" in session.log.read_text()[before:])
          or session.poll() is not None)
    assert session.poll() is None, session.log.read_text()
    return session.log.read_text()[before:]


def start_playground(session, config, *, first_level=True):
    log = attempt_hack(session, config)
    assert "native Hack playground:" in log, log
    wait_dungeon(session, first_level=first_level)


def kill_session(session):
    os.kill(session.pid, signal.SIGKILL)
    assert session.wait_exit() == -signal.SIGKILL


def test_secondary_upgrade_refreshes_assets_without_replacing_saved_game_or_scores():
    with shared_installation() as (launch, _, config):
        first, second = launch(), launch()
        start_playground(first, config)
        start_playground(second, config)
        move_player(second)
        saved_state = game_state(second.text())
        save_hack(second)
        slot = config / "HACK/PLAY0001"
        assert (slot / "HACK.SAV").is_file()
        kill_session(second)

        # These belong to the player, not to the installed game image.
        mutable = {"HACK.SAV": (slot / "HACK.SAV").read_bytes(),
                   "HACK.99": b"private dungeon level\n",
                   "bones.99": b"private bones\n",
                   "record": b"private score table\n", "perm": b"private lock data\n"}
        for name, content in mutable.items():
            (slot / name).write_bytes(content)
        untouched = {name: (slot / name).stat()
                     for name in ("HACKLIC.TXT", "FENLIC.TXT", "OWLIC.TXT", "VCPLAY.ID")}
        static = ("HACK.EXE", "data", "help", "hh", "rumors")
        for name in static:
            target = slot / name
            old = target.read_bytes()
            target.write_bytes(old[:-1] + bytes([old[-1] ^ 1]))
        (slot / "hh").unlink()  # An incomplete older install is repaired too.
        (config / "HACK/help").write_bytes(b"customized installed help\n")

        replacement = launch()
        assert (slot / "HACK.SAV").read_bytes() == mutable["HACK.SAV"]
        start_playground(replacement, config, first_level=False)
        until(replacement, lambda: game_state(replacement.text()) == saved_state)
        assert not (slot / "HACK.SAV").exists(), "the old save must be restored, not discarded"
        for name in static:
            assert (slot / name).read_bytes() == (config / "HACK" / name).read_bytes(), name
        for name, content in mutable.items():
            if name != "HACK.SAV":
                assert (slot / name).read_bytes() == content, name
        for name, prior in untouched.items():
            after = (slot / name).stat()
            assert (after.st_ino, after.st_mtime_ns) == (prior.st_ino, prior.st_mtime_ns), name


def test_primary_and_secondary_locks_are_read_write_regular_ofd_locks():
    with shared_installation() as (launch, _, config):
        for directory in (config / "HACK", config / "HACK/PLAY0001"):
            session = launch()
            start_playground(session, config)
            lock_path = directory / "VCPLAY.LCK"
            assert lock_path.is_file(), "each slot must lock a regular VCPLAY.LCK, not its directory"
            assert stat.S_ISREG(lock_path.lstat().st_mode)
            infos = []
            for descriptor in Path(f"/proc/{session.pid}/fd").iterdir():
                if os.readlink(descriptor) == str(lock_path):
                    infos.append(Path(f"/proc/{session.pid}/fdinfo/{descriptor.name}").read_text())
            assert len(infos) == 1, infos
            info = dict(line.split(":", 1) for line in infos[0].splitlines())
            assert int(info["flags"].strip(), 8) & os.O_ACCMODE == os.O_RDWR
            assert info["lock"].split()[1:5] == ["OFDLCK", "ADVISORY", "WRITE", "-1"]
            with lock_path.open("r+b") as contender:
                with pytest.raises(OSError) as failure:
                    fcntl.lockf(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
                assert failure.value.errno in (errno.EAGAIN, errno.EACCES)


def test_native_slot_allocator_obeys_an_existing_regular_file_record_lock():
    with shared_installation() as (launch, _, config):
        session = launch()
        primary = config / "HACK"
        with (primary / "VCPLAY.LCK").open("w+b") as owner:
            fcntl.lockf(owner, fcntl.LOCK_EX | fcntl.LOCK_NB)
            start_playground(session, config)
            save_hack(session)
            assert not (primary / "HACK.SAV").exists(), "a live record lock must exclude the primary"
            assert (primary / "PLAY0001/HACK.SAV").is_file()


def test_replaced_retained_lock_file_is_rejected_before_saved_game_is_read():
    with shared_installation() as (launch, _, config):
        session = launch()
        start_playground(session, config)
        save_hack(session)
        primary = config / "HACK"
        saved = (primary / "HACK.SAV").read_bytes()
        lock = primary / "VCPLAY.LCK"
        assert lock.is_file()
        lock.rename(primary / "VCPLAY.OLD")
        lock.touch()
        log = attempt_hack(session, config)
        assert "native Hack playground:" not in log, "an unlinked lock no longer owns the slot"
        assert "translation HACK.EXE" not in log
        assert (primary / "HACK.SAV").read_bytes() == saved


def test_slot_lock_never_follows_a_symlink():
    with shared_installation() as (launch, work, config):
        session = launch()
        target = work / "OWNED.TXT"
        target.write_bytes(b"unrelated host file\n")
        link = config / "HACK/VCPLAY.LCK"
        link.symlink_to(target)
        log = attempt_hack(session, config)
        assert "native Hack playground:" not in log, "a symlink must not become a playground lock"
        assert "translation HACK.EXE" not in log
        assert target.read_bytes() == b"unrelated host file\n"
        assert link.is_symlink()


@pytest.mark.parametrize("link_kind", ("symlink", "hardlink"))
def test_secondary_refresh_replaces_links_without_modifying_their_referents(link_kind):
    with shared_installation() as (launch, work, config):
        first, second = launch(), launch()
        start_playground(first, config)
        start_playground(second, config)
        save_hack(second)
        kill_session(second)
        target = work / "OWNED.TXT"
        target.write_bytes(b"this host file is not Hack help\n")
        prior = target.stat()
        help_file = config / "HACK/PLAY0001/help"
        help_file.unlink()
        if link_kind == "symlink":
            help_file.symlink_to(target)
        else:
            os.link(target, help_file)
        replacement = launch()
        start_playground(replacement, config, first_level=False)
        assert help_file.read_bytes() == (config / "HACK/help").read_bytes()
        assert not help_file.is_symlink()
        assert target.read_bytes() == b"this host file is not Hack help\n"
        after = target.stat()
        assert (after.st_ino, after.st_size, after.st_mtime_ns) == (
            prior.st_ino, prior.st_size, prior.st_mtime_ns)


def test_upgrade_does_not_refresh_a_slot_still_owned_by_another_vc():
    with shared_installation() as (launch, _, config):
        first, second = launch(), launch()
        start_playground(first, config)
        start_playground(second, config)
        save_hack(second)  # The native VC must retain its slot even between games.
        owned = config / "HACK/PLAY0001"
        saved = (owned / "HACK.SAV").read_bytes()
        outdated = b"old installed help, still locked by another VC\n"
        (owned / "help").write_bytes(outdated)
        installed = config / "HACK/help"
        installed.write_bytes(b"new installed help\n")
        third = launch()
        start_playground(third, config)
        save_hack(third)
        assert (config / "HACK/PLAY0002/HACK.SAV").is_file()
        assert (config / "HACK/PLAY0002/help").read_bytes() == installed.read_bytes()
        assert (owned / "help").read_bytes() == outdated
        assert (owned / "HACK.SAV").read_bytes() == saved


def test_secondary_reads_custom_installed_static_symlink_without_modifying_it():
    with shared_installation() as (launch, work, config):
        first, second = launch(), launch()
        custom = work / "MYHELP.TXT"
        custom.write_bytes(b"custom installed help behind a read-only reference\n")
        before = custom.stat()
        installed = config / "HACK/help"
        installed.unlink()
        installed.symlink_to(custom)
        start_playground(first, config)
        start_playground(second, config)
        assert (config / "HACK/PLAY0001/help").read_bytes() == custom.read_bytes()
        assert installed.is_symlink()
        after = custom.stat()
        assert (after.st_ino, after.st_size, after.st_mtime_ns) == (
            before.st_ino, before.st_size, before.st_mtime_ns)


@pytest.fixture(scope="session")
def lock_replacement_hook(tmp_path_factory):
    library = tmp_path_factory.mktemp("hack-lock-race") / "replace-lock.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-std=gnu11", "-Wall", "-Wextra",
                    str(Path(__file__).with_name("hack_lock_race.c")), "-ldl", "-o", str(library)],
                   check=True, capture_output=True, text=True)
    return library


def test_lock_replaced_between_open_and_fcntl_is_rejected(monkeypatch, lock_replacement_hook):
    original_session = hack_e2e.VcSession

    def instrumented_session(*args, **kwargs):
        kwargs["extra_env"] = {**kwargs.get("extra_env", {}), "LD_PRELOAD": str(lock_replacement_hook)}
        return original_session(*args, **kwargs)

    monkeypatch.setattr(hack_e2e, "VcSession", instrumented_session)
    with shared_installation() as (launch, _, config):
        session = launch()
        primary = config / "HACK"
        saved = b"a pre-existing save must not be touched before the slot is locked\n"
        (primary / "HACK.SAV").write_bytes(saved)
        log = attempt_hack(session, config)
        assert (primary / "VCPLAY.LCK.OLD").is_file(), "the open/fcntl race hook must run"
        assert (primary / "VCPLAY.LCK").stat().st_ino != (primary / "VCPLAY.LCK.OLD").stat().st_ino
        assert "native Hack playground:" not in log, "locking a replaced inode must not claim its pathname"
        assert "translation HACK.EXE" not in log
        assert (primary / "HACK.SAV").read_bytes() == saved
