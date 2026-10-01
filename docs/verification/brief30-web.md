# Brief 30: installation and browser gates

All commands here are local: no browser, listener or network access. Emscripten
is `/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc` (4.0.2), using
`EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten` and the writable
`EM_CACHE=$PWD/build/emcache`. Python is `.venv/bin/python`; test homes,
config directories and logs are temporary files under `/tmp`.

## Exact DOS files and installation layout

`tests/test_install_e2e.py::test_web_demo_installs_source_built_dos_shell_and_utilities`
compares all seven packaged files with `build/msdos2`, requires COMMAND.COM at
H:'s root and the utilities in H:\DOS, compares the guide and MIT licence,
and checks that the guide identifies the programs, provenance and three examples.

The web-demo group passed: `7 passed, 32 deselected`. The isolated native
MS-DOS installation gate passed: `1 passed, 38 deselected`, checking exact
config-directory bytes and no HOME writes, including no browser-only SRC/GAMES
directories. The embedded-file round-trip gate includes all seven DOS files,
DOS.TXT/DOSLIC.TXT and the browser-only source/game files;
`pytest -q tests/test_embed.py` passed `64 passed in 6.00s`, exercising native
raw bytes, browser LZMA decompression, the reversible 16-bit operand filter,
short/trailing input, 64 KiB wrap, arbitrary data and exact memory guards.

## Planted packaging defect

Temporarily added `if name == "COMMAND.COM": continue` to the real loop in
`tools/web_demo.py`, leaving the test and the generated source-built binary intact.

```
.venv/bin/python -m pytest -q \
  tests/test_install_e2e.py::test_web_demo_installs_source_built_dos_shell_and_utilities
```

Actual red result:

```
tests/test_install_e2e.py:219:
    assert target.read_bytes() == (ROOT / "build/msdos2" / name).read_bytes()
E   FileNotFoundError: .../test_web_demo_installs_source_0/COMMAND.COM
FAILED ...::test_web_demo_installs_source_built_dos_shell_and_utilities
1 failed in 0.10s
```

Removed only that planted defect and reran the same command: `1 passed in 0.06s`.

For the native installation gate, compiled a separate `build/brief30-install/vc`
using a copy of the production `main.c` whose installation loop skipped
`msdos_program(f->name)`. All generated objects and other runtime files were
the real production inputs; the normal binary remained untouched.
With `VC_TEST_BINARY` pointing at that executable, the unchanged
`test_install_msdos_programs_in_config_without_home_writes` failed at
`tests/test_install_e2e.py:61`: `FileNotFoundError` for config/vc-linux/COMMAND.COM
(`1 failed in 0.21s`). Removed the copied defect, checked it byte-for-byte
against `runtime/main.c`, and reran the exact gate with the normal binary:
`1 passed in 0.18s`. Finally replaced the isolated control executable with
the finished production binary and reran with the same `VC_TEST_BINARY` path:
`1 passed in 0.18s`; `cmp` proved the two binaries equal.

## Planted packing defects

Changed the production inverse in `runtime/embed_lzma.c` from
`absolute - (uint16_t)(at + 3)` to `absolute + (uint16_t)(at + 3)`. The unchanged
`test_x86_16_filter_fixed_operands_and_incomplete_tail` failed at byte 1:
`0x06 != 0x00` (`1 failed in 0.12s`). Restoring subtraction made that exact
gate pass (`1 passed in 0.12s`). This is a lossless byte filter, not an x86
decoder: bytes resembling opcodes in data are deliberately tested too.

Then forced `web_only = False` in `tools/embed.py` after validating each
argument. The unchanged
`test_web_only_startup_files_stay_out_of_native_table_and_binary` reported
`2 failed, 2 passed in 0.44s`: the empty native file leaked an `f3` symbol;
the nonempty native file added unexpected source bytes to the output.
Removed the defect and the complete 64-test embedding group passed as above.

## Startup budget and planted eager download

The first complete seven-program build exceeded the unchanged budget:
`1,313,027` gzip bytes against `1,300,000`. Moving already-present SRC/BASIC
files into the existing startup packed table and adding reversible 16-bit
operand normalization brought the normal `-O2` production build to
`1,295,840` gzip bytes, including `1,137,254` for the main wasm. All startup
files remain available immediately with exact original bytes; nothing new
is deferred. No compiler optimization or cap change was needed.

