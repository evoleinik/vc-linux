# Brief 16: aggregate verification

Date: 2026-10-01. All local build, test and play gates below pass.
The separate requirement for an immutable OpenWatcom CI download remains
blocked by missing offline archive provenance; this is not an all-requirements-
complete claim. No commit was made.

## Implemented result

The original vendored Rogue 5.4.4 and DOS PDCurses build with OpenWatcom's
8086 large model. All Unix/DOS and integer-width adaptations live in our
own shims or exact-count-checked build-copy substitutions. Neither vendor
tree was edited. The game is a real byte-matched DOS executable, not a native
Rogue replacement or a command-name shortcut. Native installation puts it
beside GW-BASIC; browser installation puts it in `H:\GAMES`, on DOS `PATH`.

The final executable is 202,816 bytes, SHA-256:

```text
8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee
```

Its generated C contains 53,400 instructions in 835 compiled-only chunks,
with 4,049 MZ relocations. Two fresh final-width Make builds and the canonical
executable compare byte-identical. See [the build record](16-rogue-build.md)
for commands, port-width tests, reproduction checks and toolchain provenance.

## Why the map-driven translation path

An actual WDIS → JWasm → JWlink roundtrip test loses opcode direction bits:
Watcom's `89 c2` (`MOV DX,AX`) becomes `8b d0`, and `29 d0` (`SUB AX,DX`)
becomes `2b c2`. These are unrelocated bytes, so linker/header adjustments
cannot make the result identical. The test also extracts selected CRT members
to work around JWlink's inability to read the newer library dictionary format.

The chosen frontend uses the verbose link map and original OMF objects,
including every selected PDCurses/CRT library member. It independently checks
initialized bytes, resolved fixups, the complete MZ relocation table and entry
point, and original OMF evidence for data inside code. WDIS boundaries are
checked against those bytes and Capstone. Callback/switch destinations and
all 256 computed `_DoINTR_` entries must exist before translation succeeds.
No runtime instruction decoder was added. Details and executable-roundtrip
evidence are in [the translator record](16-rogue-translator.md).

## Final aggregate commands

The local supplied toolchain is selected with:

```sh
export WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom
make rogue gen
make -j4 test
```

The browser build uses the existing Emscripten installation:

```sh
env WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom \
  EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
  EM_CACHE=/home/eo/src/vc-linux-wt/rogue/build/emcache \
  PATH=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten:/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin:/usr/local/bin:/usr/bin:/bin \
  make web test-web
```

`make -j4 test` exited 0 against the final 202,816-byte executable:

| Suite | Final result |
|---|---:|
| Translator, including all distinct Rogue/CRT instructions in Unicorn | 261 passed, 416.93s |
| Native end-to-end, installation and play collections | 149 passed, 220.59s |
| Actual linked-DOS port-width/save tests | 42 passed |
| INI tests | 3 passed |
| DOS filesystem | 4,317 checks, 0 failures |
| DOS EXEC | 1,332 checks passed |
| Machine / process | 23 / 9 cases, 0 failures |
| Terminal | 38 groups, 5,822 checks, 0 failures |
| CGA | 823,877 checks, 0 failures |

The native Rogue collection includes typed startup in both command paths,
movement, `Q,y` restoring the panels, and `S,y` followed by plain `rogue`
restoring the exact game. It also verifies Enter, renamed/changed images,
option callbacks, and preservation of a rejected truncated save. The final
Unicorn run includes the restored coverage helper and both review regressions.

`make web test-web` exited 0: the asset-version, speaker, CGA graphics and
all 24 Node/MEMFS smoke stages pass. Rogue stages 19–23 cover the real file in
`H:\GAMES`, typed startup, movement, exact save/restore, `Q,y` panel return
and Enter on the EXE.
Their full local logs are `build/rogue-verification/make-test-final.txt` and
`build/rogue-verification/make-web-final.txt`; generation is recorded in
`build/rogue-verification/final-generation.txt`.

The earlier generated C was explicitly compared before and after the port:

```sh
for rogue_baseline in vc_com vc_ovl gwbasic bootlogo gwbasic_graphics; do
  cmp "build/rogue-baseline/$rogue_baseline.c" "build/gen/$rogue_baseline.c" || exit
done
```

All five comparisons exit 0. `tests/test_translator_regression.py` retains
their pre-Rogue SHA-256 values as an additional normal test gate.

## Deliberate defects and recovery

Each fault below was actually exercised and then removed. Native and browser
boundary faults used isolated wrappers around the real production runtime;
the smoke-test assertions were not changed to manufacture failures. The
production source changes and artifact mutations are distinguished explicitly.

