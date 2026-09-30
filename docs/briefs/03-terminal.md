# Brief 03: screen, keyboard and mouse

Read `docs/plans/2026-09-30-native-port.md` first. It explains the whole project.

## Problem

Volkov Commander draws by writing straight into the text screen at B800:0000. It reads keys
through INT 16h and the BIOS data area, and uses the mouse through INT 33h. Implement the video,
keyboard and mouse BIOS, the DOS console functions and a terminal front end that shows VC in a
Linux terminal. Put it in `runtime/bios.c` (BIOS and console) and `runtime/term.c` (the terminal).

## Pinned decisions (do not change these)

- `runtime/cpu.h` and `runtime/hle.h` are fixed interfaces. Implement `bios_init`, `bios_int10`,
  `bios_int16`, `bios_int33`, `dos_con_int21`, `con_write` and all the `term_*` functions exactly
  as `hle.h` declares them. Do not edit either header. Put extra prototypes in `runtime/bios.h` or
  `runtime/term.h`.
- Calling convention. Each handler reads registers from `cpu` and memory from `mem`, and writes
  results back to `cpu`. `dos_con_int21` returns 1 if it handled cpu.a.h, else 0 without touching
  `cpu`.
- The BIOS data area is real memory at 0040:0000, and VC reads it directly. `bios_init` sets:
  - equipment word 0x410 for 80x25 colour
  - video mode 3 at 0x449, 80 columns at 0x44A, regen size at 0x44C, page offset 0 at 0x44E
  - cursor positions at 0x450, cursor shape at 0x460, active page at 0x462, CRTC port 3D4h at 0x463
  - rows minus one at 0x484, character height 16 at 0x485, EGA/VGA info at 0x487 and 0x489
  - keyboard flags at 0x417 and 0x418
  - keyboard buffer head and tail at 0x41A and 0x41C, ring 0x41E-0x43D, start and end at 0x480
    and 0x482
  The runtime keeps the timer at 0x46C, so do not touch it.
- Keys live in the real BIOS ring buffer. `term_idle` parses terminal input and pushes BIOS key
  words (scan code high, ASCII low) into it. INT 16h reads from it. A full buffer drops keys.
- Screen. Text mode only, cells at linear 0xB8000. Rows come from 0x484 plus 1 and columns from
  0x44A. INT 10h AX=1112h gives 50 rows at character height 8, 1111h gives 28 rows at height 14,
  1114h and mode 3 give 25 rows at height 16. INT 10h 1130h reports CX = character height and
  DL = rows - 1.
