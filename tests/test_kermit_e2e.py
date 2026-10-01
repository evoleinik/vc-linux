"""Source-built MS-DOS Kermit, launched by VC itself in a real pty.

The TCP test is a required, unskipped loopback gate. The separately named
pipe test is supplemental: it exercises the same Kermit/UART/Hayes/telnet
path when a sandbox forbids sockets, but cannot prove the TCP transport.
"""
from contextlib import contextmanager
from pathlib import Path
import os
import re
import select as io_select
import shutil
import socket
import sys
import tempfile
import threading
import time

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests/e2e"))
sys.path.insert(0, str(ROOT / "tools"))
import vcterm  # noqa: E402
import vcini  # noqa: E402
from vcterm import VcSession  # noqa: E402
from test_e2e import select, until  # noqa: E402
from test_gwbasic_e2e import panels  # noqa: E402

FIXTURE = ROOT / "tests/fixtures/enigma-connect-2026-10-02.bin"
PIPE_BINARY = Path(os.environ.get("VC_TEST_PIPE_BINARY", str(ROOT / "build/vc-pipe-modem")))
PROMPT = re.compile(r"(?m)^MS-Kermit> *$")
LINUX_DIAL_COMMAND = b"kermit take bbs.tak, stay"


class ClientBytes:
    """Discard telnet replies and ANSI DA/DSR reports before echoing input.

    Echoing Kermit's answer to ESC[0c would send it back as another device
    query, creating an endless terminal-identification loop in the fixture.
    The fake server deliberately implements no Hayes or UART behavior.
    """

    def __init__(self):
        self.telnet = "data"
        self.ansi = "data"

    def feed(self, data):
        result = bytearray()
        for byte in data:
            if self.telnet == "iac":
                if byte == 255:
                    self.telnet = "data"
                elif byte in (251, 252, 253, 254):
                    self.telnet = "option"
                    continue
                elif byte == 250:
                    self.telnet = "subnegotiation"
                    continue
                else:
                    self.telnet = "data"
                    continue
            elif self.telnet == "option":
                self.telnet = "data"
                continue
            elif self.telnet == "subnegotiation":
                if byte == 255:
                    self.telnet = "subnegotiation-iac"
                continue
            elif self.telnet == "subnegotiation-iac":
                self.telnet = "data" if byte == 240 else "subnegotiation"
                continue
            elif byte == 255:
                self.telnet = "iac"
                continue

            if self.ansi == "escape":
                self.ansi = "csi" if byte == ord("[") else "osc" if byte == ord("]") else "data"
                continue
            if self.ansi == "csi":
                if 0x40 <= byte <= 0x7E:
                    self.ansi = "data"
                continue
            if self.ansi == "osc":
                if byte == 7:
                    self.ansi = "data"
                elif byte == 27:
                    self.ansi = "osc-escape"
                continue
            if self.ansi == "osc-escape":
                self.ansi = "data" if byte == ord("\\") else "osc"
                continue
            if byte == 27:
                self.ansi = "escape"
            elif byte == 0x9B:
                self.ansi = "csi"
            elif 32 <= byte <= 126 or byte in (10, 13):
                result.append(byte)
        return bytes(result)


