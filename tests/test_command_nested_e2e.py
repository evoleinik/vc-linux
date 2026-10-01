"""EXEC through COMMAND's hooked loader must never tear down the outer VC."""
import re

import pytest

from test_e2e import until
from test_gwbasic_e2e import panels, running_vc
from test_msdos_e2e import PROMPT, back_to_vc, line, start_command


DOS_ERROR = re.compile(r"error|EXEC failure|not enough|unable|cannot|not found|unavailable", re.I)


def current_console_line(session):
    lines = session.text().splitlines()
    # GW-BASIC leaves its function-key labels on row 25. They are not part
    # of the command transcript, including while a SHELL child is active.
    if len(lines) == 25 and "1LIST" in lines[-1] and "0SCREEN" in lines[-1]:
        lines = lines[:-1]
    return next((line.strip() for line in reversed(lines) if line.strip()), "")


def command_prompt(session):
    return not panels(session.text()) and PROMPT.fullmatch(current_console_line(session)) is not None


def basic_prompt(session):
    lines = session.text().splitlines()
    return (len(lines) == 25 and "1LIST" in lines[-1] and "0SCREEN" in lines[-1]
            and current_console_line(session) == "Ok")


def kermit_prompt(session):
    return current_console_line(session) == "MS-Kermit>"


def debug_prompt(session):
    return current_console_line(session) == "-"


def response_after(session, command):
    before, separator, after = session.text().rpartition(command)
    return after if separator else ""


def com_psp(log, image):
    # COM load segments are always PSP+10h; do not apply this to high-loaded
    # MZ images, whose actual PSP cannot be inferred from this log line.
    assert image.endswith(".COM")
    loads = re.findall(rf"(?m)^loaded {re.escape(image)} at ([0-9A-F]{{4}})\b", log)
    return (int(loads[-1], 16) - 0x10) & 0xFFFF if loads else None


def terminated(log, psp=None, code=None):
    process = f"{psp:04X}" if psp is not None else "[0-9A-F]{4}"
    status = str(code) if code is not None else r"\d+"
    return re.search(rf"(?m)^terminate psp {process} code {status}$", log) is not None


def survived(session, before):
    assert session.poll() is None
    assert "no translated code at" not in session.log.read_text()[before:]


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_vc_exec_under_command_returns_or_reports_dos_error(quick):
    with running_vc(quick=quick) as (session, _, _):
        before = start_command(session)
        launch = len(session.log.read_text())
        session.send(b"vc", "enter")

        def vc_result():
            log = session.log.read_text()[launch:]
            child = com_psp(log, "VC.COM")
            if child is None or "translation VC.COM (DOS-hosted loader)" not in log:
                return False
            if terminated(log, child, 2):
                return command_prompt(session) and "Error reading overlay file." in session.text()
            # A returned outer VC also has panels. Never send F10 unless the
            # newly loaded nested VC has not already terminated.
            return panels(session.text()) and not terminated(log, child)

        until(session, vc_result, timeout=15)
        child = com_psp(session.log.read_text()[launch:], "VC.COM")
        if panels(session.text()):
            leave = len(session.log.read_text())
            session.send("f10")
            until(session, lambda: command_prompt(session) and
                  terminated(session.log.read_text()[leave:], child), timeout=15)
        else:
            # DOS2's loader copies environment strings without DOS3's
            # executable-name trailer; VC reports its own DOS file error.
            assert terminated(session.log.read_text()[launch:], child, 2)
        survived(session, before)
        line(session, "echo NESTED-VC-RETURNED", lambda text: "NESTED-VC-RETURNED" in text)
        back_to_vc(session)


