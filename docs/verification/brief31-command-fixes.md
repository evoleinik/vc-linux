# Brief 31: verification summary

## Conflict resolution and safety

All seven conflict hunks in Makefile, runtime/dos_core.c, runtime/dos_fs.c and
tools/web_demo.py are resolved in file contents. Both the door build/test/demo
paths and the COMMAND build/runtime/browser paths are retained. No git-writing
command was run; the index intentionally remains unmerged for the owner to
finish the paused rebase.

New guest transfers use guest_mem.h, including overlapping PSP copies,
extended handle tables, hosted-loader image validation and cooked input with
segment-offset wrapping. The original door host-execution denial remains
before fork/exec. Six new real-door tests exercise COMMAND and a renamed copy
against a host shell, native ELF and modified utility, requiring a guest error,
no host marker, and a still-functional VC. The quota-aware redirected character
writer also prevents the branch from bypassing main's disk limit.

## Red and green by fix

All listed red results were observed before the corresponding implementation,
or against retained pre-fix artifacts/isolated old implementations. Detailed
commands, diagnostics and controls are linked per area.

| Fix | Red evidence | Green evidence |
| --- | --- | --- |
| Linux DOS2 installation and shell routing | All five host command names failed on both launch paths: 10 failures; five installation/migration failures. | 64 installation/utility tests pass. Exact /bin/sh stdout, stderr and status match; DOS2.COM has COMMAND's bytes, only the child PATH gains DOS2. |
| COMMAND caller-based AH55 adoption | Nested-loader test failed to register EDLIN when another program was on top. | 94 focused nested-loader checks; original and moved COMMAND copies, invalid return bytes, ordinary errors and BREAK restoration covered. |
| PSP ancestry and nested fault containment | Descendant PSP and nested-VC abort tests failed; old wasm DEBUG G stranded the runtime. | 35 descendant and 62 nested-VC checks, full process restoration tests, native/browser survivor checks. |
| AH55 inherited handle lifetime | Repeated copied PSPs exhausted handles after their owning program ended. | 1,064 focused lifetime checks; full EXEC suite passes. |
| DOS TAB output | AH40 emitted the CP437 TAB glyph and wrong cursor column. | All four DOS output paths expand to eight-column stops; BIOS teletype remains unchanged; native/browser TYPE and wide DIR pass. |
| Cooked CON and COPY CON | Handle read returned EOF; COMMAND's additional FCB device lookup returned file-not-found. | Typed multi-line input, echo, backspace, partial reads, Ctrl-Z and restart pass; real native/browser COPY CON retains CRLF lines. |
| NUL device | Open/read/device bits were wrong and redirection touched an ordinary host file. | EOF reads/discarded writes/device metadata; native/browser real-file sentinels remain unchanged. |
| Private per-drive pipe files | C: creation failed and/or root sentinels were overwritten/deleted; both wasm drives failed. | DIR through actual DOS SORT works on C: and H:, using private per-drive scratch; neither visible root sentinel changes. |
| FCB wildcard REN/DEL | Mutation calls returned AL=FF; padded-blank `?` names did not change. Explicit H: source plus default target drive exposed six additional failures. | DOS padded 8.3 substitutions, deletion, read-only protection and source-drive inheritance pass; native/browser wildcard checks pass. |
| Door redirected-output quota | An isolated pre-fix AH02/06/09 raw-write implementation exceeded the door file quota. | Quota regression and the original door/memory gates pass; native guest-copy audit retained checked bounds. |
| Reproducible Rogue build | Crossing UTC midnight changed byte 193388 and broke two exact-hash gates; new pre-compilation clock test also failed first. | 51 build tests pass, including unchanged canonical/historical hashes and guarded rejection of altered date notices. No vendor source was changed. |
| Rogue readiness regression | A real crowded starting room had no visible floor dots and timed out; four deterministic predicate controls failed. | All ten positive/negative controls and 29 existing Rogue tests pass. The check still requires status, one player, terrain and the requested level; game/runtime/seed are unchanged. |

Supporting records:

- [Linux installation and routing](brief31-linux.md).
- [EXEC, PSP lifetime and native nested tests](brief31-exec.md).
- [DOS console/filesystem tests and quota regression](brief31-io.md).
- [Door host-execution and guest-memory audit](brief31-door.md).
- [Date-independent Rogue build](brief31-build.md).
- [Browser regressions, raw startup samples and artifact hashes](brief31-web.md).

The nested success/error distinction is intentional. Kermit PUSH really
launches another COMMAND. The explicit VC child currently reports overlay
error/exit 2, and the shipped GW-BASIC reports Syntax error for unsupported
SHELL. DEBUG's unapproved COM is stopped with a guest diagnostic. Every case
returns through its caller and preserves the enclosing VC; this is the brief's
permitted guest-error outcome, not a claim that every nested feature works.