class FakeBBS:
    """A local byte replay/echo endpoint, using TCP or explicit test pipes."""

    def __init__(self, mode):
        self.mode = mode
        self.stop = threading.Event()
        self.thread = None
        self.error = None
        self.listener = self.peer = None
        self.read_fd = self.write_fd = None
        self.guest_fds = []
        self.received = bytearray()
        self.wire_received = bytearray()
        self.lines = []
        self.last_outbound = time.monotonic()
        self.environment = {}
        if mode == "tcp":
            # No skip/fallback: a sandbox denying socket() must leave the
            # actual TCP gate visibly red, rather than silently pass it.
            self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.listener.bind(("127.0.0.1", 0))
            self.listener.listen(1)
            self.listener.setblocking(False)
            self.environment["VC_MODEM_555_1992"] = "127.0.0.1:%d" % self.listener.getsockname()[1]
        else:
            assert mode == "pipe"
            guest_rx, self.write_fd = os.pipe()
            self.read_fd, guest_tx = os.pipe()
            self.guest_fds = [guest_rx, guest_tx]
            for fd in self.guest_fds:
                os.set_inheritable(fd, True)
            for fd in (self.read_fd, self.write_fd):
                os.set_blocking(fd, False)
            self.environment.update(VC_MODEM_TEST_RX_FD=str(guest_rx), VC_MODEM_TEST_TX_FD=str(guest_tx))

    def start(self):
        # Start after pty.fork(), avoiding a multithreaded Python fork. The
        # child inherited its pipe ends; only the endpoint's ends stay here.
        for fd in self.guest_fds:
            os.close(fd)
        self.guest_fds.clear()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        try:
            if self.mode == "tcp":
                while not self.stop.is_set():
                    ready, _, _ = io_select.select([self.listener], [], [], 0.05)
                    if ready:
                        self.peer, _ = self.listener.accept()
                        self.peer.setblocking(False)
                        self.read_fd = self.write_fd = self.peer.fileno()
                        break
                if self.stop.is_set():
                    return
            # Keep the captured payload exact. A separate test prompt gives
            # input/echo a clear area after the fixture's ANSI welcome art.
            pending = bytearray(FIXTURE.read_bytes())
            pending.extend(b"\x1b[20;1H\x1b[0m\x1b[J\r\nTEST BBS READY\r\n")
            decoder, line = ClientBytes(), bytearray()
            while not self.stop.is_set():
                readable, writable, _ = io_select.select(
                    [self.read_fd], [self.write_fd] if pending else [], [], 0.05)
                if writable:
                    try:
                        written = os.write(self.write_fd, pending)
                    except BlockingIOError:
                        written = 0
                    del pending[:written]
                if readable:
                    try:
                        data = os.read(self.read_fd, 4096)
                    except BlockingIOError:
                        continue
                    if not data:
                        return
                    self.last_outbound = time.monotonic()
                    self.wire_received.extend(data)
                    text = decoder.feed(data)
                    self.received.extend(text)
                    for byte in text:
                        if byte in (10, 13):
                            if line:
                                self.lines.append(bytes(line))
                                pending.extend(b"\r\nBBS ECHO: " + line + b"\r\n")
                                line.clear()
                        else:
                            line.append(byte)
        except BaseException as error:
            if not self.stop.is_set():
                self.error = error

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=2)
            assert not self.thread.is_alive(), "fake BBS failed to stop"
        if self.peer:
            self.peer.close()
        elif self.mode == "pipe":
            for fd in (self.read_fd, self.write_fd):
                if fd is not None:
                    os.close(fd)
        if self.listener:
            self.listener.close()
        for fd in self.guest_fds:
            os.close(fd)
        self.guest_fds.clear()


class RenderedFrames:
    """Observe displayed frames, not raw fixture or terminal output bytes."""

    def __init__(self, session, needles):
        self.session = session
        self.pending = set(needles)
        self.seen = {}
        self.feed = session.stream.feed
        session.stream.feed = self.observe

    def observe(self, data):
        for byte in data:
            self.feed(bytes([byte]))
            # The fixture clears its banner in the next frame. Reading a
            # whole pty chunk first can lose it despite correct rendering.
            candidates = [text for text in self.pending if ord(text[-1]) == byte]
            if candidates:
                rendered = self.session.text()
                for text in candidates:
                    if text in rendered:
                        self.seen[text] = rendered
                        self.pending.remove(text)

    def close(self):
        self.session.stream.feed = self.feed


