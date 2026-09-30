# PutTime spike (2026-09-30)

The first proof that translation works. `harness.asm` holds VC's `PutTime` and `BinDec` copied
verbatim. `puttime_mech.c` is an instruction-by-instruction translation and `puttime.c` a plain C
rewrite. `check.py` runs the original machine code in unicorn and compares both on all 131,072
inputs.

    ../../tools/jwasm/jwasm -q -Zg -Zne -bin -Fo harness.com harness.asm
    gcc -O2 -shared -fPIC -o libpt.so glue.c puttime_mech.c puttime.c
    ../../.venv/bin/python check.py

It uses its own tiny `x86.h`, not `runtime/cpu.h`. It is kept as a reference, not as the design.
