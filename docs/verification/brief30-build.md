# Brief 30: original-source build evidence

All work was offline, using the existing `build/msdos/` snapshot. The pinned
commit is `2d04cacc5322951f187bb17e017c12920ac8ebe2`; the upstream repository,
import date, MIT licence and original READMEs are preserved in
`third_party/msdos2/`. Its manifest checks all 57 imported members, including
the seven shipped files used only for comparison. The assembler receives
unedited source bytes: even TDATA's four parity-marked linefeeds survive.

The source build contains 35 separately assembled modules. COMMAND follows
the original COMLINK order, with byte-preserving aliases for the matching
v211 DOSMAC/DOSSYM includes. The six utilities use the older include pair.
The build retains objects, complete listings and verbose maps. It never
modifies the source, copies a release image as output, or patches instruction
or initialized-data bytes. SORT's loader allocation metadata is explicitly
finalized after linking, with the raw linker output retained for comparison.

The exact per-program sizes, SHA-256 hashes, first differing offsets and
bytes are pinned in `third_party/msdos2/IDENTITY.json`. None of these seven
source builds matches its supplied release image. The differing revisions,
MORE's original IBM configuration, changed FC code, and differing linker
headers are documented with concrete source/binary evidence in
`third_party/msdos2/IDENTITY.md`; this is not presented as release identity.

## Green build gate

After the mutable-alias assembler fix, SORT allocation policy and all restored
negative controls:

```text
.venv/bin/python -m pytest -q tests/test_msdos_build.py --tb=short --show-capture=no
25 passed in 2.99s
```

This performs two fresh full builds and compares every executable, the
intermediate COMMAND.MZ and SORT.MZ, all maps, objects, listings and comparison JSON.
The tests also reject changed source even after a previous successful build,
check original COMLINK order and module participation, and verify exact
assembler flags/include bytes. The independent final COMMAND hash is
`46c053a9f51ad18518dfefe0f85c5b6d8e6015998a83f1412764e0cf15210a35`.

## Tool correctness, not merely reproducibility

The pinned isolated JWasm build needs contextual keyword handling, old
escaped macro directives and percent concatenation, actual IF1/IF2 passes,
legacy empty-segment reopening, parity-linefeed reading and current-pass
stamps for mutable relocatable aliases. The original Kermit assembler is
kept unchanged and provides the negative reference for minimal fixtures.
The forced NOKEYWORD include remains separate from every upstream file.

Runtime testing exposed a particularly important assembler defect that an
instruction oracle cannot discover by comparing a translation with the same
bad image. DOSMAC_v211 remembers its latest RET through `ret_l=label`.
JWasm assigned the label's value but did not stamp the current pass; its
branch encoder therefore replaced the correct target with the current IP.
In TCODE3, the branch at `040Bh` became `74FE` (JZ to itself), not `74F8`
(JZ to RET at `0405h`). The new fixture first failed on byte 7, FE versus F8.
The isolated tool now stamps mutable assignments and the fixture checks two
separate assignments against independently calculated JZ/JNZ displacements.
COMMAND's final build has the correct `040B 74F8` and `046F 74F6` branches.
That assembler correction changed only COMMAND's image hash; the other six
images remained identical at that stage.

JWlink's direct COM writer also mishandles COMMAND's third group after its
uninitialized hole. The build uses an MZ/EXE2BIN route, checks CS:IP,
relocation count and zero PSP reservation, and strips only header/padding.
The test checks a code signature at the independently linked MAP address,
including the origin subtraction; malformed intermediate headers are rejected.

SORT exposed a different build-side requirement in the browser gate: it
requests a separate buffer with AH=48h without shrinking its initial block,
but JWlink writes maximum allocation FFFFh unconditionally for DOS. Its
MAXData option is Phar Lap-only. The explicit SORT-only EXEMOD-style policy
sets `e_maxalloc=1`, matching the shipped maximum, while preserving the
linker's minimum zero: the source initializes all 224 bytes of its stack
(`SORT.ASM:58,414`). Zero maximum would select DOS 2's load-high behavior
and still consume the largest block (`build/msdos/v2.0/source/EXEC.ASM:328`).

Raw `SORT.MZ` remains the exact JWlink output, SHA-256
`095c4920dd18e8637257cd84bc3e91a060a4337e840bd8826d37b10fedfef99e`.
Final `SORT.EXE` is still 1,152 bytes, SHA-256
`23b33b636bb67ecec8404e9bda3279533d8aaf1b6acc3e1c348177afa5612cb6`.
The regression independently checks that only offsets 12 and 13 differ,
with every other header, relocation, instruction and data byte unchanged.
Malformed header extents, truncated files and minimum allocation above the
configured maximum are rejected. All other executable images are unchanged.

## Actually executed negative controls

`brief30_build_defects.py` is an opt-in pytest plugin. Each invocation mutates
production functions only inside its own test process and builds in pytest's
temporary directories. Normal pytest behavior is unchanged. Exact commands
and captured failures are in `brief30-build-negative-controls.md`.

| Defect | Observed red |
| --- | --- |
| Return an empty provenance manifest | Expected 57 members, got zero. |
| Bypass source/staging checks for a comment-edited temporary source | `DID NOT RAISE ValueError`; a prior successful build cannot mask the check. |
| Append an output-directory-dependent suffix to a generated object | Reproducibility comparison fails on `rdata.obj`, byte 2414. |
| Add one to the measured first-difference offset | Pinned COMMAND result expects 1, not 2. |
| Swap RUCODE and RDATA in COMMAND's link order | Original COMLINK comparison fails at module index 1. |
| Substitute old DOSSYM for COMMAND's v211 alias | Staging check rejects `source/DOSSYM.ASM (DOSSYM.ASM)`. |
| Repair classless code with the older data-only listing mode | MORE's instruction relocation marker disappears and the marker check fails. |
| Leave 256 extra origin bytes in EXE2BIN output | COMMAND third-group signature at its MAP address is zero instead of code. |
| Use the old assembler for the six original-syntax fixtures | Six failures: macro nesting, concatenation, alignment, parity LF, IF2 external, and contextual labels. |
| Remove the forced NOKEYWORD include | The original-symbol fixture fails to assemble. |
| Use the old assembler for the true-pass fixture | `0102C3` instead of `02C3`. |
| Use the old assembler for reassigned return aliases | `74FE`/`75FE` instead of independently expected `74F8`/`75F8`. |
| Omit SORT's allocation finalization | Header remains `(min,max)=(0,65535)` instead of `(0,1)`. |
| Also change a SORT payload byte during finalization | Changed offsets are `[12,13,32]`, not only `[12,13]`. |
| Bypass SORT MZ-header/minimum validation | The malformed-header gate reports `DID NOT RAISE ValueError`. |

All fifteen invocations exited nonzero (20 failing cases in total), and the
restored 25-test gate above passed. No mutation remained in production code.

## Scope of this evidence

Reproducibility is checked with this host's compiler, not across compiler
versions (`tools/build_msdos_jwasm.py:27`). First-difference explanations are
not a full source-to-release semantic equivalence proof
(`third_party/msdos2/IDENTITY.md:64`). Each finished artifact is atomically
replaced, but publishing the whole multi-file set is not a single filesystem
transaction (`tools/build_msdos.py:224`). Runtime, instruction-oracle and
browser gate evidence is maintained separately.
