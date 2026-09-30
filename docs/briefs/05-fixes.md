# Brief 05: fix verified review findings (translator, DOS file layer, terminal)

Read `README.md` and `docs/plans/2026-09-30-native-port.md` first. A read-only review
(`docs/briefs/04-review.md`) found these defects. Each was checked against the code. Fix exactly
these. Another worker is changing `runtime/dos_core.c`, `runtime/main.c`, `runtime/rt.c`,
`data/` and `tests/test_e2e.py` at the same time, so do not touch those files.

## A. Translator: bytes the CPU runs but the listing calls data (blocker)

`asm/VCPANELS.INC:1887` defines `_JCXZ`, which emits a JCXZ as two `DB` bytes for JWasm. The
translator treats every DB row as data. When execution falls through into those bytes, the
dispatcher faults with "no translated code". `translator/layout.py` has a special case,
`_decode_entry_data`, that fixes the same problem for the program entry point only.

Make it general:
- After building the instruction list, find every address the CPU can reach without a dynamic
  transfer: the fall-through successor of every instruction that is not an unconditional
  transfer (JMP, RET, RETF, IRET, and INT 20h is not special), every direct JMP, Jcc, LOOP,
  JCXZ or CALL target, and the entry point.
- Any such address that is not an instruction start is decoded with capstone, following
  fall-through, until a known instruction start is reached. Fail loudly on overlap with a
  listed instruction or on undecodable bytes.
- Repeat until nothing changes, because newly decoded instructions add successors.
- Replace `_decode_entry_data` with this. The unicorn gate picks up the new instructions by
  itself.
- Add a layout check to `tests/test_translator_layout.py`. After the closure, every fall-through
  successor and direct branch target must be an instruction start or outside the image. Both
  `_JCXZ` sites must now be instructions.
- Watch it fail: disable the closure, confirm the new check goes red on the `_JCXZ` site, then
  restore.

## B. Deleting a symlink to a directory must not reach the target (blocker, data loss)

VC deletes a directory by trying `rmdir` first (`asm/VCDELETE.INC:510`, `F38_65`). Only when that
fails does it list the contents and offer to delete them recursively. For a symlink to a
directory, `rmdir()` fails with ENOTDIR. VC then lists `link\*.*`, which follows the link into
the target, and one confirmation later deletes the target's files.

Fix: in the 3Ah and 713Ah handlers, when the final path component is a symlink to a directory,
or a dangling symlink, remove the link with `unlink()` and report success. A symlink to a
regular file keeps failing, as `rmdir` on a file does. Test it in `tests/test_dos_fs.c`:
- a symlink to a nonempty directory: rmdir succeeds, the link is gone, the target and its files
  are untouched
- a dangling symlink: rmdir succeeds
- a symlink to a file: rmdir fails

## C. Names that differ only in unrepresentable characters (blocker, data loss)

`face-😀.txt` and `face-😃.txt` both become `face-?.txt`. Lookup takes the first match, so F8 on
the second entry deletes the first. `?` is also a DOS wildcard.

Fix, in `runtime/dos_fs.c`:
- A character with no CP866 form becomes CP866 byte FEh (■), never `?`.
- Within one directory, a converted name that equals another entry's name, case-insensitively,
  gets a deterministic suffix `~N` before its extension. This applies whether the other entry
  was converted or already had that name. Assign N in bytewise host-name order, lowest free N
  first, the way aliases work.
- Resolving a DOS name component maps it to exactly one host name by regenerating this
  directory's name set. An ambiguous or missing match is "file not found". Never pick the first
  of several.
- A `?` or `*` in a name given to a non-wildcard function is literal, never a pattern.
- Tests in `tests/test_dos_fs.c`: two such emoji names get distinct DOS names that both round
  trip. Deleting the second removes only the second. A real file named like a converted name
  still resolves to itself, and the converted one gets the suffix.
- Update the existing checks that expect `?`.

## D. Terminal leaks raw mode on some signals (minor)

`runtime/term.c:77` handles only some fatal signals. SIGUSR2, SIGALRM, SIGVTALRM, SIGPROF, SIGXCPU,
SIGXFSZ and SIGPIPE kill VC without restoring the terminal. Route every catchable signal whose
default action terminates through the same cleanup, then re-raise it with the default action.
SIGUSR1 is NOT one of them: `runtime/rt.c` uses it for a state dump, so leave it alone.

## E. Kitty keypad navigation codes are dropped (minor)

`runtime/term.c:784`. With the kitty protocol on and NumLock off, the keypad arrow, Home, End,
PgUp, PgDn, Ins, Del and Begin keys arrive as codes 57417 to 57427 and are discarded. Map them to
the existing navigation handlers, as grey-key equivalents. Add parser tests.

## F. INT 21h AH=0Ch leaves type-ahead behind (minor)

The flush in `runtime/bios.c` (`dos_con_int21`, function 0Ch) empties the BIOS ring. Keys still
waiting in the terminal layer's `pending` queue survive and come straight back. Also call
`term_clear_pending()` (in `runtime/term.h`). Add a test to `tests/test_term.c`.

## Gates

- `make test-translator test-fs test-term` all pass.
- The watch-it-fail step in A, and one planted defect each for B and C, each shown going red and
  then restored. Report what you broke and what failed.
- No network. Use `.venv/bin/python`. You cannot commit, so leave the work uncommitted.
- Only touch: `translator/**`, `tests/test_translator_*.py`, `tests/translator_support/**`,
  `runtime/dos_fs.c`, `runtime/dos_fs.h`, `runtime/cp866.*`, `runtime/term.c`, `runtime/term.h`,
  `runtime/bios.c`, `runtime/bios.h`, `tests/test_dos_fs.c`, `tests/test_term.c`.

## Required summary

1. What you changed for A to F, one line each, with `file:line`.
2. Test results and the rerun commands. The three planted-defect results.
3. Every deviation from this brief, and why.
4. Three real weaknesses of your changes, each with a `file:line` citation.
