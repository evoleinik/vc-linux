# Brief 02: the DOS file-system layer

Read `docs/plans/2026-09-30-native-port.md` first. It explains the whole project.

## Problem

Volkov Commander's translated code calls INT 21h for everything about files. Implement those DOS
functions in C against Linux, in `runtime/dos_fs.c`. VC must then see real Linux files as a DOS
drive, with long names.

## Pinned decisions (do not change these)

- `runtime/cpu.h` and `runtime/hle.h` are fixed interfaces. Implement `dos_fs_init`,
  `dos_fs_int21` and `dos_fs_to_host` exactly as `hle.h` declares them. Do not edit either header.
  Put extra prototypes in a new `runtime/dos_fs.h`.
- Calling convention. Each call reads registers from `cpu` and memory from `mem` via
  `rd8/rd16/wr8/wr16`. It returns results the DOS way: set `cpu.cf` = 1 and put the DOS error code
  in AX on failure, `cpu.cf` = 0 on success. `dos_fs_int21` returns 1 if it handled the function
  and 0 if the function is not one of its own. It must not touch `cpu` when it returns 0.
- Drives. Only drive C: exists, and it is the host root `/`. The DOS current directory is kept
  inside this layer, starting from the host `getcwd()` at `dos_fs_init`. Never call host `chdir`.
  A:, B: and D: through Z: are invalid drives (error 15). Function 0Eh returns 3 drives in AL,
  19h returns 2.
