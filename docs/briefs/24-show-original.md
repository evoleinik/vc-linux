# Brief 24: "show me the original", the 1991 source behind the running code

Read `CLAUDE.md`, `docs/plans/2026-10-01-roadmap.md` (item 2) and
`docs/plans/2026-10-01-browser-build.md` first. Toolchain: `docs/briefs/10-browser.md`.
`export WATCOM=$PWD/build/openwatcom`. `build/emcache` is warm. No network, no browser: every check
runs under node, and I test in real browsers afterwards.

## Goal

The project's claim is that VC runs as its original source. Let a visitor see that source while it
runs. In the browser build, a "Source" button and the key Ctrl-Alt-S open a panel that shows the
original assembly the CPU is executing, with Volkov's comments. The panel updates after every key.
Closing it with the same button or key leaves VC exactly as it was. Check first that VC itself does
not use Ctrl-Alt-S; if it does, pick a free key and say which.

## What the panel shows

1. **Now:** the source line of the current CS:IP, with about eight lines either side. While VC waits
   for a key this is inside its keyboard loop, which is correct.
2. **Called from:** the chain of callers. Walk the guest stack and keep a word only if it is a
   return address: the instruction that ends at that address must be a `CALL` in the listing. Show
   up to eight callers, each as one source line. No guessing beyond that rule.
3. **Just ran:** the last 32 distinct source lines the translated code entered, newest first. Record
   block entry addresses in a ring buffer in the dispatcher, cheaply, and map them to lines only
   when the panel draws.

Each line shows the file name and line number, such as `VCOVL.ASM:4211`. Text is the original
source line with its comment, decoded from code page 866. Lines in the panel are plain text, never
HTML from the source.

## Which programs

- VC.COM and VC.OVL map to `asm/VC.ASM` and `asm/VCOVL.ASM`. They are the point of this feature and
  must work fully.
- GW-BASIC, bootLogo and VZ map to their own vendored assembly sources through their listings.
- Rogue is compiled C. Show the C function name from the map file and the offset into it, labelled
  as such. Do not invent C line numbers.
- When the current program has no map, the panel says so in one line.

## Data and size

- Build one address-to-line map per translated image at build time from the existing listings and
  map files. The sources themselves ship as files the panel fetches the first time it opens, with
  the build hash in the name as for the side modules. Nothing extra loads before VC's first screen;
  the first-load gate in `tests/web_size.mjs` must stay at or under 1.3 MB gzipped.
- The Linux build does not change.

## Layout

The VC screen keeps its size and position. On a wide window the panel sits to the right of the
screen. Otherwise it sits below the screen, above the key pad on phones. Use the IBM VGA font and
VC's own colours. The current line is highlighted.

## Gates

- Node tests for the map builder: for VC.COM and VC.OVL every instruction address in the listing
  maps to exactly one source line, and a sample of ten addresses maps to the line that contains
  that instruction's mnemonic.
- The return-address rule: a test stack holding a real return address, a data word equal to a code
  address that does not follow a CALL, and garbage. Only the first appears.
- The web smoke test opens the panel in VC, presses F9, and checks "Now" and "Called from" name
  `VC.ASM` or `VCOVL.ASM` lines whose text matches the source file. It also opens the panel inside
  GW-BASIC, then closes it and checks VC's screen and keys are unchanged.
- `make test`, `make web`, `make test-web` green. Plant a defect for each new gate in turn, watch it
  go red, restore it, and report each one.

## Done means

All gates green. Your summary lists files changed, the first-load size, the size of the source
files fetched on open, and three weaknesses of your own work, each with a real `file:line`. Do not
commit.
