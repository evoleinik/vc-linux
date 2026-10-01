# Brief 30: MS-DOS 2.0's COMMAND.COM and its utilities

Read `CLAUDE.md`, the COMMAND.COM rows in `docs/plans/2026-10-01-games.md`, and the Kermit and
VZ briefs (`docs/briefs/26-kermit-modem.md`, `docs/briefs/15-vz-editor.md`) first. No network,
no browser. `export WATCOM=$PWD/build/openwatcom`; emsdk as before.

## Goal

Microsoft released the MS-DOS 2.0 source under the MIT licence. Preserve COMMAND.COM and the
small utilities the same way as everything else: original source vendored unedited, a
reproducible build, a translation checked instruction by instruction.

In the browser, VC's command line gets a real DOS shell: typing `dir`, `type readme.txt`,
`copy`, `echo` or a `.BAT` file name runs Microsoft's own COMMAND.COM. Typing `command` opens
its prompt, and `exit` returns to VC. On Linux, VC keeps `/bin/sh` as its shell, and `command`
runs COMMAND.COM as a program.

## 1. Sources and build

- The source is at `build/msdos/` (Microsoft's MS-DOS repository at the commit in
  `build/msdos/COMMIT`, MIT `LICENSE`). Vendor what this brief needs from `v2.0/source` unedited
  into `third_party/msdos2/` with the licence, the README and an `UPSTREAM` file naming the
  repository, commit and date.
- Build COMMAND.COM from its modules (see `COMLINK` for the link order), plus EDLIN, DEBUG, FIND,
  MORE, SORT and FC, each with JWasm and JWlink in MASM-compatible mode as for VC and Kermit. If
  JWasm rejects something, fix the tool side or its options, never the source, and say what it
  was. The earlier finding was that `DOSMAC.ASM` defines a macro named `INVOKE`, which JWasm
  treats as a keyword; `NOKEYWORD` may be the answer.
- `v2.0/bin` holds the binaries Microsoft shipped. Compare each build with its shipped file and
  report the result per program: byte-identical, or the first differing offset and why. The
  repository notes that some sources are newer than the shipped 2.0 binaries; record that
  plainly where it applies. Translate the source build either way.
- Translate each program and check every distinct instruction against unicorn. Earlier programs'
  generated C stays byte-identical; prove it with `cmp`.

## 2. Running them

- DOS 2.0 programs check the DOS version. Do what DOS 5's SETVER did: a small table, kept as data
  in one place, that reports version 2.0 to these programs only. Every other program sees the
  version it sees today.
- Browser: install COMMAND.COM at `H:\COMMAND.COM` and the utilities in `H:\DOS\`, on the DOS
  `PATH`. Set `COMSPEC=H:\COMMAND.COM`, so VC's command line runs commands through it. Batch
  files work. Each program is a lazily loaded side module, like the others; the first-load gate
  in `tests/web_size.mjs` must still pass.
- Linux: install the programs next to the others in the config directory, on the DOS `PATH`.
  `COMSPEC` and `/bin/sh` stay as they are, so nothing changes for existing users. Never write
  into `$HOME`.
- COMMAND.COM's internal commands that touch hardware or disks we do not have (for example
  `CHKDSK`-style or format-related ones) need only fail the way DOS would on that hardware.
- Add `H:\DOS\DOS.TXT`: what these programs are, where they came from, and three things to try.
  Update `web/README.TXT` and `README.md`, including the "Preserved so far" table.

## Gates

- `make test`, `make web`, `make test-web` green.
- E2e in a pty on Linux: `command` starts COMMAND.COM; `dir`, `echo hello`, `type` of a file,
  `copy` and a two-line `.BAT` file all work; `exit` returns to VC with its panels redrawn.
  `edlin` edits and saves a file; `find "x" file` and `sort < file` give the right output; `debug`
  shows its `-` prompt and quits with `q`.
- The web smoke test: in VC's command line, `dir` runs through COMMAND.COM and shows H:'s files,
  and a `.BAT` file runs; each program's module is fetched only on first use.
- A test that a program not in the SETVER table still sees today's DOS version.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.
- Your sandbox blocks loopback sockets, so the two existing TCP tests fail there. Run everything
  else; I run those.

## Done means

All gates green. The summary gives, per program, its byte-identity result against the shipped
binary, the tool fixes JWasm needed, the first-load size, and three weaknesses of your own work,
each with a real `file:line`. Do not commit.
