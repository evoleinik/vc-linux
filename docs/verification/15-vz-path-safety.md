# VZ path safety verification

VZ is an unmodified DOS program, so its path buffers and remembered 8.3
spellings need stricter handling than the surrounding Linux filesystem.
The fixes below stay in the runtime; no vendor source or COM byte is changed.

## Findings and boundaries

- `third_party/vzeditor/SRC/VZ.INC:476` defines `PATHSZ=64`, and
  `OPEN.ASM:763`–`815` copies a full path without a length check. The F4 bridge
  now accepts at most 63 path bytes, including the drive and directories.
  `tests/test_dos_exec.c:test_vz_path_buffer_limit` checks both the accepted
  63-byte boundary and refusal at 64 bytes through both VC execution paths.
- `MAIN.ASM:14` defines `TMPPATHSZ=32`; `gettmppath` at line 870 copies `TMP`
  and appends `\VZTEMP.$$$` without a bound. A valid Linux `TMPDIR` can
  overflow this buffer. VZ now receives its own short, mode-0700 temporary
  directory. The other child environments remain unchanged, and child exit
  removes the known VZ swap files and directory.
- VZ treats spaces, plus and comma as path separators. Both its executable
  path and the F4 file argument must use canonical short spellings; fixing
  the text-file argument alone does not let it load a `VZ.DEF` installed
  under a configuration path containing those characters.
- Generated `~n` aliases ordinarily depend on the current directory contents.
  Remembering one across an editing session could silently redirect a save
  when a sibling is inserted or deleted. The same risk applies to every
  parent-directory component, even when the text file itself is `NOTE.TXT`.

## Scoped path lease

`runtime/dos_fs.h` exposes prepare, bind-to-PSP, and release operations.
Preparation checks the canonical DOS spelling against the already-resolved
host file or directory without changing guest registers. The child gets a sparse lease
over that one path's components, not a global cache of directory aliases.
`runtime/dos_core.c:edit_in_vz` releases a pending lease if launch fails.

The file layer consults active leases before its exact-native-name fast path
and reserves their spellings during directory enumeration. Consequently
case-folded opens, classic FindFirst, attributes and truename all see the
same file. Other PSPs retain the normal alias rules. Process exit and full
filesystem reinitialization release the leases and their descriptors.
Directory leases hold the working directory's components stable before a
typed new file exists; the executable lease likewise stabilizes DEF lookups.

The lease holds the original inode open and checks its file type, identity and
its directory entry. A deleted or replaced original returns DOS error 5;
it cannot turn into creation of a literal stale short filename. A new native
8.3 entry conflicting with any leased component also returns error 5, even
if that entry is a hard link, symlink or dangling symlink. It never wins over
the lease. For pinned create/truncate calls, truncation occurs only after
the newly opened descriptor has been checked against the held original.

This is not a lock against another program writing the original inode in
place. Nor does it follow an external rename or inode replacement: those
situations deliberately fail closed. Repeated normal in-place VZ saves
remain valid.

## Failing-first evidence

The process tests also caught the startup-path defects before their fixes.
`test_vz_temp_environment` rejected the old shared `C:\tmp` value at its
child-private-directory assertion (line 388 at the time); the private TMP
environment fix passed the 1,457-check process suite.
`test_vz_child_keeps_directory_paths` then failed resolving VZ's DEF file
after a preceding configuration-directory sibling was deleted (line 448
at the time). Binding the executable and working-directory paths for every
VZ child made all 1,482 process checks pass, including that regression.

The real pre-safety executable failed these native PTY regressions:

- `test_vz_f4_short_alias_stays_bound_during_neighbor_changes[insert]`:
  saving `sameprefix-b.txt` changed `sameprefix-a.txt` instead.
- The same test with `[delete]`: saving created an unintended
  `samepr~2.txt`; the selected original was unchanged.
- `test_vz_f4_short_alias_stays_bound_when_parent_alias_changes`:
  deleting a sibling of the working directory made the remembered parent
  alias invalid; VZ reported an unwritable path and did not save `NOTE.TXT`.

The interrupt-client unit tests were then added before wiring the prepared
lease into lookup, enumeration and open. `make test-fs` was red with 4,452
checks and 29 failures. The failures included the redirected save, literal
short-name duplicate, truncation through native file/link conflicts,
vanished/replaced originals, and the parent-component case.

After wiring the lease, that same suite passed 4,446 checks with zero
failures. Extra checks then covered H:, descriptor counts, pending-launch
cleanup, owner-only process cleanup, and reinitializing with both pending
and bound leases, reaching 4,498 passing checks.

A later real PTY run exposed the corresponding new-file case: a typed
`vz NEW.TXT` has no pre-existing file to lease. Inserting a colliding working
directory sibling made VZ report a clean saved buffer but create the file
outside the selected directory; deleting the predecessor made its saved
path unwritable. Both cases were reproduced deterministically by
`test_vz_typed_new_file_keeps_cwd_when_parent_alias_changes[insert/delete]`.
The new directory-lease unit test was first red (4,506 checks, one failed
preparation), then the existing API was generalized to hold either a regular
file or directory with an unchanged inode and type. Creation after neighbor
insertion/deletion, native-name conflicts and replaced directories now pass.
Final filesystem results:

```text
make test-fs
Function coverage: 85 advertised INT21 functions/subfunctions exercised.
DOS filesystem tests: 4522 checks, 0 failures.

ASAN_OPTIONS=detect_leaks=0 make test-fs-sanitize
Function coverage: 85 advertised INT21 functions/subfunctions exercised.
DOS filesystem tests: 4522 checks, 0 failures.
```

The unmodified sanitizer command built successfully, but LeakSanitizer
terminated with its ptrace/sandbox incompatibility diagnostic. The second
command disables only leak detection; AddressSanitizer and UBSan stay on.
Actual lease descriptor counts are checked separately in
`test_path_lease_lifetime_and_home` rather than inferred from that run.
