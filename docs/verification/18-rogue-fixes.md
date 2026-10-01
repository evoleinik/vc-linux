# Brief 18: Rogue fixes

No commit was made. The pre-existing uncommitted Rogue implementation and both
vendored source trees are preserved.

## Tests first

Each regression was added and run against the corresponding old code before
the fix was applied.

1. Automatic restore:

   ```sh
   .venv/bin/python -m pytest -q tests/test_rogue_e2e.py -k failed_auto_resume_quarantines_save_and_starts_fresh
   ```

   Old code: **4 failed, 24 deselected in 6.42s**. A real saved game was
   truncated near the end or given a mismatching version, on both COMSPEC and
   INT 2Eh command paths. Every case returned to VC's panels instead of
   starting a fresh dungeon. The regression also requires preserved rejected
   bytes in `rogue.bad`, a visible one-line notice, a fresh five-item inventory,
   and another successful plain launch.

   The final screen-only wait was also run against the retained old native
   binary (`VC_TEST_BINARY=/tmp/rogue-pre18-native.dlsfIs/vc`): the
   `truncated-comspec` case failed after 21.57s with the rejected save still in
   the panel and DOS exit code 1. Waiting for a screen, rather than any child
   termination, avoids mistaking the nested restore attempt for recovery.

   The existing-quarantine preservation case also failed against that old
   binary after 21.32s: both files remained, but no fresh game started. It
   passes with the fix, including two successive automatic recovery attempts.

   The first full aggregate run (**1 failed, 155 passed in 237.54s**) exposed
   an unnecessary random-movement precondition in this new test: a
   monster-filled starting room had no safe adjacent tile. Removing that
   setup move leaves every recovery assertion
   intact; initial saves already serialize the full player and inventory.
   All four final cases were rerun against the retained old binary and failed
   at recovery (**4 failed, 25 deselected in 84.91s**), while the corrected
   build's complete Rogue suite passed (**29 passed in 42.80s**).

2. Missing OpenWatcom:

   ```sh
   .venv/bin/python -m pytest -q tests/test_rogue_build.py -k missing_watcom_explains_install
   ```

   Old code: **2 failed, 42 deselected in 0.19s**. Both unset `WATCOM` and a
   nonexistent installation omitted the exact fetch/export command. After the
   diagnostic and documentation changes: **2 passed in 0.11s**.

