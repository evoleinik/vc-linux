# vc-linux: notes for AI coding agents

Volkov Commander 4.99.09, translated from 8086 assembly to C, with DOS and BIOS reimplemented on
Linux. Read `README.md` for what it is and `docs/plans/2026-09-30-native-port.md` for why.

## Commands

    uv sync                 # Python tools: capstone, unicorn, pytest, pyte
    make                    # images -> gen -> build/vc
    make test               # every suite, about 5 minutes
    make test-fs test-term test-ini   # the fast ones, seconds
    .venv/bin/python -m pytest -q tests/test_e2e.py   # VC in a pty, about a minute

Use `.venv/bin/python`, never a global pip.

## Contracts

`runtime/cpu.h`, `runtime/image.h` and `runtime/hle.h` are the contracts between the translator,
the runtime and the DOS/BIOS layer. Change them deliberately, never in passing.

## Rules that cost a bug to learn

- **Never trust a translation without the unicorn gate** (`make test-translator`). It checks every
  distinct instruction against unicorn from random states.
- **Instruction boundaries come from the listing.** Bytes come from the linked image. The
  translator also decodes bytes the CPU can reach that the listing calls data (`_JCXZ`,
  VC.COM's `RESIDENT` banner).
- **Code that VC copies elsewhere** is run by matching its bytes. `rt_code_delta` makes the IPs it
  computes point into the copy.
- **INT 21h 2Bh, 2Dh and 36h report errors without CF** (AL=FFh, AX=FFFFh). VC detects DESQview
  with an impossible date.
- **An empty INT 16h poll must not sleep.** VC polls for Esc during long work. Idle sleeping
  happens in INT 28h and INT 2Fh AX=1680h.
- **Names DOS cannot spell** get a suffix from a hash of their own host name, never from their
  position. That covers characters outside CP866, the characters DOS forbids and trailing spaces
  or dots. A clash is refused, never guessed. Deleting a neighbour must never rename a file.
- **File names never reach a shell as text.** F4 goes through the internal `vc-edit` command,
  which hands `$EDITOR` the resolved host path as one argument. Commands filling DOS's 126 bytes
  are refused, since VC may have cut them.
- **Change `data/VC.INI` with `tools/vcini.py` or by saving from VC (Shift-F9).** A bad checksum
  makes VC ignore the whole file. `ConvCase` must stay off, or VC uppercases 8.3 names it creates.
- **DOS paths on a command line stay under 126 bytes.** Tests use short temporary paths.
- **Ctrl-[, Ctrl-I and Ctrl-M share bytes with Esc, Tab and Enter.** VC tells them apart by scan
  code, so they work only through the kitty protocol or modifyOtherKeys. A raw 08h is Ctrl-H
  unless the tty's VERASE says Backspace sends 08h.

## Debugging

- `VC_TRACE=1` logs every INT 21h call.
- `kill -USR1 <pid>` logs the registers, the dispatch counters and the last 256 dispatch
  addresses.
- `VC_SCREEN_DUMP=file` writes video memory as text.
- A fatal error prints the registers, stack and code bytes, then exits 70.

## Every bug gets a test

Reproduce it in a test that fails, fix it, and show the test failing on the old code. The e2e
helpers in `tests/test_e2e.py` (`select`, `until`, `confirm_until`) wait on screen state, never on
timers. VC zooms its dialogs in, and a key sent on a timer can land before a box can take it.
