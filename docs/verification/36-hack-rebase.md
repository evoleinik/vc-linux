# Brief 36: Hack rebase verification

Recorded 2026-10-02 against main
`773c0afcdd7b2c2fcd3e652d9ba9c1c424b801d0` and the stopped Hack commit
`95098b42cd75485397a87e86c0c07e3cf28776e8`.
Only working-tree files and private build/test artifacts were changed. No Git
metadata was written, no rebase was continued, and no deployment was performed.
The unmerged index is deliberately left for the user to stage and finish.
The user's `docs/briefs/36-hack-rebase.md` was not changed.

## Conflict resolutions

All fifteen originally conflicted files have their markers resolved:

- `Makefile`: both VC 4.05 images and Hack remain in generation, native linking,
  browser side modules and tests. Main's grouped lazy-file manifest and fixtures
  remain; Hack's native-only `HACK/` assets and browser-only `GAMES/HACK/` lazy
  references use separate installation paths without duplicate startup bytes.
- `runtime/dos_core.c`, `runtime/main.c`, `runtime/dos_fs.c`: preserve the
  VC 4.05 registry, separate settings, DOS2/COMMAND loader, native installer and
  door paths. Retain main's per-file first-open filesystem hooks and remove
  Hack's obsolete directory-load hooks.
- `runtime/web_programs.c`, `tools/web_demo.py`, `tools/web_modules.py`: one
  shared per-file transport and manifest, fifteen lazy translations, both VC
  4.05 programs and Hack in the shared web/door demo. Preserve WebCrypto hashing
  and its deadline/snapshot protections, with the portable SHA-256 fallback
  when WebCrypto is absent. Old directory bundles are retired recoverably.
- `tools/embed.py`, `tests/test_embed.py`, `tests/test_install_e2e.py`: preserve
  main's lazy-file scopes, VC image deduplication and VC 4.05 installation;
  retain native-only scope and Hack's executable/data/save preservation tests.
- `tests/test_translator_regression.py`,
  `tests/translator_support/ops_build.py`: retain VC 4.05 and add Hack to the
  complete instruction oracle; pin all seventeen earlier generated C sources.
- `tests/web_assets.mjs`, `tests/web_smoke.mjs`: preserve main's lazy-file,
  COMMAND, VC 4.05, recovery and memory tests and adapt Hack's older bundle
  assertions to first-screen metadata and first-open per-file content fetches.
- `README.md`: document both VC versions, Hack, first-open files and the native
  concurrent-playground policy together.

An additional non-conflicted merge bug was fixed in `runtime/rt.c`: the sixteen
registry entries did not fit the seventeen combined images. The expanded
native catalog tests fail twice against main's original capacity and pass
against the eighteen-entry registry. Evidence:
`build/hack-rebase-catalog-red.log`, `build/hack-rebase-machine.log`.

The longer browser Hack PATH also exposed an existing copied-code ambiguity
in `runtime/rt.c`. COMMAND's post-close loader block at image offset `4491h`
and its COM-allocation block at `43D1h` share their first seven bytes. The
extra environment paragraph put COMMAND's second copy 192 bytes from the
first, and the old three-byte matcher chose the wrong block. This attempted
adoption of PSP zero, leading to a false "Program too big" error. A clean
archived-main browser build passed the original test.

Copied-code candidates now prefer the longest approved byte match (up to
twelve bytes), retaining the required three-byte tail fallback and trying
another candidate when a matching address is not a translated instruction.
The Source inspector shares that ranking. No generated translation changed.
Two focused collision tests failed before the fix; all 32 machine tests pass
after it, including short-tail and rejected-entry fallback checks. Three
private mutants (seven-byte ranking, no short tails, no rejected-entry
fallback) fail their intended tests. Evidence:
`build/hack-rebase-copy-collision-{red,green}.log` and
`build/hack-rebase-copy-{seven-byte,short-tail,fallthrough}-mutant-red.log`.
All twelve original browser COMMAND cases and ten new PATH-padding cases
pass; padding 13 and 14 reproduced the pre-fix failure.

