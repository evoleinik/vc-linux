# Brief 19 verification

The existing uncommitted VZ work was preserved. No commit was made, and no
vendored VZ source, definition, or executable was edited.

## Red tests before fixes

Before production edits, `make build/vc` rebuilt the original worktree and its
binary was retained as `build/vc-brief19-before`. Its SHA-256 is
`7ccf6adcc6100920b8b894571f1b5b81262f7b8f9d8c111c5c1c68ec377c88c4`.
Original runtime sources were also retained outside the worktree for replaying
the final C tests without reverting anybody's changes.

The native end-to-end regression command was:

```sh
VC_TEST_BINARY="$PWD/build/vc-brief19-before" .venv/bin/python -m pytest -q \
  tests/test_vz_e2e.py \
  -k 'backup_save or external_replacement or deep_current_directory or failed_partial_save'
```

All 16 cases failed on the original binary, covering both COMSPEC and INT 2Eh
launches:

| Regression | Original behavior |
| --- | --- |
| Backup save, short and long names | VZ reported `is unwriteable`; the original pathname disappeared after the successful backup rename. |
| External atomic replacement, short and long names | VZ could not save the edited buffer to the replacement pathname. |
| Deep CWD, typed filename / empty tail / F4 | VZ started without the required directory refusal. |
| Partial save, installed defaults | A real host write stopped after 32,768 bytes and VZ reported `Disk full.`; `NOTE.BAK` was absent. |

The deep-directory fixture uses six native eight-character components under H:
(56-byte short CWD). Its F4 operand is `AA.TXT`, whose full 63-byte pathname
was accepted by the old F4 guard. All six final deep-directory cases were
replayed against the old binary: six failures, with VZ visibly starting.

Additional red evidence:

- `make test-fs` before the filesystem repair: 4,599 checks, 28 failures.
  Recompiling the final tests against the retained original `dos_fs.c` gave
  4,606 checks, 32 failures. Failures include backup recreation, regular-file
  replacement, independent directory identity, Save As after a missing leaf,
  and AH=60h / AH=71A8h alias lookup while the renamed leaf is absent.
- The independently selectable EXEC cases `vz-cwd-boundary`,
  `vz-command-paths`, `vz-command-limits`, `vz-wildcard-paths`,
  `vz-resident-temp`, and `vz-resident-leases` each exited 1 when the final
  `tests/test_dos_exec.c` was linked with the original runtime sources.
- Installer regression command: `VC_TEST_BINARY="$PWD/build/vc-brief19-before"
  .venv/bin/python -m pytest -q tests/test_install_e2e.py -k vz_backup_default`.
  Two failures: the exact old default retained `Eb-`, and a dangling definition
  symlink was replaced. Three preservation controls already passed.
- Running the strengthened `tests/web_smoke.mjs` against the original browser
  build passed stages 1–18, then failed stage 19:
  `an F4 save must leave the previous README in README.BAK`.

## Implemented behavior

- Leases validate each directory component independently. A regular-file
  replacement can be saved; changed directory/symlink identities and native
  short-name conflicts still fail safely.
- An owner rename drops the old held file identity but retains its short-to-long
  pathname reservation, so backup creation does not redirect or prevent the
  subsequent save, including for long host names.
- VZ refuses a short CWD whose length plus 13 reaches 64, even with no document
  argument. Its one-line DOS diagnostic is rendered before VC redraws panels.
  Document arguments become full short paths through AH=60h; paths at 64 bytes
  and rebuilt tails above 126 bytes are rejected. Final wildcard masks retain
  their meaning, and VZ's auxiliary selector lookup syntax is preserved.
- `tools/vz_defaults.py` generates `build/vz/VZ.DEF` with exactly the upstream
  backup-option byte changed to `Eb+`. Native embedding and the browser demo
  share this default. Only an exact old default is migrated, atomically and
  preserving permissions; customized definitions and symlinks remain intact.
- Resident exits release launch leases and remove the private VZ temporary
  directory, while retaining ordinary DOS handles belonging to generic TSRs.

## Final gates

| Gate | Result |
| --- | --- |
| `make test` | PASS: 248 translator tests, 176 PTY/install tests, 14 VZ build tests, and all C/INI suites. |
| `make vz` | PASS: byte-identical 55,856-byte COM; generated definition has `Eb+`. |
| `make web` | PASS: release WebAssembly build with the on-disk Emscripten SDK. |
| `make test-web` | PASS: asset versions, speaker, graphics, and all 21 smoke stages, including the original README retained in `.BAK`. |

The remaining native counts are 23 machine cases, 5,822 terminal checks,
823,877 CGA checks, and three INI tests; all passed. The translator suite took
260.53 seconds, and the full PTY/install suite took 298.56 seconds.

The browser gates used the toolchain from Brief 10, with no network access:

```sh
export EMSDK=/home/eo/src/vc-linux-wt/emsdk
export EM_CONFIG="$EMSDK/.emscripten" EM_CACHE="$PWD/build/emcache"
export PATH="$EMSDK/upstream/emscripten:$EMSDK/node/20.18.0_64bit/bin:$PATH"
make web
make test-web
```

## Additional checks and limits

The filesystem suite passes 4,641 checks, and the EXEC suite passes 2,846
checks. The process suite passes nine cases. AddressSanitizer and UBSan also
pass for the filesystem and EXEC suites with `ASAN_OPTIONS=detect_leaks=0`;
LeakSanitizer cannot run under this sandbox's ptrace restrictions. Descriptor
accounting tests independently check lease cleanup.

The optional graphical-browser check could not run: the sandbox disallows the
local HTTP socket and the browser automation daemon. The required web gate
uses the actual WebAssembly build under Node/MEMFS instead.

Three limitations of this work:

1. The path checks are at launch, not instrumentation of every later path typed
   into VZ's own dialogs (`runtime/dos_core.c:1044`).
2. Migration infers provenance from exact bytes: deliberately restoring the
   entire original `Eb-` definition is indistinguishable from an untouched old
   installation and will migrate again (`runtime/main.c:121`).
3. The partial-write test uses Linux `RLIMIT_FSIZE`, not a physically full disk
   or a power-loss test (`tests/test_vz_e2e.py:472`). Recovery from the intact
   backup is manual; the failed destination remains partial.

## Files changed for this brief

- `Makefile`, `README.md`
- `runtime/dos_core.c`, `runtime/dos_fs.c`, `runtime/dos_fs.h`, `runtime/main.c`
- `tests/test_dos_exec.c`, `tests/test_dos_fs.c`, `tests/test_install_e2e.py`,
  `tests/test_vz_e2e.py`, `tests/web_smoke.mjs`
- `tools/vz_defaults.py`, `tools/web_demo.py`
- `docs/verification/19-vz-fixes.md`
