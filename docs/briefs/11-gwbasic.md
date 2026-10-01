# Brief 11: GW-BASIC and Ahl's games, started by VC itself

Read `docs/plans/2026-10-01-games.md` (option F, decided) and `CLAUDE.md` first.

Goal: `GWBASIC.EXE` is a real file on the drive, and VC starts it the way it would on DOS. The
user types `gwbasic` on VC's command line, or presses Enter on `STARTREK.BAS`. DOS EXEC loads the
file, and the CPU runs GW-BASIC's own machine code, translated ahead of time like VC's. No
emulator, and no special-case "gwbasic" command. This works in both the Linux build and the
browser build.

You have no network. Everything is on disk.

## What is already here

- `third_party/gwbasic/`: TK Chia's fork of Microsoft's 1983 GW-BASIC source (MIT). It assembles
  with JWasm after `jwasmify.awk` preprocessing. `MATH.ASM` is `MATH1.ASM` and `MATH2.ASM`
  concatenated. Its upstream Makefile is not vendored; `README.md` describes the build. The
  module list, in link order, is in `GWBASIC.LNK`.
- `tools/jwlink/jwlink`: the linker. This line, run on the fork, built a 51,296-byte
  `GWBASIC.EXE` and `GWBASIC.MAP` in 16 s with our `tools/jwasm/jwasm`:
  `jwlink format dos file A.OBJ file B.OBJ ... name GWBASIC.EXE option dosseg,map=GWBASIC.MAP`.
  JWasm assembled each module with `-Zm -fpc`.
- `third_party/basic-computer-games/`: the original `.bas` listings of David Ahl's games, public
  domain.
- Emscripten, for the browser build: see `docs/briefs/10-browser.md` § Toolchain.
  `build/emcache` is warm.

## 1. Build GWBASIC.EXE with listings

`make gwbasic` assembles every module with a listing (`-Fl`) and links `build/gwbasic/GWBASIC.EXE`
and its map. The build must be reproducible: the same bytes every time. Use the fork's version
string from `third_party/gwbasic/UPSTREAM`, never today's date.

## 2. Translate a linked, multi-module EXE

The translator today reads one listing per image. GW-BASIC has 39 modules. Each module's listing
gives offsets inside that module's piece of each segment, and the map says where each piece
landed. Teach the translator to take the map plus every module listing, and place each listed
instruction at its linked address. Instruction bytes still come from the linked EXE.

- The single-listing path must not change. `build/gen/vc_com.c` and `vc_ovl.c` must come out
  byte-identical to today's. Prove it with `cmp`.
- `make test-translator` must also cover every distinct GW-BASIC instruction against unicorn, as
  it does for VC.
- If GW-BASIC writes code at run time, or jumps into bytes its listings call data, find out and
  handle it the way `CLAUDE.md` describes for VC. Say what you found.

## 3. EXEC picks the translation by the file's bytes

Today `known_image()` in `runtime/dos_core.c` matches VC.COM and VC.OVL by name and never reads
the file. Change EXEC so that it reads the program file from the drive, like DOS. It runs the
translation whose image bytes equal that file's bytes. The name must not matter.

- A copy of GWBASIC.EXE renamed to `BASIC.EXE` still runs.
- A GWBASIC.EXE with one byte changed is refused cleanly with a DOS error, and VC keeps running.
- VC.COM and VC.OVL keep working. If that needs them on disk, install them as real files the way
  `runtime/main.c` installs VC.INI.

## 4. VC's command line finds programs like COMMAND.COM

`run_dos_command` handles `cd`, `vc-edit`, then the host shell. Before the host shell, add what
COMMAND.COM did. If the first word names a `.COM` or `.EXE` in the current DOS directory or on the
DOS `PATH`, with or without its extension, EXEC it with the rest of the line as its tail.
Anything else goes on as today: to `/bin/sh` on Linux, or to the no-shell message in the browser.

- `GWBASIC.EXE` is installed as a real file, and its directory is on the DOS `PATH`. On Linux,
  that is the config directory. In the browser, it is `H:\`.
- `data/VC.EXT` gains `bas: gwbasic !.!`, so Enter on a `.BAS` file runs it. Existing users
  have today's empty VC.EXT, so add it to `retired[]` in `runtime/main.c` to replace it. Keep the F4
  injection rule from `CLAUDE.md`: a file name must never reach `/bin/sh` as text. Add an e2e test
  with a `.bas` file whose name contains `;touch PWNED`. Enter on it must not create `PWNED`, and
  that must hold even when GWBASIC.EXE has been deleted.
- `SYSTEM` in GW-BASIC returns to VC with the panels redrawn.

## 5. What GW-BASIC needs from the machine

Find out by running it, and implement what it needs, natively and in the browser. Likely:

- **Timer interrupt.** Deliver INT 8, which chains to INT 1Ch, 18.2 times a second. Do it at a
  dispatch boundary in `rt_run` when IF=1, and never from inside translated code. GW-BASIC hooks
  INT 1Ch for `ON TIMER`, `PLAY` and its event polling.
- **Ctrl-Break.** INT 1Bh, so `10 GOTO 10` then Ctrl-Break stops with `Break in 10`. In the
  browser, Ctrl+Pause or Ctrl+Shift+B, which GW-BASIC's own README offers.
- **INT 15h and INT 17h.** Correct "not supported" answers are fine.
- **PC speaker.** Ports 42h, 43h and 61h. In the browser, drive a Web Audio square wave through a
  JavaScript hook, starting at the first key press because browsers require it. In the Linux
  build, stay silent. `BEEP`, `SOUND 440,18` and `PLAY "CDE"` must reach the hook with the right
  frequencies.

## 6. The games

Convert the listings to DOS text: CRLF line ends, 8.3 upper-case names, in `GAMES\`. For example
`STARTREK.BAS`, `HAMURABI.BAS`, `LUNARLEM.BAS`, `ANIMAL.BAS`, `BAGELS.BAS`, `HANGMAN.BAS`,
`AMAZING.BAS`, `BLACKJAC.BAS`, `HURKLE.BAS`, `LIFE.BAS`, `TICTACTO.BAS` and `ACEYDUCY.BAS`. Keep a
game only if a test proves it runs in our GW-BASIC to its first prompt with no error message.

- Browser: `H:\GWBASIC.EXE` and `H:\GAMES\`, replacing `NOTHING.TXT`. Update `web/README.TXT`.
  The whole embedded drive stays under 400 KB.
- Linux: install `GWBASIC.EXE` only. Users bring their own `.bas` files.

## Gates

- `make test` green. VC's generated C is byte-identical to before.
- New e2e tests, in a pty:
  1. `gwbasic` on VC's command line shows GW-BASIC's banner and `Ok`.
  2. `PRINT 2+2` prints 4.
  3. `SYSTEM` returns to VC's panels.
  4. Enter on a `.BAS` file runs it.
  5. The renamed and the changed EXE behave as in section 3.
  6. The injection test from section 4.
  7. Ctrl-Break stops `10 GOTO 10`.
  8. Every shipped game reaches its first prompt.
- `make web` and `make test-web` green. The node smoke test starts GW-BASIC from VC, runs
  `PRINT 2+2`, and sees `BEEP` reach the speaker hook.
- The wasm stays instant: report its gzip size and the growth from `GWBASIC.EXE`.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. Your final summary lists files changed, sizes and what GW-BASIC needed from
the machine. It names three weaknesses of your own work, each with a real `file:line`. Do not
commit; I commit.
