# Brief 25: source-panel fixes and verification

Date: 2026-10-02. No network, browser, or commit was used. The existing
uncommitted Brief 24 work was preserved and amended only for these fixes.
This report supersedes Brief 24's layout, focus, and include-label claims.

## Result and gates

All required gates passed on the local OpenWatcom/Emscripten toolchain, with
the warm `build/emcache`. The web gate used Emscripten's Node 20.18.0.

| Gate | Result |
| --- | --- |
| `make -j3 test` | PASS, exit 0: 274 translator, 208 e2e, 3 INI, 45 Rogue-build and 14 VZ-build tests; 837,398 C checks and 37 runtime cases |
| `make web` | PASS, exit 0: final assets published in `build/web/` |
| `make test-web` | PASS, exit 0: map, panel, geometry, wiring, compiled runtime, assets, size, modules, keypad, language, audio and graphics gates; all 26 normal smoke checks and download-failure, timeout and memory-limit variants |
| Closed layout against main | PASS: 309,636 exact font-size, screen-box, footer-box and keypad-box comparisons |
| Open/close geometry | PASS: 172,020 full-grid round trips, plus laptop/phone and native-scrollbar boundary tests |
| First-load gzip, level 9 | **1,285,805 bytes**, 14,195 bytes under the 1,300,000-byte limit |
| Source text fetched on first VC open | **227,324 bytes in six files** |
| First VC open including maps and index | **1,035,322 bytes**; still entirely lazy |
| Available original text | 2,582,231 bytes in 96 lazy text files |

Final command logs are `build/brief25-test-native.log`, `build/brief25-web.log`
and `build/brief25-test-web.log`. The last web run also checked Source inside
VC, BASIC, bootLogo, Rogue and VZ, including unchanged guest input on close.
The number of later source downloads depends on the executed guest paths;
that run fetched 33 immutable assets totaling 2,866,564 bytes, with no duplicate
requests. The main wasm contributes 1,129,432 gzip bytes to first load.

The source build retired two superseded generated map/index files from the
publish directory. Both remain recoverable in `build/web-work/source-assets`;
no original source was deleted.

## Red before the fix, green afterwards

Every reported defect received an observed failing regression before its
implementation was changed. The following are actual failures, not hypothetical
mutations. Updated tests which formerly required a stable gutter, source before
the keypad, unconditional close focus, and an unshrinkable open screen were
replaced because Brief 25 explicitly reverses those requirements.

| Fix | Observed red evidence | Green evidence |
| --- | --- | --- |
| Closed page matches main | Page assertions found Source inside the footer, `scrollbar-gutter: stable`, and the overflow/translation machinery; the pure sizing export was absent | Source is fixed at the lower left, outside layout; main's footer markup is restored; gutter/wrapper/translation are gone. All 309,636 closed-layout comparisons pass |
| Useful right-hand panel | Running the old placement function on main's actual 1440×900 geometry returned `below`, with top 829.35, instead of `right` | 1440×900 and 1600×900 have a fully visible right panel, at least 60 text columns and a screen no smaller than main's phone size; scaling uses main's crisp-size choices |
| Phone layout and exact close | Old layout preceded/displaced the keypad; the new page-adapter regression initially made zero pure-function calls and never assigned the below-panel box | 390×844 puts Source after the screen and keypad, uses viewport remainder with a ten-line minimum, scrolls into view once, and restores layout/scroll on close; 172,020 round trips pass |
| Compact labels and include tooltips | Visible text repeated the include prefix; row `title` was undefined | Rows show only their own file and line, with the full include chain in `title`; smoke independently checks both against original source |
| Tabs and long lines | Tests found literal tabs and no intrinsic-width rule for long source blocks | Tabs expand at eight source columns; `pre` remains unwrapped and grows to its content width inside the panel's own horizontal scroller; markup and comments remain literal text |
| Phone keyboard/focus | First lazy tap focused an unfocused textarea once; loaded-panel open/close taps did so three times. Focus options were undefined, and programmatic close also focused | Both handlers remember focus at pointerdown and use `textarea.focus({preventScroll:true})` only for that captured textarea. Unfocused and late-focus taps make zero focus calls; close never focuses |
| Generated prologue provenance | `VC.OVL.lst:29449 generated push bp -> VCKEYB.INC:197: expected VCKEYB.INC:169` | All nine Input0 setup rows 29449–29457 map to its PROC at line 169. Forty generated-row samples, including delayed prologues and RET epilogues, pass |
| Interrupt/casemap frame exclusion | FLAGS `0246h` fabricated a caller at `0243h`; casemap fabricated its own far CALL; wrapped snapshots retained SP `fffeh`. Six of seven tests failed | Snapshot walking skips exactly three interrupt words or two casemap words, with 16-bit wrap. Seven compiled-ABI/caller-filter tests pass; CPU and memory immutability assertions also pass |
| Lost shortcut keyup | After blur or either visibility transition, a plain F12 toggled Source again: two toggles instead of one | Blur and visibility changes clear the latch; plain F12 is unconsumed and reaches the guest |
| Selection on unchanged polls | Identical/cloned snapshots produced four DOM replacements instead of two; repeated no-map snapshots also rebuilt nodes | Equal snapshot contents skip rendering and retain node identity; real changes still update the panel |
| Rogue lookup cost | Looking up the last of 4,096 functions read all 4,096 table entries | Binary search uses 13 indexed reads and retains end-exclusive function/gap behavior |
| Panel keyboard ownership | The source `aside` still had `tabindex="0"` | It has no tabindex; internal wheel/touch scrolling remains enabled |

