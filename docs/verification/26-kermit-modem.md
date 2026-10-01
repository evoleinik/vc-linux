# Brief 26 verification — 2026-10-02

## Status

The implementation, browser build and browser gates pass. The brief's **all
gates green** condition is not met in this sandbox: creating even a localhost
TCP socket returns `EPERM`. The two required real-TCP tests remain enabled and
fail visibly. No live BBS, external network or real browser was used; no commit
was made.

`make -k -j4 test` completed with only these blocked targets:

- `test-modem-transport`: `socket(AF_INET, ...)` fails with “Operation not permitted”.
- `test-e2e`: `test_kermit_dials_bbs_tcp` fails at the fake server's socket
  constructor. The other **231 e2e cases pass**.

The separately named syscall and pipe tests are supplemental evidence, not
substitutes for the TCP gate. A socket-enabled environment must run `make test`
before this brief can be declared done. The actual TCP gate's requested
fault-injection cycle likewise cannot be completed under this restriction.

## Files

- `third_party/mskermit/`: all 32 original archive members, `SOURCES.sha256`,
  `LICENSE`, `licensing.html`, and `UPSTREAM`.
- `tools/kermit.mk`, `tools/build_kermit.py`, `tools/build_kermit_jwasm.py`,
  `tools/kermit-options.inc`, `tools/jwasm/{README.md,kermit.patch,jwasm-a7c6e70.tar.gz}`:
  offline tool/build integration. The pre-existing JWasm executable is untouched.
- `translator/{listing.py,linked.py}`: generic MASM/OMF listing fixes;
  `tests/translator_support/ops_build.py` and `tests/test_translator_ops.py`:
  Kermit instruction-oracle coverage.
- `runtime/modem.[ch]`, `runtime/modem_transport.[ch]`: UART, Hayes, telnet,
  pacing, phonebook, native nonblocking TCP and browser transport bridge.
- `runtime/{rt.c,rt.h,bios.c,dos_core.c,main.c,image.h,web_programs.c,web_programs.h}`:
  IRQ 4, INT 14h, process cleanup, safe TAKE launch, installation and lazy loading.
- `runtime/embed_lzma.[ch]`, `tools/embed.py`: lossless startup packaging;
  `tools/{web_demo.py,web_modules.py}` and `Makefile`: build/publication wiring.
- `data/{BBS.TAK,KERMIT.TXT,VC.EXT}`, `README.md`, `web/README.TXT`:
  dialing, escape/hangup/exit instructions, association and licensing.
- `web/{modem.js,vc-web.js}`: dial-only binary WebSocket, cancellation and cleanup.
- New tests: `tests/test_{kermit_build,translator_kermit,kermit_e2e,embed}.py`,
  `tests/test_{modem,modem_transport,modem_transport_unit,serial_machine}.c`,
  `tests/{serial_pipe_transport.c,fake_bbs.mjs,test_web_modem.mjs}` and the captured
  fixture `tests/fixtures/enigma-connect-2026-10-02.bin`.
- Extended tests: `tests/test_{dos_exec.c,rt_process.c,install_e2e.py,web_source_wiring.mjs}`,
  `tests/{web_smoke,web_assets,web_modules,web_language}.mjs`.

The supplied untracked brief itself was not changed.

## Kermit build and instruction evidence

Both supplied ZIP hashes and their original archive dates are recorded in
`third_party/mskermit/UPSTREAM`. All 32 source-member hashes match the archive;
the original CRLF bytes remain intact. The licensing evidence explicitly says
“As of 20 July 2011” and includes MS-DOS Kermit under the Revised 3-Clause BSD
license. Its footer is dated 27 September 2011. No unavailable Wayback snapshot
timestamp was invented.

`make kermit` produces:

- `KERMIT.EXE`: **228,476 bytes**;
  SHA-256 `a2847133be76c51321ba8dde7fa5aa20bc80852dc206ba91c44b4300e87ad820`.
- 16 assembly modules, **49,272 translated instructions**, 193 generated chunks,
  and 1,037 relocations; no `msn*` C networking modules.
- Two fresh builds with identical EXE, map, objects and listings.
- 9 Kermit build tests and the full **282-test translator suite** pass.
- The instruction oracle passes 103 groups, **190,997 relocation-aware cases ×
  32 states = 6,111,904 successful states**, including 3,446 divide-error cases.

Required JWasm/JWlink settings and fixes:

- `-q -Zm -Cu -Dno_network -Sg -Sa -Sl`, plus the forced options include
  `OPTION NOKEYWORD:<FS GS>` for Kermit's pre-386 ASCII control names.
