"""Build the vendored GW-BASIC with deterministic sources and rich listings.

Only the pinned UPSTREAM date is the OEM version.  The upstream awk script's
wall-clock comment and tool diagnostic timing fields are normalized too, so
the executable, map and all source listings are byte-reproducible.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "third_party" / "gwbasic"


def modules(source: Path = SOURCE) -> list[str]:
    result = re.findall(r"(?im)^([A-Z0-9_]+)\.OBJ", (source / "GWBASIC.LNK").read_text())
    # The checked-in MASM-era link file predates the fork's Ctrl-Break OEM
    # module. Its upstream GNU Makefile adds it; retain that addition here.
    if "OEMCBK" not in result:
        result.insert(result.index("OEMEV"), "OEMCBK")
    return result


def build(output: Path) -> None:
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    version = (SOURCE / "UPSTREAM").read_text().split()[1]
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", version):
        raise ValueError("UPSTREAM must provide the pinned OEM version date")
    for source in sorted(SOURCE.iterdir()):
        if source.suffix.upper() not in (".ASM", ".H") and source.name not in ("GIO86U", "MSDOSU"):
            continue
        preprocessed = subprocess.check_output(
            ["awk", "-f", str(SOURCE / "jwasmify.awk"), str(source)], text=True)
        preprocessed = preprocessed.split("\n", 1)[1]
        # -Sa cannot override a later .SALL directive. Show INS86 and other
        # DB-emitting macros explicitly; .LALL affects listings, not bytes.
        preprocessed = re.sub(r"(?im)^(\s*)\.SALL\b", r"\1.LALL", preprocessed)
        preprocessed = re.sub(r"(?im)^(\s*)\.XLIST\b", r"\1.LIST", preprocessed)
        (output / source.name).write_text(
            f"; [ Munged by jwasmify.awk; UPSTREAM {version} ]\n" + preprocessed)
    objects = []
    for module in modules():
        subprocess.run([str(ROOT / "tools/jwasm/jwasm"), "-q", "-Zm", "-fpc", "-Sg", "-Sa",
                        f"-DOEMVER={version}", f"-Fl={module}.lst", f"-Fo={module}.OBJ",
                        module + ".ASM"], cwd=output, check=True)
        listing = output / (module + ".lst")
        listing.write_text(re.sub(r", \d+ ms,", ", elapsed omitted,", listing.read_text()))
        objects.extend(("file", module + ".OBJ"))
    subprocess.run([str(ROOT / "tools/jwlink/jwlink"), "format", "dos", *objects,
                    "name", "GWBASIC.EXE", "option", "dosseg,map=GWBASIC.MAP,verbose"],
                   cwd=output, check=True)
    map_file = output / "GWBASIC.MAP"
    map_text = re.sub(r"(?m)^Created on:.*$", f"Created from UPSTREAM: {version}", map_file.read_text())
    map_text = re.sub(r"(?m)^Link time:.*$", "Link time: omitted for reproducibility", map_text)
    map_file.write_text(map_text)
    print(f"GW-BASIC {version}: {len(modules())} modules, "
          f"{(output / 'GWBASIC.EXE').stat().st_size:,} bytes")


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
