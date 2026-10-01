"""Assemble the small, offline H: drive for the WebAssembly build.

Usage: .venv/bin/python tools/web_demo.py DESTINATION GWBASIC.EXE GAMES_DIR
The original assembly lives only in asm/; this copies it at build time.
"""
from pathlib import Path
import re
import sys
import textwrap


ROOT = Path(__file__).resolve().parent.parent
LIMIT = 400_000  # Decimal KB is stricter than the brief's 400 KB ceiling.


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
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    destination = Path(sys.argv[1])
    files = {
        "README.TXT": (ROOT / "web/README.TXT").read_text(encoding="ascii").encode("ascii"),
        "HISTORY.TXT": history_text((ROOT / "README.md").read_text(encoding="utf-8")),
        "GWBASIC.EXE": Path(sys.argv[2]).read_bytes(),
        "GWBASIC.TXT": (ROOT / "third_party/gwbasic/LICENSE").read_bytes(),
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
        for path in destination.rglob("*") if path.is_file()
    } - files.keys()
    # Retire only the exact generated joke from Brief 10. Never remove a
    # user-added file merely because it is absent from the new manifest.
    old_game = destination / "GAMES/NOTHING.TXT"
    if "GAMES/NOTHING.TXT" in extras and old_game.read_bytes() == (
            b"There are no games here. You are already playing with a file manager.\n"):
        old_game.unlink()
        extras.remove("GAMES/NOTHING.TXT")
    if extras:
        raise SystemExit(f"Unexpected files in {destination}: {', '.join(sorted(extras))}")
    for name, contents in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(contents)
    print(f"Web demo: {len(files)} files, {total:,} bytes (limit {LIMIT:,})")


if __name__ == "__main__":
    main()
