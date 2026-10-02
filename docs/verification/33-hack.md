# Brief 33: Hack 1.0.3 verification

This is the historical pre-rebase record. The current lazy-file integration,
native concurrency fix and verification results are recorded in
[Brief 36](36-hack-rebase.md).

Recorded 2026-10-02. Work used only the supplied source snapshot and locally
installed toolchains. No network, browser, deployment, commit, or git metadata
write was used. The user's pre-existing `docs/briefs/33-hack.md` was not edited.

## Preserved artifact

`build/hack/HACK.EXE` is **243,304 bytes** with SHA-256:

```text
cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9
```

All 92 supplied NetBSD `games/hack` files from commit
`f037b5fcaa6db302271bbb445039a3a513b2e28c` (2026-08-14) were copied unchanged.
`third_party/hack/UPSTREAM` records provenance; the independently pinned full
manifest is `runtime/hack_dos/UPSTREAM.sha256`. All 92 direct `cmp` checks pass
(`build/hack-upstream-cmp.log`).

The build uses the same pinned OpenWatcom `2026-10-01-Build` as Rogue, with
`-0 -ml` for 8086 real-mode DOS and a 16 KiB stack. Build-directory copies
receive counted compatibility transformations; vendored files never do.
The build test independently rebuilds into another directory and compares
the complete EXE, not just its MZ header. Stable relative compiler input
paths prevent upstream `assert()`'s `__FILE__` from leaking build paths.
Compiler clock macros are rejected before compilation.

The existing compiled-C front end translates the game, DOS shims and linked
CRT without production translator changes. Fifteen earlier generated C files
compare byte-for-byte with the copies saved before this work:

```text
bootlogo command debug edlin fc find gwbasic gwbasic_graphics
kermit more rogue sort vc_com vc_ovl vz
```

The actual comparison commands and results are recorded in
`build/hack-earlier-c-cmp.log`; `tests/test_translator_regression.py` also pins
their complete source hashes.

## DOS substitutions and storage

The build excludes upstream `hack.unix.c`, `hack.tty.c`, `hack.ioctl.c` and
`hack.terminfo.c`. Project-owned replacements provide:

- `terminal.c`: BIOS screen/cursor/keyboard operations and IBM PC wall glyphs.
- `platform.c`: BSD-compatible full-width random numbers, DOS identity/date
  helpers and save-time far-pointer rebasing.
- `startup.c`: executable-relative playground selection and restoration of
  both changed drive directories and the caller's active drive on exit.
- `save_io.c`: a versioned length/checksum header, persistent-file validation
  and recoverable quarantine of invalid saves.
- `compat.h`, `hack_config.h` and `sys/*.h`: the small DOS build interface.

Native installation uses `$XDG_CONFIG_HOME/vc-linux/HACK` (default
`~/.config/vc-linux/HACK`); `hack` is on VC's DOS PATH. Offline PATH, executable,
manual-page and local package checks found no host `hack` command to shadow
(`build/hack-native-command-name.txt`). Browser and door files are under
`H:\GAMES\HACK`, with the door's files confined to its private session.
Data, `HACK.SAV`, level files, bones and scores remain beside the executable.
Existing user-owned data and save files are not overwritten by installation.

Hack code and files are separate browser first-use downloads. Moving the two
existing H: assembly source files to the same lazy-file mechanism keeps the
unchanged 1,300,000-byte first-load limit. The final startup is **1,275,545
bytes gzipped**, leaving **24,455 bytes**. Directory moves/deletion correctly
materialize unopened content; SHA-256 verification also works without
WebCrypto. The fixed wasm memory is 96 MiB, including the tested any-order
side-module fragmentation allowance. See [the browser record](33-hack-web.md).

The HACK BBS door is added beside VC and ROGUE. The maximum configured twelve
callers retain 8 MiB logical session limits; the tmpfs budget is 192 MiB,
including the existing two-times physical-allocation allowance.

## Gate record

All runnable gates passed on the final pinned image. The aggregate
`make -k test` returned **2**, solely because the sandbox forbids the two
required loopback TCP checks. It is not recorded as a fully green `make test`.
The failures were left intact:

- `tests/test_kermit_e2e.py::test_kermit_dials_bbs_tcp`: `socket()` raised
  `PermissionError: [Errno 1] Operation not permitted` before a connection.
- `test-modem-transport`: its required listener printed `Operation not
  permitted` and failed the existing `fd >= 0` assertion.

Results (`build/hack-make-test.log`):

- `make test-translator`: 335 passed. Unicorn checked 266,580 distinct
  relocation-aware instruction cases in 32 deterministic random states each:
  8,530,560 successful states, including 5,129 divide-error cases. The gate
  also records 9,452 code-overlapping states resampled before execution.
- `tests/test_hack_build.py`: 37 passed, including independent reproducibility.
- The main native E2E recipe: 289 passed, with only the one required TCP
  failure above. This includes installation, all earlier programs, and the
  separately identified supplemental pipe-based Kermit workflows.
- Final optimized `build/vc`: all 19 Hack PTY tests passed, covering startup by
  command/Enter, player and full status, movement, moved-game save/restore,
  Q back to live VC, invalid saves, renamed/modified EXEs and directory churn.
