"""Run build/vc on a directory, press some keys, and write the screen as HTML.

    .venv/bin/python tools/snapshot.py OUT.html [DIR] [KEY ...]

KEY is a name from tests/e2e/vcterm.py KEYS (f3, tab, down...) or literal text.
Colours come from vc's own 24-bit output, so the page shows the real VGA palette.
"""
import html
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "e2e"))
from vcterm import KEYS, VcSession  # noqa: E402


def color(c: str, default: str) -> str:
    if c == "default":
        return default
    return f"#{c}" if len(c) == 6 and all(ch in "0123456789abcdefABCDEF" for ch in c) else c


def main() -> None:
    out = Path(sys.argv[1])
    where = Path(sys.argv[2]) if len(sys.argv) > 2 else Path.cwd()
    keys = sys.argv[3:]
    os.environ["COLORTERM"] = "truecolor"
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as home:
        # settings in a throwaway dir, but the real $HOME, so H: is your home
        s = VcSession(where, Path(home), extra_env={"HOME": os.environ.get("HOME", home)})
        try:
            s.wait_for("10Quit", timeout=15)
            for k in keys:
                s.send(k if k in KEYS else k.encode())
                s.pump(0.6)
            s.pump(1.0)
            rows = []
            for y in range(s.screen.lines):
                line = s.screen.buffer[y]
                cells = []
                for x in range(s.screen.columns):
                    ch = line[x]
                    fg, bg = color(ch.fg, "#aaaaaa"), color(ch.bg, "#000000")
                    if ch.reverse:
                        fg, bg = bg, fg
                    cells.append(f'<span style="color:{fg};background:{bg}">{html.escape(ch.data or " ")}</span>')
                rows.append("".join(cells))
        finally:
            s.close()
    out.write_text(
        "<!doctype html><meta charset=utf-8><title>Volkov Commander on Linux</title>"
        "<style>body{background:#111;margin:24px}pre{font:16px/1.15 'DejaVu Sans Mono',Menlo,monospace;"
        "display:inline-block;padding:8px;background:#000}</style>"
        "<pre>" + "\n".join(rows) + "</pre>\n", encoding="utf-8")
    print(out)


if __name__ == "__main__":
    main()
