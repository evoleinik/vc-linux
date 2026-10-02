# Brief 38: a six-row phone key pad

Read `CLAUDE.md`, `docs/briefs/35-mobile-layout.md` and the key pad code (`web/vc-keypad.js`,
`web/index.html`, `web/vc-layout.js`, `web/vc-web.js`) first. No network, no browser; I test on
phone sizes afterwards. `export WATCOM=$PWD/build/openwatcom`; emsdk as before. Your sandbox
blocks loopback sockets and git writes: run everything else, do not commit.

Eugene approved this layout on a drawn mock on 2026-10-02. It replaces the five-row portrait
pad. Desktop and landscape do not change at all.

## Layout: six columns, six rows

| Row | Keys |
|---|---|
| 1 | F1 F2 F3 F4 F5 F6 |
| 2 | F7 F8 F9 F10 Ins Del |
| 3 | Esc Home End PgUp PgDn Backspace (label `⌫`) |
| 4 | Tab, Grey +, Grey −, ↑, Grey \*, Enter (Enter spans rows 4 and 5) |
| 5 | Shift, Keyboard (an icon button that opens the phone keyboard, with `aria-label`), ←, ↓, → |
| 6 | Ctrl, Alt, Space (spans four columns) |

- Rows stay at least 44 px high with the same 6 px gaps, so the pad is 294 px tall. It stays
  pinned 12 px above the safe-area bottom.
- Colours: Esc, Tab and Enter as today. Ctrl, Alt and Shift as today (violet, filled when
  sticky). Home, End, PgUp and PgDn get a blue border `#378ADD` with text `#B5D4F4`. Grey +, −
  and \* get a teal border `#1D9E75` with text `#9FE1CB`. Everything else as today.
- Keep the Keyboard button's behaviour exactly as today; only its label becomes an icon. Draw
  the icon with inline SVG or a text glyph, nothing loaded from the network.

## Key bytes

Every new key goes through the same input path as a real key, as kitty sequences where
`vc-web.js` already builds them, so sticky modifiers combine with them. Home, End, PgUp, PgDn,
Del, Backspace and Space send exactly what the physical keys send today. Grey +, − and \* send
exactly what the numeric keypad's `NumpadAdd`, `NumpadSubtract` and `NumpadMultiply` send, so VC
selects a group, unselects a group and inverts the selection. Check this against what VC
actually does with them in the browser smoke test.

## Gates

- `tests/test_web_keypad.mjs`: the six-row order, Enter's and Space's spans, each new key's bytes
  with and without sticky Shift, Ctrl and Alt, and the colour classes.
- Every portrait position in the layout tests, the layout fixture and the wiring tests moves by
  exactly the pad's added 50 px; update them and say so. Desktop and landscape fixtures stay
  unchanged.
- The web smoke test: on the portrait pad, Grey + then Enter on the "select" dialog selects
  files in VC's panel, and Space types a space on VC's command line.
- `make test` (apart from the loopback TCP tests), `make web`, `make test-web` green.

## Done means

All gates green. The summary lists files changed and three weaknesses of your own work, each with
a real `file:line`.
