# Listing-directed real-mode translator

`make gen` builds both linked images, validates their listing layouts, and
writes `build/gen/vc_com.c` and `build/gen/vc_ovl.c`. Run the CLI directly with:

```
.venv/bin/python -m translator build/VC.OVL build/gen/VC.OVL.lst \
  --name VC.OVL --symbol image_vc_ovl -o build/gen/vc_ovl.c
```

`make test-translator` runs the instruction differential tests, exhaustive
PutTime comparison, and focused translator/runtime regression tests.

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
