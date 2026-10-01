# Brief 27: never write into the user's home directory on Linux

Brief 26's change is uncommitted in this worktree. On Linux, `runtime/main.c` installs `BBS.TAK`
and `KERMIT.TXT` into the user's real `$HOME` (H:) on every start, and keeps them there. A file
manager must not drop files into someone's home directory. The browser's H: is a demo drive and
stays as it is.

- On Linux, install `BBS.TAK` and `KERMIT.TXT` in the config directory with `KERMIT.EXE` and the
  other installed files, and never create, rewrite or migrate anything in `$HOME`. Remove the
  home-directory install and its migration code.
- Dialing on Linux must still be one short command typed on VC's command line, from any
  directory. Read Kermit's TAKE code (`mssker.asm`, `mssfil.asm`) to see whether TAKE searches the
  DOS `PATH`. If it does, `kermit take bbs.tak` works as is. If not, find the simplest way that
  needs no edit to Kermit's source, such as Kermit's own init-file search. Say which you used.
- Update the e2e tests: they must fail if any file appears in `$HOME` after VC starts and after
  the BBS workflow, and the Linux dial must use the command the README documents. Show the new
  test red on the current code first.
- Update `README.md`, `KERMIT.TXT` and the Linux part of the BBS docs with the exact command.
- The TCP tests need sockets, which your sandbox blocks. Run everything else; I run the TCP tests.

Done means `make test` (apart from the two TCP tests), `make web`, `make test-web` green, and the
summary names three weaknesses of your own work, each with a real `file:line`. Do not commit.
