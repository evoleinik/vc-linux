"""Assemble the small, offline H: drive for the WebAssembly build.

Usage: .venv/bin/python tools/web_demo.py DESTINATION GWBASIC.EXE GAMES_DIR BOOTLOGO_IMAGE ROGUE_EXE
The original assembly lives only in asm/; this copies it at build time.
"""
from pathlib import Path
from hashlib import sha256
import re
import sys
import textwrap


ROOT = Path(__file__).resolve().parent.parent
# Brief 16 adds the complete compiled Rogue/PDCurses DOS image and runtime
# notices. The measured drive is now about 494 KB (formerly about 270 KB);
# keep a finite 600 KB packaging budget instead of the pre-Rogue 400 KB cap.
LIMIT = 600_000


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


def main() -> None:
    if len(sys.argv) != 6:
        raise SystemExit(__doc__)
    destination = Path(sys.argv[1])
    files = {
        "README.TXT": (ROOT / "web/README.TXT").read_text(encoding="ascii").encode("ascii"),
        "HISTORY.TXT": history_text((ROOT / "README.md").read_text(encoding="utf-8")),
        "GWBASIC.EXE": Path(sys.argv[2]).read_bytes(),
        "GWBASIC.TXT": (ROOT / "third_party/gwbasic/LICENSE").read_bytes(),
        "BOOTLOGO.COM": Path(sys.argv[4]).read_bytes(),
        "BOOTLOGO.TXT": (ROOT / "web/BOOTLOGO.TXT").read_text(encoding="ascii").replace("\n", "\r\n").encode("ascii"),
        "LOGOLIC.TXT": (ROOT / "third_party/bootlogo/LICENSE").read_bytes(),
        "GAMES/SPIRAL.BAS": (ROOT / "web/GAMES/SPIRAL.BAS").read_text(encoding="ascii").replace("\n", "\r\n").encode("ascii"),
        "GAMES/ROGUE.EXE": Path(sys.argv[5]).read_bytes(),
        "GAMES/ROGUELIC.TXT": (ROOT / "third_party/rogue/LICENSE.TXT").read_bytes(),
        "GAMES/PDCLIC.TXT": (ROOT / "third_party/pdcurses/README.md").read_bytes(),
        "GAMES/OWLIC.TXT": Path(sys.argv[5]).with_name("OWLIC.TXT").read_bytes(),
    }
    for path in sorted(Path(sys.argv[3]).iterdir()):
        files[f"GAMES/{path.name}"] = path.read_bytes()
    for name in ("VC.ASM", "VCOVL.ASM", "LICENSE.TXT"):
        files[f"SRC/{name}"] = (ROOT / "asm" / name).read_bytes()

    total = sum(map(len, files.values()))
    if total >= LIMIT:
        raise SystemExit(f"Web demo is {total:,} bytes; it must be below {LIMIT:,}")
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
    print(f"Web demo: {len(files)} files, {total:,} bytes (limit {LIMIT:,})")


if __name__ == "__main__":
    main()
