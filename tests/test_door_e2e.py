"""Native BBS doors in real PTYs, with no host log/screen-dump shortcuts.

Each brief-28 gate is separately selectable for deliberate-defect checks.
The PTY has its final size before exec, exactly as ENiGMA/node-pty provides it.
Screen assertions observe only ANSI output; filesystem assertions inspect the
private session directory and the test's deliberately out-of-scope sentinels.
"""
from contextlib import contextmanager
from dataclasses import dataclass
import errno
import fcntl
import os
from pathlib import Path
import pty
import re
import select
import signal
import stat
import struct
import sys
import tempfile
import termios
import time

import pyte
import pytest


ROOT = Path(__file__).resolve().parents[1]
VC = Path(os.environ.get("VC_TEST_BINARY", str(ROOT / "build/vc")))
sys.path.insert(0, str(ROOT / "tools"))
from web_demo import demo_files  # noqa: E402

CAPACITY = 8 * 1024 * 1024
KEYS = {
    "enter": b"\r", "esc": b"\x1b", "tab": b"\t",
    "up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
    "home": b"\x1b[H", "end": b"\x1b[F", "pgup": b"\x1b[5~", "pgdn": b"\x1b[6~",
    "f1": b"\x1bOP", "f2": b"\x1bOQ", "f3": b"\x1bOR", "f4": b"\x1bOS",
    "f5": b"\x1b[15~", "f6": b"\x1b[17~", "f7": b"\x1b[18~", "f8": b"\x1b[19~",
    "f9": b"\x1b[20~", "f10": b"\x1b[21~",
}


def panels(text):
    lines = text.splitlines()
    return len(lines) == 25 and text.startswith("╔") and "10Quit" in lines[24]


@dataclass
class DoorPaths:
    base: Path
    sessions: Path
    home: Path
    work: Path


@pytest.fixture
def paths():
    # Keep all native/DOS path arguments short, independently of pytest's root.
    with tempfile.TemporaryDirectory(prefix="vc-door-", dir="/tmp") as directory:
        base = Path(directory)
        result = DoorPaths(base, base / "sessions", base / "home", base / "work")
        for path in (result.sessions, result.home, result.work):
            path.mkdir(mode=0o700)
        (result.home / "HOST.TXT").write_text("PRIVATE HOME MUST STAY OUTSIDE THE DOOR\n")
        (result.work / "HOST.TXT").write_text("PRIVATE CWD MUST STAY OUTSIDE THE DOOR\n")
        yield result


