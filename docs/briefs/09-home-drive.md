# Brief 09: drive H: for the home directory

Read `README.md` and the Questions section of `docs/plans/2026-09-30-native-port.md` first. The
owner asked for a second drive: `H:` is `$HOME`, and `C:` stays `/`. Another worker is changing
`runtime/dos_core.c`, `runtime/main.c` and `tests/test_e2e.py` at the same time, so do not touch
those.

## Pinned decisions

- **The drive table** lives in `runtime/dos_fs.c`. C: (drive number 2) is `/`. H: (drive number
  7) is `realpath($HOME)`, present only when `$HOME` is set, absolute and a directory other than
  `/`. Every other letter is an invalid drive (error 15).
- **A current directory per drive**, as in DOS. Keep a current directory for each valid drive,
  plus a current drive.
- `dos_fs_init` picks the start point. If the host `getcwd()` is the H: root or below it, the
  current drive is H: and its directory is the path relative to `$HOME`. Otherwise the drive is
  C:, as now. The other drive's current directory starts at its root.
- **Paths.** A path with `H:` or `C:` resolves on that drive. A path with no drive uses the
  current drive. A relative path uses that drive's current directory. `..` at a drive root stays
  at the root: you can never leave H:'s root by `..`. Every host path produced from an H: name
  must stay inside H:'s root. A symlink that leads outside is fine, since that is the
  filesystem's business, but string handling must never escape the root.
- **Functions that now depend on the drive:** 0Eh (select drive; AL returns 8, meaning LASTDRIVE
  H) and 19h (current drive). Also 36h and 7303h free space, 47h and 7147h getcwd (DL = 0 current,
  3 C:, 8 H:), 3Bh and 713Bh chdir (changing the directory of the drive named in the path, never
  the current drive, as in DOS), 60h and 7160h truename (which must print the right letter),
  71A0h volume info for `H:\`, IOCTL 4408h, 4409h, 440Eh and 440Fh for drive 8, and 4400h on file
  handles (the drive number bits).
- **Names.** A host path under H:'s root shows as `H:\...` in truename and getcwd output. A path
  outside it stays `C:\...`. The same file is reachable by both names.
- `dos_fs_to_host` works for H: paths.

## Tests (`tests/test_dos_fs.c`)

The test sets `$HOME` to a temporary directory before `dos_fs_init`, inside the existing fixture.
Cover:
- 0Eh and 19h, and switching drives
- the per-drive current directory surviving a switch
- `H:\` resolving to `$HOME`
- `..` at `H:\` staying put
- truename and getcwd letters
- free space on H:
- IOCTL on drive 8, and invalid drives D: and Z:
- the start drive when `getcwd` is under `$HOME`
- a file reached as both `H:\x` and `C:\...\x`

Keep every existing test passing. Update only the checks that hard-code "only C: exists", and
explain each change.

Watch it fail: make H: resolve to `/` instead of `$HOME`, show the H: tests going red, restore.

## Gates

`make test-fs` and `ASAN_OPTIONS=detect_leaks=0 make test-fs-sanitize` pass. No network. Use
`.venv/bin/python`. You cannot commit, so leave the work uncommitted. Only touch
`runtime/dos_fs.c`, `runtime/dos_fs.h` and `tests/test_dos_fs.c`.

## Required summary

1. The changes, with `file:line`.
2. Test results and the planted-defect result.
3. Deviations, and why.
4. Three real weaknesses, each with `file:line`.
