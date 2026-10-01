# Brief 15: VZ Editor verification

Verified on 2026-10-01, Linux x86-64 and Emscripten/Node 20.18.0. No commit
was made. The pre-existing untracked brief and `third_party/vzeditor/` were
inputs, not files edited by this implementation.

## Result

VZ Editor 1.6 US is built from all 29 original assembly modules, translated
ahead of time, and installed as a real `VZ.COM`. Its English definitions
are installed beside it. F4 uses in-process DOS EXEC in the browser and
when Linux's `EDITOR` is unset or empty. A nonempty Linux `EDITOR` retains
the existing quoted, resolved-host-path behavior. Typed `vz NEW.TXT` also
uses DOS EXEC. Recognition checks complete executable bytes, not names.

`build/vz/VZ.COM` is exactly the shipped US binary: **55,856 bytes**, SHA-256
`198cd8ace6e1a945dacdd6ac7e272b0f8131b688d762df4d90738d2796d1eeb2`.
There are no differing instruction, settings, or padding bytes to explain.
Preprocessing and assembly happen in the build tree; vendor sources stay
unchanged. [Build details](15-vz-build.md) explain the dialect fixes and
why installed settings need no binary patch.

F4's canonical short name is leased to the selected file for the child
process's lifetime. VZ's executable and working-directory ancestors are
also leased, so neighboring directory changes cannot redirect a new-file
save or a definition-file lookup. Replaced files and conflicting native
short names fail closed. VZ gets a private, short, mode-0700 temporary
directory; its known swap files are removed on normal or forced child exit.
The browser C stack is explicitly 1 MiB; Asyncify settings are unchanged.

## Gates

| Gate | Result |
| --- | --- |
| `make vz` | Exact `cmp` against shipped `VZUS.COM`; all 29 modules. |
| `make test` | Translator 248 tests; filesystem 4,522 checks; EXEC 1,482 checks; machine 23 cases; process 9 cases; terminal 5,822 checks; CGA 823,877 checks; INI 3 tests; PTY/install 155 tests; VZ build 14 tests. |
| VZ PTY subset | 29 tests: F4 first line, edit/save/quit/reopen, typed creation, English menus/help, injection, renamed/changed executables, both command routes, unset/empty `EDITOR`, path and alias stress cases. |
| Existing host-editor subset | Six editor, injection, and command-length tests remain green. |
| Earlier generated C | Five independent `cmp` commands pass; hashes are in the [translator record](15-vz-translator.md). |
| `make web` | Release build succeeds; H: demo is 33 files / 342,205 bytes, below 400,000. |
| `make test-web` | Asset hashes, speaker, graphics, and all 21 real-WASM smoke stages pass. Stages 19–20 edit/save/read back README and create a new file in MEMFS. |
| Checked browser build | Assertions and C-stack checking enabled: all 21 stages pass. |
| Filesystem sanitizers | ASan/UBSan pass all 4,522 checks with `ASAN_OPTIONS=detect_leaks=0`; the sandbox's LeakSanitizer/ptrace incompatibility is recorded separately. |

The combined instruction oracle contains **87,012 distinct byte/offset
cases**, each checked against Unicorn from 32 randomized states. VZ adds
23,111 distinct cases, including both source-proved forms of its two-byte
macro interrupt slot. DAS also receives a 1,024-state focused check.

Release commands used the on-disk SDK, with no network access:

```sh
make vz
make test
EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
EM_CACHE=/home/eo/src/vc-linux-wt/vz/build/emcache \
make web test-web \
  EMCC=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc \
  NODE=/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin/node
```

Final aggregate output is retained in `build/brief15-final-test.log`,
`build/brief15-final-web-build.log`, and `build/brief15-final-test-web.log`.
The generated-C comparisons are:

```sh
cmp build/brief15-baseline/gen/vc_com.c build/gen/vc_com.c
cmp build/brief15-baseline/gen/vc_ovl.c build/gen/vc_ovl.c
cmp build/brief15-baseline/gen/gwbasic.c build/gen/gwbasic.c
cmp build/brief15-baseline/gen/bootlogo.c build/gen/bootlogo.c
cmp build/brief15-baseline/gen/gwbasic_graphics.c build/gen/gwbasic_graphics.c
```

## Planted defects and restoration

Each deliberate defect is isolated to a Python process or a test-only
runtime copy below `build/`; production source and generated translations
never contain a defect selector. Each failing run is followed by a clean
run with that defect removed. The copied native/browser programs execute
the real VZ translation, not a replacement editor.

| New gate | Defect and observed red | Restoration / record |
| --- | --- | --- |
| Exact US build | Disable sized-AX rewrite: mismatch at `0xd76`. | 14 build tests pass; [build record](15-vz-build.md). |
| Reproducible listings/map | Add changing build metadata: `VZ.MAP` comparison fails. | Repeated independent builds agree; [build record](15-vz-build.md). |
| Complete VZ instruction oracle | Omit VZ: coverage count is zero. | All starts covered; [translator record](15-vz-translator.md). |
| DAS semantics | Subtract 5 instead of 6: AX and parity disagree with Unicorn. | Focused oracle passes. |
| Mutable macro interrupt | Freeze its interrupt number: CS/IP disagree with Unicorn. | Live-byte oracle passes. |
| Macro opcode guard | Bypass the finite-variant check: zero word does not fault as required. | Guard regression passes. |
| Earlier generated C unchanged | Remove SBB's borrow from its carry comparison: `cmp` fails at byte 1767, line 52. | Regenerate in a clean process: `cmp` passes; command below. |
| Native F4 open | Disable F4 launch: selected first line never appears. | Same PTY test passes. |
| Native save | Report successful writes without writing: saved file is empty. | Exact edited host bytes match. |
| Native quit | Stop VC when VZ quits: panels never return. | Save/quit/reopen test passes. |
| Native typed creation | Disable only typed VZ: new-file prompt never appears. | Typed creation passes; F4 remains a separate passing control. |
| F4 injection | Put the selected filename in shell text: final `PWNED` assertion fails after a real edit/save/quit. | Original filename edited safely; no sentinel created. |
| Browser F4/save/quit/typed gates | Independently apply the same four functional defects in a copied WASM build. | Each intended assertion turns red, followed by all 21 stages green; [browser mutation record](15-vz-web-mutations.md). |

