"""Drive build/vc in a pseudo-terminal and read its screen through pyte.

This checks the whole chain: translated code, DOS layer, BIOS layer and the
ANSI output a real terminal would get.
"""
from __future__ import annotations

import fcntl
import os
import pty
import select
import signal
import struct
import termios
import time
from pathlib import Path

import pyte

ROOT = Path(__file__).resolve().parents[2]
VC = ROOT / "build" / "vc"

KEYS = {
    "enter": b"\r", "esc": b"\x1b", "tab": b"\t", "bs": b"\x7f",
    "up": b"\x1b[A", "down": b"\x1b[B", "right": b"\x1b[C", "left": b"\x1b[D",
    "home": b"\x1b[H", "end": b"\x1b[F", "pgup": b"\x1b[5~", "pgdn": b"\x1b[6~",
    "ins": b"\x1b[2~", "del": b"\x1b[3~",
    "f1": b"\x1bOP", "f2": b"\x1bOQ", "f3": b"\x1bOR", "f4": b"\x1bOS",
    "f5": b"\x1b[15~", "f6": b"\x1b[17~", "f7": b"\x1b[18~", "f8": b"\x1b[19~",
    "f9": b"\x1b[20~", "f10": b"\x1b[21~",
}


class VcSession:
    def __init__(self, cwd: Path, home: Path, cols: int = 80, rows: int = 25):
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)
        self.log = home / "vc.log"
        env = {
            "HOME": str(home),
            "XDG_CONFIG_HOME": str(home / ".config"),
            "VC_LOG": str(self.log),
            "VC_SCREEN_DUMP": str(home / "screen.txt"),
            "TERM": "xterm-256color",
            "PATH": os.environ.get("PATH", "/usr/bin:/bin"),
            "LANG": "C.UTF-8",
        }
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(cwd)
            os.execve(str(VC), [str(VC)], env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
        self.exit_status: int | None = None
        self.dump = home / "screen.txt"

    def pump(self, timeout: float = 0.05) -> None:
        while True:
            r, _, _ = select.select([self.fd], [], [], timeout)
            if not r:
                return
            try:
                data = os.read(self.fd, 65536)
            except OSError:
                return
            if not data:
                return
            self.stream.feed(data)
            timeout = 0.02

    def text(self) -> str:
        return "\n".join(self.screen.display)

    def memory_text(self) -> str:
        """What VC wrote into video memory at the last render."""
        return self.dump.read_text().rstrip("\n") if self.dump.exists() else ""

    def wait_for(self, needle: str, timeout: float = 10.0) -> str:
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.1)
            if needle in self.text():
                return self.text()
            if self.poll() is not None:
                break
        raise AssertionError(f"{needle!r} not on screen after {timeout}s\n{self.text()}\n"
                             f"--- log ---\n{self.log.read_text() if self.log.exists() else ''}")

    def send(self, *keys: str | bytes) -> None:
        for k in keys:
            os.write(self.fd, KEYS[k] if isinstance(k, str) and k in KEYS else
                     (k if isinstance(k, bytes) else k.encode()))
            time.sleep(0.08)
            self.pump(0.05)

    def poll(self) -> int | None:
        if self.exit_status is None:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid:
                self.exit_status = os.waitstatus_to_exitcode(status)
        return self.exit_status

    def wait_exit(self, timeout: float = 5.0) -> int:
        end = time.time() + timeout
        while time.time() < end:
            self.pump(0.05)
            if self.poll() is not None:
                return self.exit_status  # type: ignore[return-value]
        raise AssertionError(f"vc did not exit\n{self.text()}")

    def close(self) -> None:
        if self.poll() is None:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        os.close(self.fd)
