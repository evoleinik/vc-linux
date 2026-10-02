# Brief 37: fixes to Hack from review

The Hack branch is committed and rebased onto main (which now has the phone layout). A review
found the defects below. For each, first write a test that fails on the current code and show it
red, then fix and show it green. Same toolchain and limits as before: no git writes, loopback
sockets blocked.

1. Major: `hack` shadows a real host command. Debian and Ubuntu's `bsdgames` installs
   `/usr/games/hack`, and `/usr/games` is on Ubuntu's default PATH. On Linux, take Hack's
   directory off the native DOS PATH and accept only the typed word `hack103`, exactly the way
   `vc405` works (`runtime/dos_core.c` around 1008-1013). Typed `hack` must reach `/bin/sh`
   as on main. Update README, KERMIT-style guides and tests to `hack103` on Linux. Browser and
   door keep `H:\GAMES\HACK\HACK.EXE`.
2. Minor: secondary playgrounds keep a stale HACK.EXE after an upgrade
   (`runtime/dos_core.c:1610-1615`), so the second concurrent game fails with DOS error 11 until
   someone deletes the PLAY directories. Under the slot's lock, rewrite the slot's HACK.EXE and
   data files when they differ from the installed ones.
3. Minor: saves and bones carry no build identity (`runtime/hack_dos/save_io.c:72`,
   `platform.c:87`). Upstream discarded saves older than the binary (`hack.unix.c:216-227`).
   Put a build stamp in the 16-byte save header and reject a mismatched save or bones file with
   Hack's own "out of date" message, deleting it as upstream did.
4. Minor: the playground lock uses `flock` on a directory, which fails on NFS homes
   (`runtime/dos_core.c:1573-1588`). Lock a regular `VCPLAY.LCK` file opened read-write with
   `F_OFD_SETLK`, then re-check the path still names the locked file.
5. Minor: README says Hack has a Source panel view, but `tools/source_maps.py` has no `HACK.EXE`
   builder and `WEB_SOURCE_INPUTS` lacks `HACK.MAP`. Add the builder, modelled on Rogue's, so the
   claim is true.
6. Nit: `runtime/hack_dos/terminal.c:221` treats DEL as a one-character erase; upstream
   `hack.tty.c:259` clears the whole line on DEL. Match upstream.

Done means `make test` (apart from the loopback TCP tests), `make web`, `make test-web` green,
HACK.EXE keeps its SHA-256 unless a fix truly requires a rebuild (then pin the new one and say
why), and the summary lists red and green evidence and three weaknesses with real `file:line`.
