# Brief 31: date-independent Rogue build gate

The complete native test run crossed UTC midnight while this brief was in
progress. Two existing Rogue reproducibility tests then failed even though the
vendor files had not changed:

```text
test_rogue_build_is_byte_reproducible
At index 193388 diff: b'2' != b'1'
test_rogue_reproducibility_rejects_matching_unverified_bytes
Expected regex: 'verified ROGUE.EXE SHA-256'
Actual message: ... At index 193388 diff: b'2' != b'1'
2 failed, 43 passed in 0.79s
```

The byte is the day in PDCurses' `__DATE__` banner. The independently verified
main binary and branch binary both contain `PDCurses 3.9 - Oct  1 2026`; a
fresh build contained `Oct  2 2026`. Their expected hashes were not changed.

OpenWatcom ignores the existing `SOURCE_DATE_EPOCH=0`. A direct compiler probe
with `-d__DATE__="Oct  1 2026"` emitted warning W140 and still embedded Oct 2.
Adding `-u__DATE__` failed with `E1101: Cannot #undef '__DATE__'`.

## Red independent of the actual date

A new test inspects the real source argument immediately before WCC compiles
the notice. Thus it fails on the old builder even when the computer happens
to have the same date as the original verified image:

```text
.venv/bin/python -m pytest -q tests/test_rogue_build.py -k pins_banner
FAIL tests/test_rogue_build.py:408
AssertionError: PDCurses banner still depends on the compiler date
1 failed, 45 deselected in 0.15s
```

## Fix and green

`tools/build_rogue.py` now makes a guarded `pdcsrc/initscr.c` build copy, just
as it already does for PDCurses' CP437 include. Only the verified notice's
`__DATE__` is replaced with `"Oct  1 2026"`. A missing/duplicated notice or any
remaining compiler clock dependency in that file is refused. No vendor source
is edited and no existing expected hash is changed.

```text
.venv/bin/python -m pytest -q tests/test_rogue_build.py -k 'pins_banner or clock_notice'
6 passed, 45 deselected in 0.33s
WATCOM=/home/eo/src/vc-linux-wt/command/build/openwatcom .venv/bin/python -m pytest -q tests/test_rogue_build.py
51 passed in 1.38s
sha256sum build/rogue/ROGUE.EXE
b6350f98553e96ae5454383ec377d9feb63834cc99fc3411f7ff016321dd61a7  build/rogue/ROGUE.EXE
```

The full suite independently rebuilds and checks both the canonical binary
and the historical pre-brief-18 binary (`8844a6fbce3a9a9b6c1215d823d99b9b18c16b8786f5a2bc362988bc6801c9ee`).
It also verifies all Rogue/PDCurses vendor digests are unchanged and still
rejects two equal but unverified binaries. A first attempt without the required
WATCOM environment failed at setup with the existing installation diagnostic;
the green run above uses the repository's documented toolchain setup.
