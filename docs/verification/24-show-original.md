# Brief 24: original-source panel verification

Date: 2026-10-01. No network, browser, or commit was used.

## Result

The Source button and **Ctrl-Shift-F12** toggle the browser-only panel. It shows
the suspended guest instruction with eight surrounding lines on either side,
up to eight listing-proved CALL sites from the stack, and the newest 32 distinct
source lines found in the dispatch-entry history. Source text is assigned through
`textContent`, never interpreted as HTML. Closing restores terminal focus without
injecting a guest key. The terminal's font-size budget and layout remain independent
of the panel; below-screen placement precedes the phone keypad.

Ctrl-Alt-S was not free: `runtime/term.c:900` gives Alt precedence, producing the
Alt-S scan code 1F00h; `asm/VCFUNC.INC:2418` and `asm/VCOVL.ASM:1917` put it into
speed search. Ctrl-Shift-F12 produces the unbound 8A00h function-key word
(`runtime/term.c:965`). It also avoids VC's Alt-release menu latch and Linux's
Ctrl-Alt-function-key console shortcuts. The smoke test exercises the actual page
capture handler, modifier reports, repeats, and both modifier-release orders.

VC's input loop really resides in `VCKEYB.INC`, included by `VCOVL.ASM:3474`.
The panel retains the actual included-file line and shows that real ASM include
breadcrumb; it does not invent an instruction in the top-level ASM file.

The vendored files are not all CP866: some are already UTF-8, and some VZ comments
are Japanese Windows-31J/Shift-JIS. Those existing encodings are preserved; CP866
is used for the DOS Cyrillic fallback. A separate CP866 fixture tests that fallback.
Rogue has map-file function names and offsets, explicitly labelled as C functions,
with no fabricated C line numbers.

## Files changed

- `Makefile`: web-only map generation, lazy asset publication, and Node gates.
- `runtime/rt.c`, `runtime/web_source.h`: browser-only address history and read-only
  snapshot/resolver ABI; approved relocated/copy bytes are reused for resolution.
- `web/index.html`, `web/vc-web.js`, `web/vc-source.js`: button, shortcut, deferred
  module loading, source lookup, CALL filtering, literal rendering, and layout.
- `tools/source_maps.py`: listing/map provenance, immutable source assets, and
  recoverable publication cache.
- `tools/source_map_mutations.mjs`: reproducible isolated source-map defect proofs.
- `tests/test_source_maps.mjs`, `tests/test_web_source.mjs`: independent map,
  stack, rendering, shortcut, layout, and lifecycle checks.
- `tests/source_runtime.c`, `tests/source_runtime.mjs`: real dispatcher/ring,
  copied-code, interrupt-context, and guest-state immutability checks under Node.
- `tests/web_smoke.mjs`: Source checks in VC, BASIC, bootLogo, Rogue, and VZ.
- `tests/web_language.mjs`: supply the real module URL to the existing non-module
  VM harness so its language/exit checks can parse the new lazy URL expression.
- `docs/verification/24-show-original.md`: this report.

The pre-existing untracked `docs/briefs/24-show-original.md` was read, not changed.
An offline local `.venv` was populated from the already-installed environment;
it and generated build outputs remain ignored.

## Gates and sizes

All commands used the pinned local OpenWatcom/Emscripten toolchains and warm
`build/emcache`; the final web gate used Emscripten's Node 20.18.0.

| Gate | Result |
| --- | --- |
| `make -j3 test` | PASS: 274 translator tests, 208 e2e tests; filesystem 4,641 checks, EXEC 3,058, machine 28 cases, process 9, terminal 5,822 checks, CGA 823,877; INI 3, Rogue build 45, VZ build 14 |
| `make web` | PASS: `build/web/` published |
| `make test-web` | PASS: all map/runtime/UI/asset/size/module/keypad/language/audio/graphics gates; normal 26-check smoke plus download-failure, timeout, and memory-pressure variants |
| Independent VC listing coverage | PASS: every 2,826 COM and 37,992 OVL listed instruction address has one mapping, plus ten mnemonic samples for each image |
| Full generated maps | 2,890 COM, 40,261 OVL, 20,555 BASIC, 217 Logo, 23,159 VZ instruction starts; 887 Rogue function intervals |
| First-load gzip, level 9 | **1,283,418 bytes**, below the 1,300,000-byte limit by 16,582 bytes |
| Source text actually fetched on first open in VC | **227,324 bytes in six files** |
| First open including index and address maps | **1,035,322 bytes**; none fetched before VC's first screen |
| All available original source text | 2,582,231 bytes in 96 immutable files; 732,441 bytes when gzipping each separately |
| Published source assets | 103 current files: 96 texts, six maps, one index |

The main wasm contributes 1,129,387 gzip bytes to the first-load total. The panel's
JavaScript, maps, and additional source texts are lazy. Subsequent opens reuse the
session cache; the all-program smoke observed 32 fetched immutable assets totaling
2,849,141 bytes, without a duplicate request.

Original text available by image (these are whole-image inventories, not claims
that every file is fetched during the first draw):

| Image | Source files | UTF-8 published text bytes |
| --- | ---: | ---: |
| VC.COM | 1 | 77,048 |
| VC.OVL | 27 | 903,649 |
| GWBASIC.EXE | 36 | 1,062,152 |
| LOGO.COM | 1 | 12,921 |
| VZ.COM | 31 | 526,461 |
| ROGUE.EXE | 0 | 0; link-map functions only |

