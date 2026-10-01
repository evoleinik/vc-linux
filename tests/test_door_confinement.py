"""Kernel confinement exercised in the real door startup path, not DOS checks.

Only build/vc-door-confinement-test contains the native syscall hooks. The
ordinary binary and the static ARM64 release never accept these environment
variables as a path around their filesystem checks.
"""
import errno
from functools import lru_cache
import os
import platform
import re
import signal
import subprocess
import sys

import pytest

import test_door_e2e as door
from test_door_e2e import paths  # noqa: F401 -- shared private-root fixture


@lru_cache(maxsize=None)
def kernel_probe(kind):
    """Independent capability probe: never skip because production code failed.

    The child alone gains an allow-all seccomp filter or deny-all filesystem
    Landlock domain. A deployment without these APIs retains the actual door
    enforcement tests and gets an explicit kernel-policy skip explanation.
    """
    if platform.machine() != "x86_64":
        pytest.skip("native confinement syscall hooks require x86-64")
    probe = r'''
import ctypes as c
import errno
import os
import sys

libc = c.CDLL(None, use_errno=True)
def checked(result, stage):
    if result < 0:
        print(f"{stage}:{c.get_errno()}", flush=True)
        os._exit(1)
    return result
checked(libc.prctl(38, 1, 0, 0, 0), "no-new-privileges")
if sys.argv[1] == "landlock":
    abi = checked(libc.syscall(444, 0, 0, 1), "Landlock ABI query")
    rights = c.c_uint64((1 << 2) | (1 << 3))
    fd = checked(libc.syscall(444, c.byref(rights), c.sizeof(rights), 0), "Landlock ruleset")
    checked(libc.syscall(446, fd, 0), "Landlock restrict_self")
    os.close(fd)
    print(f"Landlock ABI {abi}: enforcement available", flush=True)
else:
    class Filter(c.Structure):
        _fields_ = [("code", c.c_ushort), ("jt", c.c_ubyte), ("jf", c.c_ubyte), ("k", c.c_uint)]
    class Program(c.Structure):
        _fields_ = [("len", c.c_ushort), ("filter", c.POINTER(Filter))]
    instruction = Filter(6, 0, 0, 0x7fff0000)
    program = Program(1, c.pointer(instruction))
    checked(libc.prctl(22, 2, c.byref(program)), "seccomp filter installation")
    print("seccomp: enforcement available", flush=True)
os._exit(0)
'''
    result = subprocess.run([sys.executable, "-c", probe, kind], capture_output=True,
                            text=True, timeout=10, check=False)
    detail = result.stdout.strip()
    if result.returncode:
        stage, separator, number = detail.rpartition(":")
        if result.returncode == 1 and separator and number.isdigit():
            error = int(number)
            if error in (errno.ENOSYS, errno.EOPNOTSUPP, errno.EPERM, errno.EACCES):
                pytest.skip(f"{kind} unavailable in this kernel/sandbox: {stage}: {os.strerror(error)}")
        pytest.fail(f"independent {kind} probe failed: {result.returncode}: {detail} {result.stderr}")
    return detail


def landlock_abi():
    match = re.search(r"Landlock ABI (\d+)", kernel_probe("landlock"))
    assert match, "Landlock probe printed no ABI"
    return int(match.group(1))


def require_signal_scoping():
    abi = landlock_abi()
    if abi < 6:
        pytest.skip(f"running kernel has Landlock ABI {abi}; signal scoping needs ABI 6 "
                    "(Linux 6.12+). axis runs 6.14, so run this test there.")


@pytest.fixture(autouse=True)
def confinement_binary(monkeypatch):
    binary = door.ROOT / "build/vc-door-confinement-test"
    assert binary.is_file(), f"{binary} missing; run make test-door-confinement"
    monkeypatch.setattr(door, "VC", binary)


def test_landlock_denies_native_open_outside_session(paths):
    kernel_probe("landlock")
    kernel_probe("seccomp")
    outside = paths.home / "HOST.TXT"
    with door.running(paths, args=("--door",),
                      extra_env={"VC_DOOR_TEST_HOOK": "outside",
                                        "VC_DOOR_TEST_OUTSIDE": str(outside)}) as session:
        assert session.wait_exit() == 0, bytes(session.raw)
        assert f"opened=0 errno={errno.EACCES}".encode() in session.raw
        session.wait_clean()
    assert outside.read_text() == "PRIVATE HOME MUST STAY OUTSIDE THE DOOR\n"


def test_seccomp_kills_native_execve(paths):
    kernel_probe("seccomp")
    with door.running(paths, args=("--door", "--door-allow-unconfined"),
                      extra_env={"VC_DOOR_TEST_HOOK": "execve"}) as session:
        assert session.wait_exit() == -signal.SIGSYS, bytes(session.raw)
        assert b"execve survived" not in session.raw
        session.wait_clean()


def test_landlock_allows_private_session_native_read_write(paths):
    kernel_probe("landlock")
    kernel_probe("seccomp")
    with door.running(paths, args=("--door",),
                      extra_env={"VC_DOOR_TEST_HOOK": "inside"}) as session:
        assert session.wait_exit() == 0, bytes(session.raw)
        assert b"read/write/delete OK" in session.raw
        session.wait_clean()


