"""Command-line interface for the listing-directed translator."""

import argparse
from pathlib import Path
import sys

from .emit import EmissionError, emit_image
from .image import ImageError, load_image
from .layout import LayoutError, build_layout
from .listing import ListingError, parse_listing
from .linked import build_linked_layout
from .nasm import build_nasm_layout, parse_nasm_listing
from .supplement import build_gwbasic_graphics_layout, emit_supplement


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("listing", type=Path, nargs="+")
    parser.add_argument("--map", type=Path, help="JWlink verbose map for a multi-module EXE")
    parser.add_argument("--format", choices=("jwasm", "nasm"), default="jwasm",
                        help="listing dialect (NASM requires -LefFt and a flat COM)")
    parser.add_argument("--supplement", choices=("gwbasic-graphics",),
                        help="emit additional source-proved indirect entries only")
    parser.add_argument("--name", required=True, help="DOS image name")
    parser.add_argument("--symbol", required=True, help="exported Image object C identifier")
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.format == "nasm":
            if args.map or len(args.listing) != 1:
                parser.error("NASM requires one flat-COM listing, without --map")
            layout = build_nasm_layout(load_image(args.image), parse_nasm_listing(args.listing[0]))
        elif args.map:
            layout = build_linked_layout(load_image(args.image), args.listing, args.map)
        elif len(args.listing) == 1:
            layout = build_layout(load_image(args.image), parse_listing(args.listing[0]))
        else:
            parser.error("multiple listings require --map")
        if args.supplement:
            if not args.map or args.format != "jwasm":
                parser.error("GW-BASIC graphics supplement requires JWasm listings and --map")
            layout = build_gwbasic_graphics_layout(layout)
            result = emit_supplement(layout, args.symbol)
        else:
            result = emit_image(layout, args.name, args.symbol)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # Translator implementation/listing diagnostics can change without
        # changing generated code. Preserve object-build caches in that case.
        if not args.output.exists() or args.output.read_text(encoding="utf-8") != result:
            args.output.write_text(result, encoding="utf-8")
    except (ImageError, ListingError, LayoutError, EmissionError, OSError) as exc:
        print(f"translator: {exc}", file=sys.stderr)
        return 1
    print(f"{args.name}: {len(layout.instructions)} instructions, {len(layout.chunks)} chunks, "
          f"{len(layout.image.relocations)} relocations -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
