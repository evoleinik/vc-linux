# Brief 11 verification

Completed 2026-10-01. All required gates pass. No commit was made.

## Result

VC launches the real DOS `GWBASIC.EXE` through current-directory / DOS `PATH`
lookup and EXEC. EXEC reads and compares the complete file, including its MZ
header, before selecting an ahead-of-time translation. Renaming the file works;
changing a byte returns a DOS error without losing VC. There is no runtime x86
decoder, host BASIC interpreter, or special `gwbasic` command implementation.

Linux installs the interpreter, VC.COM and VC.OVL in the config directory. The
browser puts GW-BASIC on `H:\` and twelve tested games in `H:\GAMES`. The old
generated `GAMES/NOTHING.TXT` is replaced. Empty legacy VC.EXT files migrate to
`bas: gwbasic !.!`; custom settings are preserved. Filename text from this
association never reaches the host shell, including after deleting the EXE.

## Files changed

Existing files:

- `Makefile`, `README.md`, `data/VC.EXT`.
- `runtime/bios.c`, `runtime/bios.h`, `runtime/cpu.h`, `runtime/dos_core.c`,
  `runtime/dos_fs.c`, `runtime/hle.h`, `runtime/image.h`, `runtime/main.c`,
  `runtime/rt.c`, `runtime/rt.h`, `runtime/term.c`.
- `translator/README.md`, `translator/__main__.py`, `translator/emit.py`,
  `translator/layout.py`, `translator/listing.py`.
- `tools/web_demo.py`, `web/README.TXT`, `web/vc-web.js`.
- `tests/e2e/vcterm.py`, `tests/test_dos_fs.c`, `tests/test_term.c`,
  `tests/test_translator_ops.py`, `tests/translator_support/ops_build.py`,
  `tests/translator_support/ops_harness.c`,
  `tests/translator_support/ops_harness.h`, `tests/web_smoke.mjs`.

Added files:

- `tools/build_gwbasic.py`, `tools/gwbasic.mk`, `tools/basic_games.py`.
- `translator/linked.py`, `web/speaker.js`.
- `tests/test_dos_exec.c`, `tests/test_machine.c`,
  `tests/test_translator_linked.py`, `tests/test_gwbasic_e2e.py`,
  `tests/test_web_speaker.mjs`.
- `docs/verification/11-gwbasic.md` (this report).

The pre-existing untracked brief, `third_party/` sources, and `tools/jwlink/`
were inputs, not newly authored changes. Vendored game and interpreter sources
remain unchanged. Build products stay under ignored `build/`.

## Build and translation findings

`make gwbasic` produces a 51,296-byte executable, a verbose link map, and all
38 linked-module listings. The source's 39 assembly units include MATH1/MATH2
combined in MATH.ASM. The old MASM link list also needs the fork's OEMCBK module.
The version is `2023-10-03`, read from `third_party/gwbasic/UPSTREAM`. Listing
and map timestamps/timing diagnostics are normalized. An independent rebuild
compared every listing, the map and the EXE byte-for-byte.

The linked translator emits 20,551 instructions in 81 chunks. It places module
contributions using the linker map and checks listing bytes and contribution
coverage against the linked image. It recovers 210 instruction starts from
data, including INS86 macros, overlapping skip idioms, initialization, and
FNINP/FNOUT. SYNCHR consumes a one-byte inline token by changing its return
address; the token is not falsely treated as the next instruction.

Six instructions have 16 mutable operand bytes. These are four `MOV AX,imm16`
segment operands (CBKDS, IMDS, ITICDS, IRQ0DS) and the two saved ISR far jumps
(CBOISR, IMOISR). Generated C reads those operands from guest memory. Matching
allows only declared operand changes; opcode and instruction-boundary changes
remain invalid. CPMEXT is a data far pointer used by PUSH/PUSH/RETF, not new
runtime-generated instructions. INP/OUT use data-encoded instructions, not a
general self-modifying instruction template.

The single-listing path remains byte-identical to the saved pre-change output:

| Generated file | Bytes | SHA-256 |
|---|---:|---|
| `vc_com.c` | 723,366 | `d4d8f2fe893a384ae7c7c6695e650e862f1a894c1f13572b8c929fc302674817` |
| `vc_ovl.c` | 10,465,518 | `f0f51d56d84994b90828127a02d55de241a3a64b898dccc932846b51d5fec9b6` |

Both `cmp` commands returned zero against
`/tmp/vc-gwbasic-before.koYWmF/gen/` after the final builds.

## What the machine needed

- IRQ 0 / INT 8 delivery at dispatch boundaries with IF/PIC gating, chaining
  to INT 1Ch, plus HLT wakeup and interrupt frames. Normal PIT timing is about
  18.2 Hz; GW-BASIC temporarily programs channel 0 to divisor 2983, about
  400 Hz, for its sound/event machinery and chains the slower BIOS tick itself.
- Ctrl-Break delivered as INT 1Bh. Kitty and modifyOtherKeys input are handled;
  the page maps Ctrl-Pause and Ctrl-Shift-B to the same break request.
- PIT channels 0 and 2, ports 40h/42h/43h/61h, and PIC 20h/21h support. Browser
  output uses one Web Audio square oscillator, unlocked by the first key
  gesture. Native output remains silent.
- DOS 1.x FCB parse/open/create/close, sequential/random/block record I/O,
  size, random-position setup, and literal deletion, including DOS AL return
  statuses, record padding, DTA handling, and child-owned handle cleanup.
- Correct create-PSP behavior and execution of the kernel-owned PSP `INT 20h`
  termination thunk for SYSTEM. INT 15h/17h give unsupported-device responses.

Observed speaker divisors/frequencies through the real interpreter:

| BASIC command | Divisor | Frequency, Hz |
|---|---:|---:|
| `BEEP` | 1491 | 800.256 |
| `SOUND 440,18` | 2712 | 439.964 |
| `PLAY "CDE"` | 1140, 1015, 905 | 1046.651, 1175.549, 1318.433 |

The fork's default PLAY octave is 4; its table yields those C/D/E frequencies,
not modern middle-C guesses. Tests check gate-off silence as well as pitches.

## Games

The twelve shipped, first-prompt-tested files are STARTREK, HAMURABI, LUNARLEM,
ANIMAL, BAGELS, HANGMAN, AMAZING, BLACKJAC, HURKLE, LIFE, TICTACTO and ACEYDUCY
(all `.BAS`). Generated filenames are uppercase DOS 8.3 and all game line
endings are CRLF. The same manifest drives packaging and parameterized PTY tests.

The unmodified Star Trek listing failed with syntax errors at 6430 and then
6820: compact `1TOS1` / `22STEP3` forms confuse this interpreter's tokenizer.
Build-time conversion inserts whitespace around ten compact TO/STEP occurrences
outside strings. It changes neither tokens nor game logic, leaves the vendored
listing intact, and records the adjustment in the generated games README.

## Final gates

| Gate | Result |
|---|---|
| `make gwbasic` | Reproducible EXE, map and all module listings |
| `make test` | Passed |
| Translator | 208 tests; 63,680 distinct instructions, 32 randomized successful states each; 2,037,760 successful states and 808 divide-error cases |
| DOS filesystem | 4,317 checks / 85 services |
| EXEC | 1,027 checks |
| Machine | 9 groups |
| Terminal | 32 groups / 5,647 checks |
| INI | 3 tests |
| Native PTY | 67 tests, including 29 new GW-BASIC cases |
| VC generated-C `cmp` | Both unchanged |
| `make web test-web` | Passed; speaker unit gate and all 11 Node smoke stages |

The PTY cases cover command launch, arithmetic, INP/OUT, repeated SYSTEM,
Enter on BAS and empty-default migration through both execution paths, renamed
EXE/COM files, changed-file refusal, all four installed/deleted-injection
combinations, both Ctrl-Break shortcuts, and all twelve first prompts.

The Node smoke gate retains all older VC checks and adds real PATH/EXEC startup,
arithmetic, BEEP/SOUND/PLAY speaker hooks and SYSTEM. The separate Web Audio
unit gate checks gesture gating, square waves, reuse, silence, absent audio,
and a partially initialized audio device.

Emscripten invocation in this offline workspace:

```sh
env EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
    EM_CACHE=/home/eo/src/vc-linux-wt/gwbasic/build/emcache \
    make web test-web \
    EMCC=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc
