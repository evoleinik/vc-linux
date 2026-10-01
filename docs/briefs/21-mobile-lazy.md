# Brief 21: programs load on first use, a portrait key pad, Russian and Ukrainian text

Read `docs/plans/2026-10-01-mobile.md` (decided), `docs/plans/2026-10-01-browser-build.md` and
`CLAUDE.md` first. Toolchain: `docs/briefs/10-browser.md`; OpenWatcom for Rogue through `WATCOM`.

You have no network, and you cannot launch a browser in this sandbox. Every check must run under
node; I test in real browsers afterwards.

## 1. Each program loads the first time DOS runs it

Today one wasm holds VC and every translated program: about 3 MB gzipped with Rogue and VZ, and
each new program adds more. The page should download VC alone first, about 1 MB, then fetch a
program's translated code the first time DOS EXEC runs that program, and keep it for the session.

- Choose the mechanism and prove it. Emscripten dynamic linking (`MAIN_MODULE` with one
  `SIDE_MODULE` per translated image, loaded with `dlopen`) is the obvious candidate. It must work
  with the existing narrow Asyncify, under which EXEC may wait for the download. If dynamic linking
  cannot work with Asyncify, explain why and use another design, such as separate wasm instances
  sharing VC's memory.
- The translated C stays generated and unedited. VC.COM and VC.OVL stay in the main module.
- While a program downloads, VC shows nothing new and simply waits, as a slow floppy did. A
  download failure ends that EXEC with a DOS error and VC keeps running.
- Every fetched file carries the build hash, like `vc.wasm` does today (`tests/web_assets.mjs`).
- Gates:
  - the gzipped size of everything fetched before VC's first screen is at most 1.3 MB, and the
    gate prints the number;
  - the node smoke test runs GW-BASIC, bootLogo, Rogue and VZ, and proves each module was fetched
    only when first run;
  - a failed module fetch gives a DOS error and VC stays usable.
- The Linux build does not change.

## 2. A key pad in portrait on touch devices

As decided in `docs/plans/2026-10-01-mobile.md`: only when the device has a coarse pointer and is
in portrait (`matchMedia('(pointer: coarse) and (orientation: portrait)')`), a key pad appears
under the screen. The screen keeps its current size and fit exactly. Landscape and desktop do not
change at all.

- Keys: F1 to F10, the four arrows, Esc, Tab, Ins, Enter, and sticky Ctrl, Alt and Shift that apply
  to the next key and show when they are on. Also a button that focuses xterm's text area, so the
  phone's own keyboard opens for typing.
- Keys go through the same input path as real keys, as kitty sequences where `vc-web.js` already
  builds them, so VC's key bar reacts to held modifiers.
- Buttons are at least 44 px high. A tap must never zoom or scroll the page (`touch-action:
  manipulation`).
- Taps on the screen stay mouse clicks, as today.
- Keep the key-pad logic in a small module with pure functions, and test those under node: each
  key's bytes, sticky modifier on and off, and the portrait media query on and off.

## 3. Russian and Ukrainian text

Translations are in `docs/translations-2026-10-01.md` (made with Gemini, reviewed for code page
fit).
- The page footer and the quit message follow `navigator.language`: Russian for `ru`, Ukrainian for
  `uk`, English otherwise.
- `H:\ПРОЧТИ.TXT` holds the Russian README, encoded in code page 866. Check it shows correctly in
  VC's viewer.
- Ukrainian stays on the page only. CP866 has no letter і, and correct Ukrainian uses it on almost
  every line.

## Done means

`make test`, `make web`, `make test-web` green. Your summary gives the before and after first-load
size and the mechanism you chose, and names three weaknesses of your own work, each with a real
`file:line`. Do not commit.

## Local toolchain

OpenWatcom is already unpacked at `build/openwatcom`: `export WATCOM=$PWD/build/openwatcom`.
`build/emcache` is warm.
