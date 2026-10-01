"""The tested Ahl listings shipped on the browser's DOS drive.

File names and line endings change; Star Trek also needs keyword-spacing
corrections for GW-BASIC's tokenizer. No game logic changes. Each entry has a
real-GW-BASIC, pseudo-terminal first-prompt test in test_gwbasic_e2e.py.
"""
from dataclasses import dataclass
from pathlib import Path
import re
import sys


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party/basic-computer-games"


@dataclass(frozen=True)
class Game:
    name: str
    source: str
    prompt: str
    title: str


GAMES = (
    Game("STARTREK.BAS", "84_Super_Star_Trek/superstartrek.bas", "COMMAND", "Super Star Trek"),
    Game("HAMURABI.BAS", "43_Hammurabi/hammurabi.bas", "HOW MANY ACRES DO YOU WISH TO BUY", "Hamurabi"),
    Game("LUNARLEM.BAS", "59_Lunar_LEM_Rocket/lem.bas", "(YES OR NO)", "Lunar LEM"),
    Game("ANIMAL.BAS", "03_Animal/animal.bas", "ARE YOU THINKING OF AN ANIMAL", "Animal"),
    Game("BAGELS.BAS", "05_Bagels/bagels.bas", "WOULD YOU LIKE THE RULES (YES OR NO)", "Bagels"),
    Game("HANGMAN.BAS", "44_Hangman/hangman.bas", "WHAT IS YOUR GUESS", "Hangman"),
    Game("AMAZING.BAS", "02_Amazing/amazing.bas", "WHAT ARE YOUR WIDTH AND LENGTH", "Amazing"),
    Game("BLACKJAC.BAS", "10_Blackjack/blackjack.bas", "DO YOU WANT INSTRUCTIONS", "Black Jack"),
    Game("HURKLE.BAS", "51_Hurkle/hurkle.bas", "GUESS #", "Hurkle"),
    Game("LIFE.BAS", "55_Life/life.bas", "ENTER YOUR PATTERN:", "Life"),
    Game("TICTACTO.BAS", "89_Tic-Tac-Toe/tictactoe1.bas", "YOUR MOVE", "Tic Tac Toe"),
    Game("ACEYDUCY.BAS", "01_Acey_Ducey/aceyducey.bas", "WHAT IS YOUR BET", "Acey Ducey"),
)


def dos_text(text: str) -> bytes:
    """Normalize existing CRLF or LF input, retaining its source text."""
    return (text.replace("\r\n", "\n").replace("\r", "\n").rstrip("\n")
            + "\n").replace("\n", "\r\n").encode("ascii")


def game_files() -> dict[str, bytes]:
    files = {}
    for game in GAMES:
        text = (SOURCE / game.source).read_text(encoding="ascii")
        if game.name == "STARTREK.BAS":
            # The 8K BASIC listing's TOS1 / STEP3 are identifiers here. The
            # original fails first at 6430, then 6820, before COMMAND. Add
            # only whitespace, outside strings, including the same compact
            # FOR syntax later in the game. No tokens or game logic change.
            parts = text.split('"')
            corrections = 0
            for index in range(0, len(parts), 2):
                parts[index], count = re.subn(r"(?<=[0-9])(TO|STEP)(?=[A-Z0-9])", r" \1 ", parts[index])
                corrections += count
            if corrections != 10:
                raise ValueError("Star Trek's compact FOR syntax changed; review the compatibility correction")
            text = '"'.join(parts)
        files[game.name] = dos_text(text)
    files["LICENSE.TXT"] = dos_text((SOURCE / "LICENSE").read_text(encoding="ascii"))
    files["README.TXT"] = dos_text(
        "BASIC Computer Games (David H. Ahl, 1978)\n\n"
        "Select a .BAS file in VC and press Enter to play.\n"
        "Use UPPER CASE for answers such as YES and NO.\n"
        "Ctrl+Pause or Ctrl+Shift+B stops a game. Then type SYSTEM to return\n"
        "to VC. Type RUN to start the same game again.\n\n"
        + "\n".join(f"{game.name:12} {game.title}" for game in GAMES)
        + "\n\nThese original listings are public domain. See LICENSE.TXT.\n"
        "They run in the 1983 GW-BASIC interpreter, not in a rewritten game.\n"
        "Star Trek has spaces around compact TO/STEP keywords for GW-BASIC.\n"
        "Every shipped game is tested through its first input prompt. Later\n"
        "gameplay retains the original listings' quirks.\n"
    )
    return files


def main() -> None:
    if sys.argv[1:] == ["--names"]:
        print(" ".join([game.name for game in GAMES] + ["LICENSE.TXT", "README.TXT"]))
        return
    if len(sys.argv) != 2:
        raise SystemExit("usage: basic_games.py DESTINATION | --names")
    destination = Path(sys.argv[1])
    files = game_files()
    extras = {p.name for p in destination.iterdir()} - files.keys() if destination.exists() else set()
    if extras:
        raise SystemExit(f"Unexpected files in {destination}: {', '.join(sorted(extras))}")
    destination.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (destination / name).write_bytes(data)
    print(f"BASIC games: {len(GAMES)} listings, {sum(map(len, files.values())):,} bytes")


if __name__ == "__main__":
    main()
