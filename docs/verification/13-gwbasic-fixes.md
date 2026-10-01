# Brief 13 verification

All three fixes are implemented. Existing uncommitted work was preserved; no commit was made.

## Old-code failures

The pre-fix native binary, WebAssembly build, and runtime sources were preserved under
`/tmp/vc-brief13.BMkv8j/` before implementation. Native regression runs selected that binary with
`VC_TEST_BINARY=/tmp/vc-brief13.BMkv8j/vc`.

- `tests/test_gwbasic_e2e.py -k untranslated_call`: all eight parameter cases failed on the old
  binary, exercised in separate runs. Both command paths exited 70 for CALL at `0000:0000`,
  an unknown BIOS-ROM entry at `F000:0200`, and CALL/USR into POKEd machine code at `9000:0000`.
  This interpreter requires a variable for CALL: `DEF SEG=0: A=0: CALL A` reaches the address
  intended by the brief's `CALL 0` example.
- `tests/web_smoke.mjs /tmp/vc-brief13.BMkv8j/web/vc.mjs`: the first ten checks passed, then the
  new child-fault check failed with `vc: fatal: no translated code at 0000:0000`. The child had
  also left its speaker running.
- `tests/test_install_e2e.py`: nine failures. Deleting, truncating, changing VC.OVL, or removing
  the config directory stopped VC; identical files were rewritten; held old inodes saw new
  bytes; a simulated failed write truncated the installed file. The final version of this
  suite failed nine tests against the old binary and passed nine against the fixed binary.
- `tests/test_command_e2e.py`: six failures, covering both COMSPEC and INT 2Eh. Typed `vc`
  launched DOS VC, untranslated FIND.EXE produced error 11, and an execute-only directory
  lookup produced error 5 instead of reaching Linux.
- Two additional `test_gwbasic_changed_image_is_refused_and_vc_survives` cases failed when
  VC.COM or VC.OVL bytes replaced GWBASIC.EXE: a different recognized translation was accepted
  as the interpreter. The association now requires its named translation.
- `tests/test_rt_process.c`, compiled against the preserved runtime: both new child recovery
  cases exited 70 instead of resuming the parent. The two controls requiring fatal errors in
  root VC and VC.OVL already passed, as intended.

All 27 native regression cases above, and the new browser check, now pass. The existing
`;touch PWNED` association tests remain green. The process harness also verifies IVT/PIT/PIC
and speaker restoration, an abandoned IRQ, pending Ctrl-Break cleanup, PSP/stack/DTA and FCB/MCB
cleanup, subsequent parent timer delivery, and a second child launch.

## Final gates

- `make test`: passed; 208 translator tests, 4,317 filesystem checks, 1,027 EXEC checks,
  9 machine cases, 4 process cases, 5,647 terminal checks, 3 configuration tests, and 92 pty
  end-to-end tests.
- `make gwbasic`: passed; the assembled image, map, and listings are up to date.
- `make web`: passed with the toolchain and narrow Asyncify flags from brief 10.
- `make test-web`: passed; speaker tests and all 12 smoke checks, including recovery and quit.
- `git -c core.whitespace=cr-at-eol diff --check`: passed. The option recognizes the existing
  DOS line endings in VC.EXT; that file was not changed by this fix.

An additional AddressSanitizer/UndefinedBehaviorSanitizer build passed all four process cases
with `ASAN_OPTIONS=detect_leaks=0`. LeakSanitizer itself could not run under this sandbox's
ptrace environment, so this is not a claim that leak checking passed.

The WebAssembly file grew from 7,306,762 to 7,309,274 bytes (0.0344%). Deterministic gzip
(`gzip -n -c`) grew from 1,574,554 to 1,577,045 bytes. Asyncify stayed narrow.

## Files changed by this brief

- `runtime/dos_core.c`, `runtime/rt.c`, `runtime/rt.h`: child fault recovery and process state;
  built-in VC loading; shell routing and association identity checks.
- `runtime/main.c`, `runtime/image.h`: atomic, permission-preserving installation, identical
  file skipping, and documentation of the built-in name exception.
- `tests/test_gwbasic_e2e.py`, `tests/web_smoke.mjs`: real interpreter recovery and identity tests.
- `tests/test_install_e2e.py`, `tests/test_command_e2e.py`, `tests/test_rt_process.c`: new
  installer, routing, and process-boundary regressions.
- `tests/test_dos_exec.c`, `tests/test_machine.c`: stubs for the deliberately extended runtime
  interface in their existing isolated harnesses.
- `Makefile`, `README.md`, `docs/verification/13-gwbasic-fixes.md`: gate wiring, behavior
  documentation, and this evidence record.

## Three weaknesses

1. `runtime/main.c:119`: rename provides atomic visibility, but there is no file/directory
   fsync to promise durability across sudden power loss.
2. `runtime/dos_core.c:915`: recovery assumes the original child PSP and intact guest process
   metadata. It is not isolation from arbitrary POKEs corrupting the PSP, stack, or MCB chain.
3. `tests/test_command_e2e.py:57`: owned mode 0111 models the effective access of a non-owner
   in mode 0711; the fixture does not run a second Linux user.