@contextmanager
def running_kermit_vc(monkeypatch, *, bbs=None, quick=False, files=None, config_files=None,
                      home_files=None, home_links=None, work_name="work"):
    with tempfile.TemporaryDirectory(prefix="vc-ker-", dir="/tmp") as directory:
        root = Path(directory)
        work, home = root / work_name, root / "home"
        work.mkdir()
        home.mkdir()
        config = root / "config/vc-linux"
        config.mkdir(parents=True)
        diagnostics = root / "diagnostics"
        diagnostics.mkdir()
        for parent, contents in ((work, files), (home, home_files), (config, config_files)):
            for name, data in (contents or {}).items():
                path = parent / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data)
        for name, target in (home_links or {}).items():
            (home / name).symlink_to(target)
        expected_home = home_snapshot(home)
        if quick:
            ini = vcini.VcIni((ROOT / "data/VC.INI").read_bytes())
            ini.set_main("ExecTyp", 1)
            (config / "VC.INI").write_bytes(ini.data)
        with monkeypatch.context() as patch:
            if bbs and bbs.mode == "pipe":
                patch.setattr(vcterm, "VC", PIPE_BINARY)
            assert vcterm.VC.exists(), f"required binary is missing: {vcterm.VC}; run make"
            environment = {
                "XDG_CONFIG_HOME": str(config.parent),
                "XDG_CACHE_HOME": str(root / "cache"),
                "VC_LOG": str(diagnostics / "vc.log"),
                "VC_SCREEN_DUMP": str(diagnostics / "screen.txt"),
                "VC_FRAME_DUMP": str(diagnostics / "frame.pgm"),
            }
            environment.update(bbs.environment if bbs else {})
            session = VcSession(work, home, extra_env=environment)
            try:
                if bbs:
                    bbs.start()
                session.wait_for("10Quit", timeout=20)
                until(session, lambda: panels(session.text()))
                assert_home_unchanged(home, expected_home)
                yield session, work, home, config
                assert_home_unchanged(home, expected_home)
            finally:
                if bbs:
                    bbs.close()
                session.close()


def assert_home_empty(home):
    entries = sorted(path.name for path in home.iterdir())
    assert not entries, f"VC wrote into HOME: {entries}"


def home_snapshot(home):
    result = {}
    for path in home.rglob("*"):
        info = path.lstat()
        contents = path.readlink() if path.is_symlink() else (
            path.read_bytes() if path.is_file() else None)
        result[str(path.relative_to(home))] = (
            info.st_ino, info.st_mtime_ns, info.st_ctime_ns, info.st_mode, contents)
    return result


def assert_home_unchanged(home, expected):
    if not expected:
        assert_home_empty(home)
    assert home_snapshot(home) == expected, "VC changed pre-existing HOME entries"


def quit_vc(session):
    session.send("f10")
    session.wait_for("Do you want to quit the Volkov Commander?")
    session.send("enter")
    assert session.wait_exit() == 0


def test_kermit_install_keeps_home_empty(monkeypatch):
    with running_kermit_vc(monkeypatch) as (session, work, home, config):
        assert_home_empty(home)
        for name in ("BBS.TAK", "KERMIT.TXT"):
            assert (config / name).read_bytes() == (ROOT / "data" / name).read_bytes()
            assert not (work / name).exists()
        assert (config / "KERMIT.EXE").read_bytes() == (ROOT / "build/kermit/KERMIT.EXE").read_bytes()
        quit_vc(session)
        assert_home_empty(home)


@pytest.mark.parametrize("document", ["README.md", "data/KERMIT.TXT", "infra/bbs/README.md"])
def test_kermit_linux_dial_command_is_documented(document):
    assert LINUX_DIAL_COMMAND.decode("ascii") in (ROOT / document).read_text()


def wait_kermit(session):
    def returned_to_vc():
        tail = session.log.read_text().rpartition("translation KERMIT.EXE")[2]
        return panels(session.text()) and "terminate psp" in tail

    try:
        until(session, lambda: PROMPT.search(session.text()) or session.poll() is not None
              or returned_to_vc(), timeout=20)
    except AssertionError as error:
        raise AssertionError(f"{error}\n--- Kermit log ---\n{session.log.read_text()}") from error
    assert session.poll() is None, session.log.read_text()
    assert PROMPT.search(session.text()), session.text()
    assert "translation KERMIT.EXE" in session.log.read_text()