| Gate | Planted defect and observed failure | Recovery/evidence |
|---|---|---|
| EXE reproducibility | Append a time-dependent footer to each real fresh link; `cmp` exits 1 | Remove footer, rebuild, `cmp` exits 0; build record |
| OMF byte verification | Disable the production initialized-byte comparison; both corruption tests fail to raise | Restore comparison; focused suite green; translator record |
| Indirect targets | Remove the translated DoINTR INT 16h entry; layout construction fails | Restore enumeration; target gate green; translator record |
| Every Rogue instruction in Unicorn | Omit Rogue from the production case list; coverage reports 53,237 missing instructions | Restore helper; final 261-test translator collection passes, including complete coverage |
| Earlier generated C | Change only the generated bootLogo header comment; `cmp` fails at byte 30 and its hash test fails | Restore exact comment; all five `cmp`s and all five hash tests pass |
| Native startup | Hide Rogue from the EXEC registry in an isolated runtime binary; dungeon gate times out | Same test on production binary passes; native record |
| Native movement | Drop vi movement keys only inside Rogue; movement gate times out | Production passes, including a repeat after prompt/trap-test hardening |
| Native quit | Drop `y` only at the real quit prompt; panel-return gate times out | Production passes; native record |
| Native save/restore | Make DOS `access(rogue.sav)` report missing after a real save; exact-state gate fails | Production restores the same game; native record |
| EXEC full-byte identity | Force equality only for comparisons against Rogue's embedded image in an isolated runtime unit binary; a changed MZ byte is wrongly accepted and the test fails | The unchanged production binary passes all 1,332 checks; `exec-identity-red-green.txt` |
| Browser package | Remove `GAMES/ROGUE.EXE` from the production manifest; exact-package test fails | Restore manifest; package test passes |
| Browser startup | Flip the last byte of the actual MEMFS Rogue EXE before launch | Stage 19 fails with no matching translation; removing fault passes all 24 stages |
| Browser movement | Drop movement input only when the real Rogue status is visible | Stage 20 times out waiting for changed `@`; removing fault passes all 24 stages |
| Browser save/restore | Hide the actual saved file just before the second `rogue` command | Stage 21 fails the exact-restored-game wait; removing fault passes all 24 stages |
| Browser quit | Drop `y` only at the real quit prompt | Stage 22 times out waiting for panels; removing fault passes all 24 stages |

The browser wrapper lives in `build/rogue-verification/web-defect-loader.mjs`.
For each of `startup`, `movement`, `restore`, and `quit`, the red command is:

```sh
WEB_SMOKE_TIMEOUT=5000 ROGUE_WEB_DEFECT=startup \
  node tests/web_smoke.mjs build/rogue-verification/web-defect-loader.mjs
```

Recovery is the same command with `ROGUE_WEB_DEFECT` absent. Each red run
exited 1 at the indicated stage; each recovery exited 0. Logs are
`build/rogue-verification/web-{startup,movement,restore,quit}-{red,restored}.txt`.
Legacy-C fault logs are `legacy-c-red.txt` and `legacy-c-restored.txt` in the
same directory. The [native record](16-rogue-e2e.md) gives its fault commands
and results; the [build record](16-rogue-build.md) additionally records real
RNG/save-width/close-error/stale-source faults and 13 pre-fix gold/screen-cell
regressions. A truncated real save and a pre-movement `--More--` sequence also
produced red regressions before their fixes. None of these faults remains in
production files.

## Wasm size

The pre-Rogue browser build was preserved in `build/rogue-baseline/web/`.
Size comparisons use `gzip -n -9 -c vc.wasm`, excluding the gzip filename and
timestamp. The final rebuilt Wasm has passed its complete smoke test:

| Artifact | Before Rogue | With Rogue | Growth |
|---|---:|---:|---:|
| `vc.wasm`, raw | 7,358,052 | 15,775,230 | 8,417,178 bytes |
| `vc.wasm`, gzip `-n -9` | 1,562,747 | 2,925,020 | 1,362,273 bytes (+87.2%) |

Final Wasm SHA-256:

```text
a2ae1906b1b859872582f5d4cdea050f4458193960ab98374cf0fe0d636a38e9
```

The demo payload now contains 29 files totaling 494,511 bytes. Its former
400,000-byte budget could not hold the added real EXE and licenses; the budget
is now explicitly 600,000 bytes. No earlier games or demo assets were removed.

## Changed files

These are task changes, excluding ignored build outputs and the pre-existing
untracked brief/vendor inputs:

```text
Makefile
README.md
runtime/dos_core.c
runtime/image.h
runtime/main.c
runtime/rt.c
runtime/rogue_dos/README.md
runtime/rogue_dos/acs437.h
runtime/rogue_dos/config.h
runtime/rogue_dos/mdport.c
runtime/rogue_dos/save_io.c
runtime/rogue_dos/startup.c
runtime/rogue_dos/state_long.h
tools/build_rogue.py
tools/rogue.mk
tools/rogue_port.py
tools/web_demo.py
translator/README.md
translator/__main__.py
translator/compiled.py
translator/omf.py
tests/test_dos_exec.c
tests/test_install_e2e.py
tests/test_machine.c
tests/test_rogue_build.py
tests/test_rogue_e2e.py
tests/test_rt_process.c
tests/test_translator_compiled.py
tests/test_translator_ops.py
tests/test_translator_regression.py
tests/translator_support/ops_build.py
tests/translator_support/ops_harness.c
tests/translator_support/ops_notes.md
tests/web_smoke.mjs
web/README.TXT
docs/verification/16-rogue.md
docs/verification/16-rogue-build.md
docs/verification/16-rogue-e2e.md
docs/verification/16-rogue-translator.md
```

## Three weaknesses

1. `translator/compiled.py:371`: computed-return recognition uses one exact
   Watcom DoINTR template. A changed compiler runtime needs another reviewed
   proof and currently fails translation instead of being handled generically.
2. `translator/omf.py:153`: the parser deliberately accepts only supported
   16-bit OMF forms; absolute/32-bit segments and other unsupported records
   require further work before arbitrary compiled-C programs can use it.
3. `runtime/rogue_dos/save_io.c:18`: a failed save preserves the live game, but
   cannot recover an older file that upstream already opened for overwrite.
   Save replacement is not atomic.

## Remaining CI prerequisite

The supplied installed toolchain came from moving `Current-build`; the original
150,304,640-byte `ow-snapshot.tar.xz` was deleted after extraction without a
recorded digest or immutable release identifier. Read-only searches of the
supplied installation, tool/source caches and relevant scratch/download caches
did not recover it. No network was used and no URL/checksum was fabricated.

Consequently `.github/workflows/ci.yml` has not been given the pinned download
required by the brief, and this task is not fully complete. An authentic pinned
archive, or its exact immutable URL and SHA-256, is needed to finish that CI
requirement. The build record contains the provenance evidence and search scope.