3. Read-only score directory:

   ```sh
   .venv/bin/python -m pytest -q tests/test_rogue_e2e.py::test_rogue_read_only_directory_does_not_delay_qualifying_score
   ```

   Final test against old code: **2 failed in 13.53s**, measuring **5.568s**
   (COMSPEC) and **5.550s** (INT 2Eh), against a 4-second bound. Both cases used
   the actual unwritable `C:\` (`/`), the original `rogue -d` death path, positive gold,
   and a matching rank-1 score. They failed only the elapsed-time assertion.
   Fixed code: **2 passed**, with **0.204s** measured on each command path.
   The privileged-user `/sys` directory-selection branch was also exercised
   and measured 0.204s. Score content is verified through Ctrl-O after timing
   the return to panels, so a fast transient score frame cannot race the test.

4. Verified executable identity:

   An isolated harness ran the repository's real rebuild test with preserved
   pre-fix source inputs and EXE/MAP. The new negative control appended the
   same deterministic DOS overlay to both isolated real builds. The old
   equality-only check accepted those matching but unverified bytes:
   **1 failed, 1 passed**, with `DID NOT RAISE AssertionError`. After adding the
   fixed SHA-256 check: **2 passed**. Shared build artifacts were not mutated.

## Executable identity

The brief's original 202,816-byte EXE was independently confirmed as
`8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee`.
Changing DOS startup and removing score locking necessarily changes that
executable. The historical toolchain-provenance check and the current shipped
artifact check must therefore be separate; neither digest substitutes for the
other. The pinned OpenWatcom release/archive checksum does not change.

The corrected EXE is 206,808 bytes with SHA-256
`b6350f98553e96ae5454383ec377d9feb63834cc99fc3411f7ff016321dd61a7`.
`test_rogue_build_is_byte_reproducible` first checks that shipping identity,
then builds the historical source variant in a temporary tree and asserts the
brief's original hash. `tests/fixtures/rogue-pre18.toml` contains only checked
reverse edits; no old behavior is included in the shipped build. Both vendor
tree digests are checked before and after both builds. All 45 Rogue build
tests passed together.

## Recursive DOS restore return

Clean restore isolation uncovered a dispatcher issue: loading the same Image
at a child's DOS segment replaced the parent's relocated-code registration.
After the child exited, the parent faulted at Watcom's `___dospawn` return
(`14B6:8A48`), despite that instruction being present in the translation.
Both ordinary and renamed-executable save/restore tests were strengthened to
reject nonzero exits and untranslated-code faults; before the runtime fix,
both failed with the nested child exiting 0 and its parent exiting 70.

Before changing the runtime API or implementation, `make test-machine` also
failed its new normal/forced same-image-return cases with
`vc: fatal: no translated code at 2000:0000` (**27 cases, 2 failures**).
The byte-mutation refusal controls remained green. These tests distinguish
restoring the original relocated-code snapshot from copying mutable guest
memory and accidentally accepting an unapproved code change.

After preserving the active parent's original registration across every
child finish, the machine gate reports **27 cases, 0 failures**, the process
gate **9 cases, 0 failures**, and DOS EXEC **1,332 checks passed**. The complete
Rogue pty suite reports **29 passed in 42.80s**, including truncated/old-version
auto-recovery, clean inventory, existing quarantine preservation, ordinary
restore, renamed-executable restore, and zero-error returns to VC.

## Final gates

Toolchain environment:

```sh
export WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom
export EMSDK=/home/eo/src/vc-linux-wt/emsdk
export EM_CONFIG=$EMSDK/.emscripten EM_CACHE=$PWD/build/emcache
export PATH=$EMSDK/upstream/emscripten:$EMSDK/node/20.18.0_64bit/bin:$PATH
```

- `make test`: passed. **261 translator tests passed in 300.42s**, all C
  filesystem/EXEC/machine/process/terminal/CGA suites passed, **3 setup tests**
  and **45 Rogue build tests** passed, and the complete native end-to-end
  suite finished with **156 passed in 239.03s**.
- `make rogue`: passed; the fresh corrected build and isolated reproducibility
  builds have the current identity recorded above.
- `make web`: passed.
- `make test-web`: passed; all asset/speaker/graphics checks and **24 smoke
  checks** passed, including Rogue save/restore and returning to VC.

The WebAssembly size changed from 15,775,230 to 15,999,376 bytes (1.42%); the
JavaScript module remains 81,561 bytes. The narrow Asyncify flags are unchanged.

## Additional checks and limits

The machine and process suites were also compiled independently with
`-fsanitize=address,undefined -fno-omit-frame-pointer`, then run with
`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1`: **27 machine cases
and 9 process cases passed**. LeakSanitizer with leak detection enabled could
not execute under this environment's ptrace restrictions; no leak-check pass
is claimed. An additional isolated diagnostic passed all six cases both
normally and under ASan/UBSan: two-deep same-image recursion with all four
normal/forced return combinations, plus normal/forced finishes without a new
child registration. Scoped `git diff --check` passes. Existing vendor
whitespace was not rewritten.

Three weaknesses of this work:

1. `runtime/rt.c:381` allocates a full original-image snapshot per active
   parent; clean recursive recovery has additional host-memory overhead.
2. `tests/test_rogue_e2e.py:353` retains movement in the renamed-restore test
   to distinguish a restored game from an identical initial random seed.
   Like the existing movement/restore tests, it can fail setup in a rare
   monster-filled room without an adjacent safe tile.
3. `tests/test_rogue_build.py:382` intentionally requires exact source text
   for the historical probe; future source edits need a fixture review as
   well as revalidation of the shipping hash.

## Files changed for brief 18

- Runtime: `runtime/rogue_dos/startup.c`, `runtime/rogue_dos/config.h`,
  `runtime/rt.c`, `runtime/rt.h`, `runtime/dos_core.c`.
- Build/port: `tools/build_rogue.py`, `tools/rogue_port.py`.
- Tests: `tests/test_rogue_e2e.py`, `tests/test_rogue_build.py`,
  `tests/test_machine.c`, `tests/test_dos_exec.c`,
  `tests/fixtures/rogue-pre18.toml`.
- Documentation: `README.md`, `CLAUDE.md`, `runtime/rogue_dos/README.md`,
  `docs/verification/16-rogue-build.md`, this verification record.