def quit_kermit(session):
    session.send(b"EXIT", "enter")
    until(session, lambda: panels(session.text()) or session.poll() is not None, timeout=15)
    assert session.poll() is None, session.log.read_text()
    until(session, lambda: session.text() == session.memory_text())


def bbs_workflow(monkeypatch, mode, *, quick=False, directory="work"):
    bbs = FakeBBS(mode)
    try:
        with running_kermit_vc(monkeypatch, bbs=bbs, quick=quick,
                               files={"PANEL.TXT": b"still here\r\n"}) as (session, work, home, config):
            assert_home_empty(home)
            assert (config / "BBS.TAK").read_bytes() == (ROOT / "data/BBS.TAK").read_bytes()
            assert (config / "KERMIT.EXE").read_bytes() == (ROOT / "build/kermit/KERMIT.EXE").read_bytes()
            current = work
            if directory == "home":
                current = home
                session.send(b"cd", "enter")
                until(session, lambda: "H:\\>" in session.text())
            elif directory == "nested":
                current = work / "nested"
                current.mkdir()
                session.send(b"cd nested", "enter")
                until(session, lambda: "\\nested>" in session.text().lower())
            else:
                assert directory == "work"
            for name in ("KERMIT.EXE", "BBS.TAK"):
                assert not (current / name).exists(), "DOS PATH must find the installed files"
            frames = RenderedFrames(session, ["ENiGMA", "BBS version", "TEST BBS READY"])
            try:
                session.send(LINUX_DIAL_COMMAND, "enter")
                until(session, lambda: not frames.pending or bbs.error is not None
                      or session.poll() is not None, timeout=25)
                assert bbs.error is None, repr(bbs.error)
                assert session.poll() is None, session.log.read_text()
                assert not frames.pending, f"Kermit never rendered {frames.pending}\n{session.text()}\n{session.log.read_text()}"
                assert "translation KERMIT.EXE" in session.log.read_text()
                session.send(b"HELLO42", "enter")
                until(session, lambda: b"HELLO42" in bbs.lines or bbs.error is not None)
                assert bbs.error is None, repr(bbs.error)
                session.wait_for("BBS ECHO: HELLO42")

                # These are the Hayes protocol's real guard intervals, not
                # timers guessing when a dialog or translated program is ready.
                until(session, lambda: time.monotonic() - bbs.last_outbound >= 1.1, timeout=5)
                session.send(b"+++")
                session.wait_for("OK", timeout=5)
                session.send(b"ATH", "enter")
                session.wait_for("NO CARRIER", timeout=5)
                assert b"+++" not in bbs.received and b"ATH" not in bbs.received, (
                    "guarded escape/hangup commands leaked into BBS data")
                session.send(b"\x1d", b"C")
                wait_kermit(session)
                quit_kermit(session)
                if directory == "work":
                    assert "PANEL" in session.text(), "VC's original panel was not redrawn"
                assert_home_empty(home)
                quit_vc(session)
                assert_home_empty(home)
            except AssertionError as error:
                raise AssertionError(
                    f"{error}\nrendered markers: {sorted(frames.seen)}\n"
                    f"BBS user bytes: {bytes(bbs.received)!r}\n"
                    f"--- Kermit log ---\n{session.log.read_text()}") from error
            finally:
                frames.close()
    finally:
        # Also close resources when VC could not start, before its context
        # manager took ownership (for example, a missing requested binary).
        if not bbs.stop.is_set():
            bbs.close()


