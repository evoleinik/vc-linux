#!/usr/bin/env python3
"""Prove Hack's pty gates reject five isolated runtime defects, then recover.

Usage: WATCOM=$PWD/build/openwatcom .venv/bin/python tools/hack_gameplay_mutations.py
Build `make` first. No checked-in source, generated translation, installed
binary, or user game is changed: each mutant links a private runtime copy to
the ordinary generated objects, and each pty test owns a fresh /tmp drive.
"""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ("rt", "dos_core", "main", "cpu", "dos_fs", "cp866", "bios", "term",
           "modem", "modem_transport", "door", "door_confinement")
GENERATED = ("vc_com", "vc_ovl", "gwbasic", "bootlogo", "gwbasic_graphics",
             "rogue", "hack", "vz", "kermit", "command", "edlin", "debug",
             "find", "more", "sort", "fc", "files", "door_demo")
IS_HACK = ('rt_image_return("HACK.EXE", rd16(cpu.ss, (uint16_t)(cpu.sp + 2)), '
           'rd16(cpu.ss, cpu.sp))')
KEY_HANDLER = "    case 0x16: bios_int16(); return 0;"


def keyboard_defect(condition):
    return ("    case 0x16: {\n"
            f"        int hack_read = ({IS_HACK}) && (cpu.a.h == 0 || cpu.a.h == 0x10);\n"
            "        bios_int16();\n"
            f"        if (hack_read && ({condition})) cpu.a.l = 27;\n"
            "        return 0;\n    }")


CASES = {
    "status": ("rt", "    case 0x10: bios_int10(); return 0;",
               "    case 0x10:\n"
               f"        if (({IS_HACK}) && cpu.a.h == 9 && rd8(0x40, 0x51) == 23) cpu.a.l = ' ';\n"
               "        bios_int10(); return 0;",
               "test_hack_command_starts_first_level_with_player_and_status[hack103-comspec]"),
    "movement": ("rt", KEY_HANDLER,
                 keyboard_defect('strchr("hjklyubn", cpu.a.l) != NULL'),
                 "test_hack_movement_moves_player"),
    "save": ("rt", KEY_HANDLER, keyboard_defect("cpu.a.l == 'S'"),
             "test_hack_save_restores_same_game_beside_executable"),
    "restore": ("dos_fs", "    int error = read_string(cpu.ds, off, dos, sizeof(dos));\n"
                "    if (error) return error;",
                "    int error = read_string(cpu.ds, off, dos, sizeof(dos));\n"
                "    if (error) return error;\n"
                "    if (!strcmp(dos, \"HACK.SAV\") && (mode & 3) == 0) return 2;",
                "test_hack_save_restores_same_game_beside_executable"),
    "quit": ("rt", KEY_HANDLER, keyboard_defect("cpu.a.l == 'Q'"),
             "test_hack_quit_returns_to_live_vc[comspec]"),
}


def run_test(binary, test, destination):
    # pytest reports subprocess kwargs on failures; pass only non-secret
    # environment settings that the terminal fixture actually needs.
    env = {"PATH": os.environ.get("PATH", os.defpath), "LANG": "C.UTF-8",
           "WATCOM": os.environ.get("WATCOM", str(ROOT / "build/openwatcom")),
           "VC_TEST_BINARY": str(binary)}
    with destination.open("w") as log:
        result = subprocess.run([str(ROOT / ".venv/bin/python"), "-m", "pytest", "-q",
                                 f"tests/test_hack_e2e.py::{test}"],
                                cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=100)
    return result.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", choices=tuple(CASES), nargs="*")
    args = parser.parse_args()
    logs = ROOT / "build/hack-gameplay-mutations"
    logs.mkdir(parents=True, exist_ok=True)
    objects = [ROOT / "build/obj" / f"{name}.o" for name in GENERATED]
    if not all(path.is_file() for path in objects):
        parser.error("build the ordinary binary with make first")
    for name in args.cases or CASES:
        source, before, after, test = CASES[name]
        content = (ROOT / "runtime" / f"{source}.c").read_text()
        if content.count(before) != 1:
            raise SystemExit(f"{name}: runtime mutation site changed")
        with tempfile.TemporaryDirectory(prefix="vc-hack-mutant-", dir="/tmp") as directory:
            directory = Path(directory)
            changed = directory / f"{source}.c"
            changed.write_text(content.replace(before, after))
            binary = directory / "vc"
            sources = [changed if stem == source else ROOT / "runtime" / f"{stem}.c"
                       for stem in RUNTIME]
            command = ["cc", "-O1", "-std=gnu11", "-pthread", "-I" + str(ROOT / "runtime"),
                       "-o", str(binary), *map(str, sources), *map(str, objects)]
            with (logs / f"{name}-build.log").open("w") as log:
                subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
            red = run_test(binary, test, logs / f"{name}-red.log")
            if red != 1:
                raise SystemExit(f"{name}: expected an assertion failure (1), got {red}; see {logs}")
        green = run_test(ROOT / "build/vc", test, logs / f"{name}-green.log")
        if green:
            raise SystemExit(f"{name}: restored production gate returned {green}; see {logs}")
        print(f"{name}: planted defect RED (1), restored production GREEN (0)", flush=True)


if __name__ == "__main__":
    main()
