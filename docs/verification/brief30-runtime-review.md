# Brief 30: rejected-child BREAK restoration

An independent read-only review found that the DOS-hosted loader's early
rejection path skipped one of the original EXEC routine's cleanup actions.
`third_party/msdos2/source/EXEC.ASM:140-145` reads the global BREAK setting
with AH=33h and disables it while loading. Both successful entry (line 926)
and ordinary errors (line 1003) call `restore_ctrlc`. The host-side rejection
at AH=55h instead restores the caller's frame directly, so BREAK ON was lost.

The regression extends the existing `test_dos_hosted_loader` without changing
its source-image or saved-register tests. It covers initial BREAK OFF and ON,
each with successful adoption, corrupted loaded bytes and a changed file.
The successful child deliberately changes BREAK to the opposite value; that
legitimate global change must survive both child and shell termination.

Before the fix, the independently compiled unit failed:

```sh
make B=build/brief30-break-unit build/brief30-break-unit/test_dos_exec
build/brief30-break-unit/test_dos_exec dos-loader
```

```text
FAIL tests/test_dos_exec.c:439: dos_core_int21() && cpu.d.l == expected_break
```

The line number is from that actual pre-fix run; subsequent independent JFT
test additions moved this function down. The failure was the initial-ON,
rejected-child case: the observed setting was still OFF.

The fix adds per-COMMAND bookkeeping for its AH=33h query followed by a
temporary disable. A later explicit global write invalidates old snapshots.
Only a rejected adoption restores the captured setting; successful execution
continues to let Microsoft's own routine perform its normal cleanup, and
ordinary BREAK commands and child changes remain global. Reused process slots
start with no pending snapshot. No source program or generated C changed.

Recompiling with the same isolated output directory and running the same
selector after the fix produced:

```text
DOS EXEC: 209 checks passed
```

Only the C unit executable was rebuilt. No native application, web module,
translator output or main Makefile was changed by this focused fix.

## Explicit late-review mutation controls

On 2026-10-02, five deliberately planted defects independently reproduced the
late-review regressions. All seven negative-control invocations exited 1. The
restored, unmodified runtime then passed the complete DOS EXEC unit suite:
**4321 checks** (exit 0).

These runs used only copies of `runtime/dos_core.c` and `runtime/dos_fs.c` in
`/tmp/brief30-runtime-mutants.q6F4D8`. The production test source was unchanged.
Each defect was applied to a fresh pair of copies with `apply_patch`, compiled
to its own executable, and exercised before restoring both copies from the
production files. Both `cmp` commands returned 0 after every mutation and again
after the final clean run. No production runtime source, application binary,
web module, generated file or Makefile was changed by this experiment.

The first isolated compile was:

```sh
cp runtime/dos_core.c runtime/dos_fs.c /tmp/brief30-runtime-mutants.q6F4D8
cc -O1 -g -Wall -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable -std=gnu11 -Iruntime tests/test_dos_exec.c /tmp/brief30-runtime-mutants.q6F4D8/dos_core.c /tmp/brief30-runtime-mutants.q6F4D8/dos_fs.c runtime/cp866.c runtime/cpu.c -o /tmp/brief30-runtime-mutants.q6F4D8/open-precheck-mutant
```

The remaining builds used the same compiler command and source list, changing
only the output basename as listed below. Every compile exited 0 without
diagnostics.

| Isolated executable | Planted defect | Selector | Exit |
| --- | --- | --- | --- |
| `open-precheck-mutant` | Remove `open_file`'s `unused_jfn()` capacity check | `jft-full-create` | 1 |
| `open-precheck-mutant` | Same defect, extended create | `jft-full-extended` | 1 |
| `open-precheck-mutant` | Same defect, create-new | `jft-full-new` | 1 |
| `temporary-precheck-mutant` | Remove `create_temporary`'s `unused_jfn()` capacity check | `jft-full-temp` | 1 |
| `growth-mutant` | Insert an unconditional `return 0` at the start of `grow_job_file_table` | `jft-grow` | 1 |
| `inherit-count-mutant` | Omit AH=55h's `wr16(cpu.d.x, 0x32, 20)` child-count reset | `jft-grow` | 1 |
| `break-restore-mutant` | Omit `if (shell->exec_break_state == 2) break_flag = shell->exec_break` | `dos-loader` | 1 |

Captured test output (commands below abbreviate the common absolute temporary
directory, but were run from the repository root):

```text
$ open-precheck-mutant jft-full-create
FAIL tests/test_dos_exec.c:334: fread(bytes, 1, sizeof bytes, file) == 16
$ open-precheck-mutant jft-full-extended
FAIL tests/test_dos_exec.c:334: fread(bytes, 1, sizeof bytes, file) == 16
$ open-precheck-mutant jft-full-new
FAIL tests/test_dos_exec.c:329: stat("NEWFULL.TXT", &st) < 0 && errno == ENOENT
$ temporary-precheck-mutant jft-full-temp
FAIL tests/test_dos_exec.c:324: !strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")
$ growth-mutant jft-grow
FAIL tests/test_dos_exec.c:350: rd16(parent_psp, 0x32) >= 40
$ inherit-count-mutant jft-grow
FAIL tests/test_dos_exec.c:376: rd16(child, 0x32) == 20 && rd16(child, 0x36) == child
$ break-restore-mutant dos-loader
FAIL tests/test_dos_exec.c:453: dos_core_int21() && cpu.d.l == expected_break
```

The restoration commands after each mutation were:

```sh
cp runtime/dos_core.c runtime/dos_fs.c /tmp/brief30-runtime-mutants.q6F4D8
cmp runtime/dos_core.c /tmp/brief30-runtime-mutants.q6F4D8/dos_core.c
cmp runtime/dos_fs.c /tmp/brief30-runtime-mutants.q6F4D8/dos_fs.c
```

After the fifth restoration, the same compiler command built the restored
copies with output basename `clean`. Running it without a selector produced:

```text
$ /tmp/brief30-runtime-mutants.q6F4D8/clean
DOS EXEC: 4321 checks passed
```

Both final source comparisons also exited 0 with no output. This closes the
deliberate-defect requirement for the late full-JFT, JFT-growth, child-inheritance
and rejected-loader BREAK gates, in addition to the earlier observed pre-fix
failure above.
