# Brief 31: DOS I/O verification

All red runs below were observed. The regression tests were added before their
corresponding fixes. Tests run in private `/tmp` fixtures; `HOME` in the pipe
tests means a fixture, never the developer's actual home. No git-writing
command was run.

## Initial failing tests

`make test-term` after adding `test_console_tabs` and
`test_console_handle_input`, before changing `runtime/bios.c`:

```text
FAIL [test_console_tabs] DOS TAB expands to the next eight-column stop:
  expected 0x0009, got 0x0005
FAIL [test_console_tabs] DOS TAB emits spaces, never the CP437 glyph:
  expected 0x0020, got 0x0009
FAIL [test_console_handle_input] DOS cooked CON handle read is handled
FAIL [test_console_handle_input] CON handle returns typed input:
  expected 0x0061, got 0x00A5
test_term: 44 groups, 6311 checks, 20 failures
```

The TAB regression specifically exposed AH=40h's `con_write` path. The other
three DOS entry points (02h, 06h, 09h) already expanded tabs; the test now keeps
all four consistent and independently checks that BIOS 10h/0Eh still emits one
CP437 glyph. Cooked input tests cover backspace editing, CR/LF, echo, short-read
buffer retention, Ctrl-Z, and a subsequent independent input operation.

`make test-fs` after adding the named-device, pipe, and FCB mutation tests,
before their implementations:

```text
stdin handle reads a cooked console line: read count 0, expected 12
open file: INT21 3d02 expected success ... AX=0002
NUL reports the DOS null-device bit
NUL reads EOF: read count 16, expected 0
NUL is not created on the host filesystem
COMMAND pipe creation succeeds on every drive ... AX=0005
pipe creation never truncates a file in HOME: selected host contents remain exact
pipe deletion never removes a file in HOME: host file exists
FCB 17 returns AL=00, got AX=17ff CF=0
FCB rename ? copies a padded blank from the source: host file exists
FCB 13 returns AL=00, got AX=13ff CF=0
FCB delete matches a one-letter extension
DOS filesystem tests: 17422 checks, 34 failures.
```

Two of those failures were downstream directory-count effects from the broken
NUL test creating a real fixture file. The assertions for the actual defects
are listed above. The tests preserve existing root files with the reserved pipe
names, use separate backing for C: and H:, and verify that NUL with an extension
or a drive/colon is still a device. FCB tests exercise `?` matching padded blanks,
matching exactly one nonblank position, copying the corresponding padded source
position on rename, extension matching, and read-only protection.

## COPY CON's additional FCB lookup

The first full Linux pty run of `tests/test_command_io_e2e.py` passed five cases
but failed COPY CON with `CON File not found`. Its trace showed a successful
CON open/IOCTL followed by AH=11h returning AL=FFh. COMMAND performs that FCB
lookup even for a device, before calling the cooked handle reader.

Before adding synthetic, literal-only device FCB results, the added
`test_fcb_device_lookup` produced:

```text
FCB 11 returns AL=00, got AX=11ff CF=0
FCB find reports literal CON so COMMAND can copy from it
FCB CON directory entry has DOS device attribute
FCB 11 returns AL=00, got AX=11ff CF=0
FCB NUL with extension remains a literal device lookup
DOS filesystem tests: 17485 checks, 5 failures.
```

After that fix, `make test-fs test-term` reported 17,485 filesystem checks and
6,311 terminal checks, both with zero failures.

A final FCB drive regression was also reproduced before fixing it: with C:
current, `H:HW?.TXT` renamed to unqualified `K??.BAK` returned AL=FFh instead of
using H: for the new names. The new test produced six failures (four direct
assertions and two downstream directory-count effects). A zero target-drive
byte now inherits the source drive; an explicitly different drive is refused.

## Door compatibility and native-copy audit

The conflict retained all main quota/inode declarations as well as the
branch's common rename helper. The pipe remapping deliberately does not apply
inside a door: its existing private root remains the descriptor-anchored,
quota-accounted location. No door operation can use the ordinary `/tmp` pipe
directory or expose an absent C: drive. NUL also creates no file there.