The first full native run also found two old Rogue pty assertions that relied
on broken CON reads returning EOF. They now acknowledge the game's genuine
final stdio prompt, while preserving the under-four-second qualifying-score
gate and checks that the read-only directory gains no score or lock file.

## Gate environment

The full unfiltered run first demonstrated both sandbox-blocked TCP gates:
`test_modem_transport` fails creating its required loopback socket with
`Operation not permitted`, and `test_kermit_dials_bbs_tcp` raises socket
`PermissionError: [Errno 1]`. Those are the only test exclusions. Native syscall
unit tests and all six supplemental Kermit pipe/BBS workflows still run.

The final combined invocation, with the repository's installed toolchain:

```sh
env WATCOM=/home/eo/src/vc-linux-wt/command/build/openwatcom \
  EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten \
  EM_CACHE=/home/eo/src/vc-linux-wt/command/build/emcache \
  EMCC=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten/emcc \
  NODE=/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin/node \
  LSAN_OPTIONS=detect_leaks=0 \
  PYTEST_ADDOPTS='-k "not test_kermit_dials_bbs_tcp" -x --tb=short' \
  make -k -j3 -o test-modem-transport test web test-web
```

Leak scanning is disabled only because LeakSanitizer refuses this sandbox's
ptrace environment; ASan and UBSan remain enabled. The door suite independently
probes kernel capabilities: its two Landlock signal-scoping cases require ABI 6
(Linux 6.12+) and are skipped by the unchanged upstream checks on this Linux
6.8 host. Other Landlock, seccomp, escape, quota, cleanup and memory tests run.

## Final gate result

The combined command above exited **0**. Its complete output is retained at
`/tmp/vc31-final-verified.log`. It runs `make test`, `make web` and
`make test-web` in one dependency graph, so shared generated files have a
single build owner.

| Gate | Final result |
| --- | --- |
| Translator / Unicorn oracle tests | 317 passed in 490.20 s |
| Main native pty suite | 286 passed, one blocked TCP case deselected, in 417.20 s |
| New native COMMAND nested + I/O suite | 11 passed in 18.24 s |
| DOS EXEC / process / machine | 38,390 checks; 11 process cases; 28 machine cases; zero failures |
| DOS filesystem | 17,525 checks, zero failures; 85 advertised functions/subfunctions exercised |
| Terminal / CGA | 6,311 terminal and 823,877 CGA checks, zero failures |
| Build/packaging | INI 3, Rogue 51, VZ 14, Kermit 64, MS-DOS 25, embed 9, door packaging 34 tests passed |
| Real-door pty tests | 34 passed (28 original plus six COMMAND host-execution regressions) |
| Door confinement | 23 passed; two upstream kernel-capability skips described above |
| Door memory / cleanup | Forged loader, overlapping PSP, cyclic/wrapped MCB controls, 250,000 ASan/UBSan fuzz calls, and deep-tree/symlink/descriptor cleanup passed |
| Browser build and tests | Full asset/size/source/module/input/media/modem/smoke/failure/timeout/memory gates and all twelve new COMMAND cases passed |

The dedicated filesystem and terminal ASan/UBSan runs also passed with leak
scanning disabled, as recorded in the I/O evidence. The measured final wasm
hash remains `57bfe083f49d2148c20ec23c7af07744e5148ab9b97ab5c1cb03c4c1e148bab5`
after the full run. `git -c core.whitespace=cr-at-eol diff --check` passes;
DOS.TXT intentionally retains CRLF. The four originally conflicted files have
no conflict markers; their unmerged index entries were deliberately untouched.

## Startup and first-load budget

Using the original rebase parent `9deb75d` and the final build with the same
toolchain, two warmups and nine alternating fresh Node processes per build:

| Measurement | Before | After | Change |
| --- | ---: | ---: | ---: |
| Median import-to-first-complete-screen | 113.798 ms | 123.859 ms | +10.061 ms |
| Median preRun-to-first-screen | 89.575 ms | 100.803 ms | +11.228 ms |
| Complete first-load gzip bytes | 1,286,227 | 1,299,288 | +13,061 |

The 1,300,000-byte cap passes with 712 bytes spare. Startup includes import,
local wasm read/compile, MEMFS setup, decompression and guest boot; it is not
an isolated decoder benchmark. No lazy module fetch occurs before the first
screen. All eight pre-existing generated C outputs compare identically to the
baseline. The full protocol, every sample and wasm hashes are in the browser
record linked above.

## Three remaining weaknesses

1. Nested VC through COMMAND still lacks DOS3's executable-path environment
   trailer and takes the allowed overlay-file error path instead of showing
   nested panels: `tests/test_command_nested_e2e.py:91`.
2. COMMAND's original COMSPEC limit still rejects resolved shell paths of
   40 bytes or more: `runtime/dos_core.c:1328`.
3. Session pipe cleanup uses atexit, so SIGKILL or a host crash can leave the
   private temporary directory behind: `runtime/dos_fs.c:177`.
