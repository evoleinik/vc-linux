# Brief 14: CGA graphics and bootLogo

Read `docs/plans/2026-10-01-games.md` (the "Turtle graphics" section, decided as option G) and
`CLAUDE.md` first. Follow the shape of brief 11 (`docs/briefs/11-gwbasic.md`): real DOS files,
EXEC by bytes, both builds.

Goal: a user types `logo` on VC's command line, or presses Enter on `LOGO.COM`, and gets
bootLogo, a real Logo with turtle graphics. `REPEAT 36 [FD 60 RT 170]` draws a star. `QUIT`
returns to VC. The same graphics screen also makes GW-BASIC's `SCREEN 1`, `DRAW`, `LINE`,
`CIRCLE` and `PSET` work. Both the Linux build and the browser build.

You have no network. Everything is on disk.

## What is already here

- `third_party/bootlogo/`: bootLogo by Oscar Toledo G., BSD-2, NASM source. Its README gives the
  commands. Build the COM file with `-Dcom_file=1`. By default it uses CGA mode 4 (320×200, four
  colours) and draws through BIOS INT 10h.
- `tools/nasm/nasm`: NASM 3.02. `nasm -f bin -l LISTING` writes a listing.
- `third_party/font8x8/`: Daniel Hepper's public-domain 8×8 fonts, for text on graphics screens.
- The Emscripten toolchain: see `docs/briefs/10-browser.md`. `build/emcache` is warm.

## 1. Translate a NASM program

The translator reads JWasm listings. Add a reader for NASM listings, so `make bootlogo` builds
`LOGO.COM` and its listing and the translator emits its image. VC's and GW-BASIC's generated C
must stay byte-identical; prove it with `cmp`. `make test-translator` covers every distinct
bootLogo instruction against unicorn, as for VC.

## 2. CGA graphics in the BIOS layer

Implement what a real CGA BIOS does, in `runtime/bios.c`:

- INT 10h AH=00h modes 4, 5 (320×200, four colours) and 6 (640×200, two colours), and back to
  text mode 3. Video memory at B800h is interlaced like real CGA: even rows at offset 0, odd rows
  at 2000h. Programs that write video memory directly, as GW-BASIC does, must see the same layout.
- AH=0Bh palette and background, AH=0Ch write pixel (bit 7 of AL means XOR), AH=0Dh read pixel,
  AH=0Fh get mode. Text output in graphics modes (AH=02h, 03h, 09h, 0Ah, 0Eh) draws 8×8 glyphs
  from `third_party/font8x8`. Map code page 437 or 866 box and block characters through
  `font8x8_box.h` and `font8x8_block.h` where a glyph exists; leave the rest blank.
- Port 3D9h, the colour select register, since some programs set the palette there.

## 3. Show the graphics screen

- **Browser:** while a graphics mode is on, a canvas replaces the xterm.js screen. It shows the
  same 640×400 area, scaled crisply (`image-rendering: pixelated`), with real CGA colours. Text
  mode brings xterm.js back. Keys keep working the same way.
- **Linux terminal:** draw the screen with Unicode braille, 2×4 dots per cell, so 80×25 cells
  show 160×100 dots. Scale 320×200 by two each way, and 640×200 by four across and two down. A dot
  is on if any source pixel in it is not the background. Colour each cell by its most common
  non-background colour, in truecolor when `COLORTERM` allows and 256 colours otherwise.
- `VC_SCREEN_DUMP` in a graphics mode writes the braille text. Add `VC_FRAME_DUMP=file`, which
  writes the exact pixels as a PGM image, so tests can check pixels.

## 4. Running it

- `LOGO.COM` is installed as a real file next to `GWBASIC.EXE`: the config directory on Linux,
  `H:\` in the browser. The DOS `PATH` already covers both, so `logo` works on the command line.
  Matching stays by bytes, as in brief 11.
- When a program exits while a graphics mode is on, VC must come back in text mode with its panels
  redrawn. Find out what VC itself does after EXEC on real DOS before adding anything.
- Browser `H:\`: add `LOGO.TXT` with bootLogo's commands and three programs to type, including
  the flower from its README. Add `GAMES\SPIRAL.BAS`, a short GW-BASIC program of our own that
  uses `SCREEN 1` and `DRAW` to draw a spiral. Update `web/README.TXT`.

## Gates

- `make test` green. VC's and GW-BASIC's generated C are byte-identical to before.
- Unit tests for every new INT 10h function and the interlaced memory layout. A pixel written by
  AH=0Ch must land at the address real CGA uses, and AH=0Dh must read it back.
- E2e tests in a pty:
  1. `logo` starts bootLogo.
  2. `REPEAT 4 [FD 50 RT 90]` draws a square. Check the frame dump for the four corners at the
     pixels bootLogo's own arithmetic puts them.
  3. `QUIT` returns to VC's panels in text mode.
  4. In GW-BASIC, `SCREEN 1: LINE (10,10)-(100,10): CIRCLE (160,100),40` sets the expected
     pixels, and `SCREEN 0` returns to text.
  5. The braille dump shows the drawing.
- `make web` and `make test-web` green. The node smoke test runs `logo`, draws, checks the canvas
  hook received a frame with the expected pixels, then quits back to VC.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. Your final summary lists files changed, the wasm gzip size and its growth. It
names three weaknesses of your own work, each with a real `file:line`. Do not commit; I commit.
