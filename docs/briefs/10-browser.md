# Brief 10: VC in the browser

Read `docs/plans/2026-10-01-browser-build.md` first. Build option B from it: the same translated
VC compiled to WebAssembly with Emscripten, with a narrow Asyncify so the existing wait loops can
pause for the browser. This is a toy, so it must start instantly: no button, no splash, no install.

You have no network. Everything you need is on disk.

## Toolchain

    export EMSDK=$HOME/src/vc-linux-wt/emsdk
    export EM_CONFIG=$EMSDK/.emscripten EM_CACHE=$PWD/build/emcache
    export PATH=$EMSDK/upstream/emscripten:$EMSDK/node/20.18.0_64bit/bin:$PATH

`build/emcache` is already warm. `build/gen/` is built, and `make` builds the native `build/vc`.
This line already compiles and links the whole program (47 s):

    emcc -O2 -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 -sMODULARIZE -sEXPORT_ES6 \
      -sENVIRONMENT=web,node -sFORCE_FILESYSTEM -include <shim> -Iruntime \
      runtime/*.c build/gen/vc_com.c build/gen/vc_ovl.c build/gen/files.c -o build/web/vc.mjs

The shim it used is `~/src/vc-linux/build/wasmprobe/shim.h`. Do not ship a shim. Put real
`#ifdef __EMSCRIPTEN__` fallbacks in `runtime/dos_fs.c` instead (`renameat2` and `statx` are
missing). In the browser no other process exists, so check-then-rename is safe there.

## Why Asyncify stays narrow

Translated code never calls an interrupt handler. `cpu_int` only sets CS:IP, and `rt_run` runs
the handler through `stub()`. So every wait is reached by direct calls:
`rt_run → stub → do_int → handler → term_idle`. Translated functions are reached only through
`Image.run`, a function pointer, which `ASYNCIFY_IGNORE_INDIRECT` skips. The one trap is
`rt_yield()` in `runtime/rt.c`, which translated code calls through `RT_TICK`. Under
`__EMSCRIPTEN__` it must never sleep. If it did, Asyncify would rewrite every translated function.
Prove the set stayed narrow: report the wasm size with and without your changes. Growth above 10%
means something wide got instrumented. Find it and fix it.

## What to build

1. **Shared C, behind `__EMSCRIPTEN__` only.** Native behaviour must not change. `make test` must
   stay green.
   - `term_idle` (runtime/term.c): render, take pending input bytes from JavaScript, feed them
     with `term_feed_input`, and sleep with `emscripten_sleep` in slices of 10 ms or less, until
     input arrives or the deadline passes. Keep the native return rules. `term_idle(0)` must yield
     to the browser at most once per 16 ms and otherwise return at once, because VC polls the
     keyboard between units of real work (the tree scan does it per directory entry).
   - Output goes through `term_set_output` to a JavaScript function that writes to xterm.js.
     Turn truecolor on.
   - `host_run` (runtime/dos_core.c) cannot fork. A command other than `cd` prints, on VC's user
     screen, exactly: `No shell in the browser, only cd works here. The Linux version runs
     commands: github.com/evoleinik/vc-linux`. F4's `vc-edit` prints: `No editor in the browser.
     F3 views the file. The Linux version opens $EDITOR.`
   - A web start-up that skips the terminal check. It sets HOME to `/home/vc`, XDG_CONFIG_HOME and
     XDG_CACHE_HOME under it, and the working directory to `/home/vc`. It feeds `"\033[?0u"` to
     the input parser so kitty mode is on and modifier events update the key bar. Then it installs
     the config files and calls `dos_start` as `runtime/main.c` does. Share code with `main.c`
     rather than copying it. When VC exits, call a JavaScript hook so the page can say so.
2. **Demo files at `H:\` (`/home/vc`)**, embedded with `--embed-file` from a directory the
   Makefile assembles in `build/`:
   - `README.TXT`: what this is, in plain short sentences, and the keys to try (F3, F5, F6, F7,
     F8, Ctrl-[, Ctrl-], Ctrl-I, Ctrl-H, Ctrl-L, Ctrl-Q, Alt-letter speed search, Alt-F10 tree).
   - `HISTORY.TXT`: the "History" section of `README.md` as plain text.
   - `SRC\VC.ASM`, `SRC\VCOVL.ASM` and `SRC\LICENSE.TXT`, copied from `asm/` at build time.
     The sources live once in git.
   - A `GAMES` directory holding `NOTHING.TXT`: "There are no games here. You are already playing
     with a file manager."
   Keep total embedded data under 200 KB.
3. **The page**: `web/index.html` and `web/vc-web.js`, copied with the wasm into `build/web/`.
   - xterm.js 6.0 is vendored at `web/vendor/xterm.mjs` and `xterm.css`. The font is
     `web/vendor/WebPlus_IBM_VGA_8x16.woff` (CC BY-SA 4.0, credit "VileR, int10h.org"), with
     `monospace` as the fallback. Load the font before opening the terminal.
   - Fixed 80×25. Pick the largest font size whose screen fits the window, preferring whole
     multiples of 16 px. Black background, centred. Refit on window resize.
   - Write `"\033[?1002h\033[?1006h"` to xterm at start, so it reports SGR mouse to VC.
   - Input: send `onData` bytes to the C queue. Use `attachCustomKeyEventHandler` to send kitty
     sequences for: Shift, Ctrl and Alt down and up (codes 57441, 57442, 57443, and the right-hand
     57447, 57448, 57449, with event type 3 on release), and Ctrl+[ `\x1b[91;5u`, Ctrl+I
     `\x1b[105;5u`, Ctrl+M `\x1b[109;5u`, Ctrl+H `\x1b[104;5u`. Call `preventDefault` for F1 to
     F12 and for Ctrl keys the browser would take, where the browser allows it.
   - Start on load. One quiet line under the screen: "Volkov Commander 4.99, machine-translated
     from 8086 assembly to WebAssembly. Not an emulator. Files live in memory and vanish on
     reload. Linux version on GitHub." with the link.
   - When VC exits, show "VC has quit. Press any key to start it again." Any key reloads.
4. **Makefile**: `make web` builds `build/web/`. Pick `-O2` or `-Os` by measurement: report gzip
   size and the time from page load to the first VC screen for each, and take the smaller one
   unless it is visibly slower. `make` and `make test` must not need emcc.

## Gate: `make test-web`

`tests/web_smoke.mjs` runs `build/web/vc.mjs` under node. Set `VC_SCREEN_DUMP` to a path in
MEMFS and read it with `FS.readFile`. It waits on screen state, never on fixed timers, and checks:

1. VC starts and the screen shows `10Quit` and `README` (the H: listing).
2. Select `README.TXT` and press F3. The viewer shows the first README line.
3. Type `echo hi` and Enter. The no-shell message appears.
4. Kitty Ctrl-[ (`\x1b[91;5u`) puts the left panel's path on the command line.
5. F10 then Enter quits, and the exit hook fires.

Plant a defect for each check in turn (for example, drop the input queue, or make `host_run` print
nothing), watch it go red, and restore it. Report each one.

## Done means

- `make test` green, and native behaviour unchanged.
- `make web` and `make test-web` green.
- Your final summary lists the files changed and the sizes measured. It names three weaknesses of
  your own work, each with a real `file:line`. Do not commit; I commit.
