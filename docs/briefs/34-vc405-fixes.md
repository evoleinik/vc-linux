# Brief 34: two fixes to the VC 4.05 lane

Brief 32's change is uncommitted in this worktree. A review found two defects that can damage
user settings. For each, first write a test that fails on the current code and show it red, then
fix and show it green. Same toolchain and limits as brief 32: no git writes, loopback sockets
blocked.

## 1. On Linux, `vc` typed on VC's command line can now start a nested translated VC

`runtime/dos_core.c:754-771`, used at `:803`. Before this branch, `vc` on Linux always ran the
host `vc` command. Now, in a directory holding a VC.COM byte-identical to 4.99.09's (for example a
config directory from `XDG_CONFIG_HOME=/tmp/x vc`), typing `vc` starts a nested translated 4.99
with that directory as its home, and Shift-F9 writes its VC.INI there. Restore the old meaning on
the native non-door build: `vc` and every other name that meant a host command before this branch
still mean it. `vc405` is the only new name. Browser and door keep this branch's behaviour.
Gate: a pty test in a directory holding a copy of 4.99.09's VC.COM, where typing `vc` reaches the
host command exactly as on main.

## 2. 4.05's private `VC=` setting leaks into a nested 4.99

`runtime/dos_core.c:1489` and the DOS2 path at `:1564-1571`. 4.05's `VC=H:\VC405` passes down to
all its children. A nested 4.99 (for example 4.05 → COMMAND → `H:\.VC\VC.COM`) reads it as its
home, and Shift-F9 there writes a 4.99-format VC.INI over 4.05's, so the next 4.05 start reports
"VC.INI is not correct". Remove `VC=` from the environment of any child that is VC 4.99.09
(`image_vc_com`); 4.99 never saw `VC=` before this branch. Gate: that exact chain in the web smoke
or a pty test, then 4.05 starts cleanly with its own settings.

## Done means

`make test` (apart from the loopback TCP tests), `make web`, `make test-web` green. The summary
gives red and green evidence for both and three weaknesses of your own work with real
`file:line`.