The five native defect/restore pairs and their exact assertions are in the
[native mutation record](15-vz-native-mutations.md). Additional red-first
path, private-temp, file/directory lease, and identity tests are recorded
in [path safety](15-vz-path-safety.md). The 64 KiB C-stack overflow and its
checked 1 MiB fix are recorded in [browser stack verification](15-vz-web.md).

The generated-C comparison probe writes only a disposable output:

```sh
.venv/bin/python - <<'PY'
import sys
import translator.emit as emitter
from translator.__main__ import main
original = emitter.emit_prelude
assert 'cpu.cf = (a < b + borrow);' in original()
emitter.emit_prelude = lambda: original().replace(
    'cpu.cf = (a < b + borrow);', 'cpu.cf = (a < b);')
sys.argv = ['translator', 'build/VC.COM', 'build/gen/VC.COM.lst',
            '--name', 'VC.COM', '--symbol', 'image_vc_com',
            '-o', 'build/brief15-cmp-mutant.c']
raise SystemExit(main())
PY
cmp build/brief15-baseline/gen/vc_com.c build/brief15-cmp-mutant.c
# red: byte 1767, line 52; exit 1
.venv/bin/python -m translator build/VC.COM build/gen/VC.COM.lst \
  --name VC.COM --symbol image_vc_com -o build/brief15-cmp-mutant.c
cmp build/brief15-baseline/gen/vc_com.c build/brief15-cmp-mutant.c
# restored: exit 0
```

## WASM size

The baseline was rebuilt from pre-change runtime, generated C, embedded
files and demo snapshots in `build/brief15-baseline/`, using the same SDK,
`-O2`, and existing Asyncify settings. Both gzip measurements use `-9n` to
exclude timestamps and filenames.

| Artifact | Before | After | Growth |
| --- | ---: | ---: | ---: |
| Raw `vc.wasm` | 7,358,052 bytes | 10,994,311 bytes | 3,636,259 bytes |
| `gzip -9n` | 1,562,747 bytes | **2,212,916 bytes** | **650,169 bytes / 41.6%** |

The final compressed artifact is 2.1104 MiB. The new translated editor and
its installed files account for the added functionality; the browser
continues to skip Asyncify rewriting of indirect translated-code calls.
The checked-module audit found no Asyncify save/restore serialization in
any of its 813 translated chunks; VZ's 91 new chunks alone account for
3,274,772 bytes of function bodies in that diagnostic build. See the
[compiled-code audit](15-vz-web.md).

Release WASM SHA-256:
`a20181623aede83f00ba322da8bcdb01a35e2d3254f83abb70465a9d36b8dd1a`.

```sh
gzip -9n -c build/brief15-baseline/web/vc.wasm | wc -c
gzip -9n -c build/web/vc.wasm | wc -c
```

## Keys and retained DOS behavior

- Save: **Alt-S**, or **Esc then S**, then **Enter** to accept the filename.
- Quit: **Alt-Q**, or **Esc then Q**, then **Y**. Unsaved changes prompt first.
- **F1** opens the English file menu; **F12** opens English help.

The unmodified settings add DOS CRLF for newly inserted lines and a final
Ctrl-Z byte on save. `Dp+` lowercases a newly created host filename, so
typing `NEW.TXT` creates `new.txt`; DOS lookup remains case-insensitive.

## Three weaknesses

1. F4 refuses canonical paths of 64 bytes or more rather than supporting
   arbitrary Linux path lengths: `runtime/dos_core.c:778`.
2. Path leases check inode and directory-entry identity, not concurrent
   content changes to the same inode. Another editor's in-place write can
   still be overwritten by a later VZ save: `runtime/dos_fs.c:364`.
3. The edit/save PTY fixtures are small ASCII documents. They do not prove
   large-file disk swapping or the whole macro language:
   `tests/test_vz_e2e.py:18`.

## Files changed

```text
Makefile
README.md
runtime/dos_core.c
runtime/dos_fs.c
runtime/dos_fs.h
runtime/image.h
runtime/main.c
runtime/rt.c
tests/test_dos_exec.c
tests/test_dos_fs.c
tests/test_install_e2e.py
tests/test_machine.c
tests/test_rt_process.c
tests/test_translator_ops.py
tests/test_translator_vz.py
tests/test_vz_build.py
tests/test_vz_e2e.py
tests/translator_support/ops_build.py
tests/translator_support/ops_harness.c
tests/web_smoke.mjs
tools/build_vz.py
tools/vz.mk
tools/web_demo.py
translator/README.md
translator/__main__.py
translator/emit.py
translator/layout.py
translator/linked.py
translator/listing.py
translator/vz.py
web/README.TXT
docs/verification/15-vz-build.md
docs/verification/15-vz-editor.md
docs/verification/15-vz-native-mutations.md
docs/verification/15-vz-path-safety.md
docs/verification/15-vz-translator.md
docs/verification/15-vz-web.md
docs/verification/15-vz-web-mutations.md
```

Generated images, C, binaries, logs, snapshots, and deliberately broken
test-only binaries remain below ignored `build/`. Do not ship the mutant
binaries; `build/vc` and `build/web/` are the production outputs.