def test_seccomp_keeps_every_allowed_terminal_ioctl(paths):
    kernel_probe("landlock")
    kernel_probe("seccomp")
    with door.running(paths, args=("--door",),
                      extra_env={"VC_DOOR_TEST_HOOK": "terminal"}) as session:
        assert session.wait_exit() == 0, bytes(session.raw)
        assert b"door-test terminal: ioctls OK" in session.raw
        session.wait_clean()


@pytest.mark.parametrize("hook", [
    "execveat", "socket", "connect", "ptrace", "mount", "bpf", "io_uring", "clone3",
    "mmap-executable", "ioctl-inject", "kill-other", "utimens-path", "x32", "compat-i386",
])
def test_seccomp_kills_other_forbidden_syscalls(paths, hook):
    kernel_probe("seccomp")
    with door.running(paths, args=("--door", "--door-allow-unconfined"),
                      extra_env={"VC_DOOR_TEST_HOOK": hook}) as session:
        assert session.wait_exit() == -signal.SIGSYS, (hook, bytes(session.raw))
        assert b"forbidden syscall survived" not in session.raw
        session.wait_clean()


def test_landlock_unavailable_refuses_before_native_hook(paths):
    with door.running(paths, args=("--door",), extra_env={
        "VC_DOOR_TEST_FAILURE": "landlock", "VC_DOOR_TEST_HOOK": "inside",
    }) as session:
        assert session.wait_exit() == 1, bytes(session.raw)
        assert b"Landlock confinement unavailable; refusing to start" in session.raw
        assert b"door-test" not in session.raw
        assert session.raw.count(b"vc:") == 1
        session.wait_clean()


def test_allow_unconfined_landlock_fallback_still_installs_seccomp(paths):
    kernel_probe("seccomp")
    with door.running(paths, args=("--door", "--door-allow-unconfined"), extra_env={
        "VC_DOOR_TEST_FAILURE": "landlock", "VC_DOOR_TEST_HOOK": "execve",
    }) as session:
        assert session.wait_exit() == -signal.SIGSYS, bytes(session.raw)
        assert b"Landlock unavailable; --door-allow-unconfined is for tests only" in session.raw
        assert b"execve survived" not in session.raw
        session.wait_clean()


def test_allow_unconfined_never_bypasses_seccomp_failure(paths):
    with door.running(paths, args=("--door", "--door-allow-unconfined"), extra_env={
        "VC_DOOR_TEST_FAILURE": "seccomp", "VC_DOOR_TEST_HOOK": "inside",
    }) as session:
        assert session.wait_exit() == 1, bytes(session.raw)
        assert b"cannot install door seccomp confinement; refusing to start" in session.raw
        assert b"door-test" not in session.raw
        session.wait_clean()


def test_seccomp_kills_sigio_aimed_at_the_janitor(paths):
    """F_SETOWN on any kernel: the worker dies, the janitor still cleans up."""
    kernel_probe("seccomp")
    with door.running(paths, args=("--door", "--door-allow-unconfined"),
                      extra_env={"VC_DOOR_TEST_HOOK": "sigio-janitor"}) as session:
        session.until(lambda: session.poll() is not None or b"sigio armed" in session.raw,
                      description="worker killed or SIGIO armed")
        if session.poll() is None:
            session.send(b"x\r")  # one input line (cooked tty) delivers SIGIO
        assert session.wait_exit() == -signal.SIGSYS, bytes(session.raw)
        assert b"sigio armed" not in session.raw
        session.wait_clean()


@pytest.mark.parametrize("hook", ["signal-janitor", "sigio-janitor"])
def test_landlock_scoping_stops_a_worker_signalling_its_janitor(paths, hook):
    """Landlock alone (seccomp switched off in the test binary): kill(2) and
    SIGIO from inside the domain cannot reach the unconfined janitor."""
    kernel_probe("seccomp")
    require_signal_scoping()
    with door.running(paths, args=("--door",), extra_env={
        "VC_DOOR_TEST_HOOK": hook, "VC_DOOR_TEST_FAILURE": "seccomp-off",
    }) as session:
        if hook == "sigio-janitor":
            session.until(lambda: b"sigio armed" in session.raw or session.poll() is not None,
                          description="SIGIO armed")
            session.send(b"x\r")
        assert session.wait_exit() == 0, bytes(session.raw)
        session.wait_clean()


def test_janitor_ends_a_worker_that_closes_its_lifetime_pipe(paths):
    kernel_probe("seccomp")
    with door.running(paths, args=("--door", "--door-allow-unconfined"),
                      extra_env={"VC_DOOR_TEST_HOOK": "close-lifetime"}) as session:
        session.until(lambda: b"lifetime pipe closed" in session.raw or session.poll() is not None,
                      description="lifetime pipe closed")
        # The default limit is 60 minutes: only the pipe rule can end it now.
        assert session.wait_exit(timeout=6) in (-signal.SIGTERM, -signal.SIGKILL), bytes(session.raw)
        session.wait_clean()
