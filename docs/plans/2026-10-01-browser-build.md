# Volkov Commander in the browser

**Type:** Decision doc
**Status:** building
**Date:** 2026-10-01

## Goal

Eugene asked on 2026-10-01: "can we run it in browser now? and find a good funny domain for it.
if it's a toy, then just make it instant". So the browser build is a toy. Opening the page should
show VC running, with nothing to install and no loading screen worth noticing.

## Criteria, fixed before the options

1. **Instant.** A small download, no install step, no page reload, no splash.
2. **Same code.** It runs the same translated VC as the Linux build. No emulator, which was the
   point of the whole port.
3. **Little new code.** The existing suites keep guarding the shared runtime.
4. **Free personal hosting.** Nothing on the AirShelf company accounts.

## Facts measured first

- The whole program compiles with Emscripten 4.0.2 after two small shims (`renameat2`, `statx`).
  The wasm is 3.9 MB, 944 KB gzipped. The compile takes 47 seconds.
- Translated code never calls an interrupt handler itself. `cpu_int` only sets CS:IP, and the
  dispatcher `rt_run` runs the handler. So every wait happens with a short C stack:
  `rt_run → stub → do_int → handler → term_idle`. The one exception is `rt_yield`, which
  translated code calls through `RT_TICK`.

## Options

| Option | Strongest objection | Fails when |
|---|---|---|
| A. Emscripten, and the dispatcher returns to the browser. A wait unwinds to `rt_run` with `longjmp`, rolls the CPU back to the interrupt, and runs it again later. | Every wait site must be safe to re-run. DOS line input (INT 21h 0Ah) would lose keys already typed. | A handler writes memory before it waits. |
| **B. Emscripten with a narrow Asyncify.** `term_idle` sleeps with `emscripten_sleep`, and only the short chain above is rewritten to pause. Translated code is left alone because the calls into it are indirect. | Asyncify is compiler magic. A sleep reached through an indirect call aborts at run time. | Someone makes `rt_yield` sleep, or adds a wait under a function pointer. |
| C. The unchanged native code in a Web Worker, blocking on `Atomics.wait`. | It needs COOP and COEP headers. GitHub Pages cannot send them, so a service worker must reload the page first. | Safari, iframes, or any host without those headers. |
| D. js-dos or v86 running the real VC.COM. | It is an emulator. Eugene rejected that on 2026-09-30. | Always, against criterion 2. |

## Recommendation

B. It changes the fewest lines of shared C, keeps every wait loop as written, and needs no special
headers. The page uses xterm.js to show the ANSI output that `term.c` already produces and that the
e2e suite already checks. Keys go in as kitty-protocol sequences built from browser key events, so
Ctrl-[, Ctrl-I and Ctrl-M work, and holding Shift, Ctrl or Alt changes the key bar.

What would change the ranking. If Asyncify grows the wasm by more than about 30%, A gets cheaper.
If GitHub Pages could send COOP and COEP headers, C would need no C changes at all.

Hosting: GitHub Pages from this repo, built and deployed by CI. A custom domain points at it.

Limits that are fine for a toy: files live in memory and vanish on reload. There is no shell, so a
command line other than `cd` explains that. F4 has no editor to open.

## Questions for Eugene

1. (Criterion 4) Which domain, and who registers it?

## Answers

1. Eugene, 2026-10-01: `notanemulator.com`, registered by him. It beat `f10toquit.com`,
   `noemulatorswereharmed.com` and `64k.lol`, all unregistered that day by RDAP. GitHub Pages
   hosts it, so nothing lands on the AirShelf accounts.

## Decision — 2026-10-01

Option B, served by GitHub Pages at `notanemulator.com`.

## Outcome — 2026-10-01

Built by codex from `docs/briefs/10-browser.md`, then checked in Chromium.

- `make web` builds `build/web/`. `make test-web` runs VC under node and checks start-up, F3,
  the no-shell message, kitty Ctrl-[ and quit. Each check went red on a planted defect.
- The wasm is 4.0 MB, 993 KB gzipped, 3.8% larger than the unchanged program. So Asyncify
  stayed narrow. `-O2` beat `-Os` on gzipped size.
- In Chromium the first VC screen appears 120 to 170 ms after page load, served locally.
- A real Ctrl-[ keypress puts `C:\` on the command line. Holding Ctrl swaps the key bar, and
  releasing it swaps back. F3 shows the README in the VGA font.
- VC's settings and log live in `/var/vc`, so H: shows only the demo files.

Known limits: the paste queue in `web/vc-web.js` has no cap. MEMFS has no birth time, so a
file's creation date falls back to its change time.

Domain, 2026-10-01: Eugene registered `notanemulator.com` at GoDaddy and added the four GitHub
Pages A records. Claude deleted GoDaddy's "Parked" A record, pointed `www` at
`evoleinik.github.io`, set the Pages custom domain and turned on HTTPS. The github.io and `www`
addresses now redirect to https://notanemulator.com/.
