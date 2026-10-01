# Brief 31: EXEC and PSP verification

Recorded on 2026-10-02. The red executables below were compiled after resolving
the rebase conflicts, before applying the corresponding runtime fix. No shared
runtime source was temporarily regressed, and no git metadata was written.

## Red first

Compiled the new focused tests against the old implementation:

```text
cc -O1 -g -Wall -std=gnu11 -Iruntime tests/test_dos_exec.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c -o /tmp/brief31-exec-red
/tmp/brief31-exec-red nested-dos-loader
FAIL tests/test_dos_exec.c:511: !hle_redirect && registered == &image_edlin
/tmp/brief31-exec-red ah55-lifetime
FAIL tests/test_dos_exec.c:532: dos_fs_int21() && !cpu.cf
/tmp/brief31-exec-red ah55-abort
FAIL tests/test_dos_exec.c:560: dos_abort_untranslated()
/tmp/brief31-exec-red dos-loader
DOS EXEC: 209 checks passed
```

The lifetime failure exhausts system handles: a program duplicates its PSP
with AH55, switches back with AH50 (as DEBUG does for a loaded program), and
terminates without closing the duplicate's inherited references. The abort
failure creates two successive PSP descendants and transfers to untranslated
code. The direct-loader control passed, distinguishing the nested defect.

After fixing adoption, a negative nested EXEC test also caught the loader's
temporary BREAK OFF surviving an invalid-image error:

```text
cc -O1 -g -Wall -std=gnu11 -Iruntime tests/test_dos_exec.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c -o /tmp/brief31-nested-break-red
/tmp/brief31-nested-break-red nested-dos-loader
FAIL tests/test_dos_exec.c:536: dos_core_int21() && cpu.d.l == 1
```

Self-review also caught the unconditional VC image exclusion: a fault in a
VC started beneath COMMAND must be contained, while faults in the original
VC and its own overlay keep main's fatal diagnostic.

```text
cc -O1 -g -Wall -std=gnu11 -Iruntime tests/test_dos_exec.c runtime/dos_core.c runtime/dos_fs.c runtime/cp866.c runtime/cpu.c -o /tmp/brief31-nested-vc-red
/tmp/brief31-nested-vc-red nested-vc-abort
FAIL tests/test_dos_exec.c:608: dos_abort_untranslated()
```

The line numbers above are the test source at the time each red was recorded.

## Green

```text
./build/test_dos_exec nested-dos-loader
DOS EXEC: 94 checks passed
./build/test_dos_exec ah55-lifetime
DOS EXEC: 1064 checks passed
./build/test_dos_exec ah55-abort
DOS EXEC: 35 checks passed
./build/test_dos_exec nested-vc-abort
DOS EXEC: 62 checks passed
make -j2 test-exec test-process
door VZ entry quota: full-cap EXEC: passed
door VZ entry quota: repeated EXEC/terminate accounting: passed
DOS EXEC: 38390 checks passed
test_rt_process: 11 cases, 0 failures
.venv/bin/python -m pytest -q tests/test_command_nested_e2e.py
5 passed in 8.44s
```

The dispatcher tests cover direct and already-observed moved COMMAND copies,
reject changed live return bytes, reject unregistered copies, and run the full
forced-exit restoration path with an AH55 descendant current. The classifier
looks up registered images by name; it does not eagerly link or fetch the
browser's lazy COMMAND module.

The native interactive gates observe screen state, not delays:

- `dos2`, then `vc`: VC reports `Error reading overlay file.` and DOS exit 2;
  COMMAND accepts an echo and exits back to the outer VC. Both COMSPEC and
  INT2E launch paths pass. The test also accepts actual panels followed by F10
  if this DOS compatibility limitation is addressed later.
- `kermit`, then `push`: a nested COMMAND actually starts, echoes a marker,
  exits to Kermit, and Kermit exits to COMMAND and then the outer VC.
- `gwbasic`, then bare `SHELL`: the shipped BASIC prints `Syntax error` and
  returns to a fresh `Ok` prompt; SYSTEM returns through COMMAND. This is the
  allowed guest-error outcome, not successful execution of a nested shell.
  If a later interpreter does launch COMMAND, the test requires a new hosted
  COMMAND load, an interactive echo marker, and its termination before the
  current BASIC `Ok` prompt permits SYSTEM.
- `debug X.COM`, then `g`: the unapproved five-byte COM is rejected/stopped
  through the DOS error/child-abort path, and COMMAND plus the outer VC survive.

The native tests also cover the brief's expressly allowed DOS-error outcome;
they do not claim every original program can execute every nested operation.

The initial quoted-SHELL test allowed this BASIC error too. Its earlier
description as an actually successful BASIC child shell was incorrect;
an explicit interactive probe observed `SHELL`, `Syntax error`, and a new `Ok`
with no COMMAND load. The test and this evidence now state that outcome
directly. Nested-process prompt checks use the current last console line (excluding
BASIC's row-25 function-key labels), not a historical prompt elsewhere on
screen; nested child returns require a fresh termination event. The VC error check
identifies the exact newly loaded COM PSP and exit code 2, so an outer panel
redraw cannot be mistaken for a nested VC that needs F10.

Additional focused probes after tightening those waits:

```text
Stale-Ok probe: old ready=True; current basic_prompt=False; COMMAND prompt=True
Native Kermit PUSH: actual nested COMMAND load, echo, exit, and fresh Kermit return verified
```

The first probe supplies an old BASIC `Ok` above a currently active COMMAND
prompt, with BASIC's function-key labels still on row 25. The second records
that the live Kermit test actually took its interactive-shell echo branch,
rather than merely passing its guest-error alternative.

## Door regression

The door-memory fixture now defines the seven additional nonexecuting Image
stubs. Its overlap test preserves the byte-copy assertion and additionally
checks the inline JFT fields which AH55 now intentionally initializes.

The first unmodified `make test-door-memory` run compiled successfully but
LeakSanitizer failed with `LeakSanitizer does not work under ptrace`. With only
leak scanning disabled for this sandbox (ASan and UBSan remain enabled):

```text
LSAN_OPTIONS=detect_leaks=0 make test-door-memory
door memory: forged high MCB EXEC refused without a host overflow
door memory: overlapping AH=55h PSP copy is safe
door memory: cyclic MCB refused without an unbounded native walk
door memory: merged MCB size that wraps 64 KiB refused as a broken chain
door memory fuzz: 250000 calls; INT 21h=62500 10h=62500 16h=62500 14h=62500; EXEC 4639 (316 loaded); files created 93; seed=29d00f5afe123456
```

## Known limitation for the required final summary

`tests/test_command_nested_e2e.py:91`: Microsoft's DOS2 loader copies only
environment strings, without DOS3's executable-name trailer. The adopted VC
therefore reports its own overlay-file DOS error. This work contains that
failure and verifies recovery, rather than claiming nested VC panels in that
explicit COMMAND path. The brief permits the DOS-error outcome.
