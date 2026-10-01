# Brief 16: DOS build and port-width verification

Date: 2026-10-01. This records the build subtask, not an assertion that all
brief gates pass. The CI archive provenance was subsequently resolved by the
comparison recorded in brief 18; see the pin record below.

## Build and adapters

The local toolchain is OpenWatcom 2.0, compiler banner
`Version 2.0 beta Oct 1 2026 06:32:05 (64-bit)`. Compilation uses:

```text
-zq -bt=dos -0 -ml -os -fpc -j -zt=1024 -zld
```

The generated link response selects `system dos`, `dosseg`, `nofarcalls`, a
16,384-byte stack and a verbose map. This is 8086 DOS large-model code with
software floating point, signed chars and large data outside DGROUP. Stack
checks remain enabled. All linked game/PDCurses objects and the PDCurses
library remain available; the map names every selected Watcom runtime member.

The files in `third_party/rogue` and `third_party/pdcurses` were not edited.
`tools/rogue_port.py` makes exact-count-checked substitutions only in generated
build copies. `runtime/rogue_dos/README.md` documents each adaptation:

- DOS identity, no Unix signals/password database/shell process, binary file
  I/O, current-directory save/score/lock paths, automatic `rogue.sav` restore.
- A 32-bit RNG seed, experience, experience thresholds, gold and scores; unsigned RNG result
  reduction avoids narrowing Unix's nonnegative 0..65535 result to signed int.
- Four-byte save integers use actual four-byte temporaries, correct signed or
  unsigned extension, and explicit long helpers for markers/RNG/experience/gold
  and all 32 bits of PDCurses screen cells. Winning inventory valuation, score
  comparison, text score files and status/death/quit displays retain gold width.
- Watcom's `environ` declaration, direct `Q,y` return, close-before-unlink on DOS,
  restore-error preservation and retryable save write/flush/close failures.
- The missing public-domain PDCurses CP437 table comes from the exact vendored
  upstream revision `e9af89a7e7fc69d5b194b5432df57eefb6d83505`.

The CP437 table was copied offline from
`/home/eo/src/vc-linux-wt/tools-cache/PDCurses/common/acs437.h`, Git blob
`24cbd78549a0ce01ed6d2bd38afa7e3f48483ecd` at that revision. Its declaration
and initializer have identical C tokens; only our provenance comment and
whitespace differ.

`build/rogue/OWLIC.TXT` contains the installed OpenWatcom license and a runtime
source-location notice for redistribution alongside the DOS executable.

## Byte reproducibility

The first working port, before additional save-error checks, was 202,068 bytes:

```text
faf4849eef9eae179c02423364af4a91c9a0264bceb8576731eb7cf664c382f5
```

The safe-save build is 202,382 bytes:

```text
fa2aca4c68edefe983d13bb2323aaf85f1a061a3fa0f936ff2d285455aca3553
```

Both versions were rebuilt into independent output directories and compared
byte-for-byte. At the safe-save stage, fresh Make builds, not merely up-to-date
no-ops, used:

```sh
export WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom
make -B B=build/rogue-repro-final-a rogue
make -B B=build/rogue-repro-final-b rogue
cmp build/rogue-repro-final-a/rogue/ROGUE.EXE build/rogue-repro-final-b/rogue/ROGUE.EXE
cmp build/rogue/ROGUE.EXE build/rogue-repro-final-b/rogue/ROGUE.EXE
```

Both comparisons exited 0; all three EXEs have the safe-save hash above.
Maps retain Watcom's human-readable creation timestamp; the executable and
its byte-matched translation do not use that timestamp.

The post-review gold/window-width build is **202,816 bytes**, SHA-256:

```text
8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee
```

It was built independently in `build/rogue-port-width-review` and by these two
fresh Make invocations; both EXEs and the review EXE compared byte-identical:

```sh
make -B B=build/rogue-port-width-repro-a rogue
make -B B=build/rogue-port-width-repro-b rogue
cmp build/rogue-port-width-repro-a/rogue/ROGUE.EXE build/rogue-port-width-repro-b/rogue/ROGUE.EXE
cmp build/rogue-port-width-repro-a/rogue/ROGUE.EXE build/rogue-port-width-review/ROGUE.EXE
```

### Brief 18: historical provenance and corrected executable

The save-recovery and score-lock fixes deliberately change the linked code.
The corrected source build is 206,808 bytes, SHA-256:

```text
b6350f98553e96ae5454383ec377d9feb63834cc99fc3411f7ff016321dd61a7
```

`test_rogue_build_is_byte_reproducible` checks that current-build digest first,
after its fresh-build byte comparison. It then applies the exact-match,
test-only reverse edits in `tests/fixtures/rogue-pre18.toml` to temporary
copies of the startup, configuration and porter files. The production builder
rebuilds that historical variant against the unchanged vendor trees and must
produce the original 202,816-byte `8844...1c9ee` identity above. Neither hash
is accepted in place of the other, and the historical EXE is never shipped.
The reconstructed files were also compared byte-for-byte with copies retained
before the brief-18 changes.

The fixed-identity negative control appends the same deterministic DOS overlay
to two isolated real builds. The old equality-only check accepted them, so
the new rejection test failed with `DID NOT RAISE AssertionError`; adding the
hash check made it pass. The historical reverse edits are intentionally
source-sensitive and must be reviewed if their exact source context changes.

## Actual 16-bit port tests

```sh
WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom \
  .venv/bin/python -m pytest -q tests/test_rogue_build.py
```

