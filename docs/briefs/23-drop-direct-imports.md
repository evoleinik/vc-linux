# Brief 23: remove the direct-import binding

Brief 22 section 5 bound five side-module imports straight to the main module's raw wasm exports
through Emscripten's internal `.orig` property. Its own measurement shows no speed gain: the busy
loop took 1,509.99 ms before and 1,500.50 ms after, which is noise. It adds a way for every program
EXEC to fail after an Emscripten upgrade. Keep it simple and take it out.

- Delete `bind_program_imports` and its call in `runtime/web_programs.c`.
- Delete the side-import identity checks: `--direct-imports` and `sideImports` in
  `tests/web_smoke.mjs`, and the `direct_imports` criterion in `tests/web_benchmark.mjs` and in
  whatever comparator reads it. Keep the benchmark itself as a measurement tool.
- In `docs/plans/2026-10-01-mobile.md`, replace the direct-call text with two plain facts. The lazy
  build is about 15% slower on a busy BASIC loop. Binding the imports directly did not change that,
  so it was removed. Keep the measured numbers. Drop weakness 1, which no longer exists.
- Run one benchmark of this build and record the median in the same table.

Done means `make test`, `make web`, `make test-web` green, first load still at most 1.3 MB gzipped,
and `rg -n "\.orig|bind_program_imports|direct.imports" runtime tests docs/plans` prints nothing.
Give the summary with three weaknesses, each with a real `file:line`. Do not commit.
