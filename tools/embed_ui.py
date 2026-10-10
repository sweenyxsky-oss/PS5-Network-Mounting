#!/usr/bin/env python3
"""Embed the browser UI as a C++ raw string literal."""
from pathlib import Path
import sys

def main() -> int:
    if len(sys.argv) != 3:
        print("usage: embed_ui.py INPUT.html OUTPUT.hpp", file=sys.stderr)
        return 2
    source = Path(sys.argv[1]).read_text(encoding="utf-8")
    delimiter = "PS5NMHTML"
    if f"){delimiter}\"" in source:
        print("HTML contains the reserved C++ raw-string delimiter", file=sys.stderr)
        return 1
    output = (
        "#pragma once\n"
        "namespace ps5nm {\n"
        f'inline constexpr char kIndexHtml[] = R"{delimiter}({source}){delimiter}";\n'
        "}\n"
    )
    Path(sys.argv[2]).write_text(output, encoding="utf-8", newline="\n")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