Additional layout defects found during implementation also went red first:

- Hiding the panel for a resize measurement clamped a scrolled page from 300
  to 0. The adapter now restores that scroll after positioning.
- A classic scrollbar changed the visible width after sizing. The below
  layout now establishes its open-only scrollbar before measuring boxes and
  footer wrapping, and restores the original root policy on close.
- Synthetic non-linear font metrics changed a below-screen toggle from size
  10 to 11. Toggle fitting now retains the original closed-fit anchor.
- Assuming 16px bars failed six minimum-width/minimum-height assertions for
  0px, 17px and 24px native bars. The adapter measures the native bar and the
  pure function budgets that value; all six pass.

## Test design and reproduction

`tests/fixtures/main-screen-layout.mjs` contains main's `fit()` verbatim from
commit `31b4c3342432f87bcb4a0d546b6ae51044a1804d`; a direct comparison against
that commit passed. The fixture does not import the new sizing function.
The grid covers 320–2560 by 480–1440 in 16px steps, also transposed, with and
without the keypad. Nine font-metric variants include ideal VGA, conservative
rounding cases, and the vendored xterm DOM renderer's DPR rounding at
1, 1.25, 1.5 and 2. Browser measurements, rather than a linear-font assumption,
are supplied to the pure function.

Focused commands, using the final published assets:

```sh
node tests/test_web_layout.mjs build/web
node tests/test_web_source_wiring.mjs build/web
node tests/test_web_source.mjs build/web
node tests/test_source_maps.mjs build/web
node tests/source_runtime.mjs build/web-work/source-runtime.mjs
```

During red/green development, the panel tests also used
`node tests/test_web_source.mjs build/web web` to combine current JavaScript
with previously built immutable data. The mapper's green build ran in an
isolated temporary publish directory. Only 139 OVL source-line assignments
changed, all to PROC declarations; addresses, lengths, files, CALL metadata
and the other five maps did not change. All 11 inherited source-map mutation
checks also failed when mutated and passed when restored.

The native preprocessed `runtime/rt.c` is unchanged: the frame-cursor fix is
inside the existing Emscripten-only source-snapshot implementation. No
translator or vendored source was edited.

## Files changed for Brief 25

- `web/index.html`, `web/vc-web.js`, `web/vc-source.js`, new `web/vc-layout.js`.
- `runtime/rt.c`, `tools/source_maps.py`, `Makefile`.
- New `tests/test_web_layout.mjs`, `tests/fixtures/main-screen-layout.mjs`,
  `tests/test_web_source_wiring.mjs`.
- `tests/test_web_source.mjs`, `tests/test_source_maps.mjs`,
  `tests/source_runtime.c`, `tests/source_runtime.mjs`, `tests/web_smoke.mjs`,
  `tests/test_web_keypad.mjs`, `tests/web_assets.mjs`.
- This report. Briefs and the historical Brief 24 report were not changed.

## Three weaknesses of this work

1. `web/vc-source.js:227`: unchanged polling still serializes the entire
   bounded snapshot every 100ms. The optimization avoids DOM replacement,
   not snapshot-copy/serialization cost.
2. `tests/test_source_maps.mjs:163`: generated-row provenance uses 40 sampled
   witnesses pinned to this JWasm listing. It does not exhaustively prove
   the original PROC/RET ownership of every generated instruction.
3. `tests/test_web_source_wiring.mjs:74`: focus, scrolling and sizing are
   exercised through DOM doubles. They cannot establish real browser
   rasterization, selection behavior, or mobile soft-keyboard behavior;
   browser execution was explicitly outside the permitted toolchain.
