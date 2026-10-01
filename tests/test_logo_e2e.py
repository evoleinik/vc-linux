"""Real DOS bootLogo and GW-BASIC CGA, including the terminal's actual pixels.

PGM bytes are raw CGA palette indices. Glyph recognition below uses the
vendored source font, not the BIOS implementation, to wait on graphics UI
state without fixed sleeps or an invented debug command in either program.
"""
from dataclasses import dataclass
from pathlib import Path
import re

import pytest

from test_gwbasic_e2e import (BASIC_ERRORS, ROOT, panels, return_to_vc, running_vc,
                              select, start_basic, until)


@dataclass(frozen=True)
class Frame:
    width: int
    height: int
    maximum: int
    pixels: bytes

    def pixel(self, x: int, y: int) -> int:
        return self.pixels[y * self.width + x]


def frame(session) -> Frame | None:
    try:
        data = session.frame_dump.read_bytes()
    except FileNotFoundError:
        return None
    match = re.match(rb"P5\n(\d+) (\d+)\n(\d+)\n", data)
    if not match:
        return None
    width, height, maximum = map(int, match.groups())
    pixels = data[match.end():]
    if width not in (320, 640) or height != 200 or len(pixels) != width * height:
        return None
    return Frame(width, height, maximum, pixels)


def wait_frame(session, predicate, timeout=10) -> Frame:
    found = None

    def check():
        nonlocal found
        found = frame(session)
        return found is not None and predicate(found)

    until(session, check, timeout=timeout)
    assert found is not None and session.poll() is None
    return found


FONT = {
    tuple(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]+)", values)): chr(int(code, 16))
    for values, code in re.findall(r"\{([^}]+)\},\s*// U\+([0-9A-Fa-f]{4})",
                                   (ROOT / "third_party/font8x8/font8x8_basic.h").read_text())
    if 32 <= int(code, 16) < 127
}


