# Brief 14 verification

Completed on 2026-10-01. All required gates pass, and all 28 deliberate defects
went red and then green after restoration. No commit was made.

## Result

`logo`, `LOGO.COM`, and Enter on the installed file execute Oscar Toledo G.'s
unchanged 503-byte DOS COM program, built by the supplied NASM with
`-Dcom_file=1`. Renamed identical copies work; changed bytes do not select the
translation. Linux installs it next to GW-BASIC in the config directory; the
browser installs it on `H:\`. No Logo command interpreter or runtime x86 decoder
was added.

CGA modes 4/5 and 6 share real interlaced B800h memory with guest direct writes.
BIOS pixels, XOR, palettes, port 3D9h, cursor services, and public-domain 8x8
ASCII/box/block glyphs work. The terminal reduces the screen to 80x25 braille
with any-visible-pixel dots and dominant source colours, using truecolor or the
fixed 256-colour palette. The browser canvas covers the existing xterm area,
uses nearest-neighbour scaling and CGA RGB colours, and retains xterm's input
path. Switching to text restores xterm or the terminal's text rendering.

`VC_SCREEN_DUMP` contains the same braille as the terminal in graphics mode.
`VC_FRAME_DUMP` is binary PGM: `P5\n320 200\n3\n` or
`P5\n640 200\n1\n`, followed by exactly one raw palette-index byte per pixel.
The last graphics PGM is retained after switching to text. The square's four
corners are `(160,100)`, `(160,50)`, `(210,50)`, `(210,100)`, all index 3;
these coordinates are independently verified against Unicorn and bootLogo's
9.7 fixed-point arithmetic, not inferred from our renderer.

`LOGO.TXT` documents the commands, square, star, and upstream README flower.
`GAMES\SPIRAL.BAS` is an original BSD-2-Clause SCREEN 1 / DRAW example, packaged
as DOS CRLF text. Its own key prompt restores text before `SYSTEM` returns to VC.

## Findings that determined the implementation

- VC already owns graphics restoration. `asm/VCOVL.ASM:748` detects an
  unsuitable mode; `:805` prints `Press ENTER to return` and waits for Enter;
  `:835` calls `RestoreVideoMode`, whose BIOS call is at `asm/VCMENU.INC:2007`.
  Thus bootLogo's `QUIT` is followed by VC's authentic Enter confirmation.
  Both pty and Node tests observe that graphics-mode prompt, press Enter, and
  require fully redrawn text panels. DOS termination was not modified to
  force a mode switch or inject a key.
- The NASM reader uses the active expanded `-LefFt` listing, checks every
  linked byte, and emits 217 instruction starts. It retains both overlapping
  DB BAh skip instructions and reads the one self-modified pen-colour immediate
  at file offset 1AFh from live memory.
- GW-BASIC DRAW's source pointer table enters four previously untranslated
  `NEGDE` / `INS86` starts: BE5h, BECh, BF5h, C01h. A separately generated
  `gwbasic_graphics.c` validates all fifteen source DB/DW table entries, their
  linked labels, both NEG DX bytes, and the following known boundary. It uses
  the ordinary instruction emitter and Unicorn oracle. Generic supplement
  dispatch still requires the original image's byte match, including copied
  code. The established `gwbasic.c` remains unchanged.
- GW-BASIC's graphics editor needs AH=08h character recognition from actual
  pixels, including guest-written pixels. SCREEN 0 also requests text modes
  0/1 from 40-column graphics and mode 2 from 640-pixel graphics. Those BIOS
  services were implemented and tested instead of changing BASIC.
- Its private keyboard queue holds 31 characters (`GWRAM.ASM:440`,
  `GIO86.ASM:2004`). Tests enter long BASIC statements in eight-byte chunks,
  waiting for the actual text/glyph echo; there are no typing-speed sleeps.
  The README documents this original-interpreter paste limitation.
- Its DOS 1.x FCB loader accepts `[dev:]filename[.extension]`, not subdirectory
  paths (`GIO86.ASM:1472`, `GIODSK.ASM:201`). The Node spiral check follows the
  documented workflow: enter GAMES in VC, select SPIRAL.BAS, press Enter.

## Final functional gates

| Gate | Result |
|---|---|
| `make bootlogo` | Real reproducible 503-byte COM and expanded listing |
| `make test` | Exit 0 |
| Translator | 235 tests; all 63,901 distinct instructions, 32 randomized states each, plus whole-program Logo/Unicorn checks |
| DOS filesystem | 4,317 checks, no failures |
| DOS EXEC | 1,129 checks passed |
| Machine / child process | 14 / 4 cases, no failures |
| Terminal | 35 groups, 5,759 checks, no failures |
| CGA BIOS | 823,877 checks, no failures |
| Config | 3 tests passed |
| Native pty | 110 tests passed, including all 17 new graphics cases |
| `make web` and `make test-web` | Exit 0; asset versions, speaker, canvas unit tests, and all 17 real-wasm smoke stages |
| Legacy generated C | All three `cmp` commands exit 0 |
| `git diff --check` | Exit 0 |

The new pty cases cover both COMSPEC and INT 2Eh command routes, Enter on
LOGO.COM and renamed COM/EXE copies, rejection of a changed COM, exact square
pixels and braille, two complete QUIT/relaunch cycles, pen/colour/backward and
nested-repeat paths, the star and flower, BASIC LINE/CIRCLE/PSET/DRAW and
SCREEN 0, modes 5/6, and the shipped spiral. The Node test retains its original
eleven stages and adds Logo launch, exact square/canvas/PGM/braille checks,
QUIT restoration, BASIC drawing/text restoration, the spiral, and final exit.

Final logs are `build/brief14-make-test.log` and
`build/brief14-make-web-test.log`. The standalone new pty suite is also green
in `build/brief14-logo-e2e-green.log`. A final production rerun of
`make bootlogo web test-web` also exited zero in
`build/brief14-final-web-gates.log`.

AddressSanitizer/UndefinedBehaviorSanitizer checks passed all 823,877 BIOS
checks and all 5,759 terminal checks. Logs:
`build/brief14-cga-sanitize.log` and
`build/brief14-render-palette-alias-sanitize.log`. These used
`ASAN_OPTIONS=detect_leaks=0`: LeakSanitizer cannot run in this ptrace sandbox,
so no leak-checking claim is made.

An extra real-browser attempt was unavailable: the sandbox denied the local
HTTP socket, and agent-browser could not start its daemon even with its
supported writable `/tmp` socket directory. Evidence is in
`build/brief14-browser-{server,launch,temp-launch}.log`. The required Node/MEMFS
gate and actual wasm graphics callbacks passed; DOM focus and physical browser
painting have only unit/structural coverage here, not a successful GUI run.

## Byte-identical legacy generated C

The pre-change native binary, runtime, generated C, demo, and build inputs were
preserved under `build/brief14-baseline/`. After the final functional runs:

```sh
cmp build/brief14-baseline/gen/vc_com.c build/gen/vc_com.c
cmp build/brief14-baseline/gen/vc_ovl.c build/gen/vc_ovl.c
cmp build/brief14-baseline/gen/gwbasic.c build/gen/gwbasic.c
```

Each returned zero. `build/brief14-generated-c-cmp.log` also records the earlier
comparison. Sizes and final SHA-256:

| File | Bytes | SHA-256 |
|---|---:|---|
| `vc_com.c` | 723,366 | `d4d8f2fe893a384ae7c7c6695e650e862f1a894c1f13572b8c929fc302674817` |
| `vc_ovl.c` | 10,465,518 | `f0f51d56d84994b90828127a02d55de241a3a64b898dccc932846b51d5fec9b6` |
| `gwbasic.c` | 7,788,469 | `8d8cf3225503fa947a969eb089b820ade134d61938765a78a257da7d43c7fe6f` |

## Deliberate defects and restoration

Each mutation is tested independently, not combined with another defect.
The first interlace unit defect briefly changed `runtime/bios.c` and was
immediately restored. All later BIOS, renderer, runtime, and pty mutations
use isolated source copies and binaries under `build/brief14-mutants/`;
restores are compared with production. The full final builds and tests ran
with the restored sources. The five browser mutations use one isolated compiled wasm with
exactly one test-only selector enabled per process. Each red run is followed
by a fresh process with that selector explicitly unset and all 17 stages
required green. No test selector is present in production sources.
Both browser C copies were then restored and passed `cmp`; the labeled
compiled test artifact and copied harness remain under `build/` for replay.

The evidence prefix in the table is relative to `build/`; brace suffixes name
the red and restored logs. The translator groups restore all 18 NASM or all
8 DRAW tests. The renderer group restores its complete terminal/canvas tests.

| Gate / planted defect | Observed red result | Evidence prefix / restoration |
|---|---|---|
| NASM linked-byte checking removed | 4 changed-byte rejection tests fail | `brief14-nasm-reader-red.log`; `brief14-nasm-restored-green.log` |
| Logo omitted from all-image oracle cases | Coverage test fails | `brief14-nasm-coverage-red.log`; same 18-test restore |
| Pen immediate frozen instead of live | AX 0C03h differs from Unicorn 0C4Ah | `brief14-nasm-color-red.log`; same restore |
| DRAW source-macro proof removed | Missing macro no longer rejected | `brief14-draw-source-red.log`; `brief14-draw-restored-green.log` |
| Supplemental NEG DX suppressed | DX 5050h differs from Unicorn AFB0h | `brief14-draw-semantics-red.log`; same 8-test restore |
| CGA odd bank 1000h instead of 2000h | 206,838 checks fail | `brief14-cga-interlace-defect.log`; `brief14-cga-green.log` |
| Pixel XOR changed to OR | 88 checks fail | `brief14-cga-xor-{defect,restored}.log` |
| Palette selection forced to zero | 11 checks fail | `brief14-cga-palette-{defect,restored}.log` |
| Glyph bit order reversed | 1,356 checks fail | `brief14-cga-glyph-{defect,restored}.log` |
| AH08 always returns unknown glyph | 315 checks fail | `brief14-cga-readchar-{defect,restored}.log` |
| Text modes 0/1/2 ignored | 22 checks fail | `brief14-cga-textmodes-{defect,restored}.log` |
| Port 3D9 writes ignored | 1 machine case fails | `brief14-machine-color-port-{red,restored}.log` |
| Generic supplement fallback disabled | 2 cases fail, direct and copied code | `brief14-machine-supplement-{red,restored}.log` |
| Braille top-left bit removed | 3 reduction and 3 dump checks fail | `brief14-render-braille-defect.log`; `brief14-render-restored.log` |
| PGM first raw pixel flipped | Exact index fails in all 3 modes | `brief14-render-pgm-defect.log`; same renderer restore |
| Source-colour count becomes presence-only | Majority/palette/truecolor checks fail | `brief14-render-colour-defect.log`; same restore |
| Canvas palette stride 2 instead of 3 | Exact RGBA assertion fails | `brief14-render-canvas-defect.log`; same restore |
| Canvas stays visible on null frame | Text-return visibility assertion fails | `brief14-render-canvas-exit-defect.log`; same restore |
| Native Logo translation removed | Launch cannot reach Logo prompt | `brief14-native-no-logo-{red,restored}.log` |
| Native drawing address shifted one byte | Square corner is 0 instead of 3 | `brief14-native-bad-pixel-{red,restored}.log` |
| Native mode 3 restoration suppressed | QUIT cannot reach text panels | `brief14-native-no-text-return-{red,restored}.log` |
| Native odd drawing rows 100-159 suppressed | BASIC PSET at (17,151) never appears, after input echo succeeds | `brief14-native-bad-direct-decode-{red,restored}.log` |
| Native braille bit 2 removed | Exact braille corner fails, raw pixels pass | `brief14-native-no-braille-dot-{red,restored}.log` |
| Web Logo EXEC selection removed | Stage 12 fails | `brief14-web-logo-{red,restored}.log` |
| Web square corner (210,50) zeroed | Stage 13 fails | `brief14-web-square-{red,restored}.log` |
| Web text-return callback suppressed | Stage 14 fails; restored run passes all 17 | `brief14-web-return-{red,restored}.log` |
| Web BASIC endpoint (100,10) zeroed | Stage 15 fails; restored run passes all 17 | `brief14-web-basic-{red,restored}.log` |
| Web spiral loses DRAW | Stage 16 fails; restored run passes all 17 | `brief14-web-spiral-{red,restored}.log` |

Exact mutation command/result indexes:

- `build/brief14-cga-defect-index.txt`
- `build/brief14-native-mutation-commands.txt`
- `build/brief14-native-mutations-results.txt`
- `build/brief14-machine-mutation-commands.txt`
- `build/brief14-render-mutations.txt`
- `build/brief14-web-mutation-index.md`

Two additional real regressions were first shown red: AH08/SCREEN 0 failed
340 new BIOS checks before the fix (`brief14-cga-screen-editor-{before,fixed}.log`), and raw
indices mapping to the same visible background incorrectly lit braille dots
(four failures, then green in `brief14-render-palette-alias-{old,fixed}.log`).

## WebAssembly size

The baseline wasm was built from the preserved pre-change inputs at
`build/brief14-before-web/vc.wasm`, using the same `-O2`, `ASYNCIFY`, and
`ASYNCIFY_IGNORE_INDIRECT=1` flags as the final build. Measurement uses
`gzip -n -c`, without an embedded timestamp or filename.

| Artifact | Before | After | Growth |
|---|---:|---:|---:|
| `vc.wasm`, raw bytes | 7,309,274 | 7,356,456 | 47,182 (0.6455%) |
| `vc.wasm`, gzip bytes | 1,577,045 | 1,586,728 | 9,683 (0.6140%) |

These numbers are wasm-only, not the JavaScript loader or complete site.
Asyncify remains on the narrow existing dispatcher/idle path.

## Files changed by this brief

Existing files:

- `Makefile`, `README.md`.
- `runtime/bios.c`, `runtime/bios.h`, `runtime/dos_core.c`, `runtime/image.h`,
  `runtime/main.c`, `runtime/rt.c`, `runtime/rt.h`, `runtime/term.c`.
- `translator/__main__.py`, `translator/README.md`.
- `tools/gwbasic.mk`, `tools/web_demo.py`.
- `web/index.html`, `web/vc-web.js`, `web/README.TXT`.
- `tests/e2e/vcterm.py`, `tests/test_dos_exec.c`, `tests/test_install_e2e.py`,
  `tests/test_machine.c`, `tests/test_rt_process.c`, `tests/test_term.c`,
  `tests/test_translator_ops.py`, `tests/translator_support/ops_build.py`,
  `tests/web_assets.mjs`, `tests/web_smoke.mjs`.

Added files:

- `translator/nasm.py`, `translator/supplement.py`, `tools/bootlogo.mk`.
- `web/graphics.js`, `web/LOGO.TXT`, `web/GAMES/SPIRAL.BAS`.
- `tests/test_cga.c`, `tests/test_logo_e2e.py`, `tests/test_translator_nasm.py`,
  `tests/test_translator_supplement.py`, `tests/test_web_graphics.mjs`,
  `tests/translator_support/bootlogo_oracle.py`.
- `docs/verification/14-logo-cga.md` (this report).

The pre-existing untracked brief, `third_party/bootlogo/`,
`third_party/font8x8/`, and `tools/nasm/` were supplied inputs, not authored
changes. Vendored sources and the existing settings/associations were left
unchanged. Generated C, DOS programs, wasm, snapshots, mutants, and logs are
ignored build products under `build/`.

## Three weaknesses

1. `translator/nasm.py:48`: the new reader intentionally accepts only expanded
   single-section, ORG 100h flat-COM listings. It is not a general NASM frontend;
   multiple sections, INCBIN, and other origins are rejected.
2. `runtime/term.c:333`: every graphics render scans all 64,000/128,000 pixels
   and recomputes the braille reduction, even when guest video memory is
   unchanged. Dirty-aperture detection could reduce that cost.
3. `tests/test_web_graphics.mjs:75`: keyboard preservation is checked through
   structural assertions and canvas stubs, not real DOM focus/paint events.
   Node executes the real wasm, but the blocked browser attempt leaves that
   extra integration coverage incomplete.