class TerminalSession:
    """Small, isolated ANSI-only driver; no fixed sleeps between input keys."""

    # Functional PTY tests explicitly permit only the Landlock fallback when
    # running on restricted developer kernels. Kernel enforcement itself is
    # tested separately without this flag in test_door_confinement.py.
    def __init__(self, paths, *, args=("--door", "--door-allow-unconfined"), extra_env=None,
                 rows=25, cols=80, cwd=None):
        assert VC.is_file(), f"{VC} is missing; run make before the door gates"
        self.paths = paths
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.raw = bytearray()
        self.pending_display = set()
        self.seen_display = set()
        self.exit_status = None
        env = {
            "HOME": str(paths.home), "XDG_CONFIG_HOME": str(paths.home / ".config"),
            "XDG_CACHE_HOME": str(paths.home / ".cache"),
            "VC_LOG": str(paths.home / "forbidden.log"),
            "VC_SCREEN_DUMP": str(paths.home / "forbidden-screen.txt"),
            "VC_FRAME_DUMP": str(paths.home / "forbidden-frame.pgm"),
            "VC_DOOR_ROOT": str(paths.sessions), "VC_DOOR_NODE": "23",
            "TERM": "vt100", "LANG": "C.UTF-8", "PATH": "/usr/bin:/bin",
        }
        for name, value in (extra_env or {}).items():
            if value is None:
                env.pop(name, None)
            else:
                env[name] = value
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.pid = os.fork()
        if self.pid == 0:
            try:
                os.close(master)
                os.setsid()
                fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
                for fd in (0, 1, 2):
                    os.dup2(slave, fd)
                if slave > 2:
                    os.close(slave)
                os.chdir(cwd or paths.work)
                os.execve(str(VC), [str(VC), *args], env)
            except BaseException as error:
                os.write(2, f"door test exec failed: {error}\n".encode())
                os._exit(127)
        os.close(slave)
        self.fd = master

    def text(self):
        return "\n".join(self.screen.display)

    def pump(self, timeout=0.03):
        if self.fd is None:
            select.select([], [], [], timeout)
            return
        # Bound a pump even if a guest continuously writes to its terminal.
        deadline = time.monotonic() + max(timeout, 0.03)
        while True:
            ready, _, _ = select.select([self.fd], [], [], timeout)
            if not ready:
                return
            try:
                data = os.read(self.fd, 65536)
            except OSError as error:
                if error.errno == errno.EIO:
                    return
                raise
            if not data:
                return
            self.raw.extend(data)
            if self.pending_display:
                # A DOS error can be painted and covered by panels in the
                # same read. Observe the actual rendered text as it appears;
                # removing ANSI escapes would lose elided unchanged spaces.
                for byte in data:
                    self.stream.feed(bytes([byte]))
                    candidates = [text for text in self.pending_display if ord(text[-1]) == byte]
                    for text in candidates:
                        if text in self.text():
                            self.seen_display.add(text)
                            self.pending_display.remove(text)
            else:
                self.stream.feed(data)
            if time.monotonic() >= deadline:
                return
            timeout = 0

    def poll(self):
        if self.exit_status is None:
            child, status = os.waitpid(self.pid, os.WNOHANG)
            if child:
                self.exit_status = os.waitstatus_to_exitcode(status)
        return self.exit_status

    def until(self, check, *, timeout=10, description="screen condition", drain=True):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if drain:
                self.pump()
            else:
                select.select([], [], [], 0.02)
            if check():
                return
        raise AssertionError(f"{description} not met after {timeout}s; exit={self.poll()}\n"
                             f"{self.text()}\nraw tail: {bytes(self.raw[-1600:])!r}")

    def wait_for(self, needle, timeout=10):
        self.until(lambda: needle in self.text(), timeout=timeout, description=repr(needle))

    def wait_displayed(self, needle, timeout=10):
        self.seen_display.discard(needle)
        self.pending_display.add(needle)
        self.until(lambda: needle in self.seen_display, timeout=timeout,
                   description=f"rendered terminal text containing {needle!r}")

    def send(self, *keys):
        data = b"".join(KEYS.get(key, key.encode()) if isinstance(key, str) else key
                        for key in keys)
        assert self.fd is not None, "cannot type after carrier loss"
        while data:
            data = data[os.write(self.fd, data):]

    def type_line(self, value):
        """Honor original DOS editors' small typeahead buffers by waiting on echo."""
        text = value.decode("ascii") if isinstance(value, bytes) else value
        self.until(lambda: not self.screen.cursor.hidden
                   and self.screen.cursor.x < self.screen.columns,
                   description="visible DOS input cursor")
        row, column = self.screen.cursor.y, self.screen.cursor.x
        assert column + len(text) < self.screen.columns, "test command must fit one visible line"
        for start in range(0, len(text), 8):
            prefix = text[:start + 8]
            self.send(text[start:start + 8])
            self.until(lambda: self.screen.display[row][column:column + len(prefix)] == prefix,
                       description=f"DOS echoes {prefix!r}")
        self.send("enter")

    def ready(self):
        self.until(lambda: panels(self.text()) and not self.screen.cursor.hidden,
                   timeout=20, description="VC panels")
        assert self.poll() is None, self.text()

    def drive(self):
        entries = list(self.paths.sessions.iterdir())
        assert len(entries) == 1 and entries[0].is_dir(), entries
        return entries[0]

    def wait_clean(self, timeout=8, *, drain=True):
        self.until(lambda: not list(self.paths.sessions.iterdir()), timeout=timeout,
                   description="session directory deletion", drain=drain)

    def wait_exit(self, timeout=10):
        self.until(lambda: self.poll() is not None, timeout=timeout, description="process exit")
        # The cleanup watchdog can append its timeout line after worker exit.
        self.pump()
        return self.exit_status

    def quit_vc(self):
        self.send("f10")
        self.wait_for("Do you want to quit the Volkov Commander?")
        self.send("enter")
        assert self.wait_exit() == 0, self.text()

    def hangup(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def close(self):
        if self.poll() is None:
            try:
                os.kill(self.pid, signal.SIGHUP)
            except ProcessLookupError:
                pass
            end = time.monotonic() + 3
            while self.poll() is None and time.monotonic() < end:
                self.pump()
            if self.poll() is None:
                os.kill(self.pid, signal.SIGKILL)
                _, status = os.waitpid(self.pid, 0)
                self.exit_status = os.waitstatus_to_exitcode(status)
        # Give an orphaned cleanup watcher time to finish before fixture removal.
        end = time.monotonic() + 3
        while list(self.paths.sessions.iterdir()) and time.monotonic() < end:
            self.pump()
        self.hangup()


@contextmanager
def running(paths, **kwargs):
    session = TerminalSession(paths, **kwargs)
    try:
        yield session
    finally:
        session.close()


def start_basic(session):
    session.send(b"gwbasic", "enter")
    session.until(lambda: "GW-BASIC" in session.text()
                  and re.search(r"(?m)^Ok\s*$", session.text())
                  and not session.screen.cursor.hidden
                  and not session.screen.display[session.screen.cursor.y].strip(),
                  description="translated GW-BASIC prompt")


def select_file(session, name):
    session.send("home")
    session.pump()
    seen = set()
    for _ in range(100):
        row = session.text().splitlines()[21]
        if name[-12:].upper() in row.upper():
            return
        assert row not in seen, f"cannot select {name!r}\n{session.text()}"
        seen.add(row)
        session.send("down")
        session.until(lambda: session.text().splitlines()[21] != row,
                      description="classic Down moves the file cursor")
    raise AssertionError(f"{name!r} not found\n{session.text()}")


PAGE = max(4096, os.sysconf("SC_PAGESIZE"))


def charged_bytes(directory):
    """H:'s quota charge: each file's size rounded up to whole tmpfs pages."""
    return sum(-(-path.stat().st_size // PAGE) * PAGE
               for path in directory.rglob("*") if path.is_file())


def test_door_timeout_cleans_a_full_undrained_terminal(paths):
    """A departing caller need not drain the tty for the watchdog to delete H:."""
    with running(paths, args=("--door", "--door-allow-unconfined", "--door-minutes", "0.04")) as session:
        session.ready()
        session.drive()
        # Fill the actual slave side instead of assuming a printing guest
        # produced enough bytes. A separately opened nonblocking description
        # leaves the worker/watchdog's blocking stdout flags unchanged.
        writer = os.open(f"/proc/{session.pid}/fd/1", os.O_WRONLY | os.O_NONBLOCK)
        try:
            last_progress = time.monotonic()
            deadline = last_progress + 2
            sent = 0
            while time.monotonic() - last_progress < 0.1:
                assert time.monotonic() < deadline and sent < 1024 * 1024, \
                    "test tty did not develop backpressure"
                try:
                    sent += os.write(writer, b"x" * 4096)
                    last_progress = time.monotonic()
                except BlockingIOError:
                    select.select([], [], [], 0.005)
            assert sent >= 4096, "the test must establish real terminal backpressure"
        finally:
            os.close(writer)
        # Deliberately never read another byte while awaiting cleanup.
        session.wait_clean(timeout=10, drain=False)
        assert session.wait_exit() != 0


@pytest.mark.parametrize("command,marker", [
    (b"/bin/sh -c 'echo escaped > SH-RAN.TXT'", "SH-RAN.TXT"),
    (b"ls > LS-RAN.TXT", "LS-RAN.TXT"),
], ids=["bin-sh", "ls"])
def test_door_refuses_host_commands(paths, command, marker):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        session.send(command, "enter")
        session.wait_displayed("Bad command or file name")
        assert not (drive / marker).exists(), "the forbidden command reached a host shell"
        assert not (paths.work / marker).exists()
        assert not (paths.home / marker).exists()


def test_door_has_no_c_drive_or_host_diagnostic_files(paths):
    with running(paths) as session:
        session.ready()
        assert "H:\\>" in session.text(), session.text()
        assert session.text().splitlines()[0].count("H:\\") == 2, session.text()
        assert "C:\\" not in session.text().splitlines()[0]
        assert ".VC" not in session.text(), "private VC settings should start hidden"
        # Query VC's actual DOS drive enumeration. An accidental C: alias of
        # the same host directory can normalize a prompt back to H:, so the
        # failed cd alone would not prove that C: is unavailable.
        session.send(b"\x1b[11;3~")  # Classic Alt-F1: left drive picker.
        session.wait_for("Choose left drive:")
        choices = session.text().splitlines()[8].split("║")[2].split()
        assert choices == ["H"], f"door advertised DOS drives {choices!r}"
        session.send("esc")
        session.ready()
        session.send(b"cd C:\\")
        session.until(lambda: session.text().splitlines()[23].rstrip().endswith("cd C:\\"),
                      description="VC accepts the attempted C: path")
        session.send("enter")
        session.until(lambda: panels(session.text())
                      and session.text().splitlines()[23].strip() == "H:\\>",
                      description="unavailable C: leaves the caller on H:")
        assert "H:\\>" in session.text() and "C:\\>" not in session.text()
        assert "PRIVATE" not in session.text()
        assert sorted(path.name for path in paths.home.iterdir()) == ["HOST.TXT"]
        assert sorted(path.name for path in paths.work.iterdir()) == ["HOST.TXT"]


def test_door_holds_the_exact_browser_demo_and_displays_readme(paths):
    expected = demo_files(ROOT / "build/gwbasic/GWBASIC.EXE", ROOT / "build/games",
                          ROOT / "build/bootlogo/LOGO.COM", ROOT / "build/rogue/ROGUE.EXE",
                          ROOT / "build/vz/VZ.COM", ROOT / "build/kermit/KERMIT.EXE")
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        assert re.fullmatch(r"node-23-[A-Za-z0-9]{6}", drive.name), drive.name
        assert stat.S_IMODE(drive.stat().st_mode) == 0o700
        actual = {path.relative_to(drive).as_posix(): path.read_bytes()
                  for path in drive.rglob("*")
                  if path.is_file() and path.relative_to(drive).parts[0] != ".VC"}
        assert actual == expected
        select_file(session, "README.TXT")
        session.send("f3")
        session.wait_for("This is the real Volkov Commander")
        session.send("esc")
        session.ready()


def test_door_f4_uses_translated_vz_and_ignores_editor(paths):
    editor = paths.base / "host-editor"
    editor.write_text('#!/bin/sh\nprintf executed > "$0.ran"\n')
    editor.chmod(0o700)
    with running(paths, extra_env={"EDITOR": str(editor)}) as session:
        session.ready()
        select_file(session, "README.TXT")
        session.send("f4")
        session.until(lambda: "Volkov Commander in your browser." in session.text()
                      and not panels(session.text()), description="VZ shows README")
        session.send("f1")
        session.wait_for("Open Files")
        session.send("esc")
        session.until(lambda: "Open Files" not in session.text(), description="VZ menu closes")
        session.send(b"\x1bQ")
        session.wait_for("Quit from editor? (Y/N)")
        session.send(b"Y")
        session.ready()
        assert not Path(str(editor) + ".ran").exists()


def test_door_writes_past_eight_mib_report_disk_full(paths):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        start_basic(session)
        seed = charged_bytes(drive)
        record = (CAPACITY - seed) // PAGE * (PAGE // 256)  # whole pages of records
        assert 0 < record < 32767
        session.type_line(f'OPEN "R",#1,"FILL.BIN",256:PUT #1,{record}')
        output = drive / "FILL.BIN"
        session.until(lambda: output.exists() and output.stat().st_size == record * 256,
                      description="BASIC sparse random-record write fills H: below its cap")
        assert charged_bytes(drive) <= CAPACITY
        session.type_line(f"PUT #1,{record + 1}")
        session.wait_for("Disk full")
        # The file ends on a page boundary, so no part of the next record
        # fits; no accepted byte may cross the cap.
        assert record * 256 <= output.stat().st_size <= CAPACITY - seed
        assert charged_bytes(drive) <= CAPACITY


def test_door_modem_answers_no_answer_even_with_an_inherited_endpoint(paths):
    # No socket listener, external service, or network request is part of this
    # test. The configured phone number must stop in the empty door phonebook.
    with running(paths, extra_env={"VC_MODEM_555_1992": "127.0.0.1:1"}) as session:
        session.ready()
        session.send(b"kermit", "enter")

        def prompt():
            return not session.screen.cursor.hidden and \
                session.screen.display[session.screen.cursor.y].strip() == "MS-Kermit>"

        session.until(prompt, timeout=20, description="translated MS-Kermit prompt")

        def command(value, *, connecting=False):
            session.type_line(value)
            if not connecting:
                session.until(prompt, description="Kermit command completes")

        command("set port com1")
        command("set speed 57600")
        command("set carrier off")
        command("set terminal type ansi-bbs")
        command("connect", connecting=True)
        # ANSI-BBS uses all 25 rows and hides Kermit's usual status line.
        session.until(lambda: not session.text().strip() and not session.screen.cursor.hidden,
                      description="Kermit's blank ANSI-BBS terminal is ready")
        session.send(b"AT", "enter")
        session.wait_for("OK")
        session.send(b"ATDT555-1992", "enter")
        session.wait_for("NO ANSWER")
        assert b"CONNECT 14400" not in session.raw
        descriptors = Path(f"/proc/{session.pid}/fd")
        assert all(not os.readlink(fd).startswith("socket:") for fd in descriptors.iterdir())
        session.send(b"\x1d")
        session.wait_for("Command>")
        session.send(b"C")
        session.until(prompt, description="Kermit returns from its offline terminal")
        command("exit", connecting=True)
        session.ready()


def test_door_normal_exit_removes_session(paths):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        session.quit_vc()
        session.wait_clean()
        assert not drive.exists()


def test_door_each_session_starts_with_a_fresh_private_drive(paths):
    with running(paths) as session:
        session.ready()
        first = session.drive()
        session.send("f7")
        session.wait_for("Make directory")
        session.send(b"ONLYHERE", "enter")
        session.until(lambda: (first / "ONLYHERE").is_dir(), description="first session change")
        session.ready()
        session.quit_vc()
        session.wait_clean()
    with running(paths) as session:
        session.ready()
        second = session.drive()
        assert second != first
        assert not (second / "ONLYHERE").exists()
        assert stat.S_IMODE(second.stat().st_mode) == 0o700


@pytest.mark.parametrize("number", [signal.SIGHUP, signal.SIGKILL], ids=["sighup", "sigkill"])
def test_door_fatal_exit_removes_session(paths, number):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        before = len(session.raw)
        os.kill(session.pid, number)
        assert session.wait_exit() == -number
        session.wait_clean()
        assert not drive.exists()
        if number == signal.SIGKILL:
            # SIGKILL skips term_shutdown: the detached janitor is the only
            # process left to restore classic VT autowrap for the BBS.
            session.until(lambda: b"\x1b[?1049l" in session.raw[before:],
                          description="janitor terminal restoration after SIGKILL")
            assert b"\x1b[?7h" in session.raw[before:], \
                "the janitor must restore autowrap before returning to the BBS"


def test_door_carrier_loss_removes_session(paths):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        session.hangup()
        assert session.wait_exit() != 0
        session.wait_clean()
        assert not drive.exists()


def test_door_time_limit_prints_one_line_and_removes_session(paths):
    with running(paths, args=("--door", "--door-allow-unconfined", "--door-minutes", "0.02")) as session:
        session.ready()
        drive = session.drive()
        before = len(session.raw)
        assert session.wait_exit() != 0
        session.wait_clean()
        session.until(lambda: b"time limit" in session.raw[before:].lower(),
                      description="one-line time-limit notice")
        notices = [line for line in bytes(session.raw[before:]).decode("utf-8").splitlines()
                   if "time limit" in line.lower()]
        assert len(notices) == 1
        assert not drive.exists()


def test_door_run_rogue_starts_directly_and_exits_with_game(paths):
    with running(paths, args=("--door", "--door-allow-unconfined", "--door-run", "ROGUE.EXE")) as session:
        session.until(lambda: re.search(r"Level:\s*1\s+Gold:", session.text())
                      and "@" in session.text(), timeout=20, description="Rogue dungeon")
        drive = session.drive()
        assert b"10Quit" not in session.raw, "--door-run must never launch VC panels"
        session.send(b"Q")
        session.until(lambda: "really quit?" in session.text().lower(),
                      description="Rogue quit confirmation")
        session.send(b"y")
        assert session.wait_exit(timeout=20) == 0
        session.wait_clean()
        assert not drive.exists()
        assert b"10Quit" not in session.raw


def hack_level(text):
    """Require the game's status row and a player on the map, not its help."""
    return (re.search(r"Level\s+1\s+Gold\s+\d+\s+Hp\s+\d+", text) is not None
            and any("@" in line for line in text.splitlines()[1:23]))


def hack_more_or(session, complete):
    # Original Hack can pause on --More-- while saving/quitting. Acknowledge
    # only a visible prompt, never send unsolicited keys on a timer.
    for _ in range(10):
        session.until(lambda: complete() or "--More--" in session.text(), timeout=20,
                      description="Hack completion or message acknowledgement")
        if complete():
            return
        before = session.text()
        session.send(b" ")
        session.until(lambda: complete() or session.text() != before,
                      description="Hack consumes visible --More--")
    raise AssertionError(f"too many Hack messages\n{session.text()}")


def test_door_run_hack_starts_directly_and_exits_with_game(paths):
    with running(paths, args=("--door", "--door-allow-unconfined", "--door-run", "HACK.EXE")) as session:
        session.wait_for("experienced player", timeout=20)
        session.send(b"y")
        session.wait_for("what kind of character")
        session.send(b"C")
        hack_more_or(session, lambda: hack_level(session.text()) and "--More--" not in session.text())
        drive = session.drive()
        assert (drive / "GAMES/HACK/HACK.EXE").is_file()
        assert b"10Quit" not in session.raw, "--door-run must not launch VC panels"
        session.send(b"Q")
        hack_more_or(session, lambda: "Really quit?" in session.text())
        session.send(b"y")
        hack_more_or(session, lambda: session.poll() is not None)
        assert session.wait_exit() == 0
        session.wait_clean()
        assert not drive.exists()
        assert b"10Quit" not in session.raw
        assert sorted(path.name for path in paths.home.iterdir()) == ["HOST.TXT"]
        assert sorted(path.name for path in paths.work.iterdir()) == ["HOST.TXT"]


def test_door_vc_hack_saves_and_restores_inside_its_session(paths):
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        session.send(b"hack -C", "enter")
        hack_more_or(session, lambda: hack_level(session.text()) and "--More--" not in session.text())
        session.send(b"S")
        hack_more_or(session, lambda: panels(session.text()))
        save = drive / "GAMES/HACK/HACK.SAV"
        assert save.is_file() and save.stat().st_size > 0
        assert not (drive / "HACK.SAV").exists(), "save belongs beside HACK.EXE"
        session.send(b"hack", "enter")
        hack_more_or(session, lambda: hack_level(session.text()) and not save.exists()
                     and "--More--" not in session.text())
        session.send(b"Q")
        hack_more_or(session, lambda: "Really quit?" in session.text())
        session.send(b"y")
        hack_more_or(session, lambda: panels(session.text()))
        session.quit_vc()
        session.wait_clean()
        assert not drive.exists()
        assert sorted(path.name for path in paths.home.iterdir()) == ["HOST.TXT"]
        assert sorted(path.name for path in paths.work.iterdir()) == ["HOST.TXT"]


def test_door_classic_keys_drive_panels_and_no_protocol_is_requested(paths):
    with running(paths) as session:
        session.ready()
        initial = session.text().splitlines()[21]
        session.send(b"\x1b[B")
        session.until(lambda: session.text().splitlines()[21] != initial,
                      description="VT Down changes selected file")
        session.send(b"\x1bOA")
        session.until(lambda: session.text().splitlines()[21] == initial,
                      description="SS3 Up restores selected file")
        session.send(b"\x1b[18~")  # F7, not kitty: original VC directory dialog.
        session.wait_for("Make directory")
        session.send(b"KEYSOK", "enter")
        session.until(lambda: (session.drive() / "KEYSOK").is_dir(),
                      description="classic F7 creates a DOS directory")
        session.ready()
        session.send(b"\x1b[?1u\x1b[?0u")  # A malicious unsolicited kitty reply.
        session.quit_vc()
        for sequence in (b"\x1b[?u", b"\x1b[>11u", b"\x1b[>4", b"\x1b[?100"):
            assert sequence not in session.raw, f"door emitted forbidden mode {sequence!r}"


@pytest.mark.parametrize("rows,cols", [(24, 80), (25, 79)], ids=["80x24", "79x25"])
def test_door_rejects_small_terminal_with_one_clear_line(paths, rows, cols):
    with running(paths, rows=rows, cols=cols) as session:
        assert session.wait_exit() != 0
        text = bytes(session.raw).decode("utf-8")
        assert "\x1b" not in text, "size rejection must precede terminal mode changes"
        assert len(text.splitlines()) == 1, repr(text)
        assert "80x25" in text and "terminal" in text.lower(), repr(text)
        assert not list(paths.sessions.iterdir())


def test_normal_run_ignores_door_environment_and_keeps_host_features(paths):
    work = paths.home / "work"
    work.mkdir()
    (work / "NORMAL.TXT").write_text("ordinary native file\n")
    with running(paths, args=(), cwd=work) as session:
        session.ready()
        assert "H:\\work>" in session.text()
        for sequence in (b"\x1b[?u", b"\x1b[>4;2m", b"\x1b[?1003h"):
            assert sequence in session.raw, f"normal terminal lost {sequence!r}"
        session.send(b"echo native-shell-ok > SHELL.TXT", "enter")
        session.until(lambda: (work / "SHELL.TXT").exists(), description="normal native shell runs")
        assert (work / "SHELL.TXT").read_text() == "native-shell-ok\n"
        session.ready()
        session.send(b"cd /", "enter")
        session.wait_for("C:\\>")
        session.send(b"cd ~", "enter")
        session.wait_for("H:\\>")
        session.quit_vc()
        assert not list(paths.sessions.iterdir()), "environment alone must not enable a door"


@pytest.mark.parametrize("arguments", [
    ("--door", "--door-minutes", "0"), ("--door", "--door-minutes", "-1"),
    ("--door", "--door-minutes", "nan"), ("--door", "--door-minutes", "inf"),
    ("--door", "--door-minutes"), ("--door", "--door-run"),
    ("--door", "--door-run", "MISSING.EXE"), ("--door-minutes", "1"),
])
def test_door_invalid_arguments_do_not_leave_a_session(paths, arguments):
    with running(paths, args=arguments) as session:
        assert session.wait_exit() != 0
        session.wait_clean()


def test_door_requires_an_explicit_session_root(paths):
    with running(paths, extra_env={"VC_DOOR_ROOT": None}) as session:
        assert session.wait_exit() != 0
        assert b"VC_DOOR_ROOT" in session.raw
        assert not list(paths.sessions.iterdir())
