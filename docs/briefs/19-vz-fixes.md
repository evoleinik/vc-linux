# Brief 19: three fixes to the VZ Editor change

A review of the uncommitted VZ work in this worktree found three problems. Fix each one. For each,
write a test that fails on the current code first, show it failing, then fix it. Keep every gate
green: `make test`, `make vz`, `make web`, `make test-web` (toolchain in
`docs/briefs/10-browser.md`). Do not commit.

## 1. Saving with VZ's backup option must work

`runtime/dos_fs.c:439` (`resolve_leased_name`), `:364` (`lease_identity`) and `:2095`
(`rename_file`, which ignores leases). On the VZ side, `TEXT.ASM:1192-1203` and `:308-312` make
the backup, and `VZIBM.DEF:384` puts the `Eb` toggle in VZ's options popup.

Steps: F4 on `NOTE.TXT`, turn on Backup File (or use a VZ.DEF with `Eb+`), edit, then Alt-S.
VZ renames `NOTE.TXT` to `NOTE.BAK`, and that succeeds. Then VZ reopens `NOTE.TXT`, the lease's
identity check fails, the open returns error 5, and VZ shows a write error. `NOTE.TXT` is gone
from the panel. Every path through that directory now fails, Save As to a new name included, so a
user who quits loses the edits. Anything that replaces the file from outside breaks it the same
way: `git checkout`, Syncthing, `sed -i`, or `mv` from VZ's DOS shell.

Fix: for a directory component, check only that component's identity, not the file's. When the
owning program renames the leased file, move or drop the lease. A file replaced from outside must
still be saveable. E2e tests: a save with `Eb+` leaves `NOTE.TXT` with the edit and `NOTE.BAK`
with the original. A file replaced from outside while VZ has it open still saves.

## 2. A long current directory must not reach VZ's 64-byte path buffers

`runtime/dos_core.c:928` only checks that the short current directory is under 64 bytes. VZ's
`makefulpath` (`OPEN.ASM:758-815`, called at `:919`) joins drive, current directory, `\` and the
name with no length check. It writes into PATHSZ=64 buffers (`VZ.INC:476`), then into the text
record's `path` field (`VZ.INC:886`), which live fields follow.

So with a short current directory of 54 to 63 bytes, `vz NOTES.TXT` writes past those buffers.
VZ's own Open with a relative name in that directory does the same. Likely results: a save under
a cut-off name, or a jump through a broken pointer that ends VZ with its unsaved buffer.

Fix: refuse to start VZ, with a one-line DOS message, when the short current directory plus 13
bytes reaches 64. Turn each command-line file name into its full short path with INT 21h AH=60h,
and refuse it at 64 bytes. E2e test with a deep directory.

## 3. A failed save must not leave a mix of old and new text

VZ saves in place. It opens the file for update (`TEXT.ASM:173-187`) and overwrites it, and
truncates only when the write succeeded (`TEXT.ASM:242-252`). `write_file` (`dos_fs.c:1795`)
correctly reports a short write or error 39. When the disk or quota fills during an F4 save, the
file keeps the new start and the old end, and with `Eb` off by default no copy exists. The old
F4 default kept the original.

Fix: with item 1 fixed, ship the installed VZ.DEF with `Eb+`, so every F4 save keeps the previous
version as `.BAK`. Test with a write that fails part way: the `.BAK` holds the original intact.

## Also, small

`vz -z` (resident mode) skips both releasing the lease and removing its temporary directory
(`dos_core.c:1043`, `:1052`), so each use leaves `/tmp/vXXXXXX` and open descriptors behind.
Release both on every exit path.

## Done means

All gates green, and each new test shown failing on the old code. Your summary lists the files
changed and names three weaknesses of your own work, each with a real `file:line`.
