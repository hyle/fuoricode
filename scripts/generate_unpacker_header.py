#!/usr/bin/env python3

from __future__ import annotations

import sys
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_unpacker_header.py <source> <output>", file=sys.stderr)
        return 1

    source = Path(sys.argv[1])
    output = Path(sys.argv[2])
    data = source.read_bytes()

    lines = [
        "#ifndef GENERATED_UNPACKER_H",
        "#define GENERATED_UNPACKER_H",
        "",
        "#include <stddef.h>",
        "",
        "static const unsigned char FUORI_UNPACKER_SCRIPT[] = {",
    ]

    for offset in range(0, len(data), 12):
        row = ", ".join(f"0x{byte:02X}" for byte in data[offset : offset + 12])
        lines.append(f"    {row},")

    lines.extend(
        [
            "    0x00",
            "};",
            "",
            f"static const size_t FUORI_UNPACKER_SCRIPT_LEN = {len(data)};",
            "",
            "#endif",
        ]
    )

    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
