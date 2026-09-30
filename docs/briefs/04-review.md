# Brief 04: whole-repo review

Read `README.md` and `docs/plans/2026-09-30-native-port.md` first.

## What exists

VC 4.99.09 now boots and works on Linux: panels, view, make directory, copy, rename, delete and
shell commands. All suites pass: `make test-translator test-fs test-term test-ini test-e2e`.

The parts:
- `translator/` turns VC.COM and VC.OVL into C.
- `runtime/cpu.c` is the CPU core.
- `runtime/rt.c` is the dispatcher, interrupt stubs, loaded-image registry, moved-code matching,
  clock and tracing.
- `runtime/dos_core.c` is the MCB chain, PSPs, EXEC and terminate, host command execution and
  internal `cd`.
- `runtime/dos_fs.c` is the file layer. `runtime/bios.c` and `runtime/term.c` are video,
  keyboard, mouse and the terminal.
- `runtime/main.c` is start-up.

## Task

Find real defects. Rank them by severity, and give each a concrete failure path. Look hardest at:

1. **Memory safety** in `runtime/`. Out-of-bounds access to `mem`, host buffers, the key queue,
   the moved-code table, snapshot copies, env and PSP building (`dos_core.c`), path buffers.
2. **DOS semantics VC depends on**:
   - MCB allocate, free and resize, including merging and the last-fit strategy
   - terminate: restoring the parent, vectors 22h to 24h, the DTA
   - EXEC: the parameter block and the command tail
   - the RETF 2 stub return with IF restored
   - INT 21h functions that report errors without CF
   Cite Ralf Brown's Interrupt List behaviour where the code differs.
3. **Dispatcher correctness**:
   - the byte check before running an image's code
   - content matching of moved code, including false matches
   - `rt_code_delta` and IP computation in `translator/emit.py`
   - images that overlap after memory is reused
4. **Host interaction**:
   - terminal restore on every exit path, including `rt_fault` and signals
   - child-process handling in `host_run`
   - `internal_cd` parsing
   - CP866 conversion edges
   - what happens when stdin or stdout is not a terminal
5. **Anything that makes VC behave differently from real DOS in a way a user would see.**

## Rules

- Read-only. Do not change files.
- No network.
- Report only defects you can point to in the code with `file:line`, and a scenario that
  triggers each. Style comments are not wanted.
- For each finding give severity (blocker / major / minor), the `file:line`, the failure scenario
  and a suggested fix in one or two sentences.
- End with the three findings you are least sure of, and why.