```

## Sizes and startup

All gzip measurements use GNU `gzip -n -c` (default level 6). The pre-change
wasm was rebuilt from the saved original sources with the same Emscripten and
`-O2`, narrow-Asyncify flags as the final build.

| Artifact | Bytes |
|---|---:|
| `build/gwbasic/GWBASIC.EXE` | 51,296 |
| `build/gen/gwbasic.c` | 7,788,469 |
| Native `build/vc` (default debug build) | 30,566,512 |
| Browser module `vc.mjs` | 81,316 |
| Browser wasm, raw | 7,306,762 |
| Browser wasm, gzip | 1,574,554 |
| Pre-GW-BASIC wasm, raw | 4,020,342 |
| Pre-GW-BASIC wasm, gzip | 993,918 |
| Gzip growth for this GW-BASIC/games change | 580,636 (+58.42%) |
| Embedded H: drive, 21 files | 259,266 (below 400,000) |
| Games directory, including README/license | 59,557 |

This growth is translated GW-BASIC plus its supporting runtime and embedded
data, not Asyncify rewriting translated chunks. An isolated identical build
without Asyncify was 7,300,449 bytes raw / 1,570,972 gzip: Asyncify adds only
6,313 raw / 3,582 gzip bytes (0.228% gzip overhead). `ASYNCIFY_ADVISE` reported
eight defined runtime functions and **zero translated chunks** instrumented;
the advice-enabled wasm was byte-identical to production. The no-Asyncify
artifact was only measured, never executed.

Three fresh production-Node processes reached the first VC panels/`10Quit`/
README screen in 56.890, 57.669 and 57.452 ms, measured from before module
import (median 57.452 ms). This excludes browser network and font loading.

Final EXE SHA-256:
`3d83e8d2bbc71e8b79e20c6cfb7bceba36c4dd4bda2a7af4c415cab6b3f3203c`.
Final wasm SHA-256:
`32b097346c4bc80337be117deb8d5bb6ba737bf1f4f2745fdbd281a166cef39a`.

## Deliberately broken gates: red, then restored green

Defects were applied individually to isolated source/output copies, or to the
oracle's explicit corruption wrappers. No defect remains in production.
The unchanged gate was used for each red/green pair; PTY mutants select the
alternate binary through `VC_TEST_BINARY`, not altered assertions.

| Gate | Planted defect and observed failure | Restoration |
|---|---|---|
| Pinned/reproducible build | Hardcoded version `2099-12-31` instead of UPSTREAM; version gate failed | UPSTREAM restored; independent EXE/map/listing comparison passed |
| Linked placement | Shifted ADVGRP contribution by 16 bytes; linked-byte mismatch at 0x540 rejected translation | Original map accepted |
| Linked completeness | Omitted a module/instruction row or corrupted contribution length; strict map/coverage gates rejected them | Complete listings accepted; linked suite passed |
| Live-operand Unicorn coverage | Froze a patched DS immediate; C returned AX=0000 while Unicorn returned AX=88E2 | Live memory operand restored; oracle and complete instruction suite passed |
| PTY 1: banner / Ok | Changed BIOS output `G` to `X`; both launch cases showed `XW-BASIC` and failed | Both passed |
| PTY 2: arithmetic | Changed BIOS output `4` to `5`; PRINT gate observed 5 and failed | PRINT gate passed |
| PTY 3: SYSTEM | Removed dispatcher PSP-thunk handling; SYSTEM faulted at PSP:0000 | SYSTEM/relaunch gate passed |
| PTY 4: Enter on BAS | FCB reads falsely returned EOF; both HELLO cases reached Ok without executing their PRINT | Both Enter/migration cases passed |
| PTY 5: renamed image | Added basename restriction to byte-matched GW-BASIC; all three renamed EXE/COM cases failed | Restriction removed; all three passed |
| PTY 5: changed image | Removed full-file byte comparison; altered-header unit case and changed-file PTY gate failed | Full comparison restored; both passed |
| DOS PATH | Skipped PATH search; PATH-only program lookup failed | Search restored; EXEC suite passed |
| PTY 6: filename injection | Removed missing-association barrier; both deleted-interpreter PTY cases failed, with controlled temporary PWNED creation confirming shell injection | Barrier restored; injection gates and EXEC suite passed |
| PTY 7: Ctrl-Break | Consumed pending break without INT 1Bh; both shortcuts left the loop running | INT 1Bh restored; both passed |
| PTY 8: every shipped game | FCB reads falsely returned EOF; all twelve first-prompt cases failed | All twelve passed after restoring reads |
| FCB records | Advanced the single-random-record field incorrectly; FCB random-read test failed | Correct position semantics restored; filesystem suite passed |
| FCB lifecycle | Omitted child FCB cleanup; lifecycle test failed after the first child | Cleanup restored; 1,027 EXEC checks passed |
| EXEC regular-file safety | Removed O_NONBLOCK before regular-file validation; FIFO test hung until alarm exit 142 | Nonblocking validation restored; suite passed |
| Interrupt IF | Removed IF guard; two machine groups failed | All nine groups passed |
| BIOS tick chain | Omitted INT 1Ch chaining; two machine groups failed | All nine groups passed |
| Fast sound timer | Ignored PIT channel-0 reload; 400 Hz group failed | Reprogrammed-timer group passed |
| HLT | Ignored halted state; HLT/wakeup group failed | Group passed |
| Speaker frequencies | Doubled PIT frequency calculation; frequency group failed | BEEP/SOUND/PLAY frequency group passed |
| Mutable-code matching | Rejected declared operand patches, then separately accepted opcode patches; respective machine gates failed | Both positive and negative matching gates passed |
| Unsupported printer | Reported a ready printer instead of unavailable; BIOS gate failed | Unsupported-device gate passed |
| Break key parsing | Removed break parsing; 25 terminal checks failed | All 5,647 terminal checks passed |
| Web speaker integration | Disabled the wasm-to-JS speaker hook; unchanged smoke stages 1–6 passed, stage 7 failed with no BEEP events | Hook restored; all eleven smoke stages passed |

Additionally, the partial-audio-device test first reproduced an uncaught
TypeError after oscillator creation failed. Clearing partial state fixed it;
the restored speaker unit suite is green. Machine/terminal sanitizer probes
also passed with ASan/UBSan and leak detection disabled (LeakSanitizer cannot
operate under this sandbox's tracing restrictions).

## Three weaknesses and verification limits

1. `translator/linked.py:263`: mutable operand discovery covers direct,
   statically identifiable CS-relative stores. Arbitrary code injected through
   CALL/USR/POKE is not supported by this ahead-of-time translation.
2. `runtime/dos_fs.c:1477`: FCB search/rename and wildcard deletion are not
   implemented. They return failure; BASIC FILES/NAME/wildcard KILL remain
   incomplete even though the shipped first-prompt paths work.
3. `runtime/rt.c:205`: PIT reads expose reload values, not cycle-accurate
   running counters/latches. Timing-sensitive BASIC beyond these tested paths
   can differ from a physical PC.

First-prompt tests do not exhaust later gameplay. Actual browser-page/audio
hardware verification was not possible here: the sandbox denied a local HTTP
listener and the browser helper's socket directory. The required real-wasm
Node gate and Web Audio contract tests passed; no claim of an audible hardware
or browser-rendering check is made.
