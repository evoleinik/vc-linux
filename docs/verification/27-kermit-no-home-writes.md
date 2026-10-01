# Brief 27 verification — 2026-10-02

## Change and command

Linux installs `BBS.TAK` and `KERMIT.TXT` beside `KERMIT.EXE` in the config
directory. The home-directory target selection and its eligibility handling
are removed; there is no migration or cleanup of old home copies. Browser H:
installation and the selected-file association are unchanged. No commit was made.

The Linux command, typed on VC's command line, is:

```text
kermit take bbs.tak, stay
```

This uses Kermit's own TAKE search, not a new init file or modified vendor code:

- `third_party/mskermit/mssker.asm:1279`: TAKE calls `spath`.
- `third_party/mskermit/mssker.asm:2091`: after the current directory and Kermit's
  own path, `spath` searches DOS `PATH`.
- `third_party/mskermit/mssfil.asm:2842`: `fparse` splits drive/path and DOS 8.3
  filenames; it does not replace that search.
- `runtime/dos_core.c:1550`: Linux puts the config/program directory on DOS `PATH`.
- `third_party/mskermit/mssker.asm:1919`: command-line STAY suppresses the automatic
  EXIT. Putting it last also leaves VC's selected-file safeguard at
  `runtime/dos_core.c:774` unchanged: that separate prefix pins a selected file
  relative to the current directory before Kermit runs.

README, the shared CRLF `data/KERMIT.TXT`, and Linux BBS docs document the same
command. Linux still requires `VC_MODEM_555_1992=host:port` when starting VC.

## Red before the runtime change

First, only the test harness and startup regression were changed. Config, cache
and diagnostics were put outside the test HOME; the production installer had
not been edited. Running:

```sh
.venv/bin/python -m pytest -q tests/test_kermit_e2e.py::test_kermit_install_keeps_home_empty
```

failed with:

```text
AssertionError: VC wrote into HOME: ['BBS.TAK', 'KERMIT.TXT']
1 failed in 0.21s
```

After the fix, the focused Kermit suite passed 33 cases with its one TCP case
deselected. A further four-case run passed after adding exact previous-guide
bytes and normal VC shutdown checks.

## Gates

- Full native non-TCP `make test` gate: passed (exit 0), including 282 translator
  tests and 242 end-to-end cases, with the one Python TCP case deselected and
  only the C TCP target omitted. All other native targets passed.
- `make web`: passed.
- `make test-web`: passed, including browser Enter-on-BBS, data, hangup, EXIT,
  asset/ABI checks, failure/timeout recovery and fixed-memory exhaustion tests.
- Browser first load: 1,285,630 gzip bytes against the unchanged 1,300,000 cap.
- Scoped whitespace check passed; `data/KERMIT.TXT` remains ASCII with CRLF.

Native command (no permanent test skips or Makefile changes):

```sh
WATCOM="$PWD/build/openwatcom" \
PYTEST_ADDOPTS='--deselect=tests/test_kermit_e2e.py::test_kermit_dials_bbs_tcp' \
make -j4 -o test-modem-transport test
```

Web commands used the installed SDK, with its writable cache in this worktree:

```sh
EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
EM_CACHE="$PWD/build/emcache" \
make -j2 web test-web \
  EMCC=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc \
  NODE=/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin/node
```

The two excluded socket tests are `test-modem-transport` and
`tests/test_kermit_e2e.py::test_kermit_dials_bbs_tcp`. They remain enabled for the
user's socket-capable run. The syscall unit and pipe tests do not substitute for
real TCP verification. Logs are in `build/brief27-{native,web}-tests.log`.

The Linux BBS tests use the documented command from a working directory, a
nested directory, and empty H:, through both COMSPEC and INT 2Eh execution.
They check HOME at startup, after BBS hangup/return to VC, and after normal VC
shutdown. Existing home files, nested user data, live/dangling symlinks, and the
exact old guide are compared by inode, mtime/ctime, mode and contents.
`BBS.TAK` and `KERMIT.TXT` config defaults retain their inode, mtime, mode and
contents on a second startup sharing the first VC's installation.

## Three weaknesses

1. `README.md:162`: choosing native TAKE lookup means a current-directory
   `BBS.TAK`, including an untouched legacy home copy, shadows the config script.
2. `runtime/main.c:197`: existing config guides are user-owned and preserved,
   so an older guide already in that directory can retain stale instructions.
3. `tests/test_kermit_e2e.py:296`: HOME checks are snapshots, not a continuous
   filesystem trace; they can miss a file created and deleted between checks.
