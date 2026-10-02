"""Assemble the shared, offline H: drive for the browser and native BBS doors.

Usage: .venv/bin/python tools/web_demo.py DESTINATION GWBASIC.EXE GAMES_DIR BOOTLOGO_IMAGE ROGUE_EXE VZ_IMAGE KERMIT_EXE [MSDOS_DIR [VC405_DIR [HACK_DIR]]]
The original VC sources stay in asm/ and asm405/; this only copies build inputs.
"""
from pathlib import Path
from hashlib import sha256
import re
import sys
import textwrap

from vz_defaults import installed_definition


ROOT = Path(__file__).resolve().parent.parent
# Keep the pre-Kermit programs within their original 600 KB raw-drive budget.
# Later programs add real DOS executables; the whole drive has a finite cap.
# First-load HTTP gzip remains independently gated against main by web_size.
LEGACY_LIMIT = 600_000
LIMIT = 1_500_000
MSDOS_PROGRAMS = ("COMMAND.COM", "EDLIN.COM", "DEBUG.COM", "FIND.EXE", "MORE.COM", "SORT.EXE", "FC.EXE")
HACK_FILES = ("HACK.EXE", "data", "help", "hh", "rumors", "record", "perm", "OWLIC.TXT")


def history_text(readme: str) -> bytes:
    section = re.search(r"^## History\s*\n(.*?)(?=^## |\Z)", readme, re.M | re.S)
    if section is None:
        raise ValueError("README.md must contain a History section")
    text = re.sub(r"(?m)^> ?", "", section[1].strip())
    text = re.sub(r"\[([^]]+)\]\(([^)]+)\)", r"\1 (\2)", text)
    text = re.sub(r"`([^`]+)`", r"\1", text)
    paragraphs = [
        textwrap.fill(
            " ".join(paragraph.split()), width=76,
            break_long_words=False, break_on_hyphens=False,
        )
        for paragraph in text.split("\n\n")
    ]
    return ("History\n\n" + "\n\n".join(paragraphs) + "\n").encode("ascii")


