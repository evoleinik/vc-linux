# Brief 31: rebase COMMAND.COM onto main and fix what review found

Brief 30's work is committed on this branch as one WIP commit. `main` has since gained BBS door
mode (`runtime/door*.c`, large changes to `runtime/dos_core.c` and `runtime/dos_fs.c`, and a
bounds-checked guest-memory helper in `runtime/guest_mem.h`). Same toolchain as brief 30.

## 0. Rebase (in progress)

Your sandbox cannot write git's metadata, so I started `git rebase main` myself. It stopped with
conflict markers in `Makefile`, `runtime/dos_core.c`, `runtime/dos_fs.c` and
`tools/web_demo.py` (7 hunks). Resolve them by editing those files only, so that both main's door
mode and this branch's COMMAND.COM work keep all their behaviour and tests. Do not run any git
command that writes (`add`, `commit`, `rebase`, `stash`); read-only ones such as `git diff` and
`git show main:path` are fine. I finish the rebase afterwards.

Every new native copy into or out of guest memory in this branch must go through
`runtime/guest_mem.h`, as main now requires. Door mode (`vc --door`) must not gain any way to reach
a host program through COMMAND.COM or the utilities.

## 1. On Linux, nothing typed on VC's command line may change meaning (major)

Today the seven programs install flat on the Linux DOS PATH, which DOS search reaches before
`/bin/sh`, so `find . -name x`, `sort`, `more`, `fc` and `command -v git` now run DOS programs.
- On Linux, put COMMAND.COM and the utilities in a `DOS2` subdirectory of the config directory,
  not on VC's PATH. Programs are matched by bytes, so also install COMMAND.COM's bytes as
  `DOS2.COM` on the PATH: typing `dos2` opens the MS-DOS 2.0 prompt. COMMAND.COM's own
  environment gets the `DOS2` directory on its PATH, so `find`, `sort`, `edlin` and the others work
  there. Linux `COMSPEC` stays `/bin/sh`.
- Browser: unchanged. There is no host shell to shadow.
- The gate: in a directory with no DOS programs, `find`, `sort`, `more`, `fc` and `command` typed
  on VC's command line on Linux reach `/bin/sh` exactly as before this branch. Show it red first.

## 2. A program that calls EXEC under COMMAND kills VC (major)

COMMAND hooks INT 21h, so its children's AH=4Bh goes through COMMAND's own loader, AH=55h arrives
with that child on top, `adopt_dos2_child` (`runtime/dos_core.c`) refuses it, and an untranslated
jump ends VC with exit 70. In the browser, typing `vc` on VC's command line now does exactly this.
- Adopt the child whenever the AH=55h caller is COMMAND's loader: the INT frame's return address
  lies in a registered or moved COMMAND copy.
- Let `abortable_child` follow PSP:16h up from the current PSP to the top child, so an
  untranslated jump stops that DOS program and never VC.
- Gates: under COMMAND, `vc` starts VC and F10 returns to COMMAND; `kermit` then `push`;
  `gwbasic` then `SHELL`; `debug X.COM` then `g` on a small test program. Each works, or fails
  with a DOS error, and VC survives in every case. Both builds.

## 3. DOS console behaviour COMMAND exposes

- TAB output: DOS's console expands TAB (09h) to the next multiple of 8 columns for INT 21h
  output (AH=02h, 06h, 09h, 40h to CON). Today `dir /w` shows the CP437 glyph instead. BIOS
  INT 10h teletype keeps today's behaviour.
- `COPY CON NOTE.TXT`: reading CON through a handle must read typed lines, echoing them, until
  Ctrl-Z, as DOS does. Today it returns end of file at once and empties the file.
- `dir > nul` must discard output. NUL is a device, not a file.
- COMMAND's pipe files (`%PIPE1.$$$`, `%PIPE2.$$$`) go to the root of the current drive. On Linux
  that root is `$HOME` or `/`. Keep them in a private temporary directory for the session instead,
  without editing COMMAND's source, and never write into `$HOME`. `dir | sort` must work on every
  drive.
- Wildcard `REN` and `DEL` through FCB calls (AH=13h, AH=17h) must work, with DOS's `?` rules.
- Handles inherited by a PSP that AH=55h made are released when that PSP's program ends.
Each with a test that fails first.

## 4. First load

First load is 1,296,214 gzip bytes against the 1.3 MB cap, with the DOS files and their source
packed into the main wasm. Measure the startup cost of the new in-wasm decompression under node
(time to VC's first screen, before and after this branch), and report it. Keep first load at or
under 1.3 MB.

## Done means

`make test` (apart from the loopback TCP tests your sandbox blocks), `make web`, `make test-web`
green, and the door tests from main still pass. The summary lists each fix with red and green
evidence, the startup timing, and three weaknesses of your own work, each with a real `file:line`.
Do not run any git command that writes.
