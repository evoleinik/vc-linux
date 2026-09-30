# Brief 07: file-layer defects from the verification review

Read `README.md` first. A focused review (`docs/briefs/06-verify.md`) of the fixes from
`docs/briefs/05-fixes.md` found these defects in `runtime/dos_fs.c`. Fix exactly these. Another
worker is changing `runtime/dos_core.c`, `runtime/main.c` and `tests/test_e2e.py` at the same time,
so do not touch those.

## 1. Collision suffixes must not depend on neighbours (blocker, data loss)

Names with unrepresentable characters convert to CP866 with FEh (■). Colliding converted names get
`~N` in host-name order (`runtime/dos_fs.c:405`). Deleting one renumbers the rest.

Scenario: `face-😀.txt`, `face-😃.txt` and `face-😄.txt` show as `face-■.txt`, `face-■~1.txt` and
`face-■~2.txt`. Select the first two and press F8. After the first is deleted, `face-😄.txt`
becomes `~1`, so VC's saved selection deletes the unselected file.

Fix: a converted (lossy) name must be a function of that file's own host name only.
- Every lossy name gets a suffix `~XXXX` before its extension, even when nothing collides.
  XXXX is 4 uppercase hex digits of a fixed hash of the host name's UTF-8 bytes. Use FNV-1a 32,
  folded to 16 bits.
- If the result still equals another entry's DOS name case-insensitively, whether that entry is
  real or converted, NEVER rename either one. Mark the lossy entry ambiguous: it is still listed,
  but resolving that DOS name fails with error 2. A real (lossless) name always keeps its own
  name and always resolves.
- Lossless names are unchanged.
- Remove the old positional `~N` assignment for lossy names. 8.3 aliases are out of scope: VC
  deletes and renames by long name.
- Tests in `tests/test_dos_fs.c`:
  - three emoji names get stable names
  - after deleting any one of them, the other two keep exactly their names
  - the scenario above, run through find, delete and delete, removes exactly the two chosen files
  - a forced hash collision (two host names giving the same suffix) resolves to error 2 for the
    ambiguous name and never to the other file
  - update checks that expect `~1`

## 2. A symlink listed as a file must be deletable (major)

Symlinks to an ancestor directory are listed as files (`links_to_ancestor`, around line 850), so
F8 deletes them with 41h/7141h. `unlink_path` (around line 1373) follows the link to check for a
directory and refuses. Fix: when the final component is a symlink, `unlink()` the link itself,
whatever it points at. Tests: 41h and 7141h on a symlink to an ancestor directory, and on a symlink
to a normal directory. Both remove the link and leave the target alone.

## 3. Long-name conversion must not list directories it does not need (minor)

AX=7147h and AX=7160h CX=2 now list every parent directory to convert names. A parent that is
execute-only (mode 0111) fails the call, though the path is reachable. Fix: a component whose
host name converts to CP866 without loss is converted directly, with no listing. List the
directory only when the component is lossy. Test with an execute-only parent.

## Gates

- `make test-fs` passes. Also run `ASAN_OPTIONS=detect_leaks=0 make test-fs-sanitize`.
- Watch it fail: restore the positional numbering for item 1 and show the stable-name test going
  red, then restore.
- No network. You cannot commit, so leave the work uncommitted.
- Only touch `runtime/dos_fs.c`, `runtime/dos_fs.h` and `tests/test_dos_fs.c`.

## Required summary

1. The change for each item, with `file:line`.
2. Test results and the planted-defect result.
3. Deviations, and why.
4. Three real weaknesses of the change, each with `file:line`.
