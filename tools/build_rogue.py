#!/usr/bin/env python3
"""Build the original Rogue and PDCurses as a reproducible 8086 DOS EXE.

Object files and a verbose OpenWatcom map are retained for the compiled-code
front end.  No build products or patches are written into third_party.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import subprocess

from rogue_port import prepare


WATCOM_SETUP = ("tools/fetch-openwatcom.sh build/openwatcom && "
                "export WATCOM=$PWD/build/openwatcom")


def build(root: Path, watcom: Path, out: Path, jobs: int) -> None:
    compiler = watcom / "binl64/wcc"
    if not compiler.is_file():
        raise SystemExit(f"OpenWatcom not found in {watcom}. Install it with:\n{WATCOM_SETUP}")
    env = os.environ | {
        "WATCOM": str(watcom), "INCLUDE": str(watcom / "h"),
        "PATH": str(watcom / "binl64") + os.pathsep + os.environ.get("PATH", ""),
        "TZ": "UTC", "SOURCE_DATE_EPOCH": "0",
    }
    for name in ("src", "obj", "pdc", "pdcsrc"):
        (out / name).mkdir(parents=True, exist_ok=True)
    prepare(root / "third_party/rogue", out / "src")
    shim = root / "runtime/rogue_dos"
    curses = root / "third_party/pdcurses"

    def run(args):
        result = subprocess.run([str(x) for x in args], cwd=root, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise RuntimeError(" ".join(map(str, args)) + "\n" + result.stdout)
        if result.stdout:
            print(result.stdout, end="")

    common = [compiler, "-zq", "-bt=dos", "-0", "-ml", "-os", "-fpc",
              "-j", "-zt=1024", "-zld", f"-i={curses}"]
    pdcsources = sorted((curses / "pdcurses").glob("*.c")) + sorted((curses / "dos").glob("*.c"))
    # The vendor subset omits the shared CP437 table; use its public-domain
    # copy in our shim directory without writing a file into third_party.
    display = curses / "dos/pdcdisp.c"
    content = display.read_text()
    old_include = '#include "../common/acs437.h"'
    if content.count(old_include) != 1:
        raise ValueError("PDCurses CP437 include changed")
    adapted_display = out / "pdcsrc/pdcdisp.c"
    adapted_display.write_text(content.replace(old_include, '#include "acs437.h"'))
    pdcsources = [adapted_display if path == display else path for path in pdcsources]
    pdcobjects = [out / "pdc" / (path.stem + ".obj") for path in pdcsources]
    # mdport is replaced by the DOS shim. xcrypt belongs only to the disabled
    # MASTER/password build and is deliberately not linked.
    sources = [out / "src" / path.name for path in sorted((root / "third_party/rogue").glob("*.c"))
               if path.stem not in ("mdport", "xcrypt")]
    sources += [shim / "mdport.c", shim / "startup.c", shim / "save_io.c"]
    objects = [out / "obj" / (path.stem + ".obj") for path in sources]
    commands = [common + [f"-i={curses / 'dos'}", f"-i={shim}", f"-fo={obj}",
                          f"-fr={obj.with_suffix('.err')}", path]
                for path, obj in zip(pdcsources, pdcobjects)]
    commands += [common + ["-dHAVE_CONFIG_H", f"-i={shim}", f"-i={out / 'src'}", f"-fo={obj}",
                           f"-fr={obj.with_suffix('.err')}", path]
                 for path, obj in zip(sources, objects)]
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(run, commands))
    initscr = out / "pdc" / "initscr.obj"
    initscr.write_bytes(pin_object_date(initscr.read_bytes()))
    library = out / "pdcurses.lib"
    run([watcom / "binl64/wlib", "-q", "-n", "-b", library] + ["+" + str(path) for path in pdcobjects])
    linkfile = out / "rogue.lnk"
    lines = ["system dos", "option quiet", "option dosseg", "option nofarcalls",
             "option stack=16384", f"option map={out / 'ROGUE.MAP'}", "option verbose",
             f"name {out / 'ROGUE.EXE'}"]
    lines += [f"file {path}" for path in objects]
    lines += [f"library {library}"]
    linkfile.write_text("\n".join(lines) + "\n")
    run([watcom / "binl64/wlink", "@" + str(linkfile)])
    image = (out / "ROGUE.EXE").read_bytes()
    if pin_build_date(image) != image:
        raise SystemExit("ROGUE.EXE: PDCurses date stamp was not pinned before linking")
    # The license and source notice accompany both redistributed executables.
    notice = (watcom / "license.txt").read_bytes()
    notice += (b"\r\nOpenWatcom runtime source:\r\n"
               b"https://github.com/open-watcom/open-watcom-v2/tree/master/bld/clib\r\n"
               b"Rogue is compiled from the BSD-3-Clause sources in third_party/rogue.\r\n")
    (out / "OWLIC.TXT").write_bytes(notice)
    print(f"Rogue DOS: {(out / 'ROGUE.EXE').stat().st_size} bytes")


# PDCurses stamps __DATE__ into its notice (initscr.c), and OpenWatcom ignores
# SOURCE_DATE_EPOCH and refuses to redefine __DATE__. Unpinned, ROGUE.EXE
# changed every day; CI went red at midnight UTC on 2026-10-02. Pin the stamp
# in the object file, before linking, to the day this build was verified, so
# the linked image, the OMF data the translator checks it against, and the
# pinned hashes all agree.
PDC_NOTICE = b"PDCurses 3.9 - "
PDC_DATE = b"Oct  1 2026"


def pin_build_date(image: bytes) -> bytes:
    at = image.find(PDC_NOTICE)
    if at < 0 or image.find(PDC_NOTICE, at + 1) >= 0:
        raise SystemExit("ROGUE.EXE: expected exactly one PDCurses date stamp")
    start = at + len(PDC_NOTICE)
    stamp = image[start:start + len(PDC_DATE)]
    if not re.fullmatch(rb"[A-Z][a-z]{2} [ 123][0-9] [0-9]{4}", stamp):
        raise SystemExit(f"ROGUE.EXE: unexpected PDCurses date stamp {stamp!r}")
    return image[:start] + PDC_DATE + image[start + len(PDC_DATE):]


def pin_object_date(obj: bytes) -> bytes:
    """Pin the stamp inside an OMF object and repair its record checksum."""
    pinned = bytearray(pin_build_date(obj))
    at = pinned.find(PDC_NOTICE)
    offset = 0
    while offset < len(pinned):
        length = int.from_bytes(pinned[offset + 1:offset + 3], "little")
        end = offset + 3 + length
        if offset <= at < end:
            if at + len(PDC_NOTICE) + len(PDC_DATE) > end - 1:
                raise SystemExit("initscr.obj: date stamp crosses an OMF record")
            if pinned[end - 1]:  # zero means "no checksum" in OMF
                pinned[end - 1] = -sum(pinned[offset:end - 1]) & 0xFF
            return bytes(pinned)
        offset = end
    raise SystemExit("initscr.obj: date stamp is outside every OMF record")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watcom", default=os.environ.get("WATCOM"))
    parser.add_argument("--out", default="build/rogue")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    if not args.watcom:
        parser.error("set WATCOM (the 524 MB toolchain is intentionally not in git). "
                     f"Install it with:\n{WATCOM_SETUP}")
    root = Path(__file__).resolve().parents[1]
    build(root, Path(args.watcom).resolve(), (root / args.out).resolve(), args.jobs)


if __name__ == "__main__":
    main()
