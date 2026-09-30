# vc-linux

Volkov Commander 4.99.09, the DOS file manager, running natively on Linux. The original 8086
assembly is translated to C by machine. The DOS and BIOS services it calls are reimplemented in C
on top of Linux. No emulator runs at run time.

Status: under construction. Design and decisions: `docs/plans/2026-09-30-native-port.md`.

## Build

    uv sync            # Python tools: capstone, unicorn, pytest
    make images        # assemble VC.COM and VC.OVL from asm/ with JWasm
    make gen           # translate both to C in build/gen/
    make               # build build/vc
    make test          # every test suite

## Layout

- `asm/` VC 4.99.09 sources, BSD-2 by Vsevolod V. Volkov, from the ddanila/vc build branch.
- `translator/` Python: JWasm listing + linked image to C.
- `runtime/` C: machine state, dispatcher, loader, DOS and BIOS services, terminal.
- `data/` default VC.INI, VC.EXT and VC.HLP.
- `tests/` translator, DOS, terminal and end-to-end tests. `tests/spike_puttime/` is the first proof.

## Notes for AI agents

- `runtime/cpu.h`, `runtime/image.h` and `runtime/hle.h` are the contracts between the translator,
  the runtime and the DOS/BIOS layer. Change them deliberately, never in passing.
- Never trust a translation without the unicorn gate: every distinct instruction in both images
  is checked against unicorn from random states.
- Use `.venv/bin/python`, never a global pip.
