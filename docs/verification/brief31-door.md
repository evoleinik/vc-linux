# Brief 31: COMMAND door confinement review

## Scope and result

Read-only review of the native runtime diff against `main`, including new
guest-memory copies and every COMMAND/utility route toward host execution.
No additional host-execution escape or unchecked guest bulk copy was found
in the inspected changes. This is a bounded review, not a claim that every
possible malicious DOS program has been explored.

The new COMMAND return-address check calls the existing `code_matches`
guest-span check (`runtime/rt.c:548`). Hosted-loader image matching obtains
the complete guest image through `guest_span` before comparing it
(`runtime/dos_core.c:1447`). JFT growth uses guest fill/move, and AH=55h
preflights both 256-byte PSP spans before the overlapping guest move
(`runtime/dos_core.c:1854`). Cooked CON input checks both segment-wrapping
pieces before copying either (`runtime/bios.c:1100`). The remaining new
bulk copies operate on host-owned buffers, embedded image bytes, or already
checked guest spans.

During review the I/O owner replaced JFT's equivalent manual range check
with `guest_span` (`runtime/dos_fs.c:1850`), making its individually indexed
guest bytes explicitly follow the shared contract. This was not an observed
overflow. That owner separately found and regression-tested redirected
console quota accounting; its evidence belongs to the I/O verification.

The final common host-command boundary still refuses door mode before
terminal suspension, fork, or exec (`runtime/dos_core.c:559`). Image identity
uses the confined DOS filesystem open (`runtime/dos_core.c:487`), and
COMMAND's loader requires both complete approved file bytes and matching
loaded guest bytes (`runtime/dos_core.c:1486`). Linux's private pipe remap
returns immediately in door mode (`runtime/dos_fs.c:164`), because the
door already has a private H: root. Existing seccomp independently denies
fork/clone and exec (`runtime/door_confinement.c:184`); the tests below
require survival, so a seccomp kill is not mistaken for success.

## Real-door regression checks

```
.venv/bin/python -m pytest -q tests/test_door_command.py -x --tb=short
6 passed in 4.22s
```

For both the installed COMMAND.COM and a byte-identical renamed SECOND.COM,
the tests invoke `/C` with:

- `sh -c "touch HOST-RAN.TXT"`, which would reach the native shell if allowed;
- a small real native ELF copied into H: as HOSTELF.COM, whose marker-writing
  behavior is first demonstrated outside the door in the test's private root;
- the bundled DOS/FIND.EXE after changing one byte, proving an unapproved
  utility does not execute merely because it has a recognized filename.

Each test first proves genuine Microsoft COMMAND execution through a DOS
ECHO output file. It waits for the hostile `/C` invocation to return to VC,
then repeats the attempt interactively and requires a rendered DOS error and
a surviving COMMAND prompt. After EXIT, a second DOS ECHO, VC's F7 directory
creation, normal F10 exit, and watchdog cleanup must work. No host marker
may appear in H:, the original working directory, or the original home.
The fixture uses only private `/tmp` state and no network or diagnostic log.

Initial harness experiments tried to observe `/C`'s error in an idle ANSI
frame, but COMMAND can return before that frame is rendered. The final test
keeps the `/C` liveness/no-marker gate and repeats the attempt at the DOS
prompt to observe the error. A leading `/bin/sh` is also parsed as switches
by this original COMMAND, so `sh -c ...` is used to exercise actual unknown
program lookup. Those harness failures were not production-security defects,
and no red security-defect result is claimed.

Door PATH deliberately retains main's `H:\;H:\GAMES;H:\.VC`; this review
uses the explicit `DOS\FIND.EXE` path. Adding DOS to the door PATH would be a
separate convenience change, not a confinement fix required by this brief.
