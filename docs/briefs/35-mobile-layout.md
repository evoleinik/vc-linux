# Brief 35: phone layout with an edge-to-edge screen and a key pad at the bottom

Read `CLAUDE.md`, `docs/plans/2026-10-01-mobile.md` and the key pad code (`web/vc-keypad.js`,
`web/index.html`, `web/vc-layout.js`, `web/vc-web.js`) first. No network, no browser; I test on
real phone sizes afterwards. `export WATCOM=$PWD/build/openwatcom`; emsdk as before. Your sandbox
blocks loopback sockets and git writes: run everything else, do not commit.

Eugene approved this layout on a drawn mock on 2026-10-02. It applies only when the page shows
the key pad today, `(pointer: coarse) and (orientation: portrait)`. Desktop and landscape do not
change at all, pixel for pixel.

## 1. The screen uses the full width

- No left or right margin or padding around the VC screen. On a 390-px-wide phone the screen is
  390 px wide and 244 px tall (80×25 at the 640×400 aspect), instead of today's 360×225. Use the
  full viewport width at any phone width, keeping the aspect and crisp text.
- Respect the safe-area insets (`env(safe-area-inset-*)`) so nothing sits under a notch or the
  home indicator. Add `viewport-fit=cover` to the viewport meta if needed for that.
- The screen sits at the top. The page footer text sits directly under it.

## 2. The key pad is pinned to the bottom

- The key pad sits at the bottom of the viewport, like a BlackBerry's keyboard, with the space
  between the footer and the pad left empty. The page never scrolls in portrait.
- Layout, five columns:
  - Row 1: F1 F2 F3 F4 F5
  - Row 2: F6 F7 F8 F9 F10 (labels are just "F1" to "F10")
  - Row 3: Esc, Tab, ↑, Ins, Enter (Enter spans rows 3 and 4)
  - Row 4: Ctrl, ←, ↓, →
  - Row 5: Shift, Alt, Keyboard (Keyboard spans three columns)
- Buttons stay at least 44 px high, and touch behaviour stays exactly as today (no zoom, no
  selection, no focus changes, sticky modifiers).
- When the phone's own keyboard opens, the page must still work: use the visual viewport so the
  pad is not hidden behind it, or hide the pad while the phone keyboard is open. Pick one, say
  which.

## 3. Colour coding

- Esc: dark red fill `#5a1616`, border `#e24b4a`, text `#ffd6d6`.
- Tab: dark amber fill `#4a3306`, border `#ba7517`, text `#ffe2b0`.
- Enter: dark green fill `#173d0a`, border `#639922`, text `#d8f0b8`, twice the height.
- Ctrl, Alt, Shift: violet border `#7f77dd`, text `#cecbf6`; when sticky they fill violet
  `#534AB7` with white text, as `aria-pressed="true"` shows today.
- Everything else as today.

## Gates

- The pure layout function from brief 25 (`vc-layout.js`) gives the new portrait geometry: at
  390×844 the screen box is 390×244 at the top and the pad's bottom edge sits at the viewport's
  bottom minus the safe area; at 360×740 and 430×932 the screen is the full width. The existing
  "closed page is identical to main" checks still pass for every non-portrait-touch size, so
  update only the portrait-touch fixture.
- `tests/test_web_keypad.mjs`: the five-row order, Enter's span, the colour classes, and every
  key's bytes unchanged.
- `make test` (apart from the loopback TCP tests), `make web`, `make test-web` green.

## Done means

All gates green. The summary lists files changed and three weaknesses of your own work, each
with a real `file:line`.