- MASM 5.1 destination-width truncation for untyped 16-bit-code immediates:
  byte `0FFFFh` and the overflowing low-speed PIT divisors. Typed operands,
  fixups and 32/64-bit code are not relaxed.
- `-Sl` exposes source/macro rows hidden by `.XLIST`/`.SALL` without changing
  instruction bytes. OMF-backed repair replaces JWasm's stale data-listing
  columns around uninitialized structure holes, not source or instructions.
- JWlink `nofarcalls` retains far-call instruction boundaries. `-Cu` resolves
  historical public/external capitalization differences.

The original assembler and every Kermit source file are unchanged. Details and
source examples are in `tools/jwasm/README.md`.

## Old generated C: actual cmp

The seven baseline files were saved before translator changes. After final
regeneration, `cmp build/baseline-gen/NAME.c build/gen/NAME.c` returned zero for
all seven names below. Their SHA-256 values remain:

```text
vc_com            d4d8f2fe893a384ae7c7c6695e650e862f1a894c1f13572b8c929fc302674817
vc_ovl            f0f51d56d84994b90828127a02d55de241a3a64b898dccc932846b51d5fec9b6
gwbasic           8d8cf3225503fa947a969eb089b820ade134d61938765a78a257da7d43c7fe6f
gwbasic_graphics  4aae5dd0c71f389a8a38a02bf8faa8eb35f70999bb52b21b14f63bad3f78b3f6
bootlogo          cbb6ff5d610dfcc97d582aaf17f6cb45aff30a42b5aab4e3038ed712274d0a65
rogue             fb20c57c153b939504cd38762088958e83d4c135baad0eb3064eb36d70bd846f
vz                c9b3d5dbbeb67a4d4d1dc5060fbdfadcbacd0903f73f2b25b168c8a581ee64fc
```

## Runtime and end-to-end gates

| Gate | Result |
|---|---|
| UART/BIOS/Hayes/telnet/pacing/bounds | All seven modem groups pass. |
| Dispatch, PIC EOI/priority, IF/mask, HLT, INT 14h, forced-child cleanup | All five serial-machine cases pass. |
| Native transport syscall controls | Nonblocking socket flags, asynchronous/cancelled DNS, partial I/O, EOF and timeouts pass; no real sockets are claimed here. |
| DOS EXEC/association | 3,212 checks pass, including interpreter identity and fixed-word TAKE arguments. |
| Native BBS pty through test-only pipes | Full captured banner, BBS-only echo, real one-second guards, ATH/NO CARRIER, Ctrl-] C, EXIT and redrawn panels pass. |
| Native real TCP | Blocked by `EPERM`; no skip or automatic pipe substitution. |
| Startup file/codec tests | 47 pass, including compiled native and web round trips for all 24 embedded files and repeated initialization. |
| `make web` | Pass. |
| `make test-web` | Pass, including all existing programs, BBS workflow, module ABI/hash, fetch failure/timeout, and exhausted/fragmented-memory cases. |

The copied 5,433-byte capture has SHA-256
`6f9335122a13585d6f4ba56be96498e5a76e284bb108e639bbdc2dbf2564f8`, identical to
the supplied cache file. The tests assert the exact 33-byte telnet reply stream.
Their echo has a server-only prefix, so local terminal echo cannot fake a pass.

