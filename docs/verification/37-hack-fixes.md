# Brief 37: Hack review fixes

## Scope and preservation

The six findings in `docs/briefs/37-hack-fixes.md` were reproduced before their
fixes. The user's brief was not edited. No Git metadata, vendored source,
network service, or browser session was changed or used.

The original HACK.EXE was 243,304 bytes, SHA-256
`cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9`.
The save-header and line-editor fixes require different DOS instructions/data.
The new pinned executable is **243,478 bytes**, SHA-256
`b6f6ca8667fc8d1e37eb81fbd1c469a371312e4a39c53052985d553aaac345ee`.
The pinned OpenWatcom toolchain independently reproduces every byte in another
directory. All 92 original NetBSD files remain unchanged.

The build identity is a 32-bit SHA-256 prefix of the linked image with its
four-byte identity field normalized to a fixed placeholder. The builder links
the placeholder, recompiles just the identity data module, and relinks; it
requires that only those four bytes differ. No executable patching or relaxed
translator proof is involved. A regression proves that the identity's DATA
bytes are checked against the original OMF object, including rejection of a
one-byte mutation.

## Red and green evidence

1. **Native command ownership.** The updated startup cases plus new shell
   collision tests failed **18/18** on the original runtime. They demonstrated
   both missing `hack103` dispatch and capture of host `hack`, mixed-case,
   suffixed, and renamed commands through COMSPEC and INT 2Eh. Two additional
   tests showed `HACK` still present in native DOS PATH. Captures:
   `build/hack-fixes-command-red.log`, `build/hack-fixes-path-red.log`.
   Only the typed word `hack103` now selects the installed game. `hack` and
   other typed names retain host-shell behavior even when a byte-identical
   DOS copy is in the current directory. Like `vc405`, native Enter sends a
   shell command and follows that same rule; explicit COMMAND.COM EXEC still
   identifies files by complete bytes. Browser/door Enter, paths and commands
   stay put. The old native Enter test now checks host noncapture followed by
   an explicit `dos2 /c HACK.EXE` launch of the selected copy. All **44** native
   Hack cases pass, together with the **10** playground cases in the final
   aggregate (`build/hack-fixes-make-test-final.log`: **54 passed**).

2. **Secondary-slot upgrades.** The regression created a secondary save,
   stopped its owner, damaged static assets, and resumed through another VC.
   The original code rejected its stale HACK.EXE with DOS error 11. Additional
   red cases covered changed installed help, linked assets, and a live owner.
   Refresh now runs after acquiring the slot lock, compares full contents,
   and atomically replaces only differing static files. It reads customized
   installed data, including read-only symlink references, and leaves saves,
   bones, levels, scores, and `perm` untouched. Identical files retain inode
   and modification time; a live foreign slot is never refreshed. All **10**
   new playground cases failed on the original runtime and now pass; the
   **7** existing concurrent/replaced-directory cases also pass. Captures:
   `build/brief37-playgrounds-green.log` and
   `build/brief37-playgrounds-smoke.log`.

3. **Build identity in saves and bones.** The initial DOS regression subset
   had **7 failed, 2 passed** across this finding and DEL. Failures included a
   missing identity, accepting legacy saves/bones, quarantining a different
   build's save, and omitting the original stale-bones message. Extending the
   test to wizard mode exposed **2 further failures**: the old debug exception
   retained rejected bones. The extended subset is **11 passed**; the complete
   DOS build suite is **58 passed**. Stale files now receive
   `Saved level is out of date. ` and are closed/deleted before pointer-bearing
   recovery. Valid wizard bones are retained. Current-build corruption still
   uses the non-overwriting HACK.BAD quarantine. Two whole-game native cases
   also verify the visible message, deletion, and a new dungeon for legacy
   and different-build saves. See
   `runtime/hack_dos/MUTATIONS.md` for the exact selectors and evidence.

4. **NFS-compatible playground locking.** A successful original launch
   failed the new regular-file lock test because VCPLAY.LCK did not exist.
   Other red tests demonstrated ignored record-lock contention and lock-path
   replacement. Every playground now has an O_RDWR regular VCPLAY.LCK locked
   with F_OFD_SETLK. Device/inode checks after acquisition and before launch
   reject a replaced lock pathname. Tests inspect the live OFD lock, contend
   using POSIX record locks, replace a retained pathname, reject a lock
   symlink, and inject replacement immediately after successful fcntl. These
   are included in the **10 passed** playground suite above.

5. **Hack Source panel.** The original Node test failed because its source
   index omitted HACK.EXE. The shared Rogue/Hack builder and HACK.EXE/HACK.MAP
   prerequisites now pass the independent map oracle and all **13** isolated
   source-map mutation probes. Hack has **608** real CODE functions and
   **4,784** validated CALL ends; no C line numbers are invented. Initial and
   final captures: `build/brief37-source-maps.log` and
   `build/brief37-source-final.log`. The full web smoke also exposed stack
   scanning beyond VC's real stack top, incorrectly listing an old Hack
   return address after exit. A focused regression reproduces that problem;
   the lazy maps now carry original MZ stack metadata for bounded scanning.
   Both caller regressions and the independent MZ-metadata assertion went
   red then green, while the alternate-stack case remains green. The full
   web smoke opens Source during an actual Hack game, checks its function
   name/offset against HACK.MAP, and verifies the unchanged game screen.
   Captures: `build/brief37-source-stack-bounds.log` and
   `build/hack-fixes-test-web-green.log`.