def test_kermit_push_under_command_survives():
    with running_vc() as (session, _, _):
        before = start_command(session)
        session.send(b"kermit", "enter")
        until(session, lambda: kermit_prompt(session) and
              "translation KERMIT.EXE (DOS-hosted loader)" in session.log.read_text()[before:], timeout=15)
        push = len(session.log.read_text())
        session.send(b"push", "enter")
        until(session, lambda: (command_prompt(session) and
              "translation COMMAND.COM (DOS-hosted loader)" in session.log.read_text()[push:]) or
              (kermit_prompt(session) and DOS_ERROR.search(response_after(session, "push"))), timeout=15)
        if command_prompt(session):
            assert "translation COMMAND.COM (DOS-hosted loader)" in session.log.read_text()[push:]
            child = com_psp(session.log.read_text()[push:], "COMMAND.COM")
            assert child is not None
            line(session, "echo PUSH-SHELL-OK", lambda text: "PUSH-SHELL-OK" in text)
            leave = len(session.log.read_text())
            session.send(b"exit", "enter")
            until(session, lambda: kermit_prompt(session) and
                  terminated(session.log.read_text()[leave:], child), timeout=15)
        leave = len(session.log.read_text())
        session.send(b"exit", "enter")
        until(session, lambda: command_prompt(session) and
              terminated(session.log.read_text()[leave:]), timeout=15)
        survived(session, before)
        back_to_vc(session)


def test_gwbasic_shell_under_command_survives():
    with running_vc() as (session, _, _):
        before = start_command(session)
        session.send(b"gwbasic", "enter")
        until(session, lambda: basic_prompt(session) and
              "translation GWBASIC.EXE (DOS-hosted loader)" in session.log.read_text()[before:], timeout=15)
        nested = len(session.log.read_text())
        session.send(b"SHELL", "enter")
        until(session, lambda: (command_prompt(session) and
              "translation COMMAND.COM (DOS-hosted loader)" in session.log.read_text()[nested:]) or
              (basic_prompt(session) and DOS_ERROR.search(response_after(session, "SHELL"))), timeout=15)
        if command_prompt(session):
            assert "translation COMMAND.COM (DOS-hosted loader)" in session.log.read_text()[nested:]
            child = com_psp(session.log.read_text()[nested:], "COMMAND.COM")
            assert child is not None
            line(session, "echo BASIC-INTERACTIVE-SHELL-OK", lambda text:
                 re.search(r"(?m)^BASIC-INTERACTIVE-SHELL-OK\s*$", text))
            leave = len(session.log.read_text())
            session.send(b"exit", "enter")
            until(session, lambda: basic_prompt(session) and
                  terminated(session.log.read_text()[leave:], child), timeout=15)
        else:
            # The shipped BASIC reports Syntax error for its unsupported
            # SHELL statement. This is a visible guest error, not a claim
            # that an interactive child shell successfully ran.
            assert DOS_ERROR.search(response_after(session, "SHELL"))
        leave = len(session.log.read_text())
        session.send(b"SYSTEM", "enter")
        until(session, lambda: command_prompt(session) and
              terminated(session.log.read_text()[leave:]), timeout=15)
        survived(session, before)
        back_to_vc(session)


def test_debug_unknown_com_go_under_command_is_contained():
    # A real tiny DOS program, deliberately not one of the byte-approved
    # images: DEBUG may report DOS bad-format, or the dispatcher may stop it.
    with running_vc(files={"X.COM": b"\xb8\x00\x4c\xcd\x21"}) as (session, _, _):
        before = start_command(session)
        session.send(b"debug X.COM", "enter")
        until(session, lambda: "translation DEBUG.COM (DOS-hosted loader)" in
              session.log.read_text()[before:] and (debug_prompt(session) or
              (command_prompt(session) and "DEBUG.COM stopped." in session.text())), timeout=15)
        if debug_prompt(session):
            go = len(session.log.read_text())
            session.send(b"g", "enter")
            until(session, lambda: (command_prompt(session) and
                  "DEBUG.COM stopped." in session.log.read_text()[go:]) or
                  (debug_prompt(session) and re.search(r"error|terminated|not found|bad|cannot",
                   response_after(session, "-g"), re.I)), timeout=15)
            if not command_prompt(session):
                leave = len(session.log.read_text())
                session.send(b"q", "enter")
                until(session, lambda: command_prompt(session) and
                      terminated(session.log.read_text()[leave:]), timeout=15)
        survived(session, before)
        line(session, "echo DEBUG-SURVIVED", lambda text: "DEBUG-SURVIVED" in text)
        back_to_vc(session)