## Preserved bytes

The independently rebuilt `build/hack/HACK.EXE` remains **243,304 bytes** with
SHA-256:

```text
cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9
```

No compiled DOS shim or vendored Hack source was changed for this rebase.
The Hack build gate passed all 37 tests, including the separate-directory
reproducible rebuild. VC 4.05's build gate passed all 12 tests.

A read-only `git archive main` was extracted to
`/tmp/hack-rebase-main.KzNXHf`. Its own main sources rebuilt all earlier DOS
images and generated C with the supplied local toolchains. Literal `cmp`
commands then passed for all seventeen current sources:

```text
bootlogo command debug edlin fc find gwbasic gwbasic_graphics kermit
more rogue sort vc405 vc_com vc_ovl vcsetup405 vz
```

The exact commands, reference commit and results are in
`build/hack-rebase-earlier-c-cmp.log`; the independent main build is recorded in
`build/hack-rebase-main-generation.log`. This comparison does not rely on the
old Hack branch's saved baseline or merely compare executable behavior.
The final regenerated build passed the same seventeen literal comparisons,
the executable SHA-256 assertion, marker checks and `git diff --check` again
(`build/hack-rebase-final-integrity.log`).

## Concurrent native games

The fix is **separate per-process playgrounds**, implemented in the host runtime
so the pinned DOS executable remains unchanged. A nonblocking directory-inode
`flock` keeps the first VC in the original directory. Other live processes use
independently locked, persistent `PLAY0001` through `PLAY0064` subdirectories.
New slots receive only the bundled EXE, data and notices; no running game's
saves, levels, bones or scores are copied. Unmarked directories and symlinks
are left untouched.

The same VC retains its slot through save/quit/relaunch. Process death releases
the kernel lock, and existing saves survive. A secondary save can be selected
explicitly by entering its directory and running its EXE when the primary slot
is free. Long and non-CP866 host paths use the filesystem's actual DOS aliases.
Native-only directory leases also reject file operations if the selected
playground pathname is replaced while Hack runs.

All 26 Hack PTY tests passed: the 19 earlier tests plus native and DOS2-loader
concurrency, process-death reuse, long-config/cross-drive paths, renamed EXEs
under Unicode ancestors, preservation of unrelated directories/symlinks, and
replacement-directory save protection. Tests use temporary XDG storage and an
unchanged HOME sentinel. The simultaneous-save assertion was observed failing
before the fix. The filesystem suite grew by 55 checks covering directory
guards and long/Unicode aliases. Disabling the guard in a private source copy
caused nine relevant failures and passes again restored
(`build/hack-rebase-fs-guard{,-red,-restored}.log`).
The filesystem and terminal sanitizer targets also pass: 17,580 and 6,311
checks, respectively (`build/hack-rebase-extra-sanitizers.log`).

## Browser publication

Hack uses main's `runtime/web_files.c`, not a second bundle loader. Its EXE,
data and notices have complete names/sizes from the first screen. Listing,
renaming and rejecting removal of nonempty directories do not fetch content.
The first DOS open downloads only the requested file; execution separately
downloads the Hack translation. All fifteen side modules remain cached.

The measured complete first load is **925,381 gzip bytes**, **2,679 bytes**
above main's **922,702**. The gate allows at most 5,000 extra bytes and retains
the older absolute and reduction checks. The count includes all thirteen
startup assets, not just the wasm (`build/hack-rebase-test-web.log`). The main
wasm contributes 766,136 gzip bytes. Final page generation is `7663eeeb0df2`;
the fifteen immutable side names use generation `3762ba81f11a`.

The full smoke loaded all fifteen side modules without growing the 96 MiB
heap. Measured high-water was 53,799,200 bytes; the conservative any-order
bound, including portable-hash scratch space, was 79,029,201, leaving
21,634,095 bytes (more than the required 20 percent and 4 MiB).

