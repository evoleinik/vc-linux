# DOS build mutation evidence — 2026-10-02

Before brief 37, the 243,304-byte image remained unchanged throughout this run:
`cd106c4ece392a9da9d1940f441c2ba5afccdabc5d46cf95167fd42169cc49f9`.
The machine defects below modify only the private Unicorn memory image used by
the gate. Source defects modify private copies or the probe's Python binding.
Pytest restores every binding between tests; no vendored or production file is
modified. The probe fixture is not part of normal test discovery.

Reproduce the deliberate failures, then remove the defects:

```sh
HACK_MUTANT=1 .venv/bin/python -m pytest -q tests/fixtures/hack-build-mutations.py
HACK_MUTANT=0 .venv/bin/python -m pytest -q tests/fixtures/hack-build-mutations.py
WATCOM=$PWD/build/openwatcom .venv/bin/python -m pytest -q tests/test_hack_build.py
```

Observed in order: **10 failed**, exit 1; **10 passed**, exit 0; production build
suite **37 passed**, exit 0. The failing assertions were the actual gate
assertions, not fabricated exceptions.

| Gate | Planted defect | Observed red result |
| --- | --- | --- |
| Full-width random sequence | Replace `random_` with `xor dx,dx; retf` | `[0,0,0,0,0]` instead of the five known BSD outputs |
| IBM wall glyph | Replace `hack_mapchar_` with `mov ax,bx; retf` | ASCII `45` instead of CP437 `196` |
| Saved monster segments | Return without rebasing the saved species pointer | Kept old segment instead of adding the tested `0x500` displacement |
| Saved pointers/callbacks | Return immediately from `hack_restore_you_` | Stale sickness pointer survives instead of being cleared/reconstructed |
| Corrupt save rejection | Make `uptodate_` always return 1 | A body-byte mutation is wrongly accepted |
| Pinned executable | Flip the final byte without changing image length | `verified HACK.EXE SHA-256 mismatch` |
| Exact upstream bytes | Append a line to the private copy of `data` | Complete source digest differs from the pinned snapshot |
| Counted build patches | Replace the probe's preparation function with a no-op | Duplicate entry-point span: `DID NOT RAISE ValueError` |
| Compiler clock guard | Same no-op preparation function | Added `__TIME__`: `DID NOT RAISE ValueError` |
| Parent drive/directory state | Return immediately from the registered exit callback | Active drive stays H: (8) instead of returning to C: (3) |

There was also a genuine red/green build regression during implementation:
the first independent-directory rebuild differed by 20 bytes because upstream
`assert()` embedded an absolute `__FILE__`. The existing reproducibility
assertion caught it. Compiling stable `src/...` and `shim/...` relative names
removed the path dependency; independent-directory EXEs now compare exactly.

The first pty quit gate also found that DOS directory changes leaked to VC's
parent state. `startup.c` now registers an exit callback that restores both
changed drive directories and the original active drive. The compiled routine
test checks both same-drive and cross-drive launches; its mutation fails at the
actual caller-drive assertion. The pty suite exercises Q and S through CRT exit.

A second, deterministic pty regression exposed numbered 8.3 directory aliases
changing when a sibling directory was added while Hack ran. All four Q/S by
COMSPEC/INT 2Eh cases failed against the short-path image; the captured run is
`build/hack-cwd-alias-regression-red.txt`. The DOS shim now captures/restores
long paths with INT 21h AX=7147h/713Bh and falls back only when the service is
unavailable (AX=7100h or 1). The production routine tests cover both active-drive
cases under all three service states, reject fallback on genuine path errors,
and preserve a full 260-byte LFN response plus its drive prefix without overflow.

Independent review then caught a classic-DOS ABI regression: Watcom's `int86x`
does not load the input carry flag from `REGS.cflag`. Both LFN calls entered the
real interrupt with carry clear, so an old DOS handler that returned AX=7100h
without changing carry was mistaken for success. A private rebuild reproduced
the previous image exactly (`cef83579cab4dafa7f53ebc6554fed9767de1308dac768d7ba110d4ae72bfae9`).
All four new capture/restore by AX=7100h/1 tests failed their actual incoming-carry
assertion on that image. The final shim uses the documented `intrf`/`REGPACK`
interface with `INTR_CF`; those same four tests now pass. Review of the linked
routine confirmed `sahf` sets carry before the interrupt and preserves the
caller's segment registers and stack frame.

The production tests separately inject missing/duplicate source spans and clock
macros, and check six integrity cases: valid, changed body, changed length,
changed format version, truncated file, and appended bytes. Whole-game pty,
browser and door mutation evidence is recorded by their respective gates.

## Brief 37: build identity and DEL regressions

Before either DOS change, the new regression subset failed on the preceding
pinned image: **7 failed, 2 passed**. The missing build identity, stale-save
quarantine, missing stale-bones message, acceptance of the legacy save/bones
header, and both DEL whole-line cases were genuine failures; Backspace and
Ctrl-U already passed. Extending the bones cases to wizard mode exposed **2
failed, 4 passed**: the original wizard guard retained rejected bones. A counted
adaptation now deletes rejected bones in both modes while preserving valid
wizard bones. The extended regression subset reports **11 passed**:

```sh
WATCOM=$PWD/build/openwatcom .venv/bin/python -m pytest -q tests/test_hack_build.py \
  -k 'identifies_linked_build or outdated_save_and_bones or getlin_matches'
```

The full DOS build suite reports **58 passed**, including an independent
different-directory rebuild and complete vendor verification. The new EXE is
243,478 bytes, SHA-256
`b6f6ca8667fc8d1e37eb81fbd1c469a371312e4a39c53052985d553aaac345ee`.
These fixes require rebuilt DOS bytes: the save header now includes the linked
build identity, and the terminal restores upstream's DEL semantics. The
identity module is recompiled and relinked rather than patched into the EXE;
a dedicated test verifies its exact four-byte DATA contribution against OMF,
and a one-byte mutation in that stamp fails the same independent byte proof.