The safe-save version passed **29 tests in 0.57s**. After review, the expanded
suite passed **42 tests in 0.59s** against the isolated final-width EXE. That
isolated run redirected only the `rogue_files` fixture so ongoing canonical
fault campaigns were not disturbed; the aggregate verification records the
normal canonical `make test` run.

Tests execute real routines from the linked DOS EXE in Unicorn, including
Watcom's multiply/divide, sprintf and sscanf runtime. Stack checks and external
curses/stream I/O are stubbed. The full save/read gold regression also stubs
unrelated collections in matching pairs while keeping every scalar call real.
Coverage includes 63 RNG combinations, thresholds through 8,000,000, sign/zero
extension with poisoned stack storage, neighbouring-byte guards, 32-bit marker
rejection, reverse/bold/colour screen-cell round trips, 32,000+1,000 winning
gold, score text and saved-scalar round trips at 33,000 and 1,000,000, and every
success/state/stream/flush/close save-result path. The reproducibility test
checks the complete file sets and hashes of both vendor trees and proves an
obsolete generated `src/stale.c` is not accidentally linked. Source-drift
rejection is tested separately. This does not replace the full translator gate.

## Deliberate defects and recovery

These were real rebuilt-artifact or tool-wrapper faults in isolated build
directories. The canonical EXE, vendor sources and production builder were
never changed for the defect campaign. The same test functions were called
with the isolated EXE/map pair so no alternate executable was substituted at
the instruction-test level.

| Defect | Observed red gate | Recovery |
|---|---|---|
| Restore the original two-byte temporary for four-byte signed/unsigned writes in the generated `state.c` | `test_dos_save_int_sign_extends_four_bytes(..., -1)` raised `AssertionError`; bad image was 201,972 bytes | Rebuild with the production preparer; gate passes and EXE equals canonical |
| Restore a 16-bit RNG seed and its original int save calls in generated copies | `test_dos_rng_keeps_all_32_bits` rejected the resulting seed; compiler also warned about shift-by-16 on int | Rebuild with the 32-bit seed; gate passes and EXE equals canonical |
| A verification-only include wrapper calls `fclose` but discards its error | `test_dos_failed_save_resumes_game_and_closes_stream(..., "close")` rejected the incorrect success result | Recompile the real `save_io.c`; gate passes and EXE equals canonical |
| An isolated builder wrapper appends an eight-byte `time.time_ns()` footer to each real fresh link | `cmp` exited 1: the two EXEs differed at byte 202,383, immediately after the actual DOS image | Remove the footer operation, rebuild both outputs; `cmp` exits 0 and both equal canonical |
| A copied builder restores the old `out/src/*.c` input glob and encounters an obsolete `#error` source | Actual DOS build exited 1 with `E1091: obsolete build copy must not be compiled` | Restore the current-vendor input list in the copy; same output directory builds and compares equal to the final-width EXE |

The nondeterminism wrapper lived at `build/rogue-repro-mutation.py`, with outputs
`build/rogue-repro-mutant-a` and `build/rogue-repro-mutant-b`. It is restored to
an ordinary builder wrapper. The width, RNG and close artifacts were likewise
rebuilt without their faults. Captured local logs are:

```text
build/rogue-verification/width-mutant-red.txt
build/rogue-verification/rng-mutant-red.txt
build/rogue-verification/close-mutant-red.txt
build/rogue-verification/width-rng-close-restored-green.txt
build/rogue-verification/repro-mutant-red.txt
build/rogue-verification/repro-restored-green.txt
build/rogue-verification/stale-source-red-green.txt
build/rogue-verification/window-gold-red-green.txt
```

The post-review regressions found two real portability defects: a PDCurses
`chtype` was narrowed through a 16-bit `int` on both save and restore, dropping
attributes, and winning gold/score accounting overflowed at 32,767. All 13 new
linked-DOS regression cases failed against the retained safe-save EXE and passed
against the isolated final-width EXE; their exact case results are in the last
log above. Production/vendor inputs were not mutated for these comparisons.

The separate pty save test also reproduced the original unchecked-restore
bug: truncating a real save caused it to be unlinked. Its old-code failure is
captured in `build/rogue-verification/truncated-save-old.txt`; the aggregate
verification record reports the fixed runtime result.

## Resolved CI prerequisite: archive provenance

The original fetch transcript records:

```sh
gh release download Current-build -R open-watcom/open-watcom-v2 \
  -p ow-snapshot.tar.xz --clobber
```

Its output reported a 150,304,640-byte archive and later deleted the archive
after extraction. That original moving-tag transcript did not record a
checksum or immutable release identifier, so it alone was insufficient for CI.

The subsequent comparison recorded in `docs/briefs/18-rogue-fixes.md` on
2026-10-01 resolved this prerequisite. `tools/fetch-openwatcom.sh` pins:

```text
https://github.com/open-watcom/open-watcom-v2/releases/download/2026-10-01-Build/ow-snapshot.tar.xz
SHA-256: e6aa1b1e40ac8bbf97658d2c70fff8a4242d6ca4a1c60806f2baa5317083d4fe
```

The release's `wcc`, `wlink`, `wdis`, `wlib` and `lib286/dos/clibl.lib` were
compared byte-for-byte with `$HOME/src/vc-linux-wt/tools-cache/openwatcom` and
matched. The digest above belongs to the release archive, not the extracted
installation. CI uses this archive checksum as its cache key and the fetch
script checks it before extracting a downloaded archive.

The matching toolchain builds the post-width-review, pre-brief-18 executable
recorded above: 202,816 bytes with SHA-256
`8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee`.
That is a historical source-and-toolchain identity; later source fixes change
the executable hash without changing the pinned toolchain archive.
