# Brief 36: rebase Hack onto main and review-ready fixes

Brief 33's Hack work is committed on this branch as one WIP commit. `main` has since gained VC 4.05
and browser H: files that load on first open (`runtime/web_files.c`, `tools/web_demo.py`,
`tools/web_modules.py`, Makefile changes). I started `git rebase main`; it stopped with conflict
markers in the files `git diff --name-only --diff-filter=U` lists. Your sandbox cannot write git
metadata: resolve the markers by editing files only, never run a git command that writes, and I
finish the rebase.

## What to do

- Resolve every conflict so that main's VC 4.05, lazy H: files, door mode and COMMAND.COM keep all
  their behaviour and tests, and Hack keeps all of its own.
- In the browser, Hack's executable and data files use main's lazy H: mechanism like everything
  else on H:: listed from the first screen, fetched on first open. First load must not grow
  beyond main's 922,702 gzip bytes by more than a few kilobytes; print the figure.
- Fix weakness 1 from your own brief 33 report: on Linux, two VC processes playing Hack at once
  share one playground (`runtime/hack_dos/platform.c:94`). Make concurrent games safe the way
  Hack itself did with its `perm` lock file and per-player save names, or keep the playground
  per process; say which and test it. Never write into `$HOME`.
- Every earlier program's generated C stays byte-identical, including VC 4.05's; prove it with
  `cmp`. HACK.EXE keeps its SHA-256
  `cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9`.

## Done means

`make test` (apart from the loopback TCP tests), `make web`, `make test-web` green. The summary
lists the conflict resolutions, the first-load size, and three weaknesses of your own work, each
with a real `file:line`.
