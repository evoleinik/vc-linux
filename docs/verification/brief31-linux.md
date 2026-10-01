# Brief 31: Linux command isolation

## Red, before changing production code

The original brief-30 `build/vc` (mtime 2026-10-02 05:52) was still available.
Tests used separate short `/tmp/vc-bas-*` working/config directories, with no
DOS executable in the working directory. No real home directory was touched.

```
.venv/bin/python -m pytest -q tests/test_install_e2e.py -k preserves_host_shell_commands
10 failed, 39 deselected in 4.93s
```

Each of `find . -name MATCH.TXT`, `sort INPUT.TXT`, `more INPUT.TXT`,
`fc INPUT.TXT MATCH.TXT`, and `command -v git` failed through both COMSPEC and
INT 2Eh. Instead of executing the host script, the log showed, respectively,
`translation FIND.EXE`, `SORT.EXE`, `MORE.COM`, `FC.EXE`, and `COMMAND.COM`.
The shell-created `STATUS.TXT` did not exist. For example:

```
exec C:\bin\sh tail[70] "/C find . -name MATCH.TXT > HOST.TXT 2>&1; printf '%s' $? > STATUS.TXT"
load C:\tmp\vc-bas-sl_hppuq\home\.config\vc-linux\find.EXE: translation FIND.EXE
```

The test independently executes the same command through `/bin/sh` in the
same directory, then compares its combined stdout/stderr and exit status
against VC's actual host-shell run. In particular, `fc` must preserve the
host shell's missing-command error on hosts where it is absent, not silently
replace it with DOS FC.

```
.venv/bin/python -m pytest -q tests/test_install_e2e.py \
  -k 'msdos_programs_in_config or unchanged_flat_dos' --tb=short
5 failed, 49 deselected in 1.47s
```

The layout test could not open `DOS2/COMMAND.COM`; the migration tests found
the old flat COMMAND still present, user-modified files rewritten, or a
dangling user symlink replaced. The symlink case also lacked the new DOS2
installation.

## Implementation

Linux installs the seven original programs under config `DOS2`, plus an
unchanged copy of COMMAND's bytes as config `DOS2.COM`. Only COMMAND's child
environment gets the DOS2 directory prepended to PATH. The initial native
VC's installation directory supplies this path even when a renamed COMMAND
copy is launched elsewhere. Parent PATH and COMSPEC remain unchanged;
browser and door environment setup do not receive the Linux addition.

After publishing the new files, migration removes only exact regular-file
copies of the old flat bundled programs. Modified files, directories,
symlinks and dangling symlinks remain user-owned. The existing atomic
installer and matching-file inode/mtime tests include DOS2 and its alias.

## Green

```
.venv/bin/python -m pytest -q tests/test_install_e2e.py \
  -k preserves_host_shell_commands --tb=short
10 passed, 44 deselected in 5.85s

.venv/bin/python -m pytest -q tests/test_msdos_e2e.py --tb=short
10 passed in 17.06s

.venv/bin/python -m pytest -q tests/test_install_e2e.py tests/test_msdos_e2e.py --tb=short
64 passed in 36.68s
```

The final combined run used the rebuilt native binary with the updated
DOS.TXT embedded. It includes all five host command names on both VC command
paths, the clean DOS2 installation, unchanged/modified/link/dangling-link
migrations for all seven programs, atomic replacement and no-rewrite checks,
the unchanged browser demo layout, and the original shell/utility pty suite.
The renamed-shell test really runs SORT from DOS2, reads its output file, and
returns to VC. After EXIT, the host `command printf ...` builtin proves that
the child's DOS2 PATH did not leak back into VC.

The first green attempt exposed a test-harness difference: GNU MORE prints
a filename header when its stdin is not a tty, unlike its execution from VC.
The independent `/bin/sh` reference now receives pty stdin and the same
PATH/LANG/TERM as VC. No expectation was weakened; output and exit status
still must match exactly. The already-red old binary failed before that
comparison because it never executed the host shell or created STATUS.TXT.

`cc -std=c11 -D_GNU_SOURCE -I runtime -fsyntax-only -Wall -Wextra
runtime/main.c runtime/dos_core.c` also passed. DOS.TXT retains CRLF;
`git -c core.whitespace=cr-at-eol diff --check` is clean for these files.

## Remaining limits

COMMAND's original COMSPEC buffer permits fewer than 40 DOS path bytes;
`command_environment` continues to reject longer resolved names rather than
overflow it. Migration deliberately leaves user-created flat symlinks and
edited files alone, so the clean-install routing gate does not promise to
override an explicit user-created DOS executable on the search path.
