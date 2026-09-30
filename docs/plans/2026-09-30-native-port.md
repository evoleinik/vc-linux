# Volkov Commander on Linux, translated from its own assembly

**Type:** Decision doc
**Status:** building
**Date:** 2026-09-30

## Goal

Run Volkov Commander 4.99.09 as a native Linux program in a terminal. It should work on real
Linux files and look and behave exactly like the DOS original. No emulator runs at run time.

## Decision so far

Translate the original 8086 assembly into C by machine. Reimplement the DOS and BIOS services it
calls in C on top of Linux. Eugene picked translation over an emulator on 2026-09-30 ("I just don't
want the emulator. I want to see something cooler").

Options considered in the chat, with Eugene:

| Option | Strongest objection | Fails when |
|---|---|---|
| Run VC.COM in dosemu2 / DOSBox-X / js-dos | It is still DOS inside: a fake drive, no real Linux files | You want to manage real files |
| Emulate the 8086 plus a DOS layer (kvikdos style) | An interpreter runs at run time, which Eugene ruled out | The goal is "no emulator" |
| Hand rewrite in C with ncurses | A new program, drifts from the original; far2l and mc exist | Fidelity to VC matters |
| **Machine translation to C, DOS layer in C** (chosen) | Large: about 39,000 instructions and 90 DOS/BIOS functions | A translator bug hides in rarely run code |

What would change the ranking: if the translation cannot be made exact, a hand rewrite that uses the
translation as its reference becomes the better path.

## Spike result (2026-09-30)

`PutTime` and `BinDec` from `VCSUBS.INC` were translated two ways, instruction by instruction and as
plain C. Both matched the original machine code, run in unicorn, on all 131,072 inputs. A planted
midnight bug went red on the first input.

## Facts that shaped the design

- The sources are BSD-2 since May 2026. `asm/` holds the 4.99.09 tree from the `build` branch of
  github.com/ddanila/vc, which carries small compile fixes.
- JWasm assembles both programs on Linux. `VC.COM` (tiny model, 9,215 bytes) is byte-identical to
  the TASM build. `VC.OVL` (medium model MZ, 111,624 bytes, 1,491 relocations) assembles too.
- The vendored JWasm listing writer overflows a buffer under FORTIFY, so `tools/jwasm/jwasm` is
  built from the same commit without it.
- VC is two programs. `VC.COM` stays resident. It runs `VC.OVL` with DOS EXEC. `VC.OVL` hands
  back a far pointer to its `Init`, splits its own MCB so it stays in memory, and exits. `VC.COM`
  then calls `Init`. To run a user command, `VC.OVL` jumps back into `VC.COM`, which runs
  `COMMAND.COM /C` and calls `Init` again with the saved state.
- `VC.COM` copies some of its own routines to a new offset at run time and calls the copies.
- `VC.OVL` pops its own return address (`Init`), switches stacks, and edits MCB headers directly.
- The code is plain 8086. It has 7 indirect jumps, no self-modifying instructions and 7 port I/O
  instructions. It hooks INT 1Bh, 22h, 23h, 24h and 27h, and INT 21h in the TSR manager.
- File calls go through one wrapper in `VCCOMMON.INC`. It tries the 71xxh long-name call first and
  falls back to the classic call.

## Design

```mermaid
flowchart LR
  ASM[asm/*.ASM] -->|JWasm| IMG[VC.COM, VC.OVL + listings]
  IMG -->|translator: capstone + listing| GEN[build/gen/*.c]
  GEN --> BIN[build/vc]
  RT[runtime: memory, dispatcher, loader, DOS core] --> BIN
  FS[dos_fs.c: files via Linux] --> BIN
  TERM[bios.c + term.c: screen, keys, mouse] --> BIN
```

1. **Keep the machine state faithful.** Registers, flags, a 1 MB memory array, a real 8086 stack,
   a real interrupt vector table, a real MCB chain, real PSPs. VC's tricks then work unchanged.
2. **Translate instructions, never execute bytes.** The translator reads instruction boundaries from
   the JWasm listing, decodes the linked bytes with capstone, and emits C with exact 386 real-mode
   semantics, flags included. Direct jumps become `goto`. Anything dynamic (RET, indirect or far
   transfer, INT) sets CS:IP and returns to one dispatcher loop.
3. **Key code by linear address.** The dispatcher maps CS:IP to an image and an offset. Code that
   VC copied elsewhere is found by matching its bytes against the original image.
4. **Default interrupt handlers are C.** Each vector starts out pointing at a stub address. When the
   dispatcher reaches a stub it calls the C handler, then returns like DOS does (RETF 2). A vector
   VC hooks points at VC's own translated code, so hooks work.
5. **DOS EXEC of `VC.OVL`** loads the translated overlay image. EXEC of anything else runs the
   command through `/bin/sh -c` with the terminal handed back.
6. **Files.** Drive `C:` is `/`. Long names come through the 71xxh functions. Linux names are
   converted from UTF-8 to code page 866 for display and back when opened.
7. **Screen.** The text buffer at B800:0000 is drawn to the terminal with ANSI escapes, only
   changed cells, whenever VC waits for input.

## Gates

- Listing bytes equal linked image bytes for every instruction, and capstone's length equals the
  listing's length. The translator refuses to emit otherwise.
- Every distinct instruction in both images is run in unicorn and in its translated C from random
  states. Registers, defined flags, memory writes and the next CS:IP must match.
- DOS file-layer unit tests against a temporary directory.
- Terminal layer unit tests: key sequences to BIOS codes, screen buffer to escape output.
- End to end: VC starts in a pty on a fixture directory and the rendered panels match.

## Known gaps, accepted for the first build

- Ctrl-O shows VC's saved copy of the screen. It will not show the output of shell commands, which
  go to the terminal's normal screen.
- Holding Shift, Ctrl or Alt only changes the key bar on terminals with the kitty keyboard protocol.
- EMS, XMS and swap-to-disk modes stay off. VC runs in its base-memory mode.

## Questions for Eugene

1. **Drives.** `C:` maps to `/`. Should `H:` map to your home directory as well?
2. **Date and time format.** The panels default to `YYYY-MM-DD` and 24-hour time. Would you
   rather follow your locale or keep VC's US default?
3. **Code page.** 866 shows Cyrillic file names. 437 matches the look of English DOS. Which one?
4. **Publishing.** Should this go to a private GitHub repo now, or a public fork of ddanila/vc once
   it runs?
