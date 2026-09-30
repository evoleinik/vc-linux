# Brief 08: final verification review

`docs/briefs/06-verify.md` found 2 partly fixed findings and 6 new defects. They were fixed in the
commits since `4d93c20`. Run `git log --stat 4d93c20..HEAD` and read those diffs.

## Task

Verify, read-only, two things only:

1. **Is each item fixed?** Give FIXED, PARTLY or NOT FIXED, with the `file:line` that shows it.
   1. Old unsafe VCEDIT.EXT and VC.EXT left in existing config dirs.
   2. Positional collision suffixes renumbering during a group delete.
   3. Ancestor symlinks listed as files but not deletable.
   4. INT 2Eh ("Quick execute commands") bypassing vc-edit.
   5. F4 passing a synthetic DOS name to the editor.
   6. F4 dropping trailing spaces. Names ending in spaces or dots now convert like
      unrepresentable ones.
   7. Long-name conversion listing directories it did not need.
2. **Did these fixes introduce a new defect?** Look only at the changed code and what directly
   depends on it: `hashed_name` and ambiguity handling, the trailing space/dot rule in
   `utf8_to_cp` and every caller of it, `run_dos_command`, `holds_retired_default`, and the
   symlink unlink path.

## Rules

- Read-only. No network. No general review. Report nothing outside these two questions.
- A new defect needs `file:line`, a concrete scenario and a severity (blocker / major / minor).
  Only a blocker or a major with a concrete path to data loss, code execution or a crash counts
  as open work.
- End with the verdicts for 1, the new defects for 2, then the item you are least sure of.
