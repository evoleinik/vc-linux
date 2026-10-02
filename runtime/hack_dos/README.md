# Hack 1.0.3 for real-mode DOS

This directory adapts NetBSD's BSD-licensed Hack 1.0.3 to an 8086 IBM PC.
The 92 files in `third_party/hack` are unedited, from NetBSD `games/hack`
commit `f037b5fcaa6db302271bbb445039a3a513b2e28c` (2026-08-14).
`UPSTREAM.sha256` records each original file; the build test also pins the
complete filename/content digest. `third_party/hack/UPSTREAM` records provenance.

## Rebuild

Use the same OpenWatcom pin as Rogue: `2026-10-01-Build`, archive SHA-256
`e6aa1b1e40ac8bbf97658d2c70fff8a4242d6ca4a1c60806f2baa5317083d4fe`.
The exact archive URL and verification are in `tools/fetch-openwatcom.sh`.
The brief's toolchain is already in `build/openwatcom`; no network is needed.

```sh
export WATCOM=$PWD/build/openwatcom
make hack
.venv/bin/python -m pytest -q tests/test_hack_build.py
```

The pinned `build/hack/HACK.EXE` is **243,478 bytes**, SHA-256
`b6f6ca8667fc8d1e37eb81fbd1c469a371312e4a39c53052985d553aaac345ee`.
Brief 37 requires a rebuild to add save/bones build identity and restore
upstream's whole-line DEL behavior; the previous 243,304-byte image was
`cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9`.
The test independently rebuilds it in a different directory, compares every
byte, checks the pin, checks vendor bytes, and verifies data and licence outputs.

Compilation uses `-bt=dos -0 -ml -os -fpc -j -zt=1024 -zld`: 8086 real mode,
large code/data pointers, software floating point, signed characters, and far
storage for large objects. The linker keeps a 16 KiB stack and `nofarcalls`, so
the OMF instruction spans remain usable by the map-driven compiled-C front end.
Upstream `makedefs.c` runs on the build host to generate integer object defines.
The DOS sources are compiled under stable relative names: upstream's `assert()`
embeds `__FILE__`, and absolute build paths would otherwise change the image.
No compiled source contains a live `__DATE__`, `__TIME__`, or `__TIMESTAMP__`.
The build links with a four-byte identity placeholder, hashes that image,
recompiles only `build_stamp.c` with the first 32 digest bits, and relinks. It
requires every final EXE byte outside that data slot to stay identical. Both
the code and initialized data retain their complete compiler-produced OMF
evidence; the translator's byte and relocation proof has no stamp exception.

## The replacements

| Unix module, not compiled | DOS replacement |
| --- | --- |
| `hack.terminfo.c` | `terminal.c`: BIOS INT 10h text mode, cursor, clear, inverse attributes; IBM PC horizontal/vertical wall glyphs |
| `hack.tty.c` | `terminal.c`: BIOS INT 16h raw keys, line editing, original command-prefix parsing and more prompts |
| `hack.ioctl.c` | `terminal.c`: no-op terminal-mode/suspend hooks; no Unix tty state exists |
| `hack.unix.c` | `platform.c` and `save_io.c`: DOS calendar, full-width BSD random sequence, DOS level names, save version/integrity checks |

`startup.c` switches to the executable's own directory, selects binary file I/O,
checks required data, and creates absent `record`/`perm` without truncating them.
An `atexit` handler restores the caller's active drive and directory, and the
Hack drive's previous directory when different. DOS shares these with the parent.
Directory capture and restoration use DOS long-filename services so generated
8.3 aliases cannot redirect the return when sibling directories change. Only an
unsupported LFN service falls back to classic DOS's stable on-disk 8.3 paths;
path errors do not. Each saved path has room for the drive prefix and all 260
bytes returned by the LFN service. Calls use Watcom's `intrf`, which actually
sets the incoming carry flag so older DOS handlers can leave it unchanged.
It does not read `$HOME` or honor an external `HACKDIR`. `compat.h` exposes the
small DOS interface, avoids Watcom's `lock()` namespace collision, and redirects
console-only stdio to BIOS without redirecting data/score-file stdio. Unix signal,
shell, pager-process and mail code are disabled; the built-in pager remains.

`tools/hack_port.py` applies counted changes to build-directory copies only:
the DOS config include, renamed entry point, `NUL`, 8.3 save names, the `far`
keyword collision, `itoa` collision, wall-output hook, DOS printf width syntax,
single-session score locking, stale-bones deletion in wizard mode, and segmented
save hooks. An upstream change that
removes or duplicates an expected span stops the build. All other gameplay code,
including generation, classes, pets, hunger, shops, equipment, worms and scoring,
is compiled from the original source.

## Files and interaction

