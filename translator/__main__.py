"""Command-line interface for the listing-directed translator."""

import argparse
from pathlib import Path
import sys

from .emit import EmissionError, emit_image
from .image import ImageError, load_image
from .layout import LayoutError, build_layout
from .listing import ListingError, parse_listing
from .linked import build_linked_layout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("listing", type=Path, nargs="+")
    parser.add_argument("--map", type=Path, help="JWlink verbose map for a multi-module EXE")
    parser.add_argument("--name", required=True, help="DOS image name")
    parser.add_argument("--symbol", required=True, help="exported Image object C identifier")
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        if args.map:
            layout = build_linked_layout(load_image(args.image), args.listing, args.map)
        elif len(args.listing) == 1:
            layout = build_layout(load_image(args.image), parse_listing(args.listing[0]))
        else:
            parser.error("multiple listings require --map")
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