6. **DEL editing.** Actual linked getlin tests produced `olreplacement` and
   `onew` on the old image. DEL now clears the entire line, including empty
   and repeated DEL; Backspace remains a single-character erase and Ctrl-U
   still clears the line. All four real-instruction cases pass in the DOS
   suite above.

## Aggregate gates

`make web` and `make test-web` both completed with status 0. Final captures:
`build/hack-fixes-web-final.log` and `build/hack-fixes-test-web-green.log`.
The latter includes the full fifteen-module smoke, Hack Source, no-WebCrypto,
lazy rename/RMDIR, failure/timeout/memory recovery, and DOS command regressions.
The first load is **927,567 gzip bytes**: below 1,300,000 overall and 4,865
above the 922,702-byte main baseline (5,000-byte allowance).
The VC405 static mutation harness also rejects/restores all **11** defects,
checks all nine Source registrations, and preserves all **17** earlier
generated-C hashes (`build/brief37-vc405-web-mutations/summary.log`).

`make test` completed with status 0, captured in
`build/hack-fixes-make-test-final.log`. The general native suite reports
**334 passed**, nested-shell/I/O **11 passed**, and combined Hack/playground
**54 passed**. All **347** translator tests passed; the exhaustive instruction
gate covered **294,026** distinct instruction/location pairs and **9,408,832**
successful states, with 5,609 expected divide-error cases. The invocation
excludes only the brief's two loopback checks, rather than weakening test code:

```sh
export WATCOM="$PWD/build/openwatcom"
LSAN_OPTIONS=detect_leaks=0 \
  PYTEST_ADDOPTS="-s -k 'not test_kermit_dials_bbs_tcp'" \
  make -j4 -o test-modem-transport test

export EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten
export EM_CACHE="$PWD/build/emcache"
export PATH=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten:/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin:$PATH
make -j3 web
make -j3 test-web
```

LeakSanitizer's ptrace-based exit check is unavailable in this sandbox;
AddressSanitizer and UndefinedBehaviorSanitizer remain enabled where provided
by the existing gates. Node subprocess oracles capture through temporary
regular files because nested pipe capture is restricted here. Their assertions
and child return codes remain mandatory.

The first aggregate native attempt failed one existing DOS-shell return test
while extra focused PTY suites were creating/deleting `/tmp/vc-bas-*` siblings;
its captured screen showed numbered 8.3 aliases changing. Both parameter cases
passed in isolation (`build/hack-fixes-dos-shell-recheck.log`). The final full
native run has no additional PTY workloads alongside it. No DOS-shell test was
changed or excluded. The two existing Landlock ABI-6 signal-scope tests skip
on this kernel's ABI 4; no new skips were introduced.

## Changed areas

- `runtime/dos_core.c`: native alias/PATH, slot refresh, regular-file locks.
- `runtime/hack_dos/{build_stamp.c,save_io.c,platform.c,terminal.c}` and
  `tools/{build_hack.py,hack_port.py}`: build identity and DOS behavior.
- `tools/source_maps.py`, `Makefile`: Hack Source maps, dependencies and gates.
- `web/vc-source.js`: stop caller scanning at the matching MZ stack boundary.
- Hack build/native/playground tests and `tests/hack_lock_race.c`; Source map
  tests, Source caller-bound regressions and their isolated mutation fixtures;
  the native gameplay probe's renamed test selector; the browser smoke's
  rebuilt-executable checksum pin.
- README, the plain-text browser guide, DOS-port documentation, and this record.

## Three remaining weaknesses

1. `tools/build_hack.py:42`: the identity is only 32 bits, so collisions are
   possible; neither it nor the payload checksum authenticates hostile saves.
2. `runtime/dos_core.c:1721`: allocation is limited to 64 secondary slots;
   persistent or incompletely initialized directories are not garbage-collected.
3. `runtime/dos_core.c:1642`: asset publication is atomic but is not fsync'd;
   power-loss durability is not guaranteed, though later acquisition compares
   and repairs the assets again.

## Known limits after verification (2026-10-02)

- Deleting or renaming `HACK/VCPLAY.LCK` while a game runs lets a second VC take the same
  playground. Any lock keyed to a file name has this weakness.
- Running a byte-identical copy of `HACK.EXE` from `$HOME` by hand (for example `dos2 /c HACK.EXE`
  there) leaves `VCPLAY.LCK` in `$HOME`. Typed `hack103` never writes there.
- After an upgrade, resuming a secondary playground by hand (`cd PLAY000N`, then
  `dos2 /c HACK.EXE`) fails with DOS error 11, because only `hack103` refreshes a slot. The old
  save would be rejected as out of date anyway.
