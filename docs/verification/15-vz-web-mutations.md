# Browser VZ gate mutations

These checks use the real translated VZ program in Node/MEMFS, not an editor
substitute. The isolated runtime copies described in
`15-vz-native-mutations.md` are compiled to
`build/brief15-web-mutants/vc.mjs` and `vc.wasm`. Production runtime sources,
generated translations, vendored files and release artifacts are not modified.

The copied smoke runner, `build/brief15-web-mutant-smoke.mjs`, differs from
`tests/web_smoke.mjs` only by setting the test module's environment before
startup:

```js
module.ENV.VZ_MUTATION = process.env.VZ_MUTATION || '';
```

Each run starts a fresh module and MEMFS. Exactly one of the following defects
is enabled, and the same module is then run again with that selector unset.
The native-only filename-injection selector is not used in the browser.

| Selector | Deliberate defect | Independent browser gate |
| --- | --- | --- |
| `f4-disabled` | Refuse only the internal F4 editor launch. | README's first line appears in VZ, not VC panels. |
| `drop-save` | Acknowledge VZ's DOS writes without writing the bytes. | Read the exact edited README back from MEMFS. |
| `quit-exits-vc` | Stop the dispatcher when the VZ child exits, after ordinary cleanup. | Saved bytes survive and VC panels return. |
| `typed-disabled` | Reject typed `vz`/`VZ.COM` lookup while retaining F4. | Typed `vz NEW.TXT` creates and saves a real H: file. |

## Build and runner

The test-only compile and ordered probe scripts are under the ignored
`build/brief15-web-mutants/` directory:

```sh
sh build/brief15-web-mutants/compile.sh
sh build/brief15-web-mutants/run-probes.sh
```

The compile script uses the copied `rt`, `dos_core`, `main`, `cpu`, `dos_fs`,
`cp866`, `bios` and `term` sources, followed by the ordinary seven
`build/gen/` translation/embedded-file sources. It embeds
`build/web-work/demo@/home/vc`. Its compiler options match the release web
build, including the ordinary C-stack fix established in `15-vz-web.md`:

```text
-O2 -sSTACK_SIZE=1048576 -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1
-sMODULARIZE -sEXPORT_ES6 -sENVIRONMENT=web,node -sFORCE_FILESYSTEM
-sEXPORTED_RUNTIME_METHODS='["FS","ENV"]' -std=gnu11
-Ibuild/brief15-native-mutants/runtime -Iruntime
```

Emscripten is `/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc`, with
`EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten` and the writable
`EM_CACHE=/home/eo/src/vc-linux-wt/vz/build/emcache`. The runner uses the
bundled Node 20.18.0 and the normal `WEB_SMOKE_TIMEOUT=10000` milliseconds.
Asyncify's save area remains at its default 4096 bytes.

Every red run must exit 1 with the expected gate-specific timeout assertion,
after passing the preceding stages. A runtime trap, abort or outside-thread
watchdog stall is rejected as unrelated evidence. Every restored run must
exit 0 and print `web smoke: all 21 checks passed`. Separate
`SELECTOR-red.log` and `SELECTOR-green.log` files retain the complete output.

The smoke runner now flushes stdout and stderr in both its worker and main
thread before exiting. An initial restored run had passed stages 1–21 and
exited 0 but lost its final summary line when redirected; flushing preserves
the success marker and failure diagnostics without weakening any assertion.

## Results

The complete ordered sequence finished successfully on 2026-10-01. All four
red runs exited 1 at their intended assertions, and all four independently
restored runs exited 0 after all 21 stages. None failed through a guest trap,
abort or event-loop stall.

| Selector | Observed red result | Restored result |
| --- | --- | --- |
| `f4-disabled` | Stage 19: `Timed out waiting for VZ showing the original README first line after F4`. Stages 1–18 passed. | All 21 checks passed. |
| `drop-save` | Stage 19: `Timed out waiting for VZ has saved the actual README bytes to MEMFS`. VZ had accepted the text and Save As command, but exact readback failed. | All 21 checks passed. |
| `quit-exits-vc` | Stage 19: `Timed out waiting for VC panels return after VZ exits`. Exact saved-byte readback had already succeeded. | All 21 checks passed. |
| `typed-disabled` | Stage 20: `Timed out waiting for VZ asks to create the typed new filename`. Stage 19's complete F4 edit/save/quit had passed with the defect still enabled. | All 21 checks passed. |

The final row is also the independent control showing that the typed-command
gate is not just another F4 test. Disabling each defect changes only its
environment selector; no sources, translated code, embedded DOS bytes or
test assertions change between that red run and its restored green run.