def test_kermit_dials_bbs_tcp(monkeypatch):
    """Required real TCP gate; socket permission failures are not skipped."""
    bbs_workflow(monkeypatch, "tcp")


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("directory", ["work", "nested", "home"])
def test_kermit_dials_bbs_pipe_supplemental(monkeypatch, quick, directory):
    """Same translated pty workflow, explicitly not a TCP-pass substitute."""
    bbs_workflow(monkeypatch, "pipe", quick=quick, directory=directory)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_kermit_offline_prompt_and_exit(monkeypatch, quick):
    with running_kermit_vc(monkeypatch, quick=quick, files={"PANEL.TXT": b"still here\r\n"}) as (
            session, _, home, config):
        assert (config / "KERMIT.EXE").read_bytes() == (ROOT / "build/kermit/KERMIT.EXE").read_bytes()
        assert (config / "KERMIT.TXT").read_bytes() == (ROOT / "data/KERMIT.TXT").read_bytes()
        assert_home_empty(home)
        for _ in range(2):
            session.send(b"kermit", "enter")
            wait_kermit(session)
            assert "3.15" in session.text(), session.text()
            quit_kermit(session)
            assert "PANEL" in session.text()


def test_kermit_migrates_previous_association_and_preserves_config_guides(monkeypatch):
    script = b"; My saved BBS script\r\necho PRESERVE THIS\r\n"
    guide = b"My own Kermit notes\r\n"
    with running_kermit_vc(monkeypatch,
                          config_files={"VC.EXT": b"bas: gwbasic !.!\r\n",
                                        "BBS.TAK": script, "KERMIT.TXT": guide}) as (_, _, home, config):
        assert (config / "VC.EXT").read_bytes() == (ROOT / "data/VC.EXT").read_bytes()
        assert b"tak: kermit stay, take !.!\r\n" in (config / "VC.EXT").read_bytes()
        assert (config / "BBS.TAK").read_bytes() == script
        assert (config / "KERMIT.TXT").read_bytes() == guide
        assert_home_empty(home)


@pytest.mark.parametrize("customized", [False, True], ids=["defaults", "customized"])
def test_kermit_does_not_migrate_home_files(monkeypatch, customized):
    previous = {name: (ROOT / "data" / name).read_bytes() for name in ("BBS.TAK", "KERMIT.TXT")}
    # The script is unchanged, but the guide must be the actual previous
    # version (with its DOS CRLF bytes), not this build's updated text.
    previous["KERMIT.TXT"] = (ROOT / "tests/fixtures/kermit-home-guide-brief26.txt").read_text(
        encoding="ascii").replace("\n", "\r\n").encode("ascii")
    if customized:
        previous = {name: b"My own file\r\n" + data for name, data in previous.items()}
    previous["notes/personal.txt"] = b"unrelated user data\n"
    with running_kermit_vc(monkeypatch, home_files=previous,
                           home_links={"LINK.TAK": "BBS.TAK", "DANGLING.TXT": "missing"}) as (
            session, _, home, config):
        # The fixture checks every entry's inode, timestamps, mode and bytes
        # against its pre-start snapshot, including symlinks and unrelated data.
        for name in ("BBS.TAK", "KERMIT.TXT"):
            assert (home / name).read_bytes() == previous[name]
            assert (config / name).read_bytes() == (ROOT / "data" / name).read_bytes()
        quit_vc(session)


def test_kermit_unchanged_script_not_rewritten_on_restart(monkeypatch):
    with running_kermit_vc(monkeypatch) as (_, work, home, config):
        expected = {}
        for name in ("BBS.TAK", "KERMIT.TXT"):
            path = config / name
            path.chmod(0o640)
            os.utime(path, ns=(1_000_000_000, 1_000_000_000))
            info = path.stat()
            expected[name] = (info.st_ino, info.st_mtime_ns, info.st_mode, path.read_bytes())
        # Same home/install, with distinct diagnostics to avoid two processes
        # truncating the first session's screen/log files.
        diagnostics = work.parent / "second"
        diagnostics.mkdir()
        second = VcSession(work, home, extra_env={
            "XDG_CONFIG_HOME": str(config.parent),
            "XDG_CACHE_HOME": str(work.parent / "cache"),
            "VC_LOG": str(diagnostics / "vc.log"),
            "VC_SCREEN_DUMP": str(diagnostics / "screen.txt"),
            "VC_FRAME_DUMP": str(diagnostics / "frame.pgm"),
        })
        try:
            second.wait_for("10Quit", timeout=20)
            for name in expected:
                path = config / name
                info = path.stat()
                assert (info.st_ino, info.st_mtime_ns, info.st_mode, path.read_bytes()) == expected[name]
            assert_home_empty(home)
        finally:
            second.close()


