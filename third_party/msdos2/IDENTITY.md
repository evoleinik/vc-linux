# Source builds versus the Microsoft release files

Measured offline on 2026-10-02. Offsets are zero-based file offsets; byte
pairs below are source build / shipped release. All seven executable images
are assembled from the pinned sources, not copied or patched from `bin/`.
`IDENTITY.json` pins the exact hashes, sizes, first differences and bytes;
`make test-msdos2-build` independently rebuilds and recomputes them.

| Program | Built / shipped bytes | First difference | Explanation |
| --- | ---: | --- | --- |
| COMMAND.COM | 17,952 / 15,480 | `0x0001`, `CD / 6D` | Initial near-JMP displacement: source `CONPROC` is at `0CD0h`, release target at `0B70h`. The snapshot is COMMAND 2.11, while the release contains `Command v. 2.02`. |
| EDLIN.COM | 8,158 / 4,389 | `0x0000`, `E9 / EB` | Source's split-module entry jumps near to `0F12h`; the release jumps short to `0111h`. The sources identify revision 14 / `Vers 2.14`, release `Vers 2.00`. |
| DEBUG.COM | 12,149 / 11,764 | `0x0009`, `33 / 31` | Literal version text: source `Vers 2.30`, shipped `Vers 2.10`. |
| FIND.EXE | 5,851 / 5,796 | `0x0002`, `DB / A4` | MZ last-page byte count. JWlink uses a 32-byte header, the release a 512-byte header; payloads are 5,819 / 5,284 bytes. Source is revision 8, shipped revision 6, including source's later 512-byte patch area. |
| MORE.COM | 4,380 / 4,364 | `0x000C`, `FC / EC` | `MOV DX,BADVER` points to `01FCh` versus `01ECh`. Original source defaults select IBM video-width/height initialization, absent in the shipped BIOS-independent version. This is a configuration difference, not evidence of a newer revision. |
| SORT.EXE | 1,152 / 1,216 | `0x0002`, `80 / C2` | MZ last-page count, changed header/layout. Source has grouped code/data, initial COLUMN/SWITCH bytes, international collation and an enlarged initialized stack, unlike the shipped layout. Source records 18 March 1983 fixes. |
| FC.EXE | 2,095 / 2,553 | `0x0002`, `2F / F9` | MZ last-page count. Headers are 32 / 512 bytes; payloads are 2,063 / 2,041 bytes. Source includes later parsing/error-output changes as well as different jump packing; see evidence below. |

## Source and binary evidence

The historical `source/README.txt:116-120` explicitly warns that the newly
split COMMAND modules will not reproduce the earlier release exactly.
`source/COMMAND.ASM:44-50` records the progression through 2.02, international
support in 2.10, and the further module split in 2.11.
`source/UINIT.ASM:20-22` retains the original TeleVideo 2.11 banner and both
TeleVideo and Microsoft copyright notices. `source/COMSW.ASM:5-7` selects
that branch. `source/TCODE2.ASM:342` needs the `Map_call` field defined by
`source/DOSSYM_v211.ASM:673`, not the older `DOSSYM.ASM`. The build aliases
the matching original v211 includes for COMMAND only.

EDLIN records the 14 April 1983 merge/copy fixes and 23 July 1983 split in
`source/EDLIN.ASM:80-90`; its revision bytes are at lines 142-143 and its
version text at line 265. DEBUG documents parity handling and its 2.3
module split at `source/DEBUG.ASM:13-21`, with `Vers 2.30` at line 112.

FIND's revision 7 patch area and revision 8 Kanji changes are documented at
`source/FIND.ASM:37-42`; the `***MAUlloa/Microsoft/V12***8` identifier is at
lines 116-117. The shipped payload begins `MAUlloa/Microsoft/V126`, and
`source/FIND.ASM:896` contributes the 512-byte patch area before messages.
Neither the version string nor this storage is altered to chase identity.

MORE's unedited `IBMVER EQU TRUE` at `source/MORE.ASM:5` includes the BIOS
video probe at lines 27-35. At COM address `0114h`, source code begins
`MOV BYTE PTR MAXROW,25; MOV AH,15; INT 10h`; the release immediately begins
`MOV DX,CRLFTXT`. Both files include the trailing 4,098-byte input buffer
declared by `source/MOREMES.ASM:12`.

SORT's history is at `source/SORT.ASM:9-13`; `internat equ true` at line 19
and grouped segments at lines 49-65 explain its changed data model. The
source begins with a zero COLUMN word and `/` SWITCH byte, entry IP 3;
the release starts with `MOV AH,30h`, entry IP 0, and has three segment
relocations where the source build has none. Its release header declares
1,218 bytes while the supplied file contains 1,216: the comparison uses the
actual preserved file, not inferred zero padding.