def demo_files(gwbasic: Path, games: Path, bootlogo: Path, rogue: Path,
               vz: Path, kermit: Path, msdos: Path | None = None,
               vc405: Path | None = None, hack: Path | None = None) -> dict[str, bytes]:
    """The sole content list/generator for both demo environments.

    Keep pathnames as UTF-8 host names and DOS text in its original encoding.
    Native doors embed this exact mapping, not a hand-maintained second list.
    """
    # Preserve the shared door generator's six-image interface. Both callers
    # use the same build tree; the browser can also name an explicit DOS tree.
    if msdos is None:
        msdos = kermit.parent.parent / "msdos2"
    if vc405 is None:
        vc405 = kermit.parent.parent / "vc405"
    if hack is None:
        hack = kermit.parent.parent / "hack"
    files = {
        "README.TXT": (ROOT / "web/README.TXT").read_text(encoding="ascii").encode("ascii"),
        # Keep a readable UTF-8 source in git; only Russian has a complete
        # CP866 alphabet. Ukrainian remains on the Unicode browser page.
        "ПРОЧТИ.TXT": (ROOT / "web/README-RU.TXT").read_text(encoding="utf-8").replace("\n", "\r\n").encode("cp866"),
        "HISTORY.TXT": history_text((ROOT / "README.md").read_text(encoding="utf-8")),
        "GWBASIC.EXE": gwbasic.read_bytes(),
        "GWBASIC.TXT": (ROOT / "third_party/gwbasic/LICENSE").read_bytes(),
        "BOOTLOGO.COM": bootlogo.read_bytes(),
        "BOOTLOGO.TXT": (ROOT / "web/BOOTLOGO.TXT").read_text(encoding="ascii").replace("\n", "\r\n").encode("ascii"),
        "LOGOLIC.TXT": (ROOT / "third_party/bootlogo/LICENSE").read_bytes(),
        "VZ.COM": vz.read_bytes(),
        "VZ.DEF": installed_definition(),
        "VZLIC.TXT": (ROOT / "third_party/vzeditor/LICENSE").read_bytes(),
        "KERMIT.EXE": kermit.read_bytes(),
        "BBS.TAK": (ROOT / "data/BBS.TAK").read_bytes(),
        "KERMIT.TXT": (ROOT / "data/KERMIT.TXT").read_bytes(),
        "KERMLIC.TXT": (ROOT / "third_party/mskermit/LICENSE").read_bytes(),
        "DOS/DOS.TXT": (ROOT / "data/DOS.TXT").read_bytes(),
        "DOS/DOSLIC.TXT": (ROOT / "third_party/msdos2/LICENSE").read_bytes(),
        "GAMES/SPIRAL.BAS": (ROOT / "web/GAMES/SPIRAL.BAS").read_text(encoding="ascii").replace("\n", "\r\n").encode("ascii"),
        "GAMES/ROGUE.EXE": rogue.read_bytes(),
        "GAMES/ROGUELIC.TXT": (ROOT / "third_party/rogue/LICENSE.TXT").read_bytes(),
        "GAMES/PDCLIC.TXT": (ROOT / "third_party/pdcurses/README.md").read_bytes(),
        "GAMES/OWLIC.TXT": rogue.with_name("OWLIC.TXT").read_bytes(),
        "VC405/VC.COM": (vc405 / "VC.COM").read_bytes(),
        "VC405/VCSETUP.COM": (vc405 / "VCSETUP.COM").read_bytes(),
        "VC405/LICENSE.TXT": (ROOT / "asm405/LICENSE.TXT").read_bytes(),
        "GAMES/HACK/HACKLIC.TXT": (ROOT / "third_party/hack/COPYRIGHT").read_bytes(),
        "GAMES/HACK/FENLIC.TXT": (ROOT / "third_party/hack/COPYRIGHT-JF").read_bytes(),
    }
    for name in HACK_FILES:
        files[f"GAMES/HACK/{name}"] = (hack / name).read_bytes()
    for name in MSDOS_PROGRAMS:
        files[name if name == "COMMAND.COM" else f"DOS/{name}"] = (msdos / name).read_bytes()
    for name in ("VZFLE.DEF", "HELPE.DEF", "BLOCK.DEF", "PALET.DEF", "BW.DEF"):
        files[name] = (ROOT / "third_party/vzeditor/VZ-IBM" / name).read_bytes()
    for path in sorted(games.iterdir()):
        files[f"GAMES/{path.name}"] = path.read_bytes()
    for name in ("VC.ASM", "VCOVL.ASM", "LICENSE.TXT"):
        files[f"SRC/{name}"] = (ROOT / "asm" / name).read_bytes()

    total = sum(map(len, files.values()))
    later_files = {"KERMIT.EXE", "BBS.TAK", "KERMIT.TXT", "KERMLIC.TXT", "COMMAND.COM"}
    legacy = sum(len(data) for name, data in files.items()
                 if name not in later_files and not name.startswith(("DOS/", "VC405/", "GAMES/HACK/")))
    if legacy >= LEGACY_LIMIT:
        raise SystemExit(f"Pre-Kermit demo is {legacy:,} bytes; it must stay below {LEGACY_LIMIT:,}")
    if total >= LIMIT:
        raise SystemExit(f"Demo is {total:,} bytes; it must be below {LIMIT:,}")
    return files


def main() -> None:
    if len(sys.argv) not in (8, 9, 10, 11):
        raise SystemExit(__doc__)
    destination = Path(sys.argv[1])
    files = demo_files(*(Path(arg) for arg in sys.argv[2:]))
    # Do not accidentally embed stale files from an earlier version of the demo.
    extras = {
        path.relative_to(destination).as_posix()
        for path in destination.rglob("*") if path.is_file() or path.is_symlink()
    } - files.keys()
    # Only exact old generated defaults can be retired. Pin the original
    # CRLF guide's digest so future edits to BOOTLOGO.TXT cannot affect this.
    retired = {
        "GAMES/NOTHING.TXT": sha256(
            b"There are no games here. You are already playing with a file manager.\n").hexdigest(),
        "LOGO.COM": sha256(files["BOOTLOGO.COM"]).hexdigest(),
        "LOGO.TXT": "fe43306998b8f35ba9102c9afe9d75e556c4b94989620e2a92f99227f7e38a10",
    }
    removable = {
        name for name in extras & retired.keys()
        if not (destination / name).is_symlink()
        and sha256((destination / name).read_bytes()).hexdigest() == retired[name]
    }
    unexpected = extras - removable
    if unexpected:
        raise SystemExit(f"Unexpected files in {destination}: {', '.join(sorted(unexpected))}")
    for name in removable:
        (destination / name).unlink()
    for name, contents in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    print(f"Web demo: {len(files)} files, {sum(map(len, files.values())):,} bytes (limit {LIMIT:,})")


if __name__ == "__main__":
    main()
