# Brief 30: translation evidence

The MS-DOS front end is opt-in (`--format msdos --map PROGRAM.MAP`). It
retains listing-directed instruction boundaries and the existing emitter.
Unlike the older flat-COM path, it preserves the separate resident,
transient and EXEC group frames in COMMAND.COM, plus explicit
`OFFSET segment:symbol` initializers. Empty historical segment classes are
accepted without treating data as instructions. Every contribution still
has independent source-byte coverage and linked-byte checks.

SORT's `/R` opcode write is a finite source-proved variant: the original
`JAE rel8` and the literal-written `JB rel8`, with the same displacement.
The source test, writer and both successors are checked before either is
accepted. The runtime guard rejects every other byte template; there is no
runtime opcode decoder.

## Earlier generated C

Before editing the translator, the eight existing outputs were retained in
`build/brief30-baseline/`. After the first complete regeneration and again
after the final COMMAND return-alias assembler fix, these commands all
exited zero:

```sh
for image in vc_com vc_ovl gwbasic gwbasic_graphics bootlogo rogue vz kermit; do
    cmp build/brief30-baseline/$image.c build/gen/$image.c || exit
done
```

Their SHA-256 values were:

```text
d4d8f2fe893a384ae7c7c6695e650e862f1a894c1f13572b8c929fc302674817  vc_com.c
f0f51d56d84994b90828127a02d55de241a3a64b898dccc932846b51d5fec9b6  vc_ovl.c
8d8cf3225503fa947a969eb089b820ade134d61938765a78a257da7d43c7fe6f  gwbasic.c
4aae5dd0c71f389a8a38a02bf8faa8eb35f70999bb52b21b14f63bad3f78b3f6  gwbasic_graphics.c
cbb6ff5d610dfcc97d582aaf17f6cb45aff30a42b5aab4e3038ed712274d0a65  bootlogo.c
fb20c57c153b939504cd38762088958e83d4c135baad0eb3064eb36d70bd846f  rogue.c
c9b3d5dbbeb67a4d4d1dc5060fbdfadcbacd0903f73f2b25b168c8a581ee64fc  vz.c
56810dbeda2586830b008fc741b99630cb7e9be2c591612e325e8822ea04fb85  kermit.c
```

## Focused source/variant negative controls

The following defects were installed in production functions in isolated
Python processes, each running its named pytest gate. The process exit
restores production behavior without touching shared generated artifacts.
These are observed failing executions, not hypothetical failure cases.

| Defect | Test selector in `tests/test_translator_msdos.py` | Observed red |
|---|---|---|
| Force `_expression` to discard explicit segment/group qualifiers | `preserves_explicit` | Linked-byte mismatch at `0x14`: expected `18`, actual `08`, for `OFFSET DSEG:message`. |
| Replace `_verify_module` with a no-op | `rejects_changed_group` | `DID NOT RAISE LayoutError` for a corrupted linked data pointer. |
| In `_InstructionEmitter.emit`, execute the first SORT form for both variants | `only_two_source_proved` | Unicorn sample zero for `JB` differs in IP: C `9ce0`, Unicorn `9ce3`. |
| Reconstruct proof bytes from instruction records before checking the damaged source image | `damaged_source_proof` | `DID NOT RAISE LayoutError`. |
| Bypass the SORT variant byte guard, executing JAE for an injected JE | `guard_refuses` | No required unsupported-template fault; instead C and Unicorn IP differ. |

Each selector failed once in its isolated process. The restored focused run
then passed:

```text
.venv/bin/python -m pytest -q tests/test_translator_msdos.py
8 passed in 0.19s
```

This early focused run covers independently assembled multi-group and
ungrouped COM fixtures, an unclassified EXE, rejected omitted instructions,
linked-data corruption, and both SORT branch templates with 1,024 Unicorn
states each.

## Real-build regressions and further negative controls

The first real utility translation found three additional source dialect
requirements. Each received a regression observed red before its fix:

- FIND's `DW 64 DUP (?,?)` was incorrectly treated as initialized bytes
  outside the MZ file. The uninitialized-only comma list is now recognized.