All 103 source assets were also restored byte-for-byte into an empty temporary
publish directory using `tools/source_maps.py --restore`. Seven obsolete generated
assets were archived in `build/web-work/source-assets`; no original was removed.

For the Linux isolation check, the entire preprocessed `runtime/rt.c` before and
after the change was identical (`cc -E -P -std=gnu11 -Iruntime`). Both SHA-256 values
were `cef73fd84b918310d5d07887d123efc20ce2a02570c9c687c61f2548b525a342`.
No translator or vendored-source file was edited.

## Planted defects

Each row was tested separately in an isolated implementation or publication copy.
Every defective run exited **1**, and its restored gate exited **0**. Published
maps/indexes were rehashed when necessary so address/provenance mutations reached
their intended oracle instead of merely failing the hash check. Each restored
program-smoke mutant passed the entire normal smoke suite, not only its own stage.

| # | Deliberate defect | Observed red gate |
| ---: | --- | --- |
| 1 | Remove COM instruction 0Ah from its map | Listed instruction has no source mapping |
| 2 | Remove OVL instruction 0Ch from its map | Listed instruction has no source mapping |
| 3 | Duplicate a COM instruction mapping | Duplicate/unsorted instruction mapping |
| 4 | Point a sampled COM instruction at source line 1 | Original JMP mnemonic missing |
| 5 | Point a sampled OVL instruction at source line 1 | Original CLD mnemonic missing |
| 6 | Remove a CALL-end entry | CALL metadata differs from the independent listing |
| 7 | Change an original source comment | Published original text differs from the repository |
| 8 | Change an include breadcrumb line | Parent line is not the original INCLUDE |
| 9 | Add invented C lines to Rogue | Compiled C must not claim source-line mappings |
| 10 | Change source bytes while retaining their filename hash | Content hash disagrees with bytes |
| 11 | Decode CP866 as Latin-1 | Original Cyrillic comment not preserved |
| 12 | Do not promote repeated history entries to newest | Expected idle recency `[500,502,501,1199]` lost |
| 13 | Require six matching bytes at a copied routine tail | Valid dispatcher's three-byte copy match rejected |
| 14 | Fail to subtract the proven INT instruction length | Current offset 42h instead of 40h |
| 15 | Change guest IP while taking a snapshot | Full CPU-state immutability assertion |
| 16 | Accept code pointers which do not follow CALL | Adversarial stack yields two callers instead of one |
| 17 | Stop deduplicating mapped history lines | Newest 32 distinct-line expectation fails |
| 18 | Render original text with `innerHTML` | Literal-text DOM double rejects HTML interpretation |
| 19 | Fetch the source index during panel construction | Closed panel makes one request instead of zero |
| 20 | Claim source exists for an unmapped program | Required one-line no-map explanation is absent |
| 21 | Toggle on repeated shortcut keydown | Two toggles instead of one |
| 22 | Leak F12 keyup after modifiers are released | Two intercepted events instead of three |
| 23 | Place the wide panel to the left | Left position 288 instead of 952 |
| 24 | Disable Source's refresh interval | F9 never produces the source-proved menu caller |
| 25 | Suppress only the BASIC source map | BASIC Source stage times out with a no-map view |
| 26 | Remove Rogue's truthful C-function/map label | Rogue provenance-label assertion |
| 27 | Suppress only the bootLogo source map | LOGO.COM Source stage times out |
| 28 | Suppress only the VZ source map | VZ.COM Source stage times out |
| 29 | Omit terminal-focus restoration on close | Zero focus returns instead of one |
| 30 | Construct the panel when its import completes after quit | One post-exit construction instead of zero |
| 31 | Reveal an unpositioned panel after module-load failure | Error overlay must remain hidden |
| 32 | Remove the stable scrollbar gutter | Centered-screen scrollbar-preservation check |
| 33 | Preload the OVL source map eagerly | First load becomes 1,509,316 gzip bytes, exceeding 1.3 MB |

The source-map cases can be repeated with:

```sh
node tools/source_map_mutations.mjs build/web
```

The other gates used `tests/test_web_source.mjs`, `tests/source_runtime.mjs`,
`tests/web_size.mjs`, and `tests/web_smoke.mjs`, with the isolated output path.
Expected smoke timeouts used `WEB_SMOKE_TIMEOUT=4000`; restored runs used the
ordinary 10-second watchdog. Live repository/build code was not left mutated.

## Three weaknesses

1. `runtime/rt.c:640`: stack inspection is capped at 1,024 words and the current
   segment end. Callers behind a larger frame can be omitted.
2. `runtime/rt.c:323`: history retains 1,024 distinct canonical block addresses.
   An unusually large expansion sharing a small number of source lines can evict
   older entries before 32 distinct source lines are found.
3. `tools/source_maps.py:377`: Rogue ranges stop at the next exported symbol or
   code-segment boundary. Private functions and precise function ends are not
   available from this map; the panel cannot provide that finer precision.

Real-browser behavior is intentionally left for the user's browser checks, as
the brief requires. All verification here ran locally under Node or the native
test toolchain.