def graphics_row(picture: Frame, row: int) -> str:
    result = []
    for column in range(picture.width // 8):
        glyph = tuple(sum((picture.pixel(column * 8 + x, row * 8 + y) != 0) << x
                          for x in range(8)) for y in range(8))
        result.append(FONT.get(glyph, "?"))
    return "".join(result)


def graphics_text(picture: Frame) -> str:
    return "\n".join(graphics_row(picture, row) for row in range(25))


def basic_line(session, command: bytes) -> None:
    # The original interpreter has its own small type-ahead buffer. Type
    # in acknowledged chunks, just as a person does; never flood that DOS
    # queue with an entire long pasted statement and assume no keys drop.
    in_graphics = any(0x2800 <= ord(ch) <= 0x28ff for ch in session.text())
    for end in range(8, len(command) + 8, 8):
        session.send(command[end - 8:end])
        expected = re.sub(r"\s+", "", command[:end].decode("ascii"))

        def echoed():
            picture = frame(session)
            text = graphics_text(picture) if in_graphics and picture else session.text()
            return expected in re.sub(r"\s+", "", text)

        until(session, echoed)
    session.send("enter")


def start_logo(session, command=b"bootlogo") -> Frame:
    session.send(command, "enter")
    picture = wait_frame(session, lambda f: f.width == 320 and graphics_row(f, 21).rstrip() == ">")
    assert picture.maximum == 3
    assert "translation LOGO.COM" in session.log.read_text()
    assert any(picture.pixels[80 * 320:110 * 320]), "bootLogo's XOR turtle was not drawn"
    return picture


def logo_line(session, command: str) -> Frame:
    assert len(command) < 40, "helper waits on bootLogo's single visible 40-column input row"
    session.send(command.encode("ascii"))
    wait_frame(session, lambda f: graphics_row(f, 21).startswith(">" + command.upper()))
    session.send("enter")
    return wait_frame(session, lambda f: graphics_row(f, 21).rstrip() == ">")


def quit_logo(session) -> None:
    session.send(b"QUIT")
    wait_frame(session, lambda f: graphics_row(f, 21).startswith(">QUIT"))
    session.send("enter")
    # bootLogo's INT 20h does not reset video. Real VC detects 40 columns
    # in Init_RestoreVideoMode (VCOVL.ASM), asks for Enter, and then executes
    # its RestoreVideoMode routine. Do not fake a mode reset in DOS EXEC.
    wait_frame(session, lambda f: "Press ENTER to return" in graphics_text(f))
    assert not panels(session.text())
    session.send("enter")
    until(session, lambda: panels(session.text()))
    assert session.text() == session.memory_text()
    assert session.poll() is None


# BootLogo's 9.7 fixed-point coordinates start at 160*128,100*128. Its
# table supplies exactly 0,+128,0,-128 at the cardinal angles, and FD draws
# BEFORE advance. The four segments therefore meet at these exact pixels.
SQUARE_CORNERS = ((160, 100), (160, 50), (210, 50), (210, 100))


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("command", [b"bootlogo", b"BOOTLOGO.COM"])
def test_logo_command_starts_real_installed_file(quick, command):
    with running_vc(quick=quick) as (session, work, config):
        assert (config / "BOOTLOGO.COM").read_bytes() == (ROOT / "build/bootlogo/LOGO.COM").read_bytes()
        assert not (config / "LOGO.COM").exists(), "bootLogo must not capture the host logo command"
        assert not (work / "BOOTLOGO.COM").exists(), "PATH must find the installed COM"
        start_logo(session, command)


def test_logo_square_exact_corners_and_braille():
    with running_vc() as (session, _, _):
        start_logo(session)
        picture = logo_line(session, "REPEAT 4 [FD 50 RT 90]")
        assert [picture.pixel(x, y) for x, y in SQUARE_CORNERS] == [3] * 4
        # Whole distant edges exclude a four-dot-only implementation, and
        # the square's interior stays blank. The turtle overlays only its
        # home corner, not these samples.
        assert all(picture.pixel(x, 50) == 3 for x in range(160, 211))
        assert all(picture.pixel(210, y) == 3 for y in range(50, 101))
        assert picture.pixel(185, 75) == 0
        until(session, lambda: session.text() == session.memory_text())
        lines = session.memory_text().splitlines()
        assert len(lines) == 25 and all(len(line) == 80 for line in lines)
        for x, y in SQUARE_CORNERS:
            code = ord(lines[y // 8][x // 4])
            # Each source 2x2 square becomes one braille dot.
            dots = ((0, 3), (1, 4), (2, 5), (6, 7))
            bit = dots[(y // 2) % 4][(x // 2) % 2]
            assert 0x2800 <= code <= 0x28ff and code & (1 << bit)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
def test_logo_quit_restores_vc_text_panels(quick):
    with running_vc(quick=quick, files={"PANEL.TXT": b"still here\r\n"}) as (session, _, _):
        start_logo(session)
        logo_line(session, "REPEAT 4 [FD 50 RT 90]")
        quit_logo(session)
        assert "PANEL" in session.text()
        start_logo(session)
        quit_logo(session)


@pytest.mark.parametrize("quick", [False, True], ids=["comspec", "int2e"])
@pytest.mark.parametrize("key", [b"\x1b[57362;5u", b"\x1b[98;6u"],
                         ids=["ctrl-pause", "ctrl-shift-b"])
def test_logo_ctrl_break_stops_nonpolling_loop(key, quick):
    with running_vc(quick=quick, files={"PANEL.TXT": b"still here\r\n"}) as (session, work, _):
        start_logo(session)
        command = "REPEAT 0 [REPEAT 0 [FD 1]]"
        session.send(command.encode("ascii"))
        wait_frame(session, lambda f: graphics_row(f, 21).startswith(">" + command))
        session.send("enter")
        # FD draws upward from (160,100). A distant new point and the
        # still-occupied command row prove the nested loop is executing,
        # not blocked in bootLogo's INT 16h input routine.
        picture = wait_frame(session, lambda f: f.pixel(160, 40) == 3)
        assert graphics_row(picture, 21).rstrip() != ">"
        session.send(key)
        wait_frame(session, lambda f: "Press ENTER to return" in graphics_text(f))
        session.send(b"\x1b[57442;1:3u\x1b[57441;1:3u", "enter")
        until(session, lambda: panels(session.text()))
        assert session.poll() is None
        assert "PANEL" in session.text()
        assert (work / "PANEL.TXT").read_bytes() == b"still here\r\n"
        assert session.text() == session.memory_text()
        # The break must not poison the next child's keyboard input or
        # leave behind its PSP, interrupt vectors, or graphics state.
        start_logo(session)
        quit_logo(session)


@pytest.mark.parametrize("name", ["BOOTLOGO.COM", "LOGO.COM", "TURTLE.COM", "TURTLE.EXE"])
def test_logo_enter_and_renamed_file_match_by_bytes(name):
    data = (ROOT / "build/bootlogo/LOGO.COM").read_bytes()
    with running_vc(files={name: data}) as (session, _, _):
        select(session, name)
        session.send("enter")
        wait_frame(session, lambda f: graphics_row(f, 21).rstrip() == ">")
        assert "translation LOGO.COM" in session.log.read_text()
        quit_logo(session)


def test_logo_changed_com_is_refused_by_exec():
    changed = bytearray((ROOT / "build/bootlogo/LOGO.COM").read_bytes())
    changed[-1] ^= 1
    with running_vc(files={"BAD.COM": bytes(changed)}) as (session, _, _):
        select(session, "BAD.COM")
        session.send("enter")
        until(session, lambda: "no matching translation" in session.log.read_text())
        until(session, lambda: panels(session.text()))
        assert "translation LOGO.COM" not in session.log.read_text()
        assert frame(session) is None and session.poll() is None


def test_logo_pen_color_backward_and_nested_procedures():
    with running_vc() as (session, _, _):
        start_logo(session)
        logo_line(session, "SETCOLOR 2")
        picture = logo_line(session, "FD 20")
        assert picture.pixel(160, 90) == 2, "live self-modified COLOR operand"
        logo_line(session, "PU")
        logo_line(session, "RT 90")
        picture = logo_line(session, "FD 20")
        assert picture.pixel(170, 80) == 0, "overlapping DB opcode in PU"
        logo_line(session, "PD")
        picture = logo_line(session, "BK 10")
        # The final east-pointing XOR turtle cancels (175,80), its nose.
        # (178,80) is on BK's line and outside that overlay (Unicorn oracle).
        assert picture.pixel(178, 80) == 2, "overlapping DB opcode in BK"
        logo_line(session, "TO SQ REPEAT 4 [FD 10 RT 90] END")
        picture = logo_line(session, "REPEAT 2 [SQ RT 180]")
        assert sum(pixel == 2 for pixel in picture.pixels[:160 * 320]) > 65


def test_logo_shipped_star_and_flower():
    with running_vc() as (session, _, _):
        start_logo(session)
        star = logo_line(session, "REPEAT 36 [FD 60 RT 170]")
        assert sum(pixel == 3 for pixel in star.pixels[:160 * 320]) > 800
        logo_line(session, "CLEARSCREEN")
        # Use the exact README flower, proving TO, nesting and the published
        # example all run in the tiny original interpreter.
        lines = (ROOT / "web/BOOTLOGO.TXT").read_text().split("3. Flower", 1)[1]
        commands = re.findall(r"(?m)^  (.+)$", lines)
        for command in commands:
            flower = logo_line(session, command)
        assert sum(pixel == 3 for pixel in flower.pixels[:160 * 320]) > 250
        quit_logo(session)


def test_gwbasic_cga_line_circle_pset_draw_and_screen_zero():
    with running_vc() as (session, _, _):
        start_basic(session)
        basic_line(session, b"SCREEN 1: LINE (10,10)-(100,10): CIRCLE (160,100),40")
        picture = wait_frame(session, lambda f: f.pixel(100, 10) == 3 and f.pixel(200, 100) == 3)
        assert picture.width == 320 and picture.maximum == 3
        assert all(picture.pixel(x, 10) == 3 for x in range(10, 101))
        assert picture.pixel(120, 100) == 3
        assert picture.pixel(160, 100) == 0
        assert not BASIC_ERRORS.search(graphics_text(picture))
        # Keep this odd-bank sample clear of the input/Ok glyph rows.
        basic_line(session, b'PSET (17,151),2: DRAW "BM250,90 R20 D20 L20 U20"')
        # This fork's SETATR leaves the PSET colour as DRAW's working ink.
        picture = wait_frame(session, lambda f: f.pixel(17, 151) == 2 and f.pixel(270, 110) == 2)
        assert all(picture.pixel(x, y) == 2 for x, y in ((250, 90), (270, 90), (250, 110)))
        basic_line(session, b"SCREEN 0")
        until(session, lambda: re.search(r"(?m)^Ok\s*$", session.text()))
        assert "\u28ff" not in session.text()
        return_to_vc(session)


@pytest.mark.parametrize("screen,width", [(b"SCREEN 1,0", 320), (b"SCREEN 2", 640)])
def test_gwbasic_other_cga_modes(screen, width):
    with running_vc() as (session, _, _):
        start_basic(session)
        basic_line(session, screen + b": PSET (319,199),1")
        picture = wait_frame(session, lambda f: f.width == width and f.pixel(319, 199) == 1)
        assert picture.maximum == (1 if width == 640 else 3)
        basic_line(session, b"SCREEN 0")
        until(session, lambda: re.search(r"(?m)^Ok\s*$", session.text()))
        return_to_vc(session)


def test_shipped_spiral_basic_draws_and_returns_to_text():
    source = (ROOT / "web/GAMES/SPIRAL.BAS").read_text().replace("\n", "\r\n").encode("ascii")
    with running_vc(files={"SPIRAL.BAS": source}) as (session, _, _):
        select(session, "SPIRAL.BAS")
        session.send("enter")
        picture = wait_frame(session, lambda f: "PRESS A KEY TO RETURN TO TEXT" in graphics_row(f, 22))
        assert not BASIC_ERRORS.search(graphics_text(picture))
        assert sum(pixel == 3 for pixel in picture.pixels[:160 * 320]) > 3000
        assert picture.pixel(160, 100) == 3 and picture.pixel(164, 100) == 3
        session.send(b" ")
        session.wait_for("SPIRAL DONE - TYPE SYSTEM FOR VC")
        return_to_vc(session)
