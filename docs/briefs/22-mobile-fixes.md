# Brief 22: fixes to the lazy-load and key pad change

Brief 21's change is uncommitted in this worktree. A review found the defects below. Fix each
one. For each, first write a test that fails on the current code, show it red, then fix and show
it green. Same toolchain as brief 21: `export WATCOM=$PWD/build/openwatcom`, emsdk on disk, no
network, no browser.

## 1. A tab open across a deploy can link a newer side module into an older vc.wasm (major)

GitHub Pages ignores the query string and always serves the latest deploy. So `gwbasic.wasm?v=OLD`
fetched after a deploy returns the new build's bytes. A function import missing from the old main
module becomes a lazy stub that throws and kills VC. A changed `Cpu` or `Image` layout links and
silently corrupts guest state.

- Put the build hash in each side module's file name, such as `gwbasic.<hash>.wasm`, written by
  `make web` into `build/web`. Then a stale tab gets a 404 after a deploy, and the existing DOS
  error 5 path handles it.
- `vc.wasm`, `vc.mjs` and the page keep today's `?v=` scheme. The deploy replaces the whole site.
- Extend `tests/web_assets.mjs` so it fails if any side module name lacks the hash, or if the
  names the main module requests differ from the files in `build/web`.

## 2. Out of memory during a load corrupts VC (minor)

Emscripten's dylink `getMemory` does not check its `calloc` result. If heap growth fails, the
side module's data lands at address 0, over VC's statics. Each load also keeps a raw copy of the
module bytes in the heap for the session, and a failed attempt leaks another copy.

- Set `INITIAL_MEMORY` to the measured peak after loading all four programs, plus clear headroom,
  so a normal session never grows the heap. Print the peak and the setting in the gate.
- In `web_load_image`, before `dlopen`, check that enough heap is free for the module (its file
  size times a margin you justify). If not, end the EXEC with DOS error 8 (insufficient memory)
  and keep VC running. Test it by limiting memory in the node smoke test.
- Correct the comment at `runtime/web_programs.c` that says no second byte copy is kept.

## 3. The key pad can zoom or select text on iOS (minor)

- Move `touch-action: manipulation` to `#keypad` itself, so a double tap in a gap cannot zoom.
- Add `-webkit-user-select: none` and `-webkit-touch-callout: none` to the buttons.
- Translate the "Keyboard" button label: Russian "Клавиатура", Ukrainian "Клавіатура". Extend the
  language test to cover it.
- Extend `tests/test_web_keypad.mjs` to check these rules are present in the built page.

## 4. A stalled download freezes VC with no way out (minor)

`fetch_program` has no timeout. While it waits, the guest is suspended, so Esc and Ctrl-Break do
nothing. Abort the fetch after 30 seconds with an `AbortController`, and end the EXEC with DOS
error 5. Test it with a transport that never answers, using a short timeout the test can set.

## 5. Side-module calls into VC now pass through JS wrappers (measure)

The Asyncify JS wrappers sit around the main module's exports, so side modules call `cpu_int`,
`flags_get`, `flags_set`, `port_in8` and `port_out8` through JS. Measure a busy GW-BASIC loop
(`FOR I=1 TO 30000: A=A+I: NEXT`) under node, on main's single-wasm build (`git stash` is shared
with other sessions, so build main in a separate `git worktree` under `build/`) and on this build.
Report both times. If this build is more than 15% slower, make side modules call these exports
directly without the wrapper, keeping Asyncify correct, and show the new time.

## Done means

`make test`, `make web`, `make test-web` green, first load still at most 1.3 MB gzipped. Your
summary lists each fix with its red and green evidence, the memory numbers, and the loop timings.
Name three weaknesses of your own work, each with a real `file:line`. Do not commit.
