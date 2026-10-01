# Listing-directed real-mode translator

`make gen` builds both linked images, validates their listing layouts, and
writes `build/gen/vc_com.c` and `build/gen/vc_ovl.c`. Run the CLI directly with:

```
.venv/bin/python -m translator build/VC.OVL build/gen/VC.OVL.lst \
  --name VC.OVL --symbol image_vc_ovl -o build/gen/vc_ovl.c
```

`make test-translator` runs the instruction differential tests, exhaustive
PutTime comparison, and focused translator/runtime regression tests.

## Linked GW-BASIC modules

`make gwbasic` builds `build/gwbasic/GWBASIC.EXE`, `GWBASIC.MAP`, and every
module listing. `tools/build_gwbasic.py` uses the date pinned in `UPSTREAM`
as `OEMVER`, never the host date; preprocessing comments and tool diagnostic
timings are normalized too. An independent rebuild test compares the EXE,
map and every listing byte-for-byte. The vendored MASM-era `GWBASIC.LNK`
omits the fork's required `OEMCBK` module, so the build adds it before
`OEMEV`. There are 38 linked modules (MATH1 and MATH2 are already concatenated
into MATH), not the brief's approximate 39.

The opt-in `--map build/gwbasic/GWBASIC.MAP` CLI accepts all module listings.
JWlink's `option verbose` supplies the **Module Segments** table: every
module-local offset is placed at that contribution's actual linked address,
including paragraph padding and independently placed data contributions.
The reader requires each module exactly once, checks contribution lengths,
checks displayed literals and resolves symbolic data fixups against the map
and module's local symbols. External symbols retain their declared segment:
GWINIT deliberately declares BEGDSG in CSEG to obtain DSEG's distance from
the code frame, not the usual data-relative offset zero.

Both existing single-listing images take the original path; regenerated
`vc_com.c` and `vc_ovl.c` were compared with their before-work copies using
`cmp`. Identical generated output is not rewritten, preserving object caches.

GW-BASIC's `.SALL` and `.XLIST` suppress bytes even when JWasm is passed
`-Sa`. The build changes only these listing controls to `.LALL` and `.LIST`.
This exposes the old `INS86` macros, whose explicit DB/DW rows encode machine
instructions. Every code-contribution byte must then belong to a source row
or an explicit ORG reservation; missing generated rows are rejected.

Two additional source idioms matter:

- DB-emitted MOV-immediate and accumulator ALU opcodes deliberately skip the
  next source instruction by consuming it as an immediate. A DB segment
  prefix similarly overlaps the following unprefixed source instruction.
  The linked-only reachable-data closure retains both executable starts;
  it never linearly sweeps unrelated data.
- SYNCHR pops its return address into SI, uses CS:CMPSB, and pushes the
  advanced SI. The full fixed machine-code prefix proves that calls return
  **after one inline token byte**. That token is data, not an instruction.

Four MOV AX,0 instructions are patched with the relocated data segment:
OEMCBK's CBKDS/IMDS, OEMEV's ITICDS and OEMSND's IRQ0DS. Two DB-emitted far
JMPs, CBOISR/IMOISR, receive saved BIOS interrupt vectors. Direct CS-relative
stores identify these sixteen operand bytes. Their opcodes and boundaries
remain fixed; emitted MOV/JMP semantics read the proved operand fields from
guest memory. `Image.mutable_offsets` lets the copied-code matcher ignore
only those bytes. A store that changes a translated opcode/boundary fails
translation. No runtime decoder or emulator is introduced.

Linked code yields before an instruction when its budget expires, so the
dispatcher can deliver hardware interrupts. HLT records the halted state and
next IP, then returns to that dispatcher; no translated function sleeps.
The original VC emission is unchanged.

The Unicorn gate now covers every distinct VC and GW-BASIC instruction,
including data-recovered starts, overlapping skip paths and HLT. It also
randomizes all proved mutable fields before each comparison; explicit
mutation tests show that freezing either the DS immediate or a saved ISR
pointer is detected. Arbitrary user-created code (CALL/USR or POKE changing
opcodes) remains outside the ahead-of-time translation contract.

## Necessary JWasm listing normalization

The original `make images` recipes are unchanged. Their listings omit
assembler-generated `PROC USES`/`LOCAL` prologues and `RET` epilogues. The new
gen-only recipes additionally assemble with `-Sg`, retain those listings in
`build/gen/`, and compare the resulting binaries byte-for-byte with the
original images before accepting the listings. Supplying the incomplete
original listing to the CLI fails with an explanation; it does not guess
missing instructions from machine bytes.

