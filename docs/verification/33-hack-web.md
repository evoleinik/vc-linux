# Brief 33: Hack browser preservation

This is the historical, pre-rebase record. Brief 36 replaced its directory
bundles with main's per-file first-OPEN mechanism and restored VC 4.05;
see [the current verification record](36-hack-rebase.md) for current gates
and first-load measurements.

Recorded 2026-10-02. All checks use the actual Emscripten main/side modules,
MEMFS, DOS loader and BIOS text screen under Node 20.18.0. No browser,
network connection or loopback listener is used.

## Publication and first use

`tools/web_modules.py` publishes thirteen immutable translated side modules.
The real `image_hack` is exported only by the Hack side module. The complete
MZ executable, six data files, both unedited BSD notices and OpenWatcom
notice travel in a separate content-addressed `hack-files.<hash>.bin`.
The main wasm pins its filename, exact size and full SHA-256.

The first VC screen has neither Hack files nor Hack translated code. Opening
`H:\GAMES\HACK` fetches and verifies the file bundle; pressing Enter on
`HACK.EXE` then fetches the translation. A later typed `hack` reuses both.
The verified executable reference is independent of mutable H: files, and
late installation never overwrites files already present in that session.

The added loader initially pushed the complete startup above the unchanged
1,300,000-byte gzip limit: 1,303,849 bytes. The two original H: assembly files,
`SRC\VC.ASM` and `SRC\VCOVL.ASM`, now use the same lazy-file mechanism in a
second immutable bundle. Their bytes, names and DOS access remain unchanged;
the separate Source panel continues using its existing lazy source maps.

Directory laziness also covers operations before the first listing. F6 on
unopened `SRC` preserves its sources, renaming unopened `GAMES` preserves
its Hack subtree, and real DOS RMDIR cannot remove the apparently empty Hack
placeholder. These three scenarios failed before the corresponding
directory hooks were added.

File integrity does not depend on WebCrypto or a secure browser context.
The small portable SHA-256 implementation runs standard empty-message,
`abc`, two-block-padding and million-`a` vectors natively. Dedicated Node
smokes explicitly remove `globalThis.crypto`, exercising the same code used
when the site is served over ordinary HTTP.

## Reproduction

From the repository root, use the pinned local toolchains:

```sh
export WATCOM="$PWD/build/openwatcom"
export EM_CONFIG=/home/eo/src/vc-linux-wt/emsdk/.emscripten
export EM_CACHE="$PWD/build/emcache"
export PATH=/home/eo/src/vc-linux-wt/emsdk/upstream/emscripten:/home/eo/src/vc-linux-wt/emsdk/node/20.18.0_64bit/bin:$PATH
make -j3 test-web
node tools/hack_web_mutations.mjs build/web
make web
```

`test-web` depends on `web` and `test-web-sha256`. Besides all pre-existing
web gates, it runs the full smoke with Hack and four focused WebCrypto-free
smokes: `--hack-only`, `--source-only`, `--lazy-parent-move`, and
`--lazy-rmdir`. Each first-use download is deliberately held while checking
that VC yields quietly, then supplied from the actual publication files.

The mutation runner creates a fresh, narrowly scoped temporary publication
for each defect. Unchanged files are symlinked and only the private
replacements are written. It never changes live publication bytes.
Retired real publication generations are recoverable
under `build/web-work/retired-modules/`.

## Final gate record

The final `HACK.EXE` is 243,304 bytes, SHA-256
`cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9`.
Its browser translation is `hack.e5af4d6d3d73.wasm` (9,416,673 bytes), and
its DOS-file bundle is `hack-files.c19d720d8d0b.bin` (311,134 bytes).
The unchanged source bundle is `source-files.94511ab10602.bin`
(143,015 bytes). These are all first-use payloads, not startup downloads.

