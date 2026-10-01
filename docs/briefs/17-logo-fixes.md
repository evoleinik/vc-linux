# Brief 17: three fixes to the Logo and CGA change

A review of the uncommitted Logo and CGA work in this worktree found three problems. Fix each one.
For each, write a test that fails on the current code first, show it failing, then fix it. Keep
every gate green: `make test`, `make web`, `make test-web` (toolchain in
`docs/briefs/10-browser.md`). Do not commit.

## 1. A graphics screen that has not changed is not redrawn

`runtime/term.c:459`, called from `term.c:1448` and `term.c:1523`, with the idle throttle at
`runtime/bios.c:429-437`. In a graphics mode, every INT 16h function 01h poll renders twice. Each
render decodes all 64,000 or 128,000 pixels and rebuilds 2,000 braille cells, even when video
memory is unchanged. The throttle sleeps only after 200 polls with no other interrupt between
them, and the 18.2 Hz timer resets that count every 55 ms. So GW-BASIC waiting in
`IF INKEY$="" THEN 80`, as the shipped SPIRAL.BAS does at its end, keeps a CPU core busy on Linux
and the main thread busy in the browser. The same loop in text mode sleeps.

Fix: keep a copy of the 16 KB CGA area and the BDA bytes at 0449h and 0466h. Skip the decode, the
dumps and the canvas call when none of them changed. Test: after the first frame of an idle
graphics screen, further polls do no decode work. Count decodes in a test hook, never by timing.

## 2. Ctrl-Break stops a child that does not handle it

`runtime/bios.c:350-361` and `bios.c:1001`. Ctrl-Pause and Ctrl-Shift-B only queue a 00h key and
run the default INT 1Bh, which does nothing. bootLogo hooks neither INT 1Bh nor INT 23h, so
`REPEAT 0 [REPEAT 0 [FD 1]]` (about 4.3 billion points) cannot be stopped. On Linux only `kill`
from another shell ends it. In the browser the page freezes until a reload, which wipes H:.

There are two halves:
- **The key must be read at all.** That loop never polls the keyboard or idles, so today input is
  never read and, in the browser, the page never gets control. Read pending input from the
  dispatcher in `rt_run` at least every 50 ms, even when the program never idles. In the browser,
  that is also where the page gets control back to paint and take keys. `rt_run` is on the direct
  Asyncify chain, so it may sleep there. `rt_yield` still must not sleep (see `CLAUDE.md`).
- **The key must end the child.** When a child is running and has not hooked INT 1Bh, Ctrl-Break
  ends it through the same `terminate` path `dos_abort_untranslated` uses, and VC restores text
  mode as it does today. The break key must not leave a 00h byte in the program's input. GW-BASIC
  hooks INT 1Bh itself, so its `Break in 10` must keep working. With no child running, Ctrl-Break
  in VC does nothing new.

Tests: e2e in a pty and the node smoke test both start bootLogo, run
`REPEAT 0 [REPEAT 0 [FD 1]]`, press Ctrl-Break, and see VC's panels back. The browser test also
shows the canvas updated while the loop ran.

## 3. Install bootLogo under its own name

`runtime/main.c:137-148`. LOGO.COM is installed on the DOS `PATH`, and DOS programs are found
before VC falls back to `/bin/sh`. So a Linux user whose `logo` is UCBLogo, which Debian installs
as `/usr/bin/logo`, gets bootLogo instead. Install it as `BOOTLOGO.COM`, its real name, in both
builds, and drop `LOGO.COM`. Typing `bootlogo` runs it. Rename `LOGO.TXT` to `BOOTLOGO.TXT` and
update `web/README.TXT` and `README.md`. Matching stays by bytes, so nothing else changes.

## Done means

All gates green, and each new test shown failing on the old code. Your summary lists the files
changed and names three weaknesses of your own work, each with a real `file:line`.
