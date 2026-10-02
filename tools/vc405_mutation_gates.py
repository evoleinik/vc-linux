"""Run bounded, process-isolated planted defects against VC 4.05's real gates.

Production files and live build outputs are never modified: each child changes
only its imported implementation or pytest's temporary build. Child exit
restores the implementation automatically, even if a gate crashes or times out.
Every rejected defect is immediately followed by a fresh, unmodified execution
of the same pytest gate, with its successful result saved separately.
"""

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "source": ("tests/test_vc405_build.py::test_changed_source_is_refused_without_replacing_previous_good_outputs",
               "DID NOT RAISE", "disable verification of the caller's staged source"),
    "vc_identity": ("tests/test_vc405_build.py::test_each_source_built_image_is_byte_identical_to_genuine_tasm[VC.COM]",
                    "AssertionError", "flip byte 0x100 in the temporary VC.COM"),
    "setup_identity": ("tests/test_vc405_build.py::test_each_source_built_image_is_byte_identical_to_genuine_tasm[VCSETUP.COM]",
                       "AssertionError", "flip byte 0x100 in the temporary VCSETUP.COM"),
    "reproducibility": ("tests/test_vc405_build.py::test_images_listings_and_identity_report_are_reproducible",
                        "AssertionError", "leak the temporary build directory into a listing"),
    "listing": ("tests/test_translator_vc405.py::test_complete_program_has_listing_proved_instruction_boundaries[VC.COM]",
                "instruction length mismatch", "remove TASM LOOP expansion support"),
    "unicorn": ("tests/test_translator_vc405.py::test_loop_expansion_each_instruction_matches_unicorn",
                "FLAGS differs", "toggle carry after the production-emitted LOOP"),
    "source_lines": ("tests/test_translator_vc405.py::test_vc405_line_numbers_are_physical_listing_lines",
                     "AssertionError", "restore logical splitlines parsing of a quoted DOS control byte"),
    "oracle_coverage": ("tests/test_translator_vc405.py::test_every_vc405_instruction_is_present_in_the_unicorn_gate[VC.COM]",
                        "instructions absent from Unicorn", "drop VC405's instructions from the oracle case list"),
}


def child(name: str) -> int:
    sys.path[:0] = [str(ROOT), str(ROOT / "tests")]
    import pytest
    from tools import build_vc405 as builder

    if name == "source":
        original = builder.verify_sources
        builder.verify_sources = lambda source=builder.SOURCE: original(builder.SOURCE)
    elif name in ("vc_identity", "setup_identity", "reproducibility"):
        original = builder.build

        def defective_build(output, source=builder.SOURCE):
            original(output, source)
            if name == "reproducibility":
                target = output / "VC.COM.lst"
                target.write_bytes(target.read_bytes() + str(output).encode() + b"\n")
            else:
                target = output / ("VC.COM" if name == "vc_identity" else "VCSETUP.COM")
                raw = bytearray(target.read_bytes())
                raw[0x100] ^= 1
                target.write_bytes(raw)

        builder.build = defective_build
    elif name == "listing":
        import translator.vc405
        from translator.layout import build_layout
        translator.vc405.build_vc405_layout = build_layout
    elif name == "source_lines":
        import translator.listing
        original = translator.listing.parse_listing

        def logical_lines(path, **kwargs):
            kwargs.pop("physical_lines", None)
            return original(path, **kwargs)

        translator.listing.parse_listing = logical_lines
    elif name == "oracle_coverage":
        from translator_support import ops_build
        original = ops_build.load_cases
        ops_build.load_cases = lambda: tuple(case for case in original() if case.image_name != "VC405.COM")
    elif name == "unicorn":
        from translator_support import ops_build
        original = ops_build.emit_instruction_function

        def wrong_carry(record, relocations, symbol):
            if record.insn.mnemonic != "loop":
                return original(record, relocations, symbol)
            return (original(record, relocations, symbol + "_correct") +
                    f"\nint {symbol}(uint16_t loadseg) {{\n"
                    f"int result = {symbol}_correct(loadseg); cpu.cf ^= 1; return result;\n}}\n")

        ops_build.emit_instruction_function = wrong_carry
    return pytest.main(["-q", CASES[name][0]])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--child", choices=CASES)
    parser.add_argument("--output", type=Path, default=ROOT / "build/vc405-mutations")
    arguments = parser.parse_args()
    if arguments.child:
        return child(arguments.child)
    arguments.output.mkdir(parents=True, exist_ok=True)
    for name, (testcase, diagnostic, description) in CASES.items():
        result = subprocess.run([sys.executable, __file__, "--child", name], cwd=ROOT,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=180)
        (arguments.output / (name + ".log")).write_text(result.stdout)
        if result.returncode != 1 or diagnostic not in result.stdout:
            print(result.stdout)
            raise RuntimeError(f"planted {name} defect did not trip its gate")
        print(f"RED {name}: {description}", flush=True)
        restored = subprocess.run([sys.executable, "-m", "pytest", "-q", testcase], cwd=ROOT,
                                  text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  timeout=180)
        (arguments.output / (name + ".restored.log")).write_text(restored.stdout)
        if restored.returncode != 0 or "1 passed" not in restored.stdout:
            print(restored.stdout)
            raise RuntimeError(f"restored {name} gate did not pass")
        print(f"RESTORED GREEN {name}: the same unmodified pytest gate passed", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
