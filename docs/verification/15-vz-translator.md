# VZ Editor translator verification

The linked COM path emits 23,159 VZ instruction starts. Its two-byte macro
interrupt slot has two source-proved forms, giving 23,160 executable forms.
Forty-nine duplicate existing byte/offset cases, so VZ contributes 23,111
additional Unicorn cases. The combined VC, GW-BASIC, bootLogo and VZ gate
contains **87,012 distinct instructions**, with 32 randomized states each.
DAS additionally runs through 1,024 randomized states.

The first complete `make test-translator` run passed all 245 collected tests
in 553.35 seconds (cold instruction-library builds). Three additional VZ
coverage/source-proof/guard regressions were then included in the clean
13-test focused run recorded below. The expanded complete suite then passed
all 248 tests in 238.05 seconds within `make test`.

## Earlier generated C is unchanged

Before translator edits, copies were retained under
`build/brief15-baseline/gen/`. After regeneration, each command exited 0:

```sh
cmp build/brief15-baseline/gen/vc_com.c build/gen/vc_com.c
cmp build/brief15-baseline/gen/vc_ovl.c build/gen/vc_ovl.c
cmp build/brief15-baseline/gen/gwbasic.c build/gen/gwbasic.c
cmp build/brief15-baseline/gen/bootlogo.c build/gen/bootlogo.c
cmp build/brief15-baseline/gen/gwbasic_graphics.c build/gen/gwbasic_graphics.c
```

The before/after SHA-256 values are:

```text
d4d8f2fe893a384ae7c7c6695e650e862f1a894c1f13572b8c929fc302674817  vc_com.c
f0f51d56d84994b90828127a02d55de241a3a64b898dccc932846b51d5fec9b6  vc_ovl.c
8d8cf3225503fa947a969eb089b820ade134d61938765a78a257da7d43c7fe6f  gwbasic.c
cbb6ff5d610dfcc97d582aaf17f6cb45aff30a42b5aab4e3038ed712274d0a65  bootlogo.c
4aae5dd0c71f389a8a38a02bf8faa8eb35f70999bb52b21b14f63bad3f78b3f6  gwbasic_graphics.c
```

## Red-first regressions

`tests/test_translator_vz.py` was run before the corresponding changes:

- Linked COM placement failed with `a link map requires an MZ executable`.
- Retaining PROC entries failed with `B::finish` placed two bytes after its
  actual map address because ENDP overwrote the label.
- Symbolic DUP alignment failed with `cannot resolve linked data ... 'DUP'`.
- The real VZ layout failed because its source writes the macro opcode slot:
  `code store at 0xa6c1 changes opcode/boundary at 0xa6da`.
- DAS failed on sample 0 with `unsupported instruction das (2f)`.

The fixes retain listing-directed boundaries and byte checks. No runtime
decoder was added. Unknown macro-slot bytes and a changed source proof are
rejected by permanent tests.

## Planted defects and restoration

Each probe below ran in its own Python process. It changed only that process's
translator functions, not repository source or shared generated files. Ending
the process restores the production implementation.

### Omit VZ from the oracle

```sh
.venv/bin/python - <<'PY'
import sys
sys.path.insert(0, 'tests')
import pytest
from translator_support import ops_build
original = ops_build.load_cases
ops_build.load_cases = lambda: tuple(case for case in original() if case.image_name != 'VZ.COM')
raise SystemExit(pytest.main(['-q', 'tests/test_translator_ops.py', '-k', 'vz_oracle_includes']))
PY
```

Observed red: `1 failed`; the VZ coverage assertion reports `0 > 20000`.

### Incorrect DAS decimal adjustment

```sh
.venv/bin/python - <<'PY'
import pytest
from translator.emit import _InstructionEmitter
original = _InstructionEmitter.semantics
def broken_das(self):
    statements, terminal = original(self)
    if self.mnemonic == 'das':
        statements = [line.replace('cpu.a.l - 6u', 'cpu.a.l - 5u') for line in statements]
    return statements, terminal
_InstructionEmitter.semantics = broken_das
raise SystemExit(pytest.main(['-q', 'tests/test_translator_vz.py', '-k', 'das_matches']))
PY
```

Observed red: sample 0, `AX differs: C=4c0c Unicorn=4c0b`, plus a parity flag
difference. The production adjustment is 6, not 5.

### Ignore the live macro interrupt number

```sh
.venv/bin/python - <<'PY'
import pytest
from translator.emit import _InstructionEmitter
original = _InstructionEmitter.read
def frozen_interrupt(self, index):
    if self.mnemonic == 'int' and getattr(self.record, 'mutable_offsets', ()):
        return str(self.ops[index].imm & 255)
    return original(self, index)
_InstructionEmitter.read = frozen_interrupt
raise SystemExit(pytest.main(['-q', 'tests/test_translator_ops.py', '-k', 'vz_oracle_includes']))
PY
```

Observed red: VZ `0xa6da`, sample 0, both CS and IP differ from Unicorn.

### Bypass the macro-byte guard

```sh
.venv/bin/python - <<'PY'
import pytest
from translator.emit import _InstructionEmitter
original = _InstructionEmitter.emit
def unguarded_variant(self):
    if getattr(self.record, 'variants', ()):
        return _InstructionEmitter(self.record.variants[0], (), self.targets).emit()
    return original(self)
_InstructionEmitter.emit = unguarded_variant
raise SystemExit(pytest.main(['-q', 'tests/test_translator_vz.py', '-k', 'guard_rejects']))
PY
```

Observed red: the zero DW executes the wrong form instead of producing the
required `unsupported bytes in source-proved instruction variant` fault.

Restored process:

```text
.venv/bin/python -m pytest -q tests/test_translator_vz.py tests/test_translator_ops.py -k 'vz or ignored_patched_operand'
13 passed, 93 deselected in 44.18s
```