The build playground contains `HACK.EXE`, `data`, `help`, `hh`, `rumors`, `record`
and `perm`, plus notices `COPYRIGHT`, `COPYRIGHT-JF` and `OWLIC.TXT`. Installed
native/browser/door playgrounds use the DOS-safe notice names `HACKLIC.TXT`
and `FENLIC.TXT` for the first two; their bytes are unchanged. Temporary
levels are `HACK.0` through `HACK.40`, bones retain upstream's `bones_XX`, and
the resumable game is `HACK.SAV`. No mutable file is written in the caller's
directory. In the browser and doors the directory is `H:\GAMES\HACK`.
`OWLIC.TXT` also includes the NetBSD declaration header's retained BSD-2 notice.

The default player is `Hacker`; `-uName` and upstream role suffixes still work.
A plain launch asks the original experience/class questions. `hack103 -C` on
Linux (or `hack -C` in the browser/door DOS shell) starts a Cave-man immediately.
`h j k l` and arrows move; `y u b n` move diagonally;
`S` saves and exits; the next launch restores; `Q`, then `y`, quits to VC.
The original supported `GOLD_ON_BOTL` and `EXP_ON_BOTL` options expose full status.
In text prompts, Backspace erases one character; DEL and Ctrl-U clear the line,
matching upstream's line editor.

## Saves

The first 16 bytes contain `HACK`, the 32-bit linked-image build identity,
32-bit file length and 32-bit FNV-1a checksum of the remainder. Persistent saves
and bones are validated before upstream reads pointer-bearing structures. A
different build or format, including the old `HACK103`/1 header, produces Hack's
original `Saved level is out of date.` message and is deleted as upstream did.
Wizard mode likewise deletes rejected bones while retaining valid bones for
repeat debugging.
Current-build saves with changed, truncated or appended payloads are renamed to
`HACK.BAD`; a fresh game can then start. If that name already exists, the damaged
save remains and the fresh game still starts. Internal level records carry the
same format and build identity. Copies and reinstalls of identical DOS bytes
retain their identity regardless of file times; an executable change gets a
new stamp automatically, without relying on developers bumping a version.

Upstream serializes pointer-bearing C structures, so this is deliberately a DOS
format, not compatible with Unix saves. Monster species pointers are rebased by
the EXE's segment displacement, including static pet/wizard records outside the
monster table. Sickness text is copied independently. The only timeout callback,
levitation's `float_down`, is reconstructed using the current image's code pointer.
Upstream's existing inventory/monster/worm/object-name restoration does the rest.

## Verification and limits

`tests/test_hack_build.py` executes actual linked routines under Unicorn: the
BSD random sequence, terrain-specific IBM wall glyphs, far-pointer rebasing at
different load segments, sickness serialization, callback restoration, and file
integrity, exact build identity, stale save/bones messages and deletion, and
Backspace/DEL/Ctrl-U line editing. It also checks same-drive/cross-drive exit
state, LFN and classic DOS fallbacks, path errors, and full-length paths through
the real `intrf` routine.
The carry flag is verified at the actual interrupt boundary, including older
DOS handlers that report unsupported services without changing carry.
Only external streams/OS I/O and stack probing are stubbed.
The native pty and browser/door tests cover whole-game launch and interaction.

The DOS executable still has one save per playground. The Linux runtime in
`dos_core.c` opens a regular `VCPLAY.LCK` file read-write without following a
symlink, acquires an `F_OFD_SETLK` lock, and rechecks the path's device/inode
identity. It keeps the first process in the original playground and gives
contenders independent `PLAY0001` through `PLAY0064` directories. Under the
slot's lock, its EXE is refreshed to the current translated image; static data
and notices are compared to the installed primary files and refreshed if they
differ. Missing primary data falls back to the bundle. Unchanged files retain
their inode and times. Same-directory atomic replacements never truncate an
old symlink or hardlink's referent; saves, levels, bones and scores are not
copied or replaced.
Each process retains its slot through S/Q and relaunch, rechecking lock-path
identity; exit or a crash releases the kernel lock. Slot data is persistent,
not silently cleaned up. If the main playground is free in a later VC session,
open a secondary slot and type `dos2 /c HACK.EXE` to resume its save. Browser and
door sessions are already private.
Real simultaneous-PTY tests cover independent movement/save/restore and slot
reuse after process death, with an unchanged HOME sentinel outside XDG storage.
The checksum detects accidental corruption, not deliberately forged files; it is
not a replacement for validating every original variable-length record. The
32-bit build stamp is likewise a compatibility guard, not a cryptographic
authenticator; the complete reproducible EXE is pinned separately by SHA-256.
Terminal bells and visual-output delays are currently no-ops; gameplay timing
still advances only on commands, as in Hack.