Kermit's own command-line parser appends EXIT unless STAY is on the command
line (`mssker.asm`'s `gcmdlin`). Therefore the association is
`tak: kermit stay, take !.!`; STAY inside the script would not work. The selected
file is passed as a pinned absolute DOS 8.3 spelling so commas cannot become
additional Kermit commands. Other command syntax remains Kermit's responsibility.

## Browser sizes

Measured with the unmodified `tests/web_size.mjs` and Node gzip level 9
(Node 20.18.0 and 25.2.1 agree):

| Artifact | Bytes |
|---|---:|
| Entire first load, separate HTTP gzip payloads | **1,285,396** |
| Unchanged first-load cap | 1,300,000 |
| Remaining first-load margin | **14,604** |
| Main `vc.wasm`, gzip | 1,126,812 |
| `kermit.97860e987e89.wasm`, raw | **7,275,210** |
| Kermit module, gzip | **1,180,370** |

Kermit is the fifth lazily loaded, non-Asyncified side module. No socket opens
before dialing and no module is an eager main dependency. Word-port imports
`port_in16`/`port_out16` are retained along with byte-port imports.

The first-load saving is actual packaging: VC files reuse their linked original
bytes and the remaining files use a bounded fixed-profile LZMA stream. Every
original DOS byte is reconstructed and checked before installation or EXEC
matching; no placeholder, deferred file, removed asset or extra fetch is used.
Native file arrays are unchanged. The decoder's Adler-32 detects corruption,
not authenticity.

All-five-program peak heap: **41,242,192 bytes**; conservative any-load-order
bound: **48,084,546 bytes**. Initial memory is **67,108,864 bytes (64 MiB)**,
with no growth and 19,024,318 bytes above the conservative bound. The genuine
fixed-memory exhaustion/fragmentation tests still return DOS error 8 and recover.

## Deliberate negative controls

Each entry below was run against an isolated source/build copy or process-local
substitution, observed to fail, then followed by the unmodified passing gate.
No production defect remains. The TCP restriction itself is not counted as a
successful negative-control cycle.

| Gate | Planted defect and observed failure |
|---|---|
| Source integrity | Bypass source hashes: stale-source refusal test no longer raises. |
| Reproducibility | Flip the second EXE's final byte: comparison fails at offset 228475. |
| Translation completeness | Drop CODE2 instruction rows: 47,848 rather than 49,272 instructions. |
| Instruction oracle | Corrupt Kermit's IN AL,DX at image offset B2FCh: AX D5B4 versus D5B5 on sample zero. |
| UART | Ignore DLAB on input: divisor-register assertion fails. |
| BIOS | Change the 110-baud divisor from 1047 to 1048: divisor assertion fails. |
| Hayes | Report CONNECT 9600: required CONNECT 14400 assertion fails. |
| Escape guard | Shorten guard to 500 ms: an ordinary `x+++` stream incorrectly escapes. |
| Telnet | Advertise NAWS width 79: exact captured-reply assertion fails. |
| Pacing | Use 300,000 ns per byte: data arrives before its allowed interval. |
| Bounds | Suppress ERROR for an overflowing command: result assertion fails. |
| EOF/redial | Retain stale end-of-call state into a new dial: fresh-call regression fails. |
| IRQ IF/mask | Ignore IF: saved interrupt flags fail. The initial probe also exposed a coupled-mask test; IF and masking are now checked separately. |
| PIC EOI | Clear the wrong in-service IRQ: expected serial interrupt count fails. |
| HLT | Leave the CPU halted on serial delivery: dispatch test times out. |
| INT 14h route | Return 8000h unconditionally: initialization AX assertion fails. |
| Child cleanup | Omit modem reset: MCR/IER reset assertion fails. |
| DOS association | Reject fixed literal arguments as untrusted: DOS-only routing test fails. |
| Native syscall layer | Individually remove nonblocking mode, consume status-peek data, lie about partial sends, resolve on the dispatcher, accept stale DNS, or double timeout: each corresponding test fails. |
| Browser transport | Individually use Blob frames, ignore bufferedAmount, report EOF before final bytes, or accept stale callbacks: each corresponding test fails. |
| Native pipe e2e | Return zero from transport read: no captured markers appear; restored build passes in 8.53 seconds. |
| Web BBS receive | Return no incoming bytes: captured ENiGMA/version wait times out. |
| Web BBS replies | Pretend writes succeeded but discard them: zero replies instead of the exact 33 bytes. |
| Web BBS hangup | Do not close the WebSocket: NO CARRIER appears but the transport-close condition times out. |
| Web publication | Change one digit of Kermit's published hash: main-request/published-file and locateFile checks fail. |
| First-load size | Add a deterministic unused startup payload: 1,321,328 exceeds the unchanged cap; removing it restores 1,285,396. |
| Embedded codec | Independently break stream-header, exact-output/EOS, trailing-data, dictionary-cap, Adler, initial-probability, and generated-checksum checks: all seven intended assertions fail. |

Additional real red-to-green findings included Kermit's missing command-line
STAY, comma-based filename command interpretation, final-payload loss at EOF,
WebSocket CLOSING-state sends, and optional-guide installation with invalid HOME.
The complete native regression run now has only its required socket failure.

The allocation-free modem and codec passed ASan/UBSan. Codec checks included
512 corrupted real streams and 200,000 bounded random streams, plus an
independent dictionary-boundary/Adler review. LeakSanitizer was disabled because
the execution environment's ptrace restriction prevents it from running.

## Three weaknesses

1. `runtime/modem.c:522`: input waits for a free RBR after host scheduling pauses.
   This avoids artificial bursts but does not reproduce an external physical
   UART's overrun behavior; real overrun flags are exercised via loopback.
2. `runtime/modem_transport.c:74`: a single resolver worker bounds resources,
   but a slow abandoned DNS request can delay the next hostname lookup.
   Numeric-address dialing bypasses it; the dispatcher never blocks on DNS.
3. `runtime/dos_core.c:785`: safe TAKE launching rejects some legal DOS
   punctuation retained in short filenames or ancestor aliases, instead of
   attempting Kermit's complicated quoting rules. Such files must be renamed.