The final `make web` and complete `make test-web` both exit zero. The latter
includes original Source/runtime/fixture/UI checks, all fifteen programs,
first-open/cache/rename/rmdir behavior, absent-WebCrypto modes, failed and
timed-out fetches, exhausted/fragmented-memory recovery, and all 22 COMMAND
regressions. Logs: `build/hack-rebase-web-final.log` and
`build/hack-rebase-test-web.log`.

The retained mutation runners reject thirteen main lazy-file defects, nine
Hack web defects and eleven VC 4.05 defects, each followed by its restored
green check. Their integration snapshots preceded the final copied-code
matcher repair; the complete final positive suite above uses that repair.
Logs: `build/hack-rebase-lazy-mutations.log`,
`build/hack-rebase-hack-web-mutations.log` and
`build/hack-rebase-vc405-web-mutations.log`.

## Aggregate gate record

The initial literal `make -k -j3 test` completed with only the two permitted
loopback-TCP failures: `test_kermit_dials_bbs_tcp` and `test-modem-transport`
cannot create sockets under this sandbox. Its translator suite passed all
347 tests, including 9,407,008 successful instruction states; the main PTY
group passed its other 334 tests. The full log is
`build/hack-rebase-make-test.log`. No test was weakened or made to silently
skip a socket failure.

After the copied-code repair, the complete native aggregate **exited zero**
with exactly those two loopback checks excluded by invocation, not source
changes. Its log is `build/hack-rebase-make-test-final.log`. Final results
include 334 main PTY cases (one TCP case deselected), 11 nested
COMMAND/IO cases, all 26 Hack cases, 36 door cases, and 23 confinement cases
with the same two kernel-ABI skips as main. The last two require Landlock
signal-scoping ABI 6; this kernel provides ABI 4
(`build/hack-rebase-kernel.log`). All 347 translator tests passed again in
845.77 seconds: 293,969 distinct instruction cases, 32 states each, and
9,407,008 successful states. All remaining build, embedding, modem, serial,
filesystem, EXEC, machine, process, terminal, CGA and door-memory targets in
`make test` also pass. Both required web targets exited zero as recorded
above. No required work remains apart from the user's Git staging/rebase step.

## Reproduction

```sh
export WATCOM="$PWD/build/openwatcom"
LSAN_OPTIONS=detect_leaks=0 PYTEST_ADDOPTS=-s make -k -j3 test

# Same complete native aggregate with only the brief's TCP exceptions omitted.
LSAN_OPTIONS=detect_leaks=0 \
  PYTEST_ADDOPTS="-s -k 'not test_kermit_dials_bbs_tcp'" \
  make -j3 -o test-modem-transport test

export EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten
export EM_CACHE="$PWD/build/emcache"
export PATH=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten:/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin:$PATH
make -j3 web
make -j3 test-web
```

`-k` allows independent native gates to continue after the sandbox's permitted
loopback-TCP failures; it does not hide them or change the tests. LeakSanitizer
cannot perform its ptrace-based exit check in this sandbox;
`LSAN_OPTIONS=detect_leaks=0` retains AddressSanitizer and UndefinedBehaviorSanitizer.

## Three weaknesses

1. `runtime/dos_core.c:1573`: playground allocation is per process, not per
   named player. A fresh VC takes the free primary directory first; restoring
   a saved secondary game then requires explicitly selecting
   `PLAYnnnn/HACK.EXE`. Slots persist and are not automatically cleaned up;
   automatic secondary allocation is bounded to 64 slots.
2. `runtime/web_programs.c:92`: the absent-WebCrypto SHA-256 fallback copies
   the whole file into temporary wasm memory and hashes synchronously. It is
   not streaming and can briefly occupy the browser thread; the measured
   memory bound includes its extra copy.
3. `runtime/hack_dos/save_io.c:82`: the pinned DOS save format checks length
   and an FNV checksum, not full structural validity. It catches accidental
   damage but does not make a deliberately forged pointer-rich save safe.
   This existing limit remains because the DOS executable is preserved.
