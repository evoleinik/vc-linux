# Brief 01: the 8086-to-C translator

Read `docs/plans/2026-09-30-native-port.md` first. It explains the whole project.

## Problem

Volkov Commander is two DOS programs assembled from `asm/`: `VC.COM` (tiny model .COM) and
`VC.OVL` (medium model MZ executable). `make images` builds both into `build/`, together with the
JWasm listings `build/VC.COM.lst` and `build/VC.OVL.lst`.

Write a translator that turns each image into C that does exactly what the machine code does, with
no interpreter. Also write `runtime/cpu.c`. Prove the output right against unicorn.

## Pinned decisions (do not change these)

- `runtime/cpu.h` and `runtime/image.h` are fixed interfaces. Do not edit them. If you believe one
  must change, stop and say why in your summary.
- The CPU model is a 386 in 16-bit real mode, as unicorn emulates it. That covers PUSH SP pushing
  the old SP, shift counts masked to 5 bits, and #DE pushing the address of the faulting DIV.
- Instruction boundaries come from the listing, never from a linear sweep. A listing line in a code
  segment that emits bytes and is an instruction (not DB, DW, DD, DQ, DT or another data
  directive) starts an instruction. Macro expansion lines count. A line holding only a prefix
  (`REP`, `ES:`, `LOCK`) belongs to the instruction after it.
- Bytes come from the linked image, never from the listing. Listing offsets are relative to their
  segment, so first find each segment's base in the image by matching its listing bytes. Skip bytes
  the listing marks as fixups, for example `9A 0000o 0000s`.
