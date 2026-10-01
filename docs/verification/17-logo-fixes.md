# Brief 17 verification

This work extends the pre-existing, uncommitted Logo/CGA changes. No commit was made.
The starting native binary and browser bundle were preserved before implementation
at `/tmp/vc-logo-fixes-before.yTasX8` for the old-code integration checks.

## Test-first evidence

### 1. Unchanged CGA frames

Added the decode-count hook and regressions before adding the source cache.
`make test-term` first reported, in each of modes 4, 5 and 6:

```text
idle INT16 polls perform no further CGA decodes: expected 0x0001, got 0x0021
test_term: 36 groups, 5768 checks, 3 failures
```

The expanded tests also failed before the cache implementation:
`38 groups, 5822 checks, 23 failures`. They include both video banks, aperture
padding, direct mode/palette writes, sink/invalidation changes, headless rendering,
and unchanged dump-file identities. Decode counts and held-open dump inodes are
used instead of timing measurements.

After implementation: `test_term: 38 groups, 5822 checks, 0 failures` and
`CGA BIOS: 823877 checks, 0 failures`.

### 2. Ctrl-Break and dispatcher input

Before the runtime fix:

```text
make test-process: 8 cases, 4 failures
make test-term: 5 Ctrl-Break chord failures (expected no key, got 0000h)
```

The process failures covered default/inherited INT 1Bh, a blocked DOS read, and
a child-owned handler whose input was polluted by a NUL word. A later machine
regression was also run red before removing native terminal I/O from `rt_yield`.

The required PTY regression was added and run with the then-current `logo`
installation name against the preserved original binary:

```text
VC_TEST_BINARY=/tmp/vc-logo-fixes-before.yTasX8/vc \
  .venv/bin/python -m pytest -q tests/test_logo_e2e.py -k ctrl_break
4 failed, 17 deselected in 44.41s
```

All four combinations (Ctrl-Pause/Ctrl-Shift-B, COMSPEC/INT 2Eh) drew the busy
loop but timed out waiting for VC's return confirmation after Ctrl-Break.

The new Node regression was also run before the runtime fix, using the preserved
original module. All 16 pre-existing checks passed, then the external watchdog
reported:

```text
FAIL: guest event loop stalled while waiting for VC return confirmation after Ctrl-Pause
```

The regression had already observed a new canvas frame and the loop's distant
pixel at (160,40). Its watchdog lives in the parent thread, so a frozen wasm/JS
event loop cannot disable the timeout itself. The final test additionally queues
ordinary typeahead during the loop, checks that input polling continues, exercises
both break shortcuts, and verifies that H: contents survive without a reload.

After implementation, the focused PTY run passed all six Logo/BASIC break cases.
Machine/process checks passed 22/9 cases, including a fake-clock <=50 ms polling
bound, restored parent state, blocked-read continuation, and no synthetic NUL.

### 3. BOOTLOGO installation

Installer and web-demo regressions were added before renaming:

```text
VC_TEST_BINARY=/tmp/vc-logo-fixes-before.yTasX8/vc \
  .venv/bin/python -m pytest -q tests/test_install_e2e.py \
  -k 'bootlogo or leaves_logo or retires_only or retired_files'
11 failed, 9 deselected in 2.23s
```

Failures included the missing `BOOTLOGO.COM`, interception of the host `logo`
command, legacy-image handling, and old browser asset names. After implementation
the installer suite passed all 20 cases, including fresh/upgrade web-demo builds.

Integration then caught a reference-binding dependency: `Image.name` retained
`LOGO.COM`, while the embedded asset was now `BOOTLOGO.COM`. Renaming the EXEC
fixture's embedded entry to match production reproduced this before the binding
fix:

```text
FAIL tests/test_dos_exec.c:219: registered == &image_bootlogo && !host_runs
make: *** [Makefile:111: test-exec] Error 1
```

