"""Build the pinned US VZ Editor without modifying its MASM 5.1 sources.

All source compatibility changes happen in the output directory.  The linker
uses the original module order and produces a verbose map and expanded source
listings for the ahead-of-time translator.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / "third_party/vzeditor"
SOURCE = VENDOR / "SRC"
REFERENCE = VENDOR / "VZ-IBM/US/VZUS.COM"
UNUSED_US_EXTERNS = frozenset({
    "init_module", "chkdosheight", "texts", "ems_free", "flsyscall", "altsize",
    "sm_sensekey", "tmpnamep", "killmemtmp", "readmemtmp", "writememtmp",
})


def modules(source: Path = SOURCE) -> list[str]:
    """Keep VZ.LNK's module order, including XSCR before SWAP and EMS."""
    return re.findall(r"(?im)^([a-z][a-z0-9_]*)\+?\s*$",
                      (source / "VZ.LNK").read_text().rstrip("\x1a").upper())


def _sized_ax_operand(match: re.Match[str]) -> str:
    operand = match[2]
    if re.match(r"(?i)word\s+ptr\b", operand):
        return match[0]
    if re.match(r"(?i)type\s", operand):
        # Unary + makes TYPE evaluate before WORD PTR. Parentheses alone
        # let JWasm apply the cast to the structure and incorrectly yield 2.
        value, marker, comment = operand.partition(";")
        operand = "(+" + value.rstrip() + ")" + (" " + marker + comment if marker else "")
    return match[1] + "word ptr " + operand


def preprocess(name: str, data: bytes) -> bytes:
    """Adapt this pinned dialect and its instruction encodings losslessly."""
    text = data.decode("latin-1").replace("\r\n", "\n").rstrip("\x1a")
    # Linux is case-sensitive, DOS was not. All staged source names use the
    # vendor's uppercase spelling, including the mixed-case scrnIBM.asm.
    text = re.sub(r"(?im)^(\s*include\s+)([\w.]+)",
                  lambda m: m[1] + m[2].upper(), text)
    if name.upper() == "KEYIBM.ASM":
        # @F is a numeric constant here, not MASM's modern anonymous label.
        text = re.sub(r"(?<![\w@$?])@F(?![\w@$?])", "VZ_FKEY", text)
    if name.upper() in ("MSG.ASM", "STRING.ASM"):
        text = re.sub(r"(?im)^(GDATA\s+\w+)(\s+)(dd|label)\b", r"\1,\2\3", text)
        text = re.sub(r"(?im)^(GDATA\s+\w+,\s*label)(\s+)(byte)\b", r"\1,\2\3", text)
    # These stale declarations have no references in the US target. JWlink
    # diagnoses even unused EXTRNs, unlike the original linker. EXTERNDEF
    # remains an unresolved error if a future source actually uses one.
    text = re.sub(r"(?im)^(\s*)extrn(\s+)(\w+)\b",
                  lambda m: m[1] + ("externdef" if m[3] in UNUSED_US_EXTERNS else "extrn")
                  + m[2] + m[3], text)
    # The shipped build selected accumulator opcodes (e.g. 3D iw) rather than
    # same-length sign-extended 83 /7 ib form. The explicit size reproduces
    # those bytes. AX's memory/register operands already have this size.
    text = re.sub(r"(?im)^(\s*(?:\w+:\s*)?(?:add|or|adc|sbb|and|sub|xor|cmp)\s+ax,\s*)([^\n]+)",
                  _sized_ax_operand, text)
    if name.upper() == "MSG.ASM":
        # The final EVEN is in class TAIL, which JWasm zero-fills. The shipped
        # assembler used a NOP. mg_remove is the start of this word-aligned
        # segment, so the subtraction is an assembler-time absolute value.
        text = re.sub(r"(?im)^(\s*)even\s*$", r"\1db (($ - mg_remove) and 1) dup (90h)", text)
    # -Sa cannot counter a later .SALL/.XLIST. Macro-expanded instructions
    # and included code must be present for listing-based translation.
    text = re.sub(r"(?im)^(\s*)\.xlist\b", r"\1.list", text)
    text = re.sub(r"(?im)^(\s*)\.sall\b", r"\1.lall", text)
    return text.encode("latin-1")


def verify_image(path: Path, reference: Path = REFERENCE) -> None:
    """Do not publish a build with an unexplained byte or setting change."""
    actual, expected = path.read_bytes(), reference.read_bytes()
    if actual != expected:
        first = next((i for i, pair in enumerate(zip(actual, expected)) if pair[0] != pair[1]),
                     min(len(actual), len(expected)))
        raise ValueError(f"VZ differs from shipped US binary at file offset 0x{first:x} "
                         f"(built {len(actual)} bytes, shipped {len(expected)} bytes)")


def build(output: Path) -> None:
    output = output.resolve()
    # A mistaken output argument must never rewrite the protected originals.
    if output == VENDOR or VENDOR in output.parents or output in VENDOR.parents:
        raise ValueError("the VZ output directory must be separate from the vendored sources")
    output.mkdir(parents=True, exist_ok=True)
    for source in sorted(SOURCE.iterdir()):
        if source.suffix.upper() in (".ASM", ".INC"):
            (output / source.name.upper()).write_bytes(preprocess(source.name, source.read_bytes()))
    objects = []
    for module in modules():
        subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-Zm", "-Cp", "-Zg", "-DUS",
                        "-Sg", "-Sa", f"-Fl={module}.lst", f"-Fo={module}.OBJ", module + ".ASM"],
                       cwd=output, check=True)
        listing = output / (module + ".lst")
        text = listing.read_bytes().decode("latin-1")
        listing.write_bytes(re.sub(r", \d+ ms,", ", elapsed omitted,", text).encode("latin-1"))
        objects.extend(("file", module + ".OBJ"))
    pending = output / "VZ.COM.pending"
    try:
        subprocess.run([str(ROOT / "tools/jwlink/jwlink"), "format", "dos", "com", *objects,
                        "name", pending.name, "option", "map=VZ.MAP,verbose"], cwd=output, check=True)
        verify_image(pending)
        pending.replace(output / "VZ.COM")
    finally:
        pending.unlink(missing_ok=True)
    map_file = output / "VZ.MAP"
    text = re.sub(r"(?m)^Created on:.*$", "Created from pinned VZ Editor US sources", map_file.read_text())
    text = re.sub(r"(?m)^Link time:.*$", "Link time: omitted for reproducibility", text)
    text = text.replace("Executable Image: VZ.COM.pending", "Executable Image: VZ.COM")
    map_file.write_text(text)
    print(f"VZ Editor US: {len(modules())} modules, {(output / 'VZ.COM').stat().st_size:,} bytes, "
          "identical to shipped VZUS.COM")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--modules", action="store_true")
    arguments = parser.parse_args()
    if arguments.modules:
        print(" ".join(modules()))
    elif arguments.output:
        build(arguments.output)
    else:
        parser.error("an output directory or --modules is required")
