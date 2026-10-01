# Brief 30: runtime and native verification

All filesystem fixtures, test homes and logs were under `/tmp`; no command
wrote to the real home directory. No network, browser, live BBS or commit
was used. See [build evidence](brief30-build.md),
[instruction evidence](brief30-translation.md), and
[browser evidence](brief30-web.md).

## Runtime changes

- A single SETVER data table selects DOS 2.00 by the byte-verified image,
  not the filename. The seven programs retain that version when renamed;
  VC, GW-BASIC and other programs retain DOS 7.10. The true-version service
  remains 7.10, and the parent version is restored after termination.
- PSP job-file tables now map DOS handles to reference-counted system
  files. COMMAND redirects by directly exchanging entries in its PSP,
  rather than using the DOS duplicate-handle API. Nested children inherit
  those mappings, and termination releases their references. AH=67h grows
  the table into a real, PSP-owned DOS memory block; DOS children still
  inherit only the first twenty entries. Full-table errors are checked
  before creating or truncating a file.
- COMMAND's original IBM/OEM branch contains its own DOS EXEC loader.
  Its AH=55h child-PSP operation is adopted only after the complete,
  just-closed executable and its relocated live bytes both match a known
  translation. The original DOS saved register/interrupt frame is retained
  for child return. Unknown or damaged loaded bytes return a DOS error;
  there is no instruction decoder or arbitrary-code interpreter.
- DOS 1.x FCB directory search supplies ordinary/extended directory
  entries and generation-checked, process-owned continuation tokens.
  Literal FCB rename uses the same no-overwrite host rename as handle DOS.
  The native command bridge prepares default PSP filename blocks using
  DOS's existing FCB parser. EDLIN needs those blocks and FCB rename for its
  input, temporary output and `.BAK` sequence.
- Buffered console input accepts EDLIN's literal Ctrl-Z terminator;
  DOS character I/O honors redirected files. Browser command operators
  are handled by the preserved shell, with the existing safe native
  helpers/associations retained. Linux's parent COMSPEC remains `/bin/sh`;
  only a secondary COMMAND child gets its own reload path in COMSPEC.

## Tests observed red before their fixes

| Gate | Actual old-code failure | Restored result |
| --- | --- | --- |
| FCB first/next search | 10 failures, including AH=11h returning FFh and missing directory entries | Filesystem suite passed |
| FCB dot directory | Dot name incorrectly encoded as a file extension; one failure | Filesystem suite passed |
| Default filename blocks | `EDIT    TXT` absent at PSP:5Dh | EXEC suite passed |
| EDLIN save / FCB rename | Four FCB tests failed; real editor left `old line` unchanged | Exact edited file and `.BAK` readback passed |
| Literal Ctrl-Z input | Two buffered-input assertions failed | 5,824 terminal checks passed |
| COMMAND startup | Assembled conditional return jumped to itself | Both native shell launch modes passed after the isolated assembler fix |
| SORT buffer allocation | Real SORT printed `SORT: Insufficient memory`; redirected output was empty | Both ascending and `/R` output passed after the build-time loader-header policy |
| Full JFT: AH=3Ch/6Ch | Failed create/truncate erased the existing 16-byte fixture | Both calls return DOS error 4 without changing bytes |
| Full JFT: AH=5Bh/5Ah | Failed new/temporary create left an unwanted file | Neither call creates a file |
| AH=67h capacity | Reported success without increasing the 20-entry PSP table | A 40-handle request permits 40 real opens; excessive requests fail |
| AH=55h after JFT growth | Copied 64-entry count into a child's 20-entry inline table | Child inherits twenty slots; parent handle 39 remains readable |
| Rejected child / BREAK ON | Early unwind left the global setting OFF | Both initial settings and legitimate child changes are preserved |

The final FCB suite passed **4,701 checks**, EXEC **4,321 checks**, terminal
**5,824 checks**, machine **28 cases**, process **9 cases**, and CGA
**823,877 checks**. Modem, serial-machine and supplemental syscall tests
also passed without sockets.

## Deliberately planted runtime defects

Each following one-line production defect was applied separately, the
named executable test was run and observed failing, then the original
line was restored and the same test passed. Logs were retained as
`/tmp/brief30-mutation-<name>-{red,green}.log`.

