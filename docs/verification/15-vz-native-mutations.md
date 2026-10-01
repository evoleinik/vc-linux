# Native VZ PTY gate mutations

The mutation build lives only under `build/brief15-native-mutants/`. Its copied
runtime sources and its executable are **test-only** and must never be installed
or shipped. Production runtime files, translated objects, and vendored sources
are not edited. The generated VZ object is the real translated US program,
including its Unicorn-checked DAS instruction.

The copied runtime is instrumented with one `VZ_MUTATION` environment selector.
Exactly one selector is enabled per failing run, followed immediately by the
same test against the same binary with the selector unset. Fixed wrapper
scripts set the selector after VcSession creates its restricted child
environment. `VC_TEST_BINARY` chooses that wrapper without changing the tests.

| Gate | Selector | Deliberate defect |
| --- | --- | --- |
| F4 opens the first line | `f4-disabled` | Refuse the internal editor launch with DOS error 2. |
| VZ saves edited host bytes | `drop-save` | Return successful DOS file-write counts without writing VZ's bytes. |
| VZ returns to VC panels | `quit-exits-vc` | Stop the whole dispatcher when the VZ child terminates. |
| Typed `vz NEW.TXT` | `typed-disabled` | Reject only typed `vz`/`VZ.COM`; retain the F4 path. |
| F4 filename injection | `shell-filename` | Send the selected host filename to `/bin/sh` as text, then continue with VZ. |

The native-only injection probe uses only the existing test's private temporary
file `safe;touch PWNED;.txt`. It restores the selected DOS filename after
`host_run` overwrites the scratch buffer while resolving the current directory.
Thus the test still edits, saves, and quits VZ before checking that `PWNED` does
not exist. The other four selectors also compile under Emscripten for the
browser gate's independent checks.

The wrapper names match their selectors. `clean` unsets `VZ_MUTATION`, then
executes `vc-mutant`. From the repository root, the complete sequence is:

```sh
build/brief15-native-mutants/compile.sh
build/brief15-native-mutants/run-probes.sh
```

The compiler command uses the copied `rt`, `dos_core`, `main`, `cpu`, `dos_fs`,
`cp866`, `bios`, and `term` C files and the ordinary `build/obj/` image and
embedded-file objects. It never links a copied or simplified editor: the VZ
translation and bytes are identical to those in the production executable.

The script records separate `SELECTOR-red.xml` and `SELECTOR-green.xml` pytest
reports in its directory. It also runs the F4 open/quit test with
`typed-disabled` still enabled to demonstrate that this defect is specific to
typed lookup. Every ordinary native probe uses the `$EDITOR`-unset, COMSPEC
path, exactly as requested by the brief.

## Results

Both the initial and final sequences returned the expected status for all eleven runs:
each defective test exited 1 with exactly one assertion failure (no pytest
errors), and each restored test exited 0. The extra F4 control also exited 0.
The final repeat used fresh copies of the runtime including executable and
current-directory leases, with logs in `final-probes.log`. The table records
that final run, completed on 2026-10-01 at 18:59 Asia/Bangkok.

| Selector | Observed failing assertion | Red | Restored green |
| --- | --- | ---: | ---: |
| `f4-disabled` | VZ's first line never appeared; the log reported the injected DOS error 2. | 15.97 s | 1.47 s |
| `drop-save` | Exact saved bytes did not match; the host `NOTE.TXT` was empty while VZ displayed the inserted text. | 10.04 s | 3.85 s |
| `quit-exits-vc` | The save succeeded, but VC panels did not return after confirming VZ's Quit prompt. | 10.48 s | 3.85 s |
| `typed-disabled` | VZ's new-file prompt never appeared; the log reported the injected DOS error 2. | 10.40 s | 2.30 s |
| `shell-filename` | After editing, saving and returning to VC, `assert not (work / "PWNED").exists()` failed. | 2.60 s | 2.58 s |

With `typed-disabled` still selected, the independent F4 open/quit control
passed in 1.47 s. This demonstrates that the typed-command gate is not merely
another test of F4.

The terminate defect still runs ordinary child cleanup before stopping the
dispatcher, including releasing filename leases and removing VZ's private
temporary directory. The selected defect is only the loss of the VC parent.
Neither production runtime sources nor tests contain the mutation selector.
