# Rogue's DOS build adapters

`make rogue` uses `WATCOM` and writes only `build/rogue`. The game and PDCurses
sources in `third_party` are never edited. `tools/rogue_port.py` copies the game
sources to the build directory and applies exact-count-checked substitutions.
An upstream change that no longer matches is an error, not a skipped patch.

The compiler options are DOS, 8086, large model, software floating point,
signed `char`, size optimization and a 1024-byte far-data threshold. The last
option puts the large map/room arrays outside DGROUP so a checked 16 KiB stack
fits. `nofarcalls` keeps the linker from rewriting object instruction spans.
All objects, the PDCurses library and a verbose map are retained for translation.

The local changes are:

- `mdport.c`: fixed operating-system player name `Rogue`; no password database,
  shell process, Unix signals, privilege changes or terminal job control.
  BIOS/PDCurses decoded keys map to Rogue's original movement/run commands.
  DOS files use binary I/O. An open save is closed before DOS unlinks it.
- `startup.c`: a plain invocation resumes `rogue.sav` if it exists in the DOS
  current directory. A separate DOS child attempts the restore so a rejection
  cannot leave half-loaded globals or inventory in the fresh game. A rejected
  save is renamed to `rogue.bad`, with a one-line notice inside the new dungeon.
  If the rename fails (including an existing `rogue.bad`), both files are kept
  and the new game still starts with an accurate warning. Explicit arguments
  retain upstream's meaning; explicit failed restores are not quarantined.
  Saves and scores default to `rogue.sav` and `rogue.scr`. DOS score locking is
  disabled: one DOS program runs at a time, and an unwritable directory must
  not impose Unix lock retries before returning to VC.
- RNG state, experience, level thresholds, gold and scores retain 32 bits.
  Winning inventory valuation, score comparisons and all related save/score
  fields and display formats retain that width as well. The RNG's original
  Unix `abs((int)RN)` receives a nonnegative 0..65535 value; the DOS expression
  therefore uses unsigned arithmetic instead of a narrowing signed 16-bit cast.
- Save integers are still four-byte little-endian values. Native 16-bit ints
  are sign/zero-extended on write and read into four-byte temporaries. Markers,
  RNG, experience, gold and threshold arrays use explicit `long` helpers.
  Saved PDCurses cells retain all 32 `chtype` bits, including standout and colour.
- The redundant Unix `environ` declaration is removed; Watcom's own declaration
  supplies its required memory-model attributes. `Q,y` skips the additional
  score-screen acknowledgement; death/win screens retain it.
- A failed deserialization does not unlink its source. Save failures from serialization,
  stream state, flush or close resume the live game, remove the partial attempt,
  and permit retry. Successful saves retain upstream's exit/restore behavior.
- `acs437.h` restores a public-domain PDCurses shared table omitted from the
  vendored subset, at the same upstream commit recorded in its `UPSTREAM` file.
  Only the generated PDCurses display copy's include path changes.

`tests/test_rogue_build.py` executes actual DOS routines in Unicorn to test
32-bit RNG arithmetic and thresholds, poisoned-stack serialization, restore
buffer guards, high marker words, PDCurses cell attributes, winning gold across
32767, score text/save-state round trips, and every save-error cleanup path.
It also rebuilds into a fresh directory, checks the verified EXE's SHA-256, and
requires identical bytes without changing either vendor tree or linking stale
source copies. A negative control appends the same deterministic overlay to
two isolated real builds, proving that byte equality alone cannot pass the
identity check. The full translator gate separately verifies every distinct
instruction; these port tests do not replace it.

The DOS port retains 16-bit `int` for ordinary game counters such as coordinates,
turn counts and inventory quantities. Fields whose ranges require 32 bits have
been widened. The legacy save parser is not a hardened parser for arbitrary
hostile files; detecting a truncated/corrupt stream is not full schema checking.
Failed replacement saves preserve the live game, not an older on-disk save:
upstream opens the selected filename for overwrite before serialization.

`OWLIC.TXT`, generated from the installed toolchain's license plus a runtime
source-location notice, accompanies the redistributed executable. The toolchain
itself is not vendored.

## OpenWatcom pin provenance

`tools/fetch-openwatcom.sh` downloads this immutable release asset:

```text
https://github.com/open-watcom/open-watcom-v2/releases/download/2026-10-01-Build/ow-snapshot.tar.xz
SHA-256: e6aa1b1e40ac8bbf97658d2c70fff8a4242d6ca4a1c60806f2baa5317083d4fe
```

The comparison recorded on 2026-10-01 in `docs/briefs/18-rogue-fixes.md` found
the release's `wcc`, `wlink`, `wdis`, `wlib` and `lib286/dos/clibl.lib`
byte-identical to `$HOME/src/vc-linux-wt/tools-cache/openwatcom`. This is the
release archive's checksum, not a digest inferred from an extracted tree.
The earlier missing-checksum prerequisite is therefore resolved.

That toolchain produced the verified post-width-review, pre-brief-18 EXE:
202,816 bytes, SHA-256
`8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee`.
Source changes require a newly verified executable identity; the release
archive's identity remains the same.

The brief-18 source fixes produce a different EXE: 206,808 bytes, SHA-256
`b6350f98553e96ae5454383ec377d9feb63834cc99fc3411f7ff016321dd61a7`.
`test_rogue_build_is_byte_reproducible` requires this corrected build's hash
and independently rebuilds the historical source variant to assert the
original `8844...1c9ee` identity above. The two hashes are never alternatives
for the shipping executable.

`tests/fixtures/rogue-pre18.toml` reverses only the brief-18 startup, score-lock
configuration and two recovery-notice substitutions in temporary source
copies. Each replacement must match exactly once; Python's standard-library
TOML reader applies them without a patch-tool dependency or fuzzy matching.
The unchanged vendor trees are only read, the production builder compiles the
historical variant in that temporary directory, and its EXE is never shipped.
This intentionally source-sensitive probe needs review if those source
fragments change again; it is historical toolchain evidence, not a substitute
for checking the corrected build.

```sh
tools/fetch-openwatcom.sh build/openwatcom && export WATCOM=$PWD/build/openwatcom
```
