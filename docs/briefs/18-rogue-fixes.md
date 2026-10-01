# Brief 18: four fixes to the Rogue change

A review of the uncommitted Rogue work in this worktree found four problems. Fix each one. For
each, write a test that fails on the current code first, show it failing, then fix it. Keep every
gate green: `make test`, `make rogue`, `make web`, `make test-web` (toolchain in
`docs/briefs/10-browser.md`; OpenWatcom via `WATCOM`). Do not commit.

## 1. A save that will not restore must not lock the player out

`runtime/rogue_dos/startup.c:11` and `tools/rogue_port.py:50`. With any `rogue.sav` that
`restore()` rejects, such as one cut short while writing or from an older save format, every plain
`rogue` tries to resume it, fails, and exits with code 1. The port keeps the bad file on purpose,
so this repeats forever. The message goes to the DOS user screen, so the player sees VC's panels
and nothing else. Any argument other than `-s` or `-d` is also read as a save file.

Fix: when an automatic resume fails, rename the file to `rogue.bad`, say so in one line, and start
a new game. Test it with a truncated save in the pty e2e suite.

## 2. A fresh clone must say how to get OpenWatcom

`README.md:41` and `tools/build_rogue.py:93`. The default target now embeds ROGUE.EXE and `gen`
needs `wdis`, so `uv sync && make` stops with "set WATCOM". Neither the README, the error, nor the
Commands block in `CLAUDE.md` names `tools/fetch-openwatcom.sh`.

Fix: the error message prints the exact command, `tools/fetch-openwatcom.sh build/openwatcom &&
export WATCOM=$PWD/build/openwatcom`. Add the same line to the README's build steps and to
`CLAUDE.md`'s Commands block. Test the error text.

## 3. A read-only directory must not stall the end of the game

`runtime/rogue_dos/config.h:7` and `third_party/rogue/mach_dep.c:384`. Run `rogue` from a
directory you cannot write to, such as `C:\`, which is `/`, then quit with gold or die with a
qualifying score. `lock_sc()` cannot create `rogue.lck`, so it calls `md_sleep(1)` five times and
the screen freezes for about 5.5 seconds before VC returns. The score is then dropped silently.

Fix: remove the `LOCKFILE` define, since DOS runs one program at a time. Test the timing from a
read-only directory in the pty suite, by elapsed wall time with a generous bound.

## 4. CI must build the exact ROGUE.EXE that was verified

`tools/fetch-openwatcom.sh:8` and `tests/test_rogue_build.py:351`. The pin is OpenWatcom
`2026-10-01-Build`, whose archive SHA-256 is
`e6aa1b1e40ac8bbf97658d2c70fff8a4242d6ca4a1c60806f2baa5317083d4fe`. Its `wcc`, `wlink`, `wdis`,
`wlib` and `clibl.lib` were compared on 2026-10-01 and are byte-identical to the toolchain in
`$HOME/src/vc-linux-wt/tools-cache/openwatcom`. That toolchain builds ROGUE.EXE with SHA-256
`8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee`, 202,816 bytes. No test
checks that hash, so a different toolchain would ship a different EXE with CI green.

Fix: assert that hash in `test_rogue_build_is_byte_reproducible`. Record the pin's origin in
`runtime/rogue_dos/README.md` and fix the two docs that say no checksum existed
(`docs/verification/16-rogue-build.md:185`, `runtime/rogue_dos/README.md:352`).

## Done means

All gates green, and each new test shown failing on the old code. Your summary lists the files
changed and names three weaknesses of your own work, each with a real `file:line`.