Two JWasm source statements still represent multiple machine instructions
even with `-Sg`. Layout recognizes only these exact source-qualified forms:

- A conditional jump expanded as its inverse short condition, displacement
  `03`, followed by `E9 rel16`: boundaries are explicitly at bytes 0 and 2.
- A call to a declared FAR procedure in the same listing segment encoded
  `0E E8 rel16`: boundaries are explicitly at bytes 0 and 1, i.e. `PUSH CS`
  followed by a near `CALL`.

These are source-driven fixed templates, not a disassembly sweep. Capstone
must consume exactly each normalized instruction length. Standalone prefix
lines are joined to the following source instruction. Every other mismatch
is rejected. Additional completeness checks require every nonempty PROC
entry to be an instruction start, and every byte of every code segment to
belong to a source instruction or explicit initialized data/alignment row.

After those source boundaries are checked, layout closes the instruction set
over static successors and the entry point. This recovers executable data such
as VC.COM's `DB 'RESIDENT',10,13` banner and PutTree's `_JCXZ` macro, which emits
two DB rows. Fall-through (including CALL/INT return sites and INT 20h), direct
relative branches/calls, and relocated direct far targets seed a worklist. Each
new instruction adds its own successors until no new starts remain. Unreachable
data stays data. Undecodable bytes and overlaps with listed or recovered
instructions are errors; recovered instructions also validate their relocations.

Targets wrap IP within the listing segment/group's CS frame before converting
back to linked image offsets. This is the nominal source frame, not every
possible runtime CS alias. Indirect transfers and relocated near displacements
are dynamic and cannot seed static targets. Unrelocated far pointers name
physical memory, not an offset from the unknown load base, so they remain
dispatcher transfers.
Reachable bytes still need a source row: this does not replace the independent
check for missing assembler-generated listing rows.

JWasm's instruction fixup markers (`o`, `s`, etc.) are retained and excluded
from pre-link-byte comparison. Its data byte column omits fixup markers, so
the parser separately locates symbolic `DW`/structure initializer fields.
Layout resolves those fields against the listing symbols and the matched
segment/group bases, then checks their linked values; it never accepts a
data mismatch merely because a symbol appears somewhere on the source line.
MASM `?` and `DUP(?)` allocations have no defined initial bytes. Their printed
zero placeholders are not compared: notably, VC.COM overlays those reserved
buffers with real code using `ORG`.

## Boundaries and limitations

The listing parser is deliberately aimed at this JWasm listing dialect and
the two checked-in assembly programs, not arbitrary MASM programs. Unknown
source expansions and unsupported symbolic initializer expressions fail
closed. In particular, symbolic `DUP` arrays are not implemented. Every
displayed defined byte is checked, but JWasm truncates the displayed portion
of long data declarations to eleven bytes; the remaining literal data bytes
are carried from the linked image, not reassembled by the translator.

Segment bases are byte-matched, not assumed paragraph-aligned: VC.OVL's
`Subs` segment starts at image offset `0xbd92` while its DGROUP frame begins
at `0xbd90`. Uninitialized segments have no bytes to match, so only their
data-symbol addresses use the listing map's group-relative memory RVA.
Generated instruction dispatch always uses image offsets, never these
listing-relative addresses.

## CPU-reference details

Both public runtime headers are unchanged. There is one important distinction
between the fixed header's inline word helpers and the required Unicorn
reference: Unicorn first wraps the effective address to 16 bits, then reads or
writes consecutive physical bytes. A word beginning at offset `FFFF` therefore
does not wrap its second byte back to offset zero. Generated code uses its own
small word/stack helpers, and `cpu_int` writes its frame the same way, instead
of using the fixed header's per-byte-wrapping helpers. Unicorn also reads a
RETF frame as one consecutive far pointer, whereas its IRET pops words with
16-bit SP updates between them. Explicit edge-state tests cover these cases.

A direct transfer normally uses a C `goto` inside its listing chunk. A runtime
CS alias can place its target outside that CS's 64-KiB window, however: wrapping
IP then changes the physical target. In that case the generated code sets the
wrapped IP and returns to the dispatcher rather than following the wrong static
label. The same guard applies to instruction fallthrough.

The all-instruction oracle accepts 32 deterministic random states per distinct
instruction, including segment aliases, boundary effective addresses, stack
wraps, REP counts, and divide errors. Writable operands are kept outside the
active Unicorn translation-block pages: VC has no self-modifying instructions,
and Unicorn's early self-invalidation exits do not constitute completed single
steps. Rejected candidate states are replaced, never dropped. Details and
concrete reference-engine failures are in
[`ops_notes.md`](../tests/translator_support/ops_notes.md).
