# Brief 33: Hack 1.0.3, the roguelike between Rogue and NetHack

Read `CLAUDE.md`, `docs/briefs/16-rogue.md`, `docs/briefs/18-rogue-fixes.md` and the roguelike
findings in `docs/plans/2026-10-01-roadmap.md` first. No network, no browser. `export
WATCOM=$PWD/build/openwatcom`; emsdk as before. Your sandbox blocks loopback sockets and cannot
write git metadata: run everything else, do not commit.

## Goal

Hack (Jay Fenlason 1982, Andries Brouwer 1985) is the game NetHack grew from. Preserve it the way
Rogue is preserved: original source vendored unedited, a reproducible 16-bit DOS build with the
pinned OpenWatcom, the compiled-C translation, and it runs from VC on Linux, in the browser and as
a BBS door.

## Source

`build/hack-inputs/` holds NetBSD's `games/hack` at commit
`f037b5fcaa6db302271bbb445039a3a513b2e28c` (2026-08-14): `src/` (all 92 files), `files.txt`,
`COMMIT`. NetBSD's tree is the maintained BSD-licensed Hack 1.0.3: `COPYRIGHT` (CWI Amsterdam,
BSD-3) and `COPYRIGHT-JF` (Jay Fenlason, BSD-3). Vendor it unedited in `third_party/hack/` with
`UPSTREAM` naming the repository, path, commit and date.

## Build and port

- Build HACK.EXE for 8086 real-mode DOS with OpenWatcom, the same way and with the same pinned
  toolchain as Rogue (`tools/build_rogue.py`). Never edit the vendored files; anything DOS needs
  goes in a small shim directory of our own, as `runtime/rogue_dos/` does for Rogue. NetBSD's
  `hack.unix.c`, `hack.tty.c`, `hack.ioctl.c` and `hack.terminfo.c` are the Unix-specific parts:
  replace them with DOS shims rather than editing them. Draw through the BIOS or DOS console the
  way our machine supports, and choose IBM PC line-drawing for walls where Hack lets you.
- Data files (`data`, `help`, `hh`, `rumors`, `record`, `perm`) live in a Hack directory, as the
  original expected, found next to HACK.EXE. Saves and bones go there too. Never into `$HOME` on
  Linux; in the browser they are H: files; in a door they stay inside the session.
- Build reproducibly and pin the HACK.EXE SHA-256 in a test, as for Rogue. If any source uses
  `__DATE__` or `__TIME__`, pin it the way `tools/build_rogue.py` now pins PDCurses' notice.
- Translate it with the compiled-C front end, checking every distinct instruction against
  unicorn. Earlier programs' generated C stays byte-identical; prove it with `cmp`.

## Running it

- Browser: `H:\GAMES\HACK\HACK.EXE` with its data files, lazily loaded like the other programs and
  H: files. Enter on it starts Hack; quitting returns to VC.
- Linux: install next to Rogue in the config directory; typing `hack` starts it. Check first that
  this does not shadow a common host command; if it would, use `hack103` and say why.
- BBS door: add a `HACK` door beside `VC` and `ROGUE` in `infra/bbs/doors/`, and include Hack on
  the door's H:.
- Update `README.md` ("Preserved so far", credits), `web/README.TXT` and the doors README.

## Gates

- `make test`, `make web`, `make test-web` green; first load still at or under 1.3 MB.
- E2e in a pty: Hack starts, shows its first level with the `@` and status lines, moves, saves with
  `S` and restores the save on the next start, and quits with `Q` back to VC. The web smoke and
  door tests start it and see the status line.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. The summary lists files changed, the HACK.EXE size and SHA-256, what the DOS
shims replace, and three weaknesses of your own work, each with a real `file:line`.
