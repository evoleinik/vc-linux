"""Make docs/social-preview.png (1200x630) from docs/screenshot.png.

    uv run --with pillow python tools/social_preview.py
"""
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]
MONO = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
MONO_BOLD = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
BG, FG, DIM, ACCENT = (11, 14, 26), (235, 238, 245), (150, 160, 180), (85, 255, 255)


def main() -> None:
    shot = Image.open(ROOT / "docs" / "screenshot.png").convert("RGB")
    # crop the page margin: keep what is not the page background
    bg = shot.getpixel((2, 2))
    mask = Image.eval(shot.convert("L"), lambda v: 255 if abs(v - sum(bg) // 3) > 6 else 0)
    shot = shot.crop(mask.getbbox())
    card = Image.new("RGB", (1200, 630), BG)
    d = ImageDraw.Draw(card)
    d.text((60, 70), "vc-linux", font=ImageFont.truetype(MONO_BOLD, 76), fill=ACCENT)
    d.text((62, 170), "Volkov Commander,\nnative on Linux.", font=ImageFont.truetype(MONO_BOLD, 34), fill=FG, spacing=10)
    small = ImageFont.truetype(MONO, 22)
    for i, line in enumerate(["8086 assembly → C,", "translated by machine.", "", "DOS and BIOS → Linux.", "No emulator."]):
        d.text((62, 300 + i * 34), line, font=small, fill=DIM)
    w = 640
    shot = shot.resize((w, round(shot.height * w / shot.width)), Image.LANCZOS)
    card.paste(shot, (1200 - w - 40, (630 - shot.height) // 2))
    out = ROOT / "docs" / "social-preview.png"
    card.save(out, optimize=True)
    print(out, card.size)


if __name__ == "__main__":
    main()
