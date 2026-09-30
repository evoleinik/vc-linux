"""Command-line interface for the listing-directed translator."""

import argparse
from pathlib import Path
import sys

from .emit import EmissionError, emit_image
from .image import ImageError, load_image
from .layout import LayoutError, build_layout
from .listing import ListingError, parse_listing


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("listing", type=Path)
    parser.add_argument("--name", required=True, help="DOS image name")
    parser.add_argument("--symbol", required=True, help="exported Image object C identifier")
    parser.add_argument("-o", "--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        layout = build_layout(load_image(args.image), parse_listing(args.listing))
        result = emit_image(layout, args.name, args.symbol)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding="utf-8")
    except (ImageError, ListingError, LayoutError, EmissionError, OSError) as exc:
        print(f"translator: {exc}", file=sys.stderr)
        return 1
    print(f"{args.name}: {len(layout.instructions)} instructions, {len(layout.chunks)} chunks, "
          f"{len(layout.image.relocations)} relocations -> {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
