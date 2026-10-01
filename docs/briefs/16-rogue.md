# Brief 16: Rogue, translated from compiled C

Read `docs/plans/2026-10-01-games.md` (the ranking section, item 6, and the "command.com and
rogue" note) and `CLAUDE.md` first. Follow the shape of briefs 11 and 14: real DOS files, EXEC
by bytes, both builds.

Goal: the original Rogue 5.4.4 (1980-1985, Toy, Arnold and Wichman, BSD-3) runs as a translated
16-bit DOS program. The user types `rogue` on VC's command line, or presses Enter on `ROGUE.EXE`,
plays, and `Q` then `y` returns to VC. Both the Linux build and the browser build.

This is the first program built from C, so it also builds a reusable path for compiled C. Maximus
BBS and Xenia, both C, come next on that path.

You have no network. Everything is on disk.

## What is already here

- `third_party/rogue/`: Rogue 5.4.4, BSD-3 (commit in `UPSTREAM`).
- `third_party/pdcurses/`: PDCurses, public domain, with a DOS port in `dos/` that supports
  Watcom.
- OpenWatcom 2.0 at `$HOME/src/vc-linux-wt/tools-cache/openwatcom` (set `WATCOM` to it, put
  `binl64` on `PATH`, `INCLUDE=$WATCOM/h`). It has `wcc`, `wlink`, `wlib` and `wdis`.
- Measured on 2026-10-01: `wcc -bt=dos -ml -zq` compiles 31 of Rogue's 33 C files as they are.
  `mdport.c` fails on `struct passwd` fields; one other file fails too.

## 1. Build ROGUE.EXE for 16-bit DOS

`make rogue` builds PDCurses for DOS and Rogue in the large model, and links `build/rogue/ROGUE.EXE`.
Never edit vendored sources; put DOS shims in our own files. No password database (a fixed player
name), no shell escape, no-op signals, and save and score files in the current directory. The build
must be reproducible: the same bytes every time.

The toolchain is 524 MB and stays out of git. Locally the Makefile finds it through `WATCOM`. CI
must download one pinned OpenWatcom build, never a moving "current" tag. Write the exact URL and
its SHA-256 in the Makefile or workflow.

## 2. Translate compiled code

The translator needs instruction boundaries. Prefer reusing the listing path that already exists.
Disassemble every object that ends up in the EXE, C library members included, into source JWasm
can assemble. Then assemble it with listings, link it with JWlink, and prove the re-linked EXE is
byte-identical to the one OpenWatcom linked. If that cannot be made exact, explain why and build a
map-driven front end instead. Either way:

- `make test-translator` covers every distinct Rogue instruction against unicorn.
- VC's, GW-BASIC's and bootLogo's generated C stay byte-identical. Prove it with `cmp`.
- Calls through function pointers, such as Rogue's command and daemon tables, must reach
  translated code. A missed target must fail loudly in a test, never at play time.

## 3. Run it

- Implement whatever BIOS and DOS services Rogue and PDCurses need, found by running them.
- Install `ROGUE.EXE` as a real file next to `GWBASIC.EXE`. Matching stays by bytes, so `rogue`
  works on the command line in both builds. In the browser it sits in `H:\GAMES`.
- Update `web/README.TXT` with how to start Rogue and its basic keys.

## Gates

- `make test` green, with byte-identical generated C for the earlier programs.
- `make rogue` reproduces the same EXE twice in a row.
- E2e tests in a pty:
  1. `rogue` shows the dungeon and the status line (`Level: 1`, `Gold:`, `Hp:`).
  2. Movement keys move `@`.
  3. `Q` then `y` ends the game and VC's panels come back.
  4. A save with `S`, then `rogue` again, restores the game.
- `make web` and `make test-web` green. The node smoke test starts Rogue, moves, and quits.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. Your final summary lists files changed, the wasm gzip size and its growth, and
which translation path you chose and why. It names three weaknesses of your own work, each with a
real `file:line`. Do not commit; I commit.
