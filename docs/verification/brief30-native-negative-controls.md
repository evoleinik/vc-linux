# Brief 30: native end-to-end negative controls

These are observed executions of the real pty gates in
`tests/test_msdos_e2e.py`, not mocked assertions or proposed experiments.
They used the final source-build objects, including SORT's corrected
one-paragraph maximum-allocation header.

## Isolation and planted defect

A private directory created with `mktemp -d` was
`/tmp/vc-msdos-native-negative-8TxMj2`. It contains seven separate copies of
`runtime/dos_core.c`, the pristine snapshot, diagnostic diffs, and pytest
logs. For each program, `apply_patch` changed exactly its own
`version_table` entry from `0x0002` to `0x0001`. Every other entry and all
other runtime behavior remained unchanged. No test ran against combined
defects, and the shared runtime, generated C, objects and `build/vc` were
never modified by this experiment.

The other nine native runtime translation units were compiled once into
private objects with the production Makefile flags. Each changed core was
then linked with those objects and the final existing `build/obj/` program
and embedded-file objects. `--strip-debug` reduced the size of the private
binaries without changing their code. The ordinary tests selected each
binary through the existing `VC_TEST_BINARY` environment variable.

For example, the COMMAND run was:

```sh
VC_TEST_BINARY=/tmp/vc-msdos-native-negative-8TxMj2/command/vc \
  .venv/bin/python -m pytest -q tests/test_msdos_e2e.py \
  -k 'command_dir_echo_type_copy_batch_exit or command_renamed' --tb=short
```

## Observed red results

All seven independent defects returned pytest exit status 1.

| Changed entry | Actual failing gate | Observed failure | Pytest result |
|---|---|---|---|
| COMMAND.COM | `test_command_dir_echo_type_copy_batch_exit`, both `comspec` and `int2e` | `ver` displayed `TeleVideo Personal Computer DOS Version  1.00`, so the required `2.00` output was absent. | 2 failed, 1 passed, 7 deselected; 29.97s |
| EDLIN.COM | `test_edlin_edits_saves_and_returns` | The editor's `*` prompt never appeared; the pty screen returned to VC instead. | 1 failed, 9 deselected; 8.49s |
| DEBUG.COM | `test_debug_prompt_quit_and_panel_redraw` | The debugger's `-` prompt never appeared; the pty screen returned to VC instead. | 1 failed, 9 deselected; 8.49s |
| FIND.EXE | FIND parameter of `test_command_original_utilities_and_redirection` | Output was `Incorrect DOS version`, not the required `x-line\r\n`. | 1 failed, 9 deselected; 0.89s |
| MORE.COM | MORE parameter of the same gate | Output contained `MORE: Incorrect DOS version`, not the original three lines. | 1 failed, 9 deselected; 0.89s |
| SORT.EXE | Both ordinary and `/r` SORT parameters of the same gate | Both outputs contained `SORT: Incorrect DOS version`, not their required ascending/descending lines. | 2 failed, 8 deselected; 1.74s |
| FC.EXE | FC parameter of the same gate | The expected `00000000` difference report was absent; output ended in `Incorrect DOS version`. | 1 failed, 9 deselected; 0.89s |

The COMMAND renamed/single-command case legitimately remained green: it
does not assert `VER`, and these commands still run with the deliberately
wrong reported version. It is included in the counts rather than reported
as a failing case. In total, nine selected test cases failed across the
seven experiments, and that one selected case passed.

## Restoration and final green

Immediately after each experiment, `apply_patch` restored the original
entry. `cmp` verified that copied source against the pristine snapshot,
and that program's isolated binary was relinked from its restored source.
All seven private binaries are therefore restored, not left as mutants.
The shared production source also still compared equal to the snapshot.
All nine checked source files had SHA-256:

```text
3c49dbbe64963d8774514e90abdb9db2f624e865bd956885b8c785b0a3f88d26
```

Finally, the complete unmodified test file was run against a restored
private binary:

```text
VC_TEST_BINARY=/tmp/vc-msdos-native-negative-8TxMj2/command/vc \
  .venv/bin/python -m pytest -q tests/test_msdos_e2e.py --tb=short
..........                                                               [100%]
10 passed in 15.37s
```

This final green covers both COMMAND entry paths, DIR/ECHO/TYPE/COPY,
two-line batch execution, EXIT and panel redraw, continued native
`/bin/sh` operation, renamed and `/C` shell launches, EDLIN file and backup
readback, FIND/MORE/both SORT directions/FC redirection, and DEBUG's prompt
and `q` return. All pty home/config/work directories were private `/tmp`
paths; no real home directory or loopback sockets were used.