| Name | Planted defect | Test command | Red / restored |
| --- | --- | --- | --- |
| `setver-guest` | COMMAND table entry reports 7.10 instead of 2.00 | `./build/test_dos_exec setver` | exit 1 / exit 0 |
| `setver-unlisted` | Table fallback reports 2.00 instead of 7.10 | `./build/test_dos_exec setver` | exit 1 / exit 0 |
| `jft` | Ignore the live PSP mapping and use raw handle numbers | `./build/test_dos_exec jft` | exit 1 / exit 0 |
| `dos-loader` | Accept loaded memory without comparing its relocated bytes | `./build/test_dos_exec dos-loader` | exit 1 / exit 0 |

The loader test separately corrupts the loaded memory and replaces the
complete backing file; both must reject and restore the original EXEC
caller. The JFT test verifies shared read position through a nested child
and runs 80 unclosed-child-handle cycles without exhausting the table.
Additional per-program native planted defects are documented in
[native negative controls](brief30-native-negative-controls.md). The late
independent review and its separately planted full-JFT, growth, inheritance
and BREAK defects are recorded in [runtime review](brief30-runtime-review.md).

## Real native programs

```
WATCOM=$PWD/build/openwatcom .venv/bin/python -m pytest -vv tests/test_msdos_e2e.py
10 passed in 15.51s
```

The cases run COMMAND through both COMSPEC and INT 2Eh, verify DIR, ECHO,
TYPE, COPY with file readback, both batch lines, VER and EXIT with exact
panel/video-memory redraw, and prove Linux host-shell operation afterward.
They also run a renamed COMMAND, COMMAND `/C`, EDLIN insert/save/backup,
FIND, MORE, SORT ascending/reverse, FC binary comparison and DEBUG prompt/Q.
Utility logs prove COMMAND's original DOS-hosted loader ran the translated
children; output files are compared byte-for-byte. MORE deliberately emits
an initial CRLF, and DOS ECHO preserves the space before redirection.

## Sanitizers

The optional ASan/UBSan terminal, filesystem and EXEC suites also passed with
`ASAN_OPTIONS=detect_leaks=0`. An initial default run failed because this
sandbox prevents LeakSanitizer's ptrace-based thread inspection; leak
detection is not claimed. Address and undefined-behavior checks remained
enabled (4,701 filesystem, 5,824 terminal and 4,321 EXEC checks).

## Aggregate gates

The canonical native aggregate was rerun after the final runtime review and
exited **0**:

```sh
WATCOM=$PWD/build/openwatcom \
PYTEST_ADDOPTS="-k 'not test_kermit_dials_bbs_tcp'" \
make -j4 -o test-modem-transport test
```

The log is `/tmp/brief30-make-test-final.log`. Translator tests passed
**317 in 507.85s**, including the 6,478,944-state instruction oracle. Native
end-to-end tests passed **261 in 406.68s**, with exactly one TCP case
deselected. The 25 MS-DOS build tests and 64 embedding tests passed too;
all earlier build, unit and non-socket modem/serial gates remained green.
After completion, all eight earlier generated C files again passed `cmp`
against their pre-task snapshots. `git diff --check` also passed.

Only the brief's two explicitly reserved TCP tests were excluded, without
changing or weakening either test:

- `make test-modem-transport`
- `.venv/bin/python -m pytest -q tests/test_kermit_e2e.py::test_kermit_dials_bbs_tcp`

Those remain for the user to run outside the loopback-blocking sandbox.

The final frozen-runtime browser build and `make test-web` passed. First
load is **1,296,214 gzip bytes**, below the unchanged **1,300,000-byte** cap.
All twelve lazy side modules pass cold/cached launches, fetch failure,
timeout and fixed-memory recovery. Peak heap use is 42,986,320 bytes of
67,108,864. See [browser evidence](brief30-web.md) for every individual
gate and the six planted-defect/restoration cycles.

## Three remaining weaknesses

- `runtime/dos_fs.c:1810`: the filename parser used by FCB rename rejects
  wildcard names. Literal EDLIN save/backup renames work, but DOS wildcard
  FCB renaming is not implemented.
- `translator/msdos.py:74`: SORT's finite self-modifying branch proof is
  intentionally tied to this source build's exact surrounding instructions.
  A later upstream layout requires a new manual proof and regression tests.
- `tests/web_smoke.mjs:674`: MORE is checked with a three-line redirected
  input. Interactive multi-page prompting and continuation are not covered
  by the new browser smoke gate.
