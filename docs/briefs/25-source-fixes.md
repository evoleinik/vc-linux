# Brief 25: fixes to the source panel

Brief 24's change is uncommitted in this worktree. A review and a real-browser test found the
defects below. For each one, first write a test that fails on the current code and show it red,
then fix it and show it green. Same toolchain and limits as brief 24: no network, no browser.

## 1. With the panel closed, the page must be pixel-identical to main (major)

The Source button made the footer about 8 px taller, so `fit()` gives some window heights a smaller
font. `scrollbar-gutter: stable` also moves VC about 8 px left on Windows and Linux.

- Position the Source button so it adds no height to anything and never moves. Drop
  `scrollbar-gutter` and any overflow wrapper the closed page does not need.
- Move the screen-sizing arithmetic into a pure function. Copy main's arithmetic into a test as a
  fixture. Test that, with the panel closed, the new function returns exactly main's font size,
  screen box and footer box for every window from 320×480 to 2560×1440 in 16 px steps, in portrait
  and landscape, with and without the phone key pad.

## 2. Layout with the panel open

- When the window is wide enough for a useful panel (at least 60 columns of panel text) beside
  a screen of at least main's phone size, put the panel on the right. The VC screen shrinks to fit
  the space left, keeping its proportions and crisp scaling. Closing restores the exact previous
  size. This covers ordinary 1440 and 1600 px laptop windows, where the panel now lands below the
  screen and out of view.
- Otherwise, as on phones, put the panel below the screen and the key pad. Size it to the space
  left in the viewport, at least ten lines, and scroll it into view on open.
- Test both layouts with the pure function: on 1440×900 the panel is beside the screen and fully
  visible, and on 390×844 it is below.

## 3. The panel's text (from the browser test)

- Every line now repeats the include site, as in `VCOVL.ASM:3482 → VCSUBS.INC:2806`. Show only the
  file and line the text is on, `VCSUBS.INC:2806`. Put the include chain in the `title` attribute.
- Long lines are cut off at the right. Expand tabs to 8 columns. Let the panel scroll sideways
  inside itself. Never wrap a source line.

## 4. Tapping Source on a phone opens the soft keyboard (major)

`terminal.focus()` runs inside the tap at `web/vc-web.js:87` and `web/vc-source.js:302`, and again in
`close()` at `vc-source.js:281`. Refocus the terminal only if its text area had focus at pointerdown,
and use `focus({preventScroll: true})`. Test with DOM doubles: a tap with the text area unfocused
never focuses it.

## 5. Prologue rows map to the wrong line (minor)

`tools/source_maps.py:192-196` maps every generated `*` row to the last ordinary row. So `push bp`
at `build/gen/VC.OVL.lst:29449-29457` maps to `VCKEYB.INC:197` (`LOCAL AltTime`), and the right
line is `VCKEYB.INC:169` (`Input0 PROC FAR USES ...`). Map `*` rows before a PROC's first ordinary
instruction to that PROC line. Extend `tests/test_source_maps.mjs` to sample generated rows,
including that exact case.

## 6. Interrupt frame words are tested as return addresses (minor)

In the stub case, `runtime/rt.c:624-626` already knows the stack top holds IP, CS and FLAGS, but the
walk starts at `cpu.sp`. Skip the words the stub consumes: three for interrupt stubs, two for the
casemap far call. Test it with a stack whose FLAGS word equals the end of a near CALL.

## 7. Smaller fixes (minor)

- The Ctrl-Shift-F12 latch clears only on an F12 keyup. Also clear it on `blur` and
  `visibilitychange`.
- The open panel rebuilds every node every 100 ms, which wipes any text selection. Skip the render
  when the snapshot has not changed. Use a binary search over Rogue's function table.
- Drop `tabindex="0"` from the panel, so a click in it never takes keys away from VC. Wheel and
  touch scrolling still work.

## Done means

`make test`, `make web`, `make test-web` green, first load at most 1.3 MB gzipped. The summary
lists each fix with its red and green evidence and names three weaknesses of your own work, each
with a real `file:line`. Do not commit.
