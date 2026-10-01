# VZ WebAssembly stack verification

The browser smoke test first passed stages 1–18, then trapped immediately
after F4 loaded VZ, before its startup screen:

```text
FAIL [19 F4 VZ edit, save, and quit]: RuntimeError: memory access out of bounds
wasm-function[1027]:0x8efa27
wasm-function[151]:0x19d3d
wasm-function[164]:0x1cecc
wasm-function[73]:0x76f3
Object.doRewind
```

The last guest log entry was `loaded VZ.COM at 04B4 (linear 04B40, 55856
bytes)`. Repeating the same test reproduced the same instruction offsets.

## Distinguishing the two stacks

An isolated copy of the release JavaScript module changed only
`Asyncify.StackSize` from 4096 to 65536; its `vc.wasm` was byte-identical to
the failing release binary (`cmp` passed). This probe still passed stages
1–18 and failed at the exact same stage-19 instruction offsets. Increasing
Asyncify's save area did not fix the defect.

The next isolated build, before adding the production C-stack setting,
enabled assertions, checked C-stack updates, and retained function names:

```sh
EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
EM_CACHE=/home/eo/src/vc-linux-wt/vz/build/emcache \
make web WEB_OUT=build/web-vz-debug \
  WEB_OPT='-O2 -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2 --profiling-funcs' \
  EMCC=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc
node tests/web_smoke.mjs build/web-vz-debug/vc.mjs
```

It detected an overflow before the initial panels, rather than letting the
ordinary C stack silently overwrite adjacent linear memory:

```text
Aborted(stack overflow (Attempt to set SP to 0x0032d9b0,
with stack limits [0x00331da0 - 0x00341da0]).
If you require more stack space build with -sSTACK_SIZE=<bytes>)
```

Those bounds reserve 65,536 bytes, while that update needs 82,928 bytes.
An `onAbort` stack trace in the same profiled module identified
`dos_fs_int21` (function 95, offset `0x1254a`), called from optimized `main`
(function 74, offset `0xa685`) during the rewind. The rewind frame was where
the corruption surfaced, not evidence of an undersized Asyncify save area.

## Verified fix

The follow-up checked build uses `-sSTACK_SIZE=1048576` while retaining
Asyncify's original 4096-byte save area:

```sh
EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
EM_CACHE=/home/eo/src/vc-linux-wt/vz/build/emcache \
make web WEB_OUT=build/web-vz-debug/large-c-stack \
  WEB_OPT='-O2 -sASSERTIONS=2 -sSTACK_OVERFLOW_CHECK=2 --profiling-funcs -sSTACK_SIZE=1048576' \
  EMCC=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc
node tests/web_smoke.mjs build/web-vz-debug/large-c-stack/vc.mjs
```

All 21 stages passed with exit status 0. In particular:

```text
PASS 19: F4 opens README in VZ; Alt-S saves real MEMFS bytes and Alt-Q restores VC
PASS 20: typed vz NEW.TXT creates and saves a real H: file
PASS 21: F10, Enter quits and fires the exit hook
```

The generated module still declares `Asyncify.StackSize:4096`. This isolates
the necessary change to `-sSTACK_SIZE=1048576`; no runtime, translator,
guest program, or Asyncify setting changed to obtain the pass. The same
C-stack setting belongs in the production web build and its mutation builds.

Both diagnostic builds and the JavaScript-only probe live below
`build/web-vz-debug`; none modified the production output or translated
program bytes. The compiler was Emscripten's bundled toolchain and the smoke
runner was its bundled Node 20.18.0 at
`/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin/node`.

## Asyncify scope and compiled-code growth

The checked, profiled 1 MiB-stack module was disassembled without rebuilding:

```sh
/home/eo/src/vc-linux-wt/emsdk/upstream/bin/wasm-dis \
  build/web-vz-debug/large-c-stack/vc.wasm \
  -o build/web-vz-debug/large-c-stack/vc.wat
```

In this artifact, exported `asyncify_start_unwind` and
`asyncify_start_rewind` identify `$global$6` as the state and `$global$7`
as the saved-stack data pointer. The genuine instrumented `term_idle`
function contains 12 references to that data pointer and explicit
rewind-time local restores. This is the positive control for the scan.

VZ's dispatcher is `run_979`: its 23,159-entry binary search identifies it
against the generated VZ source. It still contains one `call_indirect`.
All 91 VZ chunks, `chunk_0_980` through `chunk_90_1078`, have zero saved-stack
data-pointer references and zero Asyncify state writes. The dispatcher
also has none. The direct-call graph rooted at the dispatcher and all its
chunks reaches 169 functions/imports, none with Asyncify serialization and
none reaching `emscripten_sleep`. The compiled `rt_yield` calls only
`rt_update_clock`, without any saved-stack access.
An additional whole-module scan found 813 emitted `chunk_*` functions
across all translated images and zero saved-stack data-pointer references
in any of them, so the older programs were not inadvertently rewritten
either.

`ASSERTIONS=2` adds state-invariance checks around otherwise uninstrumented
calls: snapshot the state, make the call, and trap if the state changed.
Those lightweight debug checks are not unwind/rewind transformation of the
translated execution path. `ASYNCIFY_IGNORE_INDIRECT=1` remains enabled.

The binary code section independently accounts for 3,274,772 bytes in the
91 new VZ chunk bodies, out of 9,571,590 function-body bytes in this
11,195,910-byte diagnostic module. Thus the large size step has a concrete
new-program contribution; it is not evidence that Asyncify rewrote VZ's
translated instruction functions. The approximate 10% warning in
`CLAUDE.md` is useful for a fixed translated-image set, not a comparison
that adds another complete program.