- Rendering. Keep a shadow copy of the last frame and send only changed cells. Bytes 00h-1Fh and
  7Fh use the CP437 glyphs (☺ ► ◄ ↑ ↓ ▲ ▼ ⌂ and so on). Bytes 80h-FFh use CP866, so box drawing
  and Cyrillic both work. Get the table from `runtime/cp866.c` (`cp866_to_ucs`). Another worker
  writes that file in parallel, so if it is missing, add your own `static` table in term.c and say
  so. Colours: attribute low nibble = foreground, bits 4-6 = background, bit 7 = blink, or a
  bright background after INT 10h AX=1003h BL=0. VC relies on bright backgrounds, so default to
  bright backgrounds with blink off. Use 24-bit colour with the exact VGA palette (brown is
  #AA5500) when `$COLORTERM` says truecolor or 24bit. Otherwise use the 16 ANSI colours with the
  VGA-to-ANSI index mapping. The cursor position comes from 0x450, and cursor shape bit 5 of the
  start line hides it.
- Terminal. Enter the alternate screen, raw mode, application keypad mode (so the grey + - * keys
  send `ESC O k/m/j`) and SGR mouse mode 1002 + 1006. Undo all of it on `term_shutdown`,
  `term_suspend`, `atexit` and fatal signals. Redraw everything on SIGWINCH and on `term_resume`.
  Use the kitty keyboard protocol (flags 1|2|8) only if the terminal answers the `CSI ? u` query.
  When it is on, keep the Shift, Ctrl and Alt bits at 0x417 and 0x418 current from press and
  release events.
- Key mapping to BIOS words, as INT 16h AH=10h reports them:
  - printable ASCII
  - UTF-8 that maps to CP866, with scan code 0
  - Enter 1C0Dh, keypad Enter E00Dh, Ctrl-Enter 1C0Ah, Esc 011Bh, Backspace 0E08h, Tab 0F09h,
    Shift-Tab 0F00h
  - arrows, Home, End, PgUp, PgDn, Ins and Del with AL=E0h
  - F1-F12, and their Shift, Ctrl and Alt forms
  - Ctrl and Alt with letters and digits
  - Ctrl and Alt with arrows, Home, End, PgUp and PgDn
  - grey + - * as 4E2Bh 4A2Dh 372Ah
  Parse the xterm forms, both `CSI 1;mod X` and `CSI n;mod ~`, plus SS3, plus lone-ESC-then-key
  as Alt. Decide a lone Esc after a 30 ms quiet gap. INT 16h functions 00h/01h/02h are the legacy
  forms: they turn AL=E0h into 00h and hide keys that only exist in the extended set, as a real
  BIOS does.
- INT 10h: 00h, 01h, 02h, 03h, 05h (page 0 only), 06h, 07h, 08h, 09h, 0Ah, 0Eh, 0Fh, 1003h (other
  10xxh subfunctions are no-ops), 1111h, 1112h, 1114h, 1130h, 12h BL=10h, 1A00h, FEh and FFh
  (TopView: leave ES:DI unchanged). 4Fxxh VESA reports "not supported": AL unchanged from 4Fh is
  success, so return AH=01h. Unknown functions are no-ops.
- INT 16h: 00h 01h 02h 05h 10h 11h 12h. Anything else is a no-op. When 01h/11h finds the buffer
  empty, call `term_idle(10)` first, so VC's polling loop does not spin the CPU. 00h/10h loop on
  `term_idle(50)` until a key arrives.
- INT 33h: 00h and 21h reset (AX=FFFFh, BX=2), 01h, 02h, 03h (CX = column*8, DX = row*8),
  04h, 05h, 06h, 07h, 08h, 0Ah (ignore), 0Bh. 0Ch and 14h (event handlers) do nothing and say so
  in a comment. While the show counter is 0 or more, render the mouse cell with its attribute
  inverted. The shadow copy must not keep that inversion.
- Console INT 21h: 01h 02h 06h 07h 08h 09h 0Ah 0Bh 0Ch. Extended keys come back as 00h, then the
  scan code on the next call. 0Ah supports Backspace, Esc (clear) and Enter.
- `con_write`: teletype at the BIOS cursor of page 0. CR, LF, BS and BEL work, new lines use
  attribute 07h, and it scrolls at the bottom.

## Gates (all must pass, run them before you finish)

1. `tests/test_term.c`, built and run by a new `make test-term` target. It defines its own
   `Cpu cpu; uint8_t mem[MEM_SIZE];` plus stubs for anything else it needs. It must NOT link
   `runtime/cpu.c` or `runtime/dos_fs.c`, which other workers write in parallel. It checks:
   - the key parser, byte strings to BIOS words, including split sequences across reads and the
     lone-Esc timeout, for every key class above
   - the legacy versus extended INT 16h forms
   - the renderer: a known screen buffer gives the expected escape output, and a second frame
     sends only changed cells
   - scroll-up and scroll-down windows
   - teletype wrap and scroll
   - the 50-row font switch updating 0x484 and 0x485
   - INT 33h position scaling
2. **Watch a gate fail.** Swap two colour indexes in the palette map on purpose, confirm the
   renderer test goes red with a clear message, and restore it. Report what you broke.

## Constraints

- No network access. Plain C11 with glibc and termios, no ncurses and no new libraries.
- Only create or modify: `runtime/bios.c`, `runtime/bios.h`, `runtime/term.c`, `runtime/term.h`,
  `tests/test_term.c`, `Makefile` (add targets only).
- You cannot commit. Leave the work uncommitted.

## Required summary (end your run with this)

1. Files created or changed, one line each.
2. Every BIOS and console function implemented, and which ones are no-ops.
3. Test count and results, with the command to rerun them. The planted-bug result.
4. Every deviation from this brief, and why.
5. Three real weaknesses of your work, each with a `file:line` citation.
