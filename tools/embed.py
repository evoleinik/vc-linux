"""Embed files into the binary as C arrays: python3 tools/embed.py OUT.c NAME=PATH ...

Writes the `embedded_files` table declared in runtime/rt.h.
"""
import sys


def main() -> None:
    out, pairs = sys.argv[1], sys.argv[2:]
    lines = ['#include "rt.h"', ""]
    table = []
    for i, pair in enumerate(pairs):
        name, path = pair.split("=", 1)
        data = open(path, "rb").read()
        lines.append(f"static const uint8_t f{i}[{max(len(data), 1)}] = {{")
        for j in range(0, len(data), 20):
            lines.append("  " + ",".join(str(b) for b in data[j : j + 20]) + ",")
        lines.append("};")
        table.append(f'  {{"{name}", f{i}, {len(data)}}},')
    lines += ["", "const EmbeddedFile embedded_files[] = {", *table, "};"]
    lines.append(f"const int embedded_file_count = {len(pairs)};")
    open(out, "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