- The separate nested-command and command-I/O recipes also passed: 30 tests
  total with Hack (`build/hack-final-extra-e2e.log`). These recipes are run
  separately because the required TCP failure stops the earlier E2E recipe.
- `make test-web`: passed, including all thirteen translated programs and
  the four focused WebCrypto-free directory/game scenarios.
- `make web`: passed, including a final no-op recheck of the publication.
- Earlier-program `cmp`: 15 passed; upstream file `cmp`: 92 passed.
- Door gameplay/command tests: 36 passed, including direct Hack startup/status,
  save/restore from VC on session H:, and complete session cleanup.
- Door confinement: 23 passed, 2 existing kernel-capability skips. The kernel
  offers Landlock ABI 4; the skipped cross-domain signal-scoping tests require
  ABI 6 (Linux 6.12+). No new skip was added. `build/hack-door-confinement.log`
  records the independent capability result and exact skip reason.
- Door ASan/UBSan: all five cases passed, including 250,000 fuzz calls.
- Embed: 69 passed; door packaging: 35 passed. Earlier reproducible builds:
  Rogue 51, VZ 14, Kermit 9, MS-DOS utilities 25 passed; INI 3 passed.
- DOS filesystem: 17,525 checks; EXEC: 38,390; terminal: 6,311; CGA: 823,877.
  Machine: 28 cases; process: 11. Modem/UART/serial and mocked native transport
  checks passed. Portable SHA-256's four standard vectors passed.
- A final `make gen build/vc` was a successful no-op, with the EXE hash and
  all 107 direct source/generated-source comparisons checked again afterward.

The final native run uses:

```sh
export WATCOM="$PWD/build/openwatcom"
LSAN_OPTIONS=detect_leaks=0 PYTEST_ADDOPTS=-s make -k test
```

`-k` allows independent targets to finish after the expected socket denial;
it does not ignore errors. The required TCP gates are not skipped or changed.
This sandbox rejects loopback `socket()` with `Operation not permitted`.
LeakSanitizer also cannot perform its ptrace-based exit inspection here;
`LSAN_OPTIONS=detect_leaks=0` leaves the required AddressSanitizer and
UndefinedBehaviorSanitizer instrumentation enabled.
The remaining TCP and signal-scoping coverage requires a less restricted
environment and a sufficiently new kernel; no network or deployment was used
to bypass these restrictions.

## Deliberate failures and recovery

Every planted defect uses a private build, temporary publication, private
Unicorn memory or a restored test-local binding. Production sources,
generated C, installed user files and the official EXE remain unchanged.
All **37 deliberate defects** below failed their intended gates; all restored
checks passed (10 DOS + 5 translation + 8 native/door + 9 web + 5 gameplay).

### DOS build and shim gates

`tests/fixtures/hack-build-mutations.py`: ten genuine failures with
`HACK_MUTANT=1`, followed by ten passes with `HACK_MUTANT=0`.
The complete observations and reproduction commands are in
[the DOS mutation record](../../runtime/hack_dos/MUTATIONS.md).

| Defect | Gate that rejected it |
| --- | --- |
| Lose the random number's high word | Known full-width BSD sequence |
| Return ASCII walls | IBM PC wall glyph mapping |
| Skip saved monster-pointer rebasing | Relocated far pointer |
| Skip saved sickness/timeout restoration | Reconstructed pointers/callbacks |
| Accept every corrupt save | Persistent-file integrity |
| Flip the EXE's final byte | Pinned complete executable SHA-256 |
| Edit a private upstream `data` copy | Exact 92-file snapshot |
| Remove counted patch enforcement | Duplicate source span rejection |
| Remove compiler-clock enforcement | Unpinned `__TIME__` rejection |
| Skip the exit callback | Parent drive/directory restoration |

### Translation gates

`tests/fixtures/hack-translator-mutations.py` supplies five independent
negative probes: five failed with defects, then five passed restored.
Logs are `build/hack-translator-mutations-{red,green}.log`.

| Defect | Gate that rejected it |
| --- | --- |
| Drop the compiled-module coverage set | Complete game/shim/CRT map coverage |
| Disable missing-callback validation | Save command target must be translated |
| Disable linked-byte proof | Mutated opcode must fail independent OMF proof |
| Omit Hack from Unicorn cases | Complete Hack instruction coverage |
| Append bytes to the tested earlier C digest input | Earlier Rogue C identity |

### Native installation and doors

Eight private defects failed the intended public assertions, then the same
unmodified gates passed. Evidence is in `build/hack-native-mutations.txt`,
`build/hack-native-runtime-mutations.txt` and
`build/hack-native-installer-mutation.txt`.

| ID | Defect | Gate that rejected it |
| --- | --- | --- |
| N01 | Pack native-only bytes into the web payload | Native-only embed exclusion |
| N02 | HACK wrapper launches Rogue | Door command selection |
| N03 | Omit `perm` | Shared complete demo manifest |
| N04 | HACK menu passes the Rogue selector | BBS menu/wrapper agreement |
| N05 | Revert tmpfs to 128 MiB | Configured caller storage budget |
| N06 | Remove the trusted Hack EXE reference | Real door startup/status |
| N07 | Remove Hack from door PATH | Typed Hack startup |
| N08 | Refresh all Hack data as program bytes | Preserve existing user-owned data |

