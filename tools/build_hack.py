#!/usr/bin/env python3
"""Build vendored Hack 1.0.3 reproducibly as an 8086 large-model DOS EXE."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess

from hack_port import REPLACED, prepare

WATCOM_SETUP = ("tools/fetch-openwatcom.sh build/openwatcom && "
                "export WATCOM=$PWD/build/openwatcom")
DATA_FILES = ("data", "help", "hh", "rumors")


def build_identity(image: bytes, map_text: str) -> tuple[int, int]:
    """Locate the one non-relocated stamp slot and hash the first linked EXE."""
    if len(image) < 28 or image[:2] != b"MZ":
        raise ValueError("Hack build identity requires a complete MZ header")
    header = struct.unpack_from("<14H", image)
    matches = re.findall(r"^([0-9a-f]{4}):([0-9a-f]{4})[+* ]+\s+_hack_build_stamp\s*$",
                         map_text, re.M | re.I)
    if len(matches) != 1:
        raise ValueError("Hack build identity requires exactly one stamp symbol")
    segment, offset = (int(value, 16) for value in matches[0])
    header_size = header[4] * 16
    slot = segment * 16 + offset
    at = header_size + slot
    if at < header_size or image[at:at + 4] != b"HSTP":
        raise ValueError("Hack build identity placeholder differs from linked image")
    if header[12] + header[3] * 4 > header_size:
        raise ValueError("Hack build identity requires a bounded MZ relocation table")
    for index in range(header[3]):
        offset, segment = struct.unpack_from("<HH", image, header[12] + 4 * index)
        relocation = segment * 16 + offset
        if slot < relocation + 2 and relocation < slot + 4:
            raise ValueError("Hack build identity slot must not be relocated")
    return at, int.from_bytes(hashlib.sha256(image).digest()[:4], "little")


def build(root: Path, watcom: Path, out: Path, jobs: int) -> None:
    compiler = watcom / "binl64/wcc"
    if not compiler.is_file():
        raise SystemExit(f"OpenWatcom not found in {watcom}. Install it with:\n{WATCOM_SETUP}")
    env = os.environ | {
        "WATCOM": str(watcom), "INCLUDE": str(watcom / "h"),
        "PATH": str(watcom / "binl64") + os.pathsep + os.environ.get("PATH", ""),
        "TZ": "UTC", "SOURCE_DATE_EPOCH": "0",
    }
    vendor = root / "third_party/hack"
    shim = root / "runtime/hack_dos"
    for name in ("src", "obj", "shim"):
        (out / name).mkdir(parents=True, exist_ok=True)
    prepare(vendor, out / "src")

    def run(args):
        result = subprocess.run([str(arg) for arg in args],
                                cwd=out if args[0] == compiler else root, env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise RuntimeError(" ".join(map(str, args)) + "\n" + result.stdout)
        if result.stdout:
            print(result.stdout, end="")
        return result.stdout

    # Run upstream's own object-name generator on the build host. Its output
    # is plain integer #defines, independent of host width and compiler ABI.
    generator = out / "makedefs"
    run([os.environ.get("CC", "cc"), "-std=gnu89", "-O2", vendor / "makedefs.c",
         "-o", generator])
    result = subprocess.run([str(generator), str(vendor / "def.objects.h")],
                            cwd=root, env=env, check=True, capture_output=True)
    (out / "src/hack.onames.h").write_bytes(result.stdout)
    sources = [out / "src" / path.name for path in sorted(vendor.glob("*.c"))
               if path.name not in REPLACED | {"makedefs.c"}]
    for path in sorted(shim.glob("*.c")):
        copy = out / "shim" / path.name
        shutil.copyfile(path, copy)
        sources.append(copy)
    objects = [out / "obj" / (path.stem + ".obj") for path in sources]
    common = [compiler, "-zq", "-bt=dos", "-0", "-ml", "-os", "-fpc", "-j",
              "-zt=1024", "-zld", f"-i={shim}", f"-i={out / 'src'}",
              f"-fi={shim / 'compat.h'}"]
    # Hack's assert() embeds __FILE__. Stable relative inputs prevent output
    # directory names from changing the DOS image (SOURCE_DATE_EPOCH cannot).
    commands = [common + [f"-fo={obj}", f"-fr={obj.with_suffix('.err')}", source.relative_to(out)]
                for source, obj in zip(sources, objects)]
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        list(pool.map(run, commands))
    linkfile = out / "hack.lnk"
    lines = ["system dos", "option quiet", "option dosseg", "option nofarcalls",
             "option stack=16384", f"option map={out / 'HACK.MAP'}", "option verbose",
             f"name {out / 'HACK.EXE'}"]
    lines += [f"file {path}" for path in objects]
    linkfile.write_text("\n".join(lines) + "\n")
    link_command = [watcom / "binl64/wlink", "@" + str(linkfile)]
    run(link_command)
    # Identity is a digest of executable bytes, independent of build paths,
    # clocks, and installation mtimes. Recompile the isolated data module and
    # relink rather than patching EXE bytes: all initialized data still has
    # the original OMF evidence required by the translator's independent gate.
    executable = out / "HACK.EXE"
    first_link = executable.read_bytes()
    at, stamp = build_identity(first_link, (out / "HACK.MAP").read_text())
    command = commands[sources.index(out / "shim/build_stamp.c")]
    run(command[:-1] + [f"-dHACK_BUILD_STAMP=0x{stamp:08x}UL"] + command[-1:])
    run(link_command)
    expected = first_link[:at] + struct.pack("<I", stamp) + first_link[at + 4:]
    if executable.read_bytes() != expected:
        raise ValueError("Hack identity relink changed bytes outside its four-byte data slot")
    for name in DATA_FILES + ("COPYRIGHT", "COPYRIGHT-JF"):
        shutil.copyfile(vendor / name, out / name)
    for name in ("record", "perm"):
        # Build outputs may also be a developer's playground: never clobber scores.
        (out / name).touch(exist_ok=True)
    notice = (watcom / "license.txt").read_bytes()
    notice += (b"\r\nOpenWatcom runtime source:\r\n"
               b"https://github.com/open-watcom/open-watcom-v2/tree/master/bld/clib\r\n"
               b"Hack 1.0.3 BSD-3-Clause sources: third_party/hack.\r\n")
    # The declarations contributed by NetBSD retain their separate BSD-2
    # notice in addition to Hack's accompanying CWI and Fenlason licences.
    notice += b"\r\nNetBSD Hack declaration-header notice:\r\n"
    notice += (vendor / "extern.h").read_bytes().split(b"#ifndef _EXTERN_H_", 1)[0]
    (out / "OWLIC.TXT").write_bytes(notice)
    print(f"Hack DOS: {(out / 'HACK.EXE').stat().st_size} bytes")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--watcom", default=os.environ.get("WATCOM"))
    parser.add_argument("--out", default="build/hack")
    parser.add_argument("-j", "--jobs", type=int, default=min(8, os.cpu_count() or 1))
    args = parser.parse_args()
    if not args.watcom:
        parser.error("set WATCOM (the pinned toolchain is intentionally not in git). "
                     f"Install it with:\n{WATCOM_SETUP}")
    root = Path(__file__).resolve().parents[1]
    build(root, Path(args.watcom).resolve(), (root / args.out).resolve(), args.jobs)


if __name__ == "__main__":
    main()
