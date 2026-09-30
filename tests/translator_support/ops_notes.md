# Instruction oracle

`test_translator_ops.py` validates the production emitter for every distinct
`(linked bytes, image offset)` pair in both images. Identical pairs with different
relocation meanings are retained separately. Every retained instruction receives
32 accepted deterministic random states; pytest groups by mnemonic only to make
failures and `-k sbb` reruns convenient. No instruction family is sampled out.

The generated instruction bodies, `runtime/cpu.c`, and the independent Unicorn
driver are compiled with `gcc -O1 -Wall -DCPU_TRACE_WRITES` and linked into one
shared library. Compilation is sharded to bound GCC memory use. Content hashes
cover emitted text, runtime/interfaces, harness, compiler identity/options, and
Unicorn version; the temporary-directory cache therefore cannot conceal an
emitter edit in the planted-bug gate.

The C oracle directly uses the bundled Unicorn C API for speed. Its separate
memory starts from a fixed pseudorandom seed for each instruction. All general,
index, stack, segment, and IP registers and all supported FLAGS fields are
randomized. CS aliases vary while the physical instruction address remains the
same. Stack and effective-address boundary states, both directions of string
operations, zero/one/boundary shift counts, zero CX, and divide-by-zero/quotient
overflow are deliberately mixed into those states. Register/FLAGS snapshots,
seeds, operand addresses, differing fields, and missing/different written bytes
are printed on failure. Memory state is reproducible by replaying that
instruction's samples from zero.

Defined flag masks use Capstone's undefined-flag metadata, supplemented with the
architectural count-dependent shift/rotate rules. Zero counts preserve all flags;
OF is compared only for a masked count of one; AF for nonzero shifts and CF for
oversized SHL/SHR are undefined. Arithmetic flags after a successful DIV/IDIV are
undefined, but a faulting divide preserves them; the #DE path compares every
flag, including live arithmetic flags as well as the saved interrupt frame.

Every IN and OUT event is compared by direction, full 16-bit port, access width,
and value. Input data also depends on both port bytes. Focused oracle self-tests
prove that corrupting live CF after #DE and truncating the DX port of either
byte- or word-sized IN are rejected, without modifying the production emitter.

Unicorn deliberately does not perform real-mode software-interrupt dispatch.
The callback independently implements the Intel sequence: push FLAGS, CS, and
continuation IP; clear IF and TF; load the vector from the IVT. INT/INTO use the
next IP, whereas #DE uses the faulting instruction IP. It never calls `cpu_int`,
`flags_get`, or another generated/runtime helper. A pristine Unicorn CPU context
is restored before every state: otherwise its retained exception state converts
a second #DE into a spurious #DF. TF is randomized, but post-step #DB notifications
are not dispatched because the comparison observes one instruction's result,
before debugger entry.

## Deliberate state restriction: no active-code writes

VC has no self-modifying source instructions. Random writable operand spans,
string destinations, and potential stack-write spans are rejected if they touch
the executing instruction's translation-block page(s), including a 15-byte
decode lookahead. Read-only operands may still overlap code. This preserves the
fixed code required by ahead-of-time translation and avoids a Unicorn 2.1.4
single-step artifact: a write invalidating the current translated block can make
`count=1` return before that instruction commits, even if the write is after the
instruction bytes. Each rejected state is replaced; all 32 comparisons and the
forced stack/effective-address edges remain. Replacements are counted, and 128
consecutive rejections fail loudly.

This restriction was introduced only after observing these concrete failures:

- `e8 f0 ee` at VC.OVL offset `0xe3be`, load segment `0x35b9`, CS:IP
  `3930:ac4e`, SS:SP `42e0:1152`: the CALL's stack word at physical `0x43f50`
  overlaps its own last byte. Unicorn reports a write but leaves SP/IP unchanged.
- `2e 80 0e a6 cb 02` at VC.OVL offset `0x11b8d`, CS:IP `973e:cb9d`:
  `or byte ptr cs:[0xcba6],2` writes beyond the instruction, within its translated
  block. Unicorn returns with unchanged IP and flags instead of completing it.
- `f3 a4` at VC.COM offset `0x1874`, load segment `0x9343`, CS:IP
  `8a76:a544`, ES:DI `8aac:a1e9`, CX `0x1e`, DF set: REP MOVSB overwrites its
  own instruction after five iterations. Unicorn then decodes different code;
  that behavior is outside this fixed-instruction translator's scope.

The suite is broad but not exhaustive over the CPU state space: 32 seeded
states cannot prove every operand/flag combination, undefined flags are not
asserted, and self-modifying/protected-mode execution is outside its contract.

## Planted-bug gate, 2026-09-30

Temporarily changed the emitter's `tr_sub` carry computation from
`cpu.cf = (a < b + borrow);` to `cpu.cf = (a < b);`, making SBB ignore its
incoming borrow when computing CF. Ran the actual all-instruction gate:

```
.venv/bin/python -m pytest -q -x -s tests/test_translator_ops.py
```

It failed after 52 passing instruction families, with this diagnostic:

```
VC.OVL:0x0a34c sbb bx, bx
bytes: 1b db
image offset=0x0a34c loadseg=0x3085 sample=1 seed=0xca8a0177
starting state:
  AX=630f BX=cc32 CX=4aa4 DX=04d0 SI=f0f4 DI=5a5c BP=c327
  SP=0001 ES=3265 CS=2f39 SS=0ed9 DS=9b4b IP=b80c FLAGS=3d43
  defined FLAGS mask=ffff (undefined=0000)
FLAGS differs: C=3596 Unicorn=3597 xor=0001 compared_mask=ffff
written byte counts: C=0 Unicorn=0
```

The intentional edit was then restored. SHA-256 of `translator/emit.py` before
and after the mutation was identical:
`7a48998537c23aa150608cb9fb66a56ccb4d319ca6cc43b75ec236f352faae51`.
Both full image outputs were regenerated from the restored emitter before the
final gate run. To repeat this negative test, make the same one-line temporary
edit, run the command above, restore it, then run `make test-translator`.