Planted a real eager COMMAND wasm `<link rel="preload">` in the built page,
leaving both gates unchanged. `node tests/web_size.mjs build/web` failed:
`first load 1428409 exceeds 1.3 MB`; COMMAND itself added `132,548` gzip bytes.
`node tests/web_assets.mjs build/web` also rejected the eagerly referenced
side module. Removed only that link and both gates passed again:
`1,295,840` gzip bytes and all twelve immutable side names matched the main.

## Browser regression gates

The unchanged transport-failure, timeout and memory-limit smoke paths passed
against the twelve-module build: rejected/503/404/malformed downloads; response
and body stalls with default/capped 30-second deadlines and late bytes;
64 MiB exhausted/fragmented memory refusals, working F3, successful retries
and subsequent caching. Module inspection passed the explicit shared ABI,
no eager dependencies, bounded static memory and all twelve non-Asyncified sides.

Source-map/text/runtime-UI, exhaustive layout, keypad, CP866 language, speaker,
CGA and modem JavaScript gates also passed. No page listener or browser was
started; the smoke harness supplies asynchronous local module transport.

## Planted cache defect

Compiled an isolated copy of the production `runtime/web_programs.c` with
the sole change `if (entry->image && index != WEB_COMMAND)` in `web_load_image`.
The actual Emscripten main used the normal production flags, unchanged generated
translations and unchanged smoke test; output lived in `build/brief30-cache`.
The original source and normal `build/web` were never changed by this control.

```
node tests/web_smoke.mjs build/brief30-cache/vc.mjs --msdos-only
```

The first lazy `DIR` passed. The second COMMAND use (the two-line B30.BAT)
attempted another download, which the strict expected-fetch transport rejected.
The log reported `exec H:\COMMAND.COM ... "/C B30"` followed by
`DOS command error 5`; the unchanged batch gate failed waiting for a new
COMMAND load and restored panels. Restored the copied predicate and proved
`cmp runtime/web_programs.c build/brief30-cache/web_programs.c` equal.
The restored full DOS smoke and canonical `make test-web` both passed with
the production loader, including batch execution and every cached rerun.
Finally republished the production modules and copied the final main back
over the isolated control output. The exact command above passed in that
same directory; `cmp` proved its main wasm equal to the production main.

## Real defects caught while exercising the new gates

The browser `DIR` gate caught the assembler's stale mutable `ret_l` alias:
COMMAND spun on `74FE` instead of taking the source's backward `74F8` return.
That tool-side correction is documented with independent displacement tests
in the build report; the restored real `DIR` and batch executions passed.

The redirected SORT gate also caught a linker-metadata issue rather than
an instruction failure: source SORT requests a separate 64 KiB allocation
without first shrinking its program block, but JWlink's default MZ maximum
allocation was `FFFFh` (the shipped SORT uses `0001h`). COMMAND correctly
consumed available DOS memory according to that header, and SORT exited 1
with `SORT: Insufficient memory` and empty output. The guarded build-side
header finalizer now sets only that maximum to `0001h`, retaining raw SORT.MZ
and all code/data bytes. The unchanged redirected SORT smoke passed after
rebuilding, with no runtime allocator exception.

## Complete restored browser run

`make web` and `make -j2 test-web` completed successfully with Emscripten 4.0.2
and its bundled Node 20.18.0. The complete web dependency and gate chain was
rebuilt and rerun after the final JFT allocation/growth and BREAK-state review
fixes, again with exit status 0. The additional DOS-only smoke also completed:
real `DIR`, two batch lines, COMSPEC/PATH/EXIT, EDLIN exact saved bytes, DEBUG's
prompt/Q, FIND, MORE, FC's exact differing-byte offset and SORT's ordered
output. Each of the seven new modules was fetched on first use and reused
on subsequent real executions. All five earlier programs still ran too.

The twelve-module full smoke measured a `42,986,320`-byte allocator peak in
the unchanged `67,108,864`-byte initial heap. Its conservative any-order bound
was `52,058,030`, leaving `15,050,834` bytes, above the existing 4 MiB / 20%
headroom gate. Failure/timeout/fragmentation recovery paths remained green.
All eight earlier generated C files again compared byte-identically with
the pre-task baseline after regeneration.

With corrected SORT metadata and the final reviewed runtime, first-load HTTP
payloads totaled `1,296,214` gzip bytes, including `1,137,627` for main wasm,
against the unchanged `1,300,000` cap (`3,786` bytes spare). The page build
hash was `90941bc96588`, immutable side-module generation `b6ac69eee503`.