FC still uses the same revision-marker text, so that alone cannot date the
changes. Concrete differences exist: `source/FC.ASM:463-464,1401-1402` adds
message-suffix output, `source/FC.ASM:832,868` handles tabs while parsing,
and `source/FC.ASM:1663` avoids a zero-length DOS write. These instructions
are present in the source-built payload and absent in the release. JWasm
also packs short jumps where the release reserves an extra NOP. The first
payload difference is offset `0x14`: the error-message pointer is `06E0h`
instead of `06CCh`, consistent with the changed code/message placement.
These are real source/implementation differences, not a claim of byte or
whole-program semantic identity.

## Tool-side compatibility work

The source files are checked against `SOURCES.sha256` before every build,
and staged include aliases are checked before and after each program's
assembly. No source preprocessing, line-ending rewrite, switch change,
instruction/data-byte patch, or shipped-binary substitution occurs. Explicit
loader-header finalization for SORT is described below and retained for audit.

`tools/build_msdos_jwasm.py` builds a separate `build/msdos-toolchain/jwasm`
from the pinned offline JWasm archive. Neither VC's stock assembler nor
Kermit's separate assembler changes. The build uses `-Zm -Cu -Sg -Sa -Sl`
and the forced `tools/msdos-options.inc`. These adjustments are required:

- `NOKEYWORD` permits original `INVOKE`, `WAIT`, `SYSCALL`, `GOTO`, `ECHO`,
  `NAME` and `FSAVE` names. DEBUG needs IN/OUT as both actual I/O opcodes and
  labels, so contextual keyword handling, also covering TEST/IFDIF/PAGE,
  preserves both uses rather than globally disabling those instructions.
- Old DOSMAC's `&ENDM` and `&.CREF`/`&.XCREF` macro spellings are recognized
  while scanning the original source. `name&%expression` concatenation also
  retains old MASM semantics. Both are covered by standalone old-tool-fails
  regression fixtures.
- Real IF1/IF2 pass conditions replace JWasm's always-true legacy tests,
  and force full subsequent passes instead of reusing preprocessed pass-one
  lines whose condition was different.
  The matching 2.11 macros use IF2 for delayed external declarations; an
  always-true IF2 falsely declares a forward internal label external.
- A mutable relocatable `=` alias is marked current when assigned. Otherwise
  JWasm mistakes `condret`'s freshly assigned `ret_l` for a stale forward
  label, silently encoding `JZ $` (`74FE`) instead of a return branch
  (`74F8` in TCODE3's redirection check). A standalone fixture checks two
  independently calculated backward displacements after separate assignments.
- An empty segment-order declaration can acquire its actual BYTE alignment
  when reopened. Alignment changes after occupying storage still fail.
- Only a parity-marked LF immediately after CR is recognized as a line
  ending in legacy mode. TDATA has four such original `CR 8A` pairs, with
  `8A` at file offsets `09EDh`, `10D4h`, `189Bh`, `2024h`. Literal 8-bit
  characters inside lines are not transformed.
- Existing isolated Kermit tool fixes supply 16-bit MASM constant wrapping
  and `-Sl` (force all listing/macro rows despite `.XLIST`/`.SALL`). OMF-backed
  listing repair changes data byte columns only and preserves instruction
  bytes, relocation markers and source statements. MS-DOS's unnamed code
  classes require the listing parser's all-segments mode for this repair.

JWlink receives `nofarcalls`, preserving listed call encodings. Its direct
COM writer places COMMAND's third group incorrectly after an uninitialized
hole: the map says linear `041D0h`, but direct COM output writes the first
instruction at file `041D0h` instead of `040D0h`. COMMAND therefore follows
the historical LINK/EXE2BIN route: link `COMMAND.MZ`, require no relocations,
CS:IP `0000:0100`, and zero PSP padding, then remove the MZ header and 256-byte
PSP reservation. No instruction bytes are changed. `COMMAND.MZ` and the
verbose map are retained for independent checks. Five other programs use
ordinary JWlink DOS/COM output.

SORT additionally needs bounded initial memory allocation: its original
source requests a separate buffer with AH=48h at `source/SORT.ASM:145-154`
without first shrinking its loaded block. JWlink hardcodes `e_maxalloc=FFFFh`
for DOS, and its MAXData option is Phar Lap-only. The build therefore retains
raw `SORT.MZ` and applies an explicit EXEMOD-style loader-header policy:
`e_maxalloc=1`, matching the shipped maximum. Only bytes `0Ch` and `0Dh`
change; all other header, relocation, code and initialized data bytes remain
identical. The linked `e_minalloc=0` remains valid because the 224-byte stack
is fully initialized by `source/SORT.ASM:58,414`. Invalid MZ headers and a
minimum larger than the configured maximum are rejected. Maximum zero is
not equivalent: the original DOS `EXEC.ASM:328-330` selects load-high for
zero and then allocates the largest block at lines 417-418.

The repeated END start-address directives in EDLIN/DEBUG yield JWlink's
multiple-start warning; all resolve to the same original START symbol.
COMMAND's no-stack warning is expected for the intermediate COM conversion.
Every image, both intermediate MZ files, map, object, listing and comparison report
must reproduce exactly across two fresh build directories.