- Decode with capstone (`CS_ARCH_X86`, `CS_MODE_16`, detail on).
- Relocations. An MZ image's relocation table lists the words that get the load segment added. When
  an instruction's immediate or far-pointer segment field covers one, emit `(uint16_t)(0xNNNN +
  loadseg)`. If a relocated word lands anywhere else in an instruction, fail the translation.
- Emit code keyed by image offset. Compute any IP value at run time as
  `(uint16_t)(loadseg*16 + target_off - cpu.cs*16)`, because the same code can run under
  different CS values.
- Control flow:
  - A direct near JMP, Jcc, LOOP or CALL whose target is in the same chunk becomes `goto`.
    Otherwise, set `cpu.ip` and `return 0`.
  - CALL pushes the return IP computed as above.
  - Every backward jump and every LOOP/LOOPE/LOOPNE/JCXZ iteration calls `RT_TICK()`.
  - RET, RETF, IRET, indirect JMP/CALL, far JMP/CALL and INT n set `cpu.cs`/`cpu.ip` and
    `return 0`. INT uses `cpu_int(n, ret_ip)`.
  - Divide error calls `cpu_int(0, ip_of_the_div)` and then `return 0`.
  - HLT, and any byte sequence you cannot translate, becomes `rt_fault("...")` naming the image
    offset. Never emit silently wrong code.
- Chunks. Emit one C function per PROC from the listing, or per contiguous run of code between
  PROCs. Each function starts with `switch (off)` that has a case for EVERY instruction start in
  the chunk, because return addresses and table entries can land anywhere. `run()` binary-searches
  a sorted table of instruction starts to find the chunk. It returns -1 for an offset that is not
  an instruction start.
- Flags: compute every flag the 386 defines, exactly. Where a flag is architecturally undefined,
  match what unicorn does if you can. The tests compare only the defined flags.
- Segment defaults: BP-based addressing uses SS, everything else DS. Overrides apply. String-op
  destinations always use ES. REP/REPE/REPNE string ops become C loops with the exact CX, SI, DI,
  DF and ZF behavior. Effective addresses wrap at 16 bits.
- IN/OUT call `port_in8`, `port_in16`, `port_out8`, `port_out16`. LOCK and WAIT are no-ops.

Instruction set seen in the source text: ADC ADD AND CALL CBW CLC CLD CLI CMC CMP CMPSB DEC DIV
IN INC INT IRET Jcc JCXZ JMP LDS LEA LES LODSB LODSW LOOP LOOPE LOOPNE MOV MOVSB MOVSW MUL NEG NOT
OR OUT POP POPF PUSH PUSHF RCL RCR RET RETF ROR SBB SCASB SCASW SHL SHR STC STD STI STOSB STOSW SUB
TEST XCHG XLATB XOR, with REP, REPE and REPNE. Macros may add more. Handle every instruction that
capstone decodes at a listing boundary.

## Shape

- `translator/` Python package, run as `.venv/bin/python -m translator`:
  - `listing.py` parses a JWasm listing: segments, procedures, labels, and every source line with
    its segment offset, byte count, fixup markers and source text.
  - `image.py` loads a .COM or MZ file: bytes, relocations, header fields.
  - `layout.py` finds segment bases. It then checks that every non-fixup listing byte equals the
    image byte at its place, and that capstone's length equals the listing's length for every
    instruction. It fails loudly with the first mismatch otherwise.
  - `emit.py` does instruction semantics to C.
  - `__main__.py` CLI: `python -m translator IMAGE LISTING --name VC.OVL --symbol image_vc_ovl -o OUT.c`.
- `runtime/cpu.c` defines `cpu`, `mem`, `rt_budget`, `flags_get`, `flags_set` and `cpu_int`.
  Nothing else lives there. The runtime provides `port_*`, `rt_yield` and `rt_fault` elsewhere.
  Tests supply their own stubs.
- Makefile: add a `gen` target producing `build/gen/vc_com.c` and `build/gen/vc_ovl.c`. Also add
  a `test-translator` target. Keep the existing `images` target as it is.
- Generated C must compile with `gcc -O1 -Wall` and no errors. Warnings about unused labels are
  fine.

## Gates (all must pass, run them before you finish)

1. **Consistency.** `make gen` runs the layout check on both images and translates both. Report
   how many instructions each image has.
2. **Every instruction against unicorn.** `tests/test_translator_ops.py` (pytest). For every
   distinct (bytes, image offset) instruction in both images, emit its translation as a
   one-instruction test function. Compile them all into one C test library built with
   `-DCPU_TRACE_WRITES`. Run each from at least 32 random starting states in both the C code and
   unicorn (16-bit mode, memory 0..0x110000 mapped), then compare:
   - all registers, including CS:IP after the instruction
   - the defined flags
   - the set of written addresses and their values
   Keep random CX small (0..40) for REP forms. Make segment and index registers produce addresses
   inside the mapped range. Include divisors that trigger #DE. Unicorn does not perform real-mode
   INT dispatch by itself, so give INT, INTO and #DE the reference behavior from the Intel manual,
   written in the test from scratch, not by calling your own `cpu_int`.
   A failure must print the bytes, the disassembly, the starting state and the differing fields.
3. **A whole routine.** `tests/spike_puttime/` holds a proven spike: `PutTime` from `VCSUBS.INC`,
   its unicorn harness and a hand translation. Add `tests/test_translator_puttime.py`. It takes
   `PutTime` and `BinDec` from the translator's output for `VC.OVL` and checks them against the
   original machine code on all 65,536 times in both time formats.
4. **Watch a gate fail.** Change one flag computation in the emitter on purpose (for example, the
   carry flag of SBB), run gate 2, confirm it goes red with a clear message, and restore it. Report
   what you broke and what the failure printed.

## Constraints

- No network access. Use `.venv/bin/python`, which already has capstone, unicorn and pytest.
- Only create or modify: `translator/**`, `runtime/cpu.c`, `tests/test_translator_*.py`,
  `tests/translator_support/**`, `Makefile` (add targets only).
- You cannot commit. Leave the work uncommitted.

## Required summary (end your run with this)

1. Files created or changed, one line each.
2. Instruction counts per image. How many distinct instructions gate 2 tested, and how many random
   states each got. Pass/fail of every gate, with the command to rerun it.
3. The planted-bug result from gate 4.
4. Every deviation from this brief, and why.
5. Three real weaknesses of your work, each with a `file:line` citation.
