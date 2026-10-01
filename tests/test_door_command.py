"""COMMAND and its loader stay inside a real native door's private H:.

These use the production binary and ANSI PTYs. No native syscall test hook,
loopback listener, host log, or actual home directory is involved. The shared
fixture permits Landlock fallback on restricted kernels; seccomp stays on.
"""
import subprocess

import pytest

from test_door_e2e import ROOT, panels, paths, running  # noqa: F401 -- shared private-root fixture


ERRORS = ("Bad command or file name", "Error in EXE file", "EXEC failure",
          "Program too big to fit in memory", "Invalid parameter")


def dos_liveness(session, shell, drive, filename):
    session.type_line(f"{shell} /c echo DOOR-ALIVE > {filename}")
    output = drive / filename
    session.until(lambda: output.exists() and output.read_bytes() == b"DOOR-ALIVE \r\n",
                  description="Microsoft COMMAND creates its DOS liveness file")
    session.ready()
    assert session.poll() is None


def marker_elf(paths):
    # A small real ELF fits COMMAND's COM-size check. Executing it would
    # create the marker without any shell redirection that DOS itself might
    # legitimately perform before refusing the program.
    binary = paths.base / "host-elf"
    source = r'''
#include <stdio.h>
int main(void) {
    FILE *file = fopen("HOST-RAN.TXT", "wb");
    if (!file) return 2;
    if (fputs("HOST EXECUTED\n", file) < 0) return 3;
    return fclose(file) ? 4 : 0;
}
'''
    subprocess.run(["cc", "-x", "c", "-Os", "-o", str(binary), "-"], input=source,
                   text=True, capture_output=True, check=True, timeout=30)
    contents = binary.read_bytes()
    assert contents.startswith(b"\x7fELF") and len(contents) < 65536
    # Check the fixture's actual effect outside the door, then remove only
    # that test-owned marker before exercising the confined loader.
    subprocess.run([str(binary)], cwd=paths.work, check=True, timeout=5)
    marker = paths.work / "HOST-RAN.TXT"
    assert marker.read_bytes() == b"HOST EXECUTED\n"
    marker.unlink()
    return contents


@pytest.mark.parametrize("shell", ["command", "SECOND.COM"], ids=["command", "renamed-command"])
@pytest.mark.parametrize("attempt", ["host-shell", "host-elf", "changed-utility"])
def test_command_c_cannot_escape_native_door(paths, shell, attempt):
    elf = marker_elf(paths) if attempt == "host-elf" else None
    with running(paths) as session:
        session.ready()
        drive = session.drive()
        original = (ROOT / "build/msdos2/COMMAND.COM").read_bytes()
        assert (drive / "COMMAND.COM").read_bytes() == original
        if shell != "command":
            (drive / shell).write_bytes(original)
        dos_liveness(session, shell, drive, "BEFORE.TXT")

        if attempt == "host-shell":
            command = 'sh -c "touch HOST-RAN.TXT"'
        elif attempt == "host-elf":
            executable = drive / "HOSTELF.COM"
            executable.write_bytes(elf)
            executable.chmod(0o700)
            command = "HOSTELF.COM"
        else:
            utility = drive / "DOS/FIND.EXE"
            expected = (ROOT / "build/msdos2/FIND.EXE").read_bytes()
            assert utility.read_bytes() == expected
            changed = expected[:-1] + bytes([expected[-1] ^ 1])
            utility.write_bytes(changed)
            command = 'DOS\\FIND.EXE "DOS" README.TXT'

        # /C can return to VC before an idle render shows the error. Wait
        # for the submitted line to clear, then repeat interactively so the
        # DOS error is observable without enabling host diagnostic logs.
        session.type_line(f"{shell} /c {command}")
        session.until(lambda: (panels(session.text()) and
                              session.screen.display[23].strip() == "H:\\>")
                      or session.poll() is not None,
                      description="COMMAND /C returns to the outer VC")
        assert session.poll() is None, "the outer door must survive, not merely rely on seccomp killing it"
        session.type_line(shell)
        session.until(lambda: not panels(session.text()) and
                      session.screen.display[session.screen.cursor.y].strip() == "H:\\>",
                      description="Microsoft COMMAND's interactive prompt")
        session.seen_display.difference_update(ERRORS)
        session.pending_display.update(ERRORS)
        session.type_line(command)
        session.until(lambda: bool(session.seen_display.intersection(ERRORS))
                      or session.poll() is not None,
                      description="COMMAND reports a DOS error for forbidden program")
        assert session.poll() is None
        assert session.seen_display.intersection(ERRORS), session.text()
        session.pending_display.difference_update(ERRORS)
        session.until(lambda: session.screen.display[session.screen.cursor.y].strip() == "H:\\>",
                      description="COMMAND stays alive after rejecting the program")
        session.type_line("exit")
        session.ready()
        for directory in (drive, paths.home, paths.work):
            assert not (directory / "HOST-RAN.TXT").exists(), "a forbidden host program executed"

        dos_liveness(session, shell, drive, "AFTER.TXT")
        session.send("f7")
        session.wait_for("Make directory")
        session.send(b"VC-ALIVE", "enter")
        session.until(lambda: (drive / "VC-ALIVE").is_dir(), description="outer VC still works")
        session.ready()
        assert (paths.home / "HOST.TXT").read_text() == "PRIVATE HOME MUST STAY OUTSIDE THE DOOR\n"
        assert (paths.work / "HOST.TXT").read_text() == "PRIVATE CWD MUST STAY OUTSIDE THE DOOR\n"
        session.quit_vc()
        session.wait_clean()