- Capstone exposes `X86_INS_ENDING` as an enum sentinel, not an instruction.
  Treating that name as an opcode misclassified EDLIN's `ENDING DB 1 DUP (?)`.
- SORT's final `MOV AH,4Ch` / `INT 21h` was followed into message data by
  static closure. Only proved DOS termination pairs without alternate
  labeled/direct entries suppress the nonexistent return path. Emitted CPU
  interrupt semantics are unchanged; an AH=9 fixture still follows its
  return into DB-encoded instructions.

The initial actual-program Unicorn run then caught EDLIN's unsupported DAA
at image `1B02h`, sample zero. Its real source is EDLPROC's binary-to-decimal
line-number conversion. The added focused DAA test also failed before the
emitter fix, then passed 4,096 randomized states.

Additional actually executed process-local defects:

| Defect | Observed red | Restored result |
|---|---|---|
| DAA adds 5 rather than 6 to its low digit | `daa_against`: sample zero AX C=`1019`, Unicorn=`101A`; 1 failed | 1 passed, 33 deselected in 0.09s |
| Drop the last emitted instruction from each MS-DOS layout | `complete_original_source`: all seven counts are one short; 7 failed | 7 passed, 27 deselected in 2.32s |
| Discard the caller-supplied damaged image and reread the pristine file before verification | `corrupted_linked_instruction`: all seven fail with `DID NOT RAISE LayoutError` | Included in the restored 27-test run below |
| Filter all seven MS-DOS programs out of `load_cases()` | `oracle_covers`: all seven fail; missing forms COMMAND 5,422, EDLIN 1,425, DEBUG 3,229, FIND 349, MORE 90, SORT 240, FC 725 | All 34 tests pass after the isolated process exits |

The restored source/boundary/variant tests at this stage passed:

```text
.venv/bin/python -m pytest -q tests/test_translator_msdos.py -k 'not oracle_covers' --tb=short
27 passed, 7 deselected in 3.55s
```

An instruction oracle compares the built machine instructions, not MASM's
intended symbol bindings: it does not establish that an assembler correctly
resolves mutable macro aliases. Those require the separate source/build
regressions.

## Final source-build census

After the mutable return-alias assembler fix, COMMAND.COM is 17,952 bytes
with SHA-256
`46c053a9f51ad18518dfefe0f85c5b6d8e6015998a83f1412764e0cf15210a35`.
The actual layouts and oracle enumeration contain:

| Program | Source-directed instruction starts | Newly distinct oracle forms |
|---|---:|---:|
| COMMAND.COM | 5,445 | 5,422 |
| EDLIN.COM | 1,433 | 1,425 |
| DEBUG.COM | 3,242 | 3,226 |
| FIND.EXE | 351 | 348 |
| MORE.COM | 90 | 89 |
| SORT.EXE | 239 | 240 |
| FC.EXE | 726 | 720 |
| Total | 11,526 | 11,470 |

Oracle forms are deduplicated across programs only when bytes, image
offset, relocation meanings, mutable offsets and permitted variants all
agree. SORT contributes both guarded branch forms. The combined corpus is
202,467 forms (190,997 earlier plus 11,470 new), each tested with 32 states:
6,478,944 randomized instruction executions, plus focused regressions.

The final restored non-coverage tests on those builds passed:

```text
.venv/bin/python -m pytest -q tests/test_translator_msdos.py -k 'not oracle_covers' --tb=short
27 passed, 7 deselected in 3.02s
```

The oracle-inclusion negative control failed all seven program cases in
166.70s. Missing forms can exceed the newly distinct counts above because
the mutation also removes forms shared between the new programs. The
restored complete MS-DOS translator test file then passed:

```text
.venv/bin/python -m pytest -q tests/test_translator_msdos.py --tb=short
34 passed in 78.26s (0:01:18)
```

The final reviewed `make test` aggregate against these source builds passed:

```text
.venv/bin/python -m pytest -q tests/test_translator_*.py
317 passed in 507.85s (0:08:27)
```

That includes every distinct emitted instruction form, all seven new
programs, the guarded SORT variant, and the older programs. The runtime-only
JFT/BREAK review did not change these images, generated C or instruction
semantics; the complete post-review full-project result is recorded in
[runtime verification](brief30-runtime.md).
