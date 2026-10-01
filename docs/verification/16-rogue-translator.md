# Brief 16: compiled-C translator verification

Date: 2026-10-01. This records the translator subtask and its observed runs.
The historical runs below predate the final gold/score/PDCurses-cell port
changes. The final canonical aggregate subsequently passed 261 translator
tests in 416.93s, including the restored coverage helper and both review
regressions. See [the aggregate record](16-rogue.md) for the final image,
complete native/browser results and the separate unresolved CI prerequisite.

## Translation path and executable roundtrip evidence

`make rogue` retains the real Watcom DOS executable, verbose link map, game
objects and PDCurses library. Translation uses:

```sh
.venv/bin/python -m translator build/rogue/ROGUE.EXE --format watcom \
  --map build/rogue/ROGUE.MAP --name ROGUE.EXE --symbol image_rogue \
  -o build/gen/rogue.c
```

This is the map-driven path permitted by the brief, with object evidence rather
than an executable sweep. `translator/omf.py` reads original OMF records,
including selected Watcom runtime members extracted directly from their
libraries. `translator/compiled.py` matches the map's segment contributions,
group frames and publics to those objects; resolves each linker fixup; compares
every initialized byte with the linked image; and requires both the complete
MZ relocation table and the exact entry CS:IP to agree. Fixup bytes are checked,
not ignored. Missing members, unsupported OMF forms and unsupported instruction
semantics fail the build.

WDIS supplies instruction boundaries from each original object. Every listing
row must match the object's bytes, and every initialized CODE byte must have a
row. Classifying a CODE row as data additionally requires an original OMF
`COMENT FD/s` data-in-code range or a complete scalar fixup field. A forged
data row cannot conceal a callback instruction merely by preserving its bytes.
Capstone checks the final linked instruction bytes and lengths. The emitted C
uses the ordinary instruction emitter and dispatch mechanism; there is no
runtime x86 decoder or Rogue interpreter. Compiled-only chunks contain at most
64 instructions to bound C compiler resource use. Existing assembler frontends
and their generated C are unchanged.

The alternative WDIS-to-JWasm route was tested, not dismissed on assumption:

```sh
WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom \
  .venv/bin/python -m pytest -q tests/test_translator_compiled.py \
  -k wdis_jwasm_jwlink_roundtrip
```

That test compiles an actual large-model 8086 C probe with `wcc`, disassembles it
with `wdis -a`, assembles it with JWasm `-Cp -Zg -Sg`, and links both original
and reassembled objects into DOS executables. JWlink cannot read this supplied
Watcom library's dictionary attribute, so the test passes every selected CRT
member as its original extracted object. This bypasses that separate archive
compatibility issue without changing the runtime code. The resulting executable
bytes still differ at unrelocated instruction bytes:

| Image offset | Instruction | Watcom | WDIS/JWasm/JWlink |
|---|---|---|---|
| `0x0c` | `MOV DX,AX` | `89 c2` | `8b d0` |
| `0x12` | `SUB AX,DX` | `29 d0` | `2b c2` |

The printed assembly loses the opcode-direction encoding choice. Correcting
that roundtrip would require the original object-byte/boundary evidence that
the chosen frontend already checks directly. This executable test passed as
part of both successful collections recorded below.

## Indirect entries and self-modification gate

Procedure entries, direct branches, object code-pointer fixups (including
local symbols absent from the map), and their reachable successors must all
have translated instruction starts. Tests explicitly cover Rogue's daemon,
option and item-description callbacks.

Watcom's `_DoINTR_` uses a computed RETF into a `3 * intno` table, not a patched
INT operand. The frontend proves its exact arithmetic template and all 256
three-byte slots, including the INT 3 spelling and DOS 25h/26h jump thunks.
Missing slots fail translation before gameplay. Writes through CS that can
affect code require an explicit proof; unsupported fixed/indexed code writes
fail closed. The exact `_DoINTR_` recognition and deliberately limited 16-bit
OMF parser remain narrow, documented compatibility points, not generic claims
about arbitrary compiled DOS programs.

Two review probes strengthened the original gate:

- Changing MZ CS:IP to valid translated procedure `wear_` at `0000:0000`
  originally passed byte checks. The added exact map-entry comparison rejects it.
- Reformatting the indirect-only `doctor_` entry as a WDIS DATA row, without
  changing bytes or coverage, originally passed. Original OMF data provenance
  now rejects it with `CODE data lacks OMF`.

Both regression tests were observed red before their production checks were
added, and green in the 16-test post-review run below.

## Unicorn coverage and the valid INT 6 adapter

`tests/translator_support/ops_build.py` enumerates Rogue alongside the existing
images. Every distinct linked instruction/offset/relocation-meaning case gets
32 accepted deterministic states; none of the Rogue instruction families or
CRT/PDCurses objects is sampled out. A separate coverage test requires every
instruction in Rogue's compiled layout to appear in that enumeration.

The first complete Rogue oracle run failed at valid `CD 06` in `_DoINTR_`:

```text
ROGUE.EXE image offset=0x1955f loadseg=0x1666 sample=0 seed=0xc38fde89
CS:IP=1ff5:fc6f SS:SP=5c16:0000 FLAGS=31d2
UC_ERR_INSN_INVALID
93 passed, 1 failed
```

