# Brief 32: H: files that load on first open, then VC 4.05 beside 4.99.09

Read `CLAUDE.md`, `docs/plans/2026-10-01-roadmap.md` (item 1 and the 2026-10-02 outcomes) and
`docs/plans/2026-10-01-mobile.md` first. No network, no browser. `export
WATCOM=$PWD/build/openwatcom`; emsdk as before. Your sandbox blocks loopback sockets, so the two
TCP tests fail there; run everything else and I run those.

## 1. H: file contents load on first open (browser)

First load is 1,299,288 gzip bytes against the 1.3 MB cap, because every H: file's content is
packed into the main wasm. Change that:
- VC's first screen shows the complete H: listing with real names, sizes and dates, exactly as
  today.
- A file's content is fetched the first time a DOS call opens it, through the same mechanism and
  the same hashed-name rules as program modules: the DOS call waits under the existing narrow
  Asyncify, and a failed fetch gives a DOS error with VC still running. Keep small files that VC
  itself reads at start inside the main wasm, and say which.
- The translated programs' own executables keep loading as today.
- Gates: first load drops by at least 150 KB from today's figure and the gate prints it; opening a
  lazy file fetches it exactly once; F3 on `H:\SRC\VC.ASM` shows its first line; a failed fetch
  gives a DOS error and VC stays usable; Linux and door mode are unchanged.

## 2. VC 4.05

Vsevolod Volkov's VC 4.05 (June 2000) carries the same BSD-2 licence as 4.99.09 (compared byte
for byte). Inputs are in `build/vc405-inputs/`:
- `vc405.zip` (SHA-256 `c4ede94b833ea81da24483231de157bd64b67cf1ec66a77be383490046889c3e`): the
  source, from github.com/ddanila/vc, `archives/vc405.zip`, the same tree as its
  `versions/4.05/`.
- `tasm/VC.COM` and `tasm/VCSETUP.COM`, with `SHA256SUMS-4.05`: Danila Sukharev's build of that
  source with genuine TASM 4.1 and TLINK 7.1 under MS-DOS 4.0, from his `latest-build` release.
  `MAKE.BAT` in the source shows the original command: `TASMX /z/m9/kh10000`, `TLINK /t/x`.
- `vc005.zip` is VC 0.05 (1992), binary only. Do not translate it; it has no source.

Do this:
- Vendor the source unedited in `asm405/` (beside `asm/` for 4.99.09), with `UPSTREAM` naming the
  repository, commit `dbbb60578dc3a7e4cbc719f2376a3ad685d99040`, the archive hash and date.
- Build VC.COM and VCSETUP.COM with JWasm in the TASM-compatible way the source needs. Fix the
  tool side or its options, never the source. Compare each with Danila's TASM build: byte
  identical is the goal. If not, give the first differing offset and the reason, and keep going.
- Translate both and check every distinct instruction against unicorn. Every earlier program's
  generated C stays byte-identical; prove it with `cmp`.
- Running it: VC.COM 4.05 is matched by bytes like everything else. Browser: `H:\VC405\VC.COM`
  and `VCSETUP.COM`, so Enter on it starts the older VC inside the newer one, and F10 returns. Its
  own setup file must not overwrite 4.99.09's: check where 4.05 writes its configuration and give
  it its own place. Linux: install it as `VC405.COM` in the config directory, so typing `vc405`
  starts it; nothing else typed changes meaning. Door mode: include it on H: like the browser.
- The Source panel maps 4.05's code to `asm405/` files.
- README: add 4.05 to "Preserved so far" and to the History section with one plain paragraph on
  what changed between 4.05 and 4.99.09, taken from the sources and their dates, not guessed.

## Gates

- `make test`, `make web`, `make test-web` green, first load at or under 1.3 MB with the drop
  from part 1 printed.
- E2e on Linux in a pty: `vc405` starts VC 4.05, its panels show files, F3 views a file, F10 quits
  back. The web smoke test does the same from `H:\VC405`.
- Plant a defect for each new gate in turn, watch it go red, restore it, and report each one.

## Done means

All gates green. The summary gives the byte-identity result per file against the TASM build, the
JWasm options or tool fixes needed, the first-load size before and after, and three weaknesses
of your own work, each with a real `file:line`. Do not commit; you cannot write git metadata.
