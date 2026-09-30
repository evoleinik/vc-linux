# Brief 06: focused verification review

The review in `docs/briefs/04-review.md` reported 14 findings. They were fixed in these commits
on `main`: `e36389f` (core), `f61889f` (translator, file layer, terminal) and the commit after
it (tree scan and symlink cycles). Run `git log --stat -5` and read those diffs.

## Task

Verify, read-only, two things only:

1. **Is each of the 14 findings actually fixed?** For each one give FIXED, PARTLY or NOT FIXED,
   with the `file:line` that shows it and one sentence of reasoning.
   1. `$(...)` in a name run by F4
   2. rmdir through a directory symlink
   3. emoji names collapsing to one DOS name
   4. `cd` overflowing the scratch area
   5. the temporary environment block
   6. `_JCXZ` untranslated
   7. non-ASCII config path
   8. quoting of program paths
   9. waitpid and SIGCHLD
   10. non-terminal stdin or stdout
   11. signals leaving raw mode
   12. quoted `cd`
   13. kitty keypad codes
   14. 0Ch flush
2. **Did a fix introduce a new defect?** Look only at code these commits changed, and at what
   directly depends on it. Examples of what to check:
   - the exact-name fast path in `resolve_name` against the collision-suffix rules
   - `links_to_ancestor` on odd targets
   - the keyboard-poll throttle in `runtime/bios.c` and `hle_other_calls`
   - `vc-edit` parsing in `runtime/dos_core.c`
   - the new `load_image` signature
   - the translator closure

## Rules

- Read-only. No network. Do not start a general review, and do not report issues outside those
  two questions.
- A new defect needs `file:line`, a concrete triggering scenario and a severity (blocker / major
  / minor).
- End with the verdict list for 1, then any new defects for 2, then the finding you are least sure
  of.