`make -j3 test-web` completed with exit 0 on that pin. Its complete record
is `build/hack-web-gates.log`. Page version: `a91dd139f422`.
The main wasm is 6,229,000 bytes, SHA-256
`64dffd2cb437d8081bf2a6ae937b1df283fc34331eafe66d36124c953ad759e9`.

| Measurement | Final bytes |
| --- | ---: |
| Main wasm, gzip level 9 | 1,116,944 |
| Complete first load, all thirteen startup assets | 1,275,545 |
| Remaining first-load allowance | 24,455 |
| Configured initial memory, unchanged during full smoke | 100,663,296 |
| Measured allocator high-water after all thirteen programs | 54,875,464 |
| Conservative any-module-order bound | 71,702,202 |
| Headroom above that bound | 28,961,094 |

The startup count includes the HTML, transitive JS/CSS, VGA font and wasm;
it is not merely the main wasm size. The heap gate still requires at least
4 MiB and 20 percent headroom above the conservative order-independent
bound. The fixed-memory failure smoke also passes with 47,972,352 aggregate
free bytes split into unsuitable 128 KiB holes: DOS error 8, preserved files,
working F3, successful retry, and cached rerun.

All existing source-map, source-panel, layout, source-runtime, keypad,
language, speaker, graphics, modem and COMMAND regressions remain green.
The full smoke observes Hack's original experience and character prompts,
level-one `@` and Gold/Hp/Ac/Str/Exp status, Enter launch, Q/y return, cached
typed launch, and restoration of VC's original H: root directory. All four
focused WebCrypto-free cases pass as well. The existing real-fetch error,
body/response timeout, retry and cache gates also pass.

## Planted defects

All nine defects were planted independently and each gate went red for the
intended reason, not an unrelated build or setup failure:

| # | One planted defect | Gate and observed failure |
| --- | --- | --- |
| 0 | Flip one bit of SHA-256's initial state in a private C copy. | Native standard vectors reject the empty-message digest. |
| 1 | Replace the Hack wasm with the real Rogue wasm. | Module ABI gate rejects the missing Hack-only image export. |
| 2 | Flip the final byte of the Hack file bundle without renaming it. | Asset gate rejects the filename/hash mismatch. |
| 3 | Add a preload of the Hack file bundle to the private built page. | Startup gate rejects an eager lazy-file dependency. |
| 4 | Change main's `image_hack` lookup to `image_FAKE`. | Hack smoke records that its loaded translation lacks its image/runner. |
| 5 | Mark the source directory loaded in main's real wasm initializer. | Unopened SRC F6 gate times out waiting for the missing first-use file fetch. |
| 6 | Mark Hack's directory loaded in that initializer. | Unopened GAMES F6 gate times out waiting for the missing Hack subtree fetch. |
| 7 | Independently mark Hack's directory loaded again. | Real DOS RMDIR gate detects that its required nonempty-directory fetch never happened. |
| 8 | Flip the final byte of the downloaded Hack bundle. | WebCrypto-free Hack smoke records the portable-C SHA-256 mismatch. |

The mutation runner completed with exit 0. After all nine reds, the original
asset, module, startup-size and full-smoke gates returned green, as did all
four WebCrypto-free smokes and the native SHA-256 vectors. The summary is
`build/hack-web-mutation-run.log`; individual red and restored logs are in
`build/hack-web-mutations/`. The private mutant directories were removed;
live publication files were never modified by the runner.

Finally, an explicit `make web` completed with exit 0 and no recompilation,
republishing the same `e5af4d6d3d73` side generation and source index. Its
record is `build/hack-web-noop.log`.

## Remaining limitations

- `runtime/web_programs.c:115` fetches a whole directory, even for one file;
  `runtime/web_programs.c:169` retains the complete 311,134-byte Hack bundle
  for the session as its immutable executable reference.
- `tests/web_smoke.mjs:560` exercises the Cave-man role only. Hack movement
  and save/restore are covered by the separate native pty gate, not this
  browser smoke.
