# Brief 12: review the GW-BASIC change

Read-only review. Do not edit anything. The change is the uncommitted diff in this worktree against
`main` (`git diff main` plus the untracked files in `git status`). Its brief is
`docs/briefs/11-gwbasic.md`; its own report is `docs/verification/11-gwbasic.md`.

Report only findings that would matter to a user or break something: a crash, wrong behaviour,
a security hole, or data loss. Skip style. For each one give severity (blocker or major),
`file:line`, the concrete input or state that triggers it, and what goes wrong. Check at least:

1. Native VC regressions. Timer interrupts are now delivered at dispatch boundaries, and EXEC now
   reads the program file. Can either change what VC itself does on Linux?
2. The COMMAND.COM-style lookup in `run_dos_command`. Can any file name, VC.EXT line or typed
   command reach `/bin/sh` as text? Can a deleted or replaced GWBASIC.EXE open a hole?
3. The multi-module translator path. Can it place an instruction at the wrong linked address, or
   silently skip code that runs?
4. Memory safety in new C: buffer sizes, path lengths, file reads of untrusted size.
5. The browser speaker and timer hooks: can they hang the page or leak audio nodes?

End with the list, most severe first. If you find nothing in an area, say "nothing found" for it.