The corrected binding selects the trusted embedded reference by image identity;
candidate executables are still checked by complete size and bytes. Canonical
`bootlogo` launches, renamed copies, and changed-byte rejection are covered.

## Required gates

All required commands exited successfully:

| Gate | Result |
| --- | --- |
| `make test` | 235 translator tests; 4,317 filesystem checks; 22 machine cases; 9 process cases; 5,822 terminal checks; 823,877 CGA checks; 3 INI tests; 125 PTY/e2e tests. EXEC also passed; its final expanded name/byte-identity check was rerun separately: 1,162 checks. |
| `make web` | Emscripten `-O2`, narrow Asyncify, offline demo prepared successfully. |
| `make test-web` | Asset hashes, speaker, canvas renderer, and all 19 Node smoke stages passed. |

The web commands used the toolchain from Brief 10:

```sh
EMSDK=/home/eo/src/vc-linux-wt/emsdk \
EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
EM_CACHE=/home/eo/src/vc-linux-wt/logo/build/emcache \
PATH=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten:/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin:$PATH \
make web
# Same environment for make test-web.
```

| Wasm size | Before | After | Growth |
| --- | ---: | ---: | ---: |
| Raw bytes | 7,356,456 | 7,358,052 | 1,596 bytes (0.022%) |
| `gzip -c` bytes | 1,586,736 | 1,588,738 | 2,002 bytes (0.126%) |

Raw growth is well below the 10% Asyncify warning threshold in `CLAUDE.md`.
Generated guest instruction code and byte matching remain unchanged; the new
browser sleep is on the direct dispatcher chain, never in `rt_yield`.

## Browser and sanitizer checks

A real-browser check using the agent-browser skill was attempted. This sandbox
denied the local HTTP server's socket creation and prevented agent-browser's
daemon from starting, including with a writable temporary socket directory.
The required Node/MEMFS smoke test does not need a server or browser daemon.

The machine and process harnesses passed AddressSanitizer/UndefinedBehaviorSanitizer
checks. `ASAN_OPTIONS=detect_leaks=0 make test-term-sanitize` also passed all 5,822
terminal checks. Leak detection was disabled because LeakSanitizer is unsupported
under this environment's ptrace setup.

Scoped `git diff --check` passed. The repository-wide check reports only existing
vendored whitespace/ASCII-art warnings in files not edited for this brief.

## Files changed for this brief

- `runtime/term.c`, `runtime/term.h`: source cache and decode-count hook.
- `runtime/rt.c`, `runtime/rt.h`, `runtime/bios.c`, `runtime/bios.h`,
  `runtime/dos_core.c`: dispatcher input/yield, child break termination and read retry.
- `runtime/main.c`, `runtime/image.h`, `Makefile`, `tools/web_demo.py`: installed
  names, reference binding documentation and safe legacy retirement.
- `tests/test_term.c`, `tests/test_machine.c`, `tests/test_rt_process.c`,
  `tests/test_dos_exec.c`, `tests/test_install_e2e.py`, `tests/test_logo_e2e.py`,
  `tests/web_smoke.mjs`: regressions and compatibility coverage.
- `README.md`, `web/README.TXT`, `web/LOGO.TXT` -> `web/BOOTLOGO.TXT`: user-facing names.
- `docs/verification/17-logo-fixes.md`: this record.

Other dirty files were present at the start and were not edited for this brief.

## Three weaknesses of this work

1. `runtime/term.c:352`: a single changed CGA source byte still triggers a full
   pixel/braille decode; there is no dirty-region optimization.
2. `runtime/rt.c:584`: the 16 ms input pump is cooperative at dispatcher
   boundaries, not hard host preemption; slow host scheduling or a long translated
   call can extend wall-clock latency.
3. `runtime/main.c:163`: modified or symlinked legacy `LOGO.COM` files are
   intentionally preserved and can still shadow a host `logo` command. Only an
   unchanged bundled legacy image is retired automatically.
