# Brief 13: three fixes to the GW-BASIC change

A review of the uncommitted GW-BASIC work in this worktree found three problems. Fix each one.
For each, write a test that fails on the current code first, show it failing, then fix it. Keep
every existing gate green: `make test`, `make gwbasic`, `make web`, `make test-web` (the
toolchain is in `docs/briefs/10-browser.md`). Do not commit.

## 1. Code with no translation ends only the program that reached it

`runtime/rt.c:574`. When a child program jumps somewhere with no translated code, `rt_fault`
exits the whole vc process with status 70. Example: in GW-BASIC, `DEF SEG=0: CALL 0`, or `CALL`
or `USR` on machine code a listing POKEd into memory. Many old listings do this. VC dies and the
user's BASIC program is lost.

Fix: while a child process is running, print one short line on the DOS screen, for example
`No translated code at 0000:0000. GWBASIC.EXE stopped.`, and end only that child, so VC comes
back. When VC itself has no translation, keep today's fatal error. Test it in the pty e2e suite
and in the browser smoke test.

## 2. VC's own overlay never depends on the disk

`runtime/dos_core.c:828-830` and `runtime/main.c:85`. VC.COM re-runs VC.OVL from the config
directory after every command. That EXEC now reads the file and compares its bytes. But every vc
start rewrites the installed files with `fopen("wb")`, which empties a file before writing it.
So VC.COM fails to load its overlay, prints its memory error and exits. That happens if a second
vc starts at the wrong moment, a start fails half way on a full disk, a different build starts,
or the config directory is deleted.

Fix both halves:
- VC.COM and VC.OVL always run their built-in translations, whatever is on disk, as before this
  change. They are part of vc itself. Matching by the file's bytes stays for every other program.
- Install files by writing a temporary file in the same directory and calling `rename()`. Skip a
  file whose bytes already match.

Test: deleting or truncating the installed VC.OVL while VC runs, then running a command, leaves
VC working.

## 3. Typed commands reach Linux again unless they name a translated program

`runtime/dos_core.c:590-601` and `:693-698`. Every typed command now goes through the DOS
program search first, and any result other than "not found" stops it from reaching the Linux
shell. Three cases broke:
- Typing `vc` finds `VC.COM` on the DOS `PATH` and starts a nested DOS VC in an untested mode.
  Before, it started the native vc.
- `find .` in a folder holding a `FIND.EXE` with no translation prints "Invalid program format"
  instead of running `/usr/bin/find`.
- In a folder VC can enter but not list (mode 0711), looking up a missing name gets EACCES, so
  every typed command prints "Access denied".

Fix: run a found program only when its bytes match a translation. Otherwise, and on any lookup
error, the command goes to the shell as it did before. Keep VC.COM and VC.OVL out of the command
search. One exception keeps the project rule that a file name never reaches the shell as text.
`gwbasic`, the word the shipped `data/VC.EXT` line uses, never falls back to the shell. If
GWBASIC.EXE is missing or changed, the user gets a DOS error. Keep the existing `;touch PWNED`
e2e test green, and add e2e tests for the three cases above.

## Done means

All gates green, and each new test shown failing on the old code. Your summary lists the files
changed and names three weaknesses of your own work, each with a real `file:line`.
