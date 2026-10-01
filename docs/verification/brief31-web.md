# Brief 31: browser regressions and startup

Recorded 2026-10-02 with Node v20.18.0, Linux 6.8.0-142-generic x86_64,
Intel Core i9-10900K. Everything below runs offline against the actual wasm
and MEMFS, not a mocked DOS implementation.

## Red before rebuilding

The pre-fix brief-30 web build was retained at `/tmp/vc31-old-web.4QRDHk`.
Its main wasm is 6,373,247 bytes, SHA-256
`a6548c9c420626875ee8701b36b4a3c21584c06e16dc5914db24962dc9b10f51`.
Its complete first load is exactly the brief's 1,296,214 gzip bytes.
The final regression harness was rerun against that unchanged artifact:

```sh
node tests/web_command_fixes.mjs /tmp/vc31-old-web.4QRDHk/vc.mjs \
  tab con nul pipe-h pipe-c fcb-ren fcb-del debug
```

All eight cases failed, independently:

| Case | Observed old behavior |
| --- | --- |
| TAB | `type TAB.TXT` prints `A○B`, not `A       B`. |
| COPY CON | `CON File not found`; NOTE.TXT is absent after input and Ctrl-Z. |
| NUL | `dir > nul` truncates the real lowercase `nul` sentinel. |
| H: pipe | `dir | sort` deletes a reserved-name file in the visible H: root. |
| C: pipe | The same pipeline deletes a reserved-name file in the visible C: root. |
| FCB REN | `ren A?.TXT R?.DAT` does not create the substituted files. |
| FCB DEL | `del A?.TXT` leaves matching files behind. |
| DEBUG G | Untranslated `0896:0100` escapes the child and stalls the runtime. |

The retained log is `/tmp/vc31-web-red.log`; the command exits 1 with
`8 COMMAND browser regression(s) failed`. Each worker has separate MEMFS;
these sentinel mutations never touch the host home or filesystem root.

## Green and what each nested gate actually proves

```sh
node tests/web_command_fixes.mjs build/web/vc.mjs
```

All twelve scenarios pass: `vc`, `vc-line`, `kermit`, `gwbasic`, `debug`,
`tab`, `con`, `nul`, `pipe-h`, `pipe-c`, `fcb-ren`, and `fcb-del`.
The explicit-outcome log is `/tmp/vc31-web-command-green.log`; it confirms
that Kermit takes the real shell/echo/return branch and BASIC takes the
guest-error branch.

- Explicit COMMAND -> VC reports `Error reading overlay file.` and DOS exit
  2; an echo and EXIT demonstrate that COMMAND and its parent VC survive.
- Typing `vc` on VC's own line uses COMMAND /C. The same child error returns
  all the way to the outer panels. F10 and cancellation prove those panels
  remain interactive. This is not a claim that nested VC panels appeared.
- Kermit PUSH really starts another COMMAND, executes an echo, exits to
  Kermit, then returns through COMMAND to VC. The test requires a new image
  load and a new child termination, not an old prompt.
- The shipped GW-BASIC reports `Syntax error` for its unsupported SHELL
  statement (`third_party/gwbasic/GWSTS.ASM:2332`). Its current Ok, SYSTEM,
  COMMAND echo, and return to VC prove containment. The test does not claim
  a shell was launched on that permitted guest-error path.
- DEBUG's five-byte unapproved COM is stopped with a guest diagnostic;
  COMMAND continues and the enclosing VC survives.

The I/O cases check actual output/files. COPY CON retains both CRLF-terminated
lines through Ctrl-Z. NUL leaves the host-file sentinel untouched. Each pipe
case requires a new SORT.EXE load, output containing both input names, and
unchanged contents of both reserved names in both visible drive roots.
Wildcard cases include `?` matching a padded blank and leave B1.TXT alone.
The existing full web smoke additionally checks exact SORT ordering.

Every case proves no first-screen lazy fetch and at most one fetch per side
module. Each has an independent outer watchdog, including for wasm that
never yields. Review tightened stale-screen checks: BASIC's current prompt
excludes its function-key row, shell returns require new termination events,
and VC's outer reload cannot be mistaken for a successfully nested instance.

## Before/after startup measurement

The baseline is the original rebase parent
`9deb75d8f290b6e4f3638a1f1aead9869569fb22`, exported read-only with `git archive`
to `/tmp/vc31-main.0v9llS` and built with the same Emscripten toolchain and
default `-O2` web flags. `main` later advanced to `3b0385e` with its own Rogue
date-pinning fix; the benchmark intentionally retains the original parent.
No git metadata was written.

The baseline and final generated C for VC.COM, VC.OVL, GW-BASIC, its graphics
supplement, Logo, Rogue, VZ, and Kermit compare byte-for-byte equal. The
date-independent Rogue fix retains the existing verified executable hash.

```sh
/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin/node \
  tests/web_startup.mjs /tmp/vc31-main.0v9llS/build/web/vc.mjs build/web/vc.mjs
```

Each measurement uses a fresh Node process. There are two warmups and nine
recorded samples per build, with alternating before/after order and no other
test/build jobs running. The timer starts before JS import and local wasm
read, ending at the first complete VC panel screen (README plus the final
10Quit row). It includes JS import, wasm compilation, MEMFS setup, in-wasm
unpacking and guest boot, but excludes Node process startup. No sample fetches
a lazy side module. OS file caches are warmed; this is not a network or cold
disk benchmark. Private regular-file output capture avoids libuv socketpair
restrictions in this sandbox without changing the measured interval.

| Recorded sample | Before, ms | After, ms |
| --- | ---: | ---: |
| 1 | 113.906151 | 123.740646 |
| 2 | 115.567357 | 134.911192 |
| 3 | 113.797623 | 125.530460 |
| 4 | 125.844247 | 123.068566 |
| 5 | 109.969080 | 123.757530 |
| 6 | 112.176215 | 123.858520 |
| 7 | 111.201997 | 131.993202 |
| 8 | 128.176041 | 125.883440 |
| 9 | 112.270027 | 123.128090 |
| Median | 113.797623 | 123.858520 |

Warmups were 110.233272/114.344081 ms before and
127.218520/128.010593 ms after. Median total startup grows **10.060897 ms**
(about 8.8%). The narrower preRun-to-screen medians are 89.574732 ms before
and 100.802646 ms after (+11.227914 ms). Neither interval isolates the decoder:
main already used LZMA; this branch adds packed DOS programs/source, further
packed data and the reversible 16-bit E8/E9 filter. These numbers report the
incremental whole-startup cost, not a fabricated decode-only measurement.

Raw records are `/tmp/vc31-startup.json` and `/tmp/vc31-startup.log`.

| Artifact | Before | After |
| --- | ---: | ---: |
| Main wasm bytes | 6,242,552 | 6,384,246 |
| Main wasm gzip bytes | 1,127,643 | 1,140,705 |
| Complete first-load gzip bytes | 1,286,227 | 1,299,288 |

The total includes the page, transitive JS/CSS, VGA font and preloaded wasm;
it is not just the wasm size. The unchanged 1,300,000-byte gate passes with
712 bytes remaining. Lazy program/source downloads are still excluded from
first load by the existing asset/module gates.

Main wasm SHA-256 values:

- Before: `94fe0e8fe4786373510b68d25936f5ed47007dd5c0990e281ae4f72d4456aaaf`.
- After: `57bfe083f49d2148c20ec23c7af07744e5148ab9b97ab5c1cb03c4c1e148bab5`.

The final combined native/web/door gate record is in
[brief31-command-fixes.md](brief31-command-fixes.md).