The audit found a related brief-30 regression: character-output redirection
used raw `write` instead of main's quota-aware writer. A dedicated test fills a
door file, redirects stdout, and exercises AH=02h/06h/09h. To reproduce the old
line without regressing a shared production file, an isolated copy at
`/tmp/vc31-io-red.Is8EkG/dos_fs.c` restores exactly the raw-write expression from
`50cd0aa:runtime/dos_fs.c:2703`. Its separately compiled test binary reports:

```text
FAIL ... redirected DOS character output cannot bypass the door quota
DOS filesystem tests: 17512 checks, 1 failures.
```

New cooked-input transfers use `guest_span`/`guest_write`, including the DOS
offset-wrap split. The PSP job-table bounds check now also uses `guest_span`;
individual JFT reads/writes remain single-byte operations within that checked
range. No new raw bulk guest-memory copy was found.

## Final local gates

```text
make test-fs
DOS filesystem tests: 17525 checks, 0 failures.

make test-term
test_term: 44 groups, 6311 checks, 0 failures

.venv/bin/python -m pytest -q tests/test_command_io_e2e.py
6 passed in 9.91s

ASAN_OPTIONS=detect_leaks=0 make test-fs-sanitize test-term-sanitize
DOS filesystem tests: 17525 checks, 0 failures.
test_term: 44 groups, 6311 checks, 0 failures
```

The filesystem sanitizer was rerun after the final FCB-drive fix; the terminal
sanitizer had already passed and its code was unchanged. The default leak
checker cannot run in this sandbox: it reports `LeakSanitizer does not work
under ptrace`. Disabling only leak detection retains address and undefined
behavior checks. No claim of a successful leak-sanitizer run is made.

The Linux pty cases exercise wide DIR, COPY CON twice, NUL with an existing host
sentinel, `dir | sort` on both C: and H:, and wildcard REN/DEL. Parent-task
verification records the complete `make test` and both browser gates, including
the independent browser I/O regressions.

## Existing Rogue gate and intentional CON behavior change

The complete native gate exposed two failures of
`test_rogue_read_only_directory_does_not_delay_qualifying_score`, one per launch
mode (captured in `/tmp/vc31-make-test.log`). Both displayed the qualifying
score immediately but waited at Rogue's second `[Press return to continue]`
prompt. `third_party/rogue/rip.c` prints that prompt and calls `fgets` after
`score()` has completed its lock/write attempt. The test's old comment explicitly
depended on the broken immediate EOF from DOS CON to dismiss that real prompt.

No runtime workaround was added. The test now requires the qualifying score and
the second prompt within the same four-second bound, acknowledges it, then
checks parent panels, the saved qualifying score, and absence of both scoreboard
and lock files. This preserves the read-only lock-delay regression while
requiring the newly correct cooked input behavior.

```text
.venv/bin/python -m pytest -q tests/test_rogue_e2e.py -k read_only_directory_does_not_delay_qualifying_score
2 passed, 27 deselected in 3.45s
```

## Existing Rogue gate and a fully occupied room

A subsequent complete gate caught another test-only assumption:
`test_rogue_failed_auto_resume_quarantines_save_and_starts_fresh[old-version-int2e]`
timed out even though Rogue had drawn its player, complete level-1 status, and
this real room (captured in `/tmp/vc31-final-passing-gates.log`):

```text
-+--
|S@|
|IE|
|IK|
|SB|
--+-
```

The player and seven monsters occupied every floor cell, so no `.` remained.
The old readiness predicate incorrectly required one. A deterministic copy of
that screen, normal-floor/corridor/deeper-level cases, and negative controls for
missing status/player/terrain, duplicate players, wrong first level, and
message-only punctuation were added before changing the predicate.

```text
.venv/bin/python -m pytest -q tests/test_rogue_e2e.py -k dungeon_predicate
# Before: 4 failed, 6 passed, 29 deselected in 0.11s
# After: 10 passed, 29 deselected in 0.09s
```

Readiness still requires the real status, exactly one player in the map, and
the requested first level. Terrain can be a floor, passage, door, or wall;
punctuation in message/status rows cannot qualify an incomplete screen. Only
the test predicate changed: no runtime, game, saved-state format, or seed change.

Full regression result (all 29 previous Rogue cases plus the ten controls):

```text
.venv/bin/python -m pytest -q tests/test_rogue_e2e.py
39 passed in 43.59s
```