- Names. DOS strings are code page 866 bytes. Host names are UTF-8. Convert CP866 to UTF-8 on the
  way in and UTF-8 to CP866 on the way out. A character with no CP866 form becomes `?`. Matching
  a DOS name against the host is case-insensitive. Try an exact match first. Then compare the
  CP866 form of each host name case-insensitively, `?` included, so lossy names still open.
  Separators `\` and `/` are both accepted. `.` and `..` resolve.
- Short names. Classic (non-71xxh) calls use 8.3 aliases. A host name that is already a valid 8.3
  name uses its uppercase form. Any other name gets a Windows-style alias `BASENA~N.EXT`, unique in
  its directory and deterministic: host names are sorted by byte value, and the lowest free N is
  given in that order. Resolving a path accepts aliases.
- Attributes. Directory = 10h. Read-only = 01h when the owner cannot write. Hidden = 02h for names
  starting with `.` other than `.` and `..`. Archive = 20h for regular files. System is never set.
  Stat follows symlinks. A dangling symlink is a regular file of size 0. Setting read-only
  toggles the owner, group and other write bits together. Setting hidden, system or archive
  succeeds and changes nothing.
- Times. DOS packed date and time in local time, from mtime. Clamp to 1980-01-01 00:00:00 through
  2107-12-31 23:59:58. FILETIME values are 100 ns units since 1601-01-01 UTC.
- Find. Classic 4Eh/4Fh fill the DTA with the `DTAs` layout from `asm/VCDATA.INC`. Keep the search
  state in the 21 reserved bytes: a slot number plus a check value. The 71xxh find
  (714Eh/714Fh/71A1h) fills the `FDR` layout, with SI=1 meaning DOS date/time instead of FILETIME.
  Return `.` and `..` in every directory except the root, since VC shows `..` as UP--DIR. Wildcard
  rules: classic matching is 8.3-style against the alias. Long-name matching follows Windows, where
  `*` matches anything and `*.*` matches names without a dot too. Honor the attribute masks for
  hidden, system, directory and volume label. There is no volume label. Order entries by host
  name, bytewise.
- Handles. Use one global table of 64 DOS handles. 0 stdin, 1 stdout, 2 stderr, 3 aux and 4 prn are
  devices. A write to 1 or 2 calls `con_write` (declared in `hle.h`). A read from 0 returns 0 bytes.
  Handles 5 and up map to host file descriptors. A write with CX=0 truncates at the current
  position, as in DOS.
- Case map. Function 38h fills the `CNTRY` layout from `asm/VCDATA.INC`. Its `CaseMap` far pointer
  is `F000:0100`. The runtime maps that address to a far-callable stub that runs
  `void dos_casemap_upper(void)`, which you implement in `dos_fs.c`. It uppercases AL for CP866
  letters 80h-FFh and leaves everything else alone. Declare it in `runtime/dos_fs.h`. Country
  defaults: date format 2 (Y-M-D), date separator `-`, time separator `:`, 24-hour time,
  thousands `,`, decimal `.`, currency `$`.
- Unknown 71xxh subfunction: CF=1, AX=7100h, so VC falls back to the classic call.

## Functions to implement

Classic: 0Eh 19h 1Ah 2Fh 2Ah 2Bh 2Ch 2Dh 36h 38h 39h 3Ah 3Bh 3Ch 3Dh 3Eh 3Fh 40h 41h 42h 4300h
4301h 44h (00h device info, 01h set, 06h/07h status, 08h removable, 09h remote, 0Eh/0Fh logical
drive map, anything else error 1) 45h 46h 47h 4Eh 4Fh 56h 5700h 5701h (5702h-5707h error 1) 59h
5Ah 5Bh 60h 67h 68h 6Ch.

Long names: 7139h 713Ah 713Bh 7141h (SI=1 means wildcards plus a CX attribute mask) 7143h (BL
0 to 8) 7147h 714Eh 714Fh 7156h 7160h (CL 0, 1, 2) 716Ch 71A0h 71A1h 71A6h 71A7h 71A8h, and 7303h.
71A0h reports the file-system name `LINUX`, flags for case-preserved names and long-name support,
255 as the maximum name length and 260 as the maximum path.

Use Ralf Brown's Interrupt List semantics for inputs, outputs and error codes. Map errno values to
DOS errors: 2 file not found, 3 path not found, 4 too many open files, 5 access denied, 6 invalid
handle, 12 invalid access, 15 invalid drive, 16 remove current directory, 17 not same device,
18 no more files, 80 file exists.

## Gates (all must pass, run them before you finish)

1. `tests/test_dos_fs.c`, built and run by a new `make test-fs` target. It defines its own
   `Cpu cpu; uint8_t mem[MEM_SIZE];` plus stubs for `con_write`, `port_*`, `rt_*`. It must NOT
   link `runtime/cpu.c`, because another worker writes that file in parallel.
   It builds a temporary tree with: a long name, a Cyrillic name, a name with a character outside
   CP866, a hidden dotfile, a read-only file, a subdirectory, a symlink and a dangling symlink.
   It then checks through `dos_fs_int21` register calls:
   - classic and long-name find, with attribute masks and wildcards
   - alias generation and opening by alias
   - open/create/read/write/seek/close, and the CX=0 truncate
   - mkdir, rmdir, chdir, getcwd, rename, delete with and without wildcards
   - get and set attributes and times, truename in all modes, free space, country info
   - error codes for each failure case
2. **Watch a gate fail.** Break alias generation on purpose, confirm the suite goes red with a
   clear message, and restore it. Report what you broke and what failed.

## Constraints

- No network access. Plain C11 with glibc, no new libraries.
- Only create or modify: `runtime/dos_fs.c`, `runtime/dos_fs.h`, `runtime/cp866.c`,
  `runtime/cp866.h`, `tests/test_dos_fs.c`, `Makefile` (add targets only). If you write the
  CP866 table, put it in `runtime/cp866.c` with `cp866_to_ucs(uint8_t)` and
  `int ucs_to_cp866(uint32_t)` (-1 for none), because the terminal layer will reuse it.
- You cannot commit. Leave the work uncommitted.

## Required summary (end your run with this)

1. Files created or changed, one line each.
2. Every function implemented, and which ones only return a fixed answer.
3. Test count and results, with the command to rerun them. The planted-bug result.
4. Every deviation from this brief, and why.
5. Three real weaknesses of your work, each with a `file:line` citation.
