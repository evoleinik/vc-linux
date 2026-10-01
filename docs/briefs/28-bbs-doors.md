# Brief 28: our programs as doors on the notanemulator BBS

Read `CLAUDE.md`, `docs/plans/2026-10-01-roadmap.md` (item 6) and
`docs/plans/2026-10-01-bbs-experience.md` first. No network, no browser. `export
WATCOM=$PWD/build/openwatcom`. I deploy to the BBS and test it live afterwards.

## Goal

A caller on the notanemulator BBS picks "Volkov Commander" or "Rogue" from the doors menu and
plays it in their own BBS terminal. ENiGMA½ runs on an ARM64 Linux container (`aarch64`, Debian
12) and starts each door under node-pty, so the door gets a pseudo-terminal sized to the caller's
terminal. The container runs as root; the door command will be wrapped in
`setpriv --reuid=65534 --regid=65534 --clear-groups --no-new-privs`.

## 1. Door mode: `vc --door`

The caller is a stranger on the internet. Door mode must give them the browser demo's experience
and nothing of the host.

- No host programs. Never run `/bin/sh` or any Linux program. Only translated DOS programs, found
  by bytes as today. A typed Linux command gets an ordinary DOS "Bad command or file name".
- No host file system. H: is a fresh private directory for this session, filled with the same
  files as the browser's H: (README, history, sources, games, GW-BASIC, bootLogo, Rogue, VZ,
  Kermit, BBS.TAK, the Russian README). There is no C: drive or any other path into the host.
  Reuse the browser demo's file list and generator; do not keep a second list.
- The session directory lives under `$VC_DOOR_ROOT` (required in door mode), is created with
  mode 0700, and is deleted when VC exits, however it exits, including SIGHUP when the caller
  drops carrier. Cap H: at 8 MiB in total: a write past the cap fails with the DOS "disk full"
  error.
- No network. The modem's phone book is empty in door mode, so every dial answers `NO ANSWER`.
- A session limit: `--door-minutes N` (default 60). At the limit VC prints one line and exits.
- `--door-run PROGRAM` starts a program from H: directly (for the Rogue door), and VC exits when
  it ends.
- Terminal: BBS terminals send classic VT and ANSI key sequences, not kitty. Check the existing
  decoder handles them all for VC's keys. Do not request the kitty protocol, modifyOtherKeys or
  mouse tracking in door mode. Output stays UTF-8; ENiGMA converts it to the caller's code page.
  If the terminal is smaller than 80x25, print one clear line and exit.
- Door mode is a command-line switch only. Nothing in a normal run changes.

## 2. A static ARM64 build

- `make build/vc-door-aarch64` builds a static `aarch64-linux-musl` binary with Zig 0.16.0 as the
  C compiler: `$(ZIG) cc -target aarch64-linux-musl -static`. Zig 0.16.0 is unpacked at
  `build/zig/zig` here, so default `ZIG` to that. The translated C is unchanged; fix any
  portability problem in the runtime only.
- You cannot run ARM64 binaries here. Also build the same door mode for x86-64 and run every door
  test against that; the ARM64 build must at least compile and link cleanly, and `file` must
  report a static aarch64 executable.
- CI: add the ARM64 build to the release job, attached as `vc-door-linux-aarch64`. CI has network,
  so it installs the PyPI package `ziglang==0.16.0` with uv and sets `ZIG` to
  its `zig` binary.

## 3. ENiGMA configuration, as files I will apply

Under `infra/bbs/doors/`: a door wrapper script that makes the session root, applies `setpriv`,
and execs `vc --door` with the caller's node number in the session name; and the two menu entries
for `notanemulator_bbs-doors.hjson` using ENiGMA's `abracadabra` module with `io: stdio`. Read
ENiGMA's abracadabra documentation if it is anywhere on disk; if not, write the entries from the
example already in that menu file, which I will paste into `infra/bbs/doors/README.md` for you:

```
doorAbracadabraExample: {
    desc: Abracadabra Example
    module: abracadabra
    config: {
        name: Abracadabra
        dropFileType: DOOR
        cmd: /path/to/door
        args: [ "{dropFilePath}", "{node}" ]
        nodeMax: 1
        tooManyArt: DOORMANY
        io: stdio
    }
}
```

Use `dropFileType: NONE` if ENiGMA allows it, since VC needs no drop file. Explain in the README how
to install the binary and menus on axis.

## Gates

- Door-mode e2e tests in a pty on x86-64: `/bin/sh` and `ls` are refused; there is no C: drive;
  H: holds the demo files; writes past 8 MiB fail with "disk full"; ATDT answers NO ANSWER; the
  session directory is gone after a normal exit, after SIGHUP and after the time limit;
  `--door-run ROGUE.EXE` starts Rogue and exits with it; classic VT key sequences drive F-keys
  and arrows; a 80x24 terminal gets the one-line message.
- A test that a normal (non-door) run still behaves exactly as before.
- `make test`, `make web`, `make test-web` green, and `make build/vc-door-aarch64` builds.
- Plant a defect for each new door gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. The summary lists files changed, the ARM64 binary size, and three weaknesses of
your own work, each with a real `file:line`. Do not commit.
