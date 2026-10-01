# VZ Editor US build verification

`make vz` assembles the 29 modules in the original `SRC/VZ.LNK` order and
links a DOS COM using JWasm and JWlink. `build/vz/VZ.COM` is exactly equal to
`third_party/vzeditor/VZ-IBM/US/VZUS.COM`:

```text
55,856 bytes
SHA256 198cd8ace6e1a945dacdd6ac7e272b0f8131b688d762df4d90738d2796d1eeb2
cmp build/vz/VZ.COM third_party/vzeditor/VZ-IBM/US/VZUS.COM
# exit 0; no differing bytes
```

The shipped binary is used only for the comparison, never as build input data.
The builder refuses to publish a nonidentical COM. The `vz` target also compares
an already-built COM, so changing an existing output cannot evade the gate.

## Source compatibility and configuration

`tools/build_vz.py` stages copies; vendored sources are not edited. It:

- Resolves include names case-insensitively, including `scrnIBM.asm`.
- Renames the `@F` numeric key constant, which collides with JWasm's forward-label syntax.
- Adds missing separators to the `GDATA` macro arguments in STRING and MSG.
- Uses `EXTERNDEF` for eleven stale declarations that have no references in the
  US build. An actual reference to an undefined symbol still fails to link.
- Exposes included and macro-expanded instructions in the listings.
- Makes AX operands explicitly word-sized, selecting the original accumulator
  opcodes rather than equal-length `83` forms. Unary `+` preserves the original
  result of `TYPE` expressions before JWasm applies the size cast.
- Makes MSG's final `EVEN` use the shipped NOP padding byte rather than zero.

The initial link already matched the shipped size and all configuration data;
only those arithmetic encodings and the final alignment byte differed. After
the syntax fixes, every byte matches. The customization tables populated by
INST and the startup options in MAIN therefore need no installed-settings
patch or extracted binary data.

For English startup configuration, install `VZIBM.DEF` as `VZ.DEF`.
Its *Else* table names `VZFLE.DEF`; F12 loads `HELPE.DEF`. BLOCK and PALET are
optional menu-loaded macros, and BW is an optional monochrome theme.
The original key definitions are Esc, S / Alt-S for Save As (Enter accepts the
current filename), and Esc, Q / Alt-Q for Quit.

## Gates and planted defects

The defects below are scoped to one Python process. Both builds use pytest's
temporary directories, and neither mutation changes repository source or the
shared `build/vz` outputs.

Binary reproduction defect: disable the accumulator-encoding rewrite.

```sh
.venv/bin/python -c 'import tools.build_vz as vz; import pytest; vz._sized_ax_operand = lambda match: match[0]; raise SystemExit(pytest.main(["-q", "tests/test_vz_build.py::test_rebuild_matches_shipped_and_preserves_vendor"]))'
```

Observed red: `1 failed`; `ValueError: VZ differs from shipped US binary at file
offset 0xd76 (built 55856 bytes, shipped 55856 bytes)`.

Reproducibility defect: append a different build number to each normalized map.

```sh
.venv/bin/python -c 'import tools.build_vz as vz; import pytest; import itertools; original = vz.re.sub; counter = itertools.count(); vz.re.sub = lambda pattern, replacement, value, *args, **kwargs: original(pattern, replacement, value, *args, **kwargs) + ("\nunstable build " + str(next(counter)) if pattern == "(?m)^Created on:.*$" else ""); raise SystemExit(pytest.main(["-q", "tests/test_vz_build.py::test_images_maps_and_expanded_listings_are_reproducible"]))'
```

Observed red: `1 failed`; `AssertionError: VZ.MAP`, differing build numbers 0 and 1.

Restored process:

```text
make test-vz-build
VZ Editor US: 29 modules, 55,856 bytes, identical to shipped VZUS.COM
cmp build/vz/VZ.COM third_party/vzeditor/VZ-IBM/US/VZUS.COM
14 passed in 0.62s
```

This includes independently rebuilding the COM, map, and all 29 expanded
listings twice and comparing them byte-for-byte. It also rejects mutations in
code, startup settings, padding, and executable size, and verifies that a build
has not changed any vendored source file.