### Browser gates

All nine isolated defects were rejected and every restored gate passed.
[The browser record](33-hack-web.md) lists each mutation, its exact diagnostic,
reproduction command and log: wrong SHA state, wrong side-module export,
corrupt publication hash, eager first-load download, missing image registry,
missing SRC materialization, missing moved-parent materialization, incorrect
RMDIR of unopened content, and corrupt runtime bundle bytes without WebCrypto.

### Gameplay gates

`tools/hack_gameplay_mutations.py` independently suppresses Hack's status,
movement, save, restore lookup and quit behavior. Each deliberately broken
runtime links against the ordinary final generated objects. The actual public
PTY test fails against it, then passes against the unmodified optimized
`build/vc`. No terminal expectations are changed.

| Defect | Actual failed observation | Restored result |
| --- | --- | --- |
| Blank BIOS writes to the status row | First level lacks the required status | 1 passed |
| Replace movement keys with Escape | Player never leaves the original tile | 1 passed |
| Replace S with Escape | Save never exits to VC | 1 passed |
| Hide HACK.SAV from read-only open | Restarted map/status is not the saved game | 1 passed |
| Replace Q with Escape | Quit confirmation never appears | 1 passed |

The runner completed with exit 0; every red child returned 1 and every
restored child returned 0. Logs are `build/hack-gameplay-mutations.log` and
`build/hack-gameplay-mutations/{status,movement,save,restore,quit}-{red,green}.log`.

### Actual bugs caught before the fixes

- Independent output-directory builds initially differed by 20 bytes because
  upstream assertions embedded absolute compiler input paths. Relative input
  paths restored byte identity.
- Hack initially left VC inside its playground after Q or S. The PTY tests
  caught the changed directory and failed subsequent relative commands.
- Classic DOS short aliases can be renumbered when a sibling directory is
  created. Four Q/S × COMSPEC/INT2E tests failed against the old executable
  and pass with long-directory capture/restore. Logs are
  `build/hack-cwd-alias-regression-red.txt` and `build/hack-final-pty-smoke.log`.
- Watcom `int86x` treats `cflag` as output-only. Four additional real-machine
  tests failed on the exact old EXE when unsupported DOS left carry unchanged.
  The documented `intrf` primitive actually presets carry; all four now pass.
- The first browser lazy-loader build exceeded the first-load cap. Making H:
  source files lazy recovered the budget without changing the cap or assets.
- Unopened SRC/parent renames and RMDIR initially lost or deleted lazy content;
  dedicated Node tests failed before the directory hooks were corrected.
- The initial browser bundle verifier assumed WebCrypto existed. The
  crypto-absent tests failed; portable SHA-256 and standard vectors fixed it.

## Files changed

- Preservation: `third_party/hack/` (92 unedited files plus `UPSTREAM`).
- DOS port: the twelve files under `runtime/hack_dos/` listed above and its
  README, manifest and mutation record; `tools/build_hack.py`,
  `tools/hack_port.py`, `tools/hack.mk`.
- Build/runtime: `Makefile`; `runtime/{image.h,rt.c,main.c,dos_core.c,dos_fs.c,
  web_programs.c,web_programs.h,web_sha256.c,web_sha256.h}`.
- Packaging: `tools/{embed.py,web_demo.py,door_demo.py,web_modules.py}`;
  `infra/bbs/enigma.yaml`; `infra/bbs/doors/{run-door.sh,
  notanemulator_bbs-doors.hjson}`.
- Tests: `tests/test_hack_build.py`, `tests/test_hack_e2e.py`,
  `tests/test_translator_hack.py`, `tests/test_web_sha256.c`;
  `tests/test_{door_e2e,door_packaging,embed,install_e2e,translator_ops,
  translator_regression}.py`; `tests/test_{door_memory,dos_exec,rt_process}.c`;
  `tests/translator_support/ops_build.py`; `tests/web_{assets,modules,size,
  smoke}.mjs`; `tests/fixtures/hack-{build,translator}-mutations.py`;
  `tools/hack_gameplay_mutations.py`, `tools/hack_web_mutations.mjs`.
- Documentation: `README.md`, `web/README.TXT`, `infra/bbs/doors/README.md`,
  this file and `docs/verification/33-hack-web.md`.

## Three weaknesses

1. `runtime/hack_dos/platform.c:94`: native instances sharing one Hack
   directory share fixed level/save names. Concurrent play there is unsafe;
   use separate config/playground directories. Browser/door sessions are private.
2. `runtime/hack_dos/save_io.c:82`: length and FNV checksum catch accidental
   damage, not a deliberately forged pointer-rich save with a recomputed
   checksum. Full structural validation of upstream recovery is not implemented.
3. `runtime/hack_dos/terminal.c:67`: bell and output-delay hooks are no-ops,
   so this port does not preserve the original audible/timing cues.