A standalone probe of all 256 vectors reproduced the problem only for INT 6.
Unicorn 2.1.4 sends that valid software interrupt to `UC_HOOK_INSN_INVALID`,
with unchanged IP, instead of its normal interrupt callback. The narrow adapter
in `ops_harness.c` verifies the actual fetched `CD 06`, its vector/length
metadata and unchanged IP before invoking the same independent Intel
interrupt-entry oracle already used for every other vector. It does not call
the generated CPU helper, omit any state, or relax any register/flag/write
comparison. A real invalid opcode still fails.

Focused tests cover all 256 vectors, deliberately corrupted INT 6 CF, and a
real UD2 falsely labeled as INT 6. The all-vector and corrupted-CF checks were
observed failing before the adapter (`2 failed, 94 deselected in 27.80s`) and
passing afterward (`2 passed, 94 deselected in 27.25s`). The later full green
run also includes the UD2 rejection test. The oracle's existing restrictions
on undefined flags and active-code writes remain documented in
`tests/translator_support/ops_notes.md`; 32 states are not an exhaustive CPU
state-space proof.

## Observed successful runs and their scope

The pre-final-port executable was 202,382 bytes, SHA-256:

```text
fa2aca4c68edefe983d13bb2323aaf85f1a061a3fa0f936ff2d285455aca3553
```

Its layout contained 53,285 translated instructions, 833 chunks, 4,038 MZ
relocations and 1,421 expected indirect/procedure entries, including all 256
`_DoINTR_` slots. Across all images, the oracle enumerated 117,138 distinct
cases, for 3,748,416 successful randomized instruction-state comparisons in
the complete passing run:

```sh
WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom make test-translator
```

```text
259 passed in 413.26s
```

That collection predates the two review regressions described above. After
adding their checks, the focused compiled-C collection passed:

```sh
WATCOM=/home/eo/src/vc-linux-wt/tools-cache/openwatcom \
  .venv/bin/python -m pytest -q tests/test_translator_compiled.py \
  -k 'not unicorn_gate' --tb=short
```

```text
16 passed, 1 deselected in 8.36s
```

All five established generated C files were also compared with the preserved
baseline using `cmp`, each exiting zero: `vc_com.c`, `vc_ovl.c`, `gwbasic.c`,
`bootlogo.c`, and `gwbasic_graphics.c` under `build/gen` versus
`build/rogue-baseline`. `git diff --check` passed after restoration and the
documentation edits.

These translator runs were captured in the agent's tool transcript, not
redirected to persistent full logfiles. The excerpts here are the durable
record; there is no claimed `build/...log` for those runs. The build subtask's
separate captured logs are listed in `16-rogue-build.md`. Final aggregate logs
and final-image totals belong in the aggregate verification record.

## Three deliberate production defects and recovery

Each defect was actually applied to production source with `apply_patch`, its
gate was run, and the source was restored in a `finally` cleanup. No fault
remains. With the same `WATCOM` environment as above, the commands were:

```sh
.venv/bin/python -m pytest -q tests/test_translator_compiled.py \
  -k linked_byte_corruption --tb=short
.venv/bin/python -m pytest -q tests/test_translator_compiled.py \
  -k compiled_dointr --tb=short
.venv/bin/python -m pytest -q tests/test_translator_compiled.py \
  -k unicorn_gate --tb=short
```

| Planted production defect | Observed red result | Observed recovery evidence |
|---|---|---|
| Add `False and` to the initialized-byte comparison in `compiled.py` | Both opcode and resolved-fixup tests fail: `DID NOT RAISE LayoutError`; `2 failed, 13 deselected in 0.29s` | Restore comparison; both tests pass in the subsequent 16-test post-review run |
| Remove the `_DoINTR_` INT 16h record after sorting the real compiled instruction list | Layout creation raises `compiled indirect/procedure target 0x1958f lacks a translated instruction`; `14 deselected, 1 error in 3.25s` | Restore instruction enumeration; the DoINTR test passes in the subsequent 16-test post-review run |
| Remove `ROGUE.EXE` from the real `load_cases()` image tuple | `AssertionError: 53237 Rogue instructions missing from Unicorn gate`; `1 failed, 16 deselected in 45.03s` | Restore the helper byte-for-byte; final canonical aggregate passes all 261 translator tests, including complete Rogue coverage |

Before and after the final coverage mutation, the restored production files
had identical SHA-256 values:

```text
translator/compiled.py
fbb75f92250c1ddfd0c08fa723a23ff6911cb6fb1f98427f00223145f15fadb9
tests/translator_support/ops_build.py
5a921aae099bbb27b2fc31bff5910fde221a79b3590f3776724a572a0f31320f
```

These are restoration-time hashes, not a promise that later harmless comments
leave the same hashes: a `compiled.py` docstring cleanup followed. The third
mutation's restored helper was not retested immediately because port changes
were in progress and the final aggregate owner requested that generation and
tests pause. Exact restoration plus the earlier green run is evidence of
recovery, but is not misreported here as a newly observed post-restoration
green run. The later canonical aggregate run now supplies that result:
`261 passed in 416.93s`, captured in
`build/rogue-verification/make-test-final.txt`.
