# Brief 20: resolve the VZ rebase onto Rogue

This worktree is in the middle of `git rebase main`. Main now holds the Rogue work (commit
e10da4c: Rogue 5.4.4 compiled with OpenWatcom and translated by a new compiled-code front end).
The commit being replayed is the VZ Editor work. Git stopped on conflicts in 16 files:

    Makefile  README.md  runtime/dos_core.c  runtime/image.h  runtime/main.c  runtime/rt.c
    tests/test_dos_exec.c  tests/test_install_e2e.py  tests/test_machine.c
    tests/test_rt_process.c  tests/test_translator_ops.py
    tests/translator_support/ops_build.py  tests/web_smoke.mjs  tools/web_demo.py
    translator/__main__.py  web/README.TXT

Resolve every conflict so that both features work together, exactly as each did alone. Read both
sides of each conflict, and understand what each side's change was for before choosing. Never
drop either side's behaviour or tests. Typical cases:
- Lists that both sides extended, such as image tables, installed files, `MAX_KNOWN`, PATH
  entries, embedded demo files, Makefile targets and test lists: keep every entry from both.
- Counters both sides changed, such as test totals or size limits: recompute them for the
  combined build.
- The README's "Preserved so far" table: add VZ Editor 1.6 (c.mos, Village Center, 1990s, BSD-3)
  beside Rogue.

You cannot run git commands that write. Edit the files only. Leave no conflict markers anywhere
(`grep -rn '^<<<<<<<\|^>>>>>>>' .` must be empty outside `build/` and `third_party/`).

Then run every gate on the combined tree: `make test`, `make vz`, `make rogue`, `make web`,
`make test-web`. Toolchains: Emscripten as in `docs/briefs/10-browser.md`;
`WATCOM=$HOME/src/vc-linux-wt/tools-cache/openwatcom` for Rogue. Fix anything the combination
breaks, with a test where the break was not already caught.

## Done means

No conflict markers, all gates green. Your summary lists each conflicted file and how you
combined it, and names three weaknesses of your own work, each with a real `file:line`.
