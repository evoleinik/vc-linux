"""Test-only bootLogo execution with Unicorn and a minimal pixel BIOS.

This deliberately does not import the translator or native BIOS. It runs the
actual COM file and records INT 10h pixels, to independently locate drawing
coordinates and exercise the arithmetic behind the graphics e2e assertions.
Text glyphs are outside this oracle's remit; bootLogo writes them at row 21.
"""

from collections import deque
from dataclasses import dataclass

from unicorn import Uc, UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_INTR, UC_MODE_16
from unicorn.x86_const import (UC_X86_REG_AX, UC_X86_REG_CS, UC_X86_REG_CX,
                               UC_X86_REG_DS, UC_X86_REG_DX, UC_X86_REG_ES,
                               UC_X86_REG_IP, UC_X86_REG_SP, UC_X86_REG_SS)


@dataclass
class LogoOracle:
    pixels: bytearray
    visited: set[int]
    exited: bool
    coordinates: tuple[int, int]
    color: int


def run_logo(image: bytes, commands: str) -> LogoOracle:
    machine = Uc(UC_ARCH_X86, UC_MODE_16)
    machine.mem_map(0, 0x100000)
    segment = 0x2000
    base = segment * 16
    machine.mem_write(base + 0x100, image)
    machine.mem_write(base, b"\xcd\x20")
    for register in (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS):
        machine.reg_write(register, segment)
    machine.reg_write(UC_X86_REG_IP, 0x100)
    machine.reg_write(UC_X86_REG_SP, 0xfffe)
    keys = deque(commands.encode("ascii"))
    pixels = bytearray(320 * 200)
    visited = set()
    stopped = False
    exited = False

    def instruction(machine, address, size, unused):
        visited.add(address - base - 0x100)

    def interrupt(machine, number, unused):
        nonlocal stopped, exited
        ax = machine.reg_read(UC_X86_REG_AX)
        if number == 0x10:
            function = ax >> 8
            if function == 0:
                assert ax & 255 == 4
                pixels[:] = bytes(len(pixels))
            elif function == 0x0c:
                x = machine.reg_read(UC_X86_REG_CX)
                y = machine.reg_read(UC_X86_REG_DX)
                if x < 320 and y < 200:
                    at = y * 320 + x
                    color = ax & 3
                    pixels[at] = pixels[at] ^ color if ax & 128 else color
            else:
                assert function in (2, 9), f"unexpected INT 10h AX={ax:04x}"
        elif number == 0x16:
            assert ax >> 8 == 0
            if keys:
                machine.reg_write(UC_X86_REG_AX, keys.popleft())
            else:
                stopped = True
                machine.emu_stop()
        elif number == 0x20:
            exited = stopped = True
            machine.emu_stop()
        else:
            raise AssertionError(f"unexpected interrupt {number:02x}")

    machine.hook_add(UC_HOOK_CODE, instruction)
    machine.hook_add(UC_HOOK_INTR, interrupt)
    machine.emu_start(base + 0x100, 0, count=2_000_000)
    assert stopped and not keys, "bootLogo failed to consume its command input"
    x = int.from_bytes(machine.mem_read(base + 0x502, 2), "little")
    y = int.from_bytes(machine.mem_read(base + 0x504, 2), "little")
    return LogoOracle(pixels, visited, exited, (x, y),
                      machine.mem_read(base + 0x2af, 1)[0])