def test_kermit_renamed_executable_matches_bytes(monkeypatch):
    with running_kermit_vc(monkeypatch) as (session, work, _, config):
        shutil.copyfile(config / "KERMIT.EXE", work / "TTY.EXE")
        (config / "KERMIT.EXE").unlink()
        session.send(b"tty", "enter")
        wait_kermit(session)
        quit_kermit(session)


def test_kermit_modified_executable_is_refused(monkeypatch):
    with running_kermit_vc(monkeypatch) as (session, work, _, config):
        image = bytearray((config / "KERMIT.EXE").read_bytes())
        image[-1] ^= 1
        bad = work / "KERMIT.EXE"
        bad.write_bytes(image)
        session.send(b"kermit", "enter")
        until(session, lambda: "DOS command error 11" in session.log.read_text()
              or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        assert "unsupported executable" in session.log.read_text().lower()
        assert "translation KERMIT.EXE" not in session.log.read_text()
        until(session, lambda: panels(session.text()))
        bad.unlink()
        session.send(b"kermit", "enter")
        wait_kermit(session)
        quit_kermit(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("missing", [False, True], ids=["installed", "missing"])
@pytest.mark.parametrize("name", ["safe;touch PWNED;.tak", "safe,run touch PWNED,.tak",
                                 "safezz},run touch PWNED,{.tak"],
                         ids=["shell-semicolons", "kermit-commas", "brace-breakout"])
def test_kermit_take_filename_never_reaches_shell(monkeypatch, quick, missing, name):
    script = b"echo SAFE FILENAME\r\nexit\r\n"
    with running_kermit_vc(monkeypatch, quick=quick, files={name: script}) as (session, work, home, config):
        if missing:
            (config / "KERMIT.EXE").unlink()
        select(session, name)
        session.send("enter")
        if missing:
            until(session, lambda: "DOS command error 2" in session.log.read_text())
            until(session, lambda: panels(session.text()))
            assert "translation KERMIT.EXE" not in session.log.read_text()
        else:
            until(session, lambda: "translation KERMIT.EXE" in session.log.read_text()
                  or session.poll() is not None)
            assert session.poll() is None, session.log.read_text()
            until(session, lambda: PROMPT.search(session.text()) or panels(session.text())
                  or session.poll() is not None, timeout=20)
            if not panels(session.text()):
                wait_kermit(session)
                quit_kermit(session)
        assert session.poll() is None
        assert not (work / "PWNED").exists()
        assert not (home / "PWNED").exists()
        assert not (config / "PWNED").exists()
        assert (work / name).read_bytes() == script


@pytest.mark.parametrize("directory,name", [("work", "{BAD}.TAK"), ("work$", "SAFE.TAK")],
                         ids=["unsafe-short-filename", "unsafe-short-directory"])
def test_kermit_take_rejects_unquotable_canonical_paths(monkeypatch, directory, name):
    script = b"echo UNSAFE SCRIPT RAN\r\nexit\r\n"
    with running_kermit_vc(monkeypatch, work_name=directory, files={name: script}) as (
            session, work, _, _):
        select(session, name)
        session.send("enter")
        until(session, lambda: "DOS command error" in session.log.read_text()
              or session.poll() is not None)
        assert session.poll() is None, session.log.read_text()
        until(session, lambda: panels(session.text()))
        assert "translation KERMIT.EXE" not in session.log.read_text()
        assert (work / name).read_bytes() == script


def test_fake_bbs_discards_fragmented_telnet_and_terminal_reports():
    decoder = ClientBytes()
    data = (b"\xff\xfd\x01\xff\xfb\x18\xff\xfa\x18\x00ANSI\xff\xf0"
            b"\x1b[?62;1;2;6;7;8;9c\x1b[24;80RHELLO42\r")
    assert b"".join(decoder.feed(bytes([byte])) for byte in data) == b"HELLO42\r"
